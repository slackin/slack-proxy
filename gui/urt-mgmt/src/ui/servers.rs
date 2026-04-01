use std::collections::HashSet;

use eframe::egui;
use egui_extras::{Column, TableBuilder};

use crate::app::App;
use crate::protocol::ServerInfo;

/// Render the server tabs and content in the central panel.
pub fn show(app: &mut App, ctx: &egui::Context) {
    egui::CentralPanel::default().show(ctx, |ui| {
        if app.server_data.is_empty() && !app.connected {
            ui.centered_and_justified(|ui| {
                ui.label(
                    egui::RichText::new("Connect to a urt-proxy instance to begin.")
                        .size(16.0)
                        .color(egui::Color32::from_rgb(160, 160, 170)),
                );
            });
            return;
        }

        // Tab bar + Add Server button
        ui.horizontal(|ui| {
            ui.spacing_mut().item_spacing.x = 2.0;
            for (i, srv) in app.server_data.iter().enumerate() {
                let label = format!("Server #{} :{}", i + 1, srv.listen_port);
                let selected = app.active_tab == i;
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
                    app.active_tab = i;
                }
            }

            ui.spacing_mut().item_spacing.x = 8.0;
            if app.connected {
                if ui
                    .add(
                        egui::Button::new(
                            egui::RichText::new("＋ Add Server").strong(),
                        )
                        .fill(egui::Color32::from_rgb(35, 80, 50)),
                    )
                    .clicked()
                {
                    app.show_add_server = true;
                }
            }
        });

        ui.separator();

        if app.server_data.is_empty() {
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
            // Tab content
            let tab = app.active_tab;
            if tab < app.server_data.len() {
                let srv = app.server_data[tab].clone();
                let srv_idx = srv.index;

                egui::ScrollArea::vertical()
                    .auto_shrink([false; 2])
                    .show(ui, |ui| {
                        show_config(ui, &srv);
                        ui.add_space(4.0);
                        show_tunables(app, ui, tab, srv_idx, &srv);
                        ui.add_space(4.0);
                        show_sessions(app, ui, tab, srv_idx);
                        ui.add_space(8.0);
                        show_remove_button(app, ui, srv_idx);
                    });
            }
        }
    });

    // Add Server modal window
    show_add_server_modal(app, ctx);

    // Remove confirmation modal
    show_remove_confirm_modal(app, ctx);
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

fn show_tunables(app: &mut App, ui: &mut egui::Ui, tab: usize, srv_idx: usize, srv: &ServerInfo) {
    // Ensure tune_values exist for this tab
    while app.tune_values.len() <= tab {
        app.tune_values.push(Default::default());
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
                        let entry = app.tune_values[tab]
                            .entry(key.to_string())
                            .or_default();

                        ui.label(format!("{}:", label));
                        ui.add(
                            egui::TextEdit::singleline(entry)
                                .desired_width(80.0),
                        );
                        if ui.button("Apply").clicked() {
                            if let Ok(val) = entry.parse::<i64>() {
                                app.send_set(srv_idx, key, serde_json::json!(val));
                            } else {
                                app.log(&format!("Invalid integer for {}", key));
                            }
                        }
                        ui.end_row();
                    }

                    // Hostname tag (string tunable)
                    let tag_entry = app.tune_values[tab]
                        .entry("hostname_tag".to_string())
                        .or_default();
                    ui.label("Hostname Tag:");
                    ui.add(
                        egui::TextEdit::singleline(tag_entry)
                            .desired_width(160.0),
                    );
                    if ui.button("Apply").clicked() {
                        let val = tag_entry.clone();
                        app.send_set(srv_idx, "hostname_tag", serde_json::json!(val));
                    }
                    ui.end_row();
                });
        });
}

fn show_sessions(app: &mut App, ui: &mut egui::Ui, tab: usize, srv_idx: usize) {
    let sessions = app.session_data.get(&srv_idx).cloned().unwrap_or_default();
    let active_count = sessions.iter().filter(|s| !s.is_query).count();
    let query_count = sessions.iter().filter(|s| s.is_query).count();

    // Ensure selected set exists
    while app.selected_sessions.len() <= tab {
        app.selected_sessions.push(HashSet::new());
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
            .column(Column::exact(24.0))        // checkbox
            .column(Column::initial(160.0).at_least(100.0)) // client
            .column(Column::initial(55.0))       // query
            .column(Column::initial(60.0))       // idle
            .column(Column::initial(80.0))       // pkts up
            .column(Column::initial(80.0))       // pkts down
            .column(Column::initial(90.0))       // bytes up
            .column(Column::remainder().at_least(90.0)) // bytes down
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
                        let is_selected = app.selected_sessions[tab].contains(&client_key);

                        row.col(|ui| {
                            let mut sel = is_selected;
                            if ui.checkbox(&mut sel, "").changed() {
                                if sel {
                                    app.selected_sessions[tab].insert(client_key.clone());
                                } else {
                                    app.selected_sessions[tab].remove(&client_key);
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
                let selected: Vec<String> =
                    app.selected_sessions[tab].iter().cloned().collect();
                for client in selected {
                    app.send_kick(srv_idx, &client);
                }
                app.selected_sessions[tab].clear();
            }

            if ui.button("Kick All").clicked() {
                app.confirm_kick_all = Some(srv_idx);
            }
        });
    });

    // Kick All confirmation modal
    if let Some(kick_srv) = app.confirm_kick_all {
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
                        app.send_kick_all(kick_srv);
                        app.confirm_kick_all = None;
                    }
                    if ui.button("Cancel").clicked() {
                        app.confirm_kick_all = None;
                    }
                });
            });
    }
}

fn show_remove_button(app: &mut App, ui: &mut egui::Ui, srv_idx: usize) {
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
        app.confirm_remove = Some(srv_idx);
    }
}

fn show_add_server_modal(app: &mut App, ctx: &egui::Context) {
    if !app.show_add_server {
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
            egui::Grid::new("add_server_grid")
                .num_columns(2)
                .spacing([12.0, 8.0])
                .show(ui, |ui| {
                    ui.label("Listen Port:");
                    ui.add(
                        egui::TextEdit::singleline(&mut app.add_listen_port)
                            .desired_width(120.0),
                    );
                    ui.end_row();

                    ui.label("Remote Host:");
                    ui.add(
                        egui::TextEdit::singleline(&mut app.add_remote_host)
                            .desired_width(180.0)
                            .hint_text("e.g. 10.0.0.2"),
                    );
                    ui.end_row();

                    ui.label("Remote Port:");
                    ui.add(
                        egui::TextEdit::singleline(&mut app.add_remote_port)
                            .desired_width(120.0),
                    );
                    ui.end_row();

                    ui.label("Max Clients:");
                    ui.add(
                        egui::TextEdit::singleline(&mut app.add_max_clients)
                            .desired_width(80.0),
                    );
                    ui.end_row();

                    ui.label("Session Timeout:");
                    ui.add(
                        egui::TextEdit::singleline(&mut app.add_timeout)
                            .desired_width(80.0),
                    );
                    ui.end_row();

                    ui.label("Hostname Tag:");
                    ui.add(
                        egui::TextEdit::singleline(&mut app.add_hostname_tag)
                            .desired_width(180.0)
                            .hint_text("e.g. [PROXY]"),
                    );
                    ui.end_row();
                });

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
                    let listen_port = app.add_listen_port.trim().parse::<u16>();
                    let remote_port = app.add_remote_port.trim().parse::<u16>();
                    let max_clients = app.add_max_clients.trim().parse::<i64>();
                    let timeout = app.add_timeout.trim().parse::<i64>();
                    let remote_host = app.add_remote_host.trim().to_string();

                    if remote_host.is_empty() {
                        app.log("Remote host is required.");
                    } else if let (Ok(lp), Ok(rp), Ok(mc), Ok(to)) =
                        (listen_port, remote_port, max_clients, timeout)
                    {
                        let tag = app.add_hostname_tag.trim().to_string();
                        app.send_add_server(
                            lp,
                            &remote_host,
                            rp,
                            mc,
                            to,
                            &tag,
                        );
                        app.show_add_server = false;
                        // Reset form
                        app.add_listen_port = "27960".to_string();
                        app.add_remote_host = String::new();
                        app.add_remote_port = "27960".to_string();
                        app.add_max_clients = "20".to_string();
                        app.add_timeout = "30".to_string();
                        app.add_hostname_tag = String::new();
                    } else {
                        app.log("Invalid port or numeric value.");
                    }
                }

                if ui.button("Cancel").clicked() {
                    app.show_add_server = false;
                }
            });
        });

    if !open {
        app.show_add_server = false;
    }
}

fn show_remove_confirm_modal(app: &mut App, ctx: &egui::Context) {
    if let Some(srv_idx) = app.confirm_remove {
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
                        app.send_remove_server(srv_idx);
                        app.confirm_remove = None;
                        if app.active_tab > 0 {
                            app.active_tab -= 1;
                        }
                    }
                    if ui.button("Cancel").clicked() {
                        app.confirm_remove = None;
                    }
                });
            });
    }
}
