#include "audio_player.h"

#include <cstdio>
#include <cstring>

#include "common.h"

namespace xcast {

AudioPlayer::~AudioPlayer() { stop(); }

bool AudioPlayer::start() {
    stop();
    writePos_ = 0;
    readPos_ = 0;
    exclusive_ = true;
    return open(true) || open(exclusive_ = false);
}

bool AudioPlayer::open(bool exclusive) {
    closeStream();
    AAudioStreamBuilder* b = nullptr;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return false;
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSampleRate(b, kRate);
    AAudioStreamBuilder_setChannelCount(b, kChannels);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(b, exclusive ? AAUDIO_SHARING_MODE_EXCLUSIVE : AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_MEDIA);
    AAudioStreamBuilder_setContentType(b, AAUDIO_CONTENT_TYPE_MOVIE);
    AAudioStreamBuilder_setDataCallback(b, onData, this);
    AAudioStreamBuilder_setErrorCallback(b, onError, this);
    aaudio_result_t r = AAudioStreamBuilder_openStream(b, &stream_);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK) {
        LOGW("AAudio open (%s) failed: %s", exclusive ? "exclusive" : "shared", AAudio_convertResultToText(r));
        stream_ = nullptr;
        return false;
    }
    burst_ = AAudioStream_getFramesPerBurst(stream_);
    AAudioStream_setBufferSizeInFrames(stream_, burst_ * 2);  // double buffering: lowest safe latency
    needReopen_ = false;
    if (AAudioStream_requestStart(stream_) != AAUDIO_OK) {
        closeStream();
        return false;
    }
    LOGI("audio started (%s, callback): burst %d, buffer %d", exclusive ? "exclusive" : "shared", burst_.load(),
         AAudioStream_getBufferSizeInFrames(stream_));
    return true;
}

void AudioPlayer::closeStream() {
    if (!stream_) return;
    AAudioStream_requestStop(stream_);
    AAudioStream_close(stream_);
    stream_ = nullptr;
}

void AudioPlayer::stop() { closeStream(); }

// AAudio thread: only flag it; the stream is rebuilt from the producer thread.
void AudioPlayer::onError(AAudioStream*, void* self, aaudio_result_t error) {
    LOGW("AAudio error: %s", AAudio_convertResultToText(error));
    static_cast<AudioPlayer*>(self)->needReopen_ = true;
}

// Real-time audio thread: no locks, no allocation.
aaudio_data_callback_result_t AudioPlayer::onData(AAudioStream*, void* p, void* audio, int32_t frames) {
    auto* self = static_cast<AudioPlayer*>(p);
    auto* out = static_cast<int16_t*>(audio);
    uint64_t r = self->readPos_.load(std::memory_order_acquire);
    uint64_t w = self->writePos_.load(std::memory_order_acquire);
    uint64_t avail = w - r;
    int32_t n = avail < uint64_t(frames) ? int32_t(avail) : frames;
    for (int32_t i = 0; i < n; ++i) {
        size_t idx = size_t((r + i) & (kRingFrames - 1)) * kChannels;
        out[2 * i] = self->ring_[idx];
        out[2 * i + 1] = self->ring_[idx + 1];
    }
    if (n < frames) {
        memset(out + 2 * n, 0, size_t(frames - n) * kChannels * sizeof(int16_t));  // silence on underrun
        if (n > 0) self->underruns_.fetch_add(1, std::memory_order_relaxed);   // count only mid-stream gaps
    }
    // CAS so a concurrent trim by the producer (see onPes) is never undone.
    uint64_t expected = r;
    self->readPos_.compare_exchange_strong(expected, r + n, std::memory_order_acq_rel);
    self->played_.fetch_add(uint64_t(n), std::memory_order_relaxed);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void AudioPlayer::onPes(const uint8_t* data, size_t size) {
    pes_++;
    if (pes_ == 1) {
        LOGI("first audio PES: %zu bytes, header %02x %02x %02x %02x", size, size > 0 ? data[0] : 0,
             size > 1 ? data[1] : 0, size > 2 ? data[2] : 0, size > 3 ? data[3] : 0);
    }
    if (size <= 4 || data[0] != 0xa0) {
        badHeader_++;
        return;
    }
    if (needReopen_) {
        reopens_++;
        if (!open(exclusive_)) open(exclusive_ = false);
    }
    if (!stream_) return;
    data += 4;
    size -= 4;
    size_t frames = size / (2 * kChannels);

    uint64_t w = writePos_.load(std::memory_order_relaxed);
    uint64_t r = readPos_.load(std::memory_order_acquire);
    // Keep audio in step with video: if the backlog would exceed ~60 ms, skip the oldest samples.
    // The ring is far larger than the target, so the slots being written never overlap the ones
    // the callback may still be reading.
    if (w - r + frames > kTargetFrames) {
        uint64_t skip = w - r + frames - kTargetFrames;
        if (readPos_.compare_exchange_strong(r, r + skip, std::memory_order_acq_rel)) trimmed_ += skip;
    }
    for (size_t i = 0; i < frames; ++i) {
        size_t idx = size_t((w + i) & (kRingFrames - 1)) * kChannels;
        const uint8_t* s = data + i * 4;
        ring_[idx] = int16_t((s[0] << 8) | s[1]);  // big-endian -> native
        ring_[idx + 1] = int16_t((s[2] << 8) | s[3]);
    }
    writePos_.store(w + frames, std::memory_order_release);
    written_ += frames;
}

std::string AudioPlayer::debug() const {
    char b[180];
    snprintf(b, sizeof(b), "audio pes %llu bad %llu in %llu out %llu trim %llu underrun %llu reopen %llu burst %d %s",
             (unsigned long long)pes_.load(), (unsigned long long)badHeader_.load(),
             (unsigned long long)written_.load(), (unsigned long long)played_.load(),
             (unsigned long long)trimmed_.load(), (unsigned long long)underruns_.load(),
             (unsigned long long)reopens_.load(), burst_.load(),
             stream_ ? (exclusive_ ? "excl" : "shared") : "no-stream");
    return b;
}

}  // namespace xcast
