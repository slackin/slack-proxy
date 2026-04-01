use eframe::egui;

use crate::app::App;

/// Render the log panel at the bottom of the window.
pub fn show(app: &mut App, ctx: &egui::Context) {
    egui::TopBottomPanel::bottom("log_panel")
        .resizable(true)
        .min_height(60.0)
        .default_height(120.0)
        .show(ctx, |ui| {
            ui.horizontal(|ui| {
                ui.heading("Log");
                ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                    if ui.small_button("Clear").clicked() {
                        app.log_entries.clear();
                    }
                });
            });
            ui.separator();

            let text_style = egui::TextStyle::Monospace;
            let row_height = ui.text_style_height(&text_style);

            egui::ScrollArea::vertical()
                .stick_to_bottom(true)
                .auto_shrink([false; 2])
                .show_rows(ui, row_height, app.log_entries.len(), |ui, range| {
                    for i in range {
                        if let Some((ts, msg)) = app.log_entries.get(i) {
                            ui.horizontal(|ui| {
                                ui.colored_label(
                                    egui::Color32::from_rgb(120, 120, 140),
                                    ts,
                                );
                                ui.label(
                                    egui::RichText::new(msg).monospace(),
                                );
                            });
                        }
                    }
                });
        });
}
