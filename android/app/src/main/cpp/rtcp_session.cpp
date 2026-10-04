#include "rtcp_session.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <random>

#include "common.h"

namespace xcast {

namespace {
constexpr int64_t kReportIntervalUs = 500000;
constexpr uint8_t kPtSr = 200, kPtRr = 201, kPtSdes = 202;

void put16(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 8);
    p[1] = uint8_t(v);
}

void put32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);
    p[3] = uint8_t(v);
}

uint32_t get32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
}  // namespace

RtcpSession::~RtcpSession() { stop(); }

bool RtcpSession::start(int localPort, const std::string& sourceIp, int sourcePort, ReceptionFn reception) {
    stop();
    fd_ = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd_ < 0) return false;
    int one = 1;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(localPort);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        LOGE("RTCP bind %d failed: %s", localPort, strerror(errno));
        close(fd_);
        fd_ = -1;
        return false;
    }
    sourceIp_ = sourceIp;
    sourcePort_ = sourcePort;
    reception_ = std::move(reception);
    ownSsrc_ = std::random_device{}();
    lastSr_ = 0;
    lastSrArrivalUs_ = 0;
    prevExpected_ = 0;
    prevReceived_ = 0;
    running_ = true;
    thread_ = std::thread(&RtcpSession::run, this);
    LOGI("RTCP started: local %d -> %s:%d", localPort, sourceIp.c_str(), sourcePort);
    return true;
}

void RtcpSession::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
}

void RtcpSession::run() {
    uint8_t buf[2048];
    pollfd pfd{fd_, POLLIN, 0};
    int64_t nextReport = nowUs() + kReportIntervalUs;
    while (running_) {
        int timeoutMs = int((nextReport - nowUs()) / 1000);
        if (timeoutMs < 0) timeoutMs = 0;
        if (timeoutMs > 100) timeoutMs = 100;  // stay responsive to stop()
        if (poll(&pfd, 1, timeoutMs) > 0) {
            ssize_t n = recv(fd_, buf, sizeof(buf), MSG_DONTWAIT);
            if (n > 0) onPacket(buf, size_t(n));
        }
        if (nowUs() >= nextReport) {
            sendReport();
            nextReport += kReportIntervalUs;
        }
    }
}

void RtcpSession::onPacket(const uint8_t* p, size_t n) {
    // Compound packet: walk each RTCP header.
    while (n >= 4 && (p[0] >> 6) == 2) {
        size_t len = (size_t(p[2]) << 8 | p[3]) * 4 + 4;
        if (len > n) break;
        if (p[1] == kPtSr && len >= 28) {
            // SR: header(4) ssrc(4) NTP msw(4) NTP lsw(4) ... ; LSR = middle 32 bits of NTP.
            lastSr_ = (get32(p + 8) << 16) | (get32(p + 12) >> 16);
            lastSrArrivalUs_ = nowUs();
            senderReports_++;
        }
        p += len;
        n -= len;
    }
}

void RtcpSession::sendReport() {
    RtpReception rx = reception_ ? reception_() : RtpReception();
    if (!rx.valid || sourcePort_ <= 0) return;

    uint8_t pkt[64];
    // Receiver Report with one report block (RFC 3550 6.4.2).
    pkt[0] = 0x81;  // V=2, P=0, RC=1
    pkt[1] = kPtRr;
    put16(pkt + 2, 7);  // length in 32-bit words minus one
    put32(pkt + 4, ownSsrc_);
    put32(pkt + 8, rx.ssrc);

    uint32_t expected = rx.extendedMaxSeq - rx.baseSeq + 1;
    int64_t lost = int64_t(expected) - int64_t(rx.received);
    if (lost < 0) lost = 0;
    if (lost > 0x7fffff) lost = 0x7fffff;
    uint32_t expectedInterval = expected - prevExpected_;
    uint64_t receivedInterval = rx.received - prevReceived_;
    prevExpected_ = expected;
    prevReceived_ = rx.received;
    uint8_t fraction = 0;
    if (expectedInterval > 0 && receivedInterval < expectedInterval) {
        fraction = uint8_t(((expectedInterval - receivedInterval) << 8) / expectedInterval);
    }
    pkt[12] = fraction;
    pkt[13] = uint8_t(lost >> 16);
    pkt[14] = uint8_t(lost >> 8);
    pkt[15] = uint8_t(lost);
    put32(pkt + 16, rx.extendedMaxSeq);
    put32(pkt + 20, rx.jitter);
    put32(pkt + 24, lastSr_);
    uint32_t dlsr = 0;  // delay since last SR, units of 1/65536 s
    if (lastSrArrivalUs_) dlsr = uint32_t((nowUs() - lastSrArrivalUs_) * 65536 / 1000000);
    put32(pkt + 28, dlsr);

    // SDES with a CNAME item: required in every compound RTCP packet.
    const char cname[] = "xcast";
    uint8_t* s = pkt + 32;
    s[0] = 0x81;  // V=2, SC=1
    s[1] = kPtSdes;
    put32(s + 4, ownSsrc_);
    s[8] = 1;  // CNAME
    s[9] = sizeof(cname) - 1;
    memcpy(s + 10, cname, sizeof(cname) - 1);
    size_t sdesLen = 10 + sizeof(cname) - 1;
    while (sdesLen % 4) s[sdesLen++] = 0;  // null item terminator + padding
    if (s[sdesLen - 1] != 0) {             // ensure at least one terminating zero
        put32(s + sdesLen, 0);
        sdesLen += 4;
    }
    put16(s + 2, uint32_t(sdesLen / 4 - 1));

    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(sourcePort_);
    inet_pton(AF_INET, sourceIp_.c_str(), &to.sin_addr);
    if (sendto(fd_, pkt, 32 + sdesLen, 0, reinterpret_cast<sockaddr*>(&to), sizeof(to)) > 0) reportsSent_++;
}

}  // namespace xcast
