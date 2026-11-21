package app.vanta.extension;

import android.content.Context;
import android.content.pm.PackageInfo;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

/**
 * Entry point injected into the game's activity. Loads libvanta.so and keeps retrying the
 * native install until libil2cpp.so is mapped, then hands over to the menu overlay.
 */
@SuppressWarnings("unused")
public final class Loader {

    private static final String TAG = "vanta";

    private static final long RETRY_MS = 1200L;
    private static final int MAX_RETRIES = 60;

    private static boolean installed;
    private static Context appContext;
    private static int versionCode;
    private static int attempts;

    public static void install(Context context) {
        if (installed || context == null) return;
        installed = true;

        try {
            appContext = context.getApplicationContext();
            if (appContext == null) appContext = context;
            versionCode = readVersionCode(appContext);

            System.loadLibrary("vanta");
        } catch (Throwable t) {
            // Never take the game down with us: a failed load leaves it unpatched.
            return;
        }

        Handler handler = new Handler(Looper.getMainLooper());
        handler.postDelayed(new Runnable() {
            @Override
            public void run() {
                tick();
            }
        }, RETRY_MS * 2);
    }

    private static void tick() {
        if (++attempts > MAX_RETRIES) return;

        Menu.nativeStart(versionCode);

        int status = Menu.nativeStatus();
        if (status == 1) {
            Menu.attach(appContext);
            return;
        }
        if (status == 2) {
            // The offset table does not cover this build: the menu would only be decoration.
            Log.w(TAG, "libvanta has no offset table for versionCode " + versionCode);
            return;
        }

        new Handler(Looper.getMainLooper()).postDelayed(new Runnable() {
            @Override
            public void run() {
                tick();
            }
        }, RETRY_MS);
    }

    private static int readVersionCode(Context context) {
        try {
            PackageInfo info = context.getPackageManager()
                    .getPackageInfo(context.getPackageName(), 0);
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
                return (int) info.getLongVersionCode();
            }
            return info.versionCode;
        } catch (Throwable t) {
            return 0;
        }
    }
}
