#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace xcast {

struct SinkConfig {
    std::string sourceIp;
    int rtspPort = 7236;
    int rtpPort = 19000;  // RTCP uses rtpPort + 1
    int cursorPort = 19002;
    int width = 2176;
    int height = 1000;
    int fps = 60;
    bool audio = true;
    bool rtcp = false;    // advertise microsoft_rtcp_capability (receiver reports -> adaptive bitrate)
    bool cursor = false;  // advertise microsoft_cursor (hardware cursor over UDP)
    int touchMode = 0;    // UibcClient::Mode: 0 touch screen, 1 pointer follows finger, 2 touchpad
    std::string friendlyName = "Android phone";
    std::string modelName = "2602DPT53G";
};

/**
 * Wi-Fi Display sink side of the RTSP control channel (WFD M1..M16 + MS-WFDPE extensions).
 * The source (Windows) is the RTSP server; we connect to it and answer its requests.
 */
class RtspClient {
public:
    struct Callbacks {
        std::function<void()> onPlaying;
        std::function<void(int port, bool hidc)> onUibcEnabled;  // hidc: Windows chose the HIDC category
        std::function<void()> onUibcDisabled;
        // Source ports and SSRC from the SETUP response (RTCP port is 0 if the source gave none).
        std::function<void(int rtcpPort, uint32_t ssrc)> onTransport;
        std::function<void(const std::string& reason)> onEnded;
    };

    ~RtspClient();
    void start(const SinkConfig& cfg, Callbacks cb);
    void stop();
    void requestIdr();

private:
    struct Message {
        std::string startLine;
        std::string method;
        std::map<std::string, std::string> headers;  // lower-case keys
        std::string body;
        bool isResponse() const { return startLine.rfind("RTSP/", 0) == 0; }
        std::string header(const std::string& lowerName) const;
    };
    enum class Pending { Options, Setup, Play, Idr, Teardown, Other };

    void run();
    bool connectSource();
    bool readMessage(Message& m);
    void handle(const Message& m);
    void handleSetParameter(const Message& m);
    void handleTransport(const std::string& transport);
    std::string capabilityResponse(const std::string& requestBody) const;
    std::string capability(const std::string& key) const;
    // Both take sendMutex_; withSession appends the Session header read under that lock.
    void reply(const std::string& cseq, const std::string& headers, const std::string& body = {},
               bool withSession = false);
    void request(const std::string& line, const std::string& headers, const std::string& body, Pending kind,
                 bool withSession = false);
    void sendRawLocked(const std::string& s);
    std::string url() const;

    SinkConfig cfg_;
    Callbacks cb_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<int> fd_{-1};

    std::mutex sendMutex_;  // guards socket writes, cseq_, pending_, session_, lastIdrMs_
    int cseq_ = 1;
    std::map<int, Pending> pending_;
    std::string session_;
    int64_t lastIdrMs_ = 0;

    // Only touched by the RTSP thread.
    std::string rxBuf_;
    std::string presentationUrl_;
    bool uibcEnabled_ = false;
    int uibcPort_ = 0;
    bool uibcHidc_ = false;
    bool endedReported_ = false;
};

// Non-blocking connect bounded by timeoutMs; returns a blocking, TCP_NODELAY socket or -1.
int connectWithTimeout(const std::string& ip, int port, int timeoutMs);

}  // namespace xcast
