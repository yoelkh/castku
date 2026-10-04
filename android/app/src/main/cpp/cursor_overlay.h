#pragma once

#include <android/surface_control.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace xcast {

/**
 * Hardware cursor for the MS-WDHCE extension. Windows sends the pointer position and shape (PNG)
 * over UDP instead of drawing them into the video, and we compose the pointer ourselves on a child
 * SurfaceControl layer above the video. Position updates go straight from the network thread to
 * SurfaceFlinger, so the pointer skips the encode/decode pipeline (Microsoft: >100 ms -> <30 ms).
 */
class CursorOverlay {
public:
    ~CursorOverlay();
    bool start(int port, int videoWidth, int videoHeight);
    void stop();
    // parent: the SurfaceView's SurfaceControl (null to detach); view size in pixels.
    void setParent(ASurfaceControl* parent, int viewWidth, int viewHeight);
    uint64_t positions() const { return positions_; }
    uint64_t shapes() const { return shapes_; }

private:
    struct Shape {
        uint16_t id = 0;
        uint32_t total = 0;
        uint32_t received = 0;
        std::vector<uint8_t> png;
        int type = 0;
        int x = 0, y = 0;
    };

    void run();
    void onPacket(const uint8_t* p, size_t n);
    void onShapeComplete(Shape& s);
    void applyLocked();  // pushes current image/position/visibility to SurfaceFlinger

    int fd_ = -1;
    std::thread thread_;
    std::atomic<bool> running_{false};
    int videoW_ = 0, videoH_ = 0;
    Shape pending_;

    std::mutex mutex_;  // guards everything below (network thread vs. UI thread)
    ASurfaceControl* parent_ = nullptr;
    ASurfaceControl* layer_ = nullptr;
    int viewW_ = 0, viewH_ = 0;
    std::vector<uint8_t> rgba_;  // premultiplied RGBA of the current shape
    int imgW_ = 0, imgH_ = 0;
    bool imageDirty_ = false;
    bool visible_ = false;
    int posX_ = 0, posY_ = 0;  // top-left of the pointer image, in video pixels

    std::atomic<uint64_t> positions_{0};
    std::atomic<uint64_t> shapes_{0};
};

}  // namespace xcast
