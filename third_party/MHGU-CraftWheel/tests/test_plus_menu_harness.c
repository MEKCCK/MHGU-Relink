/* ARM guest-side fixtures; the test supplies the original game's menu input,
   selector, game heap lookup and hint-vector resize code. */
#include <stdbool.h>
#include <stdint.h>

static unsigned base, english;
static void *method(void *object, unsigned index) { return (*(void ***)object)[index]; }
static void copy(void *dest, const void *src, unsigned n) {
    uint8_t *dst = dest;
    const uint8_t *in = src;
    while (n--) *dst++ = *in++;
}
void *memcpy(void *dest, const void *src, unsigned n) {
    copy(dest, src, n);
    return dest;
}
static void forge_log(unsigned level, const char *fmt, ...) { (void)level; (void)fmt; }
static void forge_hook_create(void *target, void *detour, void **original) {
    (void)target; (void)detour; (void)original;
}
#include "plus_menu.inc"

static unsigned next = 0x4040000, frees, fail_at;
void *allocate(void *heap, unsigned n, unsigned align) {
    (void)heap;
    if (align != 16 || (fail_at && !--fail_at)) return 0;
    unsigned p = next;
    next += (n + 15) & ~15u;
    return (void *)p;
}
void release(void *heap, void *p) { (void)heap; (void)p; frees++; }
uintptr_t stub(uint8_t *menu) { (void)menu; return 0; }
uintptr_t mock_refresh(uint8_t *menu) {
    PlusRows *rows = (PlusRows *)(menu + 0xa8);
    for (unsigned i = 0; i < rows->size; i++)
        rows->data[i] = (PlusRow){i < plus_stock, "native"};
    return 0;
}
void configure(void) {
    plus_ready = true;
    plus_open_original = plus_close_original = plus_destroy_original = stub;
    plus_input_original = (PlusHandler)0x66f868u;
}
