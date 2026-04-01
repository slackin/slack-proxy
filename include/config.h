/*
 * config.h — INI-style configuration file parser for urt-proxy.
 *
 * Supports a [global] section for process-wide settings and multiple
 * [server:<name>] sections, each producing a relay_config_t that can
 * be handed to relay_run().
 *
 * Per-server keys:
 *   Required:
 *     remote-host       Real game server address (IP or hostname[:port])
 *
 *   Optional (defaults shown):
 *     listen-port       = 27990
 *     remote-port       = 27960
 *     max-clients       = 20
 *     timeout           = 30       (seconds, minimum 5)
 *     hostname-tag      = (none)   (prefix for sv_hostname)
 *     rate-limit        = 5        (new sessions per second)
 *     max-query-sessions = 100
 *     query-timeout     = 5        (seconds)
 *     master-server     = (none)   (repeatable, up to 4)
 *     master-broadcast  = true     (enable/disable heartbeat to masters)
 *
 * Global keys:
 *   debug = false
 *
 * Config file format example:
 *
 *   [global]
 *   debug = false
 *
 *   [server:dallas]
 *   listen-port   = 27990
 *   remote-host   = 10.0.0.2
 *   remote-port   = 27960
 *   max-clients   = 20
 *   timeout       = 30
 *   hostname-tag  = [DALLAS]
 *   rate-limit    = 5
 *   max-query-sessions = 100
 *   query-timeout = 5
 *   master-server = master.urbanterror.info
 */

#ifndef URT_CONFIG_H
#define URT_CONFIG_H

#include "relay.h"
#include "mgmt.h"

/* Maximum number of server sections allowed in a config file. */
#define CONFIG_MAX_SERVERS 32

/*
 * proxy_config_t — Top-level configuration loaded from a config file.
 *
 * Contains global settings plus an array of per-server relay configs.
 */
typedef struct {
    int             debug;                            /* Global debug flag (from [global]) */
    relay_config_t  servers[CONFIG_MAX_SERVERS];       /* Per-server configurations         */
    int             server_count;                      /* Number of servers loaded           */
    mgmt_config_t   mgmt;                              /* Management API configuration       */
} proxy_config_t;

/*
 * config_load — Parse an INI config file and populate a proxy_config_t.
 *
 * Reads the file line-by-line, resolves hostnames, and validates all
 * parameter ranges. On error, logs a message and returns -1.
 *
 * @param path  Path to the INI config file.
 * @param out   Pointer to an uninitialised proxy_config_t to populate.
 * @return      0 on success, -1 on error (details logged to stderr).
 */
int config_load(const char *path, proxy_config_t *out);

/*
 * resolve_host_port — Resolve a HOST[:PORT] string to a sockaddr_in.
 *
 * Uses getaddrinfo() so both raw IPs and DNS hostnames are supported.
 * Exposed for use by the management API (set_master command).
 *
 * @param str           The "host" or "host:port" string.
 * @param default_port  Port to use when no explicit port is given.
 * @param out           Receives the resolved address.
 * @param line          Config file line number (for error messages, 0 if N/A).
 * @return              0 on success, -1 on error (message printed).
 */
int resolve_host_port(const char *str, uint16_t default_port,
                      struct sockaddr_in *out, int line);

#endif
