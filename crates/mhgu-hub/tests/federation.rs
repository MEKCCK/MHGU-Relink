//! Two hub servers federating: a player on A lists and joins a hub hosted on B, with TCP and UDP
//! relayed through both.

use std::io::{Read, Write};
use std::net::{SocketAddr, TcpStream, UdpSocket};
use std::sync::Arc;
use std::thread;
use std::time::Duration;

use mhgu_hub::proto::{self, frame, read_frame, HEADER_SIZE, NETINFO_SIZE};
use mhgu_hub::{Config, Peer, Server};

struct TestServer {
    addr: SocketAddr,
    _srv: Arc<Server>,
}

fn start(edit: impl FnOnce(&mut Config)) -> TestServer {
    let mut cfg = Config::default();
    cfg.addr = "127.0.0.1:0".into();
    cfg.web = String::new();
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

fn info(players: u8, session: u8) -> Vec<u8> {
    let mut i = vec![0u8; NETINFO_SIZE];
    i[proto::NI_NET_TYPE] = 2;
    i[proto::NI_SSID] = 10;
    i[proto::NI_NODE_MAX] = 4;
    i[proto::NI_NODE_COUNT] = players;
    i[proto::NI_SESSION_ID] = session;
    i[proto::NI_BSSID..proto::NI_BSSID + 6].fill(session);
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

/// A protocol 4 hub hosted on a server; returns its connection and id.
fn host(ts: &TestServer, room: u8, session: u8, code: Option<&[u8; proto::CODE_LEN]>) -> (TcpStream, u64) {
    let mut c = dial(ts);
    let h = header(4, room);
    let i = info(1, session);
    match code {
        Some(k) => send(&mut c, proto::T_HOST, &[&h[..], &i[..], k]),
        None => send(&mut c, proto::T_HOST, &[&h[..], &i[..]]),
    }
    let p = expect(&mut c, proto::T_HOSTED);
    (c, u64::from_le_bytes(p[..8].try_into().unwrap()))
}

fn list(ts: &TestServer, room: u8) -> Vec<(u64, u16, [u8; proto::CODE_LEN], Vec<u8>)> {
    let mut c = dial(ts);
    send(&mut c, proto::T_LIST, &[&header(4, room)[..]]);
    let p = expect(&mut c, proto::T_HUBS4);
    let n = u32::from_le_bytes(p[..4].try_into().unwrap()) as usize;
    let entry = 8 + 2 + proto::CODE_LEN + NETINFO_SIZE;
    let mut out = Vec::new();
    for i in 0..n {
        let at = 4 + i * entry;
        let id = u64::from_le_bytes(p[at..at + 8].try_into().unwrap());
        let lp = u16::from_le_bytes(p[at + 8..at + 10].try_into().unwrap());
        let mut code = [0u8; proto::CODE_LEN];
        code.copy_from_slice(&p[at + 10..at + 10 + proto::CODE_LEN]);
        out.push((id, lp, code, p[at + 10 + proto::CODE_LEN..at + entry].to_vec()));
    }
    out
}

/// Joins `alias` on A and services the host side on B: the two links and both relay pairs.
struct Joined {
    player: TcpStream,
    host_side: TcpStream,
    /// B's pair to A, and the host's secret.
    peer_pair: u32,
    host_secret: u64,
    /// A's pair to the player, and the player's secret.
    local_pair: u32,
    local_secret: u64,
}

fn join_through(
    a: &TestServer,
    b: &TestServer,
    host_conn: &mut TcpStream,
    alias: u64,
    room: u8,
    code: Option<&[u8; proto::CODE_LEN]>,
) -> Joined {
    let mut player = dial(a);
    let h = header(4, room);
    match code {
        Some(k) => send(&mut player, proto::T_JOIN, &[&h[..], &alias.to_le_bytes(), k]),
        None => send(&mut player, proto::T_JOIN, &[&h[..], &alias.to_le_bytes()]),
    }
    let p = expect(host_conn, proto::T_INCOMING);
    let ticket = u64::from_le_bytes(p[..8].try_into().unwrap());
    let mut att = dial(b);
    send(&mut att, proto::T_ATTACH, &[&h[..], &ticket.to_le_bytes()]);
    let host_sp = expect(&mut att, proto::T_SPLICED);
    let player_sp = expect(&mut player, proto::T_SPLICED);
    // The two legs have their own pair: A's to the player, B's to the host.
    Joined {
        player,
        host_side: att,
        peer_pair: u32::from_le_bytes(host_sp[..4].try_into().unwrap()),
        host_secret: u64::from_le_bytes(host_sp[4..12].try_into().unwrap()),
        local_pair: u32::from_le_bytes(player_sp[..4].try_into().unwrap()),
        local_secret: u64::from_le_bytes(player_sp[4..12].try_into().unwrap()),
    }
}

fn a_with_peer(b: &TestServer, token: &str) -> TestServer {
    start(|c| {
        c.federation_token = token.into();
        c.federate = vec![Peer { addr: b.addr.to_string(), token: token.into(), name: "B".into(), key: String::new() }];
    })
}

#[test]
fn lists_and_joins_a_peers_hub() {
    let b = start(|c| c.federation_token = "tok".into());
    let (mut host_conn, hub_b) = host(&b, 0, 7, None);
    let a = a_with_peer(&b, "tok");

    let hubs = list(&a, 0);
    assert_eq!(hubs.len(), 1);
    let (alias, lp, code, ref_info) = hubs[0].clone();
    assert_ne!(alias, hub_b, "remote hub has a local alias");
    assert_eq!(lp, 4);
    assert_eq!(ref_info.len(), NETINFO_SIZE);
    assert_eq!(&ref_info[proto::NI_SESSION_ID..proto::NI_SESSION_ID + 16], &info(1, 7)[proto::NI_SESSION_ID..proto::NI_SESSION_ID + 16]);
    let _ = code;

    let mut j = join_through(&a, &b, &mut host_conn, alias, 0, None);
    j.player.write_all(b"from player").unwrap();
    let mut got = [0u8; 11];
    j.host_side.read_exact(&mut got).unwrap();
    assert_eq!(&got, b"from player");
    j.host_side.write_all(b"from host").unwrap();
    let mut back = [0u8; 9];
    j.player.read_exact(&mut back).unwrap();
    assert_eq!(&back, b"from host");
}

#[test]
fn transitive_advertisement_lists_and_joins_through_a_middle_server() {
    let c_srv = start(|c| { c.federation_token = "tok".into(); c.federation_name = "C".into(); });
    let (mut host_conn, _) = host(&c_srv, 0, 41, None);
    let b = start(|c| {
        c.federation_token = "tok".into();
        c.federation_name = "B".into();
        c.federation_transitive = 1;
        c.federate = vec![Peer {
            addr: c_srv.addr.to_string(),
            token: "tok".into(),
            name: "C".into(),
            key: String::new(),
        }];
    });
    let a = start(|c| {
        c.federation_token = "tok".into();
        c.federation_name = "A".into();
        c.federation_transitive = 1;
        c.federate = vec![Peer {
            addr: b.addr.to_string(),
            token: "tok".into(),
            name: "B".into(),
            key: String::new(),
        }];
    });

    thread::sleep(Duration::from_millis(300));
    let hubs = list(&a, 0);
    assert_eq!(hubs.len(), 1, "A sees C's hub two hops away");
    let alias = hubs[0].0;
    assert_ne!(alias, 0);

    // JOIN A -> B -> C: the player is on A, the host connection is on C.
    let mut player = dial(&a);
    send(&mut player, proto::T_JOIN, &[&header(4, 0)[..], &alias.to_le_bytes()]);
    let p = expect(&mut host_conn, proto::T_INCOMING);
    let ticket = u64::from_le_bytes(p[..8].try_into().unwrap());
    let mut host_attach = dial(&c_srv);
    send(&mut host_attach, proto::T_ATTACH, &[&header(4, 0)[..], &ticket.to_le_bytes()]);
    let host_sp = expect(&mut host_attach, proto::T_SPLICED);
    let player_sp = expect(&mut player, proto::T_SPLICED);
    let peer_pair = u32::from_le_bytes(host_sp[..4].try_into().unwrap());
    let host_secret = u64::from_le_bytes(host_sp[4..12].try_into().unwrap());
    let local_pair = u32::from_le_bytes(player_sp[..4].try_into().unwrap());
    let local_secret = u64::from_le_bytes(player_sp[4..12].try_into().unwrap());

    player.write_all(b"three hops").unwrap();
    let mut got = [0u8; 10];
    host_attach.read_exact(&mut got).unwrap();
    assert_eq!(&got, b"three hops");
    host_attach.write_all(b"back").unwrap();
    let mut back = [0u8; 4];
    player.read_exact(&mut back).unwrap();
    assert_eq!(&back, b"back");

    // UDP crosses the same two bridges.
    let player_udp = UdpSocket::bind("127.0.0.1:0").unwrap();
    let host_udp = UdpSocket::bind("127.0.0.1:0").unwrap();
    player_udp.set_read_timeout(Some(Duration::from_secs(3))).unwrap();
    host_udp.set_read_timeout(Some(Duration::from_secs(3))).unwrap();
    let reg = |pair: u32, secret: u64| {
        let mut d = Vec::new();
        d.extend_from_slice(&proto::UDP_MAGIC);
        d.extend_from_slice(&pair.to_le_bytes());
        d.extend_from_slice(&secret.to_le_bytes());
        d
    };
    host_udp.send_to(&reg(peer_pair, host_secret), c_srv.addr).unwrap();
    player_udp.send_to(&reg(local_pair, local_secret), a.addr).unwrap();
    thread::sleep(Duration::from_millis(100));
    let mut dg = reg(local_pair, local_secret);
    dg.extend_from_slice(b"chain-up");
    player_udp.send_to(&dg, a.addr).unwrap();
    let mut buf = [0u8; 64];
    let (n, _) = host_udp.recv_from(&mut buf).expect("host got the chained datagram");
    assert_eq!(&buf[..n], b"chain-up");
    let mut dg = reg(peer_pair, host_secret);
    dg.extend_from_slice(b"chain-down");
    host_udp.send_to(&dg, c_srv.addr).unwrap();
    let (n, _) = player_udp.recv_from(&mut buf).expect("player got the chained datagram");
    assert_eq!(&buf[..n], b"chain-down");
}

#[test]
fn transitive_advertisement_and_join_survive_three_servers() {
    let code = *b"DEEP2345";
    let d = start(|c| { c.federation_token = "tok".into(); c.federation_name = "D".into(); });
    let (mut host_conn, _) = host(&d, 0, 44, Some(&code));
    let c_srv = start(|c| {
        c.federation_token = "tok".into();
        c.federation_name = "C".into();
        c.federation_transitive = 2;
        c.federate = vec![Peer { addr: d.addr.to_string(), token: "tok".into(), name: "D".into(), key: String::new() }];
    });
    let b = start(|c| {
        c.federation_token = "tok".into();
        c.federation_name = "B".into();
        c.federation_transitive = 2;
        c.federate = vec![Peer { addr: c_srv.addr.to_string(), token: "tok".into(), name: "C".into(), key: String::new() }];
    });
    let a = start(|c| {
        c.federation_token = "tok".into();
        c.federation_name = "A".into();
        c.federation_transitive = 2;
        c.federate = vec![Peer { addr: b.addr.to_string(), token: "tok".into(), name: "B".into(), key: String::new() }];
    });

    let mut hubs = Vec::new();
    for _ in 0..10 {
        thread::sleep(Duration::from_millis(300));
        hubs = list(&a, 0);
        if !hubs.is_empty() { break; }
    }
    assert_eq!(hubs.len(), 1, "A sees D's hub three hops away");

    // FIND recurses the same two extra hops, and its alias joins back through them.
    let mut f = dial(&a);
    send(&mut f, proto::T_FIND, &[&header(4, 0)[..], &code]);
    let p = expect(&mut f, proto::T_HUBS4);
    assert_eq!(u32::from_le_bytes(p[..4].try_into().unwrap()), 1);
    let alias = u64::from_le_bytes(p[4..12].try_into().unwrap());

    let mut player = dial(&a);
    send(&mut player, proto::T_JOIN, &[&header(4, 0)[..], &alias.to_le_bytes(), &code]);
    let inc = expect(&mut host_conn, proto::T_INCOMING);
    let ticket = u64::from_le_bytes(inc[..8].try_into().unwrap());
    let mut host_attach = dial(&d);
    send(&mut host_attach, proto::T_ATTACH, &[&header(4, 0)[..], &ticket.to_le_bytes()]);
    let _ = expect(&mut host_attach, proto::T_SPLICED);
    let _ = expect(&mut player, proto::T_SPLICED);
    player.write_all(b"deep").unwrap();
    let mut got = [0u8; 4];
    host_attach.read_exact(&mut got).unwrap();
    assert_eq!(&got, b"deep");
}

#[test]
fn presence_crosses_a_middle_server() {
    let c_srv = start(|c| {
        c.federation_token = "tok".into();
        c.federation_transitive = 1;
    });
    let b = start(|c| {
        c.federation_token = "tok".into();
        c.federation_transitive = 1;
        c.federate = vec![Peer {
            addr: c_srv.addr.to_string(),
            token: "tok".into(),
            name: "C".into(),
            key: String::new(),
        }];
    });
    let a = start(|c| {
        c.federation_token = "tok".into();
        c.federation_transitive = 1;
        c.federate = vec![Peer {
            addr: b.addr.to_string(),
            token: "tok".into(),
            name: "B".into(),
            key: String::new(),
        }];
    });
    thread::sleep(Duration::from_millis(700)); // A-B and B-C presence streams

    let sp = [0x61u8; proto::FRIEND_SECRET_LEN];
    let sq = [0x62u8; proto::FRIEND_SECRET_LEN];
    let sd = [0x63u8; proto::FRIEND_SECRET_LEN];
    let (ip, iq, id) = (proto::friend_id(&sp), proto::friend_id(&sq), proto::friend_id(&sd));
    let code_a = *b"PRES2345";
    let code_c = *b"PRES3456";

    // P (A) and Q (C) list each other; both poll so their servers broadcast.
    let _ = poll_presence(&a, &presence_body(&sp, 1, &code_a, &[iq], &[], "P"));
    let _ = poll_presence(&c_srv, &presence_body(&sq, 2, &code_c, &[ip], &[], "Q"));
    thread::sleep(Duration::from_millis(900));

    let p = poll_presence(&a, &presence_body(&sp, 1, &code_a, &[iq], &[], "P"));
    assert!(online_ids(&p).contains(&iq), "P sees Q two hops away");
    let q = poll_presence(&c_srv, &presence_body(&sq, 2, &code_c, &[ip], &[], "Q"));
    assert!(online_ids(&q).contains(&ip), "Q sees P two hops away");

    // D (C) lists P but P does not list D: the request reaches A through B.
    let _ = poll_presence(&c_srv, &presence_body(&sd, 0, &[0; 8], &[ip], &[], "D"));
    thread::sleep(Duration::from_millis(700));
    let p = poll_presence(&a, &presence_body(&sp, 1, &code_a, &[iq], &[], "P"));
    assert!(
        requests(&p).iter().any(|(rid, name)| *rid == id && name == "D"),
        "P sees D's request two hops away"
    );

    // Q removes P; the removal also crosses the middle server.
    let _ = poll_presence(&c_srv, &presence_body(&sq, 2, &code_c, &[ip], &[ip], "Q"));
    thread::sleep(Duration::from_millis(700));
    let p = poll_presence(&a, &presence_body(&sp, 1, &code_a, &[iq], &[], "P"));
    assert!(removed_ids(&p).contains(&iq), "P is told Q removed it two hops away");
}

#[test]
fn a_player_who_stops_polling_is_announced_offline() {
    let b = start(|c| {
        c.federation_token = "tok".into();
        c.presence_ttl = Duration::from_secs(10);
    });
    let a = start(|c| {
        c.federation_token = "tok".into();
        c.presence_ttl = Duration::from_secs(1);
        c.federate = vec![Peer {
            addr: b.addr.to_string(),
            token: "tok".into(),
            name: "B".into(),
            key: String::new(),
        }];
    });
    thread::sleep(Duration::from_millis(500));

    let sp = [0x81u8; proto::FRIEND_SECRET_LEN];
    let sq = [0x82u8; proto::FRIEND_SECRET_LEN];
    let sr = [0x83u8; proto::FRIEND_SECRET_LEN];
    let (ip, iq) = (proto::friend_id(&sp), proto::friend_id(&sq));
    let code_a = *b"OFFL2345";
    let code_b = *b"OFFL3456";

    let _ = poll_presence(&a, &presence_body(&sp, 1, &code_a, &[iq], &[], "P"));
    let _ = poll_presence(&b, &presence_body(&sq, 2, &code_b, &[ip], &[], "Q"));
    thread::sleep(Duration::from_millis(700));
    let q = poll_presence(&b, &presence_body(&sq, 2, &code_b, &[ip], &[], "Q"));
    assert!(online_ids(&q).contains(&ip), "Q sees P while P polls");

    // P stops polling. Its 1 s TTL expires on A; another local poll makes A prune and send a tombstone.
    thread::sleep(Duration::from_millis(1200));
    let _ = poll_presence(&a, &presence_body(&sr, 0, &[0; 8], &[], &[], "R"));
    thread::sleep(Duration::from_millis(300));
    let q = poll_presence(&b, &presence_body(&sq, 2, &code_b, &[ip], &[], "Q"));
    assert!(
        !online_ids(&q).contains(&ip),
        "Q drops P immediately after A's natural-offline tombstone, without waiting for B's 10 s TTL"
    );
}

#[test]
fn fifty_server_chain_lists_finds_presences_and_joins() {
    const N: usize = 50;
    let token = "tok";
    let mut servers: Vec<TestServer> = Vec::with_capacity(N);
    let mut next_addr: Option<SocketAddr> = None;
    for i in (0..N).rev() {
        let peer = next_addr.map(|addr| Peer {
            addr: addr.to_string(),
            token: token.into(),
            name: format!("H{}", i + 2),
            key: String::new(),
        });
        let ts = start(|c| {
            c.federation_token = token.into();
            c.federation_name = format!("H{}", i + 1);
            c.federation_transitive = 64;
            if let Some(p) = peer.clone() {
                c.federate = vec![p];
            }
        });
        next_addr = Some(ts.addr);
        servers.push(ts);
    }
    servers.reverse(); // H1 -> H2 -> ... -> H50

    // Give the 49 duplex presence streams a moment to connect.
    thread::sleep(Duration::from_millis(1000));

    let sp = [0x91u8; proto::FRIEND_SECRET_LEN];
    let sq = [0x92u8; proto::FRIEND_SECRET_LEN];
    let (ip, iq) = (proto::friend_id(&sp), proto::friend_id(&sq));
    let code_a = *b"F5FT2345";
    let code_b = *b"F5FT3456";
    let _ = poll_presence(&servers[0], &presence_body(&sp, 1, &code_a, &[iq], &[], "P"));
    let _ = poll_presence(&servers[N - 1], &presence_body(&sq, 2, &code_b, &[ip], &[], "Q"));

    // A hub at the far end: H1 lists it and finds its code through 49 peers.
    let code = *b"F5FT45Q0";
    let (mut host_conn, _) = host(&servers[N - 1], 7, 61, Some(&code));
    let mut hubs = Vec::new();
    for _ in 0..6 {
        hubs = list(&servers[0], 7);
        if !hubs.is_empty() {
            break;
        }
        thread::sleep(Duration::from_millis(300));
    }
    assert_eq!(hubs.len(), 1, "H1 sees the hub at H50 across 49 servers");

    let mut f = dial(&servers[0]);
    f.set_read_timeout(Some(Duration::from_secs(20))).unwrap();
    send(&mut f, proto::T_FIND, &[&header(4, 0)[..], &code]);
    let p = expect(&mut f, proto::T_HUBS4);
    assert_eq!(u32::from_le_bytes(p[..4].try_into().unwrap()), 1);
    let alias = u64::from_le_bytes(p[4..12].try_into().unwrap());

    // JOIN H1 -> ... -> H50: 49 nested bridges, then the host side is on H50.
    let mut player = dial(&servers[0]);
    player.set_read_timeout(Some(Duration::from_secs(20))).unwrap();
    player.set_write_timeout(Some(Duration::from_secs(20))).unwrap();
    send(&mut player, proto::T_JOIN, &[&header(4, 0)[..], &alias.to_le_bytes(), &code]);
    host_conn.set_read_timeout(Some(Duration::from_secs(20))).unwrap();
    let inc = expect(&mut host_conn, proto::T_INCOMING);
    let ticket = u64::from_le_bytes(inc[..8].try_into().unwrap());
    let mut host_attach = dial(&servers[N - 1]);
    host_attach.set_read_timeout(Some(Duration::from_secs(20))).unwrap();
    send(&mut host_attach, proto::T_ATTACH, &[&header(4, 0)[..], &ticket.to_le_bytes()]);
    let host_sp = expect(&mut host_attach, proto::T_SPLICED);
    let player_sp = expect(&mut player, proto::T_SPLICED);
    let peer_pair = u32::from_le_bytes(host_sp[..4].try_into().unwrap());
    let host_secret = u64::from_le_bytes(host_sp[4..12].try_into().unwrap());
    let local_pair = u32::from_le_bytes(player_sp[..4].try_into().unwrap());
    let local_secret = u64::from_le_bytes(player_sp[4..12].try_into().unwrap());
    player.write_all(b"fifty-hop").unwrap();
    let mut got = [0u8; 9];
    host_attach.read_exact(&mut got).unwrap();
    assert_eq!(&got, b"fifty-hop");
    host_attach.write_all(b"fifty-back").unwrap();
    let mut back = [0u8; 10];
    player.read_exact(&mut back).unwrap();
    assert_eq!(&back, b"fifty-back");

    // UDP crosses all 49 bridges with a prefix rewrite at every hop.
    let player_udp = UdpSocket::bind("127.0.0.1:0").unwrap();
    let host_udp = UdpSocket::bind("127.0.0.1:0").unwrap();
    player_udp.set_read_timeout(Some(Duration::from_secs(10))).unwrap();
    host_udp.set_read_timeout(Some(Duration::from_secs(10))).unwrap();
    let reg = |pair: u32, secret: u64| {
        let mut d = Vec::new();
        d.extend_from_slice(&proto::UDP_MAGIC);
        d.extend_from_slice(&pair.to_le_bytes());
        d.extend_from_slice(&secret.to_le_bytes());
        d
    };
    host_udp.send_to(&reg(peer_pair, host_secret), servers[N - 1].addr).unwrap();
    player_udp.send_to(&reg(local_pair, local_secret), servers[0].addr).unwrap();
    thread::sleep(Duration::from_millis(100));
    let mut dg = reg(local_pair, local_secret);
    dg.extend_from_slice(b"fifty-up");
    player_udp.send_to(&dg, servers[0].addr).unwrap();
    let mut buf = [0u8; 64];
    let (n, _) = host_udp.recv_from(&mut buf).expect("host got the 49-hop UDP datagram");
    assert_eq!(&buf[..n], b"fifty-up");
    let mut dg = reg(peer_pair, host_secret);
    dg.extend_from_slice(b"fifty-down");
    host_udp.send_to(&dg, servers[N - 1].addr).unwrap();
    let (n, _) = player_udp.recv_from(&mut buf).expect("player got the 49-hop UDP datagram");
    assert_eq!(&buf[..n], b"fifty-down");

    // Presence crosses the same chain in both directions.
    let mut p_ok = false;
    let mut q_ok = false;
    for _ in 0..10 {
        let p = poll_presence(&servers[0], &presence_body(&sp, 1, &code_a, &[iq], &[], "P"));
        p_ok = online_ids(&p).contains(&iq);
        let q = poll_presence(&servers[N - 1], &presence_body(&sq, 2, &code_b, &[ip], &[], "Q"));
        q_ok = online_ids(&q).contains(&ip);
        if p_ok && q_ok {
            break;
        }
        thread::sleep(Duration::from_millis(300));
    }
    assert!(p_ok, "H1's P sees H50's Q across 49 servers");
    assert!(q_ok, "H50's Q sees H1's P across 49 servers");
}

#[test]
fn presence_survives_three_servers() {
    let d = start(|c| {
        c.federation_token = "tok".into();
        c.federation_transitive = 2;
    });
    let c_srv = start(|c| {
        c.federation_token = "tok".into();
        c.federation_transitive = 2;
        c.federate = vec![Peer {
            addr: d.addr.to_string(),
            token: "tok".into(),
            name: "D".into(),
            key: String::new(),
        }];
    });
    let b = start(|c| {
        c.federation_token = "tok".into();
        c.federation_transitive = 2;
        c.federate = vec![Peer {
            addr: c_srv.addr.to_string(),
            token: "tok".into(),
            name: "C".into(),
            key: String::new(),
        }];
    });
    let a = start(|c| {
        c.federation_token = "tok".into();
        c.federation_transitive = 2;
        c.federate = vec![Peer {
            addr: b.addr.to_string(),
            token: "tok".into(),
            name: "B".into(),
            key: String::new(),
        }];
    });
    thread::sleep(Duration::from_millis(900));

    let sp = [0x71u8; proto::FRIEND_SECRET_LEN];
    let sq = [0x72u8; proto::FRIEND_SECRET_LEN];
    let (ip, iq) = (proto::friend_id(&sp), proto::friend_id(&sq));
    let code_a = *b"PRES4567";
    let code_d = *b"PRES5678";

    let _ = poll_presence(&a, &presence_body(&sp, 1, &code_a, &[iq], &[], "P"));
    let _ = poll_presence(&d, &presence_body(&sq, 2, &code_d, &[ip], &[], "Q"));
    thread::sleep(Duration::from_millis(1000));

    let p = poll_presence(&a, &presence_body(&sp, 1, &code_a, &[iq], &[], "P"));
    assert!(online_ids(&p).contains(&iq), "P sees Q three hops away");
    let q = poll_presence(&d, &presence_body(&sq, 2, &code_d, &[ip], &[], "Q"));
    assert!(online_ids(&q).contains(&ip), "Q sees P three hops away");
}

#[test]
fn find_by_code_reaches_a_second_hop() {
    let code = *b"CHAN2345";
    let c_srv = start(|c| c.federation_token = "tok".into());
    let (mut host_conn, _) = host(&c_srv, 7, 43, Some(&code));
    let b = start(|c| {
        c.federation_token = "tok".into();
        c.federation_transitive = 1;
        c.federate = vec![Peer {
            addr: c_srv.addr.to_string(),
            token: "tok".into(),
            name: "C".into(),
            key: String::new(),
        }];
    });
    let a = start(|c| {
        c.federation_token = "tok".into();
        c.federation_transitive = 1;
        c.federate = vec![Peer {
            addr: b.addr.to_string(),
            token: "tok".into(),
            name: "B".into(),
            key: String::new(),
        }];
    });

    let mut f = dial(&a);
    send(&mut f, proto::T_FIND, &[&header(4, 0)[..], &code]);
    let p = expect(&mut f, proto::T_HUBS4);
    assert_eq!(u32::from_le_bytes(p[..4].try_into().unwrap()), 1);
    let alias = u64::from_le_bytes(p[4..12].try_into().unwrap());

    let mut player = dial(&a);
    send(&mut player, proto::T_JOIN, &[&header(4, 0)[..], &alias.to_le_bytes(), &code]);
    let inc = expect(&mut host_conn, proto::T_INCOMING);
    let ticket = u64::from_le_bytes(inc[..8].try_into().unwrap());
    let mut host_attach = dial(&c_srv);
    send(&mut host_attach, proto::T_ATTACH, &[&header(4, 0)[..], &ticket.to_le_bytes()]);
    let _ = expect(&mut host_attach, proto::T_SPLICED);
    let _ = expect(&mut player, proto::T_SPLICED);
    player.write_all(b"find-hop").unwrap();
    let mut got = [0u8; 8];
    host_attach.read_exact(&mut got).unwrap();
    assert_eq!(&got, b"find-hop");
}

#[test]
fn transitive_advertisements_are_off_at_zero_hops() {
    let c_srv = start(|c| c.federation_token = "tok".into());
    let (_host, _) = host(&c_srv, 0, 42, None);
    let b = start(|c| {
        c.federation_token = "tok".into();
        c.federation_transitive = 1;
        c.federate = vec![Peer {
            addr: c_srv.addr.to_string(),
            token: "tok".into(),
            name: "C".into(),
            key: String::new(),
        }];
    });
    let a = a_with_peer(&b, "tok"); // 0 hops by default
    thread::sleep(Duration::from_millis(300));
    assert!(list(&a, 0).is_empty(), "A only lists direct peers");
}

#[test]
fn find_and_join_by_code_across_servers() {
    let b = start(|c| c.federation_token = "tok".into());
    let code = *b"ABCD2345";
    let (mut host_conn, _) = host(&b, 7, 8, Some(&code));
    let a = a_with_peer(&b, "tok");

    let mut f = dial(&a);
    send(&mut f, proto::T_FIND, &[&header(4, 0)[..], &code]);
    let p = expect(&mut f, proto::T_HUBS4);
    assert_eq!(u32::from_le_bytes(p[..4].try_into().unwrap()), 1);
    let alias = u64::from_le_bytes(p[4..12].try_into().unwrap());

    // The code is the invitation: JOIN from another room reaches B's hub.
    let mut j = join_through(&a, &b, &mut host_conn, alias, 0, Some(&code));
    j.player.write_all(b"code join").unwrap();
    let mut got = [0u8; 9];
    j.host_side.read_exact(&mut got).unwrap();
    assert_eq!(&got, b"code join");
}

#[test]
fn a_wrong_token_lists_nothing() {
    let b = start(|c| c.federation_token = "right".into());
    let (_host, _) = host(&b, 0, 9, None);
    let a = start(|c| {
        c.federation_token = "ours".into();
        c.federate = vec![Peer { addr: b.addr.to_string(), token: "wrong".into(), name: "B".into(), key: String::new() }];
    });
    let hubs = list(&a, 0);
    assert!(hubs.is_empty());
}

#[test]
fn udp_is_bridged_both_ways() {
    let b = start(|c| c.federation_token = "tok".into());
    let (mut host_conn, _) = host(&b, 0, 10, None);
    let a = a_with_peer(&b, "tok");
    let alias = list(&a, 0)[0].0;
    let j = join_through(&a, &b, &mut host_conn, alias, 0, None);
    let player_udp = UdpSocket::bind("127.0.0.1:0").unwrap();
    let host_udp = UdpSocket::bind("127.0.0.1:0").unwrap();
    player_udp.set_read_timeout(Some(Duration::from_secs(3))).unwrap();
    host_udp.set_read_timeout(Some(Duration::from_secs(3))).unwrap();

    // Register both ends with their servers (empty relayed datagrams).
    let reg = |pair: u32, secret: u64| {
        let mut d = Vec::new();
        d.extend_from_slice(&proto::UDP_MAGIC);
        d.extend_from_slice(&pair.to_le_bytes());
        d.extend_from_slice(&secret.to_le_bytes());
        d
    };
    host_udp.send_to(&reg(j.peer_pair, j.host_secret), b.addr).unwrap();
    player_udp.send_to(&reg(j.local_pair, j.local_secret), a.addr).unwrap();
    thread::sleep(Duration::from_millis(100));

    // Player -> A -> B -> host.
    let mut dg = reg(j.local_pair, j.local_secret);
    dg.extend_from_slice(b"up");
    player_udp.send_to(&dg, a.addr).unwrap();
    let mut buf = [0u8; 64];
    let (n, _) = host_udp.recv_from(&mut buf).expect("host got the player's datagram");
    assert_eq!(&buf[..n], b"up");

    // Host -> B -> A -> player (raw, as from a normal server).
    let mut dg = reg(j.peer_pair, j.host_secret);
    dg.extend_from_slice(b"down");
    host_udp.send_to(&dg, b.addr).unwrap();
    let (n, _) = player_udp.recv_from(&mut buf).expect("player got the host's datagram");
    assert_eq!(&buf[..n], b"down");
}

#[test]
fn a_dead_peer_does_not_block_local_hubs() {
    let a = start(|c| {
        c.federation_token = "tok".into();
        c.federate = vec![Peer { addr: "127.0.0.1:1".into(), token: "tok".into(), name: "dead".into(), key: String::new() }];
    });
    let _host = host(&a, 0, 11, None);
    let hubs = list(&a, 0);
    assert_eq!(hubs.len(), 1);
    assert_eq!(hubs[0].0, _host.1);
}

fn temp_identity(tag: &str) -> (String, String) {
    let path = std::env::temp_dir().join(format!("mhgu-hub-identity-{}-{tag}", std::process::id()));
    let path = path.to_string_lossy().into_owned();
    let id = mhgu_hub::identity::Identity::load(&path).unwrap().id;
    (path, id)
}

#[test]
fn identity_is_stable_in_its_file() {
    let (path, id) = temp_identity("stable");
    assert!(std::fs::metadata(&path).unwrap().len() == 32);
    assert_eq!(mhgu_hub::identity::Identity::load(&path).unwrap().id, id);
    assert!(id.starts_with("ed25519:") && id.len() == 24);
    let _ = std::fs::remove_file(path);
}

#[test]
fn ed25519_peers_list_and_join() {
    let (path_a, id_a) = temp_identity("a");
    let (path_b, id_b) = temp_identity("b");
    let b = start(|c| {
        c.identity_file = path_b.clone();
        c.federation_allow = vec![id_a.clone()];
    });
    let (mut host_conn, _) = host(&b, 0, 12, None);
    let a = start(|c| {
        c.identity_file = path_a.clone();
        c.federation_allow = vec![id_b.clone()];
        c.federate = vec![Peer {
            addr: b.addr.to_string(),
            token: String::new(),
            name: "B".into(),
            key: id_b.clone(),
        }];
    });
    let hubs = list(&a, 0);
    assert_eq!(hubs.len(), 1);
    let mut j = join_through(&a, &b, &mut host_conn, hubs[0].0, 0, None);
    j.player.write_all(b"ed25519").unwrap();
    let mut got = [0u8; 7];
    j.host_side.read_exact(&mut got).unwrap();
    assert_eq!(&got, b"ed25519");
    let _ = std::fs::remove_file(path_a);
    let _ = std::fs::remove_file(path_b);
}

#[test]
fn an_unlisted_identity_is_refused() {
    let (path_a, _id_a) = temp_identity("ua");
    let (path_b, id_b) = temp_identity("ub");
    let b = start(|c| c.identity_file = path_b.clone()); // no -federation-allow
    let _host = host(&b, 0, 13, None);
    let a = start(|c| {
        c.identity_file = path_a.clone();
        c.federate = vec![Peer {
            addr: b.addr.to_string(),
            token: String::new(),
            name: "B".into(),
            key: id_b.clone(),
        }];
    });
    assert!(list(&a, 0).is_empty());
    let _ = std::fs::remove_file(path_a);
    let _ = std::fs::remove_file(path_b);
}

#[test]
fn well_known_serves_identity_and_endpoint() {
    let (path, id) = temp_identity("wk");
    let ts = start(|c| {
        c.identity_file = path.clone();
        c.advertise = "hub.example:39510".into();
    });
    let doc = ts._srv.well_known();
    assert!(doc.contains(&id));
    assert!(doc.contains("\"endpoint\":\"hub.example:39510\""));
    assert!(doc.contains("\"public_key\":\""));
    assert!(doc.contains("\"versions\":[2,1]"));
    let _ = std::fs::remove_file(path);
}

#[test]
fn a_well_known_url_is_resolved_to_its_endpoint_and_key() {
    use std::net::TcpListener;
    let (path_a, id_a) = temp_identity("wa");
    let (path_b, id_b) = temp_identity("wb");
    // Reserve B's hub port and a web port, then let go so the server can bind them.
    let hub_probe = TcpListener::bind("127.0.0.1:0").unwrap();
    let hub_port = hub_probe.local_addr().unwrap().port();
    drop(hub_probe);
    let web_probe = TcpListener::bind("127.0.0.1:0").unwrap();
    let web_port = web_probe.local_addr().unwrap().port();
    drop(web_probe);
    let hub_addr = format!("127.0.0.1:{hub_port}");
    let b = start(|c| {
        c.addr = hub_addr.clone();
        c.identity_file = path_b.clone();
        c.federation_allow = vec![id_a.clone()];
        c.advertise = hub_addr.clone();
        c.web = format!("127.0.0.1:{web_port}");
    });
    let (mut host_conn, _) = host(&b, 0, 14, None);
    thread::sleep(Duration::from_millis(300)); // the web listener starts with serve()
    let a = start(|c| {
        c.identity_file = path_a.clone();
        c.federation_allow = vec![id_b.clone()];
        c.federate = vec![Peer {
            addr: format!("http://127.0.0.1:{web_port}"),
            token: String::new(),
            name: "B".into(),
            key: String::new(),
        }];
    });
    let hubs = list(&a, 0);
    assert_eq!(hubs.len(), 1);
    let mut j = join_through(&a, &b, &mut host_conn, hubs[0].0, 0, None);
    j.player.write_all(b"well-known").unwrap();
    let mut got = [0u8; 10];
    j.host_side.read_exact(&mut got).unwrap();
    assert_eq!(&got, b"well-known");
    let _ = std::fs::remove_file(path_a);
    let _ = std::fs::remove_file(path_b);
}

#[test]
fn a_pinned_identity_must_match() {
    let (path_a, id_a) = temp_identity("pa");
    let (path_b, id_b) = temp_identity("pb");
    let (path_x, id_x) = temp_identity("px");
    let b = start(|c| {
        c.identity_file = path_b.clone();
        c.federation_allow = vec![id_a.clone()];
    });
    let _host = host(&b, 0, 15, None);
    let a = start(|c| {
        c.identity_file = path_a.clone();
        c.federation_allow = vec![id_b.clone()];
        c.federate = vec![Peer {
            addr: b.addr.to_string(),
            token: String::new(),
            name: "B".into(),
            key: id_x.clone(), // not B's id
        }];
    });
    assert!(list(&a, 0).is_empty());
    let _ = std::fs::remove_file(path_a);
    let _ = std::fs::remove_file(path_b);
    let _ = std::fs::remove_file(path_x);
}

fn presence_body(
    secret: &[u8; proto::FRIEND_SECRET_LEN],
    state: u8,
    code: &[u8; proto::CODE_LEN],
    friends: &[[u8; proto::FRIEND_LEN]],
    removed: &[[u8; proto::FRIEND_LEN]],
    name: &str,
) -> Vec<u8> {
    let mut b = Vec::new();
    b.extend_from_slice(secret);
    b.push(state);
    b.extend_from_slice(code);
    b.push(friends.len() as u8);
    for f in friends {
        b.extend_from_slice(f);
    }
    b.push(name.len() as u8);
    b.extend_from_slice(name.as_bytes());
    b.push(removed.len() as u8);
    for f in removed {
        b.extend_from_slice(f);
    }
    b
}

fn poll_presence(ts: &TestServer, body: &[u8]) -> Vec<u8> {
    let mut c = dial(ts);
    send(&mut c, proto::T_PRESENCE, &[&header(4, 0)[..], body]);
    expect(&mut c, proto::T_FRIENDS)
}

fn online_ids(p: &[u8]) -> Vec<[u8; proto::FRIEND_LEN]> {
    let mut out = Vec::new();
    let count = p[0] as usize;
    for i in 0..count {
        let at = 1 + i * (proto::FRIEND_LEN + 1 + proto::CODE_LEN);
        let mut id = [0u8; proto::FRIEND_LEN];
        id.copy_from_slice(&p[at..at + proto::FRIEND_LEN]);
        out.push(id);
    }
    out
}

fn requests(p: &[u8]) -> Vec<([u8; proto::FRIEND_LEN], String)> {
    let count = p[0] as usize;
    let mut at = 1 + count * (proto::FRIEND_LEN + 1 + proto::CODE_LEN);
    let n = p[at] as usize;
    at += 1;
    let mut out = Vec::new();
    for _ in 0..n {
        let mut id = [0u8; proto::FRIEND_LEN];
        id.copy_from_slice(&p[at..at + proto::FRIEND_LEN]);
        let nl = p[at + proto::FRIEND_LEN] as usize;
        at += proto::FRIEND_LEN + 1;
        out.push((id, String::from_utf8_lossy(&p[at..at + nl]).into_owned()));
        at += nl;
    }
    out
}

fn removed_ids(p: &[u8]) -> Vec<[u8; proto::FRIEND_LEN]> {
    let count = p[0] as usize;
    let mut at = 1 + count * (proto::FRIEND_LEN + 1 + proto::CODE_LEN);
    let n = p[at] as usize;
    at += 1;
    for _ in 0..n {
        let nl = p[at + proto::FRIEND_LEN] as usize;
        at += proto::FRIEND_LEN + 1 + nl;
    }
    let g = p[at] as usize;
    at += 1;
    let mut out = Vec::new();
    for _ in 0..g {
        let mut id = [0u8; proto::FRIEND_LEN];
        id.copy_from_slice(&p[at..at + proto::FRIEND_LEN]);
        out.push(id);
        at += proto::FRIEND_LEN;
    }
    out
}

#[test]
fn presence_friends_requests_and_removals_cross_peers() {
    let b = start(|c| c.federation_token = "tok".into());
    let a = a_with_peer(&b, "tok");
    thread::sleep(Duration::from_millis(400)); // presence streams connect and subscribe

    let sp = [0x51u8; proto::FRIEND_SECRET_LEN];
    let sq = [0x52u8; proto::FRIEND_SECRET_LEN];
    let sc = [0x53u8; proto::FRIEND_SECRET_LEN];
    let sd = [0x54u8; proto::FRIEND_SECRET_LEN];
    let (ip, iq, ic, id) = (
        proto::friend_id(&sp),
        proto::friend_id(&sq),
        proto::friend_id(&sc),
        proto::friend_id(&sd),
    );
    let code_a = *b"AAAA2345";
    let code_b = *b"BBBB2345";

    // P (A) and Q (B) list each other.
    let _ = poll_presence(&a, &presence_body(&sp, 1, &code_a, &[iq], &[], "P"));
    let _ = poll_presence(&b, &presence_body(&sq, 2, &code_b, &[ip], &[], "Q"));
    thread::sleep(Duration::from_millis(700));
    let p = poll_presence(&a, &presence_body(&sp, 1, &code_a, &[iq], &[], "P"));
    assert!(online_ids(&p).contains(&iq), "P sees Q online through B");
    let q = poll_presence(&b, &presence_body(&sq, 2, &code_b, &[ip], &[], "Q"));
    assert!(online_ids(&q).contains(&ip), "Q sees P online through A");

    // C (A) lists D (B) but D doesn't list C: D gets a request with C's name.
    let _ = poll_presence(&a, &presence_body(&sc, 0, &[0; 8], &[id], &[], "C"));
    thread::sleep(Duration::from_millis(700));
    let d = poll_presence(&b, &presence_body(&sd, 0, &[0; 8], &[], &[], "D"));
    let reqs = requests(&d);
    assert!(reqs.iter().any(|(rid, name)| *rid == ic && name == "C"), "D sees C's request");

    // Q removes P: P's game is told to drop Q too.
    let _ = poll_presence(&b, &presence_body(&sq, 2, &code_b, &[ip], &[ip], "Q"));
    thread::sleep(Duration::from_millis(700));
    let p = poll_presence(&a, &presence_body(&sp, 1, &code_a, &[iq], &[], "P"));
    assert!(removed_ids(&p).contains(&iq), "P is told that Q removed it");
}
