/*
 * hashmap.c — Dual-index session hash map implementation.
 *
 * Stores sessions in a flat, fixed-capacity array and provides O(1)
 * lookup by either client address (IP:port) or relay file descriptor
 * through two separate open-addressing hash tables with linear probing.
 *
 * Hash tables are sized to ~4x the session capacity (clamped to a
 * power of 2, minimum 16) to keep the load factor at ~25% and minimise
 * collision chain lengths.
 *
 * When a session is removed, both hash tables are rebuilt from scratch.
 * This is simple and correct — it avoids the "tombstone" problem with
 * linear probing, and is fast enough because max_clients is small
 * (capped at 1000).
 */

#include "hashmap.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * next_pow2 — Round an integer up to the next power of 2.
 *
 * Used to size the hash tables so that modular indexing can use a
 * bitmask instead of an expensive modulo operation.
 */
static int next_pow2(int v)
{
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    return v + 1;
}

/*
 * hash_addr — Hash a client socket address (IP + port).
 *
 * Combines the 32-bit IPv4 address and 16-bit port number, then applies
 * Murmur-style bit mixing (multiply by 0x5bd1e995, XOR-shift) to
 * distribute values evenly across the table.
 */
static uint32_t hash_addr(const struct sockaddr_in *addr)
{
    uint32_t h = addr->sin_addr.s_addr;
    h ^= (uint32_t)addr->sin_port << 16;
    h ^= h >> 13;
    h *= 0x5bd1e995;   /* Murmur2 mixing constant */
    h ^= h >> 15;
    return h;
}

/*
 * hash_fd — Hash a file descriptor integer.
 *
 * File descriptors are small sequential integers, so we apply the same
 * Murmur-style mixing to spread them across the hash table.
 */
static uint32_t hash_fd(int fd)
{
    uint32_t h = (uint32_t)fd;
    h ^= h >> 13;
    h *= 0x5bd1e995;   /* Murmur2 mixing constant */
    h ^= h >> 15;
    return h;
}

/*
 * addr_eq — Compare two socket addresses for equality (IP + port only).
 *
 * Ignores sin_family and padding — the caller guarantees AF_INET.
 */
static int addr_eq(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
    return a->sin_addr.s_addr == b->sin_addr.s_addr &&
           a->sin_port == b->sin_port;
}

/*
 * session_map_init — Allocate the session pool and both hash tables.
 *
 * Hash tables are filled with -1 (empty sentinel) via memset(0xFF).
 */
int session_map_init(session_map_t *map, int max_clients)
{
    memset(map, 0, sizeof(*map));
    map->capacity = max_clients;

    /* Pre-allocate the full session array (all slots start inactive) */
    map->sessions = calloc((size_t)max_clients, sizeof(session_t));
    if (!map->sessions)
        return -1;

    /* Size hash tables to ~4x capacity (power of 2) for ~25% load factor */
    map->table_size = next_pow2(max_clients * 4);
    if (map->table_size < 16)
        map->table_size = 16;

    map->addr_table = malloc((size_t)map->table_size * sizeof(int));
    map->fd_table   = malloc((size_t)map->table_size * sizeof(int));
    if (!map->addr_table || !map->fd_table) {
        free(map->sessions);
        free(map->addr_table);
        free(map->fd_table);
        return -1;
    }

    /* Fill both tables with -1 (0xFF bytes → all-bits-set → -1 for int) */
    memset(map->addr_table, 0xFF, (size_t)map->table_size * sizeof(int));
    memset(map->fd_table,   0xFF, (size_t)map->table_size * sizeof(int));

    return 0;
}

/*
 * session_map_destroy — Free all memory owned by the session map.
 *
 * Caller must close all relay file descriptors before calling this.
 */
void session_map_destroy(session_map_t *map)
{
    free(map->sessions);
    free(map->addr_table);
    free(map->fd_table);
    memset(map, 0, sizeof(*map));
}

/*
 * session_find_by_addr — Locate a session by client IP:port.
 *
 * Uses linear probing starting from the hashed index.  Returns NULL if
 * an empty slot (-1) is reached before finding a match.
 */
session_t *session_find_by_addr(session_map_t *map,
                                const struct sockaddr_in *addr)
{
    uint32_t mask = (uint32_t)(map->table_size - 1);
    uint32_t idx  = hash_addr(addr) & mask;

    for (int i = 0; i < map->table_size; i++) {
        int slot = map->addr_table[(idx + (uint32_t)i) & mask];
        if (slot == -1)
            return NULL;   /* Empty slot — key is definitely not in table */
        session_t *s = &map->sessions[slot];
        if (s->active && addr_eq(&s->client_addr, addr))
            return s;
    }
    return NULL;
}

/*
 * session_find_by_fd — Locate a session by relay file descriptor.
 *
 * Same linear probing strategy as session_find_by_addr, but against
 * the fd_table index.
 */
session_t *session_find_by_fd(session_map_t *map, int relay_fd)
{
    uint32_t mask = (uint32_t)(map->table_size - 1);
    uint32_t idx  = hash_fd(relay_fd) & mask;

    for (int i = 0; i < map->table_size; i++) {
        int slot = map->fd_table[(idx + (uint32_t)i) & mask];
        if (slot == -1)
            return NULL;   /* Empty slot — fd is not in the table */
        session_t *s = &map->sessions[slot];
        if (s->active && s->relay_fd == relay_fd)
            return s;
    }
    return NULL;
}

/*
 * table_insert — Insert a session index into a hash table (linear probing).
 *
 * Finds the first empty slot (-1) starting from the hashed position and
 * writes the session array index there.  Assumes the table is never full
 * (guaranteed by the 4x sizing).
 */
static void table_insert(int *table, int table_size, uint32_t hash, int slot)
{
    uint32_t mask = (uint32_t)(table_size - 1);
    uint32_t idx  = hash & mask;

    for (int i = 0; i < table_size; i++) {
        uint32_t pos = (idx + (uint32_t)i) & mask;
        if (table[pos] == -1) {
            table[pos] = slot;
            return;
        }
    }
    /* Should never be reached — table load factor is ~25% */
}

/*
 * session_insert — Add a new session to the map.
 *
 * 1. Finds the first free (inactive) slot in the session array.
 * 2. Initialises the session fields (address, fd, timestamps, stats).
 * 3. Inserts into both addr_table and fd_table for O(1) lookups.
 */
session_t *session_insert(session_map_t *map,
                          const struct sockaddr_in *client_addr,
                          int relay_fd)
{
    if (map->count >= map->capacity)
        return NULL;

    /* Linear scan for the first free slot in the session array */
    int slot = -1;
    for (int i = 0; i < map->capacity; i++) {
        if (!map->sessions[i].active) {
            slot = i;
            break;
        }
    }
    if (slot == -1)
        return NULL;   /* Should not happen if count < capacity */

    /* Initialise the session */
    session_t *s = &map->sessions[slot];
    memset(s, 0, sizeof(*s));
    s->client_addr   = *client_addr;
    s->relay_fd      = relay_fd;
    s->last_activity = time(NULL);
    s->active        = 1;

    /* Insert into both hash indices */
    table_insert(map->addr_table, map->table_size,
                 hash_addr(client_addr), slot);
    table_insert(map->fd_table, map->table_size,
                 hash_fd(relay_fd), slot);

    map->count++;
    return s;
}

/*
 * rebuild_tables — Reconstruct both hash tables from the active sessions.
 *
 * Called after every removal to eliminate "holes" in linear probing
 * chains.  Without this, a deleted slot could cause lookups for other
 * sessions in the same chain to incorrectly return NULL.
 *
 * This is a simple O(n) operation where n = capacity (max 1000), so
 * the cost is negligible compared to network I/O.
 */
static void rebuild_tables(session_map_t *map)
{
    /* Clear both tables to empty (-1) */
    memset(map->addr_table, 0xFF, (size_t)map->table_size * sizeof(int));
    memset(map->fd_table,   0xFF, (size_t)map->table_size * sizeof(int));

    /* Re-insert every active session into both tables */
    for (int i = 0; i < map->capacity; i++) {
        if (map->sessions[i].active) {
            table_insert(map->addr_table, map->table_size,
                         hash_addr(&map->sessions[i].client_addr), i);
            table_insert(map->fd_table, map->table_size,
                         hash_fd(map->sessions[i].relay_fd), i);
        }
    }
}

/*
 * session_remove — Deactivate a session and update the hash tables.
 *
 * Marks the session slot as free, invalidates its fd, decrements the
 * count, and rebuilds both hash tables to maintain probing correctness.
 * Does NOT close the relay fd — the caller is responsible for that.
 */
void session_remove(session_map_t *map, session_t *s)
{
    if (!s || !s->active)
        return;

    s->active = 0;
    s->relay_fd = -1;
    map->count--;

    /* Rebuild both tables to fix probing chains */
    rebuild_tables(map);
}

/*
 * session_foreach — Call a function for every active session.
 *
 * Iterates the flat session array and invokes fn(session, ctx) for
 * each active entry.  The callback must NOT insert or remove sessions.
 */
void session_foreach(session_map_t *map, session_iter_fn fn, void *ctx)
{
    for (int i = 0; i < map->capacity; i++) {
        if (map->sessions[i].active)
            fn(&map->sessions[i], ctx);
    }
}
