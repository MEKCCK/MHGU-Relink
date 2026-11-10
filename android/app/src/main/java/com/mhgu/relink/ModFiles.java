package com.mhgu.relink;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.Enumeration;
import java.util.LinkedHashSet;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import com.github.junrar.Archive;
import com.github.junrar.ArchiveOptions;
import com.github.junrar.rarfile.BaseBlock;
import com.github.junrar.rarfile.FileHeader;
import com.github.junrar.rarfile.HostSystem;
import com.github.junrar.rarfile.rar5.Rar5BaseBlock;
import com.github.junrar.rarfile.rar5.Rar5MainHeader;

/** Mod package handling, independent of Android so the import transaction can be tested. */
final class ModFiles {
    static final int MAX_DEPTH = 8, MAX_SEARCH = 32768;
    enum Reason { FORMAT, NO_PAYLOAD, SEARCH_LIMIT, UNSAFE_PATH, SOURCE_TARGET, UNSUPPORTED_RAR }
    static final class Problem extends IOException {
        final Reason reason;
        Problem(Reason reason) { super(reason.name()); this.reason = reason; }
    }
    interface Copier { void copy(InputStream in, OutputStream out) throws IOException; }

    private ModFiles() {}

    static boolean accepts(String name) {
        String n = name.toLowerCase(Locale.ROOT);
        return n.endsWith(".zip") || n.endsWith(".rar") || n.endsWith(".nro") || patch(n);
    }

    private static boolean patch(String n) {
        return n.endsWith(".ips") || n.endsWith(".ips32") || n.endsWith(".pchtxt");
    }

    private static boolean part(String name) {
        return name.equalsIgnoreCase("romfs") || name.equalsIgnoreCase("exefs")
                || name.equalsIgnoreCase("nativeNX");
    }

    private static boolean hidden(String name) {
        return name.startsWith(".") || name.equalsIgnoreCase("__MACOSX");
    }

    // Reject traversal rather than silently importing only the safe portion of a broken archive.
    private static String archivePath(String raw) throws IOException {
        String path = raw.replace('\\', '/');
        if (path.startsWith("/") || path.indexOf(':') >= 0 || path.indexOf('\0') >= 0)
            throw new Problem(Reason.UNSAFE_PATH);
        StringBuilder clean = new StringBuilder();
        for (String segment : path.split("/")) {
            if (segment.equals("..")) throw new Problem(Reason.UNSAFE_PATH);
            if (segment.isEmpty() || segment.equals(".")) continue;
            if (clean.length() > 0) clean.append('/');
            clean.append(segment);
        }
        return clean.toString();
    }

    /** Relative package roots. Multiple roots are alternatives for the player to choose, never merged. */
    static List<String> roots(File source) throws IOException {
        if (source.isDirectory()) {
            if (part(source.getName())) return Collections.singletonList("");
            List<String> roots = new ArrayList<>();
            List<File> queue = new ArrayList<>();
            List<Integer> levels = new ArrayList<>();
            queue.add(source); levels.add(0);
            int scanned = 0;
            String base = source.getCanonicalPath();
            for (int head = 0; head < queue.size(); head++) {
                File dir = queue.get(head);
                File[] kids = dir.listFiles();
                if (kids == null) throw new IOException("Cannot read " + dir.getName());
                if (scanned + kids.length > MAX_SEARCH) throw new Problem(Reason.SEARCH_LIMIT);
                scanned += kids.length;
                Arrays.sort(kids);
                for (File kid : kids)
                    if (!hidden(kid.getName()) && Files.isSymbolicLink(kid.toPath()))
                        throw new Problem(Reason.UNSAFE_PATH);
                boolean payload = false;
                for (File kid : kids) if (kid.isDirectory() && part(kid.getName())) payload = true;
                if (payload) {
                    roots.add(relative(base, dir.getCanonicalPath()));
                    continue;
                }
                if (levels.get(head) + 1 >= MAX_DEPTH) continue;
                for (File kid : kids) if (kid.isDirectory() && !hidden(kid.getName())) {
                    if (!within(base, kid.getCanonicalPath())) throw new Problem(Reason.UNSAFE_PATH);
                    queue.add(kid); levels.add(levels.get(head) + 1);
                }
            }
            if (roots.isEmpty()) throw new Problem(Reason.NO_PAYLOAD);
            return roots;
        }
        if (!source.isFile() || !accepts(source.getName())) throw new Problem(Reason.FORMAT);
        String ext = extension(source.getName());
        if (ext.equals(".rar")) return rarRoots(source);
        if (!ext.equals(".zip"))
            return Collections.singletonList("");
        Set<String> found = new LinkedHashSet<>();
        int scanned = 0;
        try (ZipFile zip = new ZipFile(source)) {
            for (Enumeration<? extends ZipEntry> all = zip.entries(); all.hasMoreElements(); ) {
                if (++scanned > MAX_SEARCH) throw new Problem(Reason.SEARCH_LIMIT);
                ZipEntry entry = all.nextElement();
                String path = archivePath(entry.getName());
                String[] names = path.split("/");
                String parent = "";
                int dirs = entry.isDirectory() ? names.length : names.length - 1;
                for (int i = 0; i < dirs && i < MAX_DEPTH; i++) {
                    if (hidden(names[i])) break;
                    if (part(names[i])) { found.add(parent); break; }
                    parent = parent.isEmpty() ? names[i] : parent + "/" + names[i];
                }
            }
        }
        List<String> roots = new ArrayList<>(found);
        return normalizedRoots(found);
    }

    private static boolean within(String parent, String child) {
        return child.equals(parent) || child.startsWith(parent + File.separator);
    }

    private static String relative(String parent, String child) {
        return child.equals(parent) ? "" : child.substring(parent.length() + 1).replace(File.separatorChar, '/');
    }

    static String extension(String name) {
        int dot = name.lastIndexOf('.');
        return dot < 0 ? "" : name.substring(dot).toLowerCase(Locale.ROOT);
    }

    private static List<String> rarRoots(File rar) throws IOException {
        Set<String> found = new LinkedHashSet<>();
        try (Archive archive = openRar(rar)) {
            List<FileHeader> entries = archive.getFileHeaders();
            if (archive.hasBrokenHeaders()) throw new Problem(Reason.UNSUPPORTED_RAR);
            if (entries.size() > MAX_SEARCH) throw new Problem(Reason.SEARCH_LIMIT);
            validateRar(archive, entries);
            for (FileHeader entry : entries) {
                String path = archivePath(entry.getFileName());
                String[] segments = path.split("/");
                int limit = entry.isDirectory() ? segments.length : segments.length - 1;
                String parent = "";
                for (int i = 0; i < limit && i < MAX_DEPTH; i++) {
                    if (hidden(segments[i])) break;
                    if (part(segments[i])) { found.add(parent); break; }
                    parent = parent.isEmpty() ? segments[i] : parent + "/" + segments[i];
                }
            }
        } catch (Problem e) { throw e; }
        catch (Exception e) { throw new IOException("Cannot read RAR archive", e); }
        return normalizedRoots(found);
    }

    private static Archive openRar(File rar) throws Exception {
        Archive archive = new Archive(rar, ArchiveOptions.builder().maxDictionarySize(256L << 20).build());
        try {
            if (archive.getFormat() == null || archive.isPasswordProtected())
                throw new Problem(Reason.UNSUPPORTED_RAR);
            if (archive.getMainHeader() != null && archive.getMainHeader().isMultiVolume())
                throw new Problem(Reason.UNSUPPORTED_RAR);
            for (BaseBlock block : archive.getHeaders()) {
                if (block instanceof Rar5MainHeader && ((Rar5MainHeader) block).isVolume())
                    throw new Problem(Reason.UNSUPPORTED_RAR);
                if (block instanceof Rar5BaseBlock && ((Rar5BaseBlock) block).isSplitAfter())
                    throw new Problem(Reason.UNSUPPORTED_RAR);
            }
            return archive;
        } catch (Exception e) {
            archive.close();
            throw e;
        }
    }

    private static void validateRar(Archive archive, List<FileHeader> entries) throws IOException {
        for (FileHeader entry : entries) {
            if (entry.isEncrypted() || entry.isSplitBefore() || entry.isSplitAfter()
                    || entry.getRedirection() != null || entry.isUnpSizeUnknown())
                throw new Problem(Reason.UNSUPPORTED_RAR);
            if ((entry.getFileAttr() & 0x400) != 0) throw new Problem(Reason.UNSUPPORTED_RAR);
            String path = archivePath(entry.getFileName());
            if (path.isEmpty()) throw new Problem(Reason.UNSAFE_PATH);
            if (entry.getHostOS() == HostSystem.unix) {
                int type = entry.getFileAttr() & 0170000;
                if (type != 0 && type != 0100000 && type != 0040000)
                    throw new Problem(Reason.UNSUPPORTED_RAR);
                if ((type == 0040000) != entry.isDirectory())
                    throw new Problem(Reason.UNSUPPORTED_RAR);
            }
        }
    }

    /** Keep Unicode names within ModEntry.name's 127 UTF-8 bytes. Hash changes to avoid name collisions. */
    static String safeName(String original) {
        String clean = original.replaceAll("[\\\\/:*?\"<>|\\p{Cntrl}]", "_").trim();
        clean = clean.replaceAll("[. ]+$", "");
        if (clean.isEmpty()) clean = "mod";
        if (clean.startsWith(".") || clean.startsWith("_")) clean = "mod-" + clean;
        boolean altered = !clean.equals(original) || clean.getBytes(StandardCharsets.UTF_8).length > 127;
        String suffix = altered ? "-" + hash(original) : "";
        int remaining = 127 - suffix.length();
        StringBuilder out = new StringBuilder();
        for (int offset = 0; offset < clean.length(); ) {
            int cp = clean.codePointAt(offset);
            String ch = new String(Character.toChars(cp));
            int bytes = ch.getBytes(StandardCharsets.UTF_8).length;
            if (bytes > remaining) break;
            out.append(ch); remaining -= bytes; offset += Character.charCount(cp);
        }
        return out + suffix;
    }

    private static String hash(String text) {
        try {
            byte[] bytes = MessageDigest.getInstance("SHA-256").digest(text.getBytes(StandardCharsets.UTF_8));
            StringBuilder out = new StringBuilder();
            for (int i = 0; i < 6; i++) out.append(String.format(Locale.ROOT, "%02x", bytes[i] & 255));
            return out.toString();
        } catch (NoSuchAlgorithmException e) { throw new AssertionError(e); }
    }

    static String install(File source, String selected, File mods, Copier copier) throws IOException {
        if (!roots(source).contains(selected)) throw new Problem(Reason.NO_PAYLOAD);
        String ext = extension(source.getName());
        String base = source.isFile() && !ext.equals(".zip") && !ext.equals(".rar")
                ? source.getName() : source.getName().replaceAll("(?i)\\.(zip|rar)$", "");
        if (!selected.isEmpty()) base = selected.substring(selected.lastIndexOf('/') + 1);
        else if (source.isDirectory() && part(source.getName()) && source.getParentFile() != null)
            base = source.getParentFile().getName() + "-" + source.getName();
        String name = safeName(base);
        File destination = new File(mods, name);
        String src = source.getCanonicalPath(), dst = destination.getCanonicalPath();
        if (within(src, dst) || within(dst, src)) throw new Problem(Reason.SOURCE_TARGET);
        if (!mods.isDirectory() && !mods.mkdirs()) throw new IOException("Cannot create mods folder");
        File stage = File.createTempFile(".import-", "", mods), backup = null;
        if (!stage.delete() || !stage.mkdir()) throw new IOException("Cannot create import folder");
        try {
            if (source.isDirectory()) {
                File root = selected.isEmpty() ? source : new File(source, selected);
                if (!within(src, root.getCanonicalPath())) throw new Problem(Reason.UNSAFE_PATH);
                File out = part(root.getName()) ? new File(stage, root.getName()) : stage;
                copyTree(root, out, root.getCanonicalPath(), copier);
            } else if (ext.equals(".zip")) {
                unpackZip(source, selected, stage, copier);
            } else if (ext.equals(".rar")) {
                unpackRar(source, selected, stage, copier);
            } else {
                String path = ext.equals(".nro")
                        ? "romfs/nativeNX/plugins/" : "exefs/";
                copyFile(source, new File(stage, path + source.getName()), copier);
            }
            if (destination.exists()) {
                backup = File.createTempFile(".previous-", "", mods);
                if (!backup.delete() || !destination.renameTo(backup))
                    throw new IOException("Cannot preserve existing mod");
            }
            if (!stage.renameTo(destination)) {
                if (backup != null && !backup.renameTo(destination))
                    throw new IOException("Cannot install mod; previous copy retained at " + backup);
                throw new IOException("Cannot install mod");
            }
            if (backup != null) delete(backup);
            return name;
        } finally { delete(stage); }
    }

    private static void unpackZip(File source, String selected, File stage, Copier copier) throws IOException {
        String prefix = selected.isEmpty() ? "" : selected + "/";
        String root = stage.getCanonicalPath();
        Set<String> outputs = new HashSet<>();
        try (ZipFile zip = new ZipFile(source)) {
            for (Enumeration<? extends ZipEntry> all = zip.entries(); all.hasMoreElements(); ) {
                ZipEntry entry = all.nextElement();
                String path = archivePath(entry.getName());
                if (!path.startsWith(prefix) || path.length() <= prefix.length()) continue;
                boolean skip = false;
                for (String segment : path.split("/")) if (hidden(segment)) { skip = true; break; }
                if (skip) continue;
                String rel = path.substring(prefix.length());
                File out = new File(stage, rel);
                if (!within(root, out.getCanonicalPath())) throw new Problem(Reason.UNSAFE_PATH);
                if (!outputs.add(rel.toLowerCase(Locale.ROOT))) throw new Problem(Reason.UNSAFE_PATH);
                if (entry.isDirectory()) { mkdir(out); continue; }
                mkdir(out.getParentFile());
                try (InputStream in = zip.getInputStream(entry); OutputStream stream = new FileOutputStream(out)) {
                    copier.copy(in, stream);
                }
            }
        }
    }

    private static void unpackRar(File source, String selected, File stage, Copier copier) throws IOException {
        String prefix = selected.isEmpty() ? "" : selected + "/";
        String root = stage.getCanonicalPath();
        Set<String> outputs = new HashSet<>();
        try (Archive archive = openRar(source)) {
            List<FileHeader> entries = archive.getFileHeaders();
            if (entries.size() > MAX_SEARCH) throw new Problem(Reason.SEARCH_LIMIT);
            validateRar(archive, entries);
            for (FileHeader entry : entries) {
                String path = archivePath(entry.getFileName());
                if (!path.startsWith(prefix) || path.length() <= prefix.length()) continue;
                boolean skip = false;
                for (String segment : path.split("/")) if (hidden(segment)) { skip = true; break; }
                if (skip) continue;
                String rel = path.substring(prefix.length());
                File out = new File(stage, rel);
                if (!within(root, out.getCanonicalPath())) throw new Problem(Reason.UNSAFE_PATH);
                if (!outputs.add(rel.toLowerCase(Locale.ROOT))) throw new Problem(Reason.UNSAFE_PATH);
                if (entry.isDirectory()) { mkdir(out); continue; }
                mkdir(out.getParentFile());
                try (InputStream in = archive.getInputStream(entry); OutputStream stream = new FileOutputStream(out)) {
                    copier.copy(in, stream);
                }
            }
        } catch (Problem e) { throw e; }
        catch (Exception e) { throw new IOException("Cannot extract RAR archive", e); }
    }

    private static List<String> normalizedRoots(Set<String> found) throws Problem {
        List<String> roots = new ArrayList<>(found);
        roots.removeIf(root -> found.stream().anyMatch(other -> !root.equals(other)
                && (other.isEmpty() || root.startsWith(other + "/"))));
        Collections.sort(roots);
        if (roots.isEmpty()) throw new Problem(Reason.NO_PAYLOAD);
        return roots;
    }

    private static void copyTree(File from, File to, String sourceRoot, Copier copier) throws IOException {
        if (!within(sourceRoot, from.getCanonicalPath())) throw new Problem(Reason.UNSAFE_PATH);
        if (Files.isSymbolicLink(from.toPath())) throw new Problem(Reason.UNSAFE_PATH);
        if (from.isFile()) { copyFile(from, to, copier); return; }
        mkdir(to);
        File[] kids = from.listFiles();
        if (kids == null) throw new IOException("Cannot read " + from.getName());
        for (File kid : kids) copyTree(kid, new File(to, kid.getName()), sourceRoot, copier);
    }

    private static void copyFile(File from, File to, Copier copier) throws IOException {
        mkdir(to.getParentFile());
        try (InputStream in = new FileInputStream(from); OutputStream out = new FileOutputStream(to)) {
            copier.copy(in, out);
        }
    }

    private static void mkdir(File dir) throws IOException {
        if (!dir.isDirectory() && !dir.mkdirs()) throw new IOException("Cannot create " + dir.getName());
    }

    private static void delete(File path) {
        File[] kids = null;
        try {
            if (path.getCanonicalFile().equals(path.getAbsoluteFile())) kids = path.listFiles();
        } catch (IOException ignored) {}
        if (kids != null) for (File kid : kids) delete(kid);
        path.delete();
    }
}
