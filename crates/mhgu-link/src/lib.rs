//! The in-game Local Play link, as a Rust client library.
//!
//! This is the station side of MHGURecomp's `runtime/net/ldn.c` link (`MHGL`), and the `MHGD`
//! datagrams the game's network library (Pia) prefers. A client connects to a host, sends a
//! `Hello`, receives `Info` (a scan answer) / `Welcome` (accepted) / `Refuse`, and then exchanges
//! `Packet` (a `VPacket` header + the game's bytes) and `Aux` (port-private messages) frames.
//!
//! The wire format is byte-for-byte ldn.c's, so a Rust client interoperates with MHGURecomp and
//! gu-native peers on LAN or through a hub server relay.

pub mod client;
pub mod hello;
pub mod lobby;
pub mod scan;
pub use mhgu_protocol::sha256;
pub mod udp;
pub mod vpacket;

pub use client::{Client, Event, Reply};
pub use hello::Hello;
pub use lobby::{room_from_key, HostSession, HubEntry, Lobby, Spliced};
pub use scan::ScanReply;
pub use vpacket::VPacket;

/// The shared wire formats.
pub use mhgu_protocol as proto;
