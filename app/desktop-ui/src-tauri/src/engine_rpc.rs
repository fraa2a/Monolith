use serde_json::{json, Value};
use std::io::{BufRead, BufReader, Write};
use std::net::{SocketAddr, TcpStream};
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::Duration;

const HOST: SocketAddr = SocketAddr::V4(std::net::SocketAddrV4::new(
    std::net::Ipv4Addr::LOCALHOST,
    45991,
));
static NEXT_ID: AtomicU64 = AtomicU64::new(1);

fn rpc(method: &str, params: Option<Value>) -> Result<Value, String> {
    let id = NEXT_ID.fetch_add(1, Ordering::Relaxed);
    let payload = json!({
        "jsonrpc": "2.0",
        "id": id,
        "method": method,
        "params": params,
    });

    let mut stream =
        TcpStream::connect_timeout(&HOST, Duration::from_secs(4)).map_err(|err| err.to_string())?;
    stream
        .set_read_timeout(Some(Duration::from_secs(4)))
        .map_err(|err| err.to_string())?;
    stream
        .set_write_timeout(Some(Duration::from_secs(4)))
        .map_err(|err| err.to_string())?;
    writeln!(stream, "{payload}").map_err(|err| err.to_string())?;

    read_response(BufReader::new(stream), id)
}

fn read_response(reader: impl BufRead, id: u64) -> Result<Value, String> {
    let mut line = Vec::new();
    reader
        .take(64 * 1024 + 1)
        .read_until(b'\n', &mut line)
        .map_err(|err| err.to_string())?;
    if line.len() > 64 * 1024 || line.last() != Some(&b'\n') {
        return Err("invalid or oversized engine response".into());
    }
    let value: Value = serde_json::from_slice(&line).map_err(|err| err.to_string())?;
    if value.get("jsonrpc").and_then(Value::as_str) != Some("2.0")
        || value.get("id").and_then(Value::as_u64) != Some(id)
        || value.get("result").is_some() == value.get("error").is_some()
    {
        return Err("invalid engine response envelope".into());
    }
    Ok(value)
}

#[cfg(test)]
mod tests {
    use super::read_response;
    use std::io::Cursor;
    #[test]
    fn response_envelope_and_size_are_bounded() {
        assert!(read_response(
            Cursor::new(b"{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":null}\n"),
            1
        )
        .is_ok());
        assert!(read_response(
            Cursor::new(b"{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":null}\n"),
            1
        )
        .is_err());
        assert!(read_response(Cursor::new(b"{\"id\":1,\"result\":null}\n"), 1).is_err());
        assert!(read_response(
            Cursor::new(b"{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":null,\"error\":null}\n"),
            1
        )
        .is_err());
        assert!(read_response(Cursor::new(vec![b'x'; 65537]), 1).is_err());
        assert!(read_response(Cursor::new(b"{}"), 1).is_err());
    }
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

pub fn reload_settings() -> Result<(), String> {
    let response = rpc("reload_settings", None)?;
    if let Some(error) = response.get("error") {
        let message = error
            .get("message")
            .and_then(Value::as_str)
            .unwrap_or("engine returned an error");
        return Err(message.to_string());
    }
    Ok(())
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
