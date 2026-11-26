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
    if cfg.min_proto > 0 {
        println!("mhgu-hub: refusing games older than link protocol {}", cfg.min_proto);
    }
    if cfg.max_proto > 0 {
        println!("mhgu-hub: refusing games newer than link protocol {}", cfg.max_proto);
    }
    if cfg.link_pool_low != 0 {
        println!("mhgu-hub: link protocols {}-{} play together", cfg.link_pool_low, cfg.link_pool_high);
    }
    if !cfg.separate_phones {
        println!("mhgu-hub: one public room: Android players' public hubs are listed with everyone's");
    }
    println!(
        "mhgu-hub: federation identity {}{}",
        server.identity_id(),
        if cfg.identity_file.is_empty() { " (ephemeral: -identity FILE to keep it)" } else { "" }
    );
    if !cfg.federation_allow.is_empty() {
        println!("mhgu-hub: accepting federated identities: {}", cfg.federation_allow.join(", "));
    }
    if !cfg.federation_state.is_empty() {
        println!("mhgu-hub: admin approvals are saved in {}", cfg.federation_state);
    }
    if cfg.discover_lan {
        println!("mhgu-hub: LAN discovery on UDP 39513 (candidates only, never auto-joined)");
    }
    if !cfg.federation_presence {
        println!("mhgu-hub: federation presence off: friends' online states stay local");
    }
    if cfg.federation_transitive > 0 {
        println!("mhgu-hub: transitive hub advertisements up to {} hop(s)", cfg.federation_transitive);
    }
    if !cfg.federate.is_empty() {
        let names: Vec<String> = cfg.federate.iter().map(|p| if p.name.is_empty() { p.addr.clone() } else { p.name.clone() }).collect();
        println!(
            "mhgu-hub: federating with {} server(s): {}{}",
            names.len(),
            names.join(", "),
            if cfg.federation_token.is_empty() { " (incoming federation off: no -federation-token)" } else { "" }
        );
    }
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
    cfg.identity_file = "mhgu-hub.identity".into();
    cfg.federation_state = "mhgu-hub.federation".into();
    let mut args = std::env::args().skip(1);
    while let Some(a) = args.next() {
        match a.as_str() {
            "-addr" => cfg.addr = args.next().expect("-addr needs a value"),
            "-max-conns" => cfg.max_conns = number(&mut args, "-max-conns"),
            "-max-conns-per-ip" => cfg.max_conns_per_ip = number(&mut args, "-max-conns-per-ip"),
            "-max-hubs" => cfg.max_hubs = number(&mut args, "-max-hubs"),
            "-max-hubs-per-ip" => cfg.max_hubs_per_ip = number(&mut args, "-max-hubs-per-ip"),
            "-list-max" => cfg.list_max = number(&mut args, "-list-max"),
            "-list-search-max" => cfg.list_search_max = number(&mut args, "-list-search-max"),
            "-min-proto" => cfg.min_proto = number(&mut args, "-min-proto"),
            "-max-proto" => cfg.max_proto = number(&mut args, "-max-proto"),
            "-link-pool" => match args.next().as_deref() {
                Some("off") => cfg.link_pool_low = 0,
                Some(v) => {
                    let (lo, hi) = v.split_once('-').expect("-link-pool must be low-high, or off");
                    cfg.link_pool_low = lo.parse().expect("-link-pool: low must be a number");
                    cfg.link_pool_high = hi.parse().expect("-link-pool: high must be a number");
                }
                None => panic!("-link-pool needs a value"),
            },
            "-max-presence" => cfg.max_presence = number(&mut args, "-max-presence"),
            "-presence-ttl" => {
                let secs: u64 = number(&mut args, "-presence-ttl");
                cfg.presence_ttl = std::time::Duration::from_secs(secs.max(1));
            }
            "-separate-phones" => {
                cfg.separate_phones = match args.next().as_deref() {
                    Some("false") | Some("0") | Some("off") => false,
                    _ => true,
                };
            }
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
            "-federation-token" => cfg.federation_token = args.next().expect("-federation-token needs a value"),
            "-federation-name" => cfg.federation_name = args.next().expect("-federation-name needs a value"),
            "-identity" => cfg.identity_file = args.next().expect("-identity needs a value"),
            "-federation-state" => cfg.federation_state = args.next().expect("-federation-state needs a value"),
            "-advertise" => cfg.advertise = args.next().expect("-advertise needs a value"),
            "-federation-allow" => {
                let v = args.next().expect("-federation-allow needs a value");
                for id in v.split(',') {
                    if !id.trim().is_empty() {
                        cfg.federation_allow.push(id.trim().to_string());
                    }
                }
            }
            "-discover" => {
                let v = args.next().unwrap_or_default();
                cfg.discover_lan = v == "lan" || v == "on";
            }
            "-federation-transitive" => {
                cfg.federation_transitive = match args.next().as_deref() {
                    Some("off") | Some("0") | None => 0,
                    Some(v) => v.parse().unwrap_or(1),
                };
            }
            "-federation-presence" => {
                cfg.federation_presence = !matches!(
                    args.next().as_deref(),
                    Some("false") | Some("0") | Some("off")
                );
            }
            "-federate" => {
                let v = args.next().expect("-federate needs a value");
                let mut it = v.splitn(3, ',');
                let addr = it.next().unwrap_or_default().trim().to_string();
                let second = it.next().unwrap_or_default().trim().to_string();
                let name = it.next().unwrap_or_default().to_string();
                if addr.is_empty() {
                    eprintln!("mhgu-hub: -federate needs addr[,token_or_key[,name]]");
                    std::process::exit(2);
                }
                let (token, key) = if second.starts_with("ed25519:") {
                    (String::new(), second)
                } else {
                    (second, String::new())
                };
                cfg.federate.push(mhgu_hub::Peer { addr, token, name, key });
            }
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
  -list-search-max N    Hunter Search networks in one answer, after the hubs (default 6)\n\
  -bandwidth N          bytes a second per player link and direction (default 1048576)
\
  -min-proto N          oldest link protocol served; older games are refused (default 0: any)
\
  -max-proto N          newest link protocol served; newer games are refused (default 100; 0: no newest)
\
  -link-pool LOW-HIGH   link protocols that play together, e.g. 3-4 (default), or off
\
  -max-presence N       players online the friends list keeps (default 20000)
\
  -presence-ttl SECS    friend online TTL after the last poll (default 90)
\
  -separate-phones[=false]  Android public hubs in their own room (default on; =false: one public room)\n\
  -v, --verbose         log hubs and joins (never IP addresses)\n\
  -web ADDR             web UI address (default :39511, off: none)\n\
  -web-token TOKEN      the token /admin needs (default: random, printed at startup)\n\
  -web-dir DIR          serve your own web UI from DIR (index.html, admin.html, log.html, assets)\n\
  -key KEY              only serve the matchmaking room of KEY (default: any, public room included)
\
  -federation-token T   accept federation from servers that prove token T (default: off)
\
  -federate ADDR[,TOKEN_OR_ID[,NAME]]  share hubs with this server (repeatable; default port 39510)
\
  -federation-name NAME our name in peers' logs (default: the address)
\
  -federation-allow ID  accept this Ed25519 server id (repeatable; '*' = any valid key)
\
  -identity FILE        Ed25519 seed (default mhgu-hub.identity; empty: ephemeral)
\
  -federation-state FILE  approvals from the admin page (default mhgu-hub.federation)
\
  -advertise HOST:PORT  address published in /.well-known/mhgu-federation
\
  -discover lan         announce/collect LAN federation candidates (never auto-join)
\
  -federation-presence[=false]  share online friends with peers (default on)
\
  -federation-transitive N  advertise hubs up to N hops away (default 0; joins relay through peers)
\
  -h, --help            this help\n\
\n\
Wire-compatible with gu-native and MHGURecomp clients."
    );
}
