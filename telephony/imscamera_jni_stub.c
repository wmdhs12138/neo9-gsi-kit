/*
 * Minimal zero-dependency Android 17 JNI stub for com/qualcomm/ims/vt/ImsCamera.
 */
#include <stdint.h>
#include <stddef.h>

#define JNI_VERSION_1_6 0x00010006
#define JNI_OK          0

typedef int16_t jshort;
typedef int32_t jint;
typedef uint8_t jboolean;
typedef void*   jclass;

typedef struct {
    const char* name;
    const char* signature;
    void*       fnPtr;
} JNINativeMethod;

struct JNINativeInterface_;
typedef const struct JNINativeInterface_* JNIEnv;

struct JNIInvokeInterface_ {
    void* reserved0;
    void* reserved1;
    void* reserved2;
    jint (*DestroyJavaVM)(void*);
    jint (*AttachCurrentThread)(void*, JNIEnv**, void*);
    jint (*DetachCurrentThread)(void*);
    jint (*GetEnv)(void*, void**, jint);
    jint (*AttachCurrentThreadAsDaemon)(void*, JNIEnv**, void*);
};
typedef const struct JNIInvokeInterface_* JavaVM;

static jclass jni_find_class(JNIEnv* env, const char* name) {
    typedef jclass (*fn_t)(JNIEnv*, const char*);
    fn_t fn = ((const fn_t*)(*env))[6];
    return fn(env, name);
}

static jint jni_register_natives(JNIEnv* env, jclass clazz, const JNINativeMethod* methods, jint nMethods) {
    typedef jint (*fn_t)(JNIEnv*, jclass, const JNINativeMethod*, jint);
    fn_t fn = ((const fn_t*)(*env))[215];
    return fn(env, clazz, methods, nMethods);
}

static void     stub_void(void) {}
static jint     stub_int0(void) { return 0; }
static jshort   stub_short_err(void) { return -1; }
static jboolean stub_false(void) { return 0; }

static const JNINativeMethod g_methods[] = {
    { "native_open",                  "(I)S",                       (void*)stub_short_err },
    { "native_open",                  "(ILjava/lang/String;)S",     (void*)stub_short_err },
    { "native_getMaxZoom",            "()I",                        (void*)stub_int0      },
    { "native_isZoomSupported",       "()Z",                        (void*)stub_false     },
    { "native_release",               "()S",                        (void*)stub_short_err },
    { "native_setDisplayOrientation", "(I)S",                       (void*)stub_short_err },
    { "native_setPreviewFpsRange",    "(S)S",                       (void*)stub_short_err },
    { "native_setPreviewSize",        "(II)S",                      (void*)stub_short_err },
    { "native_setPreviewTexture",     "(Landroid/view/Surface;)S",  (void*)stub_short_err },
    { "native_setZoom",               "(I)V",                       (void*)stub_void      },
    { "native_startPreview",          "()S",                        (void*)stub_short_err },
    { "native_startRecording",        "()S",                        (void*)stub_short_err },
    { "native_stopPreview",           "()S",                        (void*)stub_short_err },
    { "native_stopRecording",         "()S",                        (void*)stub_short_err },
};

__attribute__((visibility("default")))
jint JNI_OnLoad(JavaVM* vm, void* reserved) {
    (void)reserved;
    JNIEnv* env = NULL;
    if ((*vm)->GetEnv(vm, (void**)&env, JNI_VERSION_1_6) != JNI_OK || !env) return -1;
    jclass cls = jni_find_class(env, "com/qualcomm/ims/vt/ImsCamera");
    if (cls) {
        jni_register_natives(env, cls, g_methods, (jint)(sizeof(g_methods) / sizeof(g_methods[0])));
    }
    return JNI_VERSION_1_6;
}
