#!/usr/bin/env python3
"""Extract /system/framework/vivo-framework.jar classes.dex as classes16.dex for
Neo9VivoCamera.apk, stripping ONLY TYPE_HIDDENAPI_CLASS_DATA_ITEM (0xF000) so:
  1. All genuine OriginOS 4 android.hardware.vivocamera.* / IVivoCamera* /
     VIFResult / VivoReprocess* classes are available verbatim for VCF2 Binder IPC
     with vivocameraserver (media.vivocamera).
  2. Zero bootclasspath hidden-API flags block reflection or ART verification.
  3. classes15.dex (placed BEFORE classes16.dex in Neo9VivoCamera.apk) shadows
     all safe A17 compat stubs (VLog, ThemeIconManager, VivoPerfManager,
     PopupCameraManager, VivoProxCaliManager, etc.) so no Class.forName() can
     ever hit a missing vivo.app.* superclass.
  4. Verified offline with ART dexdump -f (strict DEX verifier).
"""
import hashlib
import os
import struct
import subprocess
import zipfile
import zlib

SRC_JAR = "/system/framework/vivo-framework.jar"
OUT_DEX = os.path.join(os.path.dirname(os.path.abspath(__file__)), "classes16.dex")


def main():
    with zipfile.ZipFile(SRC_JAR, "r") as z:
        raw = bytearray(z.read("classes.dex"))

    map_off = struct.unpack_from("<I", raw, 52)[0]
    map_count = struct.unpack_from("<I", raw, map_off)[0]
    raw_items = [
        struct.unpack_from("<HHII", raw, map_off + 4 + i * 12)
        for i in range(map_count)
    ]

    new_items = []
    stripped_hiddenapi = 0
    for idx, (mtype, unused, msize, moffset) in enumerate(raw_items):
        if mtype == 0xF000:
            next_off = raw_items[idx + 1][3] if idx + 1 < len(raw_items) else len(raw)
            raw[moffset:next_off] = b"\x00" * (next_off - moffset)
            stripped_hiddenapi += 1
            continue
        new_items.append((mtype, unused, msize, moffset))

    struct.pack_into("<I", raw, map_off, len(new_items))
    for i, item in enumerate(new_items):
        struct.pack_into("<HHII", raw, map_off + 4 + i * 12, *item)
    for i in range(len(new_items), map_count):
        struct.pack_into("<HHII", raw, map_off + 4 + i * 12, 0, 0, 0, 0)

    raw[12:32] = hashlib.sha1(raw[32:]).digest()
    adler = zlib.adler32(raw[12:]) & 0xFFFFFFFF
    struct.pack_into("<I", raw, 8, adler)

    with open(OUT_DEX, "wb") as f:
        f.write(raw)
    print(f"Wrote {OUT_DEX} ({len(raw)} bytes, stripped {stripped_hiddenapi} hiddenapi section, Adler32={adler:#010x})")

    res_dd = subprocess.run(["/apex/com.android.art/bin/dexdump", "-f", OUT_DEX], capture_output=True, text=True)
    if res_dd.returncode != 0 or "Failure to verify" in res_dd.stderr:
        print("dexdump stderr:", res_dd.stderr)
        raise RuntimeError(f"dexdump verification FAILED (rc={res_dd.returncode})")
    print("dexdump strict DEX verification PASSED (exit 0)!")


if __name__ == "__main__":
    main()
