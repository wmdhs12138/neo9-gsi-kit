#!/usr/bin/env python3
"""
Repack PHH Treble ims-caf-u.apk (org.codeaurora.ims) for any GSI signing key:
  1. Deterministically removes `android:sharedUserId="android.uid.phone"` and all
     `<protected-broadcast>` elements from binary AndroidManifest.xml so PackageManager
     loads /system/system_ext/priv-app/ims/ims.apk under our signing key without AMS
     marking QTI broadcasts as phone-UID-only.
  2. NOPs all void-returning Context.sendBroadcast* / sendBroadcastAsUser /
     sendStickyBroadcastAsUser calls inside classes.dex (including
     com.android.imsconnection.DISCONNECTED which is <protected-broadcast> in
     framework-res.apk, and org.codeaurora.VOIP_VOPS_SSAC_STATUS) and recomputes
     the DEX SHA-1 signature & Adler-32 checksum. Standard AOSP ImsService Binder
     callbacks (MmTelFeature / ImsRegistrationImplBase / ImsCallSessionImplBase)
     are completely untouched.
  3. Strips Android-13 vendor .so files from lib/ (replaced by Android-17 JNI stubs),
     4-byte aligns resources.arsc, and signs with APK Signature Scheme v2.
"""
import hashlib
import os
import struct
import subprocess
import sys
import tempfile
import zipfile
import zlib

TEL_DIR = os.path.dirname(os.path.abspath(__file__))
KIT_DIR = os.path.dirname(TEL_DIR)
sys.path.insert(0, os.path.join(KIT_DIR, "overlay"))
import build_overlay as bo  # noqa: E402

IN_APK = os.path.join(TEL_DIR, "ims-caf-u.apk")
OUT_APK = os.path.join(TEL_DIR, "Neo9QtiIms.apk")


def read_axml_strings(buf: bytes) -> list[str]:
    """Read string pool at offset 8 of Android binary XML."""
    ctype, hdr_sz, chunk_sz = struct.unpack_from("<HHI", buf, 8)
    assert ctype == 0x0001, f"Expected RES_STRING_POOL_TYPE, got {ctype:#x}"
    str_cnt, style_cnt, flags, str_start, _ = struct.unpack_from("<IIIII", buf, 16)
    is_utf8 = bool(flags & (1 << 8))
    base = 8 + str_start
    out = []
    for i in range(str_cnt):
        off = struct.unpack_from("<I", buf, 36 + i * 4)[0]
        p = base + off
        if is_utf8:
            if buf[p] & 0x80: p += 2
            else: p += 1
            if buf[p] & 0x80:
                blen = ((buf[p] & 0x7f) << 8) | buf[p + 1]
                p += 2
            else:
                blen = buf[p]
                p += 1
            out.append(buf[p : p + blen].decode("utf-8", "ignore"))
        else:
            clen = struct.unpack_from("<H", buf, p)[0]
            p += 2
            if clen & 0x8000:
                clen = ((clen & 0x7fff) << 16) | struct.unpack_from("<H", buf, p)[0]
                p += 2
            out.append(buf[p : p + clen * 2].decode("utf-16le", "ignore"))
    return out


def patch_manifest(axml: bytes) -> bytes:
    """Remove android:sharedUserId on <manifest> and all <protected-broadcast> elements."""
    rtype, rhdr, rsize = struct.unpack_from("<HHI", axml, 0)
    assert rtype == 0x0003 and rsize == len(axml), "Invalid AXML root header"
    strings = read_axml_strings(axml)

    out = bytearray(axml[:rhdr])
    pos = rhdr
    removed_uid = False
    removed_pb = 0
    skip_depth = 0

    while pos < len(axml):
        ctype, chdr, csize = struct.unpack_from("<HHI", axml, pos)
        chunk = axml[pos : pos + csize]

        if ctype == 0x0102:  # RES_XML_START_ELEMENT_TYPE
            ns_idx, name_idx, attr_start, attr_size, attr_cnt, id_idx, cls_idx, sty_idx = struct.unpack_from(
                "<IIHHHHHH", chunk, chdr
            )
            tag_name = strings[name_idx] if name_idx < len(strings) else ""
            if skip_depth > 0:
                skip_depth += 1
                pos += csize
                continue
            if tag_name == "protected-broadcast":
                skip_depth = 1
                removed_pb += 1
                pos += csize
                continue
            if tag_name == "manifest":
                assert attr_size == 20, f"Unexpected attr_size {attr_size}"
                attrs_base = chdr + attr_start
                rm_idx = None
                for i in range(attr_cnt):
                    a_off = attrs_base + i * 20
                    a_ns, a_name, a_raw, a_val_sz, _, a_val_type, a_val_data = struct.unpack_from("<IIIHBBI", chunk, a_off)
                    aname_s = strings[a_name] if a_name < len(strings) else ""
                    if aname_s == "sharedUserId":
                        rm_idx = i
                        break
                assert rm_idx is not None, "sharedUserId attribute not found on <manifest>"
                rm_off = attrs_base + rm_idx * 20
                new_chunk = bytearray(chunk[:rm_off] + chunk[rm_off + 20 :])
                for idx_off, val in [(chdr + 14, id_idx), (chdr + 16, cls_idx), (chdr + 18, sty_idx)]:
                    if val > rm_idx + 1:
                        struct.pack_into("<H", new_chunk, idx_off, val - 1)
                    elif val == rm_idx + 1:
                        struct.pack_into("<H", new_chunk, idx_off, 0)
                struct.pack_into("<H", new_chunk, chdr + 12, attr_cnt - 1)
                struct.pack_into("<I", new_chunk, 4, csize - 20)
                chunk = bytes(new_chunk)
                removed_uid = True
        elif ctype == 0x0103:  # RES_XML_END_ELEMENT_TYPE
            if skip_depth > 0:
                skip_depth -= 1
                pos += csize
                continue

        out.extend(chunk)
        pos += csize

    assert removed_uid, "<manifest> sharedUserId not stripped"
    assert removed_pb == 6, f"Expected 6 <protected-broadcast> tags, removed {removed_pb}"
    struct.pack_into("<I", out, 4, len(out))
    print(f"Manifest patched: removed sharedUserId + {removed_pb} <protected-broadcast> tags")
    return bytes(out)


def _uleb128(b: bytes, p: int) -> tuple[int, int]:
    v = 0
    s = 0
    while True:
        x = b[p]
        p += 1
        v |= (x & 0x7f) << s
        if not (x & 0x80):
            return v, p
        s += 7


def _dalvik_insn_len(insns: tuple[int, ...], i: int) -> int:
    op = insns[i] & 0xff
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
        0x01, 0x04, 0x07, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12,
        0x1d, 0x1e, 0x21, 0x27, 0x28, 0x3e, 0x3f, 0x40, 0x41, 0x42, 0x43,
        0x73, 0x79, 0x7a,
    ) or (0x7b <= op <= 0x8f) or (0xb0 <= op <= 0xcf) or (0xe3 <= op <= 0xf9):
        return 1
    if op in (
        0x02, 0x05, 0x08, 0x13, 0x15, 0x16, 0x19, 0x1a, 0x1c, 0x1f, 0x20, 0x22,
        0x23, 0x29, 0x2d, 0x2e, 0x2f, 0x30, 0x31,
    ) or (0x32 <= op <= 0x3d) or (0x44 <= op <= 0x6d) or (0x90 <= op <= 0xaf) or (0xd0 <= op <= 0xe2):
        return 2
    if op in (
        0x03, 0x06, 0x09, 0x14, 0x17, 0x1b, 0x24, 0x25, 0x26, 0x2a, 0x2b, 0x2c,
    ) or (0x6e <= op <= 0x72) or (0x74 <= op <= 0x78) or (0xfa <= op <= 0xff):
        return 3
    if op == 0x18:
        return 5
    return 1


def patch_dex(dex_bytes: bytes) -> bytes:
    """
    Patch classes.dex in-place and recompute SHA-1 & Adler-32:
      1. NOP all void Context.sendBroadcast* / sendStickyBroadcast* invoke calls
         inside Lorg/codeaurora/ims/* (prevents SecurityException on protected broadcasts).
      2. Stub all Java wrapper methods in ImsMedia & ImsCamera that invoke native JNI
         methods (prevents UnsatisfiedLinkError such as ImsMedia.nativeRequestRtpDataUsage
         on incoming MT VoLTE/VoNR calls, because ImsMedia.<clinit> never loads
         libimsmedia_jni.so in ims-caf-u.apk).
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
    protos = [types[struct.unpack_from("<I", dex, poff + i * 12 + 4)[0]] for i in range(pc)]

    broadcast_mids = {}
    for i in range(mc):
        ci, pi, ni = struct.unpack_from("<HHI", dex, moff + i * 8)
        if types[ci] == "Landroid/content/Context;" and "Broadcast" in dstrs[ni]:
            assert protos[pi] == "V", f"Unexpected non-void broadcast method {dstrs[ni]}: {protos[pi]}"
            broadcast_mids[i] = dstrs[ni]

    # Collect all ACC_NATIVE method IDs (all reside in ImsMedia & ImsCamera)
    native_mids = set()
    for ci in range(cdc):
        off = cdoff + ci * 32
        cls_idx, acc, sup, iface, src, ann, cdata_off, sval = struct.unpack_from("<8I", dex, off)
        if cdata_off == 0:
            continue
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
                macc, p = _uleb128(dex, p)
                _, p = _uleb128(dex, p)
                if macc & 0x0100:  # ACC_NATIVE
                    native_mids.add(mid)

    patched_bc = 0
    stubbed_vt = 0
    for ci in range(cdc):
        off = cdoff + ci * 32
        cls_idx, acc, sup, iface, src, ann, cdata_off, sval = struct.unpack_from("<8I", dex, off)
        if cdata_off == 0:
            continue
        cls_name = types[cls_idx]
        is_qti_ims = cls_name.startswith("Lorg/codeaurora/ims/")
        is_vt_jni = cls_name in ("Lcom/qualcomm/ims/vt/ImsMedia;", "Lcom/qualcomm/ims/vt/ImsCamera;")
        if not (is_qti_ims or is_vt_jni):
            continue
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
                regs, ins, outs, tries, dbg, insns_size = struct.unpack_from("<HHHHII", dex, code_off)
                insns = struct.unpack_from(f"<{insns_size}H", dex, code_off + 16)
                has_native_call = False
                i = 0
                while i < insns_size:
                    op = insns[i] & 0xff
                    l = _dalvik_insn_len(insns, i)
                    if op in (0x6e, 0x6f, 0x70, 0x71, 0x72, 0x74, 0x75, 0x76, 0x77, 0x78) and l == 3:
                        callee = insns[i + 1]
                        if is_qti_ims and callee in broadcast_mids:
                            file_off = code_off + 16 + i * 2
                            dex[file_off : file_off + 6] = b"\x00\x00\x00\x00\x00\x00"
                            m_ci, _, m_ni = struct.unpack_from("<HHI", dex, moff + mid * 8)
                            print(f"  NOP {types[m_ci]}->{dstrs[m_ni]} @+{i} ({broadcast_mids[callee]})")
                            patched_bc += 1
                        if is_vt_jni and callee in native_mids:
                            has_native_call = True
                    i += l

                if is_vt_jni and has_native_call:
                    m_ci, m_pi, m_ni = struct.unpack_from("<HHI", dex, moff + mid * 8)
                    ret_t = protos[m_pi]
                    if ret_t == "V":
                        stub = [0x000e]  # return-void
                    elif ret_t in ("Z", "F"):
                        stub = [0x0012, 0x000f]  # const/4 v0, 0; return v0
                    elif ret_t == "I":
                        stub = [0xf012, 0x000f]  # const/4 v0, -1; return v0 (DPL_INIT_FAILURE)
                    elif ret_t.startswith("L") or ret_t.startswith("["):
                        stub = [0x0012, 0x0011]  # const/4 v0, 0; return-object v0 (null)
                    else:
                        raise AssertionError(f"Unsupported VT stub return type {ret_t} for {types[m_ci]}->{dstrs[m_ni]}")
                    assert regs >= 1, f"Method {types[m_ci]}->{dstrs[m_ni]} has regs={regs}"
                    # Pad with nop (0x0000) to the next original instruction boundary so dead-code offsets stay aligned
                    end_u = 0
                    while end_u < len(stub) and end_u < insns_size:
                        end_u += _dalvik_insn_len(insns, end_u)
                    assert end_u >= len(stub) and end_u <= insns_size
                    padded = stub + [0x0000] * (end_u - len(stub))
                    struct.pack_into(f"<{len(padded)}H", dex, code_off + 16, *padded)
                    print(f"  STUB {types[m_ci]}->{dstrs[m_ni]}:{ret_t} ({len(padded)} units)")
                    stubbed_vt += 1

    assert patched_bc == 8, f"Expected 8 broadcast calls in classes.dex, patched {patched_bc}"
    assert stubbed_vt == 27, f"Expected 27 VT JNI caller methods in classes.dex, stubbed {stubbed_vt}"
    # Recompute DEX SHA-1 signature (bytes 32..end) and Adler-32 checksum (bytes 12..end)
    sha1 = hashlib.sha1(dex[32:]).digest()
    dex[12:32] = sha1
    adler = zlib.adler32(dex[12:]) & 0xffffffff
    struct.pack_into("<I", dex, 8, adler)
    print(f"DEX patched: NOPed {patched_bc} broadcasts + stubbed {stubbed_vt} VT JNI wrappers, Adler32={adler:#010x}")
    return bytes(dex)


def main():
    bo.ensure_key_and_cert()
    with tempfile.TemporaryDirectory() as td:
        patched_raw = os.path.join(td, "raw.apk")
        aligned_apk = os.path.join(td, "aligned.apk")
        with zipfile.ZipFile(IN_APK, "r") as zin, zipfile.ZipFile(patched_raw, "w") as zout:
            for info in zin.infolist():
                if info.filename.startswith("META-INF/") or info.filename.startswith("lib/"):
                    continue
                data = zin.read(info.filename)
                if info.filename == "AndroidManifest.xml":
                    data = patch_manifest(data)
                elif info.filename == "classes.dex":
                    data = patch_dex(data)
                compress = zipfile.ZIP_STORED if info.filename == "resources.arsc" else zipfile.ZIP_DEFLATED
                zout.writestr(info.filename, data, compress_type=compress)
        bo.zipalign_apk(patched_raw, aligned_apk)
        bo.sign_apk_v2(aligned_apk, OUT_APK)

    xml = subprocess.check_output(["aapt", "dump", "xmltree", OUT_APK, "AndroidManifest.xml"], text=True)
    assert "sharedUserId" not in xml, "sharedUserId still present!"
    assert "protected-broadcast" not in xml, "protected-broadcast still present!"
    assert ".ImsService" in xml and "org.codeaurora.ims" in xml, "Manifest damaged!"
    print(f"Built & APK-v2 signed: {OUT_APK} ({os.path.getsize(OUT_APK)} bytes)")


if __name__ == "__main__":
    main()
