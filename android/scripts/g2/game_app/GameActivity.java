package dev.unchartedg2.game;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.Process;
import android.system.Os;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.FileWriter;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import org.libsdl.app.SDLActivity;

/** SDLActivity running uncharted_pc's loader (libmain.so). Arguments: files/args.txt, one per line
 *  (adb shell: /sdcard/Android/data/dev.unchartedg2.game/files/args.txt); without them the launcher.
 *  sceSystemServiceLoadExec (the collection menu starting U2 or U3, a game going back to the menu)
 *  restarts the app: the port writes files/loadexec.txt (the guest path, then its arguments) and the
 *  next start boots that executable with files/args-u1.txt, args-u2.txt or args-u3.txt (if absent:
 *  args.txt for U1; for U2/U3 args.txt with files/u2 or files/u3 and no patches) and the guest
 *  arguments (--guest-argv). Environment: files/env.txt, then env-u1.txt, env-u2.txt or env-u3.txt
 *  for the game started (KEY=VALUE per line; -KEY removes KEY). */
public class GameActivity extends SDLActivity {
    private final List<String> bootArgs = new ArrayList<>();

    private File dir() {
        File d = getExternalFilesDir(null);
        return d != null ? d : getFilesDir();
    }

    /** The trimmed non-empty lines of a file (# comments left out if asked); none if it is missing. */
    private static List<String> lines(File f, boolean comments) {
        List<String> out = new ArrayList<>();
        try (BufferedReader r = new BufferedReader(new FileReader(f))) {
            for (String line; (line = r.readLine()) != null;) {
                line = line.trim();
                if (!line.isEmpty() && !(comments && line.startsWith("#"))) out.add(line);
            }
        } catch (IOException e) {
            // missing
        }
        return out;
    }

    /** The game of a boot image: its folder (files/u2/boot-linked.bin) or U1 (the collection). */
    private static String gameOf(String bootImage) {
        File parent = new File(bootImage).getParentFile();
        String name = parent != null ? parent.getName() : "";
        return name.equals("u2") || name.equals("u3") ? name : "u1";
    }

    /** U2's or U3's arguments made from U1's: the boot image and content profile from files/u2 or
     *  files/u3, no patches (they are made for one executable). */
    private static List<String> derived(List<String> u1, String d, String game) {
        List<String> out = new ArrayList<>();
        for (int i = 0; i < u1.size(); i++) {
            String a = u1.get(i);
            if (i == 0) {
                out.add(d + "/" + game + "/boot-linked.bin");
            } else if (a.equals("--content-profile") && i + 1 < u1.size()) {
                out.add(a);
                out.add(d + "/" + game + "/content.bin");
                i++;
            } else if (a.equals("--patches") && i + 1 < u1.size()) {
                i++;
            } else {
                out.add(a);
            }
        }
        return out;
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        String d = dir().getAbsolutePath();
        new File(d, "user").mkdirs();
        new File(d, "home/.local/share").mkdirs();
        // A pending sceSystemServiceLoadExec: the executable to boot and its guest arguments.
        File loadexec = new File(d, "loadexec.txt");
        List<String> guest = lines(loadexec, false);
        String game = null;
        if (!guest.isEmpty()) {
            loadexec.renameTo(new File(d, "loadexec-last.txt"));
            String exe = new File(guest.get(0)).getName().toLowerCase(Locale.ROOT);
            game = exe.startsWith("big2") ? "u2" : exe.startsWith("big3") ? "u3" : "u1";
            // The menu starts U2 and U3 with "eboot.bin ispartofuccollection"; in that mode U2
            // stays on a gray loading screen (2026-10-21, not understood yet). They start as on
            // their own instead: the path only (loadexec-last.txt keeps the whole request).
            if (!game.equals("u1")) guest = guest.subList(0, 1);
            bootArgs.addAll(lines(new File(d, "args-" + game + ".txt"), true));
            if (bootArgs.isEmpty() && !game.equals("u1") && new File(d, game + "/boot-linked.bin").isFile())
                bootArgs.addAll(derived(lines(new File(d, "args.txt"), true), d, game));
            try (FileWriter w = new FileWriter(new File(d, "guest-argv.txt"))) {
                for (String a : guest) w.write(a + "\n");
            } catch (IOException e) {
                throw new RuntimeException(e);
            }
        }
        if (bootArgs.isEmpty()) bootArgs.addAll(lines(new File(d, "args.txt"), true));
        if (bootArgs.isEmpty()) {
            // Never configured: the launcher finds the game, prepares its files and writes the
            // arguments. SDL starts the game only once the activity resumes, which it will not.
            super.onCreate(savedInstanceState);
            startActivity(new Intent(this, Launcher.class));
            finish();
            return;
        }
        if (!guest.isEmpty()) {
            bootArgs.add("--guest-argv");
            bootArgs.add(d + "/guest-argv.txt");
        }
        if (game == null) game = gameOf(bootArgs.get(0));
        try {
            // The renderer keeps its pipeline cache and settings under $HOME (shadPS4 path_util).
            Os.setenv("HOME", d + "/home", true);
            Os.setenv("XDG_DATA_HOME", d + "/home/.local/share", true);
            Os.setenv("BB_CONFIG", d + "/bbport.ini", true);
            // The collection's shaders the upscaler hooks (docs/upscaler.md; run.py sets the same
            // on a PC): the camera velocity pass, the first compute after the lit scene, the
            // first draw into the display buffer. Without them the port never finds the scene and
            // FSR 3.1 / TAA stay off whatever the panel says. env.txt may override them.
            Os.setenv("BB_VELOCITY_PS", "f4b18264", false);
            Os.setenv("BB_UPSCALE_BEFORE_CS", "aec539f0", false);
            Os.setenv("BB_HUDLESS_PS", "484cdbf5", false);
            // The sun shadow pass rebuilds positions from the jittered depth: jittered with it,
            // or its shadows flicker under FSR 3.1 / TAA (upstream 0.3.9's run.py).
            Os.setenv("BB_JITTER_PS", "f614041e", false);
            // The games' own PlayStation buttons in their prompts: upstream's automatic choice
            // shows keyboard keys until a controller is used, and the touch gamepad is none.
            // BB_BUTTON_PROMPTS=xbox (or auto) in More environment picks another style.
            Os.setenv("BB_BUTTON_PROMPTS", "0", false);
            // Extracted native libraries: hook libraries and custom Vulkan drivers for
            // libadrenotools (BB_VK_DRIVER=libvulkan_freedreno.so selects Turnip).
            Os.setenv("BB_NATIVE_LIB_DIR", getApplicationInfo().nativeLibraryDir, true);
            // sceSystemServiceLoadExec writes its request here (runtime_services.c).
            Os.setenv("BB_LOADEXEC_FILE", d + "/loadexec.txt", true);
            // A memory peek for a hung game (src/probe.c: lines "<address> <length>" moved into
            // the file; the bytes come back in peek.txt.out). A thread polls for the file.
            Os.setenv("BB_PEEK_FILE", d + "/peek.txt", false);
            // files/env.txt: extra environment (KEY=VALUE per line), e.g. BB_DUMP_SHADERS=1; then
            // the started game's env-u1.txt, env-u2.txt or env-u3.txt (-KEY removes KEY).
            for (String name : new String[] {"env.txt", "env-" + game + ".txt"}) {
                for (String line : lines(new File(d, name), true)) {
                    int eq = line.indexOf('=');
                    if (line.startsWith("-")) Os.unsetenv(line.substring(1));
                    else if (eq > 0) Os.setenv(line.substring(0, eq), line.substring(eq + 1), true);
                }
            }
        } catch (Exception e) {
            throw new RuntimeException(e);
        }
        super.onCreate(savedInstanceState);
    }

    @Override
    protected String[] getLibraries() {
        // libbbgpu.so is not loaded on its own: its runtime_* imports live in libmain.so, which
        // loads it as a dependency (the bionic linker looks them up in that load group's root).
        return new String[] {"c++_shared", "SDL3", "avutil", "swresample", "swscale", "avcodec", "avformat",
                             "main"};
    }

    @Override
    protected String[] getArguments() {
        return bootArgs.toArray(new String[0]);
    }

    /** Called by the port (runtime_restart, through JNI on one of its threads) just before it ends
     *  this process: the game starts again in a new one (Restart). */
    public void restartApp() {
        Intent i = new Intent(this, Restart.class);
        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        i.putExtra("pid", Process.myPid());
        startActivity(i);
    }

    /** Starts GameActivity again once the old game process has ended (its GPU device and memory
     *  released); runs in a process of its own (android:process=":restart"). */
    public static class Restart extends Activity {
        @Override
        protected void onCreate(Bundle savedInstanceState) {
            super.onCreate(savedInstanceState);
            int pid = getIntent().getIntExtra("pid", 0);
            if (pid > 0) {
                Process.killProcess(pid);
                for (int i = 0; i < 200 && new File("/proc/" + pid).exists(); i++) {
                    try {
                        Thread.sleep(25);
                    } catch (InterruptedException e) {
                        break;
                    }
                }
            }
            Intent i = new Intent(this, GameActivity.class);
            i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
            startActivity(i);
            finish();
            Runtime.getRuntime().exit(0);
        }
    }
}
