use serde_json::{json, Value};
use std::io::{BufRead, BufReader, Write};
use std::net::TcpStream;
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::Duration;

const HOST: &str = "127.0.0.1:45991";
static NEXT_ID: AtomicU64 = AtomicU64::new(1);

fn rpc(method: &str, params: Option<Value>) -> Result<Value, String> {
    let id = NEXT_ID.fetch_add(1, Ordering::Relaxed);
    let payload = json!({
        "jsonrpc": "2.0",
        "id": id,
        "method": method,
        "params": params,
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

pub fn reload_settings() {
    let _ = rpc("reload_settings", None);
}

pub fn get_status() -> Value {
    match rpc("get_status", None) {
        Ok(response) if response.get("error").is_none() => {
            response.get("result").cloned().unwrap_or_else(|| json!({}))
        }
        _ => json!({}),
    }
}

pub fn clip_generation() -> Option<u64> {
    get_status().get("clip_generation").and_then(Value::as_u64)
}
