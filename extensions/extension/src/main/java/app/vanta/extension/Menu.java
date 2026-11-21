package app.vanta.extension;

import android.content.Context;
import android.content.Intent;
import android.graphics.PixelFormat;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowManager;
import android.widget.CompoundButton;
import android.widget.LinearLayout;
import android.widget.SeekBar;
import android.widget.Switch;
import android.widget.TextView;

/**
 * Floating overlay: a compact panel of switches that map one-to-one onto the native
 * flag ids in libvanta.so, plus a speed slider.
 */
@SuppressWarnings("unused")
public final class Menu {

    private static final int FLAG_NO_KILL_CD = 0;
    private static final int FLAG_KILL_ANYONE = 1;
    private static final int FLAG_VENT_ANYWHERE = 2;
    private static final int FLAG_UNLOCK_COSMETICS = 3;
    private static final int FLAG_UNLOCK_FEATURES = 4;
    private static final int FLAG_TASK_WIN = 5;
    private static final int FLAG_COMPLETE_TASKS = 6;

    private static final String[] LABELS = {
            "No kill cooldown",
            "Kill anyone",
            "Vent anywhere",
            "Unlock cosmetics",
            "Unlock features",
            "Force task win",
            "Complete my tasks",
    };

    private static Context context;
    private static WindowManager windowManager;
    private static LinearLayout panel;
    private static LinearLayout bubble;
    private static WindowManager.LayoutParams panelParams;
    private static boolean overlayAsked;

    public static native void nativeStart(int versionCode);

    public static native void setFlag(int id, boolean value);

    public static native void setSpeed(float value);

    /** 0 = waiting for the engine, 1 = hooks installed, 2 = unsupported game build. */
    public static native int nativeStatus();

    private Menu() {
    }

    static void attach(Context ctx) {
        if (ctx == null || panel != null) return;
        context = ctx;
        windowManager = (WindowManager) ctx.getSystemService(Context.WINDOW_SERVICE);
        if (windowManager == null) return;

        if (!canOverlay(ctx)) {
            requestOverlay(ctx);
            return;
        }

        try {
            addBubble();
        } catch (Throwable t) {
            panel = null;
        }
    }

    private static boolean canOverlay(Context ctx) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.M) return true;
        return Settings.canDrawOverlays(ctx);
    }

    private static void requestOverlay(Context ctx) {
        if (overlayAsked) return;
        overlayAsked = true;
        try {
            Intent intent = new Intent(Settings.ACTION_MANAGE_OVERLAY_PERMISSION,
                    Uri.parse("package:" + ctx.getPackageName()));
            intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            ctx.startActivity(intent);
        } catch (Throwable ignored) {
        }
        new Handler(Looper.getMainLooper()).postDelayed(new Runnable() {
            @Override
            public void run() {
                overlayAsked = false;
                attach(context);
            }
        }, 4000L);
    }

    private static int dp(Context ctx, int value) {
        return (int) (value * ctx.getResources().getDisplayMetrics().density);
    }

    private static GradientDrawable box(int color, int radiusPx) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(color);
        d.setCornerRadius(radiusPx);
        return d;
    }

    private static WindowManager.LayoutParams params(int w, int h, boolean focusable) {
        WindowManager.LayoutParams p = new WindowManager.LayoutParams(
                w, h,
                Build.VERSION.SDK_INT >= Build.VERSION_CODES.O
                        ? WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY
                        : WindowManager.LayoutParams.TYPE_PHONE,
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                        | WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS,
                PixelFormat.TRANSLUCENT);
        if (focusable) p.flags &= ~WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE;
        p.gravity = Gravity.TOP | Gravity.START;
        p.x = dp(context, 24);
        p.y = dp(context, 120);
        return p;
    }

    private static void addBubble() {
        int size = dp(context, 48);
        bubble = new LinearLayout(context);
        bubble.setGravity(Gravity.CENTER);
        bubble.setBackground(box(0xE6141414, size / 2));
        TextView glyph = new TextView(context);
        glyph.setText("V");
        glyph.setTextColor(0xFFE4312B);
        glyph.setTextSize(20f);
        bubble.addView(glyph);

        final WindowManager.LayoutParams p = params(size, size, false);
        bubble.setOnTouchListener(new DragTouch(p, dp(context, 4)));
        bubble.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                openPanel();
            }
        });

        try {
            windowManager.addView(bubble, p);
        } catch (Throwable t) {
            bubble = null;
        }
    }

    private static void openPanel() {
        if (panel != null) return;
        if (!canOverlay(context)) return;

        panel = new LinearLayout(context);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setPadding(dp(context, 14), dp(context, 12), dp(context, 14), dp(context, 14));
        panel.setBackground(box(0xF0141414, dp(context, 14)));

        TextView title = new TextView(context);
        title.setText("VANTA  |  Among Us");
        title.setTextColor(0xFFE4312B);
        title.setTextSize(15f);
        title.setGravity(Gravity.CENTER_VERTICAL);
        panel.addView(title, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, dp(context, 34)));

        for (int id = 0; id < LABELS.length; id++) {
            // Completing tasks is a one-shot, not a state: a switch would stay lit after the
            // game has already finished the last task.
            panel.addView(id == FLAG_COMPLETE_TASKS ? makeAction(id, LABELS[id]) : makeSwitch(id, LABELS[id]));
        }

        panel.addView(makeSpeedRow());

        TextView close = new TextView(context);
        close.setText("hide");
        close.setTextColor(0xFF9E9E9E);
        close.setTextSize(13f);
        close.setGravity(Gravity.CENTER);
        close.setPadding(0, dp(context, 8), 0, 0);
        close.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                closePanel();
            }
        });
        panel.addView(close);

        panelParams = params(dp(context, 232), WindowManager.LayoutParams.WRAP_CONTENT, true);
        panelParams.x = dp(context, 24);
        panelParams.y = dp(context, 120);
        panel.setOnTouchListener(new DragTouch(panelParams, 0));

        try {
            windowManager.addView(panel, panelParams);
        } catch (Throwable t) {
            panel = null;
        }
    }

    private static View makeSwitch(final int id, String label) {
        Switch row = new Switch(context);
        row.setText(label);
        row.setTextColor(0xFFEDEDED);
        row.setTextSize(13f);
        row.setPadding(0, dp(context, 2), 0, dp(context, 2));
        row.setOnCheckedChangeListener(new CompoundButton.OnCheckedChangeListener() {
            @Override
            public void onCheckedChanged(CompoundButton button, boolean checked) {
                setFlag(id, checked);
            }
        });
        return row;
    }

    private static View makeAction(final int id, String label) {
        TextView row = new TextView(context);
        row.setText(label + "   \u203A");
        row.setTextColor(0xFFEDEDED);
        row.setTextSize(13f);
        row.setPadding(0, dp(context, 10), 0, dp(context, 10));
        row.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                setFlag(id, true);
            }
        });
        return row;
    }

    private static View makeSpeedRow() {
        LinearLayout row = new LinearLayout(context);
        row.setOrientation(LinearLayout.VERTICAL);
        row.setPadding(0, dp(context, 8), 0, 0);

        final TextView label = new TextView(context);
        label.setText("Speed  off");
        label.setTextColor(0xFFEDEDED);
        label.setTextSize(13f);
        row.addView(label);

        SeekBar bar = new SeekBar(context);
        bar.setMax(24);  // 0 = off, then 0.5 .. 12.0 in 0.5 steps
        bar.setProgress(0);
        bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                float value = progress * 0.5f;
                label.setText("Speed  " + (value <= 0.0f ? "off" : format(value)));
                setSpeed(value);
            }

            @Override
            public void onStartTrackingTouch(SeekBar seekBar) {
            }

            @Override
            public void onStopTrackingTouch(SeekBar seekBar) {
            }
        });
        row.addView(bar);
        return row;
    }

    private static String format(float value) {
        return String.format(java.util.Locale.US, "%.1fx", value);
    }

    private static void closePanel() {
        if (panel == null) return;
        try {
            windowManager.removeView(panel);
        } catch (Throwable ignored) {
        }
        panel = null;
    }

    /** Drags the attached view by moving its LayoutParams, with a small touch slop. */
    private static final class DragTouch implements View.OnTouchListener {
        private final WindowManager.LayoutParams params;
        private final int slop;
        private int startX;
        private int startY;
        private float downX;
        private float downY;
        private boolean dragging;

        DragTouch(WindowManager.LayoutParams params, int slop) {
            this.params = params;
            this.slop = slop;
        }

        @Override
        public boolean onTouch(View view, MotionEvent event) {
            switch (event.getActionMasked()) {
                case MotionEvent.ACTION_DOWN:
                    startX = params.x;
                    startY = params.y;
                    downX = event.getRawX();
                    downY = event.getRawY();
                    dragging = false;
                    return false;
                case MotionEvent.ACTION_MOVE:
                    float dx = event.getRawX() - downX;
                    float dy = event.getRawY() - downY;
                    if (!dragging && (Math.abs(dx) > slop || Math.abs(dy) > slop)) {
                        dragging = true;
                    }
                    if (dragging) {
                        params.x = startX + (int) dx;
                        params.y = startY + (int) dy;
                        try {
                            windowManager.updateViewLayout(view, params);
                        } catch (Throwable ignored) {
                        }
                        return true;
                    }
                    return false;
                default:
                    return false;
            }
        }
    }
}
