#ifndef URT_HASHMAP_H
#define URT_HASHMAP_H

#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include <netinet/in.h>

/*
 * Simple open-addressing hash map with two index structures:
 *   1. client_addr (ip:port) → session index
 *   2. relay_fd            → session index
 *
 * Sessions are stored in a flat array; the two hash tables
 * store indices into that array.
 */

typedef struct {
    struct sockaddr_in client_addr;
    int                relay_fd;
    time_t             last_activity;
    uint64_t           pkts_to_server;
    uint64_t           pkts_to_client;
    uint64_t           bytes_to_server;
    uint64_t           bytes_to_client;
    int                active;       /* 1 = in use, 0 = free */
} session_t;

typedef struct {
    session_t *sessions;       /* flat array of sessions */
    int        capacity;       /* max sessions (from --max-clients) */
    int        count;          /* current active sessions */

    /* Hash tables: store index into sessions[], -1 = empty */
    int       *addr_table;     /* keyed by client addr hash */
    int       *fd_table;       /* keyed by relay_fd hash */
    int        table_size;     /* size of both hash tables (power of 2) */
} session_map_t;

int          session_map_init(session_map_t *map, int max_clients);
void         session_map_destroy(session_map_t *map);

/* Lookup by client address. Returns session pointer or NULL. */
session_t   *session_find_by_addr(session_map_t *map,
                                  const struct sockaddr_in *addr);

/* Lookup by relay fd. Returns session pointer or NULL. */
session_t   *session_find_by_fd(session_map_t *map, int relay_fd);

/* Insert a new session. Returns session pointer or NULL if full. */
session_t   *session_insert(session_map_t *map,
                            const struct sockaddr_in *client_addr,
                            int relay_fd);

/* Remove a session (by its pointer). Frees the slot. */
void         session_remove(session_map_t *map, session_t *s);

/* Iterate all active sessions (for timeout sweeps). */
typedef void (*session_iter_fn)(session_t *s, void *ctx);
void         session_foreach(session_map_t *map, session_iter_fn fn, void *ctx);

#endif
