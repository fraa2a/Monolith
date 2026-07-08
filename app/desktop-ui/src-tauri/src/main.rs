// Monolith Desktop UI host (Tauri v2 / WebView2). Replaces the experimental Deno
// Desktop shell (which could not reliably open a visible window). The Preact
// frontend is unchanged: a local loopback HTTP server (server.rs) serves it plus
// the /api, /media and /thumb routes, and the window navigates to it. The engine
// (Monolith.exe) is a separate process reached only over JSON-RPC on
// 127.0.0.1:45991 for clip mutations / settings reload. See ADR-0011.
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod clip_catalog;
mod engine_rpc;
mod exe_icon;
mod game_catalog;
mod media;
mod paths;
mod server;
mod settings_store;

use tauri::{WebviewUrl, WebviewWindowBuilder};

fn main() {
    tauri::Builder::default()
        .setup(|app| {
            // Bring the local host up first, then point the window at it. Building
            // the window explicitly (rather than via a config-declared window that
            // loads the bundled assets) guarantees a visible WebView2 window — the
            // exact failure mode that motivated leaving Deno Desktop.
            let port = server::start(app.handle().clone())?;
            let url = format!("http://127.0.0.1:{port}/");
            // decorations(false): the frontend draws its own title bar; window
            // controls + dragging round-trip to the HTTP server (see server.rs).
            WebviewWindowBuilder::new(app, "main", WebviewUrl::External(url.parse().unwrap()))
                .title("Monolith")
                .inner_size(1240.0, 820.0)
                .min_inner_size(940.0, 620.0)
                .decorations(false)
                .shadow(true)
                .resizable(true)
                .center()
                .build()?;
            Ok(())
        })
        .run(tauri::generate_context!())
        .expect("failed to run Monolith UI");
}
