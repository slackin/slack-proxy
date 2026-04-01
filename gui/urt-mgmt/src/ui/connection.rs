use eframe::egui;

use crate::app::App;

/// Render the connection bar at the top of the window.
pub fn show(app: &mut App, ctx: &egui::Context) {
    egui::TopBottomPanel::top("connection_panel").show(ctx, |ui| {
        ui.add_space(4.0);
        ui.horizontal(|ui| {
            ui.spacing_mut().item_spacing.x = 8.0;

            ui.label("Host:");
            ui.add(
                egui::TextEdit::singleline(&mut app.host)
                    .desired_width(140.0)
                    .hint_text("127.0.0.1"),
            );

            ui.label("Port:");
            ui.add(
                egui::TextEdit::singleline(&mut app.port)
                    .desired_width(60.0)
                    .hint_text("27961"),
            );

            ui.label("API Key:");
            ui.add(
                egui::TextEdit::singleline(&mut app.api_key)
                    .desired_width(180.0)
                    .password(true),
            );

            if app.connected {
                if ui
                    .add(egui::Button::new("Disconnect").fill(egui::Color32::from_rgb(140, 40, 40)))
                    .clicked()
                {
                    app.do_disconnect();
                }
            } else if ui
                .add(egui::Button::new("  Connect  ").fill(egui::Color32::from_rgb(40, 110, 60)))
                .clicked()
            {
                app.do_connect();
            }

            ui.add_space(8.0);

            // Status indicator
            let (color, text) = if app.connected {
                (egui::Color32::from_rgb(80, 220, 100), "● Connected")
            } else {
                (egui::Color32::from_rgb(200, 60, 60), "● Disconnected")
            };
            ui.colored_label(color, text);
        });
        ui.add_space(4.0);
    });
}
