// Saving RyuLdn settings mid-session must apply without a process restart: an empty server
// list at boot, then a server saved later (hosting and scanning), then a different server.
use std::time::{Duration, Instant};
use mhgu_crossplay::ryu;

fn stub(port: u16) -> std::process::Child {
    let path = concat!(env!("CARGO_MANIFEST_DIR"), "/../../tests/ryuldn_stub_server.py");
    let python = if cfg!(windows) { "python" } else { "python3" };
    let child = std::process::Command::new(python).arg(path).arg(port.to_string())
        .stdout(std::process::Stdio::null()).stderr(std::process::Stdio::null()).spawn().expect("stub server");
    std::thread::sleep(Duration::from_millis(800));
    child
}

fn create() -> Result<(u32, u32, Vec<u8>), u32> {
    let sec = [0u8; 0x44];
    let mut user = [0u8; 0x30];
    user[..8].copy_from_slice(b"Reconfig");
    let mut cfg = [0u8; 0x20];
    cfg[0x12] = 4;
    cfg[0x14] = 1;
    cfg[..8].copy_from_slice(&[0x00, 0x80, 0xDD, 0x08, 0x00, 0x77, 0x00, 0x01]);
    ryu::create(0, &cfg, &sec, &user, b"ADV!").map(|(ip, mask, info)| (ip, mask, info.to_vec()))
}

#[test]
fn server_saved_mid_session_applies_without_restart() {
    let base = 30700 + (std::process::id() % 200) as u16;
    let mut srv1 = stub(base);
    let mut srv2 = stub(base + 1);
    let addr1 = format!("127.0.0.1:{base}");
    let addr2 = format!("127.0.0.1:{}", base + 1);

    // Boot: no server saved yet.
    ryu::set_config("", "", "");
    assert_eq!(ryu::server_count(), 0);
    assert!(ryu::scan(&[0u8; 0x60]).is_empty(), "no servers: nothing found");
    assert!(create().is_err(), "no servers: hosting fails");

    // The player saves a server in the settings; the next search/host must use it.
    ryu::set_config(&addr1, "", "");
    assert_eq!(ryu::server_count(), 1);
    let (ip, _mask, _info) = create().expect("hosting works right after saving the server");
    assert_eq!(ip, 0x0A72_0001);
    let mut found = Vec::new();
    for _ in 0..40 {
        found = ryu::scan(&[0u8; 0x60]);
        if !found.is_empty() { break; }
        std::thread::sleep(Duration::from_millis(200));
    }
    assert!(!found.is_empty(), "scan finds the lobby right after saving the server");

    // Switching servers while the old server's lobby is still up: the next scan must ask
    // the new server, not replay the previous server's cached results.
    ryu::set_config(&addr2, "", "");
    assert!(ryu::scan(&[0u8; 0x60]).is_empty(), "no stale lobbies of the previous server after a change");
    ryu::leave();

    // The old server is still running, so hosting can only succeed on the newly saved one.
    let end = Instant::now() + Duration::from_secs(10);
    loop {
        match create() {
            Ok(_) => break,
            Err(e) if Instant::now() < end => {
                eprintln!("[test] retrying after {e:#x} (previous server's cooldown)");
                std::thread::sleep(Duration::from_millis(300));
            }
            Err(e) => panic!("hosting on the newly saved server failed: {e:#x}"),
        }
    }
    ryu::leave();
    let _ = srv1.kill();
    let _ = srv1.wait();
    let _ = srv2.kill();
    let _ = srv2.wait();
    println!("PASS: RyuLdn server settings apply mid-session, no restart, no stale scans");
}
