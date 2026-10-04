package com.xcast.helper;

import android.content.AttributionSource;
import android.content.Context;
import android.content.ContextWrapper;

import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;

/**
 * Context for a process started with app_process as the shell user (uid 2000).
 * Framework services validate that the calling uid owns the package in the
 * AttributionSource, so everything is attributed to com.android.shell.
 */
public final class FakeContext extends ContextWrapper {
    public static final String PACKAGE_NAME = "com.android.shell";
    public static final int SHELL_UID = 2000;

    private static FakeContext instance;

    public static synchronized FakeContext get() {
        if (instance == null) {
            instance = new FakeContext(createSystemContext());
        }
        return instance;
    }

    private FakeContext(Context base) {
        super(base);
    }

    private static Context createSystemContext() {
        try {
            Class<?> atClass = Class.forName("android.app.ActivityThread");
            Constructor<?> ctor = atClass.getDeclaredConstructor();
            ctor.setAccessible(true);
            Object thread = ctor.newInstance();
            Field current = atClass.getDeclaredField("sCurrentActivityThread");
            current.setAccessible(true);
            current.set(null, thread);
            Method getSystemContext = atClass.getDeclaredMethod("getSystemContext");
            return (Context) getSystemContext.invoke(thread);
        } catch (ReflectiveOperationException e) {
            throw new RuntimeException("Cannot create system context", e);
        }
    }

    @Override
    public String getPackageName() {
        return PACKAGE_NAME;
    }

    @Override
    public String getOpPackageName() {
        return PACKAGE_NAME;
    }

    @Override
    public AttributionSource getAttributionSource() {
        return new AttributionSource.Builder(SHELL_UID).setPackageName(PACKAGE_NAME).build();
    }

    @Override
    public Context getApplicationContext() {
        return this;
    }

    // WifiP2pManager builds its AttributionSource from createDeviceContext(0); without this
    // override it would come from the bare system context (package "android").
    @Override
    public Context createDeviceContext(int deviceId) {
        return this;
    }
}
