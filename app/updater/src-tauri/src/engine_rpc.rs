use serde_json::{json, Value};
use std::io::{BufRead, BufReader, Write};
use std::net::TcpStream;
use std::time::{Duration, Instant};

// Same newline-delimited JSON-RPC 2.0 control socket the main UI and the
// Stream Deck plugin speak (libs/ipc): 127.0.0.1:45991, one request per
// connection. The updater is just another client.
const HOST: &str = "127.0.0.1:45991";

fn rpc(method: &str) -> Result<Value, String> {
    let payload = json!({
        "jsonrpc": "2.0",
        "id": 1,
        "method": method,
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

pub fn engine_running() -> bool {
    rpc("get_status").is_ok()
}

pub fn recording() -> bool {
    rpc("get_status")
        .ok()
        .and_then(|r| {
            r.get("result")?
                .get("recording")?
                .as_bool()
        })
        .unwrap_or(false)
}

pub fn close_ui() -> Result<(), String> {
    let r = rpc("update_close_ui")?;
    if let Some(err) = r.get("error") {
        return Err(err
            .get("message")
            .and_then(Value::as_str)
            .unwrap_or("engine returned an error")
            .to_string());
    }
    Ok(())
}

pub fn request_engine_exit() {
    // Fire and (mostly) forget: the engine tears its IPC server down during
    // shutdown, so the reply — or even the connection — may never arrive.
    // wait_engine_exit() is the real confirmation.
    let _ = rpc("update_engine_exit");
}

pub fn wait_engine_exit(timeout: Duration) -> bool {
    let start = Instant::now();
    loop {
        if !engine_running() {
            return true;
        }
        if start.elapsed() >= timeout {
            return false;
        }
        std::thread::sleep(Duration::from_millis(300));
    }
}
