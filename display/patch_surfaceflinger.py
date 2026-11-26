#!/usr/bin/env python3
"""
Patch /system/bin/surfaceflinger in-place:
Replace DT_NEEDED libpowermanager.so -> libneo9_sf_hook.so (exact 19-byte NUL-terminated
replacement) so surfaceflinger loads libneo9_sf_hook.so (which links libpowermanager.so and
bridges Android 17 SurfaceFlinger's real expectedPresentTime to Vivo HIDL 2.4 opcode 0x10020002
for RM692H5 360Hz High-TE LTPO frame synchronization).
"""
import sys

OLD = b"libpowermanager.so\x00"
NEW = b"libneo9_sf_hook.so\x00"
assert len(OLD) == len(NEW) == 19


def patch_file(path: str):
    data = bytearray(open(path, "rb").read())
    if NEW in data:
        print(f"DT_NEEDED already patched: {path}")
        return
    cnt = data.count(OLD)
    assert cnt == 1, f"Expected 1 occurrence of {OLD!r} in {path}, found {cnt}"
    idx = data.index(OLD)
    data[idx : idx + len(OLD)] = NEW
    with open(path, "wb") as f:
        f.write(data)
    print(f"Patched {path} @ {hex(idx)}: {OLD[:-1].decode()} -> {NEW[:-1].decode()}")


if __name__ == "__main__":
    patch_file(sys.argv[1])
