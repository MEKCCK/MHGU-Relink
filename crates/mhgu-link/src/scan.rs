//! LAN search (ldn.c `scan_addresses` / the host's `disc_s` answer): a UDP `MHGL` probe, and the host's
//! `MHGL` reply carrying its `NetworkInfo`.

use std::io;
use std::net::{SocketAddr, UdpSocket};
use std::time::{Duration, Instant};

use mhgu_protocol::{LINK_MAGIC, LINK_PROTO_VERSION, NETINFO_SIZE};

/// The probe a station broadcasts: `MHGL` + u16 protocol version.
pub fn probe() -> [u8; 6] {
    let mut p = [0u8; 6];
    p[..4].copy_from_slice(&LINK_MAGIC);
    p[4..6].copy_from_slice(&LINK_PROTO_VERSION.to_le_bytes());
    p
}

/// A host's reply to a probe.
#[derive(Clone, Debug)]
pub struct ScanReply {
    /// The host's TCP (and UDP) port.
    pub port: u16,
    /// The host's `NetworkInfo`.
    pub info: Vec<u8>,
    pub from: SocketAddr,
}

/// The reply a host sends for a probe: `MHGL` + u16 version + u16 port + NetworkInfo. `None` for a
/// probe that is not ours.
pub fn answer(probe: &[u8], port: u16, info: &[u8]) -> Option<Vec<u8>> {
    if probe.len() < 6
        || probe[..4] != LINK_MAGIC
        || u16::from_le_bytes([probe[4], probe[5]]) != LINK_PROTO_VERSION
    {
        return None;
    }
    let mut r = Vec::with_capacity(8 + info.len());
    r.extend_from_slice(&LINK_MAGIC);
    r.extend_from_slice(&LINK_PROTO_VERSION.to_le_bytes());
    r.extend_from_slice(&port.to_le_bytes());
    r.extend_from_slice(info);
    Some(r)
}

/// Parses a reply from `from`; `None` if it is not a scan answer.
pub fn parse_reply(b: &[u8], from: SocketAddr) -> Option<ScanReply> {
    if b.len() < 8 + NETINFO_SIZE
        || b[..4] != LINK_MAGIC
        || u16::from_le_bytes([b[4], b[5]]) != LINK_PROTO_VERSION
    {
        return None;
    }
    Some(ScanReply {
        port: u16::from_le_bytes([b[6], b[7]]),
        info: b[8..8 + NETINFO_SIZE].to_vec(),
        from,
    })
}

/// Sends the probe to each target and collects the answers until `timeout`.
pub fn scan(socket: &UdpSocket, targets: &[SocketAddr], timeout: Duration) -> io::Result<Vec<ScanReply>> {
    let p = probe();
    for t in targets {
        let _ = socket.send_to(&p, t);
    }
    let mut out = Vec::new();
    let deadline = Instant::now() + timeout;
    let mut buf = [0u8; 8 + NETINFO_SIZE + 64];
    loop {
        let now = Instant::now();
        if now >= deadline {
            break;
        }
        socket.set_read_timeout(Some(deadline.saturating_duration_since(now)))?;
        match socket.recv_from(&mut buf) {
            Ok((n, from)) => {
                if let Some(r) = parse_reply(&buf[..n], from) {
                    out.push(r);
                }
            }
            Err(_) => break,
        }
    }
    Ok(out)
}
