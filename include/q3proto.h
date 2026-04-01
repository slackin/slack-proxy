/*
 * q3proto.h — Quake 3 / Urban Terror protocol helpers.
 *
 * The Q3 engine uses two packet classes over UDP:
 *
 *   1. Connectionless (out-of-band) packets — start with 4 bytes of 0xFF
 *      followed by a plain-text command such as "getinfo", "getstatus",
 *      "statusResponse", "infoResponse", etc.
 *
 *   2. Game (in-band) packets — first 4 bytes form a sequence number.
 *      These pass through the proxy unmodified.
 *
 * This module only inspects connectionless packets to detect server
 * browser queries and optionally rewrite the sv_hostname value so
 * the proxy can be identified in the server list.
 *
 * Packet format for statusResponse / infoResponse:
 *   \xFF\xFF\xFF\xFF<command>\n\\key1\\value1\\key2\\value2...\n<players>
 */

#ifndef URT_Q3PROTO_H
#define URT_Q3PROTO_H

#include <stddef.h>
#include <stdint.h>

/* Connectionless (OOB) marker: first 4 bytes of the packet are 0xFF. */
#define Q3_CONNECTIONLESS_MARKER 0xFFFFFFFF

/* Maximum Q3 engine UDP packet size in bytes. */
#define Q3_MAX_PACKET_SIZE 16384

/*
 * Heartbeat protocol constants.
 *
 * Q3/UrT servers register with master servers by sending periodic
 * heartbeat packets.  The master responds with a getinfo challenge,
 * and the server's infoResponse confirms the listing.
 */
#define Q3_HEARTBEAT_GAME      "QuakeArena-1"  /* Game identifier in heartbeat */
#define Q3_HEARTBEAT_INTERVAL  300              /* Seconds between heartbeats   */
#define Q3_DEFAULT_MASTER_PORT 27900            /* Standard master server port  */

/*
 * q3_is_connectionless — Test whether a packet is an OOB packet.
 *
 * Checks that the packet is at least 5 bytes and starts with the
 * 4-byte marker 0xFFFFFFFF.
 *
 * @param data  Raw packet data.
 * @param len   Length of the packet in bytes.
 * @return      Non-zero if the packet is connectionless, 0 otherwise.
 */
int q3_is_connectionless(const uint8_t *data, size_t len);

/*
 * q3_connectionless_cmd — Extract the command word from an OOB packet.
 *
 * Skips the 4-byte marker and any leading whitespace, then returns a
 * pointer to the first command character.  The command word is
 * delimited by a space, newline, backslash, or end of packet.
 *
 * @param data     Raw packet data.
 * @param len      Packet length in bytes.
 * @param cmd_len  [out] Receives the length of the command word.
 * @return         Pointer into @a data where the command starts, or
 *                 NULL if the packet is not connectionless or empty.
 */
const char *q3_connectionless_cmd(const uint8_t *data, size_t len,
                                  size_t *cmd_len);

/*
 * q3_rewrite_hostname — Prepend a tag to sv_hostname in a server response.
 *
 * Searches for the key "\\sv_hostname\\" in a statusResponse or
 * infoResponse packet and rewrites the value to "<prefix> <old_value>".
 * The modified packet is written to @a out.
 *
 * Only operates on statusResponse and infoResponse packets; all other
 * packet types are left untouched (returns 0).
 *
 * @param data     Original packet data.
 * @param len      Original packet length.
 * @param out      Output buffer for the rewritten packet.
 * @param out_cap  Size of the output buffer.
 * @param prefix   Hostname prefix string (e.g. "[PROXY]").
 * @return         Length of the rewritten packet in @a out, or 0 if no
 *                 rewrite was performed (wrong packet type, key not
 *                 found, or output buffer too small).
 */
size_t q3_rewrite_hostname(const uint8_t *data, size_t len,
                           uint8_t *out, size_t out_cap,
                           const char *prefix);

/*
 * q3_is_query — Test whether a packet is a server browser query.
 *
 * Returns non-zero if the packet is a connectionless "getinfo" or
 * "getstatus" command — the single-packet queries sent by the master
 * server and player server browsers.  All other packet types
 * (getchallenge, connect, game data, etc.) return 0.
 *
 * @param data  Raw packet data.
 * @param len   Length of the packet in bytes.
 * @return      Non-zero if the packet is a browser query, 0 otherwise.
 */
int q3_is_query(const uint8_t *data, size_t len);

#endif
