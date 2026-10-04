package com.xcast.display;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.ServiceInfo;
import android.net.wifi.WifiManager;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.util.Log;

import org.json.JSONObject;

import java.net.NetworkInterface;
import java.util.Collections;
import java.util.concurrent.CopyOnWriteArrayList;

/**
 * Keeps the phone discoverable as a wireless display and owns the Miracast session. When the PC
 * connects, it starts the native sink and brings up {@link DisplayActivity}.
 */
public final class SinkService extends Service implements HelperClient.Listener, NativeSink.Listener {
    private static final String TAG = "XCast";
    static final String ACTION_STOP = "com.xcast.display.STOP";
    private static final String CHANNEL = "cast";
    private static final int NOTIF_ID = 1;

    enum State { STARTING_HELPER, HELPER_MISSING, HOTSPOT_ON, WIFI_OFF, READY, CONNECTING, STREAMING }
    private static final long IDLE_CHECK_MS = 4000;
    private static final String ACTION_AP_STATE = "android.net.wifi.WIFI_AP_STATE_CHANGED";

    interface StateListener {
        void onState(State state, String detail);
    }

    private static volatile SinkService instance;
    private static final CopyOnWriteArrayList<StateListener> listeners = new CopyOnWriteArrayList<>();

    private final Handler main = new Handler(Looper.getMainLooper());
    private HelperClient helper;
    private NativeSink sink;
    private WifiManager.WifiLock wifiLock;
    private volatile State state = State.STARTING_HELPER;
    private volatile String detail = "";
    private volatile String peerName = "";
    private volatile String linkWarning = "";
    private volatile boolean sessionActive;
    private volatile boolean helperUp;
    private volatile boolean stopping;  // after "Matikan": late callbacks must not overwrite "Nonaktif"
    private volatile long lastRequestMs;
    private int launchAttempts;

    // Wi-Fi Direct only exists while Wi-Fi is on; tell the user instead of silently being invisible.
    private final BroadcastReceiver wifiReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            refreshIdleState();
        }
    };

    static SinkService get() {
        return instance;
    }

    static void addListener(StateListener l) {
        listeners.add(l);
        SinkService s = instance;
        if (s != null) l.onState(s.state, s.detail);
    }

    static void removeListener(StateListener l) {
        listeners.remove(l);
    }

    static void start(Context ctx) {
        ctx.startForegroundService(new Intent(ctx, SinkService.class));
    }

    NativeSink sink() {
        return sink;
    }

    boolean isSessionActive() {
        return sessionActive;
    }

    /** Ends the Miracast session and tears down the Wi-Fi Direct group. */
    void disconnect() {
        if (endSession("closed on phone")) helper.send("disconnect");
    }

    @Override
    public void onCreate() {
        super.onCreate();
        instance = this;
        sink = new NativeSink(this);
        helper = new HelperClient(this);
        WifiManager wm = getSystemService(WifiManager.class);
        wifiLock = wm.createWifiLock(WifiManager.WIFI_MODE_FULL_LOW_LATENCY, "xcast");
        wifiLock.setReferenceCounted(false);
        IntentFilter filter = new IntentFilter(WifiManager.WIFI_STATE_CHANGED_ACTION);
        filter.addAction(ACTION_AP_STATE);
        registerReceiver(wifiReceiver, filter, Context.RECEIVER_NOT_EXPORTED);
        main.postDelayed(idleCheck, IDLE_CHECK_MS);
    }

    // Tethering state has no public callback for apps; re-check periodically while idle.
    private final Runnable idleCheck = new Runnable() {
        @Override
        public void run() {
            refreshIdleState();
            main.postDelayed(this, IDLE_CHECK_MS);
        }
    };

    /**
     * This phone's Wi-Fi chip supports STA+AP or STA+P2P, never AP+P2P (HalDeviceManager chip
     * combinations), so while the hotspot runs the Wi-Fi Direct interface cannot be created and
     * Windows can't discover us.
     */
    static boolean hotspotActive() {
        try {
            for (NetworkInterface ni : Collections.list(NetworkInterface.getNetworkInterfaces())) {
                String n = ni.getName();
                if ((n.startsWith("ap") || n.startsWith("swlan") || n.startsWith("softap")) && ni.isUp()) return true;
            }
        } catch (Exception ignored) {
        }
        return false;
    }

    private boolean wifiEnabled() {
        return getSystemService(WifiManager.class).isWifiEnabled();
    }

    /** Idle status line: helper missing, Wi-Fi off, or ready to be picked in Win + K. */
    private void refreshIdleState() {
        if (sessionActive || !helperUp) return;
        // Group formation takes a few seconds after the PC's request; keep showing "connecting".
        if (state == State.CONNECTING && System.currentTimeMillis() - lastRequestMs < 15000) return;
        if (hotspotActive()) {
            setStateIfChanged(State.HOTSPOT_ON, "Hotspot HP sedang aktif. Chip Wi-Fi HP ini tidak bisa menjalankan hotspot "
                    + "dan Wi-Fi Direct bersamaan, jadi HP tidak akan muncul di Win + K.\n"
                    + "Matikan hotspot, lalu beri internet ke laptop lewat USB tethering (kabel), "
                    + "atau sambungkan laptop & HP ke Wi-Fi yang sama.");
        } else if (!wifiEnabled()) {
            setStateIfChanged(State.WIFI_OFF, "Wi-Fi HP mati. Nyalakan Wi-Fi (tidak harus tersambung ke jaringan) "
                    + "agar HP muncul di menu Cast Windows.");
        } else if (state != State.READY) {
            setState(State.READY, "Siap. Di Windows tekan Win + K lalu pilih \"" + HelperLauncher.deviceName(this) + "\"");
        }
    }

    private void setStateIfChanged(State s, String d) {
        if (state != s) setState(s, d);
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent != null && ACTION_STOP.equals(intent.getAction())) {
            stopping = true;
            endSession("service stopped");
            // Pause, not quit: the helper holds the shell privileges the app can't regain by itself.
            helper.send("pause");
            stopSelf();
            return START_NOT_STICKY;
        }
        startForeground(NOTIF_ID, buildNotification("Menyiapkan…"), ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE);
        helper.start();
        main.postDelayed(this::ensureHelper, 800);
        return START_STICKY;
    }

    private void ensureHelper() {
        if (state != State.STARTING_HELPER && state != State.HELPER_MISSING) return;
        new Thread(() -> {
            if (HelperClient.isHelperRunning()) return;
            if (launchAttempts++ < 3 && HelperLauncher.launchViaShizuku(this)) {
                setState(State.STARTING_HELPER, "Menjalankan helper via Shizuku…");
                main.postDelayed(this::ensureHelper, 2500);
            } else {
                setState(State.HELPER_MISSING, "Helper belum berjalan");
            }
        }, "helper-launch").start();
    }

    void retryHelper() {
        launchAttempts = 0;
        setState(State.STARTING_HELPER, "Menjalankan helper…");
        main.post(this::ensureHelper);
    }

    @Override
    public void onDestroy() {
        endSession("service destroyed");
        main.removeCallbacks(idleCheck);
        unregisterReceiver(wifiReceiver);
        helper.stop();
        sink.release();
        instance = null;
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    // ---- helper link ----

    @Override
    public void onHelperState(boolean up) {
        helperUp = up;
        if (up) {
            helper.send("resume");  // the service is running, so the user wants to be visible
            refreshIdleState();
        } else {
            endSession("helper stopped");
            setState(State.HELPER_MISSING, "Helper berhenti");
        }
    }

    @Override
    public void onHelperEvent(String type, JSONObject data) {
        switch (type) {
            case "connected":
                startSession(data);
                break;
            case "disconnected":
                endSession("Wi-Fi Direct disconnected");
                break;
            case "request":
                lastRequestMs = System.currentTimeMillis();
                setState(State.CONNECTING, "Menerima koneksi dari " + data.optString("name"));
                break;
            case "error":
            case "warn":
                Log.w(TAG, "helper " + type + ": " + data);
                break;
            default:
                break;
        }
    }

    private synchronized void startSession(JSONObject data) {
        if (sessionActive) return;
        String sourceIp = data.optString("sourceIp");
        int port = data.optInt("rtspPort", 7236);
        peerName = data.optString("peer", "PC");
        Quality q = Quality.load(this);
        boolean audio = Quality.prefs(this).getBoolean("audio", true);
        int touchMode = Quality.prefs(this).getInt("touchMode", 0);
        if (!sink.start(sourceIp, port, q.width, q.height, q.fps, audio, HelperLauncher.deviceName(this), Build.MODEL,
                touchMode)) {
            setState(State.READY, "Gagal membuka port RTP");
            return;
        }
        sessionActive = true;
        wifiLock.acquire();
        linkWarning = channelWarning(data.optInt("freq", 0));
        setState(State.CONNECTING, "Tersambung ke " + peerName + ", negosiasi…" + linkWarning);
        Intent i = new Intent(this, DisplayActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        startActivity(i);
    }

    /**
     * The Wi-Fi Direct group and the home Wi-Fi share one radio. On different channels the radio
     * hops between them, which Microsoft identifies as the main cause of Miracast stutter.
     */
    @SuppressWarnings("deprecation")
    private String channelWarning(int groupFreq) {
        WifiManager wm = getSystemService(WifiManager.class);
        int staFreq = wm.getConnectionInfo() != null ? wm.getConnectionInfo().getFrequency() : -1;
        Log.i(TAG, "Wi-Fi Direct " + groupFreq + " MHz, Wi-Fi " + staFreq + " MHz");
        if (groupFreq <= 0 || staFreq <= 0 || groupFreq == staFreq) return "";
        return "\n⚠ Wi-Fi Direct " + groupFreq + " MHz ≠ Wi-Fi " + staFreq
                + " MHz: radio berpindah channel (bisa patah-patah). Hubungkan PC & HP ke Wi-Fi 5 GHz yang sama.";
    }

    /** Returns true if a session was actually running (so the caller should tear down the group). */
    private synchronized boolean endSession(String reason) {
        if (!sessionActive) return false;
        sessionActive = false;
        sink.stop();
        if (wifiLock.isHeld()) wifiLock.release();
        DisplayActivity.finishIfShowing();
        if (helperUp) {
            if (wifiEnabled() && !hotspotActive()) {
                setState(State.READY, "Sesi berakhir (" + reason + "). Siap untuk koneksi berikutnya.");
            }
            else refreshIdleState();
        }
        return true;
    }

    // ---- native sink events (worker threads) ----

    @Override
    public void onSinkEvent(int type, String msg) {
        switch (type) {
            case NativeSink.EVENT_PLAYING:
                setState(State.STREAMING, "Menampilkan layar " + peerName + " (" + msg + ")" + linkWarning);
                break;
            case NativeSink.EVENT_ENDED:
                main.post(() -> {
                    // Only if the session was still up; if Wi-Fi Direct already dropped, the group is gone.
                    if (endSession(msg)) helper.send("disconnect");
                });
                break;
            case NativeSink.EVENT_UIBC_ON:
                Log.i(TAG, "touch input enabled");
                break;
            default:
                break;
        }
    }

    private void setState(State s, String d) {
        if (stopping) return;
        state = s;
        detail = d;
        main.post(() -> {
            NotificationManager nm = getSystemService(NotificationManager.class);
            nm.notify(NOTIF_ID, buildNotification(d));
            for (StateListener l : listeners) l.onState(s, d);
        });
    }

    private Notification buildNotification(String text) {
        NotificationManager nm = getSystemService(NotificationManager.class);
        if (nm.getNotificationChannel(CHANNEL) == null) {
            nm.createNotificationChannel(new NotificationChannel(CHANNEL, "Wireless display", NotificationManager.IMPORTANCE_LOW));
        }
        PendingIntent open = PendingIntent.getActivity(this, 0, new Intent(this, MainActivity.class),
                PendingIntent.FLAG_IMMUTABLE);
        PendingIntent stop = PendingIntent.getService(this, 1, new Intent(this, SinkService.class).setAction(ACTION_STOP),
                PendingIntent.FLAG_IMMUTABLE);
        return new Notification.Builder(this, CHANNEL)
                .setSmallIcon(R.drawable.ic_launcher)
                .setContentTitle("CastKu")
                .setContentText(text)
                .setContentIntent(open)
                .setOngoing(true)
                .addAction(new Notification.Action.Builder(null, "Matikan", stop).build())
                .build();
    }
}
