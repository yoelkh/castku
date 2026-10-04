#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

#include "stream_receiver.h"

namespace xcast {

/**
 * RTCP receiver side (RFC 3550) for the MS-WFDPE microsoft_rtcp_capability extension: receives the
 * source's Sender Reports and sends Receiver Reports every 500 ms. Windows uses the loss and jitter
 * in them to modulate the encoder bitrate (higher when the link is clean, lower when it is not).
 */
class RtcpSession {
public:
    using ReceptionFn = std::function<RtpReception()>;

    ~RtcpSession();
    // localPort: our RTCP port (RTP port + 1); sourcePort: from the SETUP response's server_port.
    bool start(int localPort, const std::string& sourceIp, int sourcePort, ReceptionFn reception);
    void stop();
    uint64_t reportsSent() const { return reportsSent_; }
    uint64_t senderReports() const { return senderReports_; }

private:
    void run();
    void onPacket(const uint8_t* p, size_t n);
    void sendReport();

    int fd_ = -1;
    std::string sourceIp_;
    int sourcePort_ = 0;
    ReceptionFn reception_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    uint32_t ownSsrc_ = 0;

    // Last Sender Report: middle 32 bits of its NTP timestamp and when it arrived (for LSR/DLSR).
    uint32_t lastSr_ = 0;
    int64_t lastSrArrivalUs_ = 0;
    // Previous report, for the "fraction lost" interval computation.
    uint32_t prevExpected_ = 0;
    uint64_t prevReceived_ = 0;

    std::atomic<uint64_t> reportsSent_{0};
    std::atomic<uint64_t> senderReports_{0};
};

}  // namespace xcast
