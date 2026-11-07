//! Wire-level tests ported from MHGURecomp's `server/server_test.go`, proving this Rust server is
//! behaviourally compatible with the Go one (and therefore with both game clients).

use std::collections::{HashMap, HashSet};
use std::io::{Read, Write};
use std::net::{IpAddr, Ipv4Addr, SocketAddr, TcpStream, UdpSocket};
use std::sync::Arc;
use std::thread;
use std::time::Duration;

use mhgu_hub::proto::{
    self, frame, read_frame, HEADER_SIZE, NETINFO_SIZE,
};
use mhgu_hub::{Config, Server};

struct TestServer {
    addr: SocketAddr,
    _srv: Arc<Server>,
}

fn start(edit: impl FnOnce(&mut Config)) -> TestServer {
    let mut cfg = Config::default();
    cfg.addr = "127.0.0.1:0".into();
    cfg.handshake = Duration::from_secs(2);
    edit(&mut cfg);
    let srv = Arc::new(Server::new(cfg).expect("bind"));
    let addr = srv.addr();
    let s2 = Arc::clone(&srv);
    thread::spawn(move || {
        let _ = s2.serve();
    });
    TestServer { addr, _srv: srv }
}

fn dial(ts: &TestServer) -> TcpStream {
    let c = TcpStream::connect(ts.addr).expect("connect");
    c.set_read_timeout(Some(Duration::from_secs(5))).unwrap();
    c.set_write_timeout(Some(Duration::from_secs(5))).unwrap();
    c
}

fn header(link_proto: u16, room: u8) -> [u8; HEADER_SIZE] {
    let mut h = [0u8; HEADER_SIZE];
    h[..4].copy_from_slice(b"MHGS");
    h[4..6].copy_from_slice(&proto::SERVER_PROTO.to_le_bytes());
    h[6..8].copy_from_slice(&link_proto.to_le_bytes());
    for b in &mut h[8..24] {
        *b = room;
    }
    h
}

fn test_info(players: u8) -> Vec<u8> {
    let mut i = vec![0u8; NETINFO_SIZE];
    i[proto::NI_NET_TYPE] = 2;
    i[proto::NI_SSID] = 10;
    i[proto::NI_NODE_MAX] = 4;
    i[proto::NI_NODE_COUNT] = players;
    i
}

fn send(c: &mut TcpStream, t: u8, payloads: &[&[u8]]) {
    c.write_all(&frame(t, payloads)).expect("write frame");
}

fn expect(c: &mut TcpStream, want: u8) -> Vec<u8> {
    let (t, p) = read_frame(c, 1 << 20).expect("read frame");
    assert_eq!(t, want, "frame type {t}, want {want}");
    p
}

fn host(ts: &TestServer, room: u8, players: u8) -> (TcpStream, u64) {
    let mut c = dial(ts);
    let h = header(2, room);
    let info = test_info(players);
    send(&mut c, proto::T_HOST, &[&h[..], &info[..]]);
    let p = expect(&mut c, proto::T_HOSTED);
    (c, u64::from_le_bytes(p[..8].try_into().unwrap()))
}

fn list(ts: &TestServer, link_proto: u16, room: u8) -> HashMap<u64, Vec<u8>> {
    let mut c = dial(ts);
    let h = header(link_proto, room);
    send(&mut c, proto::T_LIST, &[&h[..]]);
    let p = expect(&mut c, proto::T_HUBS);
    let n = u32::from_le_bytes(p[..4].try_into().unwrap()) as usize;
    let mut out = HashMap::new();
    for i in 0..n {
        let e = &p[4 + i * (8 + NETINFO_SIZE)..];
        let id = u64::from_le_bytes(e[..8].try_into().unwrap());
        out.insert(id, e[8..8 + NETINFO_SIZE].to_vec());
    }
    out
}

/// A header for an arbitrary room (the 16 bytes a matchmaking key hashes to).
fn keyed_header(link_proto: u16, room: [u8; 16]) -> [u8; HEADER_SIZE] {
    let mut h = [0u8; HEADER_SIZE];
    h[..4].copy_from_slice(b"MHGS");
    h[4..6].copy_from_slice(&proto::SERVER_PROTO.to_le_bytes());
    h[6..8].copy_from_slice(&link_proto.to_le_bytes());
    h[8..24].copy_from_slice(&room);
    h
}

fn err_code(p: &[u8]) -> u32 {
    u32::from_le_bytes(p[..4].try_into().unwrap())
}

fn dgram(pair: &[u8], secret: &[u8], payload: &[u8]) -> Vec<u8> {
    let mut d = Vec::new();
    d.extend_from_slice(&proto::UDP_MAGIC);
    d.extend_from_slice(pair);
    d.extend_from_slice(secret);
    d.extend_from_slice(payload);
    d
}

#[test]
fn keys_and_versions() {
    let ts = start(|_| {});
    let (_pub_c, public) = host(&ts, 0, 1);
    let (_fri_c, friends) = host(&ts, 7, 2);

    let l = list(&ts, 2, 0);
    assert_eq!(l.len(), 1);
    assert!(l.contains_key(&public));

    let l = list(&ts, 2, 7);
    assert_eq!(l.len(), 1);
    assert!(l.contains_key(&friends));
    assert_eq!(l[&friends][proto::NI_NODE_COUNT], 2);

    assert!(list(&ts, 2, 8).is_empty(), "another key sees the hub");
    assert!(list(&ts, 3, 0).is_empty(), "another version sees the hub");

    let mut c = dial(&ts);
    let h = header(2, 8);
    send(&mut c, proto::T_JOIN, &[&h[..], &friends.to_le_bytes()]);
    let p = expect(&mut c, proto::T_ERROR);
    assert_eq!(err_code(&p), proto::ERR_NOT_FOUND);
}

#[test]
fn list_random_choice() {
    let ts = start(|c| {
        c.list_max = 4;
        c.max_hubs_per_ip = 20;
        c.max_conns_per_ip = 100;
        c.req_rate = 1000.0;
        c.req_burst = 1000.0;
    });
    // Keep the host connections alive: dropping one closes its hub.
    let mut keep = Vec::new();
    for i in 0..8 {
        keep.push(host(&ts, 0, (1 + i % 3) as u8));
    }
    let mut full = HashSet::new();
    for _ in 0..3 {
        let (c, id) = host(&ts, 0, 4);
        full.insert(id);
        keep.push((c, id));
    }

    let mut seen = HashSet::new();
    for _ in 0..30 {
        let mut c = dial(&ts);
        let h = header(2, 0);
        send(&mut c, proto::T_LIST, &[&h[..]]);
        let p = expect(&mut c, proto::T_HUBS);
        drop(c);
        assert_eq!(u32::from_le_bytes(p[..4].try_into().unwrap()), 4);
        let mut last = 255u8;
        for i in 0..4 {
            let e = &p[4 + i * (8 + NETINFO_SIZE)..];
            let id = u64::from_le_bytes(e[..8].try_into().unwrap());
            let players = e[8 + proto::NI_NODE_COUNT];
            assert!(
                !full.contains(&id),
                "a full hub was listed while hubs with a free place were left out"
            );
            assert!(players <= last, "answer not sorted: {players} after {last}");
            last = players;
            seen.insert(id);
        }
    }
    assert!(seen.len() > 4, "30 searches saw only {} hubs", seen.len());
}

#[test]
fn update_and_close() {
    let ts = start(|_| {});
    let (mut hc, id) = host(&ts, 0, 1);
    send(&mut hc, proto::T_UPDATE, &[&test_info(3)[..]]);
    thread::sleep(Duration::from_millis(100));
    let l = list(&ts, 2, 0);
    assert!(l.contains_key(&id));
    assert_eq!(l[&id][proto::NI_NODE_COUNT], 3);

    let mut bad = test_info(1);
    bad[proto::NI_NODE_COUNT] = 9;
    send(&mut hc, proto::T_UPDATE, &[&bad[..]]); // (the server drops the hub)
    thread::sleep(Duration::from_millis(100));
    assert!(list(&ts, 2, 0).is_empty(), "hub with a bad NetworkInfo still listed");
}

#[test]
fn bad_requests() {
    let ts = start(|_| {});
    let mut c = dial(&ts);
    let h = header(2, 0);
    let info = test_info(5); // more players than the maximum
    send(&mut c, proto::T_HOST, &[&h[..], &info[..]]);
    expect(&mut c, proto::T_ERROR);

    let mut c2 = dial(&ts);
    c2.write_all(&[0xff, 0xff, 0xff, 0x7f, 1]).unwrap(); // huge frame
    c2.set_read_timeout(Some(Duration::from_secs(2))).unwrap();
    let mut b = [0u8; 1];
    match c2.read(&mut b) {
        Ok(0) => {}
        Err(_) => {}
        Ok(_) => panic!("connection still open"),
    }
}

#[test]
fn join_splice_relay() {
    let ts = start(|_| {});
    let (mut hc, id) = host(&ts, 0, 1);
    let mut pc = dial(&ts);
    let h = header(2, 0);
    send(&mut pc, proto::T_JOIN, &[&h[..], &id.to_le_bytes()]);
    let ticket = expect(&mut hc, proto::T_INCOMING);

    let mut ac = dial(&ts);
    send(&mut ac, proto::T_ATTACH, &[&h[..], &ticket]);
    let hs = expect(&mut ac, proto::T_SPLICED);
    let ps = expect(&mut pc, proto::T_SPLICED);
    assert_eq!(&hs[..4], &ps[..4], "pair ids differ");
    assert_ne!(&hs[4..], &ps[4..], "secrets are the same");

    // The link: bytes both ways, untouched.
    ac.write_all(b"from host").unwrap();
    pc.write_all(b"from player").unwrap();
    let mut b = [0u8; 64];
    let n = pc.read(&mut b).unwrap();
    assert_eq!(&b[..n], b"from host");
    let n = ac.read(&mut b).unwrap();
    assert_eq!(&b[..n], b"from player");

    // UDP: the host announces itself, the player's datagram reaches it without the prefix.
    let ua = SocketAddr::new(IpAddr::V4(Ipv4Addr::LOCALHOST), ts.addr.port());
    let hu = UdpSocket::bind("127.0.0.1:0").unwrap();
    hu.connect(ua).unwrap();
    let pu = UdpSocket::bind("127.0.0.1:0").unwrap();
    pu.connect(ua).unwrap();

    hu.send(&dgram(&ps[..4], &hs[4..12], b"")).unwrap();
    thread::sleep(Duration::from_millis(50));
    pu.send(&dgram(&ps[..4], &ps[4..12], b"MHGDhello")).unwrap();
    pu.send(&dgram(&ps[..4], &[0u8; 8], b"forged")).unwrap(); // wrong secret: dropped

    hu.set_read_timeout(Some(Duration::from_secs(2))).unwrap();
    let n = hu.recv(&mut b).unwrap();
    assert_eq!(&b[..n], b"MHGDhello");

    hu.send(&dgram(&ps[..4], &hs[4..12], b"MHGDack")).unwrap();
    pu.set_read_timeout(Some(Duration::from_secs(2))).unwrap();
    let n = pu.recv(&mut b).unwrap();
    assert_eq!(&b[..n], b"MHGDack");

    hu.set_read_timeout(Some(Duration::from_millis(200))).unwrap();
    assert!(hu.recv(&mut b).is_err(), "forged datagram relayed");

    // The player leaves: the host's side of the link closes.
    drop(pc);
    ac.set_read_timeout(Some(Duration::from_secs(2))).unwrap();
    let closed = match ac.read(&mut b) {
        Ok(0) | Err(_) => true,
        Ok(_) => false,
    };
    assert!(closed, "host's link still open");
}

/// `-key`: only that key's room is served; every other room is told "no such hub", which is the
/// answer a client with another key has always got. The wire is unchanged.
#[test]
fn keyed_server_serves_only_its_room() {
    let key = "s3cret-room";
    let ts = start(|c| c.key = key.into());
    let room = proto::room_from_key(key);

    // Hosting from the public room is refused the way a foreign room always is.
    let mut c = dial(&ts);
    let h = header(2, 0);
    let info = test_info(1);
    send(&mut c, proto::T_HOST, &[&h[..], &info[..]]);
    assert_eq!(err_code(&expect(&mut c, proto::T_ERROR)), proto::ERR_NOT_FOUND);

    // The server's own room is served, and listed.
    let mut hc = dial(&ts);
    let kh = keyed_header(2, room);
    send(&mut hc, proto::T_HOST, &[&kh[..], &info[..]]);
    let id = u64::from_le_bytes(expect(&mut hc, proto::T_HOSTED)[..8].try_into().unwrap());

    let mut lc = dial(&ts);
    send(&mut lc, proto::T_LIST, &[&kh[..]]);
    let p = expect(&mut lc, proto::T_HUBS);
    let n = u32::from_le_bytes(p[..4].try_into().unwrap());
    assert_eq!(n, 1, "the keyed hub is listed");
    assert_eq!(u64::from_le_bytes(p[4..12].try_into().unwrap()), id);

    // The public room sees nothing and cannot join it.
    let mut lc = dial(&ts);
    let pubh = header(2, 0);
    send(&mut lc, proto::T_LIST, &[&pubh[..]]);
    let p = expect(&mut lc, proto::T_HUBS);
    assert_eq!(u32::from_le_bytes(p[..4].try_into().unwrap()), 0);

    let mut jc = dial(&ts);
    send(&mut jc, proto::T_JOIN, &[&pubh[..], &id.to_le_bytes()]);
    assert_eq!(err_code(&expect(&mut jc, proto::T_ERROR)), proto::ERR_NOT_FOUND);
}

#[test]
fn host_does_not_answer() {
    let ts = start(|c| c.handshake = Duration::from_millis(300));
    let (mut hc, id) = host(&ts, 0, 1);
    let mut pc = dial(&ts);
    let h = header(2, 0);
    send(&mut pc, proto::T_JOIN, &[&h[..], &id.to_le_bytes()]);
    let ticket = expect(&mut hc, proto::T_INCOMING);

    let p = expect(&mut pc, proto::T_ERROR);
    assert_eq!(err_code(&p), proto::ERR_NO_ANSWER);

    let mut ac = dial(&ts); // too late: the ticket is gone
    send(&mut ac, proto::T_ATTACH, &[&h[..], &ticket]);
    expect(&mut ac, proto::T_ERROR);
}

#[test]
fn rate_limit() {
    let ts = start(|c| {
        c.req_rate = 0.1;
        c.req_burst = 3.0;
    });
    for _ in 0..3 {
        list(&ts, 2, 0);
    }
    let mut c = dial(&ts);
    let h = header(2, 0);
    send(&mut c, proto::T_LIST, &[&h[..]]);
    let p = expect(&mut c, proto::T_ERROR);
    assert_eq!(err_code(&p), proto::ERR_BUSY);
}

#[test]
fn hub_limit_per_ip() {
    let ts = start(|c| c.max_hubs_per_ip = 2);
    let (_h1, _) = host(&ts, 0, 1);
    let (_h2, _) = host(&ts, 0, 1);
    let mut c = dial(&ts);
    let h = header(2, 0);
    let info = test_info(1);
    send(&mut c, proto::T_HOST, &[&h[..], &info[..]]);
    let p = expect(&mut c, proto::T_ERROR);
    assert_eq!(err_code(&p), proto::ERR_BUSY);
}

#[test]
fn live_min_proto_closes_old_hubs() {
    let ts = start(|_| {});
    let (mut host_conn, _id) = host(&ts, 0, 1);
    let mut live = ts._srv.live();
    live.min_proto = 3;
    assert_eq!(ts._srv.apply(live), 1, "the old hub is closed");
    // The host was told the hub is gone, and older clients are refused from now on.
    let _ = expect(&mut host_conn, proto::T_ERROR);
    let mut c = dial(&ts);
    send(&mut c, proto::T_LIST, &[&header(2, 0)[..]]);
    assert_eq!(err_code(&expect(&mut c, proto::T_ERROR)), proto::ERR_VERSION);
}
