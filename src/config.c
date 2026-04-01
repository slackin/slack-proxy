/*
 * config.c — INI-style configuration file parser.
 *
 * Reads an INI file with [global] and [server:<name>] sections,
 * populates a proxy_config_t with validated relay_config_t entries.
 *
 * Each [server:<name>] section defines one proxied game server with
 * its own listen port, remote address, session limits, and optional
 * master server registration.  The only required key per server is
 * "remote-host"; all other keys have sensible defaults matching the
 * CLI defaults in main.c.
 *
 * Supported global keys:
 *   debug = true|false|yes|no|1|0
 *
 * Supported per-server keys:
 *   listen-port, remote-host (required), remote-port, max-clients,
 *   timeout, hostname-tag, rate-limit, max-query-sessions,
 *   query-timeout, master-server (repeatable, up to 4).
 */

#define _POSIX_C_SOURCE 200809L

#include "config.h"
#include "mgmt.h"
#include "q3proto.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <arpa/inet.h>
#include <netdb.h>

/* Default values — must match the CLI defaults in main.c */
#define DEFAULT_LISTEN_PORT   27960
#define DEFAULT_REMOTE_PORT   27960
#define DEFAULT_MAX_CLIENTS   20
#define DEFAULT_TIMEOUT       30
#define DEFAULT_RATE_LIMIT    5
#define DEFAULT_MAX_QUERY     100
#define DEFAULT_QUERY_TIMEOUT 5

/* Maximum line length in a config file. */
#define MAX_LINE 1024

/* ------------------------------------------------------------------ */
/*  String helpers                                                    */
/* ------------------------------------------------------------------ */

/* Trim leading and trailing whitespace in-place, return pointer. */
static char *trim(char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    if (*s == '\0')
        return s;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end))
        *end-- = '\0';
    return s;
}

/* ------------------------------------------------------------------ */
/*  Host:port resolution (shared by remote-host and master-server)    */
/* ------------------------------------------------------------------ */

/*
 * resolve_host_port — Resolve a HOST[:PORT] string to a sockaddr_in.
 *
 * Uses getaddrinfo() so both raw IPs and DNS hostnames are supported.
 * If the string contains a colon, the portion after the last colon is
 * parsed as a port number, overriding @a default_port.  Otherwise the
 * entire string is treated as the host and @a default_port is used.
 *
 * The resolved address is written to @a out with sin_port set to the
 * final port value (network byte order).
 *
 * @param str           The "host" or "host:port" string.
 * @param default_port  Port to use when no explicit port is given.
 * @param out           Receives the resolved address.
 * @param line          Config file line number (for error messages).
 * @return              0 on success, -1 on error (message printed).
 */
static int resolve_host_port(const char *str, uint16_t default_port,
                             struct sockaddr_in *out, int line)
{
    char host_buf[256];
    uint16_t port = default_port;

    const char *colon = strrchr(str, ':');
    if (colon) {
        size_t hlen = (size_t)(colon - str);
        if (hlen == 0 || hlen >= sizeof(host_buf)) {
            fprintf(stderr, "Line %d: invalid host:port '%s'\n", line, str);
            return -1;
        }
        memcpy(host_buf, str, hlen);
        host_buf[hlen] = '\0';

        char *endp;
        long p = strtol(colon + 1, &endp, 10);
        if (*endp != '\0' || p < 1 || p > 65535) {
            fprintf(stderr, "Line %d: invalid port in '%s'\n", line, str);
            return -1;
        }
        port = (uint16_t)p;
    } else {
        if (strlen(str) >= sizeof(host_buf)) {
            fprintf(stderr, "Line %d: hostname too long\n", line);
            return -1;
        }
        strncpy(host_buf, str, sizeof(host_buf) - 1);
        host_buf[sizeof(host_buf) - 1] = '\0';
    }

    struct addrinfo hints = {0}, *res;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    int err = getaddrinfo(host_buf, NULL, &hints, &res);
    if (err != 0 || !res) {
        fprintf(stderr, "Line %d: cannot resolve '%s': %s\n",
                line, host_buf, gai_strerror(err));
        return -1;
    }

    *out = *(struct sockaddr_in *)res->ai_addr;
    out->sin_port = htons(port);
    freeaddrinfo(res);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Per-server defaults initialiser                                   */
/* ------------------------------------------------------------------ */

static void init_server_defaults(relay_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->listen_port       = DEFAULT_LISTEN_PORT;
    cfg->max_clients       = DEFAULT_MAX_CLIENTS;
    cfg->session_timeout   = DEFAULT_TIMEOUT;
    cfg->max_new_per_sec   = DEFAULT_RATE_LIMIT;
    cfg->max_query_sessions = DEFAULT_MAX_QUERY;
    cfg->query_timeout     = DEFAULT_QUERY_TIMEOUT;
}

/* ------------------------------------------------------------------ */
/*  Key-value handler for a [server:*] section                        */
/* ------------------------------------------------------------------ */

/*
 * apply_server_key — Set one field of a relay_config_t from a key=value pair.
 *
 * @return 0 on success, -1 on error (logged with line number).
 */
static int apply_server_key(relay_config_t *cfg, const char *key,
                            const char *value, int line)
{
    if (strcmp(key, "listen-port") == 0) {
        char *endp;
        long v = strtol(value, &endp, 10);
        if (*endp != '\0' || v < 1 || v > 65535) {
            fprintf(stderr, "Line %d: listen-port must be 1-65535\n", line);
            return -1;
        }
        cfg->listen_port = (uint16_t)v;
    }
    else if (strcmp(key, "remote-host") == 0) {
        /* Resolve immediately — also handles host:port syntax */
        uint16_t cur_port = ntohs(cfg->remote_addr.sin_port);
        if (cur_port == 0)
            cur_port = DEFAULT_REMOTE_PORT;
        if (resolve_host_port(value, cur_port, &cfg->remote_addr, line) < 0)
            return -1;
    }
    else if (strcmp(key, "remote-port") == 0) {
        char *endp;
        long v = strtol(value, &endp, 10);
        if (*endp != '\0' || v < 1 || v > 65535) {
            fprintf(stderr, "Line %d: remote-port must be 1-65535\n", line);
            return -1;
        }
        cfg->remote_addr.sin_port = htons((uint16_t)v);
    }
    else if (strcmp(key, "max-clients") == 0) {
        char *endp;
        long v = strtol(value, &endp, 10);
        if (*endp != '\0' || v < 1 || v > 1000) {
            fprintf(stderr, "Line %d: max-clients must be 1-1000\n", line);
            return -1;
        }
        cfg->max_clients = (int)v;
    }
    else if (strcmp(key, "timeout") == 0) {
        char *endp;
        long v = strtol(value, &endp, 10);
        if (*endp != '\0' || v < 5) {
            fprintf(stderr, "Line %d: timeout must be >= 5\n", line);
            return -1;
        }
        cfg->session_timeout = (int)v;
    }
    else if (strcmp(key, "hostname-tag") == 0) {
        /*
         * Duplicate the string so it outlives the config file parsing.
         *
         * This is a small deliberate "leak" that lives for the entire
         * lifetime of the process.  Each server’s hostname_tag is
         * allocated once during config load and never freed — the OS
         * reclaims the memory on exit.  A cleanup function is not
         * worth the complexity for a handful of short strings.
         */
        cfg->hostname_tag = strdup(value);
        if (!cfg->hostname_tag) {
            fprintf(stderr, "Line %d: out of memory\n", line);
            return -1;
        }
    }
    else if (strcmp(key, "rate-limit") == 0) {
        char *endp;
        long v = strtol(value, &endp, 10);
        if (*endp != '\0' || v < 1) {
            fprintf(stderr, "Line %d: rate-limit must be >= 1\n", line);
            return -1;
        }
        cfg->max_new_per_sec = (int)v;
    }
    else if (strcmp(key, "max-query-sessions") == 0) {
        char *endp;
        long v = strtol(value, &endp, 10);
        if (*endp != '\0' || v < 1 || v > 1000) {
            fprintf(stderr, "Line %d: max-query-sessions must be 1-1000\n", line);
            return -1;
        }
        cfg->max_query_sessions = (int)v;
    }
    else if (strcmp(key, "query-timeout") == 0) {
        char *endp;
        long v = strtol(value, &endp, 10);
        if (*endp != '\0' || v < 1) {
            fprintf(stderr, "Line %d: query-timeout must be >= 1\n", line);
            return -1;
        }
        cfg->query_timeout = (int)v;
    }
    else if (strcmp(key, "master-server") == 0) {
        if (cfg->master_count >= RELAY_MAX_MASTERS) {
            fprintf(stderr, "Line %d: max %d master servers per server section\n",
                    line, RELAY_MAX_MASTERS);
            return -1;
        }
        if (resolve_host_port(value, Q3_DEFAULT_MASTER_PORT,
                              &cfg->master_addrs[cfg->master_count], line) < 0)
            return -1;
        cfg->master_count++;
    }
    else {
        fprintf(stderr, "Line %d: unknown key '%s'\n", line, key);
        return -1;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Validation                                                        */
/* ------------------------------------------------------------------ */

/*
 * validate_server — Ensure a server config has all required fields set.
 *
 * remote-host is the only required key.  If remote-port was not set
 * explicitly, the port may still be zero (e.g. if remote-host was a
 * bare IP with no :port suffix and no separate remote-port key).
 * In that case we apply the default port here as a safety net.
 */
static int validate_server(relay_config_t *cfg, int index)
{
    /* remote-host is required — its absence means the address is all zeros */
    if (cfg->remote_addr.sin_addr.s_addr == 0) {
        fprintf(stderr, "Server #%d: 'remote-host' is required — "
                "set it to the real game server's address\n", index + 1);
        return -1;
    }
    /*
     * Ensure remote_addr has a port.  This can happen if the user wrote
     * "remote-host = 10.0.0.2" without a :port and omitted remote-port.
     * resolve_host_port() would have applied DEFAULT_REMOTE_PORT, but
     * this guard catches edge cases where the address was partially set.
     */
    if (cfg->remote_addr.sin_port == 0) {
        cfg->remote_addr.sin_port = htons(DEFAULT_REMOTE_PORT);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

int config_load(const char *path, proxy_config_t *out)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "Error: cannot open config file '%s': %s\n",
                path, strerror(errno));
        return -1;
    }

    memset(out, 0, sizeof(*out));

    /*
     * current_server points to the server section we're currently
     * filling in, or NULL if we're in [global] or before any section.
     */
    relay_config_t *current_server = NULL;
    int in_global = 0;

    char line_buf[MAX_LINE];
    int line_no = 0;
    int rc = 0;

    while (fgets(line_buf, sizeof(line_buf), fp)) {
        line_no++;
        char *line = trim(line_buf);

        /* Skip empty lines and comments */
        if (*line == '\0' || *line == '#' || *line == ';')
            continue;

        /* Section header: [global] or [server:name] */
        if (*line == '[') {
            char *end = strchr(line, ']');
            if (!end) {
                fprintf(stderr, "Line %d: malformed section header\n", line_no);
                rc = -1;
                break;
            }
            *end = '\0';
            const char *section = line + 1;

            if (strcmp(section, "global") == 0) {
                in_global = 1;
                current_server = NULL;
            }
            else if (strncmp(section, "server:", 7) == 0) {
                const char *name = section + 7;
                if (*name == '\0') {
                    fprintf(stderr, "Line %d: server section requires a name "
                            "(e.g. [server:myserver])\n", line_no);
                    rc = -1;
                    break;
                }
                if (out->server_count >= CONFIG_MAX_SERVERS) {
                    fprintf(stderr, "Line %d: too many servers (max %d)\n",
                            line_no, CONFIG_MAX_SERVERS);
                    rc = -1;
                    break;
                }
                in_global = 0;
                current_server = &out->servers[out->server_count];
                init_server_defaults(current_server);
                out->server_count++;
            }
            else {
                fprintf(stderr, "Line %d: unknown section [%s]\n",
                        line_no, section);
                rc = -1;
                break;
            }
            continue;
        }

        /* Key = value pair */
        char *eq = strchr(line, '=');
        if (!eq) {
            fprintf(stderr, "Line %d: expected key = value\n", line_no);
            rc = -1;
            break;
        }
        *eq = '\0';
        char *key   = trim(line);
        char *value = trim(eq + 1);

        if (in_global) {
            if (strcmp(key, "debug") == 0) {
                out->debug = (strcmp(value, "true") == 0 ||
                              strcmp(value, "1") == 0 ||
                              strcmp(value, "yes") == 0);
            } else if (strcmp(key, "mgmt-port") == 0) {
                out->mgmt.port = (uint16_t)atoi(value);
                out->mgmt.listen_addr.sin_family = AF_INET;
                out->mgmt.listen_addr.sin_port = htons(out->mgmt.port);
            } else if (strcmp(key, "mgmt-addr") == 0) {
                out->mgmt.listen_addr.sin_family = AF_INET;
                if (inet_pton(AF_INET, value,
                              &out->mgmt.listen_addr.sin_addr) != 1) {
                    fprintf(stderr, "Line %d: invalid mgmt-addr '%s'\n",
                            line_no, value);
                    rc = -1;
                    break;
                }
            } else if (strcmp(key, "mgmt-key") == 0) {
                out->mgmt.api_key = strdup(value);
                out->mgmt.enabled = 1;
                /* Apply defaults if not yet set */
                if (out->mgmt.listen_addr.sin_family == 0) {
                    out->mgmt.listen_addr.sin_family = AF_INET;
                    out->mgmt.listen_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                }
                if (out->mgmt.port == 0) {
                    out->mgmt.port = MGMT_DEFAULT_PORT;
                    out->mgmt.listen_addr.sin_port = htons(MGMT_DEFAULT_PORT);
                }
            } else {
                fprintf(stderr, "Line %d: unknown global key '%s'\n",
                        line_no, key);
                rc = -1;
                break;
            }
        }
        else if (current_server) {
            if (apply_server_key(current_server, key, value, line_no) < 0) {
                rc = -1;
                break;
            }
        }
        else {
            fprintf(stderr, "Line %d: key '%s' outside of a section\n",
                    line_no, key);
            rc = -1;
            break;
        }
    }

    fclose(fp);

    if (rc < 0)
        return -1;

    /* Allow zero servers if management API is configured (management-only mode) */
    if (out->server_count == 0 && !out->mgmt.enabled) {
        fprintf(stderr, "Error: config file has no [server:] sections — "
                "add at least one [server:<name>] block or enable the "
                "management API with mgmt-key\n");
        return -1;
    }

    /* Validate each server and check for duplicate listen ports */
    for (int i = 0; i < out->server_count; i++) {
        if (validate_server(&out->servers[i], i) < 0)
            return -1;

        for (int j = 0; j < i; j++) {
            if (out->servers[i].listen_port == out->servers[j].listen_port) {
                fprintf(stderr, "Error: servers #%d and #%d both use "
                        "listen-port %u — each server must listen on a "
                        "unique port\n", j + 1, i + 1,
                        out->servers[i].listen_port);
                return -1;
            }
        }
    }

    return 0;
}
