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

#define RECV_BUF_SIZE  (Q3_MAX_PACKET_SIZE + 64)
#define MAX_EPOLL_EVENTS 64
#define SWEEP_INTERVAL   5   /* seconds between timeout sweeps */

static volatile sig_atomic_t g_running = 1;

static void signal_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int set_socket_buffers(int fd, int size)
{
    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size)) < 0)
        return -1;
    if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) < 0)
        return -1;
    return 0;
}

static int create_listen_socket(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        log_error("socket(): %s", strerror(errno));
        return -1;
    }

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

static int create_relay_socket(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;

    set_socket_buffers(fd, 256 * 1024);
    set_nonblocking(fd);

    /* Bind to ephemeral port */
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

/* Rate limiter state */
typedef struct {
    time_t  window_start;
    int     count;
    int     max_per_sec;
} rate_limiter_t;

static int rate_limit_check(rate_limiter_t *rl)
{
    time_t now = time(NULL);
    if (now != rl->window_start) {
        rl->window_start = now;
        rl->count = 0;
    }
    if (rl->count >= rl->max_per_sec)
        return 0; /* rejected */
    rl->count++;
    return 1; /* allowed */
}

/* Timeout sweep context */
typedef struct {
    session_map_t *map;
    int            epoll_fd;
    time_t         now;
    int            timeout;
    int            expired_count;
    /* Collect sessions to remove (can't remove during iteration safely) */
    session_t    **to_remove;
    int            to_remove_count;
    int            to_remove_cap;
} sweep_ctx_t;

static void sweep_cb(session_t *s, void *ctx)
{
    sweep_ctx_t *sw = ctx;
    if (sw->now - s->last_activity > sw->timeout) {
        if (sw->to_remove_count < sw->to_remove_cap)
            sw->to_remove[sw->to_remove_count++] = s;
    }
}

static void do_timeout_sweep(session_map_t *map, int epoll_fd, int timeout)
{
    sweep_ctx_t sw = {0};
    sw.map     = map;
    sw.epoll_fd = epoll_fd;
    sw.now     = time(NULL);
    sw.timeout = timeout;

    /* Allocate worst-case array */
    sw.to_remove_cap = map->count;
    if (sw.to_remove_cap == 0)
        return;
    sw.to_remove = malloc((size_t)sw.to_remove_cap * sizeof(session_t *));
    if (!sw.to_remove)
        return;

    session_foreach(map, sweep_cb, &sw);

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

int relay_run(const relay_config_t *cfg)
{
    /* Install signal handlers */
    struct sigaction sa = {0};
    sa.sa_handler = signal_handler;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Create listen socket */
    int listen_fd = create_listen_socket(cfg->listen_port);
    if (listen_fd < 0)
        return -1;

    /* Create epoll instance */
    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        log_error("epoll_create1(): %s", strerror(errno));
        close(listen_fd);
        return -1;
    }

    /* Add listen socket to epoll */
    struct epoll_event ev = {0};
    ev.events  = EPOLLIN;
    ev.data.fd = listen_fd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev);

    /* Initialize session map */
    session_map_t sessions;
    if (session_map_init(&sessions, cfg->max_clients) < 0) {
        log_error("Failed to initialize session map");
        close(listen_fd);
        close(epoll_fd);
        return -1;
    }

    /* Rate limiter */
    rate_limiter_t rate_limiter = {0};
    rate_limiter.max_per_sec = cfg->max_new_per_sec;

    char remote_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &cfg->remote_addr.sin_addr, remote_str, sizeof(remote_str));
    log_info("Forwarding to %s:%u via WireGuard",
             remote_str, ntohs(cfg->remote_addr.sin_port));
    log_info("Max clients: %d, session timeout: %ds",
             cfg->max_clients, cfg->session_timeout);

    uint8_t recv_buf[RECV_BUF_SIZE];
    uint8_t rewrite_buf[RECV_BUF_SIZE];
    struct epoll_event events[MAX_EPOLL_EVENTS];
    time_t last_sweep = time(NULL);

    while (g_running) {
        /* Timeout for epoll: wake up periodically for sweeps */
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
            int fd = events[i].data.fd;

            if (fd == listen_fd) {
                /* ---- Packet from a client ---- */
                struct sockaddr_in client_addr;
                socklen_t addr_len = sizeof(client_addr);
                ssize_t n = recvfrom(listen_fd, recv_buf, sizeof(recv_buf), 0,
                                     (struct sockaddr *)&client_addr, &addr_len);
                if (n <= 0)
                    continue;

                /* Drop oversized packets */
                if ((size_t)n > Q3_MAX_PACKET_SIZE)
                    continue;

                /* Find or create session */
                session_t *sess = session_find_by_addr(&sessions, &client_addr);
                if (!sess) {
                    /* New client — check rate limit and capacity */
                    if (!rate_limit_check(&rate_limiter)) {
                        log_warn("Rate limit: dropping new client");
                        continue;
                    }
                    if (sessions.count >= cfg->max_clients) {
                        log_warn("Max clients reached, dropping new connection");
                        continue;
                    }

                    int relay_fd = create_relay_socket();
                    if (relay_fd < 0) {
                        log_error("Failed to create relay socket: %s",
                                  strerror(errno));
                        continue;
                    }

                    /* Connect relay socket to real server for convenience */
                    if (connect(relay_fd, (struct sockaddr *)&cfg->remote_addr,
                                sizeof(cfg->remote_addr)) < 0) {
                        log_error("connect() relay socket: %s", strerror(errno));
                        close(relay_fd);
                        continue;
                    }

                    sess = session_insert(&sessions, &client_addr, relay_fd);
                    if (!sess) {
                        close(relay_fd);
                        continue;
                    }

                    /* Add relay socket to epoll */
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

                sess->last_activity = now;
                sess->pkts_to_server++;
                sess->bytes_to_server += (uint64_t)n;

                /* Forward to real server via relay socket */
                send(sess->relay_fd, recv_buf, (size_t)n, 0);

            } else {
                /* ---- Packet from real server (on a relay socket) ---- */
                ssize_t n = recv(fd, recv_buf, sizeof(recv_buf), 0);
                if (n <= 0)
                    continue;

                session_t *sess = session_find_by_fd(&sessions, fd);
                if (!sess)
                    continue;

                sess->last_activity = now;
                sess->pkts_to_client++;
                sess->bytes_to_client += (uint64_t)n;

                /* Optional hostname rewrite for server browser responses */
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

                /* Forward to client via listen socket */
                sendto(listen_fd, send_data, send_len, 0,
                       (struct sockaddr *)&sess->client_addr,
                       sizeof(sess->client_addr));
            }
        }

        /* Periodic timeout sweep */
        if (now - last_sweep >= SWEEP_INTERVAL) {
            do_timeout_sweep(&sessions, epoll_fd, cfg->session_timeout);
            last_sweep = now;
        }
    }

    log_info("Shutting down...");

    /* Close all relay sockets */
    for (int i = 0; i < sessions.capacity; i++) {
        if (sessions.sessions[i].active) {
            close(sessions.sessions[i].relay_fd);
        }
    }

    session_map_destroy(&sessions);
    close(listen_fd);
    close(epoll_fd);

    log_info("Clean shutdown complete");
    return 0;
}
