// Through the hub server: one process hosts (listed on the hub), another joins by hub id.
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::time::{Duration, Instant};
use mhgu_link_ffi::*;

static HOST: AtomicBool = AtomicBool::new(false);

/// A NetworkInfo the hub accepts (and the host's node 0 filled in).
fn valid_info() -> Vec<u8> {
    let mut i = vec![0u8; 0x480];
    i[0x4B] = 2;   // network type: ldn
    i[0x66] = 4;   // node max
    i[0x67] = 1;   // node count
    i[0x68 + 4..0x68 + 10].copy_from_slice(&[0x02, 0, 0, 0x11, 0x22, 0x33]);  // node 0 MAC
    i[0x68 + 0x0B] = 1;  // node 0 connected
    i
}
static GOT: AtomicU32 = AtomicU32::new(0);

extern "C" fn on_packet(_p: *const MhguVPacket, data: *const u8, len: u32) {
    let _ = unsafe { std::slice::from_raw_parts(data, len as usize) };
    GOT.fetch_add(1, Ordering::SeqCst);
    if HOST.load(Ordering::SeqCst) { mhgu_ldn_send(_p, data, len); }
}
extern "C" fn on_aux(_d: *const u8, _l: u32) {}

/// Starts the hub in this process on an ephemeral port (a test must not need one running outside).
fn start_hub() -> (std::net::SocketAddr, std::sync::Arc<mhgu_hub::Server>) {
    let mut cfg = mhgu_hub::Config::default();
    cfg.addr = "127.0.0.1:0".into();
    cfg.web = "off".into();
    cfg.handshake = Duration::from_secs(2);
    let srv = std::sync::Arc::new(mhgu_hub::Server::new(cfg).expect("bind the hub"));
    let addr = srv.addr();
    let s2 = std::sync::Arc::clone(&srv);
    std::thread::spawn(move || { let _ = s2.serve(); });
    (addr, srv)
}

#[test]
fn through_the_hub() {
    if std::env::var("HUB_ROLE").as_deref() == Ok("host") { run_host(); return; }
    let (addr, _srv) = start_hub();
    let mut child = std::process::Command::new(std::env::current_exe().unwrap())
        .env("HUB_ROLE", "host").env("HUB_ADDR", addr.to_string())
        .arg("--exact").arg("through_the_hub").arg("--nocapture")
        .spawn().expect("spawn host");
    std::thread::sleep(Duration::from_millis(1500));
    mhgu_ldn_set_delivery(Some(on_packet), Some(on_aux));
    let key = std::ffi::CString::new("hubtest").unwrap();
    let server = std::ffi::CString::new(addr.to_string()).unwrap();
    // find the hub id from the server's list: the host lists it with room(key)
    let room = mhgu_link::room_from_key("hubtest");
    let mut hubs = Vec::new();
    let wait = Instant::now() + Duration::from_secs(8);
    while Instant::now() < wait {
        if let Ok(list) = mhgu_link::Lobby::new(addr).list(mhgu_protocol::LINK_PROTO_VERSION, room) {
            if !list.is_empty() { hubs = list; break; }
        }
        std::thread::sleep(Duration::from_millis(200));
    }
    let hub = hubs.iter().max_by_key(|h| h.id).expect("a hub").id;
    let info = valid_info();
    let mut ok = false;
    for _ in 0..30 {
        if mhgu_ldn_join_server(server.as_ptr(), key.as_ptr(), hub, c"guest".as_ptr() as *const _, info.as_ptr(), info.len() as u32) { ok = true; break; }
        std::thread::sleep(Duration::from_millis(200));
    }
    assert!(ok, "join through the hub failed");
    let v = MhguVPacket { src_ip: 0xA9FE0002, dst_ip: 0xA9FE0001, src_port: 0x4242, dst_port: 0x0BEB, proto: 0, kind: 0, pad: 0 };
    mhgu_ldn_send(&v, b"through-hub".as_ptr(), 11);
    let dl = Instant::now() + Duration::from_secs(8);
    while GOT.load(Ordering::SeqCst) == 0 && Instant::now() < dl { mhgu_ldn_poll(); std::thread::sleep(Duration::from_millis(20)); }
    assert!(GOT.load(Ordering::SeqCst) > 0, "no packet came back through the hub relay");
    mhgu_ldn_leave();
    let _ = child.kill(); let _ = child.wait();
}

fn run_host() {
    HOST.store(true, Ordering::SeqCst);
    mhgu_ldn_set_delivery(Some(on_packet), Some(on_aux));
    let info = valid_info();
    let key = std::ffi::CString::new("hubtest").unwrap();
    let server = std::ffi::CString::new(std::env::var("HUB_ADDR").unwrap_or_else(|_| "127.0.0.1:39510".into())).unwrap();
    let mut port = 0u16;
    assert!(mhgu_ldn_host(0, &mut port, c"host".as_ptr() as *const _, info.as_ptr(), info.len() as u32));
    assert!(mhgu_ldn_host_server(server.as_ptr(), key.as_ptr()), "could not list on the hub");
    let dl = Instant::now() + Duration::from_secs(20);
    while Instant::now() < dl { mhgu_ldn_poll(); std::thread::sleep(Duration::from_millis(20)); }
}
