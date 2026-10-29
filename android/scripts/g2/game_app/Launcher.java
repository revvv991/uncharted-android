package dev.unchartedg2.game;

import android.app.Activity;
import android.app.ActivityManager;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageInstaller;
import android.content.res.ColorStateList;
import android.content.res.Configuration;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.RippleDrawable;
import android.net.Uri;
import android.os.Bundle;
import android.os.Environment;
import android.provider.DocumentsContract;
import android.provider.Settings;
import android.system.Os;
import android.system.OsConstants;
import android.text.InputType;
import android.util.TypedValue;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;
import java.io.File;
import java.io.FileOutputStream;
import java.io.FileWriter;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.RandomAccessFile;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;
import org.json.JSONArray;
import org.json.JSONObject;

/** The app's first screen: the folder with the user's own decrypted game (a PS4 dump of
 *  Uncharted: The Nathan Drake Collection), the loader's files made from it on the device
 *  (libbbprepare.so: the port's scripts/prepare.py and link_modules.py in C), a few settings,
 *  then GameActivity with files/args*.txt and env*.txt written from them. Nothing of the game
 *  ships with the app: the patch presets are checked against the user's executables first.
 *  New versions come from the project's GitHub releases (Updater.java).
 *  adb automation: am start -n <app>/dev.unchartedg2.game.Launcher --es game_dir <path> [--ez backup_saves true]
 *  [--es driver turnip|system|pkg:<name>] [--es driver_zip <path>: install and select that package]
 *  [--es extra_env "KEY=VALUE\\nKEY=VALUE": the extra environment field]
 *  [--ez prepare true] [--ez play true]. */
public class Launcher extends Activity {
    static {
        System.loadLibrary("bbprepare");
    }

    /** null on success, else the reason (game_prepare.c). */
    static native String prepareGame(String gameDir, String exe, String outDir);

    /** A value of sce_sys/param.sfo as text, null if missing. */
    static native String sfoValue(String gameDir, String key);

    /** The three games: id, executable, folder under files/ ("" = files/ itself), the
     *  resolution recipe asset (scripts/g2/recipe_from_preset.py). */
    private static final String[][] GAMES = {
        {"u1", "eboot.bin", "", "recipes/u1.json"},
        {"u2", "big2-ps4_Shipping.elf", "u2", "recipes/u2.json"},
        {"u3", "big3-ps4_Shipping.elf", "u3", "recipes/u3.json"},
    };
    /** The sizes the game can render at: its own 1920x1080 and three below it, made by the
     *  recipes' byte patches (the display configuration's size pairs and the light tile grid). */
    private static final String[][] RESOLUTIONS = {
        {"640x360", "640x360 (fastest, a soft picture)"},
        {"960x540", "960x540"},
        {"1280x720", "1280x720 (recommended)"},
        {"1920x1080", "1920x1080, the game's own (much slower)"},
    };
    private static final String[] MODULES = {"sce_module/libc.prx", "sce_module/libSceFios2.prx", "sce_sys/param.sfo"};
    private static final int PICK_FOLDER = 1, PICK_DRIVER = 2;
    /** Where driver packages are downloaded from: the GitHub releases of the community Turnip
     *  builds (the zips the Android emulators share: meta.json and the driver library). */
    private static final String[] DRIVER_SOURCES = {
        "K11MCH1/AdrenoToolsDrivers", "MrPurple666/purple-turnip", "StevenMXZ/Adreno-Tools-Drivers",
    };

    private SharedPreferences prefs;
    private File files;
    private TextView folderText, folderInfo, prepareInfo, statusText;
    private EditText pathEdit, extraEnv;
    private Button accessButton, prepareButton, playButton;
    private RadioGroup resolution;
    private RadioGroup fpsLimit;
    private RadioGroup touchPad;
    private CheckBox showFps, uncapped, accurateGpu;
    private LinearLayout driverList;
    private RadioGroup drivers;
    private volatile boolean busy;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        // The home screen icon while a game runs: back to it, not a new menu. GameActivity is a
        // task of its own (singleInstance; after a restart into U2/U3 too) and this activity
        // finishes when it starts the game, so the icon found no task of the launcher to resume.
        Intent start = getIntent();
        if (Intent.ACTION_MAIN.equals(start.getAction()) && start.hasCategory(Intent.CATEGORY_LAUNCHER)
                && resumeGame()) {
            finish();
            return;
        }
        prefs = getSharedPreferences("launcher", MODE_PRIVATE);
        // Scripted tests (adb shell am start ... --es resolution 960x540 --es fps_limit 30): the
        // same settings a finger sets, stored before the views read them.
        if (getIntent().hasExtra("resolution")) prefs.edit().putString("resolution", getIntent().getStringExtra("resolution")).apply();
        if (getIntent().hasExtra("fps_limit")) prefs.edit().putString("fps_limit", getIntent().getStringExtra("fps_limit")).apply();
        if (getIntent().hasExtra("touch_pad")) prefs.edit().putString("touch_pad", getIntent().getStringExtra("touch_pad")).apply();
        if (getIntent().hasExtra("driver")) prefs.edit().putString("driver", getIntent().getStringExtra("driver")).apply();
        if (getIntent().hasExtra("extra_env")) // "\\n" between the lines (adb passes one string)
            prefs.edit().putString("extra_env", getIntent().getStringExtra("extra_env").replace("\\n", "\n")).apply();
        if (getIntent().hasExtra("driver_zip")) {
            try (InputStream in = new java.io.FileInputStream(getIntent().getStringExtra("driver_zip"))) {
                prefs.edit().putString("driver", "pkg:" + installDriver(in, "driver")).apply();
            } catch (IOException e) {
                android.util.Log.w("Launcher", "driver_zip: " + e.getMessage());
            }
        }
        File d = getExternalFilesDir(null);
        files = d != null ? d : getFilesDir();
        buildViews();
        handle(getIntent());
        initUpdates(getIntent());
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        handle(intent);
    }

    /** Brings the running game's task to the front; false when no game runs. */
    private boolean resumeGame() {
        ActivityManager am = getSystemService(ActivityManager.class);
        if (am == null) return false;
        for (ActivityManager.AppTask task : am.getAppTasks()) {
            try {
                ActivityManager.RecentTaskInfo info = task.getTaskInfo();
                if (info.isRunning && info.baseActivity != null
                        && GameActivity.class.getName().equals(info.baseActivity.getClassName())) {
                    task.moveToFront();
                    return true;
                }
            } catch (IllegalArgumentException gone) {
                // the task ended meanwhile
            }
        }
        return false;
    }

    /** The adb automation extras: game_dir, backup_saves, prepare, play; the package installer's
     *  answers about an update. */
    private void handle(Intent intent) {
        if (Updater.ACTION_INSTALL_STATUS.equals(intent.getAction())) {
            onInstallStatus(intent);
            return;
        }
        String dir = intent.getStringExtra("game_dir");
        if (dir != null) setFolder(dir);
        refresh();
        if (busy) return;
        if (intent.getBooleanExtra("backup_saves", false)) {
            String done = backupSaves();
            status(done != null ? "Saves copied to " + done : "Backing up failed.");
        }
        if (intent.getBooleanExtra("prepare", false)) prepare(intent.getBooleanExtra("play", false));
        else if (intent.getBooleanExtra("play", false) && prepared()) play();
    }

    @Override
    protected void onResume() {
        super.onResume();
        refresh(); // back from the system's "all files access" screen
        // Back from "install unknown apps" with an update waiting for it.
        if (pendingInstall != null && getPackageManager().canRequestPackageInstalls()) installUpdate(pendingInstall);
    }

    // ---- views ----
    // Plain widgets styled by hand (no AndroidX in this build): the theme's accent colour, sections
    // as rounded cards, a filled Play button, outlined secondary buttons, muted explanations.

    private int accent, muted, cardColor;

    private void initPalette() {
        TypedValue v = new TypedValue();
        accent = 0xFF3D8BFF;
        if (getTheme().resolveAttribute(android.R.attr.colorAccent, v, true)) {
            if (v.type >= TypedValue.TYPE_FIRST_COLOR_INT && v.type <= TypedValue.TYPE_LAST_COLOR_INT)
                accent = v.data;
            else if (v.resourceId != 0) accent = getColor(v.resourceId);
        }
        boolean night = (getResources().getConfiguration().uiMode & Configuration.UI_MODE_NIGHT_MASK)
                == Configuration.UI_MODE_NIGHT_YES;
        muted = night ? 0xB3FFFFFF : 0x99000000;
        cardColor = night ? 0x16FFFFFF : 0x0D000000;
    }

    private TextView text(LinearLayout parent, String s, float size, boolean bold) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(size);
        if (bold) t.setTypeface(Typeface.DEFAULT_BOLD);
        t.setPadding(0, dp(6), 0, dp(2));
        parent.addView(t);
        return t;
    }

    private TextView note(LinearLayout parent, String s) {
        TextView t = text(parent, s, 14, false);
        t.setTextColor(muted);
        t.setLineSpacing(0, 1.1f);
        return t;
    }

    private LinearLayout card(LinearLayout parent, String title) {
        LinearLayout c = new LinearLayout(this);
        c.setOrientation(LinearLayout.VERTICAL);
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(cardColor);
        bg.setCornerRadius(dp(16));
        c.setBackground(bg);
        c.setPadding(dp(18), dp(12), dp(18), dp(16));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                                                                     ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(12);
        parent.addView(c, lp);
        if (title != null) text(c, title, 17, true).setPadding(0, 0, 0, dp(4));
        return c;
    }

    private LinearLayout row(LinearLayout parent) {
        LinearLayout r = new LinearLayout(this);
        r.setOrientation(LinearLayout.HORIZONTAL);
        parent.addView(r);
        return r;
    }

    private Button button(LinearLayout parent, String s, View.OnClickListener l) {
        Button b = new Button(this, null, android.R.attr.borderlessButtonStyle);
        b.setText(s);
        b.setAllCaps(false);
        b.setOnClickListener(l);
        b.setTextColor(new ColorStateList(new int[][] {{-android.R.attr.state_enabled}, {}},
                                          new int[] {muted, accent}));
        b.setMinHeight(dp(44));
        b.setMinimumHeight(dp(44));
        b.setPadding(dp(18), 0, dp(18), 0);
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(Color.TRANSPARENT);
        bg.setStroke(dp(1), (accent & 0x00FFFFFF) | 0x66000000);
        bg.setCornerRadius(dp(22));
        b.setBackground(new RippleDrawable(ColorStateList.valueOf((accent & 0x00FFFFFF) | 0x33000000), bg, null));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                                                                     ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(8);
        lp.rightMargin = dp(8);
        parent.addView(b, lp);
        return b;
    }

    private Button primaryButton(LinearLayout parent, String s, View.OnClickListener l) {
        Button b = new Button(this, null, android.R.attr.borderlessButtonStyle);
        b.setText(s);
        b.setAllCaps(false);
        b.setOnClickListener(l);
        b.setTextSize(18);
        b.setTypeface(Typeface.DEFAULT_BOLD);
        b.setTextColor(Color.WHITE);
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(new ColorStateList(new int[][] {{-android.R.attr.state_enabled}, {}},
                                       new int[] {(accent & 0x00FFFFFF) | 0x59000000, accent}));
        bg.setCornerRadius(dp(28));
        b.setBackground(new RippleDrawable(ColorStateList.valueOf(0x33FFFFFF), bg, null));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(56));
        lp.topMargin = dp(14);
        parent.addView(b, lp);
        return b;
    }

    private CheckBox check(LinearLayout parent, String s, String key, boolean byDefault) {
        CheckBox c = new CheckBox(this);
        c.setText(s);
        c.setChecked(prefs.getBoolean(key, byDefault));
        c.setOnCheckedChangeListener((v, on) -> prefs.edit().putBoolean(key, on).apply());
        parent.addView(c);
        return c;
    }

    private int dp(int v) {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }

    private void buildViews() {
        initPalette();
        ScrollView scroll = new ScrollView(this);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(dp(20), dp(20), dp(20), dp(28));
        scroll.addView(root);

        text(root, "Uncharted: The Nathan Drake Collection", 24, true);
        String version = "";
        try {
            version = " " + getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
        } catch (Exception e) {
        }
        note(root, "uncharted-android" + version + ": uncharted_pc for Android, the PS4 game's code on "
                 + "FEXCore and the shadPS4 renderer. It needs your own copy of the game, already "
                 + "decrypted (a dump of CUSA02344, CUSA02320 or CUSA02343, version 01.02 for the "
                 + "sizes below 1920x1080).");

        // A newer release (Updater.java): shown by showUpdates, tinted with the accent.
        updateCard = card(root, null);
        ((GradientDrawable) updateCard.getBackground()).setColor((accent & 0x00FFFFFF) | 0x26000000);
        updateTitle = text(updateCard, "", 17, true);
        updateTitle.setPadding(0, 0, 0, dp(2));
        updateInfo = note(updateCard, "");
        LinearLayout updateRow = row(updateCard);
        button(updateRow, "Update", v -> startUpdate());
        button(updateRow, "What's new", v -> offerUpdate(true));
        updateCard.setVisibility(View.GONE);

        LinearLayout folder = card(root, "Game folder");
        folderText = text(folder, "", 15, false);
        folderInfo = note(folder, "");
        LinearLayout row = row(folder);
        button(row, "Choose folder", v -> {
            Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
            startActivityForResult(i, PICK_FOLDER);
        });
        accessButton = button(row, "Allow access to all files", v -> {
            Intent i = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                                  Uri.parse("package:" + getPackageName()));
            startActivity(i);
        });
        pathEdit = new EditText(this);
        pathEdit.setHint("or type the path, e.g. /storage/XXXX-XXXX/PS4/CUSA02344");
        pathEdit.setSingleLine(true);
        pathEdit.setTextSize(14);
        pathEdit.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        folder.addView(pathEdit);
        button(folder, "Use this path", v -> {
            String p = pathEdit.getText().toString().trim();
            if (!p.isEmpty()) setFolder(p);
            refresh();
        });

        LinearLayout prepareCard = card(root, "Game files");
        prepareInfo = note(prepareCard, "");
        prepareButton = button(prepareCard, "Prepare", v -> prepare(false));

        LinearLayout page = root;
        root = card(page, "Settings"); // the settings widgets below go into the card
        text(root, "The game renders at:", 14, false);
        resolution = new RadioGroup(this);
        String chosen = chosenResolution();
        for (String[] r : RESOLUTIONS) {
            RadioButton b = new RadioButton(this);
            b.setText(r[1]);
            b.setId(View.generateViewId());
            b.setTag(r[0]);
            resolution.addView(b);
            if (r[0].equals(chosen)) resolution.check(b.getId());
        }
        resolution.setOnCheckedChangeListener((g, id) -> {
            RadioButton b = g.findViewById(id);
            if (b != null) prefs.edit().putString("resolution", (String) b.getTag()).apply();
        });
        root.addView(resolution);
        text(root, "Below 1920x1080 the game's size is set by a few byte patches made here from your own "
                 + "executables (the app ships recipes, no code of the game) and the movies play through the "
                 + "port's decoder. FSR 3.1 and TAA (the panel in the game) are anti-aliasing at that size; the port "
                 + "scales the picture to the screen.", 14, false);
        text(root, "Vulkan driver:", 14, false);
        driverList = new LinearLayout(this);
        driverList.setOrientation(LinearLayout.VERTICAL);
        root.addView(driverList);
        buildDriverList();
        LinearLayout driverRow = row(root);
        button(driverRow, "Add a package (zip)", v -> {
            Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            i.addCategory(Intent.CATEGORY_OPENABLE);
            i.setType("*/*");
            i.putExtra(Intent.EXTRA_MIME_TYPES, new String[] {"application/zip", "application/octet-stream"});
            startActivityForResult(i, PICK_DRIVER);
        });
        button(driverRow, "Download...", v -> downloadDriver());
        button(driverRow, "Remove", v -> removeDriver());
        text(root, "The bundled Turnip (tested, with the port's KGSL fixes) is the one to keep on Adreno. Driver "
                 + "packages are the zips the Android emulators use (meta.json and the driver library): the Turnip "
                 + "builds by K11MCH1, MrPurple, StevenMXZ (the Gen8 ones for Adreno 8xx: Snapdragon 8 Elite) and "
                 + "others, or Qualcomm's own driver builds. They are untested with these games; if the bundled "
                 + "driver gives a black screen, try one of them.", 14, false);
        ActivityManager.MemoryInfo memory = new ActivityManager.MemoryInfo();
        ((ActivityManager) getSystemService(ACTIVITY_SERVICE)).getMemoryInfo(memory);
        // An 8 GB device shows ~7.1-7.5 GiB to apps (the G2: 7.15), a 6 GB one ~5.5.
        if (memory.totalMem < (6656L << 20))
            text(root, "This device has less than 8 GB of RAM: the games use about 6 GB (tested with 8 GB), "
                     + "so the system may close them.", 14, false);
        uncapped = check(root, "Show each frame at once (up to the display's refresh rate)", "uncapped", true);
        showFps = check(root, "Show the frame rate", "show_fps", false);
        text(root, "Frame rate limit:", 14, false);
        fpsLimit = new RadioGroup(this);
        fpsLimit.setOrientation(RadioGroup.HORIZONTAL);
        // Shared with the in-game panel through bbport.ini: what the panel last chose is shown.
        String current = iniGet("fps_limit");
        if (current == null || current.equals("auto")) current = prefs.getString("fps_limit", defaultFpsLimit());
        for (String l : new String[] {"30", "60", "120", "Off"}) {
            RadioButton b = new RadioButton(this);
            b.setText(l);
            b.setId(View.generateViewId());
            b.setTag(l.equals("Off") ? "0" : l);
            fpsLimit.addView(b);
            if (b.getTag().equals(current)) fpsLimit.check(b.getId());
        }
        fpsLimit.setOnCheckedChangeListener((g, id) -> {
            RadioButton b = g.findViewById(id);
            if (b != null) {
                prefs.edit().putString("fps_limit", (String) b.getTag()).apply();
                iniPut("fps_limit", (String) b.getTag());
            }
        });
        root.addView(fpsLimit);
        text(root, "Off: no limit; above 120 FPS the game's movement timing breaks. The panel in the game "
                 + "has the same setting.", 14, false);
        text(root, "On-screen gamepad:", 14, false);
        touchPad = new RadioGroup(this);
        touchPad.setOrientation(RadioGroup.HORIZONTAL);
        String pad = iniGet("touch_pad");
        if (pad == null) pad = prefs.getString("touch_pad", "auto");
        for (String[] o : new String[][] {{"0", "Off"}, {"auto", "Auto"}, {"1", "On"}}) {
            RadioButton b = new RadioButton(this);
            b.setText(o[1]);
            b.setId(View.generateViewId());
            b.setTag(o[0]);
            touchPad.addView(b);
            if (o[0].equals(pad)) touchPad.check(b.getId());
        }
        touchPad.setOnCheckedChangeListener((g, id) -> {
            RadioButton b = g.findViewById(id);
            if (b != null) {
                prefs.edit().putString("touch_pad", (String) b.getTag()).apply();
                iniPut("touch_pad", (String) b.getTag());
            }
        });
        root.addView(touchPad);
        text(root, "Auto: shown while no gamepad is connected (phones). Left half of the screen: a stick under "
                 + "the finger; right half: drag to look around; the buttons at the edges. The panel in the game "
                 + "has the same setting with opacity and size.", 14, false);
        accurateGpu = check(root, "Uncharted 2: accurate GPU memory", "u2_accurate_gpu", true);
        text(root, "Uncharted 2 reads the results of some GPU work back: grenade explosions, vehicle damage, the "
                 + "convoy of chapter 21. Off: a few percent faster in busy scenes, and those break.", 14, false);
        note(root, "In the game, swipe in from the right edge of the screen (or press L3+R3) for the settings "
                 + "panel: upscaling, sharpening, frame generation and more, applied at once.");
        text(root, "More environment for the port (KEY=VALUE per line, optional):", 14, false);
        extraEnv = new EditText(this);
        extraEnv.setText(prefs.getString("extra_env", ""));
        extraEnv.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_MULTI_LINE | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        extraEnv.setMinLines(2);
        root.addView(extraEnv);
        root = page;

        playButton = primaryButton(root, "Play", v -> play());
        statusText = note(root, "");

        LinearLayout savesCard = card(root, "Saves");
        note(savesCard, "The game's saves are in the app's own folder, which Android deletes together with the app. "
                      + "Back them up to " + backups() + " before uninstalling.");
        LinearLayout saves = row(savesCard);
        button(saves, "Back up saves", v -> {
            String done = backupSaves();
            status(done != null ? "Saves copied to " + done : "Backing up failed (no saves yet, or no access).");
        });
        button(saves, "Restore saves", v -> restoreSaves());
        LinearLayout helpCard = card(root, "Bug reports");
        note(helpCard, "The game writes its log to the app's own folder, which other apps cannot read on recent "
                     + "Android. Copy it to " + backups() + " to attach it to a report.");
        button(helpCard, "Copy the log", v -> {
            String done = exportLog();
            status(done != null ? "Log copied to " + done : "No log yet (the game has not run), or no access.");
        });
        String repo = Updater.repo(this);
        if (repo != null) {
            LinearLayout updatesCard = card(root, "Updates");
            note(updatesCard, "This is version " + Updater.installed(this) + ". New versions come from github.com/" + repo
                              + "/releases: the app downloads the APK from there and checks it (its checksum, and that it is "
                              + "this app signed with the same key), then Android installs it over this one and keeps the "
                              + "settings and saves. The first time, Android asks to let this app install apps.");
            check(updatesCard, "Check for a new version when the app starts", "update_check", true);
            button(updatesCard, "Check now", v -> checkUpdates(true));
            updateCheckText = note(updatesCard, "");
            updateCheckText.setVisibility(View.GONE);
        }
        button(root, "Licenses", v -> showLicenses());
        setContentView(scroll);
    }

    // ---- the game folder ----

    private String folder() {
        return prefs.getString("game_dir", "");
    }

    private void setFolder(String path) {
        while (path.length() > 1 && path.endsWith("/")) path = path.substring(0, path.length() - 1);
        prefs.edit().putString("game_dir", path).apply();
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (result != RESULT_OK || data == null || data.getData() == null) return;
        if (request == PICK_DRIVER) {
            Uri uri = data.getData();
            status("Installing the driver package ...");
            new Thread(() -> {
                try (InputStream in = getContentResolver().openInputStream(uri)) {
                    String name = installDriver(in, "driver");
                    prefs.edit().putString("driver", "pkg:" + name).apply();
                    runOnUiThread(this::buildDriverList);
                    status("Driver package installed and selected: " + name);
                } catch (Exception e) {
                    status("Installing the package failed: " + e.getMessage());
                }
            }).start();
            return;
        }
        if (request != PICK_FOLDER) return;
        String path = treeToPath(data.getData());
        if (path == null) status("That folder is not on the device's own storage or SD card: type its path instead.");
        else setFolder(path);
        refresh();
    }

    /** A folder picked with ACTION_OPEN_DOCUMENT_TREE as a file path (the native loader opens
     *  files by path): "primary:x" is internal storage, "XXXX-XXXX:x" a volume under /storage. */
    private static String treeToPath(Uri tree) {
        if (!"com.android.externalstorage.documents".equals(tree.getAuthority())) return null;
        String id = DocumentsContract.getTreeDocumentId(tree);
        int colon = id.indexOf(':');
        if (colon < 0) return null;
        String volume = id.substring(0, colon), rest = id.substring(colon + 1);
        String base = volume.equals("primary") ? Environment.getExternalStorageDirectory().getAbsolutePath()
                                               : "/storage/" + volume;
        return rest.isEmpty() ? base : base + "/" + rest;
    }

    /** What is missing in the folder, or null when it has the three games and their modules. */
    private String problems(String dir) {
        // FEXCore manages memory in 4 KB pages (FEX_PAGE_SIZE); a kernel with 16 KB pages refuses it.
        long page = Os.sysconf(OsConstants._SC_PAGESIZE);
        if (page != 4096)
            return "This device uses " + page / 1024 + " KB memory pages: the x86-64 emulation (FEX) needs 4 KB pages.";
        if (dir.isEmpty()) return "Choose the folder of your decrypted game.";
        if (!new File(dir).isDirectory()) return "Not a folder the app can open: " + dir;
        List<String> missing = new ArrayList<>();
        for (String[] g : GAMES)
            if (!new File(dir, g[1]).isFile()) missing.add(g[1]);
        for (String m : MODULES)
            if (!new File(dir, m).isFile()) missing.add(m);
        if (!missing.isEmpty()) return "Missing in the folder: " + String.join(", ", missing);
        return null;
    }

    /** The stamp of a preparation: the folder and its executables' sizes and times. */
    private String stamp(String dir) {
        StringBuilder b = new StringBuilder(dir).append('\n');
        for (String[] g : GAMES) {
            File f = new File(dir, g[1]);
            b.append(g[1]).append(' ').append(f.length()).append(' ').append(f.lastModified()).append('\n');
        }
        return b.toString();
    }

    private boolean prepared() {
        String dir = folder();
        if (dir.isEmpty()) return false;
        for (String[] g : GAMES) {
            File out = g[2].isEmpty() ? files : new File(files, g[2]);
            if (!new File(out, "boot-linked.bin").isFile() || !new File(out, "content.bin").isFile()) return false;
        }
        return stamp(dir).equals(readText(new File(files, "prepared.txt")));
    }

    private void refresh() {
        String dir = folder();
        boolean access = Environment.isExternalStorageManager();
        accessButton.setVisibility(access ? View.GONE : View.VISIBLE);
        folderText.setText(dir.isEmpty() ? "(none)" : dir);
        String problem = access ? problems(dir) : "Allow the app to read all files first (the game's folder is opened by path).";
        if (problem == null) {
            // (APP_VER is the base game's even when the folder has the patched executables.)
            String title = sfoValue(dir, "TITLE"), id = sfoValue(dir, "TITLE_ID");
            folderInfo.setText((title != null ? title : "?") + "  " + (id != null ? id : "?"));
        } else {
            folderInfo.setText(problem);
        }
        boolean ready = problem == null && prepared();
        String presets = "";
        if (ready) {
            List<String> fit = new ArrayList<>(), not = new ArrayList<>();
            for (String[] g : GAMES) {
                File out = g[2].isEmpty() ? files : new File(files, g[2]);
                (recipeFits(g[3], new File(out, "boot-linked.bin")) ? fit : not).add(g[0].toUpperCase(Locale.ROOT));
            }
            presets = not.isEmpty() ? " The resolution recipes fit all three games."
                    : " The resolution recipes fit " + (fit.isEmpty() ? "none of the games" : String.join(", ", fit))
                      + " (another version of the executables): " + String.join(", ", not)
                      + " run at 1920x1080 without patches.";
        }
        prepareInfo.setText(problem != null ? "-" : ready ? "Prepared for this folder." + presets
                                                     : "Not prepared yet: the app makes the loader's files from the game (a few seconds).");
        prepareButton.setEnabled(problem == null && !busy);
        playButton.setEnabled(ready && !busy);
    }

    private void status(String s) {
        runOnUiThread(() -> statusText.setText(s));
    }

    // ---- preparing ----

    private void prepare(boolean thenPlay) {
        String dir = folder();
        String problem = problems(dir);
        if (problem != null) {
            status(problem);
            return;
        }
        busy = true;
        refresh();
        new Thread(() -> {
            String error = null;
            for (String[] g : GAMES) {
                File out = g[2].isEmpty() ? files : new File(files, g[2]);
                out.mkdirs();
                status("Preparing " + g[1] + " ...");
                error = prepareGame(dir, g[1], out.getAbsolutePath());
                if (error != null) {
                    error = g[1] + ": " + error;
                    break;
                }
            }
            if (error == null) writeText(new File(files, "prepared.txt"), stamp(dir));
            final String failure = error;
            runOnUiThread(() -> {
                busy = false;
                statusText.setText(failure == null ? "The game's files are ready." : "Preparing failed: " + failure);
                refresh();
                if (failure == null && thenPlay) play();
            });
        }).start();
    }

    // ---- playing ----

    /** The chosen size, "WxH" (the old "native" setting maps to 1920x1080). */
    private String chosenResolution() {
        return prefs.getString("resolution", prefs.getBoolean("native", false) ? "1920x1080" : "1280x720");
    }

    /** Whether a recipe fits the prepared executable: every write's original bytes hash as the
     *  recipe says (another version of the game has other bytes there). */
    private boolean recipeFits(String recipe, File boot) {
        return makePreset(recipe, boot, 1280, 720) != null;
    }

    /** The BBPATCH2 preset for WxH from a recipe asset and the prepared executable
     *  (boot-linked.bin: the image is the file's tail), as scripts/g2/recipe_from_preset.py makes
     *  it: a write is the executable's own bytes with the recipe's edits and its resolution fields
     *  (W, H, W/8, H/4) put in; writes marked below_1080p are left out at 1920x1080. Null when a
     *  write's original bytes do not hash as the recipe says. */
    private byte[] makePreset(String recipe, File boot, int width, int height) {
        String text = readAsset(recipe);
        if (text == null) return null;
        try (RandomAccessFile f = new RandomAccessFile(boot, "r")) {
            byte[] head = new byte[16];
            f.readFully(head);
            long imageSize = ByteBuffer.wrap(head, 8, 8).order(ByteOrder.LITTLE_ENDIAN).getLong();
            long imageAt = f.length() - imageSize;
            JSONObject json = new JSONObject(text);
            long base = Long.decode(json.getString("base"));
            JSONArray writes = json.getJSONArray("writes");
            MessageDigest sha = MessageDigest.getInstance("SHA-256");
            List<long[]> places = new ArrayList<>();
            List<byte[]> contents = new ArrayList<>();
            int total = 0;
            for (int i = 0; i < writes.length(); i++) {
                JSONObject w = writes.getJSONObject(i);
                if (w.getString("when").equals("below_1080p") && height >= 1080) continue;
                long offset = Long.decode(w.getString("address")) - base;
                int length = w.getInt("length");
                if (offset < 0 || offset + length > imageSize) return null;
                byte[] data = new byte[length];
                f.seek(imageAt + offset);
                f.readFully(data);
                if (!hex(sha.digest(data)).equals(w.getString("sha256"))) return null;
                JSONArray edits = w.optJSONArray("edits");
                for (int j = 0; edits != null && j < edits.length(); j++) {
                    JSONObject e = edits.getJSONObject(j);
                    byte[] b = unhex(e.getString("bytes"));
                    System.arraycopy(b, 0, data, e.getInt("offset"), b.length);
                }
                JSONArray fields = w.optJSONArray("fields");
                for (int j = 0; fields != null && j < fields.length(); j++) {
                    JSONObject fd = fields.getJSONObject(j);
                    String formula = fd.getString("value");
                    int value = formula.equals("W") ? width : formula.equals("H") ? height
                              : formula.equals("W/8") ? width / 8 : formula.equals("H/4") ? height / 4 : -1;
                    if (value < 0) return null;
                    int at = fd.getInt("offset"), bytes = fd.getInt("width");
                    for (int k = 0; k < bytes; k++) data[at + k] = (byte) (value >> (8 * k));
                }
                places.add(new long[] {offset, length});
                contents.add(data);
                total += length;
            }
            ByteBuffer out = ByteBuffer.allocate(24 + places.size() * 16 + total).order(ByteOrder.LITTLE_ENDIAN);
            out.put("BBPATCH2".getBytes("US-ASCII")).putLong(base).putLong(places.size());
            for (int i = 0; i < places.size(); i++)
                out.putLong(places.get(i)[0]).putLong(places.get(i)[1]).put(contents.get(i));
            return out.array();
        } catch (Exception e) {
            return null;
        }
    }

    private static String hex(byte[] bytes) {
        StringBuilder s = new StringBuilder();
        for (byte b : bytes) s.append(String.format(Locale.ROOT, "%02x", b));
        return s.toString();
    }

    private static byte[] unhex(String s) {
        byte[] out = new byte[s.length() / 2];
        for (int i = 0; i < out.length; i++) out[i] = (byte) Integer.parseInt(s.substring(2 * i, 2 * i + 2), 16);
        return out;
    }

    /** The CPUs of every cluster but the slowest (the port's GPU threads: BB_RENDER_CPUS), when
     *  the slowest is little cores (under 3/4 of the fastest clock; the G2's run at 0.68). Else
     *  none: on a Snapdragon 8 Elite (2 + 6 big cores, 0.79) it put them all on two (issue #8). */
    private static String fastCpus() {
        List<Integer> cpus = new ArrayList<>();
        List<Long> freqs = new ArrayList<>();
        for (int i = 0; i < 64; i++) {
            String f = readText(new File("/sys/devices/system/cpu/cpu" + i + "/cpufreq/cpuinfo_max_freq")).trim();
            if (f.isEmpty()) {
                if (!new File("/sys/devices/system/cpu/cpu" + i).exists()) break;
                continue;
            }
            cpus.add(i);
            freqs.add(Long.parseLong(f));
        }
        if (cpus.isEmpty()) return null;
        long slowest = Long.MAX_VALUE, fastest = 0;
        for (long f : freqs) {
            slowest = Math.min(slowest, f);
            fastest = Math.max(fastest, f);
        }
        if (slowest * 4 >= fastest * 3) return null;
        StringBuilder b = new StringBuilder();
        for (int i = 0; i < cpus.size(); i++)
            if (freqs.get(i) > slowest) b.append(b.length() > 0 ? "," : "").append(cpus.get(i));
        return b.length() > 0 ? b.toString() : null;
    }

    /** 60 whatever the display does: at 120 a phone's GPU falls behind and the game runs
     *  ahead of it (issue #5); 120 stays a choice. */
    private String defaultFpsLimit() {
        return "60";
    }

    /** The port's settings file (bbport.ini, BB_CONFIG): the in-game panel writes it; the
     *  launcher reads and sets the keys it shares with the panel. */
    private File iniFile() {
        return new File(files, "bbport.ini");
    }

    private String iniGet(String key) {
        File f = iniFile();
        if (!f.exists()) return null;
        try {
            for (String line : new String(java.nio.file.Files.readAllBytes(f.toPath()), "UTF-8").split("\n")) {
                int eq = line.indexOf('=');
                if (eq > 0 && line.substring(0, eq).trim().equals(key)) return line.substring(eq + 1).trim();
            }
        } catch (java.io.IOException e) { }
        return null;
    }

    private void iniPut(String key, String value) {
        File f = iniFile();
        StringBuilder out = new StringBuilder();
        boolean done = false;
        try {
            if (f.exists())
                for (String line : new String(java.nio.file.Files.readAllBytes(f.toPath()), "UTF-8").split("\n")) {
                    int eq = line.indexOf('=');
                    if (eq > 0 && line.substring(0, eq).trim().equals(key)) {
                        if (!done) out.append(key).append('=').append(value).append('\n');
                        done = true;
                    } else if (!line.trim().isEmpty()) {
                        out.append(line.trim()).append('\n');
                    }
                }
        } catch (java.io.IOException e) { }
        if (!done) out.append(key).append('=').append(value).append('\n');
        writeText(f, out.toString());
    }

    private void play() {
        if (!prepared()) {
            status("Prepare the game's files first.");
            return;
        }
        prefs.edit().putString("extra_env", extraEnv.getText().toString()).apply();
        String dir = folder(), d = files.getAbsolutePath();
        String[] size = chosenResolution().split("x");
        int width = Integer.parseInt(size[0]), height = Integer.parseInt(size[1]);
        boolean below1080 = height < 1080;
        new File(files, "patches").mkdirs();
        new File(files, "user").mkdirs();
        StringBuilder report = new StringBuilder();
        String[] env = new String[GAMES.length];
        for (int i = 0; i < GAMES.length; i++) {
            String[] g = GAMES[i];
            String out = g[2].isEmpty() ? d : d + "/" + g[2];
            String preset = null;
            // The preset for the chosen size, made from the recipe and the prepared executable;
            // at 1920x1080 it holds only the writes that apply at every size (empty for U1).
            byte[] made = makePreset(g[3], new File(out, "boot-linked.bin"), width, height);
            if (made == null) {
                if (below1080)
                    report.append(g[0].toUpperCase(Locale.ROOT)).append(": the resolution recipe does not fit this version of the executable, it runs at 1920x1080 without patches. ");
            } else if (made.length > 24) {
                File p = new File(files, "patches/" + g[0] + "-" + width + "x" + height + ".bin");
                try (FileOutputStream o = new FileOutputStream(p)) {
                    o.write(made);
                    preset = p.getAbsolutePath();
                } catch (IOException e) {
                    report.append(g[0].toUpperCase(Locale.ROOT)).append(": cannot write its patches. ");
                }
            }
            StringBuilder args = new StringBuilder();
            args.append(out).append("/boot-linked.bin\n--content-profile\n").append(out).append("/content.bin\n")
                .append("--app0\n").append(dir).append("\n--user\n").append(d).append("/user\n")
                .append("--timeout\n0\n--log\n").append(d).append("/bb-probe.log\n");
            if (preset != null) args.append("--patches\n").append(preset).append('\n');
            writeText(new File(files, "args-" + g[0] + ".txt"), args.toString());
            if (g[0].equals("u1")) writeText(new File(files, "args.txt"), args.toString());
            // The movie decoder's picture size goes with a size below 1080p (port 0015).
            env[i] = preset != null && below1080 ? "BB_VDEC_CONTENT=" + width + "x" + height + "\n" : "-BB_VDEC_CONTENT\n";
            // U2's aiming autofocus reads a 1x1 render result on the CPU: copied back for U2
            // only, as upstream does (issue #7; it costs elsewhere). More environment overrides.
            if (g[0].equals("u2") && !extraEnv.getText().toString().contains("BB_TINY_READBACK"))
                env[i] += "BB_TINY_READBACK=1\n";
            // U2 reads GPU results back from memory it also writes (grenades, vehicle damage,
            // chapter 21's convoy), which merging leaves on the GPU: accurate GPU memory, as
            // upstream runs U2. More environment overrides.
            if (g[0].equals("u2") && accurateGpu.isChecked() && !extraEnv.getText().toString().contains("BB_GPU_MERGE"))
                env[i] += "BB_GPU_MERGE=0\n";
            // U2's mesh raycasters: their GPU results reach the record arrays the game reads,
            // once the GPU has made them (without it the game asserts "iSubMesh <
            // m_numSubMeshInstances" when firing, issue #21; port 0108). More environment
            // overrides.
            if (g[0].equals("u2") && !extraEnv.getText().toString().contains("BB_SCALAR_COPY64"))
                env[i] += "BB_SCALAR_COPY64_SYNC=2\nBB_SCALAR_COPY64_DEFER=1\n";
        }
        StringBuilder e = new StringBuilder();
        e.append("# Written by the launcher on every start; edit the settings there.\n");
        e.append("BB_FULLSCREEN=1\nBB_FPS_LOG=1\nBB_MIDFRAME_FLUSH=10\nBB_DEFERRED_SUBMIT=1\nBB_SKIN_BATCH=1\n");
        e.append("BB_CS_SPLIT=256:8f067540\nBB_OBJECT_MOTION=0\nBB_THREAD_NICE=0\n");
        // The game waits when it is two frames ahead of the GPU side (gnmdriver.cpp): a GPU that
        // falls behind (shaders compiling, a slow device) made it trip its own command buffer
        // check (issue #5); no cost on the G2.
        e.append("BB_MAX_FRAMES_AHEAD=2\n");
        e.append("BB_DUMP_REQUEST=").append(d).append("/dump-request\n");
        String cpus = fastCpus();
        if (cpus != null) e.append("BB_RENDER_CPUS=").append(cpus).append('\n');
        String driver = driver();
        if (driver.equals("turnip")) e.append("BB_VK_DRIVER=libvulkan_freedreno.so\n");
        else if (driver.startsWith("pkg:")) {
            File pkg = new File(driversDir(), driver.substring(4));
            String lib = packageLibrary(pkg);
            if (lib != null) e.append("BB_VK_DRIVER=").append(lib).append("\nBB_VK_DRIVER_DIR=").append(pkg.getAbsolutePath()).append('\n');
            else report.append("The chosen driver package is missing or broken: the system driver is used. ");
        }
        if (uncapped.isChecked()) e.append("BB_VBLANK_HZ=0\n");
        if (showFps.isChecked()) e.append("BB_SHOW_FPS=1\n");
        e.append(extraEnv.getText().toString().trim()).append('\n');
        writeText(new File(files, "env.txt"), e.toString());
        for (int i = 0; i < GAMES.length; i++) writeText(new File(files, "env-" + GAMES[i][0] + ".txt"), env[i]);
        new File(files, "loadexec.txt").delete();
        RadioButton limit = fpsLimit.findViewById(fpsLimit.getCheckedRadioButtonId());
        if (limit != null) iniPut("fps_limit", (String) limit.getTag());
        RadioButton pad = touchPad.findViewById(touchPad.getCheckedRadioButtonId());
        if (pad != null) iniPut("touch_pad", (String) pad.getTag());
        if (report.length() > 0) status(report.toString());
        Intent i = new Intent(this, GameActivity.class);
        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        startActivity(i);
        finish();
    }

    // ---- Vulkan drivers ----

    /** Installed driver packages live in the app's private storage: Android loads libraries from
     *  there, not from shared storage. One folder per package: meta.json and the library. */
    private File driversDir() {
        File d = new File(getFilesDir(), "drivers");
        d.mkdirs();
        return d;
    }

    /** "turnip" (the bundled Turnip), "system", or "pkg:<folder>" (an installed package). */
    private String driver() {
        String d = prefs.getString("driver", null);
        if (d == null) d = prefs.getBoolean("turnip", new File("/dev/kgsl-3d0").exists()) ? "turnip" : "system";
        return d;
    }

    private static JSONObject packageMeta(File dir) {
        try {
            String text = readText(new File(dir, "meta.json"));
            return text.isEmpty() ? null : new JSONObject(text);
        } catch (Exception e) {
            return null;
        }
    }

    /** The package's library (meta.json libraryName) when the file is there, else null. */
    private static String packageLibrary(File dir) {
        JSONObject meta = packageMeta(dir);
        String lib = meta != null ? meta.optString("libraryName", "") : "";
        return !lib.isEmpty() && new File(dir, lib).isFile() ? lib : null;
    }

    private void buildDriverList() {
        driverList.removeAllViews();
        drivers = new RadioGroup(this);
        boolean kgsl = new File("/dev/kgsl-3d0").exists();
        List<String[]> options = new ArrayList<>();
        options.add(new String[] {"turnip", "Bundled Turnip (Mesa 26.2.5 with the port's fixes; Adreno)"
                                            + (kgsl ? "" : " - no Adreno GPU (KGSL) found here")});
        options.add(new String[] {"system", "The system's Vulkan driver" + (kgsl ? " (untested with these games)" : "")});
        File[] installed = driversDir().listFiles(File::isDirectory);
        if (installed != null) {
            java.util.Arrays.sort(installed);
            for (File d : installed) {
                JSONObject meta = packageMeta(d);
                String label = meta != null ? meta.optString("name", d.getName()) : d.getName();
                String version = meta != null ? meta.optString("driverVersion", "") : "";
                String author = meta != null ? meta.optString("author", "") : "";
                String detail = version.isEmpty() ? author : author.isEmpty() ? version : version + ", " + author;
                if (!detail.isEmpty()) label += " (" + detail + ")";
                if (packageLibrary(d) == null) label += " - broken: no library";
                options.add(new String[] {"pkg:" + d.getName(), label});
            }
        }
        drivers.setOnCheckedChangeListener((g, id) -> {
            RadioButton b = g.findViewById(id);
            if (b != null) prefs.edit().putString("driver", (String) b.getTag()).apply();
        });
        String current = driver();
        for (String[] o : options) {
            RadioButton b = new RadioButton(this);
            b.setText(o[1]);
            b.setId(View.generateViewId());
            b.setTag(o[0]);
            drivers.addView(b);
            if (o[0].equals(current)) drivers.check(b.getId());
        }
        // The chosen package is gone: back to the default.
        if (drivers.getCheckedRadioButtonId() == -1) drivers.check(drivers.getChildAt(kgsl ? 0 : 1).getId());
        driverList.addView(drivers);
    }

    /** Unpacks a driver package (a zip with meta.json and the driver library) into
     *  driversDir()/<name>; the folder's name. */
    private String installDriver(InputStream in, String fallbackName) throws IOException {
        File tmp = new File(driversDir(), ".new-" + System.currentTimeMillis());
        if (!tmp.mkdirs()) throw new IOException("cannot create " + tmp);
        try {
            long total = 0;
            try (ZipInputStream zip = new ZipInputStream(new java.io.BufferedInputStream(in))) {
                for (ZipEntry e; (e = zip.getNextEntry()) != null;) {
                    if (e.isDirectory()) continue;
                    String name = new File(e.getName()).getName(); // a package is one flat folder
                    if (name.isEmpty() || name.startsWith(".")) continue;
                    try (OutputStream out = new FileOutputStream(new File(tmp, name))) {
                        byte[] buf = new byte[1 << 16];
                        for (int n; (n = zip.read(buf)) > 0;) {
                            out.write(buf, 0, n);
                            if ((total += n) > (256L << 20)) throw new IOException("the package is over 256 MB");
                        }
                    }
                }
            }
            JSONObject meta = packageMeta(tmp);
            if (meta == null) throw new IOException("no meta.json in the package (not a driver package?)");
            if (packageLibrary(tmp) == null) throw new IOException("the library named in meta.json is not in the package");
            String name = meta.optString("name", fallbackName).replaceAll("[^A-Za-z0-9._-]+", "_");
            if (name.isEmpty() || name.startsWith(".")) name = "driver";
            if (name.length() > 60) name = name.substring(0, 60);
            File dir = new File(driversDir(), name);
            if (dir.exists()) deleteTree(dir);
            if (!tmp.renameTo(dir)) throw new IOException("cannot rename to " + dir);
            return name;
        } finally {
            if (tmp.exists()) deleteTree(tmp);
        }
    }

    /** The zip assets of the sources' latest releases, picked from a list, then fetchDriver. */
    private void downloadDriver() {
        status("Looking up the driver releases ...");
        new Thread(() -> {
            List<String[]> found = new ArrayList<>(); // {label, url, file name}
            StringBuilder errors = new StringBuilder();
            for (String repo : DRIVER_SOURCES) {
                try {
                    JSONArray releases = new JSONArray(fetch("https://api.github.com/repos/" + repo + "/releases?per_page=6"));
                    for (int i = 0; i < releases.length(); i++) {
                        JSONObject r = releases.getJSONObject(i);
                        JSONArray assets = r.getJSONArray("assets");
                        for (int j = 0; j < assets.length(); j++) {
                            JSONObject a = assets.getJSONObject(j);
                            String name = a.getString("name");
                            long size = a.getLong("size");
                            if (!name.toLowerCase(Locale.ROOT).endsWith(".zip") || size > (256L << 20)) continue;
                            found.add(new String[] {repo.substring(0, repo.indexOf('/')) + ": " + name + "  ("
                                                    + r.optString("published_at", "").replaceAll("T.*", "") + ", "
                                                    + Math.max(1, size >> 20) + " MB)",
                                                    a.getString("browser_download_url"), name});
                        }
                    }
                } catch (Exception e) {
                    errors.append(repo).append(": ").append(e.getMessage()).append(". ");
                }
            }
            runOnUiThread(() -> {
                if (found.isEmpty()) {
                    status("No packages found. " + errors);
                    return;
                }
                status(errors.toString());
                CharSequence[] items = new CharSequence[found.size()];
                for (int i = 0; i < items.length; i++) items[i] = found.get(i)[0];
                new android.app.AlertDialog.Builder(this).setTitle("Download a driver package")
                    .setItems(items, (d, which) -> fetchDriver(found.get(which)[1], found.get(which)[2]))
                    .setNegativeButton("Cancel", null).show();
            });
        }).start();
    }

    private void fetchDriver(String url, String fileName) {
        status("Downloading " + fileName + " ...");
        new Thread(() -> {
            try (InputStream in = open(url).getInputStream()) {
                String name = installDriver(in, fileName.replaceAll("(?i)\\.zip$", ""));
                prefs.edit().putString("driver", "pkg:" + name).apply();
                runOnUiThread(this::buildDriverList);
                status("Driver package installed and selected: " + name);
            } catch (Exception e) {
                status("Downloading failed: " + e.getMessage());
            }
        }).start();
    }

    private void removeDriver() {
        String d = driver();
        if (!d.startsWith("pkg:")) {
            status("Select an installed package to remove it.");
            return;
        }
        File dir = new File(driversDir(), d.substring(4));
        new android.app.AlertDialog.Builder(this).setTitle("Remove the driver package")
            .setMessage("Remove " + dir.getName() + "?")
            .setPositiveButton("Remove", (dl, w) -> {
                deleteTree(dir);
                prefs.edit().putString("driver", new File("/dev/kgsl-3d0").exists() ? "turnip" : "system").apply();
                buildDriverList();
                status("Removed " + dir.getName());
            })
            .setNegativeButton("Cancel", null).show();
    }

    static HttpURLConnection open(String url) throws IOException {
        HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
        c.setConnectTimeout(15000);
        c.setReadTimeout(60000);
        c.setRequestProperty("User-Agent", "uncharted-android");
        c.setRequestProperty("Accept", "application/vnd.github+json, */*");
        if (c.getResponseCode() != 200) throw new IOException("HTTP " + c.getResponseCode() + " from " + new URL(url).getHost());
        return c;
    }

    static String fetch(String url) throws IOException {
        try (InputStream in = open(url).getInputStream()) {
            return new String(in.readAllBytes(), "UTF-8");
        }
    }

    // ---- updates (Updater.java) ----

    /** How often the app looks by itself; the last answer is shown meanwhile. */
    private static final long UPDATE_INTERVAL_MS = 60 * 60 * 1000L;

    private LinearLayout updateCard;
    private TextView updateTitle, updateInfo, updateCheckText;
    private List<Updater.Release> updates = new ArrayList<>(); // newer than this version, the newest first
    private volatile boolean updating;
    private boolean updateOffered; // the dialog by itself once per launcher
    private File pendingInstall; // an update waiting for "install unknown apps"

    /** The last check's answer, then a check when one is due, or the offer from that answer
     *  (with the automatic check on; not for adb automation or the installer's answers). */
    private void initUpdates(Intent intent) {
        if (Updater.repo(this) == null) return;
        List<Updater.Release> known = new ArrayList<>();
        try {
            JSONArray cached = new JSONArray(prefs.getString("update_releases", "[]"));
            for (int i = 0; i < cached.length(); i++) {
                Updater.Release r = Updater.Release.fromJson(cached.getJSONObject(i));
                if (Updater.compareVersions(r.version, Updater.installed(this)) > 0) known.add(r);
            }
        } catch (Exception e) {
            known.clear();
        }
        showUpdates(known);
        if (intent.hasExtra("play") || intent.hasExtra("prepare") || Updater.ACTION_INSTALL_STATUS.equals(intent.getAction())
                || !prefs.getBoolean("update_check", true)) return;
        if (System.currentTimeMillis() - prefs.getLong("update_checked", 0) > UPDATE_INTERVAL_MS) checkUpdates(false);
        else offerUpdate(false);
    }

    /** Asks the releases API in the background; `manual` (the button) reports every outcome. */
    private void checkUpdates(boolean manual) {
        String repo = Updater.repo(this);
        if (repo == null) return;
        if (manual) checkAnswer("Looking for a new version ...");
        new Thread(() -> {
            try {
                List<Updater.Release> found = Updater.newer(repo, Updater.installed(this));
                JSONArray cache = new JSONArray();
                for (Updater.Release r : found) cache.put(r.toJson());
                prefs.edit().putString("update_releases", cache.toString())
                    .putLong("update_checked", System.currentTimeMillis()).apply();
                runOnUiThread(() -> {
                    showUpdates(found);
                    if (manual) checkAnswer(found.isEmpty() ? "This is the newest version (" + Updater.installed(this) + ")." : "");
                    offerUpdate(manual);
                });
            } catch (Exception e) {
                if (manual) checkAnswer("Looking for a new version failed: " + e.getMessage());
            }
        }).start();
    }

    /** Check now answers under its button: the status line is far above it. */
    private void checkAnswer(String s) {
        runOnUiThread(() -> {
            updateCheckText.setText(s);
            updateCheckText.setVisibility(s.isEmpty() ? View.GONE : View.VISIBLE);
        });
    }

    /** The card at the top: the newest version there is, when newer than this one. */
    private void showUpdates(List<Updater.Release> found) {
        updates = found;
        if (found.isEmpty()) {
            updateCard.setVisibility(View.GONE);
            deleteTree(updateDir()); // the download of a version now installed
            return;
        }
        Updater.Release r = found.get(0);
        boolean skipped = r.version.equals(prefs.getString("update_skip", ""));
        updateTitle.setText("Version " + r.version + " is available");
        updateInfo.setText("This is " + Updater.installed(this) + ". The update: " + Math.max(1, r.size >> 20) + " MB"
                           + (r.published.isEmpty() ? "" : ", published " + r.published) + "."
                           + (skipped ? " Skipped: not offered when the app starts." : ""));
        updateCard.setVisibility(View.VISIBLE);
    }

    /** What is new, with Update and Later (and Skip when it comes by itself: once per launcher,
     *  not for a skipped version). */
    private void offerUpdate(boolean manual) {
        if (updates.isEmpty() || updating || isFinishing() || isDestroyed()) return;
        Updater.Release r = updates.get(0);
        if (!manual && (updateOffered || r.version.equals(prefs.getString("update_skip", "")))) return;
        updateOffered = true;
        StringBuilder s = new StringBuilder("This is version " + Updater.installed(this) + ". The update is "
                                            + Math.max(1, r.size >> 20) + " MB; Android installs it over this one and "
                                            + "keeps the settings and saves.");
        for (Updater.Release n : updates)
            s.append("\n\n").append(n.version).append(n.published.isEmpty() ? "" : " (" + n.published + ")")
             .append("\n\n").append(Updater.plainNotes(n.notes));
        ScrollView scroll = new ScrollView(this);
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(14);
        t.setLineSpacing(0, 1.1f);
        t.setPadding(dp(24), dp(8), dp(24), dp(8));
        t.setTextIsSelectable(true);
        scroll.addView(t);
        android.app.AlertDialog.Builder b = new android.app.AlertDialog.Builder(this)
            .setTitle("uncharted-android " + r.version + " is available").setView(scroll)
            .setPositiveButton("Update", (d, w) -> startUpdate())
            .setNegativeButton("Later", null);
        if (!manual) b.setNeutralButton("Skip this version", (d, w) -> {
            prefs.edit().putString("update_skip", r.version).apply();
            showUpdates(updates);
        });
        b.show();
    }

    /** The newest release's APK downloaded (a progress dialog) and checked, then installUpdate. */
    private void startUpdate() {
        if (updates.isEmpty() || updating) return;
        Updater.Release r = updates.get(0);
        updating = true;
        AtomicBoolean cancel = new AtomicBoolean();
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(24), dp(8), dp(24), dp(4));
        TextView label = new TextView(this);
        label.setText("Downloading " + r.apkName + " ...");
        box.addView(label);
        ProgressBar bar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        bar.setMax(100);
        box.addView(bar, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        android.app.AlertDialog dialog = new android.app.AlertDialog.Builder(this).setTitle("Updating to " + r.version)
            .setView(box).setCancelable(false).setNegativeButton("Cancel", (d, w) -> cancel.set(true)).show();
        long[] shown = {-1};
        new Thread(() -> {
            try {
                File apk = Updater.download(r, updateDir(), (done, total) -> {
                    long percent = total > 0 ? done * 100 / total : 0;
                    if (percent == shown[0]) return;
                    shown[0] = percent;
                    runOnUiThread(() -> {
                        bar.setProgress((int) percent);
                        label.setText((done >> 20) + " of " + (total >> 20) + " MB");
                    });
                }, cancel);
                runOnUiThread(() -> label.setText("Checking the download ..."));
                String problem = Updater.checkApk(this, apk);
                if (problem != null) {
                    apk.delete();
                    throw new IOException(problem);
                }
                runOnUiThread(() -> {
                    dialog.dismiss();
                    updating = false;
                    installUpdate(apk);
                });
            } catch (Exception e) {
                runOnUiThread(() -> {
                    dialog.dismiss();
                    updating = false;
                    updateStatus(cancel.get() ? "Update cancelled." : "Updating failed: " + e.getMessage());
                });
            }
        }).start();
    }

    /** Android's installer, once this app may install apps (Android asks the user once). */
    private void installUpdate(File apk) {
        if (!getPackageManager().canRequestPackageInstalls()) {
            pendingInstall = apk;
            new android.app.AlertDialog.Builder(this).setTitle("Allow the update")
                .setMessage("Android installs the update once this app may install apps: turn on \"Allow from this "
                            + "source\" on the next screen, then come back here.")
                .setPositiveButton("Open the setting", (d, w) -> startActivity(
                    new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:" + getPackageName()))))
                .setNegativeButton("Cancel", (d, w) -> pendingInstall = null).show();
            return;
        }
        pendingInstall = null;
        try {
            Updater.install(this, apk);
            updateStatus("Installing " + apk.getName() + " ... The app closes when Android has installed it: open it "
                         + "again from its icon.");
        } catch (Exception e) {
            updateStatus("Installing failed: " + e.getMessage());
        }
    }

    /** The installer's answers (Updater.ACTION_INSTALL_STATUS): its confirmation screen first. */
    private void onInstallStatus(Intent intent) {
        int code = intent.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE);
        if (code == PackageInstaller.STATUS_PENDING_USER_ACTION) {
            Intent confirm = intent.getParcelableExtra(Intent.EXTRA_INTENT, Intent.class);
            if (confirm != null) startActivity(confirm);
        } else if (code == PackageInstaller.STATUS_SUCCESS) {
            updateStatus("Updated to " + Updater.installed(this) + ".");
        } else if (code == PackageInstaller.STATUS_FAILURE_ABORTED) {
            updateStatus("Update cancelled.");
        } else {
            String message = intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE);
            updateStatus("Installing the update failed" + (message != null ? ": " + message : "."));
        }
    }

    /** An update's outcome: on the status line and as a toast (the page may be scrolled away from it). */
    private void updateStatus(String s) {
        status(s);
        runOnUiThread(() -> Toast.makeText(this, s, Toast.LENGTH_LONG).show());
    }

    private File updateDir() {
        return new File(getCacheDir(), "update");
    }

    // ---- saves ----

    /** Where save backups go: shared storage survives the app (all files access). */
    private static File backups() {
        return new File(Environment.getExternalStorageDirectory(), "Documents/uncharted-android");
    }

    /** files/user (the PS4 user profile: saves, settings) copied to backups()/saves-<time>; the
     *  backup's path, or null. */
    private String backupSaves() {
        File user = new File(files, "user");
        if (!user.isDirectory()) return null;
        String stamp = new java.text.SimpleDateFormat("yyyyMMdd-HHmmss", Locale.ROOT).format(new java.util.Date());
        File to = new File(backups(), "saves-" + stamp);
        try {
            copyTree(user, to);
            return to.getAbsolutePath();
        } catch (IOException e) {
            return null;
        }
    }

    /** files/bb-probe.log copied to backups() (other apps cannot read the app's folder on recent
     *  Android); the copy's path, or null. */
    private String exportLog() {
        File log = new File(files, "bb-probe.log");
        if (!log.isFile()) return null;
        String stamp = new java.text.SimpleDateFormat("yyyyMMdd-HHmmss", Locale.ROOT).format(new java.util.Date());
        File to = new File(backups(), "bb-probe-" + stamp + ".log");
        try {
            backups().mkdirs();
            copyTree(log, to);
            return to.getAbsolutePath();
        } catch (IOException e) {
            return null;
        }
    }

    /** The newest backup back into files/user, after backing up the current saves. */
    private void restoreSaves() {
        File[] list = backups().listFiles(f -> f.isDirectory() && f.getName().startsWith("saves-"));
        if (list == null || list.length == 0) {
            status("No backups in " + backups());
            return;
        }
        java.util.Arrays.sort(list);
        File newest = list[list.length - 1];
        new android.app.AlertDialog.Builder(this).setTitle("Restore saves")
            .setMessage("Replace the app's saves with " + newest.getName() + "? The current ones are backed up first.")
            .setPositiveButton("Restore", (d, w) -> {
                File user = new File(files, "user");
                if (user.isDirectory() && backupSaves() == null) {
                    status("Backing up the current saves failed: nothing was changed.");
                    return;
                }
                try {
                    deleteTree(user);
                    copyTree(newest, user);
                    status("Saves restored from " + newest.getName());
                } catch (IOException e) {
                    status("Restoring failed: " + e.getMessage());
                }
            })
            .setNegativeButton("Cancel", null).show();
    }

    private static void copyTree(File from, File to) throws IOException {
        if (from.isDirectory()) {
            if (!to.isDirectory() && !to.mkdirs()) throw new IOException("cannot create " + to);
            String[] names = from.list();
            if (names != null)
                for (String n : names) copyTree(new File(from, n), new File(to, n));
        } else {
            try (InputStream in = new java.io.FileInputStream(from); OutputStream out = new FileOutputStream(to)) {
                byte[] buf = new byte[1 << 16];
                for (int n; (n = in.read(buf)) > 0;) out.write(buf, 0, n);
            }
        }
    }

    private static void deleteTree(File f) {
        File[] children = f.listFiles();
        if (children != null)
            for (File c : children) deleteTree(c);
        f.delete();
    }

    /** The licenses of everything in the app (assets/licenses.txt, game_app/licenses.sh). */
    private void showLicenses() {
        String text = readAsset("licenses.txt");
        ScrollView scroll = new ScrollView(this);
        TextView t = new TextView(this);
        t.setText(text.isEmpty() ? "This build has no license file (build.sh PUBLIC=1 with licenses.sh's output)." : text);
        t.setTextSize(12);
        t.setTypeface(Typeface.MONOSPACE);
        t.setPadding(dp(16), dp(16), dp(16), dp(16));
        t.setTextIsSelectable(true);
        scroll.addView(t);
        new android.app.AlertDialog.Builder(this).setTitle("Licenses").setView(scroll)
            .setPositiveButton("Close", null).show();
    }

    // ---- files ----

    private static String readText(File f) {
        try (InputStream in = new java.io.FileInputStream(f)) {
            return new String(in.readAllBytes(), "UTF-8");
        } catch (IOException e) {
            return "";
        }
    }

    private static void writeText(File f, String s) {
        try (FileWriter w = new FileWriter(f)) {
            w.write(s);
        } catch (IOException e) {
            throw new RuntimeException(e);
        }
    }

    private byte[] readAssetBytes(String name) {
        try (InputStream in = getAssets().open(name)) {
            return in.readAllBytes();
        } catch (IOException e) {
            return null;
        }
    }

    private String readAsset(String name) {
        byte[] b = readAssetBytes(name);
        try {
            return b != null ? new String(b, "UTF-8") : "";
        } catch (IOException e) {
            return "";
        }
    }

    private boolean copyAsset(String name, File to) {
        byte[] b = readAssetBytes(name);
        if (b == null) return false;
        try (OutputStream out = new FileOutputStream(to)) {
            out.write(b);
            return true;
        } catch (IOException e) {
            return false;
        }
    }
}
