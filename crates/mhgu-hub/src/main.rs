use std::process::ExitCode;

use mhgu_hub::{Config, Server};

fn main() -> ExitCode {
    let cfg = parse_args();
    let server = match Server::new(cfg.clone()) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("mhgu-hub: {e}");
            return ExitCode::FAILURE;
        }
    };
    println!(
        "mhgu-hub: MHGU hub server on {} (TCP and UDP)",
        server.addr()
    );
    if !cfg.key.is_empty() {
        let room = mhgu_protocol::room_from_key(&cfg.key);
        println!(
            "mhgu-hub: only the room of -key is served (key {:#04x}{:02x}{:02x}{:02x}…)",
            room[0], room[1], room[2], room[3]
        );
    }
    match server.serve() {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("mhgu-hub: {e}");
            ExitCode::FAILURE
        }
    }
}

fn parse_args() -> Config {
    let mut cfg = Config::default();
    let mut args = std::env::args().skip(1);
    while let Some(a) = args.next() {
        match a.as_str() {
            "-addr" => cfg.addr = args.next().expect("-addr needs a value"),
            "-max-conns" => cfg.max_conns = number(&mut args, "-max-conns"),
            "-max-conns-per-ip" => cfg.max_conns_per_ip = number(&mut args, "-max-conns-per-ip"),
            "-max-hubs" => cfg.max_hubs = number(&mut args, "-max-hubs"),
            "-max-hubs-per-ip" => cfg.max_hubs_per_ip = number(&mut args, "-max-hubs-per-ip"),
            "-list-max" => cfg.list_max = number(&mut args, "-list-max"),
            "-bandwidth" => {
                cfg.bandwidth = args
                    .next()
                    .expect("-bandwidth needs a value")
                    .parse()
                    .expect("-bandwidth must be a number")
            }
            "-v" | "--verbose" => cfg.verbose = true,
            "-web" => cfg.web = args.next().expect("-web needs a value"),
            "-web-token" => cfg.web_token = args.next().expect("-web-token needs a value"),
            "-key" => cfg.key = args.next().expect("-key needs a value"),
            "-web-dir" => cfg.web_dir = args.next().expect("-web-dir needs a value"),
            "-h" | "--help" => {
                print_help();
                std::process::exit(0);
            }
            other => {
                eprintln!("mhgu-hub: unknown argument {other} (try --help)");
                std::process::exit(2);
            }
        }
    }
    cfg
}

fn number<T: std::str::FromStr>(args: &mut impl Iterator<Item = String>, flag: &str) -> T {
    args.next()
        .unwrap_or_else(|| panic!("{flag} needs a value"))
        .parse()
        .unwrap_or_else(|_| panic!("{flag} must be a number"))
}

fn print_help() {
    println!(
        "mhgu-hub — MHGU hub server (Local Play over the internet)\n\
\n\
Usage: mhgu-hub [options]\n\
\n\
  -addr ADDR            address and port to listen on, TCP and UDP (default :39510)\n\
  -max-conns N          connections at once (default 4000)\n\
  -max-conns-per-ip N   connections at once from one IP (default 24)\n\
  -max-hubs N           hubs listed at once (default 2000)\n\
  -max-hubs-per-ip N    hubs one IP may host (default 4)\n\
  -list-max N           hubs in one search answer (default 18)\n\
  -bandwidth N          bytes a second per player link and direction (default 1048576)\n\
  -v, --verbose         log hubs and joins (never IP addresses)\n\
  -web ADDR             web UI address (default :39511, off: none)\n\
  -web-token TOKEN      the token /admin needs (default: random, printed at startup)\n\
  -web-dir DIR          serve your own web UI from DIR (index.html, admin.html, log.html, assets)\n\
  -key KEY              only serve the matchmaking room of KEY (default: any, public room included)\n\
  -h, --help            this help\n\
\n\
Wire-compatible with gu-native and MHGURecomp clients."
    );
}
