use std::collections::HashMap;
use std::sync::mpsc;
use std::thread;

use serde_json::Value;

use crate::net::MgmtConnection;

/// Commands sent from the GUI thread to the worker.
pub enum Command {
    Connect {
        proxy_id: u64,
        host: String,
        port: u16,
        key: String,
    },
    Disconnect {
        proxy_id: u64,
    },
    Send {
        proxy_id: u64,
        cmd_name: String,
        payload: Value,
    },
}

/// Responses sent from the worker thread back to the GUI.
pub enum Response {
    Connected {
        proxy_id: u64,
        ok: bool,
        msg: String,
    },
    Disconnected {
        proxy_id: u64,
        msg: String,
    },
    CmdResult {
        proxy_id: u64,
        cmd_name: String,
        data: Option<Value>,
    },
}

/// Spawn the background worker thread that handles all network I/O.
/// Returns a sender for commands and a receiver for responses.
pub fn spawn_worker() -> (mpsc::Sender<Command>, mpsc::Receiver<Response>) {
    let (cmd_tx, cmd_rx) = mpsc::channel::<Command>();
    let (resp_tx, resp_rx) = mpsc::channel::<Response>();

    thread::Builder::new()
        .name("mgmt-worker".into())
        .spawn(move || worker_loop(cmd_rx, resp_tx))
        .expect("Failed to spawn worker thread");

    (cmd_tx, resp_rx)
}

fn worker_loop(cmd_rx: mpsc::Receiver<Command>, resp_tx: mpsc::Sender<Response>) {
    let mut conns: HashMap<u64, MgmtConnection> = HashMap::new();

    while let Ok(cmd) = cmd_rx.recv() {
        match cmd {
            Command::Connect { proxy_id, host, port, key } => {
                match MgmtConnection::connect(&host, port, &key) {
                    Ok(c) => {
                        conns.insert(proxy_id, c);
                        let _ = resp_tx.send(Response::Connected {
                            proxy_id,
                            ok: true,
                            msg: "Connected and authenticated".to_string(),
                        });
                    }
                    Err(e) => {
                        conns.remove(&proxy_id);
                        let _ = resp_tx.send(Response::Connected {
                            proxy_id,
                            ok: false,
                            msg: e,
                        });
                    }
                }
            }
            Command::Disconnect { proxy_id } => {
                conns.remove(&proxy_id);
                let _ = resp_tx.send(Response::Disconnected {
                    proxy_id,
                    msg: "Disconnected".to_string(),
                });
            }
            Command::Send { proxy_id, cmd_name, payload } => {
                if let Some(c) = conns.get_mut(&proxy_id) {
                    match c.send_command(&payload) {
                        Ok(resp) => {
                            let _ = resp_tx.send(Response::CmdResult {
                                proxy_id,
                                cmd_name,
                                data: Some(resp),
                            });
                        }
                        Err(_e) => {
                            conns.remove(&proxy_id);
                            let _ = resp_tx.send(Response::CmdResult {
                                proxy_id,
                                cmd_name,
                                data: None,
                            });
                        }
                    }
                } else {
                    let _ = resp_tx.send(Response::CmdResult {
                        proxy_id,
                        cmd_name,
                        data: None,
                    });
                }
            }
        }
    }
}
