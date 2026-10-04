package com.xcast.display;

import android.app.Activity;
import android.graphics.Color;
import android.graphics.Rect;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Display;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.TextView;
import android.widget.Toast;
import android.window.OnBackInvokedDispatcher;

import java.lang.ref.WeakReference;
import java.util.Collections;

/** Full-screen renderer for the PC's display; touches are forwarded to Windows via UIBC. */
public final class DisplayActivity extends Activity implements SurfaceHolder.Callback, SinkService.StateListener {
    private static WeakReference<DisplayActivity> current = new WeakReference<>(null);

    private final Handler main = new Handler(Looper.getMainLooper());
    private Quality quality;
    private AspectSurfaceView surfaceView;
    private TextView stats;
    private TextView status;
    private long lastBackPress;
    private final int[] ids = new int[10];
    private final float[] xs = new float[10];
    private final float[] ys = new float[10];

    static void finishIfShowing() {
        DisplayActivity a = current.get();
        if (a != null) a.runOnUiThread(a::finish);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        current = new WeakReference<>(this);
        quality = Quality.load(this);

        WindowManager.LayoutParams lp = getWindow().getAttributes();
        lp.preferredDisplayModeId = highestRefreshMode();
        lp.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS;
        getWindow().setAttributes(lp);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        // Behave like a monitor: a connection from the PC wakes the phone and shows over the lock screen.
        setShowWhenLocked(true);
        setTurnScreenOn(true);
        // Android 16 (targetSdk 36) no longer calls onBackPressed(); predictive back needs a callback.
        getOnBackInvokedDispatcher().registerOnBackInvokedCallback(
                OnBackInvokedDispatcher.PRIORITY_DEFAULT, this::onBackRequested);

        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(Color.BLACK);
        surfaceView = new AspectSurfaceView(this, quality.width, quality.height);
        surfaceView.getHolder().addCallback(this);
        surfaceView.getHolder().setFixedSize(quality.width, quality.height);  // hardware composer scales
        root.addView(surfaceView, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT, Gravity.CENTER));

        status = overlayText(Gravity.CENTER, 16);
        status.setText("Menunggu gambar dari PC…");
        root.addView(status, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.WRAP_CONTENT,
                FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.CENTER));

        stats = overlayText(Gravity.START, 11);
        stats.setVisibility(Quality.prefs(this).getBoolean("stats", false) ? View.VISIBLE : View.GONE);
        root.addView(stats, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.WRAP_CONTENT,
                FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.TOP | Gravity.START));

        setContentView(root);
        hideSystemBars();
        surfaceView.setOnTouchListener((v, e) -> onTouch(e));
        surfaceView.addOnLayoutChangeListener((v, l, t, r, b, ol, ot, or, ob) -> {
            // Keep edge swipes for the PC rather than the Android back gesture.
            v.setSystemGestureExclusionRects(Collections.singletonList(new Rect(0, 0, r - l, b - t)));
            attachCursor();  // view size changed: cursor scale follows
        });
    }

    private TextView overlayText(int gravity, int sp) {
        TextView t = new TextView(this);
        t.setTextColor(0xFFFFFFFF);
        t.setTextSize(sp);
        t.setGravity(gravity);
        t.setShadowLayer(4, 0, 0, 0xFF000000);
        t.setPadding(24, 16, 24, 16);
        return t;
    }

    @Override
    protected void onStart() {
        super.onStart();
        SinkService.addListener(this);
        main.post(statsTick);
    }

    @Override
    protected void onStop() {
        SinkService.removeListener(this);
        main.removeCallbacks(statsTick);
        super.onStop();
    }

    @Override
    protected void onDestroy() {
        if (current.get() == this) current.clear();
        super.onDestroy();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) hideSystemBars();
    }

    private void hideSystemBars() {
        WindowInsetsController c = getWindow().getInsetsController();
        if (c != null) {
            c.hide(WindowInsets.Type.systemBars());
            c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        }
    }

    private int highestRefreshMode() {
        Display d = getDisplay();
        if (d == null) return 0;
        Display.Mode best = d.getMode();
        for (Display.Mode m : d.getSupportedModes()) {
            if (m.getPhysicalWidth() == best.getPhysicalWidth() && m.getRefreshRate() > best.getRefreshRate()) best = m;
        }
        return best.getModeId();
    }

    private final Runnable statsTick = new Runnable() {
        @Override
        public void run() {
            SinkService s = SinkService.get();
            if (s != null && stats.getVisibility() == View.VISIBLE) stats.setText(s.sink().stats());
            main.postDelayed(this, 500);
        }
    };

    @Override
    public void onState(SinkService.State state, String detail) {
        status.setVisibility(state == SinkService.State.STREAMING ? View.GONE : View.VISIBLE);
        status.setText(detail);
    }

    // ---- surface ----

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        Surface s = holder.getSurface();
        s.setFrameRate(120f, Surface.FRAME_RATE_COMPATIBILITY_DEFAULT, Surface.CHANGE_FRAME_RATE_ALWAYS);
        SinkService svc = SinkService.get();
        if (svc != null) {
            svc.sink().setSurface(s);
            attachCursor();
        }
    }

    /** The hardware cursor layer lives under the SurfaceView's layer, in view pixels. */
    private void attachCursor() {
        SinkService svc = SinkService.get();
        if (svc != null && surfaceView.getHolder().getSurface().isValid()) {
            svc.sink().setCursorParent(surfaceView.getSurfaceControl(), surfaceView.getWidth(), surfaceView.getHeight());
        }
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        SinkService svc = SinkService.get();
        if (svc != null) {
            svc.sink().setCursorParent(null, 0, 0);
            svc.sink().setSurface(null);
        }
    }

    // ---- input ----

    private boolean onTouch(MotionEvent e) {
        SinkService svc = SinkService.get();
        if (svc == null) return false;
        NativeSink sink = svc.sink();
        float w = surfaceView.getWidth(), h = surfaceView.getHeight();
        int action = e.getActionMasked();
        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN:
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP: {
                int i = e.getActionIndex();
                ids[0] = e.getPointerId(i);
                xs[0] = e.getX(i) / w;
                ys[0] = e.getY(i) / h;
                boolean down = action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN;
                sink.touch(down ? NativeSink.TOUCH_DOWN : NativeSink.TOUCH_UP, 1, ids, xs, ys);
                return true;
            }
            case MotionEvent.ACTION_MOVE: {
                int n = Math.min(e.getPointerCount(), 10);
                for (int i = 0; i < n; i++) {
                    ids[i] = e.getPointerId(i);
                    xs[i] = e.getX(i) / w;
                    ys[i] = e.getY(i) / h;
                }
                sink.touch(NativeSink.TOUCH_MOVE, n, ids, xs, ys);
                return true;
            }
            case MotionEvent.ACTION_CANCEL: {
                int n = Math.min(e.getPointerCount(), 10);
                for (int i = 0; i < n; i++) {
                    ids[i] = e.getPointerId(i);
                    xs[i] = e.getX(i) / w;
                    ys[i] = e.getY(i) / h;
                }
                sink.touch(NativeSink.TOUCH_UP, n, ids, xs, ys);
                return true;
            }
            default:
                return false;
        }
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        int code = event.getKeyCode();
        if (code == KeyEvent.KEYCODE_BACK || code == KeyEvent.KEYCODE_VOLUME_UP
                || code == KeyEvent.KEYCODE_VOLUME_DOWN) {
            return super.dispatchKeyEvent(event);
        }
        int ch = event.getUnicodeChar();
        if (code == KeyEvent.KEYCODE_ENTER) ch = 0x0d;
        else if (code == KeyEvent.KEYCODE_DEL) ch = 0x08;
        else if (code == KeyEvent.KEYCODE_TAB) ch = 0x09;
        else if (code == KeyEvent.KEYCODE_ESCAPE) ch = 0x1b;
        SinkService svc = SinkService.get();
        if (ch > 0 && svc != null && event.getRepeatCount() == 0) {
            svc.sink().key(event.getAction() == KeyEvent.ACTION_DOWN, ch);
            return true;
        }
        return super.dispatchKeyEvent(event);
    }

    private void onBackRequested() {
        long now = System.currentTimeMillis();
        if (now - lastBackPress < 2000) {
            SinkService svc = SinkService.get();
            if (svc != null) svc.disconnect();
            finish();
        } else {
            lastBackPress = now;
            Toast.makeText(this, "Tekan kembali sekali lagi untuk memutus", Toast.LENGTH_SHORT).show();
        }
    }

    /** SurfaceView that keeps the stream's aspect ratio, centred (letterboxed if needed). */
    static final class AspectSurfaceView extends SurfaceView {
        private final float aspect;

        AspectSurfaceView(android.content.Context ctx, int w, int h) {
            super(ctx);
            aspect = (float) w / h;
        }

        @Override
        protected void onMeasure(int widthSpec, int heightSpec) {
            int w = MeasureSpec.getSize(widthSpec);
            int h = MeasureSpec.getSize(heightSpec);
            if (w > h * aspect) w = Math.round(h * aspect);
            else h = Math.round(w / aspect);
            setMeasuredDimension(w, h);
        }
    }
}
