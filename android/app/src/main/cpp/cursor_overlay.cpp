#include "cursor_overlay.h"

#include <android/hardware_buffer.h>
#include <android/imagedecoder.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "common.h"

namespace xcast {

namespace {
enum MsgType : uint8_t { kPosition = 1, kShape = 2, kShapeCont = 3 };
enum ImageType : uint8_t { kDisabled = 1, kMaskedColorPng = 2, kColorPng = 3 };

// MS-WDHCE does not state the byte order of its integers; we verify it per packet against the
// known packet size (see onPacket) and default to network order like the RTP header around it.
struct Reader {
    const uint8_t* p;
    bool le;
    uint16_t u16(size_t o) const { return le ? uint16_t(p[o] | p[o + 1] << 8) : uint16_t(p[o] << 8 | p[o + 1]); }
    int16_t s16(size_t o) const { return int16_t(u16(o)); }
    uint32_t u32(size_t o) const {
        return le ? uint32_t(p[o]) | uint32_t(p[o + 1]) << 8 | uint32_t(p[o + 2]) << 16 | uint32_t(p[o + 3]) << 24
                  : uint32_t(p[o]) << 24 | uint32_t(p[o + 1]) << 16 | uint32_t(p[o + 2]) << 8 | p[o + 3];
    }
};

bool decodePng(const std::vector<uint8_t>& png, std::vector<uint8_t>& out, int& w, int& h) {
    AImageDecoder* dec = nullptr;
    if (AImageDecoder_createFromBuffer(png.data(), png.size(), &dec) != ANDROID_IMAGE_DECODER_SUCCESS) return false;
    AImageDecoder_setAndroidBitmapFormat(dec, ANDROID_BITMAP_FORMAT_RGBA_8888);  // premultiplied by default
    const AImageDecoderHeaderInfo* info = AImageDecoder_getHeaderInfo(dec);
    w = AImageDecoderHeaderInfo_getWidth(info);
    h = AImageDecoderHeaderInfo_getHeight(info);
    size_t stride = AImageDecoder_getMinimumStride(dec);
    std::vector<uint8_t> tmp(stride * h);
    bool ok = AImageDecoder_decodeImage(dec, tmp.data(), stride, tmp.size()) == ANDROID_IMAGE_DECODER_SUCCESS;
    AImageDecoder_delete(dec);
    if (!ok || w <= 0 || h <= 0 || w > 512 || h > 512) return false;
    out.resize(size_t(w) * h * 4);
    for (int y = 0; y < h; ++y) memcpy(out.data() + size_t(y) * w * 4, tmp.data() + y * stride, size_t(w) * 4);
    return true;
}
}  // namespace

CursorOverlay::~CursorOverlay() {
    stop();
    setParent(nullptr, 0, 0);
}

bool CursorOverlay::start(int port, int videoWidth, int videoHeight) {
    stop();
    videoW_ = videoWidth;
    videoH_ = videoHeight;
    fd_ = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd_ < 0) return false;
    int one = 1;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        LOGE("cursor bind %d failed: %s", port, strerror(errno));
        close(fd_);
        fd_ = -1;
        return false;
    }
    pending_ = Shape();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        visible_ = false;
        rgba_.clear();
        imgW_ = imgH_ = 0;
        applyLocked();
    }
    running_ = true;
    thread_ = std::thread(&CursorOverlay::run, this);
    return true;
}

void CursorOverlay::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    visible_ = false;
    applyLocked();
}

void CursorOverlay::setParent(ASurfaceControl* parent, int viewWidth, int viewHeight) {
    std::lock_guard<std::mutex> lock(mutex_);
    viewW_ = viewWidth;
    viewH_ = viewHeight;
    if (parent == parent_) {
        if (parent) ASurfaceControl_release(parent);  // caller handed us a new reference
        applyLocked();
        return;
    }
    if (layer_) {
        ASurfaceTransaction* t = ASurfaceTransaction_create();
        ASurfaceTransaction_reparent(t, layer_, nullptr);
        ASurfaceTransaction_apply(t);
        ASurfaceTransaction_delete(t);
        ASurfaceControl_release(layer_);
        layer_ = nullptr;
    }
    if (parent_) ASurfaceControl_release(parent_);
    parent_ = parent;
    if (parent_) {
        layer_ = ASurfaceControl_create(parent_, "xcast-cursor");
        imageDirty_ = true;
        applyLocked();
    }
}

void CursorOverlay::run() {
    boostThread(-19);
    uint8_t buf[65536];
    pollfd pfd{fd_, POLLIN, 0};
    while (running_) {
        if (poll(&pfd, 1, 100) <= 0) continue;
        for (;;) {
            ssize_t n = recv(fd_, buf, sizeof(buf), MSG_DONTWAIT);
            if (n <= 0) break;
            onPacket(buf, size_t(n));
        }
    }
}

void CursorOverlay::onPacket(const uint8_t* p, size_t n) {
    if (n < 12 || (p[0] >> 6) != 2) return;
    size_t hdr = 12 + 4 * (p[0] & 0x0f);
    if (p[0] & 0x10) {
        if (n < hdr + 4) return;
        hdr += 4 + 4 * ((p[hdr + 2] << 8) | p[hdr + 3]);
    }
    if (n < hdr + 3) return;
    const uint8_t* m = p + hdr;
    size_t len = n - hdr;
    // PacketMsgSize must equal the bytes after the RTP header; that tells us the byte order.
    Reader r{m, false};
    if (r.u16(1) != len) {
        r.le = true;
        if (r.u16(1) != len) {
            static bool warned = false;
            if (!warned) LOGW("cursor packet size mismatch (type %u, %zu bytes)", m[0], len);
            warned = true;
            return;
        }
    }

    if (m[0] == kPosition && len >= 7) {
        positions_++;
        std::lock_guard<std::mutex> lock(mutex_);
        posX_ = r.s16(3);
        posY_ = r.s16(5);
        applyLocked();
    } else if (m[0] == kShape && len >= 18) {
        Shape s;
        s.total = r.u32(3);
        s.id = r.u16(7);
        s.x = r.s16(9);
        s.y = r.s16(11);
        s.type = m[13];
        size_t chunk = len - 18;
        if (s.type == kDisabled) {
            std::lock_guard<std::mutex> lock(mutex_);
            visible_ = false;
            applyLocked();
            return;
        }
        if (s.total == 0 || s.total > (1u << 20) || chunk > s.total) return;
        s.png.assign(s.total, 0);
        memcpy(s.png.data(), m + 18, chunk);
        s.received = uint32_t(chunk);
        pending_ = std::move(s);
        if (pending_.received == pending_.total) onShapeComplete(pending_);
    } else if (m[0] == kShapeCont && len >= 13) {
        uint16_t id = r.u16(7);
        uint32_t off = r.u32(9);
        size_t chunk = len - 13;
        if (id != pending_.id || pending_.png.empty() || off + chunk > pending_.png.size()) return;
        memcpy(pending_.png.data() + off, m + 13, chunk);
        pending_.received += uint32_t(chunk);
        if (pending_.received >= pending_.total) onShapeComplete(pending_);
    }
}

void CursorOverlay::onShapeComplete(Shape& s) {
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    if (!decodePng(s.png, rgba, w, h)) {
        LOGW("cursor PNG decode failed (type %d, %u bytes)", s.type, s.total);
        s.png.clear();
        return;
    }
    shapes_++;
    std::lock_guard<std::mutex> lock(mutex_);
    rgba_ = std::move(rgba);
    imgW_ = w;
    imgH_ = h;
    posX_ = s.x;
    posY_ = s.y;
    visible_ = true;
    imageDirty_ = true;
    applyLocked();
    s.png.clear();
}

void CursorOverlay::applyLocked() {
    if (!layer_) return;
    ASurfaceTransaction* t = ASurfaceTransaction_create();
    if (imageDirty_ && imgW_ > 0 && imgH_ > 0) {
        AHardwareBuffer_Desc desc{};
        desc.width = uint32_t(imgW_);
        desc.height = uint32_t(imgH_);
        desc.layers = 1;
        desc.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
        desc.usage = AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                     AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY;
        AHardwareBuffer* hb = nullptr;
        if (AHardwareBuffer_allocate(&desc, &hb) == 0) {
            AHardwareBuffer_describe(hb, &desc);
            void* dst = nullptr;
            if (AHardwareBuffer_lock(hb, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, nullptr, &dst) == 0) {
                for (int y = 0; y < imgH_; ++y) {
                    memcpy(static_cast<uint8_t*>(dst) + size_t(y) * desc.stride * 4,
                           rgba_.data() + size_t(y) * imgW_ * 4, size_t(imgW_) * 4);
                }
                AHardwareBuffer_unlock(hb, nullptr);
                ASurfaceTransaction_setBuffer(t, layer_, hb, -1);
                ASurfaceTransaction_setBufferTransparency(t, layer_, ASURFACE_TRANSACTION_TRANSPARENCY_TRANSLUCENT);
            }
            AHardwareBuffer_release(hb);  // the transaction holds its own reference
        }
        imageDirty_ = false;
    }
    // The parent is the SurfaceView's layer. With setFixedSize(video size) the SurfaceView already
    // scales that layer from video pixels to the view, and children inherit the transform, so the
    // pointer is positioned in video pixels and scales exactly like the video (measured on device:
    // applying view/video scale again overshot by that factor squared).
    ASurfaceTransaction_setPosition(t, layer_, posX_, posY_);
    ASurfaceTransaction_setZOrder(t, layer_, 1000);
    ASurfaceTransaction_setVisibility(t, layer_,
                                      visible_ && imgW_ > 0 ? ASURFACE_TRANSACTION_VISIBILITY_SHOW
                                                            : ASURFACE_TRANSACTION_VISIBILITY_HIDE);
    ASurfaceTransaction_apply(t);
    ASurfaceTransaction_delete(t);
}

}  // namespace xcast
