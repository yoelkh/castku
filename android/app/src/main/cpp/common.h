#pragma once

#include <android/log.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <cstdint>

#define XLOG_TAG "XCast"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, XLOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, XLOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, XLOG_TAG, __VA_ARGS__)

namespace xcast {

inline int64_t nowUs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

inline int64_t nowMs() { return nowUs() / 1000; }

// Latency-critical threads ask for a high (negative) nice value; failure is harmless.
inline void boostThread(int nice) {
    setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), nice);
}

}  // namespace xcast
