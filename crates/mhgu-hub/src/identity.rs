//! The hub server's Ed25519 identity: the key behind `ed25519:<fingerprint>`.
//!
//! The seed is a 32-byte file (`-identity`, default `mhgu-hub.identity`), made on first start. The id is
//! the first 16 characters of the key fingerprint in `mhgu-protocol`'s base32 alphabet, so two operators
//! can compare it by eye; the public key is published in `/.well-known/mhgu-federation` and used by the
//! federation handshake.

use std::io;
use std::sync::Arc;

use ed25519_dalek::{Signature, Signer, SigningKey, Verifier, VerifyingKey};

use mhgu_protocol as proto;

/// A server identity and its signing key.
pub struct Identity {
    signing: SigningKey,
    /// `ed25519:` followed by 16 fingerprint characters.
    pub id: String,
    pub public: [u8; 32],
}

impl Identity {
    /// Loads the seed from `path`, or makes one there the first time. Empty `path`: a fresh ephemeral
    /// identity (tests, or a quick run where pinning does not matter).
    pub fn load(path: &str) -> io::Result<Identity> {
        let mut seed = [0u8; 32];
        if path.is_empty() {
            getrandom::fill(&mut seed).expect("OS randomness unavailable");
        } else {
            match std::fs::read(path) {
                Ok(v) if v.len() == 32 => seed.copy_from_slice(&v),
                Ok(_) => {
                    return Err(io::Error::new(
                        io::ErrorKind::InvalidData,
                        format!("{path}: an identity file must be exactly 32 bytes"),
                    ))
                }
                Err(e) if e.kind() == io::ErrorKind::NotFound => {
                    getrandom::fill(&mut seed).expect("OS randomness unavailable");
                    std::fs::write(path, seed)?;
                    #[cfg(unix)]
                    {
                        use std::os::unix::fs::PermissionsExt;
                        let _ = std::fs::set_permissions(path, std::fs::Permissions::from_mode(0o600));
                    }
                }
                Err(e) => return Err(e),
            }
        }
        let signing = SigningKey::from_bytes(&seed);
        let public = signing.verifying_key().to_bytes();
        let id = identity_id(&public);
        Ok(Identity { signing, id, public })
    }

    /// Signs the concatenation of `parts`.
    pub fn sign(&self, parts: &[&[u8]]) -> [u8; 64] {
        let mut m = Vec::new();
        for p in parts {
            m.extend_from_slice(p);
        }
        self.signing.sign(&m).to_bytes()
    }

    /// Verifies a signature over the concatenation of `parts`.
    pub fn verify(public: &[u8; 32], parts: &[&[u8]], sig: &[u8; 64]) -> bool {
        let Ok(vk) = VerifyingKey::from_bytes(public) else { return false };
        let mut m = Vec::new();
        for p in parts {
            m.extend_from_slice(p);
        }
        vk.verify(&m, &Signature::from_bytes(sig)).is_ok()
    }
}

/// `ed25519:` followed by 16 characters of the key fingerprint.
pub fn identity_id(public: &[u8; 32]) -> String {
    let d = proto::sha256::digest(&[public]);
    let mut s = String::with_capacity(32);
    s.push_str(proto::FED_ID_PREFIX);
    for i in 0..16 {
        s.push(proto::CODE_ALPHABET[d[i] as usize % 32] as char);
    }
    s
}

/// Lowercase hex (well-known's public key field).
pub fn hex(b: &[u8]) -> String {
    let mut s = String::with_capacity(b.len() * 2);
    for x in b {
        s.push_str(&format!("{x:02x}"));
    }
    s
}

/// Shared handle used by the server and its federation peers.
pub type SharedIdentity = Arc<Identity>;
