use std::fs;
use std::path::PathBuf;

use crate::proxy_entry::ProxyEntry;

/// Return the config directory path: <config_dir>/urt-mgmt/
fn config_dir() -> Option<PathBuf> {
    dirs::config_dir().map(|d| d.join("urt-mgmt"))
}

/// Return the path to the saved servers JSON file.
fn servers_path() -> Option<PathBuf> {
    config_dir().map(|d| d.join("servers.json"))
}

/// Load saved proxy entries from disk. Returns an empty vec on any error.
pub fn load_proxies() -> Vec<ProxyEntry> {
    let Some(path) = servers_path() else {
        return Vec::new();
    };
    let Ok(data) = fs::read_to_string(&path) else {
        return Vec::new();
    };
    serde_json::from_str(&data).unwrap_or_default()
}

/// Save proxy entries to disk. Silently ignores errors.
pub fn save_proxies(entries: &[ProxyEntry]) {
    let Some(dir) = config_dir() else { return };
    let Some(path) = servers_path() else { return };
    let _ = fs::create_dir_all(&dir);
    if let Ok(data) = serde_json::to_string_pretty(entries) {
        let _ = fs::write(&path, data);
    }
}

/// Return the next unique ID based on existing entries.
pub fn next_id(entries: &[ProxyEntry]) -> u64 {
    entries.iter().map(|e| e.id).max().unwrap_or(0) + 1
}
