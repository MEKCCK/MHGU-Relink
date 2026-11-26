use std::time::Duration;

/// Another hub server to federate with: its address and the token it accepts from us.
#[derive(Clone, Debug, PartialEq)]
pub struct Peer {
    /// `host:port` of its hub server (TCP and UDP).
    pub addr: String,
    /// Its federation token (the token that server accepts).
    pub token: String,
    /// A name for logs and the web UI ("" = the address).
    pub name: String,
    /// The peer's expected `ed25519:<id>` (Ed25519 handshake); empty: use the shared token (v1 handshake).
    pub key: String,
}

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
    /// Hunter Search networks in one LIST answer, after the hubs (the game shows 6).
    pub list_search_max: usize,
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
    /// The oldest link protocol (the game's PROTO_VERSION) served; older games get ERR_VERSION (0: any).
    pub min_proto: u16,
    /// The newest link protocol served; newer ones get ERR_VERSION (0: no newest).
    pub max_proto: u16,
    /// The link protocols from `link_pool_low` to `link_pool_high` play together: their hubs are listed and
    /// joined across them (a protocol 4 game sees each hub's protocol and speaks the lower one). Others play
    /// with their own only. `link_pool_low == 0`: no pool.
    pub link_pool_low: u16,
    pub link_pool_high: u16,
    /// Players online at once that the friends list keeps (PRESENCE); full: ERR_BUSY.
    pub max_presence: usize,
    /// How long a PRESENCE poll keeps a player online; a player who stops polling is announced offline after this.
    pub presence_ttl: Duration,
    /// Android's public hubs (the key "public-phone") in a room of their own, as the game asks; false: they meet the
    /// public room, so phones and PCs without a key see each other's hubs.
    pub separate_phones: bool,
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
    /// Servers to federate with (their hubs are listed here and joined through them). Empty: no federation.
    pub federate: Vec<Peer>,
    /// The token other servers must prove to federate with us ("" = federation disabled).
    pub federation_token: String,
    /// Our name in peers' logs and web pages ("" = the address).
    pub federation_name: String,
    /// The seed file of our Ed25519 identity ("" = a fresh ephemeral identity).
    pub identity_file: String,
    /// The address peers should use for us, as put in `/.well-known/mhgu-federation` ("" = `addr`).
    pub advertise: String,
    /// Server ids allowed to federate with us by Ed25519 identity (exact ids, or "*" for any valid key).
    pub federation_allow: Vec<String>,
    /// Answer LAN discovery beacons and announce ourselves as a candidate (never auto-joined).
    pub discover_lan: bool,
    /// Where the admin page's approved identities/peers are saved ("" = not persisted).
    pub federation_state: String,
    /// Share friends' presence with federation peers (and accept theirs).
    pub federation_presence: bool,
    /// Extra hops for transitive hub advertisements (0 = direct peers only; joins still go through the chain).
    pub federation_transitive: u8,
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
            list_search_max: 6,
            req_rate: 2.0,
            req_burst: 15.0,
            bandwidth: 1024.0 * 1024.0,
            handshake: Duration::from_secs(10),
            ping_every: Duration::from_secs(15),
            min_proto: 0,
            max_proto: 100,
            link_pool_low: 3,
            link_pool_high: 4,
            max_presence: 20000,
            presence_ttl: Duration::from_secs(90),
            separate_phones: true,
            verbose: false,
            web: ":39511".into(),
            web_token: String::new(),
            key: String::new(),
            web_dir: String::new(),
            federate: Vec::new(),
            federation_token: String::new(),
            federation_name: String::new(),
            identity_file: String::new(),
            advertise: String::new(),
            federation_allow: Vec::new(),
            discover_lan: false,
            federation_state: String::new(),
            federation_presence: true,
            federation_transitive: 0,
        }
    }
}

/// Settings that can change while the server runs (the admin page; the JSON API): limits apply to new
/// connections/requests/pairs, and a raised min_proto takes older hubs off the list.
#[derive(Clone, Debug, PartialEq)]
pub struct Live {
    pub min_proto: u16,
    pub max_proto: u16,
    pub link_pool_low: u16,
    pub link_pool_high: u16,
    pub separate_phones: bool,
    pub verbose: bool,
    pub max_conns: usize,
    pub max_conns_per_ip: usize,
    pub max_hubs: usize,
    pub max_hubs_per_ip: usize,
    pub list_max: usize,
    pub list_search_max: usize,
    pub req_rate: f64,
    pub req_burst: f64,
    pub bandwidth: f64,
}

impl Live {
    pub fn from_config(c: &Config) -> Live {
        Live {
            min_proto: c.min_proto,
            max_proto: c.max_proto,
            link_pool_low: c.link_pool_low,
            link_pool_high: c.link_pool_high,
            separate_phones: c.separate_phones,
            verbose: c.verbose,
            max_conns: c.max_conns,
            max_conns_per_ip: c.max_conns_per_ip,
            max_hubs: c.max_hubs,
            max_hubs_per_ip: c.max_hubs_per_ip,
            list_max: c.list_max,
            list_search_max: c.list_search_max,
            req_rate: c.req_rate,
            req_burst: c.req_burst,
            bandwidth: c.bandwidth,
        }
    }

    /// Whether a game of this link protocol is served (`min_proto` up to `max_proto`).
    pub fn serves(&self, proto: u16) -> bool {
        proto >= self.min_proto && (self.max_proto == 0 || proto <= self.max_proto)
    }

    /// The link protocols that play together share one: the pool's, so their hubs are listed and joined
    /// across them. A protocol outside the pool plays with its own only.
    pub fn group(&self, proto: u16) -> u16 {
        if self.link_pool_low != 0 && proto >= self.link_pool_low && proto <= self.link_pool_high {
            self.link_pool_low
        } else {
            proto
        }
    }

    /// The pool as the settings file writes it ("3-4", or "off").
    pub fn pool_text(&self) -> String {
        if self.link_pool_low == 0 {
            "off".into()
        } else {
            format!("{}-{}", self.link_pool_low, self.link_pool_high)
        }
    }
}
