#!/usr/bin/env bash
# Build all neo9-gsi-kit native binaries, JNI stubs, RRO overlays, and patched telephony assets.
# Designed to run directly on-device in Termux (aarch64) with clang, aapt, python3, and openssl.
set -euo pipefail

KIT_DIR="$(cd "$(dirname "$0")" && pwd)"
FRAMEWORK_RES="${1:-/system/framework/framework-res.apk}"

echo "[1/5] Compiling neo9-udfps-bridge (aarch64)..."
clang -O2 -Wall -Wextra \
    -o "$KIT_DIR/udfps/neo9-udfps-bridge" \
    "$KIT_DIR/udfps/neo9-udfps-bridge.c" -ldl
strip "$KIT_DIR/udfps/neo9-udfps-bridge" 2>/dev/null || true

echo "[2/5] Compiling Android 17 VT JNI stubs (libimsmedia_jni.so / libimscamera_jni.so)..."
clang -O2 -shared -fPIC -Wall -Wextra \
    -Wl,-soname,libimsmedia_jni.so \
    -o "$KIT_DIR/telephony/libimsmedia_jni.so" \
    "$KIT_DIR/telephony/imsmedia_jni_stub.c"
clang -O2 -shared -fPIC -Wall -Wextra \
    -Wl,-soname,libimscamera_jni.so \
    -o "$KIT_DIR/telephony/libimscamera_jni.so" \
    "$KIT_DIR/telephony/imscamera_jni_stub.c"
strip "$KIT_DIR/telephony/libimsmedia_jni.so" "$KIT_DIR/telephony/libimscamera_jni.so" 2>/dev/null || true

echo "[3/5] Building & APK-v2 signing static RRO overlays..."
python3 "$KIT_DIR/overlay/build_overlay.py" "$FRAMEWORK_RES"
python3 "$KIT_DIR/telephony/build_ims_overlays.py" "$FRAMEWORK_RES"

echo "[4/5] Patching & APK-v2 signing Qualcomm CAF IMS APK (Neo9QtiIms.apk)..."
if [ -f "$KIT_DIR/telephony/ims-caf-u.apk" ]; then
    python3 "$KIT_DIR/telephony/patch_qti_ims.py"
else
    echo "  SKIP: $KIT_DIR/telephony/ims-caf-u.apk not found."
    echo "  Place PHH Treble ims-caf-u.apk at telephony/ims-caf-u.apk and re-run ./build.sh."
fi

echo "[5/5] Building patched PD2338 qcrilNr.db (persist.vendor.radio.poweron_opt=0)..."
if [ -f "$KIT_DIR/telephony/neo9_qcrilNr.db" ]; then
    echo "  Using existing $KIT_DIR/telephony/neo9_qcrilNr.db (run telephony/build_qcril_db.py to regenerate)."
elif command -v su >/dev/null 2>&1; then
    python3 "$KIT_DIR/telephony/build_qcril_db.py"
else
    echo "  SKIP: run 'python3 telephony/build_qcril_db.py --input <qcrilNr.db>' to generate neo9_qcrilNr.db."
fi

echo "All available components built successfully."
