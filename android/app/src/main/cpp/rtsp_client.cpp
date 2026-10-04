#include "rtsp_client.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <sstream>

#include "common.h"

namespace xcast {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::map<std::string, std::string> parseParams(const std::string& body) {
    std::map<std::string, std::string> out;
    std::istringstream in(body);
    std::string line;
    while (std::getline(in, line)) {
        size_t c = line.find(':');
        if (c == std::string::npos) continue;
        out[trim(line.substr(0, c))] = trim(line.substr(c + 1));
    }
    return out;
}

// WFD H.264 level bitmap: 0x10 = 4.2, 0x40 = 5.1, 0x80 = 5.2.
const char* levelFor(int w, int h, int fps) {
    long mbs = long((w + 15) / 16) * ((h + 15) / 16) * fps;
    if (mbs <= 522240) return "10";
    if (mbs <= 983040) return "40";
    return "80";
}

}  // namespace

int connectWithTimeout(const std::string& ip, int port, int timeoutMs) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);
    int r = connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (r != 0 && errno == EINPROGRESS) {
        pollfd pfd{fd, POLLOUT, 0};
        int err = 0;
        socklen_t len = sizeof(err);
        if (poll(&pfd, 1, timeoutMs) == 1 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0) {
            r = 0;
        }
    }
    if (r != 0) {
        close(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);  // blocking again for the reader thread
    return fd;
}

std::string RtspClient::Message::header(const std::string& lowerName) const {
    auto it = headers.find(lowerName);
    return it == headers.end() ? std::string() : it->second;
}

RtspClient::~RtspClient() { stop(); }

void RtspClient::start(const SinkConfig& cfg, Callbacks cb) {
    stop();
    cfg_ = cfg;
    cb_ = std::move(cb);
    // Every session starts clean; leftovers from a previous connection would corrupt this one.
    {
        std::lock_guard<std::mutex> lock(sendMutex_);
        cseq_ = 1;
        pending_.clear();
        session_.clear();
        lastIdrMs_ = 0;
    }
    rxBuf_.clear();
    presentationUrl_.clear();
    uibcEnabled_ = false;
    uibcPort_ = 0;
    uibcHidc_ = false;
    endedReported_ = false;
    running_ = true;
    thread_ = std::thread(&RtspClient::run, this);
}

void RtspClient::stop() {
    running_ = false;
    int fd = fd_.exchange(-1);
    if (fd >= 0) {
        shutdown(fd, SHUT_RDWR);
        close(fd);
    }
    if (thread_.joinable()) thread_.join();
}

bool RtspClient::connectSource() {
    // Bounded attempts with a 1 s connect timeout so stop() never waits long on a dead peer.
    for (int attempt = 0; attempt < 20 && running_; ++attempt) {
        int fd = connectWithTimeout(cfg_.sourceIp, cfg_.rtspPort, 1000);
        if (fd >= 0) {
            fd_ = fd;
            if (!running_) {  // stop() ran while we were connecting
                if (fd_.exchange(-1) >= 0) close(fd);
                return false;
            }
            return true;
        }
        usleep(200 * 1000);
    }
    return false;
}

void RtspClient::run() {
    std::string reason = "connection closed";
    if (!connectSource()) {
        reason = "cannot reach source " + cfg_.sourceIp;
    } else {
        LOGI("RTSP connected to %s:%d", cfg_.sourceIp.c_str(), cfg_.rtspPort);
        Message m;
        while (running_ && readMessage(m)) {
            handle(m);
            m = Message();
        }
    }
    if (running_ && !endedReported_ && cb_.onEnded) {
        endedReported_ = true;
        cb_.onEnded(reason);
    }
}

bool RtspClient::readMessage(Message& m) {
    char buf[4096];
    size_t headerEnd;
    while ((headerEnd = rxBuf_.find("\r\n\r\n")) == std::string::npos) {
        if (rxBuf_.size() > 65536) return false;  // no header terminator: not RTSP
        int fd = fd_;
        if (fd < 0) return false;
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) return false;
        rxBuf_.append(buf, n);
    }
    std::istringstream head(rxBuf_.substr(0, headerEnd));
    std::string line;
    bool first = true;
    while (std::getline(head, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (first) {
            m.startLine = line;
            if (!m.isResponse()) m.method = line.substr(0, line.find(' '));
            first = false;
            continue;
        }
        size_t c = line.find(':');
        if (c != std::string::npos) m.headers[lower(trim(line.substr(0, c)))] = trim(line.substr(c + 1));
    }
    size_t bodyLen = 0;
    std::string cl = m.header("content-length");
    if (!cl.empty()) bodyLen = strtoul(cl.c_str(), nullptr, 10);
    if (bodyLen > (1u << 20)) return false;
    size_t total = headerEnd + 4 + bodyLen;
    while (rxBuf_.size() < total) {
        int fd = fd_;
        if (fd < 0) return false;
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) return false;
        rxBuf_.append(buf, n);
    }
    m.body = rxBuf_.substr(headerEnd + 4, bodyLen);
    rxBuf_.erase(0, total);
    return true;
}

void RtspClient::handle(const Message& m) {
    if (m.isResponse()) {
        std::string s = m.header("session");
        Pending kind = Pending::Other;
        std::string session;
        {
            std::lock_guard<std::mutex> lock(sendMutex_);
            if (!s.empty()) session_ = trim(s.substr(0, s.find(';')));
            session = session_;
            auto it = pending_.find(std::atoi(m.header("cseq").c_str()));
            if (it != pending_.end()) {
                kind = it->second;
                pending_.erase(it);
            }
        }
        if (m.startLine.find(" 200") == std::string::npos) LOGW("RTSP error response: %s", m.startLine.c_str());
        if (kind == Pending::Setup) {
            handleTransport(m.header("transport"));
            request("PLAY " + url() + " RTSP/1.0", {}, {}, Pending::Play, true);  // M7
        } else if (kind == Pending::Play) {
            LOGI("RTSP PLAY ok, session %s", session.c_str());
            if (cb_.onPlaying) cb_.onPlaying();
        }
        return;
    }

    const std::string cseq = m.header("cseq");
    if (m.method == "OPTIONS") {  // M1
        reply(cseq, "Public: org.wfa.wfd1.0, GET_PARAMETER, SET_PARAMETER\r\n");
        request("OPTIONS * RTSP/1.0", "Require: org.wfa.wfd1.0\r\n", {}, Pending::Options);  // M2
    } else if (m.method == "GET_PARAMETER") {
        if (trim(m.body).empty()) {
            reply(cseq, {}, {}, true);  // M16 keep-alive
        } else {
            reply(cseq, "Content-Type: text/parameters\r\n", capabilityResponse(m.body));  // M3
        }
    } else if (m.method == "SET_PARAMETER") {
        reply(cseq, {}, {}, true);
        handleSetParameter(m);
    } else {
        reply(cseq, {}, {}, true);
    }
}

void RtspClient::handleTransport(const std::string& transport) {
    // e.g. RTP/AVP/UDP;unicast;client_port=19000-19001;server_port=55264-55265;ssrc=614f4929
    int rtcpPort = 0;
    uint32_t ssrc = 0;
    size_t sp = transport.find("server_port=");
    if (sp != std::string::npos) {
        size_t dash = transport.find('-', sp);
        size_t semi = transport.find(';', sp);
        if (dash != std::string::npos && (semi == std::string::npos || dash < semi)) {
            rtcpPort = std::atoi(transport.c_str() + dash + 1);
        }
    }
    size_t ss = transport.find(";ssrc=");
    if (ss != std::string::npos) ssrc = uint32_t(strtoul(transport.c_str() + ss + 6, nullptr, 16));
    LOGI("transport: source RTCP port %d, ssrc %08x", rtcpPort, ssrc);
    if (cb_.onTransport) cb_.onTransport(rtcpPort, ssrc);
}

void RtspClient::handleSetParameter(const Message& m) {
    auto p = parseParams(m.body);
    auto it = p.find("wfd_presentation_URL");
    if (it != p.end()) presentationUrl_ = it->second.substr(0, it->second.find(' '));

    for (const char* key : {"wfd_video_formats", "microsoft_custom_video_formats",
                            "microsoft_latency_management_capability", "microsoft_cursor", "wfd_uibc_capability",
                            "wfd_uibc_setting", "wfd_audio_codecs"}) {
        it = p.find(key);
        if (it != p.end()) LOGI("M4 %s: %s", key, it->second.c_str());
    }

    it = p.find("wfd_uibc_capability");
    if (it != p.end()) {
        size_t pos = it->second.find("port=");
        if (pos != std::string::npos) uibcPort_ = std::atoi(it->second.c_str() + pos + 5);
        size_t cat = it->second.find("input_category_list=");
        if (cat != std::string::npos) {
            std::string list = it->second.substr(cat, it->second.find(';', cat) - cat);
            uibcHidc_ = list.find("HIDC") != std::string::npos && list.find("GENERIC") == std::string::npos;
        }
    }
    auto setting = p.find("wfd_uibc_setting");
    if (setting != p.end()) {
        bool enable = lower(setting->second) == "enable";
        if (enable && !uibcEnabled_ && uibcPort_ > 0 && cb_.onUibcEnabled) cb_.onUibcEnabled(uibcPort_, uibcHidc_);
        if (!enable && uibcEnabled_ && cb_.onUibcDisabled) cb_.onUibcDisabled();
        uibcEnabled_ = enable && uibcPort_ > 0;
    } else if (p.count("wfd_uibc_capability") && uibcPort_ > 0 && !uibcEnabled_) {
        // Windows 11 sends the UIBC port without wfd_uibc_setting and its "Allow input" switch is
        // gone from the Cast panel; connecting and presenting our HID devices right away lets it
        // offer/enable input instead of waiting for a setting that never comes.
        LOGI("UIBC: port %d offered without wfd_uibc_setting; connecting now", uibcPort_);
        uibcEnabled_ = true;
        if (cb_.onUibcEnabled) cb_.onUibcEnabled(uibcPort_, uibcHidc_);
    }

    it = p.find("wfd_trigger_method");
    if (it != p.end()) {
        if (it->second == "SETUP") {  // M5 -> M6
            std::string ports = std::to_string(cfg_.rtpPort);
            if (cfg_.rtcp) ports += "-" + std::to_string(cfg_.rtpPort + 1);
            request("SETUP " + url() + " RTSP/1.0", "Transport: RTP/AVP/UDP;unicast;client_port=" + ports + "\r\n", {},
                    Pending::Setup);
        } else if (it->second == "TEARDOWN") {
            request("TEARDOWN " + url() + " RTSP/1.0", {}, {}, Pending::Teardown, true);
            if (!endedReported_ && cb_.onEnded) {
                endedReported_ = true;
                cb_.onEnded("teardown by source");
            }
        } else if (it->second == "PLAY") {
            request("PLAY " + url() + " RTSP/1.0", {}, {}, Pending::Play, true);
        }
    }
}

std::string RtspClient::capabilityResponse(const std::string& requestBody) const {
    std::string out;
    std::istringstream in(requestBody);
    std::string key;
    while (std::getline(in, key)) {
        key = trim(key);
        if (key.empty()) continue;
        std::string v = capability(key);
        if (!v.empty()) out += key + ": " + v + "\r\n";
    }
    return out;
}

std::string RtspClient::capability(const std::string& key) const {
    if (key == "wfd_video_formats") {
        // native=CEA 1080p60; CBP+CHP; CEA fallbacks 640x480p60, 720p60, 1080p60 only (all 60 fps).
        return std::string("40 00 03 ") + levelFor(cfg_.width, cfg_.height, cfg_.fps) +
               " 00000141 00000000 00000000 00 0000 0000 11 none none";
    }
    if (key == "microsoft_custom_video_formats") {
        char b[40];
        snprintf(b, sizeof(b), "%04X %04X %04X", cfg_.width, cfg_.height, cfg_.fps);
        return b;
    }
    if (key == "microsoft_cursor") {
        if (!cfg_.cursor) return "none";
        char b[40];  // no XOR blending, cursors up to 256x256, UDP port (MS-WDHCE 1.7)
        snprintf(b, sizeof(b), "none 0100 0100 %04X", cfg_.cursorPort);
        return b;
    }
    if (key == "microsoft_rtcp_capability") return cfg_.rtcp ? "supported" : "none";
    if (key == "wfd_audio_codecs") return cfg_.audio ? "LPCM 00000002 00" : "none";
    if (key == "wfd_client_rtp_ports") return "RTP/AVP/UDP;unicast " + std::to_string(cfg_.rtpPort) + " 0 mode=play";
    if (key == "wfd_uibc_capability") {
        // Windows' "Allow input" option depends on what it supports; offer both categories.
        return "input_category_list=GENERIC, HIDC;generic_cap_list=Mouse, SingleTouch, MultiTouch, Keyboard;"
               "hidc_cap_list=Keyboard/USB, Mouse/USB, MultiTouch/USB;port=none";
    }
    if (key == "wfd_idr_request_capability") return "1";
    if (key == "wfd_connector_type") return "05";
    if (key == "microsoft_latency_management_capability") return "supported";
    if (key == "microsoft_format_change_capability") return "supported";
    if (key == "intel_friendly_name") return cfg_.friendlyName;
    if (key == "intel_sink_manufacturer_name") return "CastKu";
    if (key == "intel_sink_model_name") return cfg_.modelName;
    if (key == "intel_sink_version") return "1.0";
    if (key == "wfd_content_protection" || key == "wfd_display_edid" || key == "wfd_coupled_sink" ||
        key == "wfd_standby_resume_capability" || key == "wfd_I2C" || key.rfind("microsoft_", 0) == 0) {
        return "none";
    }
    return {};  // unknown / WFD R2 parameters are omitted
}

void RtspClient::requestIdr() {
    int64_t now = nowMs();
    {
        std::lock_guard<std::mutex> lock(sendMutex_);
        if (now - lastIdrMs_ < 300 || session_.empty()) return;
        lastIdrMs_ = now;
    }
    request("SET_PARAMETER rtsp://localhost/wfd1.0 RTSP/1.0", "Content-Type: text/parameters\r\n",
            "wfd_idr_request\r\n", Pending::Idr, true);  // M13
}

void RtspClient::reply(const std::string& cseq, const std::string& headers, const std::string& body,
                       bool withSession) {
    std::lock_guard<std::mutex> lock(sendMutex_);
    std::string s = "RTSP/1.0 200 OK\r\nCSeq: " + cseq + "\r\n" + headers;
    if (withSession && !session_.empty()) s += "Session: " + session_ + "\r\n";
    if (!body.empty()) s += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    else s += "\r\n";
    sendRawLocked(s);
}

void RtspClient::request(const std::string& line, const std::string& headers, const std::string& body,
                         Pending kind, bool withSession) {
    std::lock_guard<std::mutex> lock(sendMutex_);
    int c = cseq_++;
    pending_[c] = kind;
    std::string s = line + "\r\nCSeq: " + std::to_string(c) + "\r\n" + headers;
    if (withSession && !session_.empty()) s += "Session: " + session_ + "\r\n";
    if (!body.empty()) s += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    else s += "\r\n";
    sendRawLocked(s);
}

void RtspClient::sendRawLocked(const std::string& s) {
    int fd = fd_;
    if (fd < 0) return;
    size_t off = 0;
    while (off < s.size()) {
        ssize_t n = send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
        if (n <= 0) return;
        off += n;
    }
}

std::string RtspClient::url() const {
    return presentationUrl_.empty() ? "rtsp://" + cfg_.sourceIp + "/wfd1.0/streamid=0" : presentationUrl_;
}

}  // namespace xcast
