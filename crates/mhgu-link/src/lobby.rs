//! The hub-server client: LIST / JOIN and the host side's HOST / ATTACH
//! (ldn.c `lobby_list`, `lobby_join`, `lobby_thread`, `lobby_attach_thread`).
//!
//! The server handles one request per connection (LIST closes it after the answer; JOIN and HOST
//! keep it), so every request here opens its own connection, as ldn.c does.
//!
//! A room is the first 16 bytes of `SHA-256("MHGURecomp matchmaking key:" + key)`, with the key trimmed of
//! spaces; an empty key means the public room (all zero). The server never sees the key.

use std::io::{self, Write};
use std::net::{SocketAddr, TcpStream};
use std::time::{Duration, Instant};

use mhgu_protocol as proto;



pub const LOBBY_PORT: u16 = 39510;

const MAX_FRAME: u32 = proto::MAX_FRAME;
/// The host sends a PING this often (ldn.c `LOBBY_PING_NS`).
pub const PING_EVERY: Duration = Duration::from_secs(15);

/// The room for a matchmaking key (trimmed of spaces); all zero for no key.
/// The derivation lives in `mhgu-protocol`, next to the header it fills.
pub use proto::room_from_key;

/// A request header: `MHGS`, u16 server protocol (1), u16 link protocol, room[16].
pub fn header(link_proto: u16, room: [u8; 16]) -> [u8; 24] {
    let mut h = [0u8; 24];
    h[..4].copy_from_slice(b"MHGS");
    h[4..6].copy_from_slice(&proto::SERVER_PROTO.to_le_bytes());
    h[6..8].copy_from_slice(&link_proto.to_le_bytes());
    h[8..].copy_from_slice(&room);
    h
}

/// A hub in a LIST answer.
#[derive(Clone, Debug)]
pub struct HubEntry {
    pub id: u64,
    /// The host's `NetworkInfo`.
    pub info: Vec<u8>,
}

/// A connection that has been spliced to the other end of a hub: it now carries the in-game link.
pub struct Spliced {
    pub stream: TcpStream,
    /// The relay pair id and our secret (for `MHGR` UDP through the server).
    pub pair: u32,
    pub secret: u64,
}

fn open(addr: SocketAddr) -> io::Result<TcpStream> {
    let s = TcpStream::connect_timeout(&addr, Duration::from_secs(5))?;
    s.set_nodelay(true)?;
    Ok(s)
}

fn send(s: &mut TcpStream, t: u8, payloads: &[&[u8]]) -> io::Result<()> {
    s.write_all(&proto::frame(t, payloads))
}

fn read(s: &mut TcpStream) -> io::Result<(u8, Vec<u8>)> {
    proto::read_frame(s, MAX_FRAME)
}

fn server_error(p: &[u8]) -> io::Error {
    io::Error::new(
        io::ErrorKind::Other,
        format!("hub server error {}", u32::from_le_bytes(p[..4].try_into().unwrap())),
    )
}

fn spliced_from((t, p): (u8, Vec<u8>), stream: TcpStream) -> io::Result<Spliced> {
    if t == proto::T_SPLICED && p.len() >= 12 {
        Ok(Spliced {
            stream,
            pair: u32::from_le_bytes(p[0..4].try_into().unwrap()),
            secret: u64::from_le_bytes(p[4..12].try_into().unwrap()),
        })
    } else if t == proto::T_ERROR && p.len() >= 4 {
        Err(server_error(&p))
    } else {
        Err(io::Error::new(io::ErrorKind::InvalidData, "not a SPLICED answer"))
    }
}

/// The hub server's address; each request opens its own connection.
pub struct Lobby {
    addr: SocketAddr,
}

impl Lobby {
    pub fn new(addr: SocketAddr) -> Lobby {
        Lobby { addr }
    }

    /// Lists the hubs the server shows this room / link protocol.
    pub fn list(&self, link_proto: u16, room: [u8; 16]) -> io::Result<Vec<HubEntry>> {
        let mut s = open(self.addr)?;
        send(&mut s, proto::T_LIST, &[&header(link_proto, room)])?;
        let (t, p) = read(&mut s)?;
        if t == proto::T_ERROR && p.len() >= 4 {
            return Err(server_error(&p));
        }
        if t != proto::T_HUBS || p.len() < 4 {
            return Err(io::Error::new(io::ErrorKind::InvalidData, "not a HUBS answer"));
        }
        let n = u32::from_le_bytes(p[..4].try_into().unwrap()) as usize;
        let mut out = Vec::new();
        for i in 0..n {
            let at = 4 + i * (8 + proto::NETINFO_SIZE);
            if at + 8 + proto::NETINFO_SIZE > p.len() {
                break;
            }
            let id = u64::from_le_bytes(p[at..at + 8].try_into().unwrap());
            let info = p[at + 8..at + 8 + proto::NETINFO_SIZE].to_vec();
            out.push(HubEntry { id, info });
        }
        Ok(out)
    }

    /// Joins a hub: the server asks its host to connect and splices the two. The returned stream is
    /// the in-game link (send a `Hello` on it next).
    pub fn join(self, link_proto: u16, room: [u8; 16], hub: u64) -> io::Result<Spliced> {
        let mut s = open(self.addr)?;
        send(&mut s, proto::T_JOIN, &[&header(link_proto, room), &hub.to_le_bytes()])?;
        spliced_from(read(&mut s)?, s)
    }

    /// Lists this hub on the server and returns the session that keeps it there (the host side).
    pub fn host(self, info: &[u8], link_proto: u16, room: [u8; 16]) -> io::Result<HostSession> {
        let mut s = open(self.addr)?;
        send(&mut s, proto::T_HOST, &[&header(link_proto, room), info])?;
        let (t, p) = read(&mut s)?;
        if t != proto::T_HOSTED || p.len() < 8 {
            return Err(if t == proto::T_ERROR && p.len() >= 4 {
                server_error(&p)
            } else {
                io::Error::new(io::ErrorKind::InvalidData, "not a HOSTED answer")
            });
        }
        let hub_id = u64::from_le_bytes(p[..8].try_into().unwrap());
        let now = Instant::now();
        Ok(HostSession {
            stream: s,
            addr: self.addr,
            link_proto,
            room,
            hub_id,
            last_rx: now,
            next_ping: now + PING_EVERY,
            pending_update: None,
        })
    }
}

/// A hub kept listed on the server, and the players it announces.
pub struct HostSession {
    stream: TcpStream,
    addr: SocketAddr,
    link_proto: u16,
    room: [u8; 16],
    hub_id: u64,
    last_rx: Instant,
    next_ping: Instant,
    pending_update: Option<Vec<u8>>,
}

impl HostSession {
    pub fn hub_id(&self) -> u64 {
        self.hub_id
    }

    /// Queues a new `NetworkInfo` to send on the next `poll`.
    pub fn set_info(&mut self, info: Vec<u8>) {
        self.pending_update = Some(info);
    }

    /// Sends pending UPDATE / due PING, then waits up to `timeout` for the next frame.
    ///
    /// `Some(ticket)` when a player joins (pass it to [`HostSession::attach`]); `None` on a timeout
    /// (call again); an error when the server is lost or refuses.
    pub fn poll(&mut self, timeout: Duration) -> io::Result<Option<u64>> {
        if let Some(info) = self.pending_update.take() {
            self.stream.write_all(&proto::frame(proto::T_UPDATE, &[&info]))?;
        }
        let now = Instant::now();
        if now >= self.next_ping {
            self.stream.write_all(&proto::frame(proto::T_PING, &[]))?;
            self.next_ping = now + PING_EVERY;
        }
        self.stream.set_read_timeout(Some(timeout))?;
        match proto::read_frame(&mut self.stream, MAX_FRAME) {
            Ok((proto::T_INCOMING, p)) if p.len() >= 8 => {
                self.last_rx = Instant::now();
                Ok(Some(u64::from_le_bytes(p[..8].try_into().unwrap())))
            }
            Ok((proto::T_PING, _)) => {
                self.last_rx = Instant::now();
                Ok(None)
            }
            Ok((proto::T_ERROR, p)) if p.len() >= 4 => Err(server_error(&p)),
            Ok(_) => Ok(None),
            Err(e)
                if e.kind() == io::ErrorKind::WouldBlock || e.kind() == io::ErrorKind::TimedOut =>
            {
                if Instant::now().duration_since(self.last_rx) > PING_EVERY * 3 {
                    Err(io::Error::new(io::ErrorKind::TimedOut, "lost the hub server"))
                } else {
                    Ok(None)
                }
            }
            Err(e) => Err(e),
        }
    }

    /// Opens the second connection the server splices to a player who joined with `ticket`.
    pub fn attach(&self, ticket: u64) -> io::Result<Spliced> {
        let mut s = open(self.addr)?;
        send(
            &mut s,
            proto::T_ATTACH,
            &[&header(self.link_proto, self.room), &ticket.to_le_bytes()],
        )?;
        spliced_from(read(&mut s)?, s)
    }
}
