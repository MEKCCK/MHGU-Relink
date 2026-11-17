// The runner extracts the actual Plugin type, initialization and app_run code.
// Guest memory/callbacks and platform services below are host fixtures only.
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

struct Cpu { uint32_t r[16]; };
struct Module {
    const char *name;
    uint32_t base, text_off, text_size, init, init_array, init_array_sz;
};
#include "../runtime/forge.h"
#include "forge_plugin_types.inc"

static Plugin plugins[16];
static int nplugins;
static Module modules[16];
static std::array<uint8_t, 0x30000> memory;
static uint32_t heap_next, stub_next, main_text = 0x100000;
static constexpr uint32_t FORGE_END = 0x800;
static uint32_t original_args[4];
static int constructors[16], module_inits[16], loads[16], inits[16], original_calls;
static bool services_ready;
static int proc_registrations;
static std::vector<int> nvn_events;
static uint32_t nvn_args[3][4];

struct Case {
    uint16_t major, minor, patch;
    const char *title;
    bool active;
    bool no_load = false;
    bool unterminated = false;
};
static std::vector<Case> cases;

static uint8_t *guest(uint32_t address, size_t size) {
    assert(address && uint64_t(address) + size <= memory.size());
    return memory.data() + address;
}
static uint8_t LD8(uint32_t address) { return *guest(address, 1); }
static uint16_t LD16(uint32_t address) {
    uint16_t value; memcpy(&value, guest(address, 2), 2); return value;
}
static uint32_t LD32(uint32_t address) {
    uint32_t value; memcpy(&value, guest(address, 4), 4); return value;
}
static void ST8(uint32_t address, uint8_t value) { *guest(address, 1) = value; }
static void ST16(uint32_t address, uint16_t value) { memcpy(guest(address, 2), &value, 2); }
static void ST32(uint32_t address, uint32_t value) { memcpy(guest(address, 4), &value, 4); }
static const char *gstr(uint32_t address) {
    assert(memchr(guest(address, 64), 0, 64));
    return reinterpret_cast<const char *>(guest(address, 1));
}
static uint32_t hle_alloc(uint32_t size) {
    assert(size == 72);
    uint32_t result = heap_next;
    heap_next += 128;
    memset(guest(result, size), 0, size);
    return result;
}
static uint32_t slot_alloc(uint32_t *next, uint32_t end, uint32_t size, const char *) {
    assert(*next && *next + size <= end);
    uint32_t result = *next; *next += size; return result;
}
static void flog(int, const char *, ...) {}
#define LOG(...) ((void)0)
// The extracted lifecycle runs on one host thread; these stand in for compiler atomic intrinsics.
template<class T> static T fixture_atomic_load(const T *value, int) { return *value; }
template<class T> static void fixture_atomic_store(T *out, T value, int) { *out = value; }
#define __atomic_load_n fixture_atomic_load
#define __atomic_store_n fixture_atomic_store
#define __ATOMIC_ACQUIRE 0
#define __ATOMIC_RELEASE 0
using HostFn = void (*)(Cpu *);
static void device_get_proc(Cpu *) {}
static HostFn hle_lookup_impl(const char *name) {
    assert(!strcmp(name, "nvnDeviceGetProcAddress")); return device_get_proc;
}
static uint32_t hle_register(const char *name, HostFn function) {
    assert(!strcmp(name, "nvnDeviceGetProcAddress") && function == device_get_proc);
    ++proc_registrations; return 0xF000;
}

static void clobber(Cpu *cpu) {
    for (int r = 0; r < 4; ++r) cpu->r[r] = 0xDEAD0000 + r;
}
static void singletons_resolve(Cpu *cpu, bool resolve) {
    assert(resolve && !services_ready); clobber(cpu);
}
static uint32_t f_sock_init_default_value(Cpu *cpu) {
    services_ready = true; clobber(cpu); return 0;
}
static void fnhook_call_original(Cpu *cpu, uint32_t address) {
    assert(address == main_text + OFF_APP_RUN);
    assert(!memcmp(cpu->r, original_args, sizeof original_args));
    ++original_calls;
    cpu->r[0] = 0x12345678;
}
static void guest_call(Cpu *cpu, uint32_t function) {
    assert(services_ready);
    for (int i = 0; i < nplugins; ++i) {
        for (int phase = 0; phase < 3; ++phase)
            if (plugins[i].on_nvn[phase] && function == plugins[i].on_nvn[phase]) {
                nvn_events.push_back(i * 10 + phase);
                memcpy(nvn_args[phase], cpu->r, sizeof nvn_args[phase]);
                memset(cpu, 0x5A, sizeof *cpu); return;
            }
        Module &module = modules[i];
        if (function == module.init) {
            assert(nop_fn && LD32(nop_fn) == 0xE12FFF1E);
            for (int k = 0; k < 4; ++k) assert(LD32(module.init_array + 4 * k) == nop_fn);
            ++module_inits[i]; clobber(cpu); return;
        }
        if (function == module.base + module.text_off + 4 ||
            function == module.base + module.text_off + 8) {
            assert(module_inits[i] == 1);
            ++constructors[i]; clobber(cpu); return;
        }
        if (function == plugins[i].on_load) {
            for (int k = 0; k < nplugins; ++k) assert(module_inits[k] == 1 && constructors[k] == 2);
            uint32_t param = cpu->r[0];
            assert(param == plugins[i].param && !cpu->r[1] && !cpu->r[2] && !cpu->r[3]);
            for (int k = 0; k < 72; ++k) assert(!LD8(param + k));
            ST16(param, cases[i].major); ST16(param + 2, cases[i].minor); ST16(param + 4, cases[i].patch);
            if (cases[i].unterminated) memset(guest(param + 6, 64), 'Q', 64);
            else memcpy(guest(param + 6, 64), cases[i].title, strlen(cases[i].title) + 1);
            ST8(param + 70, 0xA5); ST8(param + 71, 0x5A);
            ++loads[i]; clobber(cpu); return;
        }
        if (function == plugins[i].on_init) {
            assert(loads[i] == 1 && plugins[i].active);
            assert(!cpu->r[0] && !cpu->r[1] && !cpu->r[2] && !cpu->r[3]);
            ++inits[i]; clobber(cpu); return;
        }
    }
    assert(false && "Unexpected guest callback");
}

#include "forge_plugin_runtime.inc"

static void run(std::vector<Case> scenarios) {
    cases = std::move(scenarios);
    nplugins = static_cast<int>(cases.size());
    assert(nplugins <= 16);
    memory.fill(0xCC); memset(plugins, 0, sizeof plugins); memset(modules, 0, sizeof modules);
    memset(constructors, 0, sizeof constructors); memset(module_inits, 0, sizeof module_inits);
    memset(loads, 0, sizeof loads); memset(inits, 0, sizeof inits);
    heap_next = 0x1000; stub_next = 0x100; nop_fn = 0; original_calls = 0; services_ready = false;
    for (int i = 0; i < nplugins; ++i) {
        Module &module = modules[i];
        module.name = "fixture.nro"; module.base = 0x10000 + 0x1000 * i;
        module.text_off = 0x100; module.text_size = 0x400; module.init = module.base + module.text_off;
        module.init_array = 0x8000 + 32 * i; module.init_array_sz = 16;
        ST32(module.init_array, module.init + 4); ST32(module.init_array + 4, 0);
        ST32(module.init_array + 8, module.init + 8); ST32(module.init_array + 12, module.init + 5);
        snprintf(plugins[i].name, sizeof plugins[i].name, "fixture-%d.nro", i);
        plugins[i].m = &module;
        plugins[i].on_load = cases[i].no_load ? 0 : 0x400000 + 16 * i;
        plugins[i].on_init = 0x400004 + 16 * i;
    }
    Cpu cpu = {};
    for (int i = 0; i < 4; ++i) cpu.r[i] = original_args[i] = 0xA0000000 + i;
    app_run(&cpu);
    assert(original_calls == 1 && cpu.r[0] == 0x12345678);
    std::vector<std::string> expected_titles;
    for (int i = 0; i < nplugins; ++i) {
        const Case &item = cases[i]; const Plugin &plugin = plugins[i];
        assert(module_inits[i] == 1 && constructors[i] == 2);
        assert(LD32(modules[i].init_array) == modules[i].init + 4);
        assert(LD32(modules[i].init_array + 12) == modules[i].init + 5);
        assert(plugin.active == item.active && plugin.param);
        assert(loads[i] == !item.no_load && inits[i] == (item.active && !item.no_load));
        if (item.no_load) { assert(!LD8(plugin.param + 6)); continue; }
        assert(LD16(plugin.param) == item.major && LD16(plugin.param + 2) == item.minor && LD16(plugin.param + 4) == item.patch);
        assert(!LD8(plugin.param + 69) && LD8(plugin.param + 70) == 0xA5 && LD8(plugin.param + 71) == 0x5A);
        std::string expected = item.unterminated ? std::string(63, 'Q') : item.title;
        assert(gstr(plugin.param + 6) == expected);
        if (item.active && !expected.empty()) expected_titles.push_back(expected);
    }
    const char *titles[16] = {};
    int count = forge_plugin_titles(titles, 16);
    assert(count == static_cast<int>(expected_titles.size()));
    for (int i = 0; i < count; ++i) assert(titles[i] == expected_titles[i]);
    assert(forge_plugin_titles(titles, 1) == (expected_titles.empty() ? 0 : 1));
}

static void graphics_lifecycle() {
    memset(plugins, 0, sizeof plugins);
    nplugins = 0; nvn_device = nvn_queue = 0; nvn_started = nvn_ready = false;
    proc_registrations = 0; nvn_events.clear(); services_ready = true;
    assert(forge_plugins_graphics_ready());  // A legacy-only setup needs no NVN device/queue.
    nplugins = 2;
    for (int i = 0; i < nplugins; ++i)
        for (int phase = 0; phase < 3; ++phase) plugins[i].on_nvn[phase] = 0x500000 + 16 * i + phase;
    assert(forge_plugins_graphics_ready());  // Disabled callback owners do not delay legacy plugins.
    plugins[0].active = true;
    assert(!forge_plugins_graphics_ready());
    Cpu cpu, saved; memset(&cpu, 0xA5, sizeof cpu); saved = cpu;
    forge_plugins_nvn_init(&cpu); forge_plugins_nvn_ready(&cpu);
    assert(!nvn_started && !nvn_ready && nvn_events.empty());
    forge_nvn_device(0x1234);
    assert(!forge_plugins_graphics_ready());
    forge_nvn_present(&cpu, 0x2345, 0x3456, 7);
    assert(forge_plugins_graphics_ready() && nvn_events.empty());
    forge_plugins_nvn_init(&cpu); forge_plugins_nvn_init(&cpu);
    assert(nvn_started && !nvn_ready && proc_registrations == 1 && nvn_events == std::vector<int>({0}));
    assert(nvn_args[0][0] == 0xF000 && nvn_args[0][1] == 0x1234 && nvn_args[0][2] == 0x2345 && !nvn_args[0][3]);
    assert(!memcmp(&cpu, &saved, sizeof cpu));
    forge_plugins_nvn_ready(&cpu); forge_plugins_nvn_ready(&cpu);
    assert(nvn_ready && nvn_events == std::vector<int>({0, 1}));
    for (uint32_t argument : nvn_args[1]) assert(!argument);
    forge_nvn_present(&cpu, 0x4567, 0x5678, 3);
    assert(nvn_events == std::vector<int>({0, 1, 2}));
    assert(nvn_args[2][0] == 0x5678 && nvn_args[2][1] == 3 && !nvn_args[2][2] && !nvn_args[2][3]);
    assert(!memcmp(&cpu, &saved, sizeof cpu));
    puts("PASS: production NVN lifecycle waits for device/queue, ignores disabled callbacks, runs Init/Ready once before Render, and preserves full Cpu state");
}

int main() {
    assert(FORGE_MAJOR == 2 && FORGE_MINOR == 0 && FORGE_PATCH == 0);
    _putenv_s("MHGU_FORGE_HFR", "");
    run({{1, 5, 1, "Legacy 1.5.1", true}, {1, 5, 9, "Legacy 1.5.9", true},
         {2, 0, 0, "Forge 2.0.0", true}, {2, 1, 0, "Future 2.1", false},
         {3, 0, 0, "Future 3.0", false}, {2, 0, 0, "Lag Compensation", false},
         {2, 0, 0, "High Frame Rate Fixes", false}, {2, 0, 65535, "", true, false, true},
         {0, 0, 0, "", true, true}});
    puts("PASS: production app_run accepts 1.5.1/1.5.9/2.0.0, rejects 2.1/3.0, preserves initializer ordering and no-onLoad lifecycle");
    puts("PASS: PluginInitParam name stays at +6, unterminated names stop at 63 bytes, padding survives, active titles respect output bounds");
    puts("PASS: built-in Lag Compensation and High Frame Rate Fixes plugins remain disabled");
    _putenv_s("MHGU_FORGE_HFR", "1");
    run({{2, 0, 0, "High Frame Rate Fixes", true}});
    _putenv_s("MHGU_FORGE_HFR", "");
    puts("PASS: existing MHGU_FORGE_HFR=1 test override remains available; original app arguments and return value survive");
    graphics_lifecycle();
}
