// Android/LOG are spies; the bounded CPU state machine is copied verbatim from exec.c.
#include <assert.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint64_t image;
    uint32_t width, height, format, samples, aspect;
    bool gpu_owned, uploaded;
    float sx, sy;
} TexBackend;
typedef struct {
    uint64_t addr;
    uint32_t debug_id, width, height, depth, format, target, flags;
    void *backend;
    char label[64];
} NvnTexture;
typedef struct {
    uint32_t setmask, base_level, num_levels, min_layer, num_layers, format, ds_mode;
    uint8_t swizzle[4];
} NvnTextureView;
typedef struct {
    uint64_t tex;
    NvnTextureView view;
    uint8_t has_view;
    int32_t region[6];
} TexRef;
static uint64_t g_present_count;
static bool menu_open;
static bool overlay_menu_open(void) { return menu_open; }
static unsigned log_calls;
static char emitted[800 * 1024];
static void trace_log(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(emitted, sizeof emitted, fmt, args);
    va_end(args);
    assert(n >= 0 && (size_t)n < sizeof emitted);
    log_calls++;
}
#define LOG(...) trace_log(__VA_ARGS__)
#include "render_trace_under_test.h"

static void boundary(NvnTexture *target) {
    vk_trace_frame(target);
    g_present_count++;
}
static void arm(NvnTexture *target) {
    assert(!trace_active);
    vk_trace_request();
    boundary(target);
    assert(trace_active && trace_frames == 0);
}
static void finish(NvnTexture *target) {
    for (unsigned frame = 0; frame < TRACE_FRAMES; frame++) boundary(target);
    assert(!trace_active);
}
static unsigned occurrences(const char *text, const char *needle) {
    unsigned n = 0;
    for (const char *p = text; (p = strstr(p, needle)) != NULL; p += strlen(needle)) n++;
    return n;
}
int main(void) {
    TexBackend backend = {.image = 17, .width = 1386, .height = 640, .samples = 1};
    NvnTexture target = {.addr = 0x12340000, .debug_id = 9, .width = 1386, .height = 640, .depth = 1, .format = 0x25, .backend = &backend};
    TexRef copy = {.tex = (uint64_t)(uintptr_t)&target, .has_view = 1, .region = {-1, 2, 3, 4, 5, 6},
                   .view = {.setmask = 0x3f, .base_level = 2, .num_levels = 3, .min_layer = 4,
                            .num_layers = 5, .format = 0x42, .ds_mode = 1, .swizzle = {2, 1, 0, 3}}};
    assert(TRACE_CAPACITY == 768 * 1024 && TRACE_FRAMES == 6 && TRACE_DRAWS == 1024);

    // No request: neither CPU trace rows nor output, even while frames/draws occur.
    for (unsigned frame = 0; frame < 20; frame++) {
        assert(!trace_draw_begin());
        trace_append("must not be recorded\n");
        trace_auxdraw("disabled");
        trace_copy_ref("disabled", &copy);
        boundary(&target);
    }
    assert(!trace_active && trace_used == 0 && log_calls == 0);

    // Requests while the settings menu is open coalesce and retain the waiting request.
    menu_open = true;
    for (unsigned frame = 0; frame < 20; frame++) {
        vk_trace_request();
        boundary(&target);
        assert(!trace_active && trace_used == 0 && log_calls == 0);
    }
    menu_open = false;
    boundary(&target); // Only arm here; the partial frame before this boundary is excluded.
    assert(trace_active && trace_frames == 0 && log_calls == 0);
    uint64_t first_frame = g_present_count;
    for (unsigned frame = 0; frame < TRACE_FRAMES; frame++) {
        for (unsigned draw = 0; draw < TRACE_DRAWS; draw++) {
            assert(trace_draw_begin());
            trace_append("test draw %u/%u\n", frame, draw);
        }
        assert(!trace_draw_begin());
        assert(!trace_draw_begin());
        assert(trace_draws == TRACE_DRAWS);
        vk_trace_request(); // Requests during a capture do not extend or queue another capture.
        boundary(&target);
        assert(log_calls == (frame + 1 == TRACE_FRAMES ? 1u : 0u));
        if (frame + 1 < TRACE_FRAMES) assert(trace_active && trace_frames == frame + 1);
    }
    assert(!trace_active && strlen(emitted) < TRACE_CAPACITY);
    assert(occurrences(emitted, "[render-trace] begin frame=") == TRACE_FRAMES);
    assert(occurrences(emitted, "[render-trace] present frame=") == TRACE_FRAMES);
    assert(occurrences(emitted, "[render-trace] draw limit:") == TRACE_FRAMES);
    char row[128];
    snprintf(row, sizeof row, "[render-trace] present frame=%llu ", (unsigned long long)first_frame);
    assert(strstr(emitted, row));
    snprintf(row, sizeof row, "[render-trace] present frame=%llu ", (unsigned long long)(first_frame + TRACE_FRAMES - 1));
    assert(strstr(emitted, row));
    boundary(&target);
    assert(!trace_active && log_calls == 1);

    // An explicit later request starts a separate capture; NULL targets are safe metadata.
    arm(NULL);
    assert(log_calls == 1);
    trace_auxdraw("test-builtin");
    trace_copy_ref("copy", &copy);
    finish(NULL);
    assert(log_calls == 2);
    assert(occurrences(emitted, "[render-trace] present frame=") == TRACE_FRAMES);
    assert(strstr(emitted, "builtin=test-builtin"));
    assert(strstr(emitted, "copy id=9"));
    assert(strstr(emitted, "addr=12340000 fmt=25 guest=1386x640x1"));
    assert(strstr(emitted, "view=3f:m2+3:l4+5:f42:ds1:sw2,1,0,3"));
    assert(strstr(emitted, "region=-1,2,3,4,5,6"));

    // Overlong rows cannot write past the bound and output still finishes at six frames.
    arm(&target);
    char *large = malloc(TRACE_CAPACITY + 64);
    assert(large);
    memset(large, 'x', TRACE_CAPACITY + 63);
    large[TRACE_CAPACITY + 63] = 0;
    trace_append("%s", large);
    free(large);
    assert(trace_truncated && trace_used < TRACE_CAPACITY);
    assert(trace_text[trace_used] == 0);
    size_t full_used = trace_used;
    trace_append("must not grow a truncated capture\n");
    assert(trace_used == full_used && log_calls == 2);
    finish(&target);
    assert(log_calls == 3 && strlen(emitted) < TRACE_CAPACITY);
    assert(strstr(emitted, "truncat") != NULL);
    assert(occurrences(emitted, "[render-trace] present frame=") == TRACE_FRAMES);
    assert(!trace_active);

    // Every frame keeps its present metadata even if all six exhaust their detail budget.
    arm(&target);
    char chunk[4096];
    memset(chunk, 'y', sizeof chunk - 1);
    chunk[sizeof chunk - 1] = 0;
    for (unsigned frame = 0; frame < TRACE_FRAMES; frame++) {
        assert(!trace_frame_truncated && trace_draw_begin());
        size_t frame_start = trace_used;
        unsigned rows = 0;
        while (!trace_frame_truncated) {
            trace_append("%s", chunk);
            assert(trace_used < TRACE_CAPACITY && trace_text[trace_used] == 0);
            assert(++rows < TRACE_CAPACITY / sizeof chunk);
        }
        assert(trace_used - frame_start <= TRACE_CAPACITY / TRACE_FRAMES);
        boundary(&target);
        assert(trace_used - frame_start <= TRACE_CAPACITY / TRACE_FRAMES);
        assert(log_calls == (frame + 1 == TRACE_FRAMES ? 4u : 3u));
    }
    assert(!trace_active && trace_truncated && strlen(emitted) < TRACE_CAPACITY);
    assert(occurrences(emitted, "[render-trace] present frame=") == TRACE_FRAMES);
    assert(occurrences(emitted, "[render-trace] begin frame=") == TRACE_FRAMES);
    arm(&target); // Truncating one capture does not disable subsequent requests.
    assert(!trace_truncated);
    finish(&target);
    assert(log_calls == 5 && !trace_active);
    puts("PASS production CPU trace: disabled by default, menu-close/full-frame gating, six-frame capture, coalesced requests, draw/byte bounds, NULL targets, and one bounded log.");
    return 0;
}
