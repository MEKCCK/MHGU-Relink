//! A from-scratch Rust hub server for MHGU Local Play over the internet.
//!
//! The wire protocol is exactly the one MHGURecomp's hub implements (`MHGS`/`MHGR`), so its game
//! clients can host, list and join through this server and their UDP is relayed without a forwarded
//! port. gu-native is not a supported online target: it has not followed Recomp's protocol updates.

pub mod bucket;
pub mod config;
pub mod identity;
mod room;
mod quests;
mod server;
pub mod web;

pub use config::{Config, Live, Peer};
pub use server::Server;

/// The wire formats, re-exported for clients and tests.
pub use mhgu_protocol as proto;
