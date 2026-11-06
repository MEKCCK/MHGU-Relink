use mhgu_protocol::VPACKET_SIZE;

/// A packet of the virtual network (`VPacket` in net.h): the header that precedes the game's bytes
/// inside an `F_PACKET` frame. Fields are in host byte order; all supported hosts are little-endian.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct VPacket {
    pub src_ip: u32,
    pub dst_ip: u32,
    pub src_port: u16,
    pub dst_port: u16,
    /// `VPROTO_UDP` or `VPROTO_TCP`.
    pub proto: u8,
    /// For TCP: `VTCP_DATA` / `SYN` / `SYNACK` / `FIN` / `RST`.
    pub kind: u8,
    pub pad: u16,
}

impl VPacket {
    pub fn to_bytes(&self) -> [u8; VPACKET_SIZE] {
        let mut b = [0u8; VPACKET_SIZE];
        b[0..4].copy_from_slice(&self.src_ip.to_le_bytes());
        b[4..8].copy_from_slice(&self.dst_ip.to_le_bytes());
        b[8..10].copy_from_slice(&self.src_port.to_le_bytes());
        b[10..12].copy_from_slice(&self.dst_port.to_le_bytes());
        b[12] = self.proto;
        b[13] = self.kind;
        b[14..16].copy_from_slice(&self.pad.to_le_bytes());
        b
    }

    pub fn parse(b: &[u8]) -> Option<VPacket> {
        if b.len() < VPACKET_SIZE {
            return None;
        }
        Some(VPacket {
            src_ip: u32::from_le_bytes(b[0..4].try_into().unwrap()),
            dst_ip: u32::from_le_bytes(b[4..8].try_into().unwrap()),
            src_port: u16::from_le_bytes(b[8..10].try_into().unwrap()),
            dst_port: u16::from_le_bytes(b[10..12].try_into().unwrap()),
            proto: b[12],
            kind: b[13],
            pad: u16::from_le_bytes(b[14..16].try_into().unwrap()),
        })
    }
}
