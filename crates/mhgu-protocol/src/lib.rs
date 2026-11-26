//! Wire formats of the MHGU Local Play networking.
//!
//! There are two layers, both defined by the original C/Go code
//! (`runtime/net/ldn.c` and the hub in MHGURecomp):
//!
//! * the **hub-server protocol** — TCP control frames whose first payload is a `MHGS` header,
//!   plus a `MHGR` UDP relay; this is what `mhgu-hub` implements, and what both game clients speak;
//! * the **in-game link protocol** — `MHGL` TCP frames and `MHGD` UDP datagrams that two clients
//!   exchange through the relay; the server never parses these, it only splices bytes.
//!
//! Keeping these constants byte-for-byte identical is what makes a new build interoperate with
//! MHGURecomp players.

use std::io::{self, Read, Write};

// ---- hub-server protocol ---------------------------------------------------

/// Protocol version this server speaks (`serverProto` in server.go).
pub const SERVER_PROTO: u16 = 1;
/// `MHGS` + u16 server protocol + u16 link protocol + room[16].
pub const HEADER_SIZE: usize = 24;
/// The matchmaking key's room in that header.
pub const ROOM_SIZE: usize = 16;
/// `sizeof(NetworkInfo)` in ldn.c.
pub const NETINFO_SIZE: usize = 0x480;
/// Largest control payload in either direction (`MAX_FRAME` = `0x10000 + 64` in ldn.c): a LIST
/// answer holds up to `ListMax` hubs of `5 + sizeof(NetworkInfo)` bytes each.
pub const MAX_FRAME: u32 = 0x1_0040;

/// UDP relay prefix: `MHGR` + u32 pair id + u64 secret.
pub const UDP_MAGIC: [u8; 4] = *b"MHGR";
pub const UDP_HEADER: usize = 16;
/// Magic + pair + secret + the game's UDP header + the largest link payload.
pub const UDP_MAX: usize = UDP_HEADER + 12 + 16 + 8192;

/// Hole punching, step 1: a relayed link asks the hub server for the other end's public UDP address. The
/// datagram is the usual relay prefix + `MHGQ` + 12 zero bytes (`server.go` `peerQuery`).
pub const PEER_QUERY: [u8; 4] = *b"MHGQ";
/// The server's answer, as big as the query: `MHGP`, u32 pair, u64 the asker's secret, IPv4[4], u16 port
/// (big endian), 2 zero bytes, u64 the pair's direct key (`server.go` `peerAnswer`). No `MHGR` prefix.
pub const PEER_ANSWER: [u8; 4] = *b"MHGP";
/// The query and the answer are both this big: the relay prefix + 16; never bigger than the query.
pub const PEER_MSG_SIZE: usize = UDP_HEADER + 16;
/// A direct datagram starts like a relayed one (`MHGR` + u32 pair) but carries the pair's direct key
/// instead of a secret; the `MHGD` datagram follows.
pub const DIRECT_HEADER: usize = UDP_HEADER;

// NetworkInfo field offsets (mirrors `enum` in ldn.c and the Go constants).
pub const NI_SSID: usize = 0x26;
pub const NI_NET_TYPE: usize = 0x4B;
pub const NI_NODE_MAX: usize = 0x66;
pub const NI_NODE_COUNT: usize = 0x67;
pub const NI_ADV_SIZE: usize = 0x26A;
pub const NI_ADV: usize = 0x26C;
/// MHGU's advertise data: u16 passcode at 0xC0; 0xFFFF = none, else the 4-digit code in bits 0-14
/// with bit 15 set when it applies to everyone (clear: everyone but friends).
pub const ADV_PASSCODE: usize = 0xC0;
/// The node table in a `NetworkInfo`: `NI_NODES` + `i * NODE_SIZE`.
pub const NI_NODES: usize = 0x68;
pub const NODE_SIZE: usize = 0x40;
pub const NODE_IP: usize = 0x00;
pub const NODE_MAC: usize = 0x04;
pub const NODE_ID: usize = 0x0A;
pub const NODE_CONNECTED: usize = 0x0B;
pub const NODE_NAME: usize = 0x0C;
pub const NODE_LCV: usize = 0x2E;
pub const MAX_NODES: u8 = 8;
pub const ADV_MAX: usize = 0x180;

// Control frame types.
pub const T_LIST: u8 = 1;
pub const T_HOST: u8 = 2;
pub const T_ATTACH: u8 = 3;
pub const T_JOIN: u8 = 4;
pub const T_UPDATE: u8 = 5;
pub const T_PING: u8 = 6;
/// Protocol 4: the hub holding a session code, whatever its room.
pub const T_FIND: u8 = 7;
/// Protocol 4: the friends list (who is online, requests, removals).
pub const T_PRESENCE: u8 = 8;

pub const T_HUBS: u8 = 16;
pub const T_HOSTED: u8 = 17;
pub const T_INCOMING: u8 = 18;
pub const T_SPLICED: u8 = 19;
pub const T_ERROR: u8 = 20;
/// Protocol 4: `u32 count, count x (u64 id, u16 link protocol, code[8], NetworkInfo)`.
pub const T_HUBS4: u8 = 21;
/// Protocol 4: the answer to PRESENCE.
pub const T_FRIENDS: u8 = 22;

// Error codes carried by T_ERROR.
pub const ERR_NOT_FOUND: u32 = 1;
pub const ERR_NO_ANSWER: u32 = 2;
pub const ERR_BUSY: u32 = 3;
pub const ERR_VERSION: u32 = 4;
pub const ERR_BAD_REQ: u32 = 5;
/// Protocol 4: another hub holds that session code; the host picks another.
pub const ERR_CODE_TAKEN: u32 = 6;
/// Federation: the peer did not prove the shared token.
pub const ERR_FED_AUTH: u32 = 7;

// ---- federation ("MHGF") --------------------------------------------------
// Two hub servers that know each other's address and token can share their hubs: a player on A sees B's
// hubs in its search and joins them through A. The client protocol is untouched: remote hubs are listed
// under an alias A invents, and a JOIN for one is proxied to B over a second connection.
//
// A federation connection: HELLO, CHALLENGE, AUTH (HMAC-SHA256 of the shared token over both nonces),
// then one request: LIST (the hubs of one room), FIND (one session code) or JOIN (a hub, the connection
// becomes the player's link after SPLICED). Version 1 is direct peers only: no transitive advertisements.
pub const FED_MAGIC: [u8; 4] = *b"MHGF";
/// The federation protocol version this build speaks.
pub const FED_VERSION: u16 = 1;
/// Nonces in HELLO and CHALLENGE.
pub const FED_NONCE: usize = 16;
/// HMAC-SHA256 output.
pub const FED_MAC: usize = 32;
/// Federation frames can carry a room's worth of hubs, bigger than a client frame.
pub const FED_MAX_FRAME: u32 = 1 << 20;
/// Hubs in one FED_HUBS answer (each is 8 + 2 + 8 + 0x480 bytes).
pub const FED_HUBS_MAX: usize = 64;
/// The HMAC's domain separator: HMAC(token, FED_AUTH_SALT || client_nonce || server_nonce).
pub const FED_AUTH_SALT: &[u8] = b"MHGU federation auth v1";

pub const T_FED_HELLO: u8 = 32;     // u16 version, name_len u8, name, client_nonce[16]
pub const T_FED_CHALLENGE: u8 = 33; // server_nonce[16]
pub const T_FED_AUTH: u8 = 34;      // mac[32]
pub const T_FED_OK: u8 = 35;        // (nothing)
pub const T_FED_LIST: u8 = 36;      // room[16], u16 link_proto
pub const T_FED_FIND: u8 = 37;      // code[8]
pub const T_FED_JOIN: u8 = 38;      // room[16], u16 link_proto, u64 hub, u8 has_code, code[8]
pub const T_FED_HUBS: u8 = 39;      // u32 count, count x (u64 id, u16 link_proto, code[8], NetworkInfo)
// Ed25519 identity handshake (federation version 2). Both sides prove the key behind their server id.
pub const T_FED_HELLO2: u8 = 40;     // u16 2, u8 id_len, id, pk[32], u8 name_len, name, client_nonce[16]
pub const T_FED_CHALLENGE2: u8 = 41; // u8 id_len, id, pk[32], server_nonce[16], sig[64]
pub const T_FED_AUTH2: u8 = 42;      // sig[64]
/// After the handshake: this connection carries the sender's online players (EDU-style, no persistence).
pub const T_FED_PRESENCE: u8 = 43;   // (nothing) -> OK, then T_FED_PRESENCE_UPDATE frames
/// u16 count, then count x (friend id[10], state u8, code[8], u8 n, n x friend[10], u8 m, m x friend[10], u8 name_len, name).
pub const T_FED_PRESENCE_UPDATE: u8 = 44;
// Transitive advertisements (opt-in): a hub learned through another peer carries its origin and distance.
// An older peer answers ERR_BAD_REQ; the caller falls back to FED_LIST/FED_JOIN.
pub const T_FED_LIST_EXT: u8 = 45;   // room[16], u16 link_proto, u8 depth, u8 path_len, path_len x (u8 len, server id)
pub const T_FED_HUBS_EXT: u8 = 46;  // u32 count, count x (u8 origin_len, origin, u8 hops, u64 hub, u16 proto, code[8], NetworkInfo)
pub const T_FED_JOIN_EXT: u8 = 47;  // room[16], u16 link_proto, u8 hops, u8 origin_len, origin, u64 hub, u8 has_code, code[8]
pub const T_FED_FIND_EXT: u8 = 48;  // code[8], u8 depth, u8 path_len, path_len x (u8 len, server id) -> HUBS_EXT
/// Presence with forwarding: depth + requester id; the connection then carries UPDATE_EXT frames.
pub const T_FED_PRESENCE_EXT: u8 = 49;        // u8 depth, u8 requester_len, requester id
/// Like PRESENCE_UPDATE, each entry also ends with origin (u8 len + id), u8 hops, u8 flags (bit 0: gone).
pub const T_FED_PRESENCE_UPDATE_EXT: u8 = 50;
/// What the two signatures in the identity handshake cover (then both public keys, both nonces and both ids).
pub const FED_AUTH2_SALT: &[u8] = b"MHGU federation auth v2";
/// The advertised server-id prefix.
pub const FED_ID_PREFIX: &str = "ed25519:";

// ---- link protocol 4 ------------------------------------------------------
// Version 4 plays with 3: the link frames are the same (4 adds only what the hub server does: session codes,
// join by code, friends). A host takes a hello of either version and a LAN probe is answered in its own
// version; a station says to each host the version that host speaks (3 for LAN / address hosts, which every
// host of 3 or 4 takes). The hub server lists 3 and 4 together (its link pool).
/// The first link protocol with session codes, FIND, PRESENCE and HUBS4.
pub const LINK_PROTO_SESSIONS: u16 = 4;

/// Session codes: 8 characters of Crockford's base 32 (no I, L, O, U).
pub const CODE_LEN: usize = 8;
pub const CODE_ALPHABET: &[u8; 32] = b"0123456789ABCDEFGHJKMNPQRSTVWXYZ";

/// An install's friend code is the first 10 bytes of `SHA-256("MHGURecomp friend:" + secret)`.
pub const FRIEND_SALT: &[u8] = b"MHGURecomp friend:";
pub const FRIEND_LEN: usize = 10;
pub const FRIEND_SECRET_LEN: usize = 32;
pub const FRIEND_MAX: usize = 64;
/// Bytes of a hunter name in PRESENCE (UTF-8).
pub const FRIEND_NAME_MAX: usize = 32;
/// Friend requests in one FRIENDS answer (the newest).
pub const FRIEND_REQUESTS_MAX: usize = 8;
/// How long the server keeps a player online after its last PRESENCE.
pub const PRESENCE_TTL: std::time::Duration = std::time::Duration::from_secs(90);
/// What a player does (`presence.state`).
pub const FRIEND_STATE_ONLINE: u8 = 0;
pub const FRIEND_STATE_HOSTING: u8 = 1;
pub const FRIEND_STATE_IN_HUB: u8 = 2;

/// NetworkInfo offsets the protocol 4 checks need (`liSessionID`, `niBSSID`, `advHuntSearch` in server.go).
pub const NI_SESSION_ID: usize = 0x10;
pub const NI_BSSID: usize = 0x20;
/// A Hunter Search network has 0xFF at advertise byte 0x1C (hubs have 1 there, a two-player hub too).
pub const ADV_HUNT_SEARCH: usize = 0x1C;

/// Android's public hubs: the game sends this key when it has none, so they can have a room of their own.
pub const PHONE_KEY: &str = "public-phone";

/// Whether a link protocol is one this build plays (3 or 4).
pub fn link_version_ok(v: u16) -> bool {
    (LINK_PROTO_MIN..=LINK_PROTO_VERSION).contains(&v)
}

/// A session code as the game makes it: exactly `CODE_LEN` characters of the alphabet (zeros are "none").
pub fn code_ok(c: &[u8]) -> bool {
    c.len() == CODE_LEN && c.iter().all(|b| CODE_ALPHABET.contains(b))
}

/// A code as typed: case, spaces and dashes don't matter, and Crockford's look-alikes read as they're meant
/// (O as 0, I and L as 1). `None`: not 8 of the alphabet's characters.
pub fn code_normalize(typed: &str) -> Option<[u8; CODE_LEN]> {
    let mut out = [0u8; CODE_LEN];
    let mut n = 0;
    for c in typed.chars() {
        let mut ch = c;
        if ch == ' ' || ch == '-' {
            continue;
        }
        if ch.is_ascii_lowercase() {
            ch = ch.to_ascii_uppercase();
        }
        if ch == 'O' {
            ch = '0';
        } else if ch == 'I' || ch == 'L' {
            ch = '1';
        }
        if !ch.is_ascii() || !CODE_ALPHABET.contains(&(ch as u8)) || n == CODE_LEN {
            return None;
        }
        out[n] = ch as u8;
        n += 1;
    }
    (n == CODE_LEN).then_some(out)
}

/// A code shown as "ABCD-EFGH".
pub fn code_shown(code: &[u8]) -> String {
    let mut s = String::with_capacity(9);
    for (i, b) in code.iter().take(CODE_LEN).enumerate() {
        if i == 4 {
            s.push('-');
        }
        s.push(*b as char);
    }
    s
}

/// A Hunter Search network, not a hub: room for 2 players and 0xFF at advertise byte 0x1C.
pub fn hunt_search(info: &[u8]) -> bool {
    info.len() >= NETINFO_SIZE
        && info[NI_NODE_MAX] == 2
        && u16::from_le_bytes([info[NI_ADV_SIZE], info[NI_ADV_SIZE + 1]]) as usize > ADV_HUNT_SEARCH
        && info[NI_ADV + ADV_HUNT_SEARCH] == 0xFF
}

/// The room of Android's public hubs: the key `public-phone`.
pub fn phone_room() -> [u8; 16] {
    room_from_key(PHONE_KEY)
}

/// An install's friend code from its 32-byte secret.
pub fn friend_id(secret: &[u8]) -> [u8; FRIEND_LEN] {
    let d = sha256::digest(&[FRIEND_SALT, secret]);
    let mut id = [0u8; FRIEND_LEN];
    id.copy_from_slice(&d[..FRIEND_LEN]);
    id
}

/// A friend code as the game shows it: 10 bytes as 16 characters, 5 bits each (most significant first).
pub fn friend_text(id: &[u8]) -> String {
    let mut s = String::with_capacity(16);
    for i in 0..16 {
        let mut v = 0usize;
        for b in 0..5 {
            let k = i * 5 + b;
            v = v << 1 | ((id.get(k / 8).copied().unwrap_or(0) >> (7 - k % 8)) & 1) as usize;
        }
        s.push(CODE_ALPHABET[v] as char);
    }
    s
}

/// A typed 16-character friend code (with the same look-alikes as a session code) back to its 10 bytes.
pub fn friend_parse(typed: &str) -> Option<[u8; FRIEND_LEN]> {
    let mut v = [0u8; 16];
    let mut n = 0;
    for c in typed.chars() {
        let mut ch = c;
        if ch == ' ' || ch == '-' {
            continue;
        }
        if ch.is_ascii_lowercase() {
            ch = ch.to_ascii_uppercase();
        }
        if ch == 'O' {
            ch = '0';
        } else if ch == 'I' || ch == 'L' {
            ch = '1';
        }
        if n == 16 || !ch.is_ascii() {
            return None;
        }
        let b = ch as u8;
        v[n] = CODE_ALPHABET.iter().position(|&a| a == b)? as u8;
        n += 1;
    }
    if n != 16 {
        return None;
    }
    let mut id = [0u8; FRIEND_LEN];
    for i in 0..16 {
        let val = v[i];
        for b in 0..5 {
            if (val >> (4 - b)) & 1 == 1 {
                let k = i * 5 + b;
                id[k / 8] |= 0x80 >> (k % 8);
            }
        }
    }
    Some(id)
}

/// A friend code shown as "ABCD-EFGH-JKMN-PQRS".
pub fn friend_shown(id: &[u8]) -> String {
    let t = friend_text(id);
    format!("{}-{}-{}-{}", &t[..4], &t[4..8], &t[8..12], &t[12..16])
}

// ---- in-game link protocol ------------------------------------------------
// (Used between the two clients; documented here so a Rust client can share it.)
pub const LINK_MAGIC: [u8; 4] = *b"MHGL";
pub const LINK_PROTO_VERSION: u16 = 4;
/// The oldest link protocol this build speaks (4 plays with 3; the link frames are the same).
pub const LINK_PROTO_MIN: u16 = 3;
/// Largest link frame (`MAX_FRAME 0x10000 + 64` in ldn.c).
pub const LINK_MAX_FRAME: usize = 0x10000 + 64;
pub const LINK_UDP_MAGIC: [u8; 4] = *b"MHGD";
pub const LINK_UDP_MAX_PAYLOAD: usize = 8192;

// Link frame types (`F_*` in ldn.c).
pub const F_HELLO: u8 = 1;
pub const F_INFO: u8 = 2;
pub const F_WELCOME: u8 = 3;
pub const F_REFUSE: u8 = 4;
pub const F_PACKET: u8 = 5;
pub const F_DESTROY: u8 = 6;
pub const F_AUX: u8 = 7;

// `MHGD` datagram kinds and header size (magic + u32 token + u8 kind + u8 flags + pad[2]).
pub const U_DATA: u8 = 0;
pub const U_HELLO: u8 = 1;
pub const U_HELLO_ACK: u8 = 2;
pub const U_PUNCH: u8 = 3;
pub const LINK_UDP_HDR: usize = 12;
/// The header's flags byte (ldn.c's `pad[0]`).
pub const LINK_UDP_FLAGS: usize = 9;
/// A direct hello's flags, bit 0: "I receive your direct datagrams".
pub const UF_DIRECT_OK: u8 = 1;

// `VPacket` (net.h) — fields in host byte order; all supported hosts are little-endian.
pub const VPACKET_SIZE: usize = 16;
pub const VPROTO_UDP: u8 = 0;
pub const VPROTO_TCP: u8 = 1;
pub const VTCP_DATA: u8 = 0;
pub const VTCP_SYN: u8 = 1;
pub const VTCP_SYNACK: u8 = 2;
pub const VTCP_FIN: u8 = 3;
pub const VTCP_RST: u8 = 4;

// ---- framing --------------------------------------------------------------

/// Reads one control frame: `[u32 size][u8 type][payload]`, little-endian.
///
/// `size` counts the type byte and the payload, so the maximum accepted is `max + 1`.
pub fn read_frame<R: Read>(r: &mut R, max: u32) -> io::Result<(u8, Vec<u8>)> {
    let mut h = [0u8; 4];
    r.read_exact(&mut h)?;
    let size = u32::from_le_bytes(h);
    if size < 1 || size > max + 1 {
        return Err(io::Error::new(io::ErrorKind::InvalidData, "bad frame size"));
    }
    let mut p = vec![0u8; size as usize];
    r.read_exact(&mut p)?;
    Ok((p[0], p[1..].to_vec()))
}

/// Builds `[u32 size][u8 type][payloads…]`.
pub fn frame(t: u8, payloads: &[&[u8]]) -> Vec<u8> {
    let n: usize = 1 + payloads.iter().map(|p| p.len()).sum::<usize>();
    let mut b = Vec::with_capacity(4 + n);
    b.extend_from_slice(&(n as u32).to_le_bytes());
    b.push(t);
    for p in payloads {
        b.extend_from_slice(p);
    }
    b
}

pub fn write_frame<W: Write>(w: &mut W, f: &[u8]) -> io::Result<()> {
    w.write_all(f)
}

pub mod sha256;

/// HMAC-SHA256 (RFC 2104) over concatenated parts, for the federation handshake.
pub fn hmac_sha256(key: &[u8], parts: &[&[u8]]) -> [u8; 32] {
    let mut k = [0u8; 64];
    if key.len() > 64 {
        k[..32].copy_from_slice(&sha256::digest(&[key]));
    } else {
        k[..key.len()].copy_from_slice(key);
    }
    let mut ipad = [0x36u8; 64];
    let mut opad = [0x5cu8; 64];
    for i in 0..64 {
        ipad[i] ^= k[i];
        opad[i] ^= k[i];
    }
    let mut inner = sha256::Sha256::new();
    inner.update(&ipad);
    for p in parts {
        inner.update(p);
    }
    let inner = inner.finish();
    let mut outer = sha256::Sha256::new();
    outer.update(&opad);
    outer.update(&inner);
    outer.finish()
}

/// Compares two MACs without an early exit.
pub fn mac_eq(a: &[u8], b: &[u8]) -> bool {
    if a.len() != b.len() {
        return false;
    }
    let mut d = 0u8;
    for (x, y) in a.iter().zip(b) {
        d |= x ^ y;
    }
    d == 0
}

/// The matchmaking key's hash: a room is the first 16 bytes of
/// `SHA-256("MHGURecomp matchmaking key:" + key)` with the key trimmed of spaces (ldn.c
/// `lobby_header`); an empty key is the public room (all zero). The server never sees the key.
pub const KEY_SALT: &[u8] = b"MHGURecomp matchmaking key:";

/// The room (as the 24-byte request header carries it) for a matchmaking key.
pub fn room_from_key(key: &str) -> [u8; 16] {
    let k = key.trim_matches(' ');
    let mut room = [0u8; 16];
    if k.is_empty() {
        return room;
    }
    let d = sha256::digest(&[KEY_SALT, k.as_bytes()]);
    room.copy_from_slice(&d[..ROOM_SIZE]);
    room
}

// ---- NetworkInfo validation ----------------------------------------------

/// A `NetworkInfo` fit to be shown to other players (the game parses it).
pub fn info_ok(i: &[u8]) -> bool {
    if i.len() != NETINFO_SIZE {
        return false;
    }
    let max = i[NI_NODE_MAX];
    let count = i[NI_NODE_COUNT];
    let adv = u16::from_le_bytes([i[NI_ADV_SIZE], i[NI_ADV_SIZE + 1]]);
    i[NI_NET_TYPE] == 2
        && i[NI_SSID] <= 32
        && (1..=MAX_NODES).contains(&max)
        && count >= 1
        && count <= max
        && (adv as usize) <= ADV_MAX
}

/// The hub asks a passcode (of everyone, or of everyone but friends: the same here, internet hub players
/// aren't each other's Switch friends).
pub fn passcoded(i: &[u8]) -> bool {
    if i.len() < NI_ADV + ADV_PASSCODE + 2 {
        return false;
    }
    let adv = u16::from_le_bytes([i[NI_ADV_SIZE], i[NI_ADV_SIZE + 1]]) as usize;
    adv >= ADV_PASSCODE + 2
        && u16::from_le_bytes([i[NI_ADV + ADV_PASSCODE], i[NI_ADV + ADV_PASSCODE + 1]]) != 0xFFFF
}

/// A parsed `MHGS` request header.
#[derive(Clone, Copy, Debug)]
pub struct Header {
    pub link_proto: u16,
    /// First 16 bytes of SHA-256(matchmaking key); all zero when no key is set.
    pub room: [u8; 16],
}

/// Parses the first payload of a request. Returns `None` on a bad magic or a server-version
/// mismatch; the caller decides whether to answer with `ERR_VERSION`.
pub fn parse_header(p: &[u8]) -> Result<Header, HeaderError> {
    if p.len() < HEADER_SIZE || p[..4] != *b"MHGS" {
        return Err(HeaderError::BadMagic);
    }
    if u16::from_le_bytes([p[4], p[5]]) != SERVER_PROTO {
        return Err(HeaderError::Version);
    }
    let link_proto = u16::from_le_bytes([p[6], p[7]]);
    let mut room = [0u8; 16];
    room.copy_from_slice(&p[8..24]);
    Ok(Header { link_proto, room })
}

#[derive(Debug, PartialEq, Eq)]
pub enum HeaderError {
    BadMagic,
    Version,
}
