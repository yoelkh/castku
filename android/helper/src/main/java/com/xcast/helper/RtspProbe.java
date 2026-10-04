package com.xcast.helper;

import java.io.BufferedInputStream;
import java.io.ByteArrayOutputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.util.LinkedHashMap;
import java.util.Map;

/**
 * Debug-only WFD sink handshake (M1..M7 + keepalive) that logs everything the source says and
 * counts / dumps the RTP-MPEG-TS stream. Used to validate the protocol before the native sink.
 */
final class RtspProbe {
    static final int RTP_PORT = 19000;

    private final String sourceIp;
    private final int port;
    private final String localIp;
    private Socket socket;
    private OutputStream out;
    private int cseq = 1;
    private String presentationUrl;
    private String session;
    private volatile boolean running = true;
    private DatagramSocket rtpSocket;

    RtspProbe(String sourceIp, int port, String localIp) {
        this.sourceIp = sourceIp;
        this.port = port;
        this.localIp = localIp;
    }

    void start() {
        new Thread(this::run, "rtsp-probe").start();
    }

    void stop() {
        running = false;
        try {
            if (rtpSocket != null) rtpSocket.close();
            if (socket != null) socket.close();
        } catch (IOException ignored) {
        }
    }

    private static void log(String s) {
        System.out.println("PROBE " + s.replace("\r", "").replace("\n", "\n      "));
        System.out.flush();
    }

    private void run() {
        startRtpCounter();
        for (int attempt = 0; attempt < 20 && running; attempt++) {
            try {
                socket = new Socket();
                socket.setTcpNoDelay(true);
                socket.connect(new InetSocketAddress(sourceIp, port), 3000);
                break;
            } catch (IOException e) {
                log("connect attempt " + attempt + " failed: " + e);
                sleep(500);
            }
        }
        if (socket == null || !socket.isConnected()) return;
        log("connected to " + sourceIp + ":" + port);
        try {
            out = socket.getOutputStream();
            InputStream in = new BufferedInputStream(socket.getInputStream());
            while (running) {
                Msg m = Msg.read(in);
                if (m == null) break;
                log("<<< " + m.raw());
                handle(m);
            }
        } catch (IOException e) {
            log("rtsp closed: " + e);
        }
        log("rtsp session ended");
    }

    private void handle(Msg m) throws IOException {
        if (m.isResponse()) {
            if (m.header("Session") != null) session = m.header("Session").split(";")[0].trim();
            String c = m.header("CSeq");
            Kind kind = c != null ? pending.remove(Integer.parseInt(c.trim())) : null;
            if (kind == Kind.SETUP) sendPlay();
            return;
        }
        String cs = m.header("CSeq");
        switch (m.method) {
            case "OPTIONS":
                reply(cs, "Public: org.wfa.wfd1.0, GET_PARAMETER, SET_PARAMETER\r\n", null);
                sendRequest("OPTIONS * RTSP/1.0", "Require: org.wfa.wfd1.0\r\n", null, Kind.OPTIONS);
                break;
            case "GET_PARAMETER":
                if (m.body.isEmpty()) {
                    reply(cs, session != null ? "Session: " + session + "\r\n" : "", null); // keepalive M16
                } else {
                    reply(cs, "Content-Type: text/parameters\r\n", capabilities(m.body));
                }
                break;
            case "SET_PARAMETER":
                Map<String, String> p = parseParams(m.body);
                if (p.containsKey("wfd_presentation_URL")) {
                    presentationUrl = p.get("wfd_presentation_URL").split(" ")[0];
                }
                reply(cs, "", null);
                if ("SETUP".equals(p.get("wfd_trigger_method"))) sendSetup();
                else if ("TEARDOWN".equals(p.get("wfd_trigger_method"))) {
                    sendRequest("TEARDOWN " + presentationUrl + " RTSP/1.0", sessionHeader(), null, Kind.OTHER);
                }
                break;
            default:
                reply(cs, "", null);
        }
    }

    /** Answers only the parameters requested in M3. */
    private String capabilities(String body) {
        StringBuilder sb = new StringBuilder();
        for (String line : body.split("\r?\n")) {
            String k = line.trim();
            if (k.isEmpty()) continue;
            String v = capability(k);
            if (v != null) sb.append(k).append(": ").append(v).append("\r\n");
            else log("unknown M3 parameter: " + k);
        }
        return sb.toString();
    }

    private String capability(String key) {
        switch (key) {
            case "wfd_video_formats":
                // native 1080p60 (CEA idx 8); CBP+CHP, level 4.2; CEA/VESA/HH all ≤ 1080p60
                return "40 00 03 10 00000141 00000000 00000000 00 0000 0000 11 none none";
            case "wfd_audio_codecs":
                return "LPCM 00000002 00, AAC 00000001 00";
            case "wfd_client_rtp_ports":
                return "RTP/AVP/UDP;unicast " + RTP_PORT + " 0 mode=play";
            case "wfd_content_protection":
            case "wfd_display_edid":
            case "wfd_coupled_sink":
            case "wfd_standby_resume_capability":
            case "wfd_I2C":
                return "none";
            case "wfd_connector_type":
                return "05";
            case "wfd_uibc_capability":
                return "input_category_list=GENERIC;generic_cap_list=Mouse, SingleTouch, MultiTouch, Keyboard;"
                        + "hidc_cap_list=none;port=none";
            case "microsoft_latency_management_capability":
                return "supported";
            case "microsoft_format_change_capability":
                return "supported";
            case "microsoft_custom_video_formats":
                return "0880 03E8 003C";
            case "wfd_idr_request_capability":
                return "1";
            case "microsoft_diagnostics_capability":
            case "microsoft_rtcp_capability":
            case "microsoft_cursor":
            case "microsoft_video_formats":
            case "microsoft_max_bitrate":
            case "microsoft_multiscreen_projection":
            case "microsoft_audio_mute":
            case "microsoft_teardown_reason":
                return "none";
            case "intel_friendly_name":
                return "Android phone";
            case "intel_sink_manufacturer_name":
                return "CastKu";
            case "intel_sink_model_name":
                return "2602DPT53G";
            case "intel_sink_version":
                return "1.0";
            default:
                return null;
        }
    }

    private void sendSetup() throws IOException {
        String url = presentationUrl != null ? presentationUrl : "rtsp://" + sourceIp + "/wfd1.0/streamid=0";
        sendRequest("SETUP " + url + " RTSP/1.0",
                "Transport: RTP/AVP/UDP;unicast;client_port=" + RTP_PORT + "\r\n", null, Kind.SETUP);
    }

    private void sendPlay() throws IOException {
        String url = presentationUrl != null ? presentationUrl : "rtsp://" + sourceIp + "/wfd1.0/streamid=0";
        sendRequest("PLAY " + url + " RTSP/1.0", sessionHeader(), null, Kind.OTHER);
    }

    private String sessionHeader() {
        return session != null ? "Session: " + session + "\r\n" : "";
    }

    enum Kind { OPTIONS, SETUP, OTHER }

    private final Map<Integer, Kind> pending = new LinkedHashMap<>();

    private void sendRequest(String line, String headers, String body, Kind kind) throws IOException {
        int c = cseq++;
        pending.put(c, kind);
        StringBuilder sb = new StringBuilder(line).append("\r\nCSeq: ").append(c).append("\r\n").append(headers);
        appendBody(sb, body, null);
        send(sb.toString());
    }

    private void reply(String cseqHeader, String headers, String body) throws IOException {
        StringBuilder sb = new StringBuilder("RTSP/1.0 200 OK\r\nCSeq: ").append(cseqHeader).append("\r\n").append(headers);
        appendBody(sb, body, null);
        send(sb.toString());
    }

    private static void appendBody(StringBuilder sb, String body, String type) {
        if (body != null && !body.isEmpty()) {
            sb.append("Content-Length: ").append(body.getBytes(StandardCharsets.UTF_8).length).append("\r\n\r\n").append(body);
        } else {
            sb.append("\r\n");
        }
    }

    private void send(String s) throws IOException {
        log(">>> " + s);
        out.write(s.getBytes(StandardCharsets.UTF_8));
        out.flush();
    }

    private static Map<String, String> parseParams(String body) {
        Map<String, String> m = new LinkedHashMap<>();
        for (String line : body.split("\r?\n")) {
            int i = line.indexOf(':');
            if (i > 0) m.put(line.substring(0, i).trim(), line.substring(i + 1).trim());
        }
        return m;
    }

    private void startRtpCounter() {
        Thread t = new Thread(() -> {
            try (DatagramSocket ds = new DatagramSocket(RTP_PORT);
                 FileOutputStream dump = new FileOutputStream("/data/local/tmp/xcast-probe.ts")) {
                rtpSocket = ds;
                ds.setReceiveBufferSize(4 << 20);
                byte[] buf = new byte[65536];
                DatagramPacket pkt = new DatagramPacket(buf, buf.length);
                long packets = 0, bytes = 0, dumped = 0, lastLog = System.currentTimeMillis();
                int lastSeq = -1, lost = 0;
                while (running) {
                    ds.receive(pkt);
                    packets++;
                    bytes += pkt.getLength();
                    int seq = ((buf[2] & 0xff) << 8) | (buf[3] & 0xff);
                    if (lastSeq >= 0 && ((lastSeq + 1) & 0xffff) != seq) lost++;
                    lastSeq = seq;
                    if (dumped < 20_000_000 && pkt.getLength() > 12) {
                        dump.write(buf, 12, pkt.getLength() - 12);
                        dumped += pkt.getLength() - 12;
                    }
                    long now = System.currentTimeMillis();
                    if (now - lastLog >= 2000) {
                        log(String.format("rtp %d pkts, %.1f Mbps, pt=%d, gaps=%d", packets,
                                bytes * 8 / 1e6 / ((now - lastLog) / 1000.0), buf[1] & 0x7f, lost));
                        packets = 0;
                        bytes = 0;
                        lastLog = now;
                    }
                }
            } catch (IOException e) {
                log("rtp receiver: " + e);
            }
        }, "rtp-probe");
        t.setDaemon(true);
        t.start();
    }

    private static void sleep(long ms) {
        try {
            Thread.sleep(ms);
        } catch (InterruptedException ignored) {
        }
    }

    /** Minimal RTSP message. */
    static final class Msg {
        String startLine;
        String method = "";
        Map<String, String> headers = new LinkedHashMap<>();
        String body = "";

        boolean isResponse() {
            return startLine.startsWith("RTSP/");
        }

        String header(String name) {
            for (Map.Entry<String, String> e : headers.entrySet()) {
                if (e.getKey().equalsIgnoreCase(name)) return e.getValue();
            }
            return null;
        }

        String raw() {
            StringBuilder sb = new StringBuilder(startLine).append('\n');
            for (Map.Entry<String, String> e : headers.entrySet()) sb.append(e.getKey()).append(": ").append(e.getValue()).append('\n');
            if (!body.isEmpty()) sb.append('\n').append(body);
            return sb.toString();
        }

        static Msg read(InputStream in) throws IOException {
            String line;
            do {
                line = readLine(in);
                if (line == null) return null;
            } while (line.isEmpty());
            Msg m = new Msg();
            m.startLine = line;
            if (!m.isResponse()) m.method = line.split(" ")[0];
            while ((line = readLine(in)) != null && !line.isEmpty()) {
                int i = line.indexOf(':');
                if (i > 0) m.headers.put(line.substring(0, i).trim(), line.substring(i + 1).trim());
            }
            String cl = m.header("Content-Length");
            if (cl != null) {
                int n = Integer.parseInt(cl.trim());
                byte[] b = new byte[n];
                int off = 0;
                while (off < n) {
                    int r = in.read(b, off, n - off);
                    if (r < 0) return null;
                    off += r;
                }
                m.body = new String(b, StandardCharsets.UTF_8);
            }
            return m;
        }

        private static String readLine(InputStream in) throws IOException {
            ByteArrayOutputStream b = new ByteArrayOutputStream();
            int c;
            while ((c = in.read()) != -1) {
                if (c == '\n') break;
                if (c != '\r') b.write(c);
            }
            if (c == -1 && b.size() == 0) return null;
            return b.toString("UTF-8");
        }
    }
}
