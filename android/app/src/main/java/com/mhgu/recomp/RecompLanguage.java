package com.mhgu.recomp;

import android.app.Activity;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.InputStreamReader;
import java.util.Locale;

final class RecompLanguage {
    private RecompLanguage() {}

    static int from(Activity activity) {
        int selected = activity.getIntent().getIntExtra(GameActivity.EXTRA_LANGUAGE, -1);
        if (selected >= 0 && selected <= 2) return selected;
        // The launcher has no language extra; use the same saved setting as the native menu.
        File home = activity.getExternalFilesDir(null);
        if (home == null) home = activity.getFilesDir();
        try (BufferedReader in = new BufferedReader(new InputStreamReader(
                new FileInputStream(new File(home, "mhgurecomp.ini")), "UTF-8"))) {
            String line;
            int saved = -1;
            while ((line = in.readLine()) != null) {
                line = line.trim();
                if (line.startsWith(";") || line.startsWith("#") || line.startsWith("[")) continue;
                int eq = line.indexOf('=');
                if (eq < 0 || !"Language".equalsIgnoreCase(line.substring(0, eq).trim())) continue;
                String value = line.substring(eq + 1).split("[;#]", 2)[0]
                        .replaceAll("[\\s_-]", "").toLowerCase(Locale.ROOT);
                if ("english".equals(value) || "en".equals(value)) saved = 0;
                else if ("spanish".equals(value) || "es".equals(value)) saved = 1;
                else if ("simplifiedchinese".equals(value) || "chinese".equals(value)
                        || "zhcn".equals(value) || "zhhans".equals(value) || "zh".equals(value)) saved = 2;
            }
            if (saved >= 0) return saved;
        } catch (Exception ignored) {
            // First launch: no saved settings yet.
        }
        String system = Locale.getDefault().getLanguage();
        if ("es".equals(system)) return 1;
        if ("zh".equals(system)) return 2;
        return 0;
    }
}
