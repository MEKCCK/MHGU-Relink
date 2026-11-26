//! Room details carried in the game's `NetworkInfo` advertise block.
//!
//! These offsets are the game's own advertise layout, the same ones the Go hub/overlay
//! implementations use: room name, up to four hunter names, quest id, in-quest flag and passcode.

use mhgu_protocol as proto;

const ADV_ROOM_NAME: usize = 0x34;
const ADV_ROOM_NAME_LEN: usize = 32;
const ADV_QUEST_ID: usize = 0x24;
const ADV_FLAGS: usize = 0x54;
const ADV_PLAYER_BASE: usize = 0x70;
const ADV_PLAYER_STRIDE: usize = 0x14;
const ADV_PLAYER_LEN: usize = 16;
const ADV_MAX_PLAYERS: usize = 4;
const ADV_PASSCODE: usize = 0xC0;
const FLAG_IN_QUEST: u32 = 0x0010_0000;

#[derive(Clone, Debug, Default)]
pub struct RoomDetails {
    pub room_name: String,
    pub names: Vec<String>,
    pub count: u8,
    pub max: u8,
    pub quest_id: u32,
    pub in_quest: bool,
    pub passcode: Option<u16>,
    pub hunter_search: bool,
}

fn u16_at(b: &[u8], at: usize) -> Option<u16> {
    b.get(at..at + 2).map(|v| u16::from_le_bytes([v[0], v[1]]))
}

fn u32_at(b: &[u8], at: usize) -> Option<u32> {
    b.get(at..at + 4).map(|v| u32::from_le_bytes([v[0], v[1], v[2], v[3]]))
}

fn private_use(c: char) -> bool {
    matches!(c as u32, 0xE000..=0xF8FF | 0xF0000..=0xFFFFD | 0x100000..=0x10FFFD)
}

fn utf16_name(b: &[u8]) -> String {
    let mut units = Vec::new();
    for chunk in b.chunks_exact(2) {
        let u = u16::from_le_bytes([chunk[0], chunk[1]]);
        if u == 0 {
            break;
        }
        units.push(u);
    }
    let text = String::from_utf16_lossy(&units);
    text.chars()
        .filter(|c| !c.is_control() && !private_use(*c) && *c != '\u{fffd}')
        .collect::<String>()
        .trim()
        .to_string()
}

/// Parses the advertise block of a `NetworkInfo`; all offsets are bounds-checked because a
/// malformed listing must not take the server down.
pub fn parse(info: &[u8]) -> RoomDetails {
    let mut out = RoomDetails {
        count: info.get(proto::NI_NODE_COUNT).copied().unwrap_or(0),
        max: info.get(proto::NI_NODE_MAX).copied().unwrap_or(0),
        hunter_search: proto::hunt_search(info),
        ..RoomDetails::default()
    };
    let adv_len = u16_at(info, proto::NI_ADV_SIZE).unwrap_or(0) as usize;
    let end = (proto::NI_ADV + adv_len).min(info.len());
    let adv = info.get(proto::NI_ADV..end).unwrap_or(&[]);
    if out.hunter_search {
        // A Hunter Search network is not a hub: its advertise block has a different layout.
        return out;
    }
    out.room_name = adv
        .get(ADV_ROOM_NAME..ADV_ROOM_NAME + ADV_ROOM_NAME_LEN)
        .map(utf16_name)
        .unwrap_or_default();
    for i in 0..ADV_MAX_PLAYERS {
        let at = ADV_PLAYER_BASE + i * ADV_PLAYER_STRIDE;
        let Some(raw) = adv.get(at..at + ADV_PLAYER_LEN) else { break };
        let name = utf16_name(raw);
        if !name.is_empty() {
            out.names.push(name);
        }
    }
    out.quest_id = u32_at(adv, ADV_QUEST_ID).unwrap_or(0);
    if out.quest_id == 0xFFFF_FFFF {
        out.quest_id = 0;
    }
    if let Some(flags) = u32_at(adv, ADV_FLAGS) {
        out.in_quest = flags & FLAG_IN_QUEST != 0;
    }
    out.passcode = u16_at(adv, ADV_PASSCODE).and_then(|p| (p != 0xFFFF).then_some(p & 0x7FFF));
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    // The advertise block from the Go/overlay implementation's selftest.
    const SAMPLE: &str = concat!(
        "33467584000000000100000000000000",
        "00000000efcdab907856341201000000",
        "00000000e8270000000000000a000000",
        "00000000fb4ea15210625854ec53c696",
        "2d4e01ff00001834513f201a5240331f",
        "57595a0e6e7b10000100e70300000000",
        "00000000070700000100010000000000",
        "c35754588983c5960000000000000000",
        "00000000847389730000000000000000",
        "00000000000000000000000000000000",
        "00000000000000000000000000000000",
        "00000000000000000000000000000000",
        "8707000000000000",
    );

    #[test]
    fn advertises_room_players_quest_and_passcode() {
        let adv: Vec<u8> = (0..SAMPLE.len())
            .step_by(2)
            .map(|i| u8::from_str_radix(&SAMPLE[i..i + 2], 16).unwrap())
            .collect();
        assert_eq!(adv.len(), 200);
        let mut info = vec![0u8; proto::NETINFO_SIZE];
        info[proto::NI_NODE_MAX] = 4;
        info[proto::NI_NODE_COUNT] = 2;
        info[proto::NI_ADV_SIZE..proto::NI_ADV_SIZE + 2].copy_from_slice(&(adv.len() as u16).to_le_bytes());
        info[proto::NI_ADV..proto::NI_ADV + adv.len()].copy_from_slice(&adv);
        let d = parse(&info);
        assert_eq!(d.room_name, "任务成员召集中！");
        assert_eq!(d.names, ["埃塔莉雅", "玄玉"]);
        assert_eq!((d.count, d.max), (2, 4));
        assert_eq!(d.quest_id, 10216);
        assert!(d.in_quest);
        assert_eq!(d.passcode, Some(1927));
        assert!(!d.hunter_search);
    }

    #[test]
    fn hunter_search_does_not_expose_hub_metadata() {
        let mut info = vec![0u8; proto::NETINFO_SIZE];
        info[proto::NI_NET_TYPE] = 2;
        info[proto::NI_NODE_MAX] = 2;
        info[proto::NI_NODE_COUNT] = 1;
        let mut adv = vec![0u8; 0x1D];
        adv[proto::ADV_HUNT_SEARCH] = 0xFF;
        info[proto::NI_ADV_SIZE..proto::NI_ADV_SIZE + 2].copy_from_slice(&(adv.len() as u16).to_le_bytes());
        info[proto::NI_ADV..proto::NI_ADV + adv.len()].copy_from_slice(&adv);
        let d = parse(&info);
        assert!(d.hunter_search);
        assert!(d.room_name.is_empty() && d.names.is_empty());
        assert_eq!((d.quest_id, d.passcode, d.in_quest), (0, None, false));
    }
}
