#!/usr/bin/env python3
"""
urt-mgmt — Cross-platform GUI client for the urt-proxy management API.

Connects to the urt-proxy management TCP port and provides a graphical
interface for monitoring server status, tuning runtime parameters, and
managing client sessions.

Requirements: Python 3.6+, tkinter (included in standard Python installs).
No pip dependencies required.

Usage:
    python3 gui/urt-mgmt.py
"""

import json
import socket
import threading
import queue
import time
import tkinter as tk
from tkinter import ttk, messagebox, scrolledtext

# ---------------------------------------------------------------------------
#  Network layer — runs in a background thread
# ---------------------------------------------------------------------------

class MgmtConnection:
    """TCP connection to the urt-proxy management API."""

    def __init__(self):
        self.sock = None
        self.lock = threading.Lock()
        self.recv_buf = ""

    def connect(self, host, port, api_key):
        """Connect and authenticate. Returns (True, msg) or (False, err)."""
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.settimeout(5.0)
            s.connect((host, int(port)))
            self.sock = s
            self.recv_buf = ""

            # Authenticate
            resp = self._send_recv(json.dumps({"auth": api_key}))
            if resp and resp.get("ok"):
                return True, "Connected and authenticated"
            else:
                err = resp.get("error", "unknown error") if resp else "no response"
                self.disconnect()
                return False, f"Auth failed: {err}"
        except Exception as e:
            self.disconnect()
            return False, str(e)

    def disconnect(self):
        with self.lock:
            if self.sock:
                try:
                    self.sock.close()
                except OSError:
                    pass
                self.sock = None

    @property
    def connected(self):
        return self.sock is not None

    def send_command(self, cmd_dict):
        """Send a command dict and return the parsed response, or None."""
        return self._send_recv(json.dumps(cmd_dict))

    def _send_recv(self, line):
        with self.lock:
            if not self.sock:
                return None
            try:
                self.sock.sendall((line + "\n").encode("utf-8"))
                # Read until newline
                while "\n" not in self.recv_buf:
                    chunk = self.sock.recv(8192)
                    if not chunk:
                        return None
                    self.recv_buf += chunk.decode("utf-8", errors="replace")
                nl = self.recv_buf.index("\n")
                msg = self.recv_buf[:nl]
                self.recv_buf = self.recv_buf[nl + 1:]
                return json.loads(msg)
            except Exception:
                return None


# ---------------------------------------------------------------------------
#  GUI Application
# ---------------------------------------------------------------------------

class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("urt-mgmt — urt-proxy Management")
        self.geometry("900x700")
        self.minsize(700, 500)

        self.conn = MgmtConnection()
        self.cmd_queue = queue.Queue()
        self.resp_queue = queue.Queue()
        self.auto_refresh = True
        self.server_data = []     # Latest status response
        self.session_data = {}    # server_idx -> sessions list

        self._build_ui()
        self._start_worker()
        self._poll_responses()

    # ---------------------------------------------------------------
    #  UI construction
    # ---------------------------------------------------------------

    def _build_ui(self):
        # --- Connection panel ---
        conn_frame = ttk.LabelFrame(self, text="Connection", padding=5)
        conn_frame.pack(fill=tk.X, padx=5, pady=(5, 2))

        ttk.Label(conn_frame, text="Host:").grid(row=0, column=0, sticky=tk.W)
        self.host_var = tk.StringVar(value="127.0.0.1")
        ttk.Entry(conn_frame, textvariable=self.host_var, width=18).grid(
            row=0, column=1, padx=2)

        ttk.Label(conn_frame, text="Port:").grid(row=0, column=2, sticky=tk.W)
        self.port_var = tk.StringVar(value="27961")
        ttk.Entry(conn_frame, textvariable=self.port_var, width=7).grid(
            row=0, column=3, padx=2)

        ttk.Label(conn_frame, text="API Key:").grid(row=0, column=4, sticky=tk.W)
        self.key_var = tk.StringVar()
        ttk.Entry(conn_frame, textvariable=self.key_var, width=24,
                  show="*").grid(row=0, column=5, padx=2)

        self.connect_btn = ttk.Button(conn_frame, text="Connect",
                                      command=self._on_connect)
        self.connect_btn.grid(row=0, column=6, padx=(8, 2))

        self.status_label = ttk.Label(conn_frame, text="Disconnected",
                                      foreground="red")
        self.status_label.grid(row=0, column=7, padx=8)

        # --- Notebook (one tab per server, created dynamically) ---
        self.notebook = ttk.Notebook(self)
        self.notebook.pack(fill=tk.BOTH, expand=True, padx=5, pady=2)

        # --- Log panel ---
        log_frame = ttk.LabelFrame(self, text="Log", padding=2)
        log_frame.pack(fill=tk.X, padx=5, pady=(2, 5))
        self.log_text = scrolledtext.ScrolledText(log_frame, height=5,
                                                  state=tk.DISABLED,
                                                  font=("Consolas", 9))
        self.log_text.pack(fill=tk.X)

    def _create_server_tab(self, idx, info):
        """Create a notebook tab for one server."""
        frame = ttk.Frame(self.notebook)
        self.notebook.add(frame, text=f"Server #{idx + 1}  :{info['listen_port']}")

        # --- Config summary ---
        cfg_frame = ttk.LabelFrame(frame, text="Configuration", padding=5)
        cfg_frame.pack(fill=tk.X, padx=5, pady=5)

        labels = {}
        fields = [
            ("Listen Port", str(info["listen_port"])),
            ("Remote", info["remote_addr"]),
            ("Hostname Tag", info.get("hostname_tag", "")),
        ]
        for r, (name, val) in enumerate(fields):
            ttk.Label(cfg_frame, text=f"{name}:").grid(row=r, column=0,
                                                       sticky=tk.W, padx=2)
            lbl = ttk.Label(cfg_frame, text=val)
            lbl.grid(row=r, column=1, sticky=tk.W, padx=4)

        # --- Tunables ---
        tune_frame = ttk.LabelFrame(frame, text="Tune Parameters", padding=5)
        tune_frame.pack(fill=tk.X, padx=5, pady=2)

        tunables = [
            ("max_clients", "Max Clients"),
            ("session_timeout", "Session Timeout"),
            ("query_timeout", "Query Timeout"),
            ("max_new_per_sec", "Rate Limit"),
            ("max_query_sessions", "Max Query Sessions"),
        ]

        tune_vars = {}
        for r, (key, label) in enumerate(tunables):
            ttk.Label(tune_frame, text=f"{label}:").grid(row=r, column=0,
                                                         sticky=tk.W, padx=2)
            var = tk.StringVar(value=str(info.get(key, "")))
            entry = ttk.Entry(tune_frame, textvariable=var, width=10)
            entry.grid(row=r, column=1, padx=4)
            tune_vars[key] = var

            apply_btn = ttk.Button(
                tune_frame, text="Apply",
                command=lambda k=key, v=var, i=idx: self._apply_tune(i, k, v))
            apply_btn.grid(row=r, column=2, padx=2)

        # Hostname tag (string tunable)
        r = len(tunables)
        ttk.Label(tune_frame, text="Hostname Tag:").grid(row=r, column=0,
                                                         sticky=tk.W, padx=2)
        tag_var = tk.StringVar(value=info.get("hostname_tag", ""))
        ttk.Entry(tune_frame, textvariable=tag_var, width=20).grid(
            row=r, column=1, padx=4)
        ttk.Button(
            tune_frame, text="Apply",
            command=lambda v=tag_var, i=idx: self._apply_tune_str(
                i, "hostname_tag", v)
        ).grid(row=r, column=2, padx=2)
        tune_vars["hostname_tag"] = tag_var

        # --- Session table ---
        sess_frame = ttk.LabelFrame(frame, text="Sessions", padding=5)
        sess_frame.pack(fill=tk.BOTH, expand=True, padx=5, pady=5)

        columns = ("client", "query", "idle", "pkts_up", "pkts_dn",
                   "bytes_up", "bytes_dn")
        tree = ttk.Treeview(sess_frame, columns=columns, show="headings",
                            height=8)
        tree.heading("client", text="Client")
        tree.heading("query", text="Query?")
        tree.heading("idle", text="Idle (s)")
        tree.heading("pkts_up", text="Pkts \u2191")
        tree.heading("pkts_dn", text="Pkts \u2193")
        tree.heading("bytes_up", text="Bytes \u2191")
        tree.heading("bytes_dn", text="Bytes \u2193")
        tree.column("client", width=160)
        tree.column("query", width=50, anchor=tk.CENTER)
        tree.column("idle", width=60, anchor=tk.E)
        tree.column("pkts_up", width=80, anchor=tk.E)
        tree.column("pkts_dn", width=80, anchor=tk.E)
        tree.column("bytes_up", width=90, anchor=tk.E)
        tree.column("bytes_dn", width=90, anchor=tk.E)

        scrollbar = ttk.Scrollbar(sess_frame, orient=tk.VERTICAL,
                                  command=tree.yview)
        tree.configure(yscrollcommand=scrollbar.set)
        tree.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        scrollbar.pack(side=tk.RIGHT, fill=tk.Y)

        # --- Kick buttons ---
        btn_frame = ttk.Frame(frame)
        btn_frame.pack(fill=tk.X, padx=5, pady=(0, 5))

        ttk.Button(
            btn_frame, text="Kick Selected",
            command=lambda t=tree, i=idx: self._kick_selected(i, t)
        ).pack(side=tk.LEFT, padx=4)

        ttk.Button(
            btn_frame, text="Kick All",
            command=lambda i=idx: self._kick_all(i)
        ).pack(side=tk.LEFT, padx=4)

        count_lbl = ttk.Label(btn_frame, text="0 sessions")
        count_lbl.pack(side=tk.RIGHT, padx=8)

        return {"frame": frame, "tree": tree, "tune_vars": tune_vars,
                "count_lbl": count_lbl}

    # ---------------------------------------------------------------
    #  Background worker thread
    # ---------------------------------------------------------------

    def _start_worker(self):
        t = threading.Thread(target=self._worker_loop, daemon=True)
        t.start()

    def _worker_loop(self):
        while True:
            try:
                tag, payload = self.cmd_queue.get(timeout=0.1)
            except queue.Empty:
                continue

            if tag == "connect":
                host, port, key = payload
                ok, msg = self.conn.connect(host, port, key)
                self.resp_queue.put(("connect", ok, msg))
            elif tag == "disconnect":
                self.conn.disconnect()
                self.resp_queue.put(("disconnect", True, "Disconnected"))
            elif tag == "cmd":
                resp = self.conn.send_command(payload)
                self.resp_queue.put(("cmd", payload.get("cmd"), resp))

    # ---------------------------------------------------------------
    #  Response polling (runs on the main thread via after())
    # ---------------------------------------------------------------

    def _poll_responses(self):
        try:
            while True:
                tag, *args = self.resp_queue.get_nowait()
                if tag == "connect":
                    ok, msg = args
                    if ok:
                        self.status_label.config(text="Connected",
                                                 foreground="green")
                        self.connect_btn.config(text="Disconnect")
                        self._log(msg)
                        self._request_status()
                    else:
                        self.status_label.config(text="Disconnected",
                                                 foreground="red")
                        self._log(f"Connection failed: {msg}")
                elif tag == "disconnect":
                    self.status_label.config(text="Disconnected",
                                             foreground="red")
                    self.connect_btn.config(text="Connect")
                    self._clear_tabs()
                    self._log("Disconnected")
                elif tag == "cmd":
                    cmd_name, resp = args
                    self._handle_cmd_response(cmd_name, resp)
        except queue.Empty:
            pass

        self.after(100, self._poll_responses)

    # ---------------------------------------------------------------
    #  Command handling
    # ---------------------------------------------------------------

    def _on_connect(self):
        if self.conn.connected:
            self.cmd_queue.put(("disconnect", None))
        else:
            host = self.host_var.get().strip()
            port = self.port_var.get().strip()
            key = self.key_var.get().strip()
            if not host or not port or not key:
                messagebox.showwarning("Missing fields",
                                       "Host, Port, and API Key are required.")
                return
            self._log(f"Connecting to {host}:{port}...")
            self.cmd_queue.put(("connect", (host, port, key)))

    def _request_status(self):
        if self.conn.connected:
            self.cmd_queue.put(("cmd", {"cmd": "status"}))

    def _request_sessions(self, server_idx):
        if self.conn.connected:
            self.cmd_queue.put(("cmd", {"cmd": "sessions",
                                        "server": server_idx}))

    def _handle_cmd_response(self, cmd_name, resp):
        if not resp:
            self._log(f"No response for '{cmd_name}' — connection lost?")
            return

        if not resp.get("ok"):
            self._log(f"Error ({cmd_name}): {resp.get('error', 'unknown')}")
            return

        data = resp.get("data", {})

        if cmd_name == "status":
            self._update_status(data)
            # Auto-request sessions for the active tab
            active = self.notebook.index("current") if self.notebook.tabs() else 0
            if active < len(self.server_data):
                self._request_sessions(active)
            # Schedule next auto-refresh
            if self.auto_refresh and self.conn.connected:
                self.after(2000, self._request_status)

        elif cmd_name == "sessions":
            self._update_sessions(data)

        elif cmd_name in ("set", "kick", "kick_all"):
            self._log(f"{cmd_name}: OK")
            self._request_status()

    # ---------------------------------------------------------------
    #  UI update helpers
    # ---------------------------------------------------------------

    def _update_status(self, data):
        servers = data.get("servers", [])
        self.server_data = servers

        # Rebuild tabs if server count changed
        if len(servers) != self.notebook.index("end"):
            self._clear_tabs()
            self.tab_data = []
            for i, srv in enumerate(servers):
                td = self._create_server_tab(i, srv)
                self.tab_data.append(td)
        else:
            # Update tune var defaults and session counts
            for i, srv in enumerate(servers):
                td = self.tab_data[i]
                for key in ("max_clients", "session_timeout", "query_timeout",
                            "max_new_per_sec", "max_query_sessions"):
                    if key in td["tune_vars"]:
                        td["tune_vars"][key].set(str(srv.get(key, "")))
                if "hostname_tag" in td["tune_vars"]:
                    td["tune_vars"]["hostname_tag"].set(
                        srv.get("hostname_tag", ""))
                td["count_lbl"].config(
                    text=f"{srv.get('active_sessions', 0)} sessions, "
                         f"{srv.get('query_sessions', 0)} queries")

    def _update_sessions(self, data):
        server_idx = data.get("server", 0)
        sessions = data.get("sessions", [])
        self.session_data[server_idx] = sessions

        if server_idx >= len(getattr(self, "tab_data", [])):
            return

        tree = self.tab_data[server_idx]["tree"]
        # Clear existing rows
        for item in tree.get_children():
            tree.delete(item)

        for s in sessions:
            tree.insert("", tk.END, values=(
                s.get("client", ""),
                "Yes" if s.get("is_query") else "No",
                s.get("idle_secs", ""),
                s.get("pkts_to_server", 0),
                s.get("pkts_to_client", 0),
                s.get("bytes_to_server", 0),
                s.get("bytes_to_client", 0),
            ))

    def _clear_tabs(self):
        for tab_id in self.notebook.tabs():
            self.notebook.forget(tab_id)
        self.tab_data = []
        self.session_data = {}

    # ---------------------------------------------------------------
    #  Actions
    # ---------------------------------------------------------------

    def _apply_tune(self, server_idx, key, var):
        try:
            val = int(var.get())
        except ValueError:
            messagebox.showwarning("Invalid value", "Enter an integer.")
            return
        self.cmd_queue.put(("cmd", {
            "cmd": "set", "server": server_idx,
            "key": key, "value": val
        }))
        self._log(f"Setting server #{server_idx + 1} {key} = {val}")

    def _apply_tune_str(self, server_idx, key, var):
        val = var.get()
        self.cmd_queue.put(("cmd", {
            "cmd": "set", "server": server_idx,
            "key": key, "value": val
        }))
        self._log(f"Setting server #{server_idx + 1} {key} = \"{val}\"")

    def _kick_selected(self, server_idx, tree):
        sel = tree.selection()
        if not sel:
            messagebox.showinfo("No selection", "Select a session to kick.")
            return
        for item in sel:
            vals = tree.item(item, "values")
            client = vals[0]
            self.cmd_queue.put(("cmd", {
                "cmd": "kick", "server": server_idx, "client": client
            }))
            self._log(f"Kicking {client} on server #{server_idx + 1}")

    def _kick_all(self, server_idx):
        if not messagebox.askyesno(
                "Confirm", f"Kick ALL sessions on server #{server_idx + 1}?"):
            return
        self.cmd_queue.put(("cmd", {
            "cmd": "kick_all", "server": server_idx
        }))
        self._log(f"Kicking all sessions on server #{server_idx + 1}")

    # ---------------------------------------------------------------
    #  Logging
    # ---------------------------------------------------------------

    def _log(self, msg):
        ts = time.strftime("%H:%M:%S")
        self.log_text.config(state=tk.NORMAL)
        self.log_text.insert(tk.END, f"[{ts}] {msg}\n")
        self.log_text.see(tk.END)
        self.log_text.config(state=tk.DISABLED)


# ---------------------------------------------------------------------------
#  Entry point
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    app = App()
    app.mainloop()
