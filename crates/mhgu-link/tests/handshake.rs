//! Wire-level tests for the client link, against a small in-test host that speaks ldn.c's handshake.

use std::io::Write;
use std::net::TcpListener;
use std::thread;

use mhgu_link::proto::{
    self, LINK_MAX_FRAME, NETINFO_SIZE, NI_NET_TYPE, NI_NODE_COUNT, NI_NODE_MAX, NI_SSID, VPROTO_UDP,
};
use mhgu_link::{udp, Client, Event, Hello, Reply, VPacket};

#[derive(Clone, Copy)]
enum Mode {
    Welcome,
    Info,
    Refuse,
}

fn make_info() -> Vec<u8> {
    let mut i = vec![0u8; NETINFO_SIZE];
    i[NI_NET_TYPE] = 2;
    i[NI_SSID] = 10;
    i[NI_NODE_MAX] = 4;
    i[NI_NODE_COUNT] = 1;
    i
}

fn mock_host(listener: TcpListener, mode: Mode) {
    let (mut s, _) = listener.accept().unwrap();
    let (t, p) = proto::read_frame(&mut s, LINK_MAX_FRAME as u32).unwrap();
    assert_eq!(t, proto::F_HELLO);
    assert!(Hello::parse(&p).is_some(), "bad Hello");

    match mode {
        Mode::Info => {
            s.write_all(&proto::frame(proto::F_INFO, &[&make_info()]))
                .unwrap();
        }
        Mode::Refuse => {
            s.write_all(&proto::frame(proto::F_REFUSE, &[&7u32.to_le_bytes()]))
                .unwrap();
        }
        Mode::Welcome => {
            let mut w = vec![0u8; 8 + NETINFO_SIZE];
            w[0] = 1; // node
            w[4..8].copy_from_slice(&0x1122_3344u32.to_le_bytes()); // UDP token
            w[8..].copy_from_slice(&make_info());
            s.write_all(&proto::frame(proto::F_WELCOME, &[&w])).unwrap();

            // Echo one F_PACKET, proving the packet framing round-trips.
            let (t2, p2) = proto::read_frame(&mut s, LINK_MAX_FRAME as u32).unwrap();
            assert_eq!(t2, proto::F_PACKET);
            s.write_all(&proto::frame(proto::F_PACKET, &[&p2])).unwrap();
        }
    }
}

fn with_host(mode: Mode, f: impl FnOnce(std::net::SocketAddr)) {
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let addr = listener.local_addr().unwrap();
    let h = thread::spawn(move || mock_host(listener, mode));
    f(addr);
    h.join().unwrap();
}

#[test]
fn welcome_and_packet_echo() {
    with_host(Mode::Welcome, |addr| {
        let mut c = Client::connect(addr, &Hello::default()).unwrap();
        match c.reply().unwrap() {
            Reply::Welcome { node, token, info } => {
                assert_eq!(node, 1);
                assert_eq!(token, 0x1122_3344);
                assert_eq!(info.len(), NETINFO_SIZE);
                assert_eq!(info[NI_NODE_COUNT], 1);
            }
            other => panic!("{other:?}"),
        }

        let p = VPacket {
            src_ip: 0x0102_0304,
            dst_ip: 0x0506_0708,
            src_port: 1111,
            dst_port: 2222,
            proto: VPROTO_UDP,
            kind: 0,
            pad: 0,
        };
        c.send_packet(&p, b"payload").unwrap();
        match c.recv().unwrap() {
            Event::Packet { vpacket, data } => {
                assert_eq!(vpacket, p);
                assert_eq!(data, b"payload");
            }
            other => panic!("{other:?}"),
        }
    });
}

#[test]
fn scan_gets_info() {
    with_host(Mode::Info, |addr| {
        let mut hello = Hello::default();
        hello.scan_only = true;
        let mut c = Client::connect(addr, &hello).unwrap();
        match c.reply().unwrap() {
            Reply::Info(info) => {
                assert_eq!(info.len(), NETINFO_SIZE);
                assert_eq!(info[NI_NODE_MAX], 4);
            }
            other => panic!("{other:?}"),
        }
    });
}

#[test]
fn refuse_code() {
    with_host(Mode::Refuse, |addr| {
        let mut c = Client::connect(addr, &Hello::default()).unwrap();
        match c.reply().unwrap() {
            Reply::Refuse(code) => assert_eq!(code, 7),
            other => panic!("{other:?}"),
        }
    });
}

#[test]
fn udp_roundtrip() {
    let d = udp::datagram(0x1122_3344, proto::U_HELLO, b"x");
    let (token, kind, payload) = udp::parse(&d).unwrap();
    assert_eq!(token, 0x1122_3344);
    assert_eq!(kind, proto::U_HELLO);
    assert_eq!(payload, b"x");
    assert!(udp::parse(b"nope").is_none());
}

#[test]
fn hello_roundtrip() {
    let mut h = Hello::default();
    h.lcv = 0x1234;
    h.mac = [1, 2, 3, 4, 5, 6];
    h.name[..4].copy_from_slice(b"Test");
    let back = Hello::parse(&h.to_bytes()).unwrap();
    assert_eq!(back.lcv, 0x1234);
    assert_eq!(back.mac, [1, 2, 3, 4, 5, 6]);
    assert_eq!(back.name_string(), "Test");
}
