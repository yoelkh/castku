package com.xcast.display;

import android.content.Context;
import android.content.SharedPreferences;

/**
 * Stream modes offered to Windows via microsoft_custom_video_formats. The Windows encoder caps
 * H.264 at level 4.2 (522,240 macroblocks/s), so resolution × refresh must fit that budget.
 * All modes keep the phone's 20:9 aspect ratio (1268×2756 panel) so nothing is letterboxed.
 */
enum Quality {
    SMOOTH("Smooth · 2176×1000 @ 60 fps", 2176, 1000, 60),
    // Multiples of 16 so the encoder needs no cropping; the hardware composer scales to 2756×1268.
    SHARP("Sharp · 2752×1264 @ 30 fps", 2752, 1264, 30),
    FAST("Ultra-fast · 1536×704 @ 120 fps", 1536, 704, 120);

    final String label;
    final int width;
    final int height;
    final int fps;

    Quality(String label, int width, int height, int fps) {
        this.label = label;
        this.width = width;
        this.height = height;
        this.fps = fps;
    }

    static final String PREFS = "settings";

    static Quality load(Context ctx) {
        String v = prefs(ctx).getString("quality", SMOOTH.name());
        try {
            return valueOf(v);
        } catch (IllegalArgumentException e) {
            return SMOOTH;
        }
    }

    static void save(Context ctx, Quality q) {
        prefs(ctx).edit().putString("quality", q.name()).apply();
    }

    static SharedPreferences prefs(Context ctx) {
        return ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }
}
