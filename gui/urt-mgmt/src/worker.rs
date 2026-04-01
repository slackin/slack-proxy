use std::sync::mpsc;
use std::thread;

use serde_json::Value;

use crate::net::MgmtConnection;

/// Commands sent from the GUI thread to the worker.
pub enum Command {
    Connect {
        host: String,
        port: u16,
        key: String,
    },
    Disconnect,
    Send {
        cmd_name: String,
        payload: Value,
    },
}

/// Responses sent from the worker thread back to the GUI.
pub enum Response {
    Connected {
        ok: bool,
        msg: String,
    },
    Disconnected {
        msg: String,
    },
    CmdResult {
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
    let mut conn: Option<MgmtConnection> = None;

    while let Ok(cmd) = cmd_rx.recv() {
        match cmd {
            Command::Connect { host, port, key } => {
                match MgmtConnection::connect(&host, port, &key) {
                    Ok(c) => {
                        conn = Some(c);
                        let _ = resp_tx.send(Response::Connected {
                            ok: true,
                            msg: "Connected and authenticated".to_string(),
                        });
                    }
                    Err(e) => {
                        conn = None;
                        let _ = resp_tx.send(Response::Connected {
                            ok: false,
                            msg: e,
                        });
                    }
                }
            }
            Command::Disconnect => {
                conn = None;
                let _ = resp_tx.send(Response::Disconnected {
                    msg: "Disconnected".to_string(),
                });
            }
            Command::Send { cmd_name, payload } => {
                if let Some(ref mut c) = conn {
                    match c.send_command(&payload) {
                        Ok(resp) => {
                            let _ = resp_tx.send(Response::CmdResult {
                                cmd_name,
                                data: Some(resp),
                            });
                        }
                        Err(_e) => {
                            // Connection likely dead
                            conn = None;
                            let _ = resp_tx.send(Response::CmdResult {
                                cmd_name,
                                data: None,
                            });
                        }
                    }
                } else {
                    let _ = resp_tx.send(Response::CmdResult {
                        cmd_name,
                        data: None,
                    });
                }
            }
        }
    }
}
