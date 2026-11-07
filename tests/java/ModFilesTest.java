package com.mhgu.recomp;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.util.Enumeration;
import java.util.List;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import java.util.zip.ZipOutputStream;
import com.github.junrar.Archive;
import com.github.junrar.ArchiveOptions;
import com.github.junrar.rarfile.FileHeader;

/** Run with javac/java plus Junrar and SLF4J on the classpath; no Android or test framework required. */
public final class ModFilesTest {
    private static final ModFiles.Copier COPY = (in, out) -> in.transferTo(out);

    public static void main(String[] args) throws Exception {
        namesAndTypes();
        directoryAndSingleFiles();
        zipSafety();
        if (args.length > 0) verifySample(new File(args[0]), 2);
        if (args.length > 1) verifySample(new File(args[1]), 1);
        if (args.length > 2) verifySample(new File(args[2]), 1);
        if (args.length > 3) verifySample(new File(args[3]), 1);
        System.out.println("ModFilesTest: OK");
    }

    private static void namesAndTypes() {
        check(ModFiles.accepts("x.ZIP") && ModFiles.accepts("x.RAR") && ModFiles.accepts("X.NRO") && ModFiles.accepts("x.IPS")
                && ModFiles.accepts("x.ips32") && ModFiles.accepts("x.pchtxt"), "supported extensions");
        check(!ModFiles.accepts("x.exe"), "unsupported extensions");
        String shortName = ModFiles.safeName("怪物：巨戟龙&大轰龙");
        check(shortName.contains("怪物") && shortName.getBytes(java.nio.charset.StandardCharsets.UTF_8).length <= 127,
                "Unicode mod name");
        String longName = ModFiles.safeName("龙".repeat(80));
        check(longName.getBytes(java.nio.charset.StandardCharsets.UTF_8).length <= 127, "UTF-8 name limit");
        check(!longName.equals(ModFiles.safeName("龙".repeat(79) + "虎")), "long-name collision hash");
    }

    private static void directoryAndSingleFiles() throws Exception {
        File temp = Files.createTempDirectory("modfiles-test").toFile();
        File mods = new File(temp, "mods");
        try {
            File wrapped = new File(temp, "合集/中文模组");
            put(new File(wrapped, "romfs/nativeNX/data.bin"), new byte[] {1, 2, 3});
            put(new File(wrapped, "exefs/main.npdm"), new byte[] {4, 5});
            check(ModFiles.roots(new File(temp, "合集")).equals(java.util.Collections.singletonList("中文模组")),
                    "wrapped folder root");
            String installed = ModFiles.install(new File(temp, "合集"), "中文模组", mods, COPY);
            check(installed.equals("中文模组") && bytes(new File(mods, installed + "/romfs/nativeNX/data.bin"))[2] == 3,
                    "selected folder payload");

            File direct = new File(temp, "romfs");
            put(new File(direct, "nativeNX/only.bin"), new byte[] {7});
            String directName = ModFiles.install(direct, "", mods, COPY);
            check(new File(mods, directName + "/romfs/nativeNX/only.bin").isFile(), "direct romfs folder");

            File nro = new File(temp, "sample.nro"); put(nro, new byte[] {8, 9});
            String plugin = ModFiles.install(nro, "", mods, COPY);
            check(new File(mods, plugin + "/romfs/nativeNX/plugins/sample.nro").isFile(), "NRO routing");
            File patch = new File(temp, "sample.ips32"); put(patch, new byte[] {10});
            String patchName = ModFiles.install(patch, "", mods, COPY);
            check(new File(mods, patchName + "/exefs/sample.ips32").isFile(), "patch routing");
            File sameStemNro = new File(temp, "shared.nro"); put(sameStemNro, new byte[] {12});
            File sameStemPatch = new File(temp, "shared.ips32"); put(sameStemPatch, new byte[] {13});
            String nroName = ModFiles.install(sameStemNro, "", mods, COPY);
            String ipsName = ModFiles.install(sameStemPatch, "", mods, COPY);
            check(!nroName.equals(ipsName), "single-file extensions avoid collisions");

            File previous = new File(mods, installed + "/keep.bin"); put(previous, new byte[] {42});
            try {
                ModFiles.install(new File(temp, "合集"), "中文模组", mods,
                        (in, out) -> { throw new java.io.IOException("injected write failure"); });
                throw new AssertionError("copy failure expected");
            } catch (java.io.IOException expected) {
                check(bytes(previous)[0] == 42, "failed import keeps old mod");
            }

            File nested = new File(mods, "nested");
            put(new File(nested, "romfs/a"), new byte[] {11});
            try {
                ModFiles.install(nested, "", mods, COPY);
                throw new AssertionError("overlapping source expected");
            } catch (ModFiles.Problem expected) {
                check(expected.reason == ModFiles.Reason.SOURCE_TARGET, "reject self import");
            }

            File deep = new File(temp, "deep-sample");
            for (int i = 0; i < 7; i++) deep = new File(deep, "level" + i);
            put(new File(deep, "exefs/main"), new byte[] {1});
            check(ModFiles.roots(new File(temp, "deep-sample")).contains("level0/level1/level2/level3/level4/level5/level6"),
                    "eight-level payload search: " + ModFiles.roots(new File(temp, "deep-sample")));

            File aliasReal = new File(temp, "alias-base");
            put(new File(aliasReal, "romfs/alias.bin"), new byte[] {14});
            File alias = new File(temp, "alias-base/../alias-base");
            String aliasName = ModFiles.install(alias, "", mods, COPY);
            check(bytes(new File(mods, aliasName + "/romfs/alias.bin"))[0] == 14, "canonical root alias");
        } finally { erase(temp); }
    }

    private static void zipSafety() throws Exception {
        File temp = Files.createTempDirectory("modzip-test").toFile();
        try {
            File multi = new File(temp, "multi.zip");
            try (ZipOutputStream zip = new ZipOutputStream(new FileOutputStream(multi))) {
                entry(zip, "one\\romfs\\payload", new byte[] {1});
                entry(zip, "two/romfs/payload", new byte[] {2});
            }
            check(ModFiles.roots(multi).equals(java.util.Arrays.asList("one", "two")), "multi-root ZIP choices");
            File mods = new File(temp, "mods");
            String selected = ModFiles.install(multi, "two", mods, COPY);
            check(bytes(new File(mods, selected + "/romfs/payload"))[0] == 2, "extract only selected ZIP root");

            File traversal = new File(temp, "bad.zip");
            try (ZipOutputStream zip = new ZipOutputStream(new FileOutputStream(traversal))) {
                entry(zip, "mod/romfs/file", new byte[] {1});
                entry(zip, "mod\\..\\escape", new byte[] {2});
            }
            try {
                ModFiles.roots(traversal);
                throw new AssertionError("ZIP traversal expected");
            } catch (ModFiles.Problem expected) {
                check(expected.reason == ModFiles.Reason.UNSAFE_PATH, "reject ZIP traversal");
            }

            File duplicates = new File(temp, "duplicates.zip");
            try (ZipOutputStream zip = new ZipOutputStream(new FileOutputStream(duplicates))) {
                entry(zip, "mod/romfs/A", new byte[] {1});
                entry(zip, "mod/romfs/a", new byte[] {2});
            }
            try {
                ModFiles.install(duplicates, "mod", new File(temp, "dupe-mods"), COPY);
                throw new AssertionError("case-insensitive duplicate expected");
            } catch (ModFiles.Problem expected) {
                check(expected.reason == ModFiles.Reason.UNSAFE_PATH, "reject normalized path collision");
            }
        } finally { erase(temp); }
    }

    private static void verifySample(File archiveFile, int expectedRoots) throws Exception {
        List<String> roots = ModFiles.roots(archiveFile);
        check(roots.size() == expectedRoots, archiveFile.getName() + " root count: " + roots);
        File temp = Files.createTempDirectory("modsample-test").toFile();
        try {
            for (String selected : roots) {
                File mods = new File(temp, "mods-" + roots.indexOf(selected));
                String name = ModFiles.install(archiveFile, selected, mods, COPY);
                String prefix = selected.isEmpty() ? "" : selected + "/";
                int files = 0;
                if (ModFiles.extension(archiveFile.getName()).equals(".rar")) {
                    try (Archive source = new Archive(archiveFile,
                            ArchiveOptions.builder().maxDictionarySize(256L << 20).build())) {
                        for (FileHeader entry : source.getFileHeaders()) {
                            String path = entry.getFileName().replace('\\', '/');
                            if (entry.isDirectory() || !path.startsWith(prefix) || path.length() == prefix.length()) continue;
                            String rel = path.substring(prefix.length());
                            if (rel.split("/")[0].startsWith(".")) continue;
                            File extracted = new File(mods, name + "/" + rel);
                            check(extracted.isFile(), "sample missing " + rel);
                            try (InputStream in = source.getInputStream(entry); InputStream actual = new FileInputStream(extracted)) {
                                check(java.util.Arrays.equals(digest(in), digest(actual)), "sample bytes differ " + rel);
                            }
                            files++;
                        }
                    }
                } else try (ZipFile source = new ZipFile(archiveFile)) {
                    for (Enumeration<? extends ZipEntry> all = source.entries(); all.hasMoreElements(); ) {
                        ZipEntry entry = all.nextElement();
                        String path = entry.getName().replace('\\', '/');
                        if (entry.isDirectory() || !path.startsWith(prefix) || path.length() == prefix.length()) continue;
                        String rel = path.substring(prefix.length());
                        if (rel.split("/")[0].startsWith(".")) continue;
                        File extracted = new File(mods, name + "/" + rel);
                        check(extracted.isFile(), "sample missing " + rel);
                        try (InputStream in = source.getInputStream(entry); InputStream actual = new FileInputStream(extracted)) {
                            check(java.util.Arrays.equals(digest(in), digest(actual)), "sample bytes differ " + rel);
                        }
                        files++;
                    }
                }
                check(files > 0, "sample has payload");
                if (ModFiles.extension(archiveFile.getName()).equals(".rar")) {
                    check(new File(mods, name + "/exefs/subsdk9").isFile(), "RAR sample subsdk9");
                    File pluginDir = new File(mods, name + "/romfs/nativeNX/plugins");
                    check(pluginDir.isDirectory() && pluginDir.list().length > 0, "RAR sample plugin");
                }
            }
        } finally { erase(temp); }
    }

    private static byte[] digest(InputStream in) throws Exception {
        MessageDigest hash = MessageDigest.getInstance("SHA-256");
        byte[] b = new byte[65536];
        for (int n; (n = in.read(b)) >= 0; ) hash.update(b, 0, n);
        return hash.digest();
    }
    private static void entry(ZipOutputStream zip, String path, byte[] data) throws Exception {
        zip.putNextEntry(new ZipEntry(path)); zip.write(data); zip.closeEntry();
    }
    private static void put(File file, byte[] data) throws Exception {
        file.getParentFile().mkdirs(); Files.write(file.toPath(), data);
    }
    private static byte[] bytes(File file) throws Exception { return Files.readAllBytes(file.toPath()); }
    private static void check(boolean ok, String message) { if (!ok) throw new AssertionError(message); }
    private static void erase(File file) {
        File[] kids = file.listFiles(); if (kids != null) for (File kid : kids) erase(kid);
        file.delete();
    }
}
