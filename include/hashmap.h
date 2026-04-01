/*
 * hashmap.h — Session storage with dual-index O(1) lookup.
 *
 * Manages a fixed-capacity pool of client sessions, each tracked by two
 * independent open-addressing hash tables:
 *
 *   1. addr_table:  client IP:port  → session index   (for incoming packets)
 *   2. fd_table:    relay fd        → session index   (for server responses)
 *
 * Both tables use linear probing and are sized to ~4x capacity to keep the
 * load factor around 25%, minimising collision chains. Sessions themselves
 * live in a flat array; the hash tables store indices into that array.
 *
 * After a session is removed, both tables are rebuilt from scratch to
 * eliminate holes in probing chains (safe because max_clients is small).
 */

#ifndef URT_HASHMAP_H
#define URT_HASHMAP_H

#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include <netinet/in.h>

/*
 * session_t — A single client session.
 *
 * Each player who sends at least one packet through the proxy gets a
 * session entry. The session tracks the player's address, the dedicated
 * relay socket connected to the real server, and per-session traffic
 * statistics used for logging on expiry.
 */
typedef struct {
    struct sockaddr_in client_addr;   /* Player's public IP and port              */
    int                relay_fd;      /* Dedicated UDP socket to the real server  */
    time_t             last_activity; /* Timestamp of last packet (for timeouts)  */
    uint64_t           pkts_to_server;  /* Packets forwarded client → server      */
    uint64_t           pkts_to_client;  /* Packets forwarded server → client      */
    uint64_t           bytes_to_server; /* Bytes forwarded client → server        */
    uint64_t           bytes_to_client; /* Bytes forwarded server → client        */
    int                active;        /* 1 = slot in use, 0 = free               */
    int                is_query;      /* 1 = browser query, 0 = game session     */
} session_t;

/*
 * session_map_t — Hash map container for all sessions.
 *
 * Owns the session array and both hash tables. Initialised once at startup
 * with session_map_init() and torn down with session_map_destroy().
 */
typedef struct {
    session_t *sessions;       /* Flat array of sessions [0 .. capacity-1]       */
    int        capacity;       /* Max sessions allowed (from --max-clients)       */
    int        count;          /* Current number of active sessions               */

    /* Hash tables — each entry is an index into sessions[], or -1 (empty) */
    int       *addr_table;     /* Indexed by hash of client addr (IP + port)     */
    int       *fd_table;       /* Indexed by hash of relay file descriptor       */
    int        table_size;     /* Size of both hash tables (always a power of 2) */
} session_map_t;

/*
 * session_map_init — Allocate the session array and hash tables.
 *
 * @param map          Pointer to an uninitialised session_map_t.
 * @param max_clients  Maximum number of concurrent sessions.
 * @return             0 on success, -1 on allocation failure.
 */
int session_map_init(session_map_t *map, int max_clients);

/*
 * session_map_destroy — Free all memory owned by the map.
 *
 * Does NOT close relay file descriptors — the caller must do that first.
 *
 * @param map  Pointer to an initialised session_map_t.
 */
void session_map_destroy(session_map_t *map);

/*
 * session_find_by_addr — Look up a session by the client's IP:port.
 *
 * Used when a packet arrives on the listen socket to find (or confirm
 * the absence of) an existing session for that client.
 *
 * @param map   The session map.
 * @param addr  Client address to search for.
 * @return      Pointer to the matching session, or NULL if not found.
 */
session_t *session_find_by_addr(session_map_t *map,
                                const struct sockaddr_in *addr);

/*
 * session_find_by_fd — Look up a session by its relay file descriptor.
 *
 * Used when a packet arrives on a relay socket to find which client
 * the response should be forwarded to.
 *
 * @param map       The session map.
 * @param relay_fd  Relay socket file descriptor to search for.
 * @return          Pointer to the matching session, or NULL if not found.
 */
session_t *session_find_by_fd(session_map_t *map, int relay_fd);

/*
 * session_insert — Create a new session and index it in both tables.
 *
 * Finds the first free slot in the session array, initialises it, and
 * inserts entries into both addr_table and fd_table.
 *
 * @param map          The session map.
 * @param client_addr  The new client's IP:port.
 * @param relay_fd     The dedicated relay socket for this client.
 * @return             Pointer to the new session, or NULL if at capacity.
 */
session_t *session_insert(session_map_t *map,
                          const struct sockaddr_in *client_addr,
                          int relay_fd);

/*
 * session_remove — Deactivate a session and remove it from both tables.
 *
 * Marks the slot as free and rebuilds both hash tables to eliminate
 * holes left in linear probing chains.  Does NOT close the relay fd.
 *
 * @param map  The session map.
 * @param s    Pointer to the session to remove (must be active).
 */
void session_remove(session_map_t *map, session_t *s);

/*
 * session_iter_fn — Callback type for session_foreach().
 *
 * @param s    Pointer to an active session.
 * @param ctx  Opaque context passed through from session_foreach().
 */
typedef void (*session_iter_fn)(session_t *s, void *ctx);

/*
 * session_foreach — Iterate over all active sessions.
 *
 * Calls fn(session, ctx) for every active session. Used by the timeout
 * sweep to find expired sessions.  Do not insert or remove sessions
 * from within the callback — doing so invalidates the iteration and
 * corrupts the hash tables.  The timeout sweep works around this by
 * collecting pointers in the callback, then removing in a second pass.
 *
 * Thread safety: this function is NOT thread-safe.  The codebase is
 * single-threaded, so no synchronisation is needed.
 *
 * @param map  The session map.
 * @param fn   Callback invoked for each active session.
 * @param ctx  Opaque pointer forwarded to the callback.
 */
void session_foreach(session_map_t *map, session_iter_fn fn, void *ctx);

#endif
