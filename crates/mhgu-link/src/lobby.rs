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

/// A hub in a LIST / FIND answer.
#[derive(Clone, Debug)]
pub struct HubEntry {
    pub id: u64,
    /// The host's `NetworkInfo`.
    pub info: Vec<u8>,
    /// The hub's link protocol (protocol 4's HUBS4; protocol 3 for a plain HUBS answer).
    pub link_proto: u16,
    /// Its session code (protocol 4; all zero: none).
    pub code: [u8; proto::CODE_LEN],
}

impl HubEntry {
    /// The session code as "ABCD-EFGH", or None.
    pub fn code_shown(&self) -> Option<String> {
        (self.code != [0u8; proto::CODE_LEN]).then(|| proto::code_shown(&self.code))
    }
}

/// A player online for the friends list.
#[derive(Clone, Debug)]
pub struct FriendOnline {
    pub id: [u8; proto::FRIEND_LEN],
    /// 0 in the game, 1 hosting a hub, 2 in a hub; another value is a newer game's.
    pub state: u8,
    /// Its hub's session code (zeros: none known).
    pub code: [u8; proto::CODE_LEN],
}

/// A friend request: a player online that lists us while we don't list it.
#[derive(Clone, Debug)]
pub struct FriendRequest {
    pub id: [u8; proto::FRIEND_LEN],
    /// The hunter name it goes by (UTF-8, may be empty).
    pub name: String,
}

/// The answer to PRESENCE.
#[derive(Clone, Debug, Default)]
pub struct Friends {
    /// The friends that are online and list us back, with what they do.
    pub online: Vec<FriendOnline>,
    /// The players that list us while we don't list them (the newest 8).
    pub requests: Vec<FriendRequest>,
    /// The players we list that removed us (our game drops them too).
    pub removed: Vec<[u8; proto::FRIEND_LEN]>,
}

/// A hub server error with its protocol code (`ERR_*`), which some requests answer specially.
#[derive(Debug)]
pub struct ServerError(pub u32);

impl std::fmt::Display for ServerError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "hub server error {}", self.0)
    }
}

impl std::error::Error for ServerError {}

/// The `ERR_*` code an error carries, if it is a hub server error.
pub fn error_code(e: &io::Error) -> Option<u32> {
    e.get_ref()?.downcast_ref::<ServerError>().map(|s| s.0)
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
    let code = if p.len() >= 4 { u32::from_le_bytes(p[..4].try_into().unwrap()) } else { 0 };
    io::Error::new(io::ErrorKind::Other, ServerError(code))
}

/// Parses a HUBS (u64 id, NetworkInfo) or HUBS4 (u64 id, u16 protocol, code[8], NetworkInfo) answer.
fn hubs_from(p: &[u8], v4: bool) -> Vec<HubEntry> {
    if p.len() < 4 {
        return Vec::new();
    }
    let n = u32::from_le_bytes(p[..4].try_into().unwrap()) as usize;
    let entry = 8 + if v4 { 2 + proto::CODE_LEN } else { 0 } + proto::NETINFO_SIZE;
    let mut out = Vec::new();
    for i in 0..n {
        let at = 4 + i * entry;
        if at + entry > p.len() {
            break;
        }
        let id = u64::from_le_bytes(p[at..at + 8].try_into().unwrap());
        let (link_proto, code, info_at) = if v4 {
            let lp = u16::from_le_bytes(p[at + 8..at + 10].try_into().unwrap());
            let mut code = [0u8; proto::CODE_LEN];
            code.copy_from_slice(&p[at + 10..at + 10 + proto::CODE_LEN]);
            (if proto::link_version_ok(lp) { lp } else { proto::LINK_PROTO_MIN }, code, at + 10 + proto::CODE_LEN)
        } else {
            (proto::LINK_PROTO_MIN, [0u8; proto::CODE_LEN], at + 8)
        };
        out.push(HubEntry { id, info: p[info_at..info_at + proto::NETINFO_SIZE].to_vec(), link_proto, code });
    }
    out
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
        if t != proto::T_HUBS && t != proto::T_HUBS4 {
            return Err(io::Error::new(io::ErrorKind::InvalidData, "not a HUBS answer"));
        }
        Ok(hubs_from(&p, t == proto::T_HUBS4))
    }

    /// FIND (protocol 4): the hub holding a session code, whatever its room (a code is an invitation).
    pub fn find(&self, link_proto: u16, code: &[u8; proto::CODE_LEN]) -> io::Result<Vec<HubEntry>> {
        let mut s = open(self.addr)?;
        send(&mut s, proto::T_FIND, &[&header(link_proto, [0u8; 16]), code])?;
        let (t, p) = read(&mut s)?;
        if t == proto::T_ERROR && p.len() >= 4 {
            return Err(server_error(&p));
        }
        if t != proto::T_HUBS4 {
            return Err(io::Error::new(io::ErrorKind::InvalidData, "not a HUBS4 answer"));
        }
        Ok(hubs_from(&p, true))
    }

    /// Tells the hub server what we do and which friends we list (PRESENCE), and reads which of them are
    /// online and list us back. `friends` and `removed` are friend codes; `name` is what our requests show.
    pub fn presence(
        &self,
        secret: &[u8; proto::FRIEND_SECRET_LEN],
        state: u8,
        code: &[u8; proto::CODE_LEN],
        friends: &[[u8; proto::FRIEND_LEN]],
        removed: &[[u8; proto::FRIEND_LEN]],
        name: &str,
    ) -> io::Result<Friends> {
        let mut s = open(self.addr)?;
        let n = friends.len().min(proto::FRIEND_MAX);
        let nb = &name.as_bytes()[..name.len().min(255)];
        let mut body = Vec::with_capacity(
            proto::FRIEND_SECRET_LEN + 1 + proto::CODE_LEN + 1 + n * proto::FRIEND_LEN + 1 + nb.len()
                + 1 + removed.len() * proto::FRIEND_LEN,
        );
        body.extend_from_slice(secret);
        body.push(state);
        body.extend_from_slice(code);
        body.push(n as u8);
        for f in &friends[..n] {
            body.extend_from_slice(f);
        }
        body.push(nb.len() as u8);
        body.extend_from_slice(nb);
        body.push(removed.len().min(255) as u8);
        for f in removed.iter().take(255) {
            body.extend_from_slice(f);
        }
        send(&mut s, proto::T_PRESENCE, &[&header(proto::LINK_PROTO_VERSION, [0u8; 16]), &body])?;
        let (t, p) = read(&mut s)?;
        if t == proto::T_ERROR {
            return Err(server_error(&p));
        }
        if t != proto::T_FRIENDS || p.is_empty() {
            return Err(io::Error::new(io::ErrorKind::InvalidData, "not a FRIENDS answer"));
        }
        let mut out = Friends::default();
        let count = p[0] as usize;
        let ent = proto::FRIEND_LEN + 1 + proto::CODE_LEN;
        let mut at = 1;
        for _ in 0..count {
            if at + ent > p.len() {
                break;
            }
            let mut id = [0u8; proto::FRIEND_LEN];
            id.copy_from_slice(&p[at..at + proto::FRIEND_LEN]);
            let state = p[at + proto::FRIEND_LEN];
            let mut code = [0u8; proto::CODE_LEN];
            code.copy_from_slice(&p[at + proto::FRIEND_LEN + 1..at + ent]);
            out.online.push(FriendOnline { id, state, code });
            at += ent;
        }
        let reqs = p.get(at).copied().unwrap_or(0) as usize;
        at += 1;
        for _ in 0..reqs {
            if at + proto::FRIEND_LEN + 1 > p.len() {
                break;
            }
            let mut id = [0u8; proto::FRIEND_LEN];
            id.copy_from_slice(&p[at..at + proto::FRIEND_LEN]);
            let nl = p[at + proto::FRIEND_LEN] as usize;
            at += proto::FRIEND_LEN + 1;
            if at + nl > p.len() {
                break;
            }
            let name = String::from_utf8_lossy(&p[at..at + nl]).into_owned();
            at += nl;
            out.requests.push(FriendRequest { id, name });
        }
        if let Some(&gone) = p.get(at) {
            at += 1;
            for _ in 0..gone as usize {
                if at + proto::FRIEND_LEN > p.len() {
                    break;
                }
                let mut id = [0u8; proto::FRIEND_LEN];
                id.copy_from_slice(&p[at..at + proto::FRIEND_LEN]);
                out.removed.push(id);
                at += proto::FRIEND_LEN;
            }
        }
        Ok(out)
    }

    /// Joins a hub: the server asks its host to connect and splices the two. The returned stream is
    /// the in-game link (send a `Hello` on it next). `code` is the hub's session code when known (from
    /// HUBS4, or an invitation): with it the server lets us join from any room.
    pub fn join(
        self,
        link_proto: u16,
        room: [u8; 16],
        hub: u64,
        code: Option<&[u8; proto::CODE_LEN]>,
    ) -> io::Result<Spliced> {
        let mut s = open(self.addr)?;
        match code {
            Some(c) => send(&mut s, proto::T_JOIN, &[&header(link_proto, room), &hub.to_le_bytes(), c])?,
            None => send(&mut s, proto::T_JOIN, &[&header(link_proto, room), &hub.to_le_bytes()])?,
        }
        spliced_from(read(&mut s)?, s)
    }

    /// Lists this hub on the server and returns the session that keeps it there (the host side). `code`
    /// is the session code to list it under (protocol 4; None: no code).
    pub fn host(
        self,
        info: &[u8],
        link_proto: u16,
        room: [u8; 16],
        code: Option<&[u8; proto::CODE_LEN]>,
    ) -> io::Result<HostSession> {
        let mut s = open(self.addr)?;
        match code {
            Some(c) => send(&mut s, proto::T_HOST, &[&header(link_proto, room), info, c])?,
            None => send(&mut s, proto::T_HOST, &[&header(link_proto, room), info])?,
        }
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
