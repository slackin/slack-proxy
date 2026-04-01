use serde::{Deserialize, Serialize};

/// A saved proxy server connection entry.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ProxyEntry {
    pub id: u64,
    pub name: String,
    pub host: String,
    pub port: u16,
    pub api_key: String,
    pub auto_connect: bool,
}

impl ProxyEntry {
    pub fn display_addr(&self) -> String {
        format!("{}:{}", self.host, self.port)
    }
}
