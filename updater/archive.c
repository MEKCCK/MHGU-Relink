// Reading a release archive (updater.h archive_extract): .zip (stored or deflated entries, CRC-checked) or .tar.gz
// (ustar, with GNU and pax long names), streamed: nothing is held whole in memory.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>
#include <zlib.h>

#include "updater.h"

#define CHUNK (1 << 16)

static uint32_t le16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static bool fail(char *error, size_t size, const char *fmt, const char *name) {
    snprintf(error, size, fmt, name);
    return false;
}

static bool write_all(SDL_IOStream *out, const void *data, size_t n) { return SDL_WriteIO(out, data, n) == n; }

// ---- zip
static bool zip_entry(SDL_IOStream *in, const uint8_t *cd, const char *name, SDL_IOStream *out, char *error, size_t size) {
    uint32_t method = le16(cd + 10), crc = le32(cd + 16), csize = le32(cd + 20), usize = le32(cd + 24);
    uint8_t lh[30];
    if (csize == 0xffffffff || usize == 0xffffffff) return fail(error, size, "%s: zip64 is not supported", name);
    if (SDL_SeekIO(in, le32(cd + 42), SDL_IO_SEEK_SET) < 0 || SDL_ReadIO(in, lh, 30) != 30 || le32(lh) != 0x04034b50 ||
        SDL_SeekIO(in, le16(lh + 26) + le16(lh + 28), SDL_IO_SEEK_CUR) < 0)
        return fail(error, size, "%s: damaged archive", name);
    if (method != 0 && method != 8) return fail(error, size, "%s: unsupported compression", name);
    static uint8_t ibuf[CHUNK], obuf[CHUNK];
    z_stream z = {0};
    if (method == 8 && inflateInit2(&z, -MAX_WBITS) != Z_OK) return fail(error, size, "%s: zlib failed", name);
    uint32_t left = csize, got = 0, sum = crc32(0, NULL, 0);
    int zr = Z_OK;
    bool ok = true;
    while (ok && left) {
        size_t n = left < CHUNK ? left : CHUNK;
        if (SDL_ReadIO(in, ibuf, n) != n) { ok = fail(error, size, "%s: archive cut short", name); break; }
        left -= (uint32_t)n;
        if (method == 0) {
            sum = crc32(sum, ibuf, (uInt)n), got += (uint32_t)n;
            if (!write_all(out, ibuf, n)) ok = fail(error, size, "%s: can't write (disk full?)", name);
            continue;
        }
        z.next_in = ibuf, z.avail_in = (uInt)n;
        while (ok && z.avail_in && zr != Z_STREAM_END) {
            z.next_out = obuf, z.avail_out = CHUNK;
            zr = inflate(&z, Z_NO_FLUSH);
            if (zr != Z_OK && zr != Z_STREAM_END) { ok = fail(error, size, "%s: damaged data", name); break; }
            size_t made = CHUNK - z.avail_out;
            sum = crc32(sum, obuf, (uInt)made), got += (uint32_t)made;
            if (!write_all(out, obuf, made)) ok = fail(error, size, "%s: can't write (disk full?)", name);
        }
    }
    if (method == 8) inflateEnd(&z);
    if (ok && (got != usize || sum != crc)) ok = fail(error, size, "%s: damaged data (CRC)", name);
    return ok;
}

static bool zip_extract(SDL_IOStream *in, ArchiveEntry entry, void *ctx, char *error, size_t size) {
    Sint64 len = SDL_GetIOSize(in);
    uint8_t tail[65557];
    size_t tn = len < (Sint64)sizeof tail ? (size_t)len : sizeof tail;
    if (len < 22 || SDL_SeekIO(in, len - (Sint64)tn, SDL_IO_SEEK_SET) < 0 || SDL_ReadIO(in, tail, tn) != tn)
        return fail(error, size, "%s", "not a zip archive");
    const uint8_t *eocd = NULL;
    for (size_t i = tn - 22 + 1; i-- > 0;)
        if (le32(tail + i) == 0x06054b50) { eocd = tail + i; break; }
    if (!eocd) return fail(error, size, "%s", "not a zip archive");
    uint32_t count = le16(eocd + 10), cd_size = le32(eocd + 12), cd_at = le32(eocd + 16);
    uint8_t *cd = malloc(cd_size ? cd_size : 1);
    if (!cd || SDL_SeekIO(in, cd_at, SDL_IO_SEEK_SET) < 0 || SDL_ReadIO(in, cd, cd_size) != cd_size) {
        free(cd);
        return fail(error, size, "%s", "damaged zip archive");
    }
    bool ok = true;
    for (uint32_t i = 0, at = 0; ok && i < count; i++) {
        const uint8_t *e = cd + at;
        if (at + 46 > cd_size || le32(e) != 0x02014b50) { ok = fail(error, size, "%s", "damaged zip archive"); break; }
        uint32_t nlen = le16(e + 28), next = at + 46 + nlen + le16(e + 30) + le16(e + 32);
        if (next > cd_size) { ok = fail(error, size, "%s", "damaged zip archive"); break; }
        char name[1024];
        snprintf(name, sizeof name, "%.*s", (int)nlen, (const char *)e + 46);
        for (char *p = name; *p; p++) if (*p == '\\') *p = '/';
        bool dir = nlen && name[strlen(name) - 1] == '/';
        unsigned mode = e[5] == 3 ? le32(e + 38) >> 16 & 07777 : 0;  // (made on Unix: its permissions)
        SDL_IOStream *out = entry(ctx, name, dir, mode);
        if (out) {
            ok = zip_entry(in, e, name, out, error, size);
            if (!SDL_CloseIO(out) && ok) ok = fail(error, size, "%s: can't write (disk full?)", name);
        }
        at = next;
    }
    free(cd);
    return ok;
}

// ---- tar.gz
typedef struct {
    SDL_IOStream *in;
    z_stream z;
    uint8_t ibuf[CHUNK];
    bool end;
} Gz;

static bool gz_read(Gz *g, void *buf, size_t n) {
    g->z.next_out = buf, g->z.avail_out = (uInt)n;
    while (g->z.avail_out) {
        if (!g->z.avail_in) {
            size_t got = SDL_ReadIO(g->in, g->ibuf, CHUNK);
            if (!got) return false;
            g->z.next_in = g->ibuf, g->z.avail_in = (uInt)got;
        }
        int zr = inflate(&g->z, Z_NO_FLUSH);
        if (zr == Z_STREAM_END) {  // (another gzip member may follow)
            if (inflateReset(&g->z) != Z_OK) return false;
        } else if (zr != Z_OK && zr != Z_BUF_ERROR) {
            return false;
        }
    }
    return true;
}

static uint64_t octal(const uint8_t *p, size_t n) {
    uint64_t v = 0;
    for (size_t i = 0; i < n && p[i] >= '0' && p[i] <= '7'; i++) v = v * 8 + (p[i] - '0');
    return v;
}

// a pax header's "path=" record (the long name)
static void pax_path(const char *data, size_t n, char *name, size_t size) {
    for (size_t at = 0; at < n;) {
        size_t len = (size_t)strtoul(data + at, NULL, 10);
        if (!len || at + len > n) return;
        const char *rec = memchr(data + at, ' ', len);
        if (rec && !strncmp(rec + 1, "path=", 5)) {
            const char *v = rec + 6;
            snprintf(name, size, "%.*s", (int)(data + at + len - 1 - v), v);
        }
        at += len;
    }
}

static bool tar_extract(SDL_IOStream *in, ArchiveEntry entry, void *ctx, char *error, size_t size) {
    Gz *g = calloc(1, sizeof *g);
    if (!g || inflateInit2(&g->z, 16 + MAX_WBITS) != Z_OK) { free(g); return fail(error, size, "%s", "zlib failed"); }
    g->in = in;
    static uint8_t buf[CHUNK];
    uint8_t h[512];
    char longname[1024] = "";
    bool ok = true;
    for (;;) {
        if (!gz_read(g, h, 512)) { ok = fail(error, size, "%s", "damaged or cut short .tar.gz"); break; }
        bool zero = true;
        for (int i = 0; i < 512 && zero; i++) zero = !h[i];
        if (zero) break;  // (the end)
        uint64_t len = octal(h + 124, 12), padded = (len + 511) & ~(uint64_t)511;
        char type = (char)h[156], name[1024];
        if (longname[0]) {
            snprintf(name, sizeof name, "%s", longname);
            longname[0] = 0;
        } else if (!memcmp(h + 257, "ustar", 5) && h[345]) {
            snprintf(name, sizeof name, "%.155s/%.100s", (const char *)h + 345, (const char *)h);
        } else {
            snprintf(name, sizeof name, "%.100s", (const char *)h);
        }
        if (type == 'L' || type == 'x') {  // the next entry's long name
            char *data = len < sizeof longname * 4 ? malloc(padded + 1) : NULL;
            if (!data || !gz_read(g, data, padded)) { free(data); ok = fail(error, size, "%s", "damaged .tar.gz"); break; }
            data[len] = 0;
            if (type == 'L') snprintf(longname, sizeof longname, "%s", data);
            else pax_path(data, len, longname, sizeof longname);
            free(data);
            continue;
        }
        bool file = type == '0' || type == 0, dir = type == '5';
        SDL_IOStream *out = file || dir ? entry(ctx, name, dir, (unsigned)octal(h + 100, 8) & 07777) : NULL;
        // (links and the rest: skipped)
        for (uint64_t left = padded; left;) {
            size_t n = left < CHUNK ? (size_t)left : CHUNK;
            if (!gz_read(g, buf, n)) { ok = fail(error, size, "%s: damaged or cut short .tar.gz", name); break; }
            uint64_t data = padded - left < len ? len - (padded - left) : 0;
            if (out && !write_all(out, buf, data < n ? (size_t)data : n))
                ok = fail(error, size, "%s: can't write (disk full?)", name);
            left -= n;
            if (!ok) break;
        }
        if (out && !SDL_CloseIO(out) && ok) ok = fail(error, size, "%s: can't write (disk full?)", name);
        if (!ok) break;
    }
    inflateEnd(&g->z);
    free(g);
    return ok;
}

bool archive_extract(const char *path, ArchiveEntry entry, void *ctx, char *error, size_t size) {
    SDL_IOStream *in = SDL_IOFromFile(path, "rb");
    if (!in) return fail(error, size, "can't open %s", path);
    uint8_t magic[2] = {0};
    SDL_ReadIO(in, magic, 2);
    SDL_SeekIO(in, 0, SDL_IO_SEEK_SET);
    bool ok = magic[0] == 0x1f && magic[1] == 0x8b ? tar_extract(in, entry, ctx, error, size)
            : magic[0] == 'P' && magic[1] == 'K'   ? zip_extract(in, entry, ctx, error, size)
                                                   : fail(error, size, "%s: not a .zip or .tar.gz", path);
    SDL_CloseIO(in);
    return ok;
}
