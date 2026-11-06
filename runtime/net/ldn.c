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
#include <unistd.h>
#endif
#include <string.h>

#include "../../updater/updater.h"  // (sha256)
#include "../config.h"
#include "../hle.h"
#include "../kernel.h"
#include "../overlay.h"
#include "../settings_locale.h"
#include "net.h"
#include "mhgu_link.h"  /* the Rust online module (crates/mhgu-link-ffi) */
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
#define PROTO_MAGIC "MHGL"
#define PROTO_VERSION 3
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
enum { S_LIST = 1, S_HOST, S_ATTACH, S_JOIN, S_UPDATE, S_PING, S_HUBS = 16, S_HOSTED, S_INCOMING, S_SPLICED, S_ERROR };
enum { SE_NOT_FOUND = 1, SE_NO_ANSWER, SE_BUSY, SE_VERSION };


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
#define MAX_FOUND 32
static struct { uint8_t bssid[6], session[16]; struct sockaddr_in addr; uint64_t hub; uint8_t info[NETINFO_SIZE]; int ryu; bool room; } found[MAX_FOUND];
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
uint32_t ldn_broadcast_ip(void) { return L.subnet ? L.subnet | 0xFF : 0xFFFFFFFFu; }
bool ldn_net_up(void) { return L.state == ST_AP_CREATED || L.state == ST_STATION_CONNECTED; }
bool ldn_hosting(void) { return L.state == ST_AP_CREATED; }
bool ldn_crossplay(void) { return L.ryu || L.room; }
bool ldn_party(int *n, int *max, char *id, size_t size) {
    pthread_mutex_lock(&L.mu);
    bool up = ldn_net_up();
    if (up) {
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
    bool all = p->dst_ip == ldn_broadcast_ip() || p->dst_ip == 0xFFFFFFFFu;
    RyuProxy r = {p->src_ip, all ? ldn_broadcast_ip() : p->dst_ip, p->src_port, p->dst_port, 17};
    mhgu_ryu_send(&r, data, len);
}
static void room_xmit(const VPacket *p, const void *data, uint32_t len) {
    bool all = p->dst_ip == ldn_broadcast_ip() || p->dst_ip == 0xFFFFFFFFu;
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

static void room_members_changed(const uint8_t (*ips)[4], int n) { if (mhgu_xb_active()) mhgu_xb_room_members(ips, n); }
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

static bool ryu_configured(void) {
    return g_config.crossplay == XP_RYU && (g_config.ryu_servers[0] || g_config.ryu_host);
}
static bool room_configured(void) {
    return g_config.crossplay == XP_ROOM && (g_config.room_server[0] || g_config.room_host);
}
static char roomsrv_self[32];
static const char *room_server(void) { return roomsrv_self[0] ? roomsrv_self : g_config.room_server; }
static bool room_enabled(void) { return room_configured() && (room_server()[0] != 0); }

static volatile int room_keep_gen;
static volatile bool room_wanted;
static void *room_keeper(void *arg) {
    int gen = (int)(intptr_t)arg;
    uint64_t next = 0;
    while (room_keep_gen == gen) {
        uint64_t now = plat_now_ns();
        if (now >= next && room_enabled() && !mhgu_room_connected()) {
            uint8_t ip[4];
            char err[96];
            if (!mhgu_room_join(room_server(), g_config.room_name, g_config.room_password, ip, err, sizeof err)) {
                LOG("[ldn] room join failed: %s\n", err);
                next = now + 15000000000ull;
            }
        }
        plat_sleep_until_ns(plat_now_ns() + 500000000ull);
    }
    if (!room_wanted) mhgu_room_leave();
    return NULL;
}
static void room_start(void) {
    if (!room_enabled()) return;
    static bool hooked;
    if (!hooked) {
        hooked = true;
        mhgu_room_set_hooks(room_ldn_received, room_proxy_received, room_lost, room_members_changed);
    }
    pthread_t t;
    room_wanted = true;
    room_keep_gen++;
    if (!pthread_create(&t, NULL, room_keeper, (void *)(intptr_t)room_keep_gen)) pthread_detach(t);
}
static void room_shutdown(void) {
    room_wanted = false;
    room_keep_gen++;
    room_stop();
}

static void ryusrv_wanted(void) {
    if (!g_config.ryu_host || mhgu_ryusrv_running()) return;
    int port = g_config.ryu_port > 0 ? g_config.ryu_port : 30456;
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
    if (!g_config.room_host || mhgu_roomsrv_running()) return;
    int port = g_config.room_port > 0 ? g_config.room_port : 24872;
    char err[96] = "";
    if (!mhgu_roomsrv_start(port, g_config.room_name, g_config.room_password, "MHGU Host", err, sizeof err)) {
        LOG("[ldn] can't run the room server on port %d: %s\n", port, err);
        return;
    }
    snprintf(roomsrv_self, sizeof roomsrv_self, "127.0.0.1:%d", port);
    LOG("[ldn] suyu-family players can join this PC's room (port %d)\n", port);
}
static void roomsrv_unwanted(void) {
    if (!mhgu_roomsrv_running()) return;
    roomsrv_self[0] = 0;
    mhgu_roomsrv_stop();
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
    int n, state, ms;
    struct sockaddr_in addr;
    char server[LOBBY_NAME];
} LobbyList;

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
    if (n < 4 || type != S_HUBS) return HUB_NO_ANSWER;
    uint32_t count = rd32(buf);
    for (uint32_t i = 0; i < count && r->n < LOBBY_LIST_MAX && 4 + (i + 1) * (8 + NETINFO_SIZE) <= (uint32_t)n; i++) {
        const uint8_t *e = buf + 4 + i * (8 + NETINFO_SIZE);
        r->ids[r->n] = rd64(e);
        memcpy(r->infos[r->n++], e + 8, NETINFO_SIZE);
    }
    return HUB_ONLINE;
}

static void *lobby_list_thread(void *arg) {  // (while the LAN is searched; r->server set)
    LobbyList *r = arg;
    static int last = HUB_ONLINE;
    r->state = lobby_list(r->server, NULL, 1500, r);
    if (r->state != last && r->state != HUB_ONLINE)
        LOG("[ldn] hub server %s: %s: only LAN / Join hubs are searched\n", r->server,
            r->state == HUB_NOT_FOUND ? "not found" : r->state == HUB_UNREACHABLE ? "unreachable" : "no list");
    last = r->state;
    hub_report(NOTE_SEARCH, r->server, r->state, r->ms, r->state == HUB_ONLINE ? r->n : -1);
    return NULL;
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
    pthread_t th;
    if (!c) return;
    hnet_init();
    bool on = lobby_server_of(server, c->server);
    snprintf(c->key, sizeof c->key, "%s", key);
    pthread_mutex_lock(&hub_mu);
    c->check = ++hub_now.check;
    hub_set_locked(c->server, c->key, on ? HUB_CHECKING : HUB_OFF, -1, -1);
    pthread_mutex_unlock(&hub_mu);
    if (on && !pthread_create(&th, NULL, hub_check_thread, c)) {
        pthread_detach(th);
        return;
    }
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

// Joining a listed hub: the server asks its host to connect and splices the two. 0, else an nn::ldn result.


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

static void link_aux(const uint8_t *data, uint32_t len) { emsync_aux_received(data, len); }

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
            signal_change();
        }
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

HLE_FUNC("_ZN2nn3ldn10InitializeEv", ldn_initialize) { NTRACE();
    lan_wifi(true);
    pthread_mutex_lock(&L.mu);
    if (!L.ev) L.ev = kernel_create_event_pair(&(uint32_t){0}, &L.ev_r);
    static bool hooked;
    if (!hooked) {
        hooked = true;
        mhgu_ryu_set_hooks(ryu_lobby_changed, ryu_data_received);
    }
    mhgu_ryu_set_config(g_config.ryu_servers, "", g_config.ryu_password);
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
HLE_FUNC("_ZN2nn3ldn15OpenAccessPointEv", ldn_open_ap) { NTRACE();
    if (L.state != ST_INITIALIZED) { RET(R_INVALID_STATE); return; }
    L.state = ST_ACCESS_POINT;
    signal_change();
    RET(0);
}
HLE_FUNC("_ZN2nn3ldn16CloseAccessPointEv", ldn_close_ap) { NTRACE();
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
    uint32_t r = mhgu_ryu_create(-1, cfg, sec, user, L.adv, L.adv_size, info, &ip, &mask);
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
    if (g_config.ryu_host && room_configured() && mhgu_room_connected()) mhgu_xb_begin(info, ip, mask);
    signal_change();
    return 0;
}

static uint32_t create_network(const uint8_t *cfg, const uint8_t *sec, const uint8_t *sec_param, const uint8_t *user) {
    if (L.state != ST_ACCESS_POINT) return R_INVALID_STATE;
    if (ryu_configured()) return create_ryu(cfg, sec, user);
    bool via_room = room_configured() && mhgu_room_connected();
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
    else host_start();
    L.state = ST_AP_CREATED;
    LOG("[ldn] network created%s: %s, up to %d players, version %u\n", via_room ? " in the room" : "", ipstr(L.own_ip), L.info[NI_NODE_MAX], L.lcv);
    signal_change();
    return 0;
}

// CreateNetwork(const NetworkConfig &, const SecurityConfig &, const UserConfig &)
HLE_FUNC("_ZN2nn3ldn13CreateNetworkERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_10UserConfigE", ldn_create) { NTRACE();
    RET(create_network(ARGP(0), ARGP(1), NULL, ARGP(2)));
}
// CreateNetworkPrivate(config, security, SecurityParameter, user, address entry count, const AddressEntry *)
HLE_FUNC("_ZN2nn3ldn20CreateNetworkPrivateERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_17SecurityParameterERKNS0_10UserConfigEiPKNS0_12AddressEntryE",
         ldn_create_private) {
    RET(create_network(ARGP(0), ARGP(1), ARGP(2), ARGP(3)));
}

HLE_FUNC("_ZN2nn3ldn14DestroyNetworkEv", ldn_destroy) { NTRACE();
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
HLE_FUNC("_ZN2nn3ldn11OpenStationEv", ldn_open_station) { NTRACE();
    if (L.state != ST_INITIALIZED) { RET(R_INVALID_STATE); return; }
    L.state = ST_STATION;
    signal_change();
    RET(0);
}
HLE_FUNC("_ZN2nn3ldn12CloseStationEv", ldn_close_station) { NTRACE();
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
HLE_FUNC("_ZN2nn3ldn4ScanEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi", ldn_scan) { NTRACE();
    uint32_t out = ARG(0), pcount = ARG(1);
    int max = (int)ARG(2);
    const uint8_t *filter = ARGP(3);
    if (L.state != ST_STATION && L.state != ST_ACCESS_POINT) { RET(R_INVALID_STATE); return; }
    static uint8_t infos[MAX_FOUND][NETINFO_SIZE];
    static LobbyList online;
    struct sockaddr_in addrs[MAX_FOUND];
    uint64_t hubs[MAX_FOUND] = {0};
    pthread_t lt;
    hnet_init();  // (before the thread's first socket call)
    bool asked = lobby_server(online.server) && !pthread_create(&lt, NULL, lobby_list_thread, &online);
    int n = discover(infos, addrs, 16), k = 0;
    int ryu_of[MAX_FOUND] = {0};
    bool room_of[MAX_FOUND] = {0};
    if (asked) {  // the server's hubs after the LAN's (a hub found both ways is joined directly)
        pthread_join(lt, NULL);
        uint64_t own = mhgu_ldn_own_hub();
        for (int j = 0; j < online.n && n < MAX_FOUND; j++) {
            if (own && online.ids[j] == own) continue;  // our own hub: joining ourselves can't work
            bool dup = false;
            for (int i = 0; i < n; i++) dup |= !memcmp(infos[i] + NI_BSSID, online.infos[j] + NI_BSSID, 6);
            if (dup) continue;
            memcpy(infos[n], online.infos[j], NETINFO_SIZE);
            addrs[n] = online.addr;
            hubs[n++] = online.ids[j];
        }
    }
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
    if (room_enabled() && mhgu_room_connected() && n < MAX_FOUND) {
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
        found[nfound++].addr = addrs[i];
        if (k < max) memcpy((uint8_t *)G2H(out) + (size_t)k++ * NETINFO_SIZE, infos[i], NETINFO_SIZE);
    }
    if (pcount) ST32(pcount, k);
    static int last = -1, last_online = -1, last_max = -1;
    int listed = asked && online.state == HUB_ONLINE ? online.n : 0;
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
    char key[LOBBY_NAME];
    config_live_ldn(NULL, 0, key, sizeof key);
    if (found[i].hub) {
        // A hub on a server: the server splices us to the host, and keeps relaying.
        if (!mhgu_ldn_join_server(addr, key, found[i].hub, name, NULL, 0)) {
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
HLE_FUNC("_ZN2nn3ldn7ConnectERKNS0_11NetworkInfoERKNS0_14SecurityConfigERKNS0_10UserConfigEiNS0_13ConnectOptionE", ldn_connect) { NTRACE();
    RET(connect_network(ARGP(0), ARGP(2), (uint16_t)ARG(3)));
}
HLE_FUNC("_ZN2nn3ldn7ConnectERKNS0_11NetworkInfoERKNS0_14SecurityConfigERKNS0_10UserConfigEi", ldn_connect2) { NTRACE();
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
    RET(connect_private(c));
}

HLE_FUNC("_ZN2nn3ldn10DisconnectEv", ldn_disconnect) { NTRACE();
    if (L.state != ST_STATION_CONNECTED && L.state != ST_ERROR) { RET(R_INVALID_STATE); return; }
    if (L.ryu) ryu_stop();
    else if (L.room) room_stop();
    else station_leave();
    L.state = ST_STATION, L.reason = DR_BY_USER;
    L.own_ip = L.subnet = 0;
    signal_change();
    RET(0);
}
