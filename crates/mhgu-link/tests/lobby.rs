use std::io::{Read, Write};
use std::net::{SocketAddr, TcpListener, TcpStream};
use std::sync::Arc;
use std::thread;
use std::time::{Duration, Instant};

use mhgu_hub::{Config, Server};
use mhgu_link::lobby::{self, Lobby};
use mhgu_link::proto::{self, NETINFO_SIZE};
use mhgu_link::sha256;

fn make_info() -> Vec<u8> {
    let mut i = vec![0u8; NETINFO_SIZE];
    i[0x4B] = 2;
    i[0x26] = 10;
    i[0x66] = 4;
    i[0x67] = 1;
    i
}

fn hex(b: &[u8]) -> String {
    b.iter().map(|x| format!("{x:02x}")).collect()
}

fn start_hub() -> (SocketAddr, Arc<Server>) {
    let mut cfg = Config::default();
    cfg.addr = "127.0.0.1:0".into();
    cfg.handshake = Duration::from_secs(2);
    let srv = Arc::new(Server::new(cfg).expect("bind"));
    let addr = srv.addr();
    let s2 = Arc::clone(&srv);
    thread::spawn(move || {
        let _ = s2.serve();
    });
    (addr, srv)
}

#[test]
fn sha256_vectors() {
    assert_eq!(
        hex(&sha256::digest(&[&b""[..]])),
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
    );
    assert_eq!(
        hex(&sha256::digest(&[&b"abc"[..]])),
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
    );
    assert_eq!(
        hex(&sha256::digest(&[&b"ab"[..], &b"c"[..]])),
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
    );
}

#[test]
fn room_from_key() {
    assert_eq!(lobby::room_from_key(""), [0u8; 16]);
    assert_eq!(lobby::room_from_key("   "), [0u8; 16]);
    assert_ne!(lobby::room_from_key("secret"), [0u8; 16]);
    // spaces around the key are ignored, as ldn.c trims them
    assert_eq!(lobby::room_from_key(" x "), lobby::room_from_key("x"));
}

#[test]
fn list_against_hub_server() {
    let (addr, _srv) = start_hub();

    // A hub, hosted by a raw client (kept alive).
    let mut raw = TcpStream::connect(addr).unwrap();
    let h = lobby::header(proto::LINK_PROTO_VERSION, [0u8; 16]);
    let info = make_info();
    raw.write_all(&proto::frame(proto::T_HOST, &[&h[..], &info[..]]))
        .unwrap();
    let (t, p) = proto::read_frame(&mut raw, 4096).unwrap();
    assert_eq!(t, proto::T_HOSTED);
    let id = u64::from_le_bytes(p[..8].try_into().unwrap());

    // The lobby client lists it.
    let lb = Lobby::new(addr);
    let hubs = lb.list(proto::LINK_PROTO_VERSION, [0u8; 16]).unwrap();
    assert_eq!(hubs.len(), 1);
    assert_eq!(hubs[0].id, id);
    assert_eq!(hubs[0].info.len(), NETINFO_SIZE);

    drop(raw); // closes the hub
}

#[test]
fn list_parses_many_hubs() {
    // ldn.c's MAX_FRAME is 0x10000 + 64: an answer with several hubs (5 + n * sizeof(NetworkInfo))
    // exceeds the old 4096-byte read cap (4 hubs are already 4645 bytes), so a wrong cap made every
    // search with >= 4 matching hubs fail.
    let l = TcpListener::bind("127.0.0.1:0").unwrap();
    let addr = l.local_addr().unwrap();
    let info = make_info();
    let h = thread::spawn(move || {
        let (mut s, _) = l.accept().unwrap();
        let (t, _) = proto::read_frame(&mut s, 4096).unwrap();
        assert_eq!(t, proto::T_LIST);
        let mut answer = Vec::new();
        answer.extend_from_slice(&8u32.to_le_bytes());
        for i in 0..8u64 {
            answer.extend_from_slice(&(0x1000 + i).to_le_bytes());
            answer.extend_from_slice(&info);
        }
        s.write_all(&proto::frame(proto::T_HUBS, &[&answer])).unwrap();
        thread::sleep(Duration::from_millis(50));
    });
    let hubs = Lobby::new(addr).list(mhgu_protocol::LINK_PROTO_VERSION, [0u8; 16]).unwrap();
    assert_eq!(hubs.len(), 8);
    assert_eq!(hubs[7].id, 0x1007);
    h.join().unwrap();
}

#[test]
fn join_parses_spliced() {
    let l = TcpListener::bind("127.0.0.1:0").unwrap();
    let addr = l.local_addr().unwrap();
    let h = thread::spawn(move || {
        let (mut s, _) = l.accept().unwrap();
        let (t, p) = proto::read_frame(&mut s, 4096).unwrap();
        assert_eq!(t, proto::T_JOIN);
        assert_eq!(p.len(), 24 + 8);
        let mut payload = Vec::new();
        payload.extend_from_slice(&0x1122_3344u32.to_le_bytes());
        payload.extend_from_slice(&0x5566_7788_99aa_bbccu64.to_le_bytes());
        s.write_all(&proto::frame(proto::T_SPLICED, &[&payload]))
            .unwrap();
        thread::sleep(Duration::from_millis(100));
    });

    let spliced = Lobby::new(addr)
        .join(mhgu_protocol::LINK_PROTO_VERSION, [0u8; 16], 0xdead_beef, None)
        .unwrap();
    assert_eq!(spliced.pair, 0x1122_3344);
    assert_eq!(spliced.secret, 0x5566_7788_99aa_bbcc);
    h.join().unwrap();
}

#[test]
fn host_and_join_against_hub_server() {
    let (addr, _srv) = start_hub();
    let info = make_info();
    let mut host = Lobby::new(addr).host(&info, proto::LINK_PROTO_VERSION, [0u8; 16], None).unwrap();
    let hub_id = host.hub_id();

    // The player lists and joins on its own thread: JOIN blocks until the host attaches.
    let pt = thread::spawn(move || {
        let lb = Lobby::new(addr);
        let hubs = lb.list(proto::LINK_PROTO_VERSION, [0u8; 16]).unwrap();
        assert_eq!(hubs.len(), 1);
        assert_eq!(hubs[0].id, hub_id);
        lb.join(mhgu_protocol::LINK_PROTO_VERSION, [0u8; 16], hub_id, None).unwrap()
    });

    // The host waits for the server's INCOMING, then opens the ATTACH connection.
    let deadline = Instant::now() + Duration::from_secs(3);
    let ticket = loop {
        assert!(Instant::now() < deadline, "no incoming ticket");
        if let Some(t) = host.poll(Duration::from_millis(200)).unwrap() {
            break t;
        }
    };
    let mut host_side = host.attach(ticket).unwrap();
    let mut player_side = pt.join().unwrap();

    assert_eq!(host_side.pair, player_side.pair);
    assert_ne!(host_side.secret, player_side.secret);

    // Bytes flow both ways through the server's splice.
    host_side.stream.write_all(b"from host").unwrap();
    player_side
        .stream
        .set_read_timeout(Some(Duration::from_secs(2)))
        .unwrap();
    let mut b = [0u8; 9];
    player_side.stream.read_exact(&mut b).unwrap();
    assert_eq!(&b, b"from host");

    player_side.stream.write_all(b"from player").unwrap();
    host_side
        .stream
        .set_read_timeout(Some(Duration::from_secs(2)))
        .unwrap();
    let mut b2 = [0u8; 11];
    host_side.stream.read_exact(&mut b2).unwrap();
    assert_eq!(&b2, b"from player");
}

#[test]
fn code_find_join_and_presence() {
    let (addr, _srv) = start_hub();
    let info = make_info();
    let code = *b"KJMN2345";
    let host = Lobby::new(addr).host(&info, proto::LINK_PROTO_VERSION, [0u8; 16], Some(&code)).unwrap();
    let hub_id = host.hub_id();

    // HUBS4: the code and the protocol come back with the hub.
    let hubs = Lobby::new(addr).list(proto::LINK_PROTO_VERSION, [0u8; 16]).unwrap();
    assert_eq!(hubs.len(), 1);
    assert_eq!(hubs[0].code, code);
    assert_eq!(hubs[0].link_proto, proto::LINK_PROTO_VERSION);
    assert_eq!(hubs[0].code_shown().as_deref(), Some("KJMN-2345"));

    // FIND: the code alone names the hub.
    let found = Lobby::new(addr).find(proto::LINK_PROTO_VERSION, &code).unwrap();
    assert_eq!(found.len(), 1);
    assert_eq!(found[0].id, hub_id);

    // A second hub (another session id and BSSID) asking for the same code is refused with ERR_CODE_TAKEN.
    let mut info2 = info.clone();
    info2[proto::NI_SESSION_ID] = 1;
    info2[proto::NI_BSSID..proto::NI_BSSID + 6].fill(1);
    let err = Lobby::new(addr).host(&info2, proto::LINK_PROTO_VERSION, [0u8; 16], Some(&code)).err().unwrap();
    assert_eq!(mhgu_link::error_code(&err), Some(proto::ERR_CODE_TAKEN));

    // PRESENCE: two players listing each other see each other online.
    let secret_a = [0x41u8; proto::FRIEND_SECRET_LEN];
    let secret_b = [0x42u8; proto::FRIEND_SECRET_LEN];
    let id_a = proto::friend_id(&secret_a);
    let id_b = proto::friend_id(&secret_b);
    let lb = Lobby::new(addr);
    let a = lb.presence(&secret_a, 1, &code, &[id_b], &[], "Alice").unwrap();
    assert!(a.online.is_empty());
    let b = lb.presence(&secret_b, 0, &[0; 8], &[id_a], &[], "Bob").unwrap();
    assert_eq!(b.online.len(), 1);
    assert_eq!(b.online[0].id, id_a);
    assert_eq!(b.online[0].state, 1);
    assert_eq!(b.online[0].code, code);
    let a = lb.presence(&secret_a, 1, &code, &[id_b], &[], "Alice").unwrap();
    assert_eq!(a.online.len(), 1);
    assert_eq!(a.online[0].id, id_b);
    assert_eq!(a.online[0].state, proto::FRIEND_STATE_ONLINE);
}
