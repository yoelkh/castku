package com.xcast.helper;

import android.content.Context;
import android.net.MacAddress;
import android.net.wifi.p2p.WifiP2pConfig;
import android.net.wifi.p2p.WifiP2pDevice;
import android.net.wifi.p2p.WifiP2pGroup;
import android.net.wifi.p2p.WifiP2pInfo;
import android.net.wifi.p2p.WifiP2pManager;
import android.net.wifi.p2p.WifiP2pWfdInfo;
import android.os.Handler;
import android.os.IBinder;
import android.os.IInterface;
import android.os.Looper;

import java.io.BufferedReader;
import java.io.FileReader;
import java.io.InputStreamReader;
import java.lang.reflect.Constructor;
import java.lang.reflect.Method;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.NetworkInterface;
import java.util.Collections;

/**
 * Advertises this phone as a Wi-Fi Display (Miracast) primary sink and keeps it
 * discoverable, auto-accepts the PC's connection and reports the source address.
 */
public final class P2pController {
    public static final int RTSP_PORT = 7236;
    private static final long IDLE_REFRESH_MS = 25_000;
    private static final long WFD_CHECK_MS = 5_000;
    private static final long POLL_MS = 1_000;

    public interface Listener {
        void onEvent(String type, String json);
    }

    private final Context context;
    private final Handler handler;
    private final Listener listener;
    private WifiP2pManager manager;
    private WifiP2pManager.Channel channel;
    private boolean connected;
    private boolean paused;            // invisible to Windows; resumed by the app ("resume")
    private boolean approverAttached;
    private String connectedSourceIp;
    private String deviceName;

    public P2pController(Context context, Looper looper, Listener listener) {
        this.context = context;
        this.handler = new Handler(looper);
        this.listener = listener;
    }

    public void start(String name) throws Exception {
        deviceName = name;
        manager = createManager();
        channel = manager.initialize(context, handler.getLooper(), () -> {
            emit("error", "{\"msg\":\"channel disconnected\"}");
            handler.removeCallbacksAndMessages(null);
            handler.postDelayed(() -> {
                try {
                    start(deviceName);
                } catch (Exception e) {
                    emit("error", jsonMsg("re-init failed: " + e));
                }
            }, 1000);
        });
        if (channel == null) {
            throw new IllegalStateException("WifiP2pManager.initialize returned null (Wi-Fi off?)");
        }
        // A group left over from a previous (crashed) session would look like a live connection.
        // Configure only after the removal completes: overlapping P2P commands fail with BUSY.
        manager.removeGroup(channel, new WifiP2pManager.ActionListener() {
            @Override
            public void onSuccess() {
                configure();
            }

            @Override
            public void onFailure(int reason) {
                configure();  // usually "no group"
            }
        });
    }

    private void configure() {
        // Android 16 creates the P2P interface on demand: until a discovery request arrives the
        // state machine sits in P2pDisabledState and drops listen/name requests (reported as BUSY).
        // The WFD info set here is stored and applied when the interface comes up.
        applyWfdInfo(true);
        if (!approverAttached) addApprover();  // re-adding would replace it and loop via onDetached
        discover();
        handler.postDelayed(this::syncDeviceName, 2000);
        handler.postDelayed(this::idleRefresh, IDLE_REFRESH_MS);
        handler.postDelayed(this::checkWfd, WFD_CHECK_MS);
        handler.postDelayed(this::poll, POLL_MS);
    }

    public void stop() {
        handler.removeCallbacksAndMessages(null);
        if (manager == null || channel == null) return;
        manager.stopPeerDiscovery(channel, null);
        WifiP2pWfdInfo info = new WifiP2pWfdInfo();
        info.setEnabled(false);
        manager.setWfdInfo(channel, info, null);
        manager.removeGroup(channel, null);
    }

    /**
     * The app's "Matikan" switch: hide from Windows and drop any group, but keep the process (and
     * its shell privileges) alive so "Aktifkan" works again without Shizuku or a PC.
     */
    public void pause() {
        if (paused) return;
        paused = true;
        handler.removeCallbacksAndMessages(null);
        if (manager == null || channel == null) return;
        manager.stopPeerDiscovery(channel, null);
        WifiP2pWfdInfo info = new WifiP2pWfdInfo();
        info.setEnabled(false);
        manager.setWfdInfo(channel, info, null);
        manager.removeGroup(channel, null);
        if (connected) {
            connected = false;
            connectedSourceIp = null;
            emit("disconnected", "{}");
        }
        emit("paused", "{}");
    }

    public void resume() {
        if (!paused) return;
        paused = false;
        configure();
        emit("resumed", "{}");
    }

    public void disconnect() {
        if (manager != null && channel != null) {
            manager.removeGroup(channel, logListener("removeGroup"));
        }
    }

    private static WifiP2pManager createManager() throws Exception {
        Class<?> sm = Class.forName("android.os.ServiceManager");
        IBinder binder = (IBinder) sm.getMethod("getService", String.class).invoke(null, "wifip2p");
        if (binder == null) throw new IllegalStateException("wifip2p service not found");
        Class<?> stub = Class.forName("android.net.wifi.p2p.IWifiP2pManager$Stub");
        IInterface service = (IInterface) stub.getMethod("asInterface", IBinder.class).invoke(null, binder);
        Class<?> iface = Class.forName("android.net.wifi.p2p.IWifiP2pManager");
        Constructor<WifiP2pManager> ctor = WifiP2pManager.class.getDeclaredConstructor(iface);
        ctor.setAccessible(true);
        return ctor.newInstance(service);
    }

    /** Renames the Wi-Fi Direct device only when it differs (the name is a system-wide setting). */
    private void syncDeviceName() {
        if (deviceName == null || deviceName.isEmpty()) return;
        manager.requestDeviceInfo(channel, device -> {
            if (device != null && !deviceName.equals(device.deviceName)) setDeviceName(deviceName);
        });
    }

    private void setDeviceName(String name) {
        try {
            Method m = WifiP2pManager.class.getMethod("setDeviceName",
                    WifiP2pManager.Channel.class, String.class, WifiP2pManager.ActionListener.class);
            m.invoke(manager, channel, name, logListener("setDeviceName"));
        } catch (Exception e) {
            emit("warn", jsonMsg("setDeviceName: " + rootCause(e)));
        }
    }

    private WifiP2pWfdInfo desiredWfdInfo() {
        WifiP2pWfdInfo info = new WifiP2pWfdInfo();
        info.setEnabled(true);
        info.setDeviceType(WifiP2pWfdInfo.DEVICE_TYPE_PRIMARY_SINK);
        info.setSessionAvailable(!connected);
        info.setControlPort(RTSP_PORT);
        info.setMaxThroughput(300);
        info.setContentProtectionSupported(false);
        return info;
    }

    private void applyWfdInfo(boolean log) {
        try {
            manager.setWfdInfo(channel, desiredWfdInfo(), log ? logListener("setWfdInfo") : null);
        } catch (Exception e) {
            emit("error", jsonMsg("setWfdInfo: " + rootCause(e)));
        }
    }

    /**
     * system_server's own cast feature overwrites the global WFD info. Re-apply ours only when it
     * actually differs: every setWfdInfo() fires a system-wide THIS_DEVICE_CHANGED broadcast.
     */
    private void checkWfd() {
        manager.requestDeviceInfo(channel, device -> {
            if (device == null) return;  // P2P interface down; stored WFD info is applied when it comes up
            WifiP2pWfdInfo cur = device.getWfdInfo();
            WifiP2pWfdInfo want = desiredWfdInfo();
            boolean ok = cur != null && cur.isEnabled()
                    && cur.getDeviceType() == want.getDeviceType()
                    && cur.getControlPort() == want.getControlPort()
                    && cur.isSessionAvailable() == want.isSessionAvailable();
            if (!ok) {
                emit("warn", jsonMsg("WFD info was overwritten; re-applying"));
                applyWfdInfo(false);
            }
        });
        handler.postDelayed(this::checkWfd, WFD_CHECK_MS);
    }

    private void addApprover() {
        try {
            manager.addExternalApprover(channel, MacAddress.BROADCAST_ADDRESS,
                    new WifiP2pManager.ExternalApproverRequestListener() {
                        @Override
                        public void onAttached(MacAddress deviceAddress) {
                            approverAttached = true;
                            emit("approver", "{\"state\":\"attached\",\"mac\":\"" + deviceAddress + "\"}");
                        }

                        @Override
                        public void onDetached(MacAddress deviceAddress, int reason) {
                            approverAttached = false;
                            emit("approver", "{\"state\":\"detached\",\"reason\":" + reason + "}");
                            // The approver detaches after each handled request (reason 0); re-arm it.
                            // REASON_FAILURE (1) is permanent.
                            if (reason != 1) handler.postDelayed(P2pController.this::addApprover, 500);
                        }

                        @Override
                        public void onConnectionRequested(int requestType, WifiP2pConfig config,
                                WifiP2pDevice device) {
                            emit("request", "{\"type\":" + requestType + ",\"name\":\""
                                    + esc(device.deviceName) + "\",\"mac\":\"" + device.deviceAddress + "\"}");
                            manager.setConnectionRequestResult(channel,
                                    MacAddress.fromString(device.deviceAddress),
                                    paused ? WifiP2pManager.CONNECTION_REQUEST_REJECT
                                           : WifiP2pManager.CONNECTION_REQUEST_ACCEPT,
                                    logListener(paused ? "reject (paused)" : "accept"));
                        }

                        @Override
                        public void onPinGenerated(MacAddress deviceAddress, String pin) {
                            emit("pin", "{\"pin\":\"" + esc(pin) + "\"}");
                        }
                    });
        } catch (Throwable e) {
            emit("warn", jsonMsg("addExternalApprover: " + rootCause(e)));
        }
    }

    private void discover() {
        manager.discoverPeers(channel, new WifiP2pManager.ActionListener() {
            @Override
            public void onSuccess() {
            }

            @Override
            public void onFailure(int reason) {
                // BUSY (2) happens while the framework is already discovering; harmless.
                if (reason != WifiP2pManager.BUSY) {
                    emit("warn", jsonMsg("discoverPeers failed: " + reason));
                }
            }
        });
    }

    private void idleRefresh() {
        // Discovery (which is what makes the PC see us) times out and system_server can block it;
        // renew while idle. Never while streaming: scans steal airtime from the video.
        if (!connected) discover();
        handler.postDelayed(this::idleRefresh, IDLE_REFRESH_MS);
    }

    private void poll() {
        manager.requestConnectionInfo(channel, info -> {
            if (info != null && info.groupFormed) {
                manager.requestGroupInfo(channel, group -> onGroup(info, group));
            } else if (connected) {
                connected = false;
                connectedSourceIp = null;
                emit("disconnected", "{}");
                applyWfdInfo(false);
                discover();
            }
        });
        handler.postDelayed(this::poll, POLL_MS);
    }

    private void onGroup(WifiP2pInfo info, WifiP2pGroup group) {
        String iface = group != null ? group.getInterface() : null;
        String sourceIp;
        String localIp = iface != null ? localIpv4(iface) : null;
        if (info.isGroupOwner) {
            sourceIp = iface != null ? findPeerIp(iface) : null;
        } else {
            sourceIp = info.groupOwnerAddress != null ? info.groupOwnerAddress.getHostAddress() : null;
        }
        if (sourceIp == null) return; // DHCP not finished yet; retry on next poll
        if (connected && sourceIp.equals(connectedSourceIp)) return;
        connected = true;
        connectedSourceIp = sourceIp;
        String peerName = "";
        if (group != null) {
            if (info.isGroupOwner) {
                for (WifiP2pDevice d : group.getClientList()) {
                    peerName = d.deviceName;
                    break;
                }
            } else if (group.getOwner() != null) {
                peerName = group.getOwner().deviceName;
            }
        }
        int freq = group != null ? group.getFrequency() : 0;
        manager.stopPeerDiscovery(channel, null);
        applyWfdInfo(false);
        emit("connected", "{\"sourceIp\":\"" + sourceIp + "\",\"localIp\":\"" + (localIp == null ? "" : localIp)
                + "\",\"iface\":\"" + esc(iface) + "\",\"go\":" + info.isGroupOwner
                + ",\"freq\":" + freq + ",\"rtspPort\":" + RTSP_PORT + ",\"peer\":\"" + esc(peerName) + "\"}");
    }

    private static String localIpv4(String iface) {
        try {
            NetworkInterface ni = NetworkInterface.getByName(iface);
            if (ni == null) return null;
            for (InetAddress a : Collections.list(ni.getInetAddresses())) {
                if (a instanceof Inet4Address) return a.getHostAddress();
            }
        } catch (Exception ignored) {
        }
        return null;
    }

    /** We are group owner: the PC got its address from our DHCP server. */
    private static String findPeerIp(String iface) {
        try (BufferedReader r = new BufferedReader(new FileReader("/proc/net/arp"))) {
            String line;
            r.readLine();
            while ((line = r.readLine()) != null) {
                String[] p = line.trim().split("\\s+");
                if (p.length >= 6 && p[5].equals(iface) && !"00:00:00:00:00:00".equals(p[3])) return p[0];
            }
        } catch (Exception ignored) {
        }
        try {
            Process proc = new ProcessBuilder("ip", "neigh", "show", "dev", iface).start();
            try (BufferedReader r = new BufferedReader(new InputStreamReader(proc.getInputStream()))) {
                String line;
                while ((line = r.readLine()) != null) {
                    String[] p = line.trim().split("\\s+");
                    if (p.length > 0 && p[0].matches("\\d+\\.\\d+\\.\\d+\\.\\d+") && !line.contains("FAILED")) {
                        return p[0];
                    }
                }
            }
        } catch (Exception ignored) {
        }
        return null;
    }

    private WifiP2pManager.ActionListener logListener(String what) {
        return new WifiP2pManager.ActionListener() {
            @Override
            public void onSuccess() {
                emit("ok", jsonMsg(what));
            }

            @Override
            public void onFailure(int reason) {
                emit("error", jsonMsg(what + " failed: " + reason));
            }
        };
    }

    private void emit(String type, String json) {
        listener.onEvent(type, json);
    }

    static String rootCause(Throwable t) {
        while (t.getCause() != null) t = t.getCause();
        return t.toString();
    }

    static String jsonMsg(String s) {
        return "{\"msg\":\"" + esc(s) + "\"}";
    }

    static String esc(String s) {
        if (s == null) return "";
        return s.replace("\\", "\\\\").replace("\"", "\\\"").replace("\n", " ");
    }
}
