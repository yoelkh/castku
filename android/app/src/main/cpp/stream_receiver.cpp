#include "stream_receiver.h"

#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>

#include "common.h"

namespace xcast {

namespace {

bool containsIdr(const uint8_t* d, size_t n) {
    for (size_t i = 0; i + 3 < n; ++i) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            uint8_t t = d[i + 3] & 0x1f;
            if (t == 5) return true;
            if (t == 1) return false;  // first slice is non-IDR
            i += 2;
        }
    }
    return false;
}

int64_t readPts(const uint8_t* b) {
    int64_t pts = (int64_t(b[0] >> 1) & 0x7) << 30;
    pts |= int64_t(b[1]) << 22;
    pts |= int64_t(b[2] >> 1) << 15;
    pts |= int64_t(b[3]) << 7;
    pts |= int64_t(b[4] >> 1);
    return pts * 100 / 9;  // 90 kHz -> us
}

}  // namespace

StreamReceiver::~StreamReceiver() { stop(); }

bool StreamReceiver::start(int port, VideoFn video, AudioFn audio, LossFn onLoss) {
    stop();
    video_ = std::move(video);
    audio_ = std::move(audio);
    onLoss_ = std::move(onLoss);

    fd_ = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd_ < 0) return false;
    int one = 1;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    int rcv = 8 << 20;
    setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv));
    int tos = 0xb8;  // DSCP EF (video, low latency)
    setsockopt(fd_, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        LOGE("RTP bind %d failed: %s", port, strerror(errno));
        close(fd_);
        fd_ = -1;
        return false;
    }
    lastSeq_ = -1;
    {
        std::lock_guard<std::mutex> lock(receptionMutex_);
        rx_ = RtpReception();
    }
    pmtPid_ = videoPid_ = audioPid_ = -1;
    video_pes_ = Pes();
    audio_pes_ = Pes();
    video_pes_.data.reserve(2 << 20);
    waitIdr_ = true;
    running_ = true;
    thread_ = std::thread(&StreamReceiver::run, this);
    return true;
}

void StreamReceiver::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
}

void StreamReceiver::run() {
    boostThread(-19);
    uint8_t buf[65536];
    pollfd pfd{fd_, POLLIN, 0};
    while (running_) {
        if (poll(&pfd, 1, 100) <= 0) continue;
        // Drain everything queued before returning to poll.
        for (;;) {
            ssize_t n = recv(fd_, buf, sizeof(buf), MSG_DONTWAIT);
            if (n <= 0) break;
            stats_.packets++;
            stats_.bytes += n;
            onDatagram(buf, size_t(n));
        }
    }
}

void StreamReceiver::onDatagram(const uint8_t* p, size_t n) {
    if (n < 12 || (p[0] >> 6) != 2) return;
    size_t hdr = 12 + 4 * (p[0] & 0x0f);
    if (p[0] & 0x10) {  // header extension
        if (n < hdr + 4) return;
        hdr += 4 + 4 * ((p[hdr + 2] << 8) | p[hdr + 3]);
    }
    if (n <= hdr) return;
    bool marker = p[1] & 0x80;  // Windows sets the RTP M-bit on the last packet of each frame
    if (marker) stats_.markers++;

    int seq = (p[2] << 8) | p[3];
    updateReception(uint32_t(p[8]) << 24 | uint32_t(p[9]) << 16 | uint32_t(p[10]) << 8 | p[11], uint16_t(seq),
                    uint32_t(p[4]) << 24 | uint32_t(p[5]) << 16 | uint32_t(p[6]) << 8 | p[7]);
    if (lastSeq_ >= 0) {
        int expected = (lastSeq_ + 1) & 0xffff;
        if (seq != expected) {
            int gap = (seq - expected) & 0xffff;
            if (gap < 0x8000) {  // real loss (ignore late duplicates)
                stats_.lost += gap;
                video_pes_.broken = true;
                waitIdr_ = true;
                if (onLoss_) onLoss_();
            } else {
                return;  // reordered old packet; its frame is already gone
            }
        }
    }
    lastSeq_ = seq;

    const uint8_t* ts = p + hdr;
    size_t len = n - hdr;
    for (size_t off = 0; off + 188 <= len; off += 188) {
        if (ts[off] != 0x47) continue;
        onTsPacket(ts + off);
    }
    // An unbounded video PES is known complete when the source flags the frame end.
    if (marker && video_pes_.active && video_pes_.expected == 0) finishPes(video_pes_, true);
}

void StreamReceiver::updateReception(uint32_t ssrc, uint16_t seq, uint32_t rtpTs) {
    std::lock_guard<std::mutex> lock(receptionMutex_);
    if (!rx_.valid || rx_.ssrc != ssrc) {  // first packet (or the source restarted its stream)
        rx_ = RtpReception();
        rx_.valid = true;
        rx_.ssrc = ssrc;
        rx_.baseSeq = seq;
        maxSeq_ = seq;
        cycles_ = 0;
        lastTransit_ = 0;
        jitter_ = 0;
    } else {
        uint16_t delta = uint16_t(seq - maxSeq_);
        if (delta < 0x8000) {  // in order (possibly after a gap)
            if (seq < maxSeq_) cycles_ += 1u << 16;
            maxSeq_ = seq;
        }
    }
    rx_.received++;
    rx_.extendedMaxSeq = cycles_ | maxSeq_;
    // RFC 3550 A.8: transit = arrival - timestamp, both in the 90 kHz RTP clock.
    int64_t transit = nowUs() * 9 / 100 - int64_t(rtpTs);
    if (lastTransit_ != 0) {
        double d = double(transit - lastTransit_);
        if (d < 0) d = -d;
        jitter_ += (d - jitter_) / 16.0;
    }
    lastTransit_ = transit;
    rx_.jitter = uint32_t(jitter_);
}

RtpReception StreamReceiver::reception() {
    std::lock_guard<std::mutex> lock(receptionMutex_);
    return rx_;
}

void StreamReceiver::onTsPacket(const uint8_t* p) {
    bool pusi = p[1] & 0x40;
    int pid = ((p[1] & 0x1f) << 8) | p[2];
    int afc = (p[3] >> 4) & 3;
    int cc = p[3] & 0x0f;
    size_t off = 4;
    if (afc & 2) off += 1 + p[4];
    if (!(afc & 1) || off >= 188) return;
    const uint8_t* payload = p + off;
    size_t n = 188 - off;

    if (pid == 0) {
        if (pusi) parsePat(payload + 1 + payload[0], n - 1 - payload[0]);
    } else if (pid == pmtPid_) {
        if (pusi) parsePmt(payload + 1 + payload[0], n - 1 - payload[0]);
    } else if (pid == videoPid_) {
        if (video_pes_.lastCc >= 0 && ((video_pes_.lastCc + 1) & 0xf) != cc) video_pes_.broken = true;
        video_pes_.lastCc = cc;
        onPesPayload(video_pes_, pid, payload, n, pusi, true);
    } else if (pid == audioPid_) {
        onPesPayload(audio_pes_, pid, payload, n, pusi, false);
    }
}

void StreamReceiver::parsePat(const uint8_t* s, size_t n) {
    if (n < 12 || s[0] != 0x00) return;
    size_t sectionLen = ((s[1] & 0x0f) << 8) | s[2];
    size_t end = std::min(n, 3 + sectionLen - 4);
    for (size_t i = 8; i + 4 <= end; i += 4) {
        int program = (s[i] << 8) | s[i + 1];
        if (program != 0) {
            pmtPid_ = ((s[i + 2] & 0x1f) << 8) | s[i + 3];
            return;
        }
    }
}

void StreamReceiver::parsePmt(const uint8_t* s, size_t n) {
    if (n < 16 || s[0] != 0x02) return;
    size_t sectionLen = ((s[1] & 0x0f) << 8) | s[2];
    size_t end = std::min(n, 3 + sectionLen - 4);
    size_t infoLen = ((s[10] & 0x0f) << 8) | s[11];
    for (size_t i = 12 + infoLen; i + 5 <= end;) {
        int type = s[i];
        int pid = ((s[i + 1] & 0x1f) << 8) | s[i + 2];
        size_t esInfo = ((s[i + 3] & 0x0f) << 8) | s[i + 4];
        if (type == 0x1b && videoPid_ != pid) {
            videoPid_ = pid;
            LOGI("TS video PID 0x%x (H.264)", pid);
        } else if (type == 0x83 && audioPid_ != pid) {
            audioPid_ = pid;
            LOGI("TS audio PID 0x%x (LPCM)", pid);
        }
        i += 5 + esInfo;
    }
}

void StreamReceiver::onPesPayload(Pes& pes, int pid, const uint8_t* payload, size_t n, bool pusi, bool isVideo) {
    if (pusi) {
        if (pes.active) finishPes(pes, isVideo);  // unbounded PES ends here
        if (n < 9 || payload[0] != 0 || payload[1] != 0 || payload[2] != 1) {
            pes.active = false;
            return;
        }
        size_t pesLen = (payload[4] << 8) | payload[5];
        size_t hdrLen = payload[8];
        if (9 + hdrLen > n || (pesLen != 0 && pesLen < 3 + hdrLen)) {
            pes.active = false;  // malformed header
            return;
        }
        pes.ptsUs = (payload[7] & 0x80) ? readPts(payload + 9) : -1;
        pes.expected = pesLen ? pesLen - 3 - hdrLen : 0;
        pes.data.clear();
        pes.broken = false;
        pes.active = true;
        payload += 9 + hdrLen;
        n -= 9 + hdrLen;
    }
    if (!pes.active) return;
    if (pes.expected && pes.data.size() + n > pes.expected) n = pes.expected - pes.data.size();
    pes.data.insert(pes.data.end(), payload, payload + n);
    if (pes.expected && pes.data.size() >= pes.expected) finishPes(pes, isVideo);
}

void StreamReceiver::finishPes(Pes& pes, bool isVideo) {
    pes.active = false;
    if (pes.data.empty()) return;
    if (!isVideo) {
        if (audio_) audio_(pes.data.data(), pes.data.size());
        return;
    }
    if (pes.broken) {
        stats_.droppedFrames++;
        waitIdr_ = true;
        if (onLoss_) onLoss_();
        return;
    }
    bool idr = containsIdr(pes.data.data(), pes.data.size());
    if (waitIdr_) {
        if (!idr) {
            stats_.droppedFrames++;
            if (onLoss_) onLoss_();  // ask for an IDR instead of waiting for the periodic one
            return;
        }
        waitIdr_ = false;
    }
    stats_.frames++;
    if (video_) video_(pes.data.data(), pes.data.size(), pes.ptsUs, idr);
}

}  // namespace xcast
