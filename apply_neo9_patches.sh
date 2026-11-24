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
CAFIMS_PHONE_OVERLAY="$KIT_DIR/telephony/Neo9CafImsPhoneOverlay.apk"
CARRIER_CFG_OVERLAY="$KIT_DIR/telephony/Neo9CarrierConfigOverlay.apk"
QTI_IMS_APK="$KIT_DIR/telephony/Neo9QtiIms.apk"
QTI_IMS_PERM="$KIT_DIR/telephony/privapp-permissions-qti-ims.xml"
QTI_IMS_DEFPERM="$KIT_DIR/telephony/default-permissions-qti-ims.xml"
QTI_IMS_SYSCONFIG="$KIT_DIR/telephony/hiddenapi-whitelist-qti-ims.xml"
QTI_IMS_MEDIA_SO="$KIT_DIR/telephony/libimsmedia_jni.so"
QTI_IMS_CAM_SO="$KIT_DIR/telephony/libimscamera_jni.so"
QCRIL_NR_DB="$KIT_DIR/telephony/neo9_qcrilNr.db"
OVERLAY_CERT_DER="$KIT_DIR/overlay/neo9_overlay_cert.der"
AVBFORGE_CORE="$(pm path io.github.fu.avbforge | sed 's/package://;s|base.apk|lib/arm64/libavbforge_core.so|')"

# Stage at a short FUSE path (<= 64 bytes for toybox losetup, buffered I/O to avoid F2FS inlinecrypt RW error)
WORK_IMG="${WORK_IMG:-/storage/emulated/0/Download/AVBForge/.n9p.img}"
MNT="/data/local/tmp/neo9_gsi_mnt"

cleanup() {
    sync 2>/dev/null || true
    umount "$MNT" 2>/dev/null || true
    rmdir "$MNT" 2>/dev/null || true
    rm -f "$WORK_IMG" 2>/dev/null || true
}
trap cleanup EXIT

echo "[1/5] Copying base GSI to short FUSE staging path..."
rm -f "$WORK_IMG"
cp "$IN_IMG" "$WORK_IMG"

echo "[2/5] Stripping old AVB footer and preparing RW ext4..."
"$AVBFORGE_CORE" avb erase-footer --image "$WORK_IMG" >/dev/null || true
dd if=/dev/zero bs=1048576 count=512 >> "$WORK_IMG" 2>/dev/null
/system/bin/e2fsck -f -y "$WORK_IMG" || [ $? -le 2 ]
/system/bin/resize2fs "$WORK_IMG"
/system/bin/e2fsck -f -y -E unshare_blocks "$WORK_IMG" || [ $? -le 2 ]
/system/bin/e2fsck -f -y "$WORK_IMG" || [ $? -le 2 ]

echo "[3/5] Mounting GSI RW and installing Module 1: Neo9 Optical UDFPS..."
mkdir -p "$MNT"
mount -o rw,loop "$WORK_IMG" "$MNT"

# 3a. Install neo9-udfps-bridge binary with phhsu_exec context (required entrypoint for u:r:phhsu_daemon:s0!)
# Seed inode xattr from rw-system.sh (guaranteed u:object_r:phhsu_exec:s0) + overwrite in place.
rm -f "$MNT/system/bin/neo9-udfps-bridge"
cp -a "$MNT/system/bin/rw-system.sh" "$MNT/system/bin/neo9-udfps-bridge"
cat "$BRIDGE_BIN" > "$MNT/system/bin/neo9-udfps-bridge"
chmod 0755 "$MNT/system/bin/neo9-udfps-bridge"
chown 0:2000 "$MNT/system/bin/neo9-udfps-bridge"
chcon u:object_r:phhsu_exec:s0 "$MNT/system/bin/neo9-udfps-bridge" 2>/dev/null || true

# Register persistent file_contexts entry for neo9-udfps-bridge (preserve existing xattr by appending in-place)
SE_FC="$MNT/system/system_ext/etc/selinux/system_ext_file_contexts"
if [ -f "$SE_FC" ] && ! grep -q "neo9-udfps-bridge" "$SE_FC"; then
    echo "/system/bin/neo9-udfps-bridge u:object_r:phhsu_exec:s0" >> "$SE_FC"
    chmod 0644 "$SE_FC"
fi

# Allow phhsu_daemon <-> hal_fingerprint_server / system_server / radio bidirectional Binder IPC
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

# 3b. Install native static RRO Overlays (UDFPS + Qualcomm CAF IMS Phone binding + China VoLTE/VoNR CarrierConfig)
for ov in "$OVERLAY_APK" "$CAFIMS_PHONE_OVERLAY" "$CARRIER_CFG_OVERLAY"; do
    if [ -f "$ov" ]; then
        b="$(basename "$ov")"
        cp "$ov" "$MNT/system/product/overlay/$b"
        chmod 0644 "$MNT/system/product/overlay/$b"
        chown 0:0 "$MNT/system/product/overlay/$b"
        chcon u:object_r:system_file:s0 "$MNT/system/product/overlay/$b"
    fi
done

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
        chmod 0644 "$SE_SEAPP"
        chown 0:0 "$SE_SEAPP"
        chcon u:object_r:seapp_contexts_file:s0 "$SE_SEAPP"
    fi
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
    # Ensure neo9-udfps-bridge daemon is running even if boot_completed fired before phh-on-boot
    pidof neo9-udfps-bridge >/dev/null 2>&1 || /system/bin/neo9-udfps-bridge </dev/null >/dev/null 2>&1 &

    # Grant runtime permissions for Qualcomm Hardware CAF IMS (org.codeaurora.ims)
    pm grant org.codeaurora.ims android.permission.READ_PHONE_STATE 2>/dev/null || true
    pm grant org.codeaurora.ims android.permission.POST_NOTIFICATIONS 2>/dev/null || true
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
    "$MNT/system/system_ext/etc/selinux/system_ext_file_contexts:file_contexts_file" \
    "$MNT/system/system_ext/etc/selinux/system_ext_mac_permissions.xml:mac_perms_file" \
    "$MNT/system/system_ext/etc/selinux/system_ext_seapp_contexts:seapp_contexts_file"; do
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
umount "$MNT"
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
