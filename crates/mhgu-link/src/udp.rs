//! `MHGD` datagrams: the game's packets by UDP when it gets through (Pia expects datagrams), instead
//! of through the TCP link.

use mhgu_protocol::LINK_UDP_MAGIC;

/// The largest payload a datagram may carry (ldn.c `UDP_MAX_PAYLOAD`).
pub const MAX_PAYLOAD: usize = mhgu_protocol::LINK_UDP_MAX_PAYLOAD;

/// The header: magic + u32 token + u8 kind + u8 flags + pad[2].
pub const HEADER_SIZE: usize = 12;

/// Builds an `MHGD` datagram with no flags. `payload` is `VPacket` + data for `U_DATA`, empty for
/// `U_HELLO`.
pub fn datagram(token: u32, kind: u8, payload: &[u8]) -> Vec<u8> {
    datagram_flags(token, kind, 0, payload)
}

/// Builds an `MHGD` datagram; `flags` is byte 9 (a direct hello sets `UF_DIRECT_OK`).
pub fn datagram_flags(token: u32, kind: u8, flags: u8, payload: &[u8]) -> Vec<u8> {
    let mut d = Vec::with_capacity(HEADER_SIZE + payload.len());
    d.extend_from_slice(&LINK_UDP_MAGIC);
    d.extend_from_slice(&token.to_le_bytes());
    d.push(kind);
    d.push(flags);
    d.extend_from_slice(&[0, 0]);
    d.extend_from_slice(payload);
    d
}

/// Parses an `MHGD` datagram: `(token, kind, payload)`. `None` on a bad magic.
pub fn parse(b: &[u8]) -> Option<(u32, u8, &[u8])> {
    parse_flags(b).map(|(token, kind, _, payload)| (token, kind, payload))
}

/// Parses an `MHGD` datagram: `(token, kind, flags, payload)`. `None` on a bad magic.
pub fn parse_flags(b: &[u8]) -> Option<(u32, u8, u8, &[u8])> {
    if b.len() < HEADER_SIZE || b[..4] != LINK_UDP_MAGIC {
        return None;
    }
    let token = u32::from_le_bytes(b[4..8].try_into().unwrap());
    Some((token, b[8], b[mhgu_protocol::LINK_UDP_FLAGS], &b[HEADER_SIZE..]))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn flags_roundtrip() {
        let d = datagram_flags(0x1122_3344, mhgu_protocol::U_HELLO, mhgu_protocol::UF_DIRECT_OK, b"abcd");
        assert_eq!(d[9], mhgu_protocol::UF_DIRECT_OK);
        let (token, kind, flags, payload) = parse_flags(&d).unwrap();
        assert_eq!(token, 0x1122_3344);
        assert_eq!(kind, mhgu_protocol::U_HELLO);
        assert_eq!(flags, mhgu_protocol::UF_DIRECT_OK);
        assert_eq!(payload, b"abcd");
        // An older peer leaves the flags byte zero.
        let (_, _, flags, _) = parse(&d).map(|(t, k, p)| (t, k, 0, p)).unwrap();
        assert_eq!(flags, 0);
        assert_eq!(parse(&d).unwrap().2, b"abcd");
    }
}
