use std::collections::HashSet;
use std::net::UdpSocket;

use eframe::egui;
use egui_extras::{Column, TableBuilder};

use crate::app::App;
use crate::protocol::ServerInfo;

/// Starting port for auto-suggested listen ports.
const DEFAULT_LISTEN_PORT_START: u16 = 27990;

/// Compute the next available listen port starting from `DEFAULT_LISTEN_PORT_START`,
/// skipping any port already used by a server in `servers`.
fn next_available_port(servers: &[ServerInfo]) -> String {
    let used: HashSet<u16> = servers.iter().map(|s| s.listen_port).collect();
    let mut port = DEFAULT_LISTEN_PORT_START;
    while used.contains(&port) && port < u16::MAX {
        port += 1;
    }
    port.to_string()
}

/// Check whether a UDP port is already bound on this machine.
/// Returns true if the port appears to be in use.
fn is_port_in_use(port: u16) -> bool {
    UdpSocket::bind(("0.0.0.0", port)).is_err()
}

/// Render the server tabs and content in the central panel.
pub fn show(app: &mut App, ctx: &egui::Context) {
    egui::CentralPanel::default().show(ctx, |ui| {
        let proxy_id = match app.active_proxy_id {
            Some(id) => id,
            None => {
                ui.centered_and_justified(|ui| {
                    ui.label(
                        egui::RichText::new("Add a proxy in the sidebar to get started.")
                            .size(16.0)
                            .color(egui::Color32::from_rgb(160, 160, 170)),
                    );
                });
                return;
            }
        };

        let connected = app
            .proxy_states
            .get(&proxy_id)
            .map(|s| s.connected)
            .unwrap_or(false);
        let server_count = app
            .proxy_states
            .get(&proxy_id)
            .map(|s| s.server_data.len())
            .unwrap_or(0);

        if server_count == 0 && !connected {
            ui.centered_and_justified(|ui| {
                ui.label(
                    egui::RichText::new("Connect to this proxy to begin.")
                        .size(16.0)
                        .color(egui::Color32::from_rgb(160, 160, 170)),
                );
            });
            return;
        }

        // Tab bar + Add Server button
        ui.horizontal(|ui| {
            ui.spacing_mut().item_spacing.x = 2.0;

            let tab_info: Vec<(usize, u16)> = app
                .proxy_states
                .get(&proxy_id)
                .map(|s| {
                    s.server_data
                        .iter()
                        .enumerate()
                        .map(|(i, srv)| (i, srv.listen_port))
                        .collect()
                })
                .unwrap_or_default();

            let active_tab = app
                .proxy_states
                .get(&proxy_id)
                .map(|s| s.active_tab)
                .unwrap_or(0);

            for (i, listen_port) in &tab_info {
                let label = format!("Server #{} :{}", i + 1, listen_port);
                let selected = active_tab == *i;
                let btn = egui::Button::new(
                    egui::RichText::new(&label).strong(),
                )
                .fill(if selected {
                    egui::Color32::from_rgb(50, 60, 80)
                } else {
                    egui::Color32::TRANSPARENT
                })
                .corner_radius(egui::CornerRadius {
                    nw: 6,
                    ne: 6,
                    sw: 0,
                    se: 0,
                });

                if ui.add(btn).clicked() {
                    if let Some(state) = app.proxy_states.get_mut(&proxy_id) {
                        state.active_tab = *i;
                    }
                }
            }

            ui.spacing_mut().item_spacing.x = 8.0;
            if connected {
                if ui
                    .add(
                        egui::Button::new(
                            egui::RichText::new("＋ Add Server").strong(),
                        )
                        .fill(egui::Color32::from_rgb(35, 80, 50)),
                    )
                    .clicked()
                {
                    if let Some(state) = app.proxy_states.get_mut(&proxy_id) {
                        state.add_listen_port = next_available_port(&state.server_data);
                        state.show_add_server = true;
                    }
                }
            }
        });

        ui.separator();

        if server_count == 0 {
            ui.centered_and_justified(|ui| {
                ui.label(
                    egui::RichText::new(
                        "No servers configured. Click \"＋ Add Server\" to add one.",
                    )
                    .size(15.0)
                    .color(egui::Color32::from_rgb(160, 160, 170)),
                );
            });
        } else {
            // Get tab data
            let (tab, srv, srv_idx) = {
                let state = match app.proxy_states.get(&proxy_id) {
                    Some(s) => s,
                    None => return,
                };
                let tab = state.active_tab;
                if tab >= state.server_data.len() {
                    return;
                }
                let srv = state.server_data[tab].clone();
                let srv_idx = srv.index;
                (tab, srv, srv_idx)
            };

            egui::ScrollArea::vertical()
                .auto_shrink([false; 2])
                .show(ui, |ui| {
                    show_config(ui, &srv);
                    ui.add_space(4.0);
                    show_tunables(app, ui, proxy_id, tab, srv_idx, &srv);
                    ui.add_space(4.0);
                    show_sessions(app, ui, proxy_id, tab, srv_idx);
                    ui.add_space(8.0);
                    show_remove_button(app, ui, proxy_id, srv_idx);
                });
        }
    });

    // Modals need proxy_id
    if let Some(proxy_id) = app.active_proxy_id {
        show_add_server_modal(app, ctx, proxy_id);
        show_remove_confirm_modal(app, ctx, proxy_id);
    }
}

fn show_config(ui: &mut egui::Ui, srv: &ServerInfo) {
    egui::CollapsingHeader::new(egui::RichText::new("⚙  Configuration").strong())
        .default_open(true)
        .show(ui, |ui| {
            egui::Grid::new("config_grid")
                .num_columns(2)
                .spacing([16.0, 4.0])
                .show(ui, |ui| {
                    ui.label("Listen Port:");
                    ui.label(
                        egui::RichText::new(srv.listen_port.to_string()).monospace(),
                    );
                    ui.end_row();

                    ui.label("Remote:");
                    ui.label(
                        egui::RichText::new(&srv.remote_addr).monospace(),
                    );
                    ui.end_row();

                    ui.label("Hostname Tag:");
                    ui.label(
                        egui::RichText::new(&srv.hostname_tag).monospace(),
                    );
                    ui.end_row();
                });
        });
}

fn show_tunables(
    app: &mut App,
    ui: &mut egui::Ui,
    proxy_id: u64,
    tab: usize,
    srv_idx: usize,
    srv: &ServerInfo,
) {
    // Ensure tune_values exist for this tab
    if let Some(state) = app.proxy_states.get_mut(&proxy_id) {
        while state.tune_values.len() <= tab {
            state.tune_values.push(Default::default());
        }
    }

    egui::CollapsingHeader::new(egui::RichText::new("🔧  Tune Parameters").strong())
        .default_open(true)
        .show(ui, |ui| {
            egui::Grid::new(format!("tune_grid_{}", tab))
                .num_columns(3)
                .spacing([12.0, 6.0])
                .show(ui, |ui| {
                    let int_tunables = [
                        ("max_clients", "Max Clients", srv.max_clients),
                        ("session_timeout", "Session Timeout", srv.session_timeout),
                        ("query_timeout", "Query Timeout", srv.query_timeout),
                        ("max_new_per_sec", "Rate Limit", srv.max_new_per_sec),
                        ("max_query_sessions", "Max Query Sessions", srv.max_query_sessions),
                    ];

                    for (key, label, _current) in &int_tunables {
                        let entry_val = app
                            .proxy_states
                            .get_mut(&proxy_id)
                            .and_then(|s| s.tune_values.get_mut(tab))
                            .map(|tv| {
                                tv.entry(key.to_string()).or_default();
                                tv
                            });

                        if let Some(tv) = entry_val {
                            let entry = tv.get_mut(*key).unwrap();

                            ui.label(format!("{}:", label));
                            ui.add(
                                egui::TextEdit::singleline(entry).desired_width(80.0),
                            );
                            if ui.button("Apply").clicked() {
                                if let Ok(val) = entry.parse::<i64>() {
                                    app.send_set(proxy_id, srv_idx, key, serde_json::json!(val));
                                } else {
                                    app.log_active(&format!("Invalid integer for {}", key));
                                }
                            }
                            ui.end_row();
                        }
                    }

                    // Hostname tag (string tunable)
                    let tag_val = app
                        .proxy_states
                        .get_mut(&proxy_id)
                        .and_then(|s| s.tune_values.get_mut(tab))
                        .map(|tv| {
                            tv.entry("hostname_tag".to_string()).or_default();
                            tv
                        });

                    if let Some(tv) = tag_val {
                        let tag_entry = tv.get_mut("hostname_tag").unwrap();
                        ui.label("Hostname Tag:");
                        ui.add(
                            egui::TextEdit::singleline(tag_entry).desired_width(160.0),
                        );
                        if ui.button("Apply").clicked() {
                            let val = tag_entry.clone();
                            app.send_set(
                                proxy_id,
                                srv_idx,
                                "hostname_tag",
                                serde_json::json!(val),
                            );
                        }
                        ui.end_row();
                    }
                });
        });
}

fn show_sessions(app: &mut App, ui: &mut egui::Ui, proxy_id: u64, tab: usize, srv_idx: usize) {
    let sessions = app
        .proxy_states
        .get(&proxy_id)
        .and_then(|s| s.session_data.get(&srv_idx))
        .cloned()
        .unwrap_or_default();

    let active_count = sessions.iter().filter(|s| !s.is_query).count();
    let query_count = sessions.iter().filter(|s| s.is_query).count();

    // Ensure selected set exists
    if let Some(state) = app.proxy_states.get_mut(&proxy_id) {
        while state.selected_sessions.len() <= tab {
            state.selected_sessions.push(HashSet::new());
        }
    }

    egui::CollapsingHeader::new(
        egui::RichText::new(format!(
            "📋  Sessions — {} active, {} queries",
            active_count, query_count
        ))
        .strong(),
    )
    .default_open(true)
    .show(ui, |ui| {
        let table = TableBuilder::new(ui)
            .striped(true)
            .resizable(true)
            .cell_layout(egui::Layout::left_to_right(egui::Align::Center))
            .column(Column::exact(24.0))
            .column(Column::initial(160.0).at_least(100.0))
            .column(Column::initial(55.0))
            .column(Column::initial(60.0))
            .column(Column::initial(80.0))
            .column(Column::initial(80.0))
            .column(Column::initial(90.0))
            .column(Column::remainder().at_least(90.0))
            .min_scrolled_height(0.0)
            .max_scroll_height(300.0);

        table
            .header(22.0, |mut header| {
                header.col(|ui| { ui.label(""); });
                header.col(|ui| { ui.strong("Client"); });
                header.col(|ui| { ui.strong("Query?"); });
                header.col(|ui| { ui.strong("Idle (s)"); });
                header.col(|ui| { ui.strong("Pkts ↑"); });
                header.col(|ui| { ui.strong("Pkts ↓"); });
                header.col(|ui| { ui.strong("Bytes ↑"); });
                header.col(|ui| { ui.strong("Bytes ↓"); });
            })
            .body(|mut body| {
                for sess in &sessions {
                    body.row(20.0, |mut row| {
                        let client_key = sess.client.clone();
                        let is_selected = app
                            .proxy_states
                            .get(&proxy_id)
                            .and_then(|s| s.selected_sessions.get(tab))
                            .map(|ss| ss.contains(&client_key))
                            .unwrap_or(false);

                        row.col(|ui| {
                            let mut sel = is_selected;
                            if ui.checkbox(&mut sel, "").changed() {
                                if let Some(state) = app.proxy_states.get_mut(&proxy_id) {
                                    if let Some(ss) = state.selected_sessions.get_mut(tab) {
                                        if sel {
                                            ss.insert(client_key.clone());
                                        } else {
                                            ss.remove(&client_key);
                                        }
                                    }
                                }
                            }
                        });
                        row.col(|ui| {
                            ui.label(
                                egui::RichText::new(&sess.client).monospace(),
                            );
                        });
                        row.col(|ui| {
                            ui.label(if sess.is_query { "Yes" } else { "No" });
                        });
                        row.col(|ui| {
                            ui.label(
                                egui::RichText::new(sess.idle_secs.to_string()).monospace(),
                            );
                        });
                        row.col(|ui| {
                            ui.label(
                                egui::RichText::new(sess.pkts_to_server.to_string()).monospace(),
                            );
                        });
                        row.col(|ui| {
                            ui.label(
                                egui::RichText::new(sess.pkts_to_client.to_string()).monospace(),
                            );
                        });
                        row.col(|ui| {
                            ui.label(
                                egui::RichText::new(sess.bytes_to_server.to_string()).monospace(),
                            );
                        });
                        row.col(|ui| {
                            ui.label(
                                egui::RichText::new(sess.bytes_to_client.to_string()).monospace(),
                            );
                        });
                    });
                }
            });

        // Action buttons
        ui.add_space(6.0);
        ui.horizontal(|ui| {
            if ui.button("Kick Selected").clicked() {
                let to_kick: Vec<String> = app
                    .proxy_states
                    .get_mut(&proxy_id)
                    .and_then(|s| s.selected_sessions.get_mut(tab))
                    .map(|ss| {
                        let selected: Vec<String> = ss.iter().cloned().collect();
                        ss.clear();
                        selected
                    })
                    .unwrap_or_default();
                for client in to_kick {
                    app.send_kick(proxy_id, srv_idx, &client);
                }
            }

            if ui.button("Kick All").clicked() {
                if let Some(state) = app.proxy_states.get_mut(&proxy_id) {
                    state.confirm_kick_all = Some(srv_idx);
                }
            }
        });
    });

    // Kick All confirmation modal
    let confirm = app
        .proxy_states
        .get(&proxy_id)
        .and_then(|s| s.confirm_kick_all);
    if let Some(kick_srv) = confirm {
        egui::Window::new("Confirm Kick All")
            .collapsible(false)
            .resizable(false)
            .anchor(egui::Align2::CENTER_CENTER, [0.0, 0.0])
            .show(ui.ctx(), |ui| {
                ui.label(format!(
                    "Kick ALL sessions on server index {}?",
                    kick_srv
                ));
                ui.add_space(8.0);
                ui.horizontal(|ui| {
                    if ui
                        .add(
                            egui::Button::new("Yes, Kick All")
                                .fill(egui::Color32::from_rgb(160, 40, 40)),
                        )
                        .clicked()
                    {
                        app.send_kick_all(proxy_id, kick_srv);
                        if let Some(state) = app.proxy_states.get_mut(&proxy_id) {
                            state.confirm_kick_all = None;
                        }
                    }
                    if ui.button("Cancel").clicked() {
                        if let Some(state) = app.proxy_states.get_mut(&proxy_id) {
                            state.confirm_kick_all = None;
                        }
                    }
                });
            });
    }
}

fn show_remove_button(app: &mut App, ui: &mut egui::Ui, proxy_id: u64, srv_idx: usize) {
    ui.separator();
    if ui
        .add(
            egui::Button::new(
                egui::RichText::new("🗑  Remove This Server")
                    .color(egui::Color32::from_rgb(220, 80, 80)),
            )
            .fill(egui::Color32::from_rgb(60, 30, 30)),
        )
        .clicked()
    {
        if let Some(state) = app.proxy_states.get_mut(&proxy_id) {
            state.confirm_remove = Some(srv_idx);
        }
    }
}

fn show_add_server_modal(app: &mut App, ctx: &egui::Context, proxy_id: u64) {
    let show = app
        .proxy_states
        .get(&proxy_id)
        .map(|s| s.show_add_server)
        .unwrap_or(false);
    if !show {
        return;
    }

    let mut open = true;
    egui::Window::new("Add Server")
        .open(&mut open)
        .collapsible(false)
        .resizable(false)
        .anchor(egui::Align2::CENTER_CENTER, [0.0, 0.0])
        .min_width(340.0)
        .show(ctx, |ui| {
            if let Some(state) = app.proxy_states.get_mut(&proxy_id) {
                egui::Grid::new("add_server_grid")
                    .num_columns(2)
                    .spacing([12.0, 8.0])
                    .show(ui, |ui| {
                        ui.label("Listen Port:");
                        ui.add(
                            egui::TextEdit::singleline(&mut state.add_listen_port)
                                .desired_width(120.0),
                        );
                        ui.end_row();

                        ui.label("Remote Host:");
                        ui.add(
                            egui::TextEdit::singleline(&mut state.add_remote_host)
                                .desired_width(180.0)
                                .hint_text("e.g. 10.0.0.2"),
                        );
                        ui.end_row();

                        ui.label("Remote Port:");
                        ui.add(
                            egui::TextEdit::singleline(&mut state.add_remote_port)
                                .desired_width(120.0),
                        );
                        ui.end_row();

                        ui.label("Max Clients:");
                        ui.add(
                            egui::TextEdit::singleline(&mut state.add_max_clients)
                                .desired_width(80.0),
                        );
                        ui.end_row();

                        ui.label("Session Timeout:");
                        ui.add(
                            egui::TextEdit::singleline(&mut state.add_timeout)
                                .desired_width(80.0),
                        );
                        ui.end_row();

                        ui.label("Hostname Tag:");
                        ui.add(
                            egui::TextEdit::singleline(&mut state.add_hostname_tag)
                                .desired_width(180.0)
                                .hint_text("e.g. [PROXY]"),
                        );
                        ui.end_row();
                    });
            }

            // Collect form values and used ports before the horizontal layout
            let (form_data, used_ports) = {
                let state = app.proxy_states.get(&proxy_id);
                let form = state.map(|s| {
                    (
                        s.add_listen_port.trim().parse::<u16>(),
                        s.add_remote_port.trim().parse::<u16>(),
                        s.add_max_clients.trim().parse::<i64>(),
                        s.add_timeout.trim().parse::<i64>(),
                        s.add_remote_host.trim().to_string(),
                        s.add_hostname_tag.trim().to_string(),
                    )
                });
                let ports: Vec<u16> = state
                    .map(|s| s.server_data.iter().map(|srv| srv.listen_port).collect())
                    .unwrap_or_default();
                (form, ports)
            };

            ui.add_space(10.0);
            ui.horizontal(|ui| {
                if ui
                    .add(
                        egui::Button::new(
                            egui::RichText::new("Add Server").strong(),
                        )
                        .fill(egui::Color32::from_rgb(35, 80, 50)),
                    )
                    .clicked()
                {
                    if let Some((listen_port, remote_port, max_clients, timeout, remote_host, tag)) = form_data {
                        if remote_host.is_empty() {
                            if let Some(state) = app.proxy_states.get_mut(&proxy_id) {
                                state.log("Remote host is required.");
                            }
                        } else if let (Ok(lp), Ok(rp), Ok(mc), Ok(to)) =
                            (listen_port, remote_port, max_clients, timeout)
                        {
                            // Check if listen port is already used by another server on this proxy
                            if used_ports.contains(&lp) {
                                if let Some(state) = app.proxy_states.get_mut(&proxy_id) {
                                    state.log(&format!(
                                        "Port {} is already in use by another server on this proxy.",
                                        lp
                                    ));
                                }
                            } else if is_port_in_use(lp) {
                                // Check if the port is already bound on this machine
                                if let Some(state) = app.proxy_states.get_mut(&proxy_id) {
                                    state.log(&format!(
                                        "Port {} is already in use on this machine.",
                                        lp
                                    ));
                                }
                            } else {
                                if let Some(state) = app.proxy_states.get_mut(&proxy_id) {
                                    state.show_add_server = false;
                                    state.add_listen_port = next_available_port(&state.server_data);
                                    state.add_remote_host = String::new();
                                    state.add_remote_port = "27960".to_string();
                                    state.add_max_clients = "20".to_string();
                                    state.add_timeout = "30".to_string();
                                    state.add_hostname_tag = String::new();
                                }
                                app.send_add_server(
                                    proxy_id, lp, &remote_host, rp, mc, to, &tag,
                                );
                            }
                        } else {
                            if let Some(state) = app.proxy_states.get_mut(&proxy_id) {
                                state.log("Invalid port or numeric value.");
                            }
                        }
                    }
                }

                if ui.button("Cancel").clicked() {
                    if let Some(state) = app.proxy_states.get_mut(&proxy_id) {
                        state.show_add_server = false;
                    }
                }
            });
        });

    if !open {
        if let Some(state) = app.proxy_states.get_mut(&proxy_id) {
            state.show_add_server = false;
        }
    }
}

fn show_remove_confirm_modal(app: &mut App, ctx: &egui::Context, proxy_id: u64) {
    let confirm = app
        .proxy_states
        .get(&proxy_id)
        .and_then(|s| s.confirm_remove);

    if let Some(srv_idx) = confirm {
        egui::Window::new("Confirm Remove Server")
            .collapsible(false)
            .resizable(false)
            .anchor(egui::Align2::CENTER_CENTER, [0.0, 0.0])
            .show(ctx, |ui| {
                ui.label(format!(
                    "Remove server index {}? All sessions will be dropped.",
                    srv_idx
                ));
                ui.add_space(8.0);
                ui.horizontal(|ui| {
                    if ui
                        .add(
                            egui::Button::new("Yes, Remove")
                                .fill(egui::Color32::from_rgb(160, 40, 40)),
                        )
                        .clicked()
                    {
                        app.send_remove_server(proxy_id, srv_idx);
                        if let Some(state) = app.proxy_states.get_mut(&proxy_id) {
                            state.confirm_remove = None;
                            if state.active_tab > 0 {
                                state.active_tab -= 1;
                            }
                        }
                    }
                    if ui.button("Cancel").clicked() {
                        if let Some(state) = app.proxy_states.get_mut(&proxy_id) {
                            state.confirm_remove = None;
                        }
                    }
                });
            });
    }
}
