#pragma once

#include <android/native_window.h>

#include <functional>
#include <mutex>
#include <string>

#include "audio_player.h"
#include "cursor_overlay.h"
#include "rtcp_session.h"
#include "rtsp_client.h"
#include "stream_receiver.h"
#include "uibc_client.h"
#include "video_decoder.h"

namespace xcast {

enum SinkEvent {
    kEventPlaying = 1,
    kEventEnded = 2,
    kEventUibcOn = 3,
    kEventUibcOff = 4,
    kEventDecoder = 5,
};

/** One Miracast session: RTSP control + RTP media + decoder/renderer + UIBC input back-channel. */
class Sink {
public:
    using EventFn = std::function<void(int type, const std::string& msg)>;

    explicit Sink(EventFn onEvent) : onEvent_(std::move(onEvent)) {}
    ~Sink();

    bool start(const SinkConfig& cfg);
    void stop();
    void setSurface(ANativeWindow* window);  // nullptr detaches (session keeps running)
    void touch(int action, int count, const int* ids, const float* nx, const float* ny);
    void key(bool down, int code);
    // Takes ownership of parent (null detaches).
    void setCursorParent(ASurfaceControl* parent, int viewWidth, int viewHeight) {
        cursor_.setParent(parent, viewWidth, viewHeight);
    }
    std::string stats();

private:
    void startDecoderLocked();

    EventFn onEvent_;
    SinkConfig cfg_;
    RtspClient rtsp_;
    StreamReceiver receiver_;
    VideoDecoder decoder_;
    AudioPlayer audio_;
    UibcClient uibc_;
    RtcpSession rtcp_;
    CursorOverlay cursor_;

    std::mutex decoderMutex_;
    ANativeWindow* window_ = nullptr;
    bool decoderRunning_ = false;
    bool running_ = false;

    int64_t lastStatsUs_ = 0;
    uint64_t lastFrames_ = 0;
    uint64_t lastBytes_ = 0;
};

}  // namespace xcast
