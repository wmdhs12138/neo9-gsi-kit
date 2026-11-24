# neo9-gsi-kit

Modular, low-level GSI hardware adaptation kit for **vivo iQOO Neo9 (`V2338A` / `PD2338`, Qualcomm SM8550, Vendor Android 13 / VNDK 33)**.

Built and verified on **Android 17 (SDK 37) GSI** (Infinity-X 4.2) running via DSU over OriginOS 4 host vendor, with offline SELinux policy verification (`secilc`), ART bytecode verification (`dex2oat64`), and AVB 2.0 hashtree signing via [AVBForge](https://github.com/wmdhs12138/avbforge).

> [!WARNING]
> **Disclaimer**: Modifying system images, vendor bind-mounts, and AVB-signed GSI images carries risk of bootloops or data loss. Always test new images with **DSU single-boot** first before flashing any permanent partition. Proprietary vendor/QTI APKs and signing keys are intentionally excluded from this repository.

## Hardware Adaptation Status

| Module | Status | Highlights |
| :--- | :--- | :--- |
| **1. Optical UDFPS (`udfps/`, `overlay/`)** | Verified OK | Stock-native kernel protocol for Goodix `gf9578` + `vivo_display.ko` Local HBM + `vivo_ts.ko` VTS GT9916 THP. Deterministic `--patch-hal` for `(630, 2533, r=110)`, hardware VBLANK `FINGERSHOOTER_SHOW=1` uevent sync, and session-gated screen-off arming (zero false triggers when no fingerprint is enrolled). |
| **2. 5G SA + VoLTE/VoNR + Bidirectional IMS SMS (`telephony/`)** | Verified OK | Restores PD2338 5G NR SA (`33,33`) & DSDS props, binds Qualcomm hardware AIDL IMS (`org.codeaurora.ims` → `IImsRadio/imsradio0`), patches DEX protected-broadcast calls & 27 `ImsMedia`/`ImsCamera` VT JNI callers, adds China (`MCC 460`) VoLTE/VoNR/SMSoIP CarrierConfig RRO, and bind-mounts `qcrilNr.db` with `persist.vendor.radio.poweron_opt=0` so `qcrilNrd` delivers incoming MT SMS immediately instead of buffering forever. |

## Architecture Overview

```text
neo9-gsi-kit/
├── build.sh                        # On-device Termux builder (clang + aapt + openssl + python3)
├── apply_neo9_patches.sh           # Modular ext4 GSI patcher + SELinux xattr/secilc checks + AVB signing
├── udfps/
│   └── neo9-udfps-bridge.c         # HAL SensorLocation patcher + IFingerprintExt / Local HBM / VTS daemon
├── overlay/
│   ├── build_overlay.py            # Zero-JDK APK Signature Scheme v2 RRO builder/signer
│   ├── AndroidManifest.xml         # Static RRO targeting framework-res (UDFPS / display config)
│   └── res/values/config.xml
└── telephony/
    ├── patch_qti_ims.py            # Deterministic AXML + DEX patcher for PHH ims-caf-u.apk
    ├── build_ims_overlays.py       # Builds CAF IMS phone binding + China CarrierConfig RROs
    ├── build_qcril_db.py           # Pulls & patches /vendor/etc/qcril_database/qcrilNr.db
    ├── imsmedia_jni_stub.c         # Zero-dependency Android 17 JNI stub for libimsmedia_jni.so
    ├── imscamera_jni_stub.c        # Zero-dependency Android 17 JNI stub for libimscamera_jni.so
    ├── cafims_phone/               # RRO binding com.android.phone to org.codeaurora.ims
    ├── carrier_config/             # RRO enabling VoLTE / 5G SA VoNR / SMS over IMS (MCC 460)
    ├── privapp-permissions-qti-ims.xml
    ├── default-permissions-qti-ims.xml
    └── hiddenapi-whitelist-qti-ims.xml
```

### Technical Notes

1. **Optical UDFPS (`neo9-udfps-bridge.c`)**:
   - At `on post-fs` (`rw-system.sh`), `--patch-hal` patches a tmpfs copy of `/vendor/bin/hw/vendor.vivo.hardware.biometrics.fingerprint-service` so `IFingerprint::getSensorProps()` reports PD2338's exact optical sensor geometry `(630, 2533, r=110)` and bind-mounts it with original SELinux context before `late_start`.
   - Listens on `NETLINK_KOBJECT_UEVENT` for `vivo_display.ko`'s hardware VBLANK event (`FINGERSHOOTER_SHOW=1`) to fire `IFingerprintExt` `CMD_ON_SHOOTER_READY` (`32572`) with exact stock timing across all refresh rates.
   - Tracks active HAL authentication sessions (`authenticate` / `enroll`) so screen-off `/sys/fingerprint/hbm_notify_enable` & VTS gesture detection are only armed when fingerprint authentication is actually active (matching OriginOS behavior when no fingerprint is enrolled).

2. **Cellular / IMS / SMS (`telephony/`)**:
   - **IMS Binder Integration**: Repacks PHH Treble's `ims-caf-u.apk` (`org.codeaurora.ims`) under a local APK-v2 key registered in `system_ext_mac_permissions.xml` + `system_ext_seapp_contexts` (`domain=radio`), removing `sharedUserId="android.uid.phone"` and `<protected-broadcast>` tags, NOPing 8 void `Context.sendBroadcast*` calls in `classes.dex`, and stubbing all 27 `ImsMedia` / `ImsCamera` native JNI wrapper methods (preventing `UnsatisfiedLinkError` on `ImsMedia.nativeRequestRtpDataUsage` during incoming calls).
   - **Incoming SMS (`qcrilNr.db`)**: On PD2338, `qcrilNrd` reads `persist.vendor.radio.poweron_opt` from SQLite (`/vendor/etc/qcril_database/qcrilNr.db` via `libril-db.so`'s `qcril_config_get()`), ignoring `setprop`. With `poweron_opt=1` and no OriginOS `QtiTelephony` sending `OEM_HOOK` `UI_READY`, `libqcrilNrSmsModule.so` buffers incoming MT SMS indefinitely without sending ACK. `rw-system.sh` bind-mounts a patched `neo9_qcrilNr.db` (`poweron_opt=0`, `u:object_r:vendor_configs_file:s0`) before `qcrilNrd` starts.

## Prerequisites (On-Device Termux)

- Rooted vivo iQOO Neo9 (`PD2338`) running stock OriginOS 4 vendor (Android 13 / VNDK 33)
- Termux packages: `clang`, `python`, `openssl-tool`, `aapt` (or Android build-tools `aapt`)
- [AVBForge](https://github.com/wmdhs12138/avbforge) installed (used by `apply_neo9_patches.sh` for AVB footer erase & hashtree signing)
- Upstream PHH Treble `ims-caf-u.apk` placed at `telephony/ims-caf-u.apk`

## Usage

### 1. Build All Components

```bash
bash build.sh
```

This generates:
- `udfps/neo9-udfps-bridge`
- `telephony/libimsmedia_jni.so` & `telephony/libimscamera_jni.so`
- `overlay/Neo9UdfpsOverlay.apk` (auto-creates local signing key `overlay/neo9_overlay_key.pem`)
- `telephony/Neo9CafImsPhoneOverlay.apk` & `telephony/Neo9CarrierConfigOverlay.apk`
- `telephony/Neo9QtiIms.apk` (if `telephony/ims-caf-u.apk` is present)
- `telephony/neo9_qcrilNr.db` (extracted from `/vendor/etc/qcril_database/qcrilNr.db` via `su`)

### 2. Patch & Sign a Raw GSI Image

```bash
su -c 'sh /path/to/neo9-gsi-kit/apply_neo9_patches.sh \
    /path/to/base_gsi.img \
    /path/to/output_signed_gsi.img \
    /path/to/avb_key.pem'
```

`apply_neo9_patches.sh` automatically verifies:
- Every patched file's SELinux xattr (`phhsu_exec`, `mac_perms_file`, `seapp_contexts_file`, `file_contexts_file`)
- Full offline `secilc` compilation against host `/vendor/etc/selinux/vendor_sepolicy.cil`
- AVB 2.0 hashtree signing & vendor `genfscon` conflict checks via `avbforge-core`

### 3. Test Safely via DSU

Install the signed `.img` with DSU Sideloader (single-boot mode) and reboot to verify.

## License

[MIT](LICENSE)
