use eframe::egui;

use crate::app::App;
use crate::config;
use crate::proxy_entry::ProxyEntry;

/// Render the left sidebar with saved proxy entries.
pub fn show(app: &mut App, ctx: &egui::Context) {
    egui::SidePanel::left("proxy_sidebar")
        .resizable(true)
        .default_width(200.0)
        .min_width(160.0)
        .show(ctx, |ui| {
            ui.add_space(4.0);
            ui.horizontal(|ui| {
                ui.heading("Proxies");
                ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                    if ui
                        .add(
                            egui::Button::new("＋")
                                .fill(egui::Color32::from_rgb(35, 80, 50)),
                        )
                        .on_hover_text("Add proxy")
                        .clicked()
                    {
                        app.editing_proxy_id = None;
                        app.edit_name = String::new();
                        app.edit_host = "127.0.0.1".to_string();
                        app.edit_port = "27961".to_string();
                        app.edit_api_key = String::new();
                        app.edit_auto_connect = false;
                        app.show_add_proxy = true;
                    }
                });
            });
            ui.separator();

            if app.proxy_entries.is_empty() {
                ui.add_space(12.0);
                ui.centered_and_justified(|ui| {
                    ui.label(
                        egui::RichText::new("No proxies saved.\nClick ＋ to add one.")
                            .size(13.0)
                            .color(egui::Color32::from_rgb(140, 140, 150)),
                    );
                });
            } else {
                egui::ScrollArea::vertical().show(ui, |ui| {
                    // Collect info to avoid borrow issues
                    let entries: Vec<(u64, String, String, bool)> = app
                        .proxy_entries
                        .iter()
                        .map(|e| {
                            let connected = app
                                .proxy_states
                                .get(&e.id)
                                .map(|s| s.connected)
                                .unwrap_or(false);
                            (e.id, e.name.clone(), e.display_addr(), connected)
                        })
                        .collect();

                    let active_id = app.active_proxy_id;
                    let mut action: Option<SidebarAction> = None;

                    for (id, name, addr, connected) in &entries {
                        let is_active = active_id == Some(*id);

                        let frame = egui::Frame::NONE
                            .fill(if is_active {
                                egui::Color32::from_rgb(40, 50, 70)
                            } else {
                                egui::Color32::TRANSPARENT
                            })
                            .corner_radius(4.0)
                            .inner_margin(egui::Margin::symmetric(6, 4));

                        let resp = frame
                            .show(ui, |ui| {
                                ui.set_width(ui.available_width());
                                ui.horizontal(|ui| {
                                    // Status dot
                                    let dot_color = if *connected {
                                        egui::Color32::from_rgb(80, 220, 100)
                                    } else {
                                        egui::Color32::from_rgb(120, 120, 130)
                                    };
                                    ui.colored_label(dot_color, "●");

                                    ui.vertical(|ui| {
                                        ui.label(
                                            egui::RichText::new(name).strong().size(13.0),
                                        );
                                        ui.label(
                                            egui::RichText::new(addr)
                                                .size(11.0)
                                                .color(egui::Color32::from_rgb(140, 140, 155)),
                                        );
                                    });
                                });
                            })
                            .response;

                        if resp.clicked() {
                            action = Some(SidebarAction::Select(*id));
                        }

                        // Context menu
                        resp.context_menu(|ui| {
                            if *connected {
                                if ui.button("Disconnect").clicked() {
                                    action = Some(SidebarAction::Disconnect(*id));
                                    ui.close_menu();
                                }
                            } else if ui.button("Connect").clicked() {
                                action = Some(SidebarAction::Connect(*id));
                                ui.close_menu();
                            }
                            if ui.button("Edit").clicked() {
                                action = Some(SidebarAction::Edit(*id));
                                ui.close_menu();
                            }
                            if ui
                                .add(
                                    egui::Button::new(
                                        egui::RichText::new("Delete")
                                            .color(egui::Color32::from_rgb(220, 80, 80)),
                                    ),
                                )
                                .clicked()
                            {
                                action = Some(SidebarAction::Delete(*id));
                                ui.close_menu();
                            }
                        });

                        ui.add_space(1.0);
                    }

                    // Process action
                    if let Some(act) = action {
                        match act {
                            SidebarAction::Select(id) => {
                                app.active_proxy_id = Some(id);
                            }
                            SidebarAction::Connect(id) => {
                                app.do_connect(id);
                            }
                            SidebarAction::Disconnect(id) => {
                                app.do_disconnect(id);
                            }
                            SidebarAction::Edit(id) => {
                                if let Some(entry) =
                                    app.proxy_entries.iter().find(|e| e.id == id)
                                {
                                    app.editing_proxy_id = Some(id);
                                    app.edit_name = entry.name.clone();
                                    app.edit_host = entry.host.clone();
                                    app.edit_port = entry.port.to_string();
                                    app.edit_api_key = entry.api_key.clone();
                                    app.edit_auto_connect = entry.auto_connect;
                                    app.show_add_proxy = true;
                                }
                            }
                            SidebarAction::Delete(id) => {
                                app.remove_proxy_entry(id);
                            }
                        }
                    }
                });
            }
        });

    // Add/Edit proxy modal
    show_proxy_modal(app, ctx);
}

enum SidebarAction {
    Select(u64),
    Connect(u64),
    Disconnect(u64),
    Edit(u64),
    Delete(u64),
}

fn show_proxy_modal(app: &mut App, ctx: &egui::Context) {
    if !app.show_add_proxy {
        return;
    }

    let is_edit = app.editing_proxy_id.is_some();
    let title = if is_edit { "Edit Proxy" } else { "Add Proxy" };

    let mut open = true;
    egui::Window::new(title)
        .open(&mut open)
        .collapsible(false)
        .resizable(false)
        .anchor(egui::Align2::CENTER_CENTER, [0.0, 0.0])
        .min_width(320.0)
        .show(ctx, |ui| {
            egui::Grid::new("proxy_form_grid")
                .num_columns(2)
                .spacing([12.0, 8.0])
                .show(ui, |ui| {
                    ui.label("Name:");
                    ui.add(
                        egui::TextEdit::singleline(&mut app.edit_name)
                            .desired_width(180.0)
                            .hint_text("My Proxy"),
                    );
                    ui.end_row();

                    ui.label("Host:");
                    ui.add(
                        egui::TextEdit::singleline(&mut app.edit_host)
                            .desired_width(180.0)
                            .hint_text("127.0.0.1"),
                    );
                    ui.end_row();

                    ui.label("Port:");
                    ui.add(
                        egui::TextEdit::singleline(&mut app.edit_port)
                            .desired_width(80.0)
                            .hint_text("27961"),
                    );
                    ui.end_row();

                    ui.label("API Key:");
                    ui.add(
                        egui::TextEdit::singleline(&mut app.edit_api_key)
                            .desired_width(180.0)
                            .password(true),
                    );
                    ui.end_row();

                    ui.label("Auto-connect:");
                    ui.checkbox(&mut app.edit_auto_connect, "");
                    ui.end_row();
                });

            ui.add_space(10.0);
            ui.horizontal(|ui| {
                let btn_label = if is_edit { "Save" } else { "Add" };
                if ui
                    .add(
                        egui::Button::new(egui::RichText::new(btn_label).strong())
                            .fill(egui::Color32::from_rgb(35, 80, 50)),
                    )
                    .clicked()
                {
                    let name = app.edit_name.trim().to_string();
                    let host = app.edit_host.trim().to_string();
                    let key = app.edit_api_key.trim().to_string();
                    let port: Result<u16, _> = app.edit_port.trim().parse();

                    if name.is_empty() || host.is_empty() || key.is_empty() {
                        // Validation — silent for now (fields are visible)
                    } else if let Ok(port) = port {
                        if let Some(edit_id) = app.editing_proxy_id {
                            app.update_proxy_entry(ProxyEntry {
                                id: edit_id,
                                name,
                                host,
                                port,
                                api_key: key,
                                auto_connect: app.edit_auto_connect,
                            });
                        } else {
                            let id = config::next_id(&app.proxy_entries);
                            app.add_proxy_entry(ProxyEntry {
                                id,
                                name,
                                host,
                                port,
                                api_key: key,
                                auto_connect: app.edit_auto_connect,
                            });
                        }
                        app.show_add_proxy = false;
                    }
                }

                if ui.button("Cancel").clicked() {
                    app.show_add_proxy = false;
                }
            });
        });

    if !open {
        app.show_add_proxy = false;
    }
}
