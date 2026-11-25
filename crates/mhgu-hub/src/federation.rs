//! Hub federation: two servers that know each other's address and token share their hubs.
//!
//! A client still talks to one server. That server lists a peer's hubs under an alias it invents, and a
//! JOIN for one is proxied: this server opens a second, authenticated connection to the peer, asks it to
//! join the hub, and splices the player's stream to that connection. UDP is bridged by rewriting the
//! `MHGR` relay prefix on each leg. Peers are direct by default; `-federation-transitive N` forwards
//! advertisements (and joins) across up to N extra hops. Federated legs always stay on the relay
//! (no hole punching).
//!
//! Wire format (`MHGF`): HELLO (version, name, client nonce), CHALLENGE (server nonce), AUTH
//! (HMAC-SHA256 of the peer's token over the salt and both nonces), OK, then one request: LIST, FIND or
//! JOIN. See `mhgu-protocol` for the constants.

use std::collections::{HashMap, HashSet};
use std::io::{self, Read, Write};
use std::net::{IpAddr, SocketAddr, TcpStream, ToSocketAddrs};
use std::sync::atomic::AtomicU64;
use std::sync::{mpsc, Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};

use mhgu_protocol as proto;

use crate::identity::{identity_id, Identity};

use crate::bucket::TokenBucket;
use crate::config::Peer;

/// How long a peer's LIST answer is reused before asking again.
pub(super) const CACHE_TTL: Duration = Duration::from_secs(3);
/// How long a listed remote hub stays joinable after its last sighting.
pub(super) const REMOTE_TTL: Duration = Duration::from_secs(30);
/// Per-request deadline for LIST / FIND (a slow peer must not hold up a search).
pub(super) const REQUEST_TIMEOUT: Duration = Duration::from_millis(1500);
/// LAN discovery beacons: a candidate is never auto-joined.
pub(super) const DISCOVERY_PORT: u16 = 39513;
pub(super) const DISCOVERY_MAGIC: &[u8; 8] = b"MHGFDISC";
pub(super) const DISCOVERY_EVERY: Duration = Duration::from_secs(30);
pub(super) const CANDIDATE_TTL: Duration = Duration::from_secs(90);
/// One `NetworkInfo` blob.
type NetInfo = [u8; proto::NETINFO_SIZE];

/// A hub as a peer reported it, before this server gives it an alias.
#[derive(Clone, Debug)]
pub(super) struct RawHub {
    pub id: u64,
    pub link_proto: u16,
    pub code: [u8; proto::CODE_LEN],
    pub info: NetInfo,
    /// The server that owns the hub ("" on a plain LIST: the direct peer).
    pub origin: String,
    /// Relays from the answering server to the origin (0 = its own hub).
    pub hops: u8,
}

/// A peer, its health for the web UI, and its resolved address.
#[derive(Clone)]
pub(super) struct PeerState {
    pub cfg: Peer,
    pub name: String,
    pub addr: Option<SocketAddr>,
    /// Our identity, for the Ed25519 handshake.
    pub identity: Arc<Identity>,
    pub last_ok: Option<Instant>,
    pub last_error: Option<String>,
    pub rtt_ms: u64,
    pub hubs: usize,
    /// A `http://host:port` well-known URL, fetched to learn the endpoint and key.
    url: Option<String>,
    last_discover: Option<Instant>,
}

impl PeerState {
    fn new(cfg: Peer, default_token: &str, identity: Arc<Identity>) -> PeerState {
        let mut cfg = cfg;
        if cfg.token.is_empty() {
            cfg.token = default_token.to_string();
        }
        let url = if cfg.addr.starts_with("http://") || cfg.addr.starts_with("https://") {
            Some(cfg.addr.clone())
        } else {
            None
        };
        let name = if cfg.name.is_empty() { cfg.addr.clone() } else { cfg.name.clone() };
        let addr = if url.is_some() { None } else { resolve_peer(&cfg.addr) };
        let mut p = PeerState {
            cfg,
            name,
            addr,
            identity,
            last_ok: None,
            last_error: None,
            rtt_ms: 0,
            hubs: 0,
            url,
            last_discover: None,
        };
        let _ = p.ensure();
        p
    }

    /// Resolves a well-known peer again (or for the first time); other peers are left as configured.
    fn ensure(&mut self) -> bool {
        let Some(url) = self.url.clone() else { return self.addr.is_some() };
        if let Some(at) = self.last_discover {
            if self.addr.is_some() && at.elapsed() < Duration::from_secs(60) {
                return true;
            }
        }
        self.last_discover = Some(Instant::now());
        match fetch_well_known(&url) {
            Ok((id, endpoint, pk)) => {
                if identity_id(&pk) != id {
                    self.last_error = Some("well-known: the key does not match its id".into());
                    return false;
                }
                self.cfg.key = id.clone();
                self.cfg.addr = endpoint.clone();
                if self.name == url {
                    self.name = id;
                }
                match resolve_peer(&endpoint) {
                    Some(a) => {
                        self.addr = Some(a);
                        self.last_error = None;
                        true
                    }
                    None => {
                        self.last_error = Some(format!("well-known endpoint {endpoint} does not resolve"));
                        self.addr = None;
                        false
                    }
                }
            }
            Err(e) => {
                self.last_error = Some(format!("well-known: {e}"));
                false
            }
        }
    }
}

/// A remote hub with the alias this server lists it under.
#[derive(Clone, Debug)]
pub(crate) struct RemoteHub {
    pub alias: u64,
    /// The direct peer that knows/leads to the hub (the next hop for a join).
    pub peer: usize,
    /// The server that owns the hub, and the relays between us and it.
    pub origin: String,
    pub hops: u8,
    pub id: u64,
    pub link_proto: u16,
    pub code: [u8; proto::CODE_LEN],
    pub info: NetInfo,
    /// The room it was listed in (zeros for a FIND by code: the code is the invitation).
    pub room: [u8; 16],
    pub created: Instant,
    pub seen: Instant,
}

struct Cache {
    at: Instant,
}

/// A server heard on the LAN: a candidate, never trusted or connected on its own.
#[derive(Clone, Debug)]
pub(super) struct Candidate {
    pub id: String,
    pub name: String,
    pub addr: String,
    pub public: [u8; 32],
    pub seen: Instant,
    /// Already configured in `-federate` (shown, but not a new candidate).
    pub configured: bool,
}

/// One local player's presence, as sent to peers (never persisted).
#[derive(Clone, Debug)]
pub(super) struct PresenceUpdate {
    pub id: [u8; proto::FRIEND_LEN],
    pub state: u8,
    pub code: [u8; proto::CODE_LEN],
    pub friends: Vec<[u8; proto::FRIEND_LEN]>,
    pub removed: Vec<[u8; proto::FRIEND_LEN]>,
    pub name: String,
    /// The server that owns this player ("" on a plain update: the direct peer).
    pub origin: String,
    /// Relays between the receiving server and the origin (0 = the peer's own player).
    pub hops: u8,
    /// A tombstone: drop the remote player matching `id` and `origin`.
    pub gone: bool,
}

/// A presence received from a peer; `gen` is its connection, cleared when that closes.
#[derive(Clone, Debug)]
pub(super) struct RemotePresence {
    pub gen: u64,
    pub origin: String,
    /// The direct peer stream this copy came from (used to avoid echoing it back in snapshots).
    pub source: String,
    pub hops: u8,
    pub state: u8,
    pub code: [u8; proto::CODE_LEN],
    pub friends: HashSet<[u8; proto::FRIEND_LEN]>,
    pub removed: HashSet<[u8; proto::FRIEND_LEN]>,
    pub name: String,
    pub seen: Instant,
}

/// One presence stream's forwarding preferences (extended streams only in practice).
#[derive(Clone, Debug)]
pub(super) struct PresenceSub {
    pub ext: bool,
    pub depth: u8,
    /// The peer's own id as declared in the extended request (or its tag on a plain stream).
    pub requester: String,
    /// The direct stream peer's tag, for not echoing data back over the stream it came from.
    pub source_tag: String,
}

/// All federation state, inside `Shared`.
pub(super) struct FedState {
    pub identity: Arc<Identity>,
    pub peers: Vec<PeerState>,
    /// alias -> remote hub
    pub remote: HashMap<u64, RemoteHub>,
    /// (origin server id, its hub id) -> alias, so a re-listing keeps the id a client saw
    by_key: HashMap<(String, u64), u64>,
    /// (peer, room) -> last answer
    cache: HashMap<(usize, [u8; 16]), Cache>,
    /// LAN candidates by server id.
    pub candidates: HashMap<String, Candidate>,
    /// Players online on peers, by friend code.
    pub remote_presence: HashMap<[u8; proto::FRIEND_LEN], RemotePresence>,
    /// Connection generations for presence streams (a closed stream clears only its own entries).
    pub presence_gen: u64,
    /// Federated joins made, for the web UI.
    pub joins: u64,
    /// Bytes relayed for federated players.
    pub bytes: AtomicU64,
}

impl FedState {
    pub(super) fn new(peers: &[Peer], default_token: &str, identity: Arc<Identity>) -> FedState {
        FedState {
            identity: Arc::clone(&identity),
            peers: peers
                .iter()
                .cloned()
                .map(|p| PeerState::new(p, default_token, Arc::clone(&identity)))
                .collect(),
            remote: HashMap::new(),
            by_key: HashMap::new(),
            cache: HashMap::new(),
            candidates: HashMap::new(),
            remote_presence: HashMap::new(),
            presence_gen: 0,
            joins: 0,
            bytes: AtomicU64::new(0),
        }
    }

    /// A cached answer for this peer and room, if it is fresh enough.
    fn fresh(&self, peer: usize, room: &[u8; 16], now: Instant) -> Option<&Cache> {
        self.cache.get(&(peer, *room)).filter(|c| now.duration_since(c.at) < CACHE_TTL)
    }

    /// The alias for a hub, stable across re-listings and never one of this server's own hub ids.
    fn alias_for(&mut self, origin: &str, id: u64, avoid: &HashSet<u64>) -> u64 {
        if let Some(a) = self.by_key.get(&(origin.to_string(), id)) {
            return *a;
        }
        let mut a = 0u64;
        while a == 0 || self.remote.contains_key(&a) || avoid.contains(&a) {
            a = rand64();
        }
        self.by_key.insert((origin.to_string(), id), a);
        a
    }

    /// Stores a peer's answer and gives its hubs stable aliases. `avoid` is this server's own hub ids,
    /// which an alias must never shadow.
    pub(super) fn ingest(
        &mut self,
        peer: usize,
        room: [u8; 16],
        answer: Result<Vec<RawHub>, String>,
        rtt_ms: u64,
        avoid: &HashSet<u64>,
    ) {
        let now = Instant::now();
        let entries = match answer {
            Ok(v) => {
                if let Some(p) = self.peers.get_mut(peer) {
                    p.last_ok = Some(now);
                    p.last_error = None;
                    p.rtt_ms = rtt_ms;
                    p.hubs = v.len();
                }
                v
            }
            Err(e) => {
                if let Some(p) = self.peers.get_mut(peer) {
                    p.last_error = Some(e);
                    p.rtt_ms = rtt_ms;
                    p.hubs = 0;
                }
                Vec::new()
            }
        };
        self.cache.insert((peer, room), Cache { at: now });
        let keep: HashSet<u64> = entries.iter().map(|h| h.id).collect();
        // Drop what this peer no longer lists in this room.
        let gone: Vec<u64> = self
            .remote
            .values()
            .filter(|r| r.peer == peer && r.room == room && !keep.contains(&r.id))
            .map(|r| r.alias)
            .collect();
        for alias in gone {
            self.remote.remove(&alias);
        }
        for h in entries {
            let origin = if h.origin.is_empty() {
                self.peers.get(peer).map(|p| if p.cfg.key.is_empty() { p.name.clone() } else { p.cfg.key.clone() }).unwrap_or_default()
            } else {
                h.origin
            };
            let alias = self.alias_for(&origin, h.id, avoid);
            let created = self.remote.get(&alias).map(|r| r.created).unwrap_or(now);
            // Keep the shortest path (a direct peer's own listing beats a forwarded one).
            if let Some(old) = self.remote.get(&alias) {
                if old.hops < h.hops {
                    self.remote.get_mut(&alias).unwrap().seen = now;
                    continue;
                }
            }
            self.remote.insert(
                alias,
                RemoteHub {
                    alias,
                    peer,
                    origin,
                    hops: h.hops,
                    id: h.id,
                    link_proto: h.link_proto,
                    code: h.code,
                    info: h.info,
                    room,
                    created,
                    seen: now,
                },
            );
        }
    }

    /// Gives aliases to hubs found by a session code (their room is unknown: the code is the invitation).
    pub(super) fn ingest_find(&mut self, peer: usize, entries: Vec<RawHub>, avoid: &HashSet<u64>) -> Vec<u64> {
        let now = Instant::now();
        let mut out = Vec::new();
        for h in entries {
            let peer_tag = self.peers.get(peer).map(|p| if p.cfg.key.is_empty() { p.name.clone() } else { p.cfg.key.clone() }).unwrap_or_default();
            let origin = if h.origin.is_empty() { peer_tag } else { h.origin };
            let hops = if origin.is_empty() { 0 } else { h.hops };
            let alias = self.alias_for(&origin, h.id, avoid);
            let created = self.remote.get(&alias).map(|r| r.created).unwrap_or(now);
            if let Some(old) = self.remote.get(&alias) {
                if old.hops < hops {
                    self.remote.get_mut(&alias).unwrap().seen = now;
                    out.push(alias);
                    continue;
                }
            }
            self.remote.insert(
                alias,
                RemoteHub {
                    alias,
                    peer,
                    origin,
                    hops,
                    id: h.id,
                    link_proto: h.link_proto,
                    code: h.code,
                    info: h.info,
                    room: [0u8; 16],
                    created,
                    seen: now,
                },
            );
            out.push(alias);
        }
        out
    }

    /// A remote hub by the alias a client saw, if it is still fresh.
    pub(super) fn get(&self, alias: u64) -> Option<RemoteHub> {
        self.remote
            .get(&alias)
            .filter(|r| r.seen.elapsed() < REMOTE_TTL)
            .cloned()
    }

    /// A remote hub by its owner and id (for a transitive join).
    pub(super) fn by_origin(&self, origin: &str, id: u64) -> Option<RemoteHub> {
        self.remote
            .values()
            .find(|r| r.origin == origin && r.id == id && r.seen.elapsed() < REMOTE_TTL)
            .cloned()
    }

    /// Every fresh remote hub in a room, for merging into a LIST answer.
    pub(super) fn in_room(&self, room: &[u8; 16]) -> Vec<RemoteHub> {
        let now = Instant::now();
        self.remote
            .values()
            .filter(|r| r.room == *room && now.duration_since(r.seen) < REMOTE_TTL)
            .cloned()
            .collect()
    }

    pub(super) fn prune(&mut self, presence_ttl: Duration) -> Vec<PresenceUpdate> {
        let now = Instant::now();
        self.remote.retain(|_, r| now.duration_since(r.seen) < REMOTE_TTL);
        self.by_key.retain(|(origin, id), alias| {
            self.remote
                .get(alias)
                .map(|r| r.origin == *origin && r.id == *id)
                .unwrap_or(false)
        });
        self.cache.retain(|_, c| now.duration_since(c.at) < REMOTE_TTL);
        self.candidates.retain(|_, c| now.duration_since(c.seen) < CANDIDATE_TTL);
        let stale: Vec<[u8; proto::FRIEND_LEN]> = self
            .remote_presence
            .iter()
            .filter(|(_, p)| now.duration_since(p.seen) >= presence_ttl)
            .map(|(id, _)| *id)
            .collect();
        let mut gone = Vec::new();
        for id in stale {
            if let Some(p) = self.remote_presence.remove(&id) {
                gone.push(remote_tombstone(id, p.origin, p.hops));
            }
        }
        gone
    }

    /// Adds a configured peer at runtime (an approved candidate), unless its id/address is already there.
    pub(super) fn add_peer(&mut self, peer: Peer, token: &str) {
        if self.peers.iter().any(|p| {
            (!peer.key.is_empty() && p.cfg.key == peer.key) || p.cfg.addr == peer.addr
        }) {
            return;
        }
        self.peers
            .push(PeerState::new(peer, token, Arc::clone(&self.identity)));
    }

    pub(super) fn mark_configured(&mut self, id: &str) {
        if let Some(c) = self.candidates.get_mut(id) {
            c.configured = true;
        }
    }

    /// A LAN beacon: keep it as a candidate unless it is us or already known.
    pub(super) fn candidate(&mut self, id: String, name: String, addr: String, public: [u8; 32]) {
        if id == self.identity.id {
            return;
        }
        let configured = self.peers.iter().any(|p| p.cfg.key == id || p.cfg.addr == addr);
        self.candidates.insert(
            id.clone(),
            Candidate { id, name, addr, public, seen: Instant::now(), configured },
        );
    }

    pub(super) fn candidate_views(&self) -> Vec<Candidate> {
        self.candidates.values().cloned().collect()
    }

    /// The next stale peer requests a search should make, and a fresh cached answer otherwise.
    pub(super) fn room_answers(&mut self, room: [u8; 16]) -> (Vec<RemoteHub>, Vec<usize>) {
        let now = Instant::now();
        let stale: Vec<usize> = (0..self.peers.len())
            .filter(|p| self.fresh(*p, &room, now).is_none())
            .collect();
        (self.in_room(&room), stale)
    }

}

fn resolve_peer(addr: &str) -> Option<SocketAddr> {
    let with_port = if addr.rfind(':').is_some() { addr.to_string() } else { format!("{addr}:39510") };
    with_port.to_socket_addrs().ok().and_then(|mut it| it.next())
}

fn rand64() -> u64 {
    let mut b = [0u8; 8];
    getrandom::fill(&mut b).expect("OS randomness unavailable");
    u64::from_le_bytes(b)
}

fn nonce() -> [u8; proto::FED_NONCE] {
    let mut n = [0u8; proto::FED_NONCE];
    getrandom::fill(&mut n).expect("OS randomness unavailable");
    n
}

/// A remote error with its protocol code (for mapping to the player's answer).
#[derive(Debug)]
pub(super) struct RemoteCode(pub u32);
impl std::fmt::Display for RemoteCode {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "peer error {}", self.0)
    }
}
impl std::error::Error for RemoteCode {}

pub(super) fn remote_error(code: u32) -> io::Error {
    io::Error::new(io::ErrorKind::Other, RemoteCode(code))
}

pub(super) fn error_code(e: &io::Error) -> Option<u32> {
    e.get_ref()?.downcast_ref::<RemoteCode>().map(|c| c.0)
}

fn protocol(msg: &str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, msg.to_string())
}

fn write_frame(s: &mut TcpStream, t: u8, payloads: &[&[u8]]) -> io::Result<()> {
    s.write_all(&proto::frame(t, payloads))
}

fn read_frame(s: &mut TcpStream) -> io::Result<(u8, Vec<u8>)> {
    proto::read_frame(s, proto::FED_MAX_FRAME)
}

fn connect(peer: &PeerState, timeout: Duration) -> io::Result<TcpStream> {
    let addr = peer.addr.ok_or_else(|| protocol("peer address not resolved"))?;
    let s = TcpStream::connect_timeout(&addr, timeout)?;
    s.set_nodelay(true)?;
    s.set_read_timeout(Some(timeout))?;
    s.set_write_timeout(Some(timeout))?;
    Ok(s)
}

/// Proves the peer's token and leaves the connection ready for one request.
fn handshake_v1(s: &mut TcpStream, peer: &PeerState, name: &str, timeout: Duration) -> io::Result<()> {
    let client = nonce();
    let n = name.as_bytes().len().min(64);
    let mut hello = Vec::with_capacity(4 + 2 + 1 + n + proto::FED_NONCE);
    hello.extend_from_slice(&proto::FED_MAGIC);
    hello.extend_from_slice(&proto::FED_VERSION.to_le_bytes());
    hello.push(n as u8);
    hello.extend_from_slice(&name.as_bytes()[..n]);
    hello.extend_from_slice(&client);
    write_frame(s, proto::T_FED_HELLO, &[&hello])?;
    let (mut t, mut p) = read_frame(s)?;
    if t == proto::T_ERROR && p.len() >= 4 {
        return Err(remote_error(u32::from_le_bytes(p[..4].try_into().unwrap())));
    }
    if t != proto::T_FED_CHALLENGE || p.len() < proto::FED_NONCE {
        return Err(protocol("not a federation challenge"));
    }
    let server = &p[..proto::FED_NONCE];
    let mac = proto::hmac_sha256(peer.cfg.token.as_bytes(), &[proto::FED_AUTH_SALT, &client, server]);
    write_frame(s, proto::T_FED_AUTH, &[&mac])?;
    (t, p) = read_frame(s)?;
    if t == proto::T_ERROR && p.len() >= 4 {
        return Err(remote_error(u32::from_le_bytes(p[..4].try_into().unwrap())));
    }
    if t != proto::T_FED_OK {
        return Err(protocol("federation auth refused"));
    }
    s.set_read_timeout(Some(timeout))?;
    Ok(())
}

/// The transcript both sides sign in the Ed25519 handshake.
fn auth2_parts<'a>(
    b_pk: &'a [u8; 32],
    a_pk: &'a [u8; 32],
    a_nonce: &'a [u8],
    b_nonce: &'a [u8],
    b_id: &'a [u8],
    a_id: &'a [u8],
) -> [&'a [u8]; 7] {
    [proto::FED_AUTH2_SALT, b_pk, a_pk, a_nonce, b_nonce, b_id, a_id]
}

fn handshake(s: &mut TcpStream, peer: &PeerState, name: &str, timeout: Duration) -> io::Result<()> {
    if peer.cfg.key.is_empty() {
        return handshake_v1(s, peer, name, timeout);
    }
    let me = &*peer.identity;
    let client = nonce();
    let n = name.as_bytes().len().min(64);
    let mut hello = Vec::with_capacity(4 + 2 + 1 + me.id.len() + 32 + 1 + n + proto::FED_NONCE);
    hello.extend_from_slice(&proto::FED_MAGIC);
    hello.extend_from_slice(&2u16.to_le_bytes());
    hello.push(me.id.len() as u8);
    hello.extend_from_slice(me.id.as_bytes());
    hello.extend_from_slice(&me.public);
    hello.push(n as u8);
    hello.extend_from_slice(&name.as_bytes()[..n]);
    hello.extend_from_slice(&client);
    write_frame(s, proto::T_FED_HELLO2, &[&hello])?;
    let (t, p) = read_frame(s)?;
    if t == proto::T_ERROR && p.len() >= 4 {
        return Err(remote_error(u32::from_le_bytes(p[..4].try_into().unwrap())));
    }
    if t != proto::T_FED_CHALLENGE2 || p.len() < 1 + 32 + proto::FED_NONCE + 64 {
        return Err(protocol("not an identity challenge"));
    }
    let il = p[0] as usize;
    if p.len() < 1 + il + 32 + proto::FED_NONCE + 64 {
        return Err(protocol("short identity challenge"));
    }
    let b_id = &p[1..1 + il];
    let mut b_pk = [0u8; 32];
    b_pk.copy_from_slice(&p[1 + il..1 + il + 32]);
    let b_nonce = &p[1 + il + 32..1 + il + 32 + proto::FED_NONCE];
    let mut sig_b = [0u8; 64];
    sig_b.copy_from_slice(&p[1 + il + 32 + proto::FED_NONCE..1 + il + 32 + proto::FED_NONCE + 64]);
    let b_id_s = String::from_utf8_lossy(b_id);
    if b_id_s != identity_id(&b_pk) || !peer.cfg.key.is_empty() && peer.cfg.key != b_id_s {
        return Err(protocol("the peer's identity is not the one expected"));
    }
    let parts = auth2_parts(&b_pk, &me.public, &client, b_nonce, b_id, me.id.as_bytes());
    if !Identity::verify(&b_pk, &parts, &sig_b) {
        return Err(protocol("the peer's identity signature is invalid"));
    }
    let sig_a = me.sign(&parts);
    write_frame(s, proto::T_FED_AUTH2, &[&sig_a])?;
    let (t, p) = read_frame(s)?;
    if t == proto::T_ERROR && p.len() >= 4 {
        return Err(remote_error(u32::from_le_bytes(p[..4].try_into().unwrap())));
    }
    if t != proto::T_FED_OK {
        return Err(protocol("federation identity refused"));
    }
    s.set_read_timeout(Some(timeout))?;
    Ok(())
}

pub(super) fn parse_hubs(p: &[u8]) -> Vec<RawHub> {
    if p.len() < 4 {
        return Vec::new();
    }
    let n = (u32::from_le_bytes(p[..4].try_into().unwrap()) as usize).min(proto::FED_HUBS_MAX);
    let entry = 8 + 2 + proto::CODE_LEN + proto::NETINFO_SIZE;
    let mut out = Vec::new();
    for i in 0..n {
        let at = 4 + i * entry;
        if at + entry > p.len() {
            break;
        }
        let id = u64::from_le_bytes(p[at..at + 8].try_into().unwrap());
        let lp = u16::from_le_bytes(p[at + 8..at + 10].try_into().unwrap());
        let mut code = [0u8; proto::CODE_LEN];
        code.copy_from_slice(&p[at + 10..at + 10 + proto::CODE_LEN]);
        let mut info = [0u8; proto::NETINFO_SIZE];
        info.copy_from_slice(
            &p[at + 10 + proto::CODE_LEN..at + 10 + proto::CODE_LEN + proto::NETINFO_SIZE],
        );
        if proto::info_ok(&info) {
            out.push(RawHub { id, link_proto: lp, code, info, origin: String::new(), hops: 0 });
        }
    }
    out
}

pub(super) fn encode_hubs(entries: &[RawHub]) -> Vec<u8> {
    let entry = 8 + 2 + proto::CODE_LEN + proto::NETINFO_SIZE;
    let mut out = Vec::with_capacity(4 + entries.len() * entry);
    out.extend_from_slice(&(entries.len() as u32).to_le_bytes());
    for h in entries.iter().take(proto::FED_HUBS_MAX) {
        out.extend_from_slice(&h.id.to_le_bytes());
        out.extend_from_slice(&h.link_proto.to_le_bytes());
        out.extend_from_slice(&h.code);
        out.extend_from_slice(&h.info);
    }
    out
}

/// `T_FED_PRESENCE_UPDATE` payload: count, then each player's presence.
fn encode_updates(items: &[PresenceUpdate]) -> Vec<u8> {
    let mut out = Vec::with_capacity(2 + items.len() * 96);
    out.extend_from_slice(&(items.len().min(u16::MAX as usize) as u16).to_le_bytes());
    for u in items.iter().take(u16::MAX as usize) {
        out.extend_from_slice(&u.id);
        out.push(u.state);
        out.extend_from_slice(&u.code);
        let n = u.friends.len().min(proto::FRIEND_MAX);
        out.push(n as u8);
        for f in u.friends.iter().take(n) {
            out.extend_from_slice(f);
        }
        let m = u.removed.len().min(proto::FRIEND_MAX);
        out.push(m as u8);
        for f in u.removed.iter().take(m) {
            out.extend_from_slice(f);
        }
        let nb = &u.name.as_bytes()[..u.name.len().min(proto::FRIEND_NAME_MAX)];
        out.push(nb.len() as u8);
        out.extend_from_slice(nb);
    }
    out
}

fn parse_updates(p: &[u8]) -> Vec<PresenceUpdate> {
    if p.len() < 2 {
        return Vec::new();
    }
    let n = u16::from_le_bytes([p[0], p[1]]) as usize;
    let mut at = 2;
    let mut out = Vec::new();
    for _ in 0..n {
        if at + proto::FRIEND_LEN + 1 + proto::CODE_LEN + 1 > p.len() {
            break;
        }
        let mut id = [0u8; proto::FRIEND_LEN];
        id.copy_from_slice(&p[at..at + proto::FRIEND_LEN]);
        at += proto::FRIEND_LEN;
        let state = p[at];
        at += 1;
        let mut code = [0u8; proto::CODE_LEN];
        code.copy_from_slice(&p[at..at + proto::CODE_LEN]);
        at += proto::CODE_LEN;
        let nf = p[at] as usize;
        at += 1;
        if nf > proto::FRIEND_MAX || at + nf * proto::FRIEND_LEN + 1 > p.len() {
            break;
        }
        let mut friends = Vec::with_capacity(nf);
        for _ in 0..nf {
            let mut f = [0u8; proto::FRIEND_LEN];
            f.copy_from_slice(&p[at..at + proto::FRIEND_LEN]);
            at += proto::FRIEND_LEN;
            friends.push(f);
        }
        let nr = p[at] as usize;
        at += 1;
        if nr > proto::FRIEND_MAX || at + nr * proto::FRIEND_LEN + 1 > p.len() {
            break;
        }
        let mut removed = Vec::with_capacity(nr);
        for _ in 0..nr {
            let mut f = [0u8; proto::FRIEND_LEN];
            f.copy_from_slice(&p[at..at + proto::FRIEND_LEN]);
            at += proto::FRIEND_LEN;
            removed.push(f);
        }
        let nl = p[at] as usize;
        at += 1;
        if at + nl > p.len() {
            break;
        }
        let name = String::from_utf8_lossy(&p[at..at + nl])
            .chars()
            .filter(|c| (*c as u32) >= 0x20 && *c as u32 != 0x7F)
            .take(proto::FRIEND_NAME_MAX)
            .collect();
        at += nl;
        if id != [0u8; proto::FRIEND_LEN] {
            out.push(PresenceUpdate { id, state, code, friends, removed, name, origin: String::new(), hops: 0, gone: false });
        }
    }
    out
}

/// Extended presence update: the base entry plus origin, hops and a tombstone flag.
fn encode_updates_ext(items: &[PresenceUpdate]) -> Vec<u8> {
    let mut out = Vec::with_capacity(2 + items.len() * 128);
    out.extend_from_slice(&(items.len().min(u16::MAX as usize) as u16).to_le_bytes());
    for u in items.iter().take(u16::MAX as usize) {
        out.extend_from_slice(&u.id);
        out.push(u.state);
        out.extend_from_slice(&u.code);
        let n = u.friends.len().min(proto::FRIEND_MAX);
        out.push(n as u8);
        for f in u.friends.iter().take(n) {
            out.extend_from_slice(f);
        }
        let m = u.removed.len().min(proto::FRIEND_MAX);
        out.push(m as u8);
        for f in u.removed.iter().take(m) {
            out.extend_from_slice(f);
        }
        let nb = &u.name.as_bytes()[..u.name.len().min(proto::FRIEND_NAME_MAX)];
        out.push(nb.len() as u8);
        out.extend_from_slice(nb);
        let ob = u.origin.as_bytes();
        let ol = ob.len().min(255);
        out.push(ol as u8);
        out.extend_from_slice(&ob[..ol]);
        out.push(u.hops);
        out.push(u.gone as u8);
    }
    out
}

fn parse_updates_ext(p: &[u8]) -> Vec<PresenceUpdate> {
    if p.len() < 2 {
        return Vec::new();
    }
    let n = u16::from_le_bytes([p[0], p[1]]) as usize;
    let mut at = 2;
    let mut out = Vec::new();
    for _ in 0..n {
        if at + proto::FRIEND_LEN + 1 + proto::CODE_LEN + 1 > p.len() {
            break;
        }
        let mut id = [0u8; proto::FRIEND_LEN];
        id.copy_from_slice(&p[at..at + proto::FRIEND_LEN]);
        at += proto::FRIEND_LEN;
        let state = p[at];
        at += 1;
        let mut code = [0u8; proto::CODE_LEN];
        code.copy_from_slice(&p[at..at + proto::CODE_LEN]);
        at += proto::CODE_LEN;
        let nf = p[at] as usize;
        at += 1;
        if nf > proto::FRIEND_MAX || at + nf * proto::FRIEND_LEN + 1 > p.len() {
            break;
        }
        let mut friends = Vec::with_capacity(nf);
        for _ in 0..nf {
            let mut f = [0u8; proto::FRIEND_LEN];
            f.copy_from_slice(&p[at..at + proto::FRIEND_LEN]);
            at += proto::FRIEND_LEN;
            friends.push(f);
        }
        let nr = p[at] as usize;
        at += 1;
        if nr > proto::FRIEND_MAX || at + nr * proto::FRIEND_LEN + 1 > p.len() {
            break;
        }
        let mut removed = Vec::with_capacity(nr);
        for _ in 0..nr {
            let mut f = [0u8; proto::FRIEND_LEN];
            f.copy_from_slice(&p[at..at + proto::FRIEND_LEN]);
            at += proto::FRIEND_LEN;
            removed.push(f);
        }
        let nl = p[at] as usize;
        at += 1;
        if at + nl + 3 > p.len() {
            break;
        }
        let name = String::from_utf8_lossy(&p[at..at + nl])
            .chars()
            .filter(|c| (*c as u32) >= 0x20 && *c as u32 != 0x7F)
            .take(proto::FRIEND_NAME_MAX)
            .collect();
        at += nl;
        let ol = p[at] as usize;
        at += 1;
        if at + ol + 2 > p.len() {
            break;
        }
        let origin = String::from_utf8_lossy(&p[at..at + ol]).into_owned();
        at += ol;
        let hops = p[at];
        at += 1;
        let gone = p[at] & 1 != 0;
        at += 1;
        if id != [0u8; proto::FRIEND_LEN] {
            out.push(PresenceUpdate { id, state, code, friends, removed, name, origin, hops, gone });
        }
    }
    out
}

fn peer_tag(peer: &PeerState) -> String {
    if !peer.cfg.key.is_empty() {
        peer.cfg.key.clone()
    } else {
        peer.name.clone()
    }
}

/// Extended HUB entries (with origin and hops) for transitive advertisements.
pub(super) fn encode_hubs_ext(entries: &[RawHub]) -> Vec<u8> {
    let mut out = Vec::with_capacity(4 + entries.len() * 160);
    out.extend_from_slice(&(entries.len().min(proto::FED_HUBS_MAX) as u32).to_le_bytes());
    for e in entries.iter().take(proto::FED_HUBS_MAX) {
        let origin = e.origin.as_bytes();
        let olen = origin.len().min(255);
        out.push(olen as u8);
        out.extend_from_slice(&origin[..olen]);
        out.push(e.hops);
        out.extend_from_slice(&e.id.to_le_bytes());
        out.extend_from_slice(&e.link_proto.to_le_bytes());
        out.extend_from_slice(&e.code);
        out.extend_from_slice(&e.info);
    }
    out
}

pub(super) fn parse_hubs_ext(p: &[u8]) -> Vec<RawHub> {
    if p.len() < 4 {
        return Vec::new();
    }
    let n = (u32::from_le_bytes(p[..4].try_into().unwrap()) as usize).min(proto::FED_HUBS_MAX);
    let mut at = 4;
    let mut out = Vec::new();
    for _ in 0..n {
        if at + 1 > p.len() {
            break;
        }
        let ol = p[at] as usize;
        at += 1;
        if at + ol + 1 + 8 + 2 + proto::CODE_LEN + proto::NETINFO_SIZE > p.len() {
            break;
        }
        let origin = String::from_utf8_lossy(&p[at..at + ol]).into_owned();
        at += ol;
        let hops = p[at];
        at += 1;
        let id = u64::from_le_bytes(p[at..at + 8].try_into().unwrap());
        at += 8;
        let lp = u16::from_le_bytes([p[at], p[at + 1]]);
        at += 2;
        let mut code = [0u8; proto::CODE_LEN];
        code.copy_from_slice(&p[at..at + proto::CODE_LEN]);
        at += proto::CODE_LEN;
        let mut info = [0u8; proto::NETINFO_SIZE];
        info.copy_from_slice(&p[at..at + proto::NETINFO_SIZE]);
        at += proto::NETINFO_SIZE;
        if proto::info_ok(&info) {
            out.push(RawHub { id, link_proto: lp, code, info, origin, hops });
        }
    }
    out
}

/// Lists a room with transitive advertisements (falls back to a plain LIST for an older peer).
pub(super) fn list_ext(
    peer: &mut PeerState,
    room: [u8; 16],
    depth: u8,
    path: &[String],
    name: &str,
) -> io::Result<(Vec<RawHub>, u64)> {
    if !peer.ensure() {
        return Err(protocol(peer.last_error.as_deref().unwrap_or("peer not resolved")));
    }
    let start = Instant::now();
    let timeout = ext_timeout(depth) + Duration::from_millis(300);
    let mut s = connect(peer, timeout)?;
    handshake(&mut s, peer, name, timeout)?;
    let mut req = Vec::with_capacity(16 + 2 + 1 + 1 + path.iter().map(|p| p.len() + 1).sum::<usize>());
    req.extend_from_slice(&room);
    req.extend_from_slice(&proto::LINK_PROTO_VERSION.to_le_bytes());
    req.push(depth);
    req.push(path.len().min(255) as u8);
    for p in path.iter().take(255) {
        let b = p.as_bytes();
        let n = b.len().min(255);
        req.push(n as u8);
        req.extend_from_slice(&b[..n]);
    }
    write_frame(&mut s, proto::T_FED_LIST_EXT, &[&req])?;
    let (t, p) = read_frame(&mut s)?;
    if t == proto::T_ERROR && p.len() >= 4 {
        let code = u32::from_le_bytes(p[..4].try_into().unwrap());
        if code == proto::ERR_BAD_REQ || code == proto::ERR_VERSION {
            return list(peer, room, name); // (a peer that predates advertisements)
        }
        return Err(remote_error(code));
    }
    if t != proto::T_FED_HUBS_EXT {
        return Err(protocol("not an extended hubs answer"));
    }
    let tag = peer_tag(peer);
    let mut hubs = parse_hubs_ext(&p);
    for h in &mut hubs {
        if h.origin.is_empty() {
            h.origin = tag.clone();
            h.hops = 0;
        }
    }
    Ok((hubs, start.elapsed().as_millis() as u64))
}

/// Joins a hub through a chain: the direct peer relays to `origin` within `hop` relays.
pub(super) fn join_ext(
    peer: &mut PeerState,
    room: [u8; 16],
    link_proto: u16,
    origin: &str,
    hub: u64,
    code: Option<&[u8; proto::CODE_LEN]>,
    hops: u8,
    name: &str,
    timeout: Duration,
) -> io::Result<(TcpStream, u32, u64)> {
    if !peer.ensure() {
        return Err(protocol(peer.last_error.as_deref().unwrap_or("peer not resolved")));
    }
    let mut s = connect(peer, timeout)?;
    handshake(&mut s, peer, name, timeout)?;
    let ob = origin.as_bytes();
    let olen = ob.len().min(255);
    let mut req = Vec::with_capacity(16 + 2 + 1 + 1 + olen + 8 + 1 + proto::CODE_LEN);
    req.extend_from_slice(&room);
    req.extend_from_slice(&link_proto.to_le_bytes());
    req.push(hops);
    req.push(olen as u8);
    req.extend_from_slice(&ob[..olen]);
    req.extend_from_slice(&hub.to_le_bytes());
    match code {
        Some(c) => {
            req.push(1);
            req.extend_from_slice(c);
        }
        None => {
            req.push(0);
            req.extend_from_slice(&[0u8; proto::CODE_LEN]);
        }
    }
    write_frame(&mut s, proto::T_FED_JOIN_EXT, &[&req])?;
    let (t, p) = read_frame(&mut s)?;
    if t == proto::T_ERROR && !p.is_empty() {
        return Err(remote_error(u32::from_le_bytes(p[..4].try_into().unwrap_or([0; 4]))));
    }
    if t != proto::T_SPLICED || p.len() < 12 {
        return Err(protocol("join_ext: no spliced answer"));
    }
    let pair = u32::from_le_bytes(p[..4].try_into().unwrap());
    let secret = u64::from_le_bytes(p[4..12].try_into().unwrap());
    Ok((s, pair, secret))
}

/// A transitive query waits for its own hop plus every hop below it.
pub(super) fn ext_timeout(depth: u8) -> Duration {
    (REQUEST_TIMEOUT * (depth as u32 + 2)).min(Duration::from_secs(10))
}

/// Server side: local hubs plus, when depth remains, the advertisements of peers not already in `path`.
pub(super) fn list_ext_hubs(
    shared: &Arc<Mutex<super::Shared>>,
    room: [u8; 16],
    _link_proto: u16,
    depth: u8,
    path: &[String],
) -> Vec<RawHub> {
    let local = super::fed_local_hubs(shared, room);
    let (max_depth, name, self_id, peers, avoid) = {
        let s = shared.lock().unwrap();
        (
            s.cfg.federation_transitive,
            s.cfg.federation_name.clone(),
            s.identity.id.clone(),
            s.fed.peers.clone(),
            s.hubs.keys().copied().collect::<HashSet<u64>>(),
        )
    };
    if depth == 0 || max_depth == 0 || path.iter().any(|p| p == &self_id) {
        return local;
    }
    let depth = depth.min(max_depth);
    let mut next_path = path.to_vec();
    next_path.push(self_id.clone());
    let (tx, rx) = mpsc::channel();
    for (idx, mut peer) in peers.into_iter().enumerate() {
        let tag = peer_tag(&peer);
        if tag == self_id || path.iter().any(|p| p == &tag) {
            continue;
        }
        let tx = tx.clone();
        let name = name.clone();
        let path = next_path.clone();
        thread::spawn(move || {
            let _ = tx.send((idx, list_ext(&mut peer, room, depth - 1, &path, &name)));
        });
    }
    drop(tx);
    let mut out = local;
    let mut seen: HashSet<(String, u64)> = out.iter().map(|h| (h.origin.clone(), h.id)).collect();
    let deadline = Instant::now() + ext_timeout(depth).saturating_sub(Duration::from_millis(300));
    loop {
        let left = deadline.saturating_duration_since(Instant::now());
        if left.is_zero() {
            break;
        }
        match rx.recv_timeout(left) {
            Ok((idx, Ok((hubs, rtt)))) => {
                // Remember what we forward, so a join for a transitive hub can take this hop.
                let tag = {
                    let s = shared.lock().unwrap();
                    s.fed.peers.get(idx).map(peer_tag).unwrap_or_default()
                };
                shared
                    .lock()
                    .unwrap()
                    .fed
                    .ingest(idx, room, Ok(hubs.clone()), rtt, &avoid);
                for mut h in hubs {
                    if h.origin.is_empty() {
                        h.origin = tag.clone();
                    }
                    h.hops = h.hops.saturating_add(1);
                    if h.hops > max_depth {
                        continue;
                    }
                    if seen.insert((h.origin.clone(), h.id)) {
                        out.push(h);
                        if out.len() >= proto::FED_HUBS_MAX {
                            return out;
                        }
                    }
                }
            }
            Ok((_, Err(_))) => {}
            Err(_) => break,
        }
    }
    out
}

/// Fetches `/.well-known/mhgu-federation` over plain HTTP: (server id, endpoint, public key).
fn fetch_well_known(url: &str) -> io::Result<(String, String, [u8; 32])> {
    let rest = url
        .strip_prefix("http://")
        .ok_or_else(|| protocol("only http:// well-known URLs are supported (put TLS in front)"))?;
    let hostport = rest.split('/').next().unwrap_or(rest);
    let addr_text = if hostport.rsplit(':').next().map(|p| p.chars().all(|c| c.is_ascii_digit())).unwrap_or(false) {
        hostport.to_string()
    } else {
        format!("{hostport}:80")
    };
    let addr = addr_text
        .to_socket_addrs()?
        .next()
        .ok_or_else(|| protocol("well-known host does not resolve"))?;
    let mut s = TcpStream::connect_timeout(&addr, Duration::from_secs(3))?;
    s.set_read_timeout(Some(Duration::from_secs(3)))?;
    s.set_write_timeout(Some(Duration::from_secs(3)))?;
    write!(
        s,
        "GET /.well-known/mhgu-federation HTTP/1.1\r\nHost: {hostport}\r\nConnection: close\r\n\r\n"
    )?;
    let mut raw = Vec::new();
    let _ = s.take(64 * 1024).read_to_end(&mut raw);
    let at = raw
        .windows(4)
        .position(|w| w == b"\r\n\r\n")
        .ok_or_else(|| protocol("well-known: no HTTP body"))?;
    let body = String::from_utf8_lossy(&raw[at + 4..]);
    let id = json_field(&body, "server_id").ok_or_else(|| protocol("well-known: no server_id"))?;
    let endpoint = json_field(&body, "endpoint").ok_or_else(|| protocol("well-known: no endpoint"))?;
    let key_hex = json_field(&body, "public_key").ok_or_else(|| protocol("well-known: no public_key"))?;
    let mut pk = [0u8; 32];
    if key_hex.len() != 64 {
        return Err(protocol("well-known: public_key is not 32 bytes of hex"));
    }
    for i in 0..32 {
        pk[i] = u8::from_str_radix(&key_hex[i * 2..i * 2 + 2], 16)
            .map_err(|_| protocol("well-known: public_key is not hex"))?;
    }
    Ok((id, endpoint, pk))
}

fn json_field(json: &str, key: &str) -> Option<String> {
    let pat = format!("\"{key}\"");
    let mut at = json.find(&pat)? + pat.len();
    while json.as_bytes().get(at).map(|b| b.is_ascii_whitespace() || *b == b':')? {
        at += 1;
    }
    let rest = json.get(at..)?;
    let rest = rest.strip_prefix('"')?;
    let end = rest.find('"')?;
    Some(rest[..end].to_string())
}

/// Lists one room on a peer.

pub(super) fn list(peer: &mut PeerState, room: [u8; 16], name: &str) -> io::Result<(Vec<RawHub>, u64)> {
    if !peer.ensure() {
        return Err(protocol(peer.last_error.as_deref().unwrap_or("peer not resolved")));
    }
    let start = Instant::now();
    let mut s = connect(peer, REQUEST_TIMEOUT)?;
    handshake(&mut s, peer, name, REQUEST_TIMEOUT)?;
    write_frame(&mut s, proto::T_FED_LIST, &[&room, &proto::LINK_PROTO_VERSION.to_le_bytes()])?;
    let (t, p) = read_frame(&mut s)?;
    if t == proto::T_ERROR && p.len() >= 4 {
        return Err(remote_error(u32::from_le_bytes(p[..4].try_into().unwrap())));
    }
    if t != proto::T_FED_HUBS {
        return Err(protocol("not a federation hubs answer"));
    }
    Ok((parse_hubs(&p), start.elapsed().as_millis() as u64))
}

/// Finds a session code on a peer (any of its hubs, whatever the room).
pub(super) fn find(peer: &mut PeerState, code: &[u8; proto::CODE_LEN], name: &str) -> io::Result<Vec<RawHub>> {
    if !peer.ensure() {
        return Err(protocol(peer.last_error.as_deref().unwrap_or("peer not resolved")));
    }
    let mut s = connect(peer, REQUEST_TIMEOUT)?;
    handshake(&mut s, peer, name, REQUEST_TIMEOUT)?;
    write_frame(&mut s, proto::T_FED_FIND, &[code])?;
    let (t, p) = read_frame(&mut s)?;
    if t == proto::T_ERROR && p.len() >= 4 {
        return Err(remote_error(u32::from_le_bytes(p[..4].try_into().unwrap())));
    }
    if t != proto::T_FED_HUBS {
        return Err(protocol("not a federation hubs answer"));
    }
    Ok(parse_hubs(&p))
}

/// FIND with transitive advertisements: the peer may ask its own peers, up to `depth` extra hops.
pub(super) fn find_ext(
    peer: &mut PeerState,
    code: &[u8; proto::CODE_LEN],
    depth: u8,
    path: &[String],
    name: &str,
) -> io::Result<(Vec<RawHub>, u64)> {
    if !peer.ensure() {
        return Err(protocol(peer.last_error.as_deref().unwrap_or("peer not resolved")));
    }
    let start = Instant::now();
    let timeout = ext_timeout(depth) + Duration::from_millis(300);
    let mut s = connect(peer, timeout)?;
    handshake(&mut s, peer, name, timeout)?;
    let mut req = Vec::with_capacity(8 + 1 + 1 + path.iter().map(|p| p.len() + 1).sum::<usize>());
    req.extend_from_slice(code);
    req.push(depth);
    req.push(path.len().min(255) as u8);
    for p in path.iter().take(255) {
        let b = p.as_bytes();
        let n = b.len().min(255);
        req.push(n as u8);
        req.extend_from_slice(&b[..n]);
    }
    write_frame(&mut s, proto::T_FED_FIND_EXT, &[&req])?;
    let (t, p) = read_frame(&mut s)?;
    if t == proto::T_ERROR && p.len() >= 4 {
        let err = u32::from_le_bytes(p[..4].try_into().unwrap());
        if err == proto::ERR_BAD_REQ || err == proto::ERR_VERSION {
            return find(peer, code, name).map(|hubs| (hubs, start.elapsed().as_millis() as u64));
        }
        return Err(remote_error(err));
    }
    if t != proto::T_FED_HUBS_EXT {
        return Err(protocol("not an extended find answer"));
    }
    let tag = peer_tag(peer);
    let mut hubs = parse_hubs_ext(&p);
    for h in &mut hubs {
        if h.origin.is_empty() {
            h.origin = tag.clone();
            h.hops = 0;
        }
    }
    Ok((hubs, start.elapsed().as_millis() as u64))
}

/// Server side: a local session code plus matching hubs from peers not already in `path`.
pub(super) fn find_ext_hubs(
    shared: &Arc<Mutex<super::Shared>>,
    code: &[u8; proto::CODE_LEN],
    depth: u8,
    path: &[String],
) -> Vec<RawHub> {
    let local = super::fed_local_code(shared, code);
    let (max_depth, name, self_id, peers, avoid) = {
        let s = shared.lock().unwrap();
        (
            s.cfg.federation_transitive,
            s.cfg.federation_name.clone(),
            s.identity.id.clone(),
            s.fed.peers.clone(),
            s.hubs.keys().copied().collect::<HashSet<u64>>(),
        )
    };
    if depth == 0 || max_depth == 0 || path.iter().any(|p| p == &self_id) {
        return local;
    }
    let depth = depth.min(max_depth);
    let mut next_path = path.to_vec();
    next_path.push(self_id.clone());
    let (tx, rx) = mpsc::channel();
    for (idx, mut peer) in peers.into_iter().enumerate() {
        let tag = peer_tag(&peer);
        if tag == self_id || path.iter().any(|p| p == &tag) {
            continue;
        }
        let tx = tx.clone();
        let name = name.clone();
        let path = next_path.clone();
        let code = *code;
        thread::spawn(move || {
            let _ = tx.send((idx, find_ext(&mut peer, &code, depth - 1, &path, &name)));
        });
    }
    drop(tx);
    let deadline = Instant::now() + ext_timeout(depth).saturating_sub(Duration::from_millis(300));
    let mut out = local;
    let mut seen: HashSet<(String, u64)> = out.iter().map(|h| (h.origin.clone(), h.id)).collect();
    loop {
        let left = deadline.saturating_duration_since(Instant::now());
        if left.is_zero() {
            break;
        }
        match rx.recv_timeout(left) {
            Ok((idx, Ok((hubs, _)))) => {
                let tag = {
                    let s = shared.lock().unwrap();
                    s.fed.peers.get(idx).map(peer_tag).unwrap_or_default()
                };
                shared.lock().unwrap().fed.ingest_find(idx, hubs.clone(), &avoid);
                for mut h in hubs {
                    if h.origin.is_empty() {
                        h.origin = tag.clone();
                    }
                    h.hops = h.hops.saturating_add(1);
                    if h.hops > max_depth {
                        continue;
                    }
                    if seen.insert((h.origin.clone(), h.id)) {
                        out.push(h);
                        if out.len() >= proto::FED_HUBS_MAX {
                            return out;
                        }
                    }
                }
            }
            Ok((_, Err(_))) => {}
            Err(_) => break,
        }
    }
    out
}

/// Joins a hub on a peer: the returned stream carries the in-game link after `SPLICED`, and the pair and
/// secret are the peer's relay credentials for this leg.
pub(super) fn join(
    peer: &mut PeerState,
    room: [u8; 16],
    link_proto: u16,
    hub: u64,
    code: Option<&[u8; proto::CODE_LEN]>,
    name: &str,
    timeout: Duration,
) -> io::Result<(TcpStream, u32, u64)> {
    if !peer.ensure() {
        return Err(protocol(peer.last_error.as_deref().unwrap_or("peer not resolved")));
    }
    let mut s = connect(peer, REQUEST_TIMEOUT)?;
    handshake(&mut s, peer, name, REQUEST_TIMEOUT)?;
    let has = code.is_some() as u8;
    let c = code.copied().unwrap_or([0u8; proto::CODE_LEN]);
    write_frame(
        &mut s,
        proto::T_FED_JOIN,
        &[&room, &link_proto.to_le_bytes(), &hub.to_le_bytes(), &[has], &c],
    )?;
    s.set_read_timeout(Some(timeout))?;
    let (t, p) = read_frame(&mut s)?;
    if t == proto::T_ERROR && p.len() >= 4 {
        return Err(remote_error(u32::from_le_bytes(p[..4].try_into().unwrap())));
    }
    if t != proto::T_SPLICED || p.len() < 12 {
        return Err(protocol("not a federation spliced answer"));
    }
    let pair = u32::from_le_bytes(p[..4].try_into().unwrap());
    let secret = u64::from_le_bytes(p[4..12].try_into().unwrap());
    Ok((s, pair, secret))
}

/// One federated player leg's UDP bridge: this server's pair to the player and the peer's pair to the
/// host, with the prefix rewritten between them.
pub(super) struct Bridge {
    pub local: u32,
    pub local_secret: u64,
    /// The "player" side is another hub server (a transitive join), so downstream datagrams keep a prefix.
    pub player_is_peer: bool,
    pub remote: u32,
    pub remote_secret: u64,
    pub peer: usize,
    pub peer_udp: SocketAddr,
    pub player: Mutex<Option<SocketAddr>>,
    pub up: TokenBucket,
    pub down: TokenBucket,
    pub bytes_up: AtomicU64,
    pub bytes_down: AtomicU64,
    pub created: Instant,
}

impl Bridge {
    pub(super) fn new(
        local: u32,
        local_secret: u64,
        remote: u32,
        remote_secret: u64,
        peer: usize,
        peer_udp: SocketAddr,
        bandwidth: f64,
        player_is_peer: bool,
    ) -> Bridge {
        Bridge {
            local,
            local_secret,
            player_is_peer,
            remote,
            remote_secret,
            peer,
            peer_udp,
            player: Mutex::new(None),
            up: TokenBucket::new(bandwidth, bandwidth),
            down: TokenBucket::new(bandwidth, bandwidth),
            bytes_up: AtomicU64::new(0),
            bytes_down: AtomicU64::new(0),
            created: Instant::now(),
        }
    }
}

fn beacon_bytes(identity: &Identity, endpoint: &str, name: &str) -> Vec<u8> {
    let id = identity.id.as_bytes();
    let ep = endpoint.as_bytes();
    let nm = name.as_bytes().get(..64).unwrap_or(name.as_bytes());
    let mut p = Vec::with_capacity(8 + 2 + 1 + id.len() + 32 + 1 + ep.len() + 1 + nm.len() + 64);
    p.extend_from_slice(DISCOVERY_MAGIC);
    p.extend_from_slice(&1u16.to_le_bytes());
    p.push(id.len() as u8);
    p.extend_from_slice(id);
    p.extend_from_slice(&identity.public);
    p.push(ep.len() as u8);
    p.extend_from_slice(ep);
    p.push(nm.len() as u8);
    p.extend_from_slice(nm);
    let sig = identity.sign(&[&p]);
    p.extend_from_slice(&sig);
    p
}

/// Parses and verifies a beacon: (id, public key, endpoint, name).
fn parse_beacon(b: &[u8]) -> Option<(String, [u8; 32], String, String)> {
    if b.len() < 8 + 2 + 1 + 1 + 32 + 1 + 1 + 64 || &b[..8] != DISCOVERY_MAGIC {
        return None;
    }
    if u16::from_le_bytes([b[8], b[9]]) != 1 {
        return None;
    }
    let mut at = 10;
    let il = *b.get(at)? as usize;
    at += 1;
    let id = String::from_utf8_lossy(b.get(at..at + il)?).into_owned();
    at += il;
    let mut pk = [0u8; 32];
    pk.copy_from_slice(b.get(at..at + 32)?);
    at += 32;
    let el = *b.get(at)? as usize;
    at += 1;
    let endpoint = String::from_utf8_lossy(b.get(at..at + el)?).into_owned();
    at += el;
    let nl = *b.get(at)? as usize;
    at += 1;
    let name = String::from_utf8_lossy(b.get(at..at + nl)?).into_owned();
    at += nl;
    let sig: [u8; 64] = b.get(at..at + 64)?.try_into().ok()?;
    if identity_id(&pk) != id || !Identity::verify(&pk, &[&b[..at]], &sig) {
        return None;
    }
    Some((id, pk, endpoint, name))
}

/// A candidate's TCP/UDP address: its advertised host, or the packet's source when it advertises a wildcard.
fn candidate_addr(endpoint: &str, from: SocketAddr) -> Option<SocketAddr> {
    let (host, port) = match endpoint.rsplit_once(':') {
        Some((h, p)) => (h.trim_matches(['[', ']']), p.parse().ok()?),
        None => (endpoint, 39510),
    };
    let ip = if host.is_empty() || host == "0.0.0.0" || host == "::" {
        match from.ip() {
            IpAddr::V4(v4) => IpAddr::V4(v4),
            IpAddr::V6(v6) => IpAddr::V6(v6),
        }
    } else {
        host.parse().ok()?
    };
    Some(SocketAddr::new(ip, port))
}

/// LAN discovery thread: announces us and keeps verified peers as candidates. It never adds a peer.
pub(super) fn discovery(shared: Arc<Mutex<super::Shared>>) {
    let socket = match std::net::UdpSocket::bind(("0.0.0.0", DISCOVERY_PORT)) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("[fed] LAN discovery off: UDP {DISCOVERY_PORT} is in use ({e})");
            return;
        }
    };
    let _ = socket.set_broadcast(true);
    let _ = socket.set_read_timeout(Some(Duration::from_millis(500)));
    let mut next = Instant::now();
    let mut buf = [0u8; 1024];
    loop {
        let now = Instant::now();
        if now >= next {
            next = now + DISCOVERY_EVERY;
            let (beacon, our_id) = {
                let s = shared.lock().unwrap();
                let endpoint = if s.cfg.advertise.is_empty() {
                    if s.cfg.addr.starts_with(':') {
                        format!("0.0.0.0{}", s.cfg.addr)
                    } else {
                        s.cfg.addr.clone()
                    }
                } else {
                    s.cfg.advertise.clone()
                };
                let name = if s.cfg.federation_name.is_empty() {
                    s.identity.id.clone()
                } else {
                    s.cfg.federation_name.clone()
                };
                (beacon_bytes(&s.identity, &endpoint, &name), s.identity.id.clone())
            };
            let _ = socket.send_to(&beacon, ("255.255.255.255", DISCOVERY_PORT));
            let _ = socket.send_to(&beacon, ("127.0.0.1", DISCOVERY_PORT));
            let _ = our_id;
        }
        match socket.recv_from(&mut buf) {
            Ok((n, from)) => {
                if let Some((id, pk, endpoint, name)) = parse_beacon(&buf[..n]) {
                    if let Some(addr) = candidate_addr(&endpoint, from) {
                        let mut s = shared.lock().unwrap();
                        if id != s.identity.id {
                            let new = !s.fed.candidates.contains_key(&id);
                            let name = if name.is_empty() { id.clone() } else { name };
                            s.fed.candidate(id.clone(), name, addr.to_string(), pk);
                            if new && s.live.verbose {
                                s.note(format!("federation: discovered candidate {id} at {addr}"));
                            }
                        }
                    }
                }
            }
            Err(_) => {}
        }
    }
}

/// Loads admin-page approvals (`federation_state`) into the configuration.
pub(super) fn load_state(cfg: &mut crate::config::Config) {
    if cfg.federation_state.is_empty() {
        return;
    }
    let text = match std::fs::read_to_string(&cfg.federation_state) {
        Ok(t) => t,
        Err(e) if e.kind() == io::ErrorKind::NotFound => return,
        Err(e) => {
            eprintln!("[fed] {}: {e}", cfg.federation_state);
            return;
        }
    };
    for line in text.lines() {
        let line = line.trim();
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        let mut it = line.splitn(4, char::is_whitespace);
        let kind = it.next().unwrap_or("");
        let id = it.next().unwrap_or("").trim();
        if !id.starts_with(proto::FED_ID_PREFIX) || id.len() < proto::FED_ID_PREFIX.len() + 4 {
            continue;
        }
        match kind {
            "allow" => {
                if !cfg.federation_allow.iter().any(|a| a == id) {
                    cfg.federation_allow.push(id.to_string());
                }
            }
            "peer" => {
                let addr = it.next().unwrap_or("").trim();
                if addr.is_empty() {
                    continue;
                }
                let name = it.next().unwrap_or("").trim().to_string();
                if cfg
                    .federate
                    .iter()
                    .any(|p| p.key == id || p.addr == addr)
                {
                    continue;
                }
                cfg.federate.push(Peer {
                    addr: addr.to_string(),
                    token: String::new(),
                    name,
                    key: id.to_string(),
                });
            }
            _ => {}
        }
    }
}

/// Writes the current allowlist and key-based peers (tokens are never written).
pub(super) fn save_state(cfg: &crate::config::Config) -> io::Result<()> {
    if cfg.federation_state.is_empty() {
        return Ok(());
    }
    let mut out = String::from("# mhgu-hub federation approvals (edited by the admin page; one id per line)\n");
    let mut seen = HashSet::new();
    for id in &cfg.federation_allow {
        if seen.insert(id.clone()) {
            out.push_str(&format!("allow {id}\n"));
        }
    }
    let mut peers = HashSet::new();
    for p in &cfg.federate {
        if p.key.starts_with(proto::FED_ID_PREFIX) && peers.insert(p.key.clone()) {
            out.push_str(&format!("peer {} {} {}\n", p.key, p.addr, p.name));
        }
    }
    let tmp = format!("{}.new", cfg.federation_state);
    std::fs::write(&tmp, out)?;
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        let _ = std::fs::set_permissions(&tmp, std::fs::Permissions::from_mode(0o600));
    }
    std::fs::rename(tmp, &cfg.federation_state)
}

/// A peer's snapshot for the web UI.
pub(super) struct PeerView {
    pub name: String,
    pub addr: String,
    pub online: bool,
    pub rtt_ms: u64,
    pub hubs: usize,
    pub last_error: Option<String>,
}

impl FedState {
    pub(super) fn views(&self) -> Vec<PeerView> {
        self.peers
            .iter()
            .map(|p| PeerView {
                name: p.name.clone(),
                addr: p.cfg.addr.clone(),
                online: p.last_error.is_none() && p.last_ok.is_some(),
                rtt_ms: p.rtt_ms,
                hubs: p.hubs,
                last_error: p.last_error.clone(),
            })
            .collect()
    }
}

/// `MHGF` server side: authenticates a peer and runs one request. `Ok(true)` means the connection became
/// a player link (JOIN); the caller must not write to it any more.
pub(super) fn serve_peer(
    mut c: TcpStream,
    shared: &Arc<Mutex<super::Shared>>,
    ip: IpAddr,
    t: u8,
    p: &[u8],
) -> io::Result<()> {
    let token = shared.lock().unwrap().cfg.federation_token.clone();
    if t != proto::T_FED_HELLO || p.len() < 7 + proto::FED_NONCE || p[..4] != proto::FED_MAGIC {
        let _ = super::send_error(&mut c, proto::ERR_BAD_REQ);
        return Ok(());
    }
    if u16::from_le_bytes([p[4], p[5]]) != proto::FED_VERSION {
        let _ = super::send_error(&mut c, proto::ERR_VERSION);
        return Ok(());
    }
    if token.is_empty() {
        let _ = super::send_error(&mut c, proto::ERR_FED_AUTH);
        return Ok(());
    }
    let nl = p[6] as usize;
    if p.len() < 7 + nl + proto::FED_NONCE {
        let _ = super::send_error(&mut c, proto::ERR_BAD_REQ);
        return Ok(());
    }
    let client = &p[7 + nl..7 + nl + proto::FED_NONCE];
    let server = nonce();
    write_frame(&mut c, proto::T_FED_CHALLENGE, &[&server])?;
    let (at, ap) = read_frame(&mut c)?;
    if at != proto::T_FED_AUTH || ap.len() < proto::FED_MAC {
        let _ = super::send_error(&mut c, proto::ERR_FED_AUTH);
        return Ok(());
    }
    let want = proto::hmac_sha256(token.as_bytes(), &[proto::FED_AUTH_SALT, client, &server]);
    if !proto::mac_eq(&ap[..proto::FED_MAC], &want) {
        let _ = super::send_error(&mut c, proto::ERR_FED_AUTH);
        return Ok(());
    }
    write_frame(&mut c, proto::T_FED_OK, &[])?;
    let peer_name = String::from_utf8_lossy(&p[7..7 + nl]).into_owned();
    if !peer_name.is_empty() {
        let mut s = shared.lock().unwrap();
        if s.live.verbose {
            s.note(format!("federation: {peer_name} authenticated from {ip}"));
        }
    }
    let peer_tag = if peer_name.is_empty() { ip.to_string() } else { peer_name };
    serve_requests(c, shared, ip, &peer_tag)
}

/// Ed25519 handshake (federation version 2): both servers prove the key behind their server id.
pub(super) fn serve_peer2(
    mut c: TcpStream,
    shared: &Arc<Mutex<super::Shared>>,
    ip: IpAddr,
    t: u8,
    p: &[u8],
) -> io::Result<()> {
    if t != proto::T_FED_HELLO2 || p.len() < 4 + 2 + 1 + 32 + 1 + proto::FED_NONCE || p[..4] != proto::FED_MAGIC {
        let _ = super::send_error(&mut c, proto::ERR_BAD_REQ);
        return Ok(());
    }
    if u16::from_le_bytes([p[4], p[5]]) != 2 {
        let _ = super::send_error(&mut c, proto::ERR_VERSION);
        return Ok(());
    }
    let il = p[6] as usize;
    let mut at = 7;
    if p.len() < at + il + 32 + 1 + proto::FED_NONCE {
        let _ = super::send_error(&mut c, proto::ERR_BAD_REQ);
        return Ok(());
    }
    let peer_id = String::from_utf8_lossy(&p[at..at + il]).into_owned();
    at += il;
    let mut peer_pk = [0u8; 32];
    peer_pk.copy_from_slice(&p[at..at + 32]);
    at += 32;
    let nl = p[at] as usize;
    at += 1;
    if p.len() < at + nl + proto::FED_NONCE {
        let _ = super::send_error(&mut c, proto::ERR_BAD_REQ);
        return Ok(());
    }
    let peer_name = String::from_utf8_lossy(&p[at..at + nl]).into_owned();
    at += nl;
    let peer_nonce = &p[at..at + proto::FED_NONCE];
    if peer_id != identity_id(&peer_pk) {
        let _ = super::send_error(&mut c, proto::ERR_FED_AUTH);
        return Ok(());
    }
    let (identity, allow) = {
        let s = shared.lock().unwrap();
        (Arc::clone(&s.identity), s.cfg.federation_allow.clone())
    };
    if allow.is_empty() || !(allow.iter().any(|a| a == "*") || allow.iter().any(|a| a == &peer_id)) {
        let mut s = shared.lock().unwrap();
        s.note(format!("federation: refused identity {peer_id} (not in -federation-allow)"));
        let _ = super::send_error(&mut c, proto::ERR_FED_AUTH);
        return Ok(());
    }
    let server_nonce = nonce();
    let parts = auth2_parts(
        &identity.public,
        &peer_pk,
        peer_nonce,
        &server_nonce,
        identity.id.as_bytes(),
        peer_id.as_bytes(),
    );
    let sig = identity.sign(&parts);
    let mut challenge = Vec::with_capacity(1 + identity.id.len() + 32 + proto::FED_NONCE + 64);
    challenge.push(identity.id.len() as u8);
    challenge.extend_from_slice(identity.id.as_bytes());
    challenge.extend_from_slice(&identity.public);
    challenge.extend_from_slice(&server_nonce);
    challenge.extend_from_slice(&sig);
    write_frame(&mut c, proto::T_FED_CHALLENGE2, &[&challenge])?;
    let (at, ap) = read_frame(&mut c)?;
    if at != proto::T_FED_AUTH2 || ap.len() < 64 {
        let _ = super::send_error(&mut c, proto::ERR_FED_AUTH);
        return Ok(());
    }
    let mut sig_a = [0u8; 64];
    sig_a.copy_from_slice(&ap[..64]);
    let parts = auth2_parts(
        &identity.public,
        &peer_pk,
        peer_nonce,
        &server_nonce,
        identity.id.as_bytes(),
        peer_id.as_bytes(),
    );
    if !Identity::verify(&peer_pk, &parts, &sig_a) {
        let _ = super::send_error(&mut c, proto::ERR_FED_AUTH);
        return Ok(());
    }
    write_frame(&mut c, proto::T_FED_OK, &[])?;
    if !peer_name.is_empty() {
        let mut s = shared.lock().unwrap();
        if s.live.verbose {
            s.note(format!("federation: {peer_name} ({peer_id}) authenticated from {ip}"));
        }
    }
    serve_requests(c, shared, ip, &peer_id)
}

/// Authed federation requests (LIST/FIND/JOIN), shared by both handshakes.
fn serve_requests(mut c: TcpStream, shared: &Arc<Mutex<super::Shared>>, ip: IpAddr, peer_tag: &str) -> io::Result<()> {
    let (t, p) = read_frame(&mut c)?;
    match t {
        proto::T_FED_PRESENCE => {
            let enabled = shared.lock().unwrap().cfg.federation_presence;
            if !enabled {
                let _ = super::send_error(&mut c, proto::ERR_BAD_REQ);
                return Ok(());
            }
            if !p.is_empty() {
                let _ = super::send_error(&mut c, proto::ERR_BAD_REQ);
                return Ok(());
            }
            let gen = {
                let mut s = shared.lock().unwrap();
                s.fed.presence_gen += 1;
                s.fed.presence_gen
            };
            if write_frame(&mut c, proto::T_FED_OK, &[]).is_err() {
                return Ok(());
            }
            return presence_session(shared, c, gen, false, 0, peer_tag.to_string(), peer_tag.to_string());
        }
        proto::T_FED_PRESENCE_EXT => {
            let enabled = shared.lock().unwrap().cfg.federation_presence;
            if !enabled {
                let _ = super::send_error(&mut c, proto::ERR_BAD_REQ);
                return Ok(());
            }
            if p.len() < 2 {
                let _ = super::send_error(&mut c, proto::ERR_BAD_REQ);
                return Ok(());
            }
            let depth = p[0];
            let rl = p[1] as usize;
            if p.len() < 2 + rl {
                let _ = super::send_error(&mut c, proto::ERR_BAD_REQ);
                return Ok(());
            }
            let requester = String::from_utf8_lossy(&p[2..2 + rl]).into_owned();
            let gen = {
                let mut s = shared.lock().unwrap();
                s.fed.presence_gen += 1;
                s.fed.presence_gen
            };
            if write_frame(&mut c, proto::T_FED_OK, &[]).is_err() {
                return Ok(());
            }
            return presence_session(shared, c, gen, true, depth, requester, peer_tag.to_string());
        }
        proto::T_FED_LIST => {
            if p.len() < 16 {
                let _ = super::send_error(&mut c, proto::ERR_BAD_REQ);
                return Ok(());
            }
            let mut room = [0u8; 16];
            room.copy_from_slice(&p[..16]);
            if !super::allow_request(shared, ip) {
                let _ = super::send_error(&mut c, proto::ERR_BUSY);
                return Ok(());
            }
            let entries = super::fed_local_hubs(shared, room);
            let payload = encode_hubs(&entries);
            write_frame(&mut c, proto::T_FED_HUBS, &[&payload])?;
            Ok(())
        }
        proto::T_FED_LIST_EXT => {
            if p.len() < 20 {
                let _ = super::send_error(&mut c, proto::ERR_BAD_REQ);
                return Ok(());
            }
            if !super::allow_request(shared, ip) {
                let _ = super::send_error(&mut c, proto::ERR_BUSY);
                return Ok(());
            }
            let mut room = [0u8; 16];
            room.copy_from_slice(&p[..16]);
            let lp = u16::from_le_bytes([p[16], p[17]]);
            let depth = p[18];
            let n = p[19] as usize;
            let mut at = 20;
            let mut path = Vec::new();
            for _ in 0..n {
                if at >= p.len() {
                    let _ = super::send_error(&mut c, proto::ERR_BAD_REQ);
                    return Ok(());
                }
                let l = p[at] as usize;
                at += 1;
                if at + l > p.len() {
                    let _ = super::send_error(&mut c, proto::ERR_BAD_REQ);
                    return Ok(());
                }
                path.push(String::from_utf8_lossy(&p[at..at + l]).into_owned());
                at += l;
            }
            let entries = list_ext_hubs(shared, room, lp, depth, &path);
            let payload = encode_hubs_ext(&entries);
            write_frame(&mut c, proto::T_FED_HUBS_EXT, &[&payload])?;
            Ok(())
        }
        proto::T_FED_JOIN_EXT => {
            if p.len() < 20 + 8 + 1 + proto::CODE_LEN {
                let _ = super::send_error(&mut c, proto::ERR_BAD_REQ);
                return Ok(());
            }
            if !super::allow_request(shared, ip) {
                let _ = super::send_error(&mut c, proto::ERR_BUSY);
                return Ok(());
            }
            let mut room = [0u8; 16];
            room.copy_from_slice(&p[..16]);
            let lp = u16::from_le_bytes([p[16], p[17]]);
            let hops = p[18];
            let olen = p[19] as usize;
            let mut at = 20;
            if at + olen + 8 + 1 + proto::CODE_LEN > p.len() {
                let _ = super::send_error(&mut c, proto::ERR_BAD_REQ);
                return Ok(());
            }
            let origin = String::from_utf8_lossy(&p[at..at + olen]).into_owned();
            at += olen;
            let hub_id = u64::from_le_bytes(p[at..at + 8].try_into().unwrap());
            at += 8;
            let has = p[at] != 0;
            at += 1;
            let code = if has { Some(&p[at..at + proto::CODE_LEN]) } else { None };
            let (self_id, max_hops) = {
                let s = shared.lock().unwrap();
                (s.identity.id.clone(), s.cfg.federation_transitive)
            };
            if origin == self_id {
                super::join(c, shared, lp, room, hub_id, code, true);
                return Ok(());
            }
            if hops == 0 || hops > max_hops {
                let _ = super::send_error(&mut c, proto::ERR_NOT_FOUND);
                return Ok(());
            }
            let mut entry = shared.lock().unwrap().fed.by_origin(&origin, hub_id);
            if entry.is_none() {
                let (depth, self_id) = {
                    let s = shared.lock().unwrap();
                    (s.cfg.federation_transitive, s.identity.id.clone())
                };
                if depth > 0 {
                    list_ext_hubs(shared, room, lp, depth, &[self_id.clone()]);
                    entry = shared.lock().unwrap().fed.by_origin(&origin, hub_id);
                    if entry.is_none() {
                        if let Some(k) = code.filter(|k| k.len() >= proto::CODE_LEN) {
                            let mut c8 = [0u8; proto::CODE_LEN];
                            c8.copy_from_slice(&k[..proto::CODE_LEN]);
                            find_ext_hubs(shared, &c8, depth, &[self_id]);
                            entry = shared.lock().unwrap().fed.by_origin(&origin, hub_id);
                        }
                    }
                }
            }
            match entry {
                Some(hub) => super::federated_join(c, shared, hub, room, lp, code, hops - 1, true),
                None => {
                    let _ = super::send_error(&mut c, proto::ERR_NOT_FOUND);
                }
            }
            Ok(())
        }
        proto::T_FED_FIND_EXT => {
            if p.len() < 10 {
                let _ = super::send_error(&mut c, proto::ERR_BAD_REQ);
                return Ok(());
            }
            if !super::allow_request(shared, ip) {
                let _ = super::send_error(&mut c, proto::ERR_BUSY);
                return Ok(());
            }
            let mut code = [0u8; proto::CODE_LEN];
            code.copy_from_slice(&p[..proto::CODE_LEN]);
            let depth = p[proto::CODE_LEN];
            let n = p[proto::CODE_LEN + 1] as usize;
            let mut at = proto::CODE_LEN + 2;
            let mut path = Vec::new();
            for _ in 0..n {
                if at >= p.len() {
                    let _ = super::send_error(&mut c, proto::ERR_BAD_REQ);
                    return Ok(());
                }
                let l = p[at] as usize;
                at += 1;
                if at + l > p.len() {
                    let _ = super::send_error(&mut c, proto::ERR_BAD_REQ);
                    return Ok(());
                }
                path.push(String::from_utf8_lossy(&p[at..at + l]).into_owned());
                at += l;
            }
            let entries = find_ext_hubs(shared, &code, depth, &path);
            let payload = encode_hubs_ext(&entries);
            write_frame(&mut c, proto::T_FED_HUBS_EXT, &[&payload])?;
            Ok(())
        }
        proto::T_FED_FIND => {
            if p.len() < proto::CODE_LEN {
                let _ = super::send_error(&mut c, proto::ERR_BAD_REQ);
                return Ok(());
            }
            if !super::allow_request(shared, ip) {
                let _ = super::send_error(&mut c, proto::ERR_BUSY);
                return Ok(());
            }
            let mut code = [0u8; proto::CODE_LEN];
            code.copy_from_slice(&p[..proto::CODE_LEN]);
            let entries = super::fed_local_code(shared, &code);
            let payload = encode_hubs(&entries);
            write_frame(&mut c, proto::T_FED_HUBS, &[&payload])?;
            Ok(())
        }
        proto::T_FED_JOIN => {
            if p.len() < 27 {
                let _ = super::send_error(&mut c, proto::ERR_BAD_REQ);
                return Ok(());
            }
            if !super::allow_request(shared, ip) {
                let _ = super::send_error(&mut c, proto::ERR_BUSY);
                return Ok(());
            }
            let mut room = [0u8; 16];
            room.copy_from_slice(&p[..16]);
            let lp = u16::from_le_bytes([p[16], p[17]]);
            let hub = u64::from_le_bytes(p[18..26].try_into().unwrap());
            let has = p[26] != 0;
            let code = if has && p.len() >= 27 + proto::CODE_LEN {
                Some(&p[27..27 + proto::CODE_LEN])
            } else {
                None
            };
            // The id may be a local hub, or this server's alias for a hub it learned from a peer (FIND).
            let (local, remote) = {
                let s = shared.lock().unwrap();
                (s.hubs.contains_key(&hub), s.fed.get(hub))
            };
            if local {
                super::join(c, shared, lp, room, hub, code, true);
            } else if let Some(h) = remote {
                let hops = h.hops;
                super::federated_join(c, shared, h, room, lp, code, hops, true);
            } else {
                let _ = super::send_error(&mut c, proto::ERR_NOT_FOUND);
            }
            Ok(())
        }
        _ => {
            let _ = super::send_error(&mut c, proto::ERR_BAD_REQ);
            Ok(())
        }
    }
}


/// Keeps one outbound presence stream per configured peer (new approvals get one within 5 seconds).
pub(super) fn presence_supervisor(shared: Arc<Mutex<super::Shared>>) {
    let mut started = HashSet::new();
    loop {
        let enabled = shared.lock().unwrap().cfg.federation_presence;
        if !enabled {
            return;
        }
        let n = shared.lock().unwrap().fed.peers.len();
        for idx in 0..n {
            if started.insert(idx) {
                let shared = Arc::clone(&shared);
                std::thread::spawn(move || presence_link(shared, idx));
            }
        }
        std::thread::sleep(Duration::from_secs(5));
    }
}

fn presence_link(shared: Arc<Mutex<super::Shared>>, idx: usize) {
    loop {
        let enabled = shared.lock().unwrap().cfg.federation_presence;
        if !enabled {
            return;
        }
        let mut peer = match shared.lock().unwrap().fed.peers.get(idx).cloned() {
            Some(p) => p,
            None => return,
        };
        if peer.ensure() {
            if let Err(e) = run_presence(&shared, &mut peer) {
                eprintln!("[fed] presence link {}: {e}", peer.name);
            }
        }
        std::thread::sleep(Duration::from_secs(10));
    }
}

fn run_presence(shared: &Arc<Mutex<super::Shared>>, peer: &mut PeerState) -> io::Result<()> {
    let (name, transitive, self_id) = {
        let s = shared.lock().unwrap();
        (s.cfg.federation_name.clone(), s.cfg.federation_transitive, s.identity.id.clone())
    };
    // Try the extended stream even at depth 0: it carries tombstones for players who stop polling.
    let mut ext = true;
    let mut s = connect(peer, REQUEST_TIMEOUT)?;
    handshake(&mut s, peer, &name, REQUEST_TIMEOUT)?;
    if ext {
        let id = self_id.as_bytes();
        let n = id.len().min(255);
        write_frame(
            &mut s,
            proto::T_FED_PRESENCE_EXT,
            &[&[transitive, n as u8], &id[..n]],
        )?;
    } else {
        write_frame(&mut s, proto::T_FED_PRESENCE, &[])?;
    }
    let (t, p) = read_frame(&mut s)?;
    if ext && t == proto::T_ERROR && p.len() >= 4 {
        let code = u32::from_le_bytes(p[..4].try_into().unwrap());
        if code == proto::ERR_BAD_REQ || code == proto::ERR_VERSION {
            // Older server: reconnect and use the plain stream (direct players only).
            ext = false;
            s = connect(peer, REQUEST_TIMEOUT)?;
            handshake(&mut s, peer, &name, REQUEST_TIMEOUT)?;
            write_frame(&mut s, proto::T_FED_PRESENCE, &[])?;
            let (t2, p2) = read_frame(&mut s)?;
            if t2 == proto::T_ERROR && p2.len() >= 4 {
                return Err(remote_error(u32::from_le_bytes(p2[..4].try_into().unwrap())));
            }
            if t2 != proto::T_FED_OK {
                return Err(protocol("peer does not accept presence"));
            }
        } else {
            return Err(remote_error(code));
        }
    } else if !ext && t == proto::T_ERROR && p.len() >= 4 {
        return Err(remote_error(u32::from_le_bytes(p[..4].try_into().unwrap())));
    } else if t != proto::T_FED_OK {
        return Err(protocol("peer does not accept presence"));
    }
    let gen = {
        let mut sh = shared.lock().unwrap();
        sh.fed.presence_gen += 1;
        sh.fed.presence_gen
    };
    let tag = peer_tag(peer);
    presence_session(
        shared,
        s,
        gen,
        ext,
        if ext { transitive } else { 0 },
        tag.clone(),
        tag,
    )
}

/// A tombstone for a remote player that is no longer online at its origin.
fn remote_tombstone(id: [u8; proto::FRIEND_LEN], origin: String, hops: u8) -> PresenceUpdate {
    PresenceUpdate {
        id,
        state: 0,
        code: [0u8; proto::CODE_LEN],
        friends: Vec::new(),
        removed: Vec::new(),
        name: String::new(),
        origin,
        hops,
        gone: true,
    }
}

/// Passes a remote update on to extended streams that still have hop budget and did not send it.
pub(super) fn forward_remote(sh: &super::Shared, u: &PresenceUpdate, source: &str) {
    if u.origin.is_empty() || u.origin == sh.identity.id {
        return;
    }
    let max = sh.cfg.federation_transitive;
    if max == 0 {
        return;
    }
    for (_, tx, sub) in &sh.presence_subs {
        if !sub.ext
            || sub.requester == u.origin
            || sub.requester == source
            || sub.source_tag == source
            || sub.source_tag == u.origin
        {
            continue;
        }
        let hops = u.hops.saturating_add(1);
        if hops > sub.depth.min(max) {
            continue;
        }
        let mut out = u.clone();
        out.hops = hops;
        let _ = tx.try_send(out);
    }
}

fn write_presence_frame(w: &mut TcpStream, ext: bool, items: &[PresenceUpdate]) -> io::Result<()> {
    if ext {
        write_frame(w, proto::T_FED_PRESENCE_UPDATE_EXT, &[&encode_updates_ext(items)])
    } else {
        write_frame(w, proto::T_FED_PRESENCE_UPDATE, &[&encode_updates(items)])
    }
}

/// A duplex presence stream: both ends send their local online players and store the other's.
/// An extended stream also forwards players from other servers, up to its depth; tombstones clear them
/// immediately when the stream that carried them closes. Plain streams stay compatible with older peers.
fn presence_session(
    shared: &Arc<Mutex<super::Shared>>,
    stream: TcpStream,
    gen: u64,
    ext: bool,
    depth: u8,
    requester: String,
    peer_tag: String,
) -> io::Result<()> {
    let mut reader = stream;
    let mut writer = reader.try_clone()?;
    let _ = writer.set_write_timeout(Some(Duration::from_secs(10)));
    let _ = reader.set_read_timeout(Some(Duration::from_secs(120)));
    let (tx, rx) = mpsc::sync_channel::<PresenceUpdate>(1024);
    let (sid, snapshot) = {
        let mut sh = shared.lock().unwrap();
        sh.next_sub += 1;
        let sid = sh.next_sub;
        sh.presence_subs.push((
            sid,
            tx,
            PresenceSub {
                ext,
                depth,
                requester: requester.clone(),
                source_tag: peer_tag.clone(),
            },
        ));
        let self_id = sh.identity.id.clone();
        let mut local_ids: HashSet<[u8; proto::FRIEND_LEN]> = HashSet::new();
        let mut snapshot: Vec<PresenceUpdate> = sh
            .online
            .iter()
            .map(|(id, p)| {
                local_ids.insert(*id);
                PresenceUpdate {
                    id: *id,
                    state: p.state,
                    code: p.code,
                    friends: p.friends.iter().copied().collect(),
                    removed: p.removed.iter().copied().collect(),
                    name: p.name.clone(),
                    origin: self_id.clone(),
                    hops: 0,
                    gone: false,
                }
            })
            .collect();
        if ext {
            let max = depth.min(sh.cfg.federation_transitive);
            for (id, r) in &sh.fed.remote_presence {
                if local_ids.contains(id)
                    || r.origin == requester
                    || r.origin == self_id
                    || r.source == requester
                    || r.source == peer_tag
                {
                    continue;
                }
                let hops = r.hops.saturating_add(1);
                if hops > max {
                    continue;
                }
                snapshot.push(PresenceUpdate {
                    id: *id,
                    state: r.state,
                    code: r.code,
                    friends: r.friends.iter().copied().collect(),
                    removed: r.removed.iter().copied().collect(),
                    name: r.name.clone(),
                    origin: r.origin.clone(),
                    hops,
                    gone: false,
                });
            }
        }
        (sid, snapshot)
    };
    let stop = Arc::new(std::sync::atomic::AtomicBool::new(false));
    let wstop = Arc::clone(&stop);
    let writer_thread = std::thread::spawn(move || {
        for chunk in snapshot.chunks(128) {
            if chunk.is_empty() {
                continue;
            }
            if write_presence_frame(&mut writer, ext, chunk).is_err() {
                wstop.store(true, std::sync::atomic::Ordering::Relaxed);
                let _ = writer.shutdown(std::net::Shutdown::Both);
                return;
            }
        }
        if write_presence_frame(&mut writer, ext, &[]).is_err() {
            wstop.store(true, std::sync::atomic::Ordering::Relaxed);
            let _ = writer.shutdown(std::net::Shutdown::Both);
            return;
        }
        let mut last = Instant::now();
        while !wstop.load(std::sync::atomic::Ordering::Relaxed) {
            let mut batch = Vec::new();
            match rx.recv_timeout(Duration::from_millis(500)) {
                Ok(u) => batch.push(u),
                Err(mpsc::RecvTimeoutError::Timeout) => {}
                Err(mpsc::RecvTimeoutError::Disconnected) => return,
            }
            while batch.len() < 128 {
                match rx.try_recv() {
                    Ok(u) => batch.push(u),
                    Err(_) => break,
                }
            }
            if batch.is_empty() {
                if last.elapsed() < Duration::from_secs(20) {
                    continue;
                }
                last = Instant::now();
            } else {
                last = Instant::now();
            }
            if write_presence_frame(&mut writer, ext, &batch).is_err() {
                wstop.store(true, std::sync::atomic::Ordering::Relaxed);
                let _ = writer.shutdown(std::net::Shutdown::Both);
                return;
            }
        }
    });
    let result = loop {
        let (t, p) = match read_frame(&mut reader) {
            Ok(v) => v,
            Err(_) => break Ok(()),
        };
        let base = t == proto::T_FED_PRESENCE_UPDATE;
        let mut updates = if t == proto::T_FED_PRESENCE_UPDATE_EXT {
            parse_updates_ext(&p)
        } else if base {
            parse_updates(&p)
        } else {
            break Ok(());
        };
        if updates.is_empty() {
            continue;
        }
        let mut sh = shared.lock().unwrap();
        let self_id = sh.identity.id.clone();
        let max = sh.cfg.max_presence;
        let max_depth = sh.cfg.federation_transitive;
        let now = Instant::now();
        for mut u in updates.drain(..) {
            if base || u.origin.is_empty() {
                u.origin = peer_tag.clone();
                u.hops = 0;
            }
            if u.origin == self_id {
                continue; // a loop: our own players stay authoritative locally
            }
            if u.hops > max_depth {
                continue;
            }
            if u.gone {
                if let Some(old) = sh.fed.remote_presence.remove(&u.id) {
                    if old.origin == u.origin && old.hops >= u.hops {
                        let tomb = remote_tombstone(u.id, old.origin.clone(), old.hops);
                        forward_remote(&sh, &tomb, &peer_tag);
                    } else {
                        sh.fed.remote_presence.insert(u.id, old);
                    }
                }
                continue;
            }
            if sh.fed.remote_presence.len() >= max && !sh.fed.remote_presence.contains_key(&u.id) {
                continue;
            }
            let replace = match sh.fed.remote_presence.get(&u.id) {
                None => true,
                Some(old) => u.hops <= old.hops,
            };
            if !replace {
                if let Some(old) = sh.fed.remote_presence.get_mut(&u.id) {
                    old.seen = now;
                }
                continue;
            }
            sh.fed.remote_presence.insert(
                u.id,
                RemotePresence {
                    gen,
                    origin: u.origin.clone(),
                    source: peer_tag.clone(),
                    hops: u.hops,
                    state: u.state,
                    code: u.code,
                    friends: u.friends.iter().copied().collect(),
                    removed: u.removed.iter().copied().collect(),
                    name: u.name.clone(),
                    seen: now,
                },
            );
            forward_remote(&sh, &u, &peer_tag);
        }
    };
    stop.store(true, std::sync::atomic::Ordering::Relaxed);
    let _ = reader.shutdown(std::net::Shutdown::Both);
    {
        let mut sh = shared.lock().unwrap();
        sh.presence_subs.retain(|(i, _, _)| *i != sid);
        let keys: Vec<[u8; proto::FRIEND_LEN]> = sh
            .fed
            .remote_presence
            .iter()
            .filter(|(_, r)| r.gen == gen)
            .map(|(id, _)| *id)
            .collect();
        for id in keys {
            if let Some(r) = sh.fed.remote_presence.remove(&id) {
                let tomb = remote_tombstone(id, r.origin, r.hops);
                forward_remote(&sh, &tomb, &peer_tag);
            }
        }
    }
    let _ = writer_thread.join();
    result
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_beacon_carries_a_verified_identity() {
        let id = Identity::load("").unwrap();
        let b = beacon_bytes(&id, "0.0.0.0:39510", "hub-a");
        let (rid, pk, endpoint, name) = parse_beacon(&b).expect("valid beacon");
        assert_eq!(rid, id.id);
        assert_eq!(pk, id.public);
        assert_eq!(endpoint, "0.0.0.0:39510");
        assert_eq!(name, "hub-a");
        let mut bad = b.clone();
        let last = bad.len() - 1;
        bad[last] ^= 1;
        assert!(parse_beacon(&bad).is_none());
    }

    #[test]
    fn expired_remote_presence_becomes_a_tombstone() {
        let me = Identity::load("").unwrap();
        let mut fed = FedState::new(&[], "", Arc::new(me));
        let id = [7u8; proto::FRIEND_LEN];
        fed.remote_presence.insert(
            id,
            RemotePresence {
                gen: 1,
                origin: "ed25519:ORIGIN".into(),
                source: "B".into(),
                hops: 1,
                state: 2,
                code: [0u8; proto::CODE_LEN],
                friends: HashSet::new(),
                removed: HashSet::new(),
                name: "P".into(),
                seen: Instant::now() - Duration::from_secs(120),
            },
        );
        let gone = fed.prune(Duration::from_secs(90));
        assert_eq!(gone.len(), 1);
        assert_eq!(gone[0].id, id);
        assert_eq!(gone[0].origin, "ed25519:ORIGIN");
        assert_eq!(gone[0].hops, 1);
        assert!(gone[0].gone);
    }

    #[test]
    fn candidates_are_listed_but_never_peers() {
        let me = Identity::load("").unwrap();
        let (me_id, me_public) = (me.id.clone(), me.public);
        let other = Identity::load("").unwrap();
        let mut fed = FedState::new(&[], "", Arc::new(me));
        fed.candidate(other.id.clone(), "hub-b".into(), "127.0.0.1:39510".into(), other.public);
        assert_eq!(fed.candidate_views().len(), 1);
        assert!(fed.peers.is_empty());
        fed.candidate(me_id, "me".into(), "127.0.0.1:1".into(), me_public);
        assert_eq!(fed.candidate_views().len(), 1, "own beacon is ignored");
    }
}
