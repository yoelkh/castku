package com.xcast.display;

import android.util.Log;

import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/** Loopback link to the privileged helper (see com.xcast.helper.HelperMain). Reconnects forever. */
final class HelperClient {
    static final int PORT = 47291;
    private static final String TAG = "XCast";

    interface Listener {
        void onHelperState(boolean up);
        void onHelperEvent(String type, JSONObject data);
    }

    private final Listener listener;
    private volatile boolean running;
    private volatile Socket socket;
    private Thread thread;
    private final ExecutorService sender = Executors.newSingleThreadExecutor();

    HelperClient(Listener listener) {
        this.listener = listener;
    }

    void start() {
        if (running) return;
        running = true;
        thread = new Thread(this::loop, "helper-link");
        thread.start();
    }

    void stop() {
        running = false;
        // Queued behind pending sends so a final "pause" still reaches the helper.
        sender.execute(this::closeSocket);
        sender.shutdown();
        if (thread != null) thread.interrupt();
    }

    /** Asynchronous: callers are often on the main thread, where socket writes are forbidden. */
    void send(String command) {
        if (sender.isShutdown()) return;
        sender.execute(() -> {
            Socket s = socket;
            if (s == null) return;
            try {
                OutputStream out = s.getOutputStream();
                out.write((command + "\n").getBytes(StandardCharsets.UTF_8));
                out.flush();
            } catch (IOException e) {
                Log.w(TAG, "helper command '" + command + "' failed: " + e);
            }
        });
    }

    static boolean isHelperRunning() {
        try (Socket s = new Socket()) {
            s.connect(new InetSocketAddress("127.0.0.1", PORT), 300);
            return true;
        } catch (IOException e) {
            return false;
        }
    }

    private void loop() {
        while (running) {
            try (Socket s = new Socket()) {
                s.connect(new InetSocketAddress("127.0.0.1", PORT), 1000);
                s.setTcpNoDelay(true);
                socket = s;
                listener.onHelperState(true);
                BufferedReader in = new BufferedReader(new InputStreamReader(s.getInputStream(), StandardCharsets.UTF_8));
                String line;
                while (running && (line = in.readLine()) != null) {
                    dispatch(line);
                }
            } catch (IOException ignored) {
            } finally {
                if (socket != null) {
                    socket = null;
                    listener.onHelperState(false);
                }
            }
            if (!running) break;
            try {
                Thread.sleep(1000);
            } catch (InterruptedException e) {
                break;
            }
        }
    }

    private void dispatch(String line) {
        if (!line.startsWith("EVT ")) return;
        int sp = line.indexOf(' ', 4);
        if (sp < 0) return;
        String type = line.substring(4, sp);
        try {
            listener.onHelperEvent(type, new JSONObject(line.substring(sp + 1)));
        } catch (Exception e) {
            Log.w(TAG, "bad helper event: " + line);
        }
    }

    private void closeSocket() {
        Socket s = socket;
        if (s != null) {
            try {
                s.close();
            } catch (IOException ignored) {
            }
        }
    }
}
