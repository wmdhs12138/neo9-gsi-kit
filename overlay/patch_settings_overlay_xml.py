#!/usr/bin/env python3
"""
Extract res/xml/screen_refresh_rate.xml from the target GSI's Settings.apk,
strip `min_refresh_rate` and `extreme_refresh_rate` so only the single
OriginOS-style refresh-rate selector (`peak_refresh_rate`, showing 60/72/90/
120/144 Hz) and `per_app_refresh_rate` remain, append a native SettingsLib
FooterPreference explaining that 144 Hz does not support 8T LTPO, remap
Settings package resource IDs (0x7f -> 0x02), inject a RES_TABLE_LIBRARY_TYPE
(0x02 -> com.android.settings) shared-library table into Neo9SettingsOverlay.apk's
resources.arsc, and re-align/APK-v2 sign the overlay.
"""
import os
import struct
import sys
import tempfile
import zipfile

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, SCRIPT_DIR)
from build_overlay import ensure_key_and_cert, sign_apk_v2, zipalign_apk  # noqa: E402

STRIP_KEYS = {"min_refresh_rate", "extreme_refresh_rate"}
KEEP_REQUIRED = {"peak_refresh_rate", "per_app_refresh_rate"}
SETTINGS_PKG = "com.android.settings"
FOOTER_CLASS = "com.android.settingslib.widget.FooterPreference"
FOOTER_KEY = "neo9_refresh_rate_footer"


def _encode_pool_len_utf8(val: int) -> bytes:
    if val >= 0x80:
        return bytes([0x80 | ((val >> 8) & 0x7F), val & 0xFF])
    return bytes([val])


def _encode_pool_len_utf16(val: int) -> bytes:
    if val >= 0x8000:
        return struct.pack("<HH", 0x8000 | ((val >> 16) & 0x7FFF), val & 0xFFFF)
    return struct.pack("<H", val)


def append_strings_to_string_pool(chunk: bytes, extra_strings: list[str]) -> tuple[bytes, list[int]]:
    str_count, style_count, flags, strings_start, styles_start = struct.unpack_from("<5I", chunk, 8)
    if style_count != 0 or styles_start != 0:
        raise ValueError(f"Unsupported styled StringPool (styleCount={style_count}, stylesStart={styles_start})")

    is_utf8 = bool(flags & 0x100)
    offsets = list(struct.unpack_from(f"<{str_count}I", chunk, 28))

    # Find exact unpadded end of existing string data before appending.
    data_end = 0
    for off in offsets:
        pos = strings_start + off
        if is_utf8:
            pos += 2 if (chunk[pos] & 0x80) else 1
            b0 = chunk[pos]
            if b0 & 0x80:
                blen = ((b0 & 0x7F) << 8) | chunk[pos + 1]
                pos += 2
            else:
                blen = b0
                pos += 1
            end = (pos + blen + 1) - strings_start
        else:
            u0 = struct.unpack_from("<H", chunk, pos)[0]
            if u0 & 0x8000:
                slen = ((u0 & 0x7FFF) << 16) | struct.unpack_from("<H", chunk, pos + 2)[0]
                pos += 4
            else:
                slen = u0
                pos += 2
            end = (pos + slen * 2 + 2) - strings_start
        if end > data_end:
            data_end = end

    raw_data = bytearray(chunk[strings_start : strings_start + data_end])
    new_indices: list[int] = []
    for idx, s in enumerate(extra_strings):
        offsets.append(len(raw_data))
        new_indices.append(str_count + idx)
        u16_len = len(s.encode("utf-16le")) // 2
        if is_utf8:
            u8_bytes = s.encode("utf-8")
            raw_data += _encode_pool_len_utf8(u16_len) + _encode_pool_len_utf8(len(u8_bytes)) + u8_bytes + b"\x00"
        else:
            raw_data += _encode_pool_len_utf16(u16_len) + s.encode("utf-16le") + b"\x00\x00"

    while len(raw_data) % 4 != 0:
        raw_data.append(0)

    new_str_count = str_count + len(extra_strings)
    new_strings_start = strings_start + 4 * len(extra_strings)
    new_flags = flags & ~0x01  # Clear SORTED_FLAG
    new_sz = 28 + 4 * new_str_count + len(raw_data)

    new_chunk = bytearray()
    new_chunk += struct.pack("<HHIIIIII", 0x0001, 28, new_sz, new_str_count, 0, new_flags, new_strings_start, 0)
    new_chunk += struct.pack(f"<{new_str_count}I", *offsets)
    new_chunk += raw_data
    return bytes(new_chunk), new_indices


def parse_string_pool(chunk: bytes) -> list[str]:
    str_count, _, flags, strings_start, _ = struct.unpack_from("<5I", chunk, 8)
    is_utf8 = bool(flags & 0x100)
    offsets = struct.unpack_from(f"<{str_count}I", chunk, 28)
    strings = []
    for off in offsets:
        pos = strings_start + off
        if is_utf8:
            # Skip UTF-16 char count (1 or 2 bytes)
            if chunk[pos] & 0x80:
                pos += 2
            else:
                pos += 1
            # Read UTF-8 byte count (1 or 2 bytes)
            b0 = chunk[pos]
            if b0 & 0x80:
                blen = ((b0 & 0x7F) << 8) | chunk[pos + 1]
                pos += 2
            else:
                blen = b0
                pos += 1
            strings.append(chunk[pos : pos + blen].decode("utf-8", "replace"))
        else:
            u0 = struct.unpack_from("<H", chunk, pos)[0]
            if u0 & 0x8000:
                u1 = struct.unpack_from("<H", chunk, pos + 2)[0]
                slen = ((u0 & 0x7FFF) << 16) | u1
                pos += 4
            else:
                slen = u0
                pos += 2
            strings.append(chunk[pos : pos + slen * 2].decode("utf-16le", "replace"))
    return strings


def _pack_footer_preference(
    tag_idx: int,
    key_idx: int,
    title_resid: int,
    android_ns_idx: int,
    title_name_idx: int,
    key_name_idx: int,
    settings_ns_idx: int | None,
    searchable_name_idx: int | None,
) -> bytes:
    attrs: list[tuple[int, int, int, int, int, int]] = [
        (0x010101E1, android_ns_idx, title_name_idx, 0xFFFFFFFF, 0x01, title_resid),
        (0x010101E8, android_ns_idx, key_name_idx, key_idx, 0x03, key_idx),
    ]
    if settings_ns_idx is not None and searchable_name_idx is not None:
        attrs.append((0x02040567, settings_ns_idx, searchable_name_idx, 0xFFFFFFFF, 0x12, 0))
    attrs.sort(key=lambda item: item[0])

    start_sz = 36 + 20 * len(attrs)
    start_chunk = bytearray(
        struct.pack(
            "<HHIIIIIHHHHHH",
            0x0102,
            16,
            start_sz,
            0,
            0xFFFFFFFF,
            0xFFFFFFFF,
            tag_idx,
            20,
            20,
            len(attrs),
            0,
            0,
            0,
        )
    )
    for _, ns_idx, name_idx, raw_val, dtype, data in attrs:
        start_chunk += struct.pack("<IIIHBBI", ns_idx, name_idx, raw_val, 8, 0, dtype, data)

    end_chunk = struct.pack("<HHIIIII", 0x0103, 16, 24, 0, 0xFFFFFFFF, 0xFFFFFFFF, tag_idx)
    return bytes(start_chunk) + end_chunk


def patch_screen_refresh_axml(axml: bytes) -> bytes:
    if len(axml) < 8 or struct.unpack_from("<HH", axml, 0) != (0x0003, 8):
        raise ValueError("Invalid AXML header in Settings.apk res/xml/screen_refresh_rate.xml")

    out = bytearray(axml[:8])
    pos = 8
    strings: list[str] = []
    res_map: list[int] = []
    stripped_keys: set[str] = set()
    kept_keys: set[str] = set()
    skip_depth = 0
    depth = 0

    footer_tag_idx = -1
    footer_key_idx = -1
    title_name_idx = -1
    key_name_idx = -1
    searchable_name_idx: int | None = None
    android_ns_idx: int | None = None
    settings_ns_idx: int | None = None
    footer_title_resid = 0
    footer_inserted = False

    while pos < len(axml):
        ctype, hsz, sz = struct.unpack_from("<HHI", axml, pos)
        if sz < 8 or pos + sz > len(axml):
            raise ValueError(f"Corrupt AXML chunk at {pos}: type=0x{ctype:04x} sz={sz}")
        chunk = bytearray(axml[pos : pos + sz])
        pos += sz

        if ctype == 0x0001:  # RES_STRING_POOL_TYPE
            new_sp, extra_idxs = append_strings_to_string_pool(bytes(chunk), [FOOTER_CLASS, FOOTER_KEY])
            footer_tag_idx, footer_key_idx = extra_idxs
            strings = parse_string_pool(new_sp)
            out += new_sp
            continue

        if ctype == 0x0180:  # RES_XML_RESOURCE_MAP_TYPE
            count = (sz - hsz) // 4
            res_map = list(struct.unpack_from(f"<{count}I", chunk, hsz))
            for i, rid in enumerate(res_map):
                if (rid >> 24) == 0x7F:
                    res_map[i] = (0x02 << 24) | (rid & 0x00FFFFFF)
                    struct.pack_into("<I", chunk, hsz + i * 4, res_map[i])
            title_name_idx = res_map.index(0x010101E1)
            key_name_idx = res_map.index(0x010101E8)
            for i in range(min(len(res_map), len(strings))):
                if strings[i] == "searchable" and (res_map[i] >> 24) == 0x02:
                    searchable_name_idx = i
                    break
            out += chunk
            continue

        if ctype == 0x0102:  # RES_XML_START_ELEMENT_TYPE
            if skip_depth > 0:
                skip_depth += 1
                continue

            attr_start, attr_size, attr_count = struct.unpack_from("<HHH", chunk, hsz + 8)
            attrs_base = hsz + attr_start
            pref_key = None
            elem_summary_resid = 0
            for i in range(attr_count):
                aoff = attrs_base + i * attr_size
                ns_idx, attr_name_idx, raw_val_idx = struct.unpack_from("<III", chunk, aoff)
                dtype = chunk[aoff + 15]
                data = struct.unpack_from("<I", chunk, aoff + 16)[0]

                attr_name = strings[attr_name_idx] if attr_name_idx < len(strings) else ""
                attr_rid = res_map[attr_name_idx] if attr_name_idx < len(res_map) else 0
                if android_ns_idx is None and attr_rid in (0x010101E1, 0x010101E8):
                    android_ns_idx = ns_idx
                if settings_ns_idx is None and (attr_rid >> 24) == 0x02:
                    settings_ns_idx = ns_idx

                if (attr_name == "key" or attr_rid == 0x010101E8) and dtype == 0x03:
                    val_idx = data if data < len(strings) else raw_val_idx
                    if val_idx < len(strings):
                        pref_key = strings[val_idx]

                # Remap Settings package references (0x7fxxxxxx -> 0x02xxxxxx)
                if dtype in (0x01, 0x02) and (data >> 24) == 0x7F:
                    data = (0x02 << 24) | (data & 0x00FFFFFF)
                    struct.pack_into("<I", chunk, aoff + 16, data)

                if (attr_name == "summary" or attr_rid == 0x010101E9) and dtype == 0x01:
                    elem_summary_resid = data

            if pref_key in STRIP_KEYS:
                if pref_key == "extreme_refresh_rate" and elem_summary_resid:
                    footer_title_resid = elem_summary_resid
                stripped_keys.add(pref_key)
                skip_depth = 1
                continue
            if pref_key:
                kept_keys.add(pref_key)
            depth += 1
            out += chunk
            continue

        if ctype == 0x0103:  # RES_XML_END_ELEMENT_TYPE
            if skip_depth > 0:
                skip_depth -= 1
                continue
            if depth == 1:
                if footer_title_resid == 0 or android_ns_idx is None or title_name_idx < 0 or key_name_idx < 0:
                    raise RuntimeError(
                        f"Missing FooterPreference metadata (title_resid=0x{footer_title_resid:x}, "
                        f"android_ns={android_ns_idx}, title_idx={title_name_idx}, key_idx={key_name_idx})"
                    )
                out += _pack_footer_preference(
                    footer_tag_idx,
                    footer_key_idx,
                    footer_title_resid,
                    android_ns_idx,
                    title_name_idx,
                    key_name_idx,
                    settings_ns_idx,
                    searchable_name_idx,
                )
                footer_inserted = True
            depth -= 1
            out += chunk
            continue

        if skip_depth > 0:
            continue
        out += chunk

    if stripped_keys != STRIP_KEYS:
        raise RuntimeError(f"Expected to strip {sorted(STRIP_KEYS)}, actually stripped {sorted(stripped_keys)}")
    if not KEEP_REQUIRED.issubset(kept_keys):
        raise RuntimeError(f"Required keys {sorted(KEEP_REQUIRED)} not kept (kept={sorted(kept_keys)})")
    if not footer_inserted:
        raise RuntimeError("FooterPreference was not inserted before root PreferenceScreen end element")

    struct.pack_into("<I", out, 4, len(out))
    return bytes(out)


def inject_shared_library_table(arsc: bytes, pkg_id: int = 0x02, pkg_name: str = SETTINGS_PKG) -> bytes:
    if len(arsc) < 12 or struct.unpack_from("<HH", arsc, 0) != (0x0002, 12):
        raise ValueError("Invalid resources.arsc RES_TABLE_TYPE header")

    table_sz = struct.unpack_from("<I", arsc, 4)[0]
    pos = 12
    while pos < table_sz:
        ctype, hsz, sz = struct.unpack_from("<HHI", arsc, pos)
        if ctype == 0x0200:  # RES_TABLE_PACKAGE_TYPE
            pkg_end = pos + sz
            # Check if RES_TABLE_LIBRARY_TYPE (0x0203) already exists
            sub = pos + hsz
            while sub < pkg_end:
                s_type, _, s_sz = struct.unpack_from("<HHI", arsc, sub)
                if s_type == 0x0203:
                    return arsc
                sub += s_sz

            name_utf16 = pkg_name.encode("utf-16le")
            if len(name_utf16) >= 256:
                raise ValueError("Package name exceeds 128 UTF-16 code units")
            name_padded = name_utf16 + b"\x00" * (256 - len(name_utf16))
            lib_chunk = struct.pack("<HHIII", 0x0203, 12, 272, 1, pkg_id) + name_padded
            assert len(lib_chunk) == 272

            out = bytearray(arsc[:pkg_end]) + lib_chunk + arsc[pkg_end:]
            struct.pack_into("<I", out, pos + 4, sz + len(lib_chunk))
            struct.pack_into("<I", out, 4, len(out))
            return bytes(out)
        pos += sz

    raise RuntimeError("RES_TABLE_PACKAGE_TYPE not found in overlay resources.arsc")


def patch_overlay(settings_apk: str, overlay_apk: str) -> None:
    with zipfile.ZipFile(settings_apk, "r") as zs:
        raw_axml = zs.read("res/xml/screen_refresh_rate.xml")
    patched_axml = patch_screen_refresh_axml(raw_axml)

    ensure_key_and_cert()
    with tempfile.TemporaryDirectory() as td:
        tmp_raw = os.path.join(td, "raw.apk")
        tmp_aligned = os.path.join(td, "aligned.apk")

        with zipfile.ZipFile(overlay_apk, "r") as zin, zipfile.ZipFile(tmp_raw, "w") as zout:
            found_xml = False
            for info in zin.infolist():
                data = zin.read(info.filename)
                if info.filename == "res/xml/screen_refresh_rate.xml":
                    data = patched_axml
                    found_xml = True
                elif info.filename == "resources.arsc":
                    data = inject_shared_library_table(data)
                compress = zipfile.ZIP_STORED if info.filename == "resources.arsc" else zipfile.ZIP_DEFLATED
                zout.writestr(info, data, compress_type=compress)
            if not found_xml:
                raise RuntimeError("Overlay APK missing placeholder res/xml/screen_refresh_rate.xml")

        zipalign_apk(tmp_raw, tmp_aligned)
        sign_apk_v2(tmp_aligned, overlay_apk)

    print(
        f"Patched Settings RRO {overlay_apk}: stripped {sorted(STRIP_KEYS)}, "
        f"kept {sorted(KEEP_REQUIRED)}, injected FooterPreference, "
        f"added RES_TABLE_LIBRARY_TYPE (0x02 -> {SETTINGS_PKG})"
    )


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(f"Usage: {sys.argv[0]} <Settings.apk> <Neo9SettingsOverlay.apk>")
    patch_overlay(sys.argv[1], sys.argv[2])
