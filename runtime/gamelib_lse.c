// Phones whose CPU has no ARMv8.1 atomic instructions (LSE: Cortex-A53, A57, A72, A73 alone, and so on).
// The game code library is built for ARMv8.2: the guest's exclusives are compare-and-swap, one inline instruction
// (CAS, cpu.h). On those CPUs the first one is an illegal instruction, at the game's very first critical section.
// The library is made once on a PC and shared with every CPU, so it is the app that adapts it: a private copy of it
// where each of those instructions jumps to its own short sequence of exclusive load / store, the older way of doing
// the same, in a segment added at the end of the file. Nothing else in the library changes, and the player's file is
// not touched. Only on such a CPU (or with MHGU_FORCE_NOLSE, to test the copy on any phone).
#include "gamelib_lse.h"

#if defined(__ANDROID__) && defined(__aarch64__)
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "rt.h"

#ifndef HWCAP_ATOMICS
#define HWCAP_ATOMICS (1ul << 8)
#endif

#define COPY_VERSION 1       // (a new way of adapting it: the copies made before are made again)
#define SEG_ALIGN 0x4000u    // (16 KB pages: as the library is linked)
#define SEQ_MAX 14           // the longest sequence below
#define BRANCH_RANGE (1ll << 27)

typedef struct {
    uint32_t *p;
    size_t n, cap;
} Code;

static void put(Code *c, uint32_t insn) { c->p[c->n++] = insn; }

static uint32_t mov_reg(int x, int d, int m) { return (x ? 0xAA0003E0u : 0x2A0003E0u) | (uint32_t)m << 16 | (uint32_t)d; }
static uint32_t cbnz(int x, int t, int words) { return (x ? 0xB5000000u : 0x35000000u) | ((uint32_t)words & 0x7FFFFu) << 5 | (uint32_t)t; }
static uint32_t ldaxr(int sz, int t, int n) { return 0x085FFC00u | (uint32_t)sz << 30 | (uint32_t)n << 5 | (uint32_t)t; }
static uint32_t stlxr(int sz, int s, int t, int n) { return 0x0800FC00u | (uint32_t)sz << 30 | (uint32_t)s << 16 | (uint32_t)n << 5 | (uint32_t)t; }
static uint32_t branch(int64_t bytes) { return 0x14000000u | ((uint32_t)(bytes >> 2) & 0x3FFFFFFu); }
#define STP_PUSH(a, b) (0xA9BF0000u | (uint32_t)(b) << 10 | 31u << 5 | (uint32_t)(a))
#define LDP_POP(a, b)  (0xA8C10000u | (uint32_t)(b) << 10 | 31u << 5 | (uint32_t)(a))
#define CLREX 0xD5033F5Fu

// Two registers free for the sequence (pushed around it: no flags, no register changes but the instruction's own)
static void scratch(int rs, int rt, int rn, int *a, int *b) {
    int found = 0;
    for (int r = 9; r <= 15 && found < 2; r++)
        if (r != rs && r != rt && r != rn) (found++ ? b : a)[0] = r;
}

// CAS Rs, Rt, [Rn]: Rs is compared (its low 8 or 16 bits for the short forms) and gets the old value
static void seq_cas(Code *c, int sz, int rs, int rt, int rn) {
    int t0 = 0, t1 = 0, x = sz == 3;
    scratch(rs, rt, rn, &t0, &t1);
    put(c, STP_PUSH(t0, t1));
    size_t loop = c->n;
    put(c, ldaxr(sz, t0, rn));
    put(c, (x ? 0xCA000000u : 0x4A000000u) | (uint32_t)rs << 16 | (uint32_t)t0 << 5 | (uint32_t)t1);  // eor t1, t0, rs
    if (sz < 2) put(c, (sz ? 0x12003C00u : 0x12001C00u) | (uint32_t)t1 << 5 | (uint32_t)t1);          // and t1, t1, #mask
    size_t differs = c->n;
    put(c, 0);
    put(c, stlxr(sz, t1, rt, rn));
    put(c, cbnz(0, t1, (int)loop - (int)c->n));
    c->p[differs] = cbnz(x, t1, (int)c->n - (int)differs);
    put(c, CLREX);
    put(c, mov_reg(x, rs, t0));
    put(c, LDP_POP(t0, t1));
}

// SWP Rs, Rt, [Rn]: Rs stored, Rt gets the old value
static void seq_swp(Code *c, int sz, int rs, int rt, int rn) {
    int t0 = 0, t1 = 0;
    scratch(rs, rt, rn, &t0, &t1);
    put(c, STP_PUSH(t0, t1));
    size_t loop = c->n;
    put(c, ldaxr(sz, t0, rn));
    put(c, stlxr(sz, t1, rs, rn));
    put(c, cbnz(0, t1, (int)loop - (int)c->n));
    put(c, mov_reg(sz == 3, rt, t0));
    put(c, LDP_POP(t0, t1));
}

static bool is_cas(uint32_t w) { return (w & 0x3FA07C00u) == 0x08A07C00u; }
static bool is_swp(uint32_t w) { return (w & 0x3F20FC00u) == 0x38208000u; }
static bool is_other_lse(uint32_t w) { return (w & 0xBFA07C00u) == 0x08207C00u || (w & 0x3F208C00u) == 0x38200000u; }

typedef struct {
    uint32_t *text;   // in the file's image
    uint64_t addr;    // its address in the library
    size_t words;
} Text;

static bool find_text(uint8_t *f, size_t size, Text *t) {
    Elf64_Ehdr *e = (Elf64_Ehdr *)(void *)f;
    if (size < sizeof *e || memcmp(e->e_ident, ELFMAG, SELFMAG) || e->e_ident[EI_CLASS] != ELFCLASS64 ||
        e->e_machine != EM_AARCH64 || !e->e_shoff || e->e_shstrndx >= e->e_shnum ||
        e->e_shoff + (uint64_t)e->e_shnum * sizeof(Elf64_Shdr) > size)
        return false;
    Elf64_Shdr *sh = (Elf64_Shdr *)(void *)(f + e->e_shoff);
    const char *names = (const char *)f + sh[e->e_shstrndx].sh_offset;
    for (int i = 0; i < e->e_shnum; i++)
        if (!strcmp(names + sh[i].sh_name, ".text") && sh[i].sh_offset + sh[i].sh_size <= size) {
            t->text = (uint32_t *)(void *)(f + sh[i].sh_offset), t->addr = sh[i].sh_addr, t->words = sh[i].sh_size / 4;
            return true;
        }
    return false;
}

static bool write_all(int fd, const void *data, size_t n) {
    const uint8_t *p = data;
    while (n) {
        ssize_t w = write(fd, p, n > (1u << 24) ? (1u << 24) : n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
        p += w, n -= (size_t)w;
    }
    return true;
}

// The library at `from` as a new file `to`, its LSE instructions turned into jumps. 0: nothing to do, > 0: how many
// were changed, < 0: failed (why)
static int make_copy(const char *from, const char *to, char *why, size_t why_size) {
    int fd = open(from, O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st)) { snprintf(why, why_size, "%s: %s", from, strerror(errno)); if (fd >= 0) close(fd); return -1; }
    size_t size = (size_t)st.st_size;
    uint8_t *f = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    close(fd);
    if (f == MAP_FAILED) { snprintf(why, why_size, "%s: can't read it", from); return -1; }
    int result = -1;
    Code c = {0};
    Text t;
    if (!find_text(f, size, &t)) { snprintf(why, why_size, "%s: not a library for this phone", from); goto out; }
    size_t sites = 0;
    for (size_t i = 0; i < t.words; i++) {
        uint32_t w = t.text[i];
        if (is_cas(w) || is_swp(w)) sites++;
        else if (is_other_lse(w)) { snprintf(why, why_size, "%s: instruction %08x at %zx", from, w, i * 4); goto out; }
    }
    if (!sites) { result = 0; goto out; }
    c.cap = sites * SEQ_MAX;
    c.p = malloc(c.cap * 4);
    if (!c.p) { snprintf(why, why_size, "out of memory"); goto out; }

    // where the new segment goes: past everything the library maps, in the file and in its addresses
    Elf64_Ehdr *e = (Elf64_Ehdr *)(void *)f;
    Elf64_Phdr *ph = (Elf64_Phdr *)(void *)(f + e->e_phoff), *free_ph = NULL;
    uint64_t top = 0;
    for (int i = 0; i < e->e_phnum; i++) {
        if (ph[i].p_type == PT_LOAD && ph[i].p_vaddr + ph[i].p_memsz > top) top = ph[i].p_vaddr + ph[i].p_memsz;
        if (ph[i].p_type == PT_NOTE) free_ph = &ph[i];  // (the build's notes: read by no one at run time)
    }
    if (!free_ph) { snprintf(why, why_size, "%s: no room for a segment", from); goto out; }
    uint64_t seg_addr = (top + SEG_ALIGN - 1) & ~(uint64_t)(SEG_ALIGN - 1);
    uint64_t seg_off = (size + SEG_ALIGN - 1) & ~(uint64_t)(SEG_ALIGN - 1);

    for (size_t i = 0; i < t.words; i++) {
        uint32_t w = t.text[i];
        bool cas = is_cas(w);
        if (!cas && !is_swp(w)) continue;
        int sz = (int)(w >> 30), rs = (int)(w >> 16) & 31, rn = (int)(w >> 5) & 31, rt = (int)w & 31;
        if (rn == 31) { snprintf(why, why_size, "%s: an atomic on the stack at %zx", from, i * 4); goto out; }
        uint64_t site = t.addr + i * 4, seq = seg_addr + c.n * 4;
        if (cas) seq_cas(&c, sz, rs, rt, rn);
        else seq_swp(&c, sz, rs, rt, rn);
        int64_t there = (int64_t)(seq - site), back = (int64_t)(site + 4) - (int64_t)(seg_addr + c.n * 4);
        if (there >= BRANCH_RANGE || back < -BRANCH_RANGE) { snprintf(why, why_size, "%s: too far apart", from); goto out; }
        put(&c, branch(back));
        t.text[i] = branch(there);
    }

    free_ph->p_type = PT_LOAD, free_ph->p_flags = PF_R | PF_X, free_ph->p_offset = seg_off;
    free_ph->p_vaddr = free_ph->p_paddr = seg_addr, free_ph->p_filesz = free_ph->p_memsz = c.n * 4;
    free_ph->p_align = SEG_ALIGN;

    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s.tmp", to);
    unlink(tmp);
    int o = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0700);
    static const uint8_t zeros[SEG_ALIGN];
    if (o < 0 || !write_all(o, f, size) || !write_all(o, zeros, seg_off - size) || !write_all(o, c.p, c.n * 4)) {
        snprintf(why, why_size, "can't write %s: %s", tmp, strerror(errno));
        if (o >= 0) close(o);
        unlink(tmp);
        goto out;
    }
    fsync(o);
    close(o);
    chmod(tmp, 0500);
    if (rename(tmp, to)) { snprintf(why, why_size, "can't write %s: %s", to, strerror(errno)); unlink(tmp); goto out; }
    result = (int)sites;
out:
    free(c.p);
    munmap(f, size);
    return result;
}

bool gamelib_cpu_lse(void) { return (getauxval(AT_HWCAP) & HWCAP_ATOMICS) != 0; }

bool gamelib_fit_cpu(char *path, size_t path_size, char *why, size_t why_size) {
    char copy[1100], stamp[1120], want[96], have[96] = "";
    snprintf(copy, sizeof copy, "%s.v80", path);
    snprintf(stamp, sizeof stamp, "%s.stamp", copy);
    if (gamelib_cpu_lse() && !getenv("MHGU_FORCE_NOLSE")) {  // (a copy left by a test: 110 MB nobody will use)
        chmod(copy, 0700);
        unlink(copy);
        unlink(stamp);
        return true;
    }
    struct stat st;
    if (stat(path, &st)) return true;  // (dlopen says what is wrong)
    snprintf(want, sizeof want, "%d %lld %lld\n", COPY_VERSION, (long long)st.st_size, (long long)st.st_mtime);
    FILE *s = fopen(stamp, "r");
    if (s) {
        if (!fgets(have, sizeof have, s)) have[0] = 0;
        fclose(s);
    }
    struct stat cs;
    if (!strcmp(have, want) && !stat(copy, &cs)) {
        LOG("[gamelib] this CPU has no LSE atomics: the adapted copy\n");
        snprintf(path, path_size, "%s", copy);
        return true;
    }
    unlink(stamp);
    if (!stat(copy, &cs)) chmod(copy, 0700), unlink(copy);
    uint64_t t0 = plat_now_ns();
    int n = make_copy(path, copy, why, why_size);
    if (n < 0) return false;
    if (!n) {  // (a library with none of them: fine as it is)
        LOG("[gamelib] this CPU has no LSE atomics, nor the library\n");
        return true;
    }
    s = fopen(stamp, "w");
    if (s) fputs(want, s), fclose(s);
    LOG("[gamelib] this CPU has no LSE atomics: %d instructions adapted in %.1f s\n", n, (plat_now_ns() - t0) / 1e9);
    snprintf(path, path_size, "%s", copy);
    return true;
}
#else
bool gamelib_cpu_lse(void) { return true; }
bool gamelib_fit_cpu(char *path, size_t path_size, char *why, size_t why_size) {
    (void)path, (void)path_size, (void)why, (void)why_size;
    return true;
}
#endif
