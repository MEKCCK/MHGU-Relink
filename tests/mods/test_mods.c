#define WIN32_LEAN_AND_MEAN

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "../../runtime/rt.h"
#include "strings.h"

static uint8_t test_guest_memory[0x10000];
#undef G2H
#define G2H(a) (test_guest_memory + (uint32_t)(a))

#include "../../runtime/mods.c"
#define Arc ArcArchive
#include "../../runtime/arcmerge.c"
#undef Arc

Module g_modules[MAX_MODULES];
int g_nmodules;
int g_nexefs;

const char *mods_dir(void) { return "."; }
bool mods_romfs_of(const char *name, char *out, size_t size) { (void)name; (void)out; (void)size; return false; }
int mods_installed(ModEntry *out, int max) { (void)out; (void)max; return 0; }
bool mods_find_dir(const char *dir, const char *want, int depth, char *out, size_t size) {
    (void)dir; (void)want; (void)depth; (void)out; (void)size; return false;
}
uint32_t dispatch_function_at(uint32_t addr, uint32_t *next) { (void)addr; (void)next; return 0; }
int dispatch_interpret(uint32_t entry, uint32_t end) { (void)entry; (void)end; return 0; }
bool loader_moved(uint32_t addr, uint32_t slack, int32_t *shift) {
    (void)addr; (void)slack; (void)shift; return false;
}
DIR *opendir(const char *path) { (void)path; return NULL; }
struct dirent *readdir(DIR *dir) { (void)dir; return NULL; }
int closedir(DIR *dir) { (void)dir; return 0; }

static unsigned temp_id;

#define CHECK(condition)                                                                                  \
    do {                                                                                                  \
        if (!(condition)) {                                                                               \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                       \
            return 1;                                                                                     \
        }                                                                                                 \
    } while (0)

static bool temp_path(char *out, size_t size, const char *suffix) {
    char dir[MAX_PATH];
    DWORD n = GetTempPathA(sizeof dir, dir);
    if (!n || n >= sizeof dir) return false;
    int written = snprintf(out, size, "%s/mhgu_mods_test_%lu_%u.%s", dir,
                           (unsigned long)GetCurrentProcessId(), temp_id++, suffix);
    return written >= 0 && (size_t)written < size;
}

static bool write_file(const char *path, const uint8_t *bytes, size_t size) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(bytes, 1, size, f) == size;
    return fclose(f) == 0 && ok;
}

static bool check_ips(const uint8_t *bytes, size_t size, const uint8_t *expected, size_t expected_size,
                      bool should_apply) {
    char path[MAX_PATH];
    if (!temp_path(path, sizeof path, "ips") || !write_file(path, bytes, size)) return false;
    memset(test_guest_memory, 0xCC, sizeof test_guest_memory);
    Module *m = &g_modules[2];
    memset(m, 0, sizeof *m);
    m->base = 0x100;
    m->size = sizeof test_guest_memory - m->base;
    Apply a = {m, "test", "patch.ips", 0, 0, 0};
    ips(&a, path);
    remove(path);
    bool ok = should_apply ? a.data == 1 && !memcmp(test_guest_memory + m->base, expected, expected_size)
                           : a.data == 0 && test_guest_memory[m->base] == 0xCC;
    if (!should_apply) ok = ok && a.bad > 0;
    return ok;
}

static bool write_arc(const char *path, unsigned count, unsigned first_name) {
    enum { HEADER_BYTES = 12, ARC_ENTRY_BYTES = 80 };
    size_t data_off = HEADER_BYTES + (size_t)count * ARC_ENTRY_BYTES;
    size_t size = data_off + 1;
    uint8_t *bytes = calloc(1, size);
    if (!bytes) return false;
    memcpy(bytes, "ARC\0", 4);
    bytes[4] = 1;
    bytes[6] = (uint8_t)count;
    bytes[7] = (uint8_t)(count >> 8);
    for (unsigned i = 0; i < count; i++) {
        uint8_t *entry = bytes + HEADER_BYTES + (size_t)i * ARC_ENTRY_BYTES;
        snprintf((char *)entry, 64, "entry-%05x", first_name + i);
        entry[64] = 1;
        entry[68] = 1;
        entry[72] = 1;
        uint32_t off = (uint32_t)data_off;
        memcpy(entry + 76, &off, sizeof off);
    }
    bytes[data_off] = 0x5A;
    bool ok = write_file(path, bytes, size);
    free(bytes);
    return ok;
}

static int test_arc_entry_limit(void) {
    char orig[MAX_PATH], prev[MAX_PATH], mod[MAX_PATH], out[MAX_PATH];
    CHECK(temp_path(orig, sizeof orig, "arc"));
    CHECK(temp_path(prev, sizeof prev, "arc"));
    CHECK(temp_path(mod, sizeof mod, "arc"));
    CHECK(temp_path(out, sizeof out, "arc"));
    CHECK(write_arc(orig, 0, 0));
    CHECK(write_arc(prev, 40000, 0));
    CHECK(write_arc(mod, 30000, 50000));
    CHECK(!arc_merge(orig, prev, mod, out, sizeof out));
    remove(orig);
    remove(prev);
    remove(mod);
    return 0;
}

int main(void) {
    static const uint8_t ips[] = {'P','A','T','C','H', 0,1,0, 0,3, 'i','p','s', 'E','O','F'};
    static const uint8_t ips32[] = {'I','P','S','3','2', 0,0,1,0, 0,3, '3','2','!','E','E','O','F'};
    static const uint8_t rle[] = {'P','A','T','C','H', 0,1,0, 0,0, 0,4,0x5A, 'E','O','F'};
    static const uint8_t no_eof[] = {'P','A','T','C','H', 0,1,0, 0,3, 'b','a','d'};
    static const uint8_t truncated[] = {'I','P','S','3','2', 0,0,1,0, 0,3, 'n','o','!', 0,0};
    static const uint8_t expected_ips[] = {'i','p','s'};
    static const uint8_t expected_32[] = {'3','2','!'};
    static const uint8_t expected_rle[] = {0x5A,0x5A,0x5A,0x5A};

    CHECK(check_ips(ips, sizeof ips, expected_ips, sizeof expected_ips, true));
    CHECK(check_ips(ips32, sizeof ips32, expected_32, sizeof expected_32, true));
    CHECK(check_ips(rle, sizeof rle, expected_rle, sizeof expected_rle, true));
    CHECK(check_ips(no_eof, sizeof no_eof, NULL, 0, false));
    CHECK(check_ips(truncated, sizeof truncated, NULL, 0, false));
    CHECK(test_arc_entry_limit() == 0);
    puts("PASS IPS, IPS32, RLE, malformed-patch atomicity, and ARC entry limit");
    return 0;
}
