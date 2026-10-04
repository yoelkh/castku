package com.xcast.helper;

import android.os.Handler;
import android.os.Looper;

import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.concurrent.CopyOnWriteArrayList;

/**
 * Privileged helper, launched as the shell user:
 *   CLASSPATH=helper.dex app_process / com.xcast.helper.HelperMain [--name N] [--port P] [--app PKG]
 *
 * Events go to stdout and to every client of the loopback control socket as lines:
 *   EVT <type> <json>
 * Commands accepted on the socket: "disconnect", "quit", "ping".
 */
public final class HelperMain {
    public static final int DEFAULT_PORT = 47291;

    private static final List<OutputStream> clients = new CopyOnWriteArrayList<>();
    private static volatile String lastConnected;
    private static boolean probeEnabled;
    private static RtspProbe probe;

    public static void main(String[] args) throws Exception {
        String name = null;
        int port = DEFAULT_PORT;
        String appPackage = null;
        for (int i = 0; i < args.length; i++) {
            switch (args[i]) {
                case "--name": name = args[++i]; break;
                case "--port": port = Integer.parseInt(args[++i]); break;
                case "--app": appPackage = args[++i]; break;
                case "--probe": probeEnabled = true; break;
                default: System.err.println("unknown arg " + args[i]);
            }
        }

        ServerSocket server;
        try {
            server = new ServerSocket(port, 4, InetAddress.getByName("127.0.0.1"));
        } catch (IOException e) {
            System.out.println("EVT error {\"msg\":\"helper already running on port " + port + "\"}");
            return;
        }

        if (appPackage != null) exemptFromBackgroundLimits(appPackage);

        Looper.prepareMainLooper();
        Handler main = new Handler(Looper.getMainLooper());
        P2pController p2p = new P2pController(FakeContext.get(), Looper.getMainLooper(), HelperMain::broadcast);

        Thread acceptor = new Thread(() -> acceptLoop(server, main, p2p), "control");
        acceptor.setDaemon(true);
        acceptor.start();

        Runtime.getRuntime().addShutdownHook(new Thread(p2p::stop));
        p2p.start(name);
        broadcast("ready", "{\"pid\":" + android.os.Process.myPid() + ",\"port\":" + port + "}");
        Looper.loop();
    }

    private static void acceptLoop(ServerSocket server, Handler main, P2pController p2p) {
        while (true) {
            try {
                Socket s = server.accept();
                s.setTcpNoDelay(true);
                OutputStream out = s.getOutputStream();
                clients.add(out);
                String last = lastConnected;
                if (last != null) write(out, "EVT connected " + last + "\n");
                new Thread(() -> clientLoop(s, out, main, p2p), "client").start();
            } catch (IOException e) {
                return;
            }
        }
    }

    private static void clientLoop(Socket s, OutputStream out, Handler main, P2pController p2p) {
        try (BufferedReader in = new BufferedReader(new InputStreamReader(s.getInputStream(), StandardCharsets.UTF_8))) {
            String line;
            while ((line = in.readLine()) != null) {
                switch (line.trim()) {
                    case "disconnect": main.post(p2p::disconnect); break;
                    case "pause": main.post(p2p::pause); break;
                    case "resume": main.post(p2p::resume); break;
                    case "quit":
                        main.post(() -> {
                            p2p.stop();
                            System.exit(0);
                        });
                        break;
                    case "ping": write(out, "EVT pong {}\n"); break;
                    default: break;
                }
            }
        } catch (IOException ignored) {
        } finally {
            clients.remove(out);
            try {
                s.close();
            } catch (IOException ignored) {
            }
        }
    }

    private static void broadcast(String type, String json) {
        if ("connected".equals(type)) {
            lastConnected = json;
            if (probeEnabled) {
                if (probe != null) probe.stop();
                probe = new RtspProbe(jsonField(json, "sourceIp"), P2pController.RTSP_PORT, jsonField(json, "localIp"));
                probe.start();
            }
        } else if ("disconnected".equals(type)) {
            lastConnected = null;
            if (probe != null) {
                probe.stop();
                probe = null;
            }
        }
        String line = "EVT " + type + " " + json + "\n";
        System.out.print(line);
        System.out.flush();
        for (OutputStream out : clients) {
            if (!write(out, line)) clients.remove(out);
        }
    }

    static String jsonField(String json, String key) {
        java.util.regex.Matcher m = java.util.regex.Pattern.compile("\"" + key + "\":\"([^\"]*)\"").matcher(json);
        return m.find() ? m.group(1) : null;
    }

    private static boolean write(OutputStream out, String line) {
        try {
            synchronized (out) {
                out.write(line.getBytes(StandardCharsets.UTF_8));
                out.flush();
            }
            return true;
        } catch (IOException e) {
            return false;
        }
    }

    /** HyperOS aggressively kills background apps; the shell user may whitelist ours. */
    private static void exemptFromBackgroundLimits(String pkg) {
        run("cmd", "deviceidle", "whitelist", "+" + pkg);
        run("cmd", "appops", "set", pkg, "RUN_ANY_IN_BACKGROUND", "allow");
        run("cmd", "appops", "set", pkg, "RUN_IN_BACKGROUND", "allow");
        // Lets SinkService bring the display activity to the front when the PC connects.
        run("cmd", "appops", "set", pkg, "SYSTEM_ALERT_WINDOW", "allow");
        run("cmd", "appops", "set", pkg, "10021", "allow"); // MIUI/HyperOS "start in background"
        run("pm", "grant", pkg, "android.permission.POST_NOTIFICATIONS");
    }

    private static void run(String... cmd) {
        try {
            new ProcessBuilder(cmd).redirectErrorStream(true).start().waitFor();
        } catch (Exception ignored) {
        }
    }
}
