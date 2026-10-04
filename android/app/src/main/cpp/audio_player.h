#pragma once

#include <aaudio/AAudio.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace xcast {

/**
 * WFD LPCM (48 kHz, stereo, 16-bit big-endian, 4-byte 0xA0 header) to a low-latency AAudio stream.
 *
 * Uses AAudio's data callback (pull model) fed from a lock-free single-producer/single-consumer
 * ring: only callback streams get Android's FAST mixer path (small bursts). The blocking write()
 * path was denied FAST on this device (43 ms bursts) and never started because its start threshold
 * exceeded our latency cap.
 */
class AudioPlayer {
public:
    ~AudioPlayer();
    bool start();
    void stop();
    void onPes(const uint8_t* data, size_t size);  // producer: RTP receiver thread
    std::string debug() const;

private:
    static constexpr int32_t kRate = 48000;
    static constexpr int32_t kChannels = 2;
    static constexpr size_t kRingFrames = 16384;                // ~340 ms, power of two
    static constexpr size_t kTargetFrames = kRate * 60 / 1000;  // keep at most ~60 ms buffered

    bool open(bool exclusive);
    void closeStream();
    static aaudio_data_callback_result_t onData(AAudioStream* s, void* self, void* audio, int32_t frames);
    static void onError(AAudioStream* s, void* self, aaudio_result_t error);

    AAudioStream* stream_ = nullptr;
    bool exclusive_ = true;
    std::atomic<bool> needReopen_{false};

    std::vector<int16_t> ring_ = std::vector<int16_t>(kRingFrames * kChannels);
    std::atomic<uint64_t> writePos_{0};  // in frames, producer-owned
    std::atomic<uint64_t> readPos_{0};   // in frames, consumer-owned

    std::atomic<uint64_t> pes_{0}, badHeader_{0}, written_{0}, played_{0}, trimmed_{0}, underruns_{0}, reopens_{0};
    std::atomic<int32_t> burst_{0};
};

}  // namespace xcast
