# Server Patch: Real Client IP for Proxied Connections

When your game server sits behind **urt-proxy**, it only sees the proxy's internal relay address for every connecting player. This means bans, logging, and game modules all see the same IP instead of each player's real address.

The included engine patch fixes this. After applying it, your server will see the **real client IP** for every proxied player — bans, GeoIP, admin tools, and logs all work correctly.

---

## How It Works

```
                  connect with \realip\1.2.3.4:27005
Player ──► urt-proxy ─────────────────────────────────► Game Server
1.2.3.4              10.0.0.1                           (patched engine)
                                                         │
                                                         ├─ sv_trustedProxies "10.0.0.1"
                                                         ├─ Sees \ip\ as 1.2.3.4:27005
                                                         └─ Bans apply to 1.2.3.4
```

1. The proxy injects a `\realip\` key into the player's connect packet containing their real IP and port.
2. The patched server checks if the connection comes from an address listed in `sv_trustedProxies`.
3. If trusted, it uses the real IP for the `\ip\` userinfo key, ban checks, and game module visibility.
4. The `\realip\` key is **always stripped** from userinfo — non-proxied clients cannot spoof it.

---

## Requirements

- A working C build environment (`gcc`/`clang`, `make`)
- The source code for your ioquake3-based engine (ioquake3, ioUrbanTerror, or any Q3 derivative)
- The `patch` utility (included on virtually all Linux/macOS systems; on Windows use Git Bash or WSL)

---

## Applying the Patch

### 1. Copy the patch file

Copy `patches/ioq3-realip.patch` from this repository into your engine source tree's root directory (the directory that contains the `code/` folder).

```bash
cp /path/to/urt-proxy/patches/ioq3-realip.patch /path/to/ioq3/
```

### 2. Apply with `patch`

```bash
cd /path/to/ioq3
patch -p1 < ioq3-realip.patch
```

You should see output like:

```
patching file code/server/server.h
patching file code/server/sv_client.c
patching file code/server/sv_init.c
patching file code/server/sv_main.c
```

> **Tip:** If you have a custom engine with modified server code, `patch` may report fuzz or offsets — that's normal. If it reports a **failed hunk**, you'll need to apply that section by hand. The changes are straightforward; see [What the Patch Changes](#what-the-patch-changes) below.

### 3. Rebuild your server binary

```bash
make
```

Or however you normally compile your engine (e.g. `make BUILD_SERVER=1`, or your custom build script).

---

## Configuration

The patch adds a single server cvar:

| Cvar | Default | Description |
|------|---------|-------------|
| `sv_trustedProxies` | `""` (empty) | Comma-separated list of trusted proxy IP addresses |

### Setting the cvar

Add it to your server config file (e.g. `server.cfg`):

```
set sv_trustedProxies "10.0.0.1"
```

For multiple proxies, separate with commas:

```
set sv_trustedProxies "10.0.0.1, 10.0.0.2, 192.168.1.50"
```

Or set it from the server console at runtime:

```
\sv_trustedProxies "10.0.0.1"
```

The cvar is `CVAR_ARCHIVE`, so it will be saved automatically to your config on map change or shutdown.

### What IP to use

The value should be the **source IP** that the proxy's packets arrive from, as seen by your server. Typically this is:

| Setup | Value |
|-------|-------|
| Proxy on same machine via loopback | `127.0.0.1` |
| Proxy on WireGuard peer | WireGuard interface IP (e.g. `10.0.0.1`) |
| Proxy on remote machine | Public IP of the proxy machine |

> **Security:** Only add IPs you control. Any address in this list can set arbitrary client IPs for connecting players.

---

## Verifying It Works

### 1. Check the server console

When a player connects through a trusted proxy, the server prints:

```
Trusted proxy 10.0.0.1:27960: real client IP 1.2.3.4:27005
```

### 2. Check the player's IP in-game

Use your admin mod or rcon to inspect the player's userinfo:

```
\rcon status
```

The `address` / `ip` field should show the player's real public IP, not the proxy's internal address.

### 3. Test bans

Ban a player by IP through your admin mod or the `addip` command. The ban should apply to their real IP. Reconnecting through the proxy should still block them.

---

## What the Patch Changes

The patch is minimal and touches **4 files**, all in `code/server/`:

| File | Change |
|------|--------|
| `server.h` | Adds `netadr_t realAddress` field to `client_t` struct; declares `sv_trustedProxies` cvar |
| `sv_main.c` | Defines `cvar_t *sv_trustedProxies` variable |
| `sv_init.c` | Registers the cvar in `SV_Init()` with `Cvar_Get()` |
| `sv_client.c` | Adds `SV_IsTrustedProxy()` function; modifies `SV_DirectConnect()` to extract and use real IP; modifies `SV_UserinfoChanged()` to preserve it |

### Detailed behavior in `SV_DirectConnect()`

1. Reads `\realip\` from incoming userinfo.
2. Calls `SV_IsTrustedProxy()` to check if the source address is in `sv_trustedProxies`.
3. If trusted **and** the realip value is a valid address:
   - Stores it in `client_t.realAddress`.
   - Uses it for the `SV_IsBanned()` check instead of the proxy address.
   - Sets the `\ip\` userinfo key to the real address.
4. **Always** strips `\realip\` from userinfo (prevents spoofing from direct connections).
5. `SV_UserinfoChanged()` checks `client_t.realAddress` — if set, it writes the real IP back into `\ip\` on every userinfo refresh, preventing the engine from overwriting it with the proxy address.

---

## Compatibility

| Engine | Status |
|--------|--------|
| ioquake3 (main branch) | Primary target — patch applies cleanly |
| ioUrbanTerror / UrT engine forks | Should apply with minor offset adjustments |
| Other Q3 derivatives (OpenArena, Spearmint, etc.) | Likely compatible — same server code structure |

The patch does **not** modify:
- The network protocol — no client changes needed
- `netchan.remoteAddress` — UDP routing is untouched
- Any game module (QVM) interfaces
- Any client-side code

---

## Uninstalling

To revert the patch:

```bash
cd /path/to/ioq3
patch -R -p1 < ioq3-realip.patch
make
```

Or simply remove `sv_trustedProxies` from your config — when the cvar is empty (default), the patch is completely inert and all connections behave exactly as stock.

---

## Troubleshooting

### "Hunk FAILED" when applying the patch

Your engine source has modifications in the same area. Apply the failed hunks manually — look at the `.rej` file that `patch` creates, and add the changes by hand. The modifications are small and isolated.

### Players still show the proxy IP

- Verify `sv_trustedProxies` contains the correct IP: `\rcon sv_trustedProxies` to check.
- Make sure the IP matches what the proxy actually connects from (check `netstat` or the server console output).
- Restart the map after changing the cvar: `\rcon map_restart`

### Bans not applying to real IPs

Bans that were created before patching used the proxy IP. You may need to re-create bans targeting the real player IPs.

### Concern about spoofing

The `\realip\` key is **unconditionally stripped** from all incoming connections. It is only honored when:
1. The source address matches an entry in `sv_trustedProxies`, **AND**
2. The value is a valid parseable network address.

A direct-connecting player cannot inject a fake `\realip\` — it will be removed before any processing.
