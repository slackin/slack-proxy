#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod app;
mod config;
mod net;
mod protocol;
mod proxy_entry;
mod ui;
mod worker;

fn main() -> eframe::Result {
    let options = eframe::NativeOptions {
        viewport: eframe::egui::ViewportBuilder::default()
            .with_inner_size([960.0, 720.0])
            .with_min_inner_size([720.0, 540.0])
            .with_title("urt-mgmt — urt-proxy Management"),
        ..Default::default()
    };

    eframe::run_native(
        "urt-mgmt",
        options,
        Box::new(|cc| Ok(Box::new(app::App::new(cc)))),
    )
}
