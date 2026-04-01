use std::io::{BufRead, BufReader, Write};
use std::net::{TcpStream, ToSocketAddrs};
use std::time::Duration;

use serde_json::Value;

/// A TCP connection to the urt-proxy management API.
pub struct MgmtConnection {
    reader: BufReader<TcpStream>,
    writer: TcpStream,
}

impl MgmtConnection {
    /// Connect to the management server and authenticate with the given API key.
    pub fn connect(host: &str, port: u16, api_key: &str) -> Result<Self, String> {
        let addr_str = format!("{}:{}", host, port);
        let addr = addr_str
            .to_socket_addrs()
            .map_err(|e| format!("DNS resolution failed: {}", e))?
            .next()
            .ok_or_else(|| "No address resolved".to_string())?;

        let stream = TcpStream::connect_timeout(&addr, Duration::from_secs(5))
            .map_err(|e| format!("Connection failed: {}", e))?;

        stream
            .set_read_timeout(Some(Duration::from_secs(5)))
            .map_err(|e| format!("Set timeout failed: {}", e))?;

        let writer = stream.try_clone().map_err(|e| format!("Clone failed: {}", e))?;
        let reader = BufReader::new(stream);

        let mut conn = Self { reader, writer };

        // Authenticate
        let auth_req = serde_json::json!({ "auth": api_key });
        let resp = conn.send_recv(&auth_req)?;

        if resp.get("ok").and_then(Value::as_bool) == Some(true) {
            // Switch to no read timeout for normal operation (worker will block on reads
            // only when we explicitly send a command)
            conn.writer
                .set_read_timeout(None)
                .ok();
            Ok(conn)
        } else {
            let err = resp
                .get("error")
                .and_then(Value::as_str)
                .unwrap_or("unknown error");
            Err(format!("Auth failed: {}", err))
        }
    }

    /// Send a JSON command and read the JSON response line.
    pub fn send_command(&mut self, cmd: &Value) -> Result<Value, String> {
        // Set a read timeout for command responses
        self.writer
            .set_read_timeout(Some(Duration::from_secs(5)))
            .ok();
        let result = self.send_recv(cmd);
        self.writer.set_read_timeout(None).ok();
        result
    }

    fn send_recv(&mut self, msg: &Value) -> Result<Value, String> {
        let mut line = serde_json::to_string(msg).map_err(|e| e.to_string())?;
        line.push('\n');
        self.writer
            .write_all(line.as_bytes())
            .map_err(|e| format!("Send failed: {}", e))?;
        self.writer
            .flush()
            .map_err(|e| format!("Flush failed: {}", e))?;

        let mut buf = String::new();
        self.reader
            .read_line(&mut buf)
            .map_err(|e| format!("Read failed: {}", e))?;

        if buf.is_empty() {
            return Err("Connection closed".to_string());
        }

        serde_json::from_str(buf.trim()).map_err(|e| format!("Parse failed: {}", e))
    }
}
