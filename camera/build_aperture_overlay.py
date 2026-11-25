#!/usr/bin/env python3
"""Build and APK-v2-sign Neo9ApertureOverlay.apk to hide PD2338 Dummy camera IDs (4..18)
from LineageOS Aperture while keeping 0 (Main), 1 (Front), 2 (MultiCamera), 3 (Ultra-Wide)."""
import os
import subprocess
import sys
import tempfile

CAM_DIR = os.path.dirname(os.path.abspath(__file__))
KIT_DIR = os.path.dirname(CAM_DIR)
sys.path.insert(0, os.path.join(KIT_DIR, "overlay"))
import build_overlay as bo  # noqa: E402

FRAMEWORK_RES = sys.argv[1] if len(sys.argv) > 1 else "/system/framework/framework-res.apk"
SRC_DIR = os.path.join(CAM_DIR, "aperture_overlay")
OUT_APK = os.path.join(CAM_DIR, "Neo9ApertureOverlay.apk")


def main():
    bo.ensure_key_and_cert()
    with tempfile.TemporaryDirectory() as td:
        raw_apk = os.path.join(td, "raw.apk")
        aligned_apk = os.path.join(td, "aligned.apk")
        subprocess.check_call([
            "aapt", "package", "-f",
            "-M", os.path.join(SRC_DIR, "AndroidManifest.xml"),
            "-S", os.path.join(SRC_DIR, "res"),
            "-I", FRAMEWORK_RES,
            "-F", raw_apk,
            "-0", "arsc",
        ])
        bo.zipalign_apk(raw_apk, aligned_apk)
        bo.sign_apk_v2(aligned_apk, OUT_APK)
    print(f"Built & APK-v2 signed RRO overlay: {OUT_APK} ({os.path.getsize(OUT_APK)} bytes)")


if __name__ == "__main__":
    main()
