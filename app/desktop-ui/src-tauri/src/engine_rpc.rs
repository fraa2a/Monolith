use crate::paths;
use serde_json::{json, Value};
use std::io::{BufRead, BufReader, Write};
use std::net::TcpStream;
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::Duration;

const HOST: &str = "127.0.0.1:45991";
static NEXT_ID: AtomicU64 = AtomicU64::new(1);

// Written by the engine at startup (user-only ACL); read fresh on every call
// since the engine may restart and regenerate it while the UI stays open.
fn read_token() -> String {
    std::fs::read_to_string(paths::monolith_data_dir().join("ipc_token"))
        .unwrap_or_default()
        .trim()
        .to_string()
}

fn rpc(method: &str, params: Option<Value>) -> Result<Value, String> {
    let id = NEXT_ID.fetch_add(1, Ordering::Relaxed);
    let payload = json!({
        "jsonrpc": "2.0",
        "id": id,
        "method": method,
        "params": params,
        "token": read_token(),
    });

    let mut stream = TcpStream::connect(HOST).map_err(|err| err.to_string())?;
    let _ = stream.set_read_timeout(Some(Duration::from_secs(4)));
    let _ = stream.set_write_timeout(Some(Duration::from_secs(4)));
    writeln!(stream, "{payload}").map_err(|err| err.to_string())?;

    let mut line = String::new();
    let mut reader = BufReader::new(stream);
    reader.read_line(&mut line).map_err(|err| err.to_string())?;

    serde_json::from_str::<Value>(line.trim()).map_err(|err| err.to_string())
}

pub fn mutate_clip(method: &str, params: Value) -> Value {
    match rpc(method, Some(params)) {
        Ok(response) if response.get("error").is_none() => json!({ "ok": true }),
        Ok(response) => {
            let message = response
                .get("error")
                .and_then(|error| error.get("message"))
                .and_then(Value::as_str)
                .unwrap_or("engine returned an error");
            json!({ "ok": false, "error": message })
        }
        Err(err) => json!({ "ok": false, "error": err }),
    }
}

// Parameterless recorder commands (recording_start / recording_stop /
// save_replay). Same envelope handling as mutate_clip.
pub fn command(method: &str) -> Value {
    match rpc(method, None) {
        Ok(response) if response.get("error").is_none() => json!({ "ok": true }),
        Ok(response) => {
            let message = response
                .get("error")
                .and_then(|error| error.get("message"))
                .and_then(Value::as_str)
                .unwrap_or("engine returned an error");
            json!({ "ok": false, "error": message })
        }
        Err(err) => json!({ "ok": false, "error": err }),
    }
}

pub fn reload_settings() {
    let _ = rpc("reload_settings", None);
}

pub fn get_status() -> Value {
    match rpc("get_status", None) {
        Ok(response) if response.get("error").is_none() => {
            let mut result = response.get("result").cloned().unwrap_or_else(|| json!({}));
            if let Some(obj) = result.as_object_mut() {
                obj.insert("connected".into(), json!(true));
            }
            result
        }
        _ => json!({ "connected": false }),
    }
}

pub fn clip_generation() -> Option<u64> {
    get_status().get("clip_generation").and_then(Value::as_u64)
}
