package com.xcast.display;

import android.content.Context;
import android.content.pm.PackageManager;
import android.provider.Settings;
import android.util.Log;

import java.lang.reflect.Method;

import rikka.shizuku.Shizuku;

/**
 * Starts the privileged helper as the shell user. The helper classes live in this APK's dex, so
 * app_process can load them straight from the installed APK (same trick scrcpy uses).
 */
final class HelperLauncher {
    private static final String TAG = "XCast";
    static final int SHIZUKU_REQUEST = 7236;

    private HelperLauncher() {
    }

    static String deviceName(Context ctx) {
        String name = Settings.Global.getString(ctx.getContentResolver(), "device_name");
        return name == null || name.isEmpty() ? "Android phone" : name;
    }

    /** Shell command that launches the helper detached from the caller. */
    static String command(Context ctx) {
        String apk = ctx.getApplicationInfo().sourceDir;
        String name = deviceName(ctx).replace("'", "");
        return "CLASSPATH=" + apk + " setsid app_process / com.xcast.helper.HelperMain"
                + " --name '" + name + "' --app " + ctx.getPackageName()
                + " </dev/null >/data/local/tmp/xcast-helper.log 2>&1 &";
    }

    /** Command for a PC: adb shell "<this>". */
    static String adbCommand(Context ctx) {
        return "adb shell \"" + command(ctx).replace("\"", "\\\"") + "\"";
    }

    static boolean shizukuAvailable() {
        try {
            return Shizuku.pingBinder() && !Shizuku.isPreV11();
        } catch (Throwable t) {
            return false;
        }
    }

    static boolean shizukuGranted() {
        try {
            return Shizuku.checkSelfPermission() == PackageManager.PERMISSION_GRANTED;
        } catch (Throwable t) {
            return false;
        }
    }

    static void requestShizuku() {
        try {
            Shizuku.requestPermission(SHIZUKU_REQUEST);
        } catch (Throwable t) {
            Log.w(TAG, "Shizuku permission request failed", t);
        }
    }

    /** Returns true if the launch command was dispatched through Shizuku. */
    static boolean launchViaShizuku(Context ctx) {
        if (!shizukuAvailable() || !shizukuGranted()) return false;
        try {
            // Shizuku.newProcess is hidden in API 13 but still the simplest way to run a shell command.
            Method m = Shizuku.class.getDeclaredMethod("newProcess", String[].class, String[].class, String.class);
            m.setAccessible(true);
            Process p = (Process) m.invoke(null, new String[]{"sh", "-c", command(ctx)}, null, null);
            p.waitFor();
            return true;
        } catch (Throwable t) {
            Log.e(TAG, "Shizuku launch failed", t);
            return false;
        }
    }
}
