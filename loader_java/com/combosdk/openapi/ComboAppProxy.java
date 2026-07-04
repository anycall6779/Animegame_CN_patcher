package com.combosdk.openapi;

import android.util.Log;

public class ComboAppProxy extends ComboApplication {
    private static final String TAG = "ANIMEGAME_LOCALIFY";
    private static boolean nativeLoaded;

    static {
        try {
            System.loadLibrary("animegame_native_localify");
            nativeLoaded = true;
            Log.i(TAG, "[ANIMEGAME_ANDROID] Java loader loaded native library");
        } catch (Throwable e) {
            Log.e(TAG, "[ANIMEGAME_ANDROID] Java loader failed to load native library", e);
        }
    }

    private static native void nativeSetApkPath(String apkPath);

    @Override
    public void onCreate() {
        if (nativeLoaded) {
            try {
                nativeSetApkPath(getApplicationInfo().sourceDir);
            } catch (Throwable e) {
                Log.e(TAG, "[ANIMEGAME_ANDROID] Java loader failed to pass apk path", e);
            }
        }
        super.onCreate();
    }
}
