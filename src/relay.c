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
 * The loop exits cleanly on SIGINT or SIGTERM, closing all sockets and
 * freeing all allocated memory.
 *
 * Data flow:
 *
 *   Player  ─── UDP ───►  listen_fd  ─── send() ───►  relay_fd  ─── WG ───►  Server
 *   Player  ◄── sendto ──  listen_fd  ◄── recv() ────  relay_fd  ◄── WG ────  Server
 */

#define _POSIX_C_SOURCE 200809L

#include "relay.h"
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
 * rate_limiter_t — Sliding-window rate limiter (1-second granularity).
 *
 * Tracks how many new sessions have been created in the current
 * calendar second.  Resets the counter when the second rolls over.
 */
typedef struct {
    time_t  window_start;   /* Start of the current 1-second window */
    int     count;          /* Sessions created in this window       */
    int     max_per_sec;    /* Configured cap                        */
} rate_limiter_t;

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
    int            timeout;
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
    if (sw->now - s->last_activity > sw->timeout) {
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
 * @param map         Session hash map.
 * @param epoll_fd    epoll instance for relay fd cleanup.
 * @param timeout     Inactivity timeout in seconds.
 */
static void do_timeout_sweep(session_map_t *map, int epoll_fd, int timeout)
{
    sweep_ctx_t sw = {0};
    sw.map     = map;
    sw.epoll_fd = epoll_fd;
    sw.now     = time(NULL);
    sw.timeout = timeout;

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
        log_info("Session expired: %s:%u (pkts: %lu/%lu, bytes: %lu/%lu)",
                 addr_str, ntohs(s->client_addr.sin_port),
                 (unsigned long)s->pkts_to_server,
                 (unsigned long)s->pkts_to_client,
                 (unsigned long)s->bytes_to_server,
                 (unsigned long)s->bytes_to_client);

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

/* ================================================================== */
/*  Main event loop                                                   */
/* ================================================================== */

/*
 * relay_run — Run the proxy event loop until shutdown.
 *
 * High-level flow:
 *   1. Install signal handlers (SIGINT, SIGTERM → g_running = 0).
 *   2. Create the listen socket and an epoll instance.
 *   3. Initialise the session hash map and rate limiter.
 *   4. Loop:
 *      a. epoll_wait() with a timeout equal to SWEEP_INTERVAL.
 *      b. For each ready fd:
 *         - listen_fd: receive client packet, find/create session, forward.
 *         - relay_fd:  receive server response, find session, relay back.
 *      c. Every SWEEP_INTERVAL seconds, sweep expired sessions.
 *      d. Every Q3_HEARTBEAT_INTERVAL seconds, send heartbeats to masters.
 *   5. On shutdown: close every open socket, free the map.
 */
int relay_run(const relay_config_t *cfg)
{
    /* --- Install SIGINT/SIGTERM handlers for graceful shutdown --- */
    struct sigaction sa = {0};
    sa.sa_handler = signal_handler;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* --- Create the public-facing listen socket --- */
    int listen_fd = create_listen_socket(cfg->listen_port);
    if (listen_fd < 0)
        return -1;

    /* --- Create the epoll instance --- */
    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        log_error("epoll_create1(): %s", strerror(errno));
        close(listen_fd);
        return -1;
    }

    /* Register the listen socket for read events */
    struct epoll_event ev = {0};
    ev.events  = EPOLLIN;
    ev.data.fd = listen_fd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev);

    /* --- Initialise the session hash map --- */
    session_map_t sessions;
    if (session_map_init(&sessions, cfg->max_clients) < 0) {
        log_error("Failed to initialize session map");
        close(listen_fd);
        close(epoll_fd);
        return -1;
    }

    /* --- Initialise the rate limiter --- */
    rate_limiter_t rate_limiter = {0};
    rate_limiter.max_per_sec = cfg->max_new_per_sec;

    /* Log the forwarding target for operator visibility */
    char remote_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &cfg->remote_addr.sin_addr, remote_str, sizeof(remote_str));
    log_info("Forwarding to %s:%u via WireGuard",
             remote_str, ntohs(cfg->remote_addr.sin_port));
    log_info("Max clients: %d, session timeout: %ds",
             cfg->max_clients, cfg->session_timeout);

    /* Packet buffers — stack-allocated, reused every iteration */
    uint8_t recv_buf[RECV_BUF_SIZE];      /* Raw received packet       */
    uint8_t rewrite_buf[RECV_BUF_SIZE];   /* Hostname-rewritten packet */
    struct epoll_event events[MAX_EPOLL_EVENTS];
    time_t last_sweep = time(NULL);

    /*
     * Heartbeat timer — initialised to 0 so the first heartbeat fires
     * immediately on startup, then every Q3_HEARTBEAT_INTERVAL seconds.
     */
    time_t last_heartbeat = 0;

    /* ============================================================== */
    /*  Main event loop                                               */
    /* ============================================================== */
    while (g_running) {
        /* Block until packets arrive or it's time for a sweep */
        int wait_ms = SWEEP_INTERVAL * 1000;
        int nfds = epoll_wait(epoll_fd, events, MAX_EPOLL_EVENTS, wait_ms);

        if (nfds < 0) {
            if (errno == EINTR)
                continue;  /* Interrupted by signal — re-check g_running */
            log_error("epoll_wait(): %s", strerror(errno));
            break;
        }

        time_t now = time(NULL);

        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;

            if (fd == listen_fd) {
                /* ================================================== */
                /*  Packet from a game client (on the listen socket)   */
                /* ================================================== */
                struct sockaddr_in client_addr;
                socklen_t addr_len = sizeof(client_addr);
                ssize_t n = recvfrom(listen_fd, recv_buf, sizeof(recv_buf), 0,
                                     (struct sockaddr *)&client_addr, &addr_len);
                if (n <= 0)
                    continue;

                /* Silently drop oversized packets */
                if ((size_t)n > Q3_MAX_PACKET_SIZE)
                    continue;

                /* Look up the client's existing session, if any */
                session_t *sess = session_find_by_addr(&sessions, &client_addr);
                if (!sess) {
                    /*
                     * New client — enforce rate limit and capacity before
                     * allocating resources.
                     */
                    if (!rate_limit_check(&rate_limiter)) {
                        log_warn("Rate limit: dropping new client");
                        continue;
                    }
                    if (sessions.count >= cfg->max_clients) {
                        log_warn("Max clients reached, dropping new connection");
                        continue;
                    }

                    /* Create a dedicated relay socket for this client */
                    int relay_fd = create_relay_socket();
                    if (relay_fd < 0) {
                        log_error("Failed to create relay socket: %s",
                                  strerror(errno));
                        continue;
                    }

                    /*
                     * connect() the relay socket to the real server so we
                     * can use send()/recv() instead of sendto()/recvfrom(),
                     * and the kernel will filter responses for us.
                     */
                    if (connect(relay_fd, (struct sockaddr *)&cfg->remote_addr,
                                sizeof(cfg->remote_addr)) < 0) {
                        log_error("connect() relay socket: %s", strerror(errno));
                        close(relay_fd);
                        continue;
                    }

                    /* Register the session in the hash map */
                    sess = session_insert(&sessions, &client_addr, relay_fd);
                    if (!sess) {
                        close(relay_fd);
                        continue;
                    }

                    /* Add the relay socket to epoll so we get server responses */
                    struct epoll_event rev = {0};
                    rev.events  = EPOLLIN;
                    rev.data.fd = relay_fd;
                    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, relay_fd, &rev);

                    char addr_str[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &client_addr.sin_addr,
                              addr_str, sizeof(addr_str));
                    log_info("New session: %s:%u (relay fd=%d, total=%d)",
                             addr_str, ntohs(client_addr.sin_port),
                             relay_fd, sessions.count);
                }

                /* Update activity timestamp and traffic counters */
                sess->last_activity = now;
                sess->pkts_to_server++;
                sess->bytes_to_server += (uint64_t)n;

                /* Forward the packet to the real server via the relay socket */
                send(sess->relay_fd, recv_buf, (size_t)n, 0);

            } else {
                /* ================================================== */
                /*  Packet from the real server (on a relay socket)    */
                /* ================================================== */
                ssize_t n = recv(fd, recv_buf, sizeof(recv_buf), 0);
                if (n <= 0)
                    continue;

                /* Find which client this relay socket belongs to */
                session_t *sess = session_find_by_fd(&sessions, fd);
                if (!sess)
                    continue;

                /* Update activity timestamp and traffic counters */
                sess->last_activity = now;
                sess->pkts_to_client++;
                sess->bytes_to_client += (uint64_t)n;

                /*
                 * Optional hostname rewrite: if a hostname tag is configured
                 * and this is a connectionless server browser response
                 * (statusResponse or infoResponse), prepend the tag to
                 * sv_hostname so players see e.g. "[PROXY] ServerName".
                 */
                const uint8_t *send_data = recv_buf;
                size_t send_len = (size_t)n;

                if (cfg->hostname_tag &&
                    q3_is_connectionless(recv_buf, (size_t)n)) {
                    size_t new_len = q3_rewrite_hostname(
                        recv_buf, (size_t)n,
                        rewrite_buf, sizeof(rewrite_buf),
                        cfg->hostname_tag);
                    if (new_len > 0) {
                        send_data = rewrite_buf;
                        send_len  = new_len;
                    }
                }

                /* Relay the (possibly rewritten) response back to the client */
                sendto(listen_fd, send_data, send_len, 0,
                       (struct sockaddr *)&sess->client_addr,
                       sizeof(sess->client_addr));
            }
        }

        /* ---------------------------------------------------------- */
        /*  Periodic timeout sweep                                    */
        /* ---------------------------------------------------------- */
        if (now - last_sweep >= SWEEP_INTERVAL) {
            do_timeout_sweep(&sessions, epoll_fd, cfg->session_timeout);
            last_sweep = now;
        }

        /* ---------------------------------------------------------- */
        /*  Periodic heartbeat to master server(s)                    */
        /* ---------------------------------------------------------- */
        if (cfg->master_count > 0 &&
            now - last_heartbeat >= Q3_HEARTBEAT_INTERVAL) {
            send_heartbeats(listen_fd, cfg);
            last_heartbeat = now;
        }
    }

    /* ============================================================== */
    /*  Clean shutdown                                                */
    /* ============================================================== */
    log_info("Shutting down...");

    /* Close every active relay socket */
    for (int i = 0; i < sessions.capacity; i++) {
        if (sessions.sessions[i].active) {
            close(sessions.sessions[i].relay_fd);
        }
    }

    /* Free the session map, then close infrastructure fds */
    session_map_destroy(&sessions);
    close(listen_fd);
    close(epoll_fd);

    log_info("Clean shutdown complete");
    return 0;
}
