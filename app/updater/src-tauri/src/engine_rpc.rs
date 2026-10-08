use serde_json::{json, Value};
use std::io::{BufRead, BufReader, Write};
use std::net::{SocketAddr, TcpStream};
use std::time::{Duration, Instant};

// Same newline-delimited JSON-RPC 2.0 control socket the main UI and the
// Stream Deck plugin speak (libs/ipc): 127.0.0.1:45991, one request per
// connection. The updater is just another client.
const HOST: SocketAddr = SocketAddr::V4(std::net::SocketAddrV4::new(
    std::net::Ipv4Addr::LOCALHOST,
    45991,
));

fn rpc(method: &str) -> Result<Value, String> {
    let payload = json!({
        "jsonrpc": "2.0",
        "id": 1,
        "method": method,
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

    read_response(BufReader::new(stream), 1)
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

pub fn engine_running() -> bool {
    rpc("get_status").is_ok()
}

pub fn recording() -> bool {
    rpc("get_status")
        .ok()
        .and_then(|r| r.get("result")?.get("recording")?.as_bool())
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
    // Confirm shutdown through wait_engine_exit; the RPC reply can disappear during teardown.
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
