/*
 * main.c — Entry point and command-line argument parsing for urt-proxy.
 *
 * Parses command-line options with getopt_long, validates all parameters,
 * populates a relay_config_t, initialises the logger, and hands control
 * to relay_run() which runs the event loop until shutdown.
 */

#define _POSIX_C_SOURCE 200809L

#include "relay.h"
#include "q3proto.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <getopt.h>
#include <arpa/inet.h>
#include <netdb.h>

/* Default values for optional CLI arguments */
#define DEFAULT_LISTEN_PORT   27960
#define DEFAULT_REMOTE_PORT   27960
#define DEFAULT_MAX_CLIENTS   20
#define DEFAULT_TIMEOUT       30   /* seconds */
#define DEFAULT_RATE_LIMIT    5    /* new sessions per second */
#define DEFAULT_MAX_QUERY     100  /* max concurrent browser query sessions */
#define DEFAULT_QUERY_TIMEOUT 5    /* seconds */

/*
 * usage — Print a help message listing all command-line options.
 *
 * @param prog  Program name (argv[0]), embedded in the output.
 */
static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options]\n"
        "\n"
        "Urban Terror UDP Proxy — transparent Q3 relay over WireGuard\n"
        "\n"
        "Required:\n"
        "  -r, --remote-host HOST    Real server IP (WireGuard address)\n"
        "\n"
        "Optional:\n"
        "  -l, --listen-port PORT    Local listen port (default: %d)\n"
        "  -p, --remote-port PORT    Real server port (default: %d)\n"
        "  -m, --max-clients N       Max concurrent clients (default: %d)\n"
        "  -t, --timeout SECS        Session timeout in seconds (default: %d)\n"
        "  -T, --hostname-tag TAG    Prefix for sv_hostname in browser (e.g. \"[PROXY]\")\n"
        "  -R, --rate-limit N        Max new sessions per second (default: %d)\n"
        "  -Q, --max-query-sessions N\n"
        "                            Max concurrent browser query sessions (default: %d)\n"
        "  -q, --query-timeout SECS  Query session timeout in seconds (default: %d)\n"
        "  -M, --master-server HOST[:PORT]\n"
        "                            Master server for server list registration\n"
        "                            (port defaults to 27900, may be repeated up to %d)\n"
        "  -d, --debug               Enable debug logging\n"
        "  -h, --help                Show this help\n"
        "\n"
        "Example:\n"
        "  %s -r 10.0.0.2 -l 27960 -p 27960 -T \"[PROXY]\" -M master.urbanterror.info\n",
        prog,
        DEFAULT_LISTEN_PORT, DEFAULT_REMOTE_PORT,
        DEFAULT_MAX_CLIENTS, DEFAULT_TIMEOUT, DEFAULT_RATE_LIMIT,
        DEFAULT_MAX_QUERY, DEFAULT_QUERY_TIMEOUT,
        RELAY_MAX_MASTERS,
        prog);
}

int main(int argc, char **argv)
{
    /* --- Initialise config with sensible defaults --- */
    relay_config_t cfg = {0};
    cfg.listen_port    = DEFAULT_LISTEN_PORT;
    cfg.max_clients    = DEFAULT_MAX_CLIENTS;
    cfg.session_timeout = DEFAULT_TIMEOUT;
    cfg.max_new_per_sec = DEFAULT_RATE_LIMIT;
    cfg.max_query_sessions = DEFAULT_MAX_QUERY;
    cfg.query_timeout = DEFAULT_QUERY_TIMEOUT;

    uint16_t remote_port = DEFAULT_REMOTE_PORT;
    const char *remote_host = NULL;
    int debug = 0;

    /* Long option definitions for getopt_long */
    static struct option long_opts[] = {
        {"remote-host",  required_argument, NULL, 'r'},
        {"listen-port",  required_argument, NULL, 'l'},
        {"remote-port",  required_argument, NULL, 'p'},
        {"max-clients",  required_argument, NULL, 'm'},
        {"timeout",      required_argument, NULL, 't'},
        {"hostname-tag", required_argument, NULL, 'T'},
        {"rate-limit",   required_argument, NULL, 'R'},
        {"max-query-sessions", required_argument, NULL, 'Q'},
        {"query-timeout",required_argument, NULL, 'q'},
        {"master-server",required_argument, NULL, 'M'},
        {"debug",        no_argument,       NULL, 'd'},
        {"help",         no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    /* --- Parse command-line arguments --- */
    int opt;
    while ((opt = getopt_long(argc, argv, "r:l:p:m:t:T:R:Q:q:M:dh", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'r':   /* --remote-host (required) */
            remote_host = optarg;
            break;
        case 'l': { /* --listen-port: local UDP port to bind */
            long v = strtol(optarg, NULL, 10);
            if (v < 1 || v > 65535) {
                fprintf(stderr, "Error: --listen-port must be 1-65535\n");
                return 1;
            }
            cfg.listen_port = (uint16_t)v;
            break;
        }
        case 'p': { /* --remote-port: real server's game port */
            long v = strtol(optarg, NULL, 10);
            if (v < 1 || v > 65535) {
                fprintf(stderr, "Error: --remote-port must be 1-65535\n");
                return 1;
            }
            remote_port = (uint16_t)v;
            break;
        }
        case 'm':   /* --max-clients: concurrent session cap */
            cfg.max_clients = (int)strtol(optarg, NULL, 10);
            break;
        case 't':   /* --timeout: session inactivity timeout */
            cfg.session_timeout = (int)strtol(optarg, NULL, 10);
            break;
        case 'T':   /* --hostname-tag: prefix for sv_hostname */
            cfg.hostname_tag = optarg;
            break;
        case 'R':   /* --rate-limit: max new sessions per second */
            cfg.max_new_per_sec = (int)strtol(optarg, NULL, 10);
            break;
        case 'Q':   /* --max-query-sessions: browser query session cap */
            cfg.max_query_sessions = (int)strtol(optarg, NULL, 10);
            break;
        case 'q':   /* --query-timeout: browser query inactivity timeout */
            cfg.query_timeout = (int)strtol(optarg, NULL, 10);
            break;
        case 'M': { /* --master-server HOST[:PORT] — register with a master server */
            if (cfg.master_count >= RELAY_MAX_MASTERS) {
                fprintf(stderr, "Error: max %d master servers\n", RELAY_MAX_MASTERS);
                return 1;
            }

            /* Split HOST and optional :PORT (default 27900) */
            char host_buf[256];
            uint16_t mport = Q3_DEFAULT_MASTER_PORT;

            const char *colon = strrchr(optarg, ':');
            if (colon) {
                size_t hlen = (size_t)(colon - optarg);
                if (hlen >= sizeof(host_buf)) {
                    fprintf(stderr, "Error: master server hostname too long\n");
                    return 1;
                }
                memcpy(host_buf, optarg, hlen);
                host_buf[hlen] = '\0';
                long p = strtol(colon + 1, NULL, 10);
                if (p < 1 || p > 65535) {
                    fprintf(stderr, "Error: invalid master server port\n");
                    return 1;
                }
                mport = (uint16_t)p;
            } else {
                if (strlen(optarg) >= sizeof(host_buf)) {
                    fprintf(stderr, "Error: master server hostname too long\n");
                    return 1;
                }
                strncpy(host_buf, optarg, sizeof(host_buf) - 1);
                host_buf[sizeof(host_buf) - 1] = '\0';
            }

            /* Resolve hostname (supports both IPs and DNS names) */
            struct addrinfo hints = {0}, *res;
            hints.ai_family   = AF_INET;
            hints.ai_socktype = SOCK_DGRAM;

            int gai_err = getaddrinfo(host_buf, NULL, &hints, &res);
            if (gai_err != 0 || !res) {
                fprintf(stderr, "Error: cannot resolve master server '%s': %s\n",
                        host_buf, gai_strerror(gai_err));
                return 1;
            }

            cfg.master_addrs[cfg.master_count] =
                *(struct sockaddr_in *)res->ai_addr;
            cfg.master_addrs[cfg.master_count].sin_port = htons(mport);
            cfg.master_count++;
            freeaddrinfo(res);
            break;
        }
        case 'd':   /* --debug: enable LOG_DEBUG output */
            debug = 1;
            break;
        case 'h':   /* --help */
        default:
            usage(argv[0]);
            return (opt == 'h') ? 0 : 1;
        }
    }

    /* --remote-host is the only required argument */
    if (!remote_host) {
        fprintf(stderr, "Error: --remote-host is required\n\n");
        usage(argv[0]);
        return 1;
    }

    /* --- Resolve the remote host IP into a sockaddr_in --- */
    memset(&cfg.remote_addr, 0, sizeof(cfg.remote_addr));
    cfg.remote_addr.sin_family = AF_INET;
    cfg.remote_addr.sin_port   = htons(remote_port);
    if (inet_pton(AF_INET, remote_host, &cfg.remote_addr.sin_addr) != 1) {
        fprintf(stderr, "Error: invalid remote host IP: %s\n", remote_host);
        return 1;
    }

    /* --- Validate parameter ranges --- */
    if (cfg.max_clients < 1 || cfg.max_clients > 1000) {
        fprintf(stderr, "Error: --max-clients must be 1-1000\n");
        return 1;
    }
    if (cfg.session_timeout < 5) {
        fprintf(stderr, "Error: --timeout must be >= 5\n");
        return 1;
    }
    if (cfg.max_new_per_sec < 1) {
        fprintf(stderr, "Error: --rate-limit must be >= 1\n");
        return 1;
    }
    if (cfg.max_query_sessions < 1 || cfg.max_query_sessions > 1000) {
        fprintf(stderr, "Error: --max-query-sessions must be 1-1000\n");
        return 1;
    }
    if (cfg.query_timeout < 1) {
        fprintf(stderr, "Error: --query-timeout must be >= 1\n");
        return 1;
    }

    /* --- Initialise logging and print startup banner --- */
    log_init(debug ? LOG_DEBUG : LOG_INFO);

    log_info("urt-proxy starting");
    log_info("  Listen port:    %u", cfg.listen_port);
    log_info("  Remote server:  %s:%u", remote_host, remote_port);
    log_info("  Max clients:    %d", cfg.max_clients);
    log_info("  Session timeout: %ds", cfg.session_timeout);
    log_info("  Rate limit:     %d new/sec", cfg.max_new_per_sec);
    log_info("  Query sessions: max %d, timeout %ds",
             cfg.max_query_sessions, cfg.query_timeout);
    if (cfg.hostname_tag)
        log_info("  Hostname tag:   \"%s\"", cfg.hostname_tag);
    if (cfg.master_count > 0) {
        for (int i = 0; i < cfg.master_count; i++) {
            char mstr[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &cfg.master_addrs[i].sin_addr,
                      mstr, sizeof(mstr));
            log_info("  Master server:  %s:%u", mstr,
                     ntohs(cfg.master_addrs[i].sin_port));
        }
    } else {
        log_info("  Master server:  (none — not registering in server list)");
    }

    /* Hand off to the blocking event loop — returns on shutdown or error */
    return relay_run(&cfg) == 0 ? 0 : 1;
}
