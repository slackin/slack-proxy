# urt-proxy

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

Transparent UDP proxy for Urban Terror (Quake 3 engine) that relays game traffic
over a WireGuard tunnel to reduce latency by routing through a better network path.

## Features

- **Transparent relay** — gameplay packets pass through unmodified; no Q3 protocol changes
- **Server browser compatible** — responds to `getinfo`/`getstatus` queries, appears as a normal UrT server
- **Hostname tagging** — optionally prepend a tag (e.g. `[PROXY]`) to `sv_hostname` so players can identify the proxy in the server list
- **Per-client sessions** — each player gets a dedicated relay socket for proper bidirectional NAT
- **Single-threaded epoll** — efficient event loop with no threads and no external dependencies
- **Rate limiting** — caps new session creation to prevent abuse
- **Automatic cleanup** — idle sessions are expired after a configurable timeout
- **Graceful shutdown** — `SIGINT`/`SIGTERM` cleanly closes all sockets and frees resources

## How It Works

```
Player ──UDP──► urt-proxy (:27960) ──WireGuard──► Real UrT Server
       ◄──UDP──                    ◄──WireGuard──
```

The proxy binds a UDP port and appears as a normal Urban Terror server. When a player
connects, the proxy creates a dedicated relay socket and forwards all packets to the
real server over the WireGuard interface. Responses are relayed back transparently.

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

## Usage

```bash
./build/urt-proxy -r <real-server-wg-ip> [options]
```

### Required

| Flag | Description |
|------|-------------|
| `-r, --remote-host HOST` | Real server's WireGuard IP address |

### Optional

| Flag | Default | Description |
|------|---------|-------------|
| `-l, --listen-port PORT` | 27960 | Local UDP port to listen on |
| `-p, --remote-port PORT` | 27960 | Real server's game port |
| `-m, --max-clients N` | 20 | Maximum concurrent client sessions (1–1000) |
| `-t, --timeout SECS` | 30 | Session inactivity timeout (≥ 5) |
| `-T, --hostname-tag TAG` | *(none)* | Prefix added to `sv_hostname` in browser |
| `-R, --rate-limit N` | 5 | Max new sessions per second (≥ 1) |
| `-M, --master-server HOST[:PORT]` | *(none)* | Master server for server list registration (port defaults to 27900, may be repeated) |
| `-d, --debug` | off | Enable debug-level logging |

### Example

```bash
# Proxy on port 27960, forwarding to real server at 10.0.0.2:27960
# with "[US-EAST]" prefix in the server browser, registered with master
./build/urt-proxy -r 10.0.0.2 -l 27960 -p 27960 -T "[US-EAST]" \
    -M master.urbanterror.info
```

## Log Output

All logs go to stderr with timestamps and severity levels:

```
[2026-03-30 14:23:45] [INFO ] urt-proxy starting
[2026-03-30 14:23:45] [INFO ]   Listen port:    27960
[2026-03-30 14:23:45] [INFO ]   Remote server:  10.0.0.2:27960
[2026-03-30 14:23:45] [INFO ]   Max clients:    20
[2026-03-30 14:23:45] [INFO ]   Session timeout: 30s
[2026-03-30 14:23:45] [INFO ]   Rate limit:     5 new/sec
[2026-03-30 14:23:45] [INFO ]   Hostname tag:   "[US-EAST]"
[2026-03-30 14:23:45] [INFO ]   Master server:  198.51.100.10:27900
[2026-03-30 14:23:45] [INFO ] Listening on UDP port 27960
[2026-03-30 14:23:45] [INFO ] Forwarding to 10.0.0.2:27960 via WireGuard
[2026-03-30 14:23:45] [INFO ] Max clients: 20, session timeout: 30s
[2026-03-30 14:23:45] [INFO ] Sent heartbeat to 1 master server(s)
[2026-03-30 14:23:52] [INFO ] New session: 203.0.113.42:12345 (relay fd=5, total=1)
[2026-03-30 14:24:22] [INFO ] Session expired: 203.0.113.42:12345 (pkts: 847/1203, bytes: 42350/96240)
[2026-03-30 14:24:22] [INFO ] Swept 1 expired sessions, 0 active
```

Enable debug logging with `-d` for verbose packet-level diagnostics.

## Architecture

- **Single-threaded epoll** event loop — handles all I/O without threads
- **Per-client relay socket** — each client gets a unique ephemeral socket connected
  to the real server, enabling proper bidirectional NAT
- **Session hash map** — dual-index open-addressing table for O(1) lookup by
  client address or relay file descriptor
- **Rate limiting** — simple 1-second sliding window caps new session creation
- **Master server heartbeat** — periodic registration with UrT master server(s)
  so the proxy appears in the server browser
- **Graceful shutdown** — SIGINT/SIGTERM closes all sockets and frees resources

### Project Structure

```
include/
  relay.h      Relay configuration and entry point
  hashmap.h    Session storage with dual-index hash map
  q3proto.h    Quake 3 protocol helpers (OOB packet parsing)
  log.h        Timestamped levelled logging
src/
  main.c       CLI parsing, validation, startup
  relay.c      epoll event loop, session lifecycle, packet forwarding
  hashmap.c    Open-addressing hash map with linear probing
  q3proto.c    Connectionless packet inspection and hostname rewriting
  log.c        Formatted stderr logging
```

## Systemd Service (optional)

```ini
[Unit]
Description=Urban Terror UDP Proxy
After=network.target wg-quick@wg0.service

[Service]
Type=simple
ExecStart=/opt/urt-proxy/build/urt-proxy -r 10.0.0.2 -T "[PROXY]"
Restart=on-failure
RestartSec=5
LimitNOFILE=4096

[Install]
WantedBy=multi-user.target
```

## Troubleshooting

| Problem | Cause | Fix |
|---------|-------|-----|
| `bind(:27960): Address already in use` | Another process is using the port | Stop the other process or use `-l` to pick a different port |
| Server not appearing in master list | No master server configured | Add `-M master.urbanterror.info` to register with the UrT master |
| `cannot resolve master server` | DNS lookup failed for master hostname | Check DNS, ensure the hostname is correct and resolvable |
| `connect() relay socket: Network is unreachable` | WireGuard tunnel is not up | Bring up the tunnel (`wg-quick up wg0`) and verify the remote IP is reachable |
| `Rate limit: dropping new client` | Too many new connections per second | Increase `-R` or investigate a possible flood |
| `Max clients reached, dropping new connection` | Session pool is full | Increase `-m` or decrease `-t` to expire idle sessions faster |
| Sessions expire too quickly | Timeout too short for your player base | Increase `-t` (default is 30 seconds) |

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for build instructions, code style, and how to submit changes.

## License

[MIT](LICENSE)
