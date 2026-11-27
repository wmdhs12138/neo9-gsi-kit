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
| **3. Stock OriginOS 4 Camera + GSI Aperture (`camera/`)** | Verified OK | Full OriginOS 4 `VivoCamera` + `Vivo3rdAlgoService` + `vivocameraserver` (`media.vivocamera`) port AND full GSI LineageOS Aperture support (Main / Ultrawide / Front photo & video). Routes A17 `cameraserver` exclusively to Vivo VCF (`vendorVcf/0`) with `IVivoCameraProvider@1.0::setparam` app-type bridging, fixes A17 front 16MP `sensorPixelMode` mismatch & `JpegR` capture hang, bridges real hardware `IVPerf@1.0` PerfLock/CPU-limit via `HwBinder`, restores SurfaceFlinger dynamic refresh-rate & Display-P3 wide color, silences CamX/CHI logcat storm, AOT-compiles `VivoCamera`, and integrates thumbnail review directly with GSI Glimpse gallery (`REVIEW` album mode). |
| **4. RM692H5 8T LTPO + 144 Hz + 360 Hz High-TE EPT + VPerf Scroll Boost (`display/`, `udfps/`, `overlay/`)** | Verified OK | Full OriginOS 4 frame-pacing & scroll-perf stack: 12-byte vendor Composer HAL patch (`--patch-composer`) exposing 5 zero-duplicate HWC configs (`60 / 72 / 90 / 120 Hz` 8T LTPO + `144 Hz`) & config groups; `libneo9_sf_hook.so` (`surfaceflinger` `DT_NEEDED`, zero hardcoded offsets) forwarding A17's real `expectedPresentTime` via Vivo HIDL 2.4 opcode `0x10020002` → `DRMConnector "EPT"` → `msm_drm.ko` 360 Hz High-TE commit delay, unlocking `HidlComposer::isSupported(ExpectedPresentTime)` (`presentOrValidateDisplay` `SKIP_VALIDATE` ~81.5%, removing SF `sleep_until`), and emitting `EPT=0` at 144 Hz for native 144 Hz TE; stock `IVPerf@1.0` (`0x00001080`) touch/scroll/fling CPU/WALT boost in `neo9-udfps-bridge` (replacing PD2338's no-op AIDL `Power::setBoost`/`createHintSession` stub); brightness-tiered `1 / 5 / 10 Hz` 8T LTPO controller; and OriginOS-style Settings refresh-rate UI (`Neo9SettingsOverlay.apk`). |
| **5. Closed-Source Adreno 740 GPU Stack — Vulkan `1.4.295` (`V@0863.1`) + GLES 3.2 (`V@0800.77`) (`gpu/`)** | Verified OK | Upgrades Adreno 740 GPU drivers from stock `V@0676.49` (Vulkan `1.3.128`) to closed-source Qualcomm **`V@0863.1` Vulkan `1.4.295`** (**161 extensions** under Android 17 native Vulkan 1.4.0 loader, 6/6 hardware Core 1.4 tests `BIT-EXACT`) + **`V@0800.77` OpenGL ES 3.2** (`SurfaceFlinger` confirmed) while preserving stock OpenCL 3.0 & camera algorithm compatibility. Uses a zero-overlayfs, zero-RAM pure bind-mount layout (`/vendor/lib64/egl` + 5 individual GPU files, `same_process_hal_file`), leaving all 1640+ stock vendor libs untouched, plus `android.hardware.vulkan.version-1_4.xml` (`4210688`) and boot verifier `neo9-vk14-verify` (`/metadata/gsi/dsu/neo9_gpu.log`). |

## Architecture Overview

```text
neo9-gsi-kit/
├── build.sh                        # On-device Termux builder (clang/clang++ + aapt + openssl + python3)
├── apply_neo9_patches.sh           # Modular ext4 GSI patcher + SELinux xattr/secilc checks + AVB signing
├── udfps/
│   └── neo9-udfps-bridge.c         # HAL SensorLocation + Composer patcher + IFingerprintExt / Local HBM / VTS + 8T LTPO + IVPerf@1.0 scroll boost daemon
├── display/
│   ├── neo9_sf_hook.c              # surfaceflinger DT_NEEDED hook: stock 0x10020002 EPT bridge + ExpectedPresentTime/SKIP_VALIDATE + 144 Hz EPT=0 guard
│   ├── patch_surfaceflinger.py     # Replaces libpowermanager.so DT_NEEDED in /system/bin/surfaceflinger with libneo9_sf_hook.so
│   └── probe_refresh.sh            # Read-only refresh-rate & RM692H5 LTPO diagnostic probe
├── overlay/
│   ├── build_overlay.py            # Zero-JDK APK Signature Scheme v2 RRO builder/signer
│   ├── patch_settings_overlay_xml.py # Offline AXML patcher for Settings screen_refresh_rate.xml (verified via idmap2)
│   ├── AndroidManifest.xml         # Static RRO targeting framework-res (UDFPS / display / auto-brightness / color modes)
│   ├── settings_overlay/           # Static RRO targeting com.android.settings (color modes + refresh-rate UI)
│   └── res/values/config.xml
├── telephony/
│   ├── patch_qti_ims.py            # Deterministic AXML + DEX patcher for PHH ims-caf-u.apk
│   ├── build_ims_overlays.py       # Builds CAF IMS phone binding + China CarrierConfig RROs
│   ├── build_qcril_db.py           # Pulls & patches /vendor/etc/qcril_database/qcrilNr.db
│   ├── imsmedia_jni_stub.c         # Zero-dependency Android 17 JNI stub for libimsmedia_jni.so
│   ├── imscamera_jni_stub.c        # Zero-dependency Android 17 JNI stub for libimscamera_jni.so
│   ├── cafims_phone/               # RRO binding com.android.phone to org.codeaurora.ims
│   ├── carrier_config/             # RRO enabling VoLTE / 5G SA VoNR / SMS over IMS (MCC 460)
│   ├── privapp-permissions-qti-ims.xml
│   ├── default-permissions-qti-ims.xml
│   └── hiddenapi-whitelist-qti-ims.xml
└── camera/
    ├── neo9_cam_hook.cpp           # cameraserver DT_NEEDED hook: blocks raw vendor_qti/0, bridges IVivoCameraProvider@1.0::setparam
    ├── patch_cameraserver.py       # Patches A17 cameraserver DT_NEEDED & isSensorPixelModeConsistent() front 16MP mismatch
    ├── vivo_vcs_compat.cpp         # 4-symbol A14->A17 ABI shim (libvivo_vcs_comp.so) for vivocameraserver
    ├── patch_vivocameraserver.py   # Replaces libhidltransport.so DT_NEEDED with libvivo_vcs_comp.so
    ├── build_compat_dex.py         # Pure-Python DEX 035 compiler for classes15.dex (21 compat classes + real IVPerf@1.0 HwBinder client)
    ├── extract_vivocamera_dex.py   # Extracts genuine android.hardware.vivocamera.* classes16.dex (strips hiddenapi section)
    ├── patch_vivo_camera.py        # Fast streaming patcher & APK-v2 signer for VivoCamera + Vivo3rdAlgoService (incl. Glimpse gallery redirect)
    ├── build_aperture_overlay.py   # Builds Neo9ApertureOverlay.apk exposing Main (0), Front (1), Ultrawide (3)
    ├── aperture_overlay/           # Static RRO for org.lineageos.aperture
    ├── manifest_neo9_vivo_camera.xml
    ├── privapp-permissions-vivo-camera.xml
    ├── default-permissions-vivo-camera.xml
    └── hiddenapi-whitelist-vivo-camera.xml
└── gpu/
    ├── README.md                   # Module 5 zero-overlayfs bind-mount architecture (proprietary .so excluded via .gitignore)
    ├── neo9_vk14_verify.c          # Zero-header Vulkan 1.4 Core hardware verification binary (/system/bin/neo9-vk14-verify)
    └── android.hardware.vulkan.version-1_4.xml # Declares PackageManager android.hardware.vulkan.version = 1.4.0 (0x00404000)
```

### Technical Notes

1. **Optical UDFPS (`neo9-udfps-bridge.c`)**:
   - At `on post-fs` (`rw-system.sh`), `--patch-hal` patches a tmpfs copy of `/vendor/bin/hw/vendor.vivo.hardware.biometrics.fingerprint-service` so `IFingerprint::getSensorProps()` reports PD2338's exact optical sensor geometry `(630, 2533, r=110)` and bind-mounts it with original SELinux context before `late_start`.
   - Listens on `NETLINK_KOBJECT_UEVENT` for `vivo_display.ko`'s hardware VBLANK event (`FINGERSHOOTER_SHOW=1`) to fire `IFingerprintExt` `CMD_ON_SHOOTER_READY` (`32572`) with exact stock timing across all refresh rates.
   - Tracks active HAL authentication sessions (`authenticate` / `enroll`) so screen-off `/sys/fingerprint/hbm_notify_enable` & VTS gesture detection are only armed when fingerprint authentication is actually active (matching OriginOS behavior when no fingerprint is enrolled).

2. **Cellular / IMS / SMS (`telephony/`)**:
   - **IMS Binder Integration**: Repacks PHH Treble's `ims-caf-u.apk` (`org.codeaurora.ims`) under a local APK-v2 key registered in `system_ext_mac_permissions.xml` + `system_ext_seapp_contexts` (`domain=radio`), removing `sharedUserId="android.uid.phone"` and `<protected-broadcast>` tags, NOPing 8 void `Context.sendBroadcast*` calls in `classes.dex`, and stubbing all 27 `ImsMedia` / `ImsCamera` native JNI wrapper methods (preventing `UnsatisfiedLinkError` on `ImsMedia.nativeRequestRtpDataUsage` during incoming calls).
   - **Incoming SMS (`qcrilNr.db`)**: On PD2338, `qcrilNrd` reads `persist.vendor.radio.poweron_opt` from SQLite (`/vendor/etc/qcril_database/qcrilNr.db` via `libril-db.so`'s `qcril_config_get()`), ignoring `setprop`. With `poweron_opt=1` and no OriginOS `QtiTelephony` sending `OEM_HOOK` `UI_READY`, `libqcrilNrSmsModule.so` buffers incoming MT SMS indefinitely without sending ACK. `rw-system.sh` bind-mounts a patched `neo9_qcrilNr.db` (`poweron_opt=0`, `u:object_r:vendor_configs_file:s0`) before `qcrilNrd` starts.

3. **Camera Stack (`camera/`)**:
   - **VCF Provider Routing & `IVivoCameraProvider@1.0` Bridge**: `libneo9_cam_hook.so` (loaded directly by `/system/bin/cameraserver` via `DT_NEEDED`, zero `libc++_shared.so`) blocks A17 `cameraserver` from binding raw Qualcomm `vendor_qti/0` so it binds exclusively to Vivo's VCF provider (`vendorVcf/0`, 4 logical cameras + all 1411 vendor tags + automatic `usePureBypass` for 3rd-party apps), and interposes `BnCameraService::onTransact` (`connect` / `connectDevice`) to notify `IVivoCameraProvider@1.0::setparam(1=appType, 33=2, 11=0, 12=-1, 25=0)`.
   - **Android 17 Compatibility & Bug Fixes**:
     - `patch_cameraserver.py` patches `CameraDeviceClient::isSensorPixelModeConsistent()` so Front 16MP (`4608x3456`) max-resolution captures with `CONTROL_SENSOR_PIXEL_MODE=0` succeed without rejecting (`code 3`) or hanging front→back switches.
     - `rw-system.sh` bind-mounts `/vendor/lib64/com.qualcomm.mcx.policy.mfl.so` (4-byte fix at `0x28080` preventing NULL-deref SIGSEGV in `GetActiveCameraMaskBasedOnVcfSat` during lens/zoom switch) and `/vendor/etc/camera/camxoverridesettings.txt` (`logInfoMask=0x0`, `overrideLogLevels=0x0` silencing ~100k lines/min CamX/CHI logcat flood).
     - `ro.camera.disableJpegR=true` + `ro.camera.disableHeicUltraHDR=true` + `vendor.camera.aux.packagelist` + `Neo9ApertureOverlay.apk` fix GSI Aperture photo capture (`JpegRCompositeStream` `requestId -1` hang) and expose Main (0), Front (1), and Ultrawide (3).
   - **VivoCamera DEX Compat, Hardware `IVPerf@1.0`, Thermal/Color Tuning & Glimpse Gallery**:
     - `classes15.dex` (`build_compat_dex.py`) supplies 21 missing Vivo framework/widget classes (including `BbkMoveBoolButton implements Checkable` for Micro Movie) AND a live `android.os.HwBinder` implementation of `VivoPerfManager` talking directly to `vendor.vivo.hardware.vperf@1.0::IVPerf/default` (`perfLockAcquire` tx `2` / `perfLockRelease` tx `3`), restoring both `VivoBoostFrameworkUtil` launch/switch boosts and `VivoCpuLimitUtil` sustained thermal CPU ceilings.
     - `patch_vivo_camera.py` lowers `targetSdkVersion` (`34→33`), NOPs Vivo-only `setNightMode(I)V` calls, stubs `TrackProcessor` & `isExternalStorageManager`, null-guards `Vcf2VifImageEchoUtil`, and redirects `ActivityStartManager.launchGalleryReview` / `gotoGalleryPic` / `gotoGalleryEditor` to GSI Glimpse (`android.provider.action.REVIEW` + `FLAG_GRANT_READ_URI_PERMISSION`, fallback `ACTION_VIEW`).
     - Disables Android 17 software `FrameRateOverride` / `frame_rate_category_mrr` / `use_content_detection_for_refresh_rate` (`frame_rate_multiple_threshold=0`), sets Display-P3 wide color management (`has_wide_color_display=true`, `use_color_management=true`, `wcg_composition_dataspace=143261696`, `persist.sys.cct_cali_flag=1`), `_kalama` MediaCodec perf tables, and one-shot ART AOT `speed` compilation of `VivoCamera` & `Vivo3rdAlgoService` on first boot.

4. **RM692H5 8T LTPO + 144 Hz + 360 Hz High-TE EPT + VPerf Scroll Boost (`display/`, `udfps/`, `overlay/`)**:
   - **Vendor Composer HAL Unlock & Deduplication (`--patch-composer`)**: On PD2338, `vendor.qti.hardware.display.composer-service` filters out 144 Hz (`config 1`) and omits OriginOS `CONFIG_GROUP` assignments unless the client queries proprietary attribute `9600`, while exposing 3 duplicate non-LTPO configs (`2`: 120 Hz, `3`: 90 Hz, `6`: 60 Hz). `rw-system.sh` applies a deterministic 12-byte patch to a tmpfs copy of the vendor composer at `on post-fs` (`0x7ee0c`: `b 0x7ee58`, `0x7f468`: `nop`, `0x883a0`: `cmp x21, #0x1`) and bind-mounts it with `hal_graphics_composer_default_exec` before composer starts, exposing the 5 unique zero-duplicate configs (`60 / 72 / 90 / 120 Hz` LTPO in `group=0` and `144 Hz` in `group=1`) across any GSI build.
   - **Stock 360 Hz High-TE EPT Bridge & `SKIP_VALIDATE` (`display/neo9_sf_hook.c`)**: On PD2338, `msm_drm.ko` (`vivo_display_pre_kickoff_update` → `vivo_commit_delay_target_time`) paces 360 Hz High-TE panel commits (`60 / 72 / 90 / 120 Hz`) using DRMConnector property `"EPT"`, which stock OriginOS 4 `surfaceflinger` feeds via Vivo HIDL 2.4 FMQ command `0x10020002` + `lo32/hi32(EPT)` into `composer-service` (`HWCDisplay::SetExpectedPresentTime`). `libneo9_sf_hook.so` (loaded into `/system/bin/surfaceflinger` via `DT_NEEDED`, zero hardcoded offsets, zero `libc++_shared.so`) dynamically scans `HidlComposer`'s vtable in RELRO and:
     - Emits `0x10020002` with A17's exact `expectedPresentTime` on `60 / 72 / 90 / 120 Hz`, and `EPT=0` on `144 Hz` (`frame_interval_ns < 7.5 ms`) because PD2338's `timing@144` uses native 144 Hz TE while `msm_drm.ko` hardcodes `2.778 ms` (360 Hz) whenever `EPT > 0`;
     - Hooks `HidlComposer::isSupported(OptionalFeature::ExpectedPresentTime = 1)` → `true`, enabling `Feature::kExpectedPresentTime` in A17 `SurfaceFlinger` so `FrameTargeter` skips software `mEarliestPresentTime`, unlocking `presentOrValidateDisplay()` (`SKIP_VALIDATE` ~81.5%) and eliminating per-frame SF main-thread `sleep_until` stalls (`HWC missed` drops from +400/+567 to ~0.2%).
   - **Stock `IVPerf@1.0` Touch/Scroll/Fling Boost (`udfps/neo9-udfps-bridge.c`)**: Reverse-engineering `/vendor/bin/hw/android.hardware.power-service` confirmed that PD2338 stubs both AIDL `IPower::setBoost()` and `IPower::createHintSession()` (ADPF) as no-ops, routing all real SM8550 scroll/touch CPU/WALT/DDR boosts through `vendor.vivo.hardware.vperf@1.0::IVPerf/default`. `neo9-udfps-bridge` reuses its existing `vivo_ts` touchscreen input fd and calls `IVPerf@1.0::perfHintAcqRel(0x00001080)` (Type `1` Vertical Scroll / Type `5` Light Scroll from `/vendor/etc/perf/perfboostsconfig.xml`) via `/system/lib64/libhidlbase.so` during screen-on touch/drag/fling, with automatic timeout expiry and immediate `perfLockRelease` on screen-off (slashing 99th frame time from `16–17 ms` to `9–10 ms` and jank to `0.50%–0.93%`).
   - **OriginOS-Style Refresh-Rate UI & Stock Pipeline Durations**: `Neo9SettingsOverlay.apk` (`patch_settings_overlay_xml.py`, verified offline via A17 `idmap2` and `aapt dump xmltree`) strips `min_refresh_rate` and `extreme_refresh_rate` from `screen_refresh_rate.xml`, renames `peak_refresh_rate` to **屏幕刷新率** (`60 / 72 / 90 / 120 / 144 Hz` alongside Per-App refresh rate), and appends a SettingsLib `FooterPreference` (`144 Hz 不支持 8T LTPO，功耗较高`). `rw-system.sh` matches stock `advanced_sf_offsets.xml` v4 durations (`SF=10.333ms`, `App=10.500ms`) + `enable_gl_backpressure=1` + stock HWUI caches, and disables A17 software 60 Hz heuristics (`enable_frame_rate_override`, `frame_rate_category_mrr`, `use_content_detection_for_refresh_rate`) so `120 Hz` never drops to 60 fps while hardware 8T LTPO handles static 1/5/10 Hz power saving.
   - **Brightness-Tiered 8T LTPO Controller (`neo9-udfps-bridge`)**: Mirrors OriginOS 4's `RrpConfig.json` brightness rules against `/sys/lcm/bl_level` (via `LcmMapNit.json` calibration) and drives `/sys/lcm/ltpo_mode` with hysteresis whenever `auto_mode=1`:
     - `>= 200 nit` (`bl_level >= 2009`, hysteresis `1960`): `30-5-5-1-3-8-3` (**1 Hz** 8T LTPO)
     - `90..200 nit` (`bl_level >= 1310`, hysteresis `1260`): `30-20-10-5` (**5 Hz** dark-gray-safe LTPO)
     - `< 90 nit`: `30-20-10` (**10 Hz** low-brightness anti-flicker floor)
   - Automatically pauses `/sys/lcm/ltpo_mode` writes when `auto_mode=0` (144 Hz mode) or during Local HBM UDFPS capture.
   - Use `su -c "sh '$PWD/display/probe_refresh.sh' --sample"` for read-only on-device inspection of DRM modes, `DisplayManager` policy, `SurfaceFlinger` active config, and DDIC `refresh_monitor`.

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
- `camera/libvivo_vcs_comp.so` & `camera/libneo9_cam_hook.so`
- `display/libneo9_sf_hook.so`
- `overlay/Neo9UdfpsOverlay.apk`, `overlay/Neo9SettingsOverlay.apk`, `telephony/Neo9CafImsPhoneOverlay.apk`, `telephony/Neo9CarrierConfigOverlay.apk`, & `camera/Neo9ApertureOverlay.apk` (auto-creates local signing key `overlay/neo9_overlay_key.pem`)
- `telephony/Neo9QtiIms.apk` (if `telephony/ims-caf-u.apk` is present)
- `telephony/neo9_qcrilNr.db` (extracted from `/vendor/etc/qcril_database/qcrilNr.db` via `su`)
- `camera/neo9_vivocameraserver`, `camera/libvivocameraservice.so`, `camera/Neo9VivoCamera.apk`, & `camera/Neo9Vivo3rdAlgoService.apk` (extracted & patched from stock OriginOS 4 host `/system/`)

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
