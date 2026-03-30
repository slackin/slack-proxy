#include "hashmap.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Round up to next power of 2 */
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

/* Hash a client address (ip + port) */
static uint32_t hash_addr(const struct sockaddr_in *addr)
{
    uint32_t h = addr->sin_addr.s_addr;
    h ^= (uint32_t)addr->sin_port << 16;
    h ^= h >> 13;
    h *= 0x5bd1e995;
    h ^= h >> 15;
    return h;
}

/* Hash a file descriptor */
static uint32_t hash_fd(int fd)
{
    uint32_t h = (uint32_t)fd;
    h ^= h >> 13;
    h *= 0x5bd1e995;
    h ^= h >> 15;
    return h;
}

static int addr_eq(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
    return a->sin_addr.s_addr == b->sin_addr.s_addr &&
           a->sin_port == b->sin_port;
}

int session_map_init(session_map_t *map, int max_clients)
{
    memset(map, 0, sizeof(*map));
    map->capacity = max_clients;

    map->sessions = calloc((size_t)max_clients, sizeof(session_t));
    if (!map->sessions)
        return -1;

    /* Hash tables sized ~4x capacity for low collision rate */
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

    memset(map->addr_table, 0xFF, (size_t)map->table_size * sizeof(int)); /* -1 */
    memset(map->fd_table,   0xFF, (size_t)map->table_size * sizeof(int));

    return 0;
}

void session_map_destroy(session_map_t *map)
{
    free(map->sessions);
    free(map->addr_table);
    free(map->fd_table);
    memset(map, 0, sizeof(*map));
}

session_t *session_find_by_addr(session_map_t *map,
                                const struct sockaddr_in *addr)
{
    uint32_t mask = (uint32_t)(map->table_size - 1);
    uint32_t idx  = hash_addr(addr) & mask;

    for (int i = 0; i < map->table_size; i++) {
        int slot = map->addr_table[(idx + (uint32_t)i) & mask];
        if (slot == -1)
            return NULL;
        session_t *s = &map->sessions[slot];
        if (s->active && addr_eq(&s->client_addr, addr))
            return s;
    }
    return NULL;
}

session_t *session_find_by_fd(session_map_t *map, int relay_fd)
{
    uint32_t mask = (uint32_t)(map->table_size - 1);
    uint32_t idx  = hash_fd(relay_fd) & mask;

    for (int i = 0; i < map->table_size; i++) {
        int slot = map->fd_table[(idx + (uint32_t)i) & mask];
        if (slot == -1)
            return NULL;
        session_t *s = &map->sessions[slot];
        if (s->active && s->relay_fd == relay_fd)
            return s;
    }
    return NULL;
}

/* Insert into a hash table using linear probing */
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
}

session_t *session_insert(session_map_t *map,
                          const struct sockaddr_in *client_addr,
                          int relay_fd)
{
    if (map->count >= map->capacity)
        return NULL;

    /* Find a free session slot */
    int slot = -1;
    for (int i = 0; i < map->capacity; i++) {
        if (!map->sessions[i].active) {
            slot = i;
            break;
        }
    }
    if (slot == -1)
        return NULL;

    session_t *s = &map->sessions[slot];
    memset(s, 0, sizeof(*s));
    s->client_addr   = *client_addr;
    s->relay_fd      = relay_fd;
    s->last_activity = time(NULL);
    s->active        = 1;

    /* Insert into both hash tables */
    table_insert(map->addr_table, map->table_size,
                 hash_addr(client_addr), slot);
    table_insert(map->fd_table, map->table_size,
                 hash_fd(relay_fd), slot);

    map->count++;
    return s;
}

/* Rebuild both hash tables from scratch (needed after removal to fix probing chains) */
static void rebuild_tables(session_map_t *map)
{
    memset(map->addr_table, 0xFF, (size_t)map->table_size * sizeof(int));
    memset(map->fd_table,   0xFF, (size_t)map->table_size * sizeof(int));

    for (int i = 0; i < map->capacity; i++) {
        if (map->sessions[i].active) {
            table_insert(map->addr_table, map->table_size,
                         hash_addr(&map->sessions[i].client_addr), i);
            table_insert(map->fd_table, map->table_size,
                         hash_fd(map->sessions[i].relay_fd), i);
        }
    }
}

void session_remove(session_map_t *map, session_t *s)
{
    if (!s || !s->active)
        return;

    s->active = 0;
    s->relay_fd = -1;
    map->count--;

    /* Rebuild to fix linear probing holes */
    rebuild_tables(map);
}

void session_foreach(session_map_t *map, session_iter_fn fn, void *ctx)
{
    for (int i = 0; i < map->capacity; i++) {
        if (map->sessions[i].active)
            fn(&map->sessions[i], ctx);
    }
}
