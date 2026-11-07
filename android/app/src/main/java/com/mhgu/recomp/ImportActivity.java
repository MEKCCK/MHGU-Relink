package com.mhgu.recomp;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.Dialog;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.GradientDrawable;
import android.os.Bundle;
import android.os.Environment;
import android.os.Process;
import android.util.DisplayMetrics;
import android.util.Log;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.Window;
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
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Locale;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * Ajustes -> Trucos / Mods: lo que el juego lee de las carpetas cheats/, mods/ y dlc/ de la app (Android/data, que en
 * Android 11 o más no se puede abrir con un explorador de archivos), copiado desde donde el jugador lo tenga:
 *   - trucos: archivos .txt en el formato de Atmosphere (runtime/cheats.c), a cheats/;
 *   - un mod: una carpeta con romfs/ o exefs/ (o los archivos de nativeNX), o un .zip de ella, a mods/<nombre>/
 *     (runtime/mods.c; su orden y si está activo, en la página Mods);
 *   - DLC: cada carpeta con un DLC.dlcp, a dlc/<id del título o número>/ (runtime/services/dlc.c).
 * Se lee al arrancar el juego: al cerrar, si algo cambió, se reinicia (como DriverActivity). La app ya tiene acceso a todos
 * los archivos (lo pide la pantalla de la carpeta del juego), así que el explorador es propio, con el estilo del juego.
 */
public class ImportActivity extends Activity {
    public static final String KIND = "kind";
    public static final int CHEATS = 0, MOD = 1, DLC = 2;
    private static final String TAG = "MHGU";
    private static final int WINE = Color.rgb(38, 18, 14), PANEL = Color.rgb(26, 12, 9), CREAM = Color.rgb(240, 228, 200);
    private static final int GOLD = Color.rgb(226, 182, 92), BRONZE = Color.rgb(62, 38, 22), EDGE = Color.rgb(196, 150, 70);
    private static final int ALERT = Color.rgb(255, 150, 120), LIT = Color.rgb(96, 62, 24);
    private static final Pattern TITLE_ID = Pattern.compile("(?i)\\b(0100770008dd[9a-f][0-9a-f]{3})\\b");

    private int kind;
    private boolean es, zh, changed, busy;
    private TextView status;
    private LinearLayout list;
    private Button add, closeButton;
    private Dialog browser;

    private File home() {
        File f = getExternalFilesDir(null);
        return f != null ? f : getFilesDir();
    }
    private String sub() { return kind == CHEATS ? "cheats" : kind == MOD ? "mods" : "dlc"; }

    /** The game's folder (GameFolderActivity's choice), or null. */
    private File gameFolder() {
        try (FileInputStream in = new FileInputStream(new File(home(), "game_folder.txt"))) {
            byte[] b = new byte[4096];
            int n = in.read(b);
            String path = n > 0 ? new String(b, 0, n, "UTF-8").trim() : "";
            return path.isEmpty() ? null : new File(path);
        } catch (Exception e) {
            return null;
        }
    }

    /**
     * Where they go: the game folder's cheats/, mods/ or dlc/ (the game reads them there, and a player can put them
     * there by hand with any file manager); the app's own folder if the game folder can't be written (an SD card).
     */
    private File target() {
        File game = gameFolder();
        if (game != null) {
            File in = new File(game, sub());
            if ((in.isDirectory() || in.mkdirs()) && in.canWrite()) return in;
        }
        return new File(home(), sub());
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

    private Button button(String s, boolean strong, int fill) {
        Button b = new Button(this);
        b.setText(s);
        b.setAllCaps(false);
        b.setTextColor(strong ? GOLD : CREAM);
        b.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
        if (strong) b.setTypeface(Typeface.DEFAULT_BOLD);
        b.setBackground(plate(fill));
        b.setPadding(dp(14), 0, dp(14), 0);
        return b;
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        int language = RecompLanguage.from(this);
        es = language == 1;
        zh = language == 2;
        kind = getIntent().getIntExtra(KIND, CHEATS);

        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(24), dp(14), dp(24), dp(16));
        box.setBackground(plate(WINE));
        String[] titles = zh ? new String[] {"金手指", "模组", "DLC"}
                : es ? new String[] {"TRUCOS", "MODS", "DLC"} : new String[] {"CHEATS", "MODS", "DLC"};
        TextView title = text(titles[kind], 20, GOLD, true);
        title.setGravity(Gravity.CENTER);
        box.addView(title);
        String[] bodies = zh ? new String[] {
            "Atmosphere 格式的作弊 .txt 文件，适用于 1.4.0 版本（构建 ID 为 FB08F1D20FD1204F）。添加后，可在“金手指”页面逐项启用。修改游戏代码的作弊也能使用，但对应部分运行速度会变慢。",
            "支持含 romfs、exefs 或 nativeNX 的文件夹、ZIP、RAR，也可直接导入 .nro 插件、.ips、.ips32、.pchtxt 补丁。压缩包含多个版本时请选择一个。RAR 暂不支持加密或分卷包。导入后在“模组”页面排序和启用，重启后加载。模组和补丁须符合各自格式并匹配游戏版本；导入成功不代表内容一定兼容。",
            "导入你自己的游戏 DLC：每个 DLC 文件夹中都应有一个 DLC.dlcp 文件。选择一个文件夹后，其中找到的 DLC 都会被复制。进入游戏主菜单并选择“下载内容”即可使用。此功能仍在实验阶段。"}
            : es ? new String[] {
            "Archivos .txt de trucos en el formato de Atmosphere, para la versión 1.4.0 (su id de compilación es "
                + "FB08F1D20FD1204F). Después se activan uno por uno en la página Trucos. Los que cambian el código del juego "
                + "funcionan, pero esa parte del juego corre más lenta.",
            "Carpetas, ZIP o RAR con romfs, exefs o nativeNX; también plugins .nro y parches .ips, .ips32 o .pchtxt. "
                + "Si hay varias versiones, elige una. Ordénalos y actívalos en Mods; se cargan al reiniciar. Cada mod y parche "
                + "debe respetar su formato y ser compatible con la versión del juego; importarlo no garantiza que funcione. "
                + "Por ahora no se admiten RAR cifrados ni divididos en volúmenes.",
            "Tus propios DLC del juego: la carpeta de cada uno tiene un archivo DLC.dlcp. Elige una carpeta y se copian "
                + "todos los que haya adentro. Después, en el juego: menú principal, Descarga. Es experimental."}
            : new String[] {
            "Cheat .txt files in Atmosphere's format, for version 1.4.0 (its build id is FB08F1D20FD1204F). Then turn them on "
                + "one by one on the Cheats page. Those that change the game's code work, but that part of the game runs slower.",
            "Folders, ZIPs or RARs containing romfs, exefs or nativeNX, plus .nro plugins and .ips, .ips32 or .pchtxt patches. "
                + "Choose one variant if a package contains several. Order and enable them on the Mods page; they load on restart. "
                + "Each mod and patch must follow its format and match the game version; importing it does not guarantee compatibility. "
                + "Encrypted and multi-volume RARs are not supported yet.",
            "Your own DLC of the game: each one's folder holds a DLC.dlcp file. Choose a folder and every one in it is copied. "
                + "Then, in the game: main menu, Download. It is experimental."};
        TextView body = text(bodies[kind], 14, CREAM, false);
        body.setPadding(0, dp(10), 0, dp(6));
        body.setLineSpacing(dp(1), 1);
        box.addView(body);
        // (where they go, which is also where they can be put by hand)
        TextView where = text((zh ? "也可以手动将文件复制到手机的此文件夹： "
                                  : es ? "También puedes copiarlos tú a esta carpeta del teléfono: "
                                  : "You can also copy them yourself into this folder on the phone: ") + target().getAbsolutePath(),
                13, GOLD, false);
        where.setPadding(0, 0, 0, dp(10));
        box.addView(where);
        list = new LinearLayout(this);
        list.setOrientation(LinearLayout.VERTICAL);
        box.addView(list);
        status = text("", 14, GOLD, true);
        status.setGravity(Gravity.CENTER);
        status.setPadding(0, dp(8), 0, 0);
        box.addView(status);

        LinearLayout row = new LinearLayout(this);
        row.setGravity(Gravity.CENTER);
        row.setPadding(0, dp(12), 0, 0);
        String[] adds = zh ? new String[] {"添加金手指", "添加模组", "添加 DLC"}
                           : es ? new String[] {"Agregar trucos", "Agregar un mod", "Agregar DLC"}
                           : new String[] {"Add cheats", "Add a mod", "Add DLC"};
        add = button(adds[kind], true, BRONZE);
        add.setOnClickListener(v -> openBrowser(null));
        LinearLayout.LayoutParams size = new LinearLayout.LayoutParams(dp(190), dp(46));
        size.setMargins(dp(8), 0, dp(8), 0);
        row.addView(add, size);
        closeButton = button(zh ? "关闭" : es ? "Cerrar" : "Close", false, BRONZE);
        closeButton.setOnClickListener(v -> {
            if (busy) return;
            if (changed) restart();
            else finish();
        });
        row.addView(closeButton, new LinearLayout.LayoutParams(size));
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
        refresh();
    }

    private void say(String s, boolean bad) {
        status.setText(s);
        status.setTextColor(bad ? ALERT : GOLD);
    }

    /** What is installed: each with its Remove. */
    private void refresh() {
        list.removeAllViews();
        File[] all = target().listFiles(f -> !f.getName().startsWith("_") && !f.getName().startsWith(".")
                && (kind == CHEATS ? f.isFile() && f.getName().toLowerCase(Locale.ROOT).endsWith(".txt") : f.isDirectory()));
        if (all == null || all.length == 0) {
            TextView none = text(zh ? "（尚未安装）" : es ? "(nada instalado todavía)" : "(nothing installed yet)", 13, CREAM, false);
            none.setGravity(Gravity.CENTER);
            list.addView(none);
            return;
        }
        Arrays.sort(all);
        for (File f : all) {
            LinearLayout row = new LinearLayout(this);
            row.setGravity(Gravity.CENTER_VERTICAL);
            TextView name = text("◆  " + f.getName(), 14, CREAM, false);
            name.setSingleLine(true);
            row.addView(name, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
            Button remove = button(zh ? "移除" : es ? "Quitar" : "Remove", false, BRONZE);
            remove.setOnClickListener(v -> {
                if (busy) return;
                delete(f);
                changed();
                refresh();
            });
            row.addView(remove, new LinearLayout.LayoutParams(dp(92), dp(42)));
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
            lp.setMargins(0, dp(3), 0, dp(3));
            list.addView(row, lp);
        }
    }

    private void changed() {
        changed = true;
        closeButton.setText(zh ? "重启游戏" : es ? "Reiniciar el juego" : "Restart the game");
    }

    /** Back to the game through PhoenixActivity: these are read at the start. */
    private void restart() {
        Intent intent = new Intent(this, PhoenixActivity.class);
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        intent.putExtra(PhoenixActivity.EXTRA_PID, Process.myPid());
        startActivity(intent);
        finishAffinity();
    }

    private static void delete(File f) {
        File[] kids = f.listFiles();
        if (kids != null) for (File k : kids) delete(k);
        f.delete();
    }

    // ---- the browser (folders, and the files this kind takes)
    private List<File> roots() {
        List<File> out = new ArrayList<>();
        out.add(Environment.getExternalStorageDirectory());
        File[] mine = getExternalFilesDirs(null);
        if (mine != null)
            for (File f : mine) {
                if (f == null) continue;
                String p = f.getAbsolutePath();
                int cut = p.indexOf("/Android/data/");
                if (cut <= 0) continue;
                File root = new File(p.substring(0, cut));
                if (!root.equals(out.get(0)) && root.isDirectory()) out.add(root);
            }
        return out;
    }

    private boolean takes(File f) {
        String n = f.getName().toLowerCase(Locale.ROOT);
        return kind == CHEATS ? n.endsWith(".txt") : kind == MOD ? ModFiles.accepts(n) : n.equals("dlc.dlcp");
    }

    private LinearLayout.LayoutParams rowParams() {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, dp(42));
        lp.setMargins(0, dp(3), 0, dp(3));
        return lp;
    }

    private void openBrowser(File at) {
        if (busy) return;
        if (browser != null) browser.dismiss();
        List<File> roots = roots();
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setBackground(plate(PANEL));
        box.setPadding(dp(12), dp(12), dp(12), dp(12));
        String[] heads = zh ? new String[] {"选择金手指文件", "选择模组文件夹、ZIP、RAR、插件或补丁", "选择 DLC 文件夹"}
                            : es ? new String[] {"ELIGE UN ARCHIVO DE TRUCOS", "ELIGE CARPETA, ZIP, RAR, PLUGIN O PARCHE", "ELIGE LA CARPETA DE TUS DLC"}
                            : new String[] {"CHOOSE A CHEAT FILE", "CHOOSE A MOD FOLDER, ZIP, RAR, PLUGIN OR PATCH", "CHOOSE YOUR DLC'S FOLDER"};
        TextView title = text(heads[kind], 16, GOLD, true);
        title.setGravity(Gravity.CENTER);
        box.addView(title);
        LinearLayout tabs = new LinearLayout(this);
        tabs.setGravity(Gravity.CENTER);
        File dir = at != null ? at : roots.get(0);
        File root = roots.get(0);
        for (File r : roots) if (dir.getAbsolutePath().startsWith(r.getAbsolutePath())) root = r;
        for (int i = 0; i < roots.size(); i++) {
            File r = roots.get(i);
            Button t = button(i == 0 ? (zh ? "手机存储" : es ? "Memoria interna" : "Phone storage")
                    : (zh ? "SD 卡" : es ? "Tarjeta SD" : "SD card"), r.equals(root), r.equals(root) ? LIT : BRONZE);
            t.setOnClickListener(v -> openBrowser(r));
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, dp(40));
            lp.setMargins(dp(4), dp(6), dp(4), dp(4));
            tabs.addView(t, lp);
        }
        box.addView(tabs);
        String shown = dir.getAbsolutePath().substring(root.getAbsolutePath().length());
        TextView where = text(shown.isEmpty() ? "/" : shown, 13, CREAM, false);
        where.setPadding(dp(4), dp(2), dp(4), dp(6));
        box.addView(where);
        LinearLayout items = new LinearLayout(this);
        items.setOrientation(LinearLayout.VERTICAL);
        final File here = dir;
        if (!dir.equals(root)) {
            Button up = button("⬑  " + (zh ? "上级" : es ? "Subir" : "Up"), false, BRONZE);
            up.setGravity(Gravity.START | Gravity.CENTER_VERTICAL);
            up.setOnClickListener(v -> openBrowser(here.getParentFile()));
            items.addView(up, rowParams());
        }
        File[] kids = dir.listFiles(f -> !f.getName().startsWith(".") && (f.isDirectory() || takes(f)));
        if (kids != null) {
            Arrays.sort(kids, (a, b) -> a.isDirectory() != b.isDirectory() ? (a.isDirectory() ? -1 : 1)
                                                                           : a.getName().compareToIgnoreCase(b.getName()));
            for (File k : kids) {
                boolean folder = k.isDirectory();
                Button b = button((folder ? "▸  " : "◆  ") + k.getName(), !folder, folder ? BRONZE : LIT);
                b.setGravity(Gravity.START | Gravity.CENTER_VERTICAL);
                b.setOnClickListener(v -> {
                    if (folder) { openBrowser(k); return; }
                    browser.dismiss();
                    start(k);
                });
                items.addView(b, rowParams());
            }
        }
        if (kids == null || kids.length == 0) {
            TextView none = text(zh ? "（空）" : es ? "(vacío)" : "(empty)", 13, CREAM, false);
            none.setPadding(dp(6), dp(8), dp(6), dp(8));
            items.addView(none);
        }
        ScrollView scroll = new ScrollView(this);
        scroll.addView(items);
        box.addView(scroll, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1));
        LinearLayout row = new LinearLayout(this);
        row.setGravity(Gravity.CENTER);
        row.setPadding(0, dp(8), 0, 0);
        if (kind != CHEATS) {  // (a folder: the mod itself, or where the DLC are)
            Button use = button(zh ? "使用此文件夹" : es ? "Usar esta carpeta" : "Use this folder", true, BRONZE);
            use.setOnClickListener(v -> { browser.dismiss(); start(here); });
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, dp(44));
            lp.setMargins(dp(5), 0, dp(5), 0);
            row.addView(use, lp);
        }
        Button cancel = button(zh ? "取消" : es ? "Cancelar" : "Cancel", false, BRONZE);
        cancel.setOnClickListener(v -> browser.dismiss());
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, dp(44));
        lp.setMargins(dp(5), 0, dp(5), 0);
        row.addView(cancel, lp);
        box.addView(row);
        browser = new Dialog(this);
        browser.requestWindowFeature(Window.FEATURE_NO_TITLE);
        browser.setContentView(box);
        Window w = browser.getWindow();
        if (w != null) {
            w.setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
            DisplayMetrics m = getResources().getDisplayMetrics();
            w.setLayout(Math.min(dp(640), (int) (m.widthPixels * .92f)), (int) (m.heightPixels * .92f));
        }
        browser.show();
    }

    // ---- the copy (a background thread)
    private long copied, total;

    private void start(File from) {
        busy = true;
        add.setEnabled(false);
        copied = total = lastTick = 0;
        say(zh ? "正在复制…" : es ? "Copiando…" : "Copying…", false);
        if (kind == MOD) {
            say(zh ? "正在检查模组…" : es ? "Revisando el mod…" : "Checking mod…", false);
            new Thread(() -> {
                try {
                    List<String> roots = ModFiles.roots(from);
                    runOnUiThread(() -> {
                        if (isFinishing() || isDestroyed()) return;
                        if (roots.size() == 1) copy(from, roots.get(0));
                        else {
                            String[] names = roots.toArray(new String[0]);
                            new AlertDialog.Builder(this)
                                    .setTitle(zh ? "选择要导入的模组版本" : es ? "Elige una versión del mod" : "Choose one mod variant")
                                    .setItems(names, (dialog, which) -> copy(from, roots.get(which)))
                                    .setNegativeButton(zh ? "取消" : es ? "Cancelar" : "Cancel", (dialog, which) -> cancelImport())
                                    .setOnCancelListener(dialog -> cancelImport())
                                    .show();
                        }
                    });
                } catch (Exception e) { runOnUiThread(() -> finished(null, e)); }
            }).start();
        } else copy(from, null);
    }

    private void cancelImport() {
        busy = false;
        add.setEnabled(true);
        say(zh ? "已取消" : es ? "Cancelado" : "Cancelled", false);
    }

    private void copy(File from, String variant) {
        new Thread(() -> {
            String done;
            Exception error = null;
            try {
                done = kind == CHEATS ? importCheats(from) : kind == MOD
                        ? ModFiles.install(from, variant, target(), this::copyStream) : importDlc(from);
            } catch (Exception e) {
                Log.e(TAG, "import: " + e);
                done = null;
                error = e;
            }
            final String ok = done;
            final Exception why = error;
            runOnUiThread(() -> finished(ok, why));
        }).start();
    }

    private void finished(String ok, Exception why) {
        if (isFinishing() || isDestroyed()) return;
        busy = false;
        add.setEnabled(true);
        if (ok != null) {
            changed();
            say((zh ? "已导入：" : es ? "Importado: " : "Imported: ") + ok
                    + (zh ? "。重启游戏后加载。" : es ? ". Se carga al reiniciar el juego." : ". Loads when the game restarts."), false);
        } else {
            say((zh ? "操作失败：" : es ? "No se pudo: " : "Import failed: ") + importError(why), true);
        }
        refresh();
    }

    private String importError(Exception error) {
        if (!(error instanceof ModFiles.Problem)) return error == null ? "" : error.getMessage();
        switch (((ModFiles.Problem) error).reason) {
            case NO_PAYLOAD: return zh ? "未找到 romfs、exefs 或 nativeNX 文件夹（最多 8 层）"
                    : es ? "No se encontró romfs, exefs ni nativeNX (máximo 8 niveles)"
                    : "No romfs, exefs or nativeNX folder found (up to 8 levels)";
            case SEARCH_LIMIT: return zh ? "模组目录项目过多，请选择更具体的文件夹"
                    : es ? "Demasiadas entradas; elige una carpeta más específica"
                    : "Too many entries; choose a more specific folder";
            case UNSAFE_PATH: return zh ? "模组包含越界路径或目录链接"
                    : es ? "El mod contiene rutas fuera de la carpeta o enlaces"
                    : "The mod contains paths outside its folder or folder links";
            case SOURCE_TARGET: return zh ? "源文件夹与安装位置重叠，请从模组目录外导入"
                    : es ? "Origen y destino se solapan; importa desde fuera de mods"
                    : "Source and destination overlap; import from outside the mods folder";
            case UNSUPPORTED_RAR: return zh ? "RAR 暂不支持加密、分卷、链接或特殊文件"
                    : es ? "RAR cifrado, dividido, con enlaces o archivos especiales no admitido"
                    : "Encrypted, multi-volume, linked or special-file RARs are not supported";
            default: return zh ? "请选择模组文件夹、ZIP、RAR、NRO、IPS、IPS32 或 PCHTXT"
                    : es ? "Elige carpeta, ZIP, RAR, NRO, IPS, IPS32 o PCHTXT"
                    : "Choose a folder, ZIP, RAR, NRO, IPS, IPS32 or PCHTXT";
        }
    }

    private void progress() {
        long c = copied, t = total;
        runOnUiThread(() -> say((zh ? "正在复制… " : es ? "Copiando… " : "Copying… ")
                + (t > 0 ? (c * 100 / t) + " %" : (c >> 20) + " MB"), false));
    }

    private void copyFile(File from, File to) throws IOException {
        to.getParentFile().mkdirs();
        File tmp = new File(to.getPath() + ".part");
        try (InputStream in = new FileInputStream(from); OutputStream out = new FileOutputStream(tmp)) {
            copyStream(in, out);
        }
        to.delete();
        if (!tmp.renameTo(to)) throw new IOException(zh ? "无法写入 " + to.getName()
                : es ? "No se pudo escribir " + to.getName() : "can't write " + to.getName());
    }

    private long lastTick;
    private void copyStream(InputStream in, OutputStream out) throws IOException {
        byte[] buf = new byte[1 << 16];
        for (int n; (n = in.read(buf)) > 0; ) {
            out.write(buf, 0, n);
            copied += n;
            long now = System.currentTimeMillis();
            if (now - lastTick > 400) { lastTick = now; progress(); }
        }
    }

    private static long sizeOf(File f) {
        if (f.isFile()) return f.length();
        long s = 0;
        File[] kids = f.listFiles();
        if (kids != null) for (File k : kids) s += sizeOf(k);
        return s;
    }

    private void copyTree(File from, File to) throws IOException {
        if (from.isFile()) { copyFile(from, to); return; }
        if (!to.isDirectory() && !to.mkdirs()) throw new IOException(zh ? "无法创建文件夹 " + to.getName()
                : es ? "No se pudo crear " + to.getName() : "can't make " + to.getName());
        File[] kids = from.listFiles();
        if (kids != null) for (File k : kids) copyTree(k, new File(to, k.getName()));
    }

    private static String safeName(String s) {
        String n = s.replaceAll("[^A-Za-z0-9 ._()\\[\\]-]+", "_").trim();
        return n.isEmpty() || n.startsWith(".") || n.startsWith("_") ? "x" + n : n;
    }

    private String importCheats(File txt) throws IOException {
        String name = safeName(txt.getName());
        if (!name.toLowerCase(Locale.ROOT).endsWith(".txt")) name += ".txt";
        total = txt.length();
        copyFile(txt, new File(target(), name));
        // (each one starts off, to be turned on on the Cheats page: the game's list of those off, cheats/_off.txt in the
        // app's folder, gets "<file>\t<name>" for each [name] of the file; {master} codes always run)
        StringBuilder off = new StringBuilder();
        try (java.io.BufferedReader r = new java.io.BufferedReader(new java.io.InputStreamReader(new FileInputStream(txt), "UTF-8"))) {
            for (String l; (l = r.readLine()) != null; ) {
                l = l.trim();
                if (l.length() > 2 && l.startsWith("[") && l.endsWith("]")) off.append(name).append('\t').append(l, 1, l.length() - 1).append('\n');
            }
        }
        File list = new File(home(), "cheats/_off.txt");
        list.getParentFile().mkdirs();
        try (OutputStream o = new FileOutputStream(list, true)) {
            o.write(("\n" + off).getBytes("UTF-8"));
        }
        return name;
    }

    private static void findDlc(File d, int depth, List<File> out) {
        if (depth > 6 || out.size() >= 256) return;
        File[] kids = d.listFiles();
        if (kids == null) return;
        for (File k : kids)
            if (k.isFile() && k.getName().equalsIgnoreCase("DLC.dlcp")) out.add(d);
        for (File k : kids)
            if (k.isDirectory() && !k.getName().startsWith(".")) findDlc(k, depth + 1, out);
    }

    private String importDlc(File from) throws IOException {
        File start = from.isFile() ? from.getParentFile() : from;
        List<File> found = new ArrayList<>();
        findDlc(start, 0, found);
        if (found.isEmpty()) throw new IOException(zh ? "该文件夹中没有 DLC.dlcp 文件"
                : es ? "no hay ningún DLC.dlcp en esa carpeta" : "there is no DLC.dlcp in that folder");
        total = 0;
        for (File d : found) total += sizeOf(d);
        int n = 0, next = 1;
        target().mkdirs();
        for (File d : found) {
            String id = null;  // (the DLC's title id, if its path says it: the index the game knows it by)
            for (File p = d; p != null && id == null; p = p.getParentFile()) {
                Matcher m = TITLE_ID.matcher(p.getName());
                if (m.find()) id = m.group(1).toUpperCase(Locale.ROOT);
            }
            String name = id;
            if (name == null) {
                while (new File(target(), String.format(Locale.ROOT, "%03d", next)).exists()) next++;
                name = String.format(Locale.ROOT, "%03d", next++);
            }
            File to = new File(target(), name);
            delete(to);
            copyTree(d, to);
            n++;
        }
        return zh ? n + " 个 DLC 已复制" : n + (es ? (n == 1 ? " DLC copiado" : " DLC copiados") : " DLC copied");
    }
}
