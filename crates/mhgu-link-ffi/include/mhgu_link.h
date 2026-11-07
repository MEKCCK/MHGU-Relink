/* Local Play (nn::ldn) over IP for the C runtime: the C side of `mhgu-link`/`mhgu-hub`.
 *
 * The shapes are the ones `runtime/net/net.h` already uses, so this replaces the C network module
 * without touching the `nn::ldn` HLE above it: a `VPacket` header, `ldn_send` into the network, and
 * the delivery callbacks that feed the virtual sockets and the `F_AUX` messages.
 *
 * One end hosts (an embedded hub plus one link to every peer, relaying between them); the others
 * join by address. Addresses are the ones a Switch would use: 169.254.0.1 (the host) with a /16.
 */
#ifndef MHGU_LINK_H
#define MHGU_LINK_H
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { MHGU_VPROTO_UDP = 0, MHGU_VPROTO_TCP = 1 };
enum { MHGU_VTCP_DATA = 0, MHGU_VTCP_SYN, MHGU_VTCP_SYNACK, MHGU_VTCP_FIN, MHGU_VTCP_RST };

typedef struct {
    uint32_t src_ip, dst_ip;
    uint16_t src_port, dst_port;
    uint8_t proto, kind; /* MHGU_VPROTO_*; for TCP MHGU_VTCP_* */
    uint16_t pad;
} MhguVPacket;

/* Delivery: a game packet from another player, and a port-private message (`F_AUX`). */
typedef void (*MhguDeliverPacket)(const MhguVPacket *p, const uint8_t *data, uint32_t len);
typedef void (*MhguDeliverAux)(const uint8_t *data, uint32_t len);

/* The callbacks the link delivers through (set once, before hosting or joining). */
void mhgu_ldn_set_delivery(MhguDeliverPacket packet, MhguDeliverAux aux);

/* Host: starts the embedded hub on `port` (0: any) and links to it as the first player.
 * `info` is the NetworkInfo blob the game's `nn::ldn` layer handed us; `name` is the player's.
 * The hub's port is returned in `*port_out` (0 on failure). */
bool mhgu_ldn_host(uint16_t port, uint16_t *port_out, const char *name, const uint8_t *info, uint32_t info_len);
/* The game's local communication version, sent in the next join's Hello (0: send 0). */
void mhgu_ldn_set_lcv(uint16_t lcv);
/* The game's own MAC (6 bytes): the join Hello carries it, so the host's node table names this machine
 * the way the game expects. Without it a join gets a random MAC and the game finds no node of its own. */
void mhgu_ldn_set_mac(const uint8_t mac[6]);

/* Hosts through a hub server (`[Multiplayer] Server`): the server lists the hub for players with the
 * same matchmaking key and splices them to links we open. Works alongside the LAN listener. */
bool mhgu_ldn_host_server(const char *server, const char *key);
/* Joins a hub `hub` listed on `server` (its id from the list). */
bool mhgu_ldn_join_server(const char *server, const char *key, uint64_t hub, const char *name,
                          const uint8_t *info, uint32_t info_len);

/* Join a host at "a.b.c.d" (or "a.b.c.d:port"). */
bool mhgu_ldn_join(const char *addr, const char *name, const uint8_t *info, uint32_t info_len);
void mhgu_ldn_leave(void);

uint32_t mhgu_ldn_disconnect_reason(void);
bool mhgu_ldn_net_up(void);   /* a network exists */
bool mhgu_ldn_hosting(void);  /* we host it */
/* Players and the maximum, and an id for the network; false when there is no network. */
bool mhgu_ldn_party(int *n, int *max, char *id, unsigned long size);
uint32_t mhgu_ldn_own_ip(void);
uint32_t mhgu_ldn_broadcast_ip(void);
/* The current NetworkInfo blob (the game's `nn::ldn` state), copied into `buf`. */
uint32_t mhgu_ldn_info(uint8_t *buf, uint32_t size);
/* The host's listing id on the hub server the link opened (0: not listed). */
uint64_t mhgu_ldn_own_hub(void);
/* The host's NetworkInfo changed: keep it and send it to every peer (F_INFO). */
bool mhgu_ldn_update_info(const uint8_t *info, uint32_t size);
/* The host sends the player at `ip` away (F_DESTROY, DR_REJECTED) and closes its link. */
bool mhgu_ldn_reject(uint32_t ip);

/* Into the network: from a local virtual socket, and a port-private message. */
void mhgu_ldn_send(const MhguVPacket *p, const uint8_t *data, uint32_t len);
void mhgu_ldn_send_aux(const uint8_t *data, uint32_t len);

/* Delivers everything received since the last call (from the game's thread). */
void mhgu_ldn_poll(void);

/* LAN search: one host found by a broadcast probe. */
typedef struct {
    uint32_t ip;   /* the host's address */
    uint16_t port; /* its link port */
    uint16_t pad;
    uint8_t info[0x480]; /* its NetworkInfo */
} MhguScanEntry;

/* Probes the LAN for hosts listening on `port`; returns the entries written (at most `max`). */
uint32_t mhgu_ldn_scan(uint16_t port, uint32_t timeout_ms, MhguScanEntry *out, uint32_t max);

#ifdef __cplusplus
}
#endif
#endif
