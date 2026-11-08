package com.mhgu.recomp;

import android.content.Context;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.FileWriter;
import java.io.IOException;

/**
 * La carpeta de archivos de la app: donde están los ajustes, la partida, los mods, los trucos, las capturas y log.txt.
 * Es la carpeta propia de la app (Android/data/&lt;paquete&gt;/files, que los gestores de archivos del dispositivo no
 * pueden abrir desde Android 11) salvo que el jugador elija otra en Opciones del menú: su ruta queda en
 * files-folder.txt, en la carpeta propia, y se usa mientras la app pueda escribir en ella ("Acceso a todos los
 * archivos"). runtime/platform/android.c (android_home) lee lo mismo.
 */
final class FilesFolder {
    static final String POINTER = "files-folder.txt";
    static final String ASKED = "files-folder-asked";

    /** La carpeta propia de la app: siempre está, y guarda el puntero y los archivos del juego. */
    static File app(Context c) {
        File f = c.getExternalFilesDir(null);
        return f != null ? f : c.getFilesDir();
    }

    /** La carpeta elegida, tal como quedó anotada ("" si no hay ninguna). */
    static String chosenPath(Context c) {
        try (BufferedReader r = new BufferedReader(new FileReader(new File(app(c), POINTER)))) {
            String line = r.readLine();
            if (line != null && line.trim().startsWith("/")) return line.trim();
        } catch (IOException e) {
            // (ninguna elegida)
        }
        return "";
    }

    /** La carpeta en uso: la elegida mientras se pueda escribir en ella, si no la propia de la app. */
    static File get(Context c) {
        String path = chosenPath(c);
        if (!path.isEmpty()) {
            File dir = new File(path);
            if (dir.isDirectory() && dir.canWrite()) return dir;
        }
        return app(c);
    }

    /** Por qué no se usa ahora una carpeta elegida, o null. */
    static String problem(Context c) {
        String path = chosenPath(c);
        if (path.isEmpty()) return null;
        File dir = new File(path);
        if (dir.isDirectory() && dir.canWrite()) return null;
        return "The app's files folder, " + path + ", can't be used now: \"All files access\" turned off, or the folder "
                + "moved or deleted, or its drive or SD card gone?";
    }

    /** Anota la carpeta en uso en la carpeta propia (borra el puntero si es la propia). */
    static void note(Context c, File dir) throws IOException {
        File own = app(c);
        File pointer = new File(own, POINTER);
        if (dir.getCanonicalPath().equals(own.getCanonicalPath())) {
            if (pointer.exists() && !pointer.delete()) throw new IOException("can't delete " + pointer);
        } else {
            File part = new File(pointer.getPath() + ".part");
            try (FileWriter w = new FileWriter(part)) {
                w.write(dir.getAbsolutePath() + "\n");
            }
            if (!part.renameTo(pointer)) {
                part.delete();
                throw new IOException("can't write " + pointer);
            }
        }
    }

    static boolean asked(Context c) {
        return new File(app(c), ASKED).exists();
    }

    static void asked(Context c, boolean yes) {
        File marker = new File(app(c), ASKED);
        if (!yes) {
            marker.delete();
            return;
        }
        if (marker.exists()) return;
        try {
            marker.createNewFile();
        } catch (IOException e) {
            // (it will be asked again next start: harmless)
        }
    }

    private FilesFolder() {}
}
