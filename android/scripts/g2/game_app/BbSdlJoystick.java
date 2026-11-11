package org.libsdl.app;

import android.view.InputDevice;
import android.view.MotionEvent;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

/**
 * g2-arm64 (issue #16): SDL's own view of an Android input device, for the app's controller
 * setup (dev.unchartedg2.game.PadMapping). A setup is written in SDL's numbers - the joystick
 * buttons and axes the game's SDL gives the same device - so it asks SDL's code: which devices
 * SDL opens as joysticks, the order of their axes, the string the CRC in their GUID is made of,
 * the masks its generic gamepad mapping is made from. This class sits in SDL's package to call
 * that package-private code (SDLControllerManager.java) instead of copying it.
 */
public final class BbSdlJoystick {
    private static final SDLJoystickHandler handler = new SDLJoystickHandler();

    private BbSdlJoystick() {
    }

    /** Whether SDL opens the device as a joystick. */
    public static boolean isJoystick(int deviceId) {
        return SDLControllerManager.isDeviceSDLJoystick(deviceId);
    }

    /** SDL's axes of the device (the list index is SDL's axis number) and its hats, sorted and
     *  filtered as SDLJoystickHandler.pollInputDevices does. */
    public static void axes(InputDevice device, List<InputDevice.MotionRange> axes,
                            List<InputDevice.MotionRange> hats) {
        List<InputDevice.MotionRange> ranges = new ArrayList<>(device.getMotionRanges());
        Collections.sort(ranges, new SDLJoystickHandler.RangeComparator());
        Set<Integer> seen = new HashSet<>();
        for (InputDevice.MotionRange range : ranges) {
            if ((range.getSource() & InputDevice.SOURCE_CLASS_JOYSTICK) != 0 && seen.add(range.getAxis())) {
                if (range.getAxis() == MotionEvent.AXIS_HAT_X || range.getAxis() == MotionEvent.AXIS_HAT_Y) {
                    hats.add(range);
                } else {
                    axes.add(range);
                }
            }
        }
    }

    /** The string SDL computes the CRC in the device's GUID from. */
    public static String descriptor(InputDevice device) {
        return handler.getJoystickDescriptor(device);
    }

    /** SDL's button mask of the device (its GUID keeps the low 16 bits). */
    public static int buttonMask(InputDevice device) {
        return handler.getButtonMask(device);
    }

    /** SDL's axis mask of the device's axes (as axes() gives them). */
    public static int axisMask(List<InputDevice.MotionRange> axes) {
        return handler.getAxisMask(axes);
    }
}
