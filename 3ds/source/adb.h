/*
 * Minimal ADB-over-TCP client (host side), enough to run shell commands
 * on a Fire TV / Android TV. Uses POSIX sockets + mbedtls, so it builds
 * both for the 3DS (libctru) and for Linux (used by the host test).
 */
#ifndef ADB_H
#define ADB_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Called repeatedly during long blocking operations (connect, auth wait, scan).
 * `msg` describes what is happening. Return false to cancel. */
typedef bool (*adb_progress_fn)(const char *msg, void *user);

/* Load the RSA key from `key_path` (PEM, e.g. a copy of ~/.android/adbkey)
 * or generate and save a new one. Generation can take a while on a 3DS. */
int  adb_init(const char *key_path, adb_progress_fn cb, void *user);
void adb_shutdown(void);

/* Connect + authenticate + open a persistent shell. Blocks until done.
 * auth_wait_ms: how long to wait for the user to accept the TV prompt.
 * Returns 0 on success, negative on failure (see adb_last_error()). */
int  adb_connect(const char *ip, int port, int auth_wait_ms, adb_progress_fn cb, void *user);
void adb_disconnect(void);
bool adb_is_connected(void);

/* Queue a shell command (no trailing newline needed). Consecutive
 * "input keyevent N" commands are merged into one call for speed. */
bool adb_queue(const char *cmd);

/* Non-blocking: pump network I/O and send queued commands.
 * Call once per frame. Returns false if the connection dropped. */
bool adb_poll(void);

/* Scan a /24 subnet for hosts with `port` open. `ip_in_subnet` is any
 * dotted address in the subnet. Writes up to `max` dotted IPs to `out`.
 * Returns the number found (or negative if cancelled). */
int  adb_scan(const char *ip_in_subnet, int port, char out[][16], int max,
              adb_progress_fn cb, void *user);

const char *adb_last_error(void);
const char *adb_device_name(void);   /* e.g. "AFTMM" once connected */

#endif
