use serde::{Deserialize, Serialize};

/// Information about a single proxy server instance, returned by the "status" command.
#[derive(Debug, Clone, Default, Deserialize, Serialize)]
pub struct ServerInfo {
    #[serde(default)]
    pub index: usize,
    pub listen_port: u16,
    pub remote_addr: String,
    #[serde(default)]
    pub hostname_tag: String,
    #[serde(default)]
    pub max_clients: i64,
    #[serde(default)]
    pub session_timeout: i64,
    #[serde(default)]
    pub query_timeout: i64,
    #[serde(default)]
    pub max_new_per_sec: i64,
    #[serde(default)]
    pub max_query_sessions: i64,
    #[serde(default)]
    pub active_sessions: i64,
    #[serde(default)]
    pub query_sessions: i64,
}

/// A single client session, returned by the "sessions" command.
#[derive(Debug, Clone, Default, Deserialize, Serialize)]
pub struct SessionInfo {
    pub client: String,
    #[serde(default)]
    pub is_query: bool,
    #[serde(default)]
    pub idle_secs: i64,
    #[serde(default)]
    pub pkts_to_server: i64,
    #[serde(default)]
    pub pkts_to_client: i64,
    #[serde(default)]
    pub bytes_to_server: i64,
    #[serde(default)]
    pub bytes_to_client: i64,
}

/// Parsed response from the "status" command.
#[derive(Debug, Clone, Deserialize)]
pub struct StatusData {
    #[serde(default)]
    pub servers: Vec<ServerInfo>,
}

/// Parsed response from the "sessions" command.
#[derive(Debug, Clone, Deserialize)]
pub struct SessionsData {
    #[serde(default)]
    pub server: usize,
    #[serde(default)]
    pub sessions: Vec<SessionInfo>,
}
