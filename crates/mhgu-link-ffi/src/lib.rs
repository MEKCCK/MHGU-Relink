//! The Local Play link (`nn::ldn` over IP) as a C library: `mhgu-link.h` is the header the C runtime
//! includes, and this crate is `libmhgu_link.a`.
//!
//! Model (the one `runtime/net/ldn.c` uses): one player hosts — a TCP listener that speaks the in-game
//! link (`MHGL`/`MHGD` from `mhgu_protocol`) to every peer and relays their packets to the others —
//! and the rest join by address with `mhgu_link::Client`. Addresses are the ones a Switch expects:
//! the host is 169.254.0.1 in a /16, so its broadcast is 169.254.255.255.

use std::collections::VecDeque;
use std::ffi::{c_char, c_int, CStr};
use std::io::{self, Write};
use std::net::{IpAddr, Ipv4Addr, SocketAddr, TcpListener, TcpStream, UdpSocket};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::atomic::{AtomicU32, Ordering as AtomicOrdering};
use std::sync::{Arc, LazyLock, Mutex};
use std::time::{Duration, Instant};

use mhgu_link::scan;
use mhgu_link::Lobby;
use mhgu_link::{Client, Event, Hello, Reply};
use mhgu_protocol as proto;

/// The `NetworkInfo` blob's size (the C side's `NETINFO_SIZE`).
pub const NETINFO_SIZE: usize = proto::NETINFO_SIZE;

/// The `VPacket` header the C side passes (runtime/net/net.h, host byte order).
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct MhguVPacket {
    pub src_ip: u32,
    pub dst_ip: u32,
    pub src_port: u16,
    pub dst_port: u16,
    pub proto: u8,
    pub kind: u8,
    pub pad: u16,
}

pub type DeliverPacket = extern "C" fn(*const MhguVPacket, *const u8, u32);
pub type DeliverAux = extern "C" fn(*const u8, u32);

/// A peer of the link: its socket (shared with the sender) and its node slot.
struct Peer {
    /// The write side; the read loop owns its own handle so a blocking read never holds this lock.
    stream: Arc<Mutex<TcpStream>>,
    /// The peer's `Hello` (its name and MAC go into the network's node table).
    hello: Hello,
    ip: u32,
    /// The token of its WELCOME (identifies its `MHGD` datagrams), and where they come from.
    token: u32,
    udp: Option<SocketAddr>,
    udp_rx: std::time::Instant,
    /// Through a hub server: the relay pair and our secret for its datagrams (`MHGR`).
    relay: Option<(u32, u64)>,
    /// Direct UDP past the server (hole punching), when the link is relayed.
    direct: DirectState,
}

// ---- direct UDP past the hub server (hole punching) ---------------------------------------------

/// [Multiplayer] Direct for the next link: 0 keeps every game packet on the relay.
static DIRECT: AtomicBool = AtomicBool::new(true);

/// Without two-way proof of the direct path for this long, game packets go back to the relay
/// (direct hellos every second: three lost in a row).
const DIRECT_SILENCE: Duration = Duration::from_millis(3500);
/// Direct hellos every second once the path works.
const DIRECT_HELLO: Duration = Duration::from_secs(1);
/// Direct hello answers in a row needed to come back to direct after a fall (no flapping).
const DIRECT_REUP: u32 = 3;
/// Punches / hellos every 200 ms this long after the address came, then every 2 s.
const PUNCH_FAST: Duration = Duration::from_secs(15);
const PUNCH_EVERY: Duration = Duration::from_millis(200);
const PUNCH_SLOW: Duration = Duration::from_secs(2);
/// Queries every second this long after the link came up or direct fell, then every 15 s.
const QUERY_FAST: Duration = Duration::from_secs(30);
const QUERY_EVERY: Duration = Duration::from_secs(1);
const QUERY_SLOW: Duration = Duration::from_secs(15);

/// The direct state of one relayed link (ldn.c's `p_*` fields).
#[derive(Clone, Copy)]
struct DirectState {
    /// The ini option when the link came up, and the link is relayed.
    ok: bool,
    /// The other end's public address (from the server), and the pair's direct key (0: none yet).
    peer: Option<SocketAddr>,
    dkey: u64,
    /// Direct datagrams get through both ways: game packets go that way.
    up: bool,
    /// Last proof that the direct path works both ways.
    rx: Instant,
    /// Station: direct hello answers in a row (DIRECT_REUP after a fall); whether direct ever worked.
    good: u32,
    ever: bool,
    /// When the link came up / direct fell, and the query and punch schedules.
    born: Instant,
    q_since: Instant,
    p_since: Instant,
    next_query: Instant,
    next_punch: Instant,
    /// The logs that say why a pair has no direct path, once each.
    noanswer_said: bool,
    nopunch_said: bool,
}

impl DirectState {
    fn new(ok: bool) -> DirectState {
        let now = Instant::now();
        DirectState {
            ok,
            peer: None,
            dkey: 0,
            up: false,
            rx: now,
            good: 0,
            ever: false,
            born: now,
            q_since: now,
            p_since: now,
            next_query: now,
            next_punch: now,
            noanswer_said: false,
            nopunch_said: false,
        }
    }

    fn known(&self) -> bool {
        self.peer.is_some()
    }

    /// Game packets may go straight to the other end.
    fn usable(&self, now: Instant) -> bool {
        self.up && now.duration_since(self.rx) < DIRECT_SILENCE
    }

    /// MHGU_LDN_BLOCK_DIRECT=<s>[-<t>] (testing the fallback): direct datagrams are ignored from s
    /// seconds after a link came up on (0: always), until t seconds if given (a passing outage).
    fn blocked(&self) -> bool {
        let Some((from, to)) = block_window() else { return false };
        let t = self.born.elapsed();
        t >= from && to.map_or(true, |to| t < to)
    }

    /// The server's answer (already checked by the caller): the other end's address and key.
    fn answer(&mut self, addr: SocketAddr, dkey: u64, now: Instant, who: &str) -> bool {
        if !self.ok {
            return false;
        }
        self.dkey = dkey;
        if let Some(old) = self.peer {
            if old == addr {
                return true; // (the schedule keeps running: a repeated answer is no new address)
            }
            eprintln!("[link] {who}: the other end's address changed (a network change?)");
        }
        let first = self.peer.is_none();
        self.peer = Some(addr); // (never logged: the other player's address)
        self.p_since = now;
        self.next_punch = now;
        if first {
            eprintln!("[link] {who}: the hub server gave the other end's address, trying a direct connection");
        }
        true
    }

    /// A datagram straight from the other end: from its address as the server gave it (the port may
    /// differ: the latest wins), unless direct datagrams are blocked for the test.
    fn direct_from(&mut self, from: SocketAddr) -> bool {
        let Some(peer) = self.peer else { return false };
        if from.ip() != peer.ip() || self.blocked() {
            return false;
        }
        self.peer = Some(from);
        true
    }

    /// Back to the relay after DIRECT_SILENCE without proof the path works both ways; asks and
    /// punches fast again for a while.
    fn check_silence(&mut self, now: Instant, who: &str) -> bool {
        if !self.up || now.duration_since(self.rx) <= DIRECT_SILENCE {
            return false;
        }
        self.up = false;
        self.good = 0;
        self.p_since = now;
        self.q_since = now;
        self.next_query = now;
        self.nopunch_said = false;
        eprintln!(
            "[link] {who}: the direct connection went quiet ({:.1} s without proof it works both ways): game packets go through the hub server again",
            now.duration_since(self.rx).as_secs_f64()
        );
        true
    }

    /// The `MHGQ` query to send to the server, when it is due; also says why there is no direct
    /// connection when it can tell.
    fn query(&mut self, now: Instant, pair: u32, secret: u64, who: &str) -> Option<Vec<u8>> {
        if !self.ok || self.up {
            return None;
        }
        if !self.known() && !self.noanswer_said && now.duration_since(self.born) > QUERY_FAST {
            self.noanswer_said = true;
            eprintln!(
                "[link] {who}: no direct connection: the other end does not offer one (its Direct = Off, an older version, or an older hub server); game packets stay on the hub server"
            );
        }
        if self.known() && !self.nopunch_said && now.duration_since(self.p_since) > PUNCH_FAST {
            self.nopunch_said = true;
            eprintln!(
                "[link] {who}: no direct connection after {} s of trying (a router that gives each destination its own port, as symmetric / carrier-grade NAT, or a firewall): game packets stay on the hub server, still trying every 2 s",
                PUNCH_FAST.as_secs()
            );
        }
        if now < self.next_query {
            return None;
        }
        self.next_query = now
            + if now.duration_since(self.q_since) < QUERY_FAST {
                QUERY_EVERY
            } else {
                QUERY_SLOW
            };
        let mut d = Vec::with_capacity(proto::PEER_MSG_SIZE);
        d.extend_from_slice(&relay_prefix(pair, secret));
        d.extend_from_slice(&proto::PEER_QUERY);
        d.extend_from_slice(&[0u8; 12]);
        Some(d)
    }
}

fn block_window() -> Option<(Duration, Option<Duration>)> {
    static WINDOW: LazyLock<Option<(Duration, Option<Duration>)>> = LazyLock::new(|| {
        let e = std::env::var("MHGU_LDN_BLOCK_DIRECT").ok()?;
        if e.is_empty() {
            return None;
        }
        let (a, b) = match e.split_once('-') {
            Some((a, b)) => (a, Some(b)),
            None => (e.as_str(), None),
        };
        let secs = |s: &str| s.trim().parse::<u64>().unwrap_or(0);
        let from = Duration::from_secs(secs(a));
        let to = b.map(|b| Duration::from_secs(secs(b)));
        eprintln!(
            "[link] MHGU_LDN_BLOCK_DIRECT: direct datagrams ignored after {} s of a link",
            from.as_secs()
        );
        Some((from, to))
    });
    *WINDOW
}

/// The server's answer, checked against the pair and our secret: the other end's address and the
/// pair's direct key.
fn peer_answer_bytes(b: &[u8], pair: u32, secret: u64) -> Option<(SocketAddr, u64)> {
    if b.len() != proto::PEER_MSG_SIZE || b[..4] != proto::PEER_ANSWER {
        return None;
    }
    if u32::from_le_bytes(b[4..8].try_into().ok()?) != pair
        || u64::from_le_bytes(b[8..16].try_into().ok()?) != secret
    {
        return None;
    }
    let ip = Ipv4Addr::new(b[16], b[17], b[18], b[19]);
    let port = u16::from_be_bytes([b[20], b[21]]);
    let dkey = u64::from_le_bytes(b[24..32].try_into().ok()?);
    Some((SocketAddr::new(IpAddr::V4(ip), port), dkey))
}

/// A monotonic clock for hello echoes (only a difference matters).
fn now_ns() -> u64 {
    static START: LazyLock<Instant> = LazyLock::new(Instant::now);
    START.elapsed().as_nanos() as u64
}

fn rtt_enabled() -> bool {
    static ON: LazyLock<bool> = LazyLock::new(|| {
        std::env::var("MHGU_LDN_RTT").ok().and_then(|e| e.trim().parse::<i64>().ok()).is_some_and(|v| v > 0)
    });
    *ON
}

/// One path's round trips, logged every 10 s under MHGU_LDN_RTT.
#[derive(Default, Clone, Copy)]
struct Rtt {
    n: u64,
    sum: f64,
    min: f64,
    max: f64,
}

impl Rtt {
    fn add(&mut self, ms: f64) {
        self.n += 1;
        self.sum += ms;
        if self.n == 1 || ms < self.min {
            self.min = ms;
        }
        if self.n == 1 || ms > self.max {
            self.max = ms;
        }
    }

    fn text(&self) -> String {
        format!("{:.1}/{:.1}/{:.1} ms min/avg/max ({} hellos)", self.min, self.sum / self.n as f64, self.max, self.n)
    }
}

/// The round trip a hello's echoed clock shows, if MHGU_LDN_RTT is on and the echo is sane.
fn rtt_of(stamp: &[u8]) -> Option<f64> {
    if !rtt_enabled() || stamp.len() < 8 {
        return None;
    }
    let t0 = u64::from_ne_bytes(stamp[..8].try_into().ok()?);
    let now = now_ns();
    if t0 > now || now - t0 > 10_000_000_000 {
        return None;
    }
    Some((now - t0) as f64 / 1e6)
}

/// The host's direct timers: ask the server, punch, and fall back after silence (ldn.c's host loop).
fn host_direct_timers(udp: &UdpSocket) {
    let now = Instant::now();
    let mut sends: Vec<(SocketAddr, Vec<u8>)> = Vec::new();
    with(|st| {
        for p in st.peers.iter_mut() {
            if !p.direct.ok {
                continue;
            }
            let who = format!("peer {}", Ipv4Addr::from(p.ip));
            p.direct.check_silence(now, &who);
            if let (Some((pair, secret)), Some(server)) = (p.relay, p.udp) {
                if let Some(q) = p.direct.query(now, pair, secret, &who) {
                    sends.push((server, q));
                }
            }
            if p.direct.known() && !p.direct.up && now >= p.direct.next_punch {
                p.direct.next_punch = now
                    + if now.duration_since(p.direct.p_since) < PUNCH_FAST {
                        PUNCH_EVERY
                    } else {
                        PUNCH_SLOW
                    };
                if let (Some(addr), Some((pair, _))) = (p.direct.peer, p.relay) {
                    let mut d = Vec::with_capacity(proto::DIRECT_HEADER + proto::LINK_UDP_HDR);
                    d.extend_from_slice(&relay_prefix(pair, p.direct.dkey));
                    d.extend_from_slice(&mhgu_link::udp::datagram_flags(p.token, proto::U_PUNCH, 0, &[]));
                    sends.push((addr, d));
                }
            }
        }
    });
    for (to, d) in sends {
        let _ = udp.send_to(&d, to);
    }
}

struct State {
    running: bool,
    hosting: bool,
    disconnect_reason: u32,
    /// The host's listener address (the port is the one the game announced); for a joiner, the
    /// host's port (the network's id).
    listen_port: u16,
    /// This player's own IP, and the network's subnet base (169.254.0.0/16).
    own_ip: u32,
    subnet: u32,
    /// The `NetworkInfo` blob the C side provided (0x480 bytes); its node count is kept current.
    info: Vec<u8>,
    name: String,
    mac: [u8; 6],
    lcv: u16,
    /// Host: the peers we serve. Join: the one link to the host (a `Client`).
    peers: Vec<Peer>,
    /// The joiner's write side (the reader thread owns the `Client`).
    writer: Option<Arc<Mutex<TcpStream>>>,
    /// Frames received and not yet delivered to the C side.
    packets: VecDeque<(MhguVPacket, Vec<u8>)>,
    aux: VecDeque<Vec<u8>>,
    /// The join: the host's node slot for us.
    node: u8,
    stop: Arc<AtomicBool>,
    accept_thread: Option<std::thread::JoinHandle<()>>,
    udp_thread: Option<std::thread::JoinHandle<()>>,
    reader_thread: Option<std::thread::JoinHandle<()>>,
    /// The host's UDP socket (LAN answers and the game's datagrams), and the station's one.
    udp: Option<Arc<UdpSocket>>,
    usock: Option<Arc<UdpSocket>>,
    /// The station's WELCOME token and host address.
    token: u32,
    host_addr: Option<SocketAddr>,
    udp_up: bool,
    udp_rx: std::time::Instant,
    /// Through a hub server (station side): the relay pair and our secret.
    relay: Option<(u32, u64)>,
    /// Direct UDP past the server (hole punching), when the link is relayed.
    direct: DirectState,
    /// The id the hub server gave this host's listing (0: not listed); ldn.c skips it in a LAN/server scan.
    own_hub: u64,
}

static STATE: Mutex<Option<State>> = Mutex::new(None);
/// The game's local communication version, sent in the next `Hello` (`nn::ldn` refuses a mismatch).
static LCV: std::sync::atomic::AtomicU16 = std::sync::atomic::AtomicU16::new(0);

#[no_mangle]
pub extern "C" fn mhgu_ldn_set_lcv(lcv: u16) {
    LCV.store(lcv, Ordering::Relaxed);
}

/// `[Multiplayer] Direct`: in an internet hub, ask the server for the other ends' addresses and send
/// game packets straight to them when the routers let them (hole punching), else through the relay.
/// Set before hosting or joining; 0 keeps every packet on the relay.
#[no_mangle]
pub extern "C" fn mhgu_ldn_set_direct(on: c_int) {
    DIRECT.store(on != 0, Ordering::Relaxed);
}

static DELIVER_PACKET: Mutex<Option<DeliverPacket>> = Mutex::new(None);
static DELIVER_AUX: Mutex<Option<DeliverAux>> = Mutex::new(None);

impl State {
    fn new() -> State {
        State {
            running: false,
            hosting: false,
            disconnect_reason: 0,
            listen_port: 0,
            own_ip: 0,
            subnet: 0,
            info: Vec::new(),
            name: String::new(),
            mac: [0; 6],
            lcv: 0,
            peers: Vec::new(),
            writer: None,
            packets: VecDeque::new(),
            aux: VecDeque::new(),
            node: 0,
            stop: Arc::new(AtomicBool::new(false)),
            accept_thread: None,
            udp_thread: None,
            reader_thread: None,
            udp: None,
            usock: None,
            token: 0,
            host_addr: None,
            udp_up: false,
            udp_rx: std::time::Instant::now(),
            relay: None,
            direct: DirectState::new(false),
            own_hub: 0,
        }
    }
}

fn with<R>(f: impl FnOnce(&mut State) -> R) -> R {
    let mut g = STATE.lock().unwrap();
    f(g.get_or_insert_with(State::new))
}

fn cstr(p: *const c_char) -> String {
    if p.is_null() {
        return String::new();
    }
    unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned()
}

/// The host's node 1 is 169.254.0.1; joiners get 169.254.0.2, .3, ...
/// First packets each way after a join, for the "the host never answered" case (a quiet log otherwise).
static GUEST_RX: AtomicU32 = AtomicU32::new(0);
static GUEST_TX: AtomicU32 = AtomicU32::new(0);

fn node_ip(subnet: u32, node: u8) -> u32 {
    subnet | node as u32
}


/// The MAC the game uses for itself (the C base's `L.mac`): the join Hello carries it so the host's
/// node table names this machine as the game expects; a random MAC leaves the game without its own node
/// entry (Pia then refuses the network: the guest sends nothing and the game reports 2618-502).
static GAME_MAC: Mutex<[u8; 6]> = Mutex::new([0; 6]);

#[no_mangle]
pub extern "C" fn mhgu_ldn_set_mac(mac: *const u8) {
    if mac.is_null() {
        return;
    }
    let m = unsafe { std::slice::from_raw_parts(mac, 6) };
    let mut g = GAME_MAC.lock().unwrap();
    g.copy_from_slice(m);
}

fn join_mac() -> [u8; 6] {
    let m = *GAME_MAC.lock().unwrap();
    if m != [0; 6] {
        m
    } else {
        [0x02, 0, 0, rand_u32() as u8, (rand_u32() >> 8) as u8, (rand_u32() >> 16) as u8]
    }
}

/// The address a joiner takes for itself out of the host's `NetworkInfo`: the host assigns
/// `subnet | (slot + 1)` to the station in slot `node` (ldn.c `accept_one`), and the same blob rides in the
/// Welcome. Reading the node keeps us on the host's subnet — a fixed 169.254.0.0 with `subnet | node`
/// made a joiner in slot 1 claim the host's own address, and the game's Pia then routed its own packets
/// back to itself (the guest's "can't enter the gathering hub", 2618-502).
fn joiner_own_ip(info: &[u8], node: u8) -> (u32, u32) {
    let node_ip_at = |i: usize| -> u32 {
        let at = proto::NI_NODES + i * proto::NODE_SIZE + proto::NODE_IP;
        if at + 4 > info.len() {
            return 0;
        }
        u32::from_le_bytes([info[at], info[at + 1], info[at + 2], info[at + 3]])
    };
    let host_ip = node_ip_at(0);
    let subnet = if host_ip == 0 { 0xA9FE_0000 } else { host_ip & 0xFFFF_FF00 };
    let own = node_ip_at(node as usize);
    let own = if own == 0 { subnet | (node as u32 + 1) } else { own };
    (own, own & 0xFFFF_FF00)
}

/// The `NetworkInfo` blob's node count, kept in step with the peers we serve.
fn set_node_count(info: &mut [u8], n: u8) {
    if info.len() > proto::NI_NODE_COUNT {
        info[proto::NI_NODE_COUNT] = n;
    }
}

/// Rebuilds the node table of the host's `NetworkInfo` (node 0 is us, then each peer's `Hello`), and
/// returns it (`ldn.c` `node_at`/`push_info_locked`).
fn rebuild_nodes(info: &mut [u8], own_ip: u32, own_mac: &[u8; 6], own_name: &str, own_lcv: u16, peers: &[Peer]) {
    let max = info.get(proto::NI_NODE_MAX).copied().unwrap_or(proto::MAX_NODES).min(proto::MAX_NODES);
    let n = (peers.len() as u8 + 1).min(max);
    set_node_count(info, n);
    let put = |info: &mut [u8], i: usize, ip: u32, mac: &[u8], name: &str, lcv: u16| {
        let at = proto::NI_NODES + i * proto::NODE_SIZE;
        if at + proto::NODE_SIZE > info.len() {
            return;
        }
        info[at..at + proto::NODE_SIZE].fill(0);
        info[at + proto::NODE_IP..at + proto::NODE_IP + 4].copy_from_slice(&ip.to_le_bytes());
        info[at + proto::NODE_MAC..at + proto::NODE_MAC + 6].copy_from_slice(&mac[..6]);
        info[at + proto::NODE_ID] = i as u8;
        info[at + proto::NODE_CONNECTED] = 1;
        let nb = name.as_bytes();
        let k = nb.len().min(0x20);
        info[at + proto::NODE_NAME..at + proto::NODE_NAME + k].copy_from_slice(&nb[..k]);
        info[at + proto::NODE_LCV..at + proto::NODE_LCV + 2].copy_from_slice(&lcv.to_le_bytes());
    };
    put(info, 0, own_ip, own_mac, own_name, own_lcv);
    for (i, p) in peers.iter().take(max.saturating_sub(1) as usize).enumerate() {
        let name: String = p
            .hello
            .name
            .iter()
            .take_while(|c| **c != 0)
            .map(|c| *c as char)
            .collect();
        put(info, i + 1, p.ip, &p.hello.mac, &name, p.hello.lcv);
    }
}

/// Sends the host's current `NetworkInfo` to every peer (`F_INFO`).
fn push_info() {
    let (info, targets) = with(|st| {
        let info = st.info.clone();
        let t: Vec<Arc<Mutex<TcpStream>>> = st.peers.iter().map(|p| Arc::clone(&p.stream)).collect();
        (info, t)
    });
    for t in targets {
        let mut s = t.lock().unwrap();
        let _ = write_frame(&mut s, proto::F_INFO, &[&info]);
    }
}

/// Parses "a.b.c.d" or "a.b.c.d:port" (the host's link port defaults to `port`).
fn parse_addr(text: &str, port: u16) -> Option<SocketAddr> {
    let text = text.trim();
    if text.is_empty() {
        return None;
    }
    let with_port = if text.contains(':') { text.to_string() } else { format!("{text}:{port}") };
    with_port.parse().ok()
}

// ---- delivery -----------------------------------------------------------------------------------

#[no_mangle]
pub extern "C" fn mhgu_ldn_set_delivery(packet: Option<DeliverPacket>, aux: Option<DeliverAux>) {
    *DELIVER_PACKET.lock().unwrap() = packet;
    *DELIVER_AUX.lock().unwrap() = aux;
}

/// Hosts the link: listens on `port` (0: any) and serves the in-game link to whoever joins. The port
/// the game announced to its friends is `*port_out`.
#[no_mangle]
pub extern "C" fn mhgu_ldn_host(
    port: u16,
    port_out: *mut u16,
    name: *const c_char,
    info: *const u8,
    info_len: u32,
) -> bool {
    mhgu_ldn_leave();
    let blob = if info.is_null() || info_len == 0 {
        vec![0u8; proto::NETINFO_SIZE]
    } else {
        unsafe { std::slice::from_raw_parts(info, info_len as usize) }.to_vec()
    };
    let listener = match TcpListener::bind(("0.0.0.0", port)) {
        Ok(l) => l,
        Err(e) => {
            eprintln!("[link] host: bind :{port}: {e}");
            return false;
        }
    };
    let local = match listener.local_addr() {
        Ok(a) => a,
        Err(_) => return false,
    };
    let stop = Arc::new(AtomicBool::new(false));
    {
        let mut g = STATE.lock().unwrap();
        let st = g.get_or_insert_with(State::new);
        st.disconnect_reason = 0;
        st.running = true;
        st.hosting = true;
        st.listen_port = local.port();
        st.subnet = 0xA9FE_0000; // 169.254.0.0
        st.own_ip = node_ip(st.subnet, 1);
        st.info = blob;
        st.name = cstr(name);
        // Our own address is the one the game already put in node 0 (it picked the subnet): the game's
        // Pia routes by it, so the network must agree with it.
        let n0 = proto::NI_NODES;
        let own = (proto::NI_NODES + proto::NODE_IP, proto::NI_NODES + proto::NODE_MAC);
        let mut ip = u32::from_le_bytes([
            st.info.get(own.0).copied().unwrap_or(0),
            st.info.get(own.0 + 1).copied().unwrap_or(0),
            st.info.get(own.0 + 2).copied().unwrap_or(0),
            st.info.get(own.0 + 3).copied().unwrap_or(0),
        ]);
        if ip == 0 {
            ip = 0xA9FE_0001; // (the C base's default subnet, when the blob carries none)
        }
        st.own_ip = ip;
        st.subnet = ip & 0xFFFF_FF00;
        let mut mac = [0x02u8, 0, 0, 0, 0, 0];
        for i in 0..6 {
            mac[i] = st.info.get(own.1 + i).copied().unwrap_or(0);
        }
        st.mac = mac;
        st.lcv = u16::from_le_bytes([
            st.info.get(n0 + proto::NODE_LCV).copied().unwrap_or(0),
            st.info.get(n0 + proto::NODE_LCV + 1).copied().unwrap_or(0),
        ]);
        let _ = n0;
        let (own_ip, own_mac, own_name, own_lcv) = (st.own_ip, st.mac, st.name.clone(), st.lcv);
        rebuild_nodes(&mut st.info, own_ip, &own_mac, &own_name, own_lcv, &[]);
        st.stop = Arc::clone(&stop);
    }
    if !port_out.is_null() {
        unsafe { *port_out = local.port() };
    }
    // The host's own node: accept the first connection as a peer like any other.
    // The LAN search: a probe on the same port is answered with our NetworkInfo (`MHGL` reply).
    let udp = match UdpSocket::bind(("0.0.0.0", local.port())) {
        Ok(u) => Some(u),
        Err(e) => {
            eprintln!("[link] host: udp :{}: {e}", local.port());
            None
        }
    };
    if let Some(udp) = udp {
        let udp = Arc::new(udp);
        with(|st| st.udp = Some(Arc::clone(&udp)));
        let stop_udp = Arc::clone(&stop);
        let handle = std::thread::spawn(move || {
            let _ = udp.set_read_timeout(Some(Duration::from_millis(100)));
            let mut buf = vec![0u8; proto::UDP_MAX + 1];
            while !stop_udp.load(Ordering::Relaxed) {
                // Query, punch and fall back while no datagram is waiting.
                host_direct_timers(&udp);
                let (n, from) = match udp.recv_from(&mut buf) {
                    Ok(v) => v,
                    Err(_) => continue,
                };
                // Straight from a station (hole punched): MHGR + pair + the pair's direct key.
                let mut dg: &[u8] = &buf[..n];
                let mut direct_peer: Option<usize> = None;
                if n >= proto::DIRECT_HEADER && buf[..4] == proto::UDP_MAGIC {
                    let pair = u32::from_le_bytes(buf[4..8].try_into().unwrap());
                    let dkey = u64::from_le_bytes(buf[8..16].try_into().unwrap());
                    direct_peer = with(|st| {
                        st.peers.iter().position(|p| {
                            p.relay.map(|r| r.0) == Some(pair)
                                && p.direct.dkey != 0
                                && p.direct.dkey == dkey
                                && p.udp != Some(from)
                        })
                    });
                    match direct_peer {
                        Some(_) => dg = &buf[proto::DIRECT_HEADER..n],
                        None => continue, // (anything else on the port: dropped)
                    }
                }
                if let Some((token, kind, flags, payload)) = mhgu_link::udp::parse_flags(dg) {
                    // The station whose datagram this is.
                    let idx = with(|st| st.peers.iter().position(|p| p.token == token));
                    let Some(idx) = idx else { continue };
                    if direct_peer.is_some() && direct_peer != Some(idx) {
                        continue;
                    }
                    let now = Instant::now();
                    let relayed = with(|st| st.peers[idx].relay.is_some());
                    let peer_udp = with(|st| st.peers[idx].udp);
                    if relayed && peer_udp != Some(from) {
                        // Straight from the station: only with our prefix and its address.
                        if direct_peer.is_none() || !with(|st| st.peers[idx].direct.direct_from(from)) {
                            continue;
                        }
                        match kind {
                            proto::U_HELLO => {
                                let up = flags & proto::UF_DIRECT_OK != 0;
                                let (changed, ip) = with(|st| {
                                    let p = &mut st.peers[idx];
                                    p.direct.rx = now;
                                    let changed = p.direct.up != up;
                                    p.direct.up = up;
                                    (changed, p.ip)
                                });
                                if changed {
                                    eprintln!(
                                        "[link] peer {}: {}",
                                        Ipv4Addr::from(ip),
                                        if up {
                                            "direct connection up: game packets go straight to the player"
                                        } else {
                                            "the player gets no direct datagrams: game packets go through the hub server"
                                        }
                                    );
                                }
                                let echo = &payload[..payload.len().min(8)];
                                let (pair, dkey) = with(|st| {
                                    let p = &st.peers[idx];
                                    (p.relay.map(|r| r.0).unwrap_or(0), p.direct.dkey)
                                });
                                let mut a = Vec::with_capacity(proto::DIRECT_HEADER + proto::LINK_UDP_HDR + echo.len());
                                a.extend_from_slice(&relay_prefix(pair, dkey));
                                a.extend_from_slice(&mhgu_link::udp::datagram_flags(token, proto::U_HELLO_ACK, 0, echo));
                                let _ = udp.send_to(&a, from);
                            }
                            proto::U_DATA if payload.len() >= proto::VPACKET_SIZE => {
                                if let Some(mut v) = MhguVPacket::parse(payload) {
                                    let data = payload[proto::VPACKET_SIZE..].to_vec();
                                    let (up, ip) = with(|st| {
                                        let p = &mut st.peers[idx];
                                        p.direct.rx = now;
                                        let up = p.direct.up;
                                        p.direct.up = true;
                                        (up, p.ip)
                                    });
                                    if !up {
                                        eprintln!(
                                            "[link] peer {}: direct connection up: game packets go straight to the player",
                                            Ipv4Addr::from(ip)
                                        );
                                    }
                                    v.src_ip = ip;
                                    relay_from_udp(idx, &v, &data);
                                    with(|st| st.packets.push_back((v, data)));
                                }
                            }
                            _ => {}
                        }
                        continue;
                    }
                    // Through the hub server (or a LAN): the address it came from is where to answer.
                    with(|st| {
                        let p = &mut st.peers[idx];
                        p.udp = Some(from);
                        p.udp_rx = now;
                    });
                    match kind {
                        proto::U_HELLO => {
                            let echo = &payload[..payload.len().min(8)];
                            let inner = mhgu_link::udp::datagram_flags(token, proto::U_HELLO_ACK, 0, echo);
                            let d = match with(|st| st.peers[idx].relay) {
                                Some((pair, secret)) => {
                                    let mut d = Vec::with_capacity(proto::UDP_HEADER + inner.len());
                                    d.extend_from_slice(&relay_prefix(pair, secret));
                                    d.extend_from_slice(&inner);
                                    d
                                }
                                None => inner,
                            };
                            let _ = udp.send_to(&d, from);
                        }
                        proto::U_DATA if payload.len() >= proto::VPACKET_SIZE => {
                            if let Some(v) = MhguVPacket::parse(payload) {
                                let data = payload[proto::VPACKET_SIZE..].to_vec();
                                relay_from_udp(idx, &v, &data);
                                with(|st| st.packets.push_back((v, data)));
                            }
                        }
                        _ => {}
                    }
                    continue;
                }
                // The hub server's answer: a station's public address (and the pair's direct key).
                if n == proto::PEER_MSG_SIZE && buf[..4] == proto::PEER_ANSWER {
                    let now = Instant::now();
                    with(|st| {
                        for p in st.peers.iter_mut() {
                            let Some((pair, secret)) = p.relay else { continue };
                            if p.udp != Some(from) {
                                continue;
                            }
                            if let Some((addr, dkey)) = peer_answer_bytes(&buf[..n], pair, secret) {
                                let who = format!("peer {}", Ipv4Addr::from(p.ip));
                                p.direct.answer(addr, dkey, now, &who);
                                break;
                            }
                        }
                    });
                    continue;
                }
                let info = with(|st| st.info.clone());
                if let Some(reply) = scan::answer(&buf[..n], local.port(), &info) {
                    let _ = udp.send_to(&reply, from);
                }
            }
        });
        with(|st| st.udp_thread = Some(handle));
    }
    // A non-blocking accept loop: `mhgu_ldn_leave` sets the flag and joins this thread, so the port is
    // free again when the game creates a new network right after (re-hosting used to fail with
    // "bind :39500: Address already in use", the old listener still blocking in accept).
    let _ = listener.set_nonblocking(true);
    let stop_accept = Arc::clone(&stop);
    let accept = std::thread::spawn(move || loop {
        if stop_accept.load(Ordering::Relaxed) {
            break;
        }
        match listener.accept() {
            Ok((s, _)) => {
                std::thread::spawn(move || serve_peer(s));
            }
            Err(e) if e.kind() == io::ErrorKind::WouldBlock => {
                std::thread::sleep(Duration::from_millis(20));
            }
            Err(e) => {
                eprintln!("[link] host: accept: {e}");
                break;
            }
        }
    });
    with(|st| st.accept_thread = Some(accept));
    eprintln!("[link] hosting on port {}", local.port());
    true
}

/// Serves one peer on the host: the `Hello` handshake, then the relay of its packets to the others.
fn serve_peer(stream: TcpStream) { serve_peer_relay(stream, None) }

/// The 16-byte `MHGR` prefix the hub server uses to route a datagram (`ldn.c` `relay_prefix`).
fn relay_prefix(pair: u32, secret: u64) -> [u8; 16] {
    let mut b = [0u8; 16];
    b[..4].copy_from_slice(b"MHGR");
    b[4..8].copy_from_slice(&pair.to_le_bytes());
    b[8..16].copy_from_slice(&secret.to_le_bytes());
    b
}

fn serve_peer_relay(stream: TcpStream, relay: Option<(u32, u64)>) {
    // Windows accepted sockets inherit the nonblocking listener mode.
    let _ = stream.set_nonblocking(false);
    let _ = stream.set_nodelay(true);
    let _ = stream.set_read_timeout(Some(Duration::from_secs(5)));
    let mut hello_stream = stream;
    // The first frame is the peer's Hello; we answer as the host would.
    let hello = match read_frame(&mut hello_stream) {
        Ok((proto::F_HELLO, p)) => Hello::parse(&p),
        Ok((t, _)) => {
            eprintln!("[link] host: first frame {t} is not a Hello");
            None
        }
        Err(e) => {
            eprintln!("[link] host: hello: {e}");
            None
        }
    };
    let Some(hello) = hello else { return };
    if hello.scan_only {
        let info = with(|st| st.info.clone());
        let _ = write_frame(&mut hello_stream, proto::F_INFO, &[&info]);
        return;
    }

    // Through the hub server the accepted socket comes from the server; its address is where the
    // station's relayed datagrams will come from (ldn.c's `l->uaddr = acc.from`).
    let peer_addr = hello_stream.peer_addr().ok();
    // The WELCOME must already show this peer's node: the joining client checks that the entry at the
    // WELCOME's node index is marked connected in the NetworkInfo it is handed.
    let (node, info, writer) = {
        let mut g = STATE.lock().unwrap();
        let Some(st) = g.as_mut() else { return };
        // ldn's node slots are 1.. (0 is the host, and the WELCOME's byte indexes the NetworkInfo's
        // node table): the first peer is 1, its address .2.
        let node = (st.peers.len() + 1).min(proto::MAX_NODES as usize - 1) as u8;
        let ip = st.subnet | (node as u32 + 1);
        let writer = match hello_stream.try_clone() {
            Ok(w) => Arc::new(Mutex::new(w)),
            Err(_) => return,
        };
        st.peers.push(Peer {
            stream: Arc::clone(&writer),
            hello: hello.clone(),
            ip,
            token: 0, // set with the WELCOME below
            udp: if relay.is_some() { peer_addr } else { None },
            // (not "up" until a datagram from there arrives; but a relayed link knows the server already)
            udp_rx: std::time::Instant::now()
                .checked_sub(Duration::from_secs(15))
                .unwrap_or_else(std::time::Instant::now),
            relay,
            direct: DirectState::new(relay.is_some() && DIRECT.load(Ordering::Relaxed)),
        });
        let (own_ip, own_mac, own_name, own_lcv) = (st.own_ip, st.mac, st.name.clone(), st.lcv);
        let peers: Vec<Peer> = st.peers.iter().map(|p| Peer { stream: Arc::clone(&p.stream), hello: p.hello.clone(), ip: p.ip, token: p.token, udp: p.udp, udp_rx: p.udp_rx, relay: p.relay, direct: p.direct }).collect();
        rebuild_nodes(&mut st.info, own_ip, &own_mac, &own_name, own_lcv, &peers);
        (node, st.info.clone(), writer)
    };
    // WELCOME: node, pad[3], token u32, the network's NetworkInfo.
    let token: u32 = rand_u32();
    with(|st| {
        if let Some(p) = st.peers.iter_mut().rev().find(|p| p.token == 0 && p.ip == (st.subnet | (node as u32 + 1))) {
            p.token = token;
        }
    });
    if std::env::var_os("MHGU_TRACE_LINK").is_some() {
        eprintln!("[link] node {} WELCOME token {token:#x}", node);
    }
    let mut wel = vec![node, 0, 0, 0];
    wel.extend_from_slice(&token.to_le_bytes());
    wel.extend_from_slice(&info);
    {
        let mut s = writer.lock().unwrap();
        if write_frame(&mut s, proto::F_WELCOME, &[&wel]).is_err() {
            return;
        }
    }
    let _ = hello_stream.set_read_timeout(None);
    let stream = Arc::clone(&writer);
    let (name, ip) = {
        let g = STATE.lock().unwrap();
        match g.as_ref().and_then(|st| st.peers.iter().find(|p| Arc::ptr_eq(&p.stream, &Arc::clone(&stream))).map(|p| (p.hello.name_string(), p.ip))) {
            Some(v) => v,
            None => (String::new(), 0),
        }
    };
    eprintln!(
        "[link] {:?} joined as node {} ({}), network now {} node(s)",
        name,
        node,
        std::net::Ipv4Addr::from(ip),
        with(|st| st.info.get(proto::NI_NODE_COUNT).copied().unwrap_or(0))
    );
    push_info();
    // Read this peer's frames: deliver them locally and relay them to the other peers.
    loop {
        let (t, p) = match read_frame(&mut hello_stream) {
            Ok(f) => f,
            Err(_) => break,
        };
        match t {
            proto::F_PACKET if p.len() >= proto::VPACKET_SIZE => {
                let Some(v) = MhguVPacket::parse(&p) else { continue };
                let data = p[proto::VPACKET_SIZE..].to_vec();
                if std::env::var_os("MHGU_TRACE_LINK").is_some() {
                    use std::sync::atomic::{AtomicU64, Ordering};
                    static N: AtomicU64 = AtomicU64::new(0);
                    let n = N.fetch_add(1, Ordering::Relaxed);
                    if n < 5 || n % 200 == 0 {
                        eprintln!("[link] host rx #{} from node {} {} bytes ({}:{})", n, node, data.len(),
                                  std::net::Ipv4Addr::from(v.src_ip), v.src_port);
                    }
                }
                relay_packet(&stream, &v, &data);
                with(|st| st.packets.push_back((v, data)));
            }
            proto::F_AUX => {
                relay_aux(&stream, &p);
                with(|st| st.aux.push_back(p));
            }
            proto::F_INFO => {
                let blob = with(|st| st.info.clone());
                let mut s = writer.lock().unwrap();
                let _ = write_frame(&mut s, proto::F_INFO, &[&blob]);
            }
            _ => {}
        }
    }
    // Peer gone: drop it, rebuild the node table, and tell the others.
    {
        let mut g = STATE.lock().unwrap();
        if let Some(st) = g.as_mut() {
            st.peers.retain(|p| !Arc::ptr_eq(&p.stream, &stream));
            let (own_ip, own_mac, own_name, own_lcv) = (st.own_ip, st.mac, st.name.clone(), st.lcv);
            let peers: Vec<Peer> = st.peers.iter().map(|p| Peer { stream: Arc::clone(&p.stream), hello: p.hello.clone(), ip: p.ip, token: p.token, udp: p.udp, udp_rx: p.udp_rx, relay: p.relay, direct: p.direct }).collect();
            rebuild_nodes(&mut st.info, own_ip, &own_mac, &own_name, own_lcv, &peers);
        }
    }
    push_info();
}

impl MhguVPacket {
    fn parse(p: &[u8]) -> Option<MhguVPacket> {
        if p.len() < proto::VPACKET_SIZE {
            return None;
        }
        Some(MhguVPacket {
            src_ip: u32::from_le_bytes(p[0..4].try_into().unwrap()),
            dst_ip: u32::from_le_bytes(p[4..8].try_into().unwrap()),
            src_port: u16::from_le_bytes(p[8..10].try_into().unwrap()),
            dst_port: u16::from_le_bytes(p[10..12].try_into().unwrap()),
            proto: p[12],
            kind: p[13],
            pad: 0,
        })
    }

    fn to_bytes(&self) -> [u8; proto::VPACKET_SIZE] {
        let mut b = [0u8; proto::VPACKET_SIZE];
        b[0..4].copy_from_slice(&self.src_ip.to_le_bytes());
        b[4..8].copy_from_slice(&self.dst_ip.to_le_bytes());
        b[8..10].copy_from_slice(&self.src_port.to_le_bytes());
        b[10..12].copy_from_slice(&self.dst_port.to_le_bytes());
        b[12] = self.proto;
        b[13] = self.kind;
        b
    }
}

fn rand_u32() -> u32 {
    use std::time::{SystemTime, UNIX_EPOCH};
    let n = SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_nanos() as u64).unwrap_or(1);
    (n ^ (n >> 32)) as u32 | 1
}

/// Relays a packet from `from` to every other peer (the host is the relay).
fn relay_packet(from: &Arc<Mutex<TcpStream>>, v: &MhguVPacket, data: &[u8]) {
    let targets: Vec<Arc<Mutex<TcpStream>>> = with(|st| {
        st.peers.iter().filter(|p| !Arc::ptr_eq(&p.stream, from)).map(|p| Arc::clone(&p.stream)).collect()
    });
    let body = [&v.to_bytes()[..], data].concat();
    for t in targets {
        let mut s = t.lock().unwrap();
        let _ = write_frame(&mut s, proto::F_PACKET, &[&body]);
    }
}

/// Relays a datagram received on the host's UDP socket (`sender`: the peer it came from) to the other
/// players, straight to those with a working direct path and through the server for the rest.
fn relay_from_udp(sender: usize, v: &MhguVPacket, data: &[u8]) {
    let body = [&v.to_bytes()[..], data].concat();
    let udp = with(|st| st.udp.clone());
    let now = Instant::now();
    struct Target {
        stream: Arc<Mutex<TcpStream>>,
        token: u32,
        relay: Option<(u32, u64)>,
        relay_addr: Option<SocketAddr>,
        direct: Option<(u32, u64, SocketAddr)>,
    }
    let peers: Vec<Target> = with(|st| {
        st.peers
            .iter()
            .enumerate()
            .filter(|(i, _)| *i != sender)
            .map(|(_, p)| Target {
                stream: Arc::clone(&p.stream),
                token: p.token,
                relay: p.relay,
                relay_addr: p.udp.filter(|_| now.duration_since(p.udp_rx) < Duration::from_secs(15)),
                direct: match (p.direct.usable(now), p.direct.peer, p.relay) {
                    (true, Some(addr), Some((pair, _))) if p.direct.dkey != 0 => {
                        Some((pair, p.direct.dkey, addr))
                    }
                    _ => None,
                },
            })
            .collect()
    });
    for t in peers {
        let inner = mhgu_link::udp::datagram(t.token, proto::U_DATA, &body);
        let (d, to) = if let Some((pair, dkey, addr)) = t.direct {
            ([&relay_prefix(pair, dkey)[..], &inner[..]].concat(), addr)
        } else if let (Some(addr), Some(relay)) = (t.relay_addr, t.relay) {
            ([&relay_prefix(relay.0, relay.1)[..], &inner[..]].concat(), addr)
        } else if t.relay.is_none() && t.relay_addr.is_some() {
            (inner, t.relay_addr.unwrap())
        } else {
            let mut s = t.stream.lock().unwrap();
            let _ = write_frame(&mut s, proto::F_PACKET, &[&body]);
            continue;
        };
        match &udp {
            Some(udp) => {
                let _ = udp.send_to(&d, to);
            }
            None => {
                let mut s = t.stream.lock().unwrap();
                let _ = write_frame(&mut s, proto::F_PACKET, &[&body]);
            }
        }
    }
}

fn relay_aux(from: &Arc<Mutex<TcpStream>>, data: &[u8]) {
    let targets: Vec<Arc<Mutex<TcpStream>>> = with(|st| {
        st.peers.iter().filter(|p| !Arc::ptr_eq(&p.stream, from)).map(|p| Arc::clone(&p.stream)).collect()
    });
    for t in targets {
        let mut s = t.lock().unwrap();
        let _ = write_frame(&mut s, proto::F_AUX, &[data]);
    }
}

fn read_frame(s: &mut TcpStream) -> io::Result<(u8, Vec<u8>)> {
    proto::read_frame(s, proto::LINK_MAX_FRAME as u32)
}

fn write_frame(s: &mut TcpStream, t: u8, payloads: &[&[u8]]) -> io::Result<()> {
    s.write_all(&proto::frame(t, payloads))
}

/// One entry of [`mhgu_ldn_scan`]: the host's address and its `NetworkInfo` (0x480 bytes).
#[repr(C)]
#[derive(Clone, Copy)]
pub struct MhguScanEntry {
    pub ip: u32,
    pub port: u16,
    pub pad: u16,
    pub info: [u8; proto::NETINFO_SIZE],
}


/// A station's `MHGD` datagrams: a hello every 500 ms until the host answers, then every 2 s (which
/// keeps NAT mappings open). Through a hub server the datagram carries the `MHGR` prefix, and the
/// server passes it on to the host (`ldn.c` `relay_prefix`). In a relayed link the station also asks
/// the server for the host's public address and, once it has it, sends direct hellos and punches
/// straight to it (the relay stays as the fallback).
fn station_udp_start(peer: SocketAddr, token: u32, relay: Option<(u32, u64)>, stop: Arc<AtomicBool>) {
    let Ok(u) = UdpSocket::bind(("0.0.0.0", 0)).map(Arc::new) else { return };
    // Not connected: direct datagrams go to the other player, relayed ones to the server's port.
    let _ = u.set_read_timeout(Some(Duration::from_millis(100)));
    with(|st| {
        st.token = token;
        st.host_addr = Some(peer);
        st.usock = Some(Arc::clone(&u));
        st.udp_up = false;
        st.relay = relay;
        st.direct = DirectState::new(relay.is_some() && DIRECT.load(Ordering::Relaxed));
    });
    let handle = std::thread::spawn(move || {
        let mut next_hello = Instant::now();
        let mut next_dhello = Instant::now();
        let mut rtt = [Rtt::default(); 2]; // [0] relay / LAN, [1] direct
        let mut next_rtt = Instant::now() + Duration::from_secs(10);
        let mut buf = vec![0u8; proto::UDP_MAX + 1];
        while !stop.load(Ordering::Relaxed) {
            let now = Instant::now();
            // Relay hellos keep the NAT mapping and the fallback path ready, also while direct works.
            if now >= next_hello {
                let up = with(|st| st.udp_up);
                let payload = now_ns().to_ne_bytes();
                let d = mhgu_link::udp::datagram_flags(token, proto::U_HELLO, 0, &payload);
                let d = match relay {
                    Some((pair, secret)) => [&relay_prefix(pair, secret)[..], &d[..]].concat(),
                    None => d,
                };
                let _ = u.send_to(&d, peer);
                next_hello = now
                    + if up {
                        if rtt_enabled() {
                            Duration::from_millis(250)
                        } else {
                            Duration::from_secs(2)
                        }
                    } else {
                        Duration::from_millis(500)
                    };
            }
            // Direct UDP past the server: ask, punch / say hello, fall back after silence.
            let mut query = None;
            let mut direct_hello = None;
            with(|st| {
                let pair = st.relay;
                let d = &mut st.direct;
                if !d.ok {
                    return;
                }
                if d.check_silence(now, "station") {
                    next_dhello = now;
                }
                if let Some((pair, secret)) = pair {
                    if let Some(q) = d.query(now, pair, secret, "station") {
                        query = Some(q);
                    }
                }
                if d.known() && now >= next_dhello {
                    let flags = if d.up { proto::UF_DIRECT_OK } else { 0 };
                    next_dhello = now
                        + if d.up {
                            if rtt_enabled() {
                                Duration::from_millis(250)
                            } else {
                                DIRECT_HELLO
                            }
                        } else if now.duration_since(d.p_since) < PUNCH_FAST {
                            PUNCH_EVERY
                        } else {
                            PUNCH_SLOW
                        };
                    if let (Some(addr), Some((pair, _))) = (d.peer, pair) {
                        let payload = now_ns().to_ne_bytes();
                        let mut hello = Vec::with_capacity(proto::DIRECT_HEADER + proto::LINK_UDP_HDR + 8);
                        hello.extend_from_slice(&relay_prefix(pair, d.dkey));
                        hello.extend_from_slice(&mhgu_link::udp::datagram_flags(token, proto::U_HELLO, flags, &payload));
                        direct_hello = Some((addr, hello));
                    }
                }
            });
            if let Some(q) = query {
                let _ = u.send_to(&q, peer);
            }
            if let Some((addr, d)) = direct_hello {
                let _ = u.send_to(&d, addr);
            }
            if now >= next_rtt {
                next_rtt = now + Duration::from_secs(10);
                if rtt[0].n > 0 || rtt[1].n > 0 {
                    eprintln!("[link] rtt: relay {}; direct {}", rtt[0].text(), rtt[1].text());
                    rtt = [Rtt::default(); 2];
                }
            }
            if with(|st| st.udp_up) && with(|st| now.duration_since(st.udp_rx) > Duration::from_secs(15)) {
                with(|st| st.udp_up = false);
                eprintln!("[link] nothing by UDP from the host for 15 s: game packets go by TCP");
            }
            let (n, from) = match u.recv_from(&mut buf) {
                Ok(v) => v,
                Err(_) => continue,
            };
            let relayed = relay.is_some();
            let mut direct = false;
            let mut dg: &[u8] = &buf[..n];
            if from != peer {
                // Straight from the host: its pair id and the pair's direct key, else dropped.
                let dkey = with(|st| st.direct.dkey);
                if !relayed || dkey == 0 || n < proto::DIRECT_HEADER || buf[..4] != proto::UDP_MAGIC {
                    continue;
                }
                let pair_in = u32::from_le_bytes(buf[4..8].try_into().unwrap());
                let key_in = u64::from_le_bytes(buf[8..16].try_into().unwrap());
                if relay.map(|r| r.0) != Some(pair_in)
                    || key_in != dkey
                    || !with(|st| st.direct.direct_from(from))
                {
                    continue;
                }
                direct = true;
                dg = &buf[proto::DIRECT_HEADER..n];
            } else if relayed {
                // The server answered our query: the host's public address (and the pair's key).
                if let Some((addr, dkey)) =
                    relay.and_then(|(pair, secret)| peer_answer_bytes(dg, pair, secret))
                {
                    with(|st| {
                        let now = Instant::now();
                        st.direct.answer(addr, dkey, now, "station");
                    });
                    next_dhello = now;
                    continue;
                }
            }
            let Some((datagram_token, kind, _flags, payload)) = mhgu_link::udp::parse_flags(dg) else {
                continue;
            };
            if datagram_token != token {
                continue;
            }
            if direct {
                match kind {
                    proto::U_HELLO_ACK => {
                        if let Some(ms) = rtt_of(payload) {
                            rtt[1].add(ms);
                        }
                        let (became, had) = with(|st| {
                            let d = &mut st.direct;
                            let had = d.ever;
                            d.rx = now;
                            if d.up {
                                return (false, had);
                            }
                            d.good += 1;
                            let need = if d.ever { DIRECT_REUP } else { 1 };
                            if d.good >= need {
                                d.up = true;
                                d.ever = true;
                                (true, had)
                            } else {
                                (false, had)
                            }
                        });
                        if became {
                            eprintln!(
                                "[link] direct connection to the host {}: game packets go straight to it, not through the hub server",
                                if had { "back" } else { "up" }
                            );
                            next_dhello = now;
                        }
                    }
                    proto::U_PUNCH => {
                        if !with(|st| st.direct.up) {
                            next_dhello = now; // (the host's way is open: say hello now)
                        }
                    }
                    proto::U_DATA if payload.len() >= proto::VPACKET_SIZE => {
                        if let Some(v) = MhguVPacket::parse(payload) {
                            let data = payload[proto::VPACKET_SIZE..].to_vec();
                            with(|st| st.packets.push_back((v, data)));
                        }
                    }
                    _ => {}
                }
                continue;
            }
            with(|st| st.udp_rx = now);
            match kind {
                proto::U_HELLO_ACK => {
                    if let Some(ms) = rtt_of(payload) {
                        rtt[0].add(ms);
                    }
                    if !with(|st| st.udp_up) {
                        with(|st| st.udp_up = true);
                        eprintln!(
                            "[link] UDP to the host works: game packets go by UDP{}",
                            if relayed { " through the hub server" } else { "" }
                        );
                    }
                }
                proto::U_DATA if payload.len() >= proto::VPACKET_SIZE => {
                    if let Some(v) = MhguVPacket::parse(payload) {
                        let data = payload[proto::VPACKET_SIZE..].to_vec();
                        let k = GUEST_RX.fetch_add(1, AtomicOrdering::Relaxed);
                        if k < 5 {
                            eprintln!("[link] guest udp rx: {:08x} -> {:08x} {}:{} proto {} ({} B)",
                                v.src_ip, v.dst_ip, v.src_port, v.dst_port, v.proto, data.len());
                        }
                        with(|st| st.packets.push_back((v, data)));
                    }
                }
                _ => {}
            }
        }
    });
    with(|st| st.udp_thread = Some(handle));
}

/// "host[:port]" -> a socket address (the hub server's port defaults to 39510).
fn parse_server(text: &str) -> Option<SocketAddr> {
    let t = text.trim();
    if t.is_empty() {
        return None;
    }
    let with_port = if t.rfind(':').map(|i| t[i..].chars().skip(1).all(|c| c.is_ascii_digit())).unwrap_or(false) {
        t.to_string()
    } else {
        format!("{t}:39510")
    };
    with_port.parse().ok()
}

/// Hosts through a hub server (`[Multiplayer] Server`): the server lists this hub for players with the
/// same matchmaking key, splices each of them to a link we open for it, and relays the packets. The LAN
/// listener from `mhgu_ldn_host` keeps working.
#[no_mangle]
pub extern "C" fn mhgu_ldn_host_server(server: *const c_char, key: *const c_char) -> bool {
    let Some(addr) = parse_server(&cstr(server)) else {
        eprintln!("[link] host server: bad address {:?}", cstr(server));
        return false;
    };
    let key = cstr(key);
    let room = mhgu_link::room_from_key(&key);
    let info = with(|st| st.info.clone());
    if info.is_empty() {
        eprintln!("[link] host server: no network yet");
        return false;
    }
    let mut sess = match Lobby::new(addr).host(&info, proto::LINK_PROTO_VERSION, room) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("[link] host server {addr}: {e}");
            return false;
        }
    };
    let hub_id = sess.hub_id();
    with(|st| st.own_hub = hub_id);
    eprintln!("[link] hub {hub_id:#x} listed on {addr}");
    let stop = with(|st| Arc::clone(&st.stop));
    std::thread::spawn(move || {
        while !stop.load(Ordering::Relaxed) {
            match sess.poll(Duration::from_millis(200)) {
                Ok(Some(ticket)) => match sess.attach(ticket) {
                    Ok(sp) => {
                        eprintln!("[link] server sent a player (ticket {ticket:#x})");
                        let relay = (sp.pair, sp.secret);
                        // Tell the server where our end of that relay is (ldn.c `relay_register`).
                        if let Some(u) = with(|st| st.udp.clone()) {
                            let p = relay_prefix(relay.0, relay.1);
                            let _ = u.send_to(&p, addr);
                        }
                        serve_peer_relay(sp.stream, Some(relay));
                    }
                    Err(e) => eprintln!("[link] attach: {e}"),
                },
                Ok(None) => {}
                Err(e) => {
                    eprintln!("[link] hub session lost: {e}");
                    break;
                }
            }
            let info = with(|st| st.info.clone());
            sess.set_info(info);
        }
    });
    true
}

/// Joins a hub listed on a server: `hub` is its id from the LIST answer.
#[no_mangle]
pub extern "C" fn mhgu_ldn_join_server(
    server: *const c_char,
    key: *const c_char,
    hub: u64,
    name: *const c_char,
    info: *const u8,
    info_len: u32,
) -> bool {
    let Some(addr) = parse_server(&cstr(server)) else {
        eprintln!("[link] join server: bad address");
        return false;
    };
    let room = mhgu_link::room_from_key(&cstr(key));
    let sp = match Lobby::new(addr).join(proto::LINK_PROTO_VERSION, room, hub) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("[link] join server {addr} hub {hub:#x}: {e}");
            return false;
        }
    };
    let mut hello = Hello::default();
    hello.lcv = LCV.load(Ordering::Relaxed);
    let nm = cstr(name);
    let k = nm.as_bytes().len().min(hello.name.len() - 1);
    hello.name[..k].copy_from_slice(&nm.as_bytes()[..k]);
    hello.mac = join_mac();
    let writer = match sp.stream.try_clone() {
        Ok(w) => Arc::new(Mutex::new(w)),
        Err(e) => {
            eprintln!("[link] join server: {e}");
            return false;
        }
    };
    let mut client = match Client::from_stream(sp.stream, &hello) {
        Ok(c) => c,
        Err(e) => {
            eprintln!("[link] join server: hello: {e}");
            return false;
        }
    };
    let blob = if info.is_null() || info_len == 0 {
        vec![0u8; proto::NETINFO_SIZE]
    } else {
        unsafe { std::slice::from_raw_parts(info, info_len as usize) }.to_vec()
    };
    let mut wel_token: u32 = 0;
    GUEST_RX.store(0, AtomicOrdering::Relaxed);
    GUEST_TX.store(0, AtomicOrdering::Relaxed);
    let (node, info_blob) = match client.reply() {
        Ok(Reply::Welcome { node, token, info }) => {
            wel_token = token;
            (node, if info.is_empty() { blob } else { info })
        }
        Ok(Reply::Refuse(code)) => {
            eprintln!("[link] join server: refused {code:#x}");
            return false;
        }
        other => {
            eprintln!("[link] join server: unexpected answer {other:?}");
            return false;
        }
    };
    let _ = client.set_read_timeout(Some(Duration::from_millis(50)));
    let stop = Arc::new(AtomicBool::new(false));
    with(|st| {
        st.disconnect_reason = 0;
        st.running = true;
        st.hosting = false;
        let (ip, subnet) = joiner_own_ip(&info_blob, node);
        st.subnet = subnet;
        st.own_ip = ip;
        st.info = info_blob;
        st.name = nm;
        st.node = node;
        st.stop = Arc::clone(&stop);
        st.writer = Some(writer); // (the reader thread owns the client; sends write the spliced TCP stream)
    });
    station_udp_start(addr, wel_token, Some((sp.pair, sp.secret)), Arc::clone(&stop));
    let reader_stop = Arc::clone(&stop);
    let handle = std::thread::spawn(move || loop {
        if reader_stop.load(Ordering::Relaxed) {
            break;
        }
        match client.recv() {
            Ok(Event::Packet { vpacket, data }) => {
                let n = GUEST_RX.fetch_add(1, AtomicOrdering::Relaxed);
                if n < 5 {
                    eprintln!(
                        "[link] guest rx: {:08x} -> {:08x} {}:{} proto {} kind {} ({} B)",
                        vpacket.src_ip, vpacket.dst_ip, vpacket.src_port, vpacket.dst_port,
                        vpacket.proto, vpacket.kind, data.len()
                    );
                }
                let v = MhguVPacket {
                    src_ip: vpacket.src_ip,
                    dst_ip: vpacket.dst_ip,
                    src_port: vpacket.src_port,
                    dst_port: vpacket.dst_port,
                    proto: vpacket.proto,
                    kind: vpacket.kind,
                    pad: 0,
                };
                with(|st| st.packets.push_back((v, data)));
            }
            Ok(Event::Aux(d)) => with(|st| st.aux.push_back(d)),
            Ok(Event::Info(i)) => with(|st| st.info = i),
            Ok(Event::Destroy(reason)) => { station_disconnected(&reader_stop, reason); break; },
            Err(e) if e.kind() == io::ErrorKind::WouldBlock || e.kind() == io::ErrorKind::TimedOut => {}
            Err(_) => { station_disconnected(&reader_stop, 6); break; },
        }
    });
    with(|st| st.reader_thread = Some(handle));
    eprintln!("[link] joined hub {hub:#x} on {addr} as node {node}");
    true
}

/// Searches the LAN for hosts listening on `port`: a broadcast probe, then their answers. Returns
/// the number of entries written (at most `max`).
#[no_mangle]
pub extern "C" fn mhgu_ldn_scan(port: u16, timeout_ms: u32, out: *mut MhguScanEntry, max: u32) -> u32 {
    if out.is_null() || max == 0 {
        return 0;
    }
    let socket = match UdpSocket::bind(("0.0.0.0", 0)) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("[link] scan: bind: {e}");
            return 0;
        }
    };
    let _ = socket.set_broadcast(true);
    let mut targets = vec![
        SocketAddr::from((Ipv4Addr::BROADCAST, port)),
        SocketAddr::from((Ipv4Addr::new(127, 0, 0, 1), port)),
    ];
    // The interface broadcasts too: a /24 guess around our own address is what ldn.c sends to.
    if let Ok(local) = socket.local_addr() {
        if let std::net::IpAddr::V4(v4) = local.ip() {
            if !v4.is_loopback() {
                let o = v4.octets();
                targets.push(SocketAddr::from((Ipv4Addr::new(o[0], o[1], o[2], 255), port)));
            }
        }
    }
    let replies = match scan::scan(&socket, &targets, Duration::from_millis(timeout_ms.max(50) as u64)) {
        Ok(r) => r,
        Err(e) => {
            eprintln!("[link] scan: {e}");
            return 0;
        }
    };
    let mut n = 0;
    for r in replies.iter().take(max as usize) {
        let ip = match r.from.ip() {
            std::net::IpAddr::V4(v4) => u32::from_be_bytes(v4.octets()),
            _ => 0,
        };
        let mut e = MhguScanEntry { ip, port: r.port, pad: 0, info: [0u8; proto::NETINFO_SIZE] };
        let k = r.info.len().min(proto::NETINFO_SIZE);
        e.info[..k].copy_from_slice(&r.info[..k]);
        unsafe { std::ptr::write(out.add(n), e) };
        n += 1;
    }
    n as u32
}

/// Joins a host at "a.b.c.d" (the link port is `default_port`).
#[no_mangle]
pub extern "C" fn mhgu_ldn_join(addr: *const c_char, name: *const c_char, info: *const u8, info_len: u32) -> bool {
    let text = cstr(addr);
    let Some(sa) = parse_addr(&text, 0) else {
        eprintln!("[link] join: bad address {text:?}");
        return false;
    };
    let blob = if info.is_null() || info_len == 0 {
        vec![0u8; proto::NETINFO_SIZE]
    } else {
        unsafe { std::slice::from_raw_parts(info, info_len as usize) }.to_vec()
    };
    let mut hello = Hello::default();
    hello.lcv = LCV.load(Ordering::Relaxed);
    let nm = cstr(name);
    let n = nm.as_bytes().len().min(Hello::default().name.len() - 1);
    hello.name[..n].copy_from_slice(&nm.as_bytes()[..n]);
    hello.mac = join_mac();
    let mut client = match Client::connect(sa, &hello) {
        Ok(c) => c,
        Err(e) => {
            eprintln!("[link] join {sa}: {e}");
            return false;
        }
    };
    let writer = match client.stream_clone() {
        Ok(s) => s,
        Err(e) => {
            eprintln!("[link] join {sa}: {e}");
            return false;
        }
    };
    let mut wel_token: Option<u32> = None;
    let (node, info_blob) = match client.reply() {
        Ok(Reply::Welcome { node, token, info }) => {
            wel_token = Some(token);
            (node, if info.is_empty() { blob } else { info })
        }
        Ok(Reply::Refuse(code)) => {
            eprintln!("[link] join {sa}: refused {code:#x}");
            return false;
        }
        Ok(Reply::Info(_)) => {
            eprintln!("[link] join {sa}: the host asked for a scan");
            return false;
        }
        Err(e) => {
            eprintln!("[link] join {sa}: reply: {e}");
            return false;
        }
    };
    let token = match &wel_token {
        Some(t) => *t,
        None => 1,
    };
    let _ = client.set_read_timeout(Some(Duration::from_millis(50)));
    let stop = Arc::new(AtomicBool::new(false));
    GUEST_RX.store(0, AtomicOrdering::Relaxed);
    GUEST_TX.store(0, AtomicOrdering::Relaxed);
    {
        let mut g = STATE.lock().unwrap();
        let st = g.get_or_insert_with(State::new);
        st.disconnect_reason = 0;
        st.running = true;
        st.hosting = false;
        let (ip, subnet) = joiner_own_ip(&info_blob, node);
        st.subnet = subnet;
        st.own_ip = ip;
        st.info = info_blob;
        st.name = nm;
        st.node = node;
        st.listen_port = sa.port();
        st.stop = Arc::clone(&stop);
        st.writer = Some(Arc::new(Mutex::new(writer)));
    }
    station_udp_start(sa, token, None, Arc::clone(&stop));

    // The reader: every frame becomes a delivery / an INFO update.
    let reader_stop = Arc::clone(&stop);
    let handle = std::thread::spawn(move || loop {
        if reader_stop.load(Ordering::Relaxed) {
            break;
        }
        match client.recv() {
            Ok(Event::Packet { vpacket, data }) => {
                let n = GUEST_RX.fetch_add(1, AtomicOrdering::Relaxed);
                if n < 5 {
                    eprintln!(
                        "[link] guest rx: {:08x} -> {:08x} {}:{} proto {} kind {} ({} B)",
                        vpacket.src_ip, vpacket.dst_ip, vpacket.src_port, vpacket.dst_port,
                        vpacket.proto, vpacket.kind, data.len()
                    );
                }
                let v = MhguVPacket {
                    src_ip: vpacket.src_ip,
                    dst_ip: vpacket.dst_ip,
                    src_port: vpacket.src_port,
                    dst_port: vpacket.dst_port,
                    proto: vpacket.proto,
                    kind: vpacket.kind,
                    pad: 0,
                };
                with(|st| st.packets.push_back((v, data)));
            }
            Ok(Event::Aux(d)) => with(|st| st.aux.push_back(d)),
            Ok(Event::Info(i)) => with(|st| st.info = i),
            Ok(Event::Destroy(reason)) => { station_disconnected(&reader_stop, reason); break; },
            Err(e)
                if e.kind() == io::ErrorKind::WouldBlock || e.kind() == io::ErrorKind::TimedOut => {}
            Err(_) => { station_disconnected(&reader_stop, 6); break; },
        }
    });
    with(|st| st.reader_thread = Some(handle));
    eprintln!("[link] joined {sa} as node {node}");
    {
        let stop = Arc::clone(&stop);
        std::thread::spawn(move || {
            std::thread::sleep(Duration::from_secs(6));
            if !stop.load(AtomicOrdering::Relaxed) {
                let (rx, tx) = (GUEST_RX.load(AtomicOrdering::Relaxed), GUEST_TX.load(AtomicOrdering::Relaxed));
                if rx == 0 {
                    eprintln!("[link] guest: no packet from the host after 6 s (sent {tx})");
                } else {
                    eprintln!("[link] guest: {rx} packet(s) from the host, {tx} sent");
                }
            }
        });
    }
    true
}

fn station_disconnected(stop: &Arc<AtomicBool>, reason: u32) {
    with(|st| {
        if Arc::ptr_eq(&st.stop, stop) {
            st.running = false;
            st.disconnect_reason = reason;
            stop.store(true, Ordering::Relaxed);
        }
    });
}

#[no_mangle]
pub extern "C" fn mhgu_ldn_disconnect_reason() -> u32 {
    with(|st| st.disconnect_reason)
}

#[no_mangle]
pub extern "C" fn mhgu_ldn_leave() {
    let (accept, reader, udp, peers, writer) = {
        let mut g = STATE.lock().unwrap();
        match g.as_mut() {
            Some(st) => {
                st.running = false;
                st.own_hub = 0;
                st.udp = None;
                st.usock = None;
                st.udp_up = false;
                st.relay = None;
                st.direct = DirectState::new(false);
                st.packets.clear();
                st.aux.clear();
                let a = st.accept_thread.take();
                let r = st.reader_thread.take();
                let stop = Arc::clone(&st.stop);
                stop.store(true, Ordering::Relaxed);
                let peers: Vec<Arc<Mutex<TcpStream>>> = st.peers.drain(..).map(|p| p.stream).collect();
                (a, r, st.udp_thread.take(), peers, st.writer.take())
            }
            None => return,
        }
    };
    for p in peers {
        let mut stream = p.lock().unwrap();
        let _ = write_frame(&mut stream, proto::F_DESTROY, &[&3u32.to_le_bytes()]);
        let _ = stream.shutdown(std::net::Shutdown::Both);
    }
    if let Some(w) = writer {
        let _ = w.lock().unwrap().shutdown(std::net::Shutdown::Both);
    }
    // The accept loop ends on the flag; joining it releases the listening port before we return.
    if let Some(t) = accept {
        let _ = t.join();
    }
    if let Some(t) = reader { let _ = t.join(); }
    if let Some(t) = udp { let _ = t.join(); }
}

#[no_mangle]
pub extern "C" fn mhgu_ldn_net_up() -> bool {
    with(|st| st.running)
}

#[no_mangle]
pub extern "C" fn mhgu_ldn_hosting() -> bool {
    with(|st| st.running && st.hosting)
}

#[no_mangle]
pub extern "C" fn mhgu_ldn_own_ip() -> u32 {
    with(|st| if st.running { st.own_ip } else { 0 })
}

#[no_mangle]
pub extern "C" fn mhgu_ldn_broadcast_ip() -> u32 {
    with(|st| if st.running { st.subnet | 0xFF } else { 0xFFFF_FFFF })
}

/// Players in the network (`nodes`) and the maximum, and a stable id for it (the host's port).
#[no_mangle]
pub extern "C" fn mhgu_ldn_party(n: *mut i32, max: *mut i32, id: *mut c_char, size: usize) -> bool {
    with(|st| {
        if !st.running {
            return false;
        }
        let nodes = if st.hosting { st.peers.len() as u8 + 1 } else { st.info.get(proto::NI_NODE_COUNT).copied().unwrap_or(2) };
        let maximum = st.info.get(proto::NI_NODE_MAX).copied().unwrap_or(8);
        if !n.is_null() {
            unsafe { *n = nodes as i32 };
        }
        if !max.is_null() {
            unsafe { *max = maximum as i32 };
        }
        if !id.is_null() && size > 0 {
            let text = format!("{:04x}", st.listen_port);
            let bytes = text.as_bytes();
            let k = bytes.len().min(size - 1);
            unsafe {
                std::ptr::copy_nonoverlapping(bytes.as_ptr() as *const c_char, id, k);
                *id.add(k) = 0;
            }
        }
        true
    })
}

/// Copies the current `NetworkInfo` blob out (returns its size, at most `size`).
/// The host's listing id on the hub server the Rust lobby opened (0: not listed).
#[no_mangle]
pub extern "C" fn mhgu_ldn_own_hub() -> u64 {
    with(|st| st.own_hub)
}

/// The host's `NetworkInfo` changed (advertise data, accept policy): keep it and send it to every peer.
#[no_mangle]
pub extern "C" fn mhgu_ldn_update_info(info: *const u8, size: u32) -> bool {
    if info.is_null() || size as usize != proto::NETINFO_SIZE {
        return false;
    }
    let blob = unsafe { std::slice::from_raw_parts(info, size as usize) }.to_vec();
    with(|st| st.info = blob);
    if with(|st| st.hosting) {
        push_info();
    }
    true
}

/// The host sends a player away: `F_DESTROY` with the game's DR_REJECTED (5), then the link is closed.
#[no_mangle]
pub extern "C" fn mhgu_ldn_reject(ip: u32) -> bool {
    let target = with(|st| st.peers.iter().find(|p| p.ip == ip).map(|p| Arc::clone(&p.stream)));
    let Some(stream) = target else { return false };
    {
        let mut s = stream.lock().unwrap();
        let _ = write_frame(&mut s, proto::F_DESTROY, &[&5u32.to_le_bytes()]);
    }
    let _ = stream.lock().unwrap().shutdown(std::net::Shutdown::Both);
    true
}

#[no_mangle]
pub extern "C" fn mhgu_ldn_info(buf: *mut u8, size: u32) -> u32 {
    with(|st| {
        let k = st.info.len().min(size as usize);
        if !buf.is_null() && k > 0 {
            unsafe { std::ptr::copy_nonoverlapping(st.info.as_ptr(), buf, k) };
        }
        k as u32
    })
}

#[no_mangle]
pub extern "C" fn mhgu_ldn_send(p: *const MhguVPacket, data: *const u8, len: u32) {
    if p.is_null() {
        return;
    }
    let v = unsafe { *p };
    let body = if data.is_null() || len == 0 {
        Vec::new()
    } else {
        unsafe { std::slice::from_raw_parts(data, len as usize) }.to_vec()
    };
    let frame = [&v.to_bytes()[..], &body].concat();
    // The station sends straight to the host once the direct path works, else through the relay.
    let station_udp: Option<(Arc<UdpSocket>, u32, Option<(u32, u64)>, SocketAddr)> = with(|st| {
        if st.writer.is_none() {
            return None;
        }
        let now = Instant::now();
        let sock = st.usock.clone()?;
        if st.direct.usable(now) && st.direct.dkey != 0 {
            if let (Some(addr), Some((pair, _))) = (st.direct.peer, st.relay) {
                return Some((sock, st.token, Some((pair, st.direct.dkey)), addr));
            }
        }
        let fresh = now.duration_since(st.udp_rx) < Duration::from_secs(15);
        if st.udp_up && fresh {
            if let Some(server) = st.host_addr {
                return Some((sock, st.token, st.relay, server));
            }
        }
        None
    });
    let sent = GUEST_TX.fetch_add(1, AtomicOrdering::Relaxed);
    if sent < 5 {
        eprintln!(
            "[link] guest tx: {:08x} -> {:08x} {}:{} proto {} ({} B)",
            v.src_ip, v.dst_ip, v.src_port, v.dst_port, v.proto, body.len()
        );
    }
    if let Some((u, token, key, to)) = station_udp {
        let inner = mhgu_link::udp::datagram(token, proto::U_DATA, &frame);
        let d = match key {
            Some((pair, key)) => [&relay_prefix(pair, key)[..], &inner].concat(),
            None => inner,
        };
        let _ = u.send_to(&d, to);
        return;
    }
    if let Some(w) = with(|st| st.writer.clone()) {
        let mut s = w.lock().unwrap();
        let _ = write_frame(&mut s, proto::F_PACKET, &[&frame]);
        return;
    }
    // Per peer: its `MHGD` datagram path when it is up, else the TCP link.
    let udp = with(|st| st.udp.clone());
    let now = std::time::Instant::now();
    // Per peer: the direct path when it works, else its `MHGD` datagram path when it is up, else TCP.
    let peers: Vec<(Arc<Mutex<TcpStream>>, Option<SocketAddr>, u32, bool, Option<(u32, u64)>, Option<(u32, u64, SocketAddr)>)> = with(|st| {
        st.peers
            .iter()
            .map(|p| (
                Arc::clone(&p.stream),
                p.udp,
                p.token,
                now.duration_since(p.udp_rx) < Duration::from_secs(15),
                p.relay,
                match (p.direct.usable(now), p.direct.peer, p.relay) {
                    (true, Some(addr), Some((pair, _))) if p.direct.dkey != 0 => Some((pair, p.direct.dkey, addr)),
                    _ => None,
                },
            ))
            .collect()
    });
    if std::env::var_os("MHGU_TRACE_LINK").is_some() {
        use std::sync::atomic::{AtomicU64, Ordering};
        static N: AtomicU64 = AtomicU64::new(0);
        let n = N.fetch_add(1, Ordering::Relaxed);
        if n < 5 || n % 200 == 0 {
            let udp_up = peers.iter().filter(|p| p.1.is_some() && p.3).count();
            eprintln!("[link] host tx #{} to {} peer(s) ({} by udp), {} bytes", n, peers.len(), udp_up, frame.len());
        }
    }
    for (stream, addr, token, up, relay, direct) in peers {
        let inner = mhgu_link::udp::datagram(token, proto::U_DATA, &frame);
        let send = if let Some((pair, dkey, addr)) = direct {
            Some(([&relay_prefix(pair, dkey)[..], &inner[..]].concat(), addr))
        } else if let (Some(addr), true) = (addr, up) {
            let d = match relay {
                Some((pair, secret)) => [&relay_prefix(pair, secret)[..], &inner[..]].concat(),
                None => inner,
            };
            Some((d, addr))
        } else {
            None
        };
        if let (Some(udp), Some((d, addr))) = (&udp, send) {
            let _ = udp.send_to(&d, addr);
            if std::env::var_os("MHGU_TRACE_LINK").is_some() {
                use std::sync::atomic::{AtomicU64, Ordering};
                static N: AtomicU64 = AtomicU64::new(0);
                let n = N.fetch_add(1, Ordering::Relaxed);
                if n < 5 || n % 200 == 0 {
                    eprintln!("[link] udp tx #{} to {} {} bytes", n, addr, frame.len());
                }
            }
            continue;
        }
        let mut s = stream.lock().unwrap();
        let _ = write_frame(&mut s, proto::F_PACKET, &[&frame]);
    }
}

#[no_mangle]
pub extern "C" fn mhgu_ldn_send_aux(data: *const u8, len: u32) {
    if data.is_null() || len == 0 {
        return;
    }
    let body = unsafe { std::slice::from_raw_parts(data, len as usize) }.to_vec();
    if let Some(w) = with(|st| st.writer.clone()) {
        let mut s = w.lock().unwrap();
        let _ = write_frame(&mut s, proto::F_AUX, &[&body]);
        return;
    }
    let targets: Vec<Arc<Mutex<TcpStream>>> = with(|st| st.peers.iter().map(|p| Arc::clone(&p.stream)).collect());
    for t in targets {
        let mut s = t.lock().unwrap();
        let _ = write_frame(&mut s, proto::F_AUX, &[&body]);
    }
}

/// Hands everything received since the last call to the delivery callbacks (the game's thread).
#[no_mangle]
pub extern "C" fn mhgu_ldn_poll() {
    let (packets, aux) = with(|st| (std::mem::take(&mut st.packets), std::mem::take(&mut st.aux)));
    if let Some(f) = *DELIVER_PACKET.lock().unwrap() {
        for (v, data) in &packets {
            f(v, data.as_ptr(), data.len() as u32);
        }
    }
    if let Some(f) = *DELIVER_AUX.lock().unwrap() {
        for d in &aux {
            f(d.as_ptr(), d.len() as u32);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The query is the relay prefix + `MHGQ` + 12 zero bytes; never bigger than the server's answer.
    #[test]
    fn peer_query_layout() {
        let mut d = DirectState::new(true);
        let now = Instant::now();
        let q = d.query(now, 0x1122_3344, 0x5566_7788_99AA_BBCC, "test").expect("query due");
        assert_eq!(q.len(), proto::PEER_MSG_SIZE);
        assert_eq!(&q[..4], &proto::UDP_MAGIC);
        assert_eq!(u32::from_le_bytes(q[4..8].try_into().unwrap()), 0x1122_3344);
        assert_eq!(u64::from_le_bytes(q[8..16].try_into().unwrap()), 0x5566_7788_99AA_BBCC);
        assert_eq!(&q[16..20], &proto::PEER_QUERY);
        assert_eq!(&q[20..32], &[0u8; 12]);
        // A direct key is only known after the server answered.
        assert_eq!(d.dkey, 0);
        assert!(!d.known());
    }

    /// The answer: `MHGP`, pair, the asker's secret, IPv4, port (big endian), 2 zero bytes, key.
    #[test]
    fn peer_answer_layout() {
        let pair = 0x0102_0304u32;
        let secret = 0x0506_0708_090A_0B0Cu64;
        let dkey = 0x1112_1314_1516_1718u64;
        let mut a = Vec::new();
        a.extend_from_slice(&proto::PEER_ANSWER);
        a.extend_from_slice(&pair.to_le_bytes());
        a.extend_from_slice(&secret.to_le_bytes());
        a.extend_from_slice(&[192, 168, 1, 7]);
        a.extend_from_slice(&54321u16.to_be_bytes());
        a.extend_from_slice(&[0, 0]);
        a.extend_from_slice(&dkey.to_le_bytes());
        let (addr, key) = peer_answer_bytes(&a, pair, secret).expect("answer");
        assert_eq!(addr, "192.168.1.7:54321".parse().unwrap());
        assert_eq!(key, dkey);
        // Another pair or secret, or a query-sized short message, is not our answer.
        assert!(peer_answer_bytes(&a, pair + 1, secret).is_none());
        assert!(peer_answer_bytes(&a, pair, secret ^ 1).is_none());
        assert!(peer_answer_bytes(&a[..a.len() - 1], pair, secret).is_none());
        assert!(peer_answer_bytes(&proto::PEER_QUERY, pair, secret).is_none());
    }

    /// Direct datagrams need the address the server gave (the port may change: the latest wins), and
    /// there is no direct path at all before the answer.
    #[test]
    fn direct_datagrams_need_the_peer_address() {
        let mut d = DirectState::new(true);
        let a: SocketAddr = "203.0.113.9:40000".parse().unwrap();
        let other: SocketAddr = "203.0.113.10:40000".parse().unwrap();
        assert!(!d.direct_from(a), "accepted before the server answered");
        assert!(d.answer(a, 7, Instant::now(), "test"));
        assert_eq!(d.dkey, 7);
        assert!(d.direct_from(a));
        assert!(d.peer == Some(a));
        assert!(!d.direct_from(other), "accepted from another address");
        let moved: SocketAddr = "203.0.113.9:50000".parse().unwrap();
        assert!(d.direct_from(moved));
        assert_eq!(d.peer, Some(moved), "the latest port wins");
        // A repeated answer for the same address updates the key but restarts no schedule.
        let t = Instant::now();
        let mut rep = DirectState::new(true);
        assert!(rep.answer(a, 1, t, "test"));
        let since = rep.p_since;
        assert!(rep.answer(a, 2, t + Duration::from_secs(5), "test"));
        assert_eq!(rep.dkey, 2);
        assert_eq!(rep.p_since, since);
        // Direct = Off: the answer is not taken at all.
        let mut off = DirectState::new(false);
        assert!(!off.answer(a, 9, Instant::now(), "test"));
        assert!(!off.direct_from(a));
    }

    #[test]
    fn set_direct_is_stored() {
        mhgu_ldn_set_direct(0);
        assert!(!DIRECT.load(Ordering::Relaxed));
        mhgu_ldn_set_direct(1);
        assert!(DIRECT.load(Ordering::Relaxed));
    }
}
