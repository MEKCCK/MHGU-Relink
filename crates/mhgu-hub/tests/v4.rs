//! Link protocol 4 on the hub server: HUBS4, session codes, FIND, join by code, PRESENCE/friends.
//!
//! The expectations are the Go server's (`server/server_v4_test.go`) where they overlap, so a game of
//! MHGURecomp 0.19/0.20 and this build see and join each other's hubs.

use std::io::{Read, Write};
use std::net::{SocketAddr, TcpStream};
use std::sync::Arc;
use std::thread;
use std::time::Duration;

use mhgu_hub::proto::{self, frame, read_frame, HEADER_SIZE, NETINFO_SIZE};
use mhgu_hub::{Config, Server};

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

/// A hub listed with a code; returns its connection and id.
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

fn entry_size(v4: bool) -> usize {
    8 + if v4 { 2 + proto::CODE_LEN } else { 0 } + NETINFO_SIZE
}

#[test]
fn protocol4_gets_hubs4_and_protocol3_gets_hubs() {
    let ts = start(|_| {});
    let code = *b"ABCDEFGH";
    // A protocol 3 hub (no code: it cannot have one) and a protocol 4 one, in the same room.
    let mut old = dial(&ts);
    let h = header(3, 0);
    send(&mut old, proto::T_HOST, &[&h[..], &info(1, 1)[..]]);
    expect(&mut old, proto::T_HOSTED);
    let _hub4 = host(&ts, 0, 2, Some(&code));

    // The pool (3-4 by default) lists both to either game.
    let mut l4 = dial(&ts);
    send(&mut l4, proto::T_LIST, &[&header(4, 0)[..]]);
    let p = expect(&mut l4, proto::T_HUBS4);
    assert_eq!(u32::from_le_bytes(p[..4].try_into().unwrap()), 2);
    // (newest first: the protocol 4 hub, with its code; then the protocol 3 one)
    assert_eq!(u16::from_le_bytes(p[4 + 8..4 + 10].try_into().unwrap()), 4);
    assert_eq!(&p[4 + 10..4 + 10 + proto::CODE_LEN], &code);
    let e2 = 4 + entry_size(true);
    assert_eq!(u16::from_le_bytes(p[e2 + 8..e2 + 10].try_into().unwrap()), 3);
    assert_eq!(&p[e2 + 10..e2 + 10 + proto::CODE_LEN], &[0u8; proto::CODE_LEN]);

    let mut l3 = dial(&ts);
    send(&mut l3, proto::T_LIST, &[&header(3, 0)[..]]);
    let p = expect(&mut l3, proto::T_HUBS);
    assert_eq!(u32::from_le_bytes(p[..4].try_into().unwrap()), 2);
    assert_eq!(p.len(), 4 + 2 * entry_size(false));

    // A server that keeps 4 alone: the protocol 3 hub is not served.
    let ts = start(|c| c.min_proto = 4);
    let mut c3 = dial(&ts);
    let h = header(3, 0);
    send(&mut c3, proto::T_HOST, &[&h[..], &info(1, 1)[..]]);
    assert_eq!(u32::from_le_bytes(expect(&mut c3, proto::T_ERROR)[..4].try_into().unwrap()), proto::ERR_VERSION);
}

#[test]
fn find_and_join_by_code_across_rooms() {
    let ts = start(|_| {});
    let code = *b"JKMN2345";
    let (mut host_conn, id) = host(&ts, 7, 9, Some(&code));

    // FIND: the code is an invitation, the room (0) does not matter.
    let mut f = dial(&ts);
    send(&mut f, proto::T_FIND, &[&header(4, 0)[..], &code]);
    let p = expect(&mut f, proto::T_HUBS4);
    assert_eq!(u32::from_le_bytes(p[..4].try_into().unwrap()), 1);
    assert_eq!(u64::from_le_bytes(p[4..12].try_into().unwrap()), id);
    assert_eq!(&p[14..14 + proto::CODE_LEN], &code);

    // A wrong code finds nothing.
    let mut f = dial(&ts);
    send(&mut f, proto::T_FIND, &[&header(4, 0)[..], b"ZZZZZZZZ"]);
    let p = expect(&mut f, proto::T_HUBS4);
    assert_eq!(u32::from_le_bytes(p[..4].try_into().unwrap()), 0);

    // JOIN from another room, with the code.
    let mut j = dial(&ts);
    send(&mut j, proto::T_JOIN, &[&header(4, 0)[..], &id.to_le_bytes(), &code]);
    let (t, p) = read_frame(&mut host_conn, 1 << 20).unwrap();
    assert_eq!(t, proto::T_INCOMING);
    let ticket = u64::from_le_bytes(p[..8].try_into().unwrap());
    let mut att = dial(&ts);
    send(&mut att, proto::T_ATTACH, &[&header(4, 7)[..], &ticket.to_le_bytes()]);
    let host_sp = expect(&mut att, proto::T_SPLICED);
    let player_sp = expect(&mut j, proto::T_SPLICED);
    assert_eq!(host_sp[..4], player_sp[..4]);

    // Without the code, the room must match.
    let mut j2 = dial(&ts);
    send(&mut j2, proto::T_JOIN, &[&header(4, 0)[..], &id.to_le_bytes()]);
    assert_eq!(u32::from_le_bytes(expect(&mut j2, proto::T_ERROR)[..4].try_into().unwrap()), proto::ERR_NOT_FOUND);
}

#[test]
fn a_taken_code_is_refused_and_the_same_hub_gets_it_back() {
    let ts = start(|_| {});
    let code = *b"PQRS6789";
    let (mut a, id) = host(&ts, 0, 3, Some(&code));

    // Another network asking for the same code is refused at once.
    let mut b = dial(&ts);
    send(&mut b, proto::T_HOST, &[&header(4, 0)[..], &info(1, 4)[..], &code]);
    assert_eq!(u32::from_le_bytes(expect(&mut b, proto::T_ERROR)[..4].try_into().unwrap()), proto::ERR_CODE_TAKEN);

    // The same hub (session id and BSSID) listing again keeps its code; its old listing goes.
    let (again, id2) = host(&ts, 0, 3, Some(&code));
    assert_ne!(id, id2);
    let mut f = dial(&ts);
    send(&mut f, proto::T_FIND, &[&header(4, 0)[..], &code]);
    let p = expect(&mut f, proto::T_HUBS4);
    assert_eq!(u32::from_le_bytes(p[..4].try_into().unwrap()), 1);
    assert_eq!(u64::from_le_bytes(p[4..12].try_into().unwrap()), id2);
    // (the old host connection was shut down)
    assert!(a.set_read_timeout(Some(Duration::from_secs(2))).is_ok());
    let mut buf = [0u8; 1];
    assert!(matches!(a.read(&mut buf), Ok(0) | Err(_)));
    drop(again);
}

#[test]
fn presence_friends_requests_and_removals() {
    let ts = start(|_| {});
    let secret_a = [0x11u8; proto::FRIEND_SECRET_LEN];
    let secret_b = [0x22u8; proto::FRIEND_SECRET_LEN];
    let secret_c = [0x33u8; proto::FRIEND_SECRET_LEN];
    let id_a = proto::friend_id(&secret_a);
    let id_b = proto::friend_id(&secret_b);
    let id_c = proto::friend_id(&secret_c);

    // A lists B. A's own code: its hub's.
    let code_a = *b"ABCD2345";
    let p = presence(&ts, &body(&secret_a, 1, &code_a, &[id_b], &[], "Alice"));
    assert_eq!(p[0], 0, "B is not online yet");

    // B lists A and is in a hub: A sees B online, hosting, with its code.
    let code_b = *b"WXYZ2345";
    let p = presence(&ts, &body(&secret_b, 1, &code_b, &[id_a], &[], "Bob"));
    assert_eq!(p[0], 1);
    assert_eq!(&p[1..1 + proto::FRIEND_LEN], &id_a);
    assert_eq!(p[1 + proto::FRIEND_LEN], 1);
    assert_eq!(&p[2 + proto::FRIEND_LEN..2 + proto::FRIEND_LEN + proto::CODE_LEN], &code_a);

    // A polls again and sees B.
    let p = presence(&ts, &body(&secret_a, 1, &code_a, &[id_b], &[], "Alice"));
    assert_eq!(p[0], 1);
    assert_eq!(&p[1..1 + proto::FRIEND_LEN], &id_b);

    // C lists A but A doesn't list C: A gets a friend request with C's name.
    let _ = presence(&ts, &body(&secret_c, 0, &[0; 8], &[id_a], &[], "Carol"));
    let p = presence(&ts, &body(&secret_a, 1, &code_a, &[id_b], &[], "Alice"));
    let at = 1 + p[0] as usize * (proto::FRIEND_LEN + 1 + proto::CODE_LEN);
    assert_eq!(p[at], 1, "one request");
    assert_eq!(&p[at + 1..at + 1 + proto::FRIEND_LEN], &id_c);
    assert_eq!(&p[at + 1 + proto::FRIEND_LEN + 1..], b"Carol");

    // B removes A: A (which lists B) is told to drop B too.
    let _ = presence(&ts, &body(&secret_b, 1, &code_b, &[id_a], &[id_a], "Bob"));
    let p = presence(&ts, &body(&secret_a, 1, &code_a, &[id_b], &[], "Alice"));
    assert_eq!(p[0], 0, "B no longer lists A back");
    // (the tail after the requests: one removed id)
    let at = 1 + p[0] as usize * (proto::FRIEND_LEN + 1 + proto::CODE_LEN);
    let reqs = p[at] as usize;
    let mut q = at + 1;
    for _ in 0..reqs {
        let nl = p[q + proto::FRIEND_LEN] as usize;
        q += proto::FRIEND_LEN + 1 + nl;
    }
    assert_eq!(p[q], 1);
    assert_eq!(&p[q + 1..q + 1 + proto::FRIEND_LEN], &id_b);
}

/// One PRESENCE request (the server takes one request per connection).
fn presence(ts: &TestServer, body: &[u8]) -> Vec<u8> {
    let mut c = dial(ts);
    send(&mut c, proto::T_PRESENCE, &[&header(4, 0)[..], body]);
    expect(&mut c, proto::T_FRIENDS)
}

fn body(
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

#[test]
fn max_proto_refuses_newer_games() {
    let ts = start(|c| c.max_proto = 3);
    let mut c = dial(&ts);
    send(&mut c, proto::T_LIST, &[&header(4, 0)[..]]);
    assert_eq!(u32::from_le_bytes(expect(&mut c, proto::T_ERROR)[..4].try_into().unwrap()), proto::ERR_VERSION);

    let mut c = dial(&ts);
    send(&mut c, proto::T_LIST, &[&header(3, 0)[..]]);
    let p = expect(&mut c, proto::T_HUBS);
    assert_eq!(u32::from_le_bytes(p[..4].try_into().unwrap()), 0);
}

#[test]
fn presence_needs_the_full_body() {
    let ts = start(|_| {});
    let mut c = dial(&ts);
    send(&mut c, proto::T_PRESENCE, &[&header(4, 0)[..], &[0u8; 4]]);
    assert_eq!(u32::from_le_bytes(expect(&mut c, proto::T_ERROR)[..4].try_into().unwrap()), proto::ERR_BAD_REQ);
}
