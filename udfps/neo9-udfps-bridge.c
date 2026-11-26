/*
 * neo9-udfps-bridge — Stock-Native Optical UDFPS & VTS Screen-Off Bridge for vivo iQOO Neo9 (PD2338)
 * Hardware: SM8550, udfp_goodix_gf9578, Local HBM AMOLED (vivo_display.ko), VTS GT9916 THP (vivo_ts.ko)
 *
 * Mirrors OriginOS's exact kernel hardware protocol (decompiled from FingerprintUI.apk,
 * fp_dispatch_event.ko, vivo_display.ko, and vivo_ts.ko):
 *   1. `--patch-hal`: Deterministically patches `/vendor/bin/hw/vendor.vivo.hardware.biometrics.fingerprint-service`
 *      so IFingerprint::getSensorProps() reports PD2338's optical sensor at (630, 2533, r=110).
 *   2. Kernel-Driven Hardware VBLANK Synchronization (`NETLINK_KOBJECT_UEVENT`):
 *      - `vivo_display.ko` (`panel_local_hbm_notify_work`) waits for the exact AMOLED hardware VBLANK
 *        after `CMD_SET_LOCAL_HBM_ON` and broadcasts `FINGERSHOOTER_SHOW=1` over kobject uevent.
 *      - On receiving `FINGERSHOOTER_SHOW=1`, we immediately send `CMD_ON_SHOOTER_READY` (`32572 arg0=1`)
 *        to Vivo's fingerprint HAL (`IFingerprintExt`) with zero guessed delay, matching stock timing
 *        across 60/90/120/144Hz, enrollment, screen-on, AOD, and pure-black screen-off.
 *   3. Stock Kernel-Armed Pure-Black Screen-Off UDFPS (`/sys/fingerprint/hbm_notify_enable`):
 *      - On DISPLAY_OFF: arms `/sys/fingerprint/hbm_notify_enable=1`, `vts_finger_highlight=1`, `vts_lcd=0`.
 *      - On finger DOWN (`vivo_fp` EV_KEY 0xfe): `fp_dispatch_event.ko` automatically calls
 *        `vivo_display.ko`'s `panel_update_hbm_aod_ud_for_tp()` in-kernel — immediately lighting Local HBM
 *        if the panel is in DOZE/ON, or arming `local_hbm_pending = 10` if `DPMS_OFF` so `panel_doze()` /
 *        `__panel_displayon()` executes Local HBM atomically during panel power-on with zero race condition.
 *      - Bridge sends `CMD_NOTIFY_FINGER_STATE(DOWN=0)` and (if `DPMS_OFF`) wakes the panel into black
 *        `STATE_DOZE` via `com.android.systemui.doze.pulse`, then waits for `FINGERSHOOTER_SHOW=1` to
 *        trigger `CMD_ON_SHOOTER_READY(1)`.
 *      - On DISPLAY_ON: disarms `hbm_notify_enable=0`, `vts_finger_highlight=0`, `vts_lcd=1` so AOSP
 *        `UdfpsController` drives screen-on/enrollment touch directly with 100% accuracy.
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/netlink.h>
#include <linux/uinput.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ---- PD2338 Hardware Constants ---- */
#define NEO9_SENSOR_X              630
#define NEO9_SENSOR_Y              2533
#define NEO9_SENSOR_RADIUS         110

#define SYSFS_OLED_HBM             "/sys/lcm/oled_hbm"
#define SYSFS_HBM_NOTIFY_ENABLE    "/sys/fingerprint/hbm_notify_enable"
#define SYSFS_BL_LEVEL             "/sys/lcm/bl_level"
#define SYSFS_BL_FALLBACK          "/sys/class/backlight/panel0-backlight/brightness"
#define SYSFS_AOD_BL_LEVEL         "/sys/lcm/aod_bl_level"
#define SYSFS_LTPO_MODE            "/sys/lcm/ltpo_mode"
#define SYSFS_DRM_DPMS             "/sys/class/drm/card0-DSI-1/dpms"
#define OLED_LHBM_ON               10   /* 0x0a -> panel_set_ud_local_hbm ON  */
#define OLED_LHBM_OFF              11   /* 0x0b -> panel_set_ud_local_hbm OFF (also cancels pending LHBM) */

/* RM692H5 8T LTPO brightness tiers (from PD2338 RrpConfig.json + LcmMapNit.json) */
#define LTPO_BL_HIGH_ON            2009 /* >= 200 nit (smooth_brightness_max) */
#define LTPO_BL_HIGH_OFF           1960 /* hysteresis band */
#define LTPO_BL_MID_ON             1310 /* >= 90 nit */
#define LTPO_BL_MID_OFF            1260 /* hysteresis band */
#define LTPO_SYNC_INTERVAL_MS      1000

/* Zero-idle-overhead Settings refresh-rate lock (sync min_refresh_rate = peak_refresh_rate) */
#define SETTINGS_USER0_DIR         "/data/system/users/0"
#define SETTINGS_SYSTEM_XML        "/data/system/users/0/settings_system.xml"
#define PEAK_SIG_LEN               96
#define SETTINGS_DEBOUNCE_MS       250

#define SYSFS_VTS_APP_NAME         "/sys/touchscreen/app_name"
#define SYSFS_VTS_GESTURE          "/sys/devices/virtual/vts/vivo_ts/status/gesture"
#define SYSFS_VTS_LCD_ON           "/sys/devices/virtual/vts/vivo_ts/status/lcd_on"
#define VTS_GESTURE_FP_BIT         4096 /* 0x1000 */
#define KEY_VTS_FP_DETECT          0xfe /* 254: VTS_EVENT_GESTURE_FINGERPRINT_DETECT */

#define IFP_SERVICE_NAME           "android.hardware.biometrics.fingerprint.IFingerprint/default"
#define IFP_EXT_DESCRIPTOR         "vendor.vivo.hardware.biometrics.fingerprint.IFingerprintExt"
#define TX_SEND_CMD_BUNDLE         1

/* Stock OriginOS 4 VPerf HIDL 1.0 (vendor.vivo.hardware.vperf@1.0::IVPerf/default) */
#define VPERF_INTERFACE_NAME       "vendor.vivo.hardware.vperf@1.0::IVPerf"
#define VPERF_TX_LOCK_RELEASE      3u
#define VPERF_TX_HINT_ACQ_REL      21u
#define QTI_HINT_SCROLL_BOOST      0x00001080u /* perfboostsconfig.xml kalama 60/90/120/144 Hz */
#define QTI_SCROLL_TYPE_VERTICAL   1           /* Vertical Scroll boost (BIG/LITTLE/PRIME + LLCC/DDR + L3 + SCHEDBOOST) */
#define QTI_SCROLL_TYPE_LIGHT      5           /* Light Scroll boost (WALT adaptive high/low + SCHEDBOOST) */
#define VPERF_TOUCH_DOWN_MS        150
#define VPERF_TAP_OPEN_MS          400
#define VPERF_DRAG_BOOST_MS        600
#define VPERF_DRAG_RENEW_MS        300
#define VPERF_FLING_BOOST_MS       900
#define VPERF_MOVE_THRESH_PX       24

#define CMD_ON_INTERACTIVE_CHANGED 32517 /* 0x7f05 */
#define CMD_NOTIFY_FINGER_STATE    32534 /* 0x7f16: arg0=0 DOWN(evt=2), 1 UP(evt=4) */
#define CMD_ON_SHOOTER_READY       32572 /* 0x7f3c: arg0=1 Sensor::setSensorUIReady() */

#define FINGER_STATE_DOWN          0
#define FINGER_STATE_UP            1

/* Safety fallback timers (primary trigger is kernel UEvent FINGERSHOOTER_SHOW=1) */
#define UI_READY_FALLBACK_AOSP_MS  45
#define UI_READY_FALLBACK_DOZE_MS  120
#define DOZE_PULSE_FALLBACK_MS     240
#define DISPLAY_OFF_DEBOUNCE_DPMS  35
#define DISPLAY_OFF_DEBOUNCE_MS    150
#define SCREEN_OFF_COOLDOWN_MS     60
#define LHBM_WATCHDOG_MS           2500

/* ---- Dynamic libbinder_ndk.so ABI ---- */
typedef struct AIBinder AIBinder;
typedef struct AIBinder_Class AIBinder_Class;
typedef struct AParcel AParcel;
typedef int32_t binder_status_t;
typedef uint32_t transaction_code_t;
typedef uint32_t binder_flags_t;

typedef AIBinder* (*fn_AServiceManager_waitForService)(const char* instance);
typedef AIBinder* (*fn_AServiceManager_checkService)(const char* instance);
typedef binder_status_t (*fn_AIBinder_getExtension)(AIBinder* binder, AIBinder** outExt);
typedef AIBinder_Class* (*fn_AIBinder_Class_define)(const char* desc, void* onCreate, void* onDestroy, void* onTransact);
typedef bool (*fn_AIBinder_associateClass)(AIBinder* binder, const AIBinder_Class* clazz);
typedef binder_status_t (*fn_AIBinder_prepareTransaction)(AIBinder* binder, AParcel** in);
typedef binder_status_t (*fn_AIBinder_transact)(AIBinder* binder, transaction_code_t code, AParcel** in, AParcel** out, binder_flags_t flags);
typedef void (*fn_AIBinder_decStrong)(AIBinder* binder);
typedef bool (*fn_AIBinder_isAlive)(const AIBinder* binder);
typedef void (*fn_AParcel_delete)(AParcel* parcel);
typedef binder_status_t (*fn_AParcel_writeInt32)(AParcel* parcel, int32_t value);
typedef binder_status_t (*fn_AParcel_writeInt64)(AParcel* parcel, int64_t value);
typedef binder_status_t (*fn_AParcel_writeString)(AParcel* parcel, const char* string, int32_t length);
typedef binder_status_t (*fn_AParcel_readInt32)(const AParcel* parcel, int32_t* value);

typedef struct {
    void* lib;
    fn_AServiceManager_waitForService waitForService;
    fn_AServiceManager_checkService checkService;
    fn_AIBinder_getExtension getExtension;
    fn_AIBinder_Class_define classDefine;
    fn_AIBinder_associateClass associateClass;
    fn_AIBinder_prepareTransaction prepareTransaction;
    fn_AIBinder_transact transact;
    fn_AIBinder_decStrong decStrong;
    fn_AIBinder_isAlive isAlive;
    fn_AParcel_delete parcelDelete;
    fn_AParcel_writeInt32 writeInt32;
    fn_AParcel_writeInt64 writeInt64;
    fn_AParcel_writeString writeString;
    fn_AParcel_readInt32 readInt32;
    AIBinder_Class* extClass;
} BinderApi;

typedef int (*fn_android_log_vprint)(int prio, const char* tag, const char* fmt, va_list ap);
static fn_android_log_vprint g_log_vprint = NULL;
static FILE* g_log_file = NULL;
static volatile sig_atomic_t g_stop = 0;

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static void on_signal(int sig) { (void)sig; g_stop = 1; }

static void log_init(bool open_file) {
    void* lib = dlopen("liblog.so", RTLD_NOW);
    if (lib) g_log_vprint = (fn_android_log_vprint)dlsym(lib, "__android_log_vprint");
    if (open_file) {
        g_log_file = fopen("/metadata/gsi/dsu/neo9_udfps.log", "a");
        if (g_log_file) setvbuf(g_log_file, NULL, _IOLBF, 0);
    }
}

static void logi(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (g_log_vprint) {
        va_list ap2;
        va_copy(ap2, ap);
        g_log_vprint(4, "neo9-udfps", fmt, ap2);
        va_end(ap2);
    }
    if (g_log_file) {
        va_list ap3;
        va_copy(ap3, ap);
        fprintf(g_log_file, "[%lldms][bridge] ", (long long)now_ms());
        vfprintf(g_log_file, fmt, ap3);
        fprintf(g_log_file, "\n");
        fflush(g_log_file);
        va_end(ap3);
    }
    fprintf(stderr, "[neo9-udfps] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

static void* dummy_on_create(void* args) { return args; }
static void dummy_on_destroy(void* userData) { (void)userData; }
static binder_status_t dummy_on_transact(AIBinder* b, transaction_code_t c, const AParcel* in, AParcel* out) {
    (void)b; (void)c; (void)in; (void)out;
    return -38;
}

static bool binder_api_init(BinderApi* api) {
    memset(api, 0, sizeof(*api));
    api->lib = dlopen("libbinder_ndk.so", RTLD_NOW);
    if (!api->lib) { logi("dlopen(libbinder_ndk.so) failed: %s", dlerror()); return false; }
#define LOAD_SYM(field, name) do { \
    api->field = dlsym(api->lib, #name); \
    if (!api->field) { logi("missing symbol %s", #name); return false; } \
} while (0)
    LOAD_SYM(waitForService, AServiceManager_waitForService);
    api->checkService = (fn_AServiceManager_checkService)dlsym(api->lib, "AServiceManager_checkService");
    LOAD_SYM(getExtension, AIBinder_getExtension);
    LOAD_SYM(classDefine, AIBinder_Class_define);
    LOAD_SYM(associateClass, AIBinder_associateClass);
    LOAD_SYM(prepareTransaction, AIBinder_prepareTransaction);
    LOAD_SYM(transact, AIBinder_transact);
    LOAD_SYM(decStrong, AIBinder_decStrong);
    LOAD_SYM(isAlive, AIBinder_isAlive);
    LOAD_SYM(parcelDelete, AParcel_delete);
    LOAD_SYM(writeInt32, AParcel_writeInt32);
    LOAD_SYM(writeInt64, AParcel_writeInt64);
    LOAD_SYM(writeString, AParcel_writeString);
    LOAD_SYM(readInt32, AParcel_readInt32);
#undef LOAD_SYM
    api->extClass = api->classDefine(IFP_EXT_DESCRIPTOR, (void*)dummy_on_create, (void*)dummy_on_destroy, (void*)dummy_on_transact);
    return api->extClass != NULL;
}

static AIBinder* acquire_fp_ext(BinderApi* api) {
    AIBinder* fp = api->checkService ? api->checkService(IFP_SERVICE_NAME) : api->waitForService(IFP_SERVICE_NAME);
    if (!fp) return NULL;
    AIBinder* ext = NULL;
    binder_status_t st = api->getExtension(fp, &ext);
    api->decStrong(fp);
    if (st != 0 || !ext) return NULL;
    api->associateClass(ext, api->extClass);
    return ext;
}

static binder_status_t send_cmd(BinderApi* api, AIBinder* ext, int32_t cmd, int64_t arg0, int64_t arg1, const char* bundle, int32_t* out_ret) {
    if (!ext) return -32;
    AParcel* in = NULL;
    AParcel* out = NULL;
    binder_status_t st = api->prepareTransaction(ext, &in);
    if (st != 0) return st;
    st = api->writeInt32(in, cmd);
    if (st == 0) st = api->writeInt64(in, arg0);
    if (st == 0) st = api->writeInt64(in, arg1);
    if (st == 0) {
        const char* s = bundle ? bundle : "";
        st = api->writeString(in, s, (int32_t)strlen(s));
    }
    if (st != 0) { api->parcelDelete(in); return st; }
    st = api->transact(ext, TX_SEND_CMD_BUNDLE, &in, &out, 0x10000000);
    if (in) api->parcelDelete(in);
    if (st == 0 && out) {
        int32_t ex = 0, rv = 0;
        if (api->readInt32(out, &ex) == 0 && ex == 0 && api->readInt32(out, &rv) == 0 && out_ret) *out_ret = rv;
    }
    if (out) api->parcelDelete(out);
    return st;
}

/* ---- Stock OriginOS 4 IVPerf@1.0 HIDL Client (via /system/lib64/libhidlbase.so) ---- */
typedef struct {
    void*    ptr;
    uint64_t pad[2]; /* >16 bytes forces ARM64 indirect return via x8 (matches C++ sp<T>) */
} HidlSpRet;

typedef struct {
    uint64_t w0;
    uint64_t w1;
    uint64_t w2;
} LibcxxString; /* 24-byte Android arm64 libc++ std::string */

typedef struct {
    const char* mBuffer;
    uint32_t    mSize;
    uint32_t    mOwnsAndPad;
} HidlString;

typedef struct {
    const int32_t* mBuffer;
    uint32_t       mSize;
    uint32_t       mOwnsAndPad;
} HidlVecI32;

typedef struct {
    uint64_t opaque[6]; /* 48-byte empty libc++ std::function<void(Parcel&)> */
} HidlReplyCb;

typedef HidlSpRet (*fn_hidl_get_raw_svc_t)(const LibcxxString* desc, const LibcxxString* inst, bool retry, bool getStub);
typedef HidlSpRet (*fn_hidl_as_binder_t)(const void* iface);
typedef void      (*fn_hidl_parcel_ctor_t)(void* parcel);
typedef void      (*fn_hidl_parcel_dtor_t)(void* parcel);
typedef int32_t   (*fn_hidl_parcel_write_token_t)(void* parcel, const char* token);
typedef int32_t   (*fn_hidl_parcel_write_i32_t)(void* parcel, int32_t val);
typedef int32_t   (*fn_hidl_parcel_write_u32_t)(void* parcel, uint32_t val);
typedef int32_t   (*fn_hidl_parcel_write_buf_t)(void* parcel, const void* buf, size_t len, size_t* out_handle);
typedef int32_t   (*fn_hidl_parcel_write_emb_buf_t)(void* parcel, const void* buf, size_t len, size_t* out_handle, size_t parent_handle, size_t parent_offset);
typedef int32_t   (*fn_hidl_write_emb_str_t)(const HidlString* str, void* parcel, size_t parent_handle, size_t parent_offset);
typedef int32_t   (*fn_hidl_parcel_read_i32_t)(const void* parcel, int32_t* out_val);
typedef int32_t   (*fn_hidl_bphw_transact_t)(void* binder, uint32_t code, const void* data, void* reply, uint32_t flags, const HidlReplyCb* cb);

typedef struct {
    void*                          lib;
    fn_hidl_get_raw_svc_t          getRawService;
    fn_hidl_as_binder_t            asBinder;
    fn_hidl_parcel_ctor_t          parcelCtor;
    fn_hidl_parcel_dtor_t          parcelDtor;
    fn_hidl_parcel_write_token_t   writeToken;
    fn_hidl_parcel_write_i32_t     writeInt32;
    fn_hidl_parcel_write_u32_t     writeUint32;
    fn_hidl_parcel_write_buf_t     writeBuffer;
    fn_hidl_parcel_write_emb_buf_t writeEmbBuf;
    fn_hidl_write_emb_str_t        writeEmbStr;
    fn_hidl_parcel_read_i32_t      readInt32;
    fn_hidl_bphw_transact_t        transact;
    void*                          vperf_binder;
    int32_t                        active_handle;
    int64_t                        last_connect_try_ms;
    uint64_t                       boost_ok;
    uint64_t                       boost_fail;
    bool                           ready;
} VPerfClient;

static void init_libcxx_string(LibcxxString* out, const char* s) {
    memset(out, 0, sizeof(*out));
    size_t n = strlen(s);
    if (n <= 22) {
        uint8_t* b = (uint8_t*)out;
        b[0] = (uint8_t)(n << 1);
        memcpy(b + 1, s, n);
    } else {
        /* Android arm64 libc++ long string: LSB=1 in cap word, size in w1, ptr in w2 */
        out->w0 = ((n + 16u) & ~(uint64_t)15u) | 1ULL;
        out->w1 = (uint64_t)n;
        out->w2 = (uint64_t)(uintptr_t)s;
    }
}

static bool vperf_init(VPerfClient* vp) {
    memset(vp, 0, sizeof(*vp));
    vp->lib = dlopen("libhidlbase.so", RTLD_NOW);
    if (!vp->lib) {
        logi("VPERF: dlopen(libhidlbase.so) failed: %s", dlerror());
        return false;
    }
#define LOAD_VP(field, sym) do { \
    vp->field = dlsym(vp->lib, sym); \
    if (!vp->field) { logi("VPERF: missing %s", sym); return false; } \
} while (0)
    LOAD_VP(getRawService, "_ZN7android8hardware7details21getRawServiceInternalERKNSt3__112basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEESA_bb");
    LOAD_VP(asBinder,      "_ZN7android8hardware10IInterface8asBinderEPKS1_");
    LOAD_VP(parcelCtor,    "_ZN7android8hardware6ParcelC1Ev");
    LOAD_VP(parcelDtor,    "_ZN7android8hardware6ParcelD1Ev");
    LOAD_VP(writeToken,    "_ZN7android8hardware6Parcel19writeInterfaceTokenEPKc");
    LOAD_VP(writeInt32,    "_ZN7android8hardware6Parcel10writeInt32Ei");
    LOAD_VP(writeUint32,   "_ZN7android8hardware6Parcel11writeUint32Ej");
    LOAD_VP(writeBuffer,   "_ZN7android8hardware6Parcel11writeBufferEPKvmPm");
    LOAD_VP(writeEmbBuf,   "_ZN7android8hardware6Parcel19writeEmbeddedBufferEPKvmPmmm");
    LOAD_VP(writeEmbStr,   "_ZN7android8hardware21writeEmbeddedToParcelERKNS0_11hidl_stringEPNS0_6ParcelEmm");
    LOAD_VP(readInt32,     "_ZNK7android8hardware6Parcel9readInt32EPi");
    LOAD_VP(transact,      "_ZN7android8hardware10BpHwBinder8transactEjRKNS0_6ParcelEPS2_jNSt3__18functionIFvRS2_EEE");
#undef LOAD_VP
    vp->ready = true;
    return true;
}

static bool vperf_connect(VPerfClient* vp, int64_t now) {
    if (!vp->ready) return false;
    if (vp->vperf_binder) return true;
    if (now > 0 && vp->last_connect_try_ms > 0 && (now - vp->last_connect_try_ms) < 5000) return false;
    vp->last_connect_try_ms = now;

    LibcxxString desc, inst;
    init_libcxx_string(&desc, VPERF_INTERFACE_NAME);
    init_libcxx_string(&inst, "default");
    HidlSpRet ibase = vp->getRawService(&desc, &inst, false, false);
    if (!ibase.ptr) return false;
    /* BpHwBase's IInterface subobject sits at +8 (_ZTC...BpHwBaseE8_...IInterfaceE) */
    HidlSpRet binder = vp->asBinder((const uint8_t*)ibase.ptr + 8u);
    if (!binder.ptr) return false;
    vp->vperf_binder = binder.ptr;
    logi("VPERF: connected %s/default binder=%p", VPERF_INTERFACE_NAME, vp->vperf_binder);
    return true;
}

static int32_t vperf_hint_acq_rel(VPerfClient* vp, int32_t prev_handle, uint32_t hint,
                                  int32_t duration_ms, int32_t type, int64_t now) {
    if (!vperf_connect(vp, now)) return -1;

    uint64_t data_buf[64]  __attribute__((aligned(16)));
    uint64_t reply_buf[64] __attribute__((aligned(16)));
    memset(data_buf, 0, sizeof(data_buf));
    memset(reply_buf, 0, sizeof(reply_buf));
    vp->parcelCtor(data_buf);
    vp->parcelCtor(reply_buf);

    int32_t ret_handle = -1;
    size_t str_parent = 0, vec_parent = 0, vec_child = 0;
    HidlString empty_str = { "", 0u, 0u };
    int32_t reserved_vals[2] = { 0, 0 };
    HidlVecI32 reserved_vec = { reserved_vals, 0u, 0u };

    int32_t st = vp->writeToken(data_buf, VPERF_INTERFACE_NAME);
    if (st == 0) st = vp->writeInt32(data_buf, prev_handle);
    if (st == 0) st = vp->writeUint32(data_buf, hint);
    if (st == 0) st = vp->writeBuffer(data_buf, &empty_str, sizeof(empty_str), &str_parent);
    if (st == 0) st = vp->writeEmbStr(&empty_str, data_buf, str_parent, 0);
    if (st == 0) st = vp->writeInt32(data_buf, duration_ms);
    if (st == 0) st = vp->writeInt32(data_buf, type);
    if (st == 0) st = vp->writeBuffer(data_buf, &reserved_vec, sizeof(reserved_vec), &vec_parent);
    if (st == 0) st = vp->writeEmbBuf(data_buf, reserved_vals, 0, &vec_child, vec_parent, 0);

    if (st == 0) {
        HidlReplyCb cb;
        memset(&cb, 0, sizeof(cb));
        st = vp->transact(vp->vperf_binder, VPERF_TX_HINT_ACQ_REL, data_buf, reply_buf, 0, &cb);
        if (st == 0) {
            int32_t exc = -1, h = -1;
            if (vp->readInt32(reply_buf, &exc) == 0 && exc == 0 &&
                vp->readInt32(reply_buf, &h) == 0 && h > 0) {
                ret_handle = h;
                vp->active_handle = h;
                vp->boost_ok++;
            } else {
                vp->boost_fail++;
            }
        } else {
            vp->boost_fail++;
            /* Reset stale binder on dead object (-32 / EPIPE) */
            if (st == -32) {
                vp->vperf_binder = NULL;
                vp->active_handle = 0;
            }
        }
    }

    vp->parcelDtor(reply_buf);
    vp->parcelDtor(data_buf);
    return ret_handle;
}

static void vperf_release_active(VPerfClient* vp) {
    if (!vp->ready || !vp->vperf_binder || vp->active_handle <= 0) return;

    uint64_t data_buf[64]  __attribute__((aligned(16)));
    uint64_t reply_buf[64] __attribute__((aligned(16)));
    memset(data_buf, 0, sizeof(data_buf));
    memset(reply_buf, 0, sizeof(reply_buf));
    vp->parcelCtor(data_buf);
    vp->parcelCtor(reply_buf);

    size_t vec_parent = 0, vec_child = 0;
    int32_t reserved_vals[2] = { 0, 0 };
    HidlVecI32 reserved_vec = { reserved_vals, 0u, 0u };

    int32_t st = vp->writeToken(data_buf, VPERF_INTERFACE_NAME);
    if (st == 0) st = vp->writeInt32(data_buf, vp->active_handle);
    if (st == 0) st = vp->writeBuffer(data_buf, &reserved_vec, sizeof(reserved_vec), &vec_parent);
    if (st == 0) st = vp->writeEmbBuf(data_buf, reserved_vals, 0, &vec_child, vec_parent, 0);
    if (st == 0) {
        HidlReplyCb cb;
        memset(&cb, 0, sizeof(cb));
        (void)vp->transact(vp->vperf_binder, VPERF_TX_LOCK_RELEASE, data_buf, reply_buf, 0, &cb);
    }
    vp->active_handle = 0;
    vp->parcelDtor(reply_buf);
    vp->parcelDtor(data_buf);
}

/* ---- Sysfs, Netlink UEvent & Input Helpers ---- */
static int read_sysfs_int(const char* path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    char buf[32];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';
    return (int)strtol(buf, NULL, 0);
}

static int read_aod_current_bl(void) {
    int fd = open(SYSFS_AOD_BL_LEVEL, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    char buf[64];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = '\0';
    const char* p = strstr(buf, "current=");
    return p ? (int)strtol(p + 8, NULL, 10) : 0;
}

static int read_display_bl(void) {
    int bl = read_sysfs_int(SYSFS_BL_LEVEL);
    if (bl < 0) bl = read_sysfs_int(SYSFS_BL_FALLBACK);
    return bl;
}

static bool is_panel_dpms_on(void) {
    int fd = open(SYSFS_DRM_DPMS, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return (read_display_bl() > 0 || read_aod_current_bl() > 0);
    char buf[16];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return false;
    buf[n] = '\0';
    return strncmp(buf, "On", 2) == 0;
}

static bool write_sysfs_str(const char* path, const char* s) {
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return false;
    size_t len = strlen(s);
    ssize_t wr = write(fd, s, len);
    close(fd);
    return wr == (ssize_t)len;
}

static bool write_sysfs_int(const char* path, int val) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", val);
    return write_sysfs_str(path, buf);
}

static bool set_hbm_notify_enable(bool on) {
    return write_sysfs_str(SYSFS_HBM_NOTIFY_ENABLE, on ? "1" : "0");
}

static void set_vts_lcd_state(bool on) {
    write_sysfs_str(SYSFS_VTS_APP_NAME, on ? "vts_lcd:vivo_ts:1" : "vts_lcd:vivo_ts:0");
}

static void enable_vts_screen_off_fp(bool verbose) {
    int cur_gest = read_sysfs_int(SYSFS_VTS_GESTURE);
    int want_gest = (cur_gest >= 0) ? (cur_gest | VTS_GESTURE_FP_BIT) : VTS_GESTURE_FP_BIT;
    char gs_cmd[64];
    snprintf(gs_cmd, sizeof(gs_cmd), "vts_gs:vivo_ts:%d", want_gest);

    write_sysfs_str(SYSFS_VTS_APP_NAME, "vts_proximity:vivo_ts:1");
    write_sysfs_str(SYSFS_VTS_APP_NAME, "vts_finger_mode_support:vivo_ts:1");
    write_sysfs_str(SYSFS_VTS_APP_NAME, "vts_finger_unlock:vivo_ts:1");
    write_sysfs_str(SYSFS_VTS_APP_NAME, gs_cmd);
    write_sysfs_str(SYSFS_VTS_APP_NAME, "vts_finger_mode:vivo_ts:1");
    write_sysfs_str(SYSFS_VTS_APP_NAME, "vts_finger_highlight:vivo_ts:1");
    if (verbose) {
        logi("VTS FP armed: gs=%d lcd_on=%d", read_sysfs_int(SYSFS_VTS_GESTURE), read_sysfs_int(SYSFS_VTS_LCD_ON));
    }
}

static void disable_vts_screen_off_fp(void) {
    int cur_gest = read_sysfs_int(SYSFS_VTS_GESTURE);
    int want_gest = (cur_gest >= 0) ? (cur_gest & ~VTS_GESTURE_FP_BIT) : 0;
    char gs_cmd[64];
    snprintf(gs_cmd, sizeof(gs_cmd), "vts_gs:vivo_ts:%d", want_gest);

    write_sysfs_str(SYSFS_VTS_APP_NAME, "vts_finger_highlight:vivo_ts:0");
    write_sysfs_str(SYSFS_VTS_APP_NAME, "vts_finger_mode:vivo_ts:0");
    write_sysfs_str(SYSFS_VTS_APP_NAME, "vts_finger_unlock:vivo_ts:0");
    write_sysfs_str(SYSFS_VTS_APP_NAME, gs_cmd);
}

static int init_uevent_socket(void) {
    int fd = socket(PF_NETLINK, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
    if (fd < 0) return -1;
    int sz = 64 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &sz, sizeof(sz));

    struct sockaddr_nl sa;
    memset(&sa, 0, sizeof(sa));
    sa.nl_family = AF_NETLINK;
    sa.nl_pid = (uint32_t)getpid();
    sa.nl_groups = 1; /* kernel uevent multicast group */
    if (bind(fd, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
        sa.nl_pid = 0;
        if (bind(fd, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
            close(fd);
            return -1;
        }
    }
    return fd;
}

static void send_doze_pulse_async(void) {
    pid_t pid = fork();
    if (pid == 0) {
        int dn = open("/dev/null", O_RDWR);
        if (dn >= 0) {
            dup2(dn, STDIN_FILENO);
            dup2(dn, STDOUT_FILENO);
            dup2(dn, STDERR_FILENO);
            if (dn > 2) close(dn);
        }
        char* const argv[] = {
            "/system/bin/cmd", "activity", "broadcast",
            "--user", "0", "-a", "com.android.systemui.doze.pulse", NULL
        };
        execv(argv[0], argv);
        _exit(127);
    }
}

static int open_input_by_name(const char* target_name, char* out_path, size_t out_len) {
    for (int i = 0; i < 32; i++) {
        char name_path[64];
        snprintf(name_path, sizeof(name_path), "/sys/class/input/event%d/device/name", i);
        int nfd = open(name_path, O_RDONLY | O_CLOEXEC);
        if (nfd < 0) continue;
        char name[64];
        ssize_t n = read(nfd, name, sizeof(name) - 1);
        close(nfd);
        if (n <= 0) continue;
        name[n] = '\0';
        while (n > 0 && (name[n - 1] == '\n' || name[n - 1] == '\r')) name[--n] = '\0';
        if (strcmp(name, target_name) == 0) {
            char dev_path[64];
            snprintf(dev_path, sizeof(dev_path), "/dev/input/event%d", i);
            int fd = open(dev_path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd >= 0) {
                if (out_path && out_len > 0) snprintf(out_path, out_len, "%s", dev_path);
                return fd;
            }
        }
    }
    return -1;
}

static int init_uinput_wakeup(void) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;
    if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0 || ioctl(fd, UI_SET_KEYBIT, KEY_WAKEUP) < 0) {
        close(fd);
        return -1;
    }
    struct uinput_setup us;
    memset(&us, 0, sizeof(us));
    us.id.bustype = BUS_VIRTUAL;
    us.id.vendor = 0x2d95;
    us.id.product = 0x2338;
    us.id.version = 1;
    snprintf(us.name, sizeof(us.name), "neo9-udfps-wakeup");
    if (ioctl(fd, UI_DEV_SETUP, &us) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static bool send_wakeup_key(int uinput_fd) {
    if (uinput_fd < 0) return false;
    struct input_event ev[4];
    memset(ev, 0, sizeof(ev));
    ev[0].type = EV_KEY; ev[0].code = KEY_WAKEUP; ev[0].value = 1;
    ev[1].type = EV_SYN; ev[1].code = SYN_REPORT; ev[1].value = 0;
    ev[2].type = EV_KEY; ev[2].code = KEY_WAKEUP; ev[2].value = 0;
    ev[3].type = EV_SYN; ev[3].code = SYN_REPORT; ev[3].value = 0;
    return write(uinput_fd, ev, sizeof(ev)) == (ssize_t)sizeof(ev);
}

/* ---- Deterministic HAL SensorLocation Patcher ---- */
static int patch_vendor_hal(const char* src_path, const char* dst_path) {
    FILE* f = fopen(src_path, "rb");
    if (!f) { fprintf(stderr, "[neo9-udfps] open %s failed: %s\n", src_path, strerror(errno)); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0x10000 || sz > 4 * 1024 * 1024) { fclose(f); return 2; }
    uint8_t* buf = (uint8_t*)malloc((size_t)sz);
    if (!buf) { fclose(f); return 3; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return 4; }
    fclose(f);

    Elf64_Ehdr* eh = (Elf64_Ehdr*)buf;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 || eh->e_machine != EM_AARCH64) { free(buf); return 5; }
    Elf64_Phdr* ph = (Elf64_Phdr*)(buf + eh->e_phoff);
    Elf64_Phdr* load0 = NULL;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_LOAD && ph[i].p_offset == 0 && ph[i].p_vaddr == 0) { load0 = &ph[i]; break; }
    }
    if (!load0 || load0->p_filesz > 0x9fc0 || load0->p_filesz < 0x7000) { free(buf); return 6; }

    uint32_t orig_insn = 0;
    memcpy(&orig_insn, buf + 0xad5c, 4);
    if (orig_insn != 0x3dc027e0U) { free(buf); return 7; }
    for (int i = 0; i < 16; i++) { if (buf[0x9fc0 + i] != 0) { free(buf); return 8; } }

    int32_t loc[4] = { 0, NEO9_SENSOR_X, NEO9_SENSOR_Y, NEO9_SENSOR_RADIUS };
    memcpy(buf + 0x9fc0, loc, sizeof(loc));
    load0->p_filesz = 0xa000;
    load0->p_memsz = 0xa000;
    uint32_t new_insn = 0x9cff9320U; /* ldr q0, 0x9fc0 (pc - 0xd9c) */
    memcpy(buf + 0xad5c, &new_insn, 4);

    FILE* out = fopen(dst_path, "wb");
    if (!out) { free(buf); return 9; }
    if (fwrite(buf, 1, (size_t)sz, out) != (size_t)sz) { fclose(out); free(buf); return 10; }
    fclose(out);
    free(buf);
    chmod(dst_path, 0755);
    fprintf(stderr, "[neo9-udfps] patched HAL SensorLocation -> (%d, %d, r=%d) at %s\n",
            NEO9_SENSOR_X, NEO9_SENSOR_Y, NEO9_SENSOR_RADIUS, dst_path);
    return 0;
}

/* ---- Deterministic Vendor Composer HAL Patcher (5 Unique RM692H5 HWC Configs + Groups) ---- */
static int patch_vendor_composer(const char* src_path, const char* dst_path) {
    FILE* f = fopen(src_path, "rb");
    if (!f) { fprintf(stderr, "[neo9-composer] open %s failed: %s\n", src_path, strerror(errno)); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0x89000 || sz > 4 * 1024 * 1024) { fclose(f); return 2; }
    uint8_t* buf = (uint8_t*)malloc((size_t)sz);
    if (!buf) { fclose(f); return 3; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return 4; }
    fclose(f);

    Elf64_Ehdr* eh = (Elf64_Ehdr*)buf;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 || eh->e_machine != EM_AARCH64) { free(buf); return 5; }

    /* Verify exact PD2338 composer instructions before patching:
     *   0x7ee08: 3952c2a8  ldrb w8, [x21, #0x4b0]
     *   0x7ee0c: 35000268  cbnz w8, 0x7ee58         -> b 0x7ee58      (0x14000013)
     *   0x7f438: 3952c009  ldrb w9, [x0, #0x4b0]
     *   0x7f468: 340007a9  cbz  w9, 0x7f55c         -> nop            (0xd503201f)
     *   0x883a0: 7100005f  cmp  w2, #0              -> cmp x21, #0x1  (0xf10006bf)
     *   0x883a4: 1a9f17e5  cset w5, eq
     */
    uint32_t insn_7ee08 = 0, insn_7ee0c = 0, insn_7f438 = 0, insn_7f468 = 0;
    uint32_t insn_883a0 = 0, insn_883a4 = 0;
    memcpy(&insn_7ee08, buf + 0x7ee08, 4);
    memcpy(&insn_7ee0c, buf + 0x7ee0c, 4);
    memcpy(&insn_7f438, buf + 0x7f438, 4);
    memcpy(&insn_7f468, buf + 0x7f468, 4);
    memcpy(&insn_883a0, buf + 0x883a0, 4);
    memcpy(&insn_883a4, buf + 0x883a4, 4);
    if (insn_7ee08 != 0x3952c2a8U || insn_7ee0c != 0x35000268U ||
        insn_7f438 != 0x3952c009U || insn_7f468 != 0x340007a9U ||
        insn_883a0 != 0x7100005fU || insn_883a4 != 0x1a9f17e5U) {
        fprintf(stderr, "[neo9-composer] instruction mismatch: %08x %08x %08x %08x %08x %08x\n",
                insn_7ee08, insn_7ee0c, insn_7f438, insn_7f468, insn_883a0, insn_883a4);
        free(buf);
        return 6;
    }

    uint32_t patch_7ee0c = 0x14000013U; /* b 0x7ee58 */
    uint32_t patch_7f468 = 0xd503201fU; /* nop */
    uint32_t patch_883a0 = 0xf10006bfU; /* cmp x21, #0x1 (keep LTPO configs + 144Hz config 1) */
    memcpy(buf + 0x7ee0c, &patch_7ee0c, 4);
    memcpy(buf + 0x7f468, &patch_7f468, 4);
    memcpy(buf + 0x883a0, &patch_883a0, 4);

    FILE* out = fopen(dst_path, "wb");
    if (!out) { free(buf); return 7; }
    if (fwrite(buf, 1, (size_t)sz, out) != (size_t)sz) { fclose(out); free(buf); return 8; }
    fclose(out);
    free(buf);
    chmod(dst_path, 0755);
    fprintf(stderr, "[neo9-composer] patched PD2338 composer HAL (5 unique configs: 60/72/90/120 LTPO + 144Hz) at %s\n", dst_path);
    return 0;
}

/* ---- UDFPS & RM692H5 8T LTPO State Machine ---- */
typedef struct {
    bool finger_down;
    bool lhbm_on;
    bool ui_ready_sent;
    bool session_active;
    bool screen_off_armed;
    bool wake_pending;
    bool wake_fallback_sent;
    bool down_from_vts;
    bool hbm_notify_armed;
    int display_on;
    int ltpo_tier;
    int ltpo_last_auto_mode;
    int64_t bl_zero_since_ms;
    int64_t display_off_time_ms;
    int64_t fast_poll_until_ms;
    int64_t down_time_ms;
    int64_t wake_press_ms;
    int64_t last_ltpo_sync_ms;
    int64_t settings_sync_due_ms;
    int64_t last_refresh_retry_ms;
    uint8_t last_peak_sig[PEAK_SIG_LEN];
    bool has_peak_sig;
    bool initial_refresh_synced;
    int saved_bl;
    /* Touch/Scroll VPerf Boost state (screen-on only, auto-expiring via libqti-perfd) */
    bool touch_down;
    bool touch_moved;
    bool touch_have_start;
    int touch_start_x;
    int touch_start_y;
    int touch_cur_x;
    int touch_cur_y;
    int64_t last_scroll_boost_ms;
    VPerfClient vperf;
} UdfpsState;

static int init_settings_inotify(void) {
    int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd < 0) return -1;
    if (inotify_add_watch(fd, SETTINGS_USER0_DIR, IN_CLOSE_WRITE | IN_MOVED_TO) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static bool read_peak_abx_sig(uint8_t out[PEAK_SIG_LEN]) {
    int fd = open(SETTINGS_SYSTEM_XML, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    const size_t max_sz = 512 * 1024;
    uint8_t* buf = (uint8_t*)malloc(max_sz);
    if (!buf) { close(fd); return false; }
    size_t total = 0;
    while (total < max_sz) {
        ssize_t n = read(fd, buf + total, max_sz - total);
        if (n <= 0) break;
        total += (size_t)n;
    }
    close(fd);

    const uint8_t* hit = (const uint8_t*)memmem(buf, total, "peak_refresh_rate", 17);
    if (!hit) {
        free(buf);
        return false;
    }
    size_t rem = (size_t)((buf + total) - hit);
    size_t copy_len = (rem < PEAK_SIG_LEN) ? rem : PEAK_SIG_LEN;
    memset(out, 0, PEAK_SIG_LEN);
    memcpy(out, hit, copy_len);
    free(buf);
    return true;
}

static bool read_setting_float_ns(const char* ns, const char* key, float* out_val) {
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "/system/bin/settings --user current get %s %s 2>/dev/null", ns, key);
    FILE* fp = popen(cmd, "r");
    if (!fp) return false;
    char buf[64] = {0};
    bool got = fgets(buf, sizeof(buf), fp) != NULL;
    pclose(fp);
    if (!got || strstr(buf, "null") != NULL) return false;
    char* endp = NULL;
    float v = strtof(buf, &endp);
    if (endp == buf) return false;
    *out_val = v;
    return true;
}

static bool read_setting_float(const char* key, float* out_val) {
    return read_setting_float_ns("system", key, out_val);
}

static bool write_setting_str_ns(const char* ns, const char* key, const char* val) {
    int pfd[2];
    if (pipe(pfd) < 0) return false;
    fcntl(pfd[0], F_SETFD, FD_CLOEXEC);

    pid_t pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        return false;
    }
    if (pid == 0) {
        close(pfd[0]);
        /* Keep pfd[1] open across exec so parent's read() blocks until settings exits */
        int dn = open("/dev/null", O_RDWR);
        if (dn >= 0) {
            dup2(dn, STDIN_FILENO);
            dup2(dn, STDOUT_FILENO);
            dup2(dn, STDERR_FILENO);
            if (dn > 2) close(dn);
        }
        execl("/system/bin/settings", "settings", "--user", "current",
              "put", ns, key, val, (char*)NULL);
        _exit(127);
    }
    close(pfd[1]);
    char dummy;
    while (read(pfd[0], &dummy, 1) < 0 && errno == EINTR) {}
    close(pfd[0]);
    return true;
}

static bool write_setting_str(const char* key, const char* val) {
    return write_setting_str_ns("system", key, val);
}

static bool write_secure_setting_str(const char* key, const char* val) {
    return write_setting_str_ns("secure", key, val);
}

static void sync_refresh_rate_lock(UdfpsState* st, bool force) {
    uint8_t sig[PEAK_SIG_LEN];
    bool sig_ok = read_peak_abx_sig(sig);
    if (!force && st->initial_refresh_synced && sig_ok && st->has_peak_sig &&
        memcmp(sig, st->last_peak_sig, PEAK_SIG_LEN) == 0) {
        return;
    }
    if (sig_ok) {
        memcpy(st->last_peak_sig, sig, PEAK_SIG_LEN);
        st->has_peak_sig = true;
    }

    float ext_v = 0.0f;
    if (read_setting_float("extreme_refresh_rate", &ext_v) && ext_v > 0.5f) {
        write_setting_str("extreme_refresh_rate", "0");
        logi("REFRESH_SYNC: cleared stale extreme_refresh_rate=1 -> 0");
    }

    float peak = 0.0f;
    if (!read_setting_float("peak_refresh_rate", &peak) || peak < 55.0f || peak > 150.0f) {
        if (!st->initial_refresh_synced) {
            float sb = 0.0f;
            if (!read_setting_float("screen_brightness", &sb)) {
                return; /* SettingsProvider not ready yet; retry cleanly */
            }
            write_setting_str("peak_refresh_rate", "120.0");
            write_setting_str("min_refresh_rate", "120.0");
            float mcfr = 0.0f;
            if (!read_setting_float_ns("secure", "match_content_frame_rate", &mcfr)) {
                write_secure_setting_str("match_content_frame_rate", "2");
            }
            st->initial_refresh_synced = true;
            logi("REFRESH_SYNC: init default peak=120.0 min=120.0");
        }
        return;
    }
    float min_hz = 0.0f;
    bool min_ok = read_setting_float("min_refresh_rate", &min_hz);
    float diff = min_hz - peak;
    bool wrote_min = false;
    if (!min_ok || diff > 0.5f || diff < -0.5f) {
        char val[16];
        snprintf(val, sizeof(val), "%.1f", peak);
        wrote_min = write_setting_str("min_refresh_rate", val);
        logi("REFRESH_SYNC: peak=%.1f -> min=%s (prev_min=%.1f)",
             peak, val, min_ok ? min_hz : -1.0f);
    }
    if (!st->initial_refresh_synced) {
        float mcfr = 0.0f;
        if (!read_setting_float_ns("secure", "match_content_frame_rate", &mcfr)) {
            write_secure_setting_str("match_content_frame_rate", "2");
        }
        st->initial_refresh_synced = true;
        if (!wrote_min) {
            logi("REFRESH_SYNC: boot synced peak=%.1f min=%.1f", peak, min_ok ? min_hz : peak);
        }
    }
}

static void arm_screen_off_fp(UdfpsState* st, const char* reason) {
    if (st->screen_off_armed) return;
    st->screen_off_armed = true;
    st->hbm_notify_armed = set_hbm_notify_enable(true);
    enable_vts_screen_off_fp(false);
    logi("ARM_SCREEN_OFF_FP (%s): hbm_notify=1(ok=%d) gs=%d",
         reason, st->hbm_notify_armed ? 1 : 0, read_sysfs_int(SYSFS_VTS_GESTURE));
}

static void disarm_screen_off_fp(UdfpsState* st, const char* reason) {
    if (!st->screen_off_armed && !st->hbm_notify_armed) return;
    st->screen_off_armed = false;
    set_hbm_notify_enable(false);
    st->hbm_notify_armed = false;
    disable_vts_screen_off_fp();
    logi("DISARM_SCREEN_OFF_FP (%s): hbm_notify=0 gs=%d",
         reason, read_sysfs_int(SYSFS_VTS_GESTURE));
}

static void ensure_ext(BinderApi* api, AIBinder** ext_ptr) {
    if (!*ext_ptr || !api->isAlive(*ext_ptr)) {
        if (*ext_ptr) api->decStrong(*ext_ptr);
        *ext_ptr = acquire_fp_ext(api);
    }
}

static bool turn_off_lhbm(UdfpsState* st) {
    if (!st->lhbm_on) return true;
    st->lhbm_on = false;
    int cur_bl = read_display_bl();
    int restore_bl = (cur_bl > 0) ? cur_bl : st->saved_bl;
    int off_val = (restore_bl > 0 && restore_bl <= 0xffff)
        ? ((restore_bl << 8) | OLED_LHBM_OFF)
        : OLED_LHBM_OFF;
    return write_sysfs_int(SYSFS_OLED_HBM, off_val);
}

static void send_ui_ready(BinderApi* api, AIBinder* ext, UdfpsState* st, const char* src) {
    if (!st->finger_down || st->ui_ready_sent) return;
    st->ui_ready_sent = true;
    int64_t elapsed = now_ms() - st->down_time_ms;
    int32_t ret = -1;
    binder_status_t bs = send_cmd(api, ext, CMD_ON_SHOOTER_READY, 1, 0, "", &ret);
    logi("UI_READY (%s, +%lldms): cmd32572(1) st=%d ret=%d", src, (long long)elapsed, bs, ret);
}

/* Screen-on / enrollment / AOD AOSP-driven DOWN: explicitly enables oled_hbm=10, then awaits FINGERSHOOTER_SHOW=1 */
static void handle_aosp_finger_down(BinderApi* api, AIBinder* ext, UdfpsState* st) {
    if (st->finger_down) return;
    int bl = read_display_bl();
    st->saved_bl = (bl > 0) ? bl : 0;
    st->finger_down = true;
    st->ui_ready_sent = false;
    st->down_from_vts = false;
    st->wake_pending = false;
    st->down_time_ms = now_ms();

    bool hbm_ok = write_sysfs_int(SYSFS_OLED_HBM, OLED_LHBM_ON);
    st->lhbm_on = hbm_ok;

    int32_t ret = -1;
    binder_status_t bs = send_cmd(api, ext, CMD_NOTIFY_FINGER_STATE, FINGER_STATE_DOWN, 0, "", &ret);
    logi("DOWN (aosp): lhbm=10(ok=%d) bl=%d cmd32534(0) st=%d ret=%d",
         hbm_ok ? 1 : 0, st->saved_bl, bs, ret);
}

static void handle_finger_up(BinderApi* api, AIBinder* ext, UdfpsState* st, const char* reason) {
    if (!st->finger_down) return;
    st->finger_down = false;
    st->ui_ready_sent = false;
    st->down_from_vts = false;
    st->wake_pending = false;
    st->wake_fallback_sent = false;

    /* Writing OLED_LHBM_OFF(11) both turns off active Local HBM and cancels pending [panel+0x4b4] */
    bool hbm_ok = turn_off_lhbm(st);
    int32_t ret = -1;
    binder_status_t bs = send_cmd(api, ext, CMD_NOTIFY_FINGER_STATE, FINGER_STATE_UP, 0, "", &ret);
    logi("UP (%s): lhbm_off(ok=%d) cmd32534(1) st=%d ret=%d", reason, hbm_ok ? 1 : 0, bs, ret);
}

static bool read_ltpo_status(int* out_auto_mode, int* out_min_fps) {
    int fd = open(SYSFS_LTPO_MODE, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    char buf[192];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return false;
    buf[n] = '\0';
    const char* pa = strstr(buf, "auto_mode=");
    const char* pm = strstr(buf, "ltpo_min_fps=");
    if (!pa || !pm) return false;
    *out_auto_mode = (int)strtol(pa + 10, NULL, 10);
    *out_min_fps = (int)strtol(pm + 13, NULL, 10);
    return true;
}

static void sync_ltpo_policy(UdfpsState* st, int bl, int64_t now, bool force) {
    if (st->display_on != 1 || bl <= 0 || st->finger_down || st->lhbm_on) return;
    if (!force && (now - st->last_ltpo_sync_ms) < LTPO_SYNC_INTERVAL_MS) return;
    st->last_ltpo_sync_ms = now;

    int auto_mode = -1, cur_min_fps = -1;
    if (!read_ltpo_status(&auto_mode, &cur_min_fps)) return;
    if (auto_mode != 1) {
        if (st->ltpo_last_auto_mode != 0) {
            logi("LTPO: auto_mode=%d (non-LTPO mode e.g. 144Hz), pausing writes", auto_mode);
        }
        st->ltpo_last_auto_mode = 0;
        st->ltpo_tier = -1;
        return;
    }

    int tier;
    if (st->ltpo_tier == 2) {
        tier = (bl >= LTPO_BL_HIGH_OFF) ? 2 : ((bl >= LTPO_BL_MID_ON) ? 1 : 0);
    } else if (st->ltpo_tier == 1) {
        tier = (bl >= LTPO_BL_HIGH_ON) ? 2 : ((bl >= LTPO_BL_MID_OFF) ? 1 : 0);
    } else {
        tier = (bl >= LTPO_BL_HIGH_ON) ? 2 : ((bl >= LTPO_BL_MID_ON) ? 1 : 0);
    }

    const char* profile = (tier == 2) ? "30-5-5-1-3-8-3"
                        : (tier == 1) ? "30-20-10-5"
                                      : "30-20-10";
    int target_min = (tier == 2) ? 1 : ((tier == 1) ? 5 : 10);

    if (force || st->ltpo_last_auto_mode != 1 || tier != st->ltpo_tier || cur_min_fps != target_min) {
        bool ok = write_sysfs_str(SYSFS_LTPO_MODE, profile);
        logi("LTPO: bl=%d auto_mode=1 -> tier=%d (%s, min=%dHz, prev_min=%dHz, ok=%d)",
             bl, tier, profile, target_min, cur_min_fps, ok ? 1 : 0);
        st->ltpo_tier = tier;
        st->ltpo_last_auto_mode = 1;
    }
}

static void sync_display_state(UdfpsState* st, int64_t now) {
    int bl = read_display_bl();
    if (bl < 0) return;

    if (bl > 0) {
        st->bl_zero_since_ms = 0;
        bool just_turned_on = false;
        /* Hold VTS in gesture mode while a screen-off capture is in flight (LHBM raises bl_level=~956) */
        if (st->display_on != 1 && !(st->finger_down && st->down_from_vts)) {
            st->display_on = 1;
            just_turned_on = true;
            disarm_screen_off_fp(st, "dispOn");
            set_vts_lcd_state(true);
            logi("DISPLAY_ON: bl=%d -> vts_lcd=1 (lcd_on=%d session=%d)",
                 bl, read_sysfs_int(SYSFS_VTS_LCD_ON), st->session_active ? 1 : 0);
        }
        if (st->display_on == 1) {
            sync_ltpo_policy(st, bl, now, just_turned_on);
        }
    } else if (st->display_on != 0 && !st->finger_down && !st->wake_pending) {
        int64_t debounce = is_panel_dpms_on() ? DISPLAY_OFF_DEBOUNCE_MS : DISPLAY_OFF_DEBOUNCE_DPMS;
        if (st->bl_zero_since_ms == 0) {
            st->bl_zero_since_ms = now;
        } else if ((now - st->bl_zero_since_ms) >= debounce) {
            st->display_on = 0;
            st->display_off_time_ms = now;
            st->ltpo_tier = -1;
            /* Stock OriginOS parity: ONLY arm VTS FP gesture + kernel hbm_notify when an
             * active authenticate() session exists; otherwise keep GT9916 in deep sleep! */
            if (st->session_active) {
                arm_screen_off_fp(st, "dispOff");
            } else {
                disarm_screen_off_fp(st, "dispOffIdle");
            }
            set_vts_lcd_state(false);
            logi("DISPLAY_OFF: bl=0 dpms=%d aod=%d -> vts_lcd=0 (lcd_on=%d session=%d armed=%d)",
                 is_panel_dpms_on() ? 1 : 0, read_aod_current_bl(),
                 read_sysfs_int(SYSFS_VTS_LCD_ON), st->session_active ? 1 : 0,
                 st->screen_off_armed ? 1 : 0);
        }
    }
}

/*
 * Stock-native kernel-armed screen-off touch handler (`vivo_fp` EV_KEY 0xfe).
 * When `hbm_notify_enable == 1`, `fp_dispatch_event.ko` ALREADY invoked `vivo_display.ko`'s
 * `panel_update_hbm_aod_ud_for_tp()` on finger DOWN:
 *   - If panel is DPMS_OFF: kernel armed `[panel+0x4b4] = 10` (auto-lit inside `panel_doze()`/`__panel_displayon()`).
 *   - If panel is DOZE/ON: kernel immediately executed `panel_set_ud_local_hbm(panel, 10)`.
 * In BOTH cases, `vivo_display.ko` waits for the exact hardware VBLANK and emits `FINGERSHOOTER_SHOW=1`.
 */
static void handle_vts_fp_key(BinderApi* api, AIBinder** ext_ptr, UdfpsState* st,
                              bool is_down, const char* src, int64_t now) {
    sync_display_state(st, now);
    ensure_ext(api, ext_ptr);
    AIBinder* ext = *ext_ptr;

    if (is_down) {
        if (st->display_on != 0 || !st->session_active) return;
        if (st->finger_down || (now - st->display_off_time_ms) < SCREEN_OFF_COOLDOWN_MS) return;

        bool dpms_on = is_panel_dpms_on();
        st->finger_down = true;
        st->lhbm_on = true; /* kernel armed/enabled LHBM; ensure UP always writes oled_hbm=11 */
        st->ui_ready_sent = false;
        st->down_from_vts = true;
        st->saved_bl = 0;
        st->down_time_ms = now;
        st->fast_poll_until_ms = now + 1000;

        if (!st->hbm_notify_armed && dpms_on) {
            write_sysfs_int(SYSFS_OLED_HBM, OLED_LHBM_ON);
        }

        int32_t r1 = -1, r2 = -1;
        binder_status_t bs = -32;
        if (ext) {
            send_cmd(api, ext, CMD_ON_INTERACTIVE_CHANGED, 1, 0, "", &r1);
            bs = send_cmd(api, ext, CMD_NOTIFY_FINGER_STATE, FINGER_STATE_DOWN, 0, "", &r2);
        }

        if (!dpms_on) {
            st->wake_pending = true;
            st->wake_fallback_sent = false;
            st->wake_press_ms = now;
            send_doze_pulse_async();
            logi("SCREEN_OFF_DOWN (%s): dpms=0 hbm_armed=%d -> doze.pulse + cmd32534(0) st=%d ret=%d",
                 src, st->hbm_notify_armed ? 1 : 0, bs, r2);
        } else {
            st->wake_pending = false;
            logi("SCREEN_OFF_DOWN (%s): dpms=1 aod=%d hbm_armed=%d -> kernel LHBM + cmd32534(0) st=%d ret=%d",
                 src, read_aod_current_bl(), st->hbm_notify_armed ? 1 : 0, bs, r2);
        }
    } else {
        if (st->finger_down && st->down_from_vts && ext &&
            (st->wake_pending || (now - st->down_time_ms) >= 30)) {
            handle_finger_up(api, ext, st, src);
        }
    }
}

static void handle_uevent_fd(BinderApi* api, AIBinder** ext_ptr, UdfpsState* st, int uev_fd) {
    char buf[4096];
    for (;;) {
        ssize_t n = recv(uev_fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) break;
        buf[n] = '\0';

        ssize_t pos = 0;
        while (pos < n) {
            const char* item = buf + pos;
            size_t len = strlen(item);
            if (strcmp(item, "FINGERSHOOTER_SHOW=1") == 0) {
                int64_t dt = st->finger_down ? (now_ms() - st->down_time_ms) : -1;
                logi("UEVENT: FINGERSHOOTER_SHOW=1 (down=%d vts=%d +%lldms bl=%d)",
                     st->finger_down ? 1 : 0, st->down_from_vts ? 1 : 0,
                     (long long)dt, read_display_bl());
                st->wake_pending = false;
                if (st->finger_down && !st->ui_ready_sent) {
                    ensure_ext(api, ext_ptr);
                    if (*ext_ptr) send_ui_ready(api, *ext_ptr, st, "uevent");
                }
            }
            pos += (ssize_t)len + 1;
        }
    }
}

static void handle_hal_log_line(BinderApi* api, AIBinder** ext_ptr, UdfpsState* st, char* line) {
    if (line[0] != '\0' && line[1] == '/' && strncmp(line + 2, "_V_libvtsthp:", 13) == 0) {
        if (strstr(line, "change mode from") || strstr(line, "VTS_STA_LCD")) {
            logi("THP: %s", line);
        }
        return;
    }
    if (line[0] == '\0' || line[1] != '/' || strncmp(line + 2, "_V_Fingerprint", 14) != 0) return;

    ensure_ext(api, ext_ptr);
    AIBinder* ext = *ext_ptr;
    if (!ext) return;

    if (strstr(line, "onPointerDown")) {
        handle_aosp_finger_down(api, ext, st);
    } else if (strstr(line, "onPointerUp")) {
        handle_finger_up(api, ext, st, "pointerUp");
    } else if (strstr(line, "onAuthentication(fid=")) {
        logi("HAL: %s", line);
        if (strstr(line, "onAuthentication(fid=0)")) {
            handle_finger_up(api, ext, st, "authFail");
        } else {
            st->session_active = false;
            disarm_screen_off_fp(st, "authDone");
            handle_finger_up(api, ext, st, "authDone");
        }
    } else if (strstr(line, "onEnrollResult(")) {
        logi("HAL: %s", line);
        if (strstr(line, "rem=0")) {
            st->session_active = false;
            disarm_screen_off_fp(st, "enrollDone");
            handle_finger_up(api, ext, st, "enrollDone");
        } else {
            turn_off_lhbm(st);
        }
    } else if (strstr(line, "onAcquired(7, 0)") || strstr(line, "authenticate")) {
        if (!st->session_active) logi("HAL: %s", line);
        st->session_active = true;
        if (st->display_on == 0) arm_screen_off_fp(st, "authStart");
        if (strstr(line, "onAcquired(7, 0)") && st->finger_down) {
            handle_finger_up(api, ext, st, "newSession");
        }
    } else if (strstr(line, "CancellationSignal") || strstr(line, "HAL CANCEL") ||
               strstr(line, "onSessionClosed") || strstr(line, "onError(")) {
        logi("HAL: %s", line);
        st->session_active = false;
        disarm_screen_off_fp(st, "sessionEnd");
        if (st->finger_down) handle_finger_up(api, ext, st, "sessionEnd");
    }
}

static int run_daemon(BinderApi* api) {
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGCHLD, SIG_IGN);

    AIBinder* ext = NULL;
    while (!g_stop && !ext) {
        ext = acquire_fp_ext(api);
        if (!ext) sleep(1);
    }
    if (!ext) return 1;

    int32_t ret = -1;
    binder_status_t bs = send_cmd(api, ext, CMD_ON_INTERACTIVE_CHANGED, 1, 0, "", &ret);
    logi("connected IFingerprintExt, interactive(1) st=%d ret=%d", bs, ret);

    char vfp_path[64] = {0}, ts_path[64] = {0}, fp_path[64] = {0}, pwr_path[64] = {0};
    int uev_fd     = init_uevent_socket();
    int ino_fd     = init_settings_inotify();
    int vivo_fp_fd = open_input_by_name("vivo_fp", vfp_path, sizeof(vfp_path));
    int vts_ts_fd  = open_input_by_name("vivo_ts", ts_path, sizeof(ts_path));
    int vts_fp_fd  = open_input_by_name("vivo_ts_fp", fp_path, sizeof(fp_path));
    int pwr_fd     = open_input_by_name("pmic_pwrkey", pwr_path, sizeof(pwr_path));
    int uinput_fd  = init_uinput_wakeup();
    logi("fds: uevent=%d inotify=%d vivo_fp=%s(%d) vivo_ts=%s(%d) vivo_ts_fp=%s(%d) pwr=%s(%d) uinput=%d",
         uev_fd, ino_fd,
         vivo_fp_fd >= 0 ? vfp_path : "none", vivo_fp_fd,
         vts_ts_fd >= 0 ? ts_path : "none", vts_ts_fd,
         vts_fp_fd >= 0 ? fp_path : "none", vts_fp_fd,
         pwr_fd >= 0 ? pwr_path : "none", pwr_fd, uinput_fd);

    UdfpsState st = {0};
    st.ltpo_tier = -1;
    st.ltpo_last_auto_mode = -1;
    vperf_init(&st.vperf);
    int init_bl = read_display_bl();
    st.display_on = (init_bl > 0) ? 1 : 0;
    /* Start disarmed until HAL actually starts an authenticate() session */
    set_hbm_notify_enable(false);
    disable_vts_screen_off_fp();
    set_vts_lcd_state(st.display_on != 0);
    int64_t last_disp_poll_ms = now_ms();
    if (st.display_on == 1) {
        sync_ltpo_policy(&st, init_bl, last_disp_poll_ms, true);
    }
    sync_refresh_rate_lock(&st, true);
    st.last_refresh_retry_ms = last_disp_poll_ms;

    while (!g_stop) {
        FILE* lp = popen("/system/bin/logcat -b main -v tag -T 1", "r");
        if (!lp) { sleep(2); continue; }
        int log_fd = fileno(lp);
        int fl = fcntl(log_fd, F_GETFL, 0);
        if (fl >= 0) fcntl(log_fd, F_SETFL, fl | O_NONBLOCK);

        char rbuf[4096];
        size_t rlen = 0;
        bool log_eof = false;

        while (!g_stop && !log_eof) {
            struct pollfd pfds[7];
            int nfds = 0;
            int idx_uev = (uev_fd >= 0) ? nfds++ : -1;
            if (idx_uev >= 0) pfds[idx_uev] = (struct pollfd){ .fd = uev_fd, .events = POLLIN };
            int idx_ino = (ino_fd >= 0) ? nfds++ : -1;
            if (idx_ino >= 0) pfds[idx_ino] = (struct pollfd){ .fd = ino_fd, .events = POLLIN };
            int idx_log = nfds++; pfds[idx_log] = (struct pollfd){ .fd = log_fd, .events = POLLIN };
            int idx_vfp = (vivo_fp_fd >= 0) ? nfds++ : -1;
            if (idx_vfp >= 0) pfds[idx_vfp] = (struct pollfd){ .fd = vivo_fp_fd, .events = POLLIN };
            int idx_ts  = (vts_ts_fd >= 0) ? nfds++ : -1;
            if (idx_ts >= 0)  pfds[idx_ts]  = (struct pollfd){ .fd = vts_ts_fd, .events = POLLIN };
            int idx_fp  = (vts_fp_fd >= 0) ? nfds++ : -1;
            if (idx_fp >= 0)  pfds[idx_fp]  = (struct pollfd){ .fd = vts_fp_fd, .events = POLLIN };
            int idx_pwr = (pwr_fd >= 0) ? nfds++ : -1;
            if (idx_pwr >= 0) pfds[idx_pwr] = (struct pollfd){ .fd = pwr_fd, .events = POLLIN };

            int64_t pre_now = now_ms();
            int timeout_ms = 200;
            if (st.finger_down && !st.ui_ready_sent) timeout_ms = 10;
            else if (st.finger_down || st.bl_zero_since_ms != 0 || pre_now < st.fast_poll_until_ms) timeout_ms = 20;

            int pr = poll(pfds, (nfds_t)nfds, timeout_ms);
            int64_t now = now_ms();

            /* Highest priority: kernel hardware VBLANK FINGERSHOOTER_SHOW=1 */
            if (idx_uev >= 0 && (pfds[idx_uev].revents & POLLIN)) {
                handle_uevent_fd(api, &ext, &st, uev_fd);
            }

            if (idx_ino >= 0 && (pfds[idx_ino].revents & POLLIN)) {
                char ibuf[2048] __attribute__((aligned(__alignof__(struct inotify_event))));
                for (;;) {
                    ssize_t n = read(ino_fd, ibuf, sizeof(ibuf));
                    if (n <= 0) break;
                    ssize_t off = 0;
                    while (off + (ssize_t)sizeof(struct inotify_event) <= n) {
                        const struct inotify_event* ev = (const struct inotify_event*)(ibuf + off);
                        if (ev->len > 0 && strcmp(ev->name, "settings_system.xml") == 0) {
                            st.settings_sync_due_ms = now + SETTINGS_DEBOUNCE_MS;
                        }
                        off += (ssize_t)sizeof(struct inotify_event) + (ssize_t)ev->len;
                    }
                }
            }
            if (st.settings_sync_due_ms != 0 && now >= st.settings_sync_due_ms) {
                st.settings_sync_due_ms = 0;
                sync_refresh_rate_lock(&st, false);
            }
            if (!st.initial_refresh_synced && (now - st.last_refresh_retry_ms) >= 2000) {
                st.last_refresh_retry_ms = now;
                sync_refresh_rate_lock(&st, true);
            }

            if (idx_pwr >= 0 && (pfds[idx_pwr].revents & POLLIN)) {
                struct input_event ev;
                while (read(pwr_fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
                    if (ev.type == EV_KEY) st.fast_poll_until_ms = now + 800;
                }
            }

            if (st.bl_zero_since_ms != 0 || now < st.fast_poll_until_ms ||
                (st.display_on == 0 && pr > 0 && (now - last_disp_poll_ms) >= 20) ||
                (now - last_disp_poll_ms) >= 150) {
                sync_display_state(&st, now);
                if (st.display_on == 0 && st.vperf.active_handle > 0) {
                    vperf_release_active(&st.vperf);
                }
                last_disp_poll_ms = now;
            }

            if (idx_vfp >= 0 && (pfds[idx_vfp].revents & POLLIN)) {
                struct input_event ev;
                while (read(vivo_fp_fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
                    if (ev.type == EV_KEY && ev.code == KEY_VTS_FP_DETECT) {
                        handle_vts_fp_key(api, &ext, &st, ev.value != 0, ev.value ? "vivo_fp" : "vivo_fp_up", now);
                    }
                }
            }

            if (idx_ts >= 0 && (pfds[idx_ts].revents & POLLIN)) {
                struct input_event ev;
                bool ts_down_edge = false, ts_up_edge = false, ts_moved_edge = false;
                while (read(vts_ts_fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
                    if (ev.type == EV_KEY && ev.code == KEY_VTS_FP_DETECT) {
                        handle_vts_fp_key(api, &ext, &st, ev.value != 0, ev.value ? "vts_ts" : "vts_ts_up", now);
                    } else if (st.display_on == 1) {
                        if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
                            if (ev.value != 0) {
                                st.touch_down = true;
                                st.touch_moved = false;
                                st.touch_have_start = false;
                                ts_down_edge = true;
                            } else {
                                st.touch_down = false;
                                ts_up_edge = true;
                            }
                        } else if (ev.type == EV_ABS && st.touch_down) {
                            if (ev.code == ABS_MT_POSITION_X) {
                                if (!st.touch_have_start) st.touch_start_x = ev.value;
                                st.touch_cur_x = ev.value;
                            } else if (ev.code == ABS_MT_POSITION_Y) {
                                if (!st.touch_have_start) {
                                    st.touch_start_y = ev.value;
                                    st.touch_have_start = true;
                                }
                                st.touch_cur_y = ev.value;
                                int dx = st.touch_cur_x - st.touch_start_x;
                                int dy = st.touch_cur_y - st.touch_start_y;
                                if (dx < 0) dx = -dx;
                                if (dy < 0) dy = -dy;
                                if (dx + dy >= VPERF_MOVE_THRESH_PX) {
                                    st.touch_moved = true;
                                    ts_moved_edge = true;
                                }
                            }
                        }
                    }
                }
                if (st.display_on == 1 && st.vperf.ready) {
                    int32_t prev_h = ((now - st.last_scroll_boost_ms) < 1200) ? st.vperf.active_handle : 0;
                    if (ts_down_edge && !ts_moved_edge) {
                        if ((now - st.last_scroll_boost_ms) >= 80) {
                            vperf_hint_acq_rel(&st.vperf, prev_h,
                                               QTI_HINT_SCROLL_BOOST, VPERF_TOUCH_DOWN_MS,
                                               QTI_SCROLL_TYPE_LIGHT, now);
                            st.last_scroll_boost_ms = now;
                            prev_h = st.vperf.active_handle;
                        }
                    }
                    if (ts_moved_edge) {
                        if ((now - st.last_scroll_boost_ms) >= VPERF_DRAG_RENEW_MS) {
                            vperf_hint_acq_rel(&st.vperf, prev_h,
                                               QTI_HINT_SCROLL_BOOST, VPERF_DRAG_BOOST_MS,
                                               QTI_SCROLL_TYPE_VERTICAL, now);
                            st.last_scroll_boost_ms = now;
                            prev_h = st.vperf.active_handle;
                        }
                    }
                    if (ts_up_edge) {
                        int32_t dur = st.touch_moved ? VPERF_FLING_BOOST_MS : VPERF_TAP_OPEN_MS;
                        int32_t typ = st.touch_moved ? QTI_SCROLL_TYPE_VERTICAL : QTI_SCROLL_TYPE_LIGHT;
                        vperf_hint_acq_rel(&st.vperf, prev_h,
                                           QTI_HINT_SCROLL_BOOST, dur, typ, now);
                        st.last_scroll_boost_ms = now;
                        st.touch_moved = false;
                        st.touch_have_start = false;
                    }
                } else if (st.display_on == 0 && st.vperf.active_handle > 0) {
                    vperf_release_active(&st.vperf);
                }
            }

            if (idx_fp >= 0 && (pfds[idx_fp].revents & POLLIN)) {
                struct input_event ev;
                while (read(vts_fp_fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
                    if (ev.type == EV_KEY && ev.code == KEY_VTS_FP_DETECT) {
                        handle_vts_fp_key(api, &ext, &st, ev.value != 0, ev.value ? "vts_fp" : "vts_fp_up", now);
                    }
                }
            }

            /* Safety fallback if Doze pulse was suppressed and FINGERSHOOTER_SHOW=1 hasn't arrived */
            if (st.finger_down && st.wake_pending && !st.ui_ready_sent &&
                !st.wake_fallback_sent && (now - st.wake_press_ms) >= DOZE_PULSE_FALLBACK_MS) {
                st.wake_fallback_sent = true;
                bool woke = send_wakeup_key(uinput_fd);
                logi("SCREEN_OFF_FALLBACK: no uevent at +%lldms -> KEY_WAKEUP(ok=%d)",
                     (long long)(now - st.wake_press_ms), woke ? 1 : 0);
            }

            /* Edge case: panel entered DPMS_OFF right as finger pressed -> trigger doze.pulse */
            if (st.finger_down && st.down_from_vts && !st.ui_ready_sent && !st.wake_pending &&
                (now - st.down_time_ms) >= 90 && !is_panel_dpms_on()) {
                st.wake_pending = true;
                st.wake_fallback_sent = false;
                st.wake_press_ms = now;
                send_doze_pulse_async();
                logi("SCREEN_OFF_RETRY: panel went dpms=0 at +%lldms -> doze.pulse",
                     (long long)(now - st.down_time_ms));
            }

            /* Safety fallback for UI_READY only if uevent wasn't received (e.g. LHBM already active) */
            if (st.finger_down && !st.ui_ready_sent && !st.wake_pending) {
                int64_t limit = st.down_from_vts ? UI_READY_FALLBACK_DOZE_MS : UI_READY_FALLBACK_AOSP_MS;
                if ((now - st.down_time_ms) >= limit) {
                    ensure_ext(api, &ext);
                    if (ext) send_ui_ready(api, ext, &st, "fallback");
                }
            }

            if (st.finger_down && (now - st.down_time_ms) >= LHBM_WATCHDOG_MS) {
                if (ext && api->isAlive(ext)) handle_finger_up(api, ext, &st, "watchdog");
            }

            if (pr > 0 && (pfds[idx_log].revents & (POLLIN | POLLHUP | POLLERR))) {
                for (;;) {
                    if (rlen >= sizeof(rbuf) - 1) rlen = 0;
                    ssize_t n = read(log_fd, rbuf + rlen, sizeof(rbuf) - 1 - rlen);
                    if (n > 0) {
                        rlen += (size_t)n;
                        rbuf[rlen] = '\0';
                        char* start = rbuf;
                        char* nl;
                        while ((nl = strchr(start, '\n')) != NULL) {
                            *nl = '\0';
                            if (nl > start && *(nl - 1) == '\r') *(nl - 1) = '\0';
                            handle_hal_log_line(api, &ext, &st, start);
                            start = nl + 1;
                        }
                        size_t rem = (size_t)(rbuf + rlen - start);
                        if (rem > 0 && start != rbuf) memmove(rbuf, start, rem);
                        rlen = rem;
                    } else if (n == 0) {
                        log_eof = true;
                        break;
                    } else {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        if (errno == EINTR) continue;
                        log_eof = true;
                        break;
                    }
                }
            }
        }
        pclose(lp);
        if (st.finger_down && ext) handle_finger_up(api, ext, &st, "logcatEof");
        if (!g_stop) sleep(1);
    }

    if (st.finger_down && ext) handle_finger_up(api, ext, &st, "shutdown");
    vperf_release_active(&st.vperf);
    set_hbm_notify_enable(false);
    disable_vts_screen_off_fp();
    set_vts_lcd_state(true);
    if (uev_fd >= 0) close(uev_fd);
    if (ino_fd >= 0) close(ino_fd);
    if (vivo_fp_fd >= 0) close(vivo_fp_fd);
    if (vts_ts_fd >= 0) close(vts_ts_fd);
    if (vts_fp_fd >= 0) close(vts_fp_fd);
    if (pwr_fd >= 0) close(pwr_fd);
    if (uinput_fd >= 0) { ioctl(uinput_fd, UI_DEV_DESTROY); close(uinput_fd); }
    if (ext) api->decStrong(ext);
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 4 && strcmp(argv[1], "--patch-hal") == 0) {
        log_init(false);
        return patch_vendor_hal(argv[2], argv[3]);
    }
    if (argc == 4 && strcmp(argv[1], "--patch-composer") == 0) {
        log_init(false);
        return patch_vendor_composer(argv[2], argv[3]);
    }
    if (argc == 2 && strcmp(argv[1], "--test-vperf") == 0) {
        log_init(false);
        VPerfClient vp;
        if (!vperf_init(&vp)) return 1;
        int32_t h1 = vperf_hint_acq_rel(&vp, 0, QTI_HINT_SCROLL_BOOST, 400, QTI_SCROLL_TYPE_VERTICAL, 0);
        int32_t h2 = vperf_hint_acq_rel(&vp, h1 > 0 ? h1 : 0, QTI_HINT_SCROLL_BOOST, 300, QTI_SCROLL_TYPE_LIGHT, 0);
        printf("IVPerf@1.0 test: binder=%p h1=%d h2=%d ok=%llu fail=%llu\n",
               vp.vperf_binder, h1, h2,
               (unsigned long long)vp.boost_ok, (unsigned long long)vp.boost_fail);
        vperf_release_active(&vp);
        return (h1 > 0 && h2 > 0) ? 0 : 2;
    }
    log_init(argc == 1);
    if (argc == 1) logi("daemon started pid=%d uid=%d (stock-native v18)", (int)getpid(), (int)getuid());

    BinderApi api;
    if (!binder_api_init(&api)) return 1;

    if (argc == 2 && strcmp(argv[1], "--test-ext") == 0) {
        AIBinder* ext = acquire_fp_ext(&api);
        if (!ext) return 2;
        int32_t ret = -999;
        binder_status_t st = send_cmd(&api, ext, CMD_ON_INTERACTIVE_CHANGED, 1, 0, "", &ret);
        char vfp[64] = {0}, ts[64] = {0}, fp[64] = {0};
        int ufd_uev = init_uevent_socket();
        int vfd = open_input_by_name("vivo_fp", vfp, sizeof(vfp));
        int tfd = open_input_by_name("vivo_ts", ts, sizeof(ts));
        int ffd = open_input_by_name("vivo_ts_fp", fp, sizeof(fp));
        int ufd = init_uinput_wakeup();
        int hbm_ne = read_sysfs_int(SYSFS_HBM_NOTIFY_ENABLE);
        printf("IFingerprintExt test: st=%d ret=%d | uevent=%d hbm_notify_enable=%d vivo_fp=%s(%d) vivo_ts=%s(%d) vivo_ts_fp=%s(%d) uinput=%d\n",
               st, ret, ufd_uev, hbm_ne,
               vfd >= 0 ? vfp : "none", vfd,
               tfd >= 0 ? ts : "none", tfd,
               ffd >= 0 ? fp : "none", ffd, ufd);
        if (ufd_uev >= 0) close(ufd_uev);
        if (vfd >= 0) close(vfd);
        if (tfd >= 0) close(tfd);
        if (ffd >= 0) close(ffd);
        if (ufd >= 0) { ioctl(ufd, UI_DEV_DESTROY); close(ufd); }
        api.decStrong(ext);
        return (st == 0 && ufd_uev >= 0 && vfd >= 0 && ufd >= 0 && hbm_ne >= 0) ? 0 : 3;
    }

    return run_daemon(&api);
}
