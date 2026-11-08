//! The room frames an XX/GU gateway speaks (Yuzu room v1: JoinRequest / RoomInformation /
//! SetGameInfo / ProxyPacket / LdnPacket) against our own room server. The wire layout is the
//! one `crates/mhgu-crossplay/src/roomsrv.rs` implements and the XX/GU gateway's frozen
//! `room-client` encodes; a change to either side that breaks a Citra gateway shows up here.
//!
//! One test, because the room server is process-wide (bind, members, stop).

use mhgu_crossplay::roomsrv;
use rusty_enet::{Event, Host, HostSettings, Packet, PeerID};
use std::net::UdpSocket;
use std::time::{Duration, Instant};

const M_JOIN_REQUEST: u8 = 1;
const M_JOIN_SUCCESS: u8 = 2;
const M_ROOM_INFO: u8 = 3;
const M_SET_GAME_INFO: u8 = 4;
const M_PROXY_PACKET: u8 = 5;
const M_LDN_PACKET: u8 = 6;

fn be16(v: u16) -> [u8; 2] { v.to_be_bytes() }
fn be32(v: u32) -> [u8; 4] { v.to_be_bytes() }
fn be64(v: u64) -> [u8; 8] { v.to_be_bytes() }

fn string(out: &mut Vec<u8>, s: &str) {
    out.extend_from_slice(&be32(s.len() as u32));
    out.extend_from_slice(s.as_bytes());
}

/// A room member: ENet host, the peer id, and every frame it received.
struct Client {
    host: Host<UdpSocket>,
    peer: PeerID,
    ip: [u8; 4],
}

impl Client {
    fn connect(addr: &str) -> Client {
        let sock = UdpSocket::bind("127.0.0.1:0").expect("client socket");
        let host = Host::new(sock, HostSettings { peer_limit: 1, channel_limit: 1, ..Default::default() })
            .expect("enet host");
        let mut host = host;
        let peer = host.connect(addr.parse().unwrap(), 1, 0).expect("enet connect");
        let id = peer.id();
        let end = Instant::now() + Duration::from_secs(5);
        loop {
            match host.service() {
                Ok(Some(Event::Connect { .. })) => break,
                Ok(_) => {}
                Err(_) => panic!("no ENet connect"),
            }
            assert!(Instant::now() < end, "no ENet connect");
            std::thread::sleep(Duration::from_millis(2));
        }
        Client { host, peer: id, ip: [0; 4] }
    }

    fn send(&mut self, frame: &[u8]) {
        self.host.peer_mut(self.peer).send(0, &Packet::reliable(frame.to_vec())).expect("send");
        self.host.flush();
    }

    /// Runs the client until `done` says a received frame is the one, or the timeout passes.
    fn pump_until(&mut self, timeout: Duration, mut done: impl FnMut(&[u8]) -> bool) -> Option<Vec<u8>> {
        let end = Instant::now() + timeout;
        while Instant::now() < end {
            loop {
                match self.host.service() {
                    Ok(Some(Event::Receive { packet, .. })) => {
                        let d = packet.data().to_vec();
                        if done(&d) {
                            return Some(d);
                        }
                    }
                    Ok(Some(_)) => {}
                    Ok(None) => break,
                    Err(_) => break,
                }
            }
            self.host.flush();
            std::thread::sleep(Duration::from_millis(2));
        }
        None
    }

    fn join_request(nick: &str) -> Vec<u8> {
        let mut m = vec![M_JOIN_REQUEST];
        string(&mut m, nick);
        m.extend_from_slice(&[0xFF; 4]);  // no preferred address
        m.extend_from_slice(&be32(1));    // room network version
        string(&mut m, "");               // password
        string(&mut m, "");               // token
        m
    }

    fn join(&mut self, nick: &str) {
        self.send(&Client::join_request(nick));
        // The room client (yuzu v1) wants RoomInformation first, then JoinSuccess.
        self.pump_until(Duration::from_secs(3), |d| d.first() == Some(&M_ROOM_INFO))
            .expect("RoomInformation first");
        let ok = self.pump_until(Duration::from_secs(3), |d| d.first() == Some(&M_JOIN_SUCCESS))
            .expect("JoinSuccess");
        assert!(ok.len() >= 5, "JoinSuccess carries the assigned IPv4");
        self.ip.copy_from_slice(&ok[1..5]);
    }
}

#[test]
fn gateway_frames_interoperate_with_our_room() {
    // A free UDP port for the room, then the process-wide room server.
    let probe = UdpSocket::bind("127.0.0.1:0").unwrap();
    let port = probe.local_addr().unwrap().port();
    drop(probe);
    roomsrv::start(port, "MHXX + MHGU", "", "").expect("room server");
    let addr = format!("127.0.0.1:{port}");

    // A Citra-side gateway: it joins the Yuzu room as one ordinary v1 member.
    let mut gateway = Client::connect(&addr);
    gateway.join("XXGU-Gateway");
    assert_eq!(gateway.ip[0], 10, "the room assigns a fake address");

    // An MHGU player joins the same room.
    let mut player = Client::connect(&addr);
    player.join("MHGU-Player");
    assert_ne!(gateway.ip, player.ip, "distinct fake addresses");

    // ... and the gateway announces the game it fronts.
    let mut info = vec![M_SET_GAME_INFO];
    string(&mut info, "Monster Hunter Generations Ultimate");
    info.extend_from_slice(&be64(0x0100770008DD8000));
    string(&mut info, "1.4.0");
    gateway.send(&info);

    // The player's RoomInformation names the gateway's member and its game.
    let info = player
        .pump_until(Duration::from_secs(3), |d| {
            d.first() == Some(&M_ROOM_INFO)
                && d.windows(b"XXGU-Gateway".len()).any(|w| w == b"XXGU-Gateway")
                && d.windows(b"Monster Hunter Generations Ultimate".len())
                    .any(|w| w == b"Monster Hunter Generations Ultimate")
        })
        .expect("the member list with the gateway's game");
    assert!(info.len() > 32, "a real RoomInformation");

    // LDN broadcast from the gateway (a Citra hall's ScanResp) reaches the player byte for byte.
    let mut ldn = vec![M_LDN_PACKET, 1];  // ScanResp
    ldn.extend_from_slice(&gateway.ip);
    ldn.extend_from_slice(&[0xFF; 4]);
    ldn.push(1);  // broadcast
    let hall = b"MHXX hall, 200-byte advertise body".to_vec();
    ldn.extend_from_slice(&be32(hall.len() as u32));
    ldn.extend_from_slice(&hall);
    gateway.send(&ldn);
    let got = player
        .pump_until(Duration::from_secs(3), |d| d.first() == Some(&M_LDN_PACKET))
        .expect("the LDN packet");
    assert_eq!(got, ldn, "the room relays LDN frames unchanged");

    // A targeted ProxyPacket (the room's zstd blob is opaque to the server) reaches the addressed member.
    let mut proxy = vec![M_PROXY_PACKET, 1];
    proxy.extend_from_slice(&gateway.ip);
    proxy.extend_from_slice(&be16(0x1234));
    proxy.push(1);
    proxy.extend_from_slice(&player.ip);
    proxy.extend_from_slice(&be16(0x5678));
    proxy.push(4);  // UDP
    proxy.push(0);  // not broadcast
    let blob = vec![0x28, 0xB5, 0x2F, 0xFD, 0x00, 0x01, 0x02];  // (a zstd frame: not parsed here)
    proxy.extend_from_slice(&be32(blob.len() as u32));
    proxy.extend_from_slice(&blob);
    gateway.send(&proxy);
    let got = player
        .pump_until(Duration::from_secs(3), |d| d.first() == Some(&M_PROXY_PACKET))
        .expect("the ProxyPacket");
    assert_eq!(got, proxy, "addressed game packets are relayed unchanged");

    roomsrv::stop();
}
