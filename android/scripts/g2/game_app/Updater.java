package dev.unchartedg2.game;

import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageInstaller;
import android.content.pm.PackageManager;
import android.content.pm.Signature;
import android.content.pm.SigningInfo;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

/** Updates from the project's GitHub releases (the launcher's "Updates"). The releases API gives
 *  the versions newer than this one that have an APK (pre-releases count: every release so far
 *  is one); the newest APK is downloaded into the app's cache and checked (its size, its SHA-256
 *  against the API's digest of the asset, then that it is this app signed with this app's key
 *  and not older), then handed to Android's package installer, which asks the user to confirm
 *  and keeps the app's data: settings, saves, the prepared game files. The manifest's meta-data
 *  name the repository (update_repo) and the release key's certificate (update_signer, SHA-256):
 *  a build without them (the G2 development app) or signed with another key (built from source)
 *  never looks for updates, since Android would not install the releases over it. */
final class Updater {
    /** The installer's answers come back to the launcher as intents with this action. */
    static final String ACTION_INSTALL_STATUS = "dev.unchartedg2.game.INSTALL_STATUS";

    /** A release with an APK: the version is the tag without its "v". */
    static final class Release {
        String version, tag, notes, published, apkName, apkUrl, sha256;
        long size;

        JSONObject toJson() throws JSONException {
            return new JSONObject().put("version", version).put("tag", tag).put("notes", notes)
                .put("published", published).put("apkName", apkName).put("apkUrl", apkUrl)
                .put("sha256", sha256 != null ? sha256 : "").put("size", size);
        }

        static Release fromJson(JSONObject o) throws JSONException {
            Release r = new Release();
            r.version = o.getString("version");
            r.tag = o.getString("tag");
            r.notes = o.optString("notes", "");
            r.published = o.optString("published", "");
            r.apkName = o.getString("apkName");
            r.apkUrl = o.getString("apkUrl");
            r.sha256 = o.optString("sha256", "");
            if (r.sha256.isEmpty()) r.sha256 = null;
            r.size = o.optLong("size", -1);
            return r;
        }
    }

    interface Progress {
        void update(long done, long total);
    }

    private Updater() {}

    /** The repository ("owner/name") whose releases update this app, or null: no update_repo in
     *  the manifest, or the app is not signed with the release key (update_signer). */
    static String repo(Context c) {
        try {
            PackageManager pm = c.getPackageManager();
            ApplicationInfo info = pm.getApplicationInfo(c.getPackageName(), PackageManager.GET_META_DATA);
            String repo = info.metaData != null ? info.metaData.getString("update_repo") : null;
            String signer = info.metaData != null ? info.metaData.getString("update_signer") : null;
            if (repo == null || !repo.matches("[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+") || signer == null) return null;
            PackageInfo self = pm.getPackageInfo(c.getPackageName(), PackageManager.GET_SIGNING_CERTIFICATES);
            for (String s : certificates(self.signingInfo, false))
                if (s.equalsIgnoreCase(signer)) return repo;
            return null;
        } catch (PackageManager.NameNotFoundException e) {
            return null;
        }
    }

    /** The installed version (the APK's versionName: android/VERSION, a suffix for test builds). */
    static String installed(Context c) {
        try {
            return c.getPackageManager().getPackageInfo(c.getPackageName(), 0).versionName;
        } catch (PackageManager.NameNotFoundException e) {
            return "0";
        }
    }

    /** Negative when a is older than b, positive when newer: the numbers part by part ("0.2.10"
     *  after "0.2.9"), then a version with a suffix ("0.2.7-test") before the same one without. */
    static int compareVersions(String a, String b) {
        String[] x = split(a), y = split(b);
        String[] xn = x[0].split("\\."), yn = y[0].split("\\.");
        for (int i = 0; i < Math.max(xn.length, yn.length); i++) {
            long p = i < xn.length ? number(xn[i]) : 0, q = i < yn.length ? number(yn[i]) : 0;
            if (p != q) return p < q ? -1 : 1;
        }
        if (x[1].isEmpty() != y[1].isEmpty()) return x[1].isEmpty() ? 1 : -1;
        return x[1].compareTo(y[1]);
    }

    /** {numbers, suffix} of "v1.2.3-suffix". */
    private static String[] split(String v) {
        String s = v.trim().replaceFirst("^[vV]", "");
        int i = 0;
        while (i < s.length() && (Character.isDigit(s.charAt(i)) || s.charAt(i) == '.')) i++;
        return new String[] {s.substring(0, i), s.substring(i)};
    }

    private static long number(String s) {
        try {
            return s.isEmpty() ? 0 : Long.parseLong(s);
        } catch (NumberFormatException e) {
            return 0;
        }
    }

    /** The releases newer than `installed` with an APK, the newest first (at most five).
     *  Network: not on the UI thread. */
    static List<Release> newer(String repo, String installed) throws IOException, JSONException {
        JSONArray list = new JSONArray(Launcher.fetch("https://api.github.com/repos/" + repo + "/releases?per_page=20"));
        String downloads = "https://github.com/" + repo + "/releases/download/";
        List<Release> found = new ArrayList<>();
        for (int i = 0; i < list.length(); i++) {
            JSONObject r = list.getJSONObject(i);
            if (r.optBoolean("draft", false)) continue;
            String tag = r.optString("tag_name", "");
            String version = tag.replaceFirst("^[vV]", "");
            if (!version.matches("\\d+(\\.\\d+)*([-+][0-9A-Za-z.-]+)?") || compareVersions(version, installed) <= 0) continue;
            JSONArray assets = r.optJSONArray("assets");
            JSONObject apk = null;
            for (int j = 0; assets != null && j < assets.length(); j++) {
                JSONObject a = assets.getJSONObject(j);
                String name = a.optString("name", "");
                if (!name.toLowerCase(Locale.ROOT).endsWith(".apk")) continue;
                if (!a.optString("browser_download_url", "").startsWith(downloads)) continue;
                if (apk == null || name.contains(version)) apk = a; // uncharted-android-<version>.apk first
            }
            if (apk == null) continue;
            Release rel = new Release();
            rel.version = version;
            rel.tag = tag;
            rel.notes = r.optString("body", "");
            rel.published = r.optString("published_at", "").replaceAll("T.*", "");
            rel.apkName = apk.getString("name");
            rel.apkUrl = apk.getString("browser_download_url");
            rel.size = apk.optLong("size", -1);
            String digest = apk.optString("digest", "");
            rel.sha256 = digest.matches("sha256:[0-9a-fA-F]{64}") ? digest.substring(7).toLowerCase(Locale.ROOT)
                                                                 : hashInNotes(rel.notes, rel.apkName);
            found.add(rel);
        }
        found.sort((p, q) -> compareVersions(q.version, p.version));
        return found.size() > 5 ? new ArrayList<>(found.subList(0, 5)) : found;
    }

    /** The SHA-256 the notes give for the APK ("`name` SHA-256: `hex`"), or null. */
    private static String hashInNotes(String notes, String apkName) {
        Matcher m = Pattern.compile(Pattern.quote(apkName) + "[^\\n]*?\\b([0-9a-fA-F]{64})\\b").matcher(notes);
        return m.find() ? m.group(1).toLowerCase(Locale.ROOT) : null;
    }

    /** The release's APK in `dir`, downloaded unless a finished download is there; its size and
     *  SHA-256 checked. Network: not on the UI thread. */
    static File download(Release r, File dir, Progress progress, AtomicBoolean cancel) throws IOException {
        if (!dir.isDirectory() && !dir.mkdirs()) throw new IOException("cannot create " + dir);
        File done = new File(dir, r.apkName.replaceAll("[^A-Za-z0-9._-]+", "_"));
        if (done.isFile() && (r.size < 0 || done.length() == r.size) && (r.sha256 == null || r.sha256.equals(sha256(done))))
            return done;
        File part = new File(dir, done.getName() + ".part");
        File[] old = dir.listFiles(); // an older release's download
        if (old != null)
            for (File f : old)
                if (!f.equals(done) && !f.equals(part)) f.delete();
        MessageDigest md = digest();
        HttpURLConnection c = Launcher.open(r.apkUrl);
        long total = c.getContentLengthLong() > 0 ? c.getContentLengthLong() : r.size;
        try (InputStream in = c.getInputStream(); OutputStream out = new FileOutputStream(part)) {
            byte[] buf = new byte[1 << 16];
            long n = 0;
            for (int k; (k = in.read(buf)) > 0;) {
                if (cancel.get()) throw new IOException("cancelled");
                out.write(buf, 0, k);
                md.update(buf, 0, k);
                progress.update(n += k, total);
            }
        } catch (IOException e) {
            part.delete();
            throw e;
        } finally {
            c.disconnect();
        }
        if (r.size >= 0 && part.length() != r.size) {
            part.delete();
            throw new IOException("the download has " + part.length() + " bytes, the release's APK " + r.size);
        }
        if (r.sha256 != null && !r.sha256.equals(hex(md.digest()))) {
            part.delete();
            throw new IOException("the download's SHA-256 is not the release's");
        }
        done.delete();
        if (!part.renameTo(done)) throw new IOException("cannot rename " + part);
        return done;
    }

    /** null when the APK is this app, signed with this app's key and not older; else why not. */
    static String checkApk(Context c, File apk) {
        PackageManager pm = c.getPackageManager();
        PackageInfo theirs = pm.getPackageArchiveInfo(apk.getPath(), PackageManager.GET_SIGNING_CERTIFICATES);
        if (theirs == null) return "the download is not an Android package";
        if (!c.getPackageName().equals(theirs.packageName)) return "the download is another app (" + theirs.packageName + ")";
        try {
            PackageInfo mine = pm.getPackageInfo(c.getPackageName(), PackageManager.GET_SIGNING_CERTIFICATES);
            // This app's key, or a key that took over from it (the APK's signing history).
            Set<String> keys = certificates(theirs.signingInfo, true);
            if (keys.isEmpty() || !keys.containsAll(certificates(mine.signingInfo, false)))
                return "the download is signed with another key than this app (Android installs updates only with the app's own key)";
            if (theirs.getLongVersionCode() < mine.getLongVersionCode()) return "the download is older than this version";
        } catch (PackageManager.NameNotFoundException e) {
            return e.toString();
        }
        return null;
    }

    /** SHA-256 of the signing certificates: the current ones, with history also the past ones. */
    private static Set<String> certificates(SigningInfo info, boolean history) {
        Set<String> out = new HashSet<>();
        if (info == null) return out;
        List<Signature> list = new ArrayList<>(Arrays.asList(info.getApkContentsSigners()));
        if (history && !info.hasMultipleSigners() && info.getSigningCertificateHistory() != null)
            list.addAll(Arrays.asList(info.getSigningCertificateHistory()));
        MessageDigest md = digest();
        for (Signature s : list) out.add(hex(md.digest(s.toByteArray())));
        return out;
    }

    /** Hands the APK to Android's package installer. Its answers come to the launcher as
     *  ACTION_INSTALL_STATUS: first "pending user action" with the confirmation screen to show. */
    static void install(Context c, File apk) throws IOException {
        PackageInstaller installer = c.getPackageManager().getPackageInstaller();
        PackageInstaller.SessionParams params = new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
        params.setAppPackageName(c.getPackageName());
        params.setSize(apk.length());
        params.setInstallReason(PackageManager.INSTALL_REASON_USER);
        // Once the app has updated itself it is its own installer, and Android may then install
        // its later updates without asking again: the user has chosen to update in the launcher.
        params.setRequireUserAction(PackageInstaller.SessionParams.USER_ACTION_NOT_REQUIRED);
        int id = installer.createSession(params);
        try (PackageInstaller.Session session = installer.openSession(id)) {
            try (InputStream in = new FileInputStream(apk); OutputStream out = session.openWrite("base.apk", 0, apk.length())) {
                byte[] buf = new byte[1 << 16];
                for (int n; (n = in.read(buf)) > 0;) out.write(buf, 0, n);
                session.fsync(out);
            }
            Intent status = new Intent(c, Launcher.class).setAction(ACTION_INSTALL_STATUS);
            PendingIntent answer = PendingIntent.getActivity(c, id, status,
                                                             PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_MUTABLE);
            session.commit(answer.getIntentSender());
        } catch (IOException | RuntimeException e) {
            installer.abandonSession(id);
            throw e;
        }
    }

    /** Release notes (GitHub Markdown) as plain text for a dialog. */
    static String plainNotes(String md) {
        String s = md.replace("\r", "");
        s = s.replaceAll("\\[([^\\]]+)\\]\\((https?://[^)\\s]+)\\)", "$1"); // links: their text
        s = s.replaceAll("\\*\\*([^*]+)\\*\\*", "$1");
        s = s.replaceAll("`([^`]+)`", "$1");
        s = s.replaceAll("(?m)^#+\\s*", "");
        return s.trim();
    }

    static String sha256(File f) throws IOException {
        MessageDigest md = digest();
        try (InputStream in = new FileInputStream(f)) {
            byte[] buf = new byte[1 << 16];
            for (int n; (n = in.read(buf)) > 0;) md.update(buf, 0, n);
        }
        return hex(md.digest());
    }

    private static MessageDigest digest() {
        try {
            return MessageDigest.getInstance("SHA-256");
        } catch (NoSuchAlgorithmException e) {
            throw new IllegalStateException(e);
        }
    }

    private static String hex(byte[] bytes) {
        StringBuilder b = new StringBuilder();
        for (byte x : bytes) b.append(String.format(Locale.ROOT, "%02x", x));
        return b.toString();
    }
}
