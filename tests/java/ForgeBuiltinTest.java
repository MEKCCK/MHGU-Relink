import java.io.*;
import java.nio.file.*;
import java.util.*;

// The runner appends prepareForgeAssets and copyAsset unchanged from GameActivity.
class ForgeBuiltinTest {
    private static final String TAG = "test";
    private final File internal;
    private final AssetFiles assets;
    private static class Log {
        static int warnings;
        static void w(String tag, String message) { ++warnings; }
    }
    private static class AssetFiles {
        final Path root;
        AssetFiles(Path root) { this.root = root; }
        InputStream open(String name) throws IOException { return new FileInputStream(root.resolve(name).toFile()); }
    }
    ForgeBuiltinTest(Path internal, Path assets) {
        this.internal = internal.toFile();
        this.assets = new AssetFiles(assets);
    }
    private File getFilesDir() { return internal; }
    private AssetFiles getAssets() { return assets; }
    private static void checkAssets(Path internal, Path assets) throws Exception {
        try (var files = Files.walk(assets.resolve("forge-v2.0.0"))) {
            List<Path> resources = files.filter(Files::isRegularFile).toList();
            assert resources.size() == 5;
            for (Path source : resources)
                assert Arrays.equals(Files.readAllBytes(source), Files.readAllBytes(internal.resolve(assets.relativize(source))));
        }
    }
    public static void main(String[] args) throws Exception {
        Path internal = Path.of(args[0]), assets = Path.of(args[1]);
        Path userConfig = internal.resolve("mods/User/romfs/nativeNX/forge/config.ini");
        Files.createDirectories(userConfig.getParent());
        Files.writeString(userConfig, "user settings\n");
        Path legacyConfig = internal.resolve("forge-v1.5.1/romfs/nativeNX/forge/config.ini");
        Files.createDirectories(legacyConfig.getParent());
        Files.writeString(legacyConfig, "legacy private settings\n");
        Path legacyExtra = legacyConfig.resolveSibling("keep.txt");
        Files.writeString(legacyExtra, "keep old private files\n");
        Path imguiConfig = internal.resolve("forge_imgui.ini");
        Files.writeString(imguiConfig, "user window settings\n");
        ForgeBuiltinTest test = new ForgeBuiltinTest(internal, assets);
        test.prepareForgeAssets();
        checkAssets(internal, assets);
        test.prepareForgeAssets();
        checkAssets(internal, assets);
        Path config = internal.resolve("forge-v2.0.0/romfs/nativeNX/forge/config.ini");
        Files.writeString(config, "partial");
        test.prepareForgeAssets();
        checkAssets(internal, assets);
        // Same size, different bytes: a length-only freshness check would keep this file forever.
        int configSize = Files.readString(assets.resolve("forge-v2.0.0/romfs/nativeNX/forge/config.ini")).length();
        Files.writeString(config, "x".repeat(configSize));
        test.prepareForgeAssets();
        checkAssets(internal, assets);
        assert Files.readString(userConfig).equals("user settings\n");
        assert Files.readString(legacyConfig).equals("legacy private settings\n");
        assert Files.readString(legacyExtra).equals("keep old private files\n");
        assert Files.readString(imguiConfig).equals("user window settings\n");
        assert Log.warnings == 0;
        new ForgeBuiltinTest(internal.resolve("missing-assets"), assets.resolve("absent")).prepareForgeAssets();
        assert Log.warnings == 5;
        System.out.println("PASS: production Java copies all five Forge 2.0.0 resources, repairs partial and same-size stale files, preserves user/mod/legacy private config and tolerates missing assets");
    }
    // PRODUCTION_METHODS
}
