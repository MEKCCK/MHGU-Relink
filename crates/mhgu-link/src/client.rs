use std::io::{self, Write};
use std::net::{SocketAddr, TcpStream};
use std::time::Duration;

use mhgu_protocol as proto;

use crate::hello::Hello;
use crate::vpacket::VPacket;

const MAX_FRAME: u32 = proto::LINK_MAX_FRAME as u32;

/// The host's answer to our `Hello`.
#[derive(Debug)]
pub enum Reply {
    /// A scan answer: the host's NetworkInfo, without joining.
    Info(Vec<u8>),
    /// Accepted: our node slot, the UDP token, and the network's NetworkInfo.
    Welcome { node: u8, token: u32, info: Vec<u8> },
    /// Refused with an `nn::ldn` result code.
    Refuse(u32),
}

/// A frame read from the link after joining.
#[derive(Debug)]
pub enum Event {
    Packet { vpacket: VPacket, data: Vec<u8> },
    Info(Vec<u8>),
    Destroy(u32),
    Aux(Vec<u8>),
}

/// A station's connection to a host.
pub struct Client {
    stream: TcpStream,
}

impl Client {
    /// Connects to a host and sends our `Hello`.
    pub fn connect(addr: SocketAddr, hello: &Hello) -> io::Result<Client> {
        let stream = TcpStream::connect_timeout(&addr, Duration::from_secs(5))?;
        stream.set_nodelay(true)?;
        let mut c = Client { stream };
        c.send(proto::F_HELLO, &[&hello.to_bytes()])?;
        Ok(c)
    }

    /// A second handle on the connection, for a writer on another thread (the reader keeps the
    /// `Client`): frames written to it are the ones `send_packet`/`send_aux` write.
    pub fn stream_clone(&self) -> io::Result<TcpStream> {
        self.stream.try_clone()
    }

    /// A `Client` over a stream that is already spliced to a host (a hub `attach`/`join`): sends our
    /// `Hello` first, like `connect`.
    pub fn from_stream(stream: TcpStream, hello: &Hello) -> io::Result<Client> {
        stream.set_nodelay(true)?;
        let mut c = Client { stream };
        c.send(proto::F_HELLO, &[&hello.to_bytes()])?;
        Ok(c)
    }

    /// Reads the host's answer to the `Hello`.
    pub fn reply(&mut self) -> io::Result<Reply> {
        let (t, p) = self.read()?;
        match t {
            proto::F_INFO => Ok(Reply::Info(p)),
            proto::F_WELCOME if p.len() >= 8 => {
                let token = u32::from_le_bytes(p[4..8].try_into().unwrap());
                Ok(Reply::Welcome {
                    node: p[0],
                    token,
                    info: p[8..].to_vec(),
                })
            }
            proto::F_REFUSE if p.len() >= 4 => Ok(Reply::Refuse(u32::from_le_bytes(
                p[..4].try_into().unwrap(),
            ))),
            other => Err(io::Error::new(
                io::ErrorKind::InvalidData,
                format!("unexpected reply frame {other}"),
            )),
        }
    }

    /// Sends a game packet (a `VPacket` header followed by its data).
    pub fn send_packet(&mut self, p: &VPacket, data: &[u8]) -> io::Result<()> {
        self.send(proto::F_PACKET, &[&p.to_bytes(), data])
    }

    /// Sends a port-private message (emsync / hpscale) to the others.
    pub fn send_aux(&mut self, data: &[u8]) -> io::Result<()> {
        self.send(proto::F_AUX, &[data])
    }

    /// Reads the next frame after joining.
    pub fn recv(&mut self) -> io::Result<Event> {
        let (t, p) = self.read()?;
        match t {
            proto::F_PACKET => {
                let vpacket = VPacket::parse(&p)
                    .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "short packet"))?;
                Ok(Event::Packet {
                    vpacket,
                    data: p[proto::VPACKET_SIZE..].to_vec(),
                })
            }
            proto::F_INFO => Ok(Event::Info(p)),
            proto::F_DESTROY if p.len() >= 4 => {
                Ok(Event::Destroy(u32::from_le_bytes(p[..4].try_into().unwrap())))
            }
            proto::F_AUX => Ok(Event::Aux(p)),
            other => Err(io::Error::new(
                io::ErrorKind::InvalidData,
                format!("unexpected frame {other}"),
            )),
        }
    }

    pub fn set_read_timeout(&mut self, d: Option<Duration>) -> io::Result<()> {
        self.stream.set_read_timeout(d)
    }

    fn send(&mut self, t: u8, payloads: &[&[u8]]) -> io::Result<()> {
        self.stream.write_all(&proto::frame(t, payloads))
    }

    fn read(&mut self) -> io::Result<(u8, Vec<u8>)> {
        proto::read_frame(&mut self.stream, MAX_FRAME)
    }
}
