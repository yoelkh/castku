#include "video_decoder.h"

#include <media/NdkMediaFormat.h>

#include <cstdio>
#include <cstring>

#include "common.h"

namespace xcast {

namespace {
// A low-latency decoder frees an input buffer within a frame time; waiting longer means it is
// stuck or far behind, and resyncing on a fresh IDR is better than building up delay.
constexpr int64_t kInputTimeoutUs = 10000;
constexpr int64_t kOutputTimeoutUs = 20000;
}  // namespace

VideoDecoder::~VideoDecoder() { stop(); }

bool VideoDecoder::tryCreate(const char* name, ANativeWindow* window, int width, int height, int fps, bool extras) {
    AMediaCodec* codec = name[0] == '/' ? AMediaCodec_createDecoderByType("video/avc")
                                        : AMediaCodec_createCodecByName(name);
    if (!codec) return false;

    AMediaFormat* fmt = AMediaFormat_new();
    AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, width);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, height);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_LOW_LATENCY, 1);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_MAX_INPUT_SIZE, 4 << 20);
    if (extras) {
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_PRIORITY, 0);  // realtime
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_OPERATING_RATE, fps * 2);
    }

    media_status_t st = AMediaCodec_configure(codec, fmt, window, nullptr, 0);
    AMediaFormat_delete(fmt);
    if (st != AMEDIA_OK) {
        LOGW("decoder %s configure failed (%d)%s", name, st, extras ? ", retrying without extras" : "");
        AMediaCodec_delete(codec);
        return false;
    }
    if (AMediaCodec_start(codec) != AMEDIA_OK) {
        LOGW("decoder %s start failed", name);
        AMediaCodec_delete(codec);
        return false;
    }
    codec_ = codec;
    char* actual = nullptr;
    if (AMediaCodec_getName(codec, &actual) == AMEDIA_OK && actual) {
        codecName_ = actual;
        AMediaCodec_releaseName(codec, actual);
    } else {
        codecName_ = name;
    }
    return true;
}

bool VideoDecoder::start(ANativeWindow* window, int width, int height, int fps, std::function<void()> needIdr) {
    stop();
    needIdr_ = std::move(needIdr);
    waitIdr_ = true;
    const char* candidates[] = {"c2.mtk.avc.decoder.lowlatency", "c2.mtk.avc.decoder", "/by-type"};
    for (const char* name : candidates) {
        if (tryCreate(name, window, width, height, fps, true) || tryCreate(name, window, width, height, fps, false)) {
            break;
        }
    }
    if (!codec_) return false;
    alive_ = true;
    outputThread_ = std::thread(&VideoDecoder::outputLoop, this);
    LOGI("decoder %s started %dx%d@%d", codecName_.c_str(), width, height, fps);
    requestIdr();
    return true;
}

void VideoDecoder::stop() {
    if (!codec_) return;
    alive_ = false;
    if (outputThread_.joinable()) outputThread_.join();
    AMediaCodec_stop(codec_);
    AMediaCodec_delete(codec_);
    codec_ = nullptr;
}

void VideoDecoder::requestIdr() {
    waitIdr_ = true;
    if (needIdr_) needIdr_();
}

void VideoDecoder::submit(const uint8_t* data, size_t size, int64_t ptsUs, bool idr) {
    if (!alive_) return;
    int64_t arrival = nowUs();
    if (waitIdr_) {
        if (!idr) {
            dropped_++;
            return;
        }
        waitIdr_ = false;
    }
    ssize_t index = AMediaCodec_dequeueInputBuffer(codec_, kInputTimeoutUs);
    if (index < 0) {
        inputStalls_++;
        dropped_++;
        LOGW("decoder input stalled (%zd); resyncing on next IDR", index);
        requestIdr();
        return;
    }
    size_t cap = 0;
    uint8_t* buf = AMediaCodec_getInputBuffer(codec_, index, &cap);
    if (!buf || size > cap) {
        LOGW("input buffer too small (%zu > %zu)", size, cap);
        AMediaCodec_queueInputBuffer(codec_, index, 0, 0, 0, 0);
        dropped_++;
        requestIdr();
        return;
    }
    memcpy(buf, data, size);
    {
        std::lock_guard<std::mutex> lock(ringMutex_);
        ringPts_[ringPos_] = ptsUs;
        ringArrival_[ringPos_] = arrival;
        ringPos_ = (ringPos_ + 1) % kRing;
    }
    AMediaCodec_queueInputBuffer(codec_, index, 0, size, ptsUs < 0 ? 0 : ptsUs, 0);
    queued_++;
}

void VideoDecoder::outputLoop() {
    boostThread(-19);
    AMediaCodecBufferInfo info;
    while (alive_) {
        ssize_t index = AMediaCodec_dequeueOutputBuffer(codec_, &info, kOutputTimeoutUs);
        if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat* f = AMediaCodec_getOutputFormat(codec_);
            int32_t w = 0, h = 0;
            AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_WIDTH, &w);
            AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_HEIGHT, &h);
            AMediaFormat_delete(f);
            LOGI("decoder output format %dx%d", w, h);
            continue;
        }
        if (index < 0) continue;  // try-again-later / buffers changed
        AMediaCodec_releaseOutputBuffer(codec_, index, info.size > 0);
        if (info.size <= 0) continue;
        decoded_++;
        int64_t now = nowUs();
        std::lock_guard<std::mutex> lock(ringMutex_);
        for (int i = 0; i < kRing; ++i) {
            int k = (ringPos_ - 1 - i + kRing) % kRing;
            if (ringPts_[k] == info.presentationTimeUs && ringArrival_[k]) {
                int64_t lat = now - ringArrival_[k];
                ringArrival_[k] = 0;
                int64_t avg = avgLatencyUs_;
                avgLatencyUs_ = avg ? (avg * 15 + lat) / 16 : lat;
                break;
            }
        }
    }
}

std::string VideoDecoder::debug() const {
    char b[160];
    snprintf(b, sizeof(b), "queued %llu  decoded %llu  dropped %llu  stalls %llu",
             (unsigned long long)queued_.load(), (unsigned long long)decoded_.load(),
             (unsigned long long)dropped_.load(), (unsigned long long)inputStalls_.load());
    return b;
}

}  // namespace xcast
