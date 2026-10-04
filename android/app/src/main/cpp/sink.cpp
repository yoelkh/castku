#include "sink.h"

#include <sys/system_properties.h>

#include <cstdio>
#include <cstring>

#include "common.h"

namespace xcast {

Sink::~Sink() {
    stop();
    setSurface(nullptr);
}

bool Sink::start(const SinkConfig& cfg) {
    stop();
    cfg_ = cfg;
    lastStatsUs_ = 0;

    if (cfg_.audio) audio_.start();

    bool ok = receiver_.start(
        cfg_.rtpPort,
        [this](const uint8_t* d, size_t n, int64_t pts, bool idr) {
            std::lock_guard<std::mutex> lock(decoderMutex_);
            if (decoderRunning_) decoder_.submit(d, n, pts, idr);
        },
        [this](const uint8_t* d, size_t n) { audio_.onPes(d, n); },
        [this]() { rtsp_.requestIdr(); });
    if (!ok) {
        audio_.stop();
        return false;
    }

    RtspClient::Callbacks cb;
    cb.onPlaying = [this]() { onEvent_(kEventPlaying, std::to_string(cfg_.width) + "x" + std::to_string(cfg_.height)); };
    cb.onEnded = [this](const std::string& reason) { onEvent_(kEventEnded, reason); };
    cb.onUibcEnabled = [this](int port, bool hidc) {
        uibc_.setMode(cfg_.touchMode);
        if (uibc_.connect(cfg_.sourceIp, port, hidc, cfg_.width, cfg_.height)) {
            onEvent_(kEventUibcOn, std::string(hidc ? "HIDC:" : "generic:") + std::to_string(port));
        }
    };
    cb.onTransport = [this](int rtcpPort, uint32_t) {
        if (cfg_.rtcp && rtcpPort > 0) {
            rtcp_.start(cfg_.rtpPort + 1, cfg_.sourceIp, rtcpPort, [this]() { return receiver_.reception(); });
        }
    };
    cb.onUibcDisabled = [this]() {
        uibc_.close();
        onEvent_(kEventUibcOff, "");
    };
    if (cfg_.cursor && !cursor_.start(cfg_.cursorPort, cfg_.width, cfg_.height)) cfg_.cursor = false;
    rtsp_.start(cfg_, cb);
    running_ = true;

    std::lock_guard<std::mutex> lock(decoderMutex_);
    if (window_ && !decoderRunning_) startDecoderLocked();
    return true;
}

void Sink::stop() {
    if (!running_) return;
    running_ = false;
    rtsp_.stop();
    rtcp_.stop();
    cursor_.stop();
    receiver_.stop();
    uibc_.close();
    audio_.stop();
    std::lock_guard<std::mutex> lock(decoderMutex_);
    if (decoderRunning_) {
        decoder_.stop();
        decoderRunning_ = false;
    }
}

void Sink::startDecoderLocked() {
    decoderRunning_ = decoder_.start(window_, cfg_.width, cfg_.height, cfg_.fps, [this]() { rtsp_.requestIdr(); });
    if (decoderRunning_) onEvent_(kEventDecoder, decoder_.codecName());
}

void Sink::setSurface(ANativeWindow* window) {
    std::lock_guard<std::mutex> lock(decoderMutex_);
    if (decoderRunning_) {
        decoder_.stop();
        decoderRunning_ = false;
    }
    if (window_) ANativeWindow_release(window_);
    window_ = window;
    if (window_ && running_) startDecoderLocked();
}

void Sink::touch(int action, int count, const int* ids, const float* nx, const float* ny) {
    uibc_.sendTouch(static_cast<UibcClient::TouchType>(action), count, ids, nx, ny);
}

void Sink::key(bool down, int code) { uibc_.sendKey(down, uint16_t(code)); }

std::string Sink::stats() {
    const StreamStats& s = receiver_.stats();
    int64_t now = nowUs();
    uint64_t frames = s.frames, bytes = s.bytes;
    double fps = 0, mbps = 0;
    if (lastStatsUs_) {
        double dt = (now - lastStatsUs_) / 1e6;
        fps = (frames - lastFrames_) / dt;
        mbps = (bytes - lastBytes_) * 8 / 1e6 / dt;
    }
    lastStatsUs_ = now;
    lastFrames_ = frames;
    lastBytes_ = bytes;
    char buf[256];
    snprintf(buf, sizeof(buf), "%dx%d  %.0f fps  %.1f Mbps  decode %.1f ms  lost %llu  drop %llu  %s%s",
             cfg_.width, cfg_.height, fps, mbps, decoder_.avgLatencyUs() / 1000.0, (unsigned long long)s.lost.load(),
             (unsigned long long)(s.droppedFrames.load() + decoder_.dropped()), decoder_.codecName().c_str(),
             uibc_.connected() ? (uibc_.hidc() ? (uibc_.mode() == UibcClient::kPointer ? "  touch:pointer" : uibc_.mode() == UibcClient::kTouchpad ? "  touch:touchpad" : "  touch:screen") : "  touch:generic") : "");
    std::string uibcInfo = "  uibc sent " + std::to_string(uibc_.sent());
    char extra[128];
    snprintf(extra, sizeof(extra), "  frames %llu  eof %llu  rtcp rr %llu sr %llu  cursor %llu/%llu",
             (unsigned long long)frames, (unsigned long long)s.markers.load(),
             (unsigned long long)rtcp_.reportsSent(), (unsigned long long)rtcp_.senderReports(),
             (unsigned long long)cursor_.positions(), (unsigned long long)cursor_.shapes());
    return std::string(buf) + '\n' + decoder_.debug() + extra + uibcInfo + '\n' + audio_.debug();
}

}  // namespace xcast
