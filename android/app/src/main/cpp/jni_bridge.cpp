#include <android/native_window_jni.h>
#include <android/surface_control_jni.h>
#include <jni.h>
#include <pthread.h>

#include <memory>

#include "common.h"
#include "sink.h"

using xcast::Sink;
using xcast::SinkConfig;

namespace {

JavaVM* gVm = nullptr;
pthread_key_t gDetachKey;

// Native worker threads that call back into Java are attached lazily and detached on exit.
JNIEnv* attachedEnv() {
    JNIEnv* env = nullptr;
    if (gVm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) return env;
    if (gVm->AttachCurrentThread(&env, nullptr) != JNI_OK) return nullptr;
    pthread_setspecific(gDetachKey, env);
    return env;
}

struct Handle {
    jobject javaSink = nullptr;  // global ref
    jmethodID onEvent = nullptr;
    std::unique_ptr<Sink> sink;
};

std::string toString(JNIEnv* env, jstring s) {
    if (!s) return {};
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string out(c);
    env->ReleaseStringUTFChars(s, c);
    return out;
}

Handle* fromLong(jlong h) { return reinterpret_cast<Handle*>(h); }

}  // namespace

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
    gVm = vm;
    pthread_key_create(&gDetachKey, [](void*) { gVm->DetachCurrentThread(); });
    return JNI_VERSION_1_6;
}

extern "C" JNIEXPORT jlong JNICALL Java_com_xcast_display_NativeSink_nativeCreate(JNIEnv* env, jobject thiz) {
    auto* h = new Handle();
    h->javaSink = env->NewGlobalRef(thiz);
    h->onEvent = env->GetMethodID(env->GetObjectClass(thiz), "onNativeEvent", "(ILjava/lang/String;)V");
    h->sink = std::make_unique<Sink>([h](int type, const std::string& msg) {
        JNIEnv* e = attachedEnv();
        if (!e) return;
        jstring js = e->NewStringUTF(msg.c_str());
        e->CallVoidMethod(h->javaSink, h->onEvent, type, js);
        e->DeleteLocalRef(js);
        if (e->ExceptionCheck()) e->ExceptionClear();
    });
    return reinterpret_cast<jlong>(h);
}

extern "C" JNIEXPORT jboolean JNICALL Java_com_xcast_display_NativeSink_nativeStart(
    JNIEnv* env, jobject, jlong handle, jstring sourceIp, jint rtspPort, jint width, jint height, jint fps,
    jboolean audio, jstring name, jstring model, jint touchMode) {
    SinkConfig cfg;
    cfg.sourceIp = toString(env, sourceIp);
    cfg.rtspPort = rtspPort;
    cfg.width = width;
    cfg.height = height;
    cfg.fps = fps;
    cfg.audio = audio;
    cfg.rtcp = true;
    cfg.cursor = true;
    cfg.touchMode = touchMode;  // Windows sends the pointer out of band; we compose it (low-latency mouse)  // receiver reports let Windows raise the bitrate on a clean link
    cfg.friendlyName = toString(env, name);
    cfg.modelName = toString(env, model);
    return fromLong(handle)->sink->start(cfg);
}

extern "C" JNIEXPORT void JNICALL Java_com_xcast_display_NativeSink_nativeStop(JNIEnv*, jobject, jlong handle) {
    fromLong(handle)->sink->stop();
}

extern "C" JNIEXPORT void JNICALL Java_com_xcast_display_NativeSink_nativeSetSurface(JNIEnv* env, jobject, jlong handle,
                                                                                      jobject surface) {
    ANativeWindow* w = surface ? ANativeWindow_fromSurface(env, surface) : nullptr;
    fromLong(handle)->sink->setSurface(w);
}

extern "C" JNIEXPORT void JNICALL Java_com_xcast_display_NativeSink_nativeTouch(JNIEnv* env, jobject, jlong handle,
                                                                                 jint action, jint count, jintArray ids,
                                                                                 jfloatArray xs, jfloatArray ys) {
    jint idBuf[10];
    jfloat xBuf[10], yBuf[10];
    if (count > 10) count = 10;
    env->GetIntArrayRegion(ids, 0, count, idBuf);
    env->GetFloatArrayRegion(xs, 0, count, xBuf);
    env->GetFloatArrayRegion(ys, 0, count, yBuf);
    fromLong(handle)->sink->touch(action, count, idBuf, xBuf, yBuf);
}

extern "C" JNIEXPORT void JNICALL Java_com_xcast_display_NativeSink_nativeKey(JNIEnv*, jobject, jlong handle,
                                                                               jboolean down, jint code) {
    fromLong(handle)->sink->key(down, code);
}

extern "C" JNIEXPORT jstring JNICALL Java_com_xcast_display_NativeSink_nativeStats(JNIEnv* env, jobject,
                                                                                    jlong handle) {
    return env->NewStringUTF(fromLong(handle)->sink->stats().c_str());
}

extern "C" JNIEXPORT void JNICALL Java_com_xcast_display_NativeSink_nativeDestroy(JNIEnv* env, jobject, jlong handle) {
    Handle* h = fromLong(handle);
    h->sink.reset();
    env->DeleteGlobalRef(h->javaSink);
    delete h;
}

extern "C" JNIEXPORT void JNICALL Java_com_xcast_display_NativeSink_nativeSetCursorParent(JNIEnv* env, jobject,
                                                                                          jlong handle, jobject sc,
                                                                                          jint w, jint h) {
    ASurfaceControl* parent = nullptr;
    if (sc) {
        if (__builtin_available(android 34, *)) parent = ASurfaceControl_fromJava(env, sc);
    }
    fromLong(handle)->sink->setCursorParent(parent, w, h);
}
