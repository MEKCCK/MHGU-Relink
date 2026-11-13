// Exercise the real scanner and image builder with synthetic files, never game data.
#include <assert.h>
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

static const char *base;
static int base_scans;
static DIR *counted_open(const char *path) {
    if (!strncmp(path, base, strlen(base)) && (path[strlen(base)] == '/' || !path[strlen(base)])) ++base_scans;
    return opendir(path);
}
#define opendir counted_open
#ifndef ROMFS_SOURCE
#define ROMFS_SOURCE "../runtime/romfs.c"
#endif
#include ROMFS_SOURCE
#undef opendir
#ifndef TEST_PREPARE
#define TEST_PREPARE 1
#endif

uint64_t plat_now_ns(void) {
    LARGE_INTEGER t, hz; QueryPerformanceCounter(&t); QueryPerformanceFrequency(&hz);
    return (uint64_t)(t.QuadPart / hz.QuadPart) * 1000000000ull +
           (uint64_t)(t.QuadPart % hz.QuadPart) * 1000000000ull / hz.QuadPart;
}
// Archive merging and concurrent reads are outside this indexing regression.
bool arc_partial(const char *a, const char *b) { (void)a; (void)b; return false; }
bool arc_merge(const char *a, const char *b, const char *c, char *d, size_t n) {
    (void)a; (void)b; (void)c; (void)d; (void)n; return false;
}
ssize_t pread(int fd, void *buf, size_t n, off_t off) {
    if (_lseeki64(fd, off, SEEK_SET) < 0) return -1;
    return _read(fd, buf, (unsigned)n);
}

int main(int argc, char **argv) {
    assert(argc == 7);
    base = argv[1];
    const char *layers[] = {argv[2], argv[3]};
    int expected = atoi(argv[5]);
    bool cases = !strcmp(argv[6], "cases");
#if TEST_PREPARE
    if (cases) {
        assert(romfs_prepare_base("not-an-existing-romfs") == 0);
        assert(romfs_prepare_base(base) == expected);
        assert(romfs_prepare_base(layers[0]) == 2); // replace, rather than leak or reuse, another choice
        RomfsImage other = {0};
        assert(build(&other, base, NULL, 0, true) && other.nfiles == expected);
    }
#endif
    base_scans = 0;
    uint64_t t = plat_now_ns();
#if TEST_PREPARE
    assert(romfs_prepare_base(base) == expected);
    int scans = base_scans;
    assert(scans > 0);
    if (cases) {
        RomfsImage *dlc = romfs_image_open(layers[0]);
        assert(dlc && dlc->nfiles == 2); // a DLC must not consume the pending game's tree
        assert(prepared_root && !strcmp(prepared_path, base));
    }
#endif
    assert(romfs_open(base, layers, 2));
    double seconds = (plat_now_ns() - t) / 1e9;
#if TEST_PREPARE
    assert(base_scans == scans); // no second traversal of the base game's directories
    assert(!prepared_root && !prepared_path); // consumed exactly once
#endif
    assert(g_main.nfiles == expected + 2); // mods may add files, never satisfy the base validation
    assert(strstr(romfs_host_path("/nativeNX/common.bin"), layers[1])); // last mod wins
    assert(strstr(romfs_host_path("nativeNX/new.bin"), layers[0]));
    // Every serialized sibling must match the original parent-child lists, including nested/empty folders.
    uint64_t dhash_off = ((uint64_t *)g_main.hdr)[1], dmeta_off = ((uint64_t *)g_main.hdr)[3];
    uint64_t fmeta_off = ((uint64_t *)g_main.hdr)[7];
    for (int i = 0; i < g_main.nfiles; ++i) {
        File *f = g_main.files[i];
        uint32_t sibling = EMPTY;
        for (int k = 0; k + 1 < f->parent->nfiles; ++k)
            if (f->parent->files[k] == f) { sibling = f->parent->files[k + 1]->off; break; }
        uint32_t actual; memcpy(&actual, g_main.tables + fmeta_off - dhash_off + f->off + 4, 4);
        assert(actual == sibling);
        Dir *d = f->parent;
        if (d->parent) {
            sibling = EMPTY;
            for (int k = 0; k + 1 < d->parent->ndirs; ++k)
                if (d->parent->dirs[k] == d) { sibling = d->parent->dirs[k + 1]->off; break; }
            memcpy(&actual, g_main.tables + dmeta_off - dhash_off + d->off + 4, 4);
            assert(actual == sibling);
        }
    }
    FILE *out = fopen(argv[4], "wb"); assert(out);
    uint8_t *image = malloc((size_t)g_main.size); assert(image);
    assert(romfs_read(image, 0, g_main.size) == g_main.size);
    assert(fwrite(image, 1, (size_t)g_main.size, out) == g_main.size);
    fclose(out); free(image);
    printf("PASS: %d base files, %d indexed, %d base directory scans, %.6f s to index\n",
           expected, g_main.nfiles, base_scans, seconds);
}
