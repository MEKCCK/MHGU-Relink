//! The web UI: a public page listing the hubs, and an admin area behind a token (hubs, bans, log).
//!
//! A tiny HTTP server (no dependencies). Pages are in English, Spanish or Chinese (`?lang=zh`, kept
//! in a cookie): `GET /` (public, auto-refreshed), `GET /api/hubs` (JSON), `GET /admin`,
//! `GET /admin/log`, and `POST /admin/close|ban|unban`.

use std::io::{self, BufRead, BufReader, Read, Write};
use std::net::{IpAddr, TcpListener, TcpStream};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::Instant;

use crate::config::Live;
use crate::proto;
use crate::server::{Shared, WebView};

#[derive(Clone, Copy, PartialEq)]
enum Lang {
    En,
    Zh,
    Es,
}

impl Lang {
    fn code(self) -> &'static str {
        match self {
            Lang::En => "en",
            Lang::Zh => "zh",
            Lang::Es => "es",
        }
    }
    fn of(v: &str) -> Option<Lang> {
        match v {
            "en" => Some(Lang::En),
            "zh" => Some(Lang::Zh),
            "es" => Some(Lang::Es),
            _ => None,
        }
    }
}

/// A UI string. Keys are the English text (the `en` column is the key itself).
fn t(l: Lang, key: &str) -> &'static str {
    macro_rules! s {
        ($en:literal, $zh:literal, $es:literal) => {
            ($en, $zh, $es)
        };
    }
    let (en, zh, es) = match key {
        "hubs" => s!("hubs", "房间", "salas"),
        "players" => s!("players", "玩家", "jugadores"),
        "connections" => s!("connections", "连接数", "conexiones"),
        "uptime" => s!("uptime", "运行时长", "tiempo activo"),
        "hub" => s!("hub", "房间", "sala"),
        "link" => s!("link", "链路", "enlace"),
        "age" => s!("age", "存活", "edad"),
        "status" => s!("status", "状态", "estado"),
        "open" => s!("open", "可加入", "abierta"),
        "access" => s!("access", "访问", "acceso"),
        "public room" => s!("public", "公开", "pública"),
        "key room" => s!("key", "Key 房间", "sala con Key"),
        "phone room" => s!("phone only", "仅手机", "solo Android"),
        "waiting" => s!("waiting", "等待中", "esperando"),
        "no hubs" => s!("No hubs right now.", "当前没有房间。", "Ahora no hay salas."),
        "traffic" => s!("traffic", "流量", "tráfico"),
        "kick" => s!("Kick", "踢出", "Expulsar"),
        "key locked" => s!("key room:", "限定房间:", "sala de la clave:"),
        "key public" => s!("no -key: the public room is served (anyone with the default key can join).",
                           "未设置 -key：开放公共房间（用默认 key 的玩家都能加入）。",
                           "sin -key: sala pública (cualquiera con la clave por defecto)."),
        "live" => s!("live", "活跃", "activo"),
        "idle" => s!("idle ", "空闲 ", "inactivo "),
        "pair" => s!("pair", "连接", "par"),
        "players title" => s!("Players", "玩家", "Jugadores"),
        "no players" => s!("No players yet.", "还没有玩家。", "Aún no hay jugadores."),
        "all" => s!("all", "全部", "todo"),
        "room" => s!("Room", "房间", "Sala"),
        "key" => s!("Key", "Key", "Key"),
        "player" => s!("player", "玩家", "jugador"),
        "refresh" => s!("This page refreshes every 5 seconds.", "每 5 秒自动刷新。", "Se actualiza cada 5 segundos."),
        "admin" => s!("admin", "管理", "admin"),
        "api" => s!("api", "接口", "api"),
        "language" => s!("language", "语言", "idioma"),
        "close" => s!("Close", "关闭", "Cerrar"),
        "closed" => s!("closed", "已关闭", "cerrada"),
        "not found" => s!("not found", "未找到", "no encontrada"),
        "closing hint" => s!(
            "Closing a hub ends its host's link; the players in it are dropped.",
            "关闭房间会结束主机的链路，房内玩家会被断开。",
            "Cerrar una sala termina el enlace del anfitrión y desconecta a sus jugadores."
        ),
        "token required" => s!("Admin token required.", "需要管理员令牌。", "Se necesita el token de administración."),
        "open admin" => s!("Open", "进入", "Abrir"),
        "ips" => s!("IPs", "IP 数", "IPs"),
        "banned" => s!("banned", "已封禁", "bloqueadas"),
        "ban" => s!("Ban", "封禁", "Bloquear"),
        "unban" => s!("Unban", "解封", "Desbloquear"),
        "ban ip" => s!("Ban an IP address", "封禁 IP 地址", "Bloquear una dirección IP"),        "settings" => s!("Live settings", "热设置", "Ajustes en vivo"),
        "min protocol" => s!("Oldest link protocol", "最低链路协议", "Protocolo de enlace mínimo"),
        "separate phones" => s!("Android public room separate", "Android 公共房间独立", "Sala pública Android separada"),
        "verbose" => s!("Verbose logs", "详细日志", "Registros detallados"),
        "max connections" => s!("Max connections", "最大连接数", "Conexiones máximas"),
        "max connections per ip" => s!("Max connections / IP", "每 IP 最大连接", "Conexiones máximas / IP"),
        "max hubs" => s!("Max hubs", "最大房间数", "Salas máximas"),
        "max hubs per ip" => s!("Max hubs / IP", "每 IP 最大房间", "Salas máximas / IP"),
        "list max" => s!("Hubs per search", "每次搜索房间数", "Salas por búsqueda"),
        "requests rate" => s!("Requests / second", "每秒请求数", "Solicitudes / segundo"),
        "request burst" => s!("Request burst", "请求突发", "Ráfaga de solicitudes"),
        "bandwidth" => s!("Bandwidth (bytes/s)", "带宽（字节/秒）", "Ancho de banda (bytes/s)"),
        "save" => s!("Save", "保存", "Guardar"),
        "applied" => s!("Settings applied.", "设置已应用。", "Ajustes aplicados."),
        "invalid settings" => s!("Invalid settings.", "设置无效。", "Ajustes no válidos."),

        "no bans" => s!("No banned addresses.", "没有封禁地址。", "No hay direcciones bloqueadas."),
        "log" => s!("log", "日志", "registro"),
        "hubs page" => s!("hubs", "房间", "salas"),
        "log hint" => s!(
            "The last 300 admin and hub events.",
            "最近 300 条管理与房间事件。",
            "Los últimos 300 eventos de administración y salas."
        ),
        "banned note" => s!("Banned %s.", "已封禁 %s。", "%s bloqueada."),
        _ => s!("", "", ""),
    };
    match l {
        Lang::En => en,
        Lang::Zh => zh,
        Lang::Es => es,
    }
}

pub struct Web {
    listener: TcpListener,
    shared: Arc<Mutex<Shared>>,
    token: String,
    started: Instant,
}

impl Web {
    pub fn bind(addr: &str, shared: Arc<Mutex<Shared>>, token: &str) -> io::Result<Option<Web>> {
        let a = addr.trim();
        if a.is_empty() || a == "off" {
            return Ok(None);
        }
        let bind = if a.starts_with(':') { format!("0.0.0.0{a}") } else { a.to_string() };
        let listener = TcpListener::bind(&bind)?;
        let token = if token.is_empty() {
            format!("{:016x}", rand_u64())
        } else {
            token.to_string()
        };
        Ok(Some(Web { listener, shared, token, started: Instant::now() }))
    }

    pub fn addr(&self) -> io::Result<std::net::SocketAddr> {
        self.listener.local_addr()
    }

    pub fn token(&self) -> &str {
        &self.token
    }

    pub fn serve(self: Arc<Self>) -> io::Result<()> {
        for c in self.listener.incoming() {
            match c {
                Ok(c) => {
                    let me = Arc::clone(&self);
                    thread::spawn(move || me.handle(c));
                }
                Err(_) => thread::sleep(std::time::Duration::from_millis(50)),
            }
        }
        Ok(())
    }

    fn handle(&self, mut c: TcpStream) {
        let _ = c.set_read_timeout(Some(std::time::Duration::from_secs(5)));
        let clone = match c.try_clone() {
            Ok(s) => s,
            Err(_) => return,
        };
        let mut r = BufReader::new(clone);
        let mut line = String::new();
        if r.read_line(&mut line).is_err() {
            return;
        }
        let mut parts = line.split_whitespace();
        let (method, path) = (parts.next().unwrap_or(""), parts.next().unwrap_or("/"));
        let mut len = 0usize;
        let mut cookie = String::new();
        loop {
            let mut h = String::new();
            if r.read_line(&mut h).is_err() || h.trim().is_empty() {
                break;
            }
            let low = h.to_ascii_lowercase();
            if let Some(v) = low.strip_prefix("content-length:") {
                len = v.trim().parse().unwrap_or(0);
            }
            if let Some(v) = low.strip_prefix("cookie:") {
                cookie = v.trim().to_string();
            }
        }
        let mut body = vec![0u8; len.min(4096)];
        if len > 0 {
            let _ = r.read_exact(&mut body);
        }
        let (path, query) = match path.split_once('?') {
            Some((p, q)) => (p, q),
            None => (path, ""),
        };
        let q = |k: &str| kv(query, k).or_else(|| kv(&cookie, k));
        let lang_q = q("lang").filter(|v| Lang::of(v).is_some());
        let lang = lang_q.as_deref().and_then(Lang::of).unwrap_or_else(|| {
            q("lang").as_deref().and_then(Lang::of).unwrap_or(Lang::En)
        });
        let token_ok = q("token").as_deref() == Some(self.token.as_str());

        if method == "GET" {
            if let Some((status, ctype, text)) = self.custom(path, token_ok) {
                let _ = write!(
                    c,
                    "HTTP/1.1 {} OK\r\nContent-Type: {}\r\nContent-Length: {}\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n{}",
                    status, ctype, text.len(), text
                );
                let _ = c.flush();
                return;
            }
        }
        let reply: (u16, &'static str, String) = match (method, path) {
            ("GET", "/") => (200, "text/html; charset=utf-8", self.public_page(lang)),
            ("GET", "/api/hubs") => (200, "application/json; charset=utf-8", self.api()),
            ("GET", "/admin") => {
                if token_ok {
                    (200, "text/html; charset=utf-8", self.admin_page(lang))
                } else {
                    (401, "text/html; charset=utf-8", layout(lang, "admin", &login(lang), true))
                }
            }
            ("GET", "/admin/log") => {
                if token_ok {
                    let only = q("only").unwrap_or_default();
                    (200, "text/html; charset=utf-8", self.log_page(lang, &only))
                } else {
                    (401, "text/html; charset=utf-8", layout(lang, "admin", &login(lang), true))
                }
            }
            ("POST", "/admin/settings") => {
                if token_ok {
                    let cur = self.shared.lock().unwrap().live();
                    let msg = match parse_live(&body_str(&body), &cur) {
                        Ok(next) => {
                            let n = self.shared.lock().unwrap().apply_live(next);
                            format!("<p class=ok>{} ({} hub(s) closed)</p><p><a href=\"/admin?token={}\">← {}</a></p>",
                                    t(lang, "applied"), n, self.token, t(lang, "admin"))
                        }
                        Err(why) => format!("<p class=err>{}</p><p><code>{}</code></p><p><a href=\"/admin?token={}\">← {}</a></p>",
                                            t(lang, "invalid settings"), html(&why), self.token, t(lang, "admin")),
                    };
                    (200, "text/html; charset=utf-8", layout(lang, "admin", &msg, true))
                } else {
                    (401, "text/html; charset=utf-8", layout(lang, "admin", &login(lang), true))
                }
            }
            ("POST", "/admin/close") => {
                if token_ok {
                    let id = kv(&body_str(&body), "hub").and_then(|v| v.parse::<u64>().ok());
                    let ok = id.map(|i| self.shared.lock().unwrap().close_hub(i)).unwrap_or(false);
                    let msg = format!(
                        "<p class=ok>{}{}</p><p><a href=\"/admin?token={}\">← {}</a></p>",
                        if ok { t(lang, "hub") } else { t(lang, "not found") },
                        if ok { format!(" {:#x}", id.unwrap_or(0)) } else { String::new() },
                        self.token,
                        t(lang, "admin")
                    );
                    (200, "text/html; charset=utf-8", layout(lang, "admin", &msg, true))
                } else {
                    (401, "text/html; charset=utf-8", layout(lang, "admin", &login(lang), true))
                }
            }
            ("POST", "/admin/kick") => {
                if token_ok {
                    let pair = body_str(&body).split('&').find_map(|kv| kv.strip_prefix("pair=")).and_then(|v| v.parse::<u32>().ok());
                    if let Some(p) = pair {
                        self.shared.lock().unwrap().kick_pair(p);
                    }
                    (200, "text/html; charset=utf-8", self.admin_page(lang))
                } else {
                    (401, "text/html; charset=utf-8", layout(lang, "admin", &login(lang), true))
                }
            }
            ("POST", "/admin/ban") | ("POST", "/admin/unban") => {
                if token_ok {
                    let ip = kv(&body_str(&body), "ip").and_then(|v| v.parse::<IpAddr>().ok());
                    if let Some(ip) = ip {
                        let mut g = self.shared.lock().unwrap();
                        if path.ends_with("ban") {
                            g.ban(ip);
                        } else {
                            g.unban(ip);
                        }
                    }
                    (200, "text/html; charset=utf-8", self.admin_page(lang))
                } else {
                    (401, "text/html; charset=utf-8", layout(lang, "admin", &login(lang), true))
                }
            }
            _ => (404, "text/plain; charset=utf-8", "not found".into()),
        };
        let (status, ctype, text) = reply;
        // Remember the language from ?lang=, so links without it keep it.
        let cookie_hdr = match &lang_q {
            Some(c) => format!("Set-Cookie: lang={c}; Path=/; Max-Age=31536000; SameSite=Lax\r\n"),
            None => String::new(),
        };
        let _ = write!(
            c,
            "HTTP/1.1 {} {}\r\nContent-Type: {}\r\nContent-Length: {}\r\nCache-Control: no-store\r\n{}Connection: close\r\n\r\n{}",
            status,
            if status == 200 { "OK" } else { "ERR" },
            ctype,
            text.len(),
            cookie_hdr,
            text
        );
        let _ = c.flush();
    }

    fn view(&self) -> WebView {
        let g = self.shared.lock().unwrap();
        g.web_view(self.started.elapsed().as_secs())
    }

    /// A page or asset from `-web-dir`, if one is set and the file exists (`None`: use the built-in page).
    /// `/` -> `index.html`, `/admin` -> `admin.html`, `/admin/log` -> `log.html`, `/login` -> `login.html`;
    /// anything else under the directory is served as an asset (css/js/png/svg/...). `..` is refused.
    fn custom(&self, path: &str, token_ok: bool) -> Option<(u16, &'static str, String)> {
        let dir = self.shared.lock().unwrap().cfg.web_dir.clone();
        if dir.is_empty() {
            return None;
        }
        let name = match path {
            "/" => "index.html".to_string(),
            "/admin" | "/admin/" => {
                if !token_ok {
                    "login.html".to_string()
                } else {
                    "admin.html".to_string()
                }
            }
            "/admin/log" => {
                if !token_ok {
                    "login.html".to_string()
                } else {
                    "log.html".to_string()
                }
            }
            "/login" => "login.html".to_string(),
            p => {
                if p.contains("..") {
                    return None;
                }
                p.trim_start_matches('/').to_string()
            }
        };
        let full = std::path::Path::new(&dir).join(&name);
        let text = std::fs::read_to_string(&full).ok()?;
        let ctype = match full.extension().and_then(|e| e.to_str()).unwrap_or("") {
            "html" | "htm" => "text/html; charset=utf-8",
            "css" => "text/css; charset=utf-8",
            "js" => "application/javascript; charset=utf-8",
            "json" => "application/json; charset=utf-8",
            "svg" => "image/svg+xml",
            "png" => "image/png",
            "jpg" | "jpeg" => "image/jpeg",
            "ico" => "image/x-icon",
            "woff2" => "font/woff2",
            _ => "text/plain; charset=utf-8",
        };
        Some((200, ctype, text))
    }

    fn public_page(&self, l: Lang) -> String {
        let v = self.view();
        let mut rows = String::new();
        for (i, h) in v.hubs.iter().enumerate() {
            let public = h.room == [0u8; 4];
            let phone = h.room == proto::room_from_key("public-phone")[..4];
            let keyed = !public && !phone;
            rows.push_str(&format!(
                "<tr><td>{} {}</td><td>{}</td><td>{}</td><td>{}</td></tr>",
                t(l, "room"), i + 1,
                h.players,
                if keyed { format!("<span class=warn>🔒 {}</span>", t(l, "key room")) }
                else if phone { format!("<span class=warn>📱 {}</span>", t(l, "phone room")) }
                else { format!("<span class=ok>🌐 {}</span>", t(l, "public room")) },
                if h.players > 1 { format!("<span class=ok>{}</span>", t(l, "open")) } else { format!("<span class=warn>{}</span>", t(l, "waiting")) }
            ));
        }
        if rows.is_empty() {
            rows = format!("<tr><td colspan=4>{}</td></tr>", t(l, "no hubs"));
        }
        let body = format!(
            r#"<div class=stats>
<div class=stat><b>{}</b>{}</div><div class=stat><b>{}</b>{}</div>
<div class=stat><b>{}</b>{}</div><div class=stat><b>{}</b>{}</div>
<div class=stat><b>{}</b>{}</div></div>
<table><tr><th>{}</th><th>{}</th><th>{}</th><th>{}</th></tr>{rows}</table>
<p style="color:#9a8a76">{}</p>"#,
            v.hubs.len(), t(l, "hubs"), v.players, t(l, "players"),
            v.conns, t(l, "connections"), uptime(v.uptime), t(l, "uptime"), traffic(v.bytes), t(l, "traffic"),
            t(l, "room"), t(l, "players"), t(l, "access"), t(l, "status"),
            t(l, "refresh")
        );
        layout(l, "hubs", &body, false)
    }

    fn admin_page(&self, l: Lang) -> String {
        let v = self.view();
        let (bans, live) = {
            let g = self.shared.lock().unwrap();
            (g.bans(), g.live())
        };
        let mut rows = String::new();
        for (i, h) in v.hubs.iter().enumerate() {
            rows.push_str(&format!(
                "<tr><td>{} {}</td><td>{}</td><td>{}</td><td>{}</td><td><code>{:02x}{:02x}{:02x}{:02x}</code></td>\
                 <td><form method=post action=\"/admin/close?token={}\" style=\"margin:0\">\
                 <input type=hidden name=hub value=\"{}\"><button>{}</button></form></td></tr>",
                t(l, "room"), i + 1, h.players, h.link_proto, secs(h.age),
                h.room[0], h.room[1], h.room[2], h.room[3], self.token, h.id, t(l, "close")
            ));
        }
        if rows.is_empty() {
            rows = format!("<tr><td colspan=4>{}</td></tr>", t(l, "no hubs"));
        }
        let mut ban_rows = String::new();
        for ip in &bans {
            ban_rows.push_str(&format!(
                "<tr><td><code>{ip}</code></td><td><form method=post action=\"/admin/unban?token={}\" style=\"margin:0\">\
                 <input type=hidden name=ip value=\"{ip}\"><button>{}</button></form></td></tr>",
                self.token, t(l, "unban")
            ));
        }
        if ban_rows.is_empty() {
            ban_rows = format!("<tr><td colspan=2>{}</td></tr>", t(l, "no bans"));
        }
        let ban_form = format!(
            "<form method=post action=\"/admin/ban?token={}\" style=\"margin-top:10px\">\
             <input name=ip placeholder=\"1.2.3.4\"> <button>{}</button></form>",
            self.token,
            t(l, "ban")
        );
        let log_link = format!("<p><a href=\"/admin/log?token={}\">{}</a></p>", self.token, t(l, "log"));        let settings = settings_form(self, l, &live);
        let mut player_rows = String::new();
        for (k, (hub, p)) in v.pairs.iter().enumerate() {
            let room = v.hubs.iter().position(|h| h.id == *hub).map(|i| i + 1);
            player_rows.push_str(&format!(
                "<tr><td>{} {}</td><td>{} {}</td><td>{}</td>\
                 <td>{}</td><td>{}</td><td>{}</td>\
                 <td><form method=post action=\"/admin/kick?token={}\" style=\"margin:0\">\
                 <input type=hidden name=pair value=\"{}\"><button>{}</button></form></td></tr>",
                t(l, "room"), room.map(|r| r.to_string()).unwrap_or_else(|| "-".into()),
                t(l, "player"), k + 1, p.ip,
                rate(p.down_bps), rate(p.up_bps),
                if p.idle < 3 { t(l, "live").to_string() } else { format!("{}{}s", t(l, "idle"), p.idle) },
                self.token, p.pair, t(l, "kick")
            ));
        }
        if player_rows.is_empty() {
            player_rows = format!("<tr><td colspan=7>{}</td></tr>", t(l, "no players"));
        }
        let body = format!(
            r#"<div class=stats>
<div class=stat><b>{}</b>{}</div><div class=stat><b>{}</b>{}</div>
<div class=stat><b>{}</b>{}</div><div class=stat><b>{}</b>{}</div>
<div class=stat><b>{}</b>{}</div><div class=stat><b>{}</b>{}</div></div>
<table><tr><th>{}</th><th>{}</th><th>{}</th><th>{}</th><th>{}</th><th></th></tr>{rows}</table>
<p style="color:#9a8a76">{}</p>
<p>{}</p>
<h2>{}</h2>
<table><tr><th>{}</th><th>{}</th><th>IP</th><th>↓</th><th>↑</th><th>{}</th><th></th></tr>{player_rows}</table>
<table><tr><th>{}</th><th></th></tr>{ban_rows}</table>
{ban_form}{log_link}{settings}"#,
            v.hubs.len(), t(l, "hubs"), v.players, t(l, "players"),
            v.conns, t(l, "connections"), v.ips, t(l, "ips"),
            uptime(v.uptime), t(l, "uptime"), traffic(v.bytes), t(l, "traffic"),
            t(l, "room"), t(l, "players"), t(l, "link"), t(l, "age"), t(l, "key"),
            t(l, "closing hint"),
            match v.key_room {
                Some(r) => format!("{} <code>{:02x}{:02x}{:02x}{:02x}</code>", t(l, "key locked"), r[0], r[1], r[2], r[3]),
                None => t(l, "key public").to_string(),
            },
            t(l, "players title"), t(l, "room"), t(l, "player"), t(l, "status"),
            t(l, "ban ip")
        );
        layout(l, "admin", &body, true)
    }

    fn log_page(&self, l: Lang, only: &str) -> String {
        let mut lines = {
            let g = self.shared.lock().unwrap();
            g.log_tail(300)
        };
        if !only.is_empty() {
            lines.retain(|l| l.starts_with(only));
        }
        let filter = |c: &str, label: &str| {
            format!(
                "<a href=\"/admin/log?token={}&only={}\"{}>{label}</a>",
                self.token,
                c,
                if only == c { " class=cur" } else { "" }
            )
        };
        let body = format!(
            "<p style=\"color:#9a8a76\">{}</p><p>{}</p><pre style=\"white-space:pre-wrap\">{}</pre>\
             <p><a href=\"/admin?token={}\">← {}</a></p>",
            t(l, "log hint"),
            format!("{} · {} · {}", filter("", t(l, "all")), filter("hub", t(l, "hubs")), filter("admin", t(l, "admin"))),
            lines.join("\n"),
            self.token,
            t(l, "admin")
        );
        layout(l, "log", &body, true)
    }

    fn api(&self) -> String {
        let v = self.view();
        let mut hubs = Vec::new();
        for h in &v.hubs {
            hubs.push(format!(
                r#"{{"id":"{:#x}","players":{},"link":{},"age":{},"locked":{},"access":"{}"}}"#,
                h.id, h.players, h.link_proto, h.age, h.room != [0u8; 4],
                if h.room == [0u8; 4] { "public" } else if h.room == proto::room_from_key("public-phone")[..4] { "phone" } else { "key" }
            ));
        }
        format!(
            r#"{{"uptime":{},"conns":{},"players":{},"hubs":[{}]}}"#,
            v.uptime,
            v.conns,
            v.players,
            hubs.join(",")
        )
    }
}

fn login(l: Lang) -> String {
    format!(
        r#"<p>{}</p><form method=get action=/admin><input name=token placeholder="token" autofocus>
<button>{}</button></form>"#,
        t(l, "token required"),
        t(l, "open admin")
    )
}

fn layout(l: Lang, title: &str, body: &str, admin: bool) -> String {
    layout_at(l, title, body, admin, if admin { "/admin" } else { "/" })
}

/// The names of the languages, in their own language.
fn lang_name(c: Lang) -> &'static str {
    match c {
        Lang::En => "English",
        Lang::Zh => "中文",
        Lang::Es => "Español",
    }
}

fn layout_at(l: Lang, title: &str, body: &str, admin: bool, here: &str) -> String {
    // A dropdown (a <details> list: click to open, click an entry to switch), and the choice is kept in
    // a cookie so every page keeps it.
    let mut entries = String::new();
    for c in [Lang::En, Lang::Zh, Lang::Es] {
        entries.push_str(&format!(
            "<a href=\"{here}?lang={}\"{}>{}</a>",
            c.code(),
            if c == l { " class=cur" } else { "" },
            lang_name(c)
        ));
    }
    let lang_links = format!(
        "<details class=lang><summary>🌐 {}</summary><div class=menu>{entries}</div></details>",
        lang_name(l)
    );
    format!(
        r#"<!doctype html><html lang="{}"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>MHGU hub · {}</title><style>{STYLE}</style></head>
<body><header><h1>MHGU hub server · {}</h1>
<p style="margin:6px 0 0;display:flex;gap:14px;align-items:center"><a href="/">{}</a> · <a href="/admin">{}</a> · <a href="/api/hubs">{}</a>
<span style="flex:1"></span>{lang_links}</p></header>
<main>{body}</main>
{}</body></html>"#,
        l.code(),
        t(l, title),
        t(l, title),
        t(l, "hubs page"),
        t(l, "admin"),
        t(l, "api"),
        if admin { "" } else { r#"<script>setTimeout(()=>location.reload(),5000)</script>"# }
    )
}

const STYLE: &str = r#"
:root{color-scheme:dark}
body{font:14px/1.55 system-ui,-apple-system,"Segoe UI","Noto Sans CJK SC",sans-serif;margin:0;background:#14100c;color:#e8ded0}
header{padding:18px 24px;background:#241c14;border-bottom:1px solid #4a3a28}
h1{font-size:18px;margin:0}
h2{font-size:15px;margin:26px 0 8px;color:#d8c6a8}
main{padding:24px;max-width:1100px;margin:auto}
table{width:100%;border-collapse:collapse}
th,td{text-align:left;padding:8px 10px;border-bottom:1px solid #3a2e22}
th{color:#b9a68e;font-weight:600}
code{color:#f0c674}
pre{background:#1c1610;border:1px solid #3a2e22;border-radius:8px;padding:12px;max-height:420px;overflow:auto}
.ok{color:#8bc34a}.warn{color:#e5c07b}.stats{display:flex;gap:18px;flex-wrap:wrap;margin:0 0 18px}
.stat{background:#241c14;border:1px solid #4a3a28;border-radius:8px;padding:10px 16px;min-width:96px}
.stat b{display:block;font-size:20px}
button{background:#5a3f22;color:#fff;border:0;border-radius:6px;padding:5px 12px;cursor:pointer}
button:hover{background:#7a5630}
a{color:#f0c674}
input{padding:6px 10px;border-radius:6px;border:1px solid #4a3a28;background:#1c1610;color:#e8ded0}
.settings{display:grid;grid-template-columns:repeat(auto-fit,minmax(280px,1fr));gap:10px;margin:10px 0}
.settings label{display:flex;justify-content:space-between;gap:10px;align-items:center;background:#1c1610;border:1px solid #3a2e22;border-radius:8px;padding:8px 12px}
.settings input[type=number]{width:96px}
.settings button{justify-self:start}
details.lang{position:relative}
details.lang summary{cursor:pointer;list-style:none;padding:4px 10px;border:1px solid #4a3a28;border-radius:6px;background:#241c14}
details.lang summary::-webkit-details-marker{display:none}
details.lang .menu{position:absolute;right:0;top:110%;background:#241c14;border:1px solid #4a3a28;border-radius:8px;min-width:140px;z-index:9}
details.lang .menu a{display:block;padding:6px 12px;white-space:nowrap}
details.lang .menu a:hover,details.lang .menu a.cur{background:#3a2e22}
"#;

fn html(s: &str) -> String {
    s.replace('&', "&amp;").replace('<', "&lt;").replace('>', "&gt;").replace('"', "&quot;")
}

/// The live-settings form on /admin. Every field is sent on submit (unchecked checkboxes are absent).
fn settings_form(w: &Web, l: Lang, v: &Live) -> String {
    let check = |on: bool| if on { " checked" } else { "" };
    format!(
        "<h2>{}</h2>\
         <form method=post action=\"/admin/settings?token={}\" class=settings>\
         <label>{} <input type=number name=min_proto min=0 max=65535 value=\"{}\"></label>\
         <label><input type=checkbox name=separate_phones{}> {}</label>\
         <label><input type=checkbox name=verbose{}> {}</label>\
         <label>{} <input type=number name=max_conns min=1 value=\"{}\"></label>\
         <label>{} <input type=number name=max_conns_per_ip min=1 value=\"{}\"></label>\
         <label>{} <input type=number name=max_hubs min=1 value=\"{}\"></label>\
         <label>{} <input type=number name=max_hubs_per_ip min=1 value=\"{}\"></label>\
         <label>{} <input type=number name=list_max min=1 max=64 value=\"{}\"></label>\
         <label>{} <input type=number name=req_rate min=0.01 step=0.01 value=\"{}\"></label>\
         <label>{} <input type=number name=req_burst min=1 step=1 value=\"{}\"></label>\
         <label>{} <input type=number name=bandwidth min=1 step=1 value=\"{}\"></label>\
         <button>{}</button></form>",
        t(l, "settings"), w.token,
        t(l, "min protocol"), v.min_proto,
        check(v.separate_phones), t(l, "separate phones"),
        check(v.verbose), t(l, "verbose"),
        t(l, "max connections"), v.max_conns,
        t(l, "max connections per ip"), v.max_conns_per_ip,
        t(l, "max hubs"), v.max_hubs,
        t(l, "max hubs per ip"), v.max_hubs_per_ip,
        t(l, "list max"), v.list_max,
        t(l, "requests rate"), v.req_rate,
        t(l, "request burst"), v.req_burst,
        t(l, "bandwidth"), v.bandwidth,
        t(l, "save")
    )
}

/// Reads the admin form. Missing fields keep `cur`; bad values are refused (the server keeps its settings).
fn parse_live(body: &str, cur: &Live) -> Result<Live, String> {
    let num = |key: &str| kv(body, key);
    let parse = |key: &str, default: f64| -> Result<f64, String> {
        match num(key) {
            None => Ok(default),
            Some(v) => v.parse::<f64>().map_err(|_| format!("{key}: not a number")),
        }
    };
    let uint = |key: &str, default: usize| -> Result<usize, String> {
        match num(key) {
            None => Ok(default),
            Some(v) => v.parse::<usize>().map_err(|_| format!("{key}: not a number")),
        }
    };
    let min_proto = uint("min_proto", cur.min_proto as usize)?;
    if min_proto > 65535 { return Err("min_proto: at most 65535".into()); }
    let max_conns = uint("max_conns", cur.max_conns)?;
    let max_conns_per_ip = uint("max_conns_per_ip", cur.max_conns_per_ip)?;
    let max_hubs = uint("max_hubs", cur.max_hubs)?;
    let max_hubs_per_ip = uint("max_hubs_per_ip", cur.max_hubs_per_ip)?;
    let list_max = uint("list_max", cur.list_max)?;
    let req_rate = parse("req_rate", cur.req_rate)?;
    let req_burst = parse("req_burst", cur.req_burst)?;
    let bandwidth = parse("bandwidth", cur.bandwidth)?;
    if max_conns == 0 || max_conns_per_ip == 0 || max_hubs == 0 || max_hubs_per_ip == 0 || list_max == 0 || list_max > 64 {
        return Err("connection/hub/list limits must be positive (list at most 64)".into());
    }
    if !(req_rate.is_finite() && req_rate > 0.0 && req_burst.is_finite() && req_burst >= 1.0) {
        return Err("request rate must be > 0 and burst >= 1".into());
    }
    if !(bandwidth.is_finite() && bandwidth >= 1.0) {
        return Err("bandwidth must be at least 1 byte/s".into());
    }
    Ok(Live {
        min_proto: min_proto as u16,
        separate_phones: num("separate_phones").is_some(),
        verbose: num("verbose").is_some(),
        max_conns, max_conns_per_ip, max_hubs, max_hubs_per_ip, list_max, req_rate, req_burst, bandwidth,
    })
}

fn kv(s: &str, key: &str) -> Option<String> {
    for part in s.split(&['&', ';', ' '][..]) {
        if let Some(v) = part.trim().strip_prefix(&format!("{key}=")) {
            return Some(url_decode(v));
        }
    }
    None
}

fn body_str(body: &[u8]) -> String {
    String::from_utf8_lossy(body).into_owned()
}

fn url_decode(s: &str) -> String {
    let b = s.as_bytes();
    let mut out = Vec::with_capacity(b.len());
    let mut i = 0;
    while i < b.len() {
        if b[i] == b'%' && i + 2 < b.len() {
            if let Ok(v) = u8::from_str_radix(&s[i + 1..i + 3], 16) {
                out.push(v);
                i += 3;
                continue;
            }
        }
        out.push(if b[i] == b'+' { b' ' } else { b[i] });
        i += 1;
    }
    String::from_utf8_lossy(&out).into_owned()
}

fn rand_u64() -> u64 {
    use std::time::{SystemTime, UNIX_EPOCH};
    let n = SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_nanos() as u64).unwrap_or(1);
    (n ^ (n << 17) ^ (n >> 7)).wrapping_mul(0x9E37_79B9_7F4A_7C15)
}

fn secs(s: u64) -> String {
    if s < 60 { format!("{s}s") } else if s < 3600 { format!("{}m", s / 60) } else { format!("{}h", s / 3600) }
}

fn rate(bps: u64) -> String {
    if bps < 1024 {
        format!("{} B/s", bps)
    } else if bps < 1 << 20 {
        format!("{:.1} KB/s", bps as f64 / 1024.0)
    } else {
        format!("{:.1} MB/s", bps as f64 / (1u64 << 20) as f64)
    }
}

fn traffic(b: u64) -> String {
    if b < 1 << 20 {
        format!("{} KB", b / 1024)
    } else if b < 1 << 30 {
        format!("{} MB", b >> 20)
    } else {
        format!("{:.1} GB", b as f64 / (1u64 << 30) as f64)
    }
}

fn uptime(s: u64) -> String {
    format!("{}h {:02}m", s / 3600, (s % 3600) / 60)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::config::{Config, Live};

    #[test]
    fn live_settings_parse_and_validate() {
        let cur = Live::from_config(&Config::default());
        let v = parse_live("min_proto=2&max_hubs=10&separate_phones=on&verbose=on", &cur).unwrap();
        assert_eq!(v.min_proto, 2);
        assert_eq!(v.max_hubs, 10);
        assert!(v.separate_phones && v.verbose);
        assert_eq!(v.bandwidth, cur.bandwidth);
        assert!(parse_live("min_proto=99999", &cur).is_err());
        assert!(parse_live("max_conns=0", &cur).is_err());
        assert!(parse_live("list_max=65", &cur).is_err());
        assert!(parse_live("bandwidth=0", &cur).is_err());
        assert!(parse_live("req_burst=0", &cur).is_err());
    }
}
