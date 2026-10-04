#pragma once

#include <android/native_window.h>
#include <media/NdkMediaCodec.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace xcast {

/**
 * Hardware H.264 decoder driven synchronously.
 *
 * submit() runs on the RTP receiver thread and copies each complete frame straight into a codec
 * input buffer, so frames are queued strictly in arrival order with no intermediate buffering.
 * A dedicated output thread releases decoded frames to the Surface the instant they appear, so
 * display happens on the very next vsync.
 *
 * Async NDK callbacks are deliberately not used: codec calls made from inside them wait on the same
 * looper that delivers the callbacks and deadlock.
 */
class VideoDecoder {
public:
    ~VideoDecoder();
    bool start(ANativeWindow* window, int width, int height, int fps, std::function<void()> needIdr);
    void stop();
    // Not thread-safe with start()/stop(); the caller serialises them (see Sink::decoderMutex_).
    void submit(const uint8_t* data, size_t size, int64_t ptsUs, bool idr);

    std::string codecName() const { return codecName_; }
    uint64_t decoded() const { return decoded_; }
    uint64_t dropped() const { return dropped_; }
    // Mean time from "last byte received" to "frame released for display", microseconds.
    int64_t avgLatencyUs() const { return avgLatencyUs_; }
    std::string debug() const;

private:
    bool tryCreate(const char* name, ANativeWindow* window, int width, int height, int fps, bool extras);
    void outputLoop();
    void requestIdr();

    AMediaCodec* codec_ = nullptr;
    std::string codecName_;
    std::function<void()> needIdr_;
    std::atomic<bool> alive_{false};
    std::thread outputThread_;
    bool waitIdr_ = true;  // only touched by the submitting thread

    std::mutex ringMutex_;
    static constexpr int kRing = 128;
    int64_t ringPts_[kRing] = {};
    int64_t ringArrival_[kRing] = {};
    int ringPos_ = 0;

    std::atomic<uint64_t> queued_{0};
    std::atomic<uint64_t> decoded_{0};
    std::atomic<uint64_t> dropped_{0};
    std::atomic<uint64_t> inputStalls_{0};
    std::atomic<int64_t> avgLatencyUs_{0};
};

}  // namespace xcast
