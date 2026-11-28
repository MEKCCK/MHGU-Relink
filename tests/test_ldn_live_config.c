// Host end-to-end repro: the real runtime/net/ldn.c, config.c and Rust crossplay libraries.
// Boots with no RyuLdn server, saves one through config_save (what the settings panel does),
// then scans and hosts again WITHOUT re-running ldn_initialize (no restart).
#include "../runtime/net/ldn.c"
#include <sys/mman.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>

volatile uint64_t g_present_count;  // (runtime/ui.c's frame counter: config.c's model previews read it)
pthread_mutex_t g_klock = PTHREAD_MUTEX_INITIALIZER;
KEvent *kernel_create_event_pair(uint32_t *w, uint32_t *r) { static KEvent ev; *w = 1; *r = 2; return &ev; }
void kernel_signal(KObject *o) { (void)o; }
void guest_call(Cpu *c, uint32_t addr) { (void)c; (void)addr; }
uint32_t guest_sym(const char *name) { (void)name; return 0; }
void overlay_notice(const char *title, const char *text) { fprintf(stderr, "[notice] %s: %s\n", title, text); }
void emsync_aux_received(const void *data, uint32_t len) { (void)data; (void)len; }
void hitsync_aux_received(const void *data, uint32_t len) { (void)data; (void)len; }
void hpscale_aux_received(const void *data, uint32_t len) { (void)data; (void)len; }
void voice_aux_received(const void *data, uint32_t len) { (void)data; (void)len; }
void vsock_deliver(const VPacket *p, const void *data, uint32_t len) { (void)p; (void)data; (void)len; }
void vsock_network_down(void) {}
uint64_t plat_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
void plat_sleep_until_ns(uint64_t t) {
    for (;;) {
        uint64_t now = plat_now_ns();
        if (now >= t) return;
        struct timespec ts = {0, (long)((t - now) > 1000000 ? 1000000 : t - now)};
        nanosleep(&ts, NULL);
    }
}

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } \
    else fprintf(stderr, "ok line %d: %s\n", __LINE__, #x); } while (0)

static Cpu cpu;
static int scan(void) {
    memset(G2H(0x1000), 0, 16 * NETINFO_SIZE);
    ST32(0x2000, 0);
    memset(G2H(0x3000), 0, 0x80);
    cpu.r[0] = 0x1000; cpu.r[1] = 0x2000; cpu.r[2] = 16; cpu.r[3] = 0x3000;
    ldn_scan(&cpu);
    if (cpu.r[0]) fprintf(stderr, "scan result %#x\n", cpu.r[0]);
    return (int)LD32(0x2000);
}

int main(int argc, char **argv) {
    CHECK(argc == 3);  // ini path, stub port
    char ini[1024];
    snprintf(ini, sizeof ini, "%s", argv[1]);
    setenv("MHGU_CONFIG", ini, 1);
    FILE *f = fopen(ini, "wb");
    fputs("[Multiplayer]\nServer = Off\n", f);  // no internet hub server: a hermetic test
    fclose(f);
    CHECK(mmap((void *)GUEST_BASE, 0x1000000, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == (void *)GUEST_BASE);
    g_config = config_defaults();
    config_load();

    char addr[64];
    snprintf(addr, sizeof addr, "127.0.0.1:%s", argv[2]);
    pid_t host = fork();  // another game, already hosting a RyuLdn lobby on the stub server
    if (host == 0) {
        mhgu_ryu_set_config(addr, addr, "");
        uint8_t cfg[0x20] = {0}, sec[0x44] = {0}, user[0x30] = {0}, info[NETINFO_SIZE];
        cfg[0x12] = 4; cfg[0x14] = 1;
        memcpy(cfg, "\x00\x80\xDD\x08\x00\x77\x00\x01", 8);
        memcpy(user, "OtherHost", 9);
        uint32_t ip, mask;
        uint32_t r = mhgu_ryu_create(-1, cfg, sec, user, (const uint8_t *)"ADV!", 4, info, &ip, &mask);
        if (r) { fprintf(stderr, "[host] create failed %#x\n", r); _exit(1); }
        fprintf(stderr, "[host] lobby up\n");
        sleep(120);
        _exit(0);
    }
    sleep(2);

    ldn_initialize(&cpu);
    CHECK(cpu.r[0] == 0);
    ldn_open_station(&cpu);
    CHECK(cpu.r[0] == 0);
    CHECK(scan() == 0);  // crossplay off, no server: nothing to find

    // The player opens the settings, types the RyuLdn server, saves.
    Config c = config_editable();
    snprintf(c.ryu_servers, sizeof c.ryu_servers, "%s", addr);
    char error[256];
    CHECK(config_save(&c, error, sizeof error));

    // Back in local play (no ldn_finalize / ldn_initialize, no restart): the search must use it.
    int found = 0;
    for (int i = 0; i < 20 && !found; i++) {
        found = scan();
        if (!found) usleep(300000);
    }
    CHECK(found > 0);  // the other game's RyuLdn lobby shows up without a restart

    // Hosting must use the saved server too, without leaving local play.
    ldn_close_station(&cpu);
    CHECK(cpu.r[0] == 0);
    ldn_open_ap(&cpu);
    CHECK(cpu.r[0] == 0);
    memset(G2H(0x4000), 0, 0x20);
    ST8(0x4000 + 0x12, 4);
    ST8(0x4000 + 0x14, 1);
    memcpy(G2H(0x4000), "\x00\x80\xDD\x08\x00\x77\x00\x01", 8);
    memset(G2H(0x4100), 0, 0x44);
    memset(G2H(0x4200), 0, 0x30);
    memcpy(G2H(0x4200), "LiveHost", 8);
    cpu.r[0] = 0x4000; cpu.r[1] = 0x4100; cpu.r[2] = 0x4200;
    ldn_create(&cpu);
    CHECK(cpu.r[0] == 0);  // a RyuLdn lobby, not a native hub
    CHECK(ldn_crossplay());
    ldn_destroy(&cpu);
    CHECK(cpu.r[0] == 0);

    kill(host, SIGKILL);
    waitpid(host, NULL, 0);
    puts("PASS: RyuLdn server saved mid-session applies to the next scan and host, no restart");
    return 0;
}
