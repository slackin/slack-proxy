/*
 * mgmt.h — Remote management API for urt-proxy.
 *
 * Defines the configuration, state, and public interface for the TCP-based
 * management server.  The management socket integrates into the main epoll
 * loop using MGMT_SERVER_INDEX as a sentinel value in the packed epoll data,
 * so management events are routed separately from game-traffic events.
 *
 * Protocol: newline-delimited JSON over TCP, authenticated by shared API key.
 *
 * Commands:
 *   status                      — server configs + session/query counts
 *   sessions  {"server":N}      — list active sessions with traffic stats
 *   set       {"server":N,"key":"...","value":...}  — tune runtime param
 *   kick      {"server":N,"client":"IP:port"}        — close one session
 *   kick_all  {"server":N}      — close all sessions on a server
 *   add_server {"listen_port":N,"remote_host":"...","remote_port":N,...}
 *   remove_server {"server":N}  — remove a server (stops + kicks all)
 *   set_master {"server":N,"master_server":"host:port"}  — set/clear master
 */

#ifndef URT_MGMT_H
#define URT_MGMT_H

#include <stdint.h>
#include <netinet/in.h>

#include "relay.h"

/* ------------------------------------------------------------------ */
/*  Constants                                                         */
/* ------------------------------------------------------------------ */

/*
 * Sentinel value stored in the upper 32 bits of the epoll data u64
 * to distinguish management events from game-relay events.  Safe
 * because the maximum number of game servers is CONFIG_MAX_SERVERS (32).
 */
#define MGMT_SERVER_INDEX   0x7FFFFFFF

/* Default TCP port for the management API. */
#define MGMT_DEFAULT_PORT   29990

/* Maximum concurrent management client connections. */
#define MGMT_MAX_CLIENTS    4

/* Per-client line-buffer size (bytes).  One JSON command per line. */
#define MGMT_BUF_SIZE       4096

/* ------------------------------------------------------------------ */
/*  Forward declarations                                              */
/* ------------------------------------------------------------------ */

/* server_instance_t and relay_config_t are provided by relay.h */

/* ------------------------------------------------------------------ */
/*  Configuration                                                     */
/* ------------------------------------------------------------------ */

/*
 * mgmt_config_t — Management API settings from CLI flags or config file.
 *
 * The API is enabled only when `enabled` is non-zero (set automatically
 * when an API key is provided via --mgmt-key or mgmt-key config key).
 */
typedef struct mgmt_config {
    uint16_t            port;         /* TCP listen port (default 29990)      */
    struct sockaddr_in  listen_addr;  /* Resolved listen address              */
    const char         *api_key;      /* Shared secret for authentication     */
    int                 enabled;      /* Non-zero if management API is active */
} mgmt_config_t;

/* ------------------------------------------------------------------ */
/*  Runtime state                                                     */
/* ------------------------------------------------------------------ */

/*
 * mgmt_state_t — Live state of the management TCP server.
 *
 * Tracks the listening socket, connected clients (with per-client
 * authentication state and line buffers), and a pointer to the
 * server_instance_t array for querying / modifying game-server state.
 */
typedef struct {
    int                 listen_fd;                          /* TCP listen socket          */
    int                 epoll_fd;                           /* Shared epoll instance      */

    /* Per-client connection state */
    int                 client_fds[MGMT_MAX_CLIENTS];      /* -1 = slot free             */
    int                 client_authed[MGMT_MAX_CLIENTS];   /* 1 = authenticated          */
    char                client_bufs[MGMT_MAX_CLIENTS][MGMT_BUF_SIZE]; /* Line buffers    */
    int                 client_buf_len[MGMT_MAX_CLIENTS];  /* Bytes in each buffer       */
    int                 client_count;                       /* Number of connected clients*/

    /* References into the relay engine */
    const mgmt_config_t *config;                           /* Management configuration   */
    server_instance_t   *servers;                           /* Game-server instance array  */
    int                  server_count;                      /* Number of active game servers */
    relay_config_t      *dyn_cfgs;                          /* Mutable config array (for add) */
} mgmt_state_t;

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

/*
 * mgmt_init — Create TCP listener and register with epoll.
 *
 * @return 0 on success, -1 on error (details logged).
 */
int mgmt_init(mgmt_state_t *state, const mgmt_config_t *config,
              int epoll_fd, server_instance_t *servers, int server_count,
              relay_config_t *dyn_cfgs);

/*
 * mgmt_handle_event — Process an epoll event on a management fd.
 *
 * Called from the main event loop when the unpacked server index
 * equals MGMT_SERVER_INDEX.  Handles accept (if fd == listen_fd)
 * or client I/O (recv + line processing + response).
 */
void mgmt_handle_event(mgmt_state_t *state, int fd);

/*
 * mgmt_cleanup — Close all management connections and the listener.
 */
void mgmt_cleanup(mgmt_state_t *state);

#endif /* URT_MGMT_H */
