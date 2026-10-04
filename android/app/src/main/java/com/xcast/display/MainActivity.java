package com.xcast.display;

import android.app.Activity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.os.Bundle;
import android.provider.Settings;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.CompoundButton;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.ScrollView;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;

import rikka.shizuku.Shizuku;

/** Control screen: enable the wireless display, pick the stream mode, see status. */
public final class MainActivity extends Activity implements SinkService.StateListener {
    private static final int ACCENT = 0xFF14B8A6;
    private static final int CARD = 0xFF1C1D22;
    private static final int TEXT = 0xFFECECEC;
    private static final int MUTED = 0xFF9A9BA2;

    private TextView statusTitle;
    private TextView statusDetail;
    private Button toggle;
    private Button fixButton;
    private LinearLayout helperCard;
    private TextView helperText;
    private Button shizukuButton;

    private final Shizuku.OnRequestPermissionResultListener shizukuResult = (code, result) -> {
        SinkService s = SinkService.get();
        if (s != null) s.retryHelper();
        runOnUiThread(this::refreshHelperCard);
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(buildUi());
        try {
            Shizuku.addRequestPermissionResultListener(shizukuResult);
        } catch (Throwable ignored) {
        }
    }

    @Override
    protected void onDestroy() {
        try {
            Shizuku.removeRequestPermissionResultListener(shizukuResult);
        } catch (Throwable ignored) {
        }
        super.onDestroy();
    }

    @Override
    protected void onStart() {
        super.onStart();
        SinkService.addListener(this);
        if (SinkService.get() == null) showStopped();
        refreshHelperCard();
    }

    @Override
    protected void onStop() {
        SinkService.removeListener(this);
        super.onStop();
    }

    @Override
    public void onState(SinkService.State state, String detail) {
        switch (state) {
            case STARTING_HELPER: statusTitle.setText("Menyiapkan…"); break;
            case HELPER_MISSING: statusTitle.setText("Perlu helper"); break;
            case HOTSPOT_ON: statusTitle.setText("Hotspot aktif"); break;
            case WIFI_OFF: statusTitle.setText("Wi-Fi mati"); break;
            case READY: statusTitle.setText("Siap dicast"); break;
            case CONNECTING: statusTitle.setText("Menghubungkan…"); break;
            case STREAMING: statusTitle.setText("Sedang tampil"); break;
        }
        statusDetail.setText(detail);
        toggle.setText("Matikan");
        helperCard.setVisibility(state == SinkService.State.HELPER_MISSING ? View.VISIBLE : View.GONE);
        if (state == SinkService.State.HOTSPOT_ON) {
            fixButton.setText("Buka pengaturan Hotspot & USB tethering");
            fixButton.setVisibility(View.VISIBLE);
        } else if (state == SinkService.State.WIFI_OFF) {
            fixButton.setText("Nyalakan Wi-Fi");
            fixButton.setVisibility(View.VISIBLE);
        } else {
            fixButton.setVisibility(View.GONE);
        }
        refreshHelperCard();
    }

    private void showStopped() {
        statusTitle.setText("Nonaktif");
        statusDetail.setText("Tekan Aktifkan agar HP muncul di menu Cast (Win + K) Windows.");
        toggle.setText("Aktifkan");
        helperCard.setVisibility(View.GONE);
        fixButton.setVisibility(View.GONE);
    }

    private void onToggle() {
        if (SinkService.get() == null) {
            SinkService.start(this);
            statusTitle.setText("Menyiapkan…");
            toggle.setText("Matikan");
        } else {
            startService(new Intent(this, SinkService.class).setAction(SinkService.ACTION_STOP));
            showStopped();
        }
    }

    private void refreshHelperCard() {
        if (helperText == null) return;
        boolean avail = HelperLauncher.shizukuAvailable();
        boolean granted = avail && HelperLauncher.shizukuGranted();
        if (!avail) {
            helperText.setText("Fitur ini butuh helper sistem (hak shell, tanpa root). Pilih salah satu:\n\n"
                    + "• Shizuku: pasang & jalankan Shizuku (via Wireless debugging), lalu kembali ke sini.\n"
                    + "• Dari PC (USB debugging): jalankan scripts\\start-helper.bat, atau salin perintah adb di bawah.");
            shizukuButton.setText("Cek Shizuku lagi");
        } else if (!granted) {
            helperText.setText("Shizuku terdeteksi. Izinkan CastKu memakai Shizuku.");
            shizukuButton.setText("Izinkan Shizuku");
        } else {
            helperText.setText("Shizuku siap. Tekan tombol untuk menjalankan helper.");
            shizukuButton.setText("Jalankan helper");
        }
    }

    private void onShizukuButton() {
        if (HelperLauncher.shizukuAvailable() && !HelperLauncher.shizukuGranted()) {
            HelperLauncher.requestShizuku();
            return;
        }
        SinkService s = SinkService.get();
        if (s != null) s.retryHelper();
        refreshHelperCard();
    }

    // ---- UI ----

    private void openFixSettings() {
        Intent i;
        if (SinkService.hotspotActive()) {
            // AOSP/HyperOS tethering page; fall back to the generic network page if it moved.
            i = new Intent().setClassName("com.android.settings", "com.android.settings.TetherSettings");
        } else {
            i = new Intent(Settings.Panel.ACTION_WIFI);
        }
        try {
            startActivity(i);
        } catch (Exception e) {
            startActivity(new Intent(Settings.ACTION_WIRELESS_SETTINGS));
        }
    }

    private View buildUi() {
        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        int pad = dp(20);
        col.setPadding(pad, dp(36), pad, pad);
        scroll.addView(col);

        TextView title = text("CastKu", 28, TEXT);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        col.addView(title);
        col.addView(text("Jadikan HP ini layar kedua Windows — seperti Samsung Second Screen.", 14, MUTED));

        LinearLayout status = card();
        statusTitle = text("Nonaktif", 20, TEXT);
        statusTitle.setTypeface(Typeface.DEFAULT_BOLD);
        statusDetail = text("", 14, MUTED);
        status.addView(statusTitle);
        status.addView(statusDetail);
        toggle = new Button(this);
        toggle.setText("Aktifkan");
        toggle.setTextColor(0xFFFFFFFF);
        toggle.setBackground(rounded(ACCENT, dp(14)));
        toggle.setOnClickListener(v -> onToggle());
        LinearLayout.LayoutParams blp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, dp(52));
        blp.topMargin = dp(16);
        status.addView(toggle, blp);
        fixButton = new Button(this);
        fixButton.setVisibility(View.GONE);
        fixButton.setOnClickListener(v -> openFixSettings());
        status.addView(fixButton);
        col.addView(status, cardLp());

        helperCard = card();
        TextView ht = text("Helper sistem", 16, TEXT);
        ht.setTypeface(Typeface.DEFAULT_BOLD);
        helperCard.addView(ht);
        helperText = text("", 13, MUTED);
        helperCard.addView(helperText);
        shizukuButton = new Button(this);
        shizukuButton.setOnClickListener(v -> onShizukuButton());
        helperCard.addView(shizukuButton);
        Button copy = new Button(this);
        copy.setText("Salin perintah adb");
        copy.setOnClickListener(v -> {
            ClipboardManager cm = getSystemService(ClipboardManager.class);
            cm.setPrimaryClip(ClipData.newPlainText("adb", HelperLauncher.adbCommand(this)));
            Toast.makeText(this, "Perintah disalin", Toast.LENGTH_SHORT).show();
        });
        helperCard.addView(copy);
        helperCard.setVisibility(View.GONE);
        col.addView(helperCard, cardLp());

        LinearLayout q = card();
        TextView qt = text("Kualitas stream", 16, TEXT);
        qt.setTypeface(Typeface.DEFAULT_BOLD);
        q.addView(qt);
        RadioGroup group = new RadioGroup(this);
        Quality current = Quality.load(this);
        for (Quality mode : Quality.values()) {
            RadioButton rb = new RadioButton(this);
            rb.setText(mode.label);
            rb.setTextColor(TEXT);
            rb.setId(View.generateViewId());
            rb.setChecked(mode == current);
            rb.setOnCheckedChangeListener((b, checked) -> {
                if (checked) Quality.save(this, mode);
            });
            group.addView(rb);
        }
        q.addView(group);
        q.addView(text("Berlaku pada koneksi berikutnya. Smooth direkomendasikan.", 12, MUTED));

        TextView tm = text("Mode sentuh", 16, TEXT);
        tm.setTypeface(Typeface.DEFAULT_BOLD);
        tm.setPadding(0, dp(12), 0, dp(4));
        q.addView(tm);
        String[] touchModes = {
                "Touchscreen · jari langsung menyentuh (multi-touch, pinch)",
                "Pointer ikut jari · kursor pindah ke jari, tap = klik",
                "Touchpad · geser = kursor, tap = klik, 2 jari = scroll/klik kanan",
        };
        RadioGroup touchGroup = new RadioGroup(this);
        int currentTouch = Quality.prefs(this).getInt("touchMode", 0);
        for (int i = 0; i < touchModes.length; i++) {
            final int mode = i;
            RadioButton rb = new RadioButton(this);
            rb.setText(touchModes[i]);
            rb.setTextColor(TEXT);
            rb.setId(View.generateViewId());
            rb.setChecked(i == currentTouch);
            rb.setOnCheckedChangeListener((b, checked) -> {
                if (checked) Quality.prefs(this).edit().putInt("touchMode", mode).apply();
            });
            touchGroup.addView(rb);
        }
        q.addView(touchGroup);
        q.addView(text("Berlaku pada koneksi berikutnya.", 12, MUTED));
        SharedPreferences prefs = Quality.prefs(this);
        q.addView(toggleRow("Audio lewat HP", prefs.getBoolean("audio", true),
                (b, c) -> prefs.edit().putBoolean("audio", c).apply()));
        q.addView(toggleRow("Tampilkan statistik (fps, latensi)", prefs.getBoolean("stats", false),
                (b, c) -> prefs.edit().putBoolean("stats", c).apply()));
        col.addView(q, cardLp());

        LinearLayout tips = card();
        TextView tt = text("Cara pakai", 16, TEXT);
        tt.setTypeface(Typeface.DEFAULT_BOLD);
        tips.addView(tt);
        tips.addView(text("1. Tekan Aktifkan.\n"
                + "2. Di Windows tekan Win + K, pilih \"" + HelperLauncher.deviceName(this) + "\".\n"
                + "3. Agar layar sentuh HP bisa mengontrol PC, centang \"Allow mouse, keyboard, touch, and pen input\" di panel Cast.\n"
                + "4. Tekan Win + P → Extend untuk layar kedua, atau Duplicate untuk mirror.\n"
                + "5. Untuk memutus: tekan kembali dua kali di HP, atau Disconnect di Windows.", 13, MUTED));
        col.addView(tips, cardLp());
        return scroll;
    }

    private LinearLayout toggleRow(String label, boolean checked, CompoundButton.OnCheckedChangeListener l) {
        LinearLayout row = new LinearLayout(this);
        row.setGravity(Gravity.CENTER_VERTICAL);
        TextView t = text(label, 14, TEXT);
        row.addView(t, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
        Switch sw = new Switch(this);
        sw.setChecked(checked);
        sw.setOnCheckedChangeListener(l);
        row.addView(sw);
        row.setPadding(0, dp(8), 0, 0);
        return row;
    }

    private TextView text(String s, int sp, int color) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(sp);
        t.setTextColor(color);
        t.setPadding(0, dp(4), 0, dp(4));
        return t;
    }

    private LinearLayout card() {
        LinearLayout c = new LinearLayout(this);
        c.setOrientation(LinearLayout.VERTICAL);
        c.setPadding(dp(18), dp(16), dp(18), dp(16));
        c.setBackground(rounded(CARD, dp(18)));
        return c;
    }

    private LinearLayout.LayoutParams cardLp() {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(16);
        return lp;
    }

    private GradientDrawable rounded(int color, int radius) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(color);
        d.setCornerRadius(radius);
        return d;
    }

    private int dp(int v) {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }
}
