//! The hub server: LIST / HOST / JOIN / ATTACH over TCP, the pair splice, and the MHGR UDP relay.
//!
//! Behaviour and wire format mirror MHGURecomp's `server/server.go` (and hence gu-native), so both
//! clients can find each other's hubs and play through this server without a forwarded port.

use std::collections::{HashMap, HashSet, VecDeque};
use std::io::{self, Read, Write};
use std::net::{IpAddr, Shutdown, SocketAddr, TcpListener, TcpStream, UdpSocket};
use std::sync::mpsc::{self, RecvTimeoutError, SyncSender};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use mhgu_protocol as proto;

use crate::bucket::TokenBucket;
use crate::config::Config;

type NetInfo = [u8; proto::NETINFO_SIZE];

/// Bytes relayed through the pairs (both directions), for the web UI.
pub static TRAFFIC: AtomicU64 = AtomicU64::new(0);

struct Hub {
    id: u64,
    room: [u8; 16],
    link_proto: u16,
    info: NetInfo,
    created: Instant,
    /// Frames for the host's writer thread (tickets and pings).
    out: SyncSender<Vec<u8>>,
    /// Pending and spliced players.
    joins: usize,
}

struct IpState {
    conns: usize,
    hubs: usize,
    req: TokenBucket,
    seen: Instant,
}

struct DirStat {
    bytes: AtomicU64,
    last: Mutex<Instant>,
}

impl DirStat {
    fn new() -> DirStat {
        DirStat { bytes: AtomicU64::new(0), last: Mutex::new(Instant::now()) }
    }
    fn add(&self, n: usize) {
        self.bytes.fetch_add(n as u64, Ordering::Relaxed);
        *self.last.lock().unwrap() = Instant::now();
    }
}

struct Pair {
    secret: [u64; 2],
    addr: Mutex<[Option<SocketAddr>; 2]>,
    lim: TokenBucket,
    /// The hub it serves, and the player's address (for the admin page).
    hub: u64,
    player: String,
    /// The two spliced streams, kept to end the pair from the admin page (kick).
    socks: Mutex<[Option<TcpStream>; 2]>,
    /// Traffic per direction (0: host side, 1: player side), and when it was last seen.
    down: DirStat, // host -> player
    up: DirStat,   // player -> host
    started: Instant,
}

pub struct Shared {
    pub(crate) cfg: Config,
    hubs: HashMap<u64, Hub>,
    pending: HashMap<u64, SyncSender<TcpStream>>,
    pairs: HashMap<u32, Arc<Pair>>,
    ips: HashMap<IpAddr, IpState>,
    conns: usize,
    /// The room this server serves when `-key` is set; other rooms are told "no such hub".
    key_room: Option<[u8; 16]>,
    /// Banned addresses (the admin page), and the last lines for its log view.
    banned: HashSet<IpAddr>,
    log: VecDeque<String>,
}

/// A hub as the web UI shows it.
pub struct HubView { pub id: u64, pub players: usize, pub link_proto: u16, pub age: u64, pub room: [u8; 4] }

/// A player of a hub, as the admin page shows it.
pub struct PlayerView { pub pair: u32, pub ip: String, pub down_bps: u64, pub up_bps: u64, pub idle: u64 }

/// A snapshot for one web page.
pub struct WebView { pub hubs: Vec<HubView>, pub players: usize, pub conns: usize, pub ips: usize, pub uptime: u64, pub bytes: u64, pub pairs: Vec<(u64, PlayerView)>, pub key_room: Option<[u8; 4]> }

impl Shared {
    pub(crate) fn web_view(&self, uptime: u64) -> WebView {
        let mut hubs: Vec<HubView> = self
            .hubs
            .values()
            .map(|h| HubView {
                id: h.id,
                // The host, plus the players actually spliced to it (a JOIN that is still waiting for the
                // host's ATTACH is not in the room yet, so it must not show up as a player).
                players: 1 + self.pairs.values().filter(|p| p.hub == h.id).count(),
                link_proto: h.link_proto,
                age: h.created.elapsed().as_secs(),
                room: [h.room[0], h.room[1], h.room[2], h.room[3]],
            })
            .collect();
        hubs.sort_by(|a, b| b.players.cmp(&a.players).then(a.id.cmp(&b.id)));
        WebView {
            players: hubs.iter().map(|h| h.players).sum(),
            conns: self.conns,
            ips: self.ips.len(),
            pairs: self
                .pairs
                .iter()
                .map(|(id, p)| {
                    let secs = p.started.elapsed().as_secs().max(1);
                    let idle = p
                        .up
                        .last
                        .lock()
                        .unwrap()
                        .elapsed()
                        .min(p.down.last.lock().unwrap().elapsed())
                        .as_secs();
                    (p.hub, PlayerView {
                        pair: *id,
                        ip: p.player.clone(),
                        down_bps: p.down.bytes.load(Ordering::Relaxed) / secs,
                        up_bps: p.up.bytes.load(Ordering::Relaxed) / secs,
                        idle,
                    })
                })
                .collect(),
            key_room: self.key_room.map(|r| [r[0], r[1], r[2], r[3]]),
            hubs,
            uptime,
            bytes: TRAFFIC.load(Ordering::Relaxed),
        }
    }

    pub(crate) fn note(&mut self, line: String) {
        if self.log.len() >= 300 {
            self.log.pop_front();
        }
        self.log.push_back(line);
    }

    pub(crate) fn log_tail(&self, n: usize) -> Vec<String> {
        let skip = self.log.len().saturating_sub(n);
        self.log.iter().skip(skip).cloned().collect()
    }

    pub(crate) fn ban(&mut self, ip: IpAddr) {
        self.banned.insert(ip);
        self.note(format!("admin: banned {ip}"));
    }

    pub(crate) fn unban(&mut self, ip: IpAddr) {
        self.banned.remove(&ip);
        self.note(format!("admin: unbanned {ip}"));
    }

    pub(crate) fn bans(&self) -> Vec<IpAddr> {
        let mut v: Vec<IpAddr> = self.banned.iter().copied().collect();
        v.sort();
        v
    }

    /// Ends one player's link (kick): both spliced sockets are shut down, the pair goes.
    pub(crate) fn kick_pair(&mut self, pair: u32) -> bool {
        let Some(p) = self.pairs.remove(&pair) else { return false };
        for s in p.socks.lock().unwrap().iter().flatten() {
            let _ = s.shutdown(Shutdown::Both);
        }
        self.note(format!("admin: kicked pair {pair:#x}"));
        true
    }

    /// Drops a hub: its host's writer channel closes (the link ends), and its listed state goes.
    pub(crate) fn close_hub(&mut self, id: u64) -> bool {
        self.pending.remove(&id);
        if self.hubs.contains_key(&id) {
            self.note(format!("admin: closed hub {id:#x}"));
        }
        if let Some(h) = self.hubs.get(&id) {
            // Tell the host its hub is gone (its link ends), then drop the listing.
            let _ = h.out.try_send(mhgu_protocol::frame(proto::T_ERROR, &[&1u32.to_le_bytes()]));
        }
        self.hubs.remove(&id).is_some()
    }
}

/// The hub server. Open it with [`Server::new`], then [`Server::serve`].
pub struct Server {
    listener: TcpListener,
    udp: Arc<UdpSocket>,
    shared: Arc<Mutex<Shared>>,
}

impl Server {
    pub fn new(cfg: Config) -> io::Result<Server> {
        let bind = if cfg.addr.starts_with(':') {
            format!("0.0.0.0{}", cfg.addr)
        } else {
            cfg.addr.clone()
        };
        let listener = TcpListener::bind(&bind)?;
        let local = listener.local_addr()?;
        let udp = UdpSocket::bind(SocketAddr::new(local.ip(), local.port()))?;
        let key_room = if cfg.key.trim_matches(' ').is_empty() {
            None
        } else {
            Some(proto::room_from_key(&cfg.key))
        };
        let shared = Arc::new(Mutex::new(Shared {
            key_room,
            cfg,
            hubs: HashMap::new(),
            pending: HashMap::new(),
            pairs: HashMap::new(),
            ips: HashMap::new(),
            conns: 0,
            banned: HashSet::new(),
            log: VecDeque::new(),
        }));
        Ok(Server {
            listener,
            udp: Arc::new(udp),
            shared,
        })
    }

    /// The address actually bound (useful with port 0).
    pub fn addr(&self) -> SocketAddr {
        self.listener.local_addr().expect("listener bound")
    }

    /// Accepts connections until the listener fails; also runs the UDP relay and the janitor.
    pub fn serve(&self) -> io::Result<()> {
        {
            let udp = Arc::clone(&self.udp);
            let shared = Arc::clone(&self.shared);
            thread::spawn(move || serve_udp(udp, shared));
        }
        {
            let shared = Arc::clone(&self.shared);
            thread::spawn(move || janitor(shared));
        }
        if !self.shared.lock().unwrap().cfg.web.trim().is_empty() {
            let (addr, token) = {
                let g = self.shared.lock().unwrap();
                (g.cfg.web.clone(), g.cfg.web_token.clone())
            };
            match crate::web::Web::bind(&addr, Arc::clone(&self.shared), &token) {
                Ok(Some(w)) => {
                    let a = w.addr().map(|a| a.to_string()).unwrap_or(addr.clone());
                    let t = w.token().to_string();
                    eprintln!("mhgu-hub: web UI on http://{a}/ (admin: http://{a}/admin?token={t})");
                    let w = Arc::new(w);
                    let w2 = Arc::clone(&w);
                    thread::spawn(move || {
                        if let Err(e) = w2.serve() {
                            eprintln!("mhgu-hub: web UI stopped: {e}");
                        }
                    });
                }
                Ok(None) => {}
                Err(e) => eprintln!("mhgu-hub: web UI: {e}"),
            }
        }
        for conn in self.listener.incoming() {
            match conn {
                Ok(c) => {
                    let shared = Arc::clone(&self.shared);
                    thread::spawn(move || handle(c, shared));
                }
                Err(_) => thread::sleep(Duration::from_millis(50)),
            }
        }
        Ok(())
    }
}

// ---- admission ------------------------------------------------------------

struct AdmitGuard {
    shared: Arc<Mutex<Shared>>,
    ip: IpAddr,
}

impl Drop for AdmitGuard {
    fn drop(&mut self) {
        let mut s = self.shared.lock().unwrap();
        s.conns = s.conns.saturating_sub(1);
        if let Some(st) = s.ips.get_mut(&self.ip) {
            st.conns = st.conns.saturating_sub(1);
            st.seen = Instant::now();
        }
    }
}

fn admit(shared: &Arc<Mutex<Shared>>, ip: IpAddr) -> bool {
    let mut guard = shared.lock().unwrap();
    let Shared { cfg, ips, conns, banned, .. } = &mut *guard;
    let (rate, burst) = (cfg.req_rate, cfg.req_burst);
    let st = ips.entry(ip).or_insert_with(|| IpState {
        conns: 0,
        hubs: 0,
        req: TokenBucket::new(rate, burst),
        seen: Instant::now(),
    });
    st.seen = Instant::now();
    if *conns >= cfg.max_conns || st.conns >= cfg.max_conns_per_ip {
        return false;
    }
    if banned.contains(&ip) {
        return false;
    }
    *conns += 1;
    st.conns += 1;
    true
}

fn allow_request(shared: &Arc<Mutex<Shared>>, ip: IpAddr) -> bool {
    let s = shared.lock().unwrap();
    s.ips.get(&ip).map(|st| st.req.take(1.0)).unwrap_or(false)
}

// ---- connections ----------------------------------------------------------

fn handle(conn: TcpStream, shared: Arc<Mutex<Shared>>) {
    let ip = match conn.peer_addr() {
        Ok(a) => a.ip(),
        Err(_) => return,
    };
    if !admit(&shared, ip) {
        return;
    }
    let _guard = AdmitGuard {
        shared: Arc::clone(&shared),
        ip,
    };
    let _ = conn.set_nodelay(true);
    let handshake = shared.lock().unwrap().cfg.handshake;
    let _ = conn.set_read_timeout(Some(handshake));
    let mut c = conn;
    let (t, p) = match proto::read_frame(&mut c, proto::MAX_FRAME) {
        Ok(v) => v,
        Err(_) => return,
    };
    let hdr = match proto::parse_header(&p) {
        Ok(h) => h,
        Err(proto::HeaderError::Version) => {
            let _ = send_error(&mut c, proto::ERR_VERSION);
            return;
        }
        Err(proto::HeaderError::BadMagic) => return,
    };
    let (min_proto, separate_phones) = {
        let s = shared.lock().unwrap();
        (s.cfg.min_proto, s.cfg.separate_phones)
    };
    if hdr.link_proto < min_proto {
        if t == proto::T_HOST {
            shared.lock().unwrap().note(format!(
                "hub refused (protocol {}, {})",
                hdr.link_proto,
                room_name(hdr.room)
            ));
        }
        let _ = send_error(&mut c, proto::ERR_VERSION);
        return;
    }
    // Android's public hubs live in the key "public-phone"'s room, as the game asks; the server may put them in the
    // public room instead, so phones and PCs without a key see each other's hubs.
    let mut room = hdr.room;
    if !separate_phones && room == proto::room_from_key("public-phone") {
        room = [0u8; 16];
    }
    let body = &p[proto::HEADER_SIZE..];
    match t {
        proto::T_LIST => {
            if !allow_request(&shared, ip) {
                let _ = send_error(&mut c, proto::ERR_BUSY);
                return;
            }
            list(&mut c, &shared, hdr.link_proto, room);
        }
        proto::T_HOST => {
            if key_mismatch(&shared, room) {
                let _ = send_error(&mut c, proto::ERR_NOT_FOUND);
                return;
            }
            if body.len() != proto::NETINFO_SIZE || !proto::info_ok(body) {
                let _ = send_error(&mut c, proto::ERR_BAD_REQ);
                return;
            }
            host(c, &shared, ip, hdr.link_proto, room, body);
        }
        proto::T_ATTACH => {
            if body.len() != 8 {
                return;
            }
            let ticket = u64::from_le_bytes(body[..8].try_into().unwrap());
            attach(c, &shared, ticket);
        }
        proto::T_JOIN => {
            if body.len() != 8 {
                return;
            }
            if !allow_request(&shared, ip) {
                let _ = send_error(&mut c, proto::ERR_BUSY);
                return;
            }
            let id = u64::from_le_bytes(body[..8].try_into().unwrap());
            if key_mismatch(&shared, room) {
                let _ = send_error(&mut c, proto::ERR_NOT_FOUND);
                return;
            }
            join(c, &shared, hdr.link_proto, room, id);
        }
        _ => {}
    }
}

/// Whether a request's room is refused because this server runs with `-key`.
/// The answer is the same one a client with another key gets from any hub: ERR_NOT_FOUND.
fn key_mismatch(shared: &Arc<Mutex<Shared>>, room: [u8; 16]) -> bool {
    let s = shared.lock().unwrap();
    matches!(s.key_room, Some(want) if room != want)
}

/// A room for the log: "public", "phone" (Android's public room) or "key" and its hash (the key itself never arrives).
fn room_name(room: [u8; 16]) -> String {
    if room == [0u8; 16] {
        return "public".into();
    }
    if room == proto::room_from_key("public-phone") {
        return "phone".into();
    }
    format!("key {:02x?}", room)
}

fn send_error(c: &mut TcpStream, code: u32) -> io::Result<()> {
    let _ = c.set_write_timeout(Some(Duration::from_secs(10)));
    proto::write_frame(c, &proto::frame(proto::T_ERROR, &[&code.to_le_bytes()]))
}

// ---- LIST -----------------------------------------------------------------

fn list(c: &mut TcpStream, shared: &Arc<Mutex<Shared>>, link_proto: u16, room: [u8; 16]) {
    let (mut es, list_max) = {
        let s = shared.lock().unwrap();
        let v: Vec<(u64, NetInfo, Instant)> = s
            .hubs
            .values()
            .filter(|h| h.link_proto == link_proto && h.room == room)
            .map(|h| (h.id, h.info, h.created))
            .collect();
        (v, s.cfg.list_max)
    };
    let free = |info: &NetInfo| info[proto::NI_NODE_COUNT] < info[proto::NI_NODE_MAX];

    // More hubs than one answer holds: a random choice, those with a free place first, so each
    // search (the game's Retry) can show others.
    if es.len() > list_max {
        shuffle(&mut es);
        es.sort_by_key(|e| !free(&e.1));
        es.truncate(list_max);
    }
    es.sort_by(|a, b| {
        let (fa, fb) = (free(&a.1), free(&b.1));
        if fa != fb {
            return fb.cmp(&fa); // free first
        }
        let (ca, cb) = (a.1[proto::NI_NODE_COUNT], b.1[proto::NI_NODE_COUNT]);
        if ca != cb {
            return cb.cmp(&ca); // more players first
        }
        b.2.cmp(&a.2) // newest first
    });

    let mut out = Vec::with_capacity(4 + es.len() * (8 + proto::NETINFO_SIZE));
    out.extend_from_slice(&(es.len() as u32).to_le_bytes());
    for (id, info, _) in &es {
        out.extend_from_slice(&id.to_le_bytes());
        out.extend_from_slice(info);
    }
    let _ = proto::write_frame(c, &proto::frame(proto::T_HUBS, &[&out]));
}

// ---- HOST -----------------------------------------------------------------

enum Reserved {
    Ok(u64),
    Busy,
}

fn host(
    mut c: TcpStream,
    shared: &Arc<Mutex<Shared>>,
    ip: IpAddr,
    link_proto: u16,
    room: [u8; 16],
    info: &[u8],
) {
    let cfg = shared.lock().unwrap().cfg.clone();
    let (tx, rx) = mpsc::sync_channel::<Vec<u8>>(16);

    // Reserve the slot under one lock (avoids a TOCTOU with the limits).
    let mut note_after: Option<String> = None;
    let reserved = {
        let mut guard = shared.lock().unwrap();
        let Shared { cfg: cfgref, hubs, ips, .. } = &mut *guard;
        match ips.get_mut(&ip) {
            Some(st) if hubs.len() < cfgref.max_hubs && st.hubs < cfgref.max_hubs_per_ip => {
                st.hubs += 1;
                let mut cand = 0u64;
                while cand == 0 || hubs.contains_key(&cand) {
                    cand = rand64();
                }
                let mut ninfo = [0u8; proto::NETINFO_SIZE];
                ninfo.copy_from_slice(info);
                hubs.insert(
                    cand,
                    Hub {
                        id: cand,
                        room,
                        link_proto,
                        info: ninfo,
                        created: Instant::now(),
                        out: tx,
                        joins: 0,
                    },
                );
                note_after = Some(format!("hub {cand:#x} listed (protocol {link_proto}, {})", room_name(room)));
                Reserved::Ok(cand)
            }
            _ => Reserved::Busy,
        }
    };
    {
        let mut g = shared.lock().unwrap();
        if let Some(line) = note_after.take() {
            g.note(line);
        }
    }
    let id = match reserved {
        Reserved::Ok(id) => id,
        Reserved::Busy => {
            let _ = send_error(&mut c, proto::ERR_BUSY);
            return;
        }
    };

    if proto::write_frame(&mut c, &proto::frame(proto::T_HOSTED, &[&id.to_le_bytes()])).is_err() {
        cleanup_hub(shared, id, ip);
        return;
    }

    // Writer: tickets and pings; a write error closes the connection, which ends the reader too.
    let mut writer = match c.try_clone() {
        Ok(w) => w,
        Err(_) => {
            cleanup_hub(shared, id, ip);
            return;
        }
    };
    let ping = cfg.ping_every;
    thread::spawn(move || {
        let _ = writer.set_write_timeout(Some(Duration::from_secs(10)));
        loop {
            let f = match rx.recv_timeout(ping) {
                Ok(f) => f,
                Err(RecvTimeoutError::Timeout) => proto::frame(proto::T_PING, &[]),
                Err(RecvTimeoutError::Disconnected) => return,
            };
            if proto::write_frame(&mut writer, &f).is_err() {
                let _ = writer.shutdown(Shutdown::Both);
                return;
            }
        }
    });

    // Reader: UPDATE / PING from the host.
    loop {
        let _ = c.set_read_timeout(Some(ping * 3));
        let (t, p) = match proto::read_frame(&mut c, proto::MAX_FRAME) {
            Ok(v) => v,
            Err(_) => break,
        };
        match t {
            proto::T_UPDATE => {
                if !proto::info_ok(&p) {
                    break;
                }
                let mut s = shared.lock().unwrap();
                if let Some(h) = s.hubs.get_mut(&id) {
                    h.info.copy_from_slice(&p);
                }
            }
            proto::T_PING => {}
            _ => break,
        }
    }
    cleanup_hub(shared, id, ip);
}

fn cleanup_hub(shared: &Arc<Mutex<Shared>>, id: u64, ip: IpAddr) {
    let mut s = shared.lock().unwrap();
    s.hubs.remove(&id);
    if let Some(st) = s.ips.get_mut(&ip) {
        st.hubs = st.hubs.saturating_sub(1);
    }
}

// ---- JOIN / ATTACH --------------------------------------------------------

struct JoinGuard {
    shared: Arc<Mutex<Shared>>,
    hub_id: u64,
    ticket: u64,
}

impl Drop for JoinGuard {
    fn drop(&mut self) {
        let mut s = self.shared.lock().unwrap();
        s.pending.remove(&self.ticket);
        if let Some(h) = s.hubs.get_mut(&self.hub_id) {
            h.joins = h.joins.saturating_sub(1);
        }
    }
}

struct PairGuard {
    shared: Arc<Mutex<Shared>>,
    id: u32,
}

impl Drop for PairGuard {
    fn drop(&mut self) {
        let mut s = self.shared.lock().unwrap();
        let was = s.pairs.remove(&self.id).is_some();
        if was {
            s.note(format!("pair {:#x} closed", self.id));
        }
    }
}

enum Joined {
    Ok(u64, SyncSender<Vec<u8>>),
    NotFound,
    Busy,
}

fn join(
    mut c: TcpStream,
    shared: &Arc<Mutex<Shared>>,
    link_proto: u16,
    room: [u8; 16],
    id: u64,
) {
    let cfg = shared.lock().unwrap().cfg.clone();
    let (host_tx, host_rx) = mpsc::sync_channel::<TcpStream>(1);

    let setup = {
        let mut s = shared.lock().unwrap();
        let Shared { hubs, pending, .. } = &mut *s;
        match hubs.get_mut(&id) {
            None => Joined::NotFound,
            Some(h) if h.link_proto != link_proto || h.room != room => Joined::NotFound,
            Some(h) if h.joins >= (proto::MAX_NODES as usize - 1) => Joined::Busy,
            Some(h) => {
                let mut cand = 0u64;
                while cand == 0 || pending.contains_key(&cand) {
                    cand = rand64();
                }
                pending.insert(cand, host_tx);
                h.joins += 1;
                Joined::Ok(cand, h.out.clone())
            }
        }
    };

    let (ticket, hub_out) = match setup {
        Joined::Ok(ticket, out) => (ticket, out),
        Joined::NotFound => {
            let _ = send_error(&mut c, proto::ERR_NOT_FOUND);
            return;
        }
        Joined::Busy => {
            let _ = send_error(&mut c, proto::ERR_BUSY);
            return;
        }
    };
    let _guard = JoinGuard {
        shared: Arc::clone(shared),
        hub_id: id,
        ticket,
    };

    // Ask the host for a second connection with this ticket.
    if hub_out
        .try_send(proto::frame(proto::T_INCOMING, &[&ticket.to_le_bytes()]))
        .is_err()
    {
        let _ = send_error(&mut c, proto::ERR_BUSY);
        return;
    }

    // Wait for the host's ATTACH.
    let mut host_conn = match host_rx.recv_timeout(cfg.handshake) {
        Ok(hc) => hc,
        Err(_) => {
            let was_pending = {
                let mut s = shared.lock().unwrap();
                s.pending.remove(&ticket).is_some()
            };
            if was_pending {
                let _ = send_error(&mut c, proto::ERR_NO_ANSWER);
                return;
            }
            // The ATTACH took the ticket just now: its connection is on the way.
            match host_rx.recv_timeout(Duration::from_secs(2)) {
                Ok(hc) => hc,
                Err(_) => return,
            }
        }
    };

    // One pair: the host and the player, with their UDP secrets.
    let secret = [rand64(), rand64()];
    let pair_id: u32;
    {
        let mut s = shared.lock().unwrap();
        let mut cand = 0u32;
        while cand == 0 || s.pairs.contains_key(&cand) {
            cand = rand64() as u32;
        }
        pair_id = cand;
        s.pairs.insert(
            pair_id,
            Arc::new(Pair {
                secret,
                addr: Mutex::new([None, None]),
                lim: TokenBucket::new(cfg.bandwidth, cfg.bandwidth),
                hub: id,
                player: c.peer_addr().map(|a| a.ip().to_string()).unwrap_or_default(),
                socks: Mutex::new([None, None]),
                down: DirStat::new(),
                up: DirStat::new(),
                started: Instant::now(),
            }),
        );
    }
    shared
        .lock()
        .unwrap()
        .note(format!("pair {pair_id:#x} spliced (hub {id:#x}, player {})", c.peer_addr().map(|a| a.ip().to_string()).unwrap_or_default()));
    let _pair_guard = PairGuard {
        shared: Arc::clone(shared),
        id: pair_id,
    };

    let hf = proto::frame(
        proto::T_SPLICED,
        &[&pair_id.to_le_bytes(), &secret[0].to_le_bytes()],
    );
    let pf = proto::frame(
        proto::T_SPLICED,
        &[&pair_id.to_le_bytes(), &secret[1].to_le_bytes()],
    );
    let _ = host_conn.set_write_timeout(Some(Duration::from_secs(10)));
    let _ = c.set_write_timeout(Some(Duration::from_secs(10)));
    if proto::write_frame(&mut host_conn, &hf).is_err() || proto::write_frame(&mut c, &pf).is_err() {
        return;
    }

    {
        let s = shared.lock().unwrap();
        if let Some(p) = s.pairs.get(&pair_id) {
            *p.socks.lock().unwrap() = [host_conn.try_clone().ok(), c.try_clone().ok()];
        }
    }
    let pair = shared.lock().unwrap().pairs.get(&pair_id).cloned();
    splice(host_conn, c, cfg.bandwidth, pair);
}

fn attach(mut c: TcpStream, shared: &Arc<Mutex<Shared>>, ticket: u64) {
    let tx = {
        let mut s = shared.lock().unwrap();
        s.pending.remove(&ticket)
    };
    match tx {
        None => {
            let _ = send_error(&mut c, proto::ERR_NOT_FOUND);
        }
        Some(tx) => {
            // Move the connection to the waiting JOIN; it now owns the stream.
            let _ = tx.send(c);
        }
    }
}

// ---- splice ---------------------------------------------------------------

fn splice(a: TcpStream, b: TcpStream, bandwidth: f64, pair: Option<Arc<Pair>>) {
    let _ = a.set_read_timeout(None);
    let _ = a.set_write_timeout(None);
    let _ = b.set_read_timeout(None);
    let _ = b.set_write_timeout(None);
    let (a_rd, a_wr, b_rd, b_wr) = match (a.try_clone(), a.try_clone(), b.try_clone(), b.try_clone()) {
        (Ok(ar), Ok(aw), Ok(br), Ok(bw)) => (ar, aw, br, bw),
        _ => return,
    };
    let (done_tx, done_rx) = mpsc::channel::<()>();
    let done2 = done_tx.clone();
    let lim1 = TokenBucket::new(bandwidth, bandwidth);
    let lim2 = TokenBucket::new(bandwidth, bandwidth);
    let p1 = pair.clone();
    let p2 = pair;
    thread::spawn(move || {
        let st = p1.as_ref().map(|p| &p.down);
        pipe(a_rd, b_wr, lim1, st);
        let _ = done_tx.send(());
    });
    thread::spawn(move || {
        let st = p2.as_ref().map(|p| &p.up);
        pipe(b_rd, a_wr, lim2, st);
        let _ = done2.send(());
    });
    let _ = done_rx.recv();
    let _ = a.shutdown(Shutdown::Both);
    let _ = b.shutdown(Shutdown::Both);
    let _ = done_rx.recv();
}

fn pipe(mut src: TcpStream, mut dst: TcpStream, lim: TokenBucket, stat: Option<&DirStat>) {
    let mut buf = vec![0u8; 16 << 10];
    loop {
        let n = match src.read(&mut buf) {
            Ok(0) => break,
            Ok(n) => n,
            Err(_) => break,
        };
        let d = lim.wait(n as f64);
        if !d.is_zero() {
            thread::sleep(d);
        }
        TRAFFIC.fetch_add(n as u64, Ordering::Relaxed);
        if let Some(st) = stat {
            st.add(n);
        }
        let _ = dst.set_write_timeout(Some(Duration::from_secs(60)));
        if dst.write_all(&buf[..n]).is_err() {
            break;
        }
    }
}

// ---- UDP relay ------------------------------------------------------------

fn serve_udp(udp: Arc<UdpSocket>, shared: Arc<Mutex<Shared>>) {
    let mut buf = vec![0u8; proto::UDP_MAX + 1];
    loop {
        let (n, from) = match udp.recv_from(&mut buf) {
            Ok(v) => v,
            Err(_) => continue,
        };
        if n < proto::UDP_HEADER || n > proto::UDP_MAX || buf[..4] != proto::UDP_MAGIC {
            continue;
        }
        let id = u32::from_le_bytes(buf[4..8].try_into().unwrap());
        let secret = u64::from_le_bytes(buf[8..16].try_into().unwrap());
        let pair = {
            let s = shared.lock().unwrap();
            s.pairs.get(&id).cloned()
        };
        let pair = match pair {
            Some(p) => p,
            None => continue,
        };
        let side = match pair.secret.iter().position(|&s| s == secret) {
            Some(s) => s,
            None => continue,
        };
        let to = {
            let mut addr = pair.addr.lock().unwrap();
            addr[side] = Some(from); // NAT may change it: the latest wins
            addr[1 - side]
        };
        // An empty datagram only tells where its sender is.
        if n > proto::UDP_HEADER && to.is_some() && pair.lim.take(n as f64) {
            let _ = udp.send_to(&buf[proto::UDP_HEADER..n], to.unwrap());
        }
    }
}

// ---- housekeeping ---------------------------------------------------------

fn janitor(shared: Arc<Mutex<Shared>>) {
    loop {
        thread::sleep(Duration::from_secs(60));
        let mut s = shared.lock().unwrap();
        let now = Instant::now();
        s.ips.retain(|_, st| {
            !(st.conns == 0 && st.hubs == 0 && now.duration_since(st.seen) > Duration::from_secs(300))
        });
    }
}

fn rand64() -> u64 {
    use std::fs::File;
    let mut f = File::open("/dev/urandom").expect("/dev/urandom");
    let mut b = [0u8; 8];
    f.read_exact(&mut b).expect("/dev/urandom");
    u64::from_le_bytes(b)
}

fn shuffle<T>(v: &mut [T]) {
    let mut seed = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_nanos() as u64)
        .unwrap_or(1)
        | 1;
    for i in (1..v.len()).rev() {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        let j = (seed % (i as u64 + 1)) as usize;
        v.swap(i, j);
    }
}
