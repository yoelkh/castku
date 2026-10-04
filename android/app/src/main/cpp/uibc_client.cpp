#include "uibc_client.h"

#include <sys/socket.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "common.h"
#include "rtsp_client.h"

namespace xcast {

namespace {

enum Category : uint8_t { kGeneric = 0, kHidc = 1 };
enum GenericIe : uint8_t { kTouchDown = 0, kTouchUp = 1, kTouchMove = 2, kKeyDown = 3, kKeyUp = 4 };
enum HidType : uint8_t { kHidKeyboard = 0, kHidMouse = 1, kHidMultiTouch = 3 };
constexpr float kPadSpeed = 1.6f;          // touchpad: pointer pixels per video pixel of finger travel
constexpr float kPadScrollStepPx = 40.f;   // touchpad: two-finger travel per wheel notch
constexpr int64_t kTapMaxUs = 250000;      // touchpad: a longer press is not a tap
constexpr float kTapMaxTravelPx = 12.f;    // touchpad: more movement is not a tap
constexpr int64_t kDoubleTapUs = 300000;   // touchpad: tap, then press within this = drag
enum HidUsage : uint8_t { kInputReport = 0, kReportDescriptor = 1 };
constexpr uint8_t kPathUsb = 0x01;
constexpr uint8_t kTouchReportId = 1;
constexpr uint8_t kTouchFeatureId = 2;
constexpr uint16_t kLogicalMax = 32767;

// Xiaomi 17T panel: 2756x1268 px at ~460 dpi -> about 15.2 cm x 7.0 cm (units of 0.01 cm).
constexpr uint16_t kPhysWidth = 1522, kPhysHeight = 700;

// Windows multi-touch screen, parallel mode: 5 finger collections + contact count (report id 1).
std::vector<uint8_t> touchDescriptor() {
    std::vector<uint8_t> d = {
        0x05, 0x0D,        // Usage Page (Digitizer)
        0x09, 0x04,        // Usage (Touch Screen)
        0xA1, 0x01,        // Collection (Application)
        0x85, kTouchReportId,
    };
    for (int i = 0; i < 5; ++i) {
        const uint8_t finger[] = {
            0x05, 0x0D,                    // Usage Page (Digitizer)
            0x09, 0x22,                    // Usage (Finger)
            0xA1, 0x02,                    // Collection (Logical)
            0x09, 0x42, 0x09, 0x32,        //   Usage (Tip Switch), Usage (In Range)
            0x15, 0x00, 0x25, 0x01,        //   Logical 0..1
            0x75, 0x01, 0x95, 0x02,        //   1 bit x 2
            0x81, 0x02,                    //   Input (Data,Var,Abs)
            0x95, 0x06, 0x81, 0x03,        //   6 bits padding
            0x09, 0x51,                    //   Usage (Contact Identifier)
            0x25, 0x0A, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02,
            0x05, 0x01,                    //   Usage Page (Generic Desktop)
            0x26, 0xFF, 0x7F,              //   Logical Max 32767
            0x75, 0x10, 0x95, 0x01,        //   16 bits x 1
            0x55, 0x0E, 0x65, 0x11,        //   Unit exponent -2, Unit cm
            0x35, 0x00,                    //   Physical Min 0
            0x46, uint8_t(kPhysWidth), uint8_t(kPhysWidth >> 8),
            0x09, 0x30, 0x81, 0x02,        //   Usage (X), Input
            0x46, uint8_t(kPhysHeight), uint8_t(kPhysHeight >> 8),
            0x09, 0x31, 0x81, 0x02,        //   Usage (Y), Input
            0x55, 0x00, 0x65, 0x00, 0x45, 0x00,  // reset unit/physical for the next items
            0xC0,                          // End Collection
        };
        d.insert(d.end(), finger, finger + sizeof(finger));
    }
    const uint8_t tail[] = {
        0x05, 0x0D, 0x09, 0x54,            // Usage (Contact Count)
        0x15, 0x00, 0x25, 0x0A, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02,
        // Contact Count Maximum feature (required by the Windows touch stack). UIBC is one-way, so
        // Windows can't GET_FEATURE from us; logical min == max == 5 makes the value constant.
        0x85, kTouchFeatureId,
        0x09, 0x55, 0x15, 0x05, 0x25, 0x05, 0x75, 0x08, 0x95, 0x01, 0xB1, 0x02,
        0xC0,                              // End Collection
    };
    d.insert(d.end(), tail, tail + sizeof(tail));
    return d;
}

// Absolute pointer: 3 buttons + X/Y 0..32767 (no report id). Touch maps to button 1.
const uint8_t kAbsMouseDescriptor[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01,             // Generic Desktop, Mouse, Application
    0x09, 0x01, 0xA1, 0x00,                         //   Pointer, Physical
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x03, 0x81, 0x02,  // buttons
    0x75, 0x05, 0x95, 0x01, 0x81, 0x03,             //   padding
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x00, 0x26, 0xFF, 0x7F, 0x75, 0x10, 0x95, 0x02,
    0x81, 0x02,                                     //   X, Y absolute
    0xC0, 0xC0,
};

// Relative mouse for touchpad mode: 3 buttons, X/Y int16, wheel int8 (no report id).
const uint8_t kRelMouseDescriptor[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01,
    0x09, 0x01, 0xA1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x03, 0x81, 0x02,
    0x75, 0x05, 0x95, 0x01, 0x81, 0x03,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x16, 0x01, 0x80, 0x26, 0xFF, 0x7F, 0x75, 0x10, 0x95, 0x02,
    0x81, 0x06,                                                              //   X, Y relative
    0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06,  //   wheel relative
    0xC0, 0xC0,
};

// Standard boot keyboard: modifiers byte, reserved byte, 6 key codes.
const uint8_t kKeyboardDescriptor[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01,             // Generic Desktop, Keyboard, Application
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,  // modifiers
    0x95, 0x01, 0x75, 0x08, 0x81, 0x03,             // reserved
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,  // keys
    0xC0,
};

// ASCII -> HID usage; bit 8 = needs Shift.
uint16_t asciiToHid(uint16_t c) {
    if (c >= 'a' && c <= 'z') return 0x04 + (c - 'a');
    if (c >= 'A' && c <= 'Z') return 0x100 | (0x04 + (c - 'A'));
    if (c >= '1' && c <= '9') return 0x1E + (c - '1');
    switch (c) {
        case '0': return 0x27;
        case 0x0D: case 0x0A: return 0x28;
        case 0x1B: return 0x29;
        case 0x08: return 0x2A;
        case 0x09: return 0x2B;
        case ' ': return 0x2C;
        case '-': return 0x2D; case '_': return 0x12D;
        case '=': return 0x2E; case '+': return 0x12E;
        case '[': return 0x2F; case '{': return 0x12F;
        case ']': return 0x30; case '}': return 0x130;
        case '\\': return 0x31; case '|': return 0x131;
        case ';': return 0x33; case ':': return 0x133;
        case '\'': return 0x34; case '"': return 0x134;
        case '`': return 0x35; case '~': return 0x135;
        case ',': return 0x36; case '<': return 0x136;
        case '.': return 0x37; case '>': return 0x137;
        case '/': return 0x38; case '?': return 0x138;
        case '!': return 0x11E; case '@': return 0x11F; case '#': return 0x120; case '$': return 0x121;
        case '%': return 0x122; case '^': return 0x123; case '&': return 0x124; case '*': return 0x125;
        case '(': return 0x126; case ')': return 0x127;
        default: return 0;
    }
}

}  // namespace

UibcClient::~UibcClient() { close(); }

bool UibcClient::connect(const std::string& ip, int port, bool hidc, int videoWidth, int videoHeight) {
    close();
    // Runs on the RTSP thread: bounded so a missing UIBC listener can't stall the control channel.
    // TCP_NODELAY is set by connectWithTimeout (Microsoft: Nagle adds visible input lag).
    int fd = connectWithTimeout(ip, port, 1500);
    if (fd < 0) {
        LOGW("UIBC connect %s:%d failed", ip.c_str(), port);
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        hidc_ = hidc;
        videoW_ = videoWidth;
        videoH_ = videoHeight;
        for (Contact& c : contacts_) c = Contact();
    }
    fd_ = fd;
    LOGI("UIBC connected to %s:%d (%s, %s)", ip.c_str(), port, hidc ? "HIDC" : "generic",
         mode_ == kPointer ? "pointer" : mode_ == kTouchpad ? "touchpad" : "touch screen");
    if (hidc) {
        for (int& id : padIds_) id = -1;
        padActive_ = padMaxFingers_ = 0;
        padPrimary_ = pointerPrimary_ = -1;
        padDragging_ = false;
        if (mode_ == kPointer) {
            sendHidc(kHidMouse, kReportDescriptor, kAbsMouseDescriptor, sizeof(kAbsMouseDescriptor));
        } else if (mode_ == kTouchpad) {
            sendHidc(kHidMouse, kReportDescriptor, kRelMouseDescriptor, sizeof(kRelMouseDescriptor));
        } else {
            std::vector<uint8_t> touch = touchDescriptor();
            sendHidc(kHidMultiTouch, kReportDescriptor, touch.data(), uint16_t(touch.size()));
            // Also push the Contact Count Maximum value as a report so the source side can cache it
            // (harmless if the source ignores it).
            const uint8_t ccm[] = {kTouchFeatureId, 5};
            sendHidc(kHidMultiTouch, kInputReport, ccm, sizeof(ccm));
        }
        sendHidc(kHidKeyboard, kReportDescriptor, kKeyboardDescriptor, sizeof(kKeyboardDescriptor));
    }
    return connected();
}

void UibcClient::close() {
    int fd = fd_.exchange(-1);
    if (fd >= 0) ::close(fd);
}

bool UibcClient::sendPacket(uint8_t category, const uint8_t* body, uint16_t len) {
    int fd = fd_;
    if (fd < 0) return false;
    std::vector<uint8_t> pkt(4 + len);
    uint16_t total = uint16_t(4 + len);
    pkt[0] = 0x00;      // version 0, no timestamp
    pkt[1] = category;  // input category
    pkt[2] = uint8_t(total >> 8);
    pkt[3] = uint8_t(total);
    memcpy(pkt.data() + 4, body, len);
    if (send(fd, pkt.data(), pkt.size(), MSG_NOSIGNAL) != ssize_t(pkt.size())) {
        LOGW("UIBC send failed, closing");
        close();
        return false;
    }
    if (++sent_ <= 6) {  // the descriptors and first reports, for protocol debugging
        char hex[3 * 48 + 1] = {};
        for (size_t i = 0; i < pkt.size() && i < 48; ++i) snprintf(hex + 3 * i, 4, "%02x ", pkt[i]);
        LOGI("UIBC tx #%llu (%zu bytes): %s", (unsigned long long)sent_.load(), pkt.size(), hex);
    }
    return true;
}

void UibcClient::sendGeneric(uint8_t ieId, const uint8_t* describe, uint16_t len) {
    std::vector<uint8_t> b(3 + len);
    b[0] = ieId;
    b[1] = uint8_t(len >> 8);
    b[2] = uint8_t(len);
    memcpy(b.data() + 3, describe, len);
    sendPacket(kGeneric, b.data(), uint16_t(b.size()));
}

void UibcClient::sendHidc(uint8_t hidType, uint8_t usage, const uint8_t* value, uint16_t len) {
    std::vector<uint8_t> b(5 + len);
    b[0] = kPathUsb;
    b[1] = hidType;
    b[2] = usage;
    b[3] = uint8_t(len >> 8);
    b[4] = uint8_t(len);
    memcpy(b.data() + 5, value, len);
    sendPacket(kHidc, b.data(), uint16_t(b.size()));
}

void UibcClient::sendTouch(TouchType type, int count, const int* ids, const float* nx, const float* ny) {
    if (!connected() || count <= 0) return;
    std::lock_guard<std::mutex> lock(mutex_);
    auto clamp01 = [](float v) { return v < 0 ? 0.f : v > 1 ? 1.f : v; };

    if (!hidc_) {
        int n = count > 10 ? 10 : count;
        uint8_t d[1 + 10 * 5];
        d[0] = uint8_t(n);
        for (int i = 0; i < n; ++i) {
            int x = int(clamp01(nx[i]) * (videoW_ - 1) + 0.5f), y = int(clamp01(ny[i]) * (videoH_ - 1) + 0.5f);
            uint8_t* p = d + 1 + i * 5;
            p[0] = uint8_t(ids[i]);
            p[1] = uint8_t(x >> 8);
            p[2] = uint8_t(x);
            p[3] = uint8_t(y >> 8);
            p[4] = uint8_t(y);
        }
        sendGeneric(type == kDown ? kTouchDown : type == kUp ? kTouchUp : kTouchMove, d, uint16_t(1 + n * 5));
        return;
    }

    if (mode_ == kTouchpad) {
        touchpadLocked(type, count, ids, nx, ny);
        return;
    }
    if (mode_ == kPointer) {
        // Only the first finger drives the pointer (a second finger must not make it jump).
        if (type == kDown && pointerPrimary_ < 0) pointerPrimary_ = ids[0];
        int i = 0;
        while (i < count && ids[i] != pointerPrimary_) ++i;
        if (i == count) return;
        uint16_t x = uint16_t(std::lround(clamp01(nx[i]) * kLogicalMax));
        uint16_t y = uint16_t(std::lround(clamp01(ny[i]) * kLogicalMax));
        bool up = type == kUp;
        uint8_t r[5] = {uint8_t(up ? 0x00 : 0x01), uint8_t(x), uint8_t(x >> 8), uint8_t(y), uint8_t(y >> 8)};
        sendHidc(kHidMouse, kInputReport, r, sizeof(r));
        if (up) pointerPrimary_ = -1;
        return;
    }

    // HIDC: keep the full contact state and report every active finger each time (parallel mode).
    for (int i = 0; i < count; ++i) {
        uint16_t x = uint16_t(std::lround(clamp01(nx[i]) * kLogicalMax));
        uint16_t y = uint16_t(std::lround(clamp01(ny[i]) * kLogicalMax));
        Contact* slot = nullptr;
        for (Contact& c : contacts_) {
            if (c.id == ids[i]) slot = &c;
        }
        if (!slot && type != kUp) {
            for (Contact& c : contacts_) {
                if (c.id < 0) {
                    slot = &c;
                    break;
                }
            }
        }
        if (!slot) continue;  // more than 5 fingers
        slot->id = ids[i];
        slot->x = x;
        slot->y = y;
        slot->down = type != kUp;
    }
    sendTouchReportLocked();
    for (Contact& c : contacts_) {
        if (c.id >= 0 && !c.down) c = Contact();  // lifted fingers were reported once with tip off
    }
}

void UibcClient::sendRelMouseLocked(uint8_t buttons, int dx, int dy, int wheel) {
    auto c16 = [](int v) { return v < -32767 ? -32767 : v > 32767 ? 32767 : v; };
    dx = c16(dx);
    dy = c16(dy);
    wheel = wheel < -127 ? -127 : wheel > 127 ? 127 : wheel;
    uint8_t r[6] = {buttons, uint8_t(dx), uint8_t(dx >> 8), uint8_t(dy), uint8_t(dy >> 8), uint8_t(int8_t(wheel))};
    sendHidc(kHidMouse, kInputReport, r, sizeof(r));
}

// Laptop-style touchpad: 1 finger moves, tap = left click, 2-finger tap = right click,
// 2-finger drag = scroll, tap then press-and-move = drag (left button held).
void UibcClient::touchpadLocked(TouchType type, int count, const int* ids, const float* nx, const float* ny) {
    int64_t now = nowUs();
    if (type == kDown) {
        for (int& id : padIds_) {
            if (id < 0) {
                id = ids[0];
                break;
            }
        }
        padActive_++;
        if (padActive_ == 1) {
            padPrimary_ = ids[0];
            padLastX_ = padStartX_ = nx[0];
            padLastY_ = padStartY_ = ny[0];
            padResync_ = false;
            padDownUs_ = now;
            padTravel_ = 0;
            padMaxFingers_ = 1;
            padScrollAcc_ = padFracX_ = padFracY_ = 0;
            padDragging_ = now - padLastTapUs_ < kDoubleTapUs;
            if (padDragging_) sendRelMouseLocked(0x01, 0, 0, 0);
        } else if (padActive_ > padMaxFingers_) {
            padMaxFingers_ = padActive_;
        }
        padScrollRef_ = false;
        return;
    }
    if (type == kMove) {
        int i = 0;
        while (i < count && ids[i] != padPrimary_) ++i;
        if (i == count) i = 0;
        if (padResync_) {  // primary finger changed: anchor on the new one, no movement this frame
            padLastX_ = nx[i];
            padLastY_ = ny[i];
            padResync_ = false;
            return;
        }
        float dxPx = (nx[i] - padLastX_) * videoW_, dyPx = (ny[i] - padLastY_) * videoH_;
        padLastX_ = nx[i];
        padLastY_ = ny[i];
        // Displacement from the touch-down point (summing per-frame jitter would kill taps).
        float travel = std::fabs(nx[i] - padStartX_) * videoW_ + std::fabs(ny[i] - padStartY_) * videoH_;
        if (travel > padTravel_) padTravel_ = travel;
        uint8_t held = padDragging_ ? 0x01 : 0x00;
        if (padActive_ >= 2) {
            float avg = 0;
            for (int k = 0; k < count; ++k) avg += ny[k];
            avg /= float(count);
            if (padScrollRef_) {
                padScrollAcc_ += (avg - padScrollRefY_) * videoH_;
                padTravel_ += std::fabs(avg - padScrollRefY_) * videoH_;  // scrolling is never a tap
            }
            padScrollRefY_ = avg;
            padScrollRef_ = true;
            int notches = int(padScrollAcc_ / kPadScrollStepPx);
            if (notches) {
                padScrollAcc_ -= notches * kPadScrollStepPx;
                sendRelMouseLocked(held, 0, 0, notches);  // content follows the fingers
            }
            return;
        }
        padFracX_ += dxPx * kPadSpeed;
        padFracY_ += dyPx * kPadSpeed;
        int mx = int(padFracX_), my = int(padFracY_);
        padFracX_ -= mx;
        padFracY_ -= my;
        if (mx || my) sendRelMouseLocked(held, mx, my, 0);
        return;
    }
    // kUp
    for (int& id : padIds_) {
        if (id == ids[0]) id = -1;
    }
    if (padActive_ > 0) padActive_--;
    padScrollRef_ = false;
    if (padActive_ > 0) {
        if (ids[0] == padPrimary_) {  // keep tracking with a remaining finger, re-anchored
            for (int id : padIds_) {
                if (id >= 0) padPrimary_ = id;
            }
            padResync_ = true;
        }
        return;
    }
    bool tap = now - padDownUs_ < kTapMaxUs && padTravel_ < kTapMaxTravelPx;
    if (padDragging_) {
        sendRelMouseLocked(0x00, 0, 0, 0);  // end drag
        padDragging_ = false;
        padLastTapUs_ = 0;
    } else if (tap) {
        uint8_t button = padMaxFingers_ >= 2 ? 0x02 : 0x01;
        sendRelMouseLocked(button, 0, 0, 0);
        sendRelMouseLocked(0x00, 0, 0, 0);
        padLastTapUs_ = button == 0x01 ? now : 0;
    }
    padPrimary_ = -1;
}

void UibcClient::sendTouchReportLocked() {
    uint8_t r[1 + kMaxContacts * 6 + 1] = {};
    r[0] = kTouchReportId;
    int n = 0;
    for (const Contact& c : contacts_) {
        if (c.id < 0) continue;
        uint8_t* f = r + 1 + n * 6;
        f[0] = c.down ? 0x03 : 0x00;  // tip switch | in range
        f[1] = uint8_t(c.id);
        f[2] = uint8_t(c.x);
        f[3] = uint8_t(c.x >> 8);
        f[4] = uint8_t(c.y);
        f[5] = uint8_t(c.y >> 8);
        ++n;
    }
    r[1 + kMaxContacts * 6] = uint8_t(n);
    sendHidc(kHidMultiTouch, kInputReport, r, sizeof(r));
}

void UibcClient::sendKey(bool down, uint16_t code) {
    if (!connected()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!hidc_) {
        uint8_t d[5] = {0, uint8_t(code >> 8), uint8_t(code), 0, 0};
        sendGeneric(down ? kKeyDown : kKeyUp, d, sizeof(d));
        return;
    }
    uint16_t hid = asciiToHid(code);
    if (!hid) return;
    uint8_t report[8] = {};
    if (down) {
        report[0] = (hid & 0x100) ? 0x02 : 0x00;  // Left Shift
        report[2] = uint8_t(hid);
    }
    sendHidc(kHidKeyboard, kInputReport, report, sizeof(report));
}

}  // namespace xcast
