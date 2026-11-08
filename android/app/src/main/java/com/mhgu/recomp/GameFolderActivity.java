package com.mhgu.recomp;

import android.Manifest;
import android.app.Activity;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.DocumentsContract;
import android.provider.Settings;
import android.text.SpannableStringBuilder;
import android.text.Spanned;
import android.text.style.ForegroundColorSpan;
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
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * La entrada de la app: si los archivos del juego están a mano arranca el juego; si no, muestra qué hace falta y deja
 * elegir la carpeta donde están. El juego los lee desde ahí, sin copiarlos: la ruta queda en game_folder.txt (runtime/main.c).
 * También sirve data/ dentro de la carpeta de la app, como siempre. Ajustes -> Carpeta del juego la abre con EXTRA_CHANGE.
 * Mismas comprobaciones que runtime/main.c: si esa las rechaza deja game_folder_bad.txt y se vuelve a esta pantalla.
 */
public class GameFolderActivity extends Activity {
    static final String EXTRA_CHANGE = "change";
    static final String EXTRA_PICK_FOLDER = "pick_folder";  // (FilesFolderActivity: solo elige una carpeta y la devuelve)
    private static final int PICK = 1, READ = 2, FILES_PICK = 3;
    private static final int WINE = Color.rgb(38, 18, 14), PANEL = Color.rgb(26, 12, 9), CREAM = Color.rgb(240, 228, 200);
    private static final int GOLD = Color.rgb(226, 182, 92), BRONZE = Color.rgb(62, 38, 22), EDGE = Color.rgb(196, 150, 70);
    private static final int ALERT = Color.rgb(255, 150, 120);
    private static final int MIN_FILES = 35000;  // (1.4.0 has 35531; the margin as in runtime/main.c)
    private static final byte[] BUILD_ID = {(byte) 0xfb, 0x08, (byte) 0xf1, (byte) 0xd2, 0x0f, (byte) 0xd1, 0x20, 0x4f};

    private int language;
    private boolean change, waitingAccess, pickFolder, startupChecked;
    private TextView status;
    private Button pick;

    private String tr(String en, String es, String zh) {
        return language == 2 ? zh : language == 1 ? es : en;
    }

    private File home() {
        File f = getExternalFilesDir(null);
        return f != null ? f : getFilesDir();
    }
    private File savedFile() { return new File(home(), "game_folder.txt"); }
    private File rejectedFile() { return new File(home(), "game_folder_bad.txt"); }
    private File ownLib() { return new File(getFilesDir(), "game/libmhgugame.so"); }

    /** The player's game code, copied from the selected folder into app-private storage. */
    private boolean haveLib() {
        return ownLib().exists();
    }

    /** Which game code this version of the app runs with (native/build_native.sh). */
    private String wantedId() {
        try (java.io.InputStream in = getAssets().open("gamelib_id.txt")) {
            byte[] b = new byte[64];
            int n = in.read(b);
            return n > 0 ? new String(b, 0, n, "UTF-8").trim() : "";
        } catch (Exception e) {
            return "";
        }
    }

    private File saved() {
        try (FileInputStream in = new FileInputStream(savedFile())) {
            byte[] b = new byte[4096];
            int n = in.read(b);
            String path = n > 0 ? new String(b, 0, n, "UTF-8").trim() : "";
            return path.isEmpty() ? null : new File(path);
        } catch (Exception e) {
            return null;
        }
    }

    private static boolean looksRight(File dir) {
        return dir != null && new File(dir, "exefs/main").canRead() && new File(dir, "romfs").isDirectory();
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        declareGame();
        language = RecompLanguage.from(this);
        change = getIntent().getBooleanExtra(EXTRA_CHANGE, false);
        pickFolder = getIntent().getBooleanExtra(EXTRA_PICK_FOLDER, false);
        if (pickFolder) {  // (FilesFolderActivity: Android's picker and the permission, then the folder back to it)
            buildPickFolder();
            return;
        }
        if (!change && !startupChecked) {  // (antes de arrancar el juego: la carpeta de archivos se pregunta una vez)
            startupChecked = true;
            if (showFilesStartup()) return;
        }
        begin();
    }

    private void begin() {
        boolean rejected = rejectedFile().exists();
        File chosen = saved(), own = new File(home(), "data");
        if (!change && !rejected && haveLib() && (looksRight(chosen) || (chosen == null && looksRight(own)))) {
            startGame();
            return;
        }
        build();
        if (rejected) {  // (runtime/main.c turned them down: say what is wrong with them)
            File bad = chosen != null ? chosen : own;
            verify(bad);
        } else if (chosen != null && !change) {
            status.setTextColor(ALERT);
            status.setText(hasAccess()
                    ? tr("The folder " + chosen + " is gone. Choose where it is now.",
                         "Ya no se encuentra la carpeta " + chosen + ". Elige dónde está ahora.",
                         "找不到文件夹 " + chosen + "。请选择它现在的位置。")
                    : tr("The game lost its permission to read the folder. Please give it again.",
                         "El juego perdió el permiso para leer la carpeta. Vuelve a darlo.",
                         "游戏已失去读取文件夹的权限，请重新授权。"));
        }
    }

    /**
     * La carpeta de archivos al arrancar: una elegida que no se puede usar se dice (elegirla otra vez, o usar la propia
     * esta vez) en vez de usar la propia en silencio; y en el primer arranque (o tras reinstalar, que borra el puntero)
     * se pregunta dónde van los archivos. true: hay un diálogo; su botón sigue con begin() o con el selector.
     */
    private boolean showFilesStartup() {
        String problem = FilesFolder.problem(this);
        if (problem != null) {
            new android.app.AlertDialog.Builder(this)
                    .setTitle(tr("Files folder", "Carpeta de archivos", "文件文件夹"))
                    .setMessage(problem + "\n\n" + tr("Choose it again (Android asks for \"All files access\" again if it's off), or use the app's own folder this time: your files in the chosen folder are then not used until you choose it again. (If it's gone for good, the settings' Menu Options tab can forget it.)",
                            "Elígela otra vez (Android vuelve a pedir \"Acceso a todos los archivos\" si está desactivado), o usa la carpeta propia de la app esta vez: tus archivos de la carpeta elegida no se usarán hasta que la elijas de nuevo. (Si desapareció del todo, la pestaña Opciones del menú puede olvidarla.)",
                            "请重新选择它（如果权限已关闭，Android 会再次请求“所有文件访问权限”），或本次使用应用自己的文件夹：在你再次选择它之前，所选文件夹中的文件不会被使用。（如果它已永久消失，可在设置中忘记它。）"))
                    .setCancelable(false)
                    .setPositiveButton(tr("Choose it again", "Elegirla otra vez", "重新选择"), (d, w) -> pickFilesFolder())
                    .setNeutralButton(tr("Use the app's folder", "Usar la carpeta de la app", "使用应用文件夹"), (d, w) -> {
                        FilesFolder.asked(this, true);
                        begin();
                    })
                    .setNegativeButton(tr("Quit", "Salir", "退出"), (d, w) -> finishAffinity())
                    .show();
            return true;
        }
        if (!FilesFolder.chosenPath(this).isEmpty() || FilesFolder.asked(this) || Build.VERSION.SDK_INT < 30)
            return false;  // (ya hay una carpeta en uso, o ya se preguntó; Android 11 en adelante: elegir necesita ese acceso)
        boolean had = new File(FilesFolder.get(this), "mhgurecomp.ini").isFile();
        String text = (had
                ? tr("The app keeps its files (settings, save, mods, cheats, screenshots, log.txt) in its own folder, in Android/data, which this device's file managers can't open. It can copy them into a folder you choose and keep them there instead.",
                     "La app guarda sus archivos (ajustes, partida, mods, trucos, capturas, log.txt) en su carpeta propia, en Android/data, que los gestores de archivos del dispositivo no pueden abrir. Puede copiarlos a una carpeta que elijas y guardarlos allí.",
                     "应用把自己的文件（设置、存档、模组、金手指、截图、log.txt）保存在 Android/data 中，本设备的文件管理器无法打开。应用可以把它们复制到你选择的文件夹中并在那里保存。")
                : tr("Where should the app keep its files (settings, save, mods, cheats, screenshots, log.txt)? Its own folder is in Android/data, which this device's file managers can't open.",
                     "¿Dónde debe guardar la app sus archivos (ajustes, partida, mods, trucos, capturas, log.txt)? Su carpeta propia está en Android/data, que los gestores de archivos del dispositivo no pueden abrir.",
                     "应用应该把文件（设置、存档、模组、金手指、截图、log.txt）保存在哪里？它自己的文件夹位于 Android/data，本设备的文件管理器无法打开。"))
                + " " + tr("In a folder you choose (a new MHGU folder in the device's storage, say) you can open them, back them up and put mods in them yourself.",
                           "En una carpeta que elijas (por ejemplo una carpeta MHGU nueva en el almacenamiento) puedes abrirlos, hacer copias y poner mods tú mismo.",
                           "在你选择的文件夹中（例如设备存储中的新 MHGU 文件夹），你可以自行打开、备份它们并放入模组。")
                + (had ? "" : " " + tr("If you chose one before reinstalling the app, choose it again: what's in it is used.",
                                       "Si elegiste una antes de reinstalar la app, elígela otra vez: se usa lo que hay en ella.",
                                       "如果重装前选过一个，请再次选择它：其中的内容会被使用。"))
                + "\n\n" + tr("You can change this later in the settings' Menu Options tab.",
                              "Puedes cambiarlo después en la pestaña Opciones del menú.",
                              "之后可在设置的“菜单选项”中更改。");
        new android.app.AlertDialog.Builder(this)
                .setTitle(tr("Files folder", "Carpeta de archivos", "文件文件夹"))
                .setMessage(text)
                .setCancelable(false)
                .setPositiveButton(tr("Choose a folder", "Elegir una carpeta", "选择文件夹"), (d, w) -> pickFilesFolder())
                .setNegativeButton(tr("The app's own folder", "La carpeta de la app", "应用自己的文件夹"), (d, w) -> {
                    FilesFolder.asked(this, true);
                    begin();
                })
                .show();
        return true;
    }

    private void pickFilesFolder() {
        Intent intent = new Intent(this, FilesFolderActivity.class);
        intent.putExtra(FilesFolderActivity.EXTRA_MODE, FilesFolderActivity.MODE_CHOOSE);
        intent.putExtra(FilesFolderActivity.EXTRA_RETURN, true);
        intent.putExtra(GameActivity.EXTRA_LANGUAGE, language);
        startActivityForResult(intent, FILES_PICK);
    }

    /** La pantalla del selector cuando solo se elige la carpeta de archivos (FilesFolderActivity). */
    private void buildPickFolder() {
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        page.setGravity(Gravity.CENTER);
        page.setPadding(dp(24), dp(16), dp(24), dp(16));
        page.setBackgroundColor(WINE);
        page.addView(text(tr("FILES FOLDER", "CARPETA DE ARCHIVOS", "文件文件夹"), 20, GOLD, true));
        TextView intro = text(tr("Choose the folder for the app's files (settings, save, mods, cheats, screenshots, log.txt). "
                        + "The app copies its files there; the game files are not copied.",
                "Elige la carpeta para los archivos de la app (ajustes, partida, mods, trucos, capturas, log.txt). La app copia "
                        + "allí sus archivos; los del juego no se copian.",
                "选择应用文件（设置、存档、模组、金手指、截图、log.txt）的文件夹。应用会把文件复制到那里；游戏文件不会被复制。"),
                14, CREAM, false);
        intro.setGravity(Gravity.CENTER);
        intro.setPadding(0, dp(10), 0, dp(10));
        page.addView(intro);
        status = text("", 14, GOLD, false);
        status.setGravity(Gravity.CENTER);
        status.setPadding(0, dp(6), 0, dp(6));
        page.addView(status);
        LinearLayout row = new LinearLayout(this);
        row.setGravity(Gravity.CENTER);
        pick = button("", true);
        pick.setOnClickListener(v -> choose());
        row.addView(pick, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, dp(46)));
        Button cancel = button(tr("Cancel", "Cancelar", "取消"), false);
        cancel.setOnClickListener(v -> finish());
        LinearLayout.LayoutParams size = new LinearLayout.LayoutParams(dp(120), dp(46));
        size.setMargins(dp(8), 0, 0, 0);
        row.addView(cancel, size);
        page.addView(row);
        setContentView(page);
        pick.requestFocus();
        refreshButton();
    }

    private void acceptPickedFolder(File dir) {
        setResult(RESULT_OK, new Intent().putExtra(FilesFolderActivity.EXTRA_PATH, dir.getAbsolutePath()));
        finish();
    }

    /** Samsung ignora la categoría del manifiesto: cuenta la que fija quien instaló la app (Android solo se lo permite a él). */
    private void declareGame() {
        try {
            getPackageManager().setApplicationCategoryHint(getPackageName(), ApplicationInfo.CATEGORY_GAME);
            android.util.Log.i("MHGU", "category hint: game");
        } catch (RuntimeException e) {
            android.util.Log.i("MHGU", "category hint: " + e.getMessage());
        }
    }

    private void startGame() {
        startActivity(new Intent(this, GameActivity.class));
        overridePendingTransition(0, 0);
        finish();
    }

    // ---- the screen
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

    private GradientDrawable plate(int fill, int r) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(fill);
        d.setStroke(dp(2), EDGE);
        d.setCornerRadius(dp(r));
        return d;
    }

    private Button button(String s, boolean main) {
        Button b = new Button(this);
        b.setText(s);
        b.setAllCaps(false);
        b.setTextColor(main ? GOLD : CREAM);
        b.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        if (main) b.setTypeface(Typeface.DEFAULT_BOLD);
        b.setBackground(plate(BRONZE, 6));
        b.setPadding(dp(18), 0, dp(18), 0);
        return b;
    }

    /** The folder as it must look: folders in gold, what goes in them in cream. */
    private CharSequence tree() {
        String[] lines = {
            tr("Your folder", "Tu carpeta", "游戏文件夹"),
            "├─ exefs",
            "│    main",
            "│    main.npdm",
            "│    rtld, sdk, subsdk0",
            "└─ romfs",
            "     nativeNX",
            tr("     ... 35,500 files", "     ... 35.500 archivos", "     ... 约 35,500 个文件"),
        };
        SpannableStringBuilder b = new SpannableStringBuilder();
        for (int i = 0; i < lines.length; i++) {
            int at = b.length();
            b.append(lines[i]).append(i + 1 < lines.length ? "\n" : "");
            boolean folder = i == 0 || i == 1 || i == 5;
            b.setSpan(new ForegroundColorSpan(folder ? GOLD : CREAM), at, b.length(), Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
        }
        return b;
    }

    private void build() {
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        int pad = dp(14);
        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        page.setPadding(pad * 2, pad, pad * 2, pad);

        TextView title = text(tr("GAME FILES", "ARCHIVOS DEL JUEGO", "游戏文件"), 20, GOLD, true);
        title.setGravity(Gravity.CENTER);
        page.addView(title);
        TextView intro = text(tr("Choose the folder that holds your copy of the game. It is read from there: nothing is copied.",
                                 "Elige la carpeta donde tienes tu copia del juego. Se lee desde ahí: no se copia nada.",
                                 "请选择存放游戏副本的文件夹，游戏将直接读取其中的文件，无需复制。"),
                14, CREAM, false);
        intro.setGravity(Gravity.CENTER);
        intro.setPadding(0, dp(4), 0, dp(10));
        page.addView(intro);

        LinearLayout columns = new LinearLayout(this);
        columns.setOrientation(LinearLayout.HORIZONTAL);

        LinearLayout left = new LinearLayout(this);
        left.setOrientation(LinearLayout.VERTICAL);
        left.setBackground(plate(PANEL, 6));
        left.setPadding(pad, dp(10), pad, dp(10));
        left.addView(text(tr("THE FOLDER MUST LOOK LIKE THIS", "LA CARPETA DEBE TENER ESTO", "文件夹应包含以下内容"), 13, GOLD, true));
        TextView tree = new TextView(this);
        tree.setText(tree());
        tree.setTypeface(Typeface.MONOSPACE);
        tree.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        tree.setPadding(0, dp(6), 0, 0);
        left.addView(tree);
        columns.addView(left, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));

        LinearLayout right = new LinearLayout(this);
        right.setOrientation(LinearLayout.VERTICAL);
        right.setPadding(dp(18), 0, 0, 0);
        right.addView(text(tr("WHAT EACH ONE MUST BE", "QUÉ TIENE QUE SER CADA UNA", "文件要求"), 13, GOLD, true));
        TextView notes = text(tr(
                  "• exefs: the one from the 1.4.0 update.\n"
                  + "• romfs: the base game's, with the 1.4.0 update already applied over it.\n"
                  + "• Also provide compatible libmhgugame.so and libmhgugame.id files in that folder.\n"
                  + "• Keep exefs and romfs in the same folder, about 12 GB. Choose that folder.",
                  "• exefs: el de la actualización 1.4.0.\n"
                  + "• romfs: el del juego base, con la actualización 1.4.0 ya aplicada encima.\n"
                  + "• Incluye también libmhgugame.so y libmhgugame.id compatibles en esa carpeta.\n"
                  + "• exefs y romfs juntos en la misma carpeta, unos 12 GB. Elige esa carpeta.",
                  "• exefs：使用 1.4.0 更新中的文件。\n"
                  + "• romfs：使用已合并 1.4.0 更新的游戏本体文件。\n"
                  + "• 同一文件夹还需放入与此应用版本匹配的 libmhgugame.so 和 libmhgugame.id。\n"
                  + "• 将 exefs 和 romfs 放在同一文件夹内，约 12 GB，然后选择该文件夹。"), 14, CREAM, false);
        notes.setPadding(0, dp(6), 0, 0);
        notes.setLineSpacing(dp(2), 1);
        right.addView(notes);
        columns.addView(right, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
        page.addView(columns);

        // (the page scrolls on a short or square screen; the message and the buttons stay in view)
        ScrollView scroll = new ScrollView(this);
        scroll.addView(page);
        LinearLayout whole = new LinearLayout(this);
        whole.setOrientation(LinearLayout.VERTICAL);
        whole.setBackgroundColor(WINE);
        whole.addView(scroll, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1));

        status = text("", 14, GOLD, false);
        status.setGravity(Gravity.CENTER);
        status.setPadding(pad * 2, dp(6), pad * 2, dp(6));
        whole.addView(status);

        LinearLayout row = new LinearLayout(this);
        row.setGravity(Gravity.CENTER);
        pick = button("", true);
        pick.setOnClickListener(v -> choose());
        LinearLayout.LayoutParams size = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, dp(46));
        size.setMargins(dp(8), 0, dp(8), 0);
        row.addView(pick, size);
        Button leave = button(change ? tr("Cancel", "Cancelar", "取消") : tr("Exit", "Salir", "退出"), false);
        leave.setOnClickListener(v -> finish());
        LinearLayout.LayoutParams size2 = new LinearLayout.LayoutParams(dp(120), dp(46));
        size2.setMargins(dp(8), 0, dp(8), 0);
        row.addView(leave, size2);
        row.setPadding(0, 0, 0, dp(10));
        whole.addView(row);
        whole.setOnApplyWindowInsetsListener((v, insets) -> {  // (the buttons stay above the system's bar)
            v.setPadding(insets.getSystemWindowInsetLeft(), insets.getSystemWindowInsetTop(), insets.getSystemWindowInsetRight(),
                    insets.getSystemWindowInsetBottom());
            return insets;
        });
        setContentView(whole);
        pick.requestFocus();  // (a console's pad: A presses it)
        refreshButton();
    }

    private void refreshButton() {
        if (pick != null) pick.setText(hasAccess() ? tr("Choose folder", "Elegir carpeta", "选择文件夹")
                                                   : tr("Give permission and choose folder", "Dar permiso y elegir carpeta", "授权并选择文件夹"));
    }

    // ---- reading any folder: the plain file paths the game uses need this permission
    private boolean hasAccess() {
        if (Build.VERSION.SDK_INT >= 30) return Environment.isExternalStorageManager();
        return checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED;
    }

    private void choose() {
        if (hasAccess()) { openPicker(); return; }
        if (Build.VERSION.SDK_INT < 30) {
            requestPermissions(new String[] {Manifest.permission.READ_EXTERNAL_STORAGE, Manifest.permission.WRITE_EXTERNAL_STORAGE}, READ);
            return;
        }
        waitingAccess = true;
        status.setTextColor(GOLD);
        status.setText(pickFolder
                ? tr("Turn on access to all files and come back. The app copies its files into the folder you choose.",
                     "Activa el acceso a todos los archivos y vuelve. La app copia sus archivos en la carpeta que elijas.",
                     "请开启“所有文件访问权限”后返回。应用会把文件复制到你选择的文件夹。")
                : tr("Turn on access to all files and come back. The game only reads that folder: it changes nothing.",
                     "Activa el acceso a todos los archivos y vuelve. El juego solo lee esa carpeta: no cambia nada.",
                     "请开启“所有文件访问权限”后返回。游戏只读取该文件夹，不会修改其中的内容。"));
        try {
            startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, Uri.parse("package:" + getPackageName())));
        } catch (ActivityNotFoundException e) {
            startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        refreshButton();
        if (waitingAccess && hasAccess()) {
            waitingAccess = false;
            status.setText("");
            openPicker();
        }
    }

    @Override
    public void onRequestPermissionsResult(int code, String[] permissions, int[] results) {
        if (code == READ && hasAccess()) openPicker();
        refreshButton();
    }

    private void openPicker() {
        openBrowser(null);
    }

    /** Android's own folder picker: the browser's second way (some show nothing at the root, or refuse it). */
    private void systemPicker() {
        try {
            startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), PICK);
        } catch (ActivityNotFoundException e) {
            status.setTextColor(ALERT);
            status.setText(tr("This device has no folder picker.", "Este dispositivo no tiene selector de carpetas.", "此设备没有文件夹选择器。"));
        }
    }

    // ---- the app's own folder browser. Android's picker (HyperOS on a POCO F6, seen twice) showed the storage's root
    // empty and refused it; with access to all files the folders can be listed here, in the game's style
    private android.app.Dialog browser;

    /** The storage roots: the phone's own and each SD card's (from the app's folders on them). */
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

    private boolean holdsGame(File d) {
        return new File(d, "exefs").isDirectory() && new File(d, "romfs").isDirectory();
    }

    private void openBrowser(File at) {
        if (browser != null) browser.dismiss();
        List<File> roots = roots();
        int pad = dp(12);
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setBackground(plate(PANEL, 8));
        box.setPadding(pad, pad, pad, pad);
        TextView title = text(tr("CHOOSE THE GAME'S FOLDER", "ELIGE LA CARPETA DEL JUEGO", "选择游戏文件夹"), 17, GOLD, true);
        title.setGravity(Gravity.CENTER);
        box.addView(title);
        // the roots, as tabs
        LinearLayout tabs = new LinearLayout(this);
        tabs.setGravity(Gravity.CENTER);
        for (int i = 0; i < roots.size(); i++) {
            File r = roots.get(i);
            boolean on = at != null ? at.getAbsolutePath().startsWith(r.getAbsolutePath()) : i == 0;
            Button t = button(i == 0 ? tr("Phone storage", "Memoria interna", "内部存储")
                                     : tr("SD card", "Tarjeta SD", "SD 卡") + (roots.size() > 2 ? " " + i : ""), on);
            t.setOnClickListener(v -> openBrowser(r));
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, dp(40));
            lp.setMargins(dp(4), dp(6), dp(4), dp(4));
            tabs.addView(t, lp);
        }
        box.addView(tabs);
        File dir = at != null ? at : roots.get(0);
        File root = roots.get(0);
        for (File r : roots) if (dir.getAbsolutePath().startsWith(r.getAbsolutePath())) root = r;
        String shown = dir.getAbsolutePath().substring(root.getAbsolutePath().length());
        TextView where = text((shown.isEmpty() ? "/" : shown), 13, CREAM, false);
        where.setPadding(dp(4), dp(2), dp(4), dp(4));
        box.addView(where);
        boolean game = holdsGame(dir);
        TextView hint = text(game ? tr("✓ The game is here: exefs and romfs.", "✓ Aquí está el juego: exefs y romfs.", "✓ 已找到游戏文件：exefs 和 romfs。")
                                  : tr("Go into the folder that holds exefs and romfs.", "Entra a la carpeta que tiene exefs y romfs.", "请进入包含 exefs 和 romfs 的文件夹。"),
                13, game ? GOLD : CREAM, game);
        hint.setPadding(dp(4), 0, dp(4), dp(6));
        box.addView(hint);
        // the folders here
        LinearLayout list = new LinearLayout(this);
        list.setOrientation(LinearLayout.VERTICAL);
        final File here = dir, top = root;
        if (!dir.equals(root)) {
            Button up = button("⬑  " + tr("Up", "Subir", "上一级"), false);
            up.setGravity(Gravity.START | Gravity.CENTER_VERTICAL);
            up.setOnClickListener(v -> openBrowser(here.getParentFile()));
            list.addView(up, rowParams());
        }
        File[] kids = dir.listFiles(f -> f.isDirectory() && !f.getName().startsWith("."));
        if (kids != null) {
            Arrays.sort(kids, (a, b) -> a.getName().compareToIgnoreCase(b.getName()));
            for (File k : kids) {
                boolean has = holdsGame(k);
                Button b = button((has ? "✓  " : "▸  ") + k.getName(), has);
                b.setGravity(Gravity.START | Gravity.CENTER_VERTICAL);
                b.setOnClickListener(v -> openBrowser(k));
                list.addView(b, rowParams());
            }
        }
        if (kids == null || kids.length == 0) {
            TextView none = text(tr("(no folders)", "(no hay carpetas)", "（没有文件夹）"), 13, CREAM, false);
            none.setPadding(dp(6), dp(8), dp(6), dp(8));
            list.addView(none);
        }
        ScrollView scroll = new ScrollView(this);
        scroll.addView(list);
        box.addView(scroll, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1));
        // use it, Android's picker, cancel
        LinearLayout row = new LinearLayout(this);
        row.setGravity(Gravity.CENTER);
        row.setPadding(0, dp(8), 0, 0);
        Button use = button(tr("Use this folder", "Usar esta carpeta", "使用此文件夹"), true);
        use.setOnClickListener(v -> { browser.dismiss(); verify(here); });
        Button other = button(tr("Android's picker", "Selector de Android", "系统选择器"), false);
        other.setOnClickListener(v -> { browser.dismiss(); systemPicker(); });
        Button cancel = button(tr("Cancel", "Cancelar", "取消"), false);
        cancel.setOnClickListener(v -> browser.dismiss());
        for (Button b : new Button[] {use, other, cancel}) {
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, dp(44));
            lp.setMargins(dp(5), 0, dp(5), 0);
            row.addView(b, lp);
        }
        box.addView(row);
        browser = new android.app.Dialog(this);
        browser.requestWindowFeature(android.view.Window.FEATURE_NO_TITLE);
        browser.setContentView(box);
        android.view.Window w = browser.getWindow();
        if (w != null) {
            w.setBackgroundDrawable(new android.graphics.drawable.ColorDrawable(Color.TRANSPARENT));
            android.util.DisplayMetrics m = getResources().getDisplayMetrics();
            w.setLayout(Math.min(dp(640), (int) (m.widthPixels * .92f)), (int) (m.heightPixels * .92f));
        }
        browser.show();
        use.requestFocus();
    }

    private LinearLayout.LayoutParams rowParams() {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, dp(42));
        lp.setMargins(0, dp(3), 0, dp(3));
        return lp;
    }

    @Override
    protected void onActivityResult(int code, int result, Intent data) {
        super.onActivityResult(code, result, data);
        if (code == FILES_PICK) {
            if (result == RESULT_OK) {
                FilesFolder.asked(this, true);
                begin();
            } else if (!showFilesStartup()) {
                begin();  // (cancelado: se vuelve a preguntar, como el primer arranque)
            }
            return;
        }
        if (code != PICK || result != RESULT_OK || data == null || data.getData() == null) return;
        File dir = pathOf(data.getData());
        if (dir == null) {
            status.setTextColor(ALERT);
            status.setText(tr("That location can't be used. Find the folder in the phone's storage or on the SD card.",
                              "No se puede usar esa ubicación. Busca la carpeta en la memoria del teléfono o en la tarjeta SD.",
                              "无法使用此位置。请从内部存储或 SD 卡中选择文件夹。"));
            return;
        }
        verify(dir);
    }

    /** A picked folder as a plain path (null: a provider with no real folder behind it). */
    private File pathOf(Uri tree) {
        try {
            String id = DocumentsContract.getTreeDocumentId(tree);
            String authority = tree.getAuthority();
            if ("com.android.providers.downloads.documents".equals(authority) && id.startsWith("raw:"))
                return new File(id.substring(4));
            if (!"com.android.externalstorage.documents".equals(authority)) return null;
            int colon = id.indexOf(':');
            if (colon < 0) return null;
            String volume = id.substring(0, colon), rest = id.substring(colon + 1);
            File base = "primary".equalsIgnoreCase(volume) ? Environment.getExternalStorageDirectory() : new File("/storage", volume);
            return rest.isEmpty() ? base : new File(base, rest);
        } catch (Exception e) {
            return null;
        }
    }

    // ---- is it the right thing? (a background thread: the walk over romfs takes seconds)
    private void verify(File picked) {
        if (pickFolder) { acceptPickedFolder(picked); return; }  // (no es el juego: solo se elige la carpeta de archivos)
        pick.setEnabled(false);
        status.setTextColor(GOLD);
        status.setText(tr("Checking the files...", "Comprobando los archivos...", "正在检查游戏文件……"));
        new Thread(() -> {
            File[] ways = {picked, new File(picked, "data"), picked.getParentFile()};  // (or one level off)
            // (the game code changed folders or versions: what is kept goes with the new choice)
            File dir = picked;
            for (File w : ways)
                if (w != null && new File(w, "exefs").isDirectory() && new File(w, "romfs").isDirectory()) { dir = w; break; }
            final File found = dir;
            String wrong = problem(found);
            if (wrong == null) {
                try {
                    keepLib(found);
                } catch (Exception e) {
                    wrong = tr("Could not prepare the game code.", "No se pudo preparar el código del juego.", "无法准备游戏代码。") + "\n" + e;
                }
            }
            final String problem = wrong;
            runOnUiThread(() -> {
                pick.setEnabled(true);
                if (problem != null) {
                    status.setTextColor(ALERT);
                    status.setText(problem);
                } else {
                    accept(found);
                }
            });
        }).start();
    }

    private String problem(File dir) {
        File exefs = new File(dir, "exefs"), romfs = new File(dir, "romfs"), main = new File(exefs, "main");
        if (!exefs.isDirectory() && !romfs.isDirectory())
            return tr("exefs and romfs are not in " + dir.getName() + ". Choose the folder that holds them.",
                      "En " + dir.getName() + " no están exefs ni romfs. Elige la carpeta que las contiene.",
                      dir.getName() + " 中没有 exefs 和 romfs，请选择包含这两个目录的文件夹。");
        if (!exefs.isDirectory()) return tr("The exefs folder is missing, beside romfs.", "Falta la carpeta exefs, al lado de romfs.", "缺少 exefs 文件夹，请将其与 romfs 放在同一目录。");
        if (!romfs.isDirectory()) return tr("The romfs folder is missing, beside exefs.", "Falta la carpeta romfs, al lado de exefs.", "缺少 romfs 文件夹，请将其与 exefs 放在同一目录。");
        if (!main.canRead()) return tr("The file main is missing from exefs.", "En exefs falta el archivo main.", "exefs 中缺少 main 文件，或无法读取该文件。");
        byte[] head = new byte[0x48];
        try (FileInputStream in = new FileInputStream(main)) {
            if (in.read(head) != head.length) head = new byte[0x48];
        } catch (Exception e) {
            return tr("exefs/main could not be read.", "No se pudo leer exefs/main.", "无法读取 exefs/main。");
        }
        boolean nso = head[0] == 'N' && head[1] == 'S' && head[2] == 'O' && head[3] == '0';
        if (!nso || !Arrays.equals(Arrays.copyOfRange(head, 0x40, 0x48), BUILD_ID))
            return tr("This exefs is not version 1.4.0. Use the one from the 1.4.0 update.",
                      "Este exefs no es de la versión 1.4.0. Usa el de la actualización 1.4.0.",
                      "此 exefs 不是 1.4.0 版本，请使用 1.4.0 更新中的文件。");
        int files = countFiles(romfs);
        if (files < MIN_FILES)
            return tr("romfs is incomplete: it has " + files + " files where there should be 35,531. Use the base game with the 1.4.0 update applied.",
                      "romfs está incompleto: tiene " + files + " archivos y deberían ser 35.531. Usa el juego base con la actualización 1.4.0 ya aplicada.",
                      "romfs 不完整：当前有 " + files + " 个文件，完整版本应有 35,531 个。请使用已合并 1.4.0 更新的游戏本体文件。");
        File lib = new File(dir, "libmhgugame.so");
        if (!lib.canRead())
            return tr("libmhgugame.so is missing from the selected folder.",
                      "Falta libmhgugame.so en la carpeta elegida.",
                      "所选文件夹中缺少 libmhgugame.so。");
        if (!wantedId().equals(idBeside(dir)))
            return tr("The game code does not match this app version. Rebuild it for this version and select the folder again.",
                      "El código del juego no corresponde a esta versión. Genéralo para esta versión y vuelve a elegir la carpeta.",
                      "游戏代码与当前应用版本不匹配，请按此版本重新生成后再选择文件夹。");
        if (!new File(romfs, "nativeNX/system/app_shader/AppShaderPackage.mfx").exists())
            return tr("nativeNX/system/app_shader/AppShaderPackage.mfx is missing from romfs. Use the base game with the 1.4.0 update applied.",
                      "En romfs falta nativeNX/system/app_shader/AppShaderPackage.mfx. Usa el juego base con la actualización 1.4.0 ya aplicada.",
                      "romfs 中缺少 nativeNX/system/app_shader/AppShaderPackage.mfx。请使用已合并 1.4.0 更新的游戏本体文件。");
        return null;
    }

    private String idBeside(File dir) {
        try (FileInputStream in = new FileInputStream(new File(dir, "libmhgugame.id"))) {
            byte[] b = new byte[64];
            int n = in.read(b);
            return n > 0 ? new String(b, 0, n, "UTF-8").trim() : "";
        } catch (Exception e) {
            return "";
        }
    }

    /** The game code into the app's private folder, read-only: Android runs nothing from shared storage. */
    private void keepLib(File dir) throws java.io.IOException {
        File from = new File(dir, "libmhgugame.so");
        if (!from.canRead()) return;
        File to = ownLib(), tmp = new File(to.getParentFile(), "libmhgugame.tmp");
        to.getParentFile().mkdirs();
        tmp.delete();
        try (FileInputStream in = new FileInputStream(from); FileOutputStream out = new FileOutputStream(tmp)) {
            byte[] buf = new byte[1 << 20];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        }
        to.delete();
        if (!tmp.renameTo(to)) throw new java.io.IOException("libmhgugame.so");
        to.setReadOnly();
    }

    private int countFiles(File top) {
        int n = 0;
        ArrayDeque<File> dirs = new ArrayDeque<>();
        dirs.add(top);
        while (!dirs.isEmpty()) {
            File[] list = dirs.poll().listFiles();
            if (list == null) continue;
            for (File f : list) {
                if (f.isDirectory()) dirs.add(f);
                else n++;
            }
        }
        return n;
    }

    private void accept(File dir) {
        try (FileOutputStream out = new FileOutputStream(savedFile())) {
            out.write(dir.getAbsolutePath().getBytes("UTF-8"));
        } catch (Exception e) {
            status.setTextColor(ALERT);
            status.setText(tr("Could not save the selected folder.", "No se pudo guardar la carpeta elegida.", "无法保存所选文件夹。") + "\n" + e);
            return;
        }
        rejectedFile().delete();
        if (change) {
            status.setTextColor(GOLD);
            status.setText(tr("Done: it will be used the next time you open the game.", "Listo: se usará la próxima vez que abras el juego.", "已保存，下次启动游戏时生效。"));
            pick.setEnabled(false);
            return;
        }
        startGame();
    }
}
