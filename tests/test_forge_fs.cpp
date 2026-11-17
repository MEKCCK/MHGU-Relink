// The runner includes unchanged production Forge adapters and gcall.
// Only guest memory, symbol lookup and the game's nn::fs calls are fixtures.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

struct Cpu { uint32_t r[16]; };
static unsigned char memory[0x8000];
static bool kernel_mapped(uint64_t address, uint32_t size) {
    return address >= 0x1000 && address + size <= sizeof memory;
}
static uint32_t load32(uint32_t address) {
    assert(kernel_mapped(address, 4));
    uint32_t value;
    memcpy(&value, memory + address, sizeof value);
    return value;
}
static void store32(uint32_t address, uint32_t value) {
    assert(kernel_mapped(address, 4));
    memcpy(memory + address, &value, sizeof value);
}
#define LD32(address) load32(address)
#define ST32(address, value) store32(address, value)
#define ARG(i) (c->r[(i)])
#define STK(i) LD32(c->r[13] + 4 * (i))
#define RET(value) (c->r[0] = (uint32_t)(value))
#define HLE_FUNC(symbol, name) static void name(Cpu *c)

static uint32_t gsym(const char *name);
static void guest_call(Cpu *c, uint32_t function);
#include "forge_fs_production.inc"

enum { OPEN = 1, SIZE, READ, CLOSE };
static int missing_symbol, calls, expected_call;
static uint32_t sdk_result, path, mode, handle, out, buffer, count, original_sp;
static uint64_t offset, file_size;
static const char payload[] = "mounted mod bytes";

static uint32_t gsym(const char *name) {
    int symbol = !strcmp(name, "_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci") ? OPEN :
        !strcmp(name, "_ZN2nn2fs11GetFileSizeEPxNS0_10FileHandleE") ? SIZE :
        !strcmp(name, "_ZN2nn2fs8ReadFileENS0_10FileHandleExPvj") ? READ :
        !strcmp(name, "_ZN2nn2fs9CloseFileENS0_10FileHandleE") ? CLOSE : 0;
    assert(symbol);
    return symbol == missing_symbol ? 0 : symbol;
}
static void guest_call(Cpu *c, uint32_t function) {
    ++calls;
    assert(function == (uint32_t)expected_call);
    if (function == OPEN) {
        assert(c->r[0] != out && !(c->r[0] & 7));
        assert(c->r[13] == c->r[0] && c->r[13] < original_sp);
        assert(c->r[1] == path && c->r[2] == mode);
        // Failure may clobber the SDK's local output but must not publish it.
        ST32(c->r[0], handle);
    } else if (function == SIZE) {
        assert(c->r[0] == out && c->r[1] == handle);
        assert(c->r[13] == original_sp);
        if (!sdk_result) {
            ST32(out, (uint32_t)file_size);
            ST32(out + 4, (uint32_t)(file_size >> 32));
        }
    } else if (function == READ) {
        assert(c->r[0] == handle && c->r[13] == original_sp);
        assert(((uint64_t)c->r[3] << 32 | c->r[2]) == offset);
        assert(STK(0) == buffer && STK(1) == count);
        if (!sdk_result && count) {
            assert(count <= sizeof payload && kernel_mapped(buffer, count));
            memcpy(memory + buffer, payload, count);
        }
    } else {
        assert(function == CLOSE && c->r[0] == handle);
        assert(c->r[13] == original_sp);
    }
    c->r[0] = sdk_result;
    c->r[1] = 0xDEADBEEF;
    c->r[2] = 0xDEADBEEF;
    c->r[3] = 0xDEADBEEF;
}

static Cpu fresh(int function) {
    Cpu c = {};
    memset(memory, 0xA5, sizeof memory);
    calls = missing_symbol = 0;
    expected_call = function;
    sdk_result = 0;
    path = 0x1100;
    strcpy((char *)memory + path, "app:/nativeNX/plugins/example.bin");
    mode = 1;
    handle = 0xFEDCBA98;
    out = 0x1200;
    buffer = 0x1300;
    count = sizeof payload;
    original_sp = c.r[13] = 0x7000;
    offset = 0x1234567887654321ull;
    file_size = 0x23456789ABCDEF01ull;
    return c;
}
static void open_args(Cpu *c) { c->r[0] = path; c->r[1] = mode; c->r[2] = out; }
static void size_args(Cpu *c) { c->r[0] = handle; c->r[1] = out; }
static void read_args(Cpu *c) {
    c->r[0] = handle;
    c->r[1] = 0xBAD11111;  // The aligned s64 must skip r1.
    c->r[2] = (uint32_t)offset;
    c->r[3] = (uint32_t)(offset >> 32);
    ST32(c->r[13], buffer);
    ST32(c->r[13] + 4, count);
}
static void no_call(const Cpu &c) { assert(!c.r[0] && !calls && c.r[13] == original_sp); }

int main() {
    Cpu c = fresh(OPEN);
    // All enum bits pass unchanged to the mounted SDK path.
    for (uint32_t requested_mode : {1u, 2u, 3u, 6u, 7u}) {
        c = fresh(OPEN);
        mode = requested_mode;
        open_args(&c);
        f_file_open(&c);
        assert(c.r[0] == 1 && calls == 1 && LD32(out) == handle && c.r[13] == original_sp);
        assert(memory[out + 4] == 0xA5);  // Forge publishes precisely one u32.
    }
    c = fresh(OPEN); sdk_result = 0x202; open_args(&c); f_file_open(&c);
    assert(!c.r[0] && calls == 1 && LD32(out) == 0xA5A5A5A5 && c.r[13] == original_sp);
    for (uint32_t bad : {0u, 0x8000u, 0x7FFEu}) {
        c = fresh(OPEN); out = bad; open_args(&c); f_file_open(&c); no_call(c);
    }
    c = fresh(OPEN); path = 0; open_args(&c); f_file_open(&c); no_call(c);
    c = fresh(OPEN); path = 0x8000; open_args(&c); f_file_open(&c); no_call(c);
    c = fresh(OPEN); missing_symbol = OPEN; open_args(&c); f_file_open(&c); no_call(c);
    c = fresh(OPEN); original_sp = c.r[13] = 8; open_args(&c); f_file_open(&c); no_call(c);
    c = fresh(OPEN); original_sp = c.r[13] = 0x1008; open_args(&c); f_file_open(&c); no_call(c);

    c = fresh(SIZE); size_args(&c); f_file_size(&c);
    assert(c.r[0] == 1 && calls == 1 && LD32(out) == (uint32_t)file_size && LD32(out + 4) == file_size >> 32);
    c = fresh(SIZE); sdk_result = 0xBAD; size_args(&c); f_file_size(&c);
    assert(!c.r[0] && calls == 1 && LD32(out) == 0xA5A5A5A5);
    for (uint32_t bad : {0u, 0x8000u, 0x7FFCu}) {
        c = fresh(SIZE); out = bad; size_args(&c); f_file_size(&c); no_call(c);
    }
    c = fresh(SIZE); missing_symbol = SIZE; size_args(&c); f_file_size(&c); no_call(c);

    c = fresh(READ); read_args(&c); f_file_read(&c);
    assert(c.r[0] == 1 && calls == 1 && c.r[13] == original_sp);
    assert(!memcmp(memory + buffer, payload, count) && LD32(c.r[13]) == buffer && LD32(c.r[13] + 4) == count);
    c = fresh(READ); offset = UINT64_MAX; sdk_result = 0xBAD; read_args(&c); f_file_read(&c);
    assert(!c.r[0] && calls == 1 && memory[buffer] == 0xA5 && c.r[13] == original_sp);
    c = fresh(READ); buffer = count = 0; read_args(&c); f_file_read(&c);
    assert(c.r[0] == 1 && calls == 1);  // An empty read needs no writable bytes.
    for (uint32_t bad : {0u, 0x8000u, 0x7FFCu, 0xFFFFFFF8u}) {
        c = fresh(READ); buffer = bad; read_args(&c); f_file_read(&c); no_call(c);
    }
    c = fresh(READ); missing_symbol = READ; read_args(&c); f_file_read(&c); no_call(c);
    c = fresh(READ); c.r[13] = original_sp = 0x7FFC; f_file_read(&c); no_call(c);

    c = fresh(CLOSE); c.r[0] = handle; f_file_close(&c);
    assert(calls == 1 && c.r[13] == original_sp);
    c = fresh(CLOSE); missing_symbol = CLOSE; c.r[0] = handle; f_file_close(&c);
    assert(!calls && c.r[13] == original_sp);
    puts("PASS: real Forge file adapters; SDK success/failure, outputs, enum modes, AAPCS 64-bit offset/stack, buffer boundaries and close");
}
