/*
 * Minimal zero-dependency Android 17 JNI stub for com/qualcomm/ims/vt/ImsMedia.
 * Eliminates Android 13 libimsmedia_jni.so -> libbinder.so(@LIBBINDER) UnsatisfiedLinkError
 * while keeping VoLTE / VoNR voice calling (Qualcomm Hexagon DSP) 100% intact.
 */
#include <stdint.h>
#include <stddef.h>

#define JNI_VERSION_1_6 0x00010006
#define JNI_OK          0

typedef int32_t jint;
typedef uint8_t jboolean;
typedef void*   jobject;
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

/* Only the two JNIEnv vtable slots we need: FindClass (index 6) and RegisterNatives (index 215) */
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
static jint     stub_init(void) { return -1; } /* DPL_INIT_FAILURE: gracefully disables ViLTE media */
static jboolean stub_false(void) { return 0; }
static jobject  stub_null(void) { return NULL; }

static const JNINativeMethod g_methods[] = {
    { "nativeInit",                      "()I",                                      (void*)stub_init  },
    { "nativeDeInit",                    "()V",                                      (void*)stub_void  },
    { "nativeRegisterForMediaEvents",    "(Lcom/qualcomm/ims/vt/ImsMedia;)V",        (void*)stub_void  },
    { "nativeGetNegotiatedFPS",          "()I",                                      (void*)stub_int0  },
    { "nativeGetNegotiatedHeight",       "()I",                                      (void*)stub_int0  },
    { "nativeGetNegotiatedWidth",        "()I",                                      (void*)stub_int0  },
    { "nativeGetPeerHeight",             "()I",                                      (void*)stub_int0  },
    { "nativeGetPeerWidth",              "()I",                                      (void*)stub_int0  },
    { "nativeGetRecorderFrameRate",      "()I",                                      (void*)stub_int0  },
    { "nativeGetRecordingSurface",       "()Landroid/view/Surface;",                 (void*)stub_null  },
    { "nativeGetSurfaceHeight",          "(Landroid/view/Surface;)I",                (void*)stub_int0  },
    { "nativeGetSurfaceWidth",           "(Landroid/view/Surface;)I",                (void*)stub_int0  },
    { "nativeGetUIOrientationMode",      "()I",                                      (void*)stub_int0  },
    { "nativeGetVideoQualityIndication", "()I",                                      (void*)stub_int0  },
    { "nativeHandleRawFrame",            "([B)V",                                    (void*)stub_void  },
    { "nativeHaveSameParent",            "(Landroid/view/Surface;Landroid/view/Surface;)Z", (void*)stub_false },
    { "nativeRequestRtpDataUsage",       "(I)I",                                     (void*)stub_int0  },
    { "nativeSetCameraFacing",           "(I)V",                                     (void*)stub_void  },
    { "nativeSetCameraInfo",             "(II)V",                                    (void*)stub_void  },
    { "nativeSetDeviceOrientation",      "(I)V",                                     (void*)stub_void  },
    { "nativeSetSharedDisplayParameters","(II)I",                                    (void*)stub_int0  },
    { "nativeSetSurface",                "(Landroid/view/Surface;)I",                (void*)stub_int0  },
    { "nativeSetVideoImageBuffer",       "([III)I",                                  (void*)stub_int0  },
};

__attribute__((visibility("default")))
jint JNI_OnLoad(JavaVM* vm, void* reserved) {
    (void)reserved;
    JNIEnv* env = NULL;
    if ((*vm)->GetEnv(vm, (void**)&env, JNI_VERSION_1_6) != JNI_OK || !env) return -1;
    jclass cls = jni_find_class(env, "com/qualcomm/ims/vt/ImsMedia");
    if (cls) {
        jni_register_natives(env, cls, g_methods, (jint)(sizeof(g_methods) / sizeof(g_methods[0])));
    }
    return JNI_VERSION_1_6;
}
