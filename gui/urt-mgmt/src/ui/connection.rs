use eframe::egui;

use crate::app::App;

/// Render the connection bar at the top of the window.
pub fn show(app: &mut App, ctx: &egui::Context) {
    egui::TopBottomPanel::top("connection_panel").show(ctx, |ui| {
        ui.add_space(4.0);
        ui.horizontal(|ui| {
            ui.spacing_mut().item_spacing.x = 8.0;

            if let Some(entry) = app.active_entry().cloned() {
                let connected = app
                    .active_state()
                    .map(|s| s.connected)
                    .unwrap_or(false);

                ui.label(
                    egui::RichText::new(&entry.name).strong().size(14.0),
                );
                ui.separator();
                ui.label(
                    egui::RichText::new(entry.display_addr())
                        .monospace()
                        .size(13.0),
                );

                ui.separator();

                if connected {
                    if ui
                        .add(
                            egui::Button::new("Disconnect")
                                .fill(egui::Color32::from_rgb(140, 40, 40)),
                        )
                        .clicked()
                    {
                        app.do_disconnect(entry.id);
                    }
                } else if ui
                    .add(
                        egui::Button::new("  Connect  ")
                            .fill(egui::Color32::from_rgb(40, 110, 60)),
                    )
                    .clicked()
                {
                    app.do_connect(entry.id);
                }

                ui.add_space(8.0);

                // Status indicator
                let (color, text) = if connected {
                    (egui::Color32::from_rgb(80, 220, 100), "● Connected")
                } else {
                    (egui::Color32::from_rgb(200, 60, 60), "● Disconnected")
                };
                ui.colored_label(color, text);
            } else {
                ui.label(
                    egui::RichText::new("No proxy selected — add one in the sidebar")
                        .color(egui::Color32::from_rgb(160, 160, 170)),
                );
            }
        });
        ui.add_space(4.0);
    });
}
