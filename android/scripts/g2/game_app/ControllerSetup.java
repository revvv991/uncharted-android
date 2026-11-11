package dev.unchartedg2.game;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.Dialog;
import android.content.Context;
import android.content.pm.ActivityInfo;
import android.content.res.ColorStateList;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.RippleDrawable;
import android.hardware.input.InputManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.TypedValue;
import android.view.ContextThemeWrapper;
import android.view.Gravity;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;
import java.io.File;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Map;

/**
 * g2-arm64 (issue #16): the controller setup window. The launcher's Controller setup button and
 * the in-game panel's item (GameActivity.openControllerSetup, called by the port) open it over
 * their activity. It draws the game's on-screen gamepad (gpu/shim/bbport_touchpad.cpp's layout,
 * with a right stick added): a tap on a control, then a press on the controller assigns that
 * input to it; Set up all asks for every control in turn. Each change is written to
 * files/controllers.txt (PadMapping) at once; the game applies it when the controller connects,
 * or when this window closes over the game.
 *
 * The window reads the controller itself (Android's key and motion events; while it is shown
 * over the game, SDL is paused and gets none of them), in SDL's numbers. Where an axis rests is
 * learned from its events: Android sends none for an axis that has not moved, so an axis first
 * met mid-push is no answer until it has stood still at a resting place (the middle, or an end
 * for a trigger); a press is answered when the axis comes back to rest.
 */
final class ControllerSetup extends Dialog implements InputManager.InputDeviceListener {
    private static ControllerSetup shown;

    /** Opens the window over the activity (one at a time). */
    static void show(Activity activity) {
        if (shown != null && shown.isShowing()) return;
        shown = new ControllerSetup(activity);
        shown.show();
    }

    private static final String[] PROMPTS = {
        "Press the button for Cross: the bottom face button (A on most pads).",
        "Press the button for Circle: the right face button (B).",
        "Press the button for Square: the left face button (X).",
        "Press the button for Triangle: the top face button (Y).",
        "Press D-pad up.", "Press D-pad down.", "Press D-pad left.", "Press D-pad right.",
        "Press L1: the left shoulder button (LB).", "Press R1: the right shoulder button (RB).",
        "Press L2: the left trigger (LT).", "Press R2: the right trigger (RT).",
        "Press L3: click the left stick in.", "Press R3: click the right stick in.",
        "Press the button for Options: Start or Menu.",
        "Press the button for the touch pad: Select, Back, View or Share.",
        "Push the left stick to the right.", "Push the left stick down.",
        "Push the right stick to the right.", "Push the right stick down.",
    };

    // An axis answers once it has gone half its range from where it rests and comes back within a
    // quarter of it. It rests where it stands still for a while at a resting place: the middle, or
    // the bottom of a trigger's range (Android reports a trigger as 0..1, SDL's -1..1 here).
    private static final float PRESSED = 0.5f, RELEASED = 0.25f, MOVED = 0.02f;
    private static final long STILL_MS = 400, STILL_AGAIN_MS = 1200, KEY_WAIT_MS = 250, TICK_MS = 100;

    private static final int BG = 0xFF0E0F12, TEXT = 0xFFF3F3F5, MUTED = 0xA8FFFFFF;

    private final Activity activity;
    private final File dir;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final InputManager input;
    private int savedOrientation = Integer.MIN_VALUE;
    private int accent, lightAccent, onAccent, highlight, lightHighlight;

    private Map<String, PadMapping.Entry> entries;
    private List<PadMapping.Device> devices = new ArrayList<>();
    private PadMapping.Device dev; // the controller shown; null: none connected
    private String[] sources = new String[PadMapping.COUNT], defaults = new String[PadMapping.COUNT];

    // The answer waited for: the field, the ones after it (Set up all; a stick's second axis).
    private int target = -1;
    private final ArrayDeque<Integer> queue = new ArrayDeque<>();
    private int queueTotal, queueDone;
    private boolean letGo;          // an input is past its press: waiting for the release
    private int pendingKey = -1;    // a trigger's key: answered at its release unless an axis did
    private long pendingUpAt;
    private String message = "";    // the last change, or why an input was no answer

    // The controller's state, in SDL's numbers.
    private final boolean[] keyHeld = new boolean[36], hatHeld = new boolean[4];
    private float[] value = new float[0], rest = new float[0], lo = new float[0], hi = new float[0];
    private boolean[] seen = new boolean[0], known = new boolean[0];
    private long[] changed = new long[0];

    private PadView pad;
    private LinearLayout info, buttons;
    private TextView deviceText, promptText;

    private final Runnable tick = new Runnable() {
        @Override
        public void run() {
            onTick();
            handler.postDelayed(this, TICK_MS);
        }
    };

    private ControllerSetup(Activity a) {
        super(a, android.R.style.Theme_Black_NoTitleBar_Fullscreen);
        activity = a;
        File d = a.getExternalFilesDir(null);
        dir = d != null ? d : a.getFilesDir();
        input = a.getSystemService(InputManager.class);
    }

    // ---- the window ----

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        initPalette();
        Window w = getWindow();
        w.setBackgroundDrawable(new ColorDrawable(BG));
        w.setLayout(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT);
        WindowManager.LayoutParams lp = w.getAttributes();
        lp.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        w.setAttributes(lp);
        w.setDecorFitsSystemWindows(false);

        pad = new PadView(getContext());
        info = new LinearLayout(getContext());
        info.setOrientation(LinearLayout.VERTICAL);
        info.setGravity(Gravity.CENTER_HORIZONTAL);
        TextView title = text(info, "Controller setup", 20, true);
        title.setGravity(Gravity.CENTER);
        deviceText = text(info, "", 14, false);
        deviceText.setGravity(Gravity.CENTER);
        deviceText.setOnClickListener(v -> switchDevice());
        promptText = text(info, "", 15, false);
        promptText.setGravity(Gravity.CENTER);
        promptText.setLineSpacing(0, 1.12f);
        promptText.setPadding(0, dp(6), 0, dp(2));
        buttons = new LinearLayout(getContext());
        buttons.setOrientation(LinearLayout.HORIZONTAL);
        buttons.setGravity(Gravity.CENTER);
        // Baseline alignment with the buttons' margins measured the row short: it cut them off.
        buttons.setBaselineAligned(false);
        buttons.setPadding(0, dp(8), 0, dp(4));
        info.addView(buttons, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                                                            ViewGroup.LayoutParams.WRAP_CONTENT));
        Screen screen = new Screen(getContext());
        screen.addView(pad);
        screen.addView(info);
        setContentView(screen);

        // The drawn gamepad is the game's landscape layout: a launcher turned to portrait turns.
        int orientation = activity.getRequestedOrientation();
        if (orientation != ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
                && orientation != ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE
                && orientation != ActivityInfo.SCREEN_ORIENTATION_USER_LANDSCAPE) {
            savedOrientation = orientation;
            activity.setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
        }
        entries = PadMapping.load(dir);
        refreshDevices();
    }

    @Override
    protected void onStart() {
        super.onStart();
        if (input != null) input.registerInputDeviceListener(this, handler);
        handler.post(tick);
        hideBars();
    }

    @Override
    protected void onStop() {
        if (input != null) input.unregisterInputDeviceListener(this);
        handler.removeCallbacks(tick);
        if (savedOrientation != Integer.MIN_VALUE) {
            activity.setRequestedOrientation(savedOrientation);
            savedOrientation = Integer.MIN_VALUE;
        }
        if (shown == this) shown = null;
        super.onStop();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) hideBars();
    }

    /** Over the game the system bars stay hidden, as the game keeps them (GameActivity). */
    private void hideBars() {
        WindowInsetsController bars = getWindow().getInsetsController();
        if (bars != null) {
            bars.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            bars.hide(WindowInsets.Type.systemBars());
        }
    }

    @Override
    public void onBackPressed() {
        if (target >= 0) stopWaiting();
        else dismiss();
    }

    private void initPalette() {
        // The launcher's accent (the system's colour), for the window over the game too.
        Context themed = new ContextThemeWrapper(activity, android.R.style.Theme_DeviceDefault);
        TypedValue v = new TypedValue();
        accent = 0xFF3D8BFF;
        if (themed.getTheme().resolveAttribute(android.R.attr.colorAccent, v, true)) {
            if (v.type >= TypedValue.TYPE_FIRST_COLOR_INT && v.type <= TypedValue.TYPE_LAST_COLOR_INT)
                accent = v.data;
            else if (v.resourceId != 0) accent = themed.getColor(v.resourceId);
        }
        lightAccent = mix(accent, Color.WHITE, 0.45f);
        // The control waited for and the inputs changed here stand out in the accent; a grey one
        // (the G2's system accent is near white) would not set them apart: the launcher's blue.
        float[] hsv = new float[3];
        Color.colorToHSV(accent, hsv);
        highlight = hsv[1] < 0.25f ? 0xFF3D8BFF : accent;
        lightHighlight = mix(highlight, Color.WHITE, 0.35f);
        double luma = 0.299 * Color.red(accent) + 0.587 * Color.green(accent) + 0.114 * Color.blue(accent);
        onAccent = luma > 150 ? 0xFF14161A : Color.WHITE;
    }

    private static int mix(int a, int b, float t) {
        return Color.rgb(Math.round(Color.red(a) + (Color.red(b) - Color.red(a)) * t),
                         Math.round(Color.green(a) + (Color.green(b) - Color.green(a)) * t),
                         Math.round(Color.blue(a) + (Color.blue(b) - Color.blue(a)) * t));
    }

    private static int alpha(int colour, int a) {
        return (colour & 0x00FFFFFF) | (Math.max(0, Math.min(255, a)) << 24);
    }

    private int dp(float v) {
        return Math.round(v * getContext().getResources().getDisplayMetrics().density);
    }

    private TextView text(ViewGroup parent, String s, float size, boolean bold) {
        TextView t = new TextView(getContext());
        t.setText(s);
        t.setTextSize(size);
        t.setTextColor(TEXT);
        if (bold) t.setTypeface(Typeface.DEFAULT_BOLD);
        parent.addView(t, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                                                        ViewGroup.LayoutParams.WRAP_CONTENT));
        return t;
    }

    private Button button(String s, boolean primary, View.OnClickListener l) {
        Button b = new Button(getContext(), null, android.R.attr.borderlessButtonStyle);
        b.setText(s);
        b.setAllCaps(false);
        b.setOnClickListener(l);
        b.setTextSize(14.5f);
        b.setMinWidth(0);
        b.setMinHeight(0);
        b.setMinimumWidth(dp(84));
        b.setMinimumHeight(dp(42));
        b.setPadding(dp(16), 0, dp(16), 0);
        GradientDrawable bg = new GradientDrawable();
        bg.setCornerRadius(dp(21));
        if (primary) {
            bg.setColor(accent);
            b.setTextColor(onAccent);
            b.setTypeface(Typeface.DEFAULT_BOLD);
        } else {
            bg.setColor(Color.TRANSPARENT);
            bg.setStroke(dp(1), alpha(lightAccent, 0x99));
            b.setTextColor(new ColorStateList(new int[][] {{-android.R.attr.state_enabled}, {}},
                                              new int[] {0x66FFFFFF, lightAccent}));
        }
        b.setBackground(new RippleDrawable(ColorStateList.valueOf(0x33FFFFFF), bg, null));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                                                                     ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.setMargins(dp(4), 0, dp(4), 0);
        buttons.addView(b, lp);
        return b;
    }

    /** The drawn gamepad over the whole window; the text and buttons in the free middle, below
     *  the controls at the top and above the right stick. */
    private final class Screen extends ViewGroup {
        Screen(Context c) {
            super(c);
        }

        private int infoWidth(int w, int h) {
            return Math.max(Math.min(w - Math.round(0.92f * h), dp(560)), Math.min(dp(240), w));
        }

        @Override
        protected void onMeasure(int ws, int hs) {
            int w = MeasureSpec.getSize(ws), h = MeasureSpec.getSize(hs);
            pad.measure(MeasureSpec.makeMeasureSpec(w, MeasureSpec.EXACTLY), MeasureSpec.makeMeasureSpec(h, MeasureSpec.EXACTLY));
            info.measure(MeasureSpec.makeMeasureSpec(infoWidth(w, h), MeasureSpec.EXACTLY),
                         MeasureSpec.makeMeasureSpec(Math.round(h * 0.55f), MeasureSpec.AT_MOST));
            setMeasuredDimension(w, h);
        }

        @Override
        protected void onLayout(boolean changed, int l, int t, int r, int b) {
            int w = r - l, h = b - t;
            pad.layout(0, 0, w, h);
            int iw = info.getMeasuredWidth(), x = (w - iw) / 2, y = Math.round(h * 0.13f);
            info.layout(x, y, x + iw, y + info.getMeasuredHeight());
        }
    }

    // ---- what is shown ----

    private void update() {
        if (promptText == null) return;
        if (dev == null) {
            deviceText.setText("");
            promptText.setText("No controller found. Connect one in its gamepad mode (not a keyboard or mouse "
                               + "mode): it shows up here.");
        } else {
            int index = devices.indexOf(dev);
            String name = dev.name;
            if (devices.size() > 1) name += "   (" + (index + 1) + " of " + devices.size() + ": tap to switch)";
            deviceText.setText(name);
            deviceText.setTextColor(devices.size() > 1 ? lightAccent : MUTED);
            StringBuilder s = new StringBuilder();
            if (target >= 0) {
                if (queueTotal > 1) s.append(queueDone + 1).append(" of ").append(queueTotal).append(": ");
                s.append(letGo ? "Got it: let go." : PROMPTS[target]);
            } else if (allUnset()) {
                s.append("The game reads nothing from this controller yet. Set up all asks for each control in "
                         + "turn; or tap a control, then press the controller's button for it.");
            } else if (!faceButtons(sources)) {
                s.append("The game reads none of this controller's buttons yet (SDL's own layout of it names "
                         + "buttons it does not have). Set up all asks for each control in turn; or tap a "
                         + "control, then press the controller's button for it.");
            } else {
                s.append("Tap a control, then press the controller's button for it. Its controls light up "
                         + "as the controller's buttons are pressed.");
            }
            if (!message.isEmpty()) s.append("\n").append(message);
            promptText.setText(s);
        }
        buttons.removeAllViews();
        if (dev != null && target >= 0) {
            button("Clear", false, v -> clearTarget());
            if (!queue.isEmpty()) button("Skip", false, v -> next());
            button("Cancel", false, v -> stopWaiting());
        } else {
            if (dev != null) {
                button("Set up all", false, v -> begin(allFields()));
                button("Reset", false, v -> confirmReset()).setEnabled(entries.containsKey(dev.key));
            }
            button("Done", true, v -> dismiss());
        }
        pad.invalidate();
    }

    /** Nothing the game would read: no source, or only inputs the device lacks (SDL's own mapping
     *  of a pad in a PC mode with 13 buttons or more). */
    private boolean allUnset() {
        for (String s : sources)
            if (PadMapping.exists(dev, s)) return false;
        return true;
    }

    /** Whether a layout reads any of the device's face buttons. */
    private boolean faceButtons(String[] layout) {
        for (int f : new int[] {PadMapping.A, PadMapping.B, PadMapping.X, PadMapping.Y})
            if (PadMapping.exists(dev, layout[f])) return true;
        return false;
    }

    private boolean unset(int field) {
        return dev != null && !PadMapping.exists(dev, sources[field]);
    }

    private static int[] allFields() {
        int[] all = new int[PadMapping.COUNT];
        for (int i = 0; i < all.length; i++) all[i] = i;
        return all;
    }

    // ---- controllers ----

    @Override
    public void onInputDeviceAdded(int id) {
        refreshDevices();
    }

    @Override
    public void onInputDeviceRemoved(int id) {
        refreshDevices();
    }

    @Override
    public void onInputDeviceChanged(int id) {
        refreshDevices();
    }

    /** The controllers again: the one shown stays, else the newest (an external pad on a device
     *  with built-in controls). */
    private void refreshDevices() {
        devices = PadMapping.devices();
        if (dev != null) {
            for (int i = 0; i < devices.size(); i++) {
                if (devices.get(i).id == dev.id) {
                    devices.set(i, dev);
                    update();
                    return;
                }
            }
        }
        PadMapping.Device newest = null;
        for (PadMapping.Device d : devices)
            if (newest == null || d.id > newest.id) newest = d;
        select(newest);
    }

    private void switchDevice() {
        if (devices.size() < 2 || dev == null) return;
        select(devices.get((devices.indexOf(dev) + 1) % devices.size()));
    }

    private void select(PadMapping.Device d) {
        dev = d;
        target = -1;
        queue.clear();
        queueTotal = 0;
        letGo = false;
        pendingKey = -1;
        message = "";
        Arrays.fill(keyHeld, false);
        Arrays.fill(hatHeld, false);
        int n = d != null ? d.axes.size() : 0;
        value = new float[n];
        rest = new float[n];
        lo = new float[n];
        hi = new float[n];
        seen = new boolean[n];
        known = new boolean[n];
        changed = new long[n];
        if (d != null) {
            defaults = PadMapping.defaults(d);
            PadMapping.Entry e = entries.get(d.key);
            sources = e != null ? e.sources.clone() : defaults.clone();
        } else {
            defaults = new String[PadMapping.COUNT];
            sources = new String[PadMapping.COUNT];
        }
        update();
    }

    private PadMapping.Device deviceOf(int id) {
        for (PadMapping.Device d : devices)
            if (d.id == id) return d;
        return null;
    }

    // ---- answers ----

    private void begin(int... fields) {
        if (dev == null || fields.length == 0) return;
        queue.clear();
        for (int i = 1; i < fields.length; i++) queue.add(fields[i]);
        queueTotal = fields.length;
        queueDone = 0;
        message = "";
        listen(fields[0]);
    }

    private void listen(int field) {
        target = field;
        letGo = false;
        pendingKey = -1;
        freshAxes();
        update();
    }

    /** An axis's travel counts from now. */
    private void freshAxes() {
        for (int i = 0; i < value.length; i++) lo[i] = hi[i] = value[i];
    }

    private void next() {
        queueDone++;
        Integer field = queue.poll();
        if (field != null) {
            listen(field);
        } else {
            target = -1;
            queueTotal = 0;
            letGo = false;
            pendingKey = -1;
            update();
        }
    }

    private void stopWaiting() {
        target = -1;
        queue.clear();
        queueTotal = 0;
        letGo = false;
        pendingKey = -1;
        update();
    }

    private void clearTarget() {
        if (target < 0) return;
        sources[target] = null;
        message = PadMapping.NAMES[target] + ": not set";
        store();
        next();
    }

    /** The answer for the target: the input's other user takes the target's old input (a swap,
     *  as on a pad where two buttons trade places). */
    private void answer(String src) {
        int t = target;
        if (t < 0 || dev == null) return;
        String old = sources[t];
        int other = -1;
        for (int j = 0; j < PadMapping.COUNT; j++) {
            if (j == t || !PadMapping.overlaps(sources[j], src)) continue;
            sources[j] = PadMapping.fits(j, old) && !PadMapping.overlaps(old, src) ? old : null;
            other = j;
        }
        sources[t] = src;
        String m = PadMapping.NAMES[t] + ": " + PadMapping.label(dev, src);
        if (other >= 0)
            m += sources[other] != null ? " (" + PadMapping.NAMES[other] + " took " + PadMapping.label(dev, sources[other]) + ")"
                                        : " (" + PadMapping.NAMES[other] + " is not set now)";
        message = m;
        store();
        next();
    }

    private void store() {
        PadMapping.Entry e = new PadMapping.Entry();
        e.name = dev.name;
        System.arraycopy(sources, 0, e.sources, 0, PadMapping.COUNT);
        entries.put(dev.key, e);
        if (!PadMapping.save(dir, entries))
            message = "Could not write " + new File(dir, PadMapping.FILE) + ": the setup is lost when this window closes.";
    }

    private void confirmReset() {
        if (dev == null) return;
        boolean any = false;
        for (String s : defaults)
            if (PadMapping.exists(dev, s)) any = true;
        String what = faceButtons(defaults) ? "Back to its own layout (SDL's mapping of this controller)."
                      : any ? "Back to SDL's own layout of it, which reads none of its buttons."
                            : "Nothing is set for it then: the game does not read it.";
        new AlertDialog.Builder(getContext(), android.R.style.Theme_DeviceDefault_Dialog_Alert)
            .setTitle("Reset " + dev.name + "?")
            .setMessage(what)
            .setPositiveButton("Reset", (d, w) -> {
                entries.remove(dev.key);
                if (!PadMapping.save(dir, entries))
                    message = "Could not write " + new File(dir, PadMapping.FILE) + ".";
                else message = "Reset.";
                sources = defaults.clone();
                update();
            })
            .setNegativeButton("Cancel", null)
            .show();
    }

    // ---- the controller's input ----

    @Override
    public boolean dispatchKeyEvent(KeyEvent e) {
        PadMapping.Device d = deviceOf(e.getDeviceId());
        if (d == null) return super.dispatchKeyEvent(e); // the screen's Back, a keyboard
        boolean down = e.getAction() == KeyEvent.ACTION_DOWN;
        if (e.getAction() != KeyEvent.ACTION_DOWN && e.getAction() != KeyEvent.ACTION_UP) return true;
        if (d != dev) {
            // A press on another controller shows that one (not while an answer is waited for).
            if (!down || e.getRepeatCount() > 0 || target >= 0) return true;
            select(d);
        }
        int n = PadMapping.sdlButton(e.getKeyCode(), e.getScanCode());
        if (n < 0) {
            // SDL passes it on as a keyboard's key: no answer (a pad's volume keys work as ever).
            if (target < 0) return super.dispatchKeyEvent(e);
            if (down) {
                message = KeyEvent.keyCodeToString(e.getKeyCode()).replace("KEYCODE_", "")
                          + " is no gamepad button for the game (a keyboard's key).";
                update();
            }
            return true;
        }
        keyHeld[n] = down;
        if (down && e.getRepeatCount() == 0 && target >= 0 && !PadMapping.isStick(target)) {
            if (PadMapping.isTrigger(target)) {
                // A trigger's key is answered at its release, unless the trigger's axis answered:
                // an analog trigger often sends a key too (BUTTON_L2), and the axis is the better one.
                pendingKey = n;
                pendingUpAt = 0;
                letGo = true;
                update();
            } else {
                answer("b" + n);
            }
        } else if (!down && n == pendingKey) {
            pendingUpAt = SystemClock.uptimeMillis();
        }
        pad.invalidate();
        return true;
    }

    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent e) {
        if ((e.getSource() & InputDevice.SOURCE_CLASS_JOYSTICK) == 0) return super.dispatchGenericMotionEvent(e);
        PadMapping.Device d = deviceOf(e.getDeviceId());
        if (d == null) return super.dispatchGenericMotionEvent(e);
        if (d != dev || e.getActionMasked() != MotionEvent.ACTION_MOVE) return true;
        long now = SystemClock.uptimeMillis();
        int history = e.getHistorySize();
        for (int h = 0; h <= history; h++) {
            for (int i = 0; i < dev.axes.size(); i++) {
                InputDevice.MotionRange r = dev.axes.get(i);
                float raw = h < history ? e.getHistoricalAxisValue(r.getAxis(), h) : e.getAxisValue(r.getAxis());
                float range = r.getRange();
                // SDL's normalization (SDLJoystickHandler.handleMotionEvent)
                sample(i, range > 0 ? (raw - r.getMin()) / range * 2f - 1f : 0f, now);
            }
            if (dev.hats.size() >= 2) {
                float x = h < history ? e.getHistoricalAxisValue(MotionEvent.AXIS_HAT_X, h) : e.getAxisValue(MotionEvent.AXIS_HAT_X);
                float y = h < history ? e.getHistoricalAxisValue(MotionEvent.AXIS_HAT_Y, h) : e.getAxisValue(MotionEvent.AXIS_HAT_Y);
                hat(Math.round(x), Math.round(y));
            }
        }
        if (target >= 0) checkAxes();
        pad.invalidate();
        return true;
    }

    /** The hat as SDL's D-pad buttons 11..14 (up, down, left, right); a diagonal answers once. */
    private void hat(int x, int y) {
        boolean[] now = {y < 0, y > 0, x < 0, x > 0};
        int pressed = -1;
        for (int k = 0; k < 4; k++) {
            if (now[k] && !hatHeld[k] && pressed < 0) pressed = k;
            hatHeld[k] = now[k];
        }
        if (pressed >= 0 && target >= 0 && !PadMapping.isStick(target)) answer("b" + (11 + pressed));
    }

    private static boolean restingPlace(float v) {
        return Math.abs(v) < RELEASED || v < -0.9f;
    }

    private void sample(int i, float v, long now) {
        if (!seen[i]) {
            seen[i] = true;
            value[i] = lo[i] = hi[i] = v;
            changed[i] = now;
            if (restingPlace(v)) {
                rest[i] = v;
                known[i] = true;
            }
            return;
        }
        if (Math.abs(v - value[i]) > MOVED) changed[i] = now;
        value[i] = v;
        if (v < lo[i]) lo[i] = v;
        if (v > hi[i]) hi[i] = v;
    }

    /** Every TICK_MS: axes that stood still at a resting place rest there (one already resting
     *  elsewhere only after longer: a half of an axis held at its end is no new rest); a
     *  trigger's key waited long enough after its release answers. */
    private void onTick() {
        long now = SystemClock.uptimeMillis();
        for (int i = 0; i < value.length; i++) {
            if (seen[i] && now - changed[i] >= (known[i] ? STILL_AGAIN_MS : STILL_MS) && restingPlace(value[i])
                    && (!known[i] || Math.abs(value[i] - rest[i]) > RELEASED)) {
                rest[i] = value[i];
                known[i] = true;
                lo[i] = hi[i] = value[i];
            }
        }
        if (target >= 0 && pendingKey >= 0 && pendingUpAt > 0 && now - pendingUpAt >= KEY_WAIT_MS) {
            int key = pendingKey;
            pendingKey = -1;
            answer("b" + key);
        } else if (target >= 0) {
            checkAxes();
        }
    }

    /** An axis pushed at least PRESSED from where it rests and back within RELEASED answers; of
     *  several, the one pushed furthest (a stick pushed down a little to the side). Axes that
     *  went just as far are one input (Android reports a trigger twice, as GAS and RTRIGGER): one
     *  already set somewhere is taken, so a swap finds it; else the first. */
    private void checkAxes() {
        List<Integer> answered = new ArrayList<>();
        boolean pressing = false;
        for (int i = 0; i < value.length; i++) {
            if (!known[i]) continue;
            float r = rest[i], p = hi[i] - r >= r - lo[i] ? hi[i] : lo[i];
            if (Math.abs(p - r) < PRESSED) continue;
            if (Math.abs(value[i] - r) <= RELEASED) answered.add(i);
            else pressing = true;
        }
        boolean wasLetGo = letGo;
        letGo = pressing || (pendingKey >= 0 && pendingUpAt == 0);
        if (answered.isEmpty()) {
            if (letGo != wasLetGo) update();
            return;
        }
        float furthest = 0f;
        for (int i : answered) furthest = Math.max(furthest, travel(i));
        int pick = -1;
        for (int i : answered) {
            if (travel(i) < furthest - 0.02f) continue;
            if (pick < 0 || (axisInUse(i) && !axisInUse(pick))) pick = i;
        }
        float r = rest[pick], p = hi[pick] - r >= r - lo[pick] ? hi[pick] : lo[pick];
        freshAxes();
        pendingKey = -1;
        letGo = false;
        answer(axisSource(target, pick, r, p));
    }

    private float travel(int i) {
        return Math.max(hi[i] - rest[i], rest[i] - lo[i]);
    }

    private boolean axisInUse(int axis) {
        for (String s : sources)
            if (PadMapping.isAxis(s) && PadMapping.number(s) == axis) return true;
        return false;
    }

    /** An axis's source for a field from where it rests and where it was pushed: a stick takes
     *  the whole axis (reversed when pushed the other way); an axis resting at an end (a trigger)
     *  is taken whole, any other by the half it was pushed into. */
    private static String axisSource(int field, int axis, float rest, float pushed) {
        String a = "a" + axis;
        if (PadMapping.isStick(field)) return pushed > rest ? a : a + "~";
        if (rest <= -0.75f) return a;
        if (rest >= 0.75f) return a + "~";
        return (pushed > rest ? "+" : "-") + a;
    }

    // ---- the live test: what the game would read now ----

    private boolean held(int button) {
        return button >= 0 && button < keyHeld.length
               && (keyHeld[button] || (button >= 11 && button <= 14 && hatHeld[button - 11]));
    }

    /** A whole axis source as SDL reads it, -1..1. */
    private float axisValue(String src) {
        if (!PadMapping.isAxis(src) || PadMapping.isHalf(src)) return 0f;
        int n = PadMapping.number(src);
        if (n < 0 || n >= value.length) return 0f;
        return PadMapping.isReversed(src) ? -value[n] : value[n];
    }

    private boolean active(int field) {
        String src = sources[field];
        if (src == null || dev == null) return false;
        int n = PadMapping.number(src);
        if (!PadMapping.isAxis(src)) return held(n);
        if (n < 0 || n >= value.length) return false;
        if (src.startsWith("+")) return value[n] > 0.5f;
        if (src.startsWith("-")) return value[n] < -0.5f;
        if (PadMapping.isStick(field)) return Math.abs(value[n]) > 0.5f;
        return axisValue(src) > 0f; // SDL: a whole axis as a button is pressed past its middle
    }

    // ---- the drawn gamepad ----

    private static final int GLYPH_TEXT = 0, GLYPH_CROSS = 1, GLYPH_CIRCLE = 2, GLYPH_SQUARE = 3, GLYPH_TRIANGLE = 4;
    private static final int STICK_LEFT = 100, STICK_RIGHT = 101;

    /** A control of the game's on-screen gamepad (bbport_touchpad.cpp's Controls): its centre
     *  from an edge (ax 0 the left, 1 the right, 0.5 the middle; ay likewise) plus offsets, its
     *  size, all in units of the screen height. */
    private static final class Control {
        final int field, glyph;
        final String label;
        final float ax, ox, ay, oy, w, h;
        final boolean circle;

        Control(int field, String label, int glyph, float ax, float ox, float ay, float oy, float w, float h,
                boolean circle) {
            this.field = field;
            this.label = label;
            this.glyph = glyph;
            this.ax = ax;
            this.ox = ox;
            this.ay = ay;
            this.oy = oy;
            this.w = w;
            this.h = h;
            this.circle = circle;
        }
    }

    private static final Control[] CONTROLS = {
        new Control(PadMapping.Y, "", GLYPH_TRIANGLE, 1f, -0.24f, 1f, -0.405f, 0.11f, 0.11f, true),
        new Control(PadMapping.B, "", GLYPH_CIRCLE, 1f, -0.135f, 1f, -0.30f, 0.11f, 0.11f, true),
        new Control(PadMapping.A, "", GLYPH_CROSS, 1f, -0.24f, 1f, -0.195f, 0.11f, 0.11f, true),
        new Control(PadMapping.X, "", GLYPH_SQUARE, 1f, -0.345f, 1f, -0.30f, 0.11f, 0.11f, true),
        new Control(PadMapping.L2, "L2", GLYPH_TEXT, 0f, 0.14f, 0f, 0.085f, 0.22f, 0.085f, false),
        new Control(PadMapping.L1, "L1", GLYPH_TEXT, 0f, 0.14f, 0f, 0.20f, 0.22f, 0.085f, false),
        new Control(PadMapping.R2, "R2", GLYPH_TEXT, 1f, -0.14f, 0f, 0.085f, 0.22f, 0.085f, false),
        new Control(PadMapping.R1, "R1", GLYPH_TEXT, 1f, -0.14f, 0f, 0.20f, 0.22f, 0.085f, false),
        // the game's are 0.07 high: here the second line needs room
        new Control(PadMapping.PAD, "PAD", GLYPH_TEXT, 0.5f, -0.16f, 0f, 0.06f, 0.17f, 0.085f, false),
        new Control(PadMapping.OPTIONS, "OPTIONS", GLYPH_TEXT, 0.5f, 0.16f, 0f, 0.06f, 0.21f, 0.085f, false),
        new Control(PadMapping.L3, "L3", GLYPH_TEXT, 0.5f, -0.14f, 1f, -0.075f, 0.09f, 0.09f, true),
        new Control(PadMapping.R3, "R3", GLYPH_TEXT, 0.5f, 0.14f, 1f, -0.075f, 0.09f, 0.09f, true),
    };
    private static final float DPAD_X = 0.22f, DPAD_Y = 0.47f, DPAD_ARM = 0.105f, DPAD_WIDTH = 0.075f;
    // The left stick where the game hints at it; the right one (the game's is a drag on the right
    // half) beside the face buttons.
    private static final float STICK_R = 0.11f, KNOB_R = 0.05f, LSTICK_X = 0.28f, LSTICK_Y = 0.70f, RSTICK_Y = 0.78f;

    private final class PadView extends View {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint font = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final RectF rect = new RectF();
        private final Path path = new Path();
        private float W = 1, H = 1;
        private int downCode = -1;

        PadView(Context c) {
            super(c);
            font.setTextAlign(Paint.Align.CENTER);
        }

        @Override
        protected void onSizeChanged(int w, int h, int oldw, int oldh) {
            W = Math.max(1, w);
            H = Math.max(1, h);
        }

        private float cx(Control c) {
            return c.ax * W + c.ox * H;
        }

        private float cy(Control c) {
            return c.ay * H + c.oy * H;
        }

        private float rightStickX() {
            return Math.min(W * 0.5f + 0.36f * H, W - 0.57f * H);
        }

        private int hit(float px, float py) {
            for (Control c : CONTROLS) {
                float dx = px - cx(c), dy = py - cy(c), hw = c.w * H * 0.5f, hh = c.h * H * 0.5f;
                if (c.circle ? dx * dx + dy * dy <= hw * hw * 1.3f : Math.abs(dx) <= hw * 1.1f && Math.abs(dy) <= hh * 1.3f)
                    return c.field;
            }
            float dx = px / H - DPAD_X, dy = py / H - DPAD_Y;
            if (Math.abs(dx) <= DPAD_ARM + 0.02f && Math.abs(dy) <= DPAD_ARM + 0.02f && Math.max(Math.abs(dx), Math.abs(dy)) > 0.015f) {
                if (Math.abs(dx) >= Math.abs(dy)) return dx < 0 ? PadMapping.LEFT : PadMapping.RIGHT;
                return dy < 0 ? PadMapping.UP : PadMapping.DOWN;
            }
            if (Math.hypot(px - LSTICK_X * H, py - LSTICK_Y * H) <= STICK_R * H * 1.15f) return STICK_LEFT;
            if (Math.hypot(px - rightStickX(), py - RSTICK_Y * H) <= STICK_R * H * 1.15f) return STICK_RIGHT;
            return -1;
        }

        @Override
        public boolean onTouchEvent(MotionEvent e) {
            switch (e.getActionMasked()) {
                case MotionEvent.ACTION_DOWN:
                    downCode = hit(e.getX(), e.getY());
                    return true;
                case MotionEvent.ACTION_UP:
                    int code = hit(e.getX(), e.getY());
                    if (code == downCode) tapped(code);
                    downCode = -1;
                    return true;
                case MotionEvent.ACTION_CANCEL:
                    downCode = -1;
                    return true;
                default:
                    return true;
            }
        }

        private void tapped(int code) {
            if (dev == null) return;
            if (code < 0) {
                if (target >= 0) stopWaiting();
            } else if (code == STICK_LEFT) {
                begin(PadMapping.LX, PadMapping.LY);
            } else if (code == STICK_RIGHT) {
                begin(PadMapping.RX, PadMapping.RY);
            } else {
                begin(code);
            }
        }

        // colours: the game's white controls (alpha 55 filled, 150 outlined; 150 and 240 held), the
        // accent for the one waited for, fainter for one with nothing set
        private int fillColour(boolean waited, boolean on, boolean unset, float pulse) {
            if (waited) return alpha(highlight, Math.round(70 + 80 * pulse));
            return Color.argb(on ? 150 : unset ? 26 : 55, 255, 255, 255);
        }

        private int lineColour(boolean waited, boolean on, boolean unset) {
            if (waited) return lightHighlight;
            return Color.argb(on ? 240 : unset ? 90 : 150, 255, 255, 255);
        }

        private int labelColour(int field) {
            String s = sources[field];
            if (unset(field)) return 0x80FFFFFF;
            return s.equals(defaults[field]) ? 0xD9FFFFFF : lightHighlight;
        }

        private void label(Canvas canvas, String s, float x, float y, float size, int colour, boolean bold, float maxWidth) {
            font.setTypeface(bold ? Typeface.DEFAULT_BOLD : Typeface.DEFAULT);
            font.setTextSize(size);
            float width = font.measureText(s);
            if (width > maxWidth && width > 0) font.setTextSize(Math.max(size * 0.6f, size * maxWidth / width));
            font.setColor(colour);
            font.setShadowLayer(Math.max(1f, size * 0.08f), 0, Math.max(1f, size * 0.05f), 0xC0000000);
            canvas.drawText(s, x, y + font.getTextSize() * 0.36f, font);
            font.clearShadowLayer();
        }

        private void glyph(Canvas canvas, int glyph, float x, float y, float r, int colour, float width) {
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(width);
            paint.setColor(colour);
            switch (glyph) {
                case GLYPH_CROSS:
                    canvas.drawLine(x - r, y - r, x + r, y + r, paint);
                    canvas.drawLine(x - r, y + r, x + r, y - r, paint);
                    break;
                case GLYPH_CIRCLE:
                    canvas.drawCircle(x, y, r, paint);
                    break;
                case GLYPH_SQUARE:
                    canvas.drawRect(x - r, y - r, x + r, y + r, paint);
                    break;
                case GLYPH_TRIANGLE:
                    path.reset();
                    path.moveTo(x, y - r * 1.1f);
                    path.lineTo(x + r * 1.05f, y + r * 0.8f);
                    path.lineTo(x - r * 1.05f, y + r * 0.8f);
                    path.close();
                    canvas.drawPath(path, paint);
                    break;
                default:
                    break;
            }
        }

        private void shape(Canvas canvas, boolean circle, float x, float y, float w, float h, int fill, int line, float lineWidth) {
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(fill);
            if (circle) {
                canvas.drawCircle(x, y, w * 0.5f, paint);
            } else {
                rect.set(x - w * 0.5f, y - h * 0.5f, x + w * 0.5f, y + h * 0.5f);
                canvas.drawRoundRect(rect, h * 0.5f, h * 0.5f, paint);
            }
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(lineWidth);
            paint.setColor(line);
            if (circle) {
                canvas.drawCircle(x, y, w * 0.5f, paint);
            } else {
                canvas.drawRoundRect(rect, h * 0.5f, h * 0.5f, paint);
            }
        }

        @Override
        protected void onDraw(Canvas canvas) {
            canvas.drawColor(BG);
            float line = Math.max(1.5f, 2.5f * H / 1080f);
            float pulse = 0.5f + 0.5f * (float) Math.sin(SystemClock.uptimeMillis() / 170.0);
            boolean ready = dev != null;
            for (Control c : CONTROLS) {
                boolean waited = target == c.field, on = ready && active(c.field), unset = unset(c.field);
                float x = cx(c), y = cy(c), w = c.w * H, h = c.h * H;
                shape(canvas, c.circle, x, y, w, h, fillColour(waited, on, unset, pulse), lineColour(waited, on, unset),
                      waited ? line * 2 : line);
                String assigned = ready ? PadMapping.label(dev, sources[c.field]) : "";
                if (c.glyph != GLYPH_TEXT) {
                    float r = w * 0.5f;
                    glyph(canvas, c.glyph, x + line, y - r * 0.2f + line, r * 0.3f, 0xC8000000, line * 1.5f);
                    glyph(canvas, c.glyph, x, y - r * 0.2f, r * 0.3f, 0xEBFFFFFF, line * 1.5f);
                    if (ready) label(canvas, assigned, x, y + r * 0.48f, 0.022f * H, labelColour(c.field), false, w * 0.8f);
                } else if (c.circle) {
                    float r = w * 0.5f;
                    label(canvas, c.label, x, y - r * (ready ? 0.25f : 0f), 0.028f * H, 0xEBFFFFFF, true, w * 0.8f);
                    if (ready) label(canvas, assigned, x, y + r * 0.45f, 0.019f * H, labelColour(c.field), false, w * 0.85f);
                } else {
                    label(canvas, c.label, x, y - h * (ready ? 0.2f : 0f), 0.028f * H, 0xEBFFFFFF, true, w * 0.85f);
                    if (ready) label(canvas, assigned, x, y + h * 0.26f, 0.022f * H, labelColour(c.field), false, w * 0.85f);
                }
            }
            // the D-pad
            float dx = DPAD_X * H, dy = DPAD_Y * H, arm = DPAD_ARM * H, hw = DPAD_WIDTH * H * 0.5f;
            float[][] arms = {
                {dx - hw, dy - arm, dx + hw, dy - hw * 0.4f}, {dx - hw, dy + hw * 0.4f, dx + hw, dy + arm},
                {dx - arm, dy - hw, dx - hw * 0.4f, dy + hw}, {dx + hw * 0.4f, dy - hw, dx + arm, dy + hw},
            };
            int[] armFields = {PadMapping.UP, PadMapping.DOWN, PadMapping.LEFT, PadMapping.RIGHT};
            for (int k = 0; k < 4; k++) {
                int f = armFields[k];
                boolean waited = target == f, on = ready && active(f), unset = unset(f);
                rect.set(arms[k][0], arms[k][1], arms[k][2], arms[k][3]);
                paint.setStyle(Paint.Style.FILL);
                paint.setColor(fillColour(waited, on, unset, pulse));
                canvas.drawRoundRect(rect, hw * 0.35f, hw * 0.35f, paint);
                paint.setStyle(Paint.Style.STROKE);
                paint.setStrokeWidth(waited ? line * 2 : line);
                paint.setColor(lineColour(waited, on, unset));
                canvas.drawRoundRect(rect, hw * 0.35f, hw * 0.35f, paint);
                if (ready)
                    label(canvas, PadMapping.label(dev, sources[f]), rect.centerX(), rect.centerY(), 0.019f * H,
                          labelColour(f), false, rect.width() * 0.92f);
            }
            stick(canvas, LSTICK_X * H, LSTICK_Y * H, PadMapping.LX, PadMapping.LY, line, pulse, ready);
            stick(canvas, rightStickX(), RSTICK_Y * H, PadMapping.RX, PadMapping.RY, line, pulse, ready);
            if (target >= 0) postInvalidateOnAnimation();
        }

        private void stick(Canvas canvas, float x, float y, int fx, int fy, float line, float pulse, boolean ready) {
            float r = STICK_R * H, k = KNOB_R * H;
            boolean waited = target == fx || target == fy;
            boolean unset = unset(fx) && unset(fy);
            float sx = ready ? axisValue(sources[fx]) : 0f, sy = ready ? axisValue(sources[fy]) : 0f;
            boolean on = Math.abs(sx) > 0.5f || Math.abs(sy) > 0.5f;
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(waited ? alpha(highlight, Math.round(50 + 60 * pulse)) : Color.argb(unset ? 20 : 45, 255, 255, 255));
            canvas.drawCircle(x, y, r, paint);
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(waited ? line * 2 : line);
            paint.setColor(lineColour(waited, on, unset));
            canvas.drawCircle(x, y, r, paint);
            if (waited) { // the way to push
                paint.setStrokeWidth(line * 2.5f);
                paint.setColor(lightHighlight);
                boolean horizontal = target == fx;
                float ex = horizontal ? x + r * 0.85f : x, ey = horizontal ? y : y + r * 0.85f;
                canvas.drawLine(x, y, ex, ey, paint);
                float a = r * 0.18f;
                if (horizontal) {
                    canvas.drawLine(ex, ey, ex - a, ey - a, paint);
                    canvas.drawLine(ex, ey, ex - a, ey + a, paint);
                } else {
                    canvas.drawLine(ex, ey, ex - a, ey - a, paint);
                    canvas.drawLine(ex, ey, ex + a, ey - a, paint);
                }
            }
            float travel = r - k, length = (float) Math.hypot(sx, sy);
            if (length > 1f) { // a diagonal: inside the ring, as the game's stick is a circle
                sx /= length;
                sy /= length;
            }
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(Color.argb(waited ? 120 : 170, 255, 255, 255));
            canvas.drawCircle(x + sx * travel, y + sy * travel, k, paint);
            if (ready) {
                String text = PadMapping.label(dev, sources[fx]) + " · " + PadMapping.label(dev, sources[fy]);
                int colour = labelColour(fx) == lightHighlight || labelColour(fy) == lightHighlight ? lightHighlight
                             : unset ? 0x80FFFFFF : 0xD9FFFFFF;
                label(canvas, text, x, y + r + 0.032f * H, 0.022f * H, colour, false, r * 2.4f);
            }
        }

    }
}
