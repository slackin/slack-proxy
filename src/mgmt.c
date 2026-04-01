/*
 * mgmt.c — TCP-based JSON management API for urt-proxy.
 *
 * Implements a simple management server that integrates into the main
 * epoll event loop.  Supports newline-delimited JSON commands for:
 *   - Querying server status and active sessions
 *   - Tuning runtime parameters (max_clients, timeouts, rate limits)
 *   - Kicking individual sessions or all sessions on a server
 *
 * The JSON parser and writer are hand-written to avoid external
 * dependencies.  Only the minimal subset needed for the protocol is
 * supported (flat objects with string/int values).
 */

#define _POSIX_C_SOURCE 200809L

#include "mgmt.h"
#include "relay.h"
#include "hashmap.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <arpa/inet.h>

/* ------------------------------------------------------------------ */
/*  Minimal JSON helpers                                              */
/* ------------------------------------------------------------------ */

/* Maximum response buffer size. */
#define RESP_BUF_SIZE 8192

/*
 * json_find_string — Extract a string value for a given key from JSON.
 *
 * Searches for "key":"value" and copies the value into out.
 * Returns 1 if found, 0 if not.  Only handles simple flat JSON.
 */
static int json_find_string(const char *json, const char *key,
                            char *out, size_t out_size)
{
    char needle[256];
    int n = snprintf(needle, sizeof(needle), "\"%s\":\"", key);
    if (n < 0 || (size_t)n >= sizeof(needle))
        return 0;

    const char *start = strstr(json, needle);
    if (!start)
        return 0;
    start += (size_t)n;

    const char *end = strchr(start, '"');
    if (!end)
        return 0;

    size_t len = (size_t)(end - start);
    if (len >= out_size)
        len = out_size - 1;
    memcpy(out, start, len);
    out[len] = '\0';
    return 1;
}

/*
 * json_find_int — Extract an integer value for a given key from JSON.
 *
 * Searches for "key":N (no quotes around value) and returns the int.
 * Returns 1 if found, 0 if not.
 */
static int json_find_int(const char *json, const char *key, int *out)
{
    char needle[256];
    int n = snprintf(needle, sizeof(needle), "\"%s\":", key);
    if (n < 0 || (size_t)n >= sizeof(needle))
        return 0;

    const char *start = strstr(json, needle);
    if (!start)
        return 0;
    start += (size_t)n;

    /* Skip whitespace */
    while (*start == ' ')
        start++;

    if (*start == '"')
        return 0; /* It's a string, not an int */

    char *endp;
    long val = strtol(start, &endp, 10);
    if (endp == start)
        return 0;

    *out = (int)val;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  TCP helpers                                                       */
/* ------------------------------------------------------------------ */

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void send_response(int fd, const char *json)
{
    size_t len = strlen(json);
    /* Best-effort send — if the client is slow, we drop the response */
    (void)send(fd, json, len, MSG_NOSIGNAL);
    (void)send(fd, "\n", 1, MSG_NOSIGNAL);
}

static void send_ok(int fd, const char *data)
{
    char buf[RESP_BUF_SIZE];
    if (data) {
        snprintf(buf, sizeof(buf), "{\"ok\":true,\"data\":%s}", data);
    } else {
        snprintf(buf, sizeof(buf), "{\"ok\":true}");
    }
    send_response(fd, buf);
}

static void send_error(int fd, const char *msg)
{
    char buf[RESP_BUF_SIZE];
    snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}", msg);
    send_response(fd, buf);
}

/* ------------------------------------------------------------------ */
/*  Client connection management                                      */
/* ------------------------------------------------------------------ */

static int find_client_slot(mgmt_state_t *state, int fd)
{
    for (int i = 0; i < MGMT_MAX_CLIENTS; i++) {
        if (state->client_fds[i] == fd)
            return i;
    }
    return -1;
}

static void close_client(mgmt_state_t *state, int slot)
{
    int fd = state->client_fds[slot];
    if (fd < 0)
        return;

    epoll_ctl(state->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
    close(fd);

    state->client_fds[slot] = -1;
    state->client_authed[slot] = 0;
    state->client_buf_len[slot] = 0;
    state->client_count--;

    log_info("Management client disconnected (slot %d)", slot);
}

/* ------------------------------------------------------------------ */
/*  Command handlers                                                  */
/* ------------------------------------------------------------------ */

/*
 * handle_status — Return config and session counts for all servers.
 */
static void handle_status(mgmt_state_t *state, int client_fd)
{
    char buf[RESP_BUF_SIZE];
    int pos = 0;

    pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos,
                    "{\"server_count\":%d,\"servers\":[", state->server_count);

    int first = 1;
    for (int i = 0; i < RELAY_MAX_SERVERS; i++) {
        server_instance_t *srv = &state->servers[i];
        if (!srv->active)
            continue;

        char remote_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &srv->cfg->remote_addr.sin_addr,
                  remote_str, sizeof(remote_str));

        if (!first)
            pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, ",");
        first = 0;

        pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos,
            "{\"index\":%d,"
            "\"listen_port\":%u,"
            "\"remote_addr\":\"%s:%u\","
            "\"max_clients\":%d,"
            "\"session_timeout\":%d,"
            "\"query_timeout\":%d,"
            "\"max_new_per_sec\":%d,"
            "\"max_query_sessions\":%d,"
            "\"hostname_tag\":\"%s\","
            "\"active_sessions\":%d,"
            "\"query_sessions\":%d}",
            i,
            srv->cfg->listen_port,
            remote_str, ntohs(srv->cfg->remote_addr.sin_port),
            srv->cfg->max_clients,
            srv->cfg->session_timeout,
            srv->cfg->query_timeout,
            srv->cfg->max_new_per_sec,
            srv->cfg->max_query_sessions,
            srv->cfg->hostname_tag ? srv->cfg->hostname_tag : "",
            srv->sessions.count,
            srv->query_count);
    }

    pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "]}");
    (void)pos;
    send_ok(client_fd, buf);
}

/*
 * Session iteration callback for handle_sessions.
 */
typedef struct {
    char  *buf;
    int    pos;
    int    cap;
    int    first;
    time_t now;
} session_writer_ctx_t;

static void session_writer_cb(session_t *s, void *ctx)
{
    session_writer_ctx_t *sw = ctx;
    char addr_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &s->client_addr.sin_addr,
              addr_str, sizeof(addr_str));

    if (!sw->first)
        sw->pos += snprintf(sw->buf + sw->pos,
                            (size_t)(sw->cap - sw->pos), ",");
    sw->first = 0;

    sw->pos += snprintf(sw->buf + sw->pos, (size_t)(sw->cap - sw->pos),
        "{\"client\":\"%s:%u\","
        "\"relay_fd\":%d,"
        "\"is_query\":%d,"
        "\"idle_secs\":%ld,"
        "\"pkts_to_server\":%lu,"
        "\"pkts_to_client\":%lu,"
        "\"bytes_to_server\":%lu,"
        "\"bytes_to_client\":%lu}",
        addr_str, ntohs(s->client_addr.sin_port),
        s->relay_fd,
        s->is_query,
        (long)(sw->now - s->last_activity),
        (unsigned long)s->pkts_to_server,
        (unsigned long)s->pkts_to_client,
        (unsigned long)s->bytes_to_server,
        (unsigned long)s->bytes_to_client);
}

/*
 * handle_sessions — Return all active sessions for one server.
 */
static void handle_sessions(mgmt_state_t *state, int client_fd,
                            const char *json)
{
    int server_idx = -1;
    if (!json_find_int(json, "server", &server_idx) ||
        server_idx < 0 || server_idx >= RELAY_MAX_SERVERS ||
        !state->servers[server_idx].active) {
        send_error(client_fd, "invalid or missing 'server' index");
        return;
    }

    server_instance_t *srv = &state->servers[server_idx];
    char buf[RESP_BUF_SIZE];
    session_writer_ctx_t sw = {
        .buf = buf, .pos = 0, .cap = RESP_BUF_SIZE, .first = 1,
        .now = time(NULL)
    };

    sw.pos += snprintf(buf, sizeof(buf), "{\"server\":%d,\"sessions\":[",
                       server_idx);
    session_foreach(&srv->sessions, session_writer_cb, &sw);
    sw.pos += snprintf(buf + sw.pos, (size_t)(sw.cap - sw.pos), "]}");

    send_ok(client_fd, buf);
}

/*
 * handle_set — Tune a runtime parameter on a specific server.
 */
static void handle_set(mgmt_state_t *state, int client_fd, const char *json)
{
    int server_idx = -1;
    if (!json_find_int(json, "server", &server_idx) ||
        server_idx < 0 || server_idx >= RELAY_MAX_SERVERS ||
        !state->servers[server_idx].active) {
        send_error(client_fd, "invalid or missing 'server' index");
        return;
    }

    char key[64] = {0};
    if (!json_find_string(json, "key", key, sizeof(key))) {
        send_error(client_fd, "missing 'key'");
        return;
    }

    server_instance_t *srv = &state->servers[server_idx];
    /* Cast away const — management API is allowed to tune runtime params */
    relay_config_t *cfg = (relay_config_t *)srv->cfg;

    if (strcmp(key, "max_clients") == 0) {
        int val;
        if (!json_find_int(json, "value", &val) || val < 1 || val > 1000) {
            send_error(client_fd, "value must be 1-1000");
            return;
        }
        cfg->max_clients = val;
        log_info("Management: server #%d max_clients set to %d",
                 server_idx + 1, val);
    }
    else if (strcmp(key, "session_timeout") == 0) {
        int val;
        if (!json_find_int(json, "value", &val) || val < 5) {
            send_error(client_fd, "value must be >= 5");
            return;
        }
        cfg->session_timeout = val;
        log_info("Management: server #%d session_timeout set to %d",
                 server_idx + 1, val);
    }
    else if (strcmp(key, "query_timeout") == 0) {
        int val;
        if (!json_find_int(json, "value", &val) || val < 1) {
            send_error(client_fd, "value must be >= 1");
            return;
        }
        cfg->query_timeout = val;
        log_info("Management: server #%d query_timeout set to %d",
                 server_idx + 1, val);
    }
    else if (strcmp(key, "max_new_per_sec") == 0) {
        int val;
        if (!json_find_int(json, "value", &val) || val < 1) {
            send_error(client_fd, "value must be >= 1");
            return;
        }
        cfg->max_new_per_sec = val;
        srv->rate_limiter.max_per_sec = val;
        log_info("Management: server #%d max_new_per_sec set to %d",
                 server_idx + 1, val);
    }
    else if (strcmp(key, "max_query_sessions") == 0) {
        int val;
        if (!json_find_int(json, "value", &val) || val < 1 || val > 1000) {
            send_error(client_fd, "value must be 1-1000");
            return;
        }
        cfg->max_query_sessions = val;
        log_info("Management: server #%d max_query_sessions set to %d",
                 server_idx + 1, val);
    }
    else if (strcmp(key, "hostname_tag") == 0) {
        char val[256] = {0};
        if (json_find_string(json, "value", val, sizeof(val))) {
            if (val[0] == '\0') {
                cfg->hostname_tag = NULL;
            } else {
                /* strdup — intentional leak, value lives for process lifetime */
                cfg->hostname_tag = strdup(val);
            }
            log_info("Management: server #%d hostname_tag set to \"%s\"",
                     server_idx + 1, cfg->hostname_tag ? cfg->hostname_tag : "");
        } else {
            cfg->hostname_tag = NULL;
            log_info("Management: server #%d hostname_tag cleared",
                     server_idx + 1);
        }
    }
    else {
        send_error(client_fd, "unknown key");
        return;
    }

    send_ok(client_fd, NULL);
}

/*
 * handle_kick — Close a specific session by client address.
 */
static void handle_kick(mgmt_state_t *state, int client_fd, const char *json)
{
    int server_idx = -1;
    if (!json_find_int(json, "server", &server_idx) ||
        server_idx < 0 || server_idx >= RELAY_MAX_SERVERS ||
        !state->servers[server_idx].active) {
        send_error(client_fd, "invalid or missing 'server' index");
        return;
    }

    char client_str[64] = {0};
    if (!json_find_string(json, "client", client_str, sizeof(client_str))) {
        send_error(client_fd, "missing 'client' address");
        return;
    }

    /* Parse "IP:port" */
    char *colon = strrchr(client_str, ':');
    if (!colon) {
        send_error(client_fd, "invalid client address format (need IP:port)");
        return;
    }
    *colon = '\0';
    uint16_t port = (uint16_t)atoi(colon + 1);

    struct sockaddr_in target = {0};
    target.sin_family = AF_INET;
    target.sin_port   = htons(port);
    if (inet_pton(AF_INET, client_str, &target.sin_addr) != 1) {
        send_error(client_fd, "invalid client IP address");
        return;
    }

    server_instance_t *srv = &state->servers[server_idx];
    session_t *sess = session_find_by_addr(&srv->sessions, &target);
    if (!sess) {
        send_error(client_fd, "session not found");
        return;
    }

    if (sess->is_query && srv->query_count > 0)
        srv->query_count--;
    epoll_ctl(state->epoll_fd, EPOLL_CTL_DEL, sess->relay_fd, NULL);
    close(sess->relay_fd);
    session_remove(&srv->sessions, sess);

    *colon = ':'; /* Restore for logging */
    log_info("Management: kicked session %s on server #%d",
             client_str, server_idx + 1);
    send_ok(client_fd, NULL);
}

/*
 * Kick-all iteration context.
 */
typedef struct {
    session_t **to_remove;
    int         count;
    int         cap;
} kick_all_ctx_t;

static void kick_all_cb(session_t *s, void *ctx)
{
    kick_all_ctx_t *ka = ctx;
    if (ka->count < ka->cap)
        ka->to_remove[ka->count++] = s;
}

/*
 * handle_kick_all — Close all sessions on a specific server.
 */
static void handle_kick_all(mgmt_state_t *state, int client_fd,
                            const char *json)
{
    int server_idx = -1;
    if (!json_find_int(json, "server", &server_idx) ||
        server_idx < 0 || server_idx >= RELAY_MAX_SERVERS ||
        !state->servers[server_idx].active) {
        send_error(client_fd, "invalid or missing 'server' index");
        return;
    }

    server_instance_t *srv = &state->servers[server_idx];
    int total = srv->sessions.count;
    if (total == 0) {
        send_ok(client_fd, "{\"kicked\":0}");
        return;
    }

    /* Collect all sessions first (cannot modify during iteration) */
    session_t **to_remove = malloc((size_t)total * sizeof(session_t *));
    if (!to_remove) {
        send_error(client_fd, "out of memory");
        return;
    }

    kick_all_ctx_t ka = { .to_remove = to_remove, .count = 0, .cap = total };
    session_foreach(&srv->sessions, kick_all_cb, &ka);

    for (int i = 0; i < ka.count; i++) {
        session_t *s = ka.to_remove[i];
        if (s->is_query && srv->query_count > 0)
            srv->query_count--;
        epoll_ctl(state->epoll_fd, EPOLL_CTL_DEL, s->relay_fd, NULL);
        close(s->relay_fd);
        session_remove(&srv->sessions, s);
    }

    free(to_remove);

    char data[64];
    snprintf(data, sizeof(data), "{\"kicked\":%d}", ka.count);
    log_info("Management: kicked all %d sessions on server #%d",
             ka.count, server_idx + 1);
    send_ok(client_fd, data);
}

/*
 * handle_add_server — Add a new server at runtime.
 *
 * Required JSON fields: listen_port, remote_host, remote_port.
 * Optional: max_clients, session_timeout, query_timeout,
 *           max_new_per_sec, max_query_sessions, hostname_tag.
 */
static void handle_add_server(mgmt_state_t *state, int client_fd,
                              const char *json)
{
    relay_config_t cfg = {0};

    /* Required: listen_port */
    int port = 0;
    if (!json_find_int(json, "listen_port", &port) || port < 1 || port > 65535) {
        send_error(client_fd, "'listen_port' required (1-65535)");
        return;
    }
    cfg.listen_port = (uint16_t)port;

    /* Required: remote_host */
    char remote_host[256] = {0};
    if (!json_find_string(json, "remote_host", remote_host, sizeof(remote_host)) ||
        remote_host[0] == '\0') {
        send_error(client_fd, "'remote_host' required (IPv4 address)");
        return;
    }

    cfg.remote_addr.sin_family = AF_INET;
    if (inet_pton(AF_INET, remote_host, &cfg.remote_addr.sin_addr) != 1) {
        send_error(client_fd, "invalid 'remote_host' — must be IPv4 address");
        return;
    }

    /* Required: remote_port */
    int rport = 0;
    if (!json_find_int(json, "remote_port", &rport) || rport < 1 || rport > 65535) {
        send_error(client_fd, "'remote_port' required (1-65535)");
        return;
    }
    cfg.remote_addr.sin_port = htons((uint16_t)rport);

    /* Optional fields with defaults */
    int val;
    cfg.max_clients = 20;
    if (json_find_int(json, "max_clients", &val) && val >= 1 && val <= 1000)
        cfg.max_clients = val;

    cfg.session_timeout = 30;
    if (json_find_int(json, "session_timeout", &val) && val >= 5)
        cfg.session_timeout = val;

    cfg.query_timeout = 5;
    if (json_find_int(json, "query_timeout", &val) && val >= 1)
        cfg.query_timeout = val;

    cfg.max_new_per_sec = 5;
    if (json_find_int(json, "max_new_per_sec", &val) && val >= 1)
        cfg.max_new_per_sec = val;

    cfg.max_query_sessions = 100;
    if (json_find_int(json, "max_query_sessions", &val) && val >= 1 && val <= 1000)
        cfg.max_query_sessions = val;

    char tag[256] = {0};
    if (json_find_string(json, "hostname_tag", tag, sizeof(tag)) && tag[0] != '\0')
        cfg.hostname_tag = strdup(tag);

    int idx = relay_add_server(state->servers, &state->server_count,
                               state->dyn_cfgs, &cfg, state->epoll_fd);
    if (idx < 0) {
        send_error(client_fd, "failed to add server (check logs)");
        return;
    }

    char data[64];
    snprintf(data, sizeof(data), "{\"index\":%d}", idx);
    send_ok(client_fd, data);
}

/*
 * handle_remove_server — Remove a server at runtime.
 */
static void handle_remove_server(mgmt_state_t *state, int client_fd,
                                 const char *json)
{
    int server_idx = -1;
    if (!json_find_int(json, "server", &server_idx) ||
        server_idx < 0 || server_idx >= RELAY_MAX_SERVERS ||
        !state->servers[server_idx].active) {
        send_error(client_fd, "invalid or missing 'server' index");
        return;
    }

    if (relay_remove_server(state->servers, &state->server_count,
                            server_idx, state->epoll_fd) < 0) {
        send_error(client_fd, "failed to remove server (check logs)");
        return;
    }

    send_ok(client_fd, NULL);
}

/* ------------------------------------------------------------------ */
/*  Command dispatch                                                  */
/* ------------------------------------------------------------------ */

static void dispatch_command(mgmt_state_t *state, int client_fd,
                             const char *json)
{
    char cmd[32] = {0};
    if (!json_find_string(json, "cmd", cmd, sizeof(cmd))) {
        send_error(client_fd, "missing 'cmd' field");
        return;
    }

    log_debug("Management command: %s", cmd);

    if (strcmp(cmd, "status") == 0)
        handle_status(state, client_fd);
    else if (strcmp(cmd, "sessions") == 0)
        handle_sessions(state, client_fd, json);
    else if (strcmp(cmd, "set") == 0)
        handle_set(state, client_fd, json);
    else if (strcmp(cmd, "kick") == 0)
        handle_kick(state, client_fd, json);
    else if (strcmp(cmd, "kick_all") == 0)
        handle_kick_all(state, client_fd, json);
    else if (strcmp(cmd, "add_server") == 0)
        handle_add_server(state, client_fd, json);
    else if (strcmp(cmd, "remove_server") == 0)
        handle_remove_server(state, client_fd, json);
    else
        send_error(client_fd, "unknown command");
}

/* ------------------------------------------------------------------ */
/*  Line processing (newline-delimited JSON)                          */
/* ------------------------------------------------------------------ */

static void process_line(mgmt_state_t *state, int slot, char *line)
{
    int fd = state->client_fds[slot];

    /* First message must be authentication */
    if (!state->client_authed[slot]) {
        char auth_key[256] = {0};
        if (!json_find_string(line, "auth", auth_key, sizeof(auth_key))) {
            send_error(fd, "first message must be {\"auth\":\"<key>\"}");
            close_client(state, slot);
            return;
        }

        /* Constant-time comparison to prevent timing attacks */
        const char *expected = state->config->api_key;
        size_t expected_len = strlen(expected);
        size_t given_len = strlen(auth_key);
        size_t cmp_len = expected_len > given_len ? expected_len : given_len;

        volatile int diff = 0;
        for (size_t i = 0; i < cmp_len; i++) {
            char a = i < expected_len ? expected[i] : '\0';
            char b = i < given_len ? auth_key[i] : '\0';
            diff |= (a ^ b);
        }
        diff |= (int)(expected_len ^ given_len);

        if (diff != 0) {
            send_error(fd, "authentication failed");
            close_client(state, slot);
            return;
        }

        state->client_authed[slot] = 1;
        log_info("Management client authenticated (slot %d)", slot);
        send_ok(fd, NULL);
        return;
    }

    /* Authenticated — dispatch command */
    dispatch_command(state, fd, line);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

/*
 * pack_epoll_data — Encode a server index and fd into a single u64.
 *
 * Matches the packing scheme in relay.c so that the main event loop
 * can route events to either game-relay or management handlers.
 */
static inline uint64_t pack_epoll_data(int server_index, int fd)
{
    return ((uint64_t)(uint32_t)server_index << 32) | (uint64_t)(uint32_t)fd;
}

int mgmt_init(mgmt_state_t *state, const mgmt_config_t *config,
              int epoll_fd, server_instance_t *servers, int server_count,
              relay_config_t *dyn_cfgs)
{
    memset(state, 0, sizeof(*state));
    state->listen_fd    = -1;
    state->epoll_fd     = epoll_fd;
    state->config       = config;
    state->servers      = servers;
    state->server_count = server_count;
    state->dyn_cfgs     = dyn_cfgs;

    for (int i = 0; i < MGMT_MAX_CLIENTS; i++)
        state->client_fds[i] = -1;

    /* Create TCP listen socket */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        log_error("Management socket(): %s", strerror(errno));
        return -1;
    }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    set_nonblocking(fd);

    if (bind(fd, (struct sockaddr *)&config->listen_addr,
             sizeof(config->listen_addr)) < 0) {
        log_error("Management bind(): %s", strerror(errno));
        close(fd);
        return -1;
    }

    if (listen(fd, MGMT_MAX_CLIENTS) < 0) {
        log_error("Management listen(): %s", strerror(errno));
        close(fd);
        return -1;
    }

    /* Register with epoll using the management sentinel index */
    struct epoll_event ev = {0};
    ev.events   = EPOLLIN;
    ev.data.u64 = pack_epoll_data(MGMT_SERVER_INDEX, fd);
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev);

    state->listen_fd = fd;

    char addr_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &config->listen_addr.sin_addr,
              addr_str, sizeof(addr_str));
    log_info("Management API listening on %s:%u",
             addr_str, ntohs(config->listen_addr.sin_port));

    return 0;
}

void mgmt_handle_event(mgmt_state_t *state, int fd)
{
    /* Accept new connections on the listen socket */
    if (fd == state->listen_fd) {
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        int client_fd = accept(state->listen_fd,
                               (struct sockaddr *)&peer, &peer_len);
        if (client_fd < 0)
            return;

        if (state->client_count >= MGMT_MAX_CLIENTS) {
            const char *msg = "{\"ok\":false,\"error\":\"too many management connections\"}\n";
            (void)send(client_fd, msg, strlen(msg), MSG_NOSIGNAL);
            close(client_fd);
            return;
        }

        /* Find a free slot */
        int slot = -1;
        for (int i = 0; i < MGMT_MAX_CLIENTS; i++) {
            if (state->client_fds[i] == -1) {
                slot = i;
                break;
            }
        }
        if (slot == -1) {
            close(client_fd);
            return;
        }

        set_nonblocking(client_fd);
        state->client_fds[slot] = client_fd;
        state->client_authed[slot] = 0;
        state->client_buf_len[slot] = 0;
        state->client_count++;

        /* Register with epoll */
        struct epoll_event ev = {0};
        ev.events   = EPOLLIN;
        ev.data.u64 = pack_epoll_data(MGMT_SERVER_INDEX, client_fd);
        epoll_ctl(state->epoll_fd, EPOLL_CTL_ADD, client_fd, &ev);

        char addr_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &peer.sin_addr, addr_str, sizeof(addr_str));
        log_info("Management client connected from %s:%u (slot %d)",
                 addr_str, ntohs(peer.sin_port), slot);
        return;
    }

    /* Data from an existing management client */
    int slot = find_client_slot(state, fd);
    if (slot < 0)
        return;

    /* Read into the line buffer */
    int space = MGMT_BUF_SIZE - 1 - state->client_buf_len[slot];
    if (space <= 0) {
        send_error(fd, "request too large");
        close_client(state, slot);
        return;
    }

    ssize_t n = recv(fd, state->client_bufs[slot] + state->client_buf_len[slot],
                     (size_t)space, 0);
    if (n <= 0) {
        close_client(state, slot);
        return;
    }
    state->client_buf_len[slot] += (int)n;
    state->client_bufs[slot][state->client_buf_len[slot]] = '\0';

    /* Process complete lines (newline-delimited) */
    char *buf = state->client_bufs[slot];
    char *nl;
    while ((nl = strchr(buf, '\n')) != NULL) {
        *nl = '\0';
        if (nl > buf && *(nl - 1) == '\r')
            *(nl - 1) = '\0';

        process_line(state, slot, buf);

        /* Check if client was closed during processing */
        if (state->client_fds[slot] == -1)
            return;

        buf = nl + 1;
    }

    /* Move remaining partial data to the front of the buffer */
    int remaining = state->client_buf_len[slot] - (int)(buf - state->client_bufs[slot]);
    if (remaining > 0 && buf != state->client_bufs[slot])
        memmove(state->client_bufs[slot], buf, (size_t)remaining);
    state->client_buf_len[slot] = remaining;
}

void mgmt_cleanup(mgmt_state_t *state)
{
    for (int i = 0; i < MGMT_MAX_CLIENTS; i++) {
        if (state->client_fds[i] >= 0)
            close_client(state, i);
    }

    if (state->listen_fd >= 0) {
        epoll_ctl(state->epoll_fd, EPOLL_CTL_DEL, state->listen_fd, NULL);
        close(state->listen_fd);
        state->listen_fd = -1;
    }

    log_info("Management API shut down");
}
