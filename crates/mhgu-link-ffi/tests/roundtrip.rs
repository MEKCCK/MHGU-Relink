//! The C link library, driven the way the runtime drives it: one process hosts, another joins, and a
//! `VPacket` goes both ways through the delivery callbacks.
//!
//! The library keeps one link per process (that is what the game has), so the two ends are two
//! processes: this test binary runs itself again with `MHGU_LINK_TEST_ROLE=host`.

use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::time::{Duration, Instant};

use mhgu_link_ffi::*;

static HOST_ROLE: AtomicBool = AtomicBool::new(false);
static GOT: AtomicU32 = AtomicU32::new(0);
static LAST: std::sync::Mutex<Vec<u8>> = std::sync::Mutex::new(Vec::new());

extern "C" fn on_packet(p: *const MhguVPacket, data: *const u8, len: u32) {
    let body = if data.is_null() { Vec::new() } else { unsafe { std::slice::from_raw_parts(data, len as usize) }.to_vec() };
    *LAST.lock().unwrap() = body.clone();
    GOT.fetch_add(1, Ordering::SeqCst);
    if HOST_ROLE.load(Ordering::SeqCst) {
        // Echo it: the joiner's packet goes back to it.
        mhgu_ldn_send(p, data, len);
    }
}

extern "C" fn on_aux(_data: *const u8, _len: u32) {}

#[test]
fn a_host_and_a_joiner_exchange_a_packet() {
    if std::env::var("MHGU_LINK_TEST_ROLE").as_deref() == Ok("host") {
        run_host();
        return;
    }
    let port = std::env::var("MHGU_LINK_TEST_PORT").ok().and_then(|v| v.parse::<u16>().ok()).unwrap_or(39555);
    let mut child = std::process::Command::new(std::env::current_exe().unwrap())
        .env("MHGU_LINK_TEST_ROLE", "host")
        .env("MHGU_LINK_TEST_PORT", port.to_string())
        .env("RUST_BACKTRACE", "1")
        .arg("--exact")
        .arg("a_host_and_a_joiner_exchange_a_packet")
        .arg("--nocapture")
        .spawn()
        .expect("spawn the host");

    mhgu_ldn_set_delivery(Some(on_packet), Some(on_aux));
    let info = vec![0u8; mhgu_link_ffi::NETINFO_SIZE];
    let mut ok = false;
    let addr = format!("127.0.0.1:{port}");
    let addr = std::ffi::CString::new(addr).unwrap();
    for _ in 0..40 {
        if mhgu_ldn_join(addr.as_ptr(), c"joiner".as_ptr() as *const _, info.as_ptr(), info.len() as u32) {
            ok = true;
            break;
        }
        std::thread::sleep(Duration::from_millis(100));
    }
    assert!(ok, "the joiner never connected");
    assert!(mhgu_ldn_net_up());
    assert!(!mhgu_ldn_hosting());

    // Our own address must be the one the host gave this station, on the host's subnet: a joiner that
    // takes the host's address (or another subnet) sends its own packets to itself and the game's
    // Pia handshake fails ("can't enter the gathering hub", 2618-502).
    let own = mhgu_ldn_own_ip();
    assert_ne!(own, 0, "the joiner has no address");
    assert_ne!(own, 0xA9FE_0001, "the joiner took the host's address");
    assert_eq!(own & 0xFFFF_FF00, 0xA9FE_0000, "the joiner is on another subnet than the host");

    // Send one packet; the host echoes it back.
    let v = MhguVPacket { src_ip: 0xA9FE_0002, dst_ip: 0xA9FE_0001, src_port: 0x4242, dst_port: 0x0BEB, proto: 0, kind: 0, pad: 0 };
    let body = b"mhgu-online";
    mhgu_ldn_send(&v, body.as_ptr(), body.len() as u32);

    let deadline = Instant::now() + Duration::from_secs(10);
    while GOT.load(Ordering::SeqCst) == 0 && Instant::now() < deadline {
        mhgu_ldn_poll();
        std::thread::sleep(Duration::from_millis(20));
    }
    assert!(GOT.load(Ordering::SeqCst) > 0, "no packet came back through the link");
    assert_eq!(&LAST.lock().unwrap()[..], body, "the echoed payload differs");
    mhgu_ldn_leave();
    let _ = child.kill();
    let _ = child.wait();
}

fn run_host() {
    HOST_ROLE.store(true, Ordering::SeqCst);
    mhgu_ldn_set_delivery(Some(on_packet), Some(on_aux));
    let port: u16 = std::env::var("MHGU_LINK_TEST_PORT").ok().and_then(|v| v.parse().ok()).unwrap_or(39555);
    let info = vec![0u8; mhgu_link_ffi::NETINFO_SIZE];
    let mut out = 0u16;
    assert!(mhgu_ldn_host(port, &mut out, c"host".as_ptr() as *const _, info.as_ptr(), info.len() as u32));
    assert_eq!(out, port);
    assert!(mhgu_ldn_hosting());
    let deadline = Instant::now() + Duration::from_secs(15);
    while Instant::now() < deadline {
        mhgu_ldn_poll();
        std::thread::sleep(Duration::from_millis(10));
    }
}
