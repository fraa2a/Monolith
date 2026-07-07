// Local same-origin HTTP host for the desktop UI. Serves the built Preact
// frontend (embedded from ../dist), a small JSON API over the read-only clip
// catalogs, clip mutations forwarded to the engine over JSON-RPC, and
// range-streamed media/thumbnails. The Tauri window navigates to
// http://127.0.0.1:<port>/, so the frontend's existing fetch()/`<video src>`
// route contract works unchanged. Mirrors the Deno server/router.ts. See ADR-0011.

use crate::{clip_catalog, engine_rpc, game_catalog, media, settings_store};
use clip_catalog::ClipFilter;
use include_dir::{include_dir, Dir};
use serde_json::{json, Value};
use std::collections::HashMap;
use std::sync::{Arc, OnceLock};
use std::thread;
use tauri::{AppHandle, Manager};
use tiny_http::{Header, Method, Request, Response, Server, StatusCode};

// Bundled at compile time by build.ts (`deno run -A build.ts` → ../dist). The exe
// is self-contained: no loose asset files need to ship next to it.
static DIST: Dir = include_dir!("$CARGO_MANIFEST_DIR/../dist");

// The window is decorations-less (custom title bar in the frontend). Because the
// webview loads an external http:// origin it has no Tauri JS API, so window
// controls + dragging are driven over HTTP and dispatched here via the AppHandle.
static APP: OnceLock<AppHandle> = OnceLock::new();

fn window() -> Option<tauri::WebviewWindow> {
    APP.get().and_then(|app| app.get_webview_window("main"))
}

// Number of worker threads pulling requests. A handful is plenty for a single
// local user: media streams are short-lived Range chunks and the JSON API is cheap.
const WORKERS: usize = 8;

/// Binds an ephemeral loopback port, spawns worker threads, and returns the port.
/// The server keeps running for the process lifetime (workers hold an Arc to it).
pub fn start(app: AppHandle) -> std::io::Result<u16> {
    let _ = APP.set(app);
    let server = Server::http("127.0.0.1:0")
        .map_err(|err| std::io::Error::new(std::io::ErrorKind::Other, err.to_string()))?;
    let port = server
        .server_addr()
        .to_ip()
        .map(|addr| addr.port())
        .ok_or_else(|| std::io::Error::new(std::io::ErrorKind::Other, "no bound port"))?;

    let server = Arc::new(server);
    for _ in 0..WORKERS {
        let server = Arc::clone(&server);
        thread::spawn(move || loop {
            match server.recv() {
                Ok(request) => handle(request),
                Err(_) => break,
            }
        });
    }
    Ok(port)
}

fn json_header() -> Header {
    Header::from_bytes(b"Content-Type", b"application/json; charset=utf-8").unwrap()
}

fn respond_json(request: Request, status: u16, value: &Value) {
    let body = value.to_string();
    let resp = Response::from_string(body)
        .with_status_code(StatusCode(status))
        .with_header(json_header());
    let _ = request.respond(resp);
}

fn read_body(request: &mut Request) -> Option<Value> {
    let mut body = String::new();
    request.as_reader().read_to_string(&mut body).ok()?;
    serde_json::from_str(&body).ok()
}

// Splits the raw request-target into (path, query-map). Query values are
// percent-decoded via form-urlencoded semantics.
fn split_target(target: &str) -> (String, HashMap<String, String>) {
    let (path, query) = match target.split_once('?') {
        Some((path, query)) => (path, query),
        None => (target, ""),
    };
    let params = url::form_urlencoded::parse(query.as_bytes())
        .into_owned()
        .collect();
    (path.to_string(), params)
}

fn handle(mut request: Request) {
    let method = request.method().clone();
    let (path, query) = split_target(request.url());

    match (&method, path.as_str()) {
        // ── JSON API ────────────────────────────────────────────────────────
        (Method::Get, "/api/clips") => {
            let filter = ClipFilter {
                game: query.get("game").cloned(),
                hashtag: query.get("hashtag").cloned(),
                favorite: if query.get("favorite").map(String::as_str) == Some("1") {
                    Some(true)
                } else {
                    None
                },
                search: query.get("search").cloned(),
            };
            respond_json(request, 200, &json!({ "clips": clip_catalog::list_clips(&filter) }));
        }
        (Method::Get, "/api/games") => {
            respond_json(request, 200, &json!({ "games": clip_catalog::distinct_games() }));
        }
        (Method::Get, "/api/hashtags") => {
            respond_json(request, 200, &json!({ "hashtags": clip_catalog::distinct_hashtags() }));
        }

        // ── Settings ────────────────────────────────────────────────────────
        (Method::Get, "/api/settings") => match settings_store::read_config() {
            Some(config) => respond_json(request, 200, &json!({ "config": config })),
            None => respond_json(
                request,
                200,
                &json!({ "config": null, "error": "settings unavailable (engine not run yet?)" }),
            ),
        },
        (Method::Post, "/api/settings") => {
            let body = read_body(&mut request);
            let config = body.as_ref().and_then(|b| b.get("config"));
            match config {
                Some(config) if config.is_object() => match settings_store::write_config(config) {
                    Ok(()) => {
                        engine_rpc::reload_settings();
                        respond_json(request, 200, &json!({ "ok": true }));
                    }
                    Err(err) => respond_json(
                        request,
                        500,
                        &json!({ "ok": false, "error": err.to_string() }),
                    ),
                },
                _ => respond_json(request, 400, &json!({ "ok": false, "error": "bad config" })),
            }
        }

        // ── Clip mutations (forwarded to the engine over JSON-RPC) ───────────
        (Method::Post, "/api/mutate") => {
            let body = read_body(&mut request).unwrap_or_else(|| json!({}));
            let allowed = [
                "clip_set_favorite",
                "clip_add_hashtag",
                "clip_remove_hashtag",
                "clip_rename",
                "clip_set_title",
                "clip_regen_thumb",
                "clip_delete",
            ];
            let method_name = body.get("method").and_then(Value::as_str).unwrap_or("");
            if !allowed.contains(&method_name) {
                respond_json(request, 400, &json!({ "ok": false, "error": "bad method" }));
                return;
            }
            let params = json!({
                "source": body.get("source").and_then(Value::as_str).unwrap_or("replay"),
                "id": body.get("id").and_then(Value::as_i64).unwrap_or(0),
                "tag": body.get("tag"),
                "favorite": body.get("favorite"),
                "new_name": body.get("new_name"),
                "title": body.get("title"),
            });
            let result = engine_rpc::mutate_clip(method_name, params);
            let ok = result.get("ok").and_then(Value::as_bool).unwrap_or(false);
            respond_json(request, if ok { 200 } else { 500 }, &result);
        }

        (Method::Get, "/api/runtime-status") => {
            respond_json(request, 200, &json!({ "status": settings_store::read_runtime_status() }));
        }

        // ── Native folder picker (directory settings never use manual entry) ──
        (Method::Post, "/api/pick-folder") => {
            let body = read_body(&mut request).unwrap_or_else(|| json!({}));
            let start = body.get("current").and_then(Value::as_str).unwrap_or("");
            let mut dialog = rfd::FileDialog::new();
            if !start.is_empty() && std::path::Path::new(start).is_dir() {
                dialog = dialog.set_directory(start);
            }
            match dialog.pick_folder() {
                Some(path) => respond_json(
                    request,
                    200,
                    &json!({ "ok": true, "path": path.to_string_lossy() }),
                ),
                None => respond_json(request, 200, &json!({ "ok": false })), // cancelled
            }
        }

        // ── Live clip-list updates (Server-Sent Events) ─────────────────────
        // Streams a `clips` event whenever the engine's clip-generation counter
        // changes, so the library refreshes in real time after a save.
        (Method::Get, "/api/events") => {
            serve_events(request);
        }

        (Method::Get, "/api/game-catalog") => {
            respond_json(request, 200, &json!({ "catalog": game_catalog::catalog_map() }));
        }
        (Method::Get, "/api/game-icon") => {
            let icon = query
                .get("process")
                .and_then(|proc| game_catalog::resolve_icon(proc));
            respond_json(request, 200, &json!({ "icon": icon }));
        }

        // ── Custom window chrome (decorations-less window) ──────────────────
        (Method::Post, "/api/window/minimize") => {
            if let Some(w) = window() {
                let _ = w.minimize();
            }
            respond_json(request, 200, &json!({ "ok": true }));
        }
        (Method::Post, "/api/window/toggle-maximize") => {
            let maximized = window()
                .map(|w| {
                    let is_max = w.is_maximized().unwrap_or(false);
                    let _ = if is_max { w.unmaximize() } else { w.maximize() };
                    !is_max
                })
                .unwrap_or(false);
            respond_json(request, 200, &json!({ "ok": true, "maximized": maximized }));
        }
        (Method::Post, "/api/window/close") => {
            if let Some(w) = window() {
                let _ = w.close();
            }
            respond_json(request, 200, &json!({ "ok": true }));
        }
        (Method::Post, "/api/window/drag") => {
            // start_dragging() hands off to the OS move-loop, so one call on
            // mousedown is enough — no continuous messaging needed.
            if let Some(w) = window() {
                let _ = w.start_dragging();
            }
            respond_json(request, 200, &json!({ "ok": true }));
        }

        // ── Media (Range-streamed) ──────────────────────────────────────────
        (Method::Get, _) if path.starts_with("/media/") || path.starts_with("/thumb/") => {
            serve_media_route(request, &path);
        }

        // ── Static frontend ─────────────────────────────────────────────────
        (Method::Get, _) => serve_static(request, &path),

        _ => respond_json(request, 404, &json!({ "error": "not found" })),
    }
}

// Holds an SSE connection open and pushes a `clips` event whenever the engine's
// clip-generation counter advances (a clip was saved/stopped). Polls the engine
// over the existing RPC channel on a light cadence — cheap for a single local
// user and avoids adding a push channel to the engine's line-oriented protocol.
// The loop exits when the socket write fails (webview navigated away/closed).
fn serve_events(request: Request) {
    use std::io::Write;
    use std::time::Duration;

    // Move the socket to a dedicated thread so the long-lived stream never ties
    // up one of the small worker pool's threads.
    let mut writer = request.into_writer();
    thread::spawn(move || {
        // Minimal SSE response head. `retry` tells EventSource the reconnect delay.
        let head = "HTTP/1.1 200 OK\r\n\
                    Content-Type: text/event-stream\r\n\
                    Cache-Control: no-cache\r\n\
                    Connection: keep-alive\r\n\
                    \r\n\
                    retry: 2000\n\n";
        if writer.write_all(head.as_bytes()).is_err() {
            return;
        }
        let _ = writer.flush();

        // Seed with the current generation so we only fire on genuine changes.
        let mut last = engine_rpc::clip_generation();
        let mut since_ping = 0u32;
        loop {
            thread::sleep(Duration::from_millis(1000));
            if let Some(gen) = engine_rpc::clip_generation() {
                if last != Some(gen) {
                    last = Some(gen);
                    if writer.write_all(b"event: clips\ndata: {}\n\n").is_err() {
                        return;
                    }
                    let _ = writer.flush();
                    since_ping = 0;
                    continue;
                }
            }
            // Periodic comment keeps the connection alive and surfaces a dead
            // socket promptly (write fails once the webview has gone away).
            since_ping += 1;
            if since_ping >= 15 {
                since_ping = 0;
                if writer.write_all(b": ping\n\n").is_err() {
                    return;
                }
                let _ = writer.flush();
            }
        }
    });
}

// Routes `/media/<source>/<file>` and `/thumb/<source>/<file>`; the file segment
// may be percent-encoded (encodeURIComponent on the frontend).
fn serve_media_route(request: Request, path: &str) {
    let is_thumb = path.starts_with("/thumb/");
    let rest = &path[if is_thumb { "/thumb/".len() } else { "/media/".len() }..];
    let Some((source_str, file_enc)) = rest.split_once('/') else {
        let _ = request.respond(Response::from_string("Not found").with_status_code(StatusCode(404)));
        return;
    };
    let Some(source) = media::Source::parse(source_str) else {
        let _ = request.respond(Response::from_string("Not found").with_status_code(StatusCode(404)));
        return;
    };
    let file = percent_encoding::percent_decode_str(file_enc)
        .decode_utf8_lossy()
        .to_string();
    if is_thumb {
        media::serve_thumb(request, &source, &file);
    } else {
        media::serve_media(request, &source, &file);
    }
}

fn mime_for(path: &str) -> &'static str {
    match path.rsplit('.').next().map(str::to_lowercase).as_deref() {
        Some("html") => "text/html; charset=utf-8",
        Some("js") => "text/javascript; charset=utf-8",
        Some("css") => "text/css; charset=utf-8",
        Some("json") => "application/json; charset=utf-8",
        Some("png") => "image/png",
        Some("svg") => "image/svg+xml",
        Some("ico") => "image/x-icon",
        Some("woff2") => "font/woff2",
        _ => "application/octet-stream",
    }
}

// Serves an embedded asset; unknown non-asset routes fall back to index.html
// (SPA behaviour), matching the Deno router.
fn serve_static(request: Request, path: &str) {
    let rel = if path == "/" {
        "index.html"
    } else {
        path.trim_start_matches('/')
    };

    if let Some(file) = DIST.get_file(rel) {
        let header = Header::from_bytes(b"Content-Type", mime_for(rel).as_bytes()).unwrap();
        let resp = Response::from_data(file.contents()).with_header(header);
        let _ = request.respond(resp);
        return;
    }

    match DIST.get_file("index.html") {
        Some(index) => {
            let header = Header::from_bytes(b"Content-Type", b"text/html; charset=utf-8").unwrap();
            let resp = Response::from_data(index.contents()).with_header(header);
            let _ = request.respond(resp);
        }
        None => {
            let resp = Response::from_string("UI not built (run `deno task ui:build`).")
                .with_status_code(StatusCode(500));
            let _ = request.respond(resp);
        }
    }
}
