#!/usr/bin/env python3
"""
Patch /system/bin/cameraserver in-place:
1. Replace DT_NEEDED libcamera_client.so -> libneo9_cam_hook.so (exact 20-byte NUL-terminated
   replacement) so cameraserver loads libneo9_cam_hook.so (which links libcamera_client.so,
   sets OriginOS 4 IVivoCameraProvider@1.0 HalParamTags, and routes exclusively to vendorVcf/0).
2. Relax CameraDeviceClient::isSensorPixelModeConsistent() mismatch failure path (0x1c2428:
   branch to 0x1c2400 'mov w0, #1; ret') so VivoCamera's front 16MP (4608x3456) capture request
   with CONTROL_SENSOR_PIXEL_MODE=0 is not rejected by A17 CameraDeviceClient.
"""
import sys

OLD = b"libcamera_client.so\x00"
NEW = b"libneo9_cam_hook.so\x00"
assert len(OLD) == len(NEW) == 20

# Unique 32-byte signature at A17 CameraDeviceClient::isSensorPixelModeConsistent mismatch handler
# (0x1c2428: adrp x1, 0x3b000; add x1, x1, #0xc5a; adrp x2, 0x4e000; add x2, x2, #0x476; ...)
SPM_OLD = bytes.fromhex("c1f3ffb02168319162f4ff9042d81191c0008052e303042aa8150794952640f9")
# Replace first instruction (0x1c2428) with `b 0x1c2400` (-40 bytes = -10 insns -> 0x17fffff6)
# which loads `mov w0, #1` and returns true cleanly through the normal stack-canary epilogue.
SPM_NEW = bytes.fromhex("f6ffff17") + SPM_OLD[4:]
assert len(SPM_OLD) == len(SPM_NEW) == 32


def patch_file(path: str):
    data = bytearray(open(path, "rb").read())
    changed = False

    if NEW in data:
        print(f"DT_NEEDED already patched: {path}")
    else:
        cnt = data.count(OLD)
        assert cnt == 1, f"Expected 1 occurrence of {OLD!r} in {path}, found {cnt}"
        idx = data.index(OLD)
        data[idx : idx + len(OLD)] = NEW
        print(f"Patched {path} @ {hex(idx)}: {OLD[:-1].decode()} -> {NEW[:-1].decode()}")
        changed = True

    if SPM_NEW in data:
        print(f"isSensorPixelModeConsistent already patched: {path}")
    else:
        cnt = data.count(SPM_OLD)
        assert cnt == 1, f"Expected 1 occurrence of isSensorPixelModeConsistent sig in {path}, found {cnt}"
        idx = data.index(SPM_OLD)
        data[idx : idx + len(SPM_OLD)] = SPM_NEW
        print(f"Patched {path} @ {hex(idx)}: isSensorPixelModeConsistent mismatch -> b -40 (return true)")
        changed = True

    if changed:
        with open(path, "wb") as f:
            f.write(data)


if __name__ == "__main__":
    patch_file(sys.argv[1])
