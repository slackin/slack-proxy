# urt-proxy

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

Transparent UDP proxy for Urban Terror (Quake 3 engine) that relays game traffic
over a WireGuard tunnel to reduce latency by routing through a better network path.

## Features

- **Transparent relay** — gameplay packets pass through unmodified; no Q3 protocol changes
- **Server browser compatible** — responds to `getinfo`/`getstatus` queries, appears as a normal UrT server
- **Hostname tagging** — optionally prepend a tag (e.g. `[PROXY]`) to `sv_hostname` so players can identify the proxy in the server list
- **Multi-server support** — proxy multiple game servers from one process using an INI config file, each on a different port
- **Per-client sessions** — each player gets a dedicated relay socket for proper bidirectional NAT
- **Query session management** — browser queries and game sessions are tracked separately with independent limits and timeouts
- **Remote management API** — TCP-based JSON management interface for monitoring and runtime tuning
- **Management-only mode** — start with no servers configured, add them at runtime via the GUI client
- **Auto-generated API key** — key is generated on first run, saved to a file, and reused on subsequent startups
- **GUI management client** — modern Rust+egui GPU-rendered desktop client for remote proxy management
- **Single-threaded epoll** — efficient event loop with no threads and no external dependencies
- **Rate limiting** — caps new session creation to prevent abuse
- **Master server heartbeat** — periodic registration with UrT master server(s) so the proxy appears in the server browser
- **Automatic cleanup** — idle sessions are expired after a configurable timeout with per-session traffic statistics
- **Graceful shutdown** — `SIGINT`/`SIGTERM` cleanly closes all sockets and frees resources

## How It Works

```
Player ──UDP──► urt-proxy (:27960) ──WireGuard──► Real UrT Server
       ◄──UDP──                    ◄──WireGuard──
```

The proxy binds a UDP port and appears as a normal Urban Terror server. When a player
connects, the proxy creates a dedicated relay socket and forwards all packets to the
real server over the WireGuard interface. Responses are relayed back transparently.

Browser queries (`getinfo`/`getstatus`) from the master server or player server
browsers are forwarded to the real server and the response is relayed back — with
an optional hostname tag prepended so the proxy is identifiable in the server list.

## Prerequisites

- Linux server with GCC and make
- Existing WireGuard tunnel to the real game server
- The proxy host can reach the game server's WireGuard IP on the game port

## Build

```bash
make
```

Binary is produced at `build/urt-proxy`.

To clean build artifacts:

```bash
make clean
```

No external dependencies — only POSIX and standard C11 library.

## Usage

urt-proxy supports two modes: **single-server CLI mode** and **multi-server config file mode**.

### Single-Server CLI Mode

```bash
./build/urt-proxy -r <real-server-wg-ip> [options]
```

#### Required

| Flag | Description |
|------|-------------|
| `-r, --remote-host HOST` | Real server's WireGuard IP address |

#### Optional

| Flag | Default | Description |
|------|---------|-------------|
| `-l, --listen-port PORT` | 27960 | Local UDP port to listen on |
| `-p, --remote-port PORT` | 27960 | Real server's game port |
| `-m, --max-clients N` | 20 | Maximum concurrent game sessions (1–1000) |
| `-t, --timeout SECS` | 30 | Game session inactivity timeout in seconds (≥ 5) |
| `-T, --hostname-tag TAG` | *(none)* | Prefix added to `sv_hostname` in server browser responses |
| `-R, --rate-limit N` | 5 | Max new game sessions per second (≥ 1) |
| `-Q, --max-query-sessions N` | 100 | Max concurrent browser query sessions (1–1000) |
| `-q, --query-timeout SECS` | 5 | Browser query session inactivity timeout in seconds (≥ 1) |
| `-M, --master-server HOST[:PORT]` | *(none)* | Master server for server list registration (port defaults to 27900, repeatable up to 4) |
| `-d, --debug` | off | Enable debug-level logging |
| `-h, --help` | | Show usage help |

#### Management API (available in both modes)

| Flag | Default | Description |
|------|---------|-------------|
| `--mgmt-key KEY` | *(auto)* | Enable management API with this shared secret |
| `--mgmt-key-file PATH` | `.urt-proxy.key` | Path to API key file (auto-generated on first run) |
| `--mgmt-port PORT` | 27961 | TCP port for management connections |
| `--mgmt-addr ADDR` | 127.0.0.1 | Address to bind the management listener (use `0.0.0.0` for remote access) |

If `--mgmt-key` is not provided, the key is automatically loaded from the key file. On first run, a random 32-character hex key is generated, saved to the file with `0600` permissions, and displayed in the terminal.

#### Management-Only Mode

Start the proxy with no `-r` or `-c` — just the management port:

```bash
# First run: generates .urt-proxy.key and displays it
./build/urt-proxy

# Or specify a custom key file path:
./build/urt-proxy --mgmt-key-file /etc/urt-proxy.key
```

This opens only the management API port so you can add servers at runtime via the GUI client. No game traffic is relayed until servers are configured.

#### Example

```bash
# Proxy on port 27960, forwarding to real server at 10.0.0.2:27960
# with "[US-EAST]" prefix in the server browser, registered with master
./build/urt-proxy -r 10.0.0.2 -l 27960 -p 27960 -T "[US-EAST]" \
    -M master.urbanterror.info
```

### Multi-Server Config File Mode

```bash
./build/urt-proxy -c /etc/urt-proxy.conf [-d]
```

| Flag | Description |
|------|-------------|
| `-c, --config FILE` | Load servers from an INI config file (all other single-server flags are ignored) |
| `-d, --debug` | Override the config file's `debug` setting (enables debug logging) |

When `-c` is given, all single-server CLI flags (`-r`, `-l`, `-p`, etc.) are ignored — the config file is the sole source of truth.

#### Config File Format

See [`urt-proxy.conf.example`](urt-proxy.conf.example) for a complete annotated example.

```ini
[global]
debug = false
mgmt-key = my-secret-api-key    # enables management API
mgmt-port = 27961               # optional, default 27961
mgmt-addr = 127.0.0.1           # optional, default 127.0.0.1

[server:dallas]
listen-port   = 27960
remote-host   = 10.0.0.2       # REQUIRED
remote-port   = 27960
max-clients   = 20
timeout       = 30
hostname-tag  = [DALLAS]
rate-limit    = 5
max-query-sessions = 100
query-timeout = 5
master-server = master.urbanterror.info

[server:chicago]
listen-port   = 27961
remote-host   = 10.0.0.3
hostname-tag  = [CHICAGO]
master-server = master.urbanterror.info
```

Each `[server:<name>]` section defines one proxied server. The only required key is `remote-host`; all others have the same defaults as the CLI flags. Up to 32 servers can be defined. Each must use a unique `listen-port`.

## Log Output

All logs go to stderr with timestamps and severity levels:

```
[2026-03-30 14:23:45] [INFO ] urt-proxy starting (single-server mode)
[2026-03-30 14:23:45] [INFO ]   Listen port:    27960
[2026-03-30 14:23:45] [INFO ]   Remote server:  10.0.0.2:27960
[2026-03-30 14:23:45] [INFO ]   Max clients:    20
[2026-03-30 14:23:45] [INFO ]   Session timeout: 30s
[2026-03-30 14:23:45] [INFO ]   Rate limit:     5 new/sec
[2026-03-30 14:23:45] [INFO ]   Query sessions: max 100, timeout 5s
[2026-03-30 14:23:45] [INFO ]   Hostname tag:   "[US-EAST]"
[2026-03-30 14:23:45] [INFO ]   Master server:  198.51.100.10:27900
[2026-03-30 14:23:45] [INFO ] Listening on UDP port 27960
[2026-03-30 14:23:45] [INFO ] Server #1: :27960 -> 10.0.0.2:27960 (max 20 clients, 30s timeout, query pool 100, 5s query timeout)
[2026-03-30 14:23:45] [INFO ] Sent heartbeat to 1 master server(s)
[2026-03-30 14:23:52] [INFO ] Server #1: new game session: 203.0.113.42:12345 (relay fd=5, total=1, queries=0)
[2026-03-30 14:24:22] [INFO ] Session expired: 203.0.113.42:12345 [game] (pkts: 847/1203, bytes: 42350/96240)
[2026-03-30 14:24:22] [INFO ] Swept 1 expired sessions, 0 active
```

Multi-server mode prefixes each message with the server number (e.g. `Server #1:`, `Server #2:`).

Rate limit, capacity, and query session warnings include the client address and configured limit for easy diagnosis:

```
[2026-03-30 14:25:10] [WARN ] Server #1: rate limit (5/sec) exceeded — dropping new connection from 198.51.100.5:41234
[2026-03-30 14:25:11] [WARN ] Server #1: max clients (20) reached — dropping new connection from 198.51.100.6:51234
[2026-03-30 14:25:12] [WARN ] Server #1: max query sessions (100) reached — dropping query from 198.51.100.7:61234
```

Enable debug logging with `-d` for verbose packet-level diagnostics (e.g. hostname rewrite events).

## Remote Management

urt-proxy includes a TCP-based management API for remote monitoring and runtime tuning. The management API is enabled automatically — on first run, a random API key is generated, saved to `.urt-proxy.key`, and displayed in the terminal. On subsequent runs the saved key is reused.

You can also provide a key explicitly via `--mgmt-key` or specify a custom key file path with `--mgmt-key-file`.

### Quick Start

```bash
# Start proxy — API key auto-generated on first run
./build/urt-proxy -r 10.0.0.2

# Or start in management-only mode (no game servers)
./build/urt-proxy

# Build and launch the GUI client
cd gui/urt-mgmt && cargo build --release
./gui/urt-mgmt/target/release/urt-mgmt
```

### Protocol

The management API uses **newline-delimited JSON over TCP**. Each message is a single JSON object terminated by `\n`.

**Authentication** (first message):
```json
{"auth":"my-secret-key"}
```

**Commands** (after authentication):
```json
{"cmd":"status"}
{"cmd":"sessions","server":0}
{"cmd":"set","server":0,"key":"max_clients","value":50}
{"cmd":"kick","server":0,"client":"203.0.113.42:12345"}
{"cmd":"kick_all","server":0}
```

**Tunable parameters**: `max_clients`, `session_timeout`, `query_timeout`, `max_new_per_sec`, `max_query_sessions`, `hostname_tag`

### GUI Client

The `gui/urt-mgmt/` directory contains a modern desktop management client built with Rust and egui (GPU-rendered immediate-mode GUI). It compiles to a single standalone binary with no runtime dependencies.

```bash
cd gui/urt-mgmt
cargo build --release
# Binary: target/release/urt-mgmt (or urt-mgmt.exe on Windows)
```

Features:
- Dark modern UI with GPU-accelerated rendering
- Connect to any urt-proxy management endpoint
- Tabbed interface — one tab per proxied server
- View server configuration and live session counts
- Tune runtime parameters with instant apply
- Browse active sessions with traffic statistics in a sortable table
- Kick individual sessions or all sessions on a server
- Auto-refreshing display (2-second interval)
- Scrollable timestamped log panel

### Security Notes

- The management listener defaults to `127.0.0.1` (localhost only)
- Set `mgmt-addr = 0.0.0.0` to allow remote connections — ensure firewall rules are in place
- API key is transmitted in plaintext; use a VPN or SSH tunnel for remote management over untrusted networks
- Maximum 4 concurrent management connections

## Architecture

- **Single-threaded epoll** event loop — handles all I/O without threads, multiplexed across all configured servers
- **Per-client relay socket** — each client gets a unique ephemeral socket connected
  to the real server, enabling proper bidirectional NAT
- **Session hash map** — dual-index open-addressing table for O(1) lookup by
  client address or relay file descriptor
- **Query session tracking** — browser queries (`getinfo`/`getstatus`) are tracked separately from game sessions, with independent caps and shorter timeouts; a query session is automatically promoted to a game session if the client sends a non-query packet (e.g. `getchallenge`)
- **Rate limiting** — simple 1-second sliding window caps new game session creation
- **Master server heartbeat** — periodic registration with UrT master server(s)
  so the proxy appears in the server browser (every 5 minutes, per the Q3 protocol)
- **INI config parser** — file-based configuration for multi-server deployments with per-server settings and validation
- **Graceful shutdown** — `SIGINT`/`SIGTERM` closes all sockets, logs per-server relay socket counts, and frees resources

### Project Structure

```
include/
  config.h     INI config file parser types and API
  relay.h      Relay configuration struct, server instance, and entry point
  mgmt.h       Management API types and interface
  hashmap.h    Session storage with dual-index hash map
  q3proto.h    Quake 3 protocol helpers (OOB packet parsing)
  log.h        Timestamped levelled logging
src/
  main.c       CLI parsing, validation, startup (single-server and config modes)
  config.c     INI config file parser with hostname resolution and validation
  relay.c      epoll event loop, session lifecycle, packet forwarding, heartbeats
  mgmt.c       TCP management server — JSON protocol, command handlers
  hashmap.c    Open-addressing hash map with linear probing (rebuild on delete)
  q3proto.c    Connectionless packet inspection and hostname rewriting
  log.c        Formatted stderr logging with severity levels
gui/
  urt-mgmt/    Rust+egui GPU-rendered management GUI client (cargo project)
```

## Systemd Service (optional)

### Single-Server

```ini
[Unit]
Description=Urban Terror UDP Proxy
After=network.target wg-quick@wg0.service

[Service]
Type=simple
ExecStart=/opt/urt-proxy/build/urt-proxy -r 10.0.0.2 -T "[PROXY]" \
    -M master.urbanterror.info
Restart=on-failure
RestartSec=5
LimitNOFILE=4096

[Install]
WantedBy=multi-user.target
```

### Multi-Server (config file)

```ini
[Unit]
Description=Urban Terror UDP Proxy (multi-server)
After=network.target wg-quick@wg0.service

[Service]
Type=simple
ExecStart=/opt/urt-proxy/build/urt-proxy -c /etc/urt-proxy.conf
Restart=on-failure
RestartSec=5
LimitNOFILE=4096

[Install]
WantedBy=multi-user.target
```

## Troubleshooting

| Problem | Cause | Fix |
|---------|-------|-----|
| `bind(:27960): Address already in use` | Another process is using the port | Stop the other process or use `-l` (CLI) / `listen-port` (config) to pick a different port |
| Server not appearing in master list | No master server configured | Add `-M master.urbanterror.info` (CLI) or `master-server = master.urbanterror.info` (config) |
| `cannot resolve master server 'host'` | DNS lookup failed for master hostname | Check DNS, ensure the hostname is correct and resolvable |
| `connect() relay socket: Network is unreachable` | WireGuard tunnel is not up | Bring up the tunnel (`wg-quick up wg0`) and verify the remote IP is reachable |
| `rate limit (N/sec) exceeded` | Too many new connections per second | Increase `-R` / `rate-limit` or investigate a possible flood |
| `max clients (N) reached` | Game session pool is full | Increase `-m` / `max-clients` or decrease `-t` / `timeout` to expire idle sessions faster |
| `max query sessions (N) reached` | Browser query pool is full | Increase `-Q` / `max-query-sessions` or decrease `-q` / `query-timeout` |
| Sessions expire too quickly | Timeout too short for your player base | Increase `-t` / `timeout` (default is 30 seconds) |
| `config file has no [server:] sections` | Config file is missing server blocks | Add at least one `[server:<name>]` section with a `remote-host` key, or enable the management API to use management-only mode |
| `'remote-host' is required` | Server section missing the only required key | Add `remote-host = <ip>` to the server section |
| `servers #X and #Y both use listen-port Z` | Duplicate ports in config file | Give each server a unique `listen-port` |
| `'x.x.x.x' is not a valid IPv4 address` | Non-IP passed to `-r` in CLI mode | Use a dotted-quad IPv4 address (e.g. `10.0.0.2`); for hostnames, use config file mode |

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for build instructions, code style, and how to submit changes.

## License

[MIT](LICENSE)
