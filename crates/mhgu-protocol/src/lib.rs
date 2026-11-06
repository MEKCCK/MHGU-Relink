//! Wire formats of the MHGU Local Play networking.
//!
//! There are two layers, both defined by the original C/Go code
//! (`runtime/net/ldn.c` and `server/server.go` in MHGURecomp, shared by the gu-native fork):
//!
//! * the **hub-server protocol** — TCP control frames whose first payload is a `MHGS` header,
//!   plus a `MHGR` UDP relay; this is what `mhgu-hub` implements, and what both game clients speak;
//! * the **in-game link protocol** — `MHGL` TCP frames and `MHGD` UDP datagrams that two clients
//!   exchange through the relay; the server never parses these, it only splices bytes.
//!
//! Keeping these constants byte-for-byte identical is what makes a new build interoperate with
//! gu-native (Android) and MHGURecomp (PC) players.

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

// NetworkInfo field offsets (mirrors `enum` in ldn.c and the Go constants).
pub const NI_SSID: usize = 0x26;
pub const NI_NET_TYPE: usize = 0x4B;
pub const NI_NODE_MAX: usize = 0x66;
pub const NI_NODE_COUNT: usize = 0x67;
pub const NI_ADV_SIZE: usize = 0x26A;
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

pub const T_HUBS: u8 = 16;
pub const T_HOSTED: u8 = 17;
pub const T_INCOMING: u8 = 18;
pub const T_SPLICED: u8 = 19;
pub const T_ERROR: u8 = 20;

// Error codes carried by T_ERROR.
pub const ERR_NOT_FOUND: u32 = 1;
pub const ERR_NO_ANSWER: u32 = 2;
pub const ERR_BUSY: u32 = 3;
pub const ERR_VERSION: u32 = 4;
pub const ERR_BAD_REQ: u32 = 5;

// ---- in-game link protocol ------------------------------------------------
// (Used between the two clients; documented here so a Rust client can share it.)
pub const LINK_MAGIC: [u8; 4] = *b"MHGL";
pub const LINK_PROTO_VERSION: u16 = 3;
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

// `MHGD` datagram kinds and header size (magic + u32 token + u8 kind + pad[3]).
pub const U_DATA: u8 = 0;
pub const U_HELLO: u8 = 1;
pub const U_HELLO_ACK: u8 = 2;
pub const LINK_UDP_HDR: usize = 12;

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
