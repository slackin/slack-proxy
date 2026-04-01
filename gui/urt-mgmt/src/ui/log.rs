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
                        if let Some(state) = app.active_state_mut() {
                            state.log_entries.clear();
                        }
                    }
                });
            });
            ui.separator();

            let log_len = app
                .active_state()
                .map(|s| s.log_entries.len())
                .unwrap_or(0);

            let text_style = egui::TextStyle::Monospace;
            let row_height = ui.text_style_height(&text_style);

            egui::ScrollArea::vertical()
                .stick_to_bottom(true)
                .auto_shrink([false; 2])
                .show_rows(ui, row_height, log_len, |ui, range| {
                    if let Some(state) = app.active_state() {
                        for i in range {
                            if let Some((ts, msg)) = state.log_entries.get(i) {
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
                    }
                });
        });
}
