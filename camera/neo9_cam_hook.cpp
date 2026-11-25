// Neo9 GSI CameraService -> IVivoCameraProvider@1.0 Bridge Hook (libneo9_cam_hook.so)
// Loaded directly by /system/bin/cameraserver via DT_NEEDED (replaces libcamera_client.so in
// cameraserver's DT_NEEDED and re-links libcamera_client.so as its own DT_NEEDED).
//
// OriginOS 4's cameraserver calls IVivoCameraProvider@1.0::setparam(1, appType) and
// setparam(33, vcfAppType) before opening any camera so Qualcomm CamX/CHI (33 vendor modules
// calling VivoHidlInterface::GetAppType()) knows whether the client is stock VivoCamera (1)
// or a 3rd-party camera like Aperture (2). Without this, gAppType stays 0 (invalid) whenever
// ro.vivo.os.version is set, breaking capture & lens switching for BOTH VivoCamera and Aperture.

#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <android/log.h>

#define LOG_TAG "neo9_cam_hook"
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)

namespace {

// Exact 24-byte libc++ SSO std::string layout on arm64 Android (short mode: byte[0] = len << 1)
struct LibcxxSsoString {
    uint8_t raw[24];
    LibcxxSsoString(const char* s) {
        memset(raw, 0, sizeof(raw));
        size_t n = strlen(s);
        if (n > 22) n = 22;
        raw[0] = static_cast<uint8_t>(n << 1);
        memcpy(raw + 1, s, n);
    }
};

// Non-trivial struct returned via ARM64 x8 indirect result register (matches android::sp<T>)
struct SpRet {
    void* ptr;
    SpRet() : ptr(nullptr) {}
    SpRet(const SpRet& o) : ptr(o.ptr) {}
    ~SpRet() {}
};

// Non-trivial 48-byte buffer returned via ARM64 x8 indirect result register (matches Return<int32_t>)
struct HidlRetI32 {
    uint8_t raw[48];
    HidlRetI32() { memset(raw, 0, sizeof(raw)); }
    HidlRetI32(const HidlRetI32& o) { memcpy(raw, o.raw, sizeof(raw)); }
    ~HidlRetI32() {}
};

using TryGetServiceFn = SpRet (*)(const LibcxxSsoString* name, bool getStub);
using SetParamFn      = HidlRetI32 (*)(void* self, int32_t tag, int32_t val);
using ReturnDtorFn    = void (*)(void* retStatus);
using ParcelDataFn    = const uint8_t* (*)(const void* parcel);
using ParcelSizeFn    = size_t (*)(const void* parcel);
using OnTransactFn    = int32_t (*)(void* self, uint32_t code, const void* data, void* reply, uint32_t flags);
using WaitForSvcFn    = void* (*)(const char* instance);
using CheckSvcFn      = void* (*)(const char* instance);
using IsDeclaredFn    = bool (*)(const char* instance);

pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
void* g_provider = nullptr;
TryGetServiceFn g_tryGet = nullptr;
SetParamFn g_setParam = nullptr;
ReturnDtorFn g_retDtor = nullptr;
ParcelDataFn g_parcelData = nullptr;
ParcelSizeFn g_parcelSize = nullptr;
OnTransactFn g_realOnTransact = nullptr;
OnTransactFn g_realThunkOnTransact = nullptr;
WaitForSvcFn g_realWaitForService = nullptr;
CheckSvcFn g_realCheckService = nullptr;
IsDeclaredFn g_realIsDeclared = nullptr;

void* get_binder_ndk_sym(const char* sym) {
    static void* h = nullptr;
    if (!h) {
        h = dlopen("libbinder_ndk.so", RTLD_NOW | RTLD_NOLOAD);
        if (!h) h = dlopen("libbinder_ndk.so", RTLD_NOW);
    }
    return h ? dlsym(h, sym) : nullptr;
}

bool is_raw_qti_camera_provider(const char* instance) {
    return instance && strcmp(instance, "android.hardware.camera.provider.ICameraProvider/vendor_qti/0") == 0;
}

bool is_vivo_vcf_camera_provider(const char* instance) {
    return instance && strcmp(instance, "android.hardware.camera.provider.ICameraProvider/vendorVcf/0") == 0;
}

void init_syms_locked() {
    if (!g_parcelData) {
        g_parcelData = reinterpret_cast<ParcelDataFn>(dlsym(RTLD_DEFAULT, "_ZNK7android6Parcel4dataEv"));
        g_parcelSize = reinterpret_cast<ParcelSizeFn>(dlsym(RTLD_DEFAULT, "_ZNK7android6Parcel8dataSizeEv"));
    }
    if (!g_tryGet) {
        void* h = dlopen("vendor.vivo.hardware.camera.provider@1.0.so", RTLD_NOW | RTLD_GLOBAL);
        if (h) {
            g_tryGet = reinterpret_cast<TryGetServiceFn>(dlsym(
                h, "_ZN6vendor4vivo8hardware6camera8provider4V1_019IVivoCameraProvider13tryGetServiceERKNSt3__112basic_stringIcNS6_11char_traitsIcEENS6_9allocatorIcEEEEb"));
            g_setParam = reinterpret_cast<SetParamFn>(dlsym(
                h, "_ZN6vendor4vivo8hardware6camera8provider4V1_022BpHwVivoCameraProvider8setparamEii"));
        } else {
            ALOGW("dlopen vendor.vivo.hardware.camera.provider@1.0.so failed: %s", dlerror());
        }
    }
    if (!g_retDtor) {
        g_retDtor = reinterpret_cast<ReturnDtorFn>(dlsym(
            RTLD_DEFAULT, "_ZN7android8hardware7details13return_statusD2Ev"));
    }
}

bool call_setparam_locked(int32_t tag, int32_t val) {
    if (!g_provider || !g_setParam) return false;
    HidlRetI32 ret = g_setParam(g_provider, tag, val);
    // Status layout in libhidlbase: int32_t mException @ 0x00, int32_t mErrorCode @ 0x04
    int32_t exc = 0, err = 0;
    memcpy(&exc, ret.raw + 0, sizeof(exc));
    memcpy(&err, ret.raw + 4, sizeof(err));
    bool ok = (exc == 0 && err == 0);
    // Mark mCheckedStatus (offset 0x20 verified from libhidlbase.so) so ~return_status() never aborts
    ret.raw[0x20] = 1;
    if (g_retDtor) {
        g_retDtor(ret.raw);
    }
    return ok;
}

void acquire_provider_locked() {
    if (!g_tryGet) return;
    LibcxxSsoString defName("default");
    SpRet sp = g_tryGet(&defName, false);
    g_provider = sp.ptr;
    ALOGI("IVivoCameraProvider@1.0/default acquired: %p", g_provider);
}

bool parcel_contains_vivo_camera(const void* parcel) {
    if (!g_parcelData || !g_parcelSize || !parcel) return false;
    const uint8_t* buf = g_parcelData(parcel);
    size_t len = g_parcelSize(parcel);
    if (!buf || len < 18) return false;
    if (len > 4096) len = 4096;

    // 1. Check ASCII / UTF-8 "com.android.camera" (18 bytes)
    static const char kAscii[] = "com.android.camera";
    constexpr size_t kAsciiLen = 18;
    for (size_t i = 0; i + kAsciiLen <= len; ++i) {
        if (buf[i] == 'c' && memcmp(buf + i, kAscii, kAsciiLen) == 0) {
            // Ensure not a longer prefix like "com.android.camera2"
            if (i + kAsciiLen == len || buf[i + kAsciiLen] == 0) return true;
        }
    }
    // 2. Check UTF-16LE u"com.android.camera" (36 bytes)
    static const uint8_t kUtf16[36] = {
        'c',0,'o',0,'m',0,'.',0,'a',0,'n',0,'d',0,'r',0,'o',0,'i',0,'d',0,'.',0,
        'c',0,'a',0,'m',0,'e',0,'r',0,'a',0
    };
    for (size_t i = 0; i + 36 <= len; ++i) {
        if (buf[i] == 'c' && buf[i + 1] == 0 && memcmp(buf + i, kUtf16, 36) == 0) {
            if (i + 38 > len || (buf[i + 36] == 0 && buf[i + 37] == 0)) return true;
        }
    }
    return false;
}

void notify_vivo_hal_for_connect(uint32_t code, const void* parcel) {
    pthread_mutex_lock(&g_mu);
    init_syms_locked();
    if (!g_provider) {
        acquire_provider_locked();
    }
    bool isVivo = parcel_contains_vivo_camera(parcel);
    int32_t appType = isVivo ? 1 : 2;
    if (g_provider && g_setParam) {
        // Exact tag sequence verified from OriginOS 4 libcameraservice.so CameraService::connectHelper (0x147348..0x147418):
        bool ok = call_setparam_locked(1, appType);  // gAppType: 1=com.android.camera, 2=3rd-party (Aperture etc.)
        if (!ok) {
            ALOGW("setparam(1=%d) failed on stale provider %p; re-acquiring HAL...", appType, g_provider);
            g_provider = nullptr;
            acquire_provider_locked();
            if (g_provider) {
                ok = call_setparam_locked(1, appType);
            }
        }
        if (ok && g_provider) {
            call_setparam_locked(33, 2);        // mVCFAPPType: 2 (matches VivoUtils::findVCFAppType default for both!)
            call_setparam_locked(11, 0);        // mCameraLogProperty: 0
            call_setparam_locked(12, -1);       // mCameraFbLevel: -1
            call_setparam_locked(25, 0);        // mAlgoPost_for_third: 0
            ALOGI("connect tx=%u isVivo=%d -> setparam(1=%d, 33=2, 11=0, 12=-1, 25=0)", code, isVivo ? 1 : 0, appType);
        } else {
            ALOGW("connect tx=%u isVivo=%d setparam retry failed (provider=%p)", code, isVivo ? 1 : 0, g_provider);
        }
    } else {
        ALOGW("connect tx=%u isVivo=%d skipped (provider=%p setParam=%p)", code, isVivo ? 1 : 0, g_provider, (void*)g_setParam);
    }
    pthread_mutex_unlock(&g_mu);
}

}  // namespace

extern "C" {

// Interpose android::hardware::BnCameraService::onTransact(uint32_t, Parcel const&, Parcel*, uint32_t)
__attribute__((visibility("default")))
int32_t _ZN7android8hardware15BnCameraService10onTransactEjRKNS_6ParcelEPS2_j(
    void* self, uint32_t code, const void* data, void* reply, uint32_t flags) {
    if (!g_realOnTransact) {
        g_realOnTransact = reinterpret_cast<OnTransactFn>(
            dlsym(RTLD_NEXT, "_ZN7android8hardware15BnCameraService10onTransactEjRKNS_6ParcelEPS2_j"));
    }
    if (code == 3 || code == 4) {  // ICameraService::connect (3) or connectDevice (4)
        notify_vivo_hal_for_connect(code, data);
    }
    return g_realOnTransact(self, code, data, reply, flags);
}

// Interpose non-virtual thunk to android::hardware::BnCameraService::onTransact
__attribute__((visibility("default")))
int32_t _ZThn8_N7android8hardware15BnCameraService10onTransactEjRKNS_6ParcelEPS2_j(
    void* self, uint32_t code, const void* data, void* reply, uint32_t flags) {
    if (!g_realThunkOnTransact) {
        g_realThunkOnTransact = reinterpret_cast<OnTransactFn>(
            dlsym(RTLD_NEXT, "_ZThn8_N7android8hardware15BnCameraService10onTransactEjRKNS_6ParcelEPS2_j"));
    }
    if (code == 3 || code == 4) {
        notify_vivo_hal_for_connect(code, data);
    }
    return g_realThunkOnTransact(self, code, data, reply, flags);
}

// Block AOSP cameraserver from binding directly to raw Qualcomm vendor_qti/0 (19 raw devices)
// so it binds exclusively to Vivo's real VCF provider vendorVcf/0 (4 devices: 0..3 + all 1411
// vendor tags + SAT/HDR/Night/Portrait + automatic usePureBypass for 3rd-party apps like Aperture),
// matching OriginOS 4's debug.camera.vcf.provider=true routing in CameraProviderManager!
__attribute__((visibility("default")))
void* AServiceManager_waitForService(const char* instance) {
    if (is_raw_qti_camera_provider(instance)) {
        ALOGI("AServiceManager_waitForService: blocking raw %s -> forcing vendorVcf/0", instance);
        return nullptr;
    }
    if (!g_realWaitForService) {
        g_realWaitForService = reinterpret_cast<WaitForSvcFn>(get_binder_ndk_sym("AServiceManager_waitForService"));
    }
    void* ret = g_realWaitForService ? g_realWaitForService(instance) : nullptr;
    if (is_vivo_vcf_camera_provider(instance)) {
        ALOGI("AServiceManager_waitForService(%s) -> %p", instance, ret);
    }
    return ret;
}

__attribute__((visibility("default")))
void* AServiceManager_checkService(const char* instance) {
    if (is_raw_qti_camera_provider(instance)) {
        ALOGI("AServiceManager_checkService: blocking raw %s -> forcing vendorVcf/0", instance);
        return nullptr;
    }
    if (!g_realCheckService) {
        g_realCheckService = reinterpret_cast<CheckSvcFn>(get_binder_ndk_sym("AServiceManager_checkService"));
    }
    void* ret = g_realCheckService ? g_realCheckService(instance) : nullptr;
    if (is_vivo_vcf_camera_provider(instance)) {
        ALOGI("AServiceManager_checkService(%s) -> %p", instance, ret);
    }
    return ret;
}

__attribute__((visibility("default")))
bool AServiceManager_isDeclared(const char* instance) {
    if (is_raw_qti_camera_provider(instance)) {
        ALOGI("AServiceManager_isDeclared: blocking raw %s -> false", instance);
        return false;
    }
    if (!g_realIsDeclared) {
        g_realIsDeclared = reinterpret_cast<IsDeclaredFn>(get_binder_ndk_sym("AServiceManager_isDeclared"));
    }
    return g_realIsDeclared ? g_realIsDeclared(instance) : false;
}

}  // extern "C"
