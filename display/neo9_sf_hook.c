/*
 * libneo9_sf_hook.so — Stock OriginOS 4 SurfaceFlinger 360Hz High-TE EPT Bridge for Android 17 GSI
 *
 * Target: vivo iQOO Neo9 (PD2338, SM8550, RM692H5 8T LTPO 360Hz High-TE panel)
 * Loaded into /system/bin/surfaceflinger via DT_NEEDED (libpowermanager.so -> libneo9_sf_hook.so,
 * re-exporting DT_NEEDED libpowermanager.so).
 *
 * Root Cause (reverse-engineered from stock OriginOS 4 surfaceflinger, composer-service & msm_drm.ko):
 *   On PD2338 RM692H5 8T LTPO modes (60/72/90/120 Hz, group 0, auto_mode=1, high_te=360 Hz),
 *   msm_drm.ko (vivo_display_pre_kickoff_update -> vivo_commit_delay_target_time) waits for the
 *   exact 360 Hz TE immediately preceding DRMConnector property "EPT" (expected_present_time).
 *   Stock OriginOS 4 SurfaceFlinger prepends Vivo proprietary HIDL 2.4 FMQ command 0x10020002
 *   (opcode 0x10020000, len=2: lo32(EPT), hi32(EPT)) right before PRESENT_OR_VALIDATE_DISPLAY,
 *   which vendor composer-service decodes into HWCDisplay::SetExpectedPresentTime([this+0x4b8])
 *   and passes to libsdedrm -> DRMConnector "EPT".
 *
 *   AOSP Android 17 SurfaceFlinger ALREADY computes the exact ground-truth expectedPresentTime
 *   (int64_t CLOCK_MONOTONIC ns) and passes it in register x2 to both HidlComposer::validateDisplay()
 *   and HidlComposer::presentOrValidateDisplay(), but drops it because standard HIDL 2.4 has no
 *   SET_EXPECTED_PRESENT_TIME opcode.
 *
 *   This library dynamically locates HidlComposer's validateDisplay / presentOrValidateDisplay
 *   vtable slots and CommandWriterBase helpers inside /system/bin/surfaceflinger (zero hardcoded
 *   offsets) and emits the exact stock 0x10020002 + expectedPresentTime command into mWriter before
 *   forwarding to the original methods.
 */

#define _GNU_SOURCE
#include <android/log.h>
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define LOG_TAG "neo9-sf-ept"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/* Vivo proprietary HIDL 2.4 FMQ opcode: Command 0x10020000 | length 2 uint32 words */
#define VIVO_CMD_SET_EXPECTED_PRESENT_TIME 0x10020002u

/* Standard AOSP HIDL 2.1 ComposerClient Command high 16 bits */
#define HIDL_CMD_VALIDATE_DISPLAY_HI            0x0203u
#define HIDL_CMD_PRESENT_OR_VALIDATE_DISPLAY_HI 0x0206u

/* Android HWC2 CommandWriterBase memory layout (AOSP hardware/interfaces/graphics/composer/2.1) */
typedef struct {
    void*     vtable;
    uint32_t* mData;
    uint32_t  mDataWritten;
    uint32_t  mDataMaxSize;
    uint32_t  mCommandEnd;
} HwcCommandWriter;

/* Composer::OptionalFeature::ExpectedPresentTime = 1 (AOSP ComposerHal.h) */
#define COMPOSER_FEATURE_EXPECTED_PRESENT_TIME 1

/*
 * PD2338 RM692H5 timing@144 (6.94-6.97 ms) runs native 144 Hz TE (NOT 360 Hz TE),
 * whereas 60/72/90/120 Hz (8.33-16.67 ms) run 360 Hz High-TE (auto-mode).
 * msm_drm.ko vivo_display_pre_kickoff_update hardcodes 2.778 ms (360 Hz) whenever
 * DRMConnector "EPT" > 0, so 144 Hz MUST send EPT=0 to bypass the 360 Hz delay.
 */
#define RM692H5_144HZ_INTERVAL_THRESHOLD_NS 7500000

typedef bool (*fn_is_supported_t)(void* self, int32_t feature);

typedef int32_t (*fn_validate_display_t)(
    void* self, uint64_t display, int64_t expected_present_time,
    int32_t frame_interval_ns, uint32_t* out_num_types, uint32_t* out_num_requests);

typedef int32_t (*fn_present_or_validate_t)(
    void* self, uint64_t display, int64_t expected_present_time,
    int32_t frame_interval_ns, uint32_t* out_num_types, uint32_t* out_num_requests,
    int* out_present_fence, uint32_t* out_state);

typedef void (*fn_select_display_t)(HwcCommandWriter* writer, uint64_t display);
typedef void (*fn_grow_data_t)(HwcCommandWriter* writer, uint32_t grow);

static fn_is_supported_t        g_orig_is_supported = NULL;
static fn_validate_display_t    g_orig_validate     = NULL;
static fn_present_or_validate_t g_orig_pov          = NULL;
static fn_select_display_t      g_select_display    = NULL;
static fn_grow_data_t           g_grow_data         = NULL;
static size_t                   g_writer_off        = 0;

static bool neo9_isSupported(void* self, int32_t feature) {
    if (feature == COMPOSER_FEATURE_EXPECTED_PRESENT_TIME) {
        LOGI("HidlComposer::isSupported(ExpectedPresentTime) -> true");
        return true;
    }
    return g_orig_is_supported(self, feature);
}

static inline void emit_vivo_ept(void* self, uint64_t display, int64_t expected_present_time,
                                 int32_t frame_interval_ns) {
    if (!self || !g_select_display || !g_grow_data) return;

    HwcCommandWriter* w = (HwcCommandWriter*)((uint8_t*)self + g_writer_off);
    if (w->mCommandEnd != 0) return;

    bool is_144hz = (frame_interval_ns > 0 && frame_interval_ns < RM692H5_144HZ_INTERVAL_THRESHOLD_NS);
    uint64_t ept = (expected_present_time > 0 && !is_144hz) ? (uint64_t)expected_present_time : 0ULL;

    g_select_display(w, display);
    g_grow_data(w, 3u);
    if (!w->mData || w->mDataWritten + 3u > w->mDataMaxSize) return;

    uint32_t idx = w->mDataWritten;
    w->mData[idx + 0] = VIVO_CMD_SET_EXPECTED_PRESENT_TIME;
    w->mData[idx + 1] = (uint32_t)(ept & 0xffffffffu);
    w->mData[idx + 2] = (uint32_t)(ept >> 32);
    w->mDataWritten = idx + 3u;
    w->mCommandEnd  = 0;
}

static int32_t neo9_validateDisplay(
    void* self, uint64_t display, int64_t expected_present_time,
    int32_t frame_interval_ns, uint32_t* out_num_types, uint32_t* out_num_requests) {
    emit_vivo_ept(self, display, expected_present_time, frame_interval_ns);
    return g_orig_validate(self, display, expected_present_time, frame_interval_ns,
                           out_num_types, out_num_requests);
}

static int32_t neo9_presentOrValidateDisplay(
    void* self, uint64_t display, int64_t expected_present_time,
    int32_t frame_interval_ns, uint32_t* out_num_types, uint32_t* out_num_requests,
    int* out_present_fence, uint32_t* out_state) {
    emit_vivo_ept(self, display, expected_present_time, frame_interval_ns);
    return g_orig_pov(self, display, expected_present_time, frame_interval_ns,
                      out_num_types, out_num_requests, out_present_fence, out_state);
}

/* ---- Dynamic ARM64 Scanner for HidlComposer vtable & CommandWriterBase helpers ---- */

static inline uintptr_t decode_bl(uintptr_t pc, uint32_t insn) {
    if ((insn & 0xfc000000u) != 0x94000000u) return 0;
    int32_t imm26 = (int32_t)(insn & 0x03ffffffu);
    if (imm26 & 0x02000000) imm26 |= (int32_t)0xfc000000u;
    return (uintptr_t)((intptr_t)pc + ((intptr_t)imm26 << 2));
}

static bool helper_emits_cmd(uintptr_t fn_addr, uintptr_t rx_start, uintptr_t rx_end,
                             uint32_t target_hi16, uintptr_t* out_grow) {
    if (rx_end <= rx_start + 16u) return false;
    uintptr_t rx_lim16 = rx_end - 16u;
    uintptr_t rx_lim4  = rx_end - 4u;
    uintptr_t grow = 0;
    bool found_cmd = false;
    for (int k = 0; k < 24; k++) {
        uintptr_t pc = fn_addr + (uintptr_t)k * 4u;
        if (pc < rx_start || pc > rx_lim4) break;
        uint32_t insn = *(const uint32_t*)pc;
        if (grow == 0) {
            uintptr_t b = decode_bl(pc, insn);
            if (b >= rx_start && b <= rx_lim16 && (b & 3u) == 0) grow = b;
        }
        /* movz wD, #target_hi16, lsl #16 */
        if ((insn & 0xffe00000u) == 0x52a00000u &&
            ((insn >> 5) & 0xffffu) == target_hi16) {
            found_cmd = true;
        }
    }
    if (found_cmd && grow != 0) {
        *out_grow = grow;
        return true;
    }
    return false;
}

static bool match_composer_method(uintptr_t fn_addr, uintptr_t rx_start, uintptr_t rx_end,
                                  uint32_t cmd_hi16, size_t* out_writer_off,
                                  uintptr_t* out_select, uintptr_t* out_grow) {
    if (rx_end <= rx_start + 16u) return false;
    uintptr_t rx_lim16 = rx_end - 16u;
    uintptr_t rx_lim4  = rx_end - 4u;
    size_t writer_off = 0;
    uintptr_t select_fn = 0;
    bool have_first = false;

    for (int k = 0; k < 32; k++) {
        uintptr_t pc = fn_addr + (uintptr_t)k * 4u;
        if (pc < rx_start || pc > rx_lim16) break;
        uint32_t insn = *(const uint32_t*)pc;
        /* add x0, xN, #imm12  (sf=1, op=0, S=0, shift=0, Rd=x0) */
        if ((insn & 0xffc0001fu) == 0x91000000u) {
            size_t imm12 = (size_t)((insn >> 10) & 0xfffu);
            if (imm12 == 0) continue;
            for (int j = 1; j <= 3; j++) {
                uintptr_t bpc = pc + (uintptr_t)j * 4u;
                if (bpc < rx_start || bpc > rx_lim4) break;
                uintptr_t target = decode_bl(bpc, *(const uint32_t*)bpc);
                if (target >= rx_start && target <= rx_lim16 && (target & 3u) == 0) {
                    if (!have_first) {
                        have_first = true;
                        writer_off = imm12;
                        select_fn  = target;
                    } else if (imm12 == writer_off) {
                        uintptr_t grow_fn = 0;
                        if (helper_emits_cmd(target, rx_start, rx_end, cmd_hi16, &grow_fn)) {
                            *out_writer_off = writer_off;
                            *out_select     = select_fn;
                            *out_grow       = grow_fn;
                            return true;
                        }
                    }
                    break;
                }
            }
        }
    }
    return false;
}

typedef struct {
    uintptr_t rx_start;
    uintptr_t rx_end;
    uintptr_t relro_start;
    uintptr_t relro_end;
    bool      found;
} SfImageRange;

static int phdr_cb(struct dl_phdr_info* info, size_t size, void* data) {
    (void)size;
    SfImageRange* r = (SfImageRange*)data;
    const char* name = info->dlpi_name;
    bool is_main = (!name || name[0] == '\0' ||
                    (strstr(name, "/surfaceflinger") != NULL && !strstr(name, ".so")));
    if (!is_main) {
        return 0;
    }
    uintptr_t base = (uintptr_t)info->dlpi_addr;
    uintptr_t rx_s = 0, rx_e = 0, rr_s = 0, rr_e = 0;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const Elf64_Phdr* ph = &info->dlpi_phdr[i];
        if (ph->p_type == PT_LOAD && (ph->p_flags & PF_X)) {
            rx_s = base + ph->p_vaddr;
            rx_e = rx_s + ph->p_filesz;
        } else if (ph->p_type == PT_GNU_RELRO) {
            rr_s = base + ph->p_vaddr;
            rr_e = rr_s + ph->p_filesz;
        }
    }
    if (rx_s && rx_e > rx_s && rr_s && rr_e > rr_s) {
        r->rx_start    = rx_s;
        r->rx_end      = rx_e;
        r->relro_start = rr_s;
        r->relro_end   = rr_e;
        r->found       = true;
        return 1;
    }
    return 0;
}

static bool is_surfaceflinger_process(void) {
    char buf[256];
    int fd = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return false;
    buf[n] = '\0';
    return strstr(buf, "surfaceflinger") != NULL;
}

/*
 * Match HidlComposer::isSupported(OptionalFeature) in the same vtable:
 *   ldr  xN, [x0, #imm]        (0xf9400000, Rn=x0)
 *   cmp  w1, #0x0              (0x7100003f)
 *   ccmp xN, #0x0, #0x4, eq    (0xfa400804 | (N << 5))
 *   cset w0, ne                (0x1a9f07e0)
 *   ret                        (0xd65f03c0)
 */
static bool match_hidl_is_supported(uintptr_t fn_addr, uintptr_t rx_start, uintptr_t rx_end) {
    if (rx_end <= rx_start + 20u) return false;
    if (fn_addr < rx_start || fn_addr > rx_end - 20u || (fn_addr & 3u) != 0) return false;
    const uint32_t* insns = (const uint32_t*)fn_addr;
    uint32_t i0 = insns[0], i1 = insns[1], i2 = insns[2], i3 = insns[3], i4 = insns[4];
    if ((i0 & 0xffc003e0u) != 0xf9400000u) return false;
    uint32_t rt = i0 & 0x1fu;
    if (i1 != 0x7100003fu) return false;
    if (i2 != (0xfa400804u | (rt << 5))) return false;
    if (i3 != 0x1a9f07e0u) return false;
    if (i4 != 0xd65f03c0u) return false;
    return true;
}

__attribute__((constructor))
static void neo9_sf_hook_init(void) {
    if (!is_surfaceflinger_process()) return;

    SfImageRange img;
    memset(&img, 0, sizeof(img));
    dl_iterate_phdr(phdr_cb, &img);
    if (!img.found) {
        LOGE("failed to locate surfaceflinger PT_LOAD/RELRO segments");
        return;
    }

    uintptr_t* found_slot = NULL;
    uintptr_t  found_val = 0, found_pov = 0;
    uintptr_t  found_sel = 0, found_grow = 0;
    size_t     found_woff = 0;
    int        match_count = 0;

    uintptr_t scan_s = (img.relro_start + 7u) & ~(uintptr_t)7u;
    uintptr_t scan_e = img.relro_end & ~(uintptr_t)7u;
    uintptr_t rx_lim128 = (img.rx_end > img.rx_start + 128u) ? (img.rx_end - 128u) : 0u;

    for (uintptr_t addr = scan_s; addr + 16u <= scan_e; addr += 8u) {
        uintptr_t p0 = *(const uintptr_t*)addr;
        uintptr_t p1 = *(const uintptr_t*)(addr + 8u);
        /* Avoid unsigned overflow on negative C++ vtable offset-to-top entries (e.g. -8)! */
        if (p0 < img.rx_start || p0 > rx_lim128 || (p0 & 3u) != 0) continue;
        if (p1 < img.rx_start || p1 > rx_lim128 || (p1 & 3u) != 0) continue;

        size_t w0 = 0, w1 = 0;
        uintptr_t s0 = 0, s1 = 0, g0 = 0, g1 = 0;
        if (match_composer_method(p0, img.rx_start, img.rx_end,
                                  HIDL_CMD_VALIDATE_DISPLAY_HI, &w0, &s0, &g0) &&
            match_composer_method(p1, img.rx_start, img.rx_end,
                                  HIDL_CMD_PRESENT_OR_VALIDATE_DISPLAY_HI, &w1, &s1, &g1) &&
            w0 == w1 && s0 == s1 && g0 == g1) {
            found_slot = (uintptr_t*)addr;
            found_val  = p0;
            found_pov  = p1;
            found_woff = w0;
            found_sel  = s0;
            found_grow = g0;
            match_count++;
        }
    }

    if (match_count != 1 || !found_slot) {
        LOGE("HidlComposer vtable scan failed (matches=%d, rx=%p..%p, relro=%p..%p)",
             match_count, (void*)img.rx_start, (void*)img.rx_end,
             (void*)img.relro_start, (void*)img.relro_end);
        return;
    }

    /* Locate HidlComposer::isSupported(OptionalFeature) in the same vtable (typically slot 2, -36 slots) */
    uintptr_t* found_issup_slot = NULL;
    uintptr_t  found_issup = 0;
    for (int back = 1; back <= 48; back++) {
        uintptr_t cand_addr = (uintptr_t)found_slot - (uintptr_t)back * 8u;
        if (cand_addr < scan_s) break;
        uintptr_t fn = *(const uintptr_t*)cand_addr;
        if (match_hidl_is_supported(fn, img.rx_start, img.rx_end)) {
            found_issup_slot = (uintptr_t*)cand_addr;
            found_issup = fn;
            break;
        }
    }

    long pagesz = sysconf(_SC_PAGESIZE);
    if (pagesz <= 0) pagesz = 4096;
    uintptr_t lo_addr = found_issup_slot ? (uintptr_t)found_issup_slot : (uintptr_t)found_slot;
    uintptr_t hi_addr = (uintptr_t)found_slot + 16u;
    uintptr_t pstart  = lo_addr & ~((uintptr_t)pagesz - 1u);
    uintptr_t pend    = (hi_addr + (uintptr_t)pagesz - 1u) & ~((uintptr_t)pagesz - 1u);
    size_t    plen    = (size_t)(pend - pstart);

    if (mprotect((void*)pstart, plen, PROT_READ | PROT_WRITE) != 0) {
        LOGE("mprotect(RW) on vtable page %p failed: errno=%d", (void*)pstart, errno);
        return;
    }

    g_writer_off     = found_woff;
    g_select_display = (fn_select_display_t)found_sel;
    g_grow_data      = (fn_grow_data_t)found_grow;
    g_orig_validate  = (fn_validate_display_t)found_val;
    g_orig_pov       = (fn_present_or_validate_t)found_pov;

    found_slot[0] = (uintptr_t)&neo9_validateDisplay;
    found_slot[1] = (uintptr_t)&neo9_presentOrValidateDisplay;
    if (found_issup_slot && found_issup) {
        g_orig_is_supported = (fn_is_supported_t)found_issup;
        found_issup_slot[0] = (uintptr_t)&neo9_isSupported;
    }
    __sync_synchronize();

    (void)mprotect((void*)pstart, plen, PROT_READ);

    LOGI("installed OriginOS 4 EPT bridge (slot=%p writer=+0x%zx select=%p grow=%p val=%p pov=%p isSup=%p@%p)",
         (void*)found_slot, g_writer_off, (void*)g_select_display, (void*)g_grow_data,
         (void*)g_orig_validate, (void*)g_orig_pov, (void*)found_issup, (void*)found_issup_slot);
}
