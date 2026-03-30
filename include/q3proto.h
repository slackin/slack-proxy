#ifndef URT_Q3PROTO_H
#define URT_Q3PROTO_H

#include <stddef.h>
#include <stdint.h>

/* Q3 connectionless packet marker: 4 bytes of 0xFF */
#define Q3_CONNECTIONLESS_MARKER 0xFFFFFFFF

/* Max Q3 packet size */
#define Q3_MAX_PACKET_SIZE 16384

/* Check if a packet is a Q3 connectionless (OOB) packet */
int  q3_is_connectionless(const uint8_t *data, size_t len);

/*
 * Extract the command name from a connectionless packet.
 * Returns pointer into `data` where command starts, or NULL.
 * Sets *cmd_len to length of the command word.
 */
const char *q3_connectionless_cmd(const uint8_t *data, size_t len,
                                  size_t *cmd_len);

/*
 * Rewrite sv_hostname in a getstatus/getinfo response.
 * Writes the modified packet into `out` (max out_cap bytes).
 * Returns new packet length, or 0 on failure / no change needed.
 */
size_t q3_rewrite_hostname(const uint8_t *data, size_t len,
                           uint8_t *out, size_t out_cap,
                           const char *prefix);

#endif
