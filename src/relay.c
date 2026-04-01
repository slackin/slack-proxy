/*
 * relay.c — Core UDP relay event loop and session management.
 *
 * Implements the single-threaded, epoll-based proxy loop that:
 *
 *   1. Accepts UDP packets from game clients on a public listen socket.
 *   2. Creates a dedicated relay socket per client, connected to the
 *      real game server over WireGuard.
 *   3. Forwards client packets to the real server (client → relay).
 *   4. Relays server responses back to the correct client (relay → listen).
 *   5. Optionally rewrites sv_hostname in server browser responses.
 *   6. Periodically sweeps and removes sessions that have been idle
 *      longer than the configured timeout.
 *   7. Rate-limits new session creation to prevent abuse.
 *   8. Sends periodic heartbeat packets to master server(s) so the
 *      proxy appears in the UrT server browser / master list.
 *
 * Supports multi-server mode: when multiple relay_config_t entries are
 * passed to relay_run(), each server gets its own listen socket, session
 * map, rate limiter, and sweep/heartbeat timers — all multiplexed on a
 * single shared epoll instance.  The server index is packed into the
 * upper 32 bits of epoll_data.u64 so the event loop can route each
 * event to the correct server_instance_t.
 *
 * The loop exits cleanly on SIGINT or SIGTERM, closing all sockets and
 * freeing all allocated memory.
 *
 * Data flow (per server):
 *
 *   Player  ─── UDP ───►  listen_fd  ─── send() ───►  relay_fd  ─── WG ───►  Server
 *   Player  ◄── sendto ──  listen_fd  ◄── recv() ────  relay_fd  ◄── WG ────  Server
 */

#define _POSIX_C_SOURCE 200809L

#include "relay.h"
#include "mgmt.h"
#include "hashmap.h"
#include "q3proto.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <arpa/inet.h>

/* Receive buffer: large enough for the biggest Q3 packet plus headroom. */
#define RECV_BUF_SIZE  (Q3_MAX_PACKET_SIZE + 64)

/* Maximum number of events returned by a single epoll_wait() call. */
#define MAX_EPOLL_EVENTS 64

/* How often (in seconds) the main loop checks for expired sessions. */
#define SWEEP_INTERVAL   5

/*
 * g_running — Global flag cleared by the signal handler to request
 * a graceful shutdown of the event loop.
 */
static volatile sig_atomic_t g_running = 1;

/*
 * signal_handler — SIGINT/SIGTERM handler.
 *
 * Sets g_running to 0 so the main loop exits on its next iteration.
 * Must be async-signal-safe — only touches a volatile sig_atomic_t.
 */
static void signal_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

/*
 * set_nonblocking — Set the O_NONBLOCK flag on a file descriptor.
 *
 * @param fd  File descriptor to modify.
 * @return    0 on success, -1 on fcntl failure.
 */
static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/*
 * set_socket_buffers — Set both the send and receive kernel buffers.
 *
 * Larger buffers reduce the chance of dropped packets under burst
 * traffic.  256 KB is a good default for a game proxy.
 *
 * @param fd    Socket file descriptor.
 * @param size  Desired buffer size in bytes.
 * @return      0 on success, -1 on setsockopt failure.
 */
static int set_socket_buffers(int fd, int size)
{
    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size)) < 0)
        return -1;
    if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) < 0)
        return -1;
    return 0;
}

/*
 * create_listen_socket — Create and bind the public-facing UDP socket.
 *
 * The socket is set to non-blocking mode with SO_REUSEADDR and 256 KB
 * send/receive buffers.  It binds to INADDR_ANY on the given port.
 *
 * @param port  UDP port number to listen on.
 * @return      File descriptor on success, or -1 on error (logged).
 */
static int create_listen_socket(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        log_error("socket(): %s", strerror(errno));
        return -1;
    }

    /* Allow quick restarts without TIME_WAIT blocking the port */
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    set_socket_buffers(fd, 256 * 1024);

    if (set_nonblocking(fd) < 0) {
        log_error("fcntl(O_NONBLOCK): %s", strerror(errno));
        close(fd);
        return -1;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        log_error("bind(:%u): %s", port, strerror(errno));
        close(fd);
        return -1;
    }

    log_info("Listening on UDP port %u", port);
    return fd;
}

/*
 * create_relay_socket — Create a per-client UDP relay socket.
 *
 * Each new client session gets its own ephemeral socket that will be
 * connect()'d to the real server.  Using a dedicated socket per client
 * lets the kernel demultiplex server responses to the correct session
 * without us having to parse source addresses.
 *
 * @return  File descriptor on success, or -1 on error.
 */
static int create_relay_socket(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;

    set_socket_buffers(fd, 256 * 1024);
    set_nonblocking(fd);

    /* Bind to an ephemeral port (port 0 → OS picks a free port) */
    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = 0;

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

/* ------------------------------------------------------------------ */
/*  Rate limiter                                                      */
/* ------------------------------------------------------------------ */

/*
 * rate_limit_check — Attempt to consume one token from the limiter.
 *
 * @param rl  Rate limiter state.
 * @return    1 if the request is allowed, 0 if rate-limited.
 */
static int rate_limit_check(rate_limiter_t *rl)
{
    time_t now = time(NULL);
    if (now != rl->window_start) {
        /* New second — reset the window */
        rl->window_start = now;
        rl->count = 0;
    }
    if (rl->count >= rl->max_per_sec)
        return 0; /* Rejected — limit reached */
    rl->count++;
    return 1;     /* Allowed */
}

/* ------------------------------------------------------------------ */
/*  Timeout sweep                                                     */
/* ------------------------------------------------------------------ */

/*
 * sweep_ctx_t — Context passed through session_foreach() during a
 * timeout sweep.
 *
 * We cannot remove sessions inside the iterator callback (it would
 * invalidate the iteration).  Instead, sweep_cb() collects pointers
 * to expired sessions into the to_remove[] array, and do_timeout_sweep()
 * removes them in a second pass.
 */
typedef struct {
    session_map_t *map;
    int            epoll_fd;
    time_t         now;
    int            session_timeout;
    int            query_timeout;
    int            expired_count;
    session_t    **to_remove;       /* Array of sessions to remove */
    int            to_remove_count;
    int            to_remove_cap;
} sweep_ctx_t;

/*
 * sweep_cb — session_foreach callback: collect expired sessions.
 */
static void sweep_cb(session_t *s, void *ctx)
{
    sweep_ctx_t *sw = ctx;
    int timeout = s->is_query ? sw->query_timeout : sw->session_timeout;
    if (sw->now - s->last_activity > timeout) {
        if (sw->to_remove_count < sw->to_remove_cap)
            sw->to_remove[sw->to_remove_count++] = s;
    }
}

/*
 * do_timeout_sweep — Find and remove all sessions that have been idle
 * longer than the configured timeout.
 *
 * For each expired session:
 *   1. Log final traffic statistics (packets + bytes in each direction).
 *   2. Remove the relay fd from epoll.
 *   3. Close the relay socket.
 *   4. Remove the session from the hash map.
 *
 * @param map            Session hash map.
 * @param epoll_fd       epoll instance for relay fd cleanup.
 * @param session_timeout  Inactivity timeout for game sessions (seconds).
 * @param query_timeout    Inactivity timeout for query sessions (seconds).
 * @param query_count    [in/out] Pointer to the query session counter;
 *                       decremented for each expired query session.
 */
static void do_timeout_sweep(session_map_t *map, int epoll_fd,
                             int session_timeout, int query_timeout,
                             int *query_count)
{
    sweep_ctx_t sw = {0};
    sw.map     = map;
    sw.epoll_fd = epoll_fd;
    sw.now     = time(NULL);
    sw.session_timeout = session_timeout;
    sw.query_timeout   = query_timeout;

    /* Allocate a worst-case array (every session could be expired) */
    sw.to_remove_cap = map->count;
    if (sw.to_remove_cap == 0)
        return;
    sw.to_remove = malloc((size_t)sw.to_remove_cap * sizeof(session_t *));
    if (!sw.to_remove)
        return;

    /* Pass 1: collect expired sessions */
    session_foreach(map, sweep_cb, &sw);

    /* Pass 2: remove collected sessions */
    for (int i = 0; i < sw.to_remove_count; i++) {
        session_t *s = sw.to_remove[i];
        char addr_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &s->client_addr.sin_addr, addr_str, sizeof(addr_str));
        log_info("Session expired: %s:%u %s (pkts: %lu/%lu, bytes: %lu/%lu)",
                 addr_str, ntohs(s->client_addr.sin_port),
                 s->is_query ? "[query]" : "[game]",
                 (unsigned long)s->pkts_to_server,
                 (unsigned long)s->pkts_to_client,
                 (unsigned long)s->bytes_to_server,
                 (unsigned long)s->bytes_to_client);

        if (s->is_query && query_count && *query_count > 0)
            (*query_count)--;

        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, s->relay_fd, NULL);
        close(s->relay_fd);
        session_remove(map, s);
    }

    if (sw.to_remove_count > 0)
        log_info("Swept %d expired sessions, %d active",
                 sw.to_remove_count, map->count);

    free(sw.to_remove);
}

/* ================================================================== */
/*  Master server heartbeat                                           */
/* ================================================================== */

/*
 * send_heartbeats — Send a heartbeat packet to each configured master server.
 *
 * In the Q3 protocol, a game server registers itself with a master server
 * by periodically sending:
 *
 *   \xFF\xFF\xFF\xFF heartbeat QuakeArena-1\n
 *
 * The master responds with a "getinfo" challenge, which arrives on the
 * listen socket and is handled like any other client packet (forwarded
 * to the real server, response relayed back with hostname rewriting).
 * The master then records the proxy's IP:port in the server list.
 *
 * Heartbeats are sent from the listen socket so the master sees the
 * proxy's public IP and listen port as the server address.
 *
 * @param listen_fd  The proxy's public-facing UDP socket.
 * @param cfg        Relay configuration (contains master_addrs[]).
 */
static void send_heartbeats(int listen_fd, const relay_config_t *cfg)
{
    static const char pkt[] =
        "\xFF\xFF\xFF\xFF" "heartbeat " Q3_HEARTBEAT_GAME "\n";

    for (int i = 0; i < cfg->master_count; i++) {
        sendto(listen_fd, pkt, sizeof(pkt) - 1, 0,
               (struct sockaddr *)&cfg->master_addrs[i],
               sizeof(cfg->master_addrs[i]));
    }
    log_info("Sent heartbeat to %d master server(s)", cfg->master_count);
}

/* ------------------------------------------------------------------ */
/*  Epoll data packing helpers                                        */
/* ------------------------------------------------------------------ */

/*
 * Pack a server index and file descriptor into epoll_data.u64.
 *
 * Layout: upper 32 bits = server index, lower 32 bits = fd.
 *
 * This encoding assumes file descriptors fit in 32 bits, which is
 * true on all current Linux kernels (fds are non-negative ints).
 * It lets us route every epoll event to the correct server_instance_t
 * without maintaining a separate fd-to-server lookup table.
 */
static inline uint64_t pack_epoll_data(int server_index, int fd)
{
    return ((uint64_t)(uint32_t)server_index << 32) | (uint64_t)(uint32_t)fd;
}

static inline int unpack_server_index(uint64_t data)
{
    return (int)(data >> 32);
}

static inline int unpack_fd(uint64_t data)
{
    return (int)(data & 0xFFFFFFFF);
}

/* ================================================================== */
/*  Main event loop                                                   */
/* ================================================================== */

/*
 * relay_run — Run the proxy event loop until shutdown.
 *
 * High-level flow:
 *   1. Install signal handlers (SIGINT, SIGTERM → g_running = 0).
 *   2. Create an epoll instance.
 *   3. For each server config: create listen socket, session map,
 *      rate limiter; register listen fd with epoll.
 *   4. Loop:
 *      a. epoll_wait() with a timeout equal to SWEEP_INTERVAL.
 *      b. For each ready fd, unpack the server index from epoll data:
 *         - listen_fd: receive client packet, find/create session, forward.
 *         - relay_fd:  receive server response, find session, relay back.
 *      c. Every SWEEP_INTERVAL seconds, sweep expired sessions (all servers).
 *      d. Every Q3_HEARTBEAT_INTERVAL seconds, send heartbeats (all servers).
 *   5. On shutdown: close every open socket, free all maps.
 */
int relay_run(const relay_config_t *cfgs, int server_count,
              const mgmt_config_t *mgmt_cfg)
{
    /* --- Install SIGINT/SIGTERM handlers for graceful shutdown --- */
    struct sigaction sa = {0};
    sa.sa_handler = signal_handler;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* --- Create the epoll instance --- */
    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        log_error("epoll_create1(): %s", strerror(errno));
        return -1;
    }

    /* --- Create per-server instances --- */
    server_instance_t *servers = calloc(RELAY_MAX_SERVERS,
                                        sizeof(server_instance_t));
    if (!servers) {
        log_error("Failed to allocate server instances");
        close(epoll_fd);
        return -1;
    }

    /* Mutable config array for dynamically added servers.
     * Startup configs are copied here so cfg pointers stay valid. */
    relay_config_t *dyn_cfgs = calloc(RELAY_MAX_SERVERS,
                                      sizeof(relay_config_t));
    if (!dyn_cfgs) {
        log_error("Failed to allocate config array");
        free(servers);
        close(epoll_fd);
        return -1;
    }

    /* Copy initial configs into the dynamic array */
    for (int s = 0; s < server_count && s < RELAY_MAX_SERVERS; s++)
        dyn_cfgs[s] = cfgs[s];

    int init_count = 0; /* How many servers we've successfully initialised */
    mgmt_state_t mgmt_state;
    int mgmt_active = 0;

    for (int s = 0; s < server_count; s++) {
        server_instance_t *srv = &servers[s];
        srv->cfg   = &dyn_cfgs[s];
        srv->index = s;

        /* Create the public-facing listen socket */
        srv->listen_fd = create_listen_socket(srv->cfg->listen_port);
        if (srv->listen_fd < 0)
            goto cleanup;

        /* Initialise the session hash map */
        int total_cap = srv->cfg->max_clients + srv->cfg->max_query_sessions;
        if (session_map_init(&srv->sessions, total_cap) < 0) {
            log_error("Server #%d: failed to initialize session map", s + 1);
            close(srv->listen_fd);
            srv->listen_fd = -1;
            goto cleanup;
        }

        /* Initialise the rate limiter */
        srv->rate_limiter.max_per_sec = srv->cfg->max_new_per_sec;

        /* Sweep and heartbeat timers */
        srv->last_sweep = time(NULL);
        srv->last_heartbeat = 0; /* Fire immediately on first loop */

        /* Register the listen socket with epoll */
        struct epoll_event ev = {0};
        ev.events   = EPOLLIN;
        ev.data.u64 = pack_epoll_data(s, srv->listen_fd);
        epoll_ctl(epoll_fd, EPOLL_CTL_ADD, srv->listen_fd, &ev);

        /* Log the forwarding target */
        char remote_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &srv->cfg->remote_addr.sin_addr,
                  remote_str, sizeof(remote_str));
        log_info("Server #%d: :%u -> %s:%u (max %d clients, %ds timeout, "
                 "query pool %d, %ds query timeout)",
                 s + 1, srv->cfg->listen_port,
                 remote_str, ntohs(srv->cfg->remote_addr.sin_port),
                 srv->cfg->max_clients, srv->cfg->session_timeout,
                 srv->cfg->max_query_sessions, srv->cfg->query_timeout);

        srv->active = 1;
        init_count++;
    }

    /* --- Initialise management API (if configured) --- */
    if (mgmt_cfg && mgmt_cfg->enabled) {
        if (mgmt_init(&mgmt_state, mgmt_cfg, epoll_fd,
                      servers, init_count, dyn_cfgs) == 0)
            mgmt_active = 1;
        else
            log_warn("Management API failed to start — continuing without it");
    }

    /* Packet buffers — stack-allocated, reused every iteration */
    uint8_t recv_buf[RECV_BUF_SIZE];
    uint8_t rewrite_buf[RECV_BUF_SIZE];
    struct epoll_event events[MAX_EPOLL_EVENTS];

    /* ============================================================== */
    /*  Main event loop                                               */
    /* ============================================================== */
    while (g_running) {
        int wait_ms = SWEEP_INTERVAL * 1000;
        int nfds = epoll_wait(epoll_fd, events, MAX_EPOLL_EVENTS, wait_ms);

        if (nfds < 0) {
            if (errno == EINTR)
                continue;
            log_error("epoll_wait(): %s", strerror(errno));
            break;
        }

        time_t now = time(NULL);

        for (int i = 0; i < nfds; i++) {
            int srv_idx = unpack_server_index(events[i].data.u64);
            int fd      = unpack_fd(events[i].data.u64);

            /* Route management events to the mgmt handler */
            if (srv_idx == MGMT_SERVER_INDEX) {
                if (mgmt_active)
                    mgmt_handle_event(&mgmt_state, fd);
                continue;
            }

            if (srv_idx < 0 || srv_idx >= RELAY_MAX_SERVERS)
                continue;

            server_instance_t *srv = &servers[srv_idx];
            if (!srv->active)
                continue;
            const relay_config_t *cfg = srv->cfg;

            if (fd == srv->listen_fd) {
                /* ================================================== */
                /*  Packet from a game client (on the listen socket)   */
                /* ================================================== */
                struct sockaddr_in client_addr;
                socklen_t addr_len = sizeof(client_addr);
                ssize_t n = recvfrom(srv->listen_fd, recv_buf,
                                     sizeof(recv_buf), 0,
                                     (struct sockaddr *)&client_addr,
                                     &addr_len);
                if (n <= 0)
                    continue;

                if ((size_t)n > Q3_MAX_PACKET_SIZE)
                    continue;

                session_t *sess = session_find_by_addr(&srv->sessions,
                                                       &client_addr);
                if (!sess) {
                    int is_query = q3_is_query(recv_buf, (size_t)n);

                    if (is_query) {
                        if (srv->query_count >= cfg->max_query_sessions) {
                            log_warn("Server #%d: max query sessions reached",
                                     srv_idx + 1);
                            continue;
                        }
                    } else {
                        if (!rate_limit_check(&srv->rate_limiter)) {
                            char drop_str[INET_ADDRSTRLEN];
                            inet_ntop(AF_INET, &client_addr.sin_addr,
                                      drop_str, sizeof(drop_str));
                            log_warn("Server #%d: rate limit (%d/sec) "
                                     "exceeded — dropping new connection "
                                     "from %s:%u",
                                     srv_idx + 1, cfg->max_new_per_sec,
                                     drop_str,
                                     ntohs(client_addr.sin_port));
                            continue;
                        }
                        int game_count = srv->sessions.count -
                                         srv->query_count;
                        if (game_count >= cfg->max_clients) {
                            char drop_str[INET_ADDRSTRLEN];
                            inet_ntop(AF_INET, &client_addr.sin_addr,
                                      drop_str, sizeof(drop_str));
                            log_warn("Server #%d: max clients (%d) "
                                     "reached — dropping new connection "
                                     "from %s:%u",
                                     srv_idx + 1, cfg->max_clients,
                                     drop_str,
                                     ntohs(client_addr.sin_port));
                            continue;
                        }
                    }

                    int relay_fd = create_relay_socket();
                    if (relay_fd < 0) {
                        log_error("Failed to create relay socket: %s",
                                  strerror(errno));
                        continue;
                    }

                    if (connect(relay_fd,
                                (struct sockaddr *)&cfg->remote_addr,
                                sizeof(cfg->remote_addr)) < 0) {
                        log_error("connect() relay socket: %s",
                                  strerror(errno));
                        close(relay_fd);
                        continue;
                    }

                    sess = session_insert(&srv->sessions, &client_addr,
                                          relay_fd);
                    if (!sess) {
                        close(relay_fd);
                        continue;
                    }
                    sess->is_query = is_query;
                    if (is_query)
                        srv->query_count++;

                    /* Register relay fd with same server index */
                    struct epoll_event rev = {0};
                    rev.events   = EPOLLIN;
                    rev.data.u64 = pack_epoll_data(srv_idx, relay_fd);
                    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, relay_fd, &rev);

                    char addr_str[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &client_addr.sin_addr,
                              addr_str, sizeof(addr_str));
                    log_info("Server #%d: new %s session: %s:%u "
                             "(relay fd=%d, total=%d, queries=%d)",
                             srv_idx + 1,
                             is_query ? "query" : "game",
                             addr_str, ntohs(client_addr.sin_port),
                             relay_fd, srv->sessions.count,
                             srv->query_count);
                }

                /*
                 * Session promotion: query → game.
                 *
                 * When a client that initially sent a browser query
                 * (getinfo/getstatus) follows up with a non-query
                 * packet (e.g. getchallenge, connect), they are
                 * transitioning from browsing to actually joining.
                 * Re-classify the session so it counts against the
                 * game client cap instead of the query session cap.
                 */
                if (sess->is_query &&
                    !q3_is_query(recv_buf, (size_t)n)) {
                    sess->is_query = 0;
                    if (srv->query_count > 0)
                        srv->query_count--;
                    char addr_str[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &sess->client_addr.sin_addr,
                              addr_str, sizeof(addr_str));
                    log_info("Server #%d: promoted query->game: %s:%u",
                             srv_idx + 1, addr_str,
                             ntohs(sess->client_addr.sin_port));
                }

                sess->last_activity = now;
                sess->pkts_to_server++;
                sess->bytes_to_server += (uint64_t)n;
                send(sess->relay_fd, recv_buf, (size_t)n, 0);

            } else {
                /* ================================================== */
                /*  Packet from the real server (on a relay socket)    */
                /* ================================================== */
                ssize_t n = recv(fd, recv_buf, sizeof(recv_buf), 0);
                if (n <= 0)
                    continue;

                session_t *sess = session_find_by_fd(&srv->sessions, fd);
                if (!sess)
                    continue;

                sess->last_activity = now;
                sess->pkts_to_client++;
                sess->bytes_to_client += (uint64_t)n;

                const uint8_t *send_data = recv_buf;
                size_t send_len = (size_t)n;

                /*
                 * Hostname rewriting: if a tag is configured and this
                 * is a connectionless response (statusResponse or
                 * infoResponse), try to prepend the tag to the
                 * sv_hostname value.  If the rewrite can't be applied
                 * (wrong packet type, key not found, or buffer too
                 * small), the original packet is forwarded unchanged.
                 */
                if (cfg->hostname_tag &&
                    q3_is_connectionless(recv_buf, (size_t)n)) {
                    size_t new_len = q3_rewrite_hostname(
                        recv_buf, (size_t)n,
                        rewrite_buf, sizeof(rewrite_buf),
                        cfg->hostname_tag);
                    if (new_len > 0) {
                        send_data = rewrite_buf;
                        send_len  = new_len;
                        log_debug("Server #%d: rewrote hostname with "
                                  "tag \"%s\"", srv_idx + 1,
                                  cfg->hostname_tag);
                    }
                }

                sendto(srv->listen_fd, send_data, send_len, 0,
                       (struct sockaddr *)&sess->client_addr,
                       sizeof(sess->client_addr));
            }
        }

        /* ---------------------------------------------------------- */
        /*  Periodic timeout sweep and heartbeats — all servers       */
        /* ---------------------------------------------------------- */
        for (int s = 0; s < RELAY_MAX_SERVERS; s++) {
            server_instance_t *srv = &servers[s];
            if (!srv->active)
                continue;

            if (now - srv->last_sweep >= SWEEP_INTERVAL) {
                do_timeout_sweep(&srv->sessions, epoll_fd,
                                 srv->cfg->session_timeout,
                                 srv->cfg->query_timeout,
                                 &srv->query_count);
                srv->last_sweep = now;
            }

            if (srv->cfg->master_count > 0 &&
                srv->cfg->heartbeat_enabled &&
                now - srv->last_heartbeat >= Q3_HEARTBEAT_INTERVAL) {
                send_heartbeats(srv->listen_fd, srv->cfg);
                srv->last_heartbeat = now;
            }
        }
    }

    /* ============================================================== */
    /*  Clean shutdown                                                */
    /*                                                                */
    /*  Reached either when g_running is cleared by the signal        */
    /*  handler (graceful) or on a fatal initialisation failure       */
    /*  (goto cleanup).  We tear down every server that was           */
    /*  successfully initialised: close all relay sockets, free the   */
    /*  session map, and close the listen socket.                     */
    /* ============================================================== */
cleanup:
    if (mgmt_active)
        mgmt_cleanup(&mgmt_state);

    log_info("Shutting down — closing server(s)...");

    for (int s = 0; s < RELAY_MAX_SERVERS; s++) {
        server_instance_t *srv = &servers[s];
        if (!srv->active)
            continue;

        int closed = 0;

        /* Close every active relay socket */
        for (int j = 0; j < srv->sessions.capacity; j++) {
            if (srv->sessions.sessions[j].active) {
                close(srv->sessions.sessions[j].relay_fd);
                closed++;
            }
        }

        log_info("Server #%d: closed %d relay socket(s)", s + 1, closed);

        session_map_destroy(&srv->sessions);

        if (srv->listen_fd >= 0)
            close(srv->listen_fd);
    }

    free(servers);
    free(dyn_cfgs);
    close(epoll_fd);

    log_info("Clean shutdown complete");
    return g_running ? -1 : 0;
}

/* ================================================================== */
/*  Dynamic server add / remove                                       */
/* ================================================================== */

int relay_add_server(server_instance_t *servers, int *server_count,
                     relay_config_t *dyn_cfgs, const relay_config_t *cfg,
                     int epoll_fd)
{
    /* Find a free slot */
    int slot = -1;
    for (int i = 0; i < RELAY_MAX_SERVERS; i++) {
        if (!servers[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        log_error("Cannot add server — all %d slots in use", RELAY_MAX_SERVERS);
        return -1;
    }

    /* Check for duplicate listen port */
    for (int i = 0; i < RELAY_MAX_SERVERS; i++) {
        if (servers[i].active && servers[i].cfg->listen_port == cfg->listen_port) {
            log_error("Cannot add server — port %u already in use by server #%d",
                      cfg->listen_port, i + 1);
            return -1;
        }
    }

    /* Copy config into the persistent array */
    dyn_cfgs[slot] = *cfg;

    /* Initialise the server instance */
    server_instance_t *srv = &servers[slot];
    memset(srv, 0, sizeof(*srv));
    srv->cfg   = &dyn_cfgs[slot];
    srv->index = slot;

    srv->listen_fd = create_listen_socket(srv->cfg->listen_port);
    if (srv->listen_fd < 0)
        return -1;

    int total_cap = srv->cfg->max_clients + srv->cfg->max_query_sessions;
    if (session_map_init(&srv->sessions, total_cap) < 0) {
        log_error("Server #%d: failed to initialize session map", slot + 1);
        close(srv->listen_fd);
        return -1;
    }

    srv->rate_limiter.max_per_sec = srv->cfg->max_new_per_sec;
    srv->last_sweep     = time(NULL);
    srv->last_heartbeat = 0;

    struct epoll_event ev = {0};
    ev.events   = EPOLLIN;
    ev.data.u64 = pack_epoll_data(slot, srv->listen_fd);
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, srv->listen_fd, &ev);

    srv->active = 1;
    (*server_count)++;

    char remote_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &srv->cfg->remote_addr.sin_addr,
              remote_str, sizeof(remote_str));
    log_info("Server #%d added: :%u -> %s:%u", slot + 1,
             srv->cfg->listen_port, remote_str,
             ntohs(srv->cfg->remote_addr.sin_port));

    return slot;
}

int relay_remove_server(server_instance_t *servers, int *server_count,
                        int server_idx, int epoll_fd)
{
    if (server_idx < 0 || server_idx >= RELAY_MAX_SERVERS ||
        !servers[server_idx].active) {
        log_error("Cannot remove server #%d — invalid or inactive",
                  server_idx + 1);
        return -1;
    }

    server_instance_t *srv = &servers[server_idx];

    /* Close all relay sockets (kick all sessions) */
    int closed = 0;
    for (int j = 0; j < srv->sessions.capacity; j++) {
        if (srv->sessions.sessions[j].active) {
            epoll_ctl(epoll_fd, EPOLL_CTL_DEL,
                      srv->sessions.sessions[j].relay_fd, NULL);
            close(srv->sessions.sessions[j].relay_fd);
            closed++;
        }
    }

    session_map_destroy(&srv->sessions);

    /* Remove listen socket from epoll and close it */
    if (srv->listen_fd >= 0) {
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, srv->listen_fd, NULL);
        close(srv->listen_fd);
    }

    log_info("Server #%d removed (closed %d sessions)", server_idx + 1, closed);

    srv->active = 0;
    (*server_count)--;

    return 0;
}
