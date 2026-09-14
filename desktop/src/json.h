/* Tiny flat-JSON reader — enough for the wire protocol's snapshots and
 * the daemon's own config/IPC payloads. Not a general JSON parser. */

#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Copies the string value of `key` (without quotes) into `out`.
 * False if the key is absent or not a string. */
bool json_get_string(const char *json, const char *key, char *out,
		     size_t size);

/* Reads a boolean (true/false). Absent reads as false. */
bool json_get_bool(const char *json, const char *key);

/* Reads an integer value. Returns false when absent or not numeric. */
bool json_get_int(const char *json, const char *key, long *out);

/* Reads a floating-point value. Returns false when absent or not numeric. */
bool json_get_double(const char *json, const char *key, double *out);

/* Appends `s` as a JSON string to `buf` (quotes and control characters
 * escaped), respecting `size`. Always NUL-terminates. */
void json_escape_to(char *buf, size_t size, const char *s);
