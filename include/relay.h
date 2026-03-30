#ifndef URT_RELAY_H
#define URT_RELAY_H

#include <stdint.h>
#include <netinet/in.h>

typedef struct {
    /* Configuration */
    uint16_t            listen_port;
    struct sockaddr_in  remote_addr;     /* real server WireGuard IP:port */
    int                 max_clients;
    int                 session_timeout; /* seconds */
    const char         *hostname_tag;    /* e.g. "[PROXY]", or NULL */

    /* Rate limiting */
    int                 max_new_per_sec;
} relay_config_t;

/*
 * Run the relay event loop (blocking). Returns only on signal / fatal error.
 * Returns 0 on clean shutdown, -1 on error.
 */
int relay_run(const relay_config_t *cfg);

#endif
