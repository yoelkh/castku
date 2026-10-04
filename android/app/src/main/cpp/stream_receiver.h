#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace xcast {

struct StreamStats {
    std::atomic<uint64_t> packets{0};
    std::atomic<uint64_t> bytes{0};
    std::atomic<uint64_t> lost{0};
    std::atomic<uint64_t> frames{0};
    std::atomic<uint64_t> droppedFrames{0};
    std::atomic<uint64_t> markers{0};
};

/** RFC 3550 receiver-side counters, the inputs of an RTCP receiver report. */
struct RtpReception {
    uint32_t ssrc = 0;
    uint32_t baseSeq = 0;
    uint32_t extendedMaxSeq = 0;  // cycles << 16 | highest sequence number seen
    uint64_t received = 0;
    uint32_t jitter = 0;          // interarrival jitter in 90 kHz timestamp units
    bool valid = false;
};

/**
 * Receives RTP/UDP carrying MPEG-2 TS (RFC 2250), demuxes H.264 and LPCM, and hands complete
 * access units downstream the moment their last byte arrives (PES_packet_length is set by
 * Windows for nearly every frame, so no waiting for the next frame's start).
 */
class StreamReceiver {
public:
    using VideoFn = std::function<void(const uint8_t* data, size_t size, int64_t ptsUs, bool idr)>;
    using AudioFn = std::function<void(const uint8_t* data, size_t size)>;
    using LossFn = std::function<void()>;

    ~StreamReceiver();
    bool start(int port, VideoFn video, AudioFn audio, LossFn onLoss);
    void stop();
    const StreamStats& stats() const { return stats_; }
    RtpReception reception();

private:
    struct Pes {
        std::vector<uint8_t> data;
        size_t expected = 0;  // 0 = unbounded (ends at next PUSI / RTP marker)
        int64_t ptsUs = -1;
        bool active = false;
        bool broken = false;
        int lastCc = -1;
    };

    void run();
    void onDatagram(const uint8_t* p, size_t n);
    void onTsPacket(const uint8_t* p);
    void parsePat(const uint8_t* payload, size_t n);
    void parsePmt(const uint8_t* payload, size_t n);
    void onPesPayload(Pes& pes, int pid, const uint8_t* payload, size_t n, bool pusi, bool isVideo);
    void finishPes(Pes& pes, bool isVideo);

    VideoFn video_;
    AudioFn audio_;
    LossFn onLoss_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    int fd_ = -1;
    StreamStats stats_;

    int lastSeq_ = -1;
    int pmtPid_ = -1;
    int videoPid_ = -1;
    int audioPid_ = -1;
    Pes video_pes_;
    Pes audio_pes_;
    bool waitIdr_ = true;  // after loss (or at start) drop P-frames until the next IDR

    void updateReception(uint32_t ssrc, uint16_t seq, uint32_t rtpTs);
    std::mutex receptionMutex_;
    RtpReception rx_;
    uint16_t maxSeq_ = 0;
    uint32_t cycles_ = 0;
    int64_t lastTransit_ = 0;
    double jitter_ = 0;
};

}  // namespace xcast
