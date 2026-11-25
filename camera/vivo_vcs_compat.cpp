// A14 -> A17 ABI compatibility shim for OriginOS 4 vivocameraserver (libvivocameraservice.so)
// Bridges 4 exact C++ symbols changed between Android 14 (host) and Android 17 (GSI):
//   1. android::BufferItemConsumer::acquireBuffer(BufferItem*, int64_t, bool)
//      -> forwards to A17 5-arg overload with std::nullopt (engaged byte at offset 0x30 == 0)
//   2. android::BufferItemConsumer::releaseBuffer(const BufferItem&, const sp<Fence>&)
//      -> forwards to A17 4-arg overload with std::nullopt (engaged byte at offset 0x30 == 0)
//   3. android::binder::atrace_begin(uint64_t, const char*) -> no-op
//   4. android::binder::atrace_end(uint64_t) -> no-op

#include <cstdint>
#include <dlfcn.h>

namespace {

// Verified directly from A17 libgui.so disassembly:
//   acquireBuffer: ldrb w8, [x24, #0x30]; tbnz w8, #0x0, ...
//   releaseBuffer: ldrb w8, [x23, #0x30]; tbnz w8, #0x0, ...
// On AAPCS64, std::optional<std::function<void(const sp<GraphicBuffer>&)>> is >16 bytes
// and non-trivial, so it is passed indirectly by pointer in x4 (acquire) / x3 (release).
struct alignas(16) NullOptBuf {
    uint8_t bytes[64];
};

using AcquireA17Fn = int32_t (*)(void* self, void* item, int64_t presentWhen, bool waitForFence, const NullOptBuf* opt);
using ReleaseA17Fn = int32_t (*)(void* self, const void* item, const void* releaseFence, const NullOptBuf* opt);

constexpr const char* kAcquireA17Sym =
    "_ZN7android18BufferItemConsumer13acquireBufferEPNS_10BufferItemElbNSt3__18optionalINS3_8functionIFvRKNS_2spINS_13GraphicBufferEEEEEEEE";
constexpr const char* kReleaseA17Sym =
    "_ZN7android18BufferItemConsumer13releaseBufferERKNS_10BufferItemERKNS_2spINS_5FenceEEENSt3__18optionalINS9_8functionIFvRKNS4_INS_13GraphicBufferEEEEEEEE";

} // namespace

extern "C" {

// _ZN7android18BufferItemConsumer13acquireBufferEPNS_10BufferItemElb
__attribute__((visibility("default")))
int32_t _ZN7android18BufferItemConsumer13acquireBufferEPNS_10BufferItemElb(
    void* self, void* item, int64_t presentWhen, bool waitForFence) {
    static AcquireA17Fn fn = reinterpret_cast<AcquireA17Fn>(dlsym(RTLD_DEFAULT, kAcquireA17Sym));
    if (!fn) {
        fn = reinterpret_cast<AcquireA17Fn>(dlsym(RTLD_NEXT, kAcquireA17Sym));
    }
    if (!fn) return -38; // -ENOSYS
    NullOptBuf opt{};
    return fn(self, item, presentWhen, waitForFence, &opt);
}

// _ZN7android18BufferItemConsumer13releaseBufferERKNS_10BufferItemERKNS_2spINS_5FenceEEE
__attribute__((visibility("default")))
int32_t _ZN7android18BufferItemConsumer13releaseBufferERKNS_10BufferItemERKNS_2spINS_5FenceEEE(
    void* self, const void* item, const void* releaseFence) {
    static ReleaseA17Fn fn = reinterpret_cast<ReleaseA17Fn>(dlsym(RTLD_DEFAULT, kReleaseA17Sym));
    if (!fn) {
        fn = reinterpret_cast<ReleaseA17Fn>(dlsym(RTLD_NEXT, kReleaseA17Sym));
    }
    if (!fn) return -38; // -ENOSYS
    NullOptBuf opt{};
    return fn(self, item, releaseFence, &opt);
}

// _ZN7android6binder12atrace_beginEmPKc
__attribute__((visibility("default")))
void _ZN7android6binder12atrace_beginEmPKc(uint64_t /*tag*/, const char* /*name*/) {}

// _ZN7android6binder10atrace_endEm
__attribute__((visibility("default")))
void _ZN7android6binder10atrace_endEm(uint64_t /*tag*/) {}

} // extern "C"
