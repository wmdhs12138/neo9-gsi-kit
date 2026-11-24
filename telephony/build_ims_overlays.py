#!/usr/bin/env python3
"""Build and APK-v2-sign Neo9CafImsPhoneOverlay.apk and Neo9CarrierConfigOverlay.apk."""
import os
import subprocess
import sys
import tempfile

TEL_DIR = os.path.dirname(os.path.abspath(__file__))
KIT_DIR = os.path.dirname(TEL_DIR)
sys.path.insert(0, os.path.join(KIT_DIR, "overlay"))
import build_overlay as bo  # noqa: E402

FRAMEWORK_RES = sys.argv[1] if len(sys.argv) > 1 else "/system/framework/framework-res.apk"


def build_one(src_dir: str, out_apk: str):
    bo.ensure_key_and_cert()
    with tempfile.TemporaryDirectory() as td:
        raw_apk = os.path.join(td, "raw.apk")
        aligned_apk = os.path.join(td, "aligned.apk")
        subprocess.check_call([
            "aapt", "package", "-f",
            "-M", os.path.join(src_dir, "AndroidManifest.xml"),
            "-S", os.path.join(src_dir, "res"),
            "-I", FRAMEWORK_RES,
            "-F", raw_apk,
            "-0", "arsc",
        ])
        bo.zipalign_apk(raw_apk, aligned_apk)
        bo.sign_apk_v2(aligned_apk, out_apk)
    print(f"Built & APK-v2 signed RRO overlay: {out_apk} ({os.path.getsize(out_apk)} bytes)")


if __name__ == "__main__":
    build_one(os.path.join(TEL_DIR, "cafims_phone"), os.path.join(TEL_DIR, "Neo9CafImsPhoneOverlay.apk"))
    build_one(os.path.join(TEL_DIR, "carrier_config"), os.path.join(TEL_DIR, "Neo9CarrierConfigOverlay.apk"))
