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
 */

#include "q3proto.h"
#include <string.h>

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

    /* Search for the backslash-delimited key "\\sv_hostname\\" */
    const char *needle = "\\sv_hostname\\";
    size_t needle_len = strlen(needle);

    const char *haystack = (const char *)data;
    const char *found = NULL;

    for (size_t i = 0; i + needle_len <= len; i++) {
        if (memcmp(haystack + i, needle, needle_len) == 0) {
            found = haystack + i;
            break;
        }
    }

    if (!found)
        return 0;   /* sv_hostname key not present in this response */

    /* Locate the hostname value: starts right after the key */
    const char *val_start = found + needle_len;
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
        return 0;   /* Output buffer too small */

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
