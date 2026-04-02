<div align="center">

# Playing on a Proxied Server

### A guide for Urban Terror players

</div>

---

## What is this?

Some Urban Terror servers use **urt-proxy** to give you a better connection. Instead of your game traffic taking the default internet route to the server (which might go through congested or distant networks), it gets routed through a faster, more direct path using an encrypted tunnel.

**The short version:** it's like taking a highway instead of back roads to reach the game server.

You don't need to install anything, change any settings, or do anything differently. Just connect and play.

---

## How do I know if a server uses it?

Proxied servers usually have a **tag** at the beginning of their name in the server browser. Your community or server admin will let you know what to look for, but it typically looks something like:

```
[US-EAST] My Awesome Server
[PROXY]   Friday Night Fights
[EU-WEST] Competitive CTF
```

That tag (like `[US-EAST]`) is added by the proxy so you can tell which servers are using it. The rest of the server name stays the same.

---

## How to connect

It's exactly the same as connecting to any other server:

1. **Open Urban Terror** as you normally would
2. **Open the server browser** (Play Online)
3. **Find the server** — look for the name tag your admin told you about
4. **Click connect** — that's it!

You can also connect directly via the console if you know the address:

```
/connect proxy-address:27960
```

Your admin will give you the correct address and port if you're connecting this way.

---

## What to expect

### Nothing changes from your perspective

Once you're connected, everything works exactly like a normal game:

- **Same maps, same players, same gameplay** — nothing is different in-game
- **Your game client doesn't know the difference** — it thinks it's talking directly to the server
- **All your settings are the same** — configs, binds, sensitivity, everything stays as-is
- **Server browser works normally** — you'll see player count, map, ping, and game type as usual

### Your ping might improve

The whole point of the proxy is to route your connection through a faster path. Depending on your location and the server's location, you may notice:

- **Lower ping** — your connection takes a more direct route
- **More stable ping** — less jitter from congested internet routes
- **Same ping** — if your default route was already good, you might not see a difference

The improvement depends on network conditions and geography. Players who are far from the server or on congested routes tend to benefit the most.

---

## Frequently Asked Questions

### Do I need to install anything?

**No.** The proxy runs on the server side. You just connect to it like any other Urban Terror server. No downloads, no plugins, no mods.

### Do I need to change any game settings?

**No.** Everything works with your existing Urban Terror setup. No console commands, no config tweaks.

### Will this increase my ping?

**Usually the opposite.** The proxy is set up to route traffic through a faster network path. Most players see the same or lower ping. In rare cases where your default route is already optimal, you might see a very small increase (1–2ms) — but this is uncommon.

### Can I still see who's playing before I join?

**Yes.** The server browser shows all the usual info — player count, current map, game type, and ping — just like any other server.

### How is this different from a VPN?

A VPN routes **all** your internet traffic through a different path. The proxy only routes your **Urban Terror game traffic** for that specific server. You don't need to install or run anything — it's completely server-side.

### The server name has a weird tag like `[US-EAST]` — what is that?

That's the proxy's **hostname tag**. It's added so you can identify proxied servers in the browser. The actual server behind it is the same one you know — just reached through a faster route.

### Who runs the proxy?

Your **server administrator** or **community organizer** sets up and manages the proxy. If you have questions about a specific server, reach out to them.

---

## How it works (simplified)

```
  You                       Proxy                      Game Server
  ┌──┐                    ┌────────┐                   ┌──────────┐
  │  │ ── game traffic ──►│        │ ── fast route ───►│          │
  │  │                    │  relay │    (encrypted)     │  actual  │
  │  │ ◄─ game traffic ──│        │ ◄─ fast route ────│  server  │
  └──┘                    └────────┘                   └──────────┘

  Your game connects       The proxy picks             The server you're
  to the proxy address     a faster network             actually playing on
  like a normal server     path and forwards             — same players,
                           everything through             same game
```

Your game talks to the proxy. The proxy talks to the real server through a faster, encrypted tunnel. Everything happens automatically — you just play.

---

<div align="center">

*For technical details, server admin setup, and source code, see the [main README](README.md).*

</div>
