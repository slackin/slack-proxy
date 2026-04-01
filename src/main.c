/*
 * main.c — Entry point and command-line argument parsing for urt-proxy.
 *
 * Parses command-line options with getopt_long, validates all parameters,
 * populates a relay_config_t, initialises the logger, and hands control
 * to relay_run() which runs the event loop until shutdown.
 */

#define _POSIX_C_SOURCE 200809L

#include "relay.h"
#include "config.h"
#include "mgmt.h"
#include "q3proto.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <getopt.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

/* Default values for optional CLI arguments */
#define DEFAULT_LISTEN_PORT   27990
#define DEFAULT_REMOTE_PORT   27960
#define DEFAULT_MAX_CLIENTS   20
#define DEFAULT_TIMEOUT       30   /* seconds */
#define DEFAULT_RATE_LIMIT    5    /* new sessions per second */
#define DEFAULT_MAX_QUERY     100  /* max concurrent browser query sessions */
#define DEFAULT_QUERY_TIMEOUT 5    /* seconds */

/* Default path for auto-generated management API key file */
#define DEFAULT_KEY_FILE      ".urt-proxy.key"

/* Length of auto-generated API key in hex characters (32 = 16 random bytes) */
#define GENERATED_KEY_LEN     32

/*
 * generate_api_key — Create a cryptographically random hex API key.
 *
 * Reads from /dev/urandom to produce a hex string of GENERATED_KEY_LEN
 * characters.  The returned string is malloc'd and must be freed by
 * the caller.
 *
 * @return  Heap-allocated hex key string, or NULL on failure.
 */
static char *generate_api_key(void)
{
    int num_bytes = GENERATED_KEY_LEN / 2;
    unsigned char buf[16];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        perror("open /dev/urandom");
        return NULL;
    }
    ssize_t n = read(fd, buf, (size_t)num_bytes);
    close(fd);
    if (n != num_bytes) {
        fprintf(stderr, "Error: short read from /dev/urandom\n");
        return NULL;
    }

    char *key = malloc(GENERATED_KEY_LEN + 1);
    if (!key)
        return NULL;

    for (int i = 0; i < num_bytes; i++)
        snprintf(key + i * 2, 3, "%02x", buf[i]);

    key[GENERATED_KEY_LEN] = '\0';
    return key;
}

/*
 * load_key_file — Read an API key from a file, trimming whitespace.
 *
 * @param path  Path to the key file.
 * @return  Heap-allocated key string, or NULL if file doesn't exist / can't read.
 */
static char *load_key_file(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp)
        return NULL;

    char line[256];
    if (!fgets(line, sizeof(line), fp)) {
        fclose(fp);
        return NULL;
    }
    fclose(fp);

    /* Trim trailing whitespace / newlines */
    char *end = line + strlen(line) - 1;
    while (end >= line && (*end == '\n' || *end == '\r' || *end == ' '))
        *end-- = '\0';

    if (line[0] == '\0')
        return NULL;

    return strdup(line);
}

/*
 * save_key_file — Write an API key to a file with restrictive permissions.
 *
 * Creates the file with mode 0600 (owner read/write only) to prevent
 * other users from reading the key.
 *
 * @param path  Path to the key file.
 * @param key   The API key string to write.
 * @return  0 on success, -1 on failure.
 */
static int save_key_file(const char *path, const char *key)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        fprintf(stderr, "Error: cannot create key file '%s': %s\n",
                path, strerror(errno));
        return -1;
    }
    size_t len = strlen(key);
    if (write(fd, key, len) != (ssize_t)len || write(fd, "\n", 1) != 1) {
        fprintf(stderr, "Error: failed to write key file '%s'\n", path);
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

/*
 * ensure_api_key — Load or generate the management API key.
 *
 * If --mgmt-key was provided explicitly, use that.  Otherwise, try to
 * load the key from key_file_path.  If no key file exists, generate a
 * new random key, save it, and display it to the user.
 *
 * @param mgmt_cfg       Management config (may be modified).
 * @param key_file_path  Path to the persistent key file.
 * @return  0 on success (key is set), -1 on error.
 */
static int ensure_api_key(mgmt_config_t *mgmt_cfg, const char *key_file_path)
{
    /* If the user already provided a key via --mgmt-key, we're done. */
    if (mgmt_cfg->api_key && mgmt_cfg->api_key[0] != '\0')
        return 0;

    /* Try loading from the key file */
    char *key = load_key_file(key_file_path);
    if (key) {
        mgmt_cfg->api_key = key;
        mgmt_cfg->enabled = 1;
        return 0;
    }

    /* Generate a new key */
    key = generate_api_key();
    if (!key) {
        fprintf(stderr, "Error: failed to generate API key\n");
        return -1;
    }

    if (save_key_file(key_file_path, key) < 0) {
        free(key);
        return -1;
    }

    mgmt_cfg->api_key = key;
    mgmt_cfg->enabled = 1;

    fprintf(stderr,
        "\n"
        "╔══════════════════════════════════════════════════════════════╗\n"
        "║  Management API key generated and saved                     ║\n"
        "╠══════════════════════════════════════════════════════════════╣\n"
        "║                                                             ║\n"
        "║  Key:  %-32s                   ║\n"
        "║  File: %-48s   ║\n"
        "║                                                             ║\n"
        "║  Use this key in the GUI client to connect.                 ║\n"
        "║  The key is saved and will be reused on next startup.       ║\n"
        "╚══════════════════════════════════════════════════════════════╝\n"
        "\n", key, key_file_path);

    return 0;
}

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
        "Required (single-server mode):\n"
        "  -r, --remote-host HOST    Real server IP (WireGuard address)\n"
        "\n"
        "Config file mode (supports multiple servers):\n"
        "  -c, --config FILE         Load servers from an INI config file\n"
        "                            (ignores -r, -l, -p, -m, -t, -T, -R, -Q, -q, -M)\n"
        "\n"
        "Optional (single-server mode):\n"
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
        "\n"
        "Common:\n"
        "  -d, --debug               Enable debug logging\n"
        "  -h, --help                Show this help\n"
        "\n"
        "Management API:\n"
        "  --mgmt-key KEY            Enable management API with this API key\n"
        "  --mgmt-key-file PATH      Path to API key file (default: %s)\n"
        "                            If no --mgmt-key is given, the key is loaded\n"
        "                            from this file (auto-generated on first run)\n"
        "  --mgmt-port PORT          Management listen port (default: %d)\n"
        "  --mgmt-addr ADDR          Management listen address (default: 127.0.0.1)\n"
        "\n"
        "Management-only mode:\n"
        "  Start with just --mgmt-key or --mgmt-key-file and no -r / -c to open\n"
        "  only the management port.  Servers can then be added via the GUI client.\n"
        "\n"
        "Examples:\n"
        "  %s -r 10.0.0.2 -l 27990 -p 27960 -T \"[PROXY]\" -M master.urbanterror.info\n"
        "  %s -c /etc/urt-proxy.conf\n"
        "  %s --mgmt-key-file ~/.urt-proxy.key\n",
        prog,
        DEFAULT_LISTEN_PORT, DEFAULT_REMOTE_PORT,
        DEFAULT_MAX_CLIENTS, DEFAULT_TIMEOUT, DEFAULT_RATE_LIMIT,
        DEFAULT_MAX_QUERY, DEFAULT_QUERY_TIMEOUT,
        RELAY_MAX_MASTERS,
        DEFAULT_KEY_FILE,
        MGMT_DEFAULT_PORT,
        prog, prog, prog);
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
    const char *config_file = NULL;
    const char *key_file_path = DEFAULT_KEY_FILE;
    int debug = 0;

    /* Management API options (CLI, applied in both modes) */
    mgmt_config_t mgmt_cfg = {0};

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
        {"config",       required_argument, NULL, 'c'},
        {"debug",        no_argument,       NULL, 'd'},
        {"help",         no_argument,       NULL, 'h'},
        {"mgmt-key",     required_argument, NULL,  1 },
        {"mgmt-port",    required_argument, NULL,  2 },
        {"mgmt-addr",    required_argument, NULL,  3 },
        {"mgmt-key-file",required_argument, NULL,  4 },
        {NULL, 0, NULL, 0}
    };

    /* --- Parse command-line arguments --- */
    int opt;
    while ((opt = getopt_long(argc, argv, "r:l:p:m:t:T:R:Q:q:M:c:dh", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'r':   /* --remote-host (required in single-server mode) */
            remote_host = optarg;
            break;
        case 'c':   /* --config FILE */
            config_file = optarg;
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
        case 'M': { /* --master-server HOST[:PORT] — register with a master server.
                     * This parsing logic mirrors resolve_host_port() in config.c
                     * but is kept inline here to avoid adding a public API just
                     * for the CLI path. */
            if (cfg.master_count >= RELAY_MAX_MASTERS) {
                fprintf(stderr, "Error: cannot specify more than %d master "
                        "servers\n", RELAY_MAX_MASTERS);
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
            cfg.heartbeat_enabled = 1;
            freeaddrinfo(res);
            break;
        }
        case 'd':   /* --debug: enable LOG_DEBUG output */
            debug = 1;
            break;
        case 1:     /* --mgmt-key: API key to enable management */
            mgmt_cfg.api_key = optarg;
            mgmt_cfg.enabled = 1;
            break;
        case 2: {   /* --mgmt-port: management listen port */
            long v = strtol(optarg, NULL, 10);
            if (v < 1 || v > 65535) {
                fprintf(stderr, "Error: --mgmt-port must be 1-65535\n");
                return 1;
            }
            mgmt_cfg.port = (uint16_t)v;
            break;
        }
        case 3:     /* --mgmt-addr: management listen address */
            if (inet_pton(AF_INET, optarg,
                          &mgmt_cfg.listen_addr.sin_addr) != 1) {
                fprintf(stderr, "Error: '%s' is not a valid IPv4 address\n",
                        optarg);
                return 1;
            }
            mgmt_cfg.listen_addr.sin_family = AF_INET;
            break;
        case 4:     /* --mgmt-key-file: path to API key file */
            key_file_path = optarg;
            break;
        case 'h':   /* --help */
        default:
            usage(argv[0]);
            return (opt == 'h') ? 0 : 1;
        }
    }

    /* ================================================================ */
    /*  Auto-load or generate management API key.                       */
    /*                                                                  */
    /*  If no explicit --mgmt-key was given, attempt to load or         */
    /*  generate one from the key file.  This enables the management    */
    /*  API automatically when a key file exists or is created.         */
    /* ================================================================ */
    if (ensure_api_key(&mgmt_cfg, key_file_path) < 0)
        return 1;

    /* ================================================================ */
    /*  Config-file mode: load from INI, then run.                      */
    /*                                                                  */
    /*  When -c is given, all single-server CLI flags (-r, -l, -p, etc) */
    /*  are ignored — the config file is the sole source of truth.      */
    /*  Only -d (debug) is honoured as a CLI override so the operator   */
    /*  can enable verbose logging without editing the config file.     */
    /* ================================================================ */
    if (config_file) {
        proxy_config_t pcfg;
        if (config_load(config_file, &pcfg) < 0)
            return 1;

        /* -d / --debug on the CLI overrides the config file */
        if (debug)
            pcfg.debug = 1;

        log_init(pcfg.debug ? LOG_DEBUG : LOG_INFO);
        log_info("urt-proxy starting (%d server(s) from %s)",
                 pcfg.server_count, config_file);

        /* CLI --mgmt-* flags override config-file mgmt settings */
        if (mgmt_cfg.enabled) {
            pcfg.mgmt = mgmt_cfg;
        }
        /* Apply mgmt defaults if needed */
        if (pcfg.mgmt.enabled) {
            if (pcfg.mgmt.listen_addr.sin_family == 0) {
                pcfg.mgmt.listen_addr.sin_family = AF_INET;
                pcfg.mgmt.listen_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            }
            if (pcfg.mgmt.port == 0) {
                pcfg.mgmt.port = MGMT_DEFAULT_PORT;
                pcfg.mgmt.listen_addr.sin_port = htons(MGMT_DEFAULT_PORT);
            }
            if (pcfg.mgmt.listen_addr.sin_port == 0)
                pcfg.mgmt.listen_addr.sin_port = htons(pcfg.mgmt.port);
        }

        return relay_run(pcfg.servers, pcfg.server_count,
                         pcfg.mgmt.enabled ? &pcfg.mgmt : NULL) == 0 ? 0 : 1;
    }

    /* ================================================================ */
    /*  Single-server CLI mode (original behaviour)                     */
    /* ================================================================ */

    /* ================================================================ */
    /*  Management-only mode (no -r, no -c, but management enabled)     */
    /*                                                                  */
    /*  Opens only the management API port so the operator can add      */
    /*  servers via the GUI client.                                     */
    /* ================================================================ */
    if (!remote_host && mgmt_cfg.enabled) {
        log_init(debug ? LOG_DEBUG : LOG_INFO);
        log_info("urt-proxy starting (management-only mode)");

        /* Apply mgmt defaults */
        if (mgmt_cfg.listen_addr.sin_family == 0) {
            mgmt_cfg.listen_addr.sin_family = AF_INET;
            mgmt_cfg.listen_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        }
        if (mgmt_cfg.port == 0) {
            mgmt_cfg.port = MGMT_DEFAULT_PORT;
            mgmt_cfg.listen_addr.sin_port = htons(MGMT_DEFAULT_PORT);
        }
        if (mgmt_cfg.listen_addr.sin_port == 0)
            mgmt_cfg.listen_addr.sin_port = htons(mgmt_cfg.port);

        return relay_run(NULL, 0, &mgmt_cfg) == 0 ? 0 : 1;
    }

    /* --remote-host is required in single-server mode */
    if (!remote_host) {
        fprintf(stderr, "Error: --remote-host is required (or use --config "
                "for multi-server mode)\n\n");
        usage(argv[0]);
        return 1;
    }

    /* --- Resolve the remote host IP into a sockaddr_in --- */
    memset(&cfg.remote_addr, 0, sizeof(cfg.remote_addr));
    cfg.remote_addr.sin_family = AF_INET;
    cfg.remote_addr.sin_port   = htons(remote_port);
    if (inet_pton(AF_INET, remote_host, &cfg.remote_addr.sin_addr) != 1) {
        fprintf(stderr, "Error: '%s' is not a valid IPv4 address — use a "
                "dotted-quad (e.g. 10.0.0.2)\n", remote_host);
        return 1;
    }

    /* --- Validate parameter ranges --- */
    if (cfg.max_clients < 1 || cfg.max_clients > 1000) {
        fprintf(stderr, "Error: --max-clients must be between 1 and 1000\n");
        return 1;
    }
    if (cfg.session_timeout < 5) {
        fprintf(stderr, "Error: --timeout must be at least 5 seconds\n");
        return 1;
    }
    if (cfg.max_new_per_sec < 1) {
        fprintf(stderr, "Error: --rate-limit must be at least 1 session/sec\n");
        return 1;
    }
    if (cfg.max_query_sessions < 1 || cfg.max_query_sessions > 1000) {
        fprintf(stderr, "Error: --max-query-sessions must be between 1 and "
                "1000\n");
        return 1;
    }
    if (cfg.query_timeout < 1) {
        fprintf(stderr, "Error: --query-timeout must be at least 1 second\n");
        return 1;
    }

    /* --- Initialise logging and print startup banner --- */
    log_init(debug ? LOG_DEBUG : LOG_INFO);

    log_info("urt-proxy starting (single-server mode)");
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

    /* --- Apply management API defaults for single-server mode --- */
    if (mgmt_cfg.enabled) {
        if (mgmt_cfg.listen_addr.sin_family == 0) {
            mgmt_cfg.listen_addr.sin_family = AF_INET;
            mgmt_cfg.listen_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        }
        if (mgmt_cfg.port == 0) {
            mgmt_cfg.port = MGMT_DEFAULT_PORT;
            mgmt_cfg.listen_addr.sin_port = htons(MGMT_DEFAULT_PORT);
        }
        if (mgmt_cfg.listen_addr.sin_port == 0)
            mgmt_cfg.listen_addr.sin_port = htons(mgmt_cfg.port);
    }

    /* Hand off to the blocking event loop — returns on shutdown or error */
    return relay_run(&cfg, 1,
                     mgmt_cfg.enabled ? &mgmt_cfg : NULL) == 0 ? 0 : 1;
}
