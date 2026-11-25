#!/usr/bin/env python3
"""Prepare A17-compatible copies of OriginOS 4 vivocameraserver and libvivocameraservice.so
by replacing the unused 0-symbol Android stub DT_NEEDED `libhidltransport.so` (19 bytes)
with `libvivo_vcs_comp.so` (exact 19 bytes). This ensures Bionic loads our 4-symbol
A14->A17 ABI shim automatically even under AT_SECURE=1 (no LD_PRELOAD needed!).
"""
import os
import subprocess
import sys

CAM_DIR = os.path.dirname(os.path.abspath(__file__))
OLD_SONAME = b"libhidltransport.so\x00"
NEW_SONAME = b"libvivo_vcs_comp.so\x00"
assert len(OLD_SONAME) == len(NEW_SONAME)


def patch_elf(src: str, dst: str):
    data = open(src, "rb").read()
    cnt = data.count(OLD_SONAME)
    if cnt != 1:
        raise RuntimeError(f"{src}: expected 1 occurrence of {OLD_SONAME!r}, found {cnt}")
    patched = data.replace(OLD_SONAME, NEW_SONAME, 1)
    with open(dst, "wb") as f:
        f.write(patched)
    out = subprocess.check_output(["llvm-readelf", "-d", dst], text=True)
    if "[libvivo_vcs_comp.so]" not in out:
        raise RuntimeError(f"{dst}: DT_NEEDED verification failed!")
    print(f"Patched ELF DT_NEEDED: {src} -> {dst} (libhidltransport.so -> libvivo_vcs_comp.so)")


def main():
    vcs_in = sys.argv[1] if len(sys.argv) > 1 else "/system/bin/vivocameraserver"
    lib_in = sys.argv[2] if len(sys.argv) > 2 else "/system/lib64/libvivocameraservice.so"
    patch_elf(vcs_in, os.path.join(CAM_DIR, "neo9_vivocameraserver"))
    os.chmod(os.path.join(CAM_DIR, "neo9_vivocameraserver"), 0o755)
    patch_elf(lib_in, os.path.join(CAM_DIR, "libvivocameraservice.so"))


if __name__ == "__main__":
    main()
