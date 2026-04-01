use std::collections::{HashMap, HashSet, VecDeque};
use std::sync::mpsc;
use std::time::Instant;

use eframe::egui;
use serde_json::Value;

use crate::config;
use crate::protocol::{ServerInfo, SessionInfo, SessionsData, StatusData};
use crate::proxy_entry::ProxyEntry;
use crate::ui;
use crate::worker::{self, Command, Response};

const MAX_LOG_ENTRIES: usize = 500;
const REFRESH_INTERVAL_SECS: f32 = 2.0;

/// Per-proxy connection and data state.
pub struct ProxyState {
    pub connected: bool,
    pub server_data: Vec<ServerInfo>,
    pub session_data: HashMap<usize, Vec<SessionInfo>>,
    pub active_tab: usize,
    pub tune_values: Vec<HashMap<String, String>>,
    pub selected_sessions: Vec<HashSet<String>>,
    pub confirm_kick_all: Option<usize>,
    pub confirm_remove: Option<usize>,
    pub show_add_server: bool,
    pub add_listen_port: String,
    pub add_remote_host: String,
    pub add_remote_port: String,
    pub add_max_clients: String,
    pub add_timeout: String,
    pub add_hostname_tag: String,
    pub log_entries: VecDeque<(String, String)>,
    pub last_refresh: Option<Instant>,
}

impl ProxyState {
    pub fn new() -> Self {
        Self {
            connected: false,
            server_data: Vec::new(),
            session_data: HashMap::new(),
            active_tab: 0,
            tune_values: Vec::new(),
            selected_sessions: Vec::new(),
            confirm_kick_all: None,
            confirm_remove: None,
            show_add_server: false,
            add_listen_port: "27960".to_string(),
            add_remote_host: String::new(),
            add_remote_port: "27960".to_string(),
            add_max_clients: "20".to_string(),
            add_timeout: "30".to_string(),
            add_hostname_tag: String::new(),
            log_entries: VecDeque::new(),
            last_refresh: None,
        }
    }

    pub fn log(&mut self, msg: &str) {
        let ts = chrono::Local::now().format("[%H:%M:%S]").to_string();
        self.log_entries.push_back((ts, msg.to_string()));
        if self.log_entries.len() > MAX_LOG_ENTRIES {
            self.log_entries.pop_front();
        }
    }
}

pub struct App {
    // Saved proxy entries
    pub proxy_entries: Vec<ProxyEntry>,
    pub active_proxy_id: Option<u64>,
    pub proxy_states: HashMap<u64, ProxyState>,

    // Add/edit proxy form
    pub show_add_proxy: bool,
    pub editing_proxy_id: Option<u64>,
    pub edit_name: String,
    pub edit_host: String,
    pub edit_port: String,
    pub edit_api_key: String,
    pub edit_auto_connect: bool,

    // Worker communication
    cmd_tx: mpsc::Sender<Command>,
    resp_rx: mpsc::Receiver<Response>,
}

impl App {
    pub fn new(cc: &eframe::CreationContext<'_>) -> Self {
        configure_style(&cc.egui_ctx);

        let (cmd_tx, resp_rx) = worker::spawn_worker();
        let proxy_entries = config::load_proxies();

        // Create state for each saved proxy
        let mut proxy_states = HashMap::new();
        for entry in &proxy_entries {
            proxy_states.insert(entry.id, ProxyState::new());
        }

        let active_proxy_id = proxy_entries.first().map(|e| e.id);

        let mut app = Self {
            proxy_entries,
            active_proxy_id,
            proxy_states,
            show_add_proxy: false,
            editing_proxy_id: None,
            edit_name: String::new(),
            edit_host: "127.0.0.1".to_string(),
            edit_port: "27961".to_string(),
            edit_api_key: String::new(),
            edit_auto_connect: false,
            cmd_tx,
            resp_rx,
        };

        // Auto-connect entries that have it enabled
        let auto_ids: Vec<(u64, String, u16, String)> = app
            .proxy_entries
            .iter()
            .filter(|e| e.auto_connect)
            .map(|e| (e.id, e.host.clone(), e.port, e.api_key.clone()))
            .collect();
        for (id, host, port, key) in auto_ids {
            if let Some(state) = app.proxy_states.get_mut(&id) {
                state.log(&format!("Auto-connecting to {}:{}...", host, port));
            }
            let _ = app.cmd_tx.send(Command::Connect {
                proxy_id: id,
                host,
                port,
                key,
            });
        }

        app
    }

    // --- Helpers for the active proxy ---

    pub fn active_state(&self) -> Option<&ProxyState> {
        self.active_proxy_id
            .and_then(|id| self.proxy_states.get(&id))
    }

    pub fn active_state_mut(&mut self) -> Option<&mut ProxyState> {
        self.active_proxy_id
            .and_then(|id| self.proxy_states.get_mut(&id))
    }

    pub fn active_entry(&self) -> Option<&ProxyEntry> {
        self.active_proxy_id
            .and_then(|id| self.proxy_entries.iter().find(|e| e.id == id))
    }

    // --- Connection ---

    pub fn do_connect(&mut self, proxy_id: u64) {
        let entry = match self.proxy_entries.iter().find(|e| e.id == proxy_id) {
            Some(e) => e.clone(),
            None => return,
        };

        if entry.host.is_empty() || entry.api_key.is_empty() {
            if let Some(state) = self.proxy_states.get_mut(&proxy_id) {
                state.log("Host and API Key are required.");
            }
            return;
        }

        if let Some(state) = self.proxy_states.get_mut(&proxy_id) {
            state.log(&format!("Connecting to {}:{}...", entry.host, entry.port));
        }
        let _ = self.cmd_tx.send(Command::Connect {
            proxy_id,
            host: entry.host,
            port: entry.port,
            key: entry.api_key,
        });
    }

    pub fn do_disconnect(&mut self, proxy_id: u64) {
        let _ = self.cmd_tx.send(Command::Disconnect { proxy_id });
    }

    // --- Send commands (scoped to a proxy) ---

    pub fn send_set(&mut self, proxy_id: u64, server: usize, key: &str, value: Value) {
        let payload = serde_json::json!({
            "cmd": "set",
            "server": server,
            "key": key,
            "value": value,
        });
        let _ = self.cmd_tx.send(Command::Send {
            proxy_id,
            cmd_name: "set".to_string(),
            payload,
        });
    }

    pub fn send_kick(&mut self, proxy_id: u64, server: usize, client: &str) {
        let payload = serde_json::json!({
            "cmd": "kick",
            "server": server,
            "client": client,
        });
        let _ = self.cmd_tx.send(Command::Send {
            proxy_id,
            cmd_name: "kick".to_string(),
            payload,
        });
        if let Some(state) = self.proxy_states.get_mut(&proxy_id) {
            state.log(&format!("Kicking {} on server #{}", client, server + 1));
        }
    }

    pub fn send_kick_all(&mut self, proxy_id: u64, server: usize) {
        let payload = serde_json::json!({
            "cmd": "kick_all",
            "server": server,
        });
        let _ = self.cmd_tx.send(Command::Send {
            proxy_id,
            cmd_name: "kick_all".to_string(),
            payload,
        });
        if let Some(state) = self.proxy_states.get_mut(&proxy_id) {
            state.log(&format!("Kick all on server #{}", server + 1));
        }
    }

    pub fn send_add_server(
        &mut self,
        proxy_id: u64,
        listen_port: u16,
        remote_host: &str,
        remote_port: u16,
        max_clients: i64,
        session_timeout: i64,
        hostname_tag: &str,
    ) {
        let mut payload = serde_json::json!({
            "cmd": "add_server",
            "listen_port": listen_port,
            "remote_host": remote_host,
            "remote_port": remote_port,
            "max_clients": max_clients,
            "session_timeout": session_timeout,
        });
        if !hostname_tag.is_empty() {
            payload["hostname_tag"] = serde_json::json!(hostname_tag);
        }
        let _ = self.cmd_tx.send(Command::Send {
            proxy_id,
            cmd_name: "add_server".to_string(),
            payload,
        });
        if let Some(state) = self.proxy_states.get_mut(&proxy_id) {
            state.log(&format!(
                "Adding server :{} -> {}:{}",
                listen_port, remote_host, remote_port
            ));
        }
    }

    pub fn send_remove_server(&mut self, proxy_id: u64, server: usize) {
        let payload = serde_json::json!({
            "cmd": "remove_server",
            "server": server,
        });
        let _ = self.cmd_tx.send(Command::Send {
            proxy_id,
            cmd_name: "remove_server".to_string(),
            payload,
        });
        if let Some(state) = self.proxy_states.get_mut(&proxy_id) {
            state.log(&format!("Removing server #{}", server + 1));
        }
    }

    // --- Internal helpers ---

    fn request_status(&self, proxy_id: u64) {
        let _ = self.cmd_tx.send(Command::Send {
            proxy_id,
            cmd_name: "status".to_string(),
            payload: serde_json::json!({ "cmd": "status" }),
        });
    }

    fn request_sessions(&self, proxy_id: u64, server: usize) {
        let _ = self.cmd_tx.send(Command::Send {
            proxy_id,
            cmd_name: "sessions".to_string(),
            payload: serde_json::json!({ "cmd": "sessions", "server": server }),
        });
    }

    fn poll_responses(&mut self) {
        while let Ok(resp) = self.resp_rx.try_recv() {
            match resp {
                Response::Connected { proxy_id, ok, msg } => {
                    if let Some(state) = self.proxy_states.get_mut(&proxy_id) {
                        if ok {
                            state.connected = true;
                            state.log(&msg);
                            state.last_refresh = Some(Instant::now());
                        } else {
                            state.connected = false;
                            state.log(&format!("Connection failed: {}", msg));
                        }
                    }
                    if ok {
                        self.request_status(proxy_id);
                    }
                }
                Response::Disconnected { proxy_id, msg } => {
                    if let Some(state) = self.proxy_states.get_mut(&proxy_id) {
                        state.connected = false;
                        state.server_data.clear();
                        state.session_data.clear();
                        state.tune_values.clear();
                        state.selected_sessions.clear();
                        state.active_tab = 0;
                        state.log(&msg);
                    }
                }
                Response::CmdResult {
                    proxy_id,
                    cmd_name,
                    data,
                } => {
                    self.handle_cmd_response(proxy_id, &cmd_name, data);
                }
            }
        }
    }

    fn handle_cmd_response(&mut self, proxy_id: u64, cmd_name: &str, data: Option<Value>) {
        let state = match self.proxy_states.get_mut(&proxy_id) {
            Some(s) => s,
            None => return,
        };

        let Some(resp) = data else {
            state.log(&format!(
                "No response for '{}' — connection lost?",
                cmd_name
            ));
            return;
        };

        if resp.get("ok").and_then(Value::as_bool) != Some(true) {
            let err = resp
                .get("error")
                .and_then(Value::as_str)
                .unwrap_or("unknown");
            state.log(&format!("Error ({}): {}", cmd_name, err));
            return;
        }

        // Track what follow-up action is needed after releasing the borrow
        let mut followup_sessions: Option<usize> = None;
        let mut followup_status = false;

        match cmd_name {
            "status" => {
                if let Some(data_obj) = resp.get("data") {
                    if let Ok(status) = serde_json::from_value::<StatusData>(data_obj.clone()) {
                        let old_count = state.server_data.len();
                        let new_count = status.servers.len();

                        if old_count != new_count {
                            state.tune_values.clear();
                            state.selected_sessions.clear();
                            state.session_data.clear();
                            if state.active_tab >= new_count && new_count > 0 {
                                state.active_tab = 0;
                            }
                        }

                        for (i, srv) in status.servers.iter().enumerate() {
                            while state.tune_values.len() <= i {
                                state.tune_values.push(HashMap::new());
                            }
                            let tv = &mut state.tune_values[i];
                            for (key, val) in [
                                ("max_clients", srv.max_clients),
                                ("session_timeout", srv.session_timeout),
                                ("query_timeout", srv.query_timeout),
                                ("max_new_per_sec", srv.max_new_per_sec),
                                ("max_query_sessions", srv.max_query_sessions),
                            ] {
                                tv.entry(key.to_string())
                                    .and_modify(|v| {
                                        *v = val.to_string();
                                    })
                                    .or_insert_with(|| val.to_string());
                            }
                            tv.entry("hostname_tag".to_string())
                                .and_modify(|v| {
                                    *v = srv.hostname_tag.clone();
                                })
                                .or_insert_with(|| srv.hostname_tag.clone());
                        }

                        state.server_data = status.servers;
                    }
                }

                // Auto-request sessions for active tab
                if state.active_tab < state.server_data.len() {
                    followup_sessions = Some(state.server_data[state.active_tab].index);
                }
            }
            "sessions" => {
                if let Some(data_obj) = resp.get("data") {
                    if let Ok(sdata) = serde_json::from_value::<SessionsData>(data_obj.clone()) {
                        state.session_data.insert(sdata.server, sdata.sessions);
                    }
                }
            }
            "set" | "kick" | "kick_all" | "add_server" | "remove_server" => {
                state.log(&format!("{}: OK", cmd_name));
                followup_status = true;
            }
            _ => {}
        }

        // Now perform follow-up actions (state borrow is released)
        if let Some(srv_idx) = followup_sessions {
            self.request_sessions(proxy_id, srv_idx);
        }
        if followup_status {
            self.request_status(proxy_id);
        }
    }

    fn auto_refresh(&mut self, ctx: &egui::Context) {
        // Collect proxy IDs that need refresh
        let to_refresh: Vec<u64> = self
            .proxy_states
            .iter()
            .filter_map(|(&id, state)| {
                if !state.connected {
                    return None;
                }
                let should = state
                    .last_refresh
                    .map(|t| t.elapsed().as_secs_f32() >= REFRESH_INTERVAL_SECS)
                    .unwrap_or(false);
                if should { Some(id) } else { None }
            })
            .collect();

        for id in to_refresh {
            self.request_status(id);
            if let Some(state) = self.proxy_states.get_mut(&id) {
                state.last_refresh = Some(Instant::now());
            }
        }

        // Keep UI alive for next refresh
        ctx.request_repaint_after(std::time::Duration::from_secs(1));
    }

    // --- Proxy entry management ---

    pub fn add_proxy_entry(&mut self, entry: ProxyEntry) {
        let id = entry.id;
        self.proxy_states.insert(id, ProxyState::new());
        self.proxy_entries.push(entry);
        config::save_proxies(&self.proxy_entries);
        if self.active_proxy_id.is_none() {
            self.active_proxy_id = Some(id);
        }
    }

    pub fn update_proxy_entry(&mut self, updated: ProxyEntry) {
        if let Some(entry) = self.proxy_entries.iter_mut().find(|e| e.id == updated.id) {
            *entry = updated;
            config::save_proxies(&self.proxy_entries);
        }
    }

    pub fn remove_proxy_entry(&mut self, id: u64) {
        // Disconnect first if connected
        if self
            .proxy_states
            .get(&id)
            .map(|s| s.connected)
            .unwrap_or(false)
        {
            self.do_disconnect(id);
        }
        self.proxy_states.remove(&id);
        self.proxy_entries.retain(|e| e.id != id);
        config::save_proxies(&self.proxy_entries);
        if self.active_proxy_id == Some(id) {
            self.active_proxy_id = self.proxy_entries.first().map(|e| e.id);
        }
    }

    pub fn log_active(&mut self, msg: &str) {
        if let Some(state) = self.active_state_mut() {
            state.log(msg);
        }
    }
}

impl eframe::App for App {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        self.poll_responses();
        self.auto_refresh(ctx);

        ui::sidebar::show(self, ctx);
        ui::connection::show(self, ctx);
        ui::log::show(self, ctx);
        ui::servers::show(self, ctx);
    }
}

fn configure_style(ctx: &egui::Context) {
    let mut visuals = egui::Visuals::dark();

    // Slightly tinted dark background
    visuals.panel_fill = egui::Color32::from_rgb(22, 24, 30);
    visuals.window_fill = egui::Color32::from_rgb(28, 30, 38);
    visuals.faint_bg_color = egui::Color32::from_rgb(32, 36, 46);

    // More rounded corners
    visuals.window_corner_radius = egui::CornerRadius::same(8);
    visuals.widgets.noninteractive.corner_radius = egui::CornerRadius::same(4);
    visuals.widgets.inactive.corner_radius = egui::CornerRadius::same(6);
    visuals.widgets.hovered.corner_radius = egui::CornerRadius::same(6);
    visuals.widgets.active.corner_radius = egui::CornerRadius::same(6);

    // Subtle widget backgrounds
    visuals.widgets.inactive.bg_fill = egui::Color32::from_rgb(40, 44, 56);
    visuals.widgets.hovered.bg_fill = egui::Color32::from_rgb(55, 60, 78);
    visuals.widgets.active.bg_fill = egui::Color32::from_rgb(65, 75, 100);

    // Accent-colored selection
    visuals.selection.bg_fill = egui::Color32::from_rgb(50, 90, 160);

    ctx.set_visuals(visuals);

    // Adjust spacing
    let mut style = (*ctx.style()).clone();
    style.spacing.item_spacing = egui::vec2(8.0, 6.0);
    style.spacing.window_margin = egui::Margin::same(12);
    ctx.set_style(style);
}
