#include "q3proto.h"
#include <string.h>

int q3_is_connectionless(const uint8_t *data, size_t len)
{
    if (len < 5)
        return 0;
    return data[0] == 0xFF && data[1] == 0xFF &&
           data[2] == 0xFF && data[3] == 0xFF;
}

const char *q3_connectionless_cmd(const uint8_t *data, size_t len,
                                  size_t *cmd_len)
{
    if (!q3_is_connectionless(data, len))
        return NULL;

    const char *cmd = (const char *)data + 4;
    size_t remaining = len - 4;

    /* Skip leading spaces/newlines */
    while (remaining > 0 && (*cmd == ' ' || *cmd == '\n')) {
        cmd++;
        remaining--;
    }

    if (remaining == 0)
        return NULL;

    /* Command word ends at space, newline, backslash, or end of data */
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
 * Q3 statusResponse / infoResponse format:
 *   \xFF\xFF\xFF\xFFstatusResponse\n\\key\\value\\key\\value...\n<players>
 *   \xFF\xFF\xFF\xFFinfoResponse\n\\key\\value\\key\\value...
 *
 * We look for \\sv_hostname\\ and rewrite the value.
 */
size_t q3_rewrite_hostname(const uint8_t *data, size_t len,
                           uint8_t *out, size_t out_cap,
                           const char *prefix)
{
    if (!prefix || !prefix[0])
        return 0;

    size_t cmd_len;
    const char *cmd = q3_connectionless_cmd(data, len, &cmd_len);
    if (!cmd)
        return 0;

    /* Only rewrite statusResponse and infoResponse */
    if (!(cmd_len == 14 && memcmp(cmd, "statusResponse", 14) == 0) &&
        !(cmd_len == 12 && memcmp(cmd, "infoResponse", 12) == 0))
        return 0;

    /* Find \\sv_hostname\\ in the packet */
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
        return 0;

    /* Value starts after the needle */
    const char *val_start = found + needle_len;
    size_t val_offset = (size_t)(val_start - haystack);

    /* Value ends at next backslash or newline */
    const char *val_end = val_start;
    while ((size_t)(val_end - haystack) < len &&
           *val_end != '\\' && *val_end != '\n') {
        val_end++;
    }

    size_t old_val_len = (size_t)(val_end - val_start);
    size_t prefix_len = strlen(prefix);

    /* New packet: before val | prefix + " " + old val | rest */
    size_t new_val_len = prefix_len + 1 + old_val_len;
    size_t new_len = val_offset + new_val_len + (len - val_offset - old_val_len);

    if (new_len > out_cap)
        return 0;

    /* Copy: header up to value start */
    memcpy(out, data, val_offset);

    /* Write new value: "prefix old_value" */
    memcpy(out + val_offset, prefix, prefix_len);
    out[val_offset + prefix_len] = ' ';
    memcpy(out + val_offset + prefix_len + 1, val_start, old_val_len);

    /* Copy remainder */
    size_t rest_offset = val_offset + old_val_len;
    memcpy(out + val_offset + new_val_len,
           data + rest_offset,
           len - rest_offset);

    return new_len;
}
