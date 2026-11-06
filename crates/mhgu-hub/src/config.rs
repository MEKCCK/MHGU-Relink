use std::time::Duration;

/// Server options, matching the Go server's flags.
#[derive(Clone, Debug)]
pub struct Config {
    /// Address and port to listen on (TCP and UDP share it).
    pub addr: String,
    /// Connections at once.
    pub max_conns: usize,
    /// Connections at once from one IP address.
    pub max_conns_per_ip: usize,
    /// Hubs listed at once.
    pub max_hubs: usize,
    /// Hubs one IP address may host.
    pub max_hubs_per_ip: usize,
    /// Hubs in one LIST answer (the game shows 18: 3 pages of 6).
    pub list_max: usize,
    /// LIST/JOIN requests a second per IP address.
    pub req_rate: f64,
    /// Burst of requests per IP address.
    pub req_burst: f64,
    /// Bytes a second per pair and direction (TCP), and per pair (UDP).
    pub bandwidth: f64,
    /// Deadline for the first frame, and a joining player's wait for the host.
    pub handshake: Duration,
    /// Keep-alive interval on a host's connection.
    pub ping_every: Duration,
    /// Log hubs and joins (never IP addresses).
    pub verbose: bool,
    /// The web UI's address ("" = off), public page and /admin.
    pub web: String,
    /// The token /admin needs ("" = a random one, printed at startup).
    pub web_token: String,
    /// A directory of your own web UI: `index.html`, `admin.html`, `log.html` and any assets under it are
    /// served instead of the built-in pages ("" = the built-in pages). The JSON API stays.
    pub web_dir: String,
    /// A matchmaking key this server insists on ("" = any key, the public room included).
    /// The key itself never leaves the server: only its room hash is compared.
    pub key: String,
}

impl Default for Config {
    fn default() -> Self {
        Config {
            addr: ":39510".into(),
            max_conns: 4000,
            max_conns_per_ip: 24,
            max_hubs: 2000,
            max_hubs_per_ip: 4,
            list_max: 18,
            req_rate: 2.0,
            req_burst: 15.0,
            bandwidth: 1024.0 * 1024.0,
            handshake: Duration::from_secs(10),
            ping_every: Duration::from_secs(15),
            verbose: false,
            web: ":39511".into(),
            web_token: String::new(),
            key: String::new(),
            web_dir: String::new(),
        }
    }
}
