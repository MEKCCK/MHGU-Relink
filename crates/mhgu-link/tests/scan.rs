use std::net::{SocketAddr, UdpSocket};
use std::thread;
use std::time::Duration;

use mhgu_link::proto::NETINFO_SIZE;
use mhgu_link::scan;

fn make_info() -> Vec<u8> {
    let mut i = vec![0u8; NETINFO_SIZE];
    i[0x4B] = 2;
    i[0x26] = 10;
    i[0x66] = 4;
    i[0x67] = 1;
    i
}

#[test]
fn codec() {
    let p = scan::probe();
    let r = scan::answer(&p, 39510, &make_info()).expect("answer");
    let from: SocketAddr = "127.0.0.1:1".parse().unwrap();
    let parsed = scan::parse_reply(&r, from).expect("parse");
    assert_eq!(parsed.port, 39510);
    assert_eq!(parsed.info.len(), NETINFO_SIZE);
    assert_eq!(parsed.from, from);

    assert!(scan::answer(b"nope", 1, &make_info()).is_none());
    assert!(scan::parse_reply(b"nope", from).is_none());
}

#[test]
fn end_to_end() {
    let responder = UdpSocket::bind("127.0.0.1:0").unwrap();
    let raddr = responder.local_addr().unwrap();
    let info = make_info();
    let port = raddr.port();
    let h = thread::spawn(move || {
        let mut buf = [0u8; 64];
        let (n, from) = responder.recv_from(&mut buf).unwrap();
        let reply = scan::answer(&buf[..n], port, &info).expect("answer");
        responder.send_to(&reply, from).unwrap();
    });

    let scanner = UdpSocket::bind("127.0.0.1:0").unwrap();
    let replies = scan::scan(&scanner, &[raddr], Duration::from_millis(500)).unwrap();
    assert_eq!(replies.len(), 1);
    assert_eq!(replies[0].port, raddr.port());
    assert_eq!(replies[0].info.len(), NETINFO_SIZE);
    h.join().unwrap();
}
