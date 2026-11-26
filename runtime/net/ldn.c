// nn::ldn (local wireless play) over IP. The game that creates a network (the hub's host, an "access point") listens
// on a TCP port (ini [Multiplayer] Port); the others ("stations") find it with Scan - a LAN broadcast plus the
// addresses in ini [Multiplayer] Join - and Connect to it. Each station keeps that one connection: the host hands
// out node numbers and addresses (169.254.x.n, the host n = 1), sends the network's NetworkInfo whenever it changes
// (players joining or leaving, the hub's advertise data), and relays the game's packets between the players
// (net.h, vsock.c). Only the host needs a reachable port.
//
// Internet hubs (ini [Multiplayer] Server, crates/mhgu-hub in the repository): with a hub server, a created hub is also listed
// there, for players with the same matchmaking key (ini Key, empty: public) and port version, and Scan adds the
// server's hubs to the LAN's. Joining one goes through the server, which splices the player's connection to one the
// host opens for it: the same link as a direct connection, so nobody forwards a port. Game packets go by UDP through
// the server too ("MHGR" relay prefix), else through the link.
//
// Link frames: u32 size (of what follows), u8 type, payload. Structures are the SDK's (sizes and offsets below).
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <stdarg.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#ifdef __ANDROID__
#include <jni.h>
#endif

#include "../../updater/updater.h"  // (sha256)
#include "../config.h"
#include "../hle.h"
#include "../kernel.h"
#include "../overlay.h"
#include "../settings_locale.h"
#include "../voice.h"
#include "discord.h"  // (presence_hunter_name)
#include "net.h"
#include "mhgu_link.h"  /* the Rust online module (crates/mhgu-link-ffi) */
// Each caller thread owns one snapshot of the saved crossplay settings.
static __thread Config xp_config;
static void crossplay_snapshot(void) { config_live_crossplay(&xp_config); }

// ---- crossplay: the Rust modules (crates/mhgu-crossplay), their C API
typedef struct { uint32_t src_ip, dst_ip; uint16_t src_port, dst_port; uint32_t proto; } RyuProxy;
extern bool mhgu_ryu_enabled(void);
extern void mhgu_ryu_set_config(const char *servers, const char *host, const char *pass);
extern void mhgu_ryu_set_self(const char *addr);
extern void mhgu_ryu_set_hooks(void (*lobby)(const uint8_t *), void (*data)(const RyuProxy *, const uint8_t *, uint32_t));
extern int mhgu_ryu_server_count(void);
extern int mhgu_ryu_scan(const uint8_t *filter, uint8_t *infos, int *server_of, int max);
extern uint32_t mhgu_ryu_create(int server, const uint8_t *net, const uint8_t *sec, const uint8_t *user,
                                const uint8_t *adv, uint32_t adv_size, uint8_t *info, uint32_t *ip, uint32_t *mask);
extern uint32_t mhgu_ryu_connect(int server, const uint8_t *info, const uint8_t *sec, const uint8_t *user, uint16_t lcv,
                                 uint8_t *out, uint32_t *ip, uint32_t *mask);
extern void mhgu_ryu_leave(void);
extern bool mhgu_ryu_in_lobby(void);
extern void mhgu_ryu_set_advertise(const uint8_t *data, uint32_t len);
extern void mhgu_ryu_set_accept_policy(uint8_t policy);
extern bool mhgu_ryu_reject(uint32_t node_id, uint32_t reason);
extern void mhgu_ryu_send(const RyuProxy *p, const uint8_t *data, uint32_t len);
extern bool mhgu_ryusrv_start(const char *addr, uint16_t port, char *err, int err_size);
extern void mhgu_ryusrv_stop(void);
extern bool mhgu_ryusrv_running(void);

typedef void (*RoomLdnCb)(uint8_t, const uint8_t *, const uint8_t *, bool, const uint8_t *, uint32_t);
typedef void (*RoomProxyCb)(const uint8_t *, uint16_t, const uint8_t *, uint16_t, uint8_t, bool, const uint8_t *, uint32_t);
extern void mhgu_room_set_hooks(RoomLdnCb ldn, RoomProxyCb proxy, void (*lost)(void), void (*members)(const uint8_t (*)[4], int));
extern bool mhgu_room_join(const char *server, const char *nick, const char *password, uint8_t *ip, char *err, int err_size);
extern void mhgu_room_leave(void);
extern bool mhgu_room_connected(void);
extern void mhgu_room_own_ip(uint8_t *ip);
extern void mhgu_room_send_ldn(uint8_t type, const uint8_t *lip, const uint8_t *rip, bool bc, const uint8_t *data, uint32_t len);
extern void mhgu_room_send_proxy(const uint8_t *sip, uint16_t sport, const uint8_t *dip, uint16_t dport, uint8_t proto, bool bc,
                                 const uint8_t *data, uint32_t len);
extern bool mhgu_roomsrv_start(int port, const char *name, const char *password, const char *host_nick, char *err, int err_size);
extern void mhgu_roomsrv_stop(void);
extern bool mhgu_roomsrv_running(void);

extern void mhgu_xb_begin(const uint8_t *info, uint32_t host_ip, uint32_t mask);
extern void mhgu_xb_end(void);
extern bool mhgu_xb_active(void);
extern int mhgu_xb_players(void);
extern void mhgu_xb_lobby(const uint8_t *info);
extern void mhgu_xb_room_ldn(uint8_t type, const uint8_t *lip, const uint8_t *rip, bool bc, const uint8_t *data, uint32_t len);
extern void mhgu_xb_room_members(const uint8_t (*ips)[4], int n);
extern void mhgu_xb_room_proxy(const uint8_t *sip, uint16_t sport, const uint8_t *dip, uint16_t dport, uint8_t proto, bool bc,
                               const uint8_t *data, uint32_t len);


#ifdef _WIN32
typedef SOCKET hsock;
#define BADSOCK INVALID_SOCKET
#define hclose closesocket
static void hnet_init(void) {
    static int done;
    if (!done) { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); done = 1; }
}
static void hset_nonblock(hsock s, bool on) { u_long v = on; ioctlsocket(s, FIONBIO, &v); }
static void hset_timeout(hsock s, int ms) {
    DWORD v = (DWORD)ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&v, sizeof v);
}
#else
typedef int hsock;
#define BADSOCK (-1)
#define hclose close
static void hnet_init(void) {}
static void hset_nonblock(hsock s, bool on) {
    int f = fcntl(s, F_GETFL);
    fcntl(s, F_SETFL, on ? f | O_NONBLOCK : f & ~O_NONBLOCK);
}
static void hset_timeout(hsock s, int ms) {
    struct timeval tv = {ms / 1000, (ms % 1000) * 1000};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
}
#endif

// ---- SDK structures (bytes)
#define NETINFO_SIZE 0x480
enum {
    NI_LOCAL_COMM_ID = 0x00, NI_SCENE_ID = 0x0A, NI_SESSION_ID = 0x10, NI_BSSID = 0x20, NI_SSID = 0x26,
    NI_CHANNEL = 0x48, NI_LINK_LEVEL = 0x4A, NI_NETWORK_TYPE = 0x4B, NI_SEC_PARAM = 0x50, NI_SEC_MODE = 0x60,
    NI_ACCEPT_POLICY = 0x62, NI_NODE_MAX = 0x66, NI_NODE_COUNT = 0x67, NI_NODES = 0x68, NI_ADV_SIZE = 0x26A,
    NI_ADV = 0x26C, NI_AUTH_ID = 0x478,
};
#define NODE_SIZE 0x40
enum { NODE_IP = 0x00, NODE_MAC = 0x04, NODE_ID = 0x0A, NODE_CONNECTED = 0x0B, NODE_NAME = 0x0C, NODE_LCV = 0x2E };
#define MAX_NODES 8
#define ADV_MAX 0x180
// suyu-family room LDN packets (crates/mhgu-crossplay/src/room.rs)
enum { RLDN_SCAN, RLDN_SCAN_RESP, RLDN_CONNECT, RLDN_SYNC_NETWORK, RLDN_DISCONNECT, RLDN_DESTROY_NETWORK };
// NetworkConfig (0x20): IntentId 0x10, u16 channel, u8 node count max, u8 -, u16 local communication version
// SecurityConfig (0x44): u16 mode, u16 passphrase size, passphrase[0x40]; UserConfig (0x30): user name[0x21]
// SecurityParameter (0x20): data[0x10], session id[0x10]
// ScanFilter (0x60): NetworkId 0x20, u32 network type, bssid[6], ssid 0x22, -[0x10], u32 flags
enum { SF_LOCAL_COMM_ID = 1, SF_SESSION_ID = 2, SF_NETWORK_TYPE = 4, SF_BSSID = 8, SF_SSID = 0x10, SF_SCENE_ID = 0x20 };

enum { ST_NONE, ST_INITIALIZED, ST_ACCESS_POINT, ST_AP_CREATED, ST_STATION, ST_STATION_CONNECTED, ST_ERROR };
enum { DR_NONE, DR_BY_USER, DR_BY_SYSTEM, DR_DESTROYED_BY_USER, DR_DESTROYED_BY_SYSTEM, DR_REJECTED, DR_SIGNAL_LOST };
#define LDN_RESULT(d) MAKE_RESULT(203, d)
#define R_INVALID_STATE LDN_RESULT(32)
#define R_CONNECT_FAILURE LDN_RESULT(64)
#define R_CONNECT_NOT_FOUND LDN_RESULT(65)
#define R_CONNECT_TIMEOUT LDN_RESULT(66)
#define R_CONNECT_REJECTED LDN_RESULT(67)
#define R_VERSION_TOO_LOW LDN_RESULT(113)
#define R_VERSION_TOO_HIGH LDN_RESULT(114)
#define R_TOO_MANY_PLAYERS LDN_RESULT(144)

// ---- link protocol
// Version 4 plays with 3: the link frames are the same (4 adds only what the hub server does: session codes, join by
// code, friends), so a host takes a hello of either, a LAN probe of either is answered in its own version, and a
// station says to each host the version that host speaks (from the hub server's list; 3 for a host found on the LAN
// or by address, which every host of 3 or 4 takes). The hub server lists 3 and 4 together.
#define PROTO_MAGIC "MHGL"
#define PROTO_VERSION 4
#define PROTO_MIN 3
static bool link_version_ok(uint16_t v) { return v >= PROTO_MIN && v <= PROTO_VERSION; }
enum { F_HELLO = 1, F_INFO, F_WELCOME, F_REFUSE, F_PACKET, F_DESTROY, F_AUX };  // F_AUX: the port's own messages (emsync.c, hle/hpscale.c)
typedef struct {
    char magic[4];
    uint16_t version;
    uint8_t scan_only, pad;
    uint16_t lcv;
    uint8_t mac[6];
    char name[0x21];
    uint8_t pad2[3];
} Hello;
#define MAX_FRAME (0x10000 + 64)

// The game's packets go by UDP when it gets through (Pia, the game's network library, expects datagrams: over TCP
// one lost packet would hold up everything behind it): datagrams "MHGD" + token + kind [+ VPacket + data] between
// a station and the host's port. A station says hello every 2 s (keeping NAT mappings open), the host answers;
// without UDP (blocked, not forwarded) or after 15 s without any, the packets go through the TCP link instead.
#define UDP_MAGIC "MHGD"
enum { U_DATA = 0, U_HELLO, U_HELLO_ACK };
typedef struct { char magic[4]; uint32_t token; uint8_t kind, pad[3]; } UHdr;

// The hub server (crates/mhgu-hub/src/server.rs has the protocol): control frames like the link's; requests start with a header
// "MHGS", u16 LOBBY_PROTO, u16 PROTO_VERSION, room[16] (the matchmaking key's hash).
#define LOBBY_PROTO 1
#define LOBBY_PORT 39510
#define LOBBY_HEADER 24
enum { S_LIST = 1, S_HOST, S_ATTACH, S_JOIN, S_UPDATE, S_PING, S_FIND, S_PRESENCE, S_HUBS = 16, S_HOSTED, S_INCOMING,
       S_SPLICED, S_ERROR, S_HUBS4, S_FRIENDS };
enum { SE_NOT_FOUND = 1, SE_NO_ANSWER, SE_BUSY, SE_VERSION, SE_BAD_REQUEST, SE_CODE_TAKEN };
// Session codes (link version 4): 8 characters of Crockford's base 32 that a host picks for its network and sends
// with its HOST; the hub server finds the hub by it (FIND) and lets a player join with it from any room. Friend codes:
// the first 10 bytes of SHA-256("MHGURecomp friend:" + this install's secret), shown as 16 of the same characters.
#define CODE_LEN 8
#define CODE_ALPHABET "0123456789ABCDEFGHJKMNPQRSTVWXYZ"
#define FRIEND_LEN 10
#define FRIEND_SECRET 32


static struct {
    pthread_mutex_t mu;
    int state, reason;
    uint8_t info[NETINFO_SIZE];
    uint32_t own_ip, subnet;
    uint8_t mac[6];
    uint8_t adv[ADV_MAX];
    uint32_t adv_size;
    int accept_policy;
    uint16_t lcv;
    KEvent *ev;
    uint32_t ev_r;
    // host
    bool hosting;
    int port;  // [Multiplayer] Port as it was when this hub started (a Save changes the next hub's, not this one's)
    // crossplay
    bool ryu;                    // the network is a lobby on a RyuLdn server
    uint32_t ryu_mask;
    bool room;                   // the network is a hub in a suyu-family room
    bool room_client[MAX_NODES]; // host: the room players in the hub, by node
    uint8_t room_cip[MAX_NODES][4];
} L = {.mu = PTHREAD_MUTEX_INITIALIZER};

// Scan results: where each found network's host is (hub: on the hub server at addr, 0: the host's own address).
// ver: the link version to say to its host; code: its session code (hub server, version 4), "" when not known.
#define MAX_FOUND 32
static struct { uint8_t bssid[6], session[16]; struct sockaddr_in addr; uint64_t hub; uint16_t ver;
                char code[CODE_LEN + 1]; uint8_t info[NETINFO_SIZE]; int ryu; bool room; } found[MAX_FOUND];
static int nfound;

static void signal_change(void) {
    if (!L.ev) return;
    pthread_mutex_lock(&g_klock);
    kernel_signal(L.ev);
    pthread_mutex_unlock(&g_klock);
}

static void rand_bytes(void *p, size_t n) {
    for (size_t i = 0; i < n; i++) ((uint8_t *)p)[i] = (uint8_t)arc4random();
}
static const char *ipstr(uint32_t ip) {
    static __thread char b[2][20];
    static __thread int k;
    char *s = b[k++ & 1];
    snprintf(s, 20, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
    return s;
}
static uint8_t *node_at(uint8_t *info, int i) { return info + NI_NODES + i * NODE_SIZE; }
static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void wr16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }
static void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

// A NetworkInfo from the network (LAN, hub server, host), before the game sees it: the game trusts it as it came from
// the system. Counts and sizes in range, strings terminated, node ids = their index (free entries all zero, as ours).
static bool info_sane(uint8_t *info) {
    uint8_t max = info[NI_NODE_MAX], n = info[NI_NODE_COUNT];
    if (info[NI_SSID] > 32 || max < 1 || max > MAX_NODES || n < 1 || n > max || rd16(info + NI_ADV_SIZE) > ADV_MAX)
        return false;
    info[NI_SSID + 0x21] = 0;
    for (int i = 0; i < MAX_NODES; i++) {
        uint8_t *nd = node_at(info, i);
        if (!nd[NODE_CONNECTED]) { memset(nd, 0, NODE_SIZE); continue; }
        nd[NODE_CONNECTED] = 1;
        nd[NODE_ID] = (uint8_t)i;
        nd[NODE_NAME + 0x20] = 0;
    }
    return true;
}

#define ONCE(...) do { static int once_; if (!once_++) LOG(__VA_ARGS__); } while (0)

// A thread that ends by itself, nobody waiting for it. Detached threads leak a handle each in winpthreads (14.0), and
// under Hunter Search some start every beacon (the hub's listing, the search's list request): so they are created
// joinable, and each start first joins those that said they are done (a join of a thread that has ended returns at
// once). False: not started.
#define SPAWN_MAX 64
typedef struct { void *(*fn)(void *); void *arg; int slot; } SpawnArg;
static pthread_mutex_t spawn_mu = PTHREAD_MUTEX_INITIALIZER;
static struct { pthread_t th; bool used; volatile bool done; } spawned[SPAWN_MAX];
static void *spawn_main(void *p) {
    SpawnArg a = *(SpawnArg *)p;
    free(p);
    void *r = a.fn(a.arg);
    if (a.slot >= 0) __atomic_store_n(&spawned[a.slot].done, true, __ATOMIC_RELEASE);
    return r;
}
static bool spawn(void *(*fn)(void *), void *arg) {
    SpawnArg *a = malloc(sizeof *a);
    if (!a) return false;
    a->fn = fn, a->arg = arg, a->slot = -1;
    pthread_mutex_lock(&spawn_mu);
    for (int i = 0; i < SPAWN_MAX; i++) {
        if (spawned[i].used && __atomic_load_n(&spawned[i].done, __ATOMIC_ACQUIRE)) {
            pthread_join(spawned[i].th, NULL);
            spawned[i].used = false;
        }
        if (!spawned[i].used && a->slot < 0) a->slot = i;
    }
    pthread_t th;
    bool ok;
    if (a->slot >= 0) {  // (all 64 still running: detached after all)
        spawned[a->slot].used = true, spawned[a->slot].done = false;
        ok = !pthread_create(&spawned[a->slot].th, NULL, spawn_main, a);
        if (!ok) spawned[a->slot].used = false;
    } else if ((ok = !pthread_create(&th, NULL, spawn_main, a))) {
        pthread_detach(th);
    }
    pthread_mutex_unlock(&spawn_mu);
    if (!ok) free(a);
    return ok;
}

// ---- frames
static bool send_all(hsock s, const void *b, size_t n) {
    for (const char *p = b; n;) {
        int k = send(s, p, (int)n, 0);
        if (k <= 0) return false;
        p += k, n -= (size_t)k;
    }
    return true;
}
static bool recv_all(hsock s, void *b, size_t n) {
    for (char *p = b; n;) {
        int k = recv(s, p, (int)n, 0);
        if (k <= 0) return false;
        p += k, n -= (size_t)k;
    }
    return true;
}
static bool frame_send(hsock s, uint8_t type, const void *a, uint32_t alen, const void *b, uint32_t blen) {
    uint8_t h[5];
    uint32_t size = 1 + alen + blen;
    memcpy(h, &size, 4);
    h[4] = type;
    return send_all(s, h, 5) && (!alen || send_all(s, a, alen)) && (!blen || send_all(s, b, blen));
}
// -1: closed / error; else the payload size (buf holds MAX_FRAME bytes)
static int frame_recv(hsock s, uint8_t *type, uint8_t *buf) {
    uint32_t size;
    if (!recv_all(s, &size, 4) || size < 1 || size > MAX_FRAME + 1 || !recv_all(s, type, 1)) return -1;
    return recv_all(s, buf, size - 1) ? (int)size - 1 : -1;
}

static hsock connect_to(const struct sockaddr_in *a, int timeout_ms) {
    hsock s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == BADSOCK) return BADSOCK;
    hset_nonblock(s, true);
    connect(s, (const struct sockaddr *)a, sizeof *a);
    fd_set w, e;
    FD_ZERO(&w);
    FD_ZERO(&e);
    FD_SET(s, &w);
    FD_SET(s, &e);
    struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    int err = 1;
    socklen_t el = sizeof err;
    if (select((int)s + 1, NULL, &w, &e, &tv) <= 0 || !FD_ISSET(s, &w) ||
        getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &el) || err) {
        hclose(s);
        return BADSOCK;
    }
    hset_nonblock(s, false);
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    return s;
}

// Through the hub server, a datagram starts with "MHGR", the pair and our secret; the server passes on the rest.
// The host tells the server where its end of a relayed link is (until the station's datagrams come through).

// "host[:port]" (port: def without one) -> its IPv4 address
static bool resolve_host(const char *text, int def, struct sockaddr_in *out) {
    char host[256];
    int port = def;
    snprintf(host, sizeof host, "%s", text);
    char *colon = strrchr(host, ':');
    if (colon) { *colon = 0; port = atoi(colon + 1); }
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (port < 1 || port > 65535 || getaddrinfo(host, NULL, &hints, &res) || !res) return false;
    *out = *(struct sockaddr_in *)res->ai_addr;
    out->sin_port = htons((uint16_t)port);
    freeaddrinfo(res);
    return true;
}

// One datagram on link l: header [+ packet]. to = NULL: s is connected (a station's socket).

static Hello make_hello(bool scan_only, const uint8_t *user_name) {
    Hello h = {0};
    memcpy(h.magic, PROTO_MAGIC, 4);
    h.version = PROTO_VERSION;
    h.scan_only = scan_only;
    h.lcv = L.lcv;
    memcpy(h.mac, L.mac, 6);
    if (user_name) memcpy(h.name, user_name, 0x20);
    return h;
}

// ---- the virtual network's packets (vsock.c -> here)
uint32_t ldn_own_ip(void) { return L.own_ip; }
uint32_t ldn_broadcast_ip(void) { return L.subnet ? L.subnet | (L.ryu && L.ryu_mask ? ~L.ryu_mask : 0xFFu) : 0xFFFFFFFFu; }
bool ldn_net_up(void) { return L.state == ST_AP_CREATED || L.state == ST_STATION_CONNECTED; }
// No network, and none for a while: Hunter Search closes its network for each scan (~10 s apart) and opens it again
static uint64_t g_net_left_ns;  // when the game last closed a network or a search (to_initialized)
#define OFFLINE_AFTER_NS 15000000000ull
bool ldn_offline(void) {
    int s = __atomic_load_n(&L.state, __ATOMIC_ACQUIRE);
    uint64_t left = __atomic_load_n(&g_net_left_ns, __ATOMIC_ACQUIRE);
    return (s == ST_NONE || s == ST_INITIALIZED) && (!left || plat_now_ns() - left >= OFFLINE_AFTER_NS);
}
static bool ldn_multiplayer_active(void) {
    return ldn_net_up() || mhgu_room_connected() || mhgu_ryu_in_lobby()
        || mhgu_roomsrv_running() || mhgu_ryusrv_running();
}
#ifdef __ANDROID__
JNIEXPORT jboolean JNICALL Java_com_mhgu_relink_GameActivity_nativeIsMultiplayerActive(JNIEnv *env, jclass cls) {
    (void)env;
    (void)cls;
    return ldn_multiplayer_active() ? JNI_TRUE : JNI_FALSE;
}
#endif

bool ldn_hosting(void) { return L.state == ST_AP_CREATED; }
bool ldn_crossplay(void) { return (L.ryu || L.room) && ldn_net_up(); }  // (a lobby that is not up yet is not crossplay)
bool ldn_party(int *n, int *max, char *id, size_t size) {
    pthread_mutex_lock(&L.mu);
    bool up = ldn_net_up();
    if (up) {
        if (!L.ryu && !L.room) mhgu_ldn_info(L.info, NETINFO_SIZE);
        *n = L.info[NI_NODE_COUNT], *max = L.info[NI_NODE_MAX];
        Sha256 h;  // (a hash of the session id: Discord shows the party id to others, the network's own id stays here)
        char hex[65];
        sha256_init(&h);
        sha256_update(&h, "mhgu-party", 10);
        sha256_update(&h, L.info + NI_SESSION_ID, 16);
        sha256_hex(&h, hex);
        snprintf(id, size, "%.32s", hex);
    }
    pthread_mutex_unlock(&L.mu);
    return up;
}


static void ip_bytes(uint32_t ip, uint8_t b[4]) { b[0] = (uint8_t)(ip >> 24); b[1] = (uint8_t)(ip >> 16); b[2] = (uint8_t)(ip >> 8); b[3] = (uint8_t)ip; }
static void ryu_xmit(const VPacket *p, const void *data, uint32_t len) {
    if (p->proto != VPROTO_UDP) return;  // (the game's virtual network is UDP; the server relays no TCP)
    bool all = p->dst_ip == ldn_broadcast_ip() || p->dst_ip == 0xFFFFFFFFu;
    RyuProxy r = {p->src_ip, all ? ldn_broadcast_ip() : p->dst_ip, p->src_port, p->dst_port, 17};
    mhgu_ryu_send(&r, data, len);
}
static void room_xmit(const VPacket *p, const void *data, uint32_t len) {
    if (p->proto != VPROTO_UDP) return;
    bool all = p->dst_ip == ldn_broadcast_ip() || p->dst_ip == 0xFFFFFFFFu;
    // (a suyu room relays a broadcast to the other members only: the sender's own copy comes from here)
    if (all || p->dst_ip == L.own_ip) vsock_deliver(p, data, len);
    if (p->dst_ip == L.own_ip) return;
    uint8_t s[4], d[4];
    ip_bytes(p->src_ip, s);
    ip_bytes(p->dst_ip, d);
    mhgu_room_send_proxy(s, p->src_port, d, p->dst_port, 4, all, data, len);
}
void ldn_send(const VPacket *p, const void *data, uint32_t len) {
    if (L.ryu || L.room) {
        if (!ldn_net_up()) return;
        if (L.ryu) ryu_xmit(p, data, len); else room_xmit(p, data, len);
        return;
    }
    if (L.state == ST_AP_CREATED) {
        bool all = p->dst_ip == ldn_broadcast_ip() || p->dst_ip == 0xFFFFFFFFu;
        if (all || p->dst_ip == L.own_ip) vsock_deliver(p, data, len);  // (a broadcast reaches the sender too)
        if (p->dst_ip == L.own_ip) return;
        MhguVPacket mp;
        memcpy(&mp, p, sizeof mp);
        mhgu_ldn_send(&mp, data, len);
        return;
    }
    if (L.state != ST_STATION_CONNECTED) return;
    bool all = p->dst_ip == ldn_broadcast_ip() || p->dst_ip == 0xFFFFFFFFu;
    if (all || p->dst_ip == L.own_ip) vsock_deliver(p, data, len);  // (a broadcast reaches the sender too)
    if (p->dst_ip == L.own_ip) return;
    // The station's one link is the Rust link (mhgu-link): it sends by UDP through the hub relay when the
    // host's datagrams get through, else over the link's TCP.
    MhguVPacket mp;
    memcpy(&mp, p, sizeof mp);
    mhgu_ldn_send(&mp, data, len);
}

// The port's own messages to every other game in the network (reliable, in order: the TCP link).
void ldn_send_aux(const void *data, uint32_t len) {
    if (L.ryu || L.room) return;
    if (L.state == ST_AP_CREATED) {
        mhgu_ldn_send_aux(data, len);
    } else if (L.state == ST_STATION_CONNECTED) {
        mhgu_ldn_send_aux(data, len);
    }
}

// Voice chat (runtime/voice.c) to every other game: in crossplay F_AUX doesn't travel, so the packets go as a UDP
// broadcast on VOICE_UDP_PORT of the local play network, as the game's own datagrams do (vsock.c takes them out of
// the game's sockets on arrival). Packets from the same address as ours are dropped there: a room relays a broadcast
// to its members and back to the sender.
void ldn_send_voice(const void *data, uint32_t len) {
    if (!ldn_net_up() || len == 0) return;
    if (!ldn_crossplay()) {
        ldn_send_aux(data, len);
        return;
    }
    VPacket p = {ldn_own_ip(), ldn_broadcast_ip(), VOICE_UDP_PORT, VOICE_UDP_PORT, VPROTO_UDP, 0, 0};
    ldn_send(&p, data, len);
}

// ---- MHGU_LDN_LAG=<ms>: received packets wait that long before they go on (testing high ping: set it on both
// ends, the round trip grows by twice the value). Kept in arrival order by one delivery thread.
typedef struct Delayed { struct Delayed *next; uint64_t due; VPacket p; uint32_t len; uint8_t data[]; } Delayed;
static Delayed *lag_head, *lag_tail;
static pthread_mutex_t lag_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t lag_cv = PTHREAD_COND_INITIALIZER;

static void *lag_thread(void *arg) {
    (void)arg;
    pthread_mutex_lock(&lag_mu);
    for (;;) {
        while (!lag_head) pthread_cond_wait(&lag_cv, &lag_mu);
        Delayed *d = lag_head;
        uint64_t now = plat_now_ns();
        if (now < d->due) {
            pthread_mutex_unlock(&lag_mu);
            plat_sleep_until_ns(d->due);
            pthread_mutex_lock(&lag_mu);
            continue;
        }
        lag_head = d->next;
        if (!lag_head) lag_tail = NULL;
        pthread_mutex_unlock(&lag_mu);
        vsock_deliver(&d->p, d->data, d->len);
        free(d);
        pthread_mutex_lock(&lag_mu);
    }
    return NULL;
}

// A packet from the Rust link (the host already relayed it).
// MHGU_LDN_LOSS=<percent>: that share of the received game packets is dropped (testing a lossy connection).
static void link_received(const VPacket *p, const void *data, uint32_t len) {
    static int lag_ms = -1, loss_pct;
    if (lag_ms < 0) {
        const char *e = getenv("MHGU_LDN_LAG");
        lag_ms = e ? atoi(e) : 0;
        if (lag_ms > 0) {
            pthread_t t;
            pthread_create(&t, NULL, lag_thread, NULL);
            LOG("[ldn] MHGU_LDN_LAG: received packets are held back %d ms\n", lag_ms);
        }
        if ((e = getenv("MHGU_LDN_LOSS")) && atoi(e) > 0) {
            loss_pct = atoi(e);
            LOG("[ldn] MHGU_LDN_LOSS: dropping %d%% of the received packets\n", loss_pct);
        }
    }
    if (loss_pct && (int)(arc4random() % 100) < loss_pct) return;
    if (lag_ms <= 0) {
        vsock_deliver(p, data, len);
        return;
    }
    Delayed *d = malloc(sizeof *d + len);
    if (!d) return;
    d->next = NULL, d->due = plat_now_ns() + (uint64_t)lag_ms * 1000000ull, d->p = *p, d->len = len;
    memcpy(d->data, data, len);
    pthread_mutex_lock(&lag_mu);
    if (lag_tail) lag_tail->next = d; else lag_head = d;
    lag_tail = d;
    pthread_cond_signal(&lag_cv);
    pthread_mutex_unlock(&lag_mu);
}


// ---- crossplay: a lobby on a RyuLdn server (Ryujinx/suyu)
static void ryu_lobby_changed(const uint8_t *info) {
    pthread_mutex_lock(&L.mu);
    if (!L.ryu) { pthread_mutex_unlock(&L.mu); return; }
    if (info) {
        bool ok = info_sane((uint8_t *)info);
        if (ok) memcpy(L.info, info, NETINFO_SIZE);
        LOG("[ldn] RyuLdn lobby update: %d of %d players%s\n", info[NI_NODE_COUNT], info[NI_NODE_MAX], ok ? "" : " (rejected as malformed)");
    } else if (L.state == ST_AP_CREATED) {
        L.state = ST_ACCESS_POINT, L.reason = DR_DESTROYED_BY_SYSTEM;
        LOG("[ldn] the RyuLdn server closed the hub\n");
    } else if (L.state == ST_STATION_CONNECTED) {
        L.state = ST_ERROR, L.reason = DR_SIGNAL_LOST;
        LOG("[ldn] connection to the RyuLdn lobby ended\n");
    }
    bool gone = !info;
    pthread_mutex_unlock(&L.mu);
    if (mhgu_xb_active()) mhgu_xb_lobby(info);
    if (gone) vsock_network_down();
    signal_change();
}

static void ryu_data_received(const RyuProxy *r, const uint8_t *data, uint32_t len) {
    if (!L.ryu || r->proto != 17) return;
    VPacket p = {.src_ip = r->src_ip, .dst_ip = r->dst_ip, .src_port = r->src_port, .dst_port = r->dst_port,
                 .proto = VPROTO_UDP, .kind = 0};
    vsock_deliver(&p, data, len);
}

static void ryu_stop(void) {
    if (!L.ryu) return;
    mhgu_xb_end();
    mhgu_ryu_leave();
    L.ryu = false;
    L.ryu_mask = 0;
    vsock_network_down();
}

// ---- crossplay: a suyu-family room
static uint32_t ip_u32(const uint8_t b[4]) { return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3]; }
static void room_mac(uint8_t mac[6]) { uint8_t ip[4]; mhgu_room_own_ip(ip); mac[0] = 0x02, mac[1] = 0; memcpy(mac + 2, ip, 4); }

static struct { uint8_t info[NETINFO_SIZE]; } room_scan[MAX_FOUND];
static int room_nscan;
static bool room_scanning, room_info_dirty;
static pthread_mutex_t room_scan_mu = PTHREAD_MUTEX_INITIALIZER;

static void room_sync_all(void) {
    uint8_t own[4], info[NETINFO_SIZE];
    mhgu_room_own_ip(own);
    pthread_mutex_lock(&L.mu);
    memcpy(info, L.info, NETINFO_SIZE);
    pthread_mutex_unlock(&L.mu);
    for (int i = 1; i < MAX_NODES; i++)
        if (L.room_client[i]) mhgu_room_send_ldn(RLDN_SYNC_NETWORK, own, L.room_cip[i], false, info, NETINFO_SIZE);
}

static void room_stop(void) {
    if (!L.room) return;
    if (L.state == ST_AP_CREATED) {
        uint8_t own[4];
        mhgu_room_own_ip(own);
        for (int i = 1; i < MAX_NODES; i++)
            if (L.room_client[i]) mhgu_room_send_ldn(RLDN_DESTROY_NETWORK, own, L.room_cip[i], false, NULL, 0);
    } else if (L.state == ST_STATION_CONNECTED) {
        uint8_t own[4], host[4];
        mhgu_room_own_ip(own);
        ip_bytes(rd32(node_at(L.info, 0) + NODE_IP), host);
        uint8_t node[NODE_SIZE] = {0};
        wr32(node + NODE_IP, L.own_ip);
        room_mac(node + NODE_MAC);
        mhgu_room_send_ldn(RLDN_DISCONNECT, own, host, false, node, NODE_SIZE);
    }
    memset(L.room_client, 0, sizeof L.room_client);
    L.room = false;
    vsock_network_down();
}

static bool room_from_host_locked(const uint8_t ip[4]) { return ip_u32(ip) == rd32(node_at(L.info, 0) + NODE_IP); }
static bool room_in_hub_locked(uint32_t ip) {
    for (int i = 0; i < MAX_NODES; i++)
        if (node_at(L.info, i)[NODE_CONNECTED] && rd32(node_at(L.info, i) + NODE_IP) == ip) return true;
    return false;
}

static void room_ldn_received(uint8_t type, const uint8_t lip[4], const uint8_t rip[4], bool bc, const uint8_t *data, uint32_t len) {
    if (mhgu_xb_active() && (type == RLDN_SCAN || type == RLDN_CONNECT || type == RLDN_DISCONNECT)) {
        mhgu_xb_room_ldn(type, lip, rip, bc, data, len);
        return;
    }
    uint8_t own[4];
    mhgu_room_own_ip(own);
    switch (type) {
    case RLDN_SCAN: {
        uint8_t info[NETINFO_SIZE];
        bool host;
        pthread_mutex_lock(&L.mu);
        host = L.room && L.state == ST_AP_CREATED;
        if (host) memcpy(info, L.info, NETINFO_SIZE);
        pthread_mutex_unlock(&L.mu);
        if (host) mhgu_room_send_ldn(RLDN_SCAN_RESP, own, lip, false, info, NETINFO_SIZE);
        break;
    }
    case RLDN_SCAN_RESP:
        if (len >= NETINFO_SIZE && info_sane((uint8_t *)data)) {
            pthread_mutex_lock(&room_scan_mu);
            if (room_scanning) {
                int i = 0;
                while (i < room_nscan && memcmp(room_scan[i].info + NI_BSSID, (const uint8_t *)data + NI_BSSID, 6)) i++;
                if (i < MAX_FOUND) { memcpy(room_scan[i].info, data, NETINFO_SIZE); if (i == room_nscan) room_nscan++; }
            }
            pthread_mutex_unlock(&room_scan_mu);
        }
        break;
    case RLDN_CONNECT: {
        if (len < NODE_SIZE) break;
        bool changed = false, refused = false, again = false;
        pthread_mutex_lock(&L.mu);
        if (L.room && L.state == ST_AP_CREATED) {
            uint16_t lcv = rd16((const uint8_t *)data + NODE_LCV);
            for (int k = 1; k < MAX_NODES; k++) again |= L.room_client[k] && !memcmp(L.room_cip[k], lip, 4);
            refused = !again && (L.info[NI_NODE_COUNT] >= L.info[NI_NODE_MAX] || L.info[NI_ACCEPT_POLICY] == 1 ||
                                 (lcv && lcv != L.lcv));
        }
        if (L.room && L.state == ST_AP_CREATED && !again && !refused) {
            int i = 1;
            while (i < MAX_NODES && node_at(L.info, i)[NODE_CONNECTED]) i++;
            refused = i == MAX_NODES;
            if (i < MAX_NODES) {
                uint8_t *nd = node_at(L.info, i);
                memcpy(nd, data, NODE_SIZE);
                nd[NODE_ID] = (uint8_t)i, nd[NODE_CONNECTED] = 1;
                L.room_client[i] = true;
                memcpy(L.room_cip[i], lip, 4);
                L.info[NI_NODE_COUNT]++;
                changed = true;
                LOG("[ldn] room member %u.%u.%u.%u joined the hub as node %d\n", lip[0], lip[1], lip[2], lip[3], i);
            }
        }
        pthread_mutex_unlock(&L.mu);
        if (refused) mhgu_room_send_ldn(RLDN_DESTROY_NETWORK, own, lip, false, NULL, 0);
        if (changed || again) room_sync_all();
        if (changed) signal_change();
        break;
    }
    case RLDN_DISCONNECT: {
        if (len < NODE_SIZE) break;
        bool changed = false;
        pthread_mutex_lock(&L.mu);
        if (L.room && L.state == ST_AP_CREATED)
            for (int i = 1; i < MAX_NODES; i++)
                if (L.room_client[i] && !memcmp(L.room_cip[i], lip, 4)) {
                    memset(node_at(L.info, i), 0, NODE_SIZE);
                    node_at(L.info, i)[NODE_ID] = (uint8_t)i;
                    L.room_client[i] = false;
                    if (L.info[NI_NODE_COUNT] > 1) L.info[NI_NODE_COUNT]--;
                    changed = true;
                }
        pthread_mutex_unlock(&L.mu);
        if (changed) { room_sync_all(); signal_change(); }
        break;
    }
    case RLDN_SYNC_NETWORK:
        if (len >= NETINFO_SIZE && info_sane((uint8_t *)data)) {
            pthread_mutex_lock(&L.mu);
            if (L.room && (L.state == ST_STATION || L.state == ST_STATION_CONNECTED) && room_from_host_locked(lip)) {
                memcpy(L.info, data, NETINFO_SIZE);
                L.state = ST_STATION_CONNECTED, L.reason = DR_NONE;
            }
            pthread_mutex_unlock(&L.mu);
            signal_change();
        }
        break;
    case RLDN_DESTROY_NETWORK: {
        bool gone = false;
        pthread_mutex_lock(&L.mu);
        if (L.room && L.state == ST_STATION_CONNECTED && room_from_host_locked(lip)) {
            L.state = ST_STATION, L.reason = DR_DESTROYED_BY_SYSTEM;
            L.own_ip = L.subnet = 0, gone = true;
            L.room = false;
        }
        pthread_mutex_unlock(&L.mu);
        if (gone) { LOG("[ldn] the room's hub was closed by its host\n"); vsock_network_down(); signal_change(); }
        break;
    }
    }
}

static void room_proxy_received(const uint8_t sip[4], uint16_t sport, const uint8_t dip[4], uint16_t dport, uint8_t proto, bool bc,
                                const uint8_t *data, uint32_t len) {
    if (mhgu_xb_active()) { mhgu_xb_room_proxy(sip, sport, dip, dport, proto, bc, data, len); return; }
    if (!L.room || proto != 4) return;
    pthread_mutex_lock(&L.mu);
    bool ours = L.room && ldn_net_up() && room_in_hub_locked(ip_u32(sip));
    pthread_mutex_unlock(&L.mu);
    if (!ours) return;
    VPacket p = {.src_ip = ip_u32(sip), .dst_ip = ip_u32(dip), .src_port = sport, .dst_port = dport, .proto = VPROTO_UDP, .kind = 0};
    vsock_deliver(&p, data, len);
}

static void room_members_changed(const uint8_t (*ips)[4], int n) {
    if (mhgu_xb_active()) { mhgu_xb_room_members(ips, n); return; }
    bool changed = false;
    pthread_mutex_lock(&L.mu);
    if (L.room && L.state == ST_AP_CREATED) {
        for (int i = 1; i < MAX_NODES; i++) {
            if (!L.room_client[i]) continue;
            bool present = false;
            for (int j = 0; j < n; j++) present |= !memcmp(L.room_cip[i], ips[j], 4);
            if (present) continue;
            memset(node_at(L.info, i), 0, NODE_SIZE);
            node_at(L.info, i)[NODE_ID] = (uint8_t)i;
            L.room_client[i] = false;
            if (L.info[NI_NODE_COUNT] > 1) L.info[NI_NODE_COUNT]--;
            changed = true;
        }
    }
    pthread_mutex_unlock(&L.mu);
    if (changed) { room_sync_all(); signal_change(); }
}
static void room_lost(void) {
    LOG("[room] the room connection ended\n");
    mhgu_xb_end();
    bool gone = false;
    pthread_mutex_lock(&L.mu);
    if (L.room) {
        if (L.state == ST_AP_CREATED) L.state = ST_ACCESS_POINT, L.reason = DR_DESTROYED_BY_SYSTEM, gone = true;
        else if (L.state == ST_STATION_CONNECTED) L.state = ST_ERROR, L.reason = DR_SIGNAL_LOST, gone = true;
        if (gone) { memset(L.room_client, 0, sizeof L.room_client); L.room = false; }
    }
    pthread_mutex_unlock(&L.mu);
    if (gone) { vsock_network_down(); signal_change(); }
}

static void mixed_bridge_start(void) {
    pthread_mutex_lock(&L.mu);
    if (L.ryu && L.state == ST_AP_CREATED && mhgu_room_connected() && !mhgu_xb_active())
        mhgu_xb_begin(L.info, L.own_ip, L.ryu_mask);
    pthread_mutex_unlock(&L.mu);
}
static bool ryu_configured(void) {
    return (xp_config.crossplay == XP_RYU || xp_config.crossplay == XP_MIXED) &&
           (xp_config.ryu_servers[0] || xp_config.ryu_host);
}
static bool room_configured(void) {
    return (xp_config.crossplay == XP_ROOM || xp_config.crossplay == XP_MIXED) &&
           (xp_config.room_server[0] || xp_config.room_host);
}
static char roomsrv_self[32];
static pthread_mutex_t roomsrv_addr_mu = PTHREAD_MUTEX_INITIALIZER;
static const char *room_server(void) {
    static __thread char addr[sizeof xp_config.room_server];
    pthread_mutex_lock(&roomsrv_addr_mu);
    snprintf(addr, sizeof addr, "%s", roomsrv_self[0] ? roomsrv_self : xp_config.room_server);
    pthread_mutex_unlock(&roomsrv_addr_mu);
    return addr;
}
static bool room_enabled(void) { return room_configured() && (room_server()[0] != 0); }

static pthread_t room_keep_th;
static pthread_mutex_t room_keep_mu = PTHREAD_MUTEX_INITIALIZER;
static bool room_keep_started;
static int room_wanted;
static Config room_keep_config;
static void *room_keeper(void *arg) {
    (void)arg;
    xp_config = room_keep_config;  // keep the connection on the settings applied when it started
    uint64_t next = 0;
    while (__atomic_load_n(&room_wanted, __ATOMIC_ACQUIRE)) {
        uint64_t now = plat_now_ns();
        if (now >= next && room_enabled() && !mhgu_room_connected()) {
            uint8_t ip[4];
            char err[96];
            if (!mhgu_room_join(room_server(), xp_config.room_name, xp_config.room_password, ip, err, sizeof err)) {
                LOG("[ldn] room join failed: %s\n", err);
                next = now + 15000000000ull;
            }
        }
        mixed_bridge_start();
        plat_sleep_until_ns(plat_now_ns() + 500000000ull);
    }
    return NULL;
}
static void room_start(void) {
    if (!room_enabled()) return;
    pthread_mutex_lock(&room_keep_mu);
    if (room_keep_started) { pthread_mutex_unlock(&room_keep_mu); return; }
    static bool hooked;
    if (!hooked) {
        hooked = true;
        mhgu_room_set_hooks(room_ldn_received, room_proxy_received, room_lost, room_members_changed);
    }
    room_keep_config = xp_config;
    __atomic_store_n(&room_wanted, 1, __ATOMIC_RELEASE);
    if (!pthread_create(&room_keep_th, NULL, room_keeper, NULL)) room_keep_started = true;
    else __atomic_store_n(&room_wanted, 0, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&room_keep_mu);
}
static void room_shutdown(void) {
    pthread_mutex_lock(&room_keep_mu);
    __atomic_store_n(&room_wanted, 0, __ATOMIC_RELEASE);
    if (room_keep_started) {
        pthread_join(room_keep_th, NULL);
        room_keep_started = false;
    }
    room_stop();
    mhgu_room_leave();
    pthread_mutex_unlock(&room_keep_mu);
}

static void ryusrv_wanted(void) {
    if (!xp_config.ryu_host || mhgu_ryusrv_running()) return;
    int port = xp_config.ryu_port > 0 ? xp_config.ryu_port : 30456;
    char err[96] = "";
    if (!mhgu_ryusrv_start("0.0.0.0", (uint16_t)port, err, sizeof err)) {
        LOG("[ldn] can't run the RyuLdn server on port %d: %s\n", port, err);
        return;
    }
    char addr[32];
    snprintf(addr, sizeof addr, "127.0.0.1:%d", port);
    mhgu_ryu_set_self(addr);
    LOG("[ldn] Ryujinx players can use this PC as their LDN server (port %d)\n", port);
}
static void ryusrv_unwanted(void) {
    if (!mhgu_ryusrv_running()) return;
    mhgu_ryu_set_self(NULL);
    mhgu_ryusrv_stop();
}
static void roomsrv_wanted(void) {
    if (!xp_config.room_host || mhgu_roomsrv_running()) return;
    int port = xp_config.room_port > 0 ? xp_config.room_port : 24872;
    char err[96] = "";
    if (!mhgu_roomsrv_start(port, xp_config.room_name, xp_config.room_password, xp_config.room_name, err, sizeof err)) {
        LOG("[ldn] can't run the room server on port %d: %s\n", port, err);
        return;
    }
    pthread_mutex_lock(&roomsrv_addr_mu);
    snprintf(roomsrv_self, sizeof roomsrv_self, "127.0.0.1:%d", port);
    pthread_mutex_unlock(&roomsrv_addr_mu);
    LOG("[ldn] suyu-family players can join this PC's room (port %d)\n", port);
}
static void roomsrv_unwanted(void) {
    if (!mhgu_roomsrv_running()) return;
    pthread_mutex_lock(&roomsrv_addr_mu);
    roomsrv_self[0] = 0;
    pthread_mutex_unlock(&roomsrv_addr_mu);
    mhgu_roomsrv_stop();
}

// Saved settings apply before the next search or hub creation. Existing connections
// retain their transport until they leave; each reconnect reads the saved snapshot.
static void crossplay_prepare(void) {
    static Config applied;
    static bool prepared;
    static pthread_mutex_t prepare_mu = PTHREAD_MUTEX_INITIALIZER;
    crossplay_snapshot();
    pthread_mutex_lock(&prepare_mu);
    pthread_mutex_lock(&L.mu);
    bool active = L.state == ST_AP_CREATED || L.state == ST_STATION_CONNECTED;
    pthread_mutex_unlock(&L.mu);
    bool room_changed = prepared && (applied.crossplay != xp_config.crossplay ||
        applied.room_host != xp_config.room_host || applied.room_port != xp_config.room_port ||
        strcmp(applied.room_server, xp_config.room_server) || strcmp(applied.room_name, xp_config.room_name) ||
        strcmp(applied.room_password, xp_config.room_password));
    bool ryu_changed = prepared && (applied.crossplay != xp_config.crossplay ||
        applied.ryu_host != xp_config.ryu_host || applied.ryu_port != xp_config.ryu_port ||
        strcmp(applied.ryu_servers, xp_config.ryu_servers) || strcmp(applied.ryu_password, xp_config.ryu_password));
    if (!active) {
        if (room_changed) { room_shutdown(); roomsrv_unwanted(); }
        if (ryu_changed) ryusrv_unwanted();
        applied = xp_config;
        prepared = true;
    } else if (prepared) {
        xp_config = applied;
    }
    mhgu_ryu_set_config(xp_config.ryu_servers, "", xp_config.ryu_password);
    if (ryu_configured()) ryusrv_wanted();
    if (room_configured()) {
        roomsrv_wanted();
        room_start();
    }
    pthread_mutex_unlock(&prepare_mu);
}

// ---- host
static void push_info_locked(int except) {  // to every station (and the hub server's list): the network changed
    (void)except;
    mhgu_ldn_update_info(L.info, NETINFO_SIZE);  // the Rust host sends F_INFO and refreshes its hub listing
}

// A station's connection: accepted here, or opened to the hub server for a player it announced (relay_pair != 0).

// Accepts stations and answers LAN discovery probes until the listening sockets close.

// ---- the hub server (internet hubs)
// ini Server: empty = the default server (LDN_DEFAULT_SERVER, config.h), "Off" = none. Its name into out
// (LOBBY_NAME bytes); false: no internet hubs. Server and Key are live settings (config_live_ldn): a change applies
// from the next search, join or connection to the server on.
#define LOBBY_NAME sizeof g_config.ldn_server
static bool lobby_server_of(const char *s, char *out) {
    if ((s[0] | 0x20) == 'o' && (s[1] | 0x20) == 'f' && (s[2] | 0x20) == 'f' && !s[3]) s = "";
    else if (!*s) s = LDN_DEFAULT_SERVER;
    snprintf(out, LOBBY_NAME, "%s", s);
    return *out;
}
static bool lobby_server(char *out) {
    char s[LOBBY_NAME];
    config_live_ldn(s, sizeof s, NULL, 0);
    return lobby_server_of(s, out);
}

// A request's header. The room is a hash of the matchmaking key (key NULL: ini Key; spaces around it ignored): hubs
// are listed only for players whose key matches, all of them without one. The server never sees the key itself.
static bool lobby_header(uint8_t *h, const char *key) {
    static const char salt[] = "MHGURecomp matchmaking key:";
    char live[sizeof g_config.ldn_key];
    if (!key) config_live_ldn(NULL, 0, live, sizeof live), key = live;
    memcpy(h, "MHGS", 4);
    wr16(h + 4, LOBBY_PROTO);
    wr16(h + 6, PROTO_VERSION);
    memset(h + 8, 0, 16);
    const char *k = key;
    while (*k == ' ') k++;
    size_t n = strlen(k);
    while (n && k[n - 1] == ' ') n--;
    if (!n) return false;
    Sha256 s;
    char hex[65];
    sha256_init(&s);
    sha256_update(&s, salt, sizeof salt - 1);
    sha256_update(&s, k, n);
    sha256_hex(&s, hex);
    for (int i = 0; i < 16; i++) {
        unsigned v;
        sscanf(hex + 2 * i, "%2x", &v);
        h[8 + i] = (uint8_t)v;
    }
    return true;
}

static const char *lobby_error(uint32_t code) {
    switch (code) {
    case SE_NOT_FOUND: return "the hub is gone";
    case SE_NO_ANSWER: return "the host did not answer";
    case SE_BUSY: return "the server is busy (too many hubs or requests from this address)";
    case SE_VERSION: return "the server speaks another protocol version";
    default: return "refused";
    }
}
static int lobby_state(uint32_t code) {  // a server's error as a HUB_* state (net.h)
    return code == SE_VERSION ? HUB_VERSION : code == SE_BUSY ? HUB_BUSY : HUB_REFUSED;
}

// What the player sees of the hub server: its last known state, for the settings menu (ldn_hub_status), and a notice
// on screen (overlay_notice) when it fails: hosting (the hub is not listed online), searching (only the LAN's hubs
// are found) or joining. Hosting notices come when the state changes; a search's every time, but not twice within
// 10 s. Texts in UI_TEXT are translated as the settings menu's (settings_locale.h,
// tests/test_settings_locale.py).
#define UI_TEXT(s) s
enum { NOTE_HOST, NOTE_SEARCH, NOTE_JOIN };
static pthread_mutex_t hub_mu = PTHREAD_MUTEX_INITIALIZER;
static struct { int state, ms, hubs; char server[LOBBY_NAME], key[sizeof g_config.ldn_key]; uint32_t check; } hub_now;
static int note_last[3];  // HUB_*: HUB_UNKNOWN and HUB_ONLINE are fine
static uint64_t note_at[3];

static void hub_set_locked(const char *srv, const char *key, int state, int ms, int hubs) {
    hub_now.state = state, hub_now.ms = ms, hub_now.hubs = hubs;
    snprintf(hub_now.server, sizeof hub_now.server, "%s", srv);
    snprintf(hub_now.key, sizeof hub_now.key, "%s", key);
}

static void hub_report(int what, const char *srv, int state, int ms, int hubs) {
    char key[sizeof g_config.ldn_key];
    config_live_ldn(NULL, 0, key, sizeof key);
    uint64_t now = plat_now_ns();
    pthread_mutex_lock(&hub_mu);
    if (hub_now.state != HUB_CHECKING && what != NOTE_JOIN) hub_set_locked(srv, key, state, ms, hubs);
    bool show = state == HUB_ONLINE ? what == NOTE_HOST && note_last[what] > HUB_ONLINE
                                    : what == NOTE_JOIN || note_last[what] != state || now - note_at[what] > 10000000000ull;
    note_last[what] = state;
    if (show) note_at[what] = now;
    pthread_mutex_unlock(&hub_mu);
    if (!show) return;
    const char *title;
    switch (state) {
    case HUB_ONLINE: title = UI_TEXT("Connected to the hub server %s"); break;
    case HUB_NOT_FOUND: title = UI_TEXT("Hub server %s not found"); break;
    case HUB_UNREACHABLE: title = UI_TEXT("Can't reach the hub server %s"); break;
    case HUB_LOST: title = UI_TEXT("Lost the connection to the hub server %s"); break;
    case HUB_BUSY: title = UI_TEXT("The hub server %s is busy"); break;
    case HUB_VERSION: title = UI_TEXT("The hub server %s runs another version"); break;
    case HUB_REFUSED: title = UI_TEXT("The hub server %s refused the request"); break;
    default: title = UI_TEXT("The hub server %s did not answer"); break;
    }
    static const char *const next[] = {UI_TEXT("Your hub is not listed online. Trying again..."),
                                       UI_TEXT("Only hubs on your local network are shown."),
                                       UI_TEXT("Could not join the hub.")};
    int lang = config_language();
    char line[LOBBY_NAME + 128];
    snprintf(line, sizeof line, settings_text(lang, title), srv);
    const char *text = state == HUB_ONLINE ? UI_TEXT("Your hub is listed online again.") : next[what];
    char join[sizeof g_config.ldn_join];
    config_live_ldn_join(join, sizeof join);
    if (what == NOTE_SEARCH && join[0]) text = UI_TEXT("Only hubs on your local network and Join addresses are shown.");
    overlay_notice(line, settings_text(lang, text));
}

// Host: one thread per created network keeps it listed and opens a connection for each player the server announces.
// Threads of a network that is gone (lobby_gen moved on) end by themselves; nobody waits for them.
static volatile uint32_t lobby_gen;
typedef struct { uint32_t gen; struct sockaddr_in addr; uint64_t ticket; } LobbyTicket;

static bool lobby_wait(uint32_t gen, int ms) {  // false: the network is gone
    for (uint64_t end = plat_now_ns() + (uint64_t)ms * 1000000ull; plat_now_ns() < end && lobby_gen == gen;)
        plat_sleep_until_ns(plat_now_ns() + 100000000ull);
    return lobby_gen == gen;
}




// Station: the hubs the server lists for a key (NULL: ini Key). A HUB_* state; ms: the request's round trip.
#define LOBBY_LIST_MAX 24
typedef struct {
    uint8_t infos[LOBBY_LIST_MAX][NETINFO_SIZE];
    uint64_t ids[LOBBY_LIST_MAX];
    uint16_t vers[LOBBY_LIST_MAX];             // each hub's link version
    char codes[LOBBY_LIST_MAX][CODE_LEN + 1];  // and session code ("": none)
    int n, state, ms;
    struct sockaddr_in addr;
    char server[LOBBY_NAME];
} LobbyList;

bool ldn_code_normalize(const char *in, char *out);

// A HUBS (u64 id, NetworkInfo each) or HUBS4 (u64 id, u16 link version, code[8], NetworkInfo) answer into r. A server
// older than version 4 answers HUBS, of hubs of our version only.
static void lobby_hubs(const uint8_t *buf, int n, uint8_t type, LobbyList *r) {
    const uint32_t entry = type == S_HUBS4 ? 8 + 2 + CODE_LEN + NETINFO_SIZE : 8 + NETINFO_SIZE;
    uint32_t count = rd32(buf);
    r->n = 0;
    for (uint32_t i = 0; i < count && r->n < LOBBY_LIST_MAX && 4 + (i + 1) * entry <= (uint32_t)n; i++) {
        const uint8_t *e = buf + 4 + i * entry;
        r->ids[r->n] = rd64(e);
        r->vers[r->n] = PROTO_VERSION, r->codes[r->n][0] = 0;
        if (type == S_HUBS4) {
            uint16_t v = rd16(e + 8);
            r->vers[r->n] = link_version_ok(v) ? v : PROTO_MIN;
            char raw[CODE_LEN + 1];
            memcpy(raw, e + 10, CODE_LEN), raw[CODE_LEN] = 0;  // (zeros: no code; anything else not a code: none)
            if (!raw[0] || strlen(raw) != CODE_LEN || !ldn_code_normalize(raw, r->codes[r->n])) r->codes[r->n][0] = 0;
            e += 2 + CODE_LEN;
        }
        memcpy(r->infos[r->n++], e + 8, NETINFO_SIZE);
    }
}

static int lobby_list(const char *srv, const char *key, int connect_ms, LobbyList *r) {
    static __thread uint8_t buf[MAX_FRAME];
    r->n = 0, r->ms = -1;
    if (!resolve_host(srv, LOBBY_PORT, &r->addr)) return HUB_NOT_FOUND;
    hsock s = connect_to(&r->addr, connect_ms);
    if (s == BADSOCK) return HUB_UNREACHABLE;
    uint8_t req[LOBBY_HEADER], type;
    lobby_header(req, key);
    hset_timeout(s, 2000);
    uint64_t asked = plat_now_ns();
    int n = frame_send(s, S_LIST, req, sizeof req, NULL, 0) ? frame_recv(s, &type, buf) : -1;
    r->ms = (int)((plat_now_ns() - asked) / 1000000);
    hclose(s);
    if (n >= 4 && type == S_ERROR) return lobby_state(rd32(buf));
    if (n < 4 || (type != S_HUBS && type != S_HUBS4)) return HUB_NO_ANSWER;
    lobby_hubs(buf, n, type, r);
    return HUB_ONLINE;
}

// Scan's hub-server request: a thread of its own, because the game's network thread waits for Scan and a DNS lookup
// can block far longer than a player will wait. Scan waits SCAN_LIST_WAIT_MS at most; a late answer is dropped. While
// one request is still out, later scans search the LAN only, so a stuck lookup leaves one thread behind, not one per scan.
#define SCAN_LIST_WAIT_MS 4000
typedef struct {
    LobbyList list;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool done;
    int refs;  // Scan and the thread: the last one to let go frees it
} ScanJob;
static int scan_jobs_out;

static void scan_job_unref(ScanJob *j) {
    pthread_mutex_lock(&j->mu);
    bool last = --j->refs == 0;
    pthread_mutex_unlock(&j->mu);
    if (!last) return;
    pthread_cond_destroy(&j->cv);
    pthread_mutex_destroy(&j->mu);
    free(j);
}

static void *lobby_list_thread(void *arg) {
    ScanJob *j = arg;
    LobbyList *r = &j->list;
    static int last = HUB_ONLINE;
    r->state = lobby_list(r->server, NULL, 1500, r);
    if (r->state != last && r->state != HUB_ONLINE)
        LOG("[ldn] hub server %s: %s: only LAN / Join hubs are searched\n", r->server,
            r->state == HUB_NOT_FOUND ? "not found" : r->state == HUB_UNREACHABLE ? "unreachable" : "no list");
    last = r->state;
    hub_report(NOTE_SEARCH, r->server, r->state, r->ms, r->state == HUB_ONLINE ? r->n : -1);
    pthread_mutex_lock(&j->mu);
    j->done = true;
    pthread_cond_signal(&j->cv);
    pthread_mutex_unlock(&j->mu);
    __atomic_sub_fetch(&scan_jobs_out, 1, __ATOMIC_ACQ_REL);
    scan_job_unref(j);
    return NULL;
}

// Starts this scan's request, or NULL when there is no server or an earlier request is still out.
static ScanJob *scan_job_start(void) {
    static bool said;
    if (__atomic_load_n(&scan_jobs_out, __ATOMIC_ACQUIRE) > 0) {
        if (!said) LOG("[ldn] the hub server's list from an earlier search has not come: only LAN / Join hubs are searched\n");
        said = true;
        return NULL;
    }
    said = false;
    ScanJob *j = calloc(1, sizeof *j);
    if (!j) return NULL;
    if (!lobby_server(j->list.server)) { free(j); return NULL; }
    pthread_mutex_init(&j->mu, NULL);
    pthread_cond_init(&j->cv, NULL);
    j->refs = 2;
    __atomic_add_fetch(&scan_jobs_out, 1, __ATOMIC_ACQ_REL);
    pthread_t th;
    if (pthread_create(&th, NULL, lobby_list_thread, j)) {
        __atomic_sub_fetch(&scan_jobs_out, 1, __ATOMIC_ACQ_REL);
        j->refs = 1;
        scan_job_unref(j);
        return NULL;
    }
    pthread_detach(th);
    return j;
}

// true: the list arrived within SCAN_LIST_WAIT_MS (j->list holds it).
static bool scan_job_wait(ScanJob *j) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += SCAN_LIST_WAIT_MS / 1000;
    ts.tv_nsec += (long)(SCAN_LIST_WAIT_MS % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) ts.tv_sec++, ts.tv_nsec -= 1000000000L;
    pthread_mutex_lock(&j->mu);
    while (!j->done && !pthread_cond_timedwait(&j->cv, &j->mu, &ts)) {}
    bool done = j->done;
    pthread_mutex_unlock(&j->mu);
    if (!done)
        LOG("[ldn] hub server %s: no list after %d s: only LAN / Join hubs are given to the game this time\n",
            j->list.server, SCAN_LIST_WAIT_MS / 1000);
    return done;
}

// The settings menu's check (net.h): the server and key being edited, in a thread of its own.
typedef struct { uint32_t check; char server[LOBBY_NAME], key[sizeof g_config.ldn_key]; } HubCheck;

static void *hub_check_thread(void *arg) {
    HubCheck *c = arg;
    LobbyList *r = malloc(sizeof *r);
    int state = r ? lobby_list(c->server, c->key, 3000, r) : HUB_UNKNOWN;
    int ms = r ? r->ms : -1, hubs = state == HUB_ONLINE ? r->n : -1;
    pthread_mutex_lock(&hub_mu);
    if (hub_now.check == c->check) hub_set_locked(c->server, c->key, state, ms, hubs);
    pthread_mutex_unlock(&hub_mu);
    LOG("[ldn] hub server check: %s: state %d, %d ms, %d hub(s) for this key\n", c->server, state, ms, hubs);
    free(r);
    free(c);
    return NULL;
}

void ldn_hub_check(const char *server, const char *key) {
    HubCheck *c = calloc(1, sizeof *c);
    if (!c) return;
    hnet_init();
    bool on = lobby_server_of(server, c->server);
    snprintf(c->key, sizeof c->key, "%s", key);
    pthread_mutex_lock(&hub_mu);
    c->check = ++hub_now.check;
    hub_set_locked(c->server, c->key, on ? HUB_CHECKING : HUB_OFF, -1, -1);
    pthread_mutex_unlock(&hub_mu);
    if (on && spawn(hub_check_thread, c)) return;
    if (on) {
        pthread_mutex_lock(&hub_mu);
        hub_set_locked(c->server, c->key, HUB_UNKNOWN, -1, -1);
        pthread_mutex_unlock(&hub_mu);
    }
    free(c);
}

int ldn_hub_status(const char *server, const char *key, int *ms, int *hubs) {
    char name[LOBBY_NAME];
    if (!lobby_server_of(server, name)) return HUB_OFF;
    pthread_mutex_lock(&hub_mu);
    // (the server's state holds whatever the key; the hub count only for the key it was for)
    int state = strcmp(hub_now.server, name) ? HUB_UNKNOWN : hub_now.state;
    if (ms) *ms = hub_now.ms;
    if (hubs) *hubs = strcmp(hub_now.key, key) ? -1 : hub_now.hubs;
    pthread_mutex_unlock(&hub_mu);
    return state;
}

// ---- session codes (link version 4)
// A code as typed: case, spaces and dashes don't matter, and Crockford's look-alikes are read as they're meant (O as
// 0, I and L as 1). false: not 8 of the alphabet's characters.
bool ldn_code_normalize(const char *in, char *out) {
    int n = 0;
    for (; *in; in++) {
        char ch = *in;
        if (ch == ' ' || ch == '-') continue;
        if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
        if (ch == 'O') ch = '0';
        if (ch == 'I' || ch == 'L') ch = '1';
        if (!strchr(CODE_ALPHABET, ch) || n == CODE_LEN) return false;
        out[n++] = ch;
    }
    out[n] = 0;
    return n == CODE_LEN;
}

static void code_shown(const char *code, char *out, size_t size) {  // "ABCD-EFGH"
    snprintf(out, size, "%.4s-%.4s", code, code + 4);
}

// A Hunter Search network, not a hub (server/server.go huntSearch): room for 2, 0xFF at advertise byte 0x1C (hubs have
// 1). The 8 bytes before it, at 0x14, are the host's account id (ldn_account_id), not a constant.
static bool hunt_search(const uint8_t *info) {
    const uint8_t *adv = info + NI_ADV;
    return info[NI_NODE_MAX] == 2 && rd16(info + NI_ADV_SIZE) > 0x1C && adv[0x1C] == 0xFF;
}

// The code of the hub we're in, for sharing: ours when hosting one the hub server listed with it, else the one we
// joined through the hub server. A Hunter Search network (it lasts seconds) has none worth sharing.
bool ldn_session_code(char *out, size_t size) {
    char own[16] = "", joined[16] = "";
    bool hosting = false, relayed = false;
    pthread_mutex_lock(&L.mu);
    bool search = hunt_search(L.info);
    if (L.state == ST_AP_CREATED) hosting = true;
    if (L.state == ST_STATION_CONNECTED && !L.ryu && !L.room) relayed = true;
    pthread_mutex_unlock(&L.mu);
    if (search) return false;
    if (hosting && mhgu_ldn_own_code(own, sizeof own)) {
        snprintf(out, size, "%s", own);
        return true;
    }
    if (relayed && mhgu_ldn_relayed() && mhgu_ldn_joined_code(joined, sizeof joined)) {
        snprintf(out, size, "%s", joined);
        return true;
    }
    return false;
}

// Join by code (invites): the hub a session code names, found on the hub server (FIND) and put first in the game's
// searches for INVITE_KEEP_NS, so the player picks it in Join Hub; joining it sends the code, which the server lets in
// from any room. One invite at a time: a new code replaces the last.
#define INVITE_KEEP_NS 600000000000ull  // 10 minutes
static pthread_mutex_t invite_mu = PTHREAD_MUTEX_INITIALIZER;
static struct {
    int state;  // LDN_INVITE_*
    uint32_t gen;
    char code[CODE_LEN + 1], server[LOBBY_NAME];
    struct sockaddr_in addr;
    uint64_t hub, at;
    uint16_t ver;
    uint8_t info[NETINFO_SIZE];
} invite;

static void *invite_thread(void *arg) {
    uint32_t gen = (uint32_t)(uintptr_t)arg;
    static __thread uint8_t buf[MAX_FRAME];
    char code[CODE_LEN + 1], srv[LOBBY_NAME];
    pthread_mutex_lock(&invite_mu);
    snprintf(code, sizeof code, "%s", invite.code);
    snprintf(srv, sizeof srv, "%s", invite.server);
    pthread_mutex_unlock(&invite_mu);
    int state = LDN_INVITE_FAILED;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    LobbyList *r = NULL;
    if (resolve_host(srv, LOBBY_PORT, &addr)) {
        hsock s = connect_to(&addr, 3000);
        if (s != BADSOCK) {
            uint8_t req[LOBBY_HEADER + CODE_LEN], type;
            lobby_header(req, NULL);
            memcpy(req + LOBBY_HEADER, code, CODE_LEN);
            hset_timeout(s, 4000);
            uint64_t asked = plat_now_ns();
            int n = frame_send(s, S_FIND, req, sizeof req, NULL, 0) ? frame_recv(s, &type, buf) : -1;
            hclose(s);
            if (n >= 4 && type == S_HUBS4 && (r = malloc(sizeof *r))) {
                lobby_hubs(buf, n, type, r);
                state = r->n == 1 && info_sane(r->infos[0]) ? LDN_INVITE_READY : LDN_INVITE_NOT_FOUND;
            } else if (n >= 4 && type == S_ERROR) {
                state = rd32(buf) == SE_BUSY ? LDN_INVITE_FAILED : LDN_INVITE_OLD_SERVER;
            } else {  // a server older than version 4 hangs up on FIND at once; one that runs out the time is just slow
                state = plat_now_ns() - asked < 3000000000ull ? LDN_INVITE_OLD_SERVER : LDN_INVITE_FAILED;
            }
        }
    }
    LOG("[ldn] session code %s on %s: %s\n", code, srv,
        state == LDN_INVITE_READY ? "found" : state == LDN_INVITE_NOT_FOUND ? "no such hub" :
        state == LDN_INVITE_OLD_SERVER ? "the hub server doesn't know session codes" : "no answer");
    pthread_mutex_lock(&invite_mu);
    bool mine = invite.gen == gen;
    if (mine) {
        invite.state = state, invite.addr = addr, invite.at = plat_now_ns();
        if (state == LDN_INVITE_READY) {
            invite.hub = r->ids[0], invite.ver = r->vers[0];
            memcpy(invite.info, r->infos[0], NETINFO_SIZE);
        }
    }
    pthread_mutex_unlock(&invite_mu);
    free(r);
    if (!mine) return NULL;
    int lang = config_language();
    if (state == LDN_INVITE_READY)
        overlay_notice(settings_text(lang, UI_TEXT("Hub found")),
                       settings_text(lang, UI_TEXT("Search for hubs in Join Hub: it is first in the list.")));
    else if (state == LDN_INVITE_NOT_FOUND)
        overlay_notice(settings_text(lang, UI_TEXT("No hub has that code")),
                       settings_text(lang, UI_TEXT("It may have closed, or be on another hub server.")));
    else
        overlay_notice(settings_text(lang, UI_TEXT("Could not look up the code")),
                       settings_text(lang, state == LDN_INVITE_OLD_SERVER ? UI_TEXT("The hub server is older than this game.")
                                                                          : UI_TEXT("The hub server did not answer.")));
    return NULL;
}

bool ldn_join_code(const char *typed, const char *server) {
    char code[CODE_LEN + 1], srv[LOBBY_NAME];
    if (!ldn_code_normalize(typed, code)) return false;
    if (server && *server) snprintf(srv, sizeof srv, "%s", server);
    else if (!lobby_server(srv)) return false;
    hnet_init();
    pthread_mutex_lock(&invite_mu);
    uint32_t gen = ++invite.gen;
    invite.state = LDN_INVITE_SEARCHING;
    snprintf(invite.code, sizeof invite.code, "%s", code);
    snprintf(invite.server, sizeof invite.server, "%s", srv);
    pthread_mutex_unlock(&invite_mu);
    if (!spawn(invite_thread, (void *)(uintptr_t)gen)) {
        pthread_mutex_lock(&invite_mu);
        if (invite.gen == gen) invite.state = LDN_INVITE_FAILED;
        pthread_mutex_unlock(&invite_mu);
    }
    return true;
}

int ldn_invite_state(char *code, size_t size) {
    pthread_mutex_lock(&invite_mu);
    int state = invite.state;
    if (state == LDN_INVITE_READY && plat_now_ns() - invite.at > INVITE_KEEP_NS) state = invite.state = LDN_INVITE_NONE;
    if (code) code_shown(invite.code, code, size);
    pthread_mutex_unlock(&invite_mu);
    return state;
}

// For Discord: "code@server" of the hub we're in (empty: none to share).
bool ldn_join_secret(char *out, size_t size) {
    char code[16], srv[LOBBY_NAME];
    if (!ldn_session_code(code, sizeof code) || !lobby_server(srv)) return false;
    snprintf(out, size, "%s@%s", code, srv);
    return true;
}

// A join secret from Discord (someone's "Join"): only for our own hub server, so that a made-up one can't send the
// game to a server the player never chose.
void ldn_join_from_secret(const char *secret) {
    char code[CODE_LEN + 1], srv[LOBBY_NAME], mine[LOBBY_NAME], part[32];
    const char *at = strchr(secret, '@');
    if (!at || (size_t)(at - secret) >= sizeof part) return;
    memcpy(part, secret, (size_t)(at - secret)), part[at - secret] = 0;
    snprintf(srv, sizeof srv, "%s", at + 1);
    if (!ldn_code_normalize(part, code)) return;
    int lang = config_language();
    if (!lobby_server(mine) || strcmp(mine, srv)) {
        LOG("[ldn] Discord join: the hub is on another hub server (%s)\n", srv);
        char line[LOBBY_NAME + 128];
        snprintf(line, sizeof line, settings_text(lang, UI_TEXT("That hub is on the hub server %s")), srv);
        overlay_notice(line, settings_text(lang, UI_TEXT("Set the same hub server in Multiplayer to join it.")));
        return;
    }
    LOG("[ldn] Discord join: session code %s\n", code);
    ldn_join_code(code, NULL);
}

// ---- friends (link version 4)
// This install's identity is a random secret in the save folder (friends-id.bin: it goes with the saves, never in the
// settings file people share); its friend code is SHA-256("MHGURecomp friend:" + secret), the first 10 bytes, shown
// as 16 characters. friends.txt beside it lists the friends: a code and a name a line. A thread tells the hub server
// every FRIENDS_POLL_NS (and soon after we start or stop hosting, join or leave) what we do and the hunter name our
// requests show, and hears which friends are online that list us back, and who lists us that we don't list: friend
// requests. Adding a player is the request; accepting is adding them back. A rejected request stays out until the game
// restarts (F.rejected), unless we add that player ourselves. In a hub, the games of version 4 tell each other their
// friend codes and hunter names (F_AUX 0xE9 hellos), so a hunter there is added with a click. ini FriendRequests Off:
// no requests shown, no hellos sent (adding by code still works).
#define FRIENDS_POLL_NS 30000000000ull
#define FRIENDS_GAP_NS 5000000000ull  // (the least between two requests: a hosted or joined hub may change fast)
#define HELLO_EVERY_NS 60000000000ull  // a hub's hellos also again this often (and whenever its players change)
#define FRIEND_REQUESTS_MAX 8
#define FRIENDS_REMOVED_MAX 64  // (the server takes this many in a PRESENCE; full: the oldest goes)
#define HUNTERS_MAX (MAX_NODES - 1)
#define FRIEND_MAGIC 0xE9u  // aux message (emsync.c 0xE5, hpscale.c 0xE6, hitsync.c 0xE7, partsync.c 0xE8)
static pthread_mutex_t friends_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t friends_cv = PTHREAD_COND_INITIALIZER;
static struct {
    bool loaded, started, poked, polled;
    uint8_t secret[FRIEND_SECRET], id[FRIEND_LEN];
    int n, server_state;
    LdnFriend list[LDN_FRIENDS_MAX];
    uint8_t ids[LDN_FRIENDS_MAX][FRIEND_LEN];
    // requests now (from the last answer), the ones rejected this session, and when each player's request last got a
    // notice (friend_request_noted)
    int nreq;
    LdnRequest req[FRIEND_REQUESTS_MAX];
    uint8_t req_ids[FRIEND_REQUESTS_MAX][FRIEND_LEN];
    int nrejected, ntold;
    uint8_t rejected[64][FRIEND_LEN], told[64][FRIEND_LEN];
    uint64_t told_ns[64];
    // the players removed from the friends (friends.txt "removed" lines): their requests aren't shown until they are
    // added again, and the hub server is told, so their games drop us too (an old listing of theirs is no request)
    int nremoved;
    uint8_t removed[FRIENDS_REMOVED_MAX][FRIEND_LEN];
    char removed_names[FRIENDS_REMOVED_MAX][33];
    // the hub's hunters that said hello: by their address in the network
    int nhunters;
    struct { uint32_t ip; uint8_t id[FRIEND_LEN]; char name[33]; } hunters[HUNTERS_MAX];
} F = {.server_state = HUB_UNKNOWN};

static bool id_in(const uint8_t (*set)[FRIEND_LEN], int n, const uint8_t *id) {
    for (int i = 0; i < n; i++)
        if (!memcmp(set[i], id, FRIEND_LEN)) return true;
    return false;
}
static void id_add(uint8_t (*set)[FRIEND_LEN], int *n, int max, const uint8_t *id) {  // (full: the oldest goes)
    if (id_in((const uint8_t (*)[FRIEND_LEN])set, *n, id)) return;
    if (*n == max) memmove(set[0], set[1], (size_t)(max - 1) * FRIEND_LEN), (*n)--;
    memcpy(set[(*n)++], id, FRIEND_LEN);
}
// A notice for a friend request: when it is new since the last answer (sent, sent again, or its sender back online),
// but not twice within FRIEND_NOTE_AGAIN_NS from the same player, so nobody can fill the screen with them (friends_mu held)
#define FRIEND_NOTE_AGAIN_NS 600000000000ull
static bool friend_request_noted(const uint8_t *id) {
    uint64_t now = plat_now_ns();
    for (int i = 0; i < F.ntold; i++)
        if (!memcmp(F.told[i], id, FRIEND_LEN)) {
            if (now - F.told_ns[i] < FRIEND_NOTE_AGAIN_NS) return false;
            F.told_ns[i] = now;
            return true;
        }
    if (F.ntold == 64) {  // (full: the oldest goes)
        memmove(F.told[0], F.told[1], 63 * FRIEND_LEN);
        memmove(&F.told_ns[0], &F.told_ns[1], 63 * sizeof F.told_ns[0]);
        F.ntold--;
    }
    memcpy(F.told[F.ntold], id, FRIEND_LEN);
    F.told_ns[F.ntold++] = now;
    return true;
}
static bool friend_removed_del(const uint8_t *id);
static bool friend_drop_locked(const uint8_t *id);
static void friends_save_locked(void);
static void id_remove(uint8_t (*set)[FRIEND_LEN], int *n, const uint8_t *id) {
    for (int i = 0; i < *n; i++)
        if (!memcmp(set[i], id, FRIEND_LEN)) {
            memmove(set[i], set[i + 1], (size_t)(*n - i - 1) * FRIEND_LEN);
            (*n)--;
            return;
        }
}

static void friends_file(char *out, size_t size, const char *name) {
    const char *dir = getenv("MHGU_SAVE_DIR");
    snprintf(out, size, "%s/%s", dir && *dir ? dir : g_save_dir, name);
}

static void friend_text(const uint8_t *id, char *out) {  // 10 bytes -> 16 characters, 5 bits each (most significant first)
    for (int i = 0; i < 16; i++) {
        int v = 0;
        for (int b = 0; b < 5; b++) {
            int k = i * 5 + b;
            v = v << 1 | (id[k / 8] >> (7 - k % 8) & 1);
        }
        out[i] = CODE_ALPHABET[v];
    }
    out[16] = 0;
}

static bool friend_parse(const char *typed, uint8_t *id) {  // the 16 characters (as ldn_code_normalize reads them)
    uint8_t v[16];
    int n = 0;
    for (; *typed && n <= 16; typed++) {
        char ch = *typed;
        if (ch == ' ' || ch == '-') continue;
        if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
        if (ch == 'O') ch = '0';
        if (ch == 'I' || ch == 'L') ch = '1';
        const char *p = strchr(CODE_ALPHABET, ch);
        if (!p || !ch || n == 16) return false;
        v[n++] = (uint8_t)(p - CODE_ALPHABET);
    }
    if (n != 16) return false;
    memset(id, 0, FRIEND_LEN);
    for (int i = 0; i < 16; i++)
        for (int b = 0; b < 5; b++)
            if (v[i] >> (4 - b) & 1) id[(i * 5 + b) / 8] |= (uint8_t)(0x80 >> ((i * 5 + b) % 8));
    return true;
}

static void friend_shown(const char *text, char *out, size_t size) {  // "ABCD-EFGH-JKMN-PQRS"
    snprintf(out, size, "%.4s-%.4s-%.4s-%.4s", text, text + 4, text + 8, text + 12);
}

static void friends_save_locked(void) {
    char path[512], tmp[520];
    friends_file(path, sizeof path, "friends.txt");
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *f = fopen(tmp, "w");
    if (!f) { LOG("[ldn] can't write %s\n", tmp); return; }
    fprintf(f, "# MHGURecomp friends: a friend code and a name a line (the settings menu's Friends tab edits it)\n");
    for (int i = 0; i < F.n; i++) {
        char t[17];
        friend_text(F.ids[i], t);
        fprintf(f, "%s %s\n", t, F.list[i].name);
    }
    // ("removed" lines: players taken out of the friends; older games skip them, not being codes)
    for (int i = 0; i < F.nremoved; i++) {
        char t[17];
        friend_text(F.removed[i], t);
        fprintf(f, "removed %s %s\n", t, F.removed_names[i]);
    }
    if (fclose(f) || (remove(path), rename(tmp, path))) LOG("[ldn] can't write %s\n", path);
}

static void friends_load_locked(void) {
    if (F.loaded) return;
    F.loaded = true;
    char path[512];
    friends_file(path, sizeof path, "friends-id.bin");
    FILE *f = fopen(path, "rb");
    bool have = f && fread(F.secret, 1, FRIEND_SECRET, f) == FRIEND_SECRET;
    if (f) fclose(f);
    if (!have) {  // a new identity: made once, then kept with the saves
        rand_bytes(F.secret, FRIEND_SECRET);
        mkdir(getenv("MHGU_SAVE_DIR") && *getenv("MHGU_SAVE_DIR") ? getenv("MHGU_SAVE_DIR") : g_save_dir, 0755);  // (a new install)
        if ((f = fopen(path, "wb"))) {
            fwrite(F.secret, 1, FRIEND_SECRET, f);
            fclose(f);
            LOG("[ldn] a friend code made for this install (%s)\n", path);
        } else {
            LOG("[ldn] can't write %s: the friend code lasts until the game closes\n", path);
        }
    }
    Sha256 s;
    char hex[65];
    sha256_init(&s);
    sha256_update(&s, "MHGURecomp friend:", 18);
    sha256_update(&s, F.secret, FRIEND_SECRET);
    sha256_hex(&s, hex);
    for (int i = 0; i < FRIEND_LEN; i++) {
        unsigned v;
        sscanf(hex + 2 * i, "%2x", &v);
        F.id[i] = (uint8_t)v;
    }
    friends_file(path, sizeof path, "friends.txt");
    if (!(f = fopen(path, "r"))) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *e = line + strlen(line);
        while (e > line && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = 0;
        char *sp = strchr(line, ' ');
        if (line[0] == '#' || !line[0]) continue;
        if (sp) *sp = 0;
        else sp = e - 1;  // (a code without a name: sp + 1 is the empty string at the end)
        uint8_t id[FRIEND_LEN];
        if (!strcmp(line, "removed")) {  // removed CODE name
            char *code = sp + 1, *nsp = strchr(code, ' ');
            if (nsp) *nsp = 0;
            if (friend_parse(code, id) && memcmp(id, F.id, FRIEND_LEN) && F.nremoved < FRIENDS_REMOVED_MAX &&
                !id_in((const uint8_t (*)[FRIEND_LEN])F.removed, F.nremoved, id)) {
                snprintf(F.removed_names[F.nremoved], sizeof F.removed_names[0], "%s", nsp ? nsp + 1 : "");
                memcpy(F.removed[F.nremoved++], id, FRIEND_LEN);
            }
            continue;
        }
        if (F.n >= LDN_FRIENDS_MAX || !friend_parse(line, id) || !memcmp(id, F.id, FRIEND_LEN)) continue;
        LdnFriend *fr = &F.list[F.n];
        memset(fr, 0, sizeof *fr);
        char t[17];
        friend_text(id, t);
        friend_shown(t, fr->code, sizeof fr->code);
        snprintf(fr->name, sizeof fr->name, "%s", sp[1] ? sp + 1 : fr->code);  // (no name: the code, as when added)
        fr->state = LDN_FRIEND_UNKNOWN;
        memcpy(F.ids[F.n++], id, FRIEND_LEN);
    }
    fclose(f);
}

static void friends_poke(void) {
    pthread_mutex_lock(&friends_mu);
    F.poked = true;
    pthread_cond_signal(&friends_cv);
    pthread_mutex_unlock(&friends_mu);
}

// What we do, for the friends: hosting a hub (with its code once listed with one), in a hub joined through the hub
// server (its code when we know it), or just in the game. Hunter Search networks don't count.
static uint8_t friends_my_state(char *code) {
    uint8_t state = 0;
    char own[16] = "", joined[16] = "";
    memset(code, 0, CODE_LEN);
    pthread_mutex_lock(&L.mu);
    if (!L.ryu && !L.room && !hunt_search(L.info)) {
        if (L.state == ST_AP_CREATED) {
            state = 1;
            if (mhgu_ldn_own_code(own, sizeof own)) {
                char raw[CODE_LEN + 1] = "";
                ldn_code_normalize(own, raw);
                memcpy(code, raw, CODE_LEN);
            }
        } else if (L.state == ST_STATION_CONNECTED) {
            state = 2;
            if (mhgu_ldn_relayed() && mhgu_ldn_joined_code(joined, sizeof joined)) {
                char raw[CODE_LEN + 1] = "";
                ldn_code_normalize(joined, raw);
                memcpy(code, raw, CODE_LEN);
            }
        }
    }
    pthread_mutex_unlock(&L.mu);
    return state;
}

// The name our friend requests and hellos show: the hunter's, without anything that isn't plain text (at most 32 bytes).
static size_t friends_my_name(char *out) {
    char name[33] = "";
    presence_hunter_name(name, sizeof name);
    size_t n = 0;
    for (const char *p = name; *p && n < 32; p++)
        if ((unsigned char)*p >= 0x20 && *p != 0x7F) out[n++] = *p;
    out[n] = 0;
    return n;
}

// A friend hello into the hub (to every other game; version 3 games drop it): u8 magic, u8 1, u8 name length, u8 0,
// u32 our address in the network, friend code[10], name. Sent when the hub's players change and every
// HELLO_EVERY_NS; not in crossplay (no F_AUX there) nor with FriendRequests Off. Also forgets hunters who left.
static void friends_hello_tick(void) {
    static uint64_t sig, next;
    bool up = ldn_net_up() && !ldn_crossplay() && !L.ryu && !L.room;
    uint64_t s = 0;
    uint32_t own = 0;
    uint32_t ips[MAX_NODES] = {0};
    int nips = 0;
    if (up) {
        pthread_mutex_lock(&L.mu);
        own = L.own_ip;
        for (int i = 0; i < MAX_NODES; i++) {
            const uint8_t *nd = node_at(L.info, i);
            if (!nd[NODE_CONNECTED]) continue;
            ips[nips++] = rd32(nd + NODE_IP);
            s = s * 1000003u + rd32(nd + NODE_IP);
        }
        pthread_mutex_unlock(&L.mu);
        s = s * 31 + own;
    }
    pthread_mutex_lock(&friends_mu);  // (hunters no longer in the hub, or every one when there is none)
    for (int i = 0; i < F.nhunters;) {
        bool in = false;
        for (int k = 0; k < nips; k++) in |= ips[k] == F.hunters[i].ip && ips[k] != own;
        if (in) { i++; continue; }
        memmove(&F.hunters[i], &F.hunters[i + 1], (size_t)(F.nhunters - i - 1) * sizeof F.hunters[0]);
        F.nhunters--;
    }
    uint8_t id[FRIEND_LEN];
    memcpy(id, F.id, FRIEND_LEN);
    pthread_mutex_unlock(&friends_mu);
    uint64_t now = plat_now_ns();
    if (!up || !config_live_friend_requests() || nips < 2) { sig = 0; return; }
    if (s == sig && now < next) return;
    sig = s, next = now + HELLO_EVERY_NS;
    uint8_t m[8 + FRIEND_LEN + 32];
    char name[33];
    size_t nl = friends_my_name(name);
    m[0] = FRIEND_MAGIC, m[1] = 1, m[2] = (uint8_t)nl, m[3] = 0;
    wr32(m + 4, own);
    memcpy(m + 8, id, FRIEND_LEN);
    memcpy(m + 8 + FRIEND_LEN, name, nl);
    ldn_send_aux(m, (uint32_t)(8 + FRIEND_LEN + nl));
}

void friends_aux_received(const void *data, uint32_t len) {
    const uint8_t *m = data;
    if (len < 8 + FRIEND_LEN || m[0] != FRIEND_MAGIC || m[1] != 1 || m[2] > 32 || len < 8u + FRIEND_LEN + m[2]) return;
    uint32_t ip = rd32(m + 4);
    pthread_mutex_lock(&friends_mu);
    if (memcmp(m + 8, F.id, FRIEND_LEN)) {
        int i = 0;
        while (i < F.nhunters && F.hunters[i].ip != ip) i++;
        if (i == F.nhunters && F.nhunters < HUNTERS_MAX)
            F.nhunters++, LOG("[ldn] friends: a hunter in the hub shared their friend code (%d now)\n", F.nhunters);
        if (i < F.nhunters) {
            F.hunters[i].ip = ip;
            memcpy(F.hunters[i].id, m + 8, FRIEND_LEN);
            size_t k = 0;
            for (size_t j = 0; j < m[2]; j++)
                if (m[8 + FRIEND_LEN + j] >= 0x20 && m[8 + FRIEND_LEN + j] != 0x7F) F.hunters[i].name[k++] = (char)m[8 + FRIEND_LEN + j];
            F.hunters[i].name[k] = 0;
        }
        // A friend in our list with no name of their own takes the hunter name their game goes by.
        uint8_t was[FRIEND_LEN];
        char shown[33] = "";
        size_t k = 0;
        for (size_t j = 0; j < m[2]; j++)
            if (m[8 + FRIEND_LEN + j] >= 0x20 && m[8 + FRIEND_LEN + j] != 0x7F) shown[k++] = (char)m[8 + FRIEND_LEN + j];
        shown[k] = 0;
        for (int f = 0; k && f < F.n; f++) {
            LdnFriend *fr = &F.list[f];
            if (memcmp(F.ids[f], m + 8, FRIEND_LEN) || (fr->name[0] && !(friend_parse(fr->name, was) && !memcmp(was, F.ids[f], FRIEND_LEN))))
                continue;
            snprintf(fr->name, sizeof fr->name, "%s", shown);
            LOG("[ldn] friends: %s is named %s from the hub\n", fr->code, fr->name);
            friends_save_locked();
            break;
        }
    }
    pthread_mutex_unlock(&friends_mu);
}

static void *friends_thread(void *arg) {
    (void)arg;
    static uint8_t buf[MAX_FRAME];
    uint64_t last = 0;
    for (;;) {
        pthread_mutex_lock(&friends_mu);
        for (;;) {
            uint64_t now = plat_now_ns();
            if (now - last >= FRIENDS_POLL_NS || (F.poked && now - last >= FRIENDS_GAP_NS)) break;
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 1;
            pthread_cond_timedwait(&friends_cv, &friends_mu, &ts);
            pthread_mutex_unlock(&friends_mu);
            friends_hello_tick();  // (every second: the hub's hellos)
            pthread_mutex_lock(&friends_mu);
        }
        F.poked = false;
        int n = F.n;
        uint8_t ids[LDN_FRIENDS_MAX][FRIEND_LEN], secret[FRIEND_SECRET], removed[FRIENDS_REMOVED_MAX][FRIEND_LEN];
        memcpy(ids, F.ids, sizeof ids);
        memcpy(secret, F.secret, sizeof secret);
        int nremoved = F.nremoved;
        memcpy(removed, F.removed, sizeof removed);
        pthread_mutex_unlock(&friends_mu);
        last = plat_now_ns();
        char srv[LOBBY_NAME];
        int state = HUB_UNKNOWN;
        bool requests = config_live_friend_requests();
        if (!n && !requests) continue;  // (nobody to tell, nobody to hear from)
        if (!lobby_server(srv)) state = HUB_OFF;
        uint8_t req[LOBBY_HEADER + FRIEND_SECRET + 1 + CODE_LEN + 1 + LDN_FRIENDS_MAX * FRIEND_LEN + 1 + 32 + 1 +
                    FRIENDS_REMOVED_MAX * FRIEND_LEN], type = 0;
        int got = -1;
        struct sockaddr_in addr;
        if (state != HUB_OFF) {
            state = HUB_NOT_FOUND;
            if (resolve_host(srv, LOBBY_PORT, &addr)) {
                state = HUB_UNREACHABLE;
                hsock s = connect_to(&addr, 3000);
                if (s != BADSOCK) {
                    lobby_header(req, NULL);
                    uint8_t *p = req + LOBBY_HEADER;
                    memcpy(p, secret, FRIEND_SECRET), p += FRIEND_SECRET;
                    *p = friends_my_state((char *)p + 1), p += 1 + CODE_LEN;
                    *p++ = (uint8_t)n;
                    memcpy(p, ids, (size_t)n * FRIEND_LEN), p += n * FRIEND_LEN;
                    char name[33];
                    size_t nl = friends_my_name(name);  // (what our requests show the players we added)
                    if (nl || nremoved) *p++ = (uint8_t)nl, memcpy(p, name, nl), p += nl;
                    if (nremoved) {  // (after the name: the players we removed; an older server ignores them)
                        *p++ = (uint8_t)nremoved;
                        memcpy(p, removed, (size_t)nremoved * FRIEND_LEN), p += nremoved * FRIEND_LEN;
                    }
                    hset_timeout(s, 4000);
                    got = frame_send(s, S_PRESENCE, req, (uint32_t)(p - req), NULL, 0) ? frame_recv(s, &type, buf) : -1;
                    hclose(s);
                    state = got >= 1 && type == S_FRIENDS ? HUB_ONLINE
                          : got >= 4 && type == S_ERROR   ? lobby_state(rd32(buf))
                                                          : HUB_VERSION;  // (an older server hangs up on PRESENCE)
                }
            }
        }
        char notes[6][96];
        int nn = 0;
        pthread_mutex_lock(&friends_mu);
        if (state != F.server_state && (state == HUB_ONLINE || F.server_state == HUB_ONLINE || F.server_state == HUB_UNKNOWN))
            LOG("[ldn] friends: the hub server %s\n", state == HUB_ONLINE ? "answers"
                : state == HUB_VERSION ? "doesn't know friends (older than this game)" : "can't be asked now");
        F.server_state = state;
        int was_req = F.nreq, was_mutual = 0, now_mutual = 0;
        for (int i = 0; i < F.n; i++) was_mutual += F.list[i].mutual && F.list[i].state != LDN_FRIEND_OFFLINE;
        if (state == HUB_ONLINE) {
            int count = buf[0];
            const int entry = FRIEND_LEN + 1 + CODE_LEN;
            for (int i = 0; i < F.n; i++) {
                LdnFriend *fr = &F.list[i];
                int was = fr->state, now = LDN_FRIEND_OFFLINE;
                fr->hub[0] = 0;
                fr->mutual = false;
                for (int k = 0; k < count && 1 + (k + 1) * entry <= got; k++) {
                    const uint8_t *e = buf + 1 + k * entry;
                    if (memcmp(e, F.ids[i], FRIEND_LEN)) continue;
                    uint8_t st = e[FRIEND_LEN];
                    now = st == 1 ? LDN_FRIEND_HOSTING : st == 2 ? LDN_FRIEND_IN_HUB : LDN_FRIEND_ONLINE;
                    memcpy(fr->hub, e + FRIEND_LEN + 1, CODE_LEN);
                    fr->hub[CODE_LEN] = 0;
                    fr->mutual = true;  // (the server tells only friends who list us back)
                }
                fr->state = now;
                // A notice when a friend starts hosting (FriendHubNotices) or comes online (FriendOnlineNotices);
                // none from the first answer (it only learns who is online already) or for a friend still unknown.
                bool hosting = now == LDN_FRIEND_HOSTING && was != LDN_FRIEND_HOSTING;
                bool came = now != LDN_FRIEND_OFFLINE && was == LDN_FRIEND_OFFLINE;
                if (F.polled && was != LDN_FRIEND_UNKNOWN && nn < 4 &&
                    ((hosting && config_live_friend_hub_notices()) || (came && config_live_friend_online_notices())))
                    snprintf(notes[nn++], sizeof notes[0], "%c%s", hosting ? 'H' : 'O', fr->name);
            }
            // the requests after the friends: u8 r, r x (friend[10], u8 len, name)
            uint8_t before[FRIEND_REQUESTS_MAX][FRIEND_LEN];
            int nbefore = F.nreq;
            memcpy(before, F.req_ids, sizeof before);
            F.nreq = 0;
            int at = 1 + count * entry;
            int r = at < got ? buf[at++] : 0, k = 0;
            for (; k < r && at + FRIEND_LEN + 1 <= got; k++) {
                const uint8_t *e = buf + at;
                int nl = e[FRIEND_LEN];
                if (nl > 32 || at + FRIEND_LEN + 1 + nl > got) break;
                at += FRIEND_LEN + 1 + nl;
                // (not shown: rejected this session, removed from the friends until added again, a friend already)
                if (!requests || F.nreq == FRIEND_REQUESTS_MAX ||
                    id_in((const uint8_t (*)[FRIEND_LEN])F.rejected, F.nrejected, e) ||
                    id_in((const uint8_t (*)[FRIEND_LEN])F.removed, F.nremoved, e) ||
                    id_in((const uint8_t (*)[FRIEND_LEN])F.ids, F.n, e) || !memcmp(e, F.id, FRIEND_LEN))
                    continue;
                LdnRequest *q = &F.req[F.nreq];
                char t[17];
                friend_text(e, t);
                friend_shown(t, q->code, sizeof q->code);
                size_t w = 0;
                for (int j = 0; j < nl && w < 32; j++)
                    if (e[FRIEND_LEN + 1 + j] >= 0x20 && e[FRIEND_LEN + 1 + j] != 0x7F) q->name[w++] = (char)e[FRIEND_LEN + 1 + j];
                q->name[w] = 0;
                if (!w) snprintf(q->name, sizeof q->name, "%s", q->code);
                memcpy(F.req_ids[F.nreq++], e, FRIEND_LEN);
                if (!id_in((const uint8_t (*)[FRIEND_LEN])before, nbefore, e) && nn < 6 && friend_request_noted(e))
                    snprintf(notes[nn++], sizeof notes[0], "R%s", q->name);
            }
            // then (a server that knows removals, only when there are any) u8 g, g x friend[10]: friends that removed
            // us. They go from the list too (not into the removed list: a request of theirs later is a new one).
            if (k == r && at < got) {
                int g = buf[at++];
                bool changed = false;
                for (int i = 0; i < g && at + FRIEND_LEN <= got; i++, at += FRIEND_LEN)
                    for (int f = 0; f < F.n; f++)
                        if (!memcmp(F.ids[f], buf + at, FRIEND_LEN)) {
                            LOG("[ldn] friends: %s removed us: out of the friends list\n", F.list[f].name);
                            friend_drop_locked(buf + at);
                            changed = true;
                            break;
                        }
                if (changed) friends_save_locked();
            }
            F.polled = true;
            for (int i = 0; i < F.n; i++) now_mutual += F.list[i].mutual && F.list[i].state != LDN_FRIEND_OFFLINE;
            if (F.nreq != was_req) LOG("[ldn] friends: %d friend request(s)\n", F.nreq);
            if (now_mutual != was_mutual) LOG("[ldn] friends: %d of %d friend(s) online\n", now_mutual, F.n);
        }
        pthread_mutex_unlock(&friends_mu);
        int lang = config_language();
        for (int i = 0; i < nn; i++) {
            char line[160];
            const char *title = notes[i][0] == 'H' ? UI_TEXT("%s is hosting a hub")
                              : notes[i][0] == 'R' ? UI_TEXT("%s wants to be friends")
                                                   : UI_TEXT("%s is online");
            const char *text = notes[i][0] == 'H' ? UI_TEXT("To join: settings menu, Friends tab.")
                             : notes[i][0] == 'R' ? UI_TEXT("To accept or reject: settings menu, Friends tab.")
                                                  : UI_TEXT("Your friend is playing.");
            snprintf(line, sizeof line, settings_text(lang, title), notes[i] + 1);
            overlay_notice(line, settings_text(lang, text));
        }
    }
    return NULL;
}

void ldn_friends_start(void) {
    pthread_mutex_lock(&friends_mu);
    friends_load_locked();
    bool start = !F.started;
    F.started = true;
    pthread_mutex_unlock(&friends_mu);
    if (start) hnet_init(), spawn(friends_thread, NULL);
}

void ldn_my_friend_code(char *out, size_t size) {
    pthread_mutex_lock(&friends_mu);
    friends_load_locked();
    char t[17];
    friend_text(F.id, t);
    pthread_mutex_unlock(&friends_mu);
    friend_shown(t, out, size);
}

int ldn_friends(LdnFriend *out, int max, int *server_state) {
    pthread_mutex_lock(&friends_mu);
    friends_load_locked();
    int n = F.n < max ? F.n : max;
    memcpy(out, F.list, (size_t)n * sizeof *out);
    if (server_state) *server_state = F.server_state;
    pthread_mutex_unlock(&friends_mu);
    return n;
}

// "code name" as typed (the name may be left out). NULL: added; else why not (UI_TEXT: the menu translates it).
const char *ldn_friend_add(const char *typed) {
    char code[64], name[64] = "";
    while (*typed == ' ') typed++;
    const char *sp = strchr(typed, ' ');
    size_t cl = sp ? (size_t)(sp - typed) : strlen(typed);
    if (cl >= sizeof code) return UI_TEXT("That is not a friend code: 16 letters and digits.");
    memcpy(code, typed, cl), code[cl] = 0;
    if (sp) {
        while (*sp == ' ') sp++;
        snprintf(name, sizeof name, "%.32s", sp);
    }
    uint8_t id[FRIEND_LEN];
    if (!friend_parse(code, id)) return UI_TEXT("That is not a friend code: 16 letters and digits.");
    pthread_mutex_lock(&friends_mu);
    friends_load_locked();
    const char *why = NULL;
    if (!memcmp(id, F.id, FRIEND_LEN)) why = UI_TEXT("That is your own friend code.");
    for (int i = 0; !why && i < F.n; i++)
        if (!memcmp(id, F.ids[i], FRIEND_LEN)) why = UI_TEXT("That friend is in the list already.");
    if (!why && F.n >= LDN_FRIENDS_MAX) why = UI_TEXT("The friends list is full.");
    if (!why) {
        LdnFriend *fr = &F.list[F.n];
        memset(fr, 0, sizeof *fr);
        char t[17];
        friend_text(id, t);
        friend_shown(t, fr->code, sizeof fr->code);
        snprintf(fr->name, sizeof fr->name, "%s", *name ? name : fr->code);
        fr->state = LDN_FRIEND_UNKNOWN;
        memcpy(F.ids[F.n++], id, FRIEND_LEN);
        friend_removed_del(id);  // (a player we removed, added again: out of the removed list)
        friends_save_locked();
        // (adding a player we rejected takes them back; their request, if any, is answered by this)
        id_remove(F.rejected, &F.nrejected, id);
        for (int i = 0; i < F.nreq; i++)
            if (!memcmp(F.req_ids[i], id, FRIEND_LEN)) {
                memmove(&F.req[i], &F.req[i + 1], (size_t)(F.nreq - i - 1) * sizeof F.req[0]);
                memmove(F.req_ids[i], F.req_ids[i + 1], (size_t)(F.nreq - i - 1) * FRIEND_LEN);
                F.nreq--;
                break;
            }
    }
    pthread_mutex_unlock(&friends_mu);
    if (!why) friends_poke();
    return why;
}

// The removed list (friends_mu held): a player in (with the name they had; full: the oldest goes), or out
static void friend_removed_add(const uint8_t *id, const char *name) {
    for (int i = 0; i < F.nremoved; i++)
        if (!memcmp(F.removed[i], id, FRIEND_LEN)) return;
    if (F.nremoved == FRIENDS_REMOVED_MAX) {
        memmove(F.removed[0], F.removed[1], (FRIENDS_REMOVED_MAX - 1) * FRIEND_LEN);
        memmove(F.removed_names[0], F.removed_names[1], (FRIENDS_REMOVED_MAX - 1) * sizeof F.removed_names[0]);
        F.nremoved--;
    }
    snprintf(F.removed_names[F.nremoved], sizeof F.removed_names[0], "%s", name);
    memcpy(F.removed[F.nremoved++], id, FRIEND_LEN);
}
static bool friend_removed_del(const uint8_t *id) {
    for (int i = 0; i < F.nremoved; i++)
        if (!memcmp(F.removed[i], id, FRIEND_LEN)) {
            memmove(F.removed[i], F.removed[i + 1], (size_t)(F.nremoved - i - 1) * FRIEND_LEN);
            memmove(F.removed_names[i], F.removed_names[i + 1], (size_t)(F.nremoved - i - 1) * sizeof F.removed_names[0]);
            F.nremoved--;
            return true;
        }
    return false;
}
// friends_mu held: a friend out of the list (and saved); false: not in it
static bool friend_drop_locked(const uint8_t *id) {
    for (int i = 0; i < F.n; i++)
        if (!memcmp(id, F.ids[i], FRIEND_LEN)) {
            memmove(&F.list[i], &F.list[i + 1], (size_t)(F.n - i - 1) * sizeof F.list[0]);
            memmove(F.ids[i], F.ids[i + 1], (size_t)(F.n - i - 1) * FRIEND_LEN);
            F.n--;
            return true;
        }
    return false;
}

// Removing a friend: out of the list, into the removed list (kept in friends.txt): their request is not shown again
// (their game may still list us: that would come back as one) until we add them ourselves, and the hub server tells
// their game, which drops us too.
void ldn_friend_remove(const char *code) {
    uint8_t id[FRIEND_LEN];
    if (!friend_parse(code, id)) return;
    pthread_mutex_lock(&friends_mu);
    for (int i = 0; i < F.n; i++)
        if (!memcmp(id, F.ids[i], FRIEND_LEN)) {
            friend_removed_add(id, F.list[i].name);
            friend_drop_locked(id);
            friends_save_locked();
            break;
        }
    pthread_mutex_unlock(&friends_mu);
    friends_poke();
}

int ldn_friend_requests(LdnRequest *out, int max) {
    pthread_mutex_lock(&friends_mu);
    int n = config_live_friend_requests() ? (F.nreq < max ? F.nreq : max) : 0;
    memcpy(out, F.req, (size_t)n * sizeof *out);
    pthread_mutex_unlock(&friends_mu);
    return n;
}

// Accept: the player is added (with the name their request showed). Reject: out of the requests until the game
// restarts (or until we add them ourselves).
void ldn_friend_request_answer(const char *code, bool accept) {
    uint8_t id[FRIEND_LEN];
    if (!friend_parse(code, id)) return;
    char name[33] = "";
    pthread_mutex_lock(&friends_mu);
    for (int i = 0; i < F.nreq; i++)
        if (!memcmp(F.req_ids[i], id, FRIEND_LEN)) {
            snprintf(name, sizeof name, "%s", F.req[i].name);
            memmove(&F.req[i], &F.req[i + 1], (size_t)(F.nreq - i - 1) * sizeof F.req[0]);
            memmove(F.req_ids[i], F.req_ids[i + 1], (size_t)(F.nreq - i - 1) * FRIEND_LEN);
            F.nreq--;
            break;
        }
    if (!accept) id_add(F.rejected, &F.nrejected, 64, id);
    pthread_mutex_unlock(&friends_mu);
    if (accept) {
        char typed[64];
        snprintf(typed, sizeof typed, "%s %s", code, name);
        ldn_friend_add(typed);
    }
    LOG("[ldn] friend request %s\n", accept ? "accepted" : "rejected (until the game restarts)");
}

int ldn_hub_hunters(LdnHunter *out, int max) {
    pthread_mutex_lock(&friends_mu);
    int n = 0;
    for (int i = 0; i < F.nhunters && n < max; i++) {
        LdnHunter *h = &out[n++];
        char t[17];
        friend_text(F.hunters[i].id, t);
        friend_shown(t, h->code, sizeof h->code);
        snprintf(h->name, sizeof h->name, "%s", F.hunters[i].name[0] ? F.hunters[i].name : h->code);
        h->state = LDN_HUNTER_ADD;
        for (int k = 0; k < F.n; k++)
            if (!memcmp(F.ids[k], F.hunters[i].id, FRIEND_LEN)) h->state = F.list[k].mutual ? LDN_HUNTER_FRIEND : LDN_HUNTER_ADDED;
    }
    pthread_mutex_unlock(&friends_mu);
    return n;
}

// An account id from a friend code: SHA-256("MHGURecomp account:" + code), the first 8 bytes (little endian), never 0
// nor the id every game had before version 4 (0x1234567890abcdef). The hub's advertise data carries its host's at
// 0x14, and the game compares its friends' ids with it.
static uint64_t account_of(const uint8_t *id) {
    Sha256 s;
    char hex[65];
    sha256_init(&s);
    sha256_update(&s, "MHGURecomp account:", 19);
    sha256_update(&s, id, FRIEND_LEN);
    sha256_hex(&s, hex);
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) {
        unsigned b;
        sscanf(hex + 2 * i, "%2x", &b);
        v = v << 8 | b;
    }
    if (!v || v == 0x1234567890abcdefull) v ^= 0x8000000000000001ull;
    return v;
}

uint64_t ldn_account_id(void) {
    pthread_mutex_lock(&friends_mu);
    friends_load_locked();
    uint64_t v = account_of(F.id);
    pthread_mutex_unlock(&friends_mu);
    return v;
}

// The game's friends: those in our list the hub server has told us list us back (adding a code alone doesn't make
// someone a friend to the game: that would let them past a friends-only passcode one-sidedly).
int ldn_game_friends(uint64_t *ids, char (*names)[33], int max) {
    pthread_mutex_lock(&friends_mu);
    friends_load_locked();
    int n = 0;
    for (int i = 0; i < F.n && n < max; i++)
        if (F.list[i].mutual) {
            if (names) snprintf(names[n], 33, "%s", F.list[i].name);
            ids[n++] = account_of(F.ids[i]);
        }
    pthread_mutex_unlock(&friends_mu);
    return n;
}
int ldn_friend_account_ids(uint64_t *out, int max) { return ldn_game_friends(out, NULL, max); }

// ---- the Rust online module (mhgu-link-ffi) ----------------------------------------------------
// The host's transport, the LAN answer and the relay live there now; this file keeps the `nn::ldn`
// handlers and the guest structures, and hands the packets to the same local path as before.

static pthread_t link_poll_th;
static volatile int link_poll_run;

static void link_deliver(const MhguVPacket *p, const uint8_t *data, uint32_t len) {
    VPacket v;
    memcpy(&v, p, sizeof v);
    link_received(&v, data, len);
}

static void link_aux(const uint8_t *data, uint32_t len) {
    emsync_aux_received(data, len);
    hpscale_aux_received(data, len);
    hitsync_aux_received(data, len);
    partsync_aux_received(data, len);
    friends_aux_received(data, len);
    voice_aux_received(data, len);  // (runtime/voice.c: its own magic number)
}

static void *link_poll_thread(void *arg) {
    (void)arg;
    int last_nodes = -1;
    while (link_poll_run) {
        mhgu_ldn_poll();
        // A player joined or left: the host's game learns it from the state-change event, as ldn.c's
        // own host thread told it (signal_change), and refreshes its NetworkInfo.
        int n = 0, max = 0;
        char id[16];
        if (mhgu_ldn_party(&n, &max, id, sizeof id) && n != last_nodes) {
            last_nodes = n;
            pthread_mutex_lock(&L.mu);
            mhgu_ldn_info(L.info, NETINFO_SIZE);
            pthread_mutex_unlock(&L.mu);
            signal_change();
        }
        bool gone = false;
        pthread_mutex_lock(&L.mu);
        if (!L.ryu && !L.room && L.state == ST_STATION_CONNECTED && !mhgu_ldn_net_up()) {
            L.state = ST_ERROR;
            L.reason = mhgu_ldn_disconnect_reason();
            if (!L.reason) L.reason = DR_SIGNAL_LOST;
            gone = true;
        }
        pthread_mutex_unlock(&L.mu);
        if (gone) { vsock_network_down(); signal_change(); }
        plat_sleep_until_ns(plat_now_ns() + 2000000ull);
    }
    return NULL;
}

static void link_poll_start(void) {
    if (link_poll_run) return;
    mhgu_ldn_set_delivery(link_deliver, link_aux);
    link_poll_run = 1;
    if (pthread_create(&link_poll_th, NULL, link_poll_thread, NULL)) link_poll_run = 0;
}

static void link_poll_stop(void) {
    if (!link_poll_run) return;
    link_poll_run = 0;
    pthread_join(link_poll_th, NULL);
}

static bool host_start(void) {
    int port = config_live_ldn_port();
    char name[0x21] = {0};
    memcpy(name, node_at(L.info, 0) + NODE_NAME, 0x20);
    uint16_t bound = 0;
    mhgu_ldn_set_direct(config_live_ldn_direct());
    if (!mhgu_ldn_host((uint16_t)port, &bound, name, L.info, NETINFO_SIZE)) {
        LOG("[ldn] mhgu-link could not host on port %u (another game instance or another program holds it?)\n",
            (unsigned)port);
        return false;
    }
    L.hosting = true;
    L.port = port;
    link_poll_start();
    LOG("[ldn] hosting via mhgu-link on port %u\n", bound);
    char server[LOBBY_NAME], key[LOBBY_NAME];
    config_live_ldn(NULL, 0, key, sizeof key);  // [Multiplayer] Key as saved now
    // [Multiplayer] Server: empty = the default server, Off = none (lobby_server_of, as the settings menu shows)
    if (lobby_server(server) && mhgu_ldn_host_server(server, key)) LOG("[ldn] hub listed on %s\n", server);
    else if (server[0]) LOG("[ldn] hub server %s: the hub could not be listed (see [link] above)\n", server);
    return true;
}

static void host_stop(uint32_t reason) {
    (void)reason;
    if (!L.hosting) return;
    L.hosting = false;
    mhgu_ldn_leave();
    link_poll_stop();
}

// ---- station

// The station's datagrams: hello (every 2 s once answered, else every 0.5 s) and the host's packets.

static void station_leave(void) {  // (caller: not holding L.mu)
    mhgu_ldn_leave();
    link_poll_stop();
    vsock_network_down();
}

#ifdef __ANDROID__
#include <SDL3/SDL.h>
#include <ifaddrs.h>
#include <jni.h>
#include <net/if.h>
// Android's Wi-Fi drops broadcasts to save battery unless an app asks otherwise: on while the local play service is up
// (GameActivity.lanSearch)
static void lan_wifi(bool on) {
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jobject act = env ? (jobject)SDL_GetAndroidActivity() : NULL;
    if (!act) return;
    jclass cls = (*env)->GetObjectClass(env, act);
    jmethodID m = cls ? (*env)->GetStaticMethodID(env, cls, "lanSearch", "(Z)V") : NULL;
    if (m) (*env)->CallStaticVoidMethod(env, cls, m, (jboolean)on);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    if (cls) (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, act);
}
// The probe to each network's own broadcast address as well: 255.255.255.255 follows the default route, and on a phone
// that can be mobile data, or nothing when it is the Wi-Fi hotspot
static void probe_networks(hsock u, const uint8_t *probe, size_t size, struct sockaddr_in to) {
    struct ifaddrs *all = NULL;
    if (getifaddrs(&all)) return;
    int sent = 0;
    for (struct ifaddrs *i = all; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !(i->ifa_flags & IFF_BROADCAST) || !(i->ifa_flags & IFF_UP) ||
            !i->ifa_broadaddr)
            continue;
        to.sin_addr = ((struct sockaddr_in *)(void *)i->ifa_broadaddr)->sin_addr;
        sendto(u, (const char *)probe, size, 0, (struct sockaddr *)&to, sizeof to);
        ONCE("[ldn] searching on %s (%s)\n", i->ifa_name, inet_ntoa(to.sin_addr));
        sent++;
    }
    freeifaddrs(all);
    ONCE("[ldn] %d local network(s) to search\n", sent);
}
#else
static void lan_wifi(bool on) { (void)on; }
#endif

// Hosts to ask: ini Join (comma separated host[:port]) and this computer.
static int scan_addresses(struct sockaddr_in *out, int max) {
    int n = 0;
    char list[sizeof g_config.ldn_join];
    config_live_ldn_join(list, sizeof list);  // (as saved now: a Save counts from the next search)
    int port = config_live_ldn_port();
    for (char *t = strtok(list, ", ;"); t && n < max; t = strtok(NULL, ", ;")) {
        if (resolve_host(t, port, &out[n])) n++;
        else LOG("[ldn] Join address \"%s\" not found\n", t);
    }
    return n;
}

#define SEARCH_NS 1500000000ull      // how long a LAN search listens
#define SEARCH_ASK_NS 500000000ull   // and how often it asks
// Collects the networks of hosts that answer: by address (TCP) and by LAN broadcast (UDP).
static int discover(uint8_t (*infos)[NETINFO_SIZE], struct sockaddr_in *addrs, int max) {
    hnet_init();
    int n = 0;
    static __thread uint8_t buf[MAX_FRAME];
    struct sockaddr_in targets[8];
    int nt = scan_addresses(targets, 8);
    Hello h = make_hello(true, NULL);
    for (int i = 0; i < nt && n < max; i++) {
        hsock s = connect_to(&targets[i], 1500);
        if (s == BADSOCK) continue;
        hset_timeout(s, 1500);
        uint8_t type;
        int k;
        if (frame_send(s, F_HELLO, &h, sizeof h, NULL, 0) && (k = frame_recv(s, &type, buf)) >= NETINFO_SIZE && type == F_INFO) {
            memcpy(infos[n], buf, NETINFO_SIZE);
            addrs[n++] = targets[i];
        }
        hclose(s);
    }
    hsock u = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (u == BADSOCK) return n;
    int one = 1;
    setsockopt(u, SOL_SOCKET, SO_BROADCAST, (const char *)&one, sizeof one);
    uint8_t probe[8];
    memcpy(probe, PROTO_MAGIC, 4);
    wr16(probe + 4, PROTO_VERSION);
    wr16(probe + 6, 0);
    struct sockaddr_in to = {0};
    to.sin_family = AF_INET;
    to.sin_port = htons((uint16_t)config_live_ldn_port());
    // Asked more than once, and given time: on Wi-Fi a broadcast waits at the router for the next beacon that wakes a
    // device saving power (a phone's reply took longer than the 400 ms this used to wait)
    uint64_t next_ask = 0, end = plat_now_ns() + SEARCH_NS;
    for (uint64_t now; (now = plat_now_ns()) < end && n < max;) {
        if (now >= next_ask) {
            next_ask = now + SEARCH_ASK_NS;
            to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
            sendto(u, (const char *)probe, sizeof probe, 0, (struct sockaddr *)&to, sizeof to);
#ifdef __ANDROID__
            probe_networks(u, probe, sizeof probe, to);
#endif
            to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            sendto(u, (const char *)probe, sizeof probe, 0, (struct sockaddr *)&to, sizeof to);
        }
        fd_set r;
        FD_ZERO(&r);
        FD_SET(u, &r);
        uint64_t left = ((next_ask < end ? next_ask : end) - now) / 1000;
        struct timeval tv = {(long)(left / 1000000), (long)(left % 1000000)};
        if (select((int)u + 1, &r, NULL, NULL, &tv) <= 0) continue;
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        int k = recvfrom(u, (char *)buf, MAX_FRAME, 0, (struct sockaddr *)&from, &fl);
        if (k < 8 + NETINFO_SIZE || memcmp(buf, PROTO_MAGIC, 4) || rd16(buf + 4) != PROTO_VERSION) continue;
        bool dup = false;
        for (int i = 0; i < n; i++) dup |= !memcmp(infos[i] + NI_BSSID, buf + 8 + NI_BSSID, 6);
        if (dup) continue;
        memcpy(infos[n], buf + 8, NETINFO_SIZE);
        from.sin_port = htons(rd16(buf + 6));
        addrs[n++] = from;
    }
    hclose(u);
    return n;
}

// ---- nn::ldn

static uint64_t trace_until;  // set at a join: log the game's ldn calls for a few seconds
static void trace(const char *fmt, ...) {
    if (!trace_until || plat_now_ns() > trace_until) return;
    va_list ap;
    va_start(ap, fmt);
    char b[256];
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    LOG("[ldn] trace: %s\n", b);
}

// Whether the last state-changing call returned: the window's close logs it (ldn_log_state), to tell a call
// that never returned (a hang of ours) from a game that stopped asking.
static const char *volatile ldn_call = "none";
static volatile uint64_t ldn_call_ns;
static volatile bool ldn_call_done = true;
static void ldn_call_end(int *unused) { (void)unused; ldn_call_done = true; }
#define LDN_CALL()                                                                                                 \
    __attribute__((cleanup(ldn_call_end))) int ldn_call_scope_ =                                                   \
        (ldn_call = __func__, ldn_call_ns = plat_now_ns(), ldn_call_done = false, 0);                             \
    (void)ldn_call_scope_

HLE_FUNC("_ZN2nn3ldn10InitializeEv", ldn_initialize) { NTRACE();
    lan_wifi(true);
    pthread_mutex_lock(&L.mu);
    if (!L.ev) L.ev = kernel_create_event_pair(&(uint32_t){0}, &L.ev_r);
    static bool hooked;
    if (!hooked) {
        hooked = true;
        mhgu_ryu_set_hooks(ryu_lobby_changed, ryu_data_received);
    }
    crossplay_snapshot();
    mhgu_ryu_set_config(xp_config.ryu_servers, "", xp_config.ryu_password);
    if (L.state == ST_NONE) {
        ryusrv_wanted();
        roomsrv_wanted();
        room_start();
        L.state = ST_INITIALIZED, L.reason = DR_NONE;
        rand_bytes(L.mac, 6);
        L.mac[0] = (uint8_t)((L.mac[0] & 0xFC) | 0x02);  // locally administered, unicast
        mhgu_ldn_set_mac(L.mac);  // the join Hello carries it: the host's node table must name us as the game does
    }
    pthread_mutex_unlock(&L.mu);
    char join[sizeof g_config.ldn_join];
    config_live_ldn_join(join, sizeof join);  // (as saved now, like the hosting and the search)
    ONCE("[ldn] local play over IP: host port %d, join %s (state event %x)\n", config_live_ldn_port(),
         join[0] ? join : "(LAN search only)", L.ev_r);
    RET(0);
}

static void to_initialized(void) {
    int st = L.state;
    if (st > ST_INITIALIZED) __atomic_store_n(&g_net_left_ns, plat_now_ns(), __ATOMIC_RELEASE);  // (ldn_offline)
    if (L.ryu || L.room) {
        if (L.ryu) ryu_stop();
        else room_stop();
        L.own_ip = L.subnet = 0;
        return;
    }
    if (st == ST_AP_CREATED || st == ST_ACCESS_POINT) host_stop(DR_DESTROYED_BY_USER);
    if (st == ST_STATION_CONNECTED || st == ST_STATION || st == ST_ERROR) station_leave();
    if (st == ST_AP_CREATED) vsock_network_down();
    L.own_ip = L.subnet = 0;
}

HLE_FUNC("_ZN2nn3ldn8FinalizeEv", ldn_finalize) { NTRACE();
    lan_wifi(false);
    to_initialized();
    room_shutdown();
    roomsrv_unwanted();
    ryusrv_unwanted();
    L.state = ST_NONE;
    (void)c;
}
HLE_FUNC("_ZN2nn3ldn8GetStateEv", ldn_get_state) { NTRACE(); trace("GetState -> %d", L.state); RET(L.state); }
HLE_FUNC("_ZN2nn3ldn19GetDisconnectReasonEv", ldn_get_reason) { NTRACE(); RET(L.reason); }

// AttachStateChangeEvent(SystemEventType *): the event fires on every change of the state or the network
HLE_FUNC("_ZN2nn3ldn22AttachStateChangeEventEPNS_2os15SystemEventTypeE", ldn_attach_event) { NTRACE();
    if (!L.ev) L.ev = kernel_create_event_pair(&(uint32_t){0}, &L.ev_r);
    c->r[1] = L.ev_r;
    c->r[2] = 0;  // not managed: the handle stays ours
    c->r[3] = 1;  // auto clear
    guest_call(c, guest_sym("_ZN2nn2os33AttachReadableHandleToSystemEventEPNS0_15SystemEventTypeEjbNS0_14EventClearModeE"));
}

HLE_FUNC("_ZN2nn3ldn14GetNetworkInfoEPNS0_11NetworkInfoE", ldn_get_network_info) { NTRACE();
    if (!ldn_net_up()) { RET(R_INVALID_STATE); return; }
    // The players in the network come from the Rust link (it keeps the node table); the game reads it
    // here, and Pia routes by those addresses.
    if (ldn_net_up() && !L.ryu && !L.room) mhgu_ldn_info(L.info, NETINFO_SIZE);
    pthread_mutex_lock(&L.mu);
    memcpy(ARGP(0), L.info, NETINFO_SIZE);
    trace("GetNetworkInfo -> %u node(s), host %s", L.info[NI_NODE_COUNT], ipstr(rd32(node_at(L.info, 0) + NODE_IP)));
    pthread_mutex_unlock(&L.mu);
    RET(0);
}

HLE_FUNC("_ZN2nn3ldn14GetIpv4AddressEPNS0_11Ipv4AddressEPNS0_10SubnetMaskE", ldn_get_ipv4) { NTRACE();
    if (!ldn_net_up()) { RET(R_INVALID_STATE); return; }
    if (ARG(0)) ST32(ARG(0), L.own_ip);
    if (ARG(1)) ST32(ARG(1), L.ryu && L.ryu_mask ? L.ryu_mask : 0xFFFFFF00u);
    trace("GetIpv4Address -> %s", ipstr(L.own_ip));
    RET(0);
}

HLE_FUNC("_ZN2nn3ldn20GetSecurityParameterEPNS0_17SecurityParameterE", ldn_get_sec_param) { NTRACE();
    if (!ldn_net_up()) { RET(R_INVALID_STATE); return; }
    memcpy(ARGP(0), L.info + NI_SEC_PARAM, 0x10);
    memcpy((uint8_t *)ARGP(0) + 0x10, L.info + NI_SESSION_ID, 0x10);
    trace("GetSecurityParameter -> param %02x%02x.. session %02x%02x..", L.info[NI_SEC_PARAM], L.info[NI_SEC_PARAM+1],
          L.info[NI_SESSION_ID], L.info[NI_SESSION_ID+1]);
    RET(0);
}

HLE_FUNC("_ZN2nn3ldn16GetNetworkConfigEPNS0_13NetworkConfigE", ldn_get_net_config) { NTRACE();
    if (!ldn_net_up()) { RET(R_INVALID_STATE); return; }
    uint8_t *o = ARGP(0);
    memset(o, 0, 0x20);
    memcpy(o, L.info, 0x10);  // IntentId
    wr16(o + 0x10, rd16(L.info + NI_CHANNEL));
    o[0x12] = L.info[NI_NODE_MAX];
    wr16(o + 0x14, L.lcv);
    RET(0);
}

// ---- access point (host)
HLE_FUNC("_ZN2nn3ldn15OpenAccessPointEv", ldn_open_ap) { NTRACE(); LDN_CALL();
    if (L.state != ST_INITIALIZED) { RET(R_INVALID_STATE); return; }
    L.state = ST_ACCESS_POINT;
    signal_change();
    RET(0);
}
HLE_FUNC("_ZN2nn3ldn16CloseAccessPointEv", ldn_close_ap) { NTRACE(); LDN_CALL();
    if (L.state != ST_ACCESS_POINT && L.state != ST_AP_CREATED) { RET(R_INVALID_STATE); return; }
    to_initialized();
    L.state = ST_INITIALIZED;
    signal_change();
    RET(0);
}

static uint32_t create_ryu(const uint8_t *cfg, const uint8_t *sec, const uint8_t *user) {
    uint8_t info[NETINFO_SIZE];
    uint32_t ip = 0, mask = 0;
    L.ryu = true;
    uint32_t r = mhgu_ryu_create(mhgu_ryusrv_running() ? -1 : 0, cfg, sec, user, L.adv, L.adv_size, info, &ip, &mask);
    if (r) {
        L.ryu = false;
        LOG("[ldn] creating the hub on the RyuLdn server failed (result %#x)\n", r);
        return r;
    }
    pthread_mutex_lock(&L.mu);
    if (info_sane(info)) memcpy(L.info, info, NETINFO_SIZE);
    L.own_ip = ip, L.ryu_mask = mask, L.subnet = ip & mask, L.lcv = rd16(cfg + 0x14);
    L.state = ST_AP_CREATED, L.reason = DR_NONE;
    pthread_mutex_unlock(&L.mu);
    LOG("[ldn] hub created on the RyuLdn server: %s, up to %d players\n", ipstr(L.own_ip), L.info[NI_NODE_MAX]);
    if (room_configured()) mixed_bridge_start();
    signal_change();
    return 0;
}

static uint32_t create_network(const uint8_t *cfg, const uint8_t *sec, const uint8_t *sec_param, const uint8_t *user) {
    if (L.state != ST_ACCESS_POINT) return R_INVALID_STATE;
    crossplay_prepare();
    if (ryu_configured()) return create_ryu(cfg, sec, user);
    bool via_room = room_configured() && mhgu_room_connected();
    if (room_configured() && !via_room) return R_CONNECT_FAILURE;
    if (via_room) room_mac(L.mac);
    pthread_mutex_lock(&L.mu);
    memset(L.info, 0, NETINFO_SIZE);
    memcpy(L.info, cfg, 0x10);  // IntentId
    if (sec_param) {
        memcpy(L.info + NI_SEC_PARAM, sec_param, 0x10);
        memcpy(L.info + NI_SESSION_ID, sec_param + 0x10, 0x10);
    } else {
        rand_bytes(L.info + NI_SEC_PARAM, 0x10);
        rand_bytes(L.info + NI_SESSION_ID, 0x10);
    }
    memcpy(L.info + NI_BSSID, L.mac, 6);
    char ssid[33];
    snprintf(ssid, sizeof ssid, "MHGURecomp%08x%08x", arc4random(), arc4random());
    L.info[NI_SSID] = (uint8_t)strlen(ssid);
    memcpy(L.info + NI_SSID + 1, ssid, strlen(ssid));
    uint16_t ch = rd16(cfg + 0x10);
    wr16(L.info + NI_CHANNEL, ch ? ch : 6);
    L.info[NI_LINK_LEVEL] = 3;
    L.info[NI_NETWORK_TYPE] = 2;  // ldn
    wr16(L.info + NI_SEC_MODE, rd16(sec));
    L.info[NI_ACCEPT_POLICY] = (uint8_t)L.accept_policy;
    uint8_t max = cfg[0x12];
    L.info[NI_NODE_MAX] = max >= 1 && max <= MAX_NODES ? max : MAX_NODES;
    L.info[NI_NODE_COUNT] = 1;
    L.lcv = rd16(cfg + 0x14);
    if (via_room) {
        uint8_t ip[4];
        mhgu_room_own_ip(ip);
        L.own_ip = ip_u32(ip), L.subnet = L.own_ip & 0xFFFFFF00u;
        memset(L.room_client, 0, sizeof L.room_client);
        memcpy(L.info + NI_SSID, "\x12YuzuFakeSsidForLdn", 19);
    } else {
        L.subnet = 0xA9FE0000u | (uint32_t)(1 + arc4random() % 254) << 8;  // 169.254.x.0/24
        L.own_ip = L.subnet | 1;
    }
    uint8_t *nd = node_at(L.info, 0);
    wr32(nd + NODE_IP, L.own_ip);
    memcpy(nd + NODE_MAC, L.mac, 6);
    nd[NODE_ID] = 0;
    nd[NODE_CONNECTED] = 1;
    memcpy(nd + NODE_NAME, user, 0x20);
    wr16(nd + NODE_LCV, L.lcv);
    wr16(L.info + NI_ADV_SIZE, (uint16_t)L.adv_size);
    memcpy(L.info + NI_ADV, L.adv, L.adv_size);
    uint64_t auth = ((uint64_t)arc4random() << 32) | arc4random();
    memcpy(L.info + NI_AUTH_ID, &auth, 8);
    pthread_mutex_unlock(&L.mu);
    if (via_room) L.room = true;
    else if (!host_start()) {
        L.own_ip = L.subnet = 0;
        return R_CONNECT_FAILURE;
    }
    L.state = ST_AP_CREATED;
    LOG("[ldn] network created%s: %s, up to %d players, version %u\n", via_room ? " in the room" : "", ipstr(L.own_ip), L.info[NI_NODE_MAX], L.lcv);
    signal_change();
    return 0;
}

// CreateNetwork(const NetworkConfig &, const SecurityConfig &, const UserConfig &)
HLE_FUNC("_ZN2nn3ldn13CreateNetworkERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_10UserConfigE", ldn_create) { NTRACE(); LDN_CALL();
    RET(create_network(ARGP(0), ARGP(1), NULL, ARGP(2)));
}
// CreateNetworkPrivate(config, security, SecurityParameter, user, address entry count, const AddressEntry *)
HLE_FUNC("_ZN2nn3ldn20CreateNetworkPrivateERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_17SecurityParameterERKNS0_10UserConfigEiPKNS0_12AddressEntryE",
         ldn_create_private) {
    LDN_CALL();
    RET(create_network(ARGP(0), ARGP(1), ARGP(2), ARGP(3)));
}

HLE_FUNC("_ZN2nn3ldn14DestroyNetworkEv", ldn_destroy) { NTRACE(); LDN_CALL();
    if (L.state != ST_AP_CREATED) { RET(R_INVALID_STATE); return; }
    if (L.ryu) ryu_stop();
    else if (L.room) room_stop();
    else host_stop(DR_DESTROYED_BY_USER);
    vsock_network_down();
    L.state = ST_ACCESS_POINT;
    L.own_ip = L.subnet = 0;
    LOG("[ldn] network destroyed\n");
    signal_change();
    RET(0);
}

HLE_FUNC("_ZN2nn3ldn16SetAdvertiseDataEPKvj", ldn_set_advertise) { NTRACE();
    uint32_t n = ARG(1) < ADV_MAX ? ARG(1) : ADV_MAX;
    pthread_mutex_lock(&L.mu);
    memcpy(L.adv, ARGP(0), n);
    L.adv_size = n;
    if (L.state == ST_AP_CREATED) {
        wr16(L.info + NI_ADV_SIZE, (uint16_t)n);
        memset(L.info + NI_ADV, 0, ADV_MAX);
        memcpy(L.info + NI_ADV, L.adv, n);
        if (L.ryu) mhgu_ryu_set_advertise(L.adv, n);
        else if (L.room) room_info_dirty = true;
        else push_info_locked(0);
    }
    pthread_mutex_unlock(&L.mu);
    if (room_info_dirty) { room_info_dirty = false; room_sync_all(); }
    RET(0);
}

HLE_FUNC("_ZN2nn3ldn22SetStationAcceptPolicyENS0_12AcceptPolicyE", ldn_set_accept) { NTRACE();
    pthread_mutex_lock(&L.mu);
    L.accept_policy = (int)ARG(0);
    if (L.state == ST_AP_CREATED) {
        L.info[NI_ACCEPT_POLICY] = (uint8_t)ARG(0);
        if (L.ryu) mhgu_ryu_set_accept_policy((uint8_t)ARG(0));
        else if (L.room) room_info_dirty = true;
        else push_info_locked(0);
    }
    pthread_mutex_unlock(&L.mu);
    if (room_info_dirty) { room_info_dirty = false; room_sync_all(); }
    RET(0);
}

// Reject(Ipv4Address): the host sends a player away
HLE_FUNC("_ZN2nn3ldn6RejectENS0_11Ipv4AddressE", ldn_reject) { NTRACE();
    if (L.ryu) {
        for (int i = 1; i < MAX_NODES; i++)
            if (node_at(L.info, i)[NODE_CONNECTED] && rd32(node_at(L.info, i) + NODE_IP) == ARG(0))
                mhgu_ryu_reject((uint32_t)i, DR_REJECTED);
        RET(0);
        return;
    }
    if (L.room) {
        uint8_t own[4], to[MAX_NODES][4];
        int n = 0;
        mhgu_room_own_ip(own);
        pthread_mutex_lock(&L.mu);
        for (int i = 1; i < MAX_NODES; i++)
            if (L.room_client[i] && rd32(node_at(L.info, i) + NODE_IP) == ARG(0)) {
                memcpy(to[n++], L.room_cip[i], 4);
                memset(node_at(L.info, i), 0, NODE_SIZE);
                node_at(L.info, i)[NODE_ID] = (uint8_t)i;
                L.room_client[i] = false;
                if (L.info[NI_NODE_COUNT] > 1) L.info[NI_NODE_COUNT]--;
            }
        pthread_mutex_unlock(&L.mu);
        for (int i = 0; i < n; i++) mhgu_room_send_ldn(RLDN_DESTROY_NETWORK, own, to[i], false, NULL, 0);
        if (n) { room_sync_all(); signal_change(); }
        RET(0);
        return;
    }
    mhgu_ldn_reject(ARG(0));  // the Rust host sends F_DESTROY (DR_REJECTED) and closes that peer
    RET(0);
}

// ---- station (joining)
HLE_FUNC("_ZN2nn3ldn11OpenStationEv", ldn_open_station) { NTRACE(); LDN_CALL();
    if (L.state != ST_INITIALIZED) { RET(R_INVALID_STATE); return; }
    L.state = ST_STATION;
    signal_change();
    RET(0);
}
HLE_FUNC("_ZN2nn3ldn12CloseStationEv", ldn_close_station) { NTRACE(); LDN_CALL();
    if (L.state != ST_STATION && L.state != ST_STATION_CONNECTED && L.state != ST_ERROR) { RET(R_INVALID_STATE); return; }
    to_initialized();
    L.state = ST_INITIALIZED;
    signal_change();
    RET(0);
}

static bool filter_ok(const uint8_t *info, const uint8_t *f) {
    uint32_t flags = rd32(f + 0x5C);
    if ((flags & SF_LOCAL_COMM_ID) && memcmp(info + NI_LOCAL_COMM_ID, f, 8)) return false;
    if ((flags & SF_SCENE_ID) && rd16(info + NI_SCENE_ID) != rd16(f + 0x0A)) return false;
    if ((flags & SF_SESSION_ID) && memcmp(info + NI_SESSION_ID, f + 0x10, 0x10)) return false;
    if ((flags & SF_BSSID) && memcmp(info + NI_BSSID, f + 0x24, 6)) return false;
    return true;
}

// Scan(NetworkInfo *out, int *count, int max, const ScanFilter &, int channel)
HLE_FUNC("_ZN2nn3ldn4ScanEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi", ldn_scan) { NTRACE(); LDN_CALL();
    uint32_t out = ARG(0), pcount = ARG(1);
    int max = (int)ARG(2);
    const uint8_t *filter = ARGP(3);
    if (L.state != ST_STATION && L.state != ST_ACCESS_POINT) { RET(R_INVALID_STATE); return; }
    crossplay_prepare();
    static uint8_t infos[MAX_FOUND][NETINFO_SIZE];
    struct sockaddr_in addrs[MAX_FOUND];
    uint64_t hubs[MAX_FOUND] = {0};
    uint16_t vers[MAX_FOUND];
    static char codes[MAX_FOUND][CODE_LEN + 1];
    for (int i = 0; i < MAX_FOUND; i++) vers[i] = PROTO_MIN, codes[i][0] = 0;  // (LAN / address hosts: 3 takes them all)
    hnet_init();  // (before the thread's first socket call)
    ScanJob *job = scan_job_start();
    int n = 0, k = 0;
    // a hub found by its code (Join by code) comes first, so the player sees it at the top of Join Hub
    pthread_mutex_lock(&invite_mu);
    if (invite.state == LDN_INVITE_READY && plat_now_ns() - invite.at <= INVITE_KEEP_NS) {
        memcpy(infos[0], invite.info, NETINFO_SIZE);
        addrs[0] = invite.addr, hubs[0] = invite.hub, vers[0] = invite.ver;
        snprintf(codes[0], sizeof codes[0], "%s", invite.code);
        n = 1;
    }
    pthread_mutex_unlock(&invite_mu);
    int invited = n;
    int lan = discover(infos + n, addrs + n, MAX_FOUND - n);
    for (int i = invited; i < invited + lan; i++) {  // (the invited hub found on the LAN too: once, the invite's way)
        if (invited && !memcmp(infos[i] + NI_BSSID, infos[0] + NI_BSSID, 6)) continue;
        if (i != n) memcpy(infos[n], infos[i], NETINFO_SIZE), addrs[n] = addrs[i], vers[n] = vers[i];
        n++;
    }
    int ryu_of[MAX_FOUND] = {0};
    bool room_of[MAX_FOUND] = {0};
    const LobbyList *online = NULL;
    if (job && scan_job_wait(job)) online = &job->list;
    if (online) {  // the server's hubs after the LAN's (a hub found both ways is joined directly)
        uint64_t own = mhgu_ldn_own_hub();
        for (int j = 0; j < online->n && n < MAX_FOUND; j++) {
            if (own && online->ids[j] == own) continue;  // our own hub: joining ourselves can't work
            bool dup = false;
            for (int i = 0; i < n; i++) dup |= !memcmp(infos[i] + NI_BSSID, online->infos[j] + NI_BSSID, 6);
            if (dup) continue;
            memcpy(infos[n], online->infos[j], NETINFO_SIZE);
            addrs[n] = online->addr;
            vers[n] = online->vers[j];
            snprintf(codes[n], sizeof codes[n], "%s", online->codes[j]);
            hubs[n++] = online->ids[j];
        }
    }
    if (job) scan_job_unref(job);
    if (ryu_configured() && mhgu_ryu_server_count() > 0 && n < MAX_FOUND) {
        static uint8_t rinfo[MAX_FOUND][NETINFO_SIZE];
        int rsrv[MAX_FOUND];
        int rn = mhgu_ryu_scan(filter, (uint8_t *)rinfo, rsrv, MAX_FOUND - n);
        for (int j = 0; j < rn; j++) {
            memcpy(infos[n], rinfo[j], NETINFO_SIZE);
            memset(&addrs[n], 0, sizeof addrs[n]);
            ryu_of[n++] = rsrv[j] + 1;
        }
    }
    if (xp_config.crossplay == XP_ROOM && room_enabled() && mhgu_room_connected() && n < MAX_FOUND) {
        uint8_t own[4], all[4] = {0xFF, 0xFF, 0xFF, 0xFF};
        mhgu_room_own_ip(own);
        pthread_mutex_lock(&room_scan_mu);
        room_nscan = 0, room_scanning = true;
        pthread_mutex_unlock(&room_scan_mu);
        mhgu_room_send_ldn(RLDN_SCAN, own, all, true, NULL, 0);
        plat_sleep_until_ns(plat_now_ns() + 700000000ull);
        pthread_mutex_lock(&room_scan_mu);
        room_scanning = false;
        for (int j = 0; j < room_nscan && n < MAX_FOUND; j++) {
            memcpy(infos[n], room_scan[j].info, NETINFO_SIZE);
            memset(&addrs[n], 0, sizeof addrs[n]);
            room_of[n++] = true;
        }
        pthread_mutex_unlock(&room_scan_mu);
    }
    nfound = 0;
    for (int i = 0; i < n; i++) {
        if (!info_sane(infos[i]) || !filter_ok(infos[i], filter)) continue;
        found[nfound].ryu = ryu_of[i];
        found[nfound].room = room_of[i];
        memcpy(found[nfound].info, infos[i], NETINFO_SIZE);
        memcpy(found[nfound].bssid, infos[i] + NI_BSSID, 6);
        memcpy(found[nfound].session, infos[i] + NI_SESSION_ID, 16);
        found[nfound].hub = hubs[i];
        found[nfound].ver = vers[i];
        snprintf(found[nfound].code, sizeof found[nfound].code, "%s", codes[i]);
        found[nfound++].addr = addrs[i];
        if (k < max) memcpy((uint8_t *)G2H(out) + (size_t)k++ * NETINFO_SIZE, infos[i], NETINFO_SIZE);
    }
    if (pcount) ST32(pcount, k);
    static int last = -1, last_online = -1, last_max = -1;
    int listed = online && online->state == HUB_ONLINE ? online->n : 0;
    if (nfound != last || listed != last_online || max != last_max) {
        LOG("[ldn] scan: %d network(s) found (%d listed on the hub server), %d given to the game (room for %d)\n",
            nfound, listed, k, max);
        last = nfound, last_online = listed, last_max = max;
    }
    RET(0);
}

static uint32_t connect_network(const uint8_t *info, const uint8_t *user, uint16_t lcv) {
    if (L.state != ST_STATION) return R_INVALID_STATE;
    int i = 0;
    while (i < nfound && memcmp(found[i].bssid, info + NI_BSSID, 6)) i++;
    if (i == nfound) return R_CONNECT_NOT_FOUND;
    if (found[i].ryu) {
        uint8_t sec[0x44] = {0}, out[NETINFO_SIZE];
        uint32_t ip = 0, mask = 0;
        memcpy(sec, found[i].info + NI_SEC_MODE, 2);
        L.ryu = true;
        uint32_t r = mhgu_ryu_connect(found[i].ryu - 1, found[i].info, sec, user, lcv, out, &ip, &mask);
        if (r) {
            L.ryu = false;
            LOG("[ldn] joining the RyuLdn lobby failed (result %#x)\n", r);
            return r;
        }
        pthread_mutex_lock(&L.mu);
        if (info_sane(out)) memcpy(L.info, out, NETINFO_SIZE);
        L.lcv = lcv, L.own_ip = ip, L.ryu_mask = mask, L.subnet = ip & mask;
        L.state = ST_STATION_CONNECTED, L.reason = DR_NONE;
        pthread_mutex_unlock(&L.mu);
        LOG("[ldn] joined a RyuLdn lobby as %s\n", ipstr(L.own_ip));
        signal_change();
        return 0;
    }
    if (found[i].room) {
        if (!mhgu_room_connected()) return R_CONNECT_FAILURE;
        uint8_t ipb[4], hostb[4];
        mhgu_room_own_ip(ipb);
        pthread_mutex_lock(&L.mu);
        memcpy(L.info, found[i].info, NETINFO_SIZE);
        L.room = true, L.own_ip = ip_u32(ipb), L.subnet = L.own_ip & 0xFFFFFF00u, L.lcv = lcv;
        pthread_mutex_unlock(&L.mu);
        uint8_t node[NODE_SIZE] = {0};
        wr32(node + NODE_IP, L.own_ip);
        room_mac(node + NODE_MAC);
        node[NODE_CONNECTED] = 1;
        memcpy(node + NODE_NAME, user, 0x20);
        wr16(node + NODE_LCV, lcv);
        ip_bytes(rd32(node_at(found[i].info, 0) + NODE_IP), hostb);
        mhgu_room_send_ldn(RLDN_CONNECT, ipb, hostb, false, node, NODE_SIZE);
        for (int w = 0; w < 300 && L.state != ST_STATION_CONNECTED; w++) plat_sleep_until_ns(plat_now_ns() + 10000000ull);
        if (L.state != ST_STATION_CONNECTED) {
            L.room = false, L.own_ip = L.subnet = 0;
            LOG("[ldn] the room's host did not answer the join\n");
            return R_CONNECT_TIMEOUT;
        }
        LOG("[ldn] joined a hub in the room as %s\n", ipstr(L.own_ip));
        signal_change();
        return 0;
    }
    char addr[64];
    snprintf(addr, sizeof addr, "%s:%u", inet_ntoa(found[i].addr.sin_addr), (unsigned)ntohs(found[i].addr.sin_port));
    char name[0x21] = {0};
    memcpy(name, user, 0x20);
    // The Rust client does the MHGL handshake and keeps the link; the game reads the network it answered.
    mhgu_ldn_set_lcv(lcv);
    mhgu_ldn_set_direct(config_live_ldn_direct());
    char key[LOBBY_NAME];
    config_live_ldn(NULL, 0, key, sizeof key);
    if (found[i].hub) {
        // A hub on a server: the server splices us to the host, and keeps relaying. Its session code, when known,
        // is an invitation (any room); the link version is the host's (HUBS4).
        char code[CODE_LEN + 1] = {0};
        ldn_code_normalize(found[i].code, code);
        if (!mhgu_ldn_join_server(addr, key, found[i].hub, code[0] ? (const uint8_t *)code : NULL,
                                  found[i].ver, name, NULL, 0)) {
            LOG("[ldn] mhgu-link could not join hub %#llx on %s\n", (unsigned long long)found[i].hub, addr);
            return R_CONNECT_TIMEOUT;
        }
    } else if (!mhgu_ldn_join(addr, name, NULL, 0)) {
        LOG("[ldn] mhgu-link could not join %s\n", addr);
        return R_CONNECT_TIMEOUT;
    }
    uint8_t theirs[NETINFO_SIZE] = {0};
    mhgu_ldn_info(theirs, NETINFO_SIZE);
    pthread_mutex_lock(&L.mu);
    memcpy(L.info, theirs, NETINFO_SIZE);
    L.own_ip = mhgu_ldn_own_ip();
    L.subnet = L.own_ip & 0xFFFFFF00u;
    L.lcv = lcv;
    L.state = ST_STATION_CONNECTED, L.reason = DR_NONE;
    pthread_mutex_unlock(&L.mu);
    trace_until = plat_now_ns() + 5000000000ull;
    link_poll_start();
    LOG("[ldn] joined %s as %s via mhgu-link\n", addr, ipstr(L.own_ip));
    {   // what the game gets: its own node entry must be the one the host wrote for us
        int cnt = L.info[NI_NODE_COUNT], max = L.info[NI_NODE_MAX];
        const uint8_t *n0 = node_at(L.info, 0);
        LOG("[ldn] info: ssid %u ch %u level %u type %u secmode %04x policy %u adv %u, localcomm %02x%02x scene %02x%02x\n",
            L.info[NI_SSID], rd16(L.info + NI_CHANNEL), L.info[NI_LINK_LEVEL], L.info[NI_NETWORK_TYPE],
            rd16(L.info + NI_SEC_MODE), L.info[NI_ACCEPT_POLICY], rd16(L.info + NI_ADV_SIZE),
            L.info[NI_LOCAL_COMM_ID], L.info[NI_LOCAL_COMM_ID + 1], L.info[NI_SCENE_ID], L.info[NI_SCENE_ID + 1]);
        LOG("[ldn] connected network: %d/%d node(s), host %s mac %02x:%02x:%02x:%02x:%02x:%02x, session %02x%02x%02x%02x\n",
            cnt, max, ipstr(rd32(n0 + NODE_IP)), n0[NODE_MAC], n0[NODE_MAC+1], n0[NODE_MAC+2],
            n0[NODE_MAC+3], n0[NODE_MAC+4], n0[NODE_MAC+5], L.info[NI_SESSION_ID], L.info[NI_SESSION_ID+1],
            L.info[NI_SESSION_ID+2], L.info[NI_SESSION_ID+3]);
        for (int i = 0; i < cnt && i < MAX_NODES; i++) {
            const uint8_t *nd = node_at(L.info, i);
            LOG("[ldn]   node %d: ip %s mac %02x:%02x:%02x:%02x:%02x:%02x id %u conn %u\n", i,
                ipstr(rd32(nd + NODE_IP)), nd[NODE_MAC], nd[NODE_MAC+1], nd[NODE_MAC+2],
                nd[NODE_MAC+3], nd[NODE_MAC+4], nd[NODE_MAC+5], nd[NODE_ID], nd[NODE_CONNECTED]);
        }
    }
    signal_change();
    return 0;
}

// Connect(const NetworkInfo &, const SecurityConfig &, const UserConfig &, int version, ConnectOption)
HLE_FUNC("_ZN2nn3ldn7ConnectERKNS0_11NetworkInfoERKNS0_14SecurityConfigERKNS0_10UserConfigEiNS0_13ConnectOptionE", ldn_connect) { NTRACE(); LDN_CALL();
    RET(connect_network(ARGP(0), ARGP(2), (uint16_t)ARG(3)));
}
HLE_FUNC("_ZN2nn3ldn7ConnectERKNS0_11NetworkInfoERKNS0_14SecurityConfigERKNS0_10UserConfigEi", ldn_connect2) { NTRACE(); LDN_CALL();
    RET(connect_network(ARGP(0), ARGP(2), (uint16_t)ARG(3)));
}
// ConnectPrivate(config, security, SecurityParameter, user, int version, ConnectOption): a network known by its
// session id (from the scan list)
static uint32_t connect_private(Cpu *c) {
    const uint8_t *session = (const uint8_t *)ARGP(2) + 0x10;
    for (int i = 0; i < nfound; i++)
        if (!memcmp(found[i].session, session, 0x10)) {
            uint8_t info[NETINFO_SIZE] = {0};
            if (found[i].ryu) memcpy(info, found[i].info, NETINFO_SIZE);
            memcpy(info + NI_BSSID, found[i].bssid, 6);
            return connect_network(info, ARGP(3), (uint16_t)STK(0));
        }
    LOG("[ldn] ConnectPrivate: no scanned network has that session\n");
    return R_CONNECT_NOT_FOUND;
}
HLE_FUNC("_ZN2nn3ldn14ConnectPrivateERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_17SecurityParameterERKNS0_10UserConfigEiNS0_13ConnectOptionE",
         ldn_connect_private) {
    LDN_CALL();
    RET(connect_private(c));
}

HLE_FUNC("_ZN2nn3ldn10DisconnectEv", ldn_disconnect) { NTRACE(); LDN_CALL();
    if (L.state != ST_STATION_CONNECTED && L.state != ST_ERROR) { RET(R_INVALID_STATE); return; }
    if (L.ryu) ryu_stop();
    else if (L.room) room_stop();
    else station_leave();
    L.state = ST_STATION, L.reason = DR_BY_USER;
    L.own_ip = L.subnet = 0;
    signal_change();
    RET(0);
}

void ldn_log_state(void) {
    LOG("[ldn] state: %d%s%s, last call %s, %.1f s ago, %s\n", L.state, L.hosting ? ", hosting" : "",
        L.ryu ? ", Ryujinx server" : L.room ? ", room" : "", ldn_call,
        ldn_call_ns ? (double)(plat_now_ns() - ldn_call_ns) / 1e9 : 0.0, ldn_call_done ? "returned" : "NOT RETURNED");
}
