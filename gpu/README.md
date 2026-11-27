# Module 5: Closed-Source Adreno 740 GPU Stack (`V@0863.1` Vulkan 1.4.295 + `V@0800.77` GLES 3.2)

Upgrades the stock OriginOS 4 Vendor 33 Adreno 740 GPU driver (`V@0676.49`, 2023-10, Vulkan `1.3.128`, 114 extensions) inside the Android 17 GSI to:
- **Vulkan**: Qualcomm closed-source **`V@0863.1`** (`512.863.1`, Vulkan **`1.4.295`**, **161 system extensions** under Android 17's native Vulkan 1.4.0 loader)
- **OpenGL ES / EGL / SurfaceFlinger**: Qualcomm closed-source **`V@0800.77`** (`2026-06-11`, OpenGL ES 3.2)
- **OpenCL / Camera ISP algorithms**: Preserves stock `/vendor/lib64/libOpenCL.so` + `libCB.so` + `libllvm-qcom.so` (80/80 `libgsl.so` symbols compatible)

## Zero-OverlayFS Pure Bind-Mount Architecture (Verified OK on `v76.img`)
- Both Android `[system] sphal` and `[vendor]` linker namespaces already include `/vendor/lib64/egl` in `search.paths`, and `/vendor/lib64/egl` contains exclusively the 7 Adreno EGL libraries.
- `apply_neo9_patches.sh` stages the 21 verified 64-bit `.so` blobs from `gpu/blobs/lib64/` (gitignored; built by `~/projects/neo9-adreno-vulkan`) with exact stock `u:object_r:same_process_hal_file:s0` labels into:
  - `/system/system_ext/neo9_adreno_v863/egl/` (16 files: 7 EGL libs + 9 new companion/V863/shim libs including `libc35.so`, `libmapperutils.so`, `android.hardware.graphics.allocator-V2-ndk.so`, `adrenoutils_extra.so`, `notgsl.so`, `notadreno_utils.so`, `notllvm-*.so`) → single `mount -o bind` onto `/vendor/lib64/egl`
  - `/system/system_ext/neo9_adreno_v863/bind/` (5 existing GPU libs: `vulkan.adreno.so`, `libgsl.so`, `libadreno_utils.so`, `libllvm-glnext.so`, `libllvm-qgl.so`) → 5 single-file `mount -o bind` onto `/vendor/lib64/hw/vulkan.adreno.so` and `/vendor/lib64/*.so`
- Zero `overlayfs` on `/vendor/lib64`, zero `tmpfs` RAM usage, leaving all 1640+ stock vendor libraries (`keymint`, `qseecomd`, `OpenCL`, `camera`, `audio`, `RIL`) completely untouched.
- Installs `android.hardware.vulkan.version-1_4.xml` (`version="4210688"`, `0x00404000`) into `/system/system_ext/etc/permissions/` and `/system/bin/neo9-vk14-verify` (14 KB self-check binary logging full verification to `/metadata/gsi/dsu/neo9_gpu.log` via `phh-on-boot.sh`).
