#!/usr/bin/env bash
# Build all neo9-gsi-kit native binaries, shims, RRO overlays, and patched telephony/camera assets.
# Designed to run directly on-device in Termux (aarch64) with clang++, aapt, python3, and openssl.
set -euo pipefail

KIT_DIR="$(cd "$(dirname "$0")" && pwd)"
FRAMEWORK_RES="${1:-/system/framework/framework-res.apk}"

echo "[1/8] Compiling neo9-udfps-bridge (aarch64)..."
clang -O2 -Wall -Wextra \
    -o "$KIT_DIR/udfps/neo9-udfps-bridge" \
    "$KIT_DIR/udfps/neo9-udfps-bridge.c" -ldl
strip "$KIT_DIR/udfps/neo9-udfps-bridge" 2>/dev/null || true

echo "[2/8] Compiling Android 17 VT JNI stubs (libimsmedia_jni.so / libimscamera_jni.so)..."
clang -O2 -shared -fPIC -Wall -Wextra \
    -Wl,-soname,libimsmedia_jni.so \
    -o "$KIT_DIR/telephony/libimsmedia_jni.so" \
    "$KIT_DIR/telephony/imsmedia_jni_stub.c"
clang -O2 -shared -fPIC -Wall -Wextra \
    -Wl,-soname,libimscamera_jni.so \
    -o "$KIT_DIR/telephony/libimscamera_jni.so" \
    "$KIT_DIR/telephony/imscamera_jni_stub.c"
strip "$KIT_DIR/telephony/libimsmedia_jni.so" "$KIT_DIR/telephony/libimscamera_jni.so" 2>/dev/null || true

echo "[3/8] Compiling Module 3 Camera A17 shims (libvivo_vcs_comp.so / libneo9_cam_hook.so)..."
clang++ -O2 -shared -fPIC -nostdlib++ -fno-exceptions -fno-rtti -Wall -Wextra \
    -Wl,-soname,libvivo_vcs_comp.so \
    -o "$KIT_DIR/camera/libvivo_vcs_comp.so" \
    "$KIT_DIR/camera/vivo_vcs_compat.cpp" -ldl
clang++ -O2 -shared -fPIC -nostdlib++ -fno-exceptions -fno-rtti -Wall -Wextra \
    -Wl,-soname,libneo9_cam_hook.so \
    -L/system/lib64 -Wl,--no-as-needed -lcamera_client -llog -ldl \
    -o "$KIT_DIR/camera/libneo9_cam_hook.so" \
    "$KIT_DIR/camera/neo9_cam_hook.cpp"
strip "$KIT_DIR/camera/libvivo_vcs_comp.so" "$KIT_DIR/camera/libneo9_cam_hook.so" 2>/dev/null || true
if command -v llvm-readelf >/dev/null 2>&1; then
    for lib in "$KIT_DIR/camera/libvivo_vcs_comp.so" "$KIT_DIR/camera/libneo9_cam_hook.so"; do
        if llvm-readelf -d "$lib" | grep -q "libc++_shared"; then
            echo "FATAL: $lib unexpectedly links libc++_shared.so!" >&2
            exit 1
        fi
    done
fi

echo "[3b/8] Compiling Module 4 SurfaceFlinger 360Hz-TE EPT bridge (libneo9_sf_hook.so)..."
clang -O2 -shared -fPIC -mbranch-protection=standard -Wall -Wextra -Werror \
    -Wl,-soname,libneo9_sf_hook.so \
    -L/system/lib64 -Wl,--no-as-needed -lpowermanager -llog -ldl \
    -o "$KIT_DIR/display/libneo9_sf_hook.so" \
    "$KIT_DIR/display/neo9_sf_hook.c"
strip "$KIT_DIR/display/libneo9_sf_hook.so" 2>/dev/null || true
if command -v llvm-readelf >/dev/null 2>&1; then
    if llvm-readelf -d "$KIT_DIR/display/libneo9_sf_hook.so" | grep -q "libc++_shared"; then
        echo "FATAL: libneo9_sf_hook.so unexpectedly links libc++_shared.so!" >&2
        exit 1
    fi
    if ! llvm-readelf -d "$KIT_DIR/display/libneo9_sf_hook.so" | grep -q "libpowermanager.so"; then
        echo "FATAL: libneo9_sf_hook.so missing DT_NEEDED libpowermanager.so!" >&2
        exit 1
    fi
fi

echo "[4/8] Building & APK-v2 signing static RRO overlays..."
python3 "$KIT_DIR/overlay/build_overlay.py" "$FRAMEWORK_RES"
python3 "$KIT_DIR/telephony/build_ims_overlays.py" "$FRAMEWORK_RES"
python3 "$KIT_DIR/camera/build_aperture_overlay.py" "$FRAMEWORK_RES"

echo "[5/8] Patching & APK-v2 signing Qualcomm CAF IMS APK (Neo9QtiIms.apk)..."
if [ -f "$KIT_DIR/telephony/ims-caf-u.apk" ]; then
    python3 "$KIT_DIR/telephony/patch_qti_ims.py"
else
    echo "  SKIP: $KIT_DIR/telephony/ims-caf-u.apk not found."
    echo "  Place PHH Treble ims-caf-u.apk at telephony/ims-caf-u.apk and re-run ./build.sh."
fi

echo "[6/8] Building patched PD2338 qcrilNr.db (persist.vendor.radio.poweron_opt=0)..."
if [ -f "$KIT_DIR/telephony/neo9_qcrilNr.db" ]; then
    echo "  Using existing $KIT_DIR/telephony/neo9_qcrilNr.db (run telephony/build_qcril_db.py to regenerate)."
elif command -v su >/dev/null 2>&1; then
    python3 "$KIT_DIR/telephony/build_qcril_db.py"
else
    echo "  SKIP: run 'python3 telephony/build_qcril_db.py --input <qcrilNr.db>' to generate neo9_qcrilNr.db."
fi

echo "[7/8] Patching OriginOS 4 vivocameraserver + libvivocameraservice.so (DT_NEEDED shim)..."
if [ -f /system/bin/vivocameraserver ] && [ -f /system/lib64/libvivocameraservice.so ]; then
    python3 "$KIT_DIR/camera/patch_vivocameraserver.py"
elif command -v su >/dev/null 2>&1 && su -c '[ -f /system/bin/vivocameraserver ] && [ -f /system/lib64/libvivocameraservice.so ]'; then
    VCS_TMP="$KIT_DIR/camera/.vcs_orig.tmp"
    su -c "cp /system/bin/vivocameraserver '$VCS_TMP' && chmod 644 '$VCS_TMP'"
    python3 "$KIT_DIR/camera/patch_vivocameraserver.py" "$VCS_TMP" /system/lib64/libvivocameraservice.so
    rm -f "$VCS_TMP"
elif [ -f "$KIT_DIR/camera/neo9_vivocameraserver" ] && [ -f "$KIT_DIR/camera/libvivocameraservice.so" ]; then
    echo "  Using existing patched camera/neo9_vivocameraserver & camera/libvivocameraservice.so."
else
    echo "  SKIP: host OriginOS 4 vivocameraserver not found (run on rooted stock OriginOS 4 host)."
fi

echo "[8/8] Building compat DEX + patching & APK-v2 signing VivoCamera & Vivo3rdAlgoService..."
if [ -f /system/app/VivoCamera/VivoCamera.apk ] && [ -f /system/framework/vivo-framework.jar ]; then
    python3 "$KIT_DIR/camera/patch_vivo_camera.py"
else
    echo "  SKIP: host OriginOS 4 VivoCamera.apk / vivo-framework.jar not found (run on stock OriginOS 4 host)."
fi

echo "All available components built successfully."
