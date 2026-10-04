#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace xcast {

/**
 * UIBC (WFD spec §4.11): input from the phone back to the source.
 *
 * Windows negotiates the HIDC category: the sink presents itself as USB HID devices (a 5-finger
 * touch screen and a keyboard) by sending their report descriptors, then sends HID input reports.
 * The GENERIC category (coordinates in video pixels) is kept for sources that choose it.
 */
class UibcClient {
public:
    enum TouchType : uint8_t { kDown = 0, kUp = 1, kMove = 2 };

    ~UibcClient();
    bool connect(const std::string& ip, int port, bool hidc, int videoWidth, int videoHeight);
    void close();
    bool connected() const { return fd_ >= 0; }
    bool hidc() const { return hidc_; }
    // How finger input is presented to Windows (set before connect()).
    enum Mode : int { kTouchScreen = 0, kPointer = 1, kTouchpad = 2 };
    void setMode(int m) { mode_ = m < 0 || m > 2 ? kTouchScreen : Mode(m); }
    Mode mode() const { return mode_; }
    // Coordinates normalised to 0..1 over the video frame.
    void sendTouch(TouchType type, int count, const int* ids, const float* nx, const float* ny);
    void sendKey(bool down, uint16_t asciiCode);
    uint64_t sent() const { return sent_; }

private:
    struct Contact {
        int id = -1;
        uint16_t x = 0, y = 0;
        bool down = false;
    };
    static constexpr int kMaxContacts = 5;

    bool sendPacket(uint8_t category, const uint8_t* body, uint16_t len);
    void sendGeneric(uint8_t ieId, const uint8_t* describe, uint16_t len);
    void sendHidc(uint8_t hidType, uint8_t usage, const uint8_t* value, uint16_t len);
    void sendTouchReportLocked();

    std::atomic<int> fd_{-1};
    std::mutex mutex_;
    bool hidc_ = false;
    Mode mode_ = kTouchScreen;

    // Touchpad gesture state (relative mouse).
    void touchpadLocked(TouchType type, int count, const int* ids, const float* nx, const float* ny);
    void sendRelMouseLocked(uint8_t buttons, int dx, int dy, int wheel);
    int padIds_[10] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    int padActive_ = 0, padMaxFingers_ = 0, padPrimary_ = -1;
    float padLastX_ = 0, padLastY_ = 0, padScrollAcc_ = 0, padFracX_ = 0, padFracY_ = 0, padTravel_ = 0;
    float padStartX_ = 0, padStartY_ = 0, padScrollRefY_ = 0;
    bool padResync_ = false;     // finger set changed: re-anchor before computing deltas
    bool padScrollRef_ = false;  // padScrollRefY_ holds a valid average
    int64_t padDownUs_ = 0, padLastTapUs_ = 0;
    bool padDragging_ = false;
    int pointerPrimary_ = -1;
    int videoW_ = 0, videoH_ = 0;
    Contact contacts_[kMaxContacts];
    std::atomic<uint64_t> sent_{0};
};

}  // namespace xcast
