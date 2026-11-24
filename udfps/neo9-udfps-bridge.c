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
#define SYSFS_DRM_DPMS             "/sys/class/drm/card0-DSI-1/dpms"
#define OLED_LHBM_ON               10   /* 0x0a -> panel_set_ud_local_hbm ON  */
#define OLED_LHBM_OFF              11   /* 0x0b -> panel_set_ud_local_hbm OFF (also cancels pending LHBM) */

#define SYSFS_VTS_APP_NAME         "/sys/touchscreen/app_name"
#define SYSFS_VTS_GESTURE          "/sys/devices/virtual/vts/vivo_ts/status/gesture"
#define SYSFS_VTS_LCD_ON           "/sys/devices/virtual/vts/vivo_ts/status/lcd_on"
#define VTS_GESTURE_FP_BIT         4096 /* 0x1000 */
#define KEY_VTS_FP_DETECT          0xfe /* 254: VTS_EVENT_GESTURE_FINGERPRINT_DETECT */

#define IFP_SERVICE_NAME           "android.hardware.biometrics.fingerprint.IFingerprint/default"
#define IFP_EXT_DESCRIPTOR         "vendor.vivo.hardware.biometrics.fingerprint.IFingerprintExt"
#define TX_SEND_CMD_BUNDLE         1

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

/* ---- UDFPS State Machine ---- */
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
    int64_t bl_zero_since_ms;
    int64_t display_off_time_ms;
    int64_t fast_poll_until_ms;
    int64_t down_time_ms;
    int64_t wake_press_ms;
    int saved_bl;
} UdfpsState;

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

static void sync_display_state(UdfpsState* st, int64_t now) {
    int bl = read_display_bl();
    if (bl < 0) return;

    if (bl > 0) {
        st->bl_zero_since_ms = 0;
        /* Hold VTS in gesture mode while a screen-off capture is in flight (LHBM raises bl_level=~956) */
        if (st->display_on != 1 && !(st->finger_down && st->down_from_vts)) {
            st->display_on = 1;
            disarm_screen_off_fp(st, "dispOn");
            set_vts_lcd_state(true);
            logi("DISPLAY_ON: bl=%d -> vts_lcd=1 (lcd_on=%d session=%d)",
                 bl, read_sysfs_int(SYSFS_VTS_LCD_ON), st->session_active ? 1 : 0);
        }
    } else if (st->display_on != 0 && !st->finger_down && !st->wake_pending) {
        int64_t debounce = is_panel_dpms_on() ? DISPLAY_OFF_DEBOUNCE_MS : DISPLAY_OFF_DEBOUNCE_DPMS;
        if (st->bl_zero_since_ms == 0) {
            st->bl_zero_since_ms = now;
        } else if ((now - st->bl_zero_since_ms) >= debounce) {
            st->display_on = 0;
            st->display_off_time_ms = now;
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
    int vivo_fp_fd = open_input_by_name("vivo_fp", vfp_path, sizeof(vfp_path));
    int vts_ts_fd  = open_input_by_name("vivo_ts", ts_path, sizeof(ts_path));
    int vts_fp_fd  = open_input_by_name("vivo_ts_fp", fp_path, sizeof(fp_path));
    int pwr_fd     = open_input_by_name("pmic_pwrkey", pwr_path, sizeof(pwr_path));
    int uinput_fd  = init_uinput_wakeup();
    logi("fds: uevent=%d vivo_fp=%s(%d) vivo_ts=%s(%d) vivo_ts_fp=%s(%d) pwr=%s(%d) uinput=%d",
         uev_fd,
         vivo_fp_fd >= 0 ? vfp_path : "none", vivo_fp_fd,
         vts_ts_fd >= 0 ? ts_path : "none", vts_ts_fd,
         vts_fp_fd >= 0 ? fp_path : "none", vts_fp_fd,
         pwr_fd >= 0 ? pwr_path : "none", pwr_fd, uinput_fd);

    UdfpsState st = {0};
    st.display_on = (read_display_bl() != 0) ? 1 : 0;
    /* Start disarmed until HAL actually starts an authenticate() session */
    set_hbm_notify_enable(false);
    disable_vts_screen_off_fp();
    set_vts_lcd_state(st.display_on != 0);
    int64_t last_disp_poll_ms = now_ms();

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
            struct pollfd pfds[6];
            int nfds = 0;
            int idx_uev = (uev_fd >= 0) ? nfds++ : -1;
            if (idx_uev >= 0) pfds[idx_uev] = (struct pollfd){ .fd = uev_fd, .events = POLLIN };
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
                while (read(vts_ts_fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
                    if (ev.type == EV_KEY && ev.code == KEY_VTS_FP_DETECT) {
                        handle_vts_fp_key(api, &ext, &st, ev.value != 0, ev.value ? "vts_ts" : "vts_ts_up", now);
                    }
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
    set_hbm_notify_enable(false);
    disable_vts_screen_off_fp();
    set_vts_lcd_state(true);
    if (uev_fd >= 0) close(uev_fd);
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
    log_init(argc == 1);
    if (argc == 1) logi("daemon started pid=%d uid=%d (stock-native v17)", (int)getpid(), (int)getuid());

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
