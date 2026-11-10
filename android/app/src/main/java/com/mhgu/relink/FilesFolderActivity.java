package com.mhgu.relink;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.DialogInterface;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.os.Build;
import android.os.Bundle;
import android.os.Process;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.text.DateFormat;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;

/**
 * La carpeta de archivos de la app (Ajustes -> Opciones del menú): elegir una carpeta para los ajustes, la partida, los
 * mods, los trucos, las capturas y log.txt (Android/data no se puede abrir con un gestor de archivos), o volver a la
 * carpeta propia de la app. Copia los archivos con un diálogo de progreso y Stop; los archivos que ya estén en la
 * carpeta de destino se conservan, y los más antiguos se reemplazan solo si el jugador lo pide (los reemplazados quedan
 * como .bak). La ruta elegida queda en files-folder.txt (FilesFolder; runtime/platform/android.c lee lo mismo) y la app
 * se reinicia. Con EXTRA_RETURN solo devuelve el resultado (el primer arranque, antes de que el runtime lea nada).
 */
public class FilesFolderActivity extends Activity {
    static final String EXTRA_MODE = "mode", EXTRA_RETURN = "return", EXTRA_PATH = "path";
    static final int MODE_CHOOSE = 0, MODE_BACK = 1;
    private static final int PICK = 1;
    private static final int WINE = Color.rgb(38, 18, 14), PANEL = Color.rgb(26, 12, 9), CREAM = Color.rgb(240, 228, 200);
    private static final int GOLD = Color.rgb(226, 182, 92), BRONZE = Color.rgb(62, 38, 22), EDGE = Color.rgb(196, 150, 70);
    private static final int ALERT = Color.rgb(255, 150, 120);

    // Los nombres de la app en lo alto de su carpeta: una carpeta elegida puede tener cualquier otra cosa del jugador
    // (Documentos, por ejemplo). No los archivos del juego (data/: se usan donde están) ni la caché de shaders (se
    // rehace) ni el registro de esta sesión.
    private static final Set<String> COPIED = new LinkedHashSet<>(Arrays.asList(
            "mhgurecomp.ini", "MHGURecomp", "mods", "cheats", "shots", "env.txt", "crash-log.txt", "pipelines.list",
            "chat_box.txt", "forge_imgui.ini", "cjk.ttf"));

    private int language, mode;
    private boolean returnOnly;
    private TextView status;
    private Button primary;
    private volatile boolean stop;
    private AlertDialog progress;

    private String tr(String en, String es, String zh) {
        return language == 2 ? zh : language == 1 ? es : en;
    }

    private int dp(int v) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics());
    }

    private TextView text(String s, int size, int color, boolean bold) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, size);
        t.setTextColor(color);
        if (bold) t.setTypeface(Typeface.DEFAULT_BOLD);
        return t;
    }

    private GradientDrawable plate(int fill) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(fill);
        d.setStroke(dp(2), EDGE);
        d.setCornerRadius(dp(6));
        return d;
    }

    private Button button(String s, boolean strong) {
        Button b = new Button(this);
        b.setText(s);
        b.setAllCaps(false);
        b.setTextColor(strong ? GOLD : CREAM);
        b.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
        if (strong) b.setTypeface(Typeface.DEFAULT_BOLD);
        b.setBackground(plate(BRONZE));
        b.setPadding(dp(14), 0, dp(14), 0);
        return b;
    }

    private String title() {
        return tr("Files folder", "Carpeta de archivos", "文件文件夹");
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        language = RecompLanguage.from(this);
        mode = getIntent().getIntExtra(EXTRA_MODE, MODE_CHOOSE);
        returnOnly = getIntent().getBooleanExtra(EXTRA_RETURN, false);

        if (mode == MODE_BACK) {
            String problem = FilesFolder.problem(this);
            if (problem != null) {
                forgetDialog(problem);
                return;
            }
        }
        build();
    }

    private void build() {
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(24), dp(14), dp(24), dp(16));
        box.setBackground(plate(WINE));
        TextView heading = text(title(), 20, GOLD, true);
        heading.setGravity(Gravity.CENTER);
        box.addView(heading);

        String from = FilesFolder.get(this).getAbsolutePath();
        String body = mode == MODE_BACK
                ? tr("The app's files go back into its own folder (in Android/data): the settings, the save, mods, cheats "
                     + "and screenshots in " + from + " are copied there (it asks before replacing older files already "
                     + "there), and the app closes: start it again to play. The files in " + from + " are left where they are.",
                     "Los archivos de la app vuelven a su carpeta propia (en Android/data): los ajustes, la partida, los "
                     + "mods, los trucos y las capturas de " + from + " se copian allí (pregunta antes de reemplazar los "
                     + "más antiguos que ya haya) y la app se cierra: vuelve a abrirla para jugar. Los archivos de " + from
                     + " se quedan donde están.",
                     "应用文件将回到自己的文件夹（Android/data）：" + from + " 中的设置、存档、模组、金手指和截图会被复制"
                     + "过去（替换其中较旧的文件前会询问），然后应用关闭：请重新打开以游玩。" + from + " 中的文件保持原样。")
                : tr("Pick a folder for the app's files: the settings (mhgurecomp.ini), the save, mods, cheats, "
                     + "screenshots and log.txt, so you can open them with the file managers on this device (the app's own "
                     + "folder, in Android/data, can't be opened by them). For example a new MHGU folder in the device's "
                     + "storage; if you used one before (and reinstalled the app), pick it again. The app copies its files "
                     + "there (it asks before replacing older files already there) and uses that folder from now on"
                     + (returnOnly ? "." : ", then closes: start it again to play.")
                     + " The game files are not copied: they are used where they are. The app reads and writes the folder "
                     + "itself, so Android asks you to allow \"All files access\" for it.",
                     "Elige una carpeta para los archivos de la app: los ajustes (mhgurecomp.ini), la partida, los mods, "
                     + "los trucos, las capturas y log.txt, para poder abrirlos con los gestores de archivos del "
                     + "dispositivo (la carpeta propia de la app, en Android/data, no se puede abrir con ellos). Por "
                     + "ejemplo una carpeta MHGU nueva en el almacenamiento; si usaste una antes (y reinstalaste la app), "
                     + "elígela otra vez. La app copia ahí sus archivos (pregunta antes de reemplazar los más antiguos que "
                     + "ya haya) y usa esa carpeta desde ahora" + (returnOnly ? "." : ", y se cierra: vuelve a abrirla para jugar.")
                     + " Los archivos del juego no se copian: se usan donde están. La app lee y escribe la carpeta, así que "
                     + "Android pide \"Acceso a todos los archivos\".",
                     "为应用文件选择文件夹：设置（mhgurecomp.ini）、存档、模组、金手指、截图和 log.txt，以便用本设备的文件"
                     + "管理器打开（应用自己的文件夹位于 Android/data，文件管理器无法访问）。例如在设备存储中新建一个 "
                     + "MHGU 文件夹；如果以前用过（并重装了应用），请再次选择它。应用会把文件复制到那里（替换其中较旧的"
                     + "文件前会询问），并从现在起使用该文件夹" + (returnOnly ? "。" : "，然后关闭：请重新打开以游玩。")
                     + " 游戏文件不会被复制：它们仍在原位置使用。应用会直接读写该文件夹，因此 Android 会请求“所有文件"
                     + "访问权限”。");
        TextView intro = text(body, 14, CREAM, false);
        intro.setPadding(0, dp(10), 0, dp(10));
        intro.setLineSpacing(dp(1), 1);
        box.addView(intro);

        status = text("", 14, GOLD, true);
        status.setGravity(Gravity.CENTER);
        status.setPadding(0, dp(6), 0, dp(6));
        box.addView(status);

        LinearLayout row = new LinearLayout(this);
        row.setGravity(Gravity.CENTER);
        primary = button(mode == MODE_BACK ? tr("Copy", "Copiar", "复制")
                                           : tr("Choose folder", "Elegir carpeta", "选择文件夹"), true);
        primary.setOnClickListener(v -> {
            if (mode == MODE_BACK) { copyFiles(FilesFolder.app(this)); return; }
            if (Build.VERSION.SDK_INT < 30) {
                fail(tr("This needs Android 11 or newer.", "Esto necesita Android 11 o posterior.", "需要 Android 11 或更高版本。"));
                return;
            }
            Intent pick = new Intent(this, GameFolderActivity.class);
            pick.putExtra(GameFolderActivity.EXTRA_PICK_FOLDER, true);
            pick.putExtra(GameActivity.EXTRA_LANGUAGE, language);
            startActivityForResult(pick, PICK);
        });
        Button cancel = button(tr("Cancel", "Cancelar", "取消"), false);
        cancel.setOnClickListener(v -> finish());
        LinearLayout.LayoutParams size = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, dp(46));
        size.setMargins(dp(8), 0, dp(8), 0);
        row.addView(primary, size);
        row.addView(cancel, size);
        row.setPadding(0, dp(8), 0, 0);
        box.addView(row);

        ScrollView scroll = new ScrollView(this);
        scroll.setBackgroundColor(PANEL);
        scroll.setFillViewport(true);
        LinearLayout page = new LinearLayout(this);
        page.setGravity(Gravity.CENTER);
        page.setPadding(dp(32), dp(10), dp(32), dp(10));
        int room = getResources().getDisplayMetrics().widthPixels - dp(64);
        page.addView(box, new LinearLayout.LayoutParams(Math.min(dp(640), room), LinearLayout.LayoutParams.WRAP_CONTENT));
        scroll.addView(page);
        setContentView(scroll);
        primary.requestFocus();
    }

    private void say(String s, boolean bad) {
        status.setText(s);
        status.setTextColor(bad ? ALERT : GOLD);
    }

    private void fail(String why) {
        say(why, true);
        primary.setEnabled(true);
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != PICK) return;
        if (result != RESULT_OK || data == null || data.getStringExtra(EXTRA_PATH) == null) { say("", false); return; }
        copyFiles(new File(data.getStringExtra(EXTRA_PATH)));
    }

    // ------------------------------------------------------------------ la copia (un hilo aparte)
    private void copyFiles(final File to) {
        final File from = FilesFolder.get(this);
        String a, b;
        try {
            a = from.getCanonicalPath();
            b = to.getCanonicalPath();
        } catch (IOException e) {
            a = from.getPath();
            b = to.getPath();
        }
        if (a.equals(b)) {
            fail(tr("That is the folder the app's files are in already.",
                    "Esa ya es la carpeta donde están los archivos de la app.",
                    "应用文件已经在这个文件夹中。"));
            return;
        }
        if (mode != MODE_BACK && b.startsWith(a + "/")) {  // (elegir una carpeta dentro de la actual: se copiaría a sí misma)
            fail(tr("That folder is inside the one the app's files are in now: pick another one.",
                    "Esa carpeta está dentro de la que tiene ahora los archivos de la app: elige otra.",
                    "该文件夹位于当前应用文件文件夹内部：请选择其他文件夹。"));
            return;
        }
        stop = false;
        primary.setEnabled(false);
        progress = new AlertDialog.Builder(this).setTitle(title())
                .setMessage(tr("Looking at the app's files...", "Mirando los archivos de la app...", "正在检查应用文件……"))
                .setNegativeButton(tr("Stop", "Detener", "停止"), null).show();
        progress.getButton(DialogInterface.BUTTON_NEGATIVE).setOnClickListener(v -> {
            stop = true;
            show(tr("Stopping...", "Deteniendo...", "正在停止……"));
        });
        new Thread(() -> {
            final List<File> files = new ArrayList<>(), newer = new ArrayList<>();
            list(from, true, files);
            for (File f : files) {
                File out = target(from, to, f);
                if (out.isFile() && f.lastModified() > out.lastModified() + 2000 && !sameContent(f, out)) newer.add(f);
            }
            runOnUiThread(() -> {
                if (stop) {
                    progress.dismiss();
                    end();
                    return;
                }
                if (newer.isEmpty()) {
                    copyAll(from, to, files, false);
                    return;
                }
                StringBuilder names = new StringBuilder();
                DateFormat when = DateFormat.getDateTimeInstance(DateFormat.MEDIUM, DateFormat.SHORT);
                for (int i = 0; i < newer.size() && i < 6; i++) {
                    File f = newer.get(i);
                    names.append("\n").append(from.toURI().relativize(f.toURI()).getPath()).append(": ")
                            .append(when.format(new java.util.Date(f.lastModified()))).append(" ")
                            .append(tr("here", "aquí", "此处")).append(", ")
                            .append(when.format(new java.util.Date(target(from, to, f).lastModified()))).append(" ")
                            .append(tr("there", "allí", "那里"));
                }
                if (newer.size() > 6)
                    names.append("\n").append(tr("... and ", "... y ", "……以及 ")).append(newer.size() - 6)
                            .append(tr(" more", " más", " 个"));
                progress.hide();
                new AlertDialog.Builder(this).setTitle(title())
                        .setMessage(tr(newer.size() + " files in " + to + " are older than the ones being copied (if the save is one of them, the progress there is older):",
                                       newer.size() + " archivos en " + to + " son más antiguos que los que se copian (si la partida es uno de ellos, el progreso de allí es más antiguo):",
                                       to + " 中有 " + newer.size() + " 个文件比正在复制的文件更旧（如果其中有存档，那里的进度会更旧）：")
                                + names + "\n\n" + tr("Replace them with the newer ones? The replaced files are kept beside them as .bak. Or keep what is there and copy only what is missing.",
                                       "¿Reemplazarlos por los más nuevos? Los reemplazados se conservan al lado como .bak. O conserva lo que hay y copia solo lo que falta.",
                                       "用较新的文件替换它们？被替换的文件会以 .bak 保留在旁边。或者保留现有文件，只复制缺少的。"))
                        .setCancelable(false)
                        .setPositiveButton(tr("Replace", "Reemplazar", "替换"), (d, w) -> {
                            progress.show();
                            copyAll(from, to, files, true);
                        })
                        .setNegativeButton(tr("Keep what's there", "Conservar lo que hay", "保留现有文件"), (d, w) -> {
                            progress.show();
                            copyAll(from, to, files, false);
                        })
                        .setNeutralButton(tr("Cancel", "Cancelar", "取消"), (d, w) -> {
                            progress.dismiss();
                            end();
                        })
                        .show();
            });
        }, "files folder").start();
    }

    // replace: los archivos de allí más antiguos que los de aquí se reemplazan (quedan como .bak); si no, solo se
    // copia lo que falta
    private void copyAll(final File from, final File to, final List<File> files, final boolean replace) {
        new Thread(() -> {
            String error = null;
            final int[] counts = new int[3];  // copiados, conservados, reemplazados
            final boolean existing = new File(to, "mhgurecomp.ini").isFile();
            try {
                long total = 0;
                for (File f : files) total += f.length();
                long done = 0;
                byte[] buf = new byte[1 << 16];
                for (File f : files) {
                    if (stop) throw new IOException("stopped");
                    File out = target(from, to, f);
                    done += f.length();
                    boolean replacing = false;
                    if (out.exists()) {
                        replacing = replace && out.isFile() && f.lastModified() > out.lastModified() + 2000 && !sameContent(f, out);
                        if (!replacing) {
                            counts[1]++;
                            continue;
                        }
                    }
                    File dir = out.getParentFile();
                    if (dir != null && !dir.isDirectory() && !dir.mkdirs()) throw new IOException("can't make " + dir);
                    File part = new File(out.getPath() + ".part");
                    try (InputStream in = new FileInputStream(f)) {
                        copy(in, part, buf);
                    }
                    part.setLastModified(f.lastModified());  // (que el más nuevo siga siendo el más nuevo)
                    if (replacing && !out.renameTo(backupOf(out))) {
                        part.delete();
                        throw new IOException("can't keep " + out + " as .bak");
                    }
                    if (!part.renameTo(out)) {
                        part.delete();
                        throw new IOException("can't write " + out);
                    }
                    counts[replacing ? 2 : 0]++;
                    show(tr("Copying the app's files: ", "Copiando los archivos de la app: ", "正在复制应用文件：")
                            + (total > 0 ? done * 100 / total : 100) + "% (" + f.getName() + ")");
                }
            } catch (Exception e) {
                error = stop ? null : tr("The copy failed (" + e.getMessage() + "). The app keeps its files where they are. "
                        + "What was copied is left in " + to + (counts[2] > 0 ? ", and the " + counts[2] + " files it replaced "
                        + "there are kept beside them as .bak." : "."),
                        "La copia falló (" + e.getMessage() + "). La app conserva sus archivos donde están. Lo copiado queda en "
                        + to + (counts[2] > 0 ? ", y los " + counts[2] + " archivos que reemplazó allí se conservan al lado "
                        + "como .bak." : "."),
                        "复制失败（" + e.getMessage() + "）。应用会保留原有文件。已复制的内容留在 " + to
                        + (counts[2] > 0 ? "，被替换的 " + counts[2] + " 个文件以 .bak 保留在旁边。" : "。"));
            }
            final String err = error;
            runOnUiThread(() -> {
                progress.dismiss();
                if (stop) {
                    end();
                    return;
                }
                if (err != null) {
                    fail(err);
                    return;
                }
                try {
                    FilesFolder.note(FilesFolderActivity.this, to);
                    FilesFolder.asked(FilesFolderActivity.this, true);
                } catch (IOException e) {
                    fail(tr("The app couldn't note the new files folder in its own folder: it keeps using the one it had. ("
                                    + e.getMessage() + ")",
                            "La app no pudo anotar la nueva carpeta de archivos en su carpeta propia: sigue usando la que tenía. ("
                                    + e.getMessage() + ")",
                            "应用无法在自己的文件夹中记录新的文件文件夹：将继续使用原来的文件夹。（" + e.getMessage() + "）"));
                    return;
                }
                say((existing && mode == MODE_CHOOSE
                                ? tr("The app's files from before were found in " + to + " and are used. ",
                                     "Se encontraron los archivos de la app de antes en " + to + " y se usan. ",
                                     "在 " + to + " 中找到了以前的应用文件，将使用它们。 ")
                                : "")
                        + tr(counts[0] + " files copied", counts[0] + " archivos copiados", counts[0] + " 个文件已复制")
                        + (counts[2] > 0 ? tr(", " + counts[2] + " older ones replaced (kept as .bak)",
                                              ", " + counts[2] + " más antiguos reemplazados (como .bak)",
                                              "，替换了 " + counts[2] + " 个较旧文件（保留为 .bak）") : "")
                        + (counts[1] > 0 ? tr(", " + counts[1] + " already there kept",
                                              ", " + counts[1] + " ya estaban y se conservan",
                                              "，" + counts[1] + " 个已存在并保留") : "")
                        + tr(". The app's files are in " + to + " from now on.",
                             ". Los archivos de la app están en " + to + " desde ahora.",
                             "。应用文件从现在起位于 " + to + "。")
                        + (returnOnly ? "" : tr(" The app closes now: start it again to play.",
                                                " La app se cierra: vuelve a abrirla para jugar.",
                                                " 应用即将关闭：请重新打开以游玩。")), false);
                if (returnOnly) {
                    setResult(RESULT_OK, new Intent().putExtra(EXTRA_PATH, to.getAbsolutePath()));
                    finish();
                    return;
                }
                restart();
            });
        }, "files folder copy").start();
    }

    private static File target(File from, File to, File f) {
        return new File(to, from.toURI().relativize(f.toURI()).getPath());
    }

    private static boolean sameContent(File a, File b) {
        if (a.length() != b.length()) return false;
        byte[] x = new byte[1 << 16], y = new byte[1 << 16];
        try (InputStream p = new FileInputStream(a); InputStream q = new FileInputStream(b)) {
            for (int n; (n = p.read(x)) > 0; ) {
                int m = 0;
                while (m < n) {
                    int r = q.read(y, m, n - m);
                    if (r <= 0) return false;
                    m += r;
                }
                for (int i = 0; i < n; i++)
                    if (x[i] != y[i]) return false;
            }
            return true;
        } catch (IOException e) {
            return false;
        }
    }

    // Un nombre para conservarlo al lado: x.bak, o x.2.bak si ya hay uno de un reemplazo anterior (nunca se escribe encima)
    private static File backupOf(File out) {
        File bak = new File(out.getPath() + ".bak");
        for (int i = 2; bak.exists(); i++) bak = new File(out.getPath() + "." + i + ".bak");
        return bak;
    }

    private void list(File dir, boolean top, List<File> out) {
        File[] in = dir.listFiles();
        if (in == null) return;
        for (File f : in) {
            if (top && !COPIED.contains(f.getName())) continue;
            if (f.getName().endsWith(".part") || f.getName().endsWith(".bak")) continue;
            if (f.isDirectory()) list(f, false, out);
            else if (f.isFile()) out.add(f);
        }
    }

    private static void copy(InputStream in, File to, byte[] buf) throws IOException {
        try (OutputStream out = new FileOutputStream(to)) {
            for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n);
        }
    }

    private void show(final String s) {
        runOnUiThread(() -> {
            if (progress != null && progress.isShowing()) progress.setMessage(s);
        });
    }

    private void end() {
        if (returnOnly) setResult(RESULT_CANCELED);
        finish();
    }

    /** De vuelta al juego por PhoenixActivity: el runtime tiene abiertos los archivos de la carpeta anterior. */
    private void restart() {
        Intent intent = new Intent(this, PhoenixActivity.class);
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        intent.putExtra(PhoenixActivity.EXTRA_PID, Process.myPid());
        startActivity(intent);
        finishAffinity();
    }

    /** La carpeta elegida ya no existe: se puede olvidar (no se copia ni se borra nada). */
    private void forgetDialog(String problem) {
        new AlertDialog.Builder(this).setTitle(title())
                .setMessage(problem + "\n\n" + tr("Forget that folder and keep the app's files in its own folder from now on? "
                                + "Nothing is copied or deleted, and you can choose it again later. The app closes: start it again.",
                        "¿Olvidar esa carpeta y guardar los archivos de la app en su carpeta propia desde ahora? No se copia ni se "
                                + "borra nada, y puedes elegirla otra vez más adelante. La app se cierra: vuelve a abrirla.",
                        "要忘记该文件夹，并从现在起把应用文件保存在应用自己的文件夹中吗？不会复制或删除任何内容，之后仍可再次"
                                + "选择它。应用将关闭：请重新打开。"))
                .setCancelable(false)
                .setPositiveButton(tr("Forget it", "Olvidarla", "忘记它"), (d, w) -> {
                    new File(FilesFolder.app(this), FilesFolder.POINTER).delete();
                    FilesFolder.asked(this, true);
                    if (returnOnly) {
                        setResult(RESULT_OK, new Intent().putExtra(EXTRA_PATH, FilesFolder.app(this).getAbsolutePath()));
                        finish();
                    } else {
                        restart();
                    }
                })
                .setNegativeButton(tr("Cancel", "Cancelar", "取消"), (d, w) -> finish())
                .show();
    }

    @Override
    protected void onDestroy() {
        stop = true;
        if (progress != null && progress.isShowing()) progress.dismiss();
        super.onDestroy();
    }
}
