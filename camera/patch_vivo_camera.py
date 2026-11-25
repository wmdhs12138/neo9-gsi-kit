#!/usr/bin/env python3
"""
Fast deterministic patcher & APK-v2 signer for OriginOS 4 VivoCamera.apk and
Vivo3rdAlgoService.apk on Android 17 GSI:
  1. Streams ZIP raw entries without re-compressing already-deflated assets (208 MB in ~3s),
     4-byte aligning all STORED entries (including resources.arsc and classes*.dex).
  2. NOPs `Landroid/view/View;->setNightMode(I)V` inside `classes3.dex` (only Vivo-only
     method invoked on a standard AOSP class) and recomputes DEX SHA-1 & Adler-32.
  3. Injects `classes15.dex` (built by build_compat_dex.py) with the 14 missing Vivo
     framework/widget compatibility classes.
  4. Signs both APKs with Neo9's APK Signature Scheme v2 key so both receive
     `seinfo=neo9_ims` -> `domain=platform_app` (unlocking vendor hal_camera3rd_client).
"""
import argparse
import hashlib
import os
import struct
import subprocess
import sys
import tempfile
import zipfile
import zlib

CAM_DIR = os.path.dirname(os.path.abspath(__file__))
KIT_DIR = os.path.dirname(CAM_DIR)
sys.path.insert(0, os.path.join(KIT_DIR, "overlay"))
sys.path.insert(0, CAM_DIR)
import build_overlay as bo  # noqa: E402
import build_compat_dex as bcd  # noqa: E402


def _uleb128(b: bytes, p: int) -> tuple[int, int]:
    v = s = 0
    while True:
        x = b[p]
        p += 1
        v |= (x & 0x7F) << s
        if not (x & 0x80):
            return v, p
        s += 7


def _insn_len(insns: tuple[int, ...], i: int) -> int:
    op = insns[i] & 0xFF
    if op == 0x00:
        hi = insns[i] >> 8
        if hi == 0x01:
            return insns[i + 1] * 2 + 4
        if hi == 0x02:
            return insns[i + 1] * 4 + 2
        if hi == 0x03:
            w = insns[i + 1]
            sz = insns[i + 2] | (insns[i + 3] << 16)
            return (sz * w + 1) // 2 + 4
        return 1
    if op in (
        0x01, 0x04, 0x07, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12,
        0x1D, 0x1E, 0x21, 0x27, 0x28, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43,
        0x73, 0x79, 0x7A,
    ) or (0x7B <= op <= 0x8F) or (0xB0 <= op <= 0xCF) or (0xE3 <= op <= 0xF9):
        return 1
    if op in (
        0x02, 0x05, 0x08, 0x13, 0x15, 0x16, 0x19, 0x1A, 0x1C, 0x1F, 0x20, 0x22,
        0x23, 0x29, 0x2D, 0x2E, 0x2F, 0x30, 0x31,
    ) or (0x32 <= op <= 0x3D) or (0x44 <= op <= 0x6D) or (0x90 <= op <= 0xAF) or (0xD0 <= op <= 0xE2):
        return 2
    if op in (
        0x03, 0x06, 0x09, 0x14, 0x17, 0x1B, 0x24, 0x25, 0x26, 0x2A, 0x2B, 0x2C,
    ) or (0x6E <= op <= 0x72) or (0x74 <= op <= 0x78) or (0xFA <= op <= 0xFF):
        return 3
    if op == 0x18:
        return 5
    return 1


def patch_manifest_target_sdk(axml: bytes, new_target_sdk: int = 33) -> bytes:
    """
    Lower android:targetSdkVersion from 34 -> 33 in binary AndroidManifest.xml so
    AOSP Android 17 does not enforce DYNAMIC_RECEIVER_EXPLICIT_EXPORT_REQUIRED (API 34+)
    on VivoCamera's 18 legacy registerReceiver() calls listening to vivo.* broadcasts
    (which are <protected-broadcast> on OriginOS but not in AOSP framework-res).
    """
    out = bytearray(axml)
    rtype, rhdr, rsize = struct.unpack_from("<HHI", out, 0)
    assert rtype == 0x0003 and rsize == len(out), "Invalid AXML root header"

    # Parse resource ID map chunk (0x0180) to find string pool index of attr 0x01010270 (targetSdkVersion)
    pos = rhdr
    target_attr_name_idx = None
    patched = 0
    while pos < len(out):
        ctype, chdr, csize = struct.unpack_from("<HHI", out, pos)
        if ctype == 0x0180:  # RES_XML_RESOURCE_MAP_TYPE
            cnt = (csize - chdr) // 4
            for i in range(cnt):
                rid = struct.unpack_from("<I", out, pos + chdr + i * 4)[0]
                if rid == 0x01010270:  # android:targetSdkVersion
                    target_attr_name_idx = i
                    break
        elif ctype == 0x0102 and target_attr_name_idx is not None:  # RES_XML_START_ELEMENT_TYPE
            _, _, attr_start, attr_size, attr_cnt, _, _, _ = struct.unpack_from("<IIHHHHHH", out, pos + chdr)
            attrs_base = pos + chdr + attr_start
            for i in range(attr_cnt):
                a_off = attrs_base + i * attr_size
                a_ns, a_name, a_raw, a_val_sz, _, a_val_type, a_val_data = struct.unpack_from("<IIIHBBI", out, a_off)
                if a_name == target_attr_name_idx and a_val_type == 0x10:  # TYPE_INT_DEC
                    struct.pack_into("<I", out, a_off + 16, new_target_sdk)
                    print(f"Patched AndroidManifest.xml: targetSdkVersion {a_val_data} -> {new_target_sdk}")
                    patched += 1
        pos += csize

    assert patched == 1, f"Expected 1 targetSdkVersion attribute in AndroidManifest.xml, patched {patched}"
    return bytes(out)


NIGHT_MODE_TARGET_CLASSES = {
    "Landroid/graphics/Canvas;",
    "Landroid/view/View;",
    "Landroid/widget/Button;",
    "Landroid/widget/ImageButton;",
    "Landroid/widget/LinearLayout;",
    "Landroid/widget/ScrollView;",
    "Landroid/widget/TextView;",
}


def patch_dex_compat(dex_bytes: bytes, dex_name: str, stub_storage_mgr: bool = False,
                     stub_track_processor: bool = False, patch_gallery: bool = False) -> bytes | None:
    """
    Patch a VivoCamera DEX in-place and recompute SHA-1 & Adler-32:
      1. NOP all invoke calls to Vivo-only AOSP `setNightMode(I)V` methods across
         Canvas/View/Button/LinearLayout/TextView/ImageButton/ScrollView.
      2. Optionally stub StorageCommonUtils->isExternalStorageManager()Z -> true,
         null-guard Vcf2VifImageEchoUtil->isImageEcho*(), and redirect Utils->viewUri()
         (in classes3.dex).
      3. Optionally stub TrackProcessor->{addTrackData,buildTrackEvent,uploadTrackEvent} ->
         return-void (in classes4.dex) so Vivo vcode analytics never NPE on photo shutter.
      4. Optionally redirect ActivityStartManager->{launchGalleryReview,gotoGalleryPic,
         gotoGalleryEditor} (in classes9.dex) to standard Android/GSI Gallery (Glimpse)
         using SystemProperties.get(action, "android.intent.action.VIEW") -> REVIEW +
         FLAG_GRANT_READ_URI_PERMISSION, and drop hardcoded setPackage("com.vivo.gallery").
    Returns None if no modifications were needed in this DEX.
    """
    dex = bytearray(dex_bytes)
    sc, soff, tc, toff, pc, poff, fc, foff, mc, moff, cdc, cdoff = struct.unpack_from("<12I", dex, 56)
    dstrs = []
    for i in range(sc):
        doff = struct.unpack_from("<I", dex, soff + i * 4)[0]
        _, p = _uleb128(dex, doff)
        e = dex.index(0, p)
        dstrs.append(dex[p:e].decode("utf-8", "replace"))
    types = [dstrs[struct.unpack_from("<I", dex, toff + i * 4)[0]] for i in range(tc)]
    target_mids = {
        i for i in range(mc)
        if types[struct.unpack_from("<H", dex, moff + i * 8)[0]] in NIGHT_MODE_TARGET_CLASSES
        and dstrs[struct.unpack_from("<I", dex, moff + i * 8 + 4)[0]] == "setNightMode"
    }

    def find_method(cls_t: str, mname: str, param_types: tuple[str, ...] | None = None) -> int:
        for i in range(mc):
            ci, pi, ni = struct.unpack_from("<HHI", dex, moff + i * 8)
            if types[ci] == cls_t and dstrs[ni] == mname:
                if param_types is None:
                    return i
                po = struct.unpack_from("<I", dex, poff + pi * 12 + 8)[0]
                if po:
                    pcnt = struct.unpack_from("<I", dex, po)[0]
                    pts = tuple(types[struct.unpack_from("<H", dex, po + 4 + k * 2)[0]] for k in range(pcnt))
                else:
                    pts = ()
                if pts == param_types:
                    return i
        raise AssertionError(f"Method {cls_t}->{mname}({param_types}) not found in {dex_name}")

    def find_field(cls_t: str, fname: str) -> int:
        for i in range(fc):
            ci, _, ni = struct.unpack_from("<HHI", dex, foff + i * 8)
            if types[ci] == cls_t and dstrs[ni] == fname:
                return i
        raise AssertionError(f"Field {cls_t}->{fname} not found in {dex_name}")

    gal_ids = None
    if patch_gallery:
        gal_ids = {
            "s_view": dstrs.index("android.intent.action.VIEW"),
            "s_vg": dstrs.index("com.vivo.gallery"),
            "t_intent": types.index("Landroid/content/Intent;"),
            "m_sp_get": find_method("Lcom/android/camera/utils/SystemProperties;", "get", ("Ljava/lang/String;", "Ljava/lang/String;")),
            "m_int_init2": find_method("Landroid/content/Intent;", "<init>", ("Ljava/lang/String;", "Landroid/net/Uri;")),
            "m_int_addf": find_method("Landroid/content/Intent;", "addFlags", ("I",)),
            "m_int_setpkg": find_method("Landroid/content/Intent;", "setPackage", ("Ljava/lang/String;",)),
            "m_start_act": find_method("Landroid/app/Activity;", "startActivityForResult", ("Landroid/content/Intent;", "I")),
            "f_is_jump": find_field("Lcom/android/camera/utils/Utils;", "isJumpGallery"),
        }
    if not target_mids and not stub_storage_mgr and not stub_track_processor and not patch_gallery:
        return None
    patched_nm = 0
    patched_esm = 0
    patched_echo = 0
    patched_track = 0
    patched_gal = 0
    for ci in range(cdc):
        off = cdoff + ci * 32
        cls_idx, _, _, _, _, _, cdata_off, _ = struct.unpack_from("<8I", dex, off)
        if cdata_off == 0:
            continue
        cname = types[cls_idx]
        p = cdata_off
        sf, p = _uleb128(dex, p)
        inf, p = _uleb128(dex, p)
        dm, p = _uleb128(dex, p)
        vm, p = _uleb128(dex, p)
        for _ in range(sf + inf):
            _, p = _uleb128(dex, p)
            _, p = _uleb128(dex, p)
        for cnt in (dm, vm):
            mid = 0
            for _ in range(cnt):
                diff, p = _uleb128(dex, p)
                mid += diff
                _, p = _uleb128(dex, p)
                code_off, p = _uleb128(dex, p)
                if code_off == 0:
                    continue
                _, _, _, _, _, insns_size = struct.unpack_from("<HHHHII", dex, code_off)
                insns = struct.unpack_from(f"<{insns_size}H", dex, code_off + 16)
                mci, _, mni = struct.unpack_from("<HHI", dex, moff + mid * 8)
                if stub_storage_mgr and cname == "Lcom/android/camera/utils/storageutils/StorageCommonUtils;" and dstrs[mni] == "isExternalStorageManager":
                    # Replace initial 2-unit instruction with: const/4 v0, 1; return v0
                    end_u = 0
                    while end_u < 2 and end_u < insns_size:
                        end_u += _insn_len(insns, end_u)
                    stub = [0x1012, 0x000F] + [0x0000] * (end_u - 2)
                    struct.pack_into(f"<{len(stub)}H", dex, code_off + 16, *stub)
                    print(f"  [{dex_name}] STUB {cname}->isExternalStorageManager()Z -> true ({len(stub)} units)")
                    patched_esm += 1
                    continue
                if stub_storage_mgr and cname == "Lcom/android/vcamera/core/vif/vcf/util/Vcf2VifImageEchoUtil;" and dstrs[mni] in ("isImageEcho", "isImageEcho_CR"):
                    # Now that cameraserver uses real vendorVcf/0, preserve genuine ImageEcho logic
                    # (needed by front-camera OFF_R2Y_FRONT_ECHO_OUT and ReferenceImageReader!), while
                    # making the 48-unit (VTotalCaptureResult) and 44-unit (CaptureResult) overloads
                    # null-safe by replacing their verbose StringBuilder/Logger.d block with an
                    # `if-eqz v2, return_false` guard.
                    if insns_size in (44, 48):
                        start_u = insns_size - 36  # 0x0c for 48-unit, 0x08 for 44-unit
                        # Verify stock instruction at start_u is invoke-virtual {v2}, Integer.intValue() (0x106e)
                        assert (insns[start_u] & 0xFF) == 0x6E and insns[start_u + 3] == 0x020A, \
                            f"Unexpected Vcf2VifImageEchoUtil.{dstrs[mni]} layout at +{start_u}"
                        orig_intval = list(insns[start_u : start_u + 4])
                        # if-eqz v2, +0x1e (-> const/4 v0, 0); invoke-virtual intValue; move-result v2; goto +0x14; nops
                        guard = [0x0238, 0x001E] + orig_intval + [0x1428] + [0x0000] * 19
                        assert len(guard) == 26
                        struct.pack_into("<26H", dex, code_off + 16 + start_u * 2, *guard)
                        print(f"  [{dex_name}] NULL-GUARD {cname}->{dstrs[mni]} ({insns_size}u @+{start_u})")
                        patched_echo += 1
                    continue
                if stub_track_processor and cname == "Lcom/vivo/trackprocessorimp/TrackProcessor;" and dstrs[mni] in ("addTrackData", "buildTrackEvent", "uploadTrackEvent"):
                    # Stub Vivo vcode big-data telemetry void entrypoints to return-void (0x000E):
                    # eliminates MainThread NPE in PhotoContext.lambda$addTrackCaptureParamOnCapturePrepare$0
                    # on every photo shutter tap (vcode service does not exist on AOSP GSI).
                    end_u = _insn_len(insns, 0)
                    stub = [0x000E] + [0x0000] * (end_u - 1)
                    struct.pack_into(f"<{len(stub)}H", dex, code_off + 16, *stub)
                    print(f"  [{dex_name}] STUB {cname}->{dstrs[mni]} -> return-void ({len(stub)} units)")
                    patched_track += 1
                    continue
                if patch_gallery and cname == "Lcom/android/camera/manager/ActivityStartManager;":
                    if dstrs[mni] == "launchGalleryReview" and insns_size == 30:
                        # Redirect com.vivo.gallery.*ACTION_VIEW -> SystemProperties.get(action, "android.intent.action.VIEW")
                        # (set to "android.provider.action.REVIEW" on GSI for full swipeable Glimpse album review!)
                        # + FLAG_ACTIVITY_NEW_DOCUMENT | FLAG_GRANT_READ_URI_PERMISSION (0x00080001).
                        g = gal_ids
                        body = [
                            0x001A, g["s_view"],                # 00: const-string v0, "android.intent.action.VIEW"
                            0x2071, g["m_sp_get"], 0x0001,      # 02: invoke-static {v1, v0}, SystemProperties.get(v1, v0)
                            0x010C,                             # 05: move-result-object v1
                            0x0022, g["t_intent"],              # 06: new-instance v0, Intent
                            0x3070, g["m_int_init2"], 0x0210,   # 08: invoke-direct {v0, v1, v2}, Intent.<init>(String, Uri)
                            0x0114, 0x0001, 0x0008,             # 0b: const v1, 0x00080001 (NEW_DOCUMENT | GRANT_READ_URI)
                            0x206E, g["m_int_addf"], 0x0010,    # 0e: invoke-virtual {v0, v1}, Intent.addFlags(I)
                            0x0213, 600,                        # 11: const/16 v2, 600
                            0x306E, g["m_start_act"], 0x0204,   # 13: invoke-virtual {v4, v0, v2}, Activity.startActivityForResult
                            0x1112,                             # 16: const/4 v1, 1
                            0x016A, g["f_is_jump"],             # 17: sput-boolean v1, Utils.isJumpGallery
                            0x000E,                             # 19: return-void
                            0x0000, 0x0000, 0x0000, 0x0000,     # 1a..1d: nop padding
                        ]
                        assert len(body) == 30
                        struct.pack_into("<30H", dex, code_off + 16, *body)
                        print(f"  [{dex_name}] REDIRECT {cname}->launchGalleryReview -> GSI Gallery (REVIEW/VIEW)")
                        patched_gal += 1
                        continue
                    if dstrs[mni] in ("gotoGalleryPic", "gotoGalleryEditor"):
                        # Drop hardcoded intent.setPackage("com.vivo.gallery") so PICK / CROP resolve to GSI Glimpse
                        i_g = 0
                        while i_g < insns_size:
                            op_g = insns[i_g] & 0xFF
                            l_g = _insn_len(insns, i_g)
                            if (op_g == 0x1A and insns[i_g + 1] == gal_ids["s_vg"]
                                    and i_g + 2 < insns_size and (insns[i_g + 2] & 0xFF) == 0x6E
                                    and insns[i_g + 3] == gal_ids["m_int_setpkg"]):
                                fo = code_off + 16 + (i_g + 2) * 2
                                dex[fo : fo + 6] = b"\x00\x00\x00\x00\x00\x00"
                                print(f"  [{dex_name}] NOP {cname}->{dstrs[mni]} @+{i_g + 2} (setPackage com.vivo.gallery)")
                                patched_gal += 1
                            i_g += l_g
                if not target_mids:
                    continue
                i = 0
                while i < insns_size:
                    op = insns[i] & 0xFF
                    l = _insn_len(insns, i)
                    if op in (0x6E, 0x6F, 0x70, 0x71, 0x72, 0x74, 0x75, 0x76, 0x77, 0x78) and l == 3:
                        if insns[i + 1] in target_mids:
                            t_cls = types[struct.unpack_from("<H", dex, moff + insns[i + 1] * 8)[0]]
                            fo = code_off + 16 + i * 2
                            dex[fo : fo + 6] = b"\x00\x00\x00\x00\x00\x00"
                            print(f"  [{dex_name}] NOP {types[mci]}->{dstrs[mni]} @+{i} ({t_cls}->setNightMode)")
                            patched_nm += 1
                    i += l

    if stub_storage_mgr:
        assert patched_esm == 1, f"Expected 1 StorageCommonUtils.isExternalStorageManager in {dex_name}, patched {patched_esm}"
        assert patched_echo == 2, f"Expected 2 null-guarded Vcf2VifImageEchoUtil.isImageEcho* in {dex_name}, patched {patched_echo}"
    if stub_track_processor:
        assert patched_track == 3, f"Expected 3 TrackProcessor void entrypoints in {dex_name}, patched {patched_track}"
    if patch_gallery:
        assert patched_gal == 3, f"Expected 3 ActivityStartManager gallery patches in {dex_name}, patched {patched_gal}"
    if patched_nm == 0 and patched_esm == 0 and patched_echo == 0 and patched_track == 0 and patched_gal == 0:
        return None
    dex[12:32] = hashlib.sha1(dex[32:]).digest()
    adler = zlib.adler32(dex[12:]) & 0xFFFFFFFF
    struct.pack_into("<I", dex, 8, adler)
    print(f"Patched {dex_name}: NOPed {patched_nm} setNightMode + stubbed {patched_esm} isExternalStorageManager + {patched_echo} isImageEcho + {patched_track} TrackProcessor + {patched_gal} Gallery, Adler32={adler:#010x}")
    return bytes(dex)


def _is_apk_signature_entry(name: str) -> bool:
    if not name.startswith("META-INF/"):
        return False
    base = name[len("META-INF/"):]
    if "/" in base:
        return False  # Preserve META-INF/services/*, META-INF/gradle/*, etc.!
    up = base.upper()
    return (
        up == "MANIFEST.MF"
        or up.endswith((".SF", ".RSA", ".DSA", ".EC"))
        or up.startswith("SIG-")
    )


def fast_repack_aligned_apk(src_apk: str, dst_aligned_apk: str,
                            replacements: dict[str, bytes] = None,
                            additions: dict[str, bytes] = None):
    """
    Copy raw compressed payloads directly from src_apk (zero re-compression overhead),
    strip ONLY APK v1 signature files in META-INF/ (preserving META-INF/services/* and
    *.kotlin_module!), apply replacements/additions, and 4-byte align all STORED entries.
    """
    replacements = replacements or {}
    additions = additions or {}

    cd_records = []
    out_pos = 0

    with open(src_apk, "rb") as fin, open(dst_aligned_apk, "wb") as fout:
        with zipfile.ZipFile(fin, "r") as zin:
            for info in zin.infolist():
                name = info.filename
                if _is_apk_signature_entry(name):
                    continue
                fn_bytes = name.encode("utf-8")

                if name in replacements:
                    raw = replacements[name]
                    crc = zlib.crc32(raw) & 0xFFFFFFFF
                    method = 0  # STORE patched DEX for fast mmap & 4-byte alignment
                    comp = raw
                    raw_len = len(raw)
                else:
                    # Read raw compressed bytes directly from local file header!
                    fin.seek(info.header_offset)
                    lh = fin.read(30)
                    sig, _, _, method, _, _, _, _, _, fn_len, ex_len = struct.unpack("<IHHHHHIIIHH", lh)
                    assert sig == 0x04034B50
                    fin.seek(info.header_offset + 30 + fn_len + ex_len)
                    comp = fin.read(info.compress_size)
                    crc = info.CRC
                    raw_len = info.file_size

                extra = b""
                if method == 0:
                    data_pos = out_pos + 30 + len(fn_bytes)
                    pad = (4 - (data_pos % 4)) % 4
                    extra = b"\x00" * pad

                lh_off = out_pos
                fout.write(struct.pack(
                    "<IHHHHHIIIHH",
                    0x04034B50, 20, 0, method, 0, 0, crc, len(comp), raw_len,
                    len(fn_bytes), len(extra),
                ))
                fout.write(fn_bytes)
                if extra:
                    fout.write(extra)
                fout.write(comp)
                out_pos += 30 + len(fn_bytes) + len(extra) + len(comp)

                cd_records.append(struct.pack(
                    "<IHHHHHHIIIHHHHHII",
                    0x02014B50, 20, 20, 0, method, 0, 0, crc, len(comp), raw_len,
                    len(fn_bytes), 0, 0, 0, 0, 0, lh_off,
                ) + fn_bytes)

        for name, raw in additions.items():
            fn_bytes = name.encode("utf-8")
            crc = zlib.crc32(raw) & 0xFFFFFFFF
            method = 0  # STORED + 4-byte aligned
            data_pos = out_pos + 30 + len(fn_bytes)
            pad = (4 - (data_pos % 4)) % 4
            extra = b"\x00" * pad
            lh_off = out_pos
            fout.write(struct.pack(
                "<IHHHHHIIIHH",
                0x04034B50, 20, 0, method, 0, 0, crc, len(raw), len(raw),
                len(fn_bytes), len(extra),
            ))
            fout.write(fn_bytes)
            if extra:
                fout.write(extra)
            fout.write(raw)
            out_pos += 30 + len(fn_bytes) + len(extra) + len(raw)

            cd_records.append(struct.pack(
                "<IHHHHHHIIIHHHHHII",
                0x02014B50, 20, 20, 0, method, 0, 0, crc, len(raw), len(raw),
                len(fn_bytes), 0, 0, 0, 0, 0, lh_off,
            ) + fn_bytes)

        cd_off = out_pos
        cd = b"".join(cd_records)
        fout.write(cd)
        fout.write(struct.pack(
            "<IHHHHIIH",
            0x06054B50, 0, 0, len(cd_records), len(cd_records), len(cd), cd_off, 0,
        ))


def main():
    ap = argparse.ArgumentParser(description="Patch & APK-v2 sign VivoCamera.apk and Vivo3rdAlgoService.apk")
    ap.add_argument("--camera-in", default="/system/app/VivoCamera/VivoCamera.apk")
    ap.add_argument("--algo-in", default="/system/app/Vivo3rdAlgoService/Vivo3rdAlgoService.apk")
    ap.add_argument("--camera-out", default=os.path.join(CAM_DIR, "Neo9VivoCamera.apk"))
    ap.add_argument("--algo-out", default=os.path.join(CAM_DIR, "Neo9Vivo3rdAlgoService.apk"))
    args = ap.parse_args()

    bo.ensure_key_and_cert()

    compat_dex_path = os.path.join(CAM_DIR, "classes15.dex")
    bcd.build_compat_dex(compat_dex_path)
    compat_dex = open(compat_dex_path, "rb").read()

    # Genuine OriginOS 4 android.hardware.vivocamera.* / IVivoCamera* / VIFResult classes
    vivocam_dex_path = os.path.join(CAM_DIR, "classes16.dex")
    if not os.path.exists(vivocam_dex_path):
        subprocess.check_call([sys.executable, os.path.join(CAM_DIR, "extract_vivocamera_dex.py")])
    vivocam_dex = open(vivocam_dex_path, "rb").read()

    with tempfile.TemporaryDirectory() as td:
        # 1. Patch & sign VivoCamera.apk (scan ALL classes*.dex for Vivo-only setNightMode calls!)
        replacements = {}
        with zipfile.ZipFile(args.camera_in, "r") as zin:
            replacements["AndroidManifest.xml"] = patch_manifest_target_sdk(zin.read("AndroidManifest.xml"), 33)
            for name in sorted(zin.namelist()):
                if name.endswith(".dex") and "/" not in name:
                    patched = patch_dex_compat(
                        zin.read(name),
                        name,
                        stub_storage_mgr=(name == "classes3.dex"),
                        stub_track_processor=(name == "classes4.dex"),
                        patch_gallery=(name == "classes9.dex"),
                    )
                    if patched is not None:
                        replacements[name] = patched
        aligned_cam = os.path.join(td, "cam_aligned.apk")
        fast_repack_aligned_apk(
            args.camera_in, aligned_cam,
            replacements=replacements,
            additions={"classes15.dex": compat_dex, "classes16.dex": vivocam_dex},
        )
        bo.sign_apk_v2(aligned_cam, args.camera_out)
        badging = subprocess.check_output(["aapt", "dump", "badging", args.camera_out], text=True)
        assert "targetSdkVersion:'33'" in badging, "targetSdkVersion:'33' verification failed!"
        print(f"Built & APK-v2 signed: {args.camera_out} ({os.path.getsize(args.camera_out)} bytes, targetSdk=33)")

        # 2. Re-sign Vivo3rdAlgoService.apk with same Neo9 key (shared seinfo -> platform_app)
        if os.path.exists(args.algo_in):
            aligned_algo = os.path.join(td, "algo_aligned.apk")
            fast_repack_aligned_apk(args.algo_in, aligned_algo)
            bo.sign_apk_v2(aligned_algo, args.algo_out)
            print(f"Built & APK-v2 signed: {args.algo_out} ({os.path.getsize(args.algo_out)} bytes)")


if __name__ == "__main__":
    main()
