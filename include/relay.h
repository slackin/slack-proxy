/*
 * relay.h — Core UDP relay configuration and entry point.
 *
 * Defines the relay_config_t structure that holds all runtime parameters
 * (listen port, remote server address, session limits, rate limiting, etc.)
 * and exposes relay_run(), the blocking event loop that drives the proxy.
 */

#ifndef URT_RELAY_H
#define URT_RELAY_H

#include <stdint.h>
#include <time.h>
#include <netinet/in.h>

#include "hashmap.h"

/* Forward declaration — full definition in mgmt.h */
typedef struct mgmt_config mgmt_config_t;

/* Maximum number of master servers the proxy can register with. */
#define RELAY_MAX_MASTERS 4

/*
 * relay_config_t — Runtime configuration for one proxied server.
 *
 * Populated from command-line arguments in main() (single-server mode)
 * or from config_load() (multi-server config file mode), then passed
 * to relay_run().  All fields must be set before calling relay_run();
 * use the init helpers in config.c or the defaults in main.c.
 *
 * Required fields:
 *   - remote_addr (must have a non-zero IP and port)
 *
 * Optional fields (have sensible defaults if zero-initialised):
 *   - listen_port        (default: 27960)
 *   - max_clients        (default: 20)
 *   - session_timeout    (default: 30 seconds)
 *   - hostname_tag       (default: NULL — no rewriting)
 *   - max_query_sessions (default: 100)
 *   - query_timeout      (default: 5 seconds)
 *   - max_new_per_sec    (default: 5)
 *   - master_addrs/count (default: 0 — no master registration)
 */
typedef struct {
    /* --- Network settings --- */
    uint16_t            listen_port;     /* Local UDP port to bind (e.g. 27960)       */
    struct sockaddr_in  remote_addr;     /* Real game server address (WireGuard IP:port) */

    /* --- Session management --- */
    int                 max_clients;     /* Max concurrent client sessions (1-1000)   */
    int                 session_timeout; /* Inactivity timeout in seconds (>= 5)      */
    const char         *hostname_tag;    /* Prefix for sv_hostname (e.g. "[PROXY]"), or NULL */

    /* --- Query session management --- */
    int                 max_query_sessions; /* Max concurrent browser query sessions   */
    int                 query_timeout;      /* Query session inactivity timeout (secs) */

    /* --- Rate limiting --- */
    int                 max_new_per_sec; /* Max new sessions created per second (>= 1) */

    /* --- Master server registration --- */
    struct sockaddr_in  master_addrs[RELAY_MAX_MASTERS]; /* Resolved master server addresses */
    int                 master_count;    /* Number of configured masters (0 = no registration) */
} relay_config_t;

/*
 * rate_limiter_t — Sliding-window rate limiter (1-second granularity).
 */
typedef struct {
    time_t  window_start;   /* Start of the current 1-second window */
    int     count;          /* Sessions created in this window       */
    int     max_per_sec;    /* Configured cap                        */
} rate_limiter_t;

/*
 * server_instance_t — All runtime state for a single proxied server.
 *
 * When multiple [server:*] sections are configured, relay_run() creates
 * one instance per section.  Each has its own listen socket, session
 * map, rate limiter, and sweep/heartbeat timers.
 */
struct server_instance {
    const relay_config_t *cfg;        /* Points into the cfgs array          */
    int                   listen_fd;  /* Public-facing UDP socket            */
    session_map_t         sessions;   /* Per-server session hash map         */
    int                   query_count;/* Active browser query sessions       */
    rate_limiter_t        rate_limiter;
    time_t                last_sweep;
    time_t                last_heartbeat; /* 0 → fire immediately on start   */
    int                   index;      /* Server index (for epoll routing)    */
};

/*
 * relay_run — Run the main relay event loop (blocking).
 *
 * Sets up a UDP listen socket per server, a shared epoll instance, and
 * per-server session hash maps, then enters the main loop: forwarding
 * client packets to the correct real server and relaying responses back.
 * Handles session creation, timeout sweeps, rate limiting, and optional
 * hostname rewriting — all independently per server instance.
 *
 * The loop runs until SIGINT or SIGTERM is received, at which point it
 * performs a clean shutdown (closes all sockets, frees all memory).
 *
 * @param cfgs          Array of fully-populated relay_config_t structs.
 * @param server_count  Number of entries in @a cfgs (1 or more).
 * @param mgmt_cfg      Management API config (NULL to disable).
 * @return              0 on clean shutdown, -1 on fatal error.
 */
int relay_run(const relay_config_t *cfgs, int server_count,
              const mgmt_config_t *mgmt_cfg);

#endif
