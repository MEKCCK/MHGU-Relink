//! The hub server: LIST / HOST / JOIN / ATTACH over TCP, the pair splice, and the MHGR UDP relay.
//!
//! Behaviour and wire format mirror MHGURecomp's hub, so its clients can find each other's hubs
//! and play through this server without a forwarded port. gu-native is not a supported target.

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
use crate::config::{Config, Live};

#[path = "federation.rs"]
mod federation;

type NetInfo = [u8; proto::NETINFO_SIZE];

/// Bytes relayed through the pairs (both directions), for the web UI.
pub static TRAFFIC: AtomicU64 = AtomicU64::new(0);

struct Hub {
    id: u64,
    room: [u8; 16],
    link_proto: u16,
    /// Protocol 4: the session code the host picked (zeros: none).
    code: [u8; proto::CODE_LEN],
    info: NetInfo,
    created: Instant,
    /// Frames for the host's writer thread (tickets and pings).
    out: SyncSender<Vec<u8>>,
    /// The host's connection, to end it when an admin closes the hub or raises the minimum protocol.
    conn: TcpStream,
    ip: std::net::IpAddr,
    /// Pending and spliced players.
    joins: usize,
}

/// A player online for its friends (PRESENCE): kept `-presence-ttl` after its last request.
struct Presence {
    /// The friend codes it lists.
    friends: HashSet<[u8; proto::FRIEND_LEN]>,
    /// The players it removed from its friends (told to them: they drop it too).
    removed: HashSet<[u8; proto::FRIEND_LEN]>,
    state: u8,
    code: [u8; proto::CODE_LEN],
    /// The hunter name its friend requests show.
    name: String,
    seen: Instant,
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
    /// Each end asked for the other's address (its consent to show its own); a game too old to ask stays
    /// on the relay.
    direct: Mutex<[bool; 2]>,
    /// The pair's direct key, given to both ends with the address; direct datagrams carry it.
    dkey: u64,
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
    /// The player side is another hub server (federated join): relayed data to it keeps the `MHGR`
    /// prefix so it can route by pair, instead of being stripped as for a game.
    fed_player: bool,
}

pub struct Shared {
    pub(crate) cfg: Config,
    /// Settings changeable from the admin page without a restart.
    pub(crate) live: Live,
    hubs: HashMap<u64, Hub>,
    /// Hubs by session code (protocol 4).
    codes: HashMap<[u8; proto::CODE_LEN], u64>,
    /// Players online for the friends list, and who lists whom the other way around: `listed_by[b][a]` is
    /// since when a (online) lists b. A listing not returned is a friend request to b.
    online: HashMap<[u8; proto::FRIEND_LEN], Presence>,
    listed_by: HashMap<[u8; proto::FRIEND_LEN], HashMap<[u8; proto::FRIEND_LEN], Instant>>,
    pending: HashMap<u64, SyncSender<TcpStream>>,
    pairs: HashMap<u32, Arc<Pair>>,
    ips: HashMap<IpAddr, IpState>,
    conns: usize,
    /// The room this server serves when `-key` is set; other rooms are told "no such hub".
    key_room: Option<[u8; 16]>,
    /// Banned addresses (the admin page), and the last lines for its log view.
    banned: HashSet<IpAddr>,
    log: VecDeque<String>,
    /// Peers, their cached hub listings, and the aliases remote hubs are listed under.
    fed: federation::FedState,
    /// Federated player legs: our pair to the player, the peer's pair to the host.
    bridges: HashMap<u32, Arc<federation::Bridge>>,
    /// (peer's pair, secret) -> our bridge, for datagrams coming back from a peer.
    bridge_by_remote: HashMap<(u32, u64), u32>,
    /// The server's UDP socket, for federated relay registration.
    udp: Arc<UdpSocket>,
    /// This server's Ed25519 identity (well-known and the v2 federation handshake).
    identity: Arc<crate::identity::Identity>,
    /// Outbound presence streams (one per peer): local updates are fanned out here.
    presence_subs: Vec<(u64, SyncSender<federation::PresenceUpdate>, federation::PresenceSub)>,
    next_sub: u64,
}

/// A hub as the web UI shows it.
pub struct HubView {
    pub id: u64,
    pub players: usize,
    pub link_proto: u16,
    pub age: u64,
    pub room: [u8; 4],
    pub room_name: String,
    pub names: Vec<String>,
    pub count: u8,
    pub max: u8,
    pub quest_id: u32,
    pub in_quest: bool,
    pub passcode: Option<u16>,
    pub hunter_search: bool,
}

/// A federated peer, as the web UI shows it.
pub struct PeerRow {
    pub name: String,
    pub addr: String,
    pub online: bool,
    pub rtt_ms: u64,
    pub hubs: usize,
    pub last_error: Option<String>,
}

/// A LAN-discovery candidate (never auto-joined).
pub struct CandidateRow {
    pub id: String,
    pub name: String,
    pub addr: String,
    pub age: u64,
    pub configured: bool,
    pub public_key: String,
}

/// A player of a hub, as the admin page shows it.
pub struct PlayerView { pub pair: u32, pub ip: String, pub down_bps: u64, pub up_bps: u64, pub idle: u64 }

/// A snapshot for one web page.
pub struct WebView { pub hubs: Vec<HubView>, pub players: usize, pub conns: usize, pub ips: usize, pub uptime: u64, pub bytes: u64, pub pairs: Vec<(u64, PlayerView)>, pub key_room: Option<[u8; 4]>, pub peers: Vec<PeerRow>, pub candidates: Vec<CandidateRow>, pub fed_joins: u64, pub fed_bytes: u64, pub federated_presence: usize }

impl Shared {
    pub(crate) fn web_view(&self, uptime: u64) -> WebView {
        let mut hubs: Vec<HubView> = self
            .hubs
            .values()
            .map(|h| {
                let d = crate::room::parse(&h.info);
                HubView {
                    id: h.id,
                    // The host, plus the players actually spliced to it (a JOIN that is still waiting for the
                    // host's ATTACH is not in the room yet, so it must not show up as a player).
                    players: 1 + self.pairs.values().filter(|p| p.hub == h.id).count(),
                    link_proto: h.link_proto,
                    age: h.created.elapsed().as_secs(),
                    room: [h.room[0], h.room[1], h.room[2], h.room[3]],
                    room_name: d.room_name,
                    names: d.names,
                    count: d.count,
                    max: d.max,
                    quest_id: d.quest_id,
                    in_quest: d.in_quest,
                    passcode: d.passcode,
                    hunter_search: d.hunter_search,
                }
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
            peers: self
                .fed
                .views()
                .into_iter()
                .map(|p| PeerRow {
                    name: p.name,
                    addr: p.addr,
                    online: p.online,
                    rtt_ms: p.rtt_ms,
                    hubs: p.hubs,
                    last_error: p.last_error,
                })
                .collect(),
            candidates: self
                .fed
                .candidate_views()
                .into_iter()
                .map(|c| CandidateRow {
                    id: c.id,
                    name: c.name,
                    addr: c.addr,
                    age: c.seen.elapsed().as_secs(),
                    configured: c.configured,
                    public_key: crate::identity::hex(&c.public),
                })
                .collect(),
            fed_joins: self.fed.joins,
            fed_bytes: self.fed.bytes.load(Ordering::Relaxed),
            federated_presence: self.fed.remote_presence.len(),
            hubs,
            uptime,
            bytes: TRAFFIC.load(Ordering::Relaxed),
        }
    }

    /// `/.well-known/mhgu-federation`: who we are, where to reach us, and the key behind the id.
    pub(crate) fn well_known(&self) -> String {
        let endpoint = if self.cfg.advertise.is_empty() {
            let a = &self.cfg.addr;
            if a.starts_with(':') {
                format!("127.0.0.1{a}")
            } else {
                a.clone()
            }
        } else {
            self.cfg.advertise.clone()
        };
        format!(
            r#"{{"server_id":"{}","endpoint":"{}","versions":[2,1],"keys":[{{"key_id":"{}","public_key":"{}"}}],"capabilities":["list","find","join","identity"]}}"#,
            self.identity.id,
            endpoint.replace('"', ""),
            self.identity.id,
            crate::identity::hex(&self.identity.public)
        )
    }

    /// Approves a LAN candidate: allow its identity in and list its hubs (both persisted by the admin page).
    pub(crate) fn approve_candidate(&mut self, id: &str) -> Result<(), String> {
        let Some(c) = self.fed.candidates.get(id).cloned() else {
            return Err("that candidate is no longer known".into());
        };
        if !self.cfg.federation_allow.iter().any(|a| a == id) {
            self.cfg.federation_allow.push(id.to_string());
        }
        let addr = c.addr.clone();
        if !self.cfg.federate.iter().any(|p| p.key == id || p.addr == addr) {
            self.cfg.federate.push(crate::config::Peer {
                addr: addr.clone(),
                token: String::new(),
                name: c.name.clone(),
                key: id.to_string(),
            });
            let token = self.cfg.federation_token.clone();
            let name = c.name.clone();
            self.fed.add_peer(
                crate::config::Peer {
                    addr,
                    token: String::new(),
                    name,
                    key: id.to_string(),
                },
                &token,
            );
        }
        self.fed.mark_configured(id);
        self.note(format!("admin: approved federation candidate {id}"));
        if let Err(e) = federation::save_state(&self.cfg) {
            return Err(format!("approved, but the approvals file could not be saved: {e}"));
        }
        Ok(())
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

    /// The settings the admin page changes (a copy while the lock is held).
    pub(crate) fn live(&self) -> Live {
        self.live.clone()
    }

    /// Applies live settings. A raised minimum protocol closes the hosts of older hubs; other limits apply
    /// to the next connection/request/pair. Returns how many hubs were closed.
    pub(crate) fn apply_live(&mut self, next: Live) -> usize {
        let old = self.live.clone();
        self.live = next.clone();
        for st in self.ips.values_mut() {
            st.req = TokenBucket::new(next.req_rate, next.req_burst);
        }
        let mut gone = Vec::new();
        if next.min_proto != old.min_proto || next.max_proto != old.max_proto {
            gone.extend(self.hubs.values().filter(|h| !next.serves(h.link_proto)).map(|h| h.id));
        }
        for id in &gone {
            self.close_hub(*id);
        }
        // Turning "separate Android public room" off merges the hubs already in the phone room into
        // the public room. (Turning it back on cannot identify the Android hubs among the public ones.)
        let mut merged = 0usize;
        if old.separate_phones && !next.separate_phones {
            let phone = proto::phone_room();
            for h in self.hubs.values_mut() {
                if h.room == phone {
                    h.room = [0u8; 16];
                    merged += 1;
                }
            }
        }
        if old != next {
            self.note(format!(
                "admin: settings min-proto {} -> {}, max-proto {} -> {}, link-pool {} -> {}, separate-phones {} -> {} ({} hub(s) closed, {} merged)",
                old.min_proto, next.min_proto, old.max_proto, next.max_proto, old.pool_text(), next.pool_text(),
                old.separate_phones, next.separate_phones, gone.len(), merged
            ));
        }
        gone.len()
    }

    /// Drops a hub: its host's connection and writer channel close, and its listed state goes.
    pub(crate) fn close_hub(&mut self, id: u64) -> bool {
        self.pending.remove(&id);
        let Some(h) = self.hubs.remove(&id) else { return false };
        if h.code != [0u8; proto::CODE_LEN] && self.codes.get(&h.code) == Some(&id) {
            self.codes.remove(&h.code);
        }
        if let Some(st) = self.ips.get_mut(&h.ip) {
            st.hubs = st.hubs.saturating_sub(1);
        }
        // Tell the host its hub is gone, then close the connection after the writer had a moment to deliver it.
        let _ = h.out.try_send(mhgu_protocol::frame(proto::T_ERROR, &[&1u32.to_le_bytes()]));
        thread::spawn(move || {
            thread::sleep(Duration::from_millis(100));
            let _ = h.conn.shutdown(Shutdown::Both);
        });
        self.note(format!("admin: closed hub {id:#x}"));
        true
    }
}

/// The hub server. Open it with [`Server::new`], then [`Server::serve`].
pub struct Server {
    listener: TcpListener,
    udp: Arc<UdpSocket>,
    shared: Arc<Mutex<Shared>>,
}

impl Server {
    pub fn new(mut cfg: Config) -> io::Result<Server> {
        federation::load_state(&mut cfg);
        // A peer without a token uses the server's token (both ends of a small deployment often share one).
        let own_token = cfg.federation_token.clone();
        for p in &mut cfg.federate {
            if p.token.is_empty() {
                p.token = own_token.clone();
            }
        }
        let bind = if cfg.addr.starts_with(':') {
            format!("0.0.0.0{}", cfg.addr)
        } else {
            cfg.addr.clone()
        };
        let listener = TcpListener::bind(&bind)?;
        let local = listener.local_addr()?;
        let udp = Arc::new(UdpSocket::bind(SocketAddr::new(local.ip(), local.port()))?);
        let key_room = if cfg.key.trim_matches(' ').is_empty() {
            None
        } else {
            Some(proto::room_from_key(&cfg.key))
        };
        let live = Live::from_config(&cfg);
        let identity = Arc::new(crate::identity::Identity::load(&cfg.identity_file)?);
        let fed = federation::FedState::new(&cfg.federate, &cfg.federation_token, Arc::clone(&identity));
        let shared = Arc::new(Mutex::new(Shared {
            key_room,
            live,
            cfg,
            hubs: HashMap::new(),
            codes: HashMap::new(),
            online: HashMap::new(),
            listed_by: HashMap::new(),
            pending: HashMap::new(),
            pairs: HashMap::new(),
            ips: HashMap::new(),
            conns: 0,
            banned: HashSet::new(),
            log: VecDeque::new(),
            fed,
            bridges: HashMap::new(),
            bridge_by_remote: HashMap::new(),
            udp: Arc::clone(&udp),
            identity,
            presence_subs: Vec::new(),
            next_sub: 0,
        }));
        Ok(Server { listener, udp, shared })
    }

    /// Live settings as they are now.
    pub fn live(&self) -> Live {
        self.shared.lock().unwrap().live()
    }

    /// Applies settings without a restart (the admin page and tests); returns the hubs closed by a raised min_proto.
    pub fn apply(&self, live: Live) -> usize {
        self.shared.lock().unwrap().apply_live(live)
    }

    /// This server's `ed25519:<fingerprint>` identity.
    pub fn identity_id(&self) -> String {
        self.shared.lock().unwrap().identity.id.clone()
    }

    /// The `/.well-known/mhgu-federation` document (also served on the web port).
    pub fn well_known(&self) -> String {
        self.shared.lock().unwrap().well_known()
    }

    /// LAN discovery candidates (never peers).
    pub fn candidates(&self) -> Vec<CandidateRow> {
        self.shared
            .lock()
            .unwrap()
            .fed
            .candidate_views()
            .into_iter()
            .map(|c| CandidateRow {
                id: c.id,
                name: c.name,
                addr: c.addr,
                age: c.seen.elapsed().as_secs(),
                configured: c.configured,
                public_key: crate::identity::hex(&c.public),
            })
            .collect()
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
        if self.shared.lock().unwrap().cfg.discover_lan {
            let shared = Arc::clone(&self.shared);
            thread::spawn(move || federation::discovery(shared));
        }
        if self.shared.lock().unwrap().cfg.federation_presence {
            let shared = Arc::clone(&self.shared);
            thread::spawn(move || federation::presence_supervisor(shared));
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
    let live = guard.live.clone();
    let Shared { ips, conns, banned, .. } = &mut *guard;
    let st = ips.entry(ip).or_insert_with(|| IpState {
        conns: 0,
        hubs: 0,
        req: TokenBucket::new(live.req_rate, live.req_burst),
        seen: Instant::now(),
    });
    st.seen = Instant::now();
    if *conns >= live.max_conns || st.conns >= live.max_conns_per_ip {
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
    if t == proto::T_FED_HELLO2 {
        let _ = federation::serve_peer2(c, &shared, ip, t, &p);
        return;
    }
    if t == proto::T_FED_HELLO {
        let _ = federation::serve_peer(c, &shared, ip, t, &p);
        return;
    }
    let hdr = match proto::parse_header(&p) {
        Ok(h) => h,
        Err(proto::HeaderError::Version) => {
            let _ = send_error(&mut c, proto::ERR_VERSION);
            return;
        }
        Err(proto::HeaderError::BadMagic) => return,
    };
    let serves = {
        let s = shared.lock().unwrap();
        s.live.serves(hdr.link_proto)
    };
    if !serves {
        if t == proto::T_HOST {
            let mut s = shared.lock().unwrap();
            if s.live.verbose {
                s.note(format!("hub refused (protocol {}, {})", hdr.link_proto, room_name(hdr.room)));
            }
        }
        let _ = send_error(&mut c, proto::ERR_VERSION);
        return;
    }
    let v4 = hdr.link_proto >= proto::LINK_PROTO_SESSIONS;
    // Android's public hubs live in the key "public-phone"'s room, as the game asks; the server may put them in the
    // public room instead, so phones and PCs without a key see each other's hubs.
    let mut room = hdr.room;
    if !shared.lock().unwrap().live.separate_phones && room == proto::phone_room() {
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
            // NetworkInfo [, code[8]]; bytes after those are ignored (a newer game may add some).
            let mut code = [0u8; proto::CODE_LEN];
            if v4 && body.len() >= proto::NETINFO_SIZE + proto::CODE_LEN {
                let raw = &body[proto::NETINFO_SIZE..proto::NETINFO_SIZE + proto::CODE_LEN];
                if raw.iter().any(|&b| b != 0) {
                    if !proto::code_ok(raw) {
                        let _ = send_error(&mut c, proto::ERR_BAD_REQ);
                        return;
                    }
                    code.copy_from_slice(raw);
                }
            }
            if body.len() < proto::NETINFO_SIZE || !proto::info_ok(&body[..proto::NETINFO_SIZE]) {
                let _ = send_error(&mut c, proto::ERR_BAD_REQ);
                return;
            }
            host(c, &shared, ip, hdr.link_proto, room, &body[..proto::NETINFO_SIZE], code);
        }
        proto::T_ATTACH => {
            if body.len() < 8 {
                return;
            }
            let ticket = u64::from_le_bytes(body[..8].try_into().unwrap());
            attach(c, &shared, ticket);
        }
        proto::T_JOIN => {
            if body.len() < 8 {
                return;
            }
            if !allow_request(&shared, ip) {
                let _ = send_error(&mut c, proto::ERR_BUSY);
                return;
            }
            let id = u64::from_le_bytes(body[..8].try_into().unwrap());
            let code = if v4 && body.len() >= 8 + proto::CODE_LEN {
                let raw = &body[8..8 + proto::CODE_LEN];
                if raw.iter().any(|&b| b != 0) {
                    Some(raw)
                } else {
                    None
                }
            } else {
                None
            };
            // A session code is an invitation: with the right one the room need not match.
            let invited = code.map(|c| proto::code_ok(c)).unwrap_or(false);
            if !invited && key_mismatch(&shared, room) {
                let _ = send_error(&mut c, proto::ERR_NOT_FOUND);
                return;
            }
            let (local, remote) = {
                let s = shared.lock().unwrap();
                (s.hubs.contains_key(&id), s.fed.get(id))
            };
            if local {
                join(c, &shared, hdr.link_proto, room, id, code, false);
            } else if let Some(hub) = remote {
                let hops = hub.hops;
                federated_join(c, &shared, hub, room, hdr.link_proto, code, hops, false);
            } else {
                let _ = send_error(&mut c, proto::ERR_NOT_FOUND);
            }
        }
        proto::T_FIND => {
            if !v4 || body.len() < proto::CODE_LEN {
                let _ = send_error(&mut c, proto::ERR_BAD_REQ);
                return;
            }
            if !allow_request(&shared, ip) {
                let _ = send_error(&mut c, proto::ERR_BUSY);
                return;
            }
            find(&mut c, &shared, hdr.link_proto, &body[..proto::CODE_LEN]);
        }
        proto::T_PRESENCE => {
            if !v4 {
                let _ = send_error(&mut c, proto::ERR_BAD_REQ);
                return;
            }
            if !allow_request(&shared, ip) {
                let _ = send_error(&mut c, proto::ERR_BUSY);
                return;
            }
            presence(&mut c, &shared, body);
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

fn write_frame(c: &mut TcpStream, t: u8, payloads: &[&[u8]]) -> io::Result<()> {
    proto::write_frame(c, &proto::frame(t, payloads))
}

/// The `MHGR` relay prefix for a pair.
fn relay_prefix(pair: u32, secret: u64) -> [u8; proto::UDP_HEADER] {
    let mut p = [0u8; proto::UDP_HEADER];
    p[..4].copy_from_slice(&proto::UDP_MAGIC);
    p[4..8].copy_from_slice(&pair.to_le_bytes());
    p[8..16].copy_from_slice(&secret.to_le_bytes());
    p
}

// ---- LIST -----------------------------------------------------------------

type HubEntry = (u64, NetInfo, Instant, u16, [u8; proto::CODE_LEN]);

// A Hunter Search network, not a hub: 2-player, and 0xFF at advertise byte 0x1C where hubs have 1
// (the same test as the Go server's huntSearch; the account id at 0x14 is not looked at since protocol 4,
// when it became each install's own).
fn hunt_search(info: &NetInfo) -> bool {
    proto::hunt_search(info)
}

// At most max entries: a random choice by rank (so passcoded hubs only take the places open hubs with room
// leave), then sorted: open with room first, then the fullest, then the newest.
fn pick(mut es: Vec<HubEntry>, max: usize) -> Vec<HubEntry> {
    let free = |info: &NetInfo| info[proto::NI_NODE_COUNT] < info[proto::NI_NODE_MAX];
    // 0 room and no passcode, 1 room and a passcode, 2 full, 3 full with a passcode
    let rank = |info: &NetInfo| (if free(info) { 0 } else { 2 }) + u8::from(proto::passcoded(info));
    if es.len() > max {
        shuffle(&mut es);
        es.sort_by_key(|e| rank(&e.1));
        es.truncate(max);
    }
    es.sort_by(|a, b| {
        let (ra, rb) = (rank(&a.1), rank(&b.1));
        if ra != rb {
            return ra.cmp(&rb);
        }
        let (ca, cb) = (a.1[proto::NI_NODE_COUNT], b.1[proto::NI_NODE_COUNT]);
        if ca != cb {
            return cb.cmp(&ca);
        }
        b.2.cmp(&a.2)
    });
    es
}

/// This server's hubs in one room, as federation advertisements.
fn fed_local_hubs(shared: &Arc<Mutex<Shared>>, room: [u8; 16]) -> Vec<federation::RawHub> {
    let s = shared.lock().unwrap();
    s.hubs
        .values()
        .filter(|h| h.room == room && s.live.serves(h.link_proto))
        .take(proto::FED_HUBS_MAX)
        .map(|h| federation::RawHub {
            id: h.id,
            link_proto: h.link_proto,
            code: h.code,
            info: h.info,
            origin: s.identity.id.clone(),
            hops: 0,
        })
        .collect()
}

/// The hub holding a session code, as federation advertisements.
fn fed_local_code(shared: &Arc<Mutex<Shared>>, code: &[u8; proto::CODE_LEN]) -> Vec<federation::RawHub> {
    let s = shared.lock().unwrap();
    s.codes
        .get(code)
        .and_then(|id| s.hubs.get(id))
        .filter(|h| s.live.serves(h.link_proto))
        .map(|h| {
            vec![federation::RawHub {
                id: h.id,
                link_proto: h.link_proto,
                code: h.code,
                info: h.info,
                origin: s.identity.id.clone(),
                hops: 0,
            }]
        })
        .unwrap_or_default()
}

/// Asks peers about a room whose cache is stale, in parallel, and merges what they answer.
fn refresh_federation(shared: &Arc<Mutex<Shared>>, room: [u8; 16]) {
    let (name, peers, stale, avoid, transitive, self_id) = {
        let mut s = shared.lock().unwrap();
        let (_, stale) = s.fed.room_answers(room);
        if stale.is_empty() {
            return;
        }
        let peers: Vec<(usize, federation::PeerState)> =
            stale.iter().map(|i| (*i, s.fed.peers[*i].clone())).collect();
        let avoid: HashSet<u64> = s.hubs.keys().copied().collect();
        (s.cfg.federation_name.clone(), peers, stale, avoid, s.cfg.federation_transitive, s.identity.id.clone())
    };
    let timeout = if transitive > 0 {
        federation::ext_timeout(transitive) + Duration::from_millis(300)
    } else {
        federation::REQUEST_TIMEOUT
    };
    let (tx, rx) = mpsc::channel();
    for (idx, mut peer) in peers {
        let tx = tx.clone();
        let name = name.clone();
        let path = vec![self_id.clone()];
        thread::spawn(move || {
            let r = if transitive > 0 {
                federation::list_ext(&mut peer, room, transitive, &path, &name)
            } else {
                federation::list(&mut peer, room, &name)
            };
            let _ = tx.send((idx, r));
        });
    }
    drop(tx);
    let deadline = Instant::now() + timeout + Duration::from_millis(150);
    let mut answered = HashSet::new();
    loop {
        let left = deadline.saturating_duration_since(Instant::now());
        if left.is_zero() {
            break;
        }
        match rx.recv_timeout(left) {
            Ok((idx, r)) => {
                answered.insert(idx);
                let (answer, rtt) = match r {
                    Ok((hubs, rtt)) => (Ok(hubs), rtt),
                    Err(e) => (Err(format!("{e}")), 0),
                };
                shared.lock().unwrap().fed.ingest(idx, room, answer, rtt, &avoid);
                if answered.len() == stale.len() {
                    break;
                }
            }
            Err(_) => break,
        }
    }
    for idx in stale {
        if !answered.contains(&idx) {
            // Record the timeout so the next search does not wait on it again straight away.
            shared
                .lock()
                .unwrap()
                .fed
                .ingest(idx, room, Err("timeout".into()), 0, &avoid);
        }
    }
}

/// Remote hubs in a room that fit a client's link-protocol group, deduplicated against the local ones.
fn federated_list_entries(
    shared: &Arc<Mutex<Shared>>,
    room: [u8; 16],
    link_proto: u16,
    local: &[HubEntry],
) -> Vec<HubEntry> {
    let (group, serves, remote) = {
        let s = shared.lock().unwrap();
        (s.live.group(link_proto), s.live.clone(), s.fed.in_room(&room))
    };
    let mut seen: HashSet<([u8; 16], [u8; 6])> = local
        .iter()
        .map(|e| {
            let mut sid = [0u8; 16];
            sid.copy_from_slice(&e.1[proto::NI_SESSION_ID..proto::NI_SESSION_ID + 16]);
            let mut mac = [0u8; 6];
            mac.copy_from_slice(&e.1[proto::NI_BSSID..proto::NI_BSSID + 6]);
            (sid, mac)
        })
        .collect();
    let mut out = Vec::new();
    for r in remote {
        if !serves.serves(r.link_proto) || serves.group(r.link_proto) != group {
            continue;
        }
        let mut sid = [0u8; 16];
        sid.copy_from_slice(&r.info[proto::NI_SESSION_ID..proto::NI_SESSION_ID + 16]);
        let mut mac = [0u8; 6];
        mac.copy_from_slice(&r.info[proto::NI_BSSID..proto::NI_BSSID + 6]);
        if !seen.insert((sid, mac)) {
            continue;
        }
        out.push((r.alias, r.info, r.created, r.link_proto, r.code));
    }
    out
}

/// Asks peers for a session code (the first hit wins; local codes are checked before this).
fn federated_find(shared: &Arc<Mutex<Shared>>, code: &[u8; proto::CODE_LEN]) -> Option<federation::RemoteHub> {
    let (peers, avoid, name, transitive, self_id) = {
        let s = shared.lock().unwrap();
        if s.fed.peers.is_empty() {
            return None;
        }
        (
            s.fed.peers.iter().cloned().enumerate().collect::<Vec<_>>(),
            s.hubs.keys().copied().collect::<HashSet<u64>>(),
            s.cfg.federation_name.clone(),
            s.cfg.federation_transitive,
            s.identity.id.clone(),
        )
    };
    let path = vec![self_id];
    let (tx, rx) = mpsc::channel();
    for (idx, mut peer) in peers {
        let tx = tx.clone();
        let name = name.clone();
        let code = *code;
        let path = path.clone();
        thread::spawn(move || {
            let r = if transitive > 0 {
                federation::find_ext(&mut peer, &code, transitive, &path, &name)
            } else {
                federation::find(&mut peer, &code, &name).map(|hubs| (hubs, 0))
            };
            let _ = tx.send((idx, r));
        });
    }
    drop(tx);
    let wait = if transitive > 0 {
        federation::ext_timeout(transitive) + Duration::from_millis(450)
    } else {
        federation::REQUEST_TIMEOUT + Duration::from_millis(150)
    };
    let deadline = Instant::now() + wait;
    loop {
        let left = deadline.saturating_duration_since(Instant::now());
        if left.is_zero() {
            break;
        }
        match rx.recv_timeout(left) {
            Ok((idx, r)) => {
                if let Ok((hubs, _)) = r {
                    let mut s = shared.lock().unwrap();
                    let aliases = s.fed.ingest_find(idx, hubs, &avoid);
                    if let Some(alias) = aliases.first().copied() {
                        return s.fed.get(alias);
                    }
                }
            }
            Err(_) => break,
        }
    }
    None
}

pub(super) fn federated_join(
    mut c: TcpStream,
    shared: &Arc<Mutex<Shared>>,
    hub: federation::RemoteHub,
    room: [u8; 16],
    link_proto: u16,
    code: Option<&[u8]>,
    budget: u8,
    fed_player: bool,
 ) {
    let (cfg, live, mut peer, name) = {
        let s = shared.lock().unwrap();
        let Some(peer) = s.fed.peers.get(hub.peer).cloned() else {
            let _ = send_error(&mut c, proto::ERR_NOT_FOUND);
            return;
        };
        (s.cfg.clone(), s.live.clone(), peer, s.cfg.federation_name.clone())
    };
    if !live.serves(link_proto) {
        let _ = send_error(&mut c, proto::ERR_VERSION);
        return;
    }
    if live.group(hub.link_proto) != live.group(link_proto) {
        let _ = send_error(&mut c, proto::ERR_NOT_FOUND);
        return;
    }
    let invited = code
        .filter(|k| k.len() >= proto::CODE_LEN && proto::code_ok(k))
        .map(|k| hub.code != [0u8; proto::CODE_LEN] && k[..proto::CODE_LEN] == hub.code)
        .unwrap_or(false);
    if !invited && hub.room != room {
        let _ = send_error(&mut c, proto::ERR_NOT_FOUND);
        return;
    }
    let code8 = code.and_then(|k| k.get(..proto::CODE_LEN)).map(|k| {
        let mut a = [0u8; proto::CODE_LEN];
        a.copy_from_slice(k);
        a
    });
    let timeout = cfg.handshake
        + Duration::from_secs(5)
        + if budget > 0 { federation::ext_timeout(budget) } else { Duration::ZERO };
    let joined = if hub.hops == 0 {
        federation::join(&mut peer, room, link_proto, hub.id, code8.as_ref(), &name, timeout)
    } else if budget > 0 {
        federation::join_ext(&mut peer, room, link_proto, &hub.origin, hub.id, code8.as_ref(), budget, &name, timeout)
    } else {
        Err(federation::remote_error(proto::ERR_NOT_FOUND))
    };
    let (stream, remote_pair, remote_secret) =
        match joined {
            Ok(v) => v,
            Err(e) => {
                let code = match federation::error_code(&e) {
                    Some(proto::ERR_NOT_FOUND) => proto::ERR_NOT_FOUND,
                    Some(proto::ERR_BUSY) => proto::ERR_BUSY,
                    Some(proto::ERR_VERSION) => proto::ERR_VERSION,
                    _ => proto::ERR_NO_ANSWER,
                };
                if cfg.verbose {
                    let mut s = shared.lock().unwrap();
                    s.note(format!("federation: join on {}: {e}", peer.name));
                }
                let _ = send_error(&mut c, code);
                return;
            }
        };
    let Some(peer_udp) = peer.addr else {
        let _ = send_error(&mut c, proto::ERR_NOT_FOUND);
        return;
    };
    let (local_pair, local_secret) = {
        let mut s = shared.lock().unwrap();
        let mut pair = 0u32;
        while pair == 0 || s.pairs.contains_key(&pair) || s.bridges.contains_key(&pair) {
            pair = rand64() as u32;
        }
        let mut secret = 0u64;
        while secret == 0 {
            secret = rand64();
        }
        let bandwidth = s.live.bandwidth;
        let bridge = Arc::new(federation::Bridge::new(
            pair,
            secret,
            remote_pair,
            remote_secret,
            hub.peer,
            peer_udp,
            bandwidth,
            fed_player,
        ));
        s.bridge_by_remote.insert((remote_pair, remote_secret), pair);
        s.bridges.insert(pair, bridge);
        s.fed.joins += 1;
        (pair, secret)
    };
    if write_frame(&mut c, proto::T_SPLICED, &[&local_pair.to_le_bytes(), &local_secret.to_le_bytes()])
        .is_err()
    {
        shared.lock().unwrap().bridges.remove(&local_pair);
        shared.lock().unwrap().bridge_by_remote.remove(&(remote_pair, remote_secret));
        return;
    }
    // Tell the peer where our UDP is (an empty relayed datagram, as a game registers).
    let reg = relay_prefix(remote_pair, remote_secret);
    let _ = shared.lock().unwrap().udp.send_to(&reg, peer_udp);
    if cfg.verbose {
        let mut s = shared.lock().unwrap();
        s.note(format!("federation: joined hub {:#x} on {}", hub.id, peer.name));
    }
    splice(c, stream, cfg.bandwidth, None);
    let mut s = shared.lock().unwrap();
    s.bridges.remove(&local_pair);
    s.bridge_by_remote.remove(&(remote_pair, remote_secret));
}

fn list(c: &mut TcpStream, shared: &Arc<Mutex<Shared>>, link_proto: u16, room: [u8; 16]) {
    let (mut hubs, mut searches, list_max, search_max) = {
        let s = shared.lock().unwrap();
        let group = s.live.group(link_proto);
        let mut hubs = Vec::new();
        let mut searches = Vec::new();
        for h in s.hubs.values() {
            if s.live.group(h.link_proto) != group || h.room != room {
                continue;
            }
            let e = (h.id, h.info, h.created, h.link_proto, h.code);
            if hunt_search(&h.info) { searches.push(e) } else { hubs.push(e) }
        }
        (hubs, searches, s.live.list_max, s.live.list_search_max)
    };
    // Federation: add the hubs a peer listed in this room. Local hubs win a duplicate (a hub seen both ways).
    if !key_mismatch(shared, room) {
        refresh_federation(shared, room);
        let local: Vec<HubEntry> = hubs.iter().chain(searches.iter()).cloned().collect();
        let remote = federated_list_entries(shared, room, link_proto, &local);
        for e in remote {
            if hunt_search(&e.1) {
                searches.push(e);
            } else {
                hubs.push(e);
            }
        }
    }
    let mut es = pick(hubs, list_max);
    es.extend(pick(searches, search_max));
    write_hubs(c, link_proto, &es);
}

/// The answer to a list (or find): HUBS for a game before protocol 4, HUBS4 (with each hub's link protocol and
/// session code) for one of 4 and later.
fn write_hubs(c: &mut TcpStream, link_proto: u16, es: &[HubEntry]) {
    let v4 = link_proto >= proto::LINK_PROTO_SESSIONS;
    let entry = 8 + if v4 { 2 + proto::CODE_LEN } else { 0 } + proto::NETINFO_SIZE;
    let mut out = Vec::with_capacity(4 + es.len() * entry);
    out.extend_from_slice(&(es.len() as u32).to_le_bytes());
    for (id, info, _, hub_proto, code) in es {
        out.extend_from_slice(&id.to_le_bytes());
        if v4 {
            out.extend_from_slice(&hub_proto.to_le_bytes());
            out.extend_from_slice(code);
        }
        out.extend_from_slice(info);
    }
    let frame = proto::frame(if v4 { proto::T_HUBS4 } else { proto::T_HUBS }, &[&out]);
    let _ = proto::write_frame(c, &frame);
}

/// FIND (protocol 4): the hub holding a session code, whatever its room (a code is an invitation).
fn find(c: &mut TcpStream, shared: &Arc<Mutex<Shared>>, link_proto: u16, code: &[u8]) {
    let mut key = [0u8; proto::CODE_LEN];
    key.copy_from_slice(&code[..proto::CODE_LEN]);
    let mut es = {
        let s = shared.lock().unwrap();
        let group = s.live.group(link_proto);
        s.codes.get(&key).and_then(|id| s.hubs.get(id)).filter(|h| s.live.group(h.link_proto) == group).map(|h| {
            vec![(h.id, h.info, h.created, h.link_proto, h.code)]
        }).unwrap_or_default()
    };
    if es.is_empty() {
        // A code is an invitation across federation too; the peer checks its room only if the code matches.
        if let Some(h) = federated_find(shared, &key) {
            es.push((h.alias, h.info, h.created, h.link_proto, h.code));
        }
    }
    write_hubs(c, link_proto, &es);
}

// ---- HOST -----------------------------------------------------------------

enum Reserved {
    Ok(u64),
    Busy,
    CodeTaken,
}

fn host(
    mut c: TcpStream,
    shared: &Arc<Mutex<Shared>>,
    ip: IpAddr,
    link_proto: u16,
    room: [u8; 16],
    info: &[u8],
    code: [u8; proto::CODE_LEN],
) {
    let cfg = shared.lock().unwrap().cfg.clone();
    let host_conn = match c.try_clone() {
        Ok(h) => h,
        Err(_) => return,
    };
    let (tx, rx) = mpsc::sync_channel::<Vec<u8>>(16);
    let has_code = code.iter().any(|&b| b != 0);

    // Reserve the slot under one lock (avoids a TOCTOU with the limits).
    let mut note_after: Option<String> = None;
    let reserved = {
        let mut guard = shared.lock().unwrap();
        let Shared { live: live_ref, hubs, codes, ips, .. } = &mut *guard;
        let mut ninfo = [0u8; proto::NETINFO_SIZE];
        ninfo.copy_from_slice(info);
        // The same hub (session id and BSSID) listing again takes its code back from the listing it left.
        let mut taken = false;
        let mut stale: Option<u64> = None;
        if has_code {
            if let Some(id) = codes.get(&code).copied() {
                if let Some(old) = hubs.get(&id) {
                    let same = old.info[proto::NI_SESSION_ID..proto::NI_BSSID + 6]
                        == ninfo[proto::NI_SESSION_ID..proto::NI_BSSID + 6];
                    if same {
                        stale = Some(id);
                    } else {
                        taken = true;
                    }
                }
            }
        }
        let slot = ips.get_mut(&ip);
        match slot {
            _ if taken => Reserved::CodeTaken,
            Some(st) if hubs.len() < live_ref.max_hubs && st.hubs < live_ref.max_hubs_per_ip => {
                if let Some(id) = stale {
                    // (the same hub's old listing goes now; its reader ends and cleans up without the code)
                    if let Some(old) = hubs.remove(&id) {
                        let _ = old.conn.shutdown(Shutdown::Both);
                        st.hubs = st.hubs.saturating_sub(1);
                    }
                }
                st.hubs += 1;
                let mut cand = 0u64;
                while cand == 0 || hubs.contains_key(&cand) {
                    cand = rand64();
                }
                hubs.insert(
                    cand,
                    Hub {
                        id: cand,
                        room,
                        link_proto,
                        code,
                        info: ninfo,
                        created: Instant::now(),
                        out: tx,
                        conn: host_conn,
                        ip,
                        joins: 0,
                    },
                );
                if has_code {
                    codes.insert(code, cand);
                }
                if live_ref.verbose {
                    note_after = Some(format!(
                        "hub {cand:#x} listed (protocol {link_proto}, {}{})",
                        room_name(room),
                        if has_code { format!(", code {}", proto::code_shown(&code)) } else { String::new() }
                    ));
                }
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
        Reserved::CodeTaken => {
            let _ = send_error(&mut c, proto::ERR_CODE_TAKEN);
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
    if let Some(h) = s.hubs.remove(&id) {
        if h.code != [0u8; proto::CODE_LEN] && s.codes.get(&h.code) == Some(&id) {
            s.codes.remove(&h.code);
        }
    }
    if let Some(st) = s.ips.get_mut(&ip) {
        st.hubs = st.hubs.saturating_sub(1);
    }
}

/// PRESENCE (protocol 4): the asker is online (doing `state`, hosting or in the hub with `code`) until
/// `-presence-ttl` after this. The answer: which of the friends it lists are online and list it back, and what
/// they do; then its friend requests (the players online that list it while it doesn't list them, with the
/// names they go by; the newest 8); then, only when there are any, the players it lists that removed it.
///
/// Body: secret[32], u8 state, code[8] (zeros: none), u8 n, n x friend code[10], and optionally u8 len,
/// name[len], and after the name optionally u8 m, m x friend code[10] (the players it removed). Anything
/// after that is ignored; the limits are taken as far as they go, never refused.
fn presence(c: &mut TcpStream, shared: &Arc<Mutex<Shared>>, body: &[u8]) {
    const FIXED: usize = proto::FRIEND_SECRET_LEN + 1 + proto::CODE_LEN + 1;
    if body.len() < FIXED {
        let _ = send_error(c, proto::ERR_BAD_REQ);
        return;
    }
    let n = body[FIXED - 1] as usize;
    if body.len() < FIXED + n * proto::FRIEND_LEN {
        let _ = send_error(c, proto::ERR_BAD_REQ);
        return;
    }
    let secret = &body[..proto::FRIEND_SECRET_LEN];
    let state = body[proto::FRIEND_SECRET_LEN];
    let raw_code = &body[proto::FRIEND_SECRET_LEN + 1..proto::FRIEND_SECRET_LEN + 1 + proto::CODE_LEN];
    let mut code = [0u8; proto::CODE_LEN];
    if proto::code_ok(raw_code) {
        code.copy_from_slice(raw_code);
    }
    let mut removed = HashSet::new();
    let mut name = String::new();
    let end = FIXED + n * proto::FRIEND_LEN;
    if body.len() > end {
        let nl = body[end] as usize;
        if body.len() >= end + 1 + nl {
            name = clean_name(&body[end + 1..end + 1 + nl]);
            let at = end + 1 + nl;
            if body.len() > at {
                let m = (body[at] as usize)
                    .min((body.len() - at - 1) / proto::FRIEND_LEN)
                    .min(proto::FRIEND_MAX);
                for i in 0..m {
                    let mut f = [0u8; proto::FRIEND_LEN];
                    f.copy_from_slice(&body[at + 1 + i * proto::FRIEND_LEN..at + 1 + (i + 1) * proto::FRIEND_LEN]);
                    removed.insert(f);
                }
            }
        }
    }
    let id = proto::friend_id(secret);
    let mut friends = HashSet::new();
    for i in 0..n.min(proto::FRIEND_MAX) {
        let mut f = [0u8; proto::FRIEND_LEN];
        f.copy_from_slice(&body[FIXED + i * proto::FRIEND_LEN..FIXED + (i + 1) * proto::FRIEND_LEN]);
        if f != id {
            friends.insert(f);
        }
    }
    let now = Instant::now();
    let max_presence = shared.lock().unwrap().cfg.max_presence;
    let mut out = Vec::new();
    let mut gone = Vec::new();
    let mut count = 0usize;
    let mut reqs: Vec<([u8; proto::FRIEND_LEN], String, Instant)> = Vec::new();
    {
        let mut s = shared.lock().unwrap();
        let ttl = s.cfg.presence_ttl;
        let dead = s.prune_presence(now);
        for t in &dead {
            broadcast_local_offline(&s, t);
        }
        if !s.online.contains_key(&id) && s.online.len() >= max_presence {
            drop(s);
            let _ = send_error(c, proto::ERR_BUSY);
            return;
        }
        if let Some(old) = s.online.remove(&id) {
            s.unlist(id, &old.friends, &friends);
        }
        for f in &friends {
            s.listed_by.entry(*f).or_default().entry(id).or_insert(now);
        }
        let update = federation::PresenceUpdate {
            id,
            state,
            code,
            friends: friends.iter().copied().collect(),
            removed: removed.iter().copied().collect(),
            name: name.clone(),
            origin: s.identity.id.clone(),
            hops: 0,
            gone: false,
        };
        let me = Presence { friends: friends.clone(), removed, state, code, name: name.clone(), seen: now };
        s.online.insert(id, me);
        for (_, tx, _) in &s.presence_subs {
            let _ = tx.try_send(update.clone()); // (full: this poll's update is lost, the next one arrives)
        }
        let mut answered = HashSet::new();
        for f in &friends {
            let Some(o) = s.online.get(f) else { continue };
            if o.seen.elapsed() > ttl {
                continue;
            }
            answered.insert(*f);
            if o.removed.contains(&id) {
                // (it removed the asker: the asker's game drops it too)
                gone.push(*f);
                continue;
            }
            if !o.friends.contains(&id) {
                continue;
            }
            out.extend_from_slice(f);
            out.push(o.state);
            out.extend_from_slice(&o.code);
            count += 1;
        }
        // Friends online on a federation peer (same mutual rules, their state as last sent).
        for f in &friends {
            if answered.contains(f) {
                continue;
            }
            let Some(r) = s.fed.remote_presence.get(f) else { continue };
            if r.hops > s.cfg.federation_transitive || now.duration_since(r.seen) > ttl {
                continue;
            }
            if r.removed.contains(&id) {
                gone.push(*f);
                continue;
            }
            if !r.friends.contains(&id) {
                continue;
            }
            out.extend_from_slice(f);
            out.push(r.state);
            out.extend_from_slice(&r.code);
            count += 1;
        }
        let mut req_ids = HashSet::new();
        if let Some(list) = s.listed_by.get(&id) {
            for (r, since) in list {
                if friends.contains(r) {
                    continue;
                }
                let Some(o) = s.online.get(r) else { continue };
                if o.seen.elapsed() > ttl {
                    continue;
                }
                req_ids.insert(*r);
                reqs.push((*r, o.name.clone(), *since));
            }
        }
        for (rid, r) in &s.fed.remote_presence {
            if *rid == id || friends.contains(rid) || req_ids.contains(rid) || s.online.contains_key(rid) {
                continue;
            }
            if r.hops > s.cfg.federation_transitive
                || now.duration_since(r.seen) > ttl
                || r.removed.contains(&id)
                || !r.friends.contains(&id)
            {
                continue;
            }
            reqs.push((*rid, r.name.clone(), r.seen));
        }
    }
    reqs.sort_by(|a, b| b.2.cmp(&a.2));
    reqs.truncate(proto::FRIEND_REQUESTS_MAX);
    let mut tail = vec![reqs.len() as u8];
    for (r, name, _) in &reqs {
        tail.extend_from_slice(r);
        let nb = name.as_bytes();
        let nb = &nb[..nb.len().min(255)];
        tail.push(nb.len() as u8);
        tail.extend_from_slice(nb);
    }
    if !gone.is_empty() {
        tail.push(gone.len() as u8);
        for f in &gone {
            tail.extend_from_slice(f);
        }
    }
    let frame = proto::frame(proto::T_FRIENDS, &[&[count as u8], &out, &tail]);
    let _ = proto::write_frame(c, &frame);
}

/// A name from PRESENCE as the game sent it: valid UTF-8, no control characters, at most `FRIEND_NAME_MAX`
/// bytes cut at a whole character.
fn clean_name(b: &[u8]) -> String {
    let mut t = String::new();
    for ch in String::from_utf8_lossy(b).chars() {
        if (ch as u32) < 0x20 || ch as u32 == 0x7F {
            continue;
        }
        t.push(ch);
    }
    while t.len() > proto::FRIEND_NAME_MAX {
        let _ = t.pop();
    }
    t
}

fn broadcast_local_offline(s: &Shared, t: &federation::PresenceUpdate) {
    for (_, tx, sub) in &s.presence_subs {
        if sub.ext {
            let _ = tx.try_send(t.clone());
        }
    }
}

impl Shared {
    /// Drops players not seen for the configured presence TTL, and their listings.
    /// Every dropped player becomes a local tombstone for extended presence streams.
    fn prune_presence(&mut self, now: Instant) -> Vec<federation::PresenceUpdate> {
        let ttl = self.cfg.presence_ttl;
        let origin = self.identity.id.clone();
        let stale: Vec<[u8; proto::FRIEND_LEN]> = self
            .online
            .iter()
            .filter(|(_, p)| now.duration_since(p.seen) > ttl)
            .map(|(id, _)| *id)
            .collect();
        let mut out = Vec::new();
        for id in stale {
            if let Some(p) = self.online.remove(&id) {
                self.unlist(id, &p.friends, &HashSet::new());
                out.push(federation::PresenceUpdate {
                    id,
                    state: 0,
                    code: [0u8; proto::CODE_LEN],
                    friends: Vec::new(),
                    removed: Vec::new(),
                    name: String::new(),
                    origin: origin.clone(),
                    hops: 0,
                    gone: true,
                });
            }
        }
        out
    }

    /// Takes `a`'s listings of `old` out of `listed_by` (except those in `keep`).
    fn unlist(&mut self, a: [u8; proto::FRIEND_LEN], old: &HashSet<[u8; proto::FRIEND_LEN]>, keep: &HashSet<[u8; proto::FRIEND_LEN]>) {
        for f in old {
            if keep.contains(f) {
                continue;
            }
            let empty = match self.listed_by.get_mut(f) {
                Some(m) => {
                    m.remove(&a);
                    m.is_empty()
                }
                None => false,
            };
            if empty {
                self.listed_by.remove(f);
            }
        }
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
    code: Option<&[u8]>,
    fed_player: bool,
 ) {
    let (cfg, live) = {
        let g = shared.lock().unwrap();
        (g.cfg.clone(), g.live.clone())
    };
    let (host_tx, host_rx) = mpsc::sync_channel::<TcpStream>(1);

    let setup = {
        let mut s = shared.lock().unwrap();
        let Shared { hubs, pending, .. } = &mut *s;
        match hubs.get_mut(&id) {
            None => Joined::NotFound,
            Some(h) => {
                // A session code is an invitation: with the right one the room need not match.
                let invited = code
                    .filter(|c| proto::code_ok(c) && h.code != [0u8; proto::CODE_LEN])
                    .map(|c| c[..proto::CODE_LEN] == h.code)
                    .unwrap_or(false);
                let group = live.group(link_proto);
                if (!invited && h.room != room)
                    || live.group(h.link_proto) != group
                {
                    Joined::NotFound
                } else if h.joins >= (proto::MAX_NODES as usize - 1) {
                    Joined::Busy
                } else {
                    let mut cand = 0u64;
                    while cand == 0 || pending.contains_key(&cand) {
                        cand = rand64();
                    }
                    pending.insert(cand, host_tx);
                    h.joins += 1;
                    Joined::Ok(cand, h.out.clone())
                }
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
        let bandwidth = s.live.bandwidth;
        // The direct key is the pair's own: never 0 (a link treats 0 as "no key yet").
        let mut dkey = rand64();
        while dkey == 0 {
            dkey = rand64();
        }
        s.pairs.insert(
            pair_id,
            Arc::new(Pair {
                secret,
                addr: Mutex::new([None, None]),
                direct: Mutex::new([false, false]),
                dkey,
                lim: TokenBucket::new(bandwidth, bandwidth),
                hub: id,
                player: c.peer_addr().map(|a| a.ip().to_string()).unwrap_or_default(),
                socks: Mutex::new([None, None]),
                down: DirStat::new(),
                up: DirStat::new(),
                started: Instant::now(),
                fed_player,
            }),
        );
    }
    {
        let mut s = shared.lock().unwrap();
        if s.live.verbose {
            s.note(format!("pair {pair_id:#x} spliced (hub {id:#x}, player {})", c.peer_addr().map(|a| a.ip().to_string()).unwrap_or_default()));
        }
    }
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
    splice(host_conn, c, live.bandwidth, pair);
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
        // A federated player's leg: rewrite the relay prefix between our pair to the player and the
        // peer's pair to the host. Peer data carries its prefix (the peer marks the pair as federated).
        let bridge = {
            let s = shared.lock().unwrap();
            if let Some(b) = s.bridges.get(&id).filter(|b| b.local_secret == secret) {
                Some((Arc::clone(b), false, None))
            } else if let Some(local) = s.bridge_by_remote.get(&(id, secret)).copied() {
                let b = s.bridges.get(&local).cloned();
                let ip = b
                    .as_ref()
                    .and_then(|b| s.fed.peers.get(b.peer))
                    .and_then(|p| p.addr)
                    .map(|a| a.ip());
                b.map(|b| (b, true, ip))
            } else {
                None
            }
        };
        if let Some((b, from_peer, peer_ip)) = bridge {
            if from_peer {
                if peer_ip != Some(from.ip()) || n <= proto::UDP_HEADER {
                    continue;
                }
                if let Some(to) = *b.player.lock().unwrap() {
                    if b.down.take(n as f64) {
                        if b.player_is_peer {
                            // The player is another hub server: keep our prefix so it can route to its game.
                            let mut out = Vec::with_capacity(n);
                            out.extend_from_slice(&relay_prefix(b.local, b.local_secret));
                            out.extend_from_slice(&buf[proto::UDP_HEADER..n]);
                            let _ = udp.send_to(&out, to);
                        } else {
                            // The player's game expects the bare datagram, as from a normal hub server.
                            let _ = udp.send_to(&buf[proto::UDP_HEADER..n], to);
                        }
                        b.bytes_down.fetch_add(n as u64, Ordering::Relaxed);
                        shared.lock().unwrap().fed.bytes.fetch_add(n as u64, Ordering::Relaxed);
                    }
                }
            } else {
                *b.player.lock().unwrap() = Some(from);
                let query = n == proto::PEER_MSG_SIZE
                    && buf[proto::UDP_HEADER..proto::UDP_HEADER + 4] == proto::PEER_QUERY;
                if query {
                    continue; // (no hole punching for federated legs: both ends stay on the relay)
                }
                if n > proto::UDP_HEADER && b.up.take(n as f64) {
                    let mut out = Vec::with_capacity(n);
                    out.extend_from_slice(&relay_prefix(b.remote, b.remote_secret));
                    out.extend_from_slice(&buf[proto::UDP_HEADER..n]);
                    let _ = udp.send_to(&out, b.peer_udp);
                    b.bytes_up.fetch_add(n as u64, Ordering::Relaxed);
                    shared.lock().unwrap().fed.bytes.fetch_add(n as u64, Ordering::Relaxed);
                }
            }
            continue;
        }
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
        // Direct UDP (hole punching): a datagram of exactly PEER_MSG_SIZE whose payload starts with MHGQ
        // asks for the other end's public address; it is never relayed.
        let query = n == proto::PEER_MSG_SIZE
            && buf[proto::UDP_HEADER..proto::UDP_HEADER + 4] == proto::PEER_QUERY;
        let (to, both) = {
            let mut addr = pair.addr.lock().unwrap();
            addr[side] = Some(from); // NAT may change it: the latest wins
            let mut direct = pair.direct.lock().unwrap();
            if query {
                direct[side] = true; // this end may now see the other's address
            }
            (addr[1 - side], direct[0] && direct[1])
        };
        if query {
            // The answer goes to the asker only once both ends asked; a game that never asks (Direct =
            // Off, an older version, an older server) stays on the relay. Never bigger than the query.
            if both {
                if let Some(to) = to {
                    // (a v4 client on a v6 socket arrives as a mapped address)
                    let ip4 = match to.ip() {
                        IpAddr::V4(ip) => Some(ip),
                        IpAddr::V6(ip) => ip.to_ipv4_mapped(),
                    };
                    if let Some(ip) = ip4 {
                        if pair.lim.take(n as f64) {
                            let mut a = Vec::with_capacity(proto::PEER_MSG_SIZE);
                            a.extend_from_slice(&proto::PEER_ANSWER);
                            a.extend_from_slice(&id.to_le_bytes());
                            a.extend_from_slice(&secret.to_le_bytes());
                            a.extend_from_slice(&ip.octets());
                            a.extend_from_slice(&to.port().to_be_bytes());
                            a.extend_from_slice(&[0, 0]);
                            a.extend_from_slice(&pair.dkey.to_le_bytes());
                            let _ = udp.send_to(&a, from);
                        }
                    }
                }
            }
            continue; // (the query is not data: an older server would pass it on and the other end ignores it)
        }
        // An empty datagram only tells where its sender is.
        if n > proto::UDP_HEADER && to.is_some() && pair.lim.take(n as f64) {
            let to = to.unwrap();
            if side == 0 && pair.fed_player {
                // The "player" is another hub server: keep the prefix so it can route by pair.
                let mut out = Vec::with_capacity(n);
                out.extend_from_slice(&relay_prefix(id, pair.secret[1]));
                out.extend_from_slice(&buf[proto::UDP_HEADER..n]);
                let _ = udp.send_to(&out, to);
            } else {
                let _ = udp.send_to(&buf[proto::UDP_HEADER..n], to);
            }
        }
    }
}

// ---- housekeeping ---------------------------------------------------------

fn janitor(shared: Arc<Mutex<Shared>>) {
    loop {
        thread::sleep(Duration::from_secs(10));
        let mut s = shared.lock().unwrap();
        let now = Instant::now();
        let dead = s.prune_presence(now);
        for t in &dead {
            broadcast_local_offline(&s, t);
        }
        let ttl = s.cfg.presence_ttl;
        let remote_dead = s.fed.prune(ttl);
        for t in &remote_dead {
            federation::forward_remote(&s, t, "");
        }
        s.ips.retain(|_, st| {
            !(st.conns == 0 && st.hubs == 0 && now.duration_since(st.seen) > Duration::from_secs(300))
        });
        s.bridges.retain(|_, b| now.duration_since(b.created) < Duration::from_secs(86400));
    }
}

fn rand64() -> u64 {
    let mut b = [0u8; 8];
    getrandom::fill(&mut b).expect("OS randomness unavailable");
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn approving_a_candidate_persists_and_reloads() {
        let dir = std::env::temp_dir().join(format!("mhgu-hub-approve-{}", std::process::id()));
        let _ = std::fs::create_dir_all(&dir);
        let identity = dir.join("id");
        let state = dir.join("approvals");
        let mut cfg = Config::default();
        cfg.addr = "127.0.0.1:0".into();
        cfg.web = String::new();
        cfg.identity_file = identity.to_string_lossy().into_owned();
        cfg.federation_state = state.to_string_lossy().into_owned();
        let srv = Server::new(cfg.clone()).unwrap();
        let other = crate::identity::Identity::load("").unwrap();
        let other_id = other.id.clone();
        {
            let mut s = srv.shared.lock().unwrap();
            s.fed.candidate(
                other_id.clone(),
                "Hub B".into(),
                "127.0.0.1:39999".into(),
                other.public,
            );
            s.approve_candidate(&other_id).unwrap();
        }
        let text = std::fs::read_to_string(&state).unwrap();
        assert!(text.contains(&format!("allow {other_id}")));
        assert!(text.contains(&format!("peer {other_id} 127.0.0.1:39999 Hub B")));
        let srv2 = Server::new(cfg).unwrap();
        {
            let s = srv2.shared.lock().unwrap();
            assert!(s.cfg.federation_allow.iter().any(|a| a == &other_id));
            assert!(s.cfg.federate.iter().any(|p| p.key == other_id));
            assert!(s.fed.peers.iter().any(|p| p.cfg.key == other_id));
        }
        let _ = std::fs::remove_dir_all(dir);
    }
}
