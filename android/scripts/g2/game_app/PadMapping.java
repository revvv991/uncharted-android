package dev.unchartedg2.game;

import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStreamWriter;
import java.io.Writer;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import org.libsdl.app.BbSdlJoystick;

/**
 * g2-arm64 (issue #16): controller setups in SDL's terms, for ControllerSetup (the window the
 * launcher and the in-game panel open) and the port (gpu/shim/bbport_padmap.cpp).
 *
 * The game reads SDL gamepads. SDL makes a gamepad of an Android joystick through a mapping: its
 * own (SDL_CreateMappingForAndroidGamepad, only for a device with Android's face button keys; a
 * pad in a PC / DirectInput mode reports BUTTON_1..16 instead and got none, so the game did not
 * read it) or one the app adds. A setup is such a mapping: SDL's gamepad fields (a, leftx, ...)
 * to the device's SDL joystick buttons (bN: Android's keys and hats become SDL's buttons by
 * fixed rules, sdlButton) and axes (aN, +aN or -aN for a half, aN~ reversed; N is the axis's
 * place in SDL's sorted list, BbSdlJoystick.axes). files/controllers.txt keeps one line per
 * controller,
 *     vendor:product:crc TAB name TAB a:b20,b:b21,...
 * where crc is SDL's CRC-16 of the device's descriptor: the port finds the same three numbers in
 * the GUID SDL gives the device and adds the mapping under that GUID (one physical controller).
 */
final class PadMapping {
    static final String FILE = "controllers.txt";

    /** SDL's gamepad fields the game reads, in the order Set up all asks for them. */
    static final String[] FIELDS = {"a", "b", "x", "y", "dpup", "dpdown", "dpleft", "dpright",
        "leftshoulder", "rightshoulder", "lefttrigger", "righttrigger", "leftstick", "rightstick",
        "start", "back", "leftx", "lefty", "rightx", "righty"};
    /** Their PlayStation names (runtime_pad.c: SDL's start is Options, back the touch pad button). */
    static final String[] NAMES = {"Cross", "Circle", "Square", "Triangle", "D-pad up", "D-pad down",
        "D-pad left", "D-pad right", "L1", "R1", "L2", "R2", "L3", "R3", "Options", "Touch pad",
        "Left stick X", "Left stick Y", "Right stick X", "Right stick Y"};
    static final int A = 0, B = 1, X = 2, Y = 3, UP = 4, DOWN = 5, LEFT = 6, RIGHT = 7, L1 = 8, R1 = 9,
        L2 = 10, R2 = 11, L3 = 12, R3 = 13, OPTIONS = 14, PAD = 15, LX = 16, LY = 17, RX = 18, RY = 19;
    static final int COUNT = FIELDS.length;

    static boolean isStick(int field) {
        return field >= LX;
    }

    static boolean isTrigger(int field) {
        return field == L2 || field == R2;
    }

    /** The keys a controller can have that SDL turns into buttons, in the order a button is
     *  named by (the first key the device has that gives SDL's button). */
    private static final int[] KEYS = {
        KeyEvent.KEYCODE_BUTTON_A, KeyEvent.KEYCODE_BUTTON_B, KeyEvent.KEYCODE_BUTTON_X,
        KeyEvent.KEYCODE_BUTTON_Y, KeyEvent.KEYCODE_BUTTON_L1, KeyEvent.KEYCODE_BUTTON_R1,
        KeyEvent.KEYCODE_BUTTON_L2, KeyEvent.KEYCODE_BUTTON_R2, KeyEvent.KEYCODE_BUTTON_THUMBL,
        KeyEvent.KEYCODE_BUTTON_THUMBR, KeyEvent.KEYCODE_BUTTON_START, KeyEvent.KEYCODE_BUTTON_SELECT,
        KeyEvent.KEYCODE_BUTTON_MODE, KeyEvent.KEYCODE_MENU, KeyEvent.KEYCODE_BACK,
        KeyEvent.KEYCODE_BUTTON_C, KeyEvent.KEYCODE_BUTTON_Z, KeyEvent.KEYCODE_DPAD_UP,
        KeyEvent.KEYCODE_DPAD_DOWN, KeyEvent.KEYCODE_DPAD_LEFT, KeyEvent.KEYCODE_DPAD_RIGHT,
        KeyEvent.KEYCODE_DPAD_CENTER, KeyEvent.KEYCODE_MEDIA_RECORD,
        KeyEvent.KEYCODE_BUTTON_1, KeyEvent.KEYCODE_BUTTON_2, KeyEvent.KEYCODE_BUTTON_3,
        KeyEvent.KEYCODE_BUTTON_4, KeyEvent.KEYCODE_BUTTON_5, KeyEvent.KEYCODE_BUTTON_6,
        KeyEvent.KEYCODE_BUTTON_7, KeyEvent.KEYCODE_BUTTON_8, KeyEvent.KEYCODE_BUTTON_9,
        KeyEvent.KEYCODE_BUTTON_10, KeyEvent.KEYCODE_BUTTON_11, KeyEvent.KEYCODE_BUTTON_12,
        KeyEvent.KEYCODE_BUTTON_13, KeyEvent.KEYCODE_BUTTON_14, KeyEvent.KEYCODE_BUTTON_15,
        KeyEvent.KEYCODE_BUTTON_16,
    };

    /** A controller as SDL sees it. */
    static final class Device {
        final InputDevice device;
        final int id;
        final String name, key;
        final List<InputDevice.MotionRange> axes = new ArrayList<>(), hats = new ArrayList<>();
        final boolean[] keys; // KEYS the device has

        Device(InputDevice d) {
            device = d;
            id = d.getId();
            String n = d.getName();
            name = n != null && !n.trim().isEmpty() ? n.trim() : "Controller";
            BbSdlJoystick.axes(d, axes, hats);
            key = key(d);
            keys = d.hasKeys(KEYS);
        }

        boolean hasKey(int keycode) {
            for (int i = 0; i < KEYS.length; i++)
                if (KEYS[i] == keycode) return keys[i];
            return false;
        }

        int axisIndex(int axis) {
            for (int i = 0; i < axes.size(); i++)
                if (axes.get(i).getAxis() == axis) return i;
            return -1;
        }
    }

    /** The controllers SDL opens: its joysticks that are gamepads or have joystick axes (SDL also
     *  takes keyboards with arrow keys and remotes, no use here). */
    static List<Device> devices() {
        List<Device> out = new ArrayList<>();
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice d = InputDevice.getDevice(id);
            if (d == null || !BbSdlJoystick.isJoystick(id)) continue;
            int sources = d.getSources();
            if ((sources & InputDevice.SOURCE_GAMEPAD) != InputDevice.SOURCE_GAMEPAD
                    && (sources & InputDevice.SOURCE_CLASS_JOYSTICK) == 0) continue;
            out.add(new Device(d));
        }
        return out;
    }

    /** vendor:product:crc as the port reads them from SDL's GUID of the device (SDL_sysjoystick.c
     *  Android_AddJoystick: SDL_CreateJoystickGUID with the descriptor as the name; the product
     *  only with a vendor). */
    static String key(InputDevice d) {
        int vendor = d.getVendorId() & 0xffff, product = d.getProductId() & 0xffff;
        if (vendor == 0) product = 0;
        return String.format(Locale.ROOT, "%04x:%04x:%04x", vendor, product, crc16(BbSdlJoystick.descriptor(d)));
    }

    /** SDL_crc16 (CRC-16/ARC) of the string's bytes. */
    static int crc16(String s) {
        int crc = 0;
        for (byte b : s.getBytes(StandardCharsets.UTF_8)) {
            int r = (crc ^ b) & 0xff, c = 0;
            for (int i = 0; i < 8; i++) {
                c = (((c ^ r) & 1) != 0 ? 0xA001 : 0) ^ (c >>> 1);
                r >>>= 1;
            }
            crc = c ^ (crc >>> 8);
        }
        return crc & 0xffff;
    }

    /** SDL's joystick button for a key (SDL_sysjoystick.c keycode_to_SDL, then scancode_to_SDL);
     *  -1: SDL passes the key on as a keyboard's, no button. */
    static int sdlButton(int keycode, int scancode) {
        switch (keycode) {
            case KeyEvent.KEYCODE_BUTTON_A:
            case KeyEvent.KEYCODE_DPAD_CENTER:
                return 0;
            case KeyEvent.KEYCODE_BUTTON_B:
                return 1;
            case KeyEvent.KEYCODE_BUTTON_X:
                return 2;
            case KeyEvent.KEYCODE_BUTTON_Y:
                return 3;
            case KeyEvent.KEYCODE_BACK:
            case KeyEvent.KEYCODE_BUTTON_SELECT:
                return 4;
            case KeyEvent.KEYCODE_BUTTON_MODE:
                return 5;
            case KeyEvent.KEYCODE_MENU:
            case KeyEvent.KEYCODE_BUTTON_START:
                return 6;
            case KeyEvent.KEYCODE_BUTTON_THUMBL:
                return 7;
            case KeyEvent.KEYCODE_BUTTON_THUMBR:
                return 8;
            case KeyEvent.KEYCODE_BUTTON_L1:
                return 9;
            case KeyEvent.KEYCODE_BUTTON_R1:
                return 10;
            case KeyEvent.KEYCODE_DPAD_UP:
                return 11;
            case KeyEvent.KEYCODE_DPAD_DOWN:
                return 12;
            case KeyEvent.KEYCODE_DPAD_LEFT:
                return 13;
            case KeyEvent.KEYCODE_DPAD_RIGHT:
                return 14;
            case KeyEvent.KEYCODE_BUTTON_L2:
            case KeyEvent.KEYCODE_MEDIA_RECORD:
                return 15;
            case KeyEvent.KEYCODE_BUTTON_R2:
                return 16;
            case KeyEvent.KEYCODE_BUTTON_C:
                return 17;
            case KeyEvent.KEYCODE_BUTTON_Z:
                return 18;
            default:
                break;
        }
        if (keycode >= KeyEvent.KEYCODE_BUTTON_1 && keycode <= KeyEvent.KEYCODE_BUTTON_16)
            return 20 + keycode - KeyEvent.KEYCODE_BUTTON_1;
        switch (scancode) { // BTN_DPAD_UP..RIGHT (the left Joy-Con)
            case 0x220:
                return 11;
            case 0x221:
                return 12;
            case 0x222:
                return 13;
            case 0x223:
                return 14;
            default:
                return -1;
        }
    }

    /** SDL's own mapping of the device when it makes one (SDL_gamepad.c
     *  SDL_CreateMappingForAndroidGamepad, from the masks in the GUID): all null when it makes
     *  none - no face button keys, a pad in a PC / DirectInput mode. SDL's database of known
     *  controllers comes first in the game; a few Android pads are in it, mapped much the same. */
    static String[] defaults(Device dev) {
        String[] s = new String[COUNT];
        int vendor = dev.device.getVendorId(), product = dev.device.getProductId();
        if (vendor == 0x057e && (product == 0x2006 || product == 0x2007)) return s; // Joy-Cons: SDL's fixed ones
        int mask = BbSdlJoystick.buttonMask(dev.device);
        if (!dev.hats.isEmpty()) mask |= 0x7800; // the hat as the D-pad's buttons
        mask &= 0xffff;
        int axes = BbSdlJoystick.axisMask(dev.axes) & 0xffff;
        if ((mask == 0 && axes == 0) || (mask & 0xf) == 0) return s;
        if ((mask & 0x1) != 0) s[A] = "b0";
        if ((mask & 0x2) != 0) {
            s[B] = "b1";
        } else if ((mask & 0x10) != 0) { // a TV remote's Back as B
            s[B] = "b4";
            mask &= ~0x10;
        }
        if ((mask & 0x4) != 0) s[X] = "b2";
        if ((mask & 0x8) != 0) s[Y] = "b3";
        if ((mask & 0x10) != 0) s[PAD] = "b4";
        if ((mask & 0x40) != 0) s[OPTIONS] = "b6";
        if ((mask & 0x80) != 0) s[L3] = "b7";
        if ((mask & 0x100) != 0) s[R3] = "b8";
        if ((mask & 0x200) != 0) s[L1] = "b9";
        if ((mask & 0x400) != 0) s[R1] = "b10";
        if ((mask & 0x800) != 0) s[UP] = "b11";
        if ((mask & 0x1000) != 0) s[DOWN] = "b12";
        if ((mask & 0x2000) != 0) s[LEFT] = "b13";
        if ((mask & 0x4000) != 0) s[RIGHT] = "b14";
        if ((axes & 0x1) != 0) s[LX] = "a0";
        if ((axes & 0x2) != 0) s[LY] = "a1";
        if ((axes & 0x4) != 0) s[RX] = "a2";
        if ((axes & 0x8) != 0) s[RY] = "a3";
        if ((axes & 0x10) != 0) s[L2] = "a4";
        if ((axes & 0x20) != 0) s[R2] = "a5";
        return s;
    }

    static boolean isAxis(String src) {
        return src != null && !src.startsWith("b");
    }

    static boolean isHalf(String src) {
        return src != null && (src.startsWith("+") || src.startsWith("-"));
    }

    static boolean isReversed(String src) {
        return src != null && src.endsWith("~");
    }

    /** The button or axis number of a source. */
    static int number(String src) {
        String s = src;
        if (isHalf(s)) s = s.substring(1);
        if (s.endsWith("~")) s = s.substring(0, s.length() - 1);
        try {
            return Integer.parseInt(s.substring(1));
        } catch (NumberFormatException e) {
            return -1;
        }
    }

    static boolean valid(String src) {
        return src != null && src.matches("b([0-9]|[1-2][0-9]|3[0-5])|[+-]?a[0-9]{1,2}~?");
    }

    /** Whether a field can take a source: a stick's needs a whole axis. */
    static boolean fits(int field, String src) {
        return src == null || !isStick(field) || (isAxis(src) && !isHalf(src));
    }

    /** Whether two sources read the same input: one button, or overlapping parts of one axis. */
    static boolean overlaps(String a, String b) {
        if (a == null || b == null) return false;
        if (isAxis(a) != isAxis(b) || number(a) != number(b)) return false;
        if (!isAxis(a)) return true;
        return !isHalf(a) || !isHalf(b) || a.charAt(0) == b.charAt(0);
    }

    private static String keyName(int keycode) {
        switch (keycode) {
            case KeyEvent.KEYCODE_BUTTON_A: return "A";
            case KeyEvent.KEYCODE_BUTTON_B: return "B";
            case KeyEvent.KEYCODE_BUTTON_X: return "X";
            case KeyEvent.KEYCODE_BUTTON_Y: return "Y";
            case KeyEvent.KEYCODE_BUTTON_L1: return "L1";
            case KeyEvent.KEYCODE_BUTTON_R1: return "R1";
            case KeyEvent.KEYCODE_BUTTON_L2: return "L2";
            case KeyEvent.KEYCODE_BUTTON_R2: return "R2";
            case KeyEvent.KEYCODE_BUTTON_THUMBL: return "L3";
            case KeyEvent.KEYCODE_BUTTON_THUMBR: return "R3";
            case KeyEvent.KEYCODE_BUTTON_START: return "Start";
            case KeyEvent.KEYCODE_BUTTON_SELECT: return "Select";
            case KeyEvent.KEYCODE_BUTTON_MODE: return "Home";
            case KeyEvent.KEYCODE_MENU: return "Menu";
            case KeyEvent.KEYCODE_BACK: return "Back";
            case KeyEvent.KEYCODE_BUTTON_C: return "C";
            case KeyEvent.KEYCODE_BUTTON_Z: return "Z";
            case KeyEvent.KEYCODE_DPAD_UP: return "Up";
            case KeyEvent.KEYCODE_DPAD_DOWN: return "Down";
            case KeyEvent.KEYCODE_DPAD_LEFT: return "Left";
            case KeyEvent.KEYCODE_DPAD_RIGHT: return "Right";
            case KeyEvent.KEYCODE_DPAD_CENTER: return "OK";
            case KeyEvent.KEYCODE_MEDIA_RECORD: return "Record";
            default:
                return "#" + (keycode - KeyEvent.KEYCODE_BUTTON_1 + 1);
        }
    }

    private static String axisName(int axis) {
        switch (axis) {
            case MotionEvent.AXIS_X: return "X";
            case MotionEvent.AXIS_Y: return "Y";
            case MotionEvent.AXIS_Z: return "Z";
            case MotionEvent.AXIS_RX: return "RX";
            case MotionEvent.AXIS_RY: return "RY";
            case MotionEvent.AXIS_RZ: return "RZ";
            case MotionEvent.AXIS_LTRIGGER: return "LT";
            case MotionEvent.AXIS_RTRIGGER: return "RT";
            case MotionEvent.AXIS_GAS: return "Gas";
            case MotionEvent.AXIS_BRAKE: return "Brake";
            case MotionEvent.AXIS_THROTTLE: return "Throttle";
            case MotionEvent.AXIS_RUDDER: return "Rudder";
            case MotionEvent.AXIS_WHEEL: return "Wheel";
            default:
                if (axis >= MotionEvent.AXIS_GENERIC_1 && axis <= MotionEvent.AXIS_GENERIC_16)
                    return "Axis " + (axis - MotionEvent.AXIS_GENERIC_1 + 1);
                return MotionEvent.axisToString(axis).replace("AXIS_", "");
        }
    }

    /** Whether the device has the source's input. SDL's own mapping can name buttons a device
     *  lacks: a device with BUTTON_13..16 gets every bit of SDL's button mask (SDL's Java
     *  getButtonMask: "We're out of room"), face buttons included, so SDL maps a pad in a PC mode
     *  with 13 or more buttons as a gamepad whose buttons 0..14 it does not have. */
    static boolean exists(Device d, String src) {
        if (src == null) return false;
        int n = number(src);
        if (isAxis(src)) return n >= 0 && n < d.axes.size();
        for (int i = 0; i < KEYS.length; i++)
            if (d.keys[i] && sdlButton(KEYS[i], 0) == n) return true;
        return n >= 11 && n <= 14 && !d.hats.isEmpty();
    }

    /** What a source is on the device, short: the key's name ("A", "#3"), the hat's direction,
     *  the axis ("Z+", "Gas", "X rev"); a dash for none, or one the device does not have. */
    static String label(Device d, String src) {
        if (!exists(d, src)) return "—";
        int n = number(src);
        if (!isAxis(src)) {
            for (int i = 0; i < KEYS.length; i++)
                if (d.keys[i] && sdlButton(KEYS[i], 0) == n) return keyName(KEYS[i]);
            return new String[] {"Hat up", "Hat down", "Hat left", "Hat right"}[n - 11];
        }
        String name = axisName(d.axes.get(n).getAxis());
        if (src.startsWith("+")) name += "+";
        else if (src.startsWith("-")) name += "−";
        if (isReversed(src)) name += " rev";
        return name;
    }

    /** A controller's line of the file. */
    static final class Entry {
        String name;
        final String[] sources = new String[COUNT];
    }

    /** The setups in the file by key, in its order; empty when there is none. */
    static Map<String, Entry> load(File dir) {
        Map<String, Entry> out = new LinkedHashMap<>();
        File f = new File(dir, FILE);
        if (!f.isFile()) return out;
        try (BufferedReader r = new BufferedReader(new InputStreamReader(new FileInputStream(f), StandardCharsets.UTF_8))) {
            for (String line; (line = r.readLine()) != null;) {
                if (line.startsWith("#") || line.trim().isEmpty()) continue;
                String[] parts = line.split("\t", -1);
                if (parts.length < 3 || !parts[0].matches("[0-9a-f]{4}:[0-9a-f]{4}:[0-9a-f]{4}")) continue;
                Entry e = new Entry();
                e.name = parts[1];
                for (String item : parts[2].split(",")) {
                    int colon = item.indexOf(':');
                    if (colon <= 0) continue;
                    String field = item.substring(0, colon), src = item.substring(colon + 1);
                    for (int i = 0; i < COUNT; i++)
                        if (FIELDS[i].equals(field) && valid(src) && fits(i, src)) e.sources[i] = src;
                }
                out.put(parts[0], e);
            }
        } catch (IOException e) {
            // unreadable: no setups
        }
        return out;
    }

    /** The SDL mapping fields of a setup ("a:b20,b:b21,..."). */
    static String fields(String[] sources) {
        StringBuilder b = new StringBuilder();
        for (int i = 0; i < COUNT; i++) {
            if (sources[i] == null) continue;
            if (b.length() > 0) b.append(',');
            b.append(FIELDS[i]).append(':').append(sources[i]);
        }
        return b.toString();
    }

    /** Writes all setups (through a temporary file: a failed write keeps the old ones; none, no
     *  file). False when it could not be written. */
    static boolean save(File dir, Map<String, Entry> all) {
        File f = new File(dir, FILE);
        if (all.isEmpty()) return !f.exists() || f.delete();
        File tmp = new File(dir, FILE + ".tmp");
        try (Writer w = new OutputStreamWriter(new FileOutputStream(tmp), StandardCharsets.UTF_8)) {
            w.write("# Controller setups (the app's Controller setup): vendor:product:crc of SDL's GUID,\n"
                    + "# the name, the SDL gamepad mapping's fields. Read by the port when a controller connects.\n");
            for (Map.Entry<String, Entry> e : all.entrySet()) {
                String name = e.getValue().name.replaceAll("[\\t\\r\\n]", " ");
                w.write(e.getKey() + "\t" + name + "\t" + fields(e.getValue().sources) + "\n");
            }
        } catch (IOException e) {
            tmp.delete();
            return false;
        }
        return tmp.renameTo(f);
    }
}
