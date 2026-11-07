//! A from-scratch Rust hub server for MHGU Local Play over the internet.
//!
//! The wire protocol is exactly the one MHGURecomp's Go server implements (`MHGS`/`MHGR`), which the
//! gu-native Android fork also speaks, so hubs hosted by either game client are listed and joined
//! through this server and their UDP is relayed without a forwarded port.

pub mod bucket;
pub mod config;
mod server;
pub mod web;

pub use config::{Config, Live};
pub use server::Server;

/// The wire formats, re-exported for clients and tests.
pub use mhgu_protocol as proto;
