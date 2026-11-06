use mhgu_protocol::{LINK_MAGIC, LINK_PROTO_VERSION};

/// `sizeof(Hello)` in ldn.c.
pub const HELLO_SIZE: usize = 52;
/// `name[0x21]`.
pub const NAME_MAX: usize = 0x21;

/// The first frame a station sends: who we are, and whether this is only a scan.
#[derive(Clone, Debug)]
pub struct Hello {
    /// True for a Scan: ask for the host's NetworkInfo, do not join.
    pub scan_only: bool,
    /// Local communication version (the game's), checked against the host's.
    pub lcv: u16,
    pub mac: [u8; 6],
    /// The player's name, NUL-terminated, up to `NAME_MAX` bytes.
    pub name: [u8; NAME_MAX],
}

impl Default for Hello {
    fn default() -> Self {
        Hello {
            scan_only: false,
            lcv: 0,
            mac: [0; 6],
            name: [0; NAME_MAX],
        }
    }
}

impl Hello {
    /// `MHGL` + u16 version + flags + lcv + mac + name + padding.
    pub fn to_bytes(&self) -> [u8; HELLO_SIZE] {
        let mut b = [0u8; HELLO_SIZE];
        b[..4].copy_from_slice(&LINK_MAGIC);
        b[4..6].copy_from_slice(&LINK_PROTO_VERSION.to_le_bytes());
        b[6] = self.scan_only as u8;
        // b[7]: pad
        b[8..10].copy_from_slice(&self.lcv.to_le_bytes());
        b[10..16].copy_from_slice(&self.mac);
        b[16..16 + NAME_MAX].copy_from_slice(&self.name);
        // b[16 + NAME_MAX ..]: pad2[3]
        b
    }

    /// Parses a `Hello`; `None` on a bad magic or an unsupported protocol version.
    pub fn parse(b: &[u8]) -> Option<Hello> {
        if b.len() < HELLO_SIZE || b[..4] != LINK_MAGIC {
            return None;
        }
        if u16::from_le_bytes([b[4], b[5]]) != LINK_PROTO_VERSION {
            return None;
        }
        let mut mac = [0u8; 6];
        mac.copy_from_slice(&b[10..16]);
        let mut name = [0u8; NAME_MAX];
        name.copy_from_slice(&b[16..16 + NAME_MAX]);
        Some(Hello {
            scan_only: b[6] != 0,
            lcv: u16::from_le_bytes([b[8], b[9]]),
            mac,
            name,
        })
    }

    /// The name as a string (up to the first NUL).
    pub fn name_string(&self) -> String {
        let n = self.name.iter().position(|&c| c == 0).unwrap_or(NAME_MAX);
        String::from_utf8_lossy(&self.name[..n]).into_owned()
    }
}
