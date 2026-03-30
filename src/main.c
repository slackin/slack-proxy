#include "relay.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <getopt.h>
#include <arpa/inet.h>

#define DEFAULT_LISTEN_PORT   27960
#define DEFAULT_REMOTE_PORT   27960
#define DEFAULT_MAX_CLIENTS   20
#define DEFAULT_TIMEOUT       30
#define DEFAULT_RATE_LIMIT    5

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
        "  -d, --debug               Enable debug logging\n"
        "  -h, --help                Show this help\n"
        "\n"
        "Example:\n"
        "  %s -r 10.0.0.2 -l 27960 -p 27960 -T \"[PROXY]\"\n",
        prog,
        DEFAULT_LISTEN_PORT, DEFAULT_REMOTE_PORT,
        DEFAULT_MAX_CLIENTS, DEFAULT_TIMEOUT, DEFAULT_RATE_LIMIT,
        prog);
}

int main(int argc, char **argv)
{
    relay_config_t cfg = {0};
    cfg.listen_port    = DEFAULT_LISTEN_PORT;
    cfg.max_clients    = DEFAULT_MAX_CLIENTS;
    cfg.session_timeout = DEFAULT_TIMEOUT;
    cfg.max_new_per_sec = DEFAULT_RATE_LIMIT;

    uint16_t remote_port = DEFAULT_REMOTE_PORT;
    const char *remote_host = NULL;
    int debug = 0;

    static struct option long_opts[] = {
        {"remote-host",  required_argument, NULL, 'r'},
        {"listen-port",  required_argument, NULL, 'l'},
        {"remote-port",  required_argument, NULL, 'p'},
        {"max-clients",  required_argument, NULL, 'm'},
        {"timeout",      required_argument, NULL, 't'},
        {"hostname-tag", required_argument, NULL, 'T'},
        {"rate-limit",   required_argument, NULL, 'R'},
        {"debug",        no_argument,       NULL, 'd'},
        {"help",         no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "r:l:p:m:t:T:R:dh", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'r':
            remote_host = optarg;
            break;
        case 'l': {
            long v = strtol(optarg, NULL, 10);
            if (v < 1 || v > 65535) {
                fprintf(stderr, "Error: --listen-port must be 1-65535\n");
                return 1;
            }
            cfg.listen_port = (uint16_t)v;
            break;
        }
        case 'p': {
            long v = strtol(optarg, NULL, 10);
            if (v < 1 || v > 65535) {
                fprintf(stderr, "Error: --remote-port must be 1-65535\n");
                return 1;
            }
            remote_port = (uint16_t)v;
            break;
        }
        case 'm':
            cfg.max_clients = (int)strtol(optarg, NULL, 10);
            break;
        case 't':
            cfg.session_timeout = (int)strtol(optarg, NULL, 10);
            break;
        case 'T':
            cfg.hostname_tag = optarg;
            break;
        case 'R':
            cfg.max_new_per_sec = (int)strtol(optarg, NULL, 10);
            break;
        case 'd':
            debug = 1;
            break;
        case 'h':
        default:
            usage(argv[0]);
            return (opt == 'h') ? 0 : 1;
        }
    }

    if (!remote_host) {
        fprintf(stderr, "Error: --remote-host is required\n\n");
        usage(argv[0]);
        return 1;
    }

    /* Validate remote host */
    memset(&cfg.remote_addr, 0, sizeof(cfg.remote_addr));
    cfg.remote_addr.sin_family = AF_INET;
    cfg.remote_addr.sin_port   = htons(remote_port);
    if (inet_pton(AF_INET, remote_host, &cfg.remote_addr.sin_addr) != 1) {
        fprintf(stderr, "Error: invalid remote host IP: %s\n", remote_host);
        return 1;
    }

    /* Validate parameters */
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

    log_init(debug ? LOG_DEBUG : LOG_INFO);

    log_info("urt-proxy starting");
    log_info("  Listen port:    %u", cfg.listen_port);
    log_info("  Remote server:  %s:%u", remote_host, remote_port);
    log_info("  Max clients:    %d", cfg.max_clients);
    log_info("  Session timeout: %ds", cfg.session_timeout);
    log_info("  Rate limit:     %d new/sec", cfg.max_new_per_sec);
    if (cfg.hostname_tag)
        log_info("  Hostname tag:   \"%s\"", cfg.hostname_tag);

    return relay_run(&cfg) == 0 ? 0 : 1;
}
