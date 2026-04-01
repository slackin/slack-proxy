use std::collections::{HashMap, HashSet, VecDeque};
use std::sync::mpsc;
use std::time::Instant;

use eframe::egui;
use serde_json::Value;

use crate::protocol::{ServerInfo, SessionInfo, SessionsData, StatusData};
use crate::ui;
use crate::worker::{self, Command, Response};

const MAX_LOG_ENTRIES: usize = 500;
const REFRESH_INTERVAL_SECS: f32 = 2.0;

pub struct App {
    // Connection fields
    pub host: String,
    pub port: String,
    pub api_key: String,
    pub connected: bool,

    // Server state
    pub server_data: Vec<ServerInfo>,
    pub session_data: HashMap<usize, Vec<SessionInfo>>,
    pub active_tab: usize,

    // Tune parameter edit buffers: per-tab map of key -> string value
    pub tune_values: Vec<HashMap<String, String>>,

    // Session selection: per-tab set of client addresses
    pub selected_sessions: Vec<HashSet<String>>,

    // Kick All confirmation modal
    pub confirm_kick_all: Option<usize>,

    // Remove Server confirmation modal
    pub confirm_remove: Option<usize>,

    // Add Server form state
    pub show_add_server: bool,
    pub add_listen_port: String,
    pub add_remote_host: String,
    pub add_remote_port: String,
    pub add_max_clients: String,
    pub add_timeout: String,
    pub add_hostname_tag: String,

    // Log
    pub log_entries: VecDeque<(String, String)>,

    // Worker communication
    cmd_tx: mpsc::Sender<Command>,
    resp_rx: mpsc::Receiver<Response>,

    // Auto-refresh
    last_refresh: Option<Instant>,
}

impl App {
    pub fn new(cc: &eframe::CreationContext<'_>) -> Self {
        configure_style(&cc.egui_ctx);

        let (cmd_tx, resp_rx) = worker::spawn_worker();

        Self {
            host: "127.0.0.1".to_string(),
            port: "27961".to_string(),
            api_key: String::new(),
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
            cmd_tx,
            resp_rx,
            last_refresh: None,
        }
    }

    // --- Public helpers called by UI ---

    pub fn log(&mut self, msg: &str) {
        let ts = chrono::Local::now().format("[%H:%M:%S]").to_string();
        self.log_entries.push_back((ts, msg.to_string()));
        if self.log_entries.len() > MAX_LOG_ENTRIES {
            self.log_entries.pop_front();
        }
    }

    pub fn do_connect(&mut self) {
        let host = self.host.trim().to_string();
        let port_str = self.port.trim().to_string();
        let key = self.api_key.trim().to_string();

        if host.is_empty() || port_str.is_empty() || key.is_empty() {
            self.log("Host, Port, and API Key are all required.");
            return;
        }

        let port: u16 = match port_str.parse() {
            Ok(p) => p,
            Err(_) => {
                self.log("Invalid port number.");
                return;
            }
        };

        self.log(&format!("Connecting to {}:{}...", host, port));
        let _ = self.cmd_tx.send(Command::Connect { host, port, key });
    }

    pub fn do_disconnect(&mut self) {
        let _ = self.cmd_tx.send(Command::Disconnect);
    }

    pub fn send_set(&mut self, server: usize, key: &str, value: Value) {
        let payload = serde_json::json!({
            "cmd": "set",
            "server": server,
            "key": key,
            "value": value,
        });
        let _ = self.cmd_tx.send(Command::Send {
            cmd_name: "set".to_string(),
            payload,
        });
    }

    pub fn send_kick(&mut self, server: usize, client: &str) {
        let payload = serde_json::json!({
            "cmd": "kick",
            "server": server,
            "client": client,
        });
        let _ = self.cmd_tx.send(Command::Send {
            cmd_name: "kick".to_string(),
            payload,
        });
        self.log(&format!("Kicking {} on server #{}", client, server + 1));
    }

    pub fn send_kick_all(&mut self, server: usize) {
        let payload = serde_json::json!({
            "cmd": "kick_all",
            "server": server,
        });
        let _ = self.cmd_tx.send(Command::Send {
            cmd_name: "kick_all".to_string(),
            payload,
        });
        self.log(&format!("Kick all on server #{}", server + 1));
    }

    pub fn send_add_server(&mut self, listen_port: u16, remote_host: &str,
                           remote_port: u16, max_clients: i64,
                           session_timeout: i64, hostname_tag: &str) {
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
            cmd_name: "add_server".to_string(),
            payload,
        });
        self.log(&format!("Adding server :{} -> {}:{}", listen_port, remote_host, remote_port));
    }

    pub fn send_remove_server(&mut self, server: usize) {
        let payload = serde_json::json!({
            "cmd": "remove_server",
            "server": server,
        });
        let _ = self.cmd_tx.send(Command::Send {
            cmd_name: "remove_server".to_string(),
            payload,
        });
        self.log(&format!("Removing server #{}", server + 1));
    }

    // --- Internal helpers ---

    fn request_status(&self) {
        let _ = self.cmd_tx.send(Command::Send {
            cmd_name: "status".to_string(),
            payload: serde_json::json!({ "cmd": "status" }),
        });
    }

    fn request_sessions(&self, server: usize) {
        let _ = self.cmd_tx.send(Command::Send {
            cmd_name: "sessions".to_string(),
            payload: serde_json::json!({ "cmd": "sessions", "server": server }),
        });
    }

    fn poll_responses(&mut self) {
        while let Ok(resp) = self.resp_rx.try_recv() {
            match resp {
                Response::Connected { ok, msg } => {
                    if ok {
                        self.connected = true;
                        self.log(&msg);
                        self.request_status();
                        self.last_refresh = Some(Instant::now());
                    } else {
                        self.connected = false;
                        self.log(&format!("Connection failed: {}", msg));
                    }
                }
                Response::Disconnected { msg } => {
                    self.connected = false;
                    self.server_data.clear();
                    self.session_data.clear();
                    self.tune_values.clear();
                    self.selected_sessions.clear();
                    self.active_tab = 0;
                    self.log(&msg);
                }
                Response::CmdResult { cmd_name, data } => {
                    self.handle_cmd_response(&cmd_name, data);
                }
            }
        }
    }

    fn handle_cmd_response(&mut self, cmd_name: &str, data: Option<Value>) {
        let Some(resp) = data else {
            self.log(&format!("No response for '{}' — connection lost?", cmd_name));
            return;
        };

        if resp.get("ok").and_then(Value::as_bool) != Some(true) {
            let err = resp
                .get("error")
                .and_then(Value::as_str)
                .unwrap_or("unknown");
            self.log(&format!("Error ({}): {}", cmd_name, err));
            return;
        }

        match cmd_name {
            "status" => {
                if let Some(data_obj) = resp.get("data") {
                    if let Ok(status) = serde_json::from_value::<StatusData>(data_obj.clone()) {
                        let old_count = self.server_data.len();
                        let new_count = status.servers.len();

                        // Update tune value buffers from server if tab count changed
                        if old_count != new_count {
                            self.tune_values.clear();
                            self.selected_sessions.clear();
                            self.session_data.clear();
                            if self.active_tab >= new_count && new_count > 0 {
                                self.active_tab = 0;
                            }
                        }

                        // Sync tune_values with server data
                        for (i, srv) in status.servers.iter().enumerate() {
                            while self.tune_values.len() <= i {
                                self.tune_values.push(HashMap::new());
                            }
                            let tv = &mut self.tune_values[i];
                            // Only set if not currently being edited (empty = first load)
                            for (key, val) in [
                                ("max_clients", srv.max_clients),
                                ("session_timeout", srv.session_timeout),
                                ("query_timeout", srv.query_timeout),
                                ("max_new_per_sec", srv.max_new_per_sec),
                                ("max_query_sessions", srv.max_query_sessions),
                            ] {
                                tv.entry(key.to_string())
                                    .and_modify(|v| {
                                        // Only update if the user hasn't changed it from what we last set
                                        *v = val.to_string();
                                    })
                                    .or_insert_with(|| val.to_string());
                            }
                            tv.entry("hostname_tag".to_string())
                                .and_modify(|v| { *v = srv.hostname_tag.clone(); })
                                .or_insert_with(|| srv.hostname_tag.clone());
                        }

                        self.server_data = status.servers;
                    }
                }

                // Auto-request sessions for active tab
                if self.active_tab < self.server_data.len() {
                    let srv_idx = self.server_data[self.active_tab].index;
                    self.request_sessions(srv_idx);
                }
            }
            "sessions" => {
                if let Some(data_obj) = resp.get("data") {
                    if let Ok(sdata) = serde_json::from_value::<SessionsData>(data_obj.clone()) {
                        self.session_data.insert(sdata.server, sdata.sessions);
                    }
                }
            }
            "set" | "kick" | "kick_all" | "add_server" | "remove_server" => {
                self.log(&format!("{}: OK", cmd_name));
                self.request_status();
            }
            _ => {}
        }
    }

    fn auto_refresh(&mut self, ctx: &egui::Context) {
        if !self.connected {
            self.last_refresh = None;
            return;
        }

        let should_refresh = self
            .last_refresh
            .map(|t| t.elapsed().as_secs_f32() >= REFRESH_INTERVAL_SECS)
            .unwrap_or(false);

        if should_refresh {
            self.request_status();
            self.last_refresh = Some(Instant::now());
        }

        // Keep UI alive for next refresh
        ctx.request_repaint_after(std::time::Duration::from_secs(1));
    }
}

impl eframe::App for App {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        self.poll_responses();
        self.auto_refresh(ctx);

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
