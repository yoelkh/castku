package com.xcast.display;

import android.view.Surface;
import android.view.SurfaceControl;

/** Thin JNI wrapper around the native Miracast sink (RTSP + RTP/TS + decoder + UIBC). */
public final class NativeSink {
    public static final int EVENT_PLAYING = 1;
    public static final int EVENT_ENDED = 2;
    public static final int EVENT_UIBC_ON = 3;
    public static final int EVENT_UIBC_OFF = 4;
    public static final int EVENT_DECODER = 5;

    public static final int TOUCH_DOWN = 0;
    public static final int TOUCH_UP = 1;
    public static final int TOUCH_MOVE = 2;

    public interface Listener {
        /** Called on native worker threads. */
        void onSinkEvent(int type, String msg);
    }

    static {
        System.loadLibrary("xcast");
    }

    private final Listener listener;
    private long handle;

    public NativeSink(Listener listener) {
        this.listener = listener;
        handle = nativeCreate();
    }

    @SuppressWarnings("unused") // called from native code
    private void onNativeEvent(int type, String msg) {
        listener.onSinkEvent(type, msg);
    }

    public boolean start(String sourceIp, int rtspPort, int width, int height, int fps, boolean audio,
                         String name, String model, int touchMode) {
        return nativeStart(handle, sourceIp, rtspPort, width, height, fps, audio, name, model, touchMode);
    }

    public void stop() {
        nativeStop(handle);
    }

    public void setSurface(Surface surface) {
        nativeSetSurface(handle, surface);
    }

    public void touch(int action, int count, int[] ids, float[] xs, float[] ys) {
        nativeTouch(handle, action, count, ids, xs, ys);
    }

    /** Parent layer for the hardware cursor: the SurfaceView's SurfaceControl and its size (null detaches). */
    public void setCursorParent(SurfaceControl sc, int width, int height) {
        nativeSetCursorParent(handle, sc, width, height);
    }

    public void key(boolean down, int code) {
        nativeKey(handle, down, code);
    }

    public String stats() {
        return nativeStats(handle);
    }

    public void release() {
        if (handle != 0) {
            nativeDestroy(handle);
            handle = 0;
        }
    }

    private native long nativeCreate();
    private native boolean nativeStart(long h, String sourceIp, int rtspPort, int width, int height, int fps,
                                       boolean audio, String name, String model, int touchMode);
    private native void nativeStop(long h);
    private native void nativeSetSurface(long h, Surface surface);
    private native void nativeTouch(long h, int action, int count, int[] ids, float[] xs, float[] ys);
    private native void nativeSetCursorParent(long h, SurfaceControl sc, int width, int height);
    private native void nativeKey(long h, boolean down, int code);
    private native String nativeStats(long h);
    private native void nativeDestroy(long h);
}
