# urt-proxy

Transparent UDP proxy for Urban Terror (Quake 3 engine) that relays game traffic
over a WireGuard tunnel to reduce latency by routing through a better network path.

## How It Works

```
Player ──UDP──► urt-proxy (:27960) ──WireGuard──► Real UrT Server
```

The proxy binds a UDP port and appears as a normal Urban Terror server. When a player
connects, the proxy creates a dedicated relay socket and forwards all packets to the
real server over the WireGuard interface. Responses are relayed back transparently.

- No Q3 protocol modification — gameplay packets pass through unmodified
- Appears in the server browser (responds to `getinfo`/`getstatus` queries)
- Optional hostname rewriting so players can identify the proxy in the browser
- Per-client session tracking with automatic timeout cleanup

## Prerequisites

- Linux server with GCC and make
- Existing WireGuard tunnel to the real game server
- The proxy host can reach the game server's WireGuard IP on the game port

## Build

```bash
make
```

Binary is produced at `build/urt-proxy`.

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
| `-m, --max-clients N` | 20 | Maximum concurrent client sessions |
| `-t, --timeout SECS` | 30 | Session inactivity timeout |
| `-T, --hostname-tag TAG` | *(none)* | Prefix added to `sv_hostname` in browser |
| `-R, --rate-limit N` | 5 | Max new sessions per second |
| `-d, --debug` | off | Enable debug-level logging |

### Example

```bash
# Proxy on port 27960, forwarding to real server at 10.0.0.2:27960
# with "[US-EAST]" prefix in the server browser
./build/urt-proxy -r 10.0.0.2 -l 27960 -p 27960 -T "[US-EAST]"
```

## Architecture

- **Single-threaded epoll** event loop — handles all I/O without threads
- **Per-client relay socket** — each client gets a unique ephemeral socket connected
  to the real server, enabling proper bidirectional NAT
- **Session hash map** — O(1) lookup by client address or relay fd
- **Rate limiting** — caps new session creation to prevent abuse
- **Graceful shutdown** — SIGINT/SIGTERM closes all sockets and frees resources

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
