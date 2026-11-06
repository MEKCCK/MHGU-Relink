//! `MHGD` datagrams: the game's packets by UDP when it gets through (Pia expects datagrams), instead
//! of through the TCP link.

use mhgu_protocol::LINK_UDP_MAGIC;

/// The largest payload a datagram may carry (ldn.c `UDP_MAX_PAYLOAD`).
pub const MAX_PAYLOAD: usize = mhgu_protocol::LINK_UDP_MAX_PAYLOAD;

/// The header: magic + u32 token + u8 kind + pad[3].
pub const HEADER_SIZE: usize = 12;

/// Builds an `MHGD` datagram. `payload` is `VPacket` + data for `U_DATA`, empty for `U_HELLO`.
pub fn datagram(token: u32, kind: u8, payload: &[u8]) -> Vec<u8> {
    let mut d = Vec::with_capacity(HEADER_SIZE + payload.len());
    d.extend_from_slice(&LINK_UDP_MAGIC);
    d.extend_from_slice(&token.to_le_bytes());
    d.push(kind);
    d.extend_from_slice(&[0, 0, 0]);
    d.extend_from_slice(payload);
    d
}

/// Parses an `MHGD` datagram: `(token, kind, payload)`. `None` on a bad magic.
pub fn parse(b: &[u8]) -> Option<(u32, u8, &[u8])> {
    if b.len() < HEADER_SIZE || b[..4] != LINK_UDP_MAGIC {
        return None;
    }
    let token = u32::from_le_bytes(b[4..8].try_into().unwrap());
    let kind = b[8];
    Some((token, kind, &b[HEADER_SIZE..]))
}
