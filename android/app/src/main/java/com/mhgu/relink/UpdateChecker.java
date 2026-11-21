package com.mhgu.relink;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.DownloadManager;
import android.content.Context;
import android.net.Uri;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.widget.Toast;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.Locale;

/**
 * Settings -> Check for updates: asks the project's GitHub releases for the newest one and, when it is newer than this
 * app, offers to download its APK. Never downloads by itself: the dialog is the player's choice, and "Not now" leaves
 * everything as it is. The download goes through Android's DownloadManager (its notification opens the file, which the
 * system then installs when the player allows it).
 */
public final class UpdateChecker {
    private static final String TAG = "MHGU";
    private static final String API = "https://api.github.com/repos/MEKCCK/MHGU-Relink/releases/latest";
    private static final Handler UI = new Handler(Looper.getMainLooper());
    private static volatile boolean running;

    private UpdateChecker() {}

    public static void check(final Activity act) {
        if (running) return;
        running = true;
        final boolean zh = Locale.getDefault().getLanguage().startsWith("zh");
        new Thread(() -> {
            String version = null, apk = null, notes = "", error = null;
            try {
                HttpURLConnection c = (HttpURLConnection) new URL(API).openConnection();
                c.setConnectTimeout(15000);
                c.setReadTimeout(20000);
                c.setRequestProperty("Accept", "application/vnd.github+json");
                c.setRequestProperty("User-Agent", "MHGU-Relink");
                StringBuilder sb = new StringBuilder();
                try (BufferedReader r = new BufferedReader(new InputStreamReader(c.getInputStream(), "UTF-8"))) {
                    String line;
                    while ((line = r.readLine()) != null) sb.append(line).append('\n');
                }
                JSONObject o = new JSONObject(sb.toString());
                version = o.optString("tag_name", "").replaceFirst("^[vV]", "");
                notes = o.optString("body", "");
                JSONArray assets = o.optJSONArray("assets");
                if (assets != null) {
                    for (int i = 0; i < assets.length(); i++) {
                        JSONObject a = assets.getJSONObject(i);
                        if (a.optString("name", "").endsWith(".apk")) {
                            apk = a.optString("browser_download_url", null);
                            break;
                        }
                    }
                }
            } catch (Exception e) {
                error = e.toString();
            }
            final String fv = version, fa = apk, fn = notes, fe = error;
            UI.post(() -> {
                running = false;
                if (fe != null || fv == null || fv.isEmpty()) {
                    toast(act, (zh ? "检查更新失败：" : "Update check failed: ") + (fe != null ? fe : "no release"));
                    return;
                }
                String current = versionName(act);
                if (!newer(fv, current)) {
                    toast(act, (zh ? "已是最新版本 " : "Up to date: ") + current);
                    return;
                }
                offer(act, zh, fv, current, fa, fn);
            });
        }, "update-check").start();
    }

    private static void offer(Activity act, boolean zh, String ver, String current, String apk, String notes) {
        String text = (zh ? "发现新版本 " : "A new version is out: ") + ver + (zh ? "（当前 " : " (running ") + current + ")\n\n" + clip(notes);
        AlertDialog.Builder b = new AlertDialog.Builder(act)
                .setTitle(zh ? "发现新版本" : "New version")
                .setMessage(text)
                .setNegativeButton(zh ? "以后再说" : "Not now", null);
        if (apk != null) b.setPositiveButton(zh ? "下载 APK" : "Download APK", (d, w) -> download(act, zh, ver, apk));
        else b.setPositiveButton(zh ? "打开下载页" : "Open the releases page", (d, w) ->
                act.startActivity(new android.content.Intent(android.content.Intent.ACTION_VIEW,
                        Uri.parse("https://github.com/MEKCCK/MHGU-Relink/releases/latest"))));
        b.show();
    }

    private static void download(Activity act, boolean zh, String ver, String apk) {
        try {
            DownloadManager dm = (DownloadManager) act.getSystemService(Context.DOWNLOAD_SERVICE);
            DownloadManager.Request req = new DownloadManager.Request(Uri.parse(apk))
                    .setTitle("MHGU Relink " + ver)
                    .setDescription(zh ? "下载完成后在通知栏点开安装" : "Open the finished download in the notifications to install")
                    .setNotificationVisibility(DownloadManager.Request.VISIBILITY_VISIBLE_NOTIFY_COMPLETED)
                    .setDestinationInExternalFilesDir(act, Environment.DIRECTORY_DOWNLOADS, "mhgu-relink-" + ver + ".apk");
            dm.enqueue(req);
            toast(act, zh ? "开始下载，完成后在通知栏点开安装" : "Downloading: open it from the notifications when it is done");
        } catch (Exception e) {
            Log.w(TAG, "download: " + e);
            Toast.makeText(act, e.toString(), Toast.LENGTH_LONG).show();
        }
    }

    private static boolean newer(String a, String b) {
        String[] x = a.split("[^0-9]+"), y = b.split("[^0-9]+");
        for (int i = 0; i < Math.max(x.length, y.length); i++) {
            int p = i < x.length && !x[i].isEmpty() ? Integer.parseInt(x[i]) : 0;
            int q = i < y.length && !y[i].isEmpty() ? Integer.parseInt(y[i]) : 0;
            if (p != q) return p > q;
        }
        return false;
    }

    private static String versionName(Context c) {
        try {
            return c.getPackageManager().getPackageInfo(c.getPackageName(), 0).versionName;
        } catch (Exception e) {
            return "0";
        }
    }

    private static String clip(String notes) {
        if (notes == null) return "";
        String s = notes.trim();
        return s.length() > 700 ? s.substring(0, 700) + "…" : s;
    }

    private static void toast(Activity act, String text) {
        Toast.makeText(act, text, Toast.LENGTH_LONG).show();
    }
}
