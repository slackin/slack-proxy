/*
 * q3proto.c — Quake 3 / Urban Terror connectionless packet parsing.
 *
 * Provides helpers for inspecting and modifying Q3 out-of-band (OOB)
 * packets.  Only OOB packets are examined — normal game-play packets
 * (whose first 4 bytes are a sequence number) pass through the proxy
 * completely untouched.
 *
 * The main feature is hostname rewriting: when the real server replies
 * to a "getinfo" or "getstatus" query from the master server or a
 * player's server browser, the proxy can prepend a tag (e.g. "[PROXY]")
 * to the sv_hostname value so users can identify the proxy in the list.
 *
 * Note on key names:
 *   - statusResponse uses raw cvar names, so the key is "sv_hostname".
 *   - infoResponse uses a curated key list where the engine maps the
 *     cvar to the shorter name "hostname".
 *   We search for "\\sv_hostname\\" first (longer, more specific) to
 *   avoid accidentally matching a substring, then fall back to
 *   "\\hostname\\".
 */

#include "q3proto.h"
#include <string.h>
#include <stdio.h>
#include <arpa/inet.h>

/*
 * q3_is_connectionless — Check if a packet starts with the OOB marker.
 *
 * A Q3 connectionless packet begins with 4 bytes of 0xFF followed by
 * at least one byte of command data.
 */
int q3_is_connectionless(const uint8_t *data, size_t len)
{
    if (len < 5)
        return 0;
    return data[0] == 0xFF && data[1] == 0xFF &&
           data[2] == 0xFF && data[3] == 0xFF;
}

/*
 * q3_connectionless_cmd — Extract the command word after the OOB marker.
 *
 * Skips the 4 marker bytes and any leading whitespace / newlines, then
 * scans forward until a delimiter (space, newline, backslash, NUL) is
 * found.  Returns a pointer into the original packet data (no copy).
 */
const char *q3_connectionless_cmd(const uint8_t *data, size_t len,
                                  size_t *cmd_len)
{
    if (!q3_is_connectionless(data, len))
        return NULL;

    const char *cmd = (const char *)data + 4;
    size_t remaining = len - 4;

    /* Skip leading whitespace (spaces and newlines before the command) */
    while (remaining > 0 && (*cmd == ' ' || *cmd == '\n')) {
        cmd++;
        remaining--;
    }

    if (remaining == 0)
        return NULL;

    /* Scan the command word — ends at space, newline, backslash, or NUL */
    const char *end = cmd;
    size_t clen = 0;
    while (clen < remaining && end[clen] != ' ' && end[clen] != '\n' &&
           end[clen] != '\\' && end[clen] != '\0') {
        clen++;
    }

    if (cmd_len)
        *cmd_len = clen;
    return cmd;
}

/*
 * q3_rewrite_hostname — Prepend a prefix to sv_hostname in a response.
 *
 * Q3 statusResponse / infoResponse packets carry server info as
 * backslash-delimited key–value pairs:
 *
 *   \xFF\xFF\xFF\xFFstatusResponse\n\\key1\\value1\\key2\\value2...\n
 *
 * This function locates the "\\sv_hostname\\" key, then rebuilds the
 * packet with the value changed from "OldName" to "prefix OldName".
 *
 * The reconstruction is done in three memcpy steps:
 *   1. Everything before the hostname value  (header + key)
 *   2. The new value: "<prefix> <original_value>"
 *   3. Everything after the original value   (remaining keys + players)
 */
size_t q3_rewrite_hostname(const uint8_t *data, size_t len,
                           uint8_t *out, size_t out_cap,
                           const char *prefix)
{
    if (!prefix || !prefix[0])
        return 0;

    /* Identify the command to filter for response packets only */
    size_t cmd_len;
    const char *cmd = q3_connectionless_cmd(data, len, &cmd_len);
    if (!cmd)
        return 0;

    /* Only rewrite statusResponse and infoResponse packets */
    if (!(cmd_len == 14 && memcmp(cmd, "statusResponse", 14) == 0) &&
        !(cmd_len == 12 && memcmp(cmd, "infoResponse", 12) == 0))
        return 0;

    /*
     * Search for the hostname key in the info string.
     *
     * statusResponse packets dump raw cvars, so the key is "sv_hostname".
     * infoResponse packets use a curated key list where the Q3 engine
     * maps it to just "hostname".  Try the longer key first so we don't
     * accidentally match a substring, then fall back to the shorter one.
     */
    const char *needles[] = { "\\sv_hostname\\", "\\hostname\\" };
    const int num_needles = 2;

    const char *haystack = (const char *)data;
    const char *found = NULL;
    size_t matched_needle_len = 0;

    for (int ni = 0; ni < num_needles && !found; ni++) {
        size_t needle_len = strlen(needles[ni]);
        for (size_t i = 0; i + needle_len <= len; i++) {
            if (memcmp(haystack + i, needles[ni], needle_len) == 0) {
                found = haystack + i;
                matched_needle_len = needle_len;
                break;
            }
        }
    }

    if (!found)
        return 0;   /* hostname key not present in this response */

    /* Locate the hostname value: starts right after the key */
    const char *val_start = found + matched_needle_len;
    size_t val_offset = (size_t)(val_start - haystack);

    /* Value ends at the next backslash (next key) or newline (player list) */
    const char *val_end = val_start;
    while ((size_t)(val_end - haystack) < len &&
           *val_end != '\\' && *val_end != '\n') {
        val_end++;
    }

    size_t old_val_len = (size_t)(val_end - val_start);
    size_t prefix_len = strlen(prefix);

    /* Compute total size of the rewritten packet */
    size_t new_val_len = prefix_len + 1 + old_val_len; /* "prefix <old>" */
    size_t new_len = val_offset + new_val_len + (len - val_offset - old_val_len);

    if (new_len > out_cap)
        return 0;   /* Output buffer too small — caller falls back to
                      * forwarding the original unmodified packet. */

    /*
     * Three-step packet reconstruction:
     *   1. Copy everything before the hostname value (OOB header + keys
     *      up to and including the hostname key delimiter).
     *   2. Write the new value: "<prefix> <original_value>".
     *   3. Copy the remainder (subsequent keys and/or player data).
     *
     * This avoids modifying the packet in-place, which would be unsafe
     * because the new value is longer than the original.
     */

    /* Step 1: copy everything up to (but not including) the old value */
    memcpy(out, data, val_offset);

    /* Step 2: write the new value — "<prefix> <original_value>" */
    memcpy(out + val_offset, prefix, prefix_len);
    out[val_offset + prefix_len] = ' ';
    memcpy(out + val_offset + prefix_len + 1, val_start, old_val_len);

    /* Step 3: copy the remainder of the packet (other keys, player data) */
    size_t rest_offset = val_offset + old_val_len;
    memcpy(out + val_offset + new_val_len,
           data + rest_offset,
           len - rest_offset);

    return new_len;
}

/*
 * q3_is_query — Test whether a packet is a server browser query.
 *
 * Matches connectionless "getinfo" and "getstatus" commands only.
 */
int q3_is_query(const uint8_t *data, size_t len)
{
    size_t cmd_len;
    const char *cmd = q3_connectionless_cmd(data, len, &cmd_len);
    if (!cmd)
        return 0;

    if (cmd_len == 7 && memcmp(cmd, "getinfo", 7) == 0)
        return 1;
    if (cmd_len == 9 && memcmp(cmd, "getstatus", 9) == 0)
        return 1;

    return 0;
}

/*
 * q3_is_connect — Test whether a packet is a "connect" command.
 *
 * Matches only the connectionless "connect" command sent by clients.
 */
int q3_is_connect(const uint8_t *data, size_t len)
{
    size_t cmd_len;
    const char *cmd = q3_connectionless_cmd(data, len, &cmd_len);
    if (!cmd)
        return 0;

    return (cmd_len == 7 && memcmp(cmd, "connect", 7) == 0);
}

/*
 * q3_inject_realip — Inject \realip\<ip:port> into a connect packet.
 *
 * The Q3 connect packet has the format:
 *
 *   \xFF\xFF\xFF\xFFconnect "\\key1\\val1\\key2\\val2\\..."
 *
 * or without quotes:
 *
 *   \xFF\xFF\xFF\xFFconnect \\key1\\val1\\key2\\val2\\...
 *
 * This function locates the end of the userinfo string (just before a
 * closing quote or at the end of the packet) and inserts the key-value
 * pair \realip\<ip:port>.  The result is written to the output buffer.
 */
size_t q3_inject_realip(const uint8_t *data, size_t len,
                        uint8_t *out, size_t out_cap,
                        const struct sockaddr_in *client_addr)
{
    if (!q3_is_connect(data, len))
        return 0;

    /* Format the real IP string: "\\realip\\1.2.3.4:12345" */
    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr->sin_addr, ip_str, sizeof(ip_str));

    char realip_kv[64];
    int kv_len = snprintf(realip_kv, sizeof(realip_kv),
                          "\\realip\\%s:%u",
                          ip_str, ntohs(client_addr->sin_port));
    if (kv_len < 0 || (size_t)kv_len >= sizeof(realip_kv))
        return 0;

    /*
     * Find the userinfo string.  After the OOB marker and "connect ",
     * the userinfo may optionally be wrapped in double quotes.
     *
     * Scan past: \xFF\xFF\xFF\xFF  "connect"  " "
     */
    const char *p = (const char *)data + 4; /* skip OOB marker */
    size_t remain = len - 4;

    /* Skip "connect" */
    while (remain > 0 && *p != ' ' && *p != '\n') { p++; remain--; }
    /* Skip whitespace after "connect" */
    while (remain > 0 && *p == ' ') { p++; remain--; }

    if (remain == 0)
        return 0;

    /*
     * Determine if the userinfo is quoted and find the insertion point.
     * We insert our key-value pair just before the closing quote (if
     * quoted) or at the end of the packet data.
     */
    int quoted = (*p == '"');
    size_t info_start = (size_t)(p - (const char *)data);

    /* Find insertion point — end of the userinfo string */
    size_t insert_offset;

    if (quoted) {
        /* Look for the closing quote */
        const char *close_quote = NULL;
        for (size_t i = info_start + 1; i < len; i++) {
            if (data[i] == '"') {
                close_quote = (const char *)data + i;
                break;
            }
        }
        if (close_quote)
            insert_offset = (size_t)(close_quote - (const char *)data);
        else
            insert_offset = len; /* No closing quote — append at end */
    } else {
        /* Unquoted: insert at end, but before any trailing newline/NUL */
        insert_offset = len;
        while (insert_offset > info_start &&
               (data[insert_offset - 1] == '\n' ||
                data[insert_offset - 1] == '\0')) {
            insert_offset--;
        }
    }

    /* Compute new packet length */
    size_t new_len = len + (size_t)kv_len;
    if (new_len > out_cap || new_len > Q3_MAX_PACKET_SIZE)
        return 0;

    /* Build the output packet:
     *   1. Everything before the insertion point
     *   2. The \realip\ip:port key-value pair
     *   3. Everything from the insertion point onward (closing quote, etc.)
     */
    memcpy(out, data, insert_offset);
    memcpy(out + insert_offset, realip_kv, (size_t)kv_len);
    memcpy(out + insert_offset + (size_t)kv_len,
           data + insert_offset,
           len - insert_offset);

    return new_len;
}
