// Re-hosting in one process: the game destroys a network and creates another right away (a kick, a
// change of room). The listening port must be free again, or the second create fails with
// "bind: Address already in use".
use mhgu_link_ffi::*;

extern "C" fn on_packet(_p: *const MhguVPacket, _d: *const u8, _l: u32) {}
extern "C" fn on_aux(_d: *const u8, _l: u32) {}

fn info() -> Vec<u8> {
    let mut i = vec![0u8; 0x480];
    i[0x4B] = 2;
    i[0x66] = 4;
    i[0x67] = 1;
    i[0x68 + 4..0x68 + 10].copy_from_slice(&[0x02, 0, 0, 1, 2, 3]);
    i[0x68 + 0x0B] = 1;
    i
}

#[test]
fn host_leave_host_again() {
    mhgu_ldn_set_delivery(Some(on_packet), Some(on_aux));
    let info = info();
    let mut port = 0u16;
    assert!(mhgu_ldn_host(0, &mut port, c"one".as_ptr() as *const _, info.as_ptr(), info.len() as u32));
    let first = port;
    mhgu_ldn_leave();
    drop(std::net::UdpSocket::bind(("0.0.0.0", first)).expect("leave must release the UDP scan port"));
    let mut port2 = 0u16;
    assert!(
        mhgu_ldn_host(first, &mut port2, c"two".as_ptr() as *const _, info.as_ptr(), info.len() as u32),
        "hosting again on the same port must work after leave"
    );
    assert_eq!(port2, first);
    mhgu_ldn_leave();
}
