#!/system/bin/sh
# Modular iQOO Neo9 (PD2338) GSI Patcher — injects neo9-gsi-kit modules into a raw ext4 GSI image
# Usage (as root): sh apply_neo9_patches.sh <input_gsi.img> <output_signed.img> <avb_key.pem>
set -eu

IN_IMG="$1"
OUT_IMG="$2"
KEY_PEM="$3"
KIT_DIR="$(cd "$(dirname "$0")" && pwd)"
BRIDGE_BIN="$KIT_DIR/udfps/neo9-udfps-bridge"
OVERLAY_APK="$KIT_DIR/overlay/Neo9UdfpsOverlay.apk"
SETTINGS_OVERLAY="$KIT_DIR/overlay/Neo9SettingsOverlay.apk"
CAFIMS_PHONE_OVERLAY="$KIT_DIR/telephony/Neo9CafImsPhoneOverlay.apk"
CARRIER_CFG_OVERLAY="$KIT_DIR/telephony/Neo9CarrierConfigOverlay.apk"
QTI_IMS_APK="$KIT_DIR/telephony/Neo9QtiIms.apk"
QTI_IMS_PERM="$KIT_DIR/telephony/privapp-permissions-qti-ims.xml"
QTI_IMS_DEFPERM="$KIT_DIR/telephony/default-permissions-qti-ims.xml"
QTI_IMS_SYSCONFIG="$KIT_DIR/telephony/hiddenapi-whitelist-qti-ims.xml"
QTI_IMS_MEDIA_SO="$KIT_DIR/telephony/libimsmedia_jni.so"
QTI_IMS_CAM_SO="$KIT_DIR/telephony/libimscamera_jni.so"
QCRIL_NR_DB="$KIT_DIR/telephony/neo9_qcrilNr.db"
VIVO_CAM_APK="$KIT_DIR/camera/Neo9VivoCamera.apk"
VIVO_ALGO_APK="$KIT_DIR/camera/Neo9Vivo3rdAlgoService.apk"
APERTURE_OVERLAY="$KIT_DIR/camera/Neo9ApertureOverlay.apk"
VIVO_CAM_PERM="$KIT_DIR/camera/privapp-permissions-vivo-camera.xml"
VIVO_CAM_DEFPERM="$KIT_DIR/camera/default-permissions-vivo-camera.xml"
VIVO_CAM_SYSCONFIG="$KIT_DIR/camera/hiddenapi-whitelist-vivo-camera.xml"
VIVO_CAM_VINTF="$KIT_DIR/camera/manifest_neo9_vivo_camera.xml"
VIVO_VCS_BIN="$KIT_DIR/camera/neo9_vivocameraserver"
VIVO_VCS_LIB="$KIT_DIR/camera/libvivocameraservice.so"
VIVO_VCS_COMP="$KIT_DIR/camera/libvivo_vcs_comp.so"
VIVO_CAM_HOOK="$KIT_DIR/camera/libneo9_cam_hook.so"
VIVO_CAM_PATCH_PY="$KIT_DIR/camera/patch_cameraserver.py"
OVERLAY_CERT_DER="$KIT_DIR/overlay/neo9_overlay_cert.der"
AVBFORGE_CORE="$(pm path io.github.fu.avbforge | sed 's/package://;s|base.apk|lib/arm64/libavbforge_core.so|')"

# Stage at a short FUSE path (<= 64 bytes for toybox losetup, buffered I/O to avoid F2FS inlinecrypt RW error)
WORK_IMG="${WORK_IMG:-/storage/emulated/0/Download/AVBForge/.n9p.img}"
MNT="/data/local/tmp/neo9_gsi_mnt"

cleanup() {
    sync 2>/dev/null || true
    /system/bin/umount "$MNT" 2>/dev/null || true
    rmdir "$MNT" 2>/dev/null || true
    rm -f "$WORK_IMG" 2>/dev/null || true
}
trap cleanup EXIT

echo "[1/5] Copying base GSI to short FUSE staging path..."
rm -f "$WORK_IMG"
cp "$IN_IMG" "$WORK_IMG"

echo "[2/5] Stripping old AVB footer and preparing RW ext4..."
"$AVBFORGE_CORE" avb erase-footer --image "$WORK_IMG" >/dev/null || true
dd if=/dev/zero bs=1048576 count=768 >> "$WORK_IMG" 2>/dev/null
/system/bin/e2fsck -f -y "$WORK_IMG" || [ $? -le 2 ]
/system/bin/resize2fs "$WORK_IMG"
/system/bin/e2fsck -f -y -E unshare_blocks "$WORK_IMG" || [ $? -le 2 ]
/system/bin/e2fsck -f -y "$WORK_IMG" || [ $? -le 2 ]

echo "[3/5] Mounting GSI RW and installing Module 1: Neo9 Optical UDFPS..."
mkdir -p "$MNT"
/system/bin/mount -o rw,loop "$WORK_IMG" "$MNT"

# 3a. Install neo9-udfps-bridge binary with phhsu_exec context (required entrypoint for u:r:phhsu_daemon:s0!)
# Seed inode xattr from rw-system.sh (guaranteed u:object_r:phhsu_exec:s0) + overwrite in place.
rm -f "$MNT/system/bin/neo9-udfps-bridge"
cp -a "$MNT/system/bin/rw-system.sh" "$MNT/system/bin/neo9-udfps-bridge"
cat "$BRIDGE_BIN" > "$MNT/system/bin/neo9-udfps-bridge"
chmod 0755 "$MNT/system/bin/neo9-udfps-bridge"
chown 0:2000 "$MNT/system/bin/neo9-udfps-bridge"
chcon u:object_r:phhsu_exec:s0 "$MNT/system/bin/neo9-udfps-bridge" 2>/dev/null || true

# Register persistent file_contexts entries (preserve existing xattr by appending in-place)
SE_FC="$MNT/system/system_ext/etc/selinux/system_ext_file_contexts"
if [ -f "$SE_FC" ]; then
    if ! grep -q "neo9-udfps-bridge" "$SE_FC"; then
        echo "/system/bin/neo9-udfps-bridge u:object_r:phhsu_exec:s0" >> "$SE_FC"
    fi
    if [ -f "$VIVO_VCS_BIN" ] && ! grep -q "/system/bin/vivocameraserver" "$SE_FC"; then
        echo "/system/bin/vivocameraserver u:object_r:cameraserver_exec:s0" >> "$SE_FC"
    fi
    chmod 0644 "$SE_FC"
fi

# Register media.vivocamera in system_ext_service_contexts (preserve service_contexts_file xattr!)
SE_SVC="$MNT/system/system_ext/etc/selinux/system_ext_service_contexts"
if [ -f "$SE_SVC" ] && [ -f "$VIVO_VCS_BIN" ]; then
    if ! grep -q "^media\.vivocamera " "$SE_SVC"; then
        echo "media.vivocamera u:object_r:cameraserver_service:s0" >> "$SE_SVC"
    fi
    chmod 0644 "$SE_SVC"
    chown 0:0 "$SE_SVC"
    chcon u:object_r:service_contexts_file:s0 "$SE_SVC"
fi

# Allow phhsu_daemon <-> hal_fingerprint_server / system_server / radio / cameraserver bidirectional Binder IPC
# Allow cameraserver (vivocameraserver) <-> platform_app (VivoCamera) callbacks + FD transfer
SE_CIL="$MNT/system/system_ext/etc/selinux/system_ext_sepolicy.cil"
if [ -f "$SE_CIL" ] && ! grep -q "phhsu_daemon hal_fingerprint_service" "$SE_CIL"; then
    cat >> "$SE_CIL" <<'EOF'
(allow phhsu_daemon hal_fingerprint_service (service_manager (find)))
(allow hal_fingerprint_server phhsu_daemon (binder (call transfer)))
(allow hal_fingerprint_server phhsu_daemon (fd (use)))
(allow system_server phhsu_daemon (binder (call transfer)))
(allow system_server phhsu_daemon (fd (use)))
(allow radio phhsu_daemon (binder (call transfer)))
(allow radio phhsu_daemon (fd (use)))
(allow radio phhsu_daemon (fifo_file (write getattr)))
EOF
    chmod 0644 "$SE_CIL"
fi
if [ -f "$SE_CIL" ] && ! grep -q "phhsu_daemon cameraserver_service" "$SE_CIL"; then
    cat >> "$SE_CIL" <<'EOF'
(allow phhsu_daemon cameraserver_service (service_manager (find)))
(allow cameraserver phhsu_daemon (binder (call transfer)))
(allow cameraserver phhsu_daemon (fd (use)))
(allow cameraserver phhsu_daemon (fifo_file (write getattr)))
(allow platform_app default_android_hwservice (hwservice_manager (find)))
(allow cameraserver default_android_hwservice (hwservice_manager (find)))
(allow cameraserver platform_app (binder (call transfer)))
(allow cameraserver platform_app (fd (use)))
(allow cameraserver sysfs_rsc (dir (search)))
(allow cameraserver sysfs_rsc (file (read write open getattr)))
(allow hal_camera_default sysfs_rsc (dir (search)))
(allow hal_camera3rd_default sysfs_rsc (dir (search)))
(allow hal_camera_default vendor_hal_qspmhal_hwservice (hwservice_manager (find)))
(allow hal_camera_default proc_stat (file (getattr)))
(allow hal_camera_default qcom_soc1_info (dir (search)))
(allow hal_camera3rd_default qcom_soc1_info (dir (search)))
(allow hal_camera_default debugfs_fuelsummary (file (getattr)))
(allow hal_camera_default graphics_config_prop (file (read open getattr map)))
(allow hal_camera_default persist_debug_prop (file (read open getattr map)))
(allow hal_camera_default zygote_config_prop (file (read open getattr map)))
(allow hal_camera_default boot_status_prop (file (read open getattr map)))
EOF
    chmod 0644 "$SE_CIL"
fi
# Restore PD2338 /oem/etc/qdcm_calib_data_mdss_dsi_pd2338_m383_rm692h5_cmd_panel.json SELinux mapping
# Vendor plat_pub_versioned.cil declares (type oem_qdcm_file) + (typeattribute oem_qdcm_file_33_0) without binding them,
# so on non-DSU boots (where real /oem is mounted) hal_graphics_composer_default needs explicit oem_qdcm_file access.
if [ -f "$SE_CIL" ] && ! grep -q "typeattributeset oem_qdcm_file_33_0" "$SE_CIL"; then
    cat >> "$SE_CIL" <<'EOF'
(typeattributeset oem_qdcm_file_33_0 (oem_qdcm_file))
(typeattributeset oem_vivo_config_ini_file_33_0 (oem_vivo_config_ini_file))
(allow hal_graphics_composer_default oem_qdcm_file (file (ioctl read getattr lock map open watch watch_reads)))
(allow hal_graphics_composer_default oemfs (dir (ioctl read getattr lock open watch watch_reads search)))
(allow hal_graphics_composer_default system_file (dir (search getattr open read)))
EOF
    chmod 0644 "$SE_CIL"
fi

# 3b. Install native static RRO Overlays (UDFPS/Display + Settings ColorModes + CAF IMS Phone + CarrierConfig + Aperture)
for ov in "$OVERLAY_APK" "$SETTINGS_OVERLAY" "$CAFIMS_PHONE_OVERLAY" "$CARRIER_CFG_OVERLAY" "$APERTURE_OVERLAY"; do
    if [ -f "$ov" ]; then
        b="$(basename "$ov")"
        cp "$ov" "$MNT/system/product/overlay/$b"
        chmod 0644 "$MNT/system/product/overlay/$b"
        chown 0:0 "$MNT/system/product/overlay/$b"
        chcon u:object_r:system_file:s0 "$MNT/system/product/overlay/$b"
    fi
done

# 3b0. Stage PD2338 RM692H5 QDCM display calibration JSON into /system_ext/etc/ for DSU boots
# Under DSU, first_stage_mount does NOT mount the host /oem erofs partition, leaving /oem empty so
# vendor.qti.hardware.display.composer-service (libqdcm-json-mode-parser.so) loads zero color modes.
# rw-system.sh bind-mounts this JSON into /oem/etc/ at post-fs before composer starts.
QDCM_SRC="/oem/etc/qdcm_calib_data_mdss_dsi_pd2338_m383_rm692h5_cmd_panel.json"
if [ -f "$QDCM_SRC" ]; then
    echo "[3b0] Staging PD2338 RM692H5 QDCM calibration JSON into /system_ext/etc/..."
    mkdir -p "$MNT/system/system_ext/etc"
    cp "$QDCM_SRC" "$MNT/system/system_ext/etc/neo9_qdcm_rm692h5.json"
    chmod 0644 "$MNT/system/system_ext/etc/neo9_qdcm_rm692h5.json"
    chown 0:0 "$MNT/system/system_ext/etc/neo9_qdcm_rm692h5.json"
    chcon u:object_r:system_file:s0 "$MNT/system/system_ext/etc/neo9_qdcm_rm692h5.json"
fi

# 3b2. Module 2: Install Qualcomm Hardware AIDL IMS (org.codeaurora.ims -> IImsRadio/imsradio0,1)
if [ -f "$QTI_IMS_APK" ] && [ -f "$QTI_IMS_PERM" ] && [ -f "$OVERLAY_CERT_DER" ]; then
    IMS_DIR="$MNT/system/system_ext/priv-app/ims"
    mkdir -p "$IMS_DIR/lib/arm64"
    cp "$QTI_IMS_APK" "$IMS_DIR/ims.apk"
    cp "$QTI_IMS_MEDIA_SO" "$IMS_DIR/lib/arm64/libimsmedia_jni.so"
    cp "$QTI_IMS_CAM_SO" "$IMS_DIR/lib/arm64/libimscamera_jni.so"
    chmod 0755 "$IMS_DIR" "$IMS_DIR/lib" "$IMS_DIR/lib/arm64"
    chmod 0644 "$IMS_DIR/ims.apk" "$IMS_DIR/lib/arm64/libimsmedia_jni.so" "$IMS_DIR/lib/arm64/libimscamera_jni.so"
    chown -R 0:0 "$IMS_DIR"
    chcon -R u:object_r:system_file:s0 "$IMS_DIR"

    cp "$QTI_IMS_PERM" "$MNT/system/system_ext/etc/permissions/privapp-permissions-qti-ims.xml"
    chmod 0644 "$MNT/system/system_ext/etc/permissions/privapp-permissions-qti-ims.xml"
    chown 0:0 "$MNT/system/system_ext/etc/permissions/privapp-permissions-qti-ims.xml"
    chcon u:object_r:system_file:s0 "$MNT/system/system_ext/etc/permissions/privapp-permissions-qti-ims.xml"

    mkdir -p "$MNT/system/system_ext/etc/default-permissions"
    cp "$QTI_IMS_DEFPERM" "$MNT/system/system_ext/etc/default-permissions/default-permissions-qti-ims.xml"
    chmod 0755 "$MNT/system/system_ext/etc/default-permissions"
    chmod 0644 "$MNT/system/system_ext/etc/default-permissions/default-permissions-qti-ims.xml"
    chown -R 0:0 "$MNT/system/system_ext/etc/default-permissions"
    chcon -R u:object_r:system_file:s0 "$MNT/system/system_ext/etc/default-permissions"

    mkdir -p "$MNT/system/system_ext/etc/sysconfig"
    cp "$QTI_IMS_SYSCONFIG" "$MNT/system/system_ext/etc/sysconfig/hiddenapi-whitelist-qti-ims.xml"
    chmod 0644 "$MNT/system/system_ext/etc/sysconfig/hiddenapi-whitelist-qti-ims.xml"
    chown 0:0 "$MNT/system/system_ext/etc/sysconfig/hiddenapi-whitelist-qti-ims.xml"
    chcon u:object_r:system_file:s0 "$MNT/system/system_ext/etc/sysconfig/hiddenapi-whitelist-qti-ims.xml"

    # Install patched PD2338 qcrilNr.db (persist.vendor.radio.poweron_opt='0') for early post-fs bind-mount
    if [ -f "$QCRIL_NR_DB" ]; then
        cp "$QCRIL_NR_DB" "$MNT/system/system_ext/etc/neo9_qcrilNr.db"
        chmod 0644 "$MNT/system/system_ext/etc/neo9_qcrilNr.db"
        chown 0:0 "$MNT/system/system_ext/etc/neo9_qcrilNr.db"
        chcon u:object_r:system_file:s0 "$MNT/system/system_ext/etc/neo9_qcrilNr.db"
    fi

    # Register Neo9 signing cert in system_ext_mac_permissions.xml and map org.codeaurora.ims -> domain=radio
    # NEVER use sed -i without restoring mac_perms_file xattr! Overwrite in-place + explicit chcon.
    CERT_HEX="$(od -An -tx1 -v "$OVERLAY_CERT_DER" | tr -d ' \n\r')"
    SE_MAC="$MNT/system/system_ext/etc/selinux/system_ext_mac_permissions.xml"
    if [ -f "$SE_MAC" ]; then
        if ! grep -q 'seinfo value="neo9_ims"' "$SE_MAC"; then
            MAC_NEW="$(sed "s|</policy>|<signer signature=\"${CERT_HEX}\"><seinfo value=\"neo9_ims\"/></signer></policy>|" "$SE_MAC")"
            printf '%s\n' "$MAC_NEW" > "$SE_MAC"
        fi
        chmod 0644 "$SE_MAC"
        chown 0:0 "$SE_MAC"
        chcon u:object_r:mac_perms_file:s0 "$SE_MAC"
    fi
    SE_SEAPP="$MNT/system/system_ext/etc/selinux/system_ext_seapp_contexts"
    if [ -f "$SE_SEAPP" ]; then
        if ! grep -q "org.codeaurora.ims" "$SE_SEAPP"; then
            echo "user=_app isPrivApp=true seinfo=neo9_ims name=org.codeaurora.ims domain=radio type=radio_data_file" >> "$SE_SEAPP"
        fi
        if [ -f "$VIVO_CAM_APK" ] && ! grep -q "com.android.camera" "$SE_SEAPP"; then
            echo "user=_app isPrivApp=true seinfo=neo9_ims name=com.android.camera domain=platform_app type=app_data_file levelFrom=user" >> "$SE_SEAPP"
        fi
        if [ -f "$VIVO_ALGO_APK" ] && ! grep -q "com.vivo.vivo3rdalgoservice" "$SE_SEAPP"; then
            echo "user=_app seinfo=neo9_ims name=com.vivo.vivo3rdalgoservice domain=platform_app type=app_data_file levelFrom=user" >> "$SE_SEAPP"
        fi
        chmod 0644 "$SE_SEAPP"
        chown 0:0 "$SE_SEAPP"
        chcon u:object_r:seapp_contexts_file:s0 "$SE_SEAPP"
    fi
fi

# 3b3. Module 3: Install OriginOS 4 Stock Camera (com.android.camera) + Vivo3rdAlgoService
if [ -f "$VIVO_CAM_APK" ] && [ -f "$VIVO_CAM_PERM" ]; then
    echo "[3b3] Installing Module 3: OriginOS 4 Stock Camera + Vivo3rdAlgoService..."
    CAM_DIR="$MNT/system/system_ext/priv-app/VivoCamera"
    rm -rf "$CAM_DIR"
    mkdir -p "$CAM_DIR/lib/arm64"
    cp "$VIVO_CAM_APK" "$CAM_DIR/VivoCamera.apk"
    # Dereference all 14 /system/lib64/*_n1.so symlinks so VivoCamera is 100% self-contained
    if [ -d /system/app/VivoCamera/lib/arm64 ]; then
        cp -L /system/app/VivoCamera/lib/arm64/*.so "$CAM_DIR/lib/arm64/"
    fi
    chmod 0755 "$CAM_DIR" "$CAM_DIR/lib" "$CAM_DIR/lib/arm64"
    chmod 0644 "$CAM_DIR/VivoCamera.apk" "$CAM_DIR/lib/arm64/"*.so
    chown -R 0:0 "$CAM_DIR"
    chcon -R u:object_r:system_file:s0 "$CAM_DIR"

    cp "$VIVO_CAM_PERM" "$MNT/system/system_ext/etc/permissions/privapp-permissions-vivo-camera.xml"
    chmod 0644 "$MNT/system/system_ext/etc/permissions/privapp-permissions-vivo-camera.xml"
    chown 0:0 "$MNT/system/system_ext/etc/permissions/privapp-permissions-vivo-camera.xml"
    chcon u:object_r:system_file:s0 "$MNT/system/system_ext/etc/permissions/privapp-permissions-vivo-camera.xml"

    mkdir -p "$MNT/system/system_ext/etc/default-permissions"
    cp "$VIVO_CAM_DEFPERM" "$MNT/system/system_ext/etc/default-permissions/default-permissions-vivo-camera.xml"
    chmod 0644 "$MNT/system/system_ext/etc/default-permissions/default-permissions-vivo-camera.xml"
    chown 0:0 "$MNT/system/system_ext/etc/default-permissions/default-permissions-vivo-camera.xml"
    chcon u:object_r:system_file:s0 "$MNT/system/system_ext/etc/default-permissions/default-permissions-vivo-camera.xml"

    mkdir -p "$MNT/system/system_ext/etc/sysconfig"
    cp "$VIVO_CAM_SYSCONFIG" "$MNT/system/system_ext/etc/sysconfig/hiddenapi-whitelist-vivo-camera.xml"
    chmod 0644 "$MNT/system/system_ext/etc/sysconfig/hiddenapi-whitelist-vivo-camera.xml"
    chown 0:0 "$MNT/system/system_ext/etc/sysconfig/hiddenapi-whitelist-vivo-camera.xml"
    chcon u:object_r:system_file:s0 "$MNT/system/system_ext/etc/sysconfig/hiddenapi-whitelist-vivo-camera.xml"

    if [ -f "$VIVO_ALGO_APK" ]; then
        ALGO_DIR="$MNT/system/system_ext/app/Vivo3rdAlgoService"
        rm -rf "$ALGO_DIR"
        mkdir -p "$ALGO_DIR/lib/arm64" "$MNT/system/system_ext/lib64"
        cp "$VIVO_ALGO_APK" "$ALGO_DIR/Vivo3rdAlgoService.apk"
        if [ -d /system/app/Vivo3rdAlgoService/lib/arm64 ]; then
            cp -L /system/app/Vivo3rdAlgoService/lib/arm64/*.so "$ALGO_DIR/lib/arm64/"
        fi
        # Install complete OriginOS 4 VAS AIDL V2 native stack (0 missing symbols on A17!)
        # Connects Vivo3rdAlgoService -> /vendor/bin/hw/vendor.vivo.hardware.camera3rd.provider@1.0-service
        # (vendor.vivo.hardware.vivoalgoservice.IVASAlgoProvider/default)
        for vlib in \
            /system/lib64/libvivo.vas.native.interface.so \
            /system/lib64/libvivo.vac.interface.so \
            /system/lib64/vendor.vivo.hardware.vivoalgoservice-V2-ndk.so \
            /system/lib64/libvivo.algo.metadata.so \
            /system/lib64/libvas.utils.mm.interface.so \
            /system/lib64/libdmabufheap_wrapper.so \
            /system/lib64/libvivo.vaf.settingprovider.so \
            /system/lib64/libvivo.vaf.tinyxml2.so \
            /system/lib64/libvivo.vas.file.ops.so \
            /system/lib64/libvivo.vas.cameralog.so \
            /system_ext/lib64/vendor.vivo.hardware.camera.cameralog@1.0.so \
            /system/lib64/vendor.vivo.hardware.camera.cameralog-V1-ndk.so \
            /system/lib64/libvivo.camera3rd.log.so \
            /system_ext/lib64/vendor.vivo.hardware.camera3rd.context@1.0.so \
            /system_ext/lib64/vendor.vivo.hardware.camera3rd.provider@1.0.so; do
            if [ -f "$vlib" ]; then
                vb="$(basename "$vlib")"
                cp "$vlib" "$ALGO_DIR/lib/arm64/$vb"
                cp "$vlib" "$MNT/system/system_ext/lib64/$vb"
                chmod 0644 "$MNT/system/system_ext/lib64/$vb"
                chown 0:0 "$MNT/system/system_ext/lib64/$vb"
                chcon u:object_r:system_file:s0 "$MNT/system/system_ext/lib64/$vb"
            fi
        done
        chmod 0755 "$ALGO_DIR" "$ALGO_DIR/lib" "$ALGO_DIR/lib/arm64"
        chmod 0644 "$ALGO_DIR/Vivo3rdAlgoService.apk" "$ALGO_DIR/lib/arm64/"*.so
        chown -R 0:0 "$ALGO_DIR"
        chcon -R u:object_r:system_file:s0 "$ALGO_DIR"
    fi

    # 3b4. Install Framework VINTF manifest for Vivo Camera HIDL HALs (IVivoCameraProvider@1.0 etc.)
    if [ -f "$VIVO_CAM_VINTF" ]; then
        mkdir -p "$MNT/system/system_ext/etc/vintf/manifest"
        cp "$VIVO_CAM_VINTF" "$MNT/system/system_ext/etc/vintf/manifest/manifest_neo9_vivo_camera.xml"
        chmod 0644 "$MNT/system/system_ext/etc/vintf/manifest/manifest_neo9_vivo_camera.xml"
        chown 0:0 "$MNT/system/system_ext/etc/vintf/manifest/manifest_neo9_vivo_camera.xml"
        chcon u:object_r:system_file:s0 "$MNT/system/system_ext/etc/vintf/manifest/manifest_neo9_vivo_camera.xml"
    fi

    # 3b5. Install standalone vivocameraserver (media.vivocamera) + A14->A17 4-symbol compat shim
    # IMPORTANT: /system/bin/vivocameraserver runs in Android's `system` linker namespace,
    # so ALL 26 zero-conflict vivocameraserver libraries MUST live in /system/lib64/!
    if [ -f "$VIVO_VCS_BIN" ] && [ -f "$VIVO_VCS_LIB" ] && [ -f "$VIVO_VCS_COMP" ]; then
        echo "[3b5] Installing vivocameraserver (media.vivocamera) + A17 ABI shim into /system/lib64..."
        mkdir -p "$MNT/system/lib64" "$MNT/system/etc/init"
        # Place in /system/bin/vivocameraserver (matching /system/bin/cameraserver) seeded with cameraserver_exec xattr!
        rm -f "$MNT/system/bin/vivocameraserver" "$MNT/system/system_ext/bin/vivocameraserver" "$MNT/system/system_ext/etc/init/vivocameraserver.rc"
        cp -a "$MNT/system/bin/cameraserver" "$MNT/system/bin/vivocameraserver"
        cat "$VIVO_VCS_BIN" > "$MNT/system/bin/vivocameraserver"
        chmod 0755 "$MNT/system/bin/vivocameraserver"
        chown 0:2000 "$MNT/system/bin/vivocameraserver"
        chcon u:object_r:cameraserver_exec:s0 "$MNT/system/bin/vivocameraserver" 2>/dev/null || true

        cp "$VIVO_VCS_COMP" "$MNT/system/lib64/libvivo_vcs_comp.so"
        cp "$VIVO_VCS_LIB" "$MNT/system/lib64/libvivocameraservice.so"
        chmod 0644 "$MNT/system/lib64/libvivo_vcs_comp.so" "$MNT/system/lib64/libvivocameraservice.so"
        chown 0:0 "$MNT/system/lib64/libvivo_vcs_comp.so" "$MNT/system/lib64/libvivocameraservice.so"
        chcon u:object_r:system_file:s0 "$MNT/system/lib64/libvivo_vcs_comp.so" "$MNT/system/lib64/libvivocameraservice.so"

        for slib in \
            /system/lib64/libvif3ainfoutils.so \
            /system/lib64/libconfiginfoutils.so \
            /system/lib64/libprocessinfoservice_aidl.so \
            /system/lib64/media_permission-aidl-cpp.so \
            /system/lib64/android.hardware.camera.common-V1-ndk.so \
            /system/lib64/android.hardware.camera.device-V2-ndk.so \
            /system/lib64/android.hardware.camera.metadata-V2-ndk.so \
            /system/lib64/android.hardware.graphics.common-V4-ndk.so \
            /system/lib64/android.hardware.sensors-V2-ndk.so \
            /system/lib64/libvcode_nc.so \
            /system/lib64/libvivofmkmedialogctrl.so \
            /system/lib64/vendor.vivo.hardware.vivocameraprovider-V1-ndk.so \
            /system/lib64/vendor.vivo.hardware.camera.vivodevice-V1-ndk.so \
            /system/lib64/vendor.vivo.hardware.camera.vif-V1-ndk.so \
            /system/lib64/vendor.vivo.hardware.camera.vivopostproc-V1-ndk.so \
            /system/lib64/vendor.vivo.hardware.camera.jpegencoder-V1-ndk.so \
            /system/lib64/vendor.vivo.hardware.camera.vif3ainfotransmitter-V1-ndk.so \
            /system/lib64/vendor.vivo.hardware.camera.provider@1.0.so \
            /system/lib64/vendor.vivo.hardware.camera.vivopostproc@1.0.so \
            /system/lib64/vendor.vivo.hardware.camera.vop@1.0.so \
            /system/lib64/vendor.vivo.hardware.camera.jpegencoder@1.0.so \
            /system/lib64/vendor.vivo.hardware.camera.vif3ainfotransmitter@1.0.so \
            /system_ext/lib64/vendor.vivo.hardware.camera.vif@1.0.so \
            /system_ext/lib64/vendor.vivo.hardware.camera.vivodevice@1.0.so \
            /system_ext/lib64/vendor.vivo.hardware.camera.vivoreprocess@1.0.so; do
            if [ -f "$slib" ]; then
                sb="$(basename "$slib")"
                cp "$slib" "$MNT/system/lib64/$sb"
                chmod 0644 "$MNT/system/lib64/$sb"
                chown 0:0 "$MNT/system/lib64/$sb"
                chcon u:object_r:system_file:s0 "$MNT/system/lib64/$sb"
            fi
        done

        cat > "$MNT/system/etc/init/vivocameraserver.rc" <<'VCSRC'
service vivocameraserver /system/bin/vivocameraserver
    class main
    user cameraserver
    group audio camera input drmrpc
    ioprio rt 4
    rlimit rtprio 10 10
VCSRC
        chmod 0644 "$MNT/system/etc/init/vivocameraserver.rc"
        chown 0:0 "$MNT/system/etc/init/vivocameraserver.rc"
        chcon u:object_r:system_file:s0 "$MNT/system/etc/init/vivocameraserver.rc"
    fi

    # 3b5b. Install cameraserver -> IVivoCameraProvider@1.0 appType hook (libneo9_cam_hook.so)
    if [ -f "$VIVO_CAM_HOOK" ] && [ -f "$VIVO_CAM_PATCH_PY" ]; then
        echo "[3b5b] Installing libneo9_cam_hook.so and hooking /system/bin/cameraserver..."
        cp "$VIVO_CAM_HOOK" "$MNT/system/lib64/libneo9_cam_hook.so"
        chmod 0644 "$MNT/system/lib64/libneo9_cam_hook.so"
        chown 0:0 "$MNT/system/lib64/libneo9_cam_hook.so"
        chcon u:object_r:system_file:s0 "$MNT/system/lib64/libneo9_cam_hook.so"
        /data/data/com.termux/files/usr/bin/python3 "$VIVO_CAM_PATCH_PY" "$MNT/system/bin/cameraserver"
        chmod 0755 "$MNT/system/bin/cameraserver"
        chown 0:2000 "$MNT/system/bin/cameraserver"
    fi
    rm -f "$MNT/system/bin/neo9-cam-diag.sh"
fi

# 3c. Inject/refresh early post-fs HAL SensorLocation & telephony hook in rw-system.sh (preserve phhsu_exec xattr!)
RW_SH="$MNT/system/bin/rw-system.sh"
if grep -q "# NEO9_UDFPS_HOOK:" "$RW_SH"; then
    RW_TMP="/data/local/tmp/neo9_rw_clean.sh"
    sed '/^# NEO9_UDFPS_HOOK:/,$d' "$RW_SH" > "$RW_TMP"
    cat "$RW_TMP" > "$RW_SH"
    rm -f "$RW_TMP"
fi
if ! grep -q "NEO9_UDFPS_HOOK" "$RW_SH"; then
cat >> "$RW_SH" <<'EOF'

# NEO9_UDFPS_HOOK: patch PD2338 vendor fingerprint HAL SensorLocation -> (630, 2533, r=110) before late_start
if getprop ro.vendor.build.fingerprint | grep -qi "vivo/PD2338"; then
    # NEO9_TELEPHONY_HOOK: restore PD2338 stock 5G NR (33,33) + DSDS props omitted from /vendor/build.prop
    /system/bin/resetprop_phh ro.telephony.default_network 33,33
    /system/bin/resetprop_phh ro.telephony.sim_slots.count 2
    /system/bin/resetprop_phh telephony.active_modems.max_count 2
    /system/bin/resetprop_phh telephony.lteOnCdmaDevice 1
    /system/bin/resetprop_phh ril.subscription.types NV,RUIM
    # Disable QTI bootup poweron_opt: without OriginOS QtiTelephony OEM_HOOK UI_READY, qcrilNrd buffers all MT SMS forever!
    /system/bin/resetprop_phh persist.vendor.radio.poweron_opt 0

    # NEO9_CAMERA_HOOK: restore PD2338 ro.vivo.* & camera privapp props for OriginOS VivoCamera
    /system/bin/resetprop_phh ro.vivo.product.model PD2338
    /system/bin/resetprop_phh ro.product.model.bbk PD2338
    /system/bin/resetprop_phh ro.vivo.product.release.name PD2338
    /system/bin/resetprop_phh ro.vivo.product.series IQOO
    /system/bin/resetprop_phh ro.vivo.product.platform SM8550
    /system/bin/resetprop_phh ro.vivo.product.solution QCOM
    /system/bin/resetprop_phh ro.vivo.product.overseas no
    /system/bin/resetprop_phh ro.vivo.market.name "iQOO Neo9"
    /system/bin/resetprop_phh ro.vivo.internet.name "iQOO Neo9"
    /system/bin/resetprop_phh ro.vivo.hardware.version PD2338MA
    /system/bin/resetprop_phh ro.vivo.oem.model PD2338B
    /system/bin/resetprop_phh ro.vivo.os.name Funtouch
    /system/bin/resetprop_phh ro.vivo.os.version 14.1
    /system/bin/resetprop_phh ro.vivo.os.build.display.id "OriginOS 4"
    /system/bin/resetprop_phh ro.vivo.rom.style vigour
    /system/bin/resetprop_phh ro.vivo.ozo_camera_enable true
    /system/bin/resetprop_phh ro.vivo.camera.support_third_app_beauty 1
    /system/bin/resetprop_phh ro.vivo.device.type phone
    /system/bin/resetprop_phh ro.vivo.lcm.xhd FHD_20_9
    /system/bin/resetprop_phh ro.vivo.product.version PD2338B_A_14.0.17.2.W10.V000L1
    /system/bin/resetprop_phh ro.camera.enableCamera1MaxZsl 1
    # Disable A17 synthetic JpegR/HeicUltraHDR composite streams (fail on Vendor 33 HAL with capture error requestId -1!)
    /system/bin/resetprop_phh ro.camera.disableJpegR true
    /system/bin/resetprop_phh ro.camera.disableHeicUltraHDR true
    /system/bin/resetprop_phh persist.camera.privapp.list org.codeaurora.snapcam,com.android.camera
    /system/bin/resetprop_phh vendor.camera.aux.packagelist "com.android.camera,org.lineageos.aperture,org.codeaurora.snapcam"
    /system/bin/resetprop_phh persist.sys.phh.include_all_cameras true
    /system/bin/resetprop_phh persist.vivo.camera.max_optical_zoom_factor 2
    /system/bin/resetprop_phh persist.vendor.camera.vivo_torch 1
    /system/bin/resetprop_phh persist.vendor.camera.vivo_torch.use_google_interface 1
    /system/bin/resetprop_phh persist.sys.cct_cali_flag 1
    /system/bin/resetprop_phh persist.vivo.displayp3.support 1
    /system/bin/resetprop_phh persist.vendor.camera.vivop3.support 1
    /system/bin/resetprop_phh persist.sys.log.ctrl no
    /system/bin/resetprop_phh persist.vendor.log.ctrl no
    # Silence CamX/CHI per-frame logcat flood on AOSP liblog (major CPU/thermal saver!)
    /system/bin/resetprop_phh log.tag.CAMX S
    /system/bin/resetprop_phh log.tag.CHI S
    /system/bin/resetprop_phh log.tag.VCameraSdk W
    # Redirect VivoCamera thumbnail review -> GSI Glimpse full-album REVIEW mode
    /system/bin/resetprop_phh com.vivo.gallery.ACTION_VIEW android.provider.action.REVIEW
    /system/bin/resetprop_phh com.vivo.gallery.camera.ACTION_VIEW android.provider.action.REVIEW
    # Restore OriginOS 4 VPerf & SM8550 (kalama) MediaCodec perf tables
    /system/bin/resetprop_phh persist.vendor.vperf_launch_thermal true
    /system/bin/resetprop_phh ro.vendor.perf-hal.ver 2.3
    /system/bin/resetprop_phh ro.media.xml_variant.codecs_performance _kalama
    /system/bin/resetprop_phh ro.media.recorder-max-base-layer-fps 60
    # Match OriginOS 4 SurfaceFlinger dynamic refresh-rate (auto-drops to 60Hz on 30fps viewfinder!),
    # Display-P3 wide color management (fixes viewfinder color/WB parity), and 120Hz frame pacing
    /system/bin/resetprop_phh ro.surface_flinger.use_content_detection_for_refresh_rate true
    /system/bin/resetprop_phh ro.surface_flinger.set_touch_timer_ms 200
    /system/bin/resetprop_phh ro.surface_flinger.has_wide_color_display true
    /system/bin/resetprop_phh ro.surface_flinger.has_HDR_display true
    /system/bin/resetprop_phh ro.surface_flinger.use_color_management true
    /system/bin/resetprop_phh ro.surface_flinger.wcg_composition_dataspace 143261696
    /system/bin/resetprop_phh ro.surface_flinger.max_frame_buffer_acquired_buffers 3
    /system/bin/resetprop_phh vendor.display.enable_dpps_dynamic_fps 1
    /system/bin/resetprop_phh debug.sf.latch_unsignaled 1
    /system/bin/resetprop_phh debug.sf.enable_advanced_sf_phase_offset 1
    /system/bin/resetprop_phh debug.sf.use_phase_offsets_as_durations 1
    /system/bin/resetprop_phh debug.sf.enable_gl_backpressure 1
    /system/bin/resetprop_phh debug.sf.disable_client_composition_cache 1
    /system/bin/resetprop_phh debug.sf.early.app.duration 10500000
    /system/bin/resetprop_phh debug.sf.early.sf.duration 10500000
    /system/bin/resetprop_phh debug.sf.earlyGl.app.duration 10500000
    /system/bin/resetprop_phh debug.sf.earlyGl.sf.duration 10500000
    /system/bin/resetprop_phh debug.sf.late.app.duration 10500000
    /system/bin/resetprop_phh debug.sf.late.sf.duration 10500000

    NEO9_LOG="/metadata/gsi/dsu/neo9_udfps.log"
    echo "=== NEO9 UDFPS BOOT $(date 2>/dev/null) ===" > "$NEO9_LOG" 2>/dev/null || true
    NEO9_FP_SRC="/vendor/bin/hw/vendor.vivo.hardware.biometrics.fingerprint-service"
    NEO9_FP_DST="/mnt/phh/neo9_fp_svc"
    if [ -x "$NEO9_FP_SRC" ] && [ -x /system/bin/neo9-udfps-bridge ]; then
        if /system/bin/neo9-udfps-bridge --patch-hal "$NEO9_FP_SRC" "$NEO9_FP_DST" >> "$NEO9_LOG" 2>&1; then
            NEO9_FP_CTX="$(ls -lZ "$NEO9_FP_SRC" | grep -oE 'u:object_r:[^:]*:s0')"
            chmod 0755 "$NEO9_FP_DST"
            [ -n "$NEO9_FP_CTX" ] && chcon "$NEO9_FP_CTX" "$NEO9_FP_DST" >> "$NEO9_LOG" 2>&1
            mount -o bind "$NEO9_FP_DST" "$NEO9_FP_SRC" >> "$NEO9_LOG" 2>&1
            echo "[rw-system] bind-mounted $NEO9_FP_DST -> $NEO9_FP_SRC (ctx=$NEO9_FP_CTX, rc=$?)" >> "$NEO9_LOG" 2>&1
            ls -lZ "$NEO9_FP_SRC" >> "$NEO9_LOG" 2>&1
        else
            echo "[rw-system] --patch-hal failed (rc=$?)" >> "$NEO9_LOG" 2>&1
        fi
    else
        echo "[rw-system] missing src or bridge binary" >> "$NEO9_LOG" 2>&1
    fi

    # NEO9_QCRIL_DB_HOOK: bind-mount qcrilNr.db with persist.vendor.radio.poweron_opt='0' before qcrilNrd starts.
    # qcrilNrd reads poweron_opt via qcril_config_get (libril-db.so -> qcrilNr.db), NOT Android system properties!
    NEO9_QDB_SRC="/system/system_ext/etc/neo9_qcrilNr.db"
    NEO9_QDB_TGT="/vendor/etc/qcril_database/qcrilNr.db"
    NEO9_QDB_DST="/mnt/phh/neo9_qcrilNr.db"
    if [ -f "$NEO9_QDB_SRC" ] && [ -f "$NEO9_QDB_TGT" ]; then
        cp "$NEO9_QDB_SRC" "$NEO9_QDB_DST"
        NEO9_QDB_CTX="$(ls -lZ "$NEO9_QDB_TGT" | grep -oE 'u:object_r:[^:]*:s0')"
        chmod 0644 "$NEO9_QDB_DST"
        [ -n "$NEO9_QDB_CTX" ] && chcon "$NEO9_QDB_CTX" "$NEO9_QDB_DST" >> "$NEO9_LOG" 2>&1
        mount -o bind "$NEO9_QDB_DST" "$NEO9_QDB_TGT" >> "$NEO9_LOG" 2>&1
        echo "[rw-system] bind-mounted $NEO9_QDB_DST -> $NEO9_QDB_TGT (ctx=$NEO9_QDB_CTX, rc=$?)" >> "$NEO9_LOG" 2>&1
        ls -lZ "$NEO9_QDB_TGT" >> "$NEO9_LOG" 2>&1
    fi

    # NEO9_MCX_MFL_HOOK: patch 4-byte NULL-deref debug log branch in PD2338 com.qualcomm.mcx.policy.mfl.so
    # (GetActiveCameraMaskBasedOnVcfSat @ 0x28080: tbz w17,#7,0x25948 [51 c6 3e 36] -> b 0x25948 [32 f6 ff 17])
    # Prevents SIGSEGV (fault addr 0xc) inside vendor.qti.camera.provider-service_64 on lens/zoom switch!
    NEO9_MCX_SRC="/vendor/lib64/com.qualcomm.mcx.policy.mfl.so"
    NEO9_MCX_DST="/mnt/phh/neo9_mcx_mfl.so"
    if [ -f "$NEO9_MCX_SRC" ]; then
        NEO9_MCX_ORIG="$(od -An -tx1 -j 163968 -N 4 "$NEO9_MCX_SRC" 2>/dev/null | tr -d ' \n\r')"
        if [ "$NEO9_MCX_ORIG" = "51c63e36" ]; then
            cp "$NEO9_MCX_SRC" "$NEO9_MCX_DST"
            /system/bin/printf '\062\366\377\027' | dd of="$NEO9_MCX_DST" bs=1 seek=163968 count=4 conv=notrunc 2>/dev/null
            NEO9_MCX_NEW="$(od -An -tx1 -j 163968 -N 4 "$NEO9_MCX_DST" 2>/dev/null | tr -d ' \n\r')"
            if [ "$NEO9_MCX_NEW" = "32f6ff17" ]; then
                NEO9_MCX_CTX="$(ls -lZ "$NEO9_MCX_SRC" | grep -oE 'u:object_r:[^:]*:s0')"
                chmod 0644 "$NEO9_MCX_DST"
                [ -n "$NEO9_MCX_CTX" ] && chcon "$NEO9_MCX_CTX" "$NEO9_MCX_DST" >> "$NEO9_LOG" 2>&1
                mount -o bind "$NEO9_MCX_DST" "$NEO9_MCX_SRC" >> "$NEO9_LOG" 2>&1
                echo "[rw-system] bind-mounted $NEO9_MCX_DST -> $NEO9_MCX_SRC (0x28080: $NEO9_MCX_ORIG -> $NEO9_MCX_NEW, ctx=$NEO9_MCX_CTX, rc=$?)" >> "$NEO9_LOG" 2>&1
            else
                echo "[rw-system] mcx.policy.mfl patch verify failed (got=$NEO9_MCX_NEW)" >> "$NEO9_LOG" 2>&1
                rm -f "$NEO9_MCX_DST"
            fi
        else
            echo "[rw-system] mcx.policy.mfl byte mismatch at 0x28080 (got=$NEO9_MCX_ORIG)" >> "$NEO9_LOG" 2>&1
        fi
    fi

    # NEO9_CAMX_LOG_HOOK: disable Qualcomm CamX/CHI per-frame verbose/info log storm at the source!
    # Stock /vendor/etc/camera/camxoverridesettings.txt sets logInfoMask=0xFFFFFFFFFFFFFFFF & overrideLogLevels=0x1F,
    # which OriginOS drops in custom liblog/hal_cameralog_default, but floods AOSP logd (~100k lines/min -> CPU/heat!).
    NEO9_CAMX_SRC="/vendor/etc/camera/camxoverridesettings.txt"
    NEO9_CAMX_DST="/mnt/phh/neo9_camxoverridesettings.txt"
    if [ -f "$NEO9_CAMX_SRC" ]; then
        sed -e 's/^logInfoMask=.*/logInfoMask=0x0/' -e 's/^overrideLogLevels=.*/overrideLogLevels=0x0/' "$NEO9_CAMX_SRC" > "$NEO9_CAMX_DST"
        NEO9_CAMX_CTX="$(ls -lZ "$NEO9_CAMX_SRC" | grep -oE 'u:object_r:[^:]*:s0')"
        chmod 0644 "$NEO9_CAMX_DST"
        [ -n "$NEO9_CAMX_CTX" ] && chcon "$NEO9_CAMX_CTX" "$NEO9_CAMX_DST" >> "$NEO9_LOG" 2>&1
        mount -o bind "$NEO9_CAMX_DST" "$NEO9_CAMX_SRC" >> "$NEO9_LOG" 2>&1
        echo "[rw-system] bind-mounted $NEO9_CAMX_DST -> $NEO9_CAMX_SRC (logInfoMask=0x0, overrideLogLevels=0x0, ctx=$NEO9_CAMX_CTX, rc=$?)" >> "$NEO9_LOG" 2>&1
    fi

    # NEO9_QDCM_HOOK: under DSU, /oem is not mounted by first_stage_mount, leaving /oem empty so
    # vendor.qti.hardware.display.composer-service loads zero RM692H5 color modes (only NATIVE 0).
    # Bind-mount staged RM692H5 QDCM calibration JSON onto /oem/etc/ before composer starts (on post-fs).
    NEO9_QDCM_SRC="/system/system_ext/etc/neo9_qdcm_rm692h5.json"
    NEO9_QDCM_NAME="qdcm_calib_data_mdss_dsi_pd2338_m383_rm692h5_cmd_panel.json"
    if [ -f "$NEO9_QDCM_SRC" ] && [ ! -f "/oem/etc/$NEO9_QDCM_NAME" ]; then
        mkdir -p /mnt/phh/neo9_oem/etc
        cp "$NEO9_QDCM_SRC" "/mnt/phh/neo9_oem/etc/$NEO9_QDCM_NAME"
        chmod 0755 /mnt/phh/neo9_oem /mnt/phh/neo9_oem/etc
        chmod 0644 "/mnt/phh/neo9_oem/etc/$NEO9_QDCM_NAME"
        chcon u:object_r:vendor_configs_file:s0 /mnt/phh/neo9_oem /mnt/phh/neo9_oem/etc "/mnt/phh/neo9_oem/etc/$NEO9_QDCM_NAME" >> "$NEO9_LOG" 2>&1 || true
        mount -o bind /mnt/phh/neo9_oem /oem >> "$NEO9_LOG" 2>&1
        echo "[rw-system] bind-mounted /mnt/phh/neo9_oem -> /oem (rc=$?)" >> "$NEO9_LOG" 2>&1
        ls -laZ /oem /oem/etc >> "$NEO9_LOG" 2>&1
    fi
fi
EOF
fi
chmod 0755 "$RW_SH"
chcon u:object_r:phhsu_exec:s0 "$RW_SH" 2>/dev/null || true

echo "[3d] Verifying SELinux labels inside GSI:"
ls -lZ "$MNT/system/bin/rw-system.sh" "$MNT/system/bin/neo9-udfps-bridge" "$MNT/system/bin/phh-on-boot.sh" "$SE_FC" "$MNT/system/product/overlay/Neo9UdfpsOverlay.apk"

# 3c. Clean out old temporary avbforge_diag lines & refresh neo9_udfps_bridge / telephony props in vndk.rc (idempotent)
VNDK_RC="$MNT/system/etc/init/vndk.rc"
if grep -q "service avbforge_diag" "$VNDK_RC"; then
    sed -i '/^service avbforge_diag/,$d' "$VNDK_RC"
    rm -f "$MNT/system/bin/avbforge_diag.sh"
fi
if grep -q "# NEO9_TELEPHONY:" "$VNDK_RC"; then
    VNDK_TMP="/data/local/tmp/neo9_vndk_clean.rc"
    sed '/^# NEO9_TELEPHONY:/,$d' "$VNDK_RC" > "$VNDK_TMP"
    cat "$VNDK_TMP" > "$VNDK_RC"
    rm -f "$VNDK_TMP"
fi
if ! grep -q "service neo9_udfps_bridge" "$VNDK_RC"; then
cat >> "$VNDK_RC" <<'EOF'

# NEO9_TELEPHONY: restore PD2338 stock QTI RIL + Qualcomm Hardware CAF IMS (VoLTE/VoNR/SMSoIP) props before late_start
on post-fs-data && property:ro.product.vendor.device=PD2338
    # CRITICAL: poweron_opt MUST be 0 on AOSP GSI (otherwise qcrilNrd buffers all MT SMS waiting for OEM_HOOK UI_READY!)
    setprop persist.vendor.radio.poweron_opt 0
    setprop persist.vendor.radio.always_send_plmn 1
    setprop persist.vendor.radio.apm_sim_not_pwdn 1
    setprop persist.vendor.radio.procedure_bytes SKIP
    setprop persist.vendor.radio.sib16_support 1
    setprop persist.vendor.radio.support_keep_pdn true
    setprop persist.vendor.radio.custom_ecc 1
    setprop persist.vendor.radio.enableadvancedscan true
    setprop persist.vendor.radio.data_con_rprt true
    setprop persist.vendor.radio.process_sups_ind 1
    setprop persist.vendor.radio.mt_sms_ack 30
    setprop persist.vendor.radio.atfwd.start true
    setprop persist.vendor.ims.dropset_feature 2
    setprop persist.vendor.ims.playout_delay 50
    setprop persist.radio.is_vonr_enabled_0 true
    setprop persist.radio.is_vonr_enabled_1 true
    setprop persist.sys.phh.ims.caf true
    setprop persist.vendor.vilte_support 0
    setprop persist.dbg.volte_avail_ovr 1
    setprop persist.dbg.vt_avail_ovr 1
    setprop persist.dbg.wfc_avail_ovr 1
    setprop persist.dbg.ims_volte_enable 1
    setprop persist.camera.privapp.list org.codeaurora.snapcam,com.android.camera
    setprop persist.vendor.camera.privapp.list org.codeaurora.snapcam
    setprop vendor.camera.aux.packagelist com.android.camera,org.lineageos.aperture,org.codeaurora.snapcam
    setprop ro.camera.disableJpegR true
    setprop ro.camera.disableHeicUltraHDR true
    setprop persist.sys.phh.include_all_cameras true
    setprop persist.sys.log.ctrl no
    setprop persist.vendor.log.ctrl no
    setprop persist.sys.cct_cali_flag 1
    setprop persist.vivo.displayp3.support 1
    setprop persist.vendor.camera.vivop3.support 1
    setprop log.tag.CAMX S
    setprop log.tag.CHI S
    setprop log.tag.VCameraSdk W
    setprop persist.vivo.camera.max_optical_zoom_factor 2
    setprop persist.vendor.camera.vivo_torch 1
    setprop persist.vendor.camera.vivo_torch.use_google_interface 1

# NEO9_UDFPS_BRIDGE: translate AOSP UDFPS pointer/UI events to Vivo IFingerprintExt + Hardware Local HBM
service neo9_udfps_bridge /system/bin/neo9-udfps-bridge
    user root
    group root system
    seclabel u:r:phhsu_daemon:s0
    disabled

on property:sys.boot_completed=1
    setprop doze.pulse.proxcheck false
    start neo9_udfps_bridge
EOF
    chmod 0644 "$VNDK_RC"
    chcon u:object_r:system_file:s0 "$VNDK_RC"
fi

# 3d. Also add a lightweight boot-time diagnostic check in phh-on-boot.sh
# IMPORTANT: preserve u:object_r:phhsu_exec:s0 xattr by seeding from rw-system.sh (host kernel rejects chcon phhsu_exec!)
PHH_BOOT="$MNT/system/bin/phh-on-boot.sh"
if [ -f "$PHH_BOOT" ]; then
    PHH_ORIG="/data/local/tmp/neo9_phh_boot_clean.sh"
    sed '/^# NEO9_UDFPS_BOOT_CHECK/,$d' "$PHH_BOOT" > "$PHH_ORIG"
    rm -f "$PHH_BOOT"
    cp -a "$MNT/system/bin/rw-system.sh" "$PHH_BOOT"
    cat "$PHH_ORIG" > "$PHH_BOOT"
    rm -f "$PHH_ORIG"
cat >> "$PHH_BOOT" <<'EOF'

# NEO9_UDFPS_BOOT_CHECK
if getprop ro.vendor.build.fingerprint | grep -qi "vivo/PD2338"; then
    # Ensure neo9-udfps-bridge is running even if boot_completed fired before phh-on-boot
    pidof neo9-udfps-bridge >/dev/null 2>&1 || /system/bin/neo9-udfps-bridge </dev/null >/dev/null 2>&1 &

    # Grant runtime permissions for Qualcomm Hardware CAF IMS (org.codeaurora.ims)
    pm grant org.codeaurora.ims android.permission.READ_PHONE_STATE 2>/dev/null || true
    pm grant org.codeaurora.ims android.permission.POST_NOTIFICATIONS 2>/dev/null || true

    # Pre-grant runtime permissions & required AppOps for OriginOS Stock Camera (com.android.camera)
    # CRITICAL: PermissionUtil.checkHasAllPermissions() requires MANAGE_EXTERNAL_STORAGE AppOp on Android 13+!
    for p in CAMERA RECORD_AUDIO ACCESS_FINE_LOCATION ACCESS_COARSE_LOCATION READ_MEDIA_IMAGES READ_MEDIA_VIDEO READ_MEDIA_AUDIO POST_NOTIFICATIONS; do
        pm grant com.android.camera "android.permission.$p" 2>/dev/null || true
    done
    appops set --uid com.android.camera MANAGE_EXTERNAL_STORAGE allow 2>/dev/null || true
    appops set com.android.camera MANAGE_EXTERNAL_STORAGE allow 2>/dev/null || true
    appops set com.android.camera WRITE_SETTINGS allow 2>/dev/null || true
    appops set com.android.camera SYSTEM_ALERT_WINDOW allow 2>/dev/null || true

    # One-shot AOT native compilation for VivoCamera (212MB, 16 DEX) & Vivo3rdAlgoService
    # Matches OriginOS 4's prebuilt 9MB .odex to eliminate JIT/interpreter UI lag and heat!
    NEO9_CAM_SZ="$(stat -c %s /system/system_ext/priv-app/VivoCamera/VivoCamera.apk 2>/dev/null || echo 1)"
    NEO9_AOT_STAMP="/data/misc/.neo9_cam_aot_${NEO9_CAM_SZ}"
    if [ ! -f "$NEO9_AOT_STAMP" ]; then
        cmd package compile -m speed -f com.android.camera >/dev/null 2>&1 || true
        cmd package compile -m speed -f com.vivo.vivo3rdalgoservice >/dev/null 2>&1 || true
        touch "$NEO9_AOT_STAMP" 2>/dev/null || true
    fi
fi
EOF
    chmod 0755 "$PHH_BOOT"
    chown 0:2000 "$PHH_BOOT"
    chcon u:object_r:phhsu_exec:s0 "$PHH_BOOT"
fi

echo "[3d2] Final SELinux label verification (ALL patched files):"
ls -lZ "$MNT/system/bin/rw-system.sh" \
       "$MNT/system/bin/phh-on-boot.sh" \
       "$MNT/system/bin/neo9-udfps-bridge" \
       "$MNT/system/system_ext/etc/selinux/system_ext_file_contexts" \
       "$MNT/system/system_ext/etc/selinux/system_ext_mac_permissions.xml" \
       "$MNT/system/system_ext/etc/selinux/system_ext_seapp_contexts" \
       "$MNT/system/system_ext/etc/sysconfig/hiddenapi-whitelist-qti-ims.xml" \
       "$MNT/system/system_ext/priv-app/ims/ims.apk"
for chk in \
    "$MNT/system/bin/rw-system.sh:phhsu_exec" \
    "$MNT/system/bin/phh-on-boot.sh:phhsu_exec" \
    "$MNT/system/bin/neo9-udfps-bridge:phhsu_exec" \
    "$MNT/system/bin/vivocameraserver:cameraserver_exec" \
    "$MNT/system/system_ext/etc/selinux/system_ext_file_contexts:file_contexts_file" \
    "$MNT/system/system_ext/etc/selinux/system_ext_service_contexts:service_contexts_file" \
    "$MNT/system/system_ext/etc/selinux/system_ext_mac_permissions.xml:mac_perms_file" \
    "$MNT/system/system_ext/etc/selinux/system_ext_seapp_contexts:seapp_contexts_file" \
    "$MNT/system/system_ext/etc/neo9_qdcm_rm692h5.json:system_file"; do
    f="${chk%:*}"; t="${chk#*:}"
    ls -lZ "$f" | grep -q ":$t:" || { echo "FATAL: SELinux label mismatch on $f (expected $t)!"; exit 1; }
done

echo "[3e] Offline secilc verification against host Vendor 33..."
LD_LIBRARY_PATH="$MNT/system/lib64" "$MNT/system/bin/secilc" -m -M true -G -N -c 33 \
    "$MNT/system/etc/selinux/plat_sepolicy.cil" \
    "$MNT/system/etc/selinux/mapping/33.0.cil" \
    "$MNT/system/system_ext/etc/selinux/system_ext_sepolicy.cil" \
    "$MNT/system/system_ext/etc/selinux/mapping/33.0.cil" \
    "$MNT/system/product/etc/selinux/product_sepolicy.cil" \
    "$MNT/system/product/etc/selinux/mapping/33.0.cil" \
    "$MNT/system/etc/selinux/plat_sepolicy_genfs_202504.cil" \
    "$MNT/system/etc/selinux/plat_sepolicy_genfs_202604.cil" \
    "$MNT/system/etc/selinux/plat_sepolicy_genfs_202704.cil" \
    /vendor/etc/selinux/vendor_sepolicy.cil \
    /vendor/etc/selinux/plat_pub_versioned.cil \
    -o /data/local/tmp/neo9_secilc_check -f /dev/null
rm -f /data/local/tmp/neo9_secilc_check
echo "secilc check PASSED (exit 0)!"

sync
/system/bin/umount "$MNT"
rmdir "$MNT"

echo "[4/5] Shrinking patched ext4 image..."
/system/bin/e2fsck -f -y "$WORK_IMG" || [ $? -le 2 ]
/system/bin/resize2fs -M "$WORK_IMG"

echo "[5/5] Auto-checking SELinux genfscon conflicts & signing via avbforge-core..."
"$AVBFORGE_CORE" gsi sign \
    --input "$WORK_IMG" \
    --output "$OUT_IMG" \
    --key "$KEY_PEM" \
    --fix-vendor-genfs true

echo "DONE! Output ready at: $OUT_IMG"
