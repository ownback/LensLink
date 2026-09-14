#include "json.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The scanner walks the object looking for `"key"` tokens at depth 1.
 * Strings are skipped wholesale (a quoted key or value can never contain
 * a match at depth 1), and nested objects/arrays are skipped by depth,
 * so a value like {"mics":[{"id":"a"}]} can't produce false matches.
 */

static const char *skip_string(const char *p)
{
	p++; /* opening quote */
	while (*p && *p != '"') {
		if (*p == '\\' && p[1])
			p++;
		p++;
	}
	return *p ? p + 1 : p;
}

/* Returns the value token of `key` at depth 1, or NULL. */
static const char *find_value(const char *json, const char *key)
{
	size_t klen = strlen(key);
	int depth = 0;
	bool in_str = false;
	const char *p = json;

	while (*p) {
		if (in_str) {
			p = skip_string(p);
			in_str = false;
			continue;
		}
		if (*p == '"') {
			const char *tok = p;
			p = skip_string(p);
			/* A token is a key iff the next non-space char is ':'
			 * at depth 1. */
			const char *q = p;
			while (isspace((unsigned char)*q))
				q++;
			if (depth == 1 && *q == ':' &&
			    (size_t)(p - tok) == klen + 2 &&
			    strncmp(tok + 1, key, klen) == 0) {
				q++;
				while (isspace((unsigned char)*q))
					q++;
				return *q ? q : NULL;
			}
			continue;
		}
		switch (*p) {
		case '{':
		case '[':
			depth++;
			break;
		case '}':
		case ']':
			depth--;
			break;
		}
		p++;
	}
	return NULL;
}

bool json_get_string(const char *json, const char *key, char *out,
		     size_t size)
{
	if (size == 0)
		return false;
	const char *v = find_value(json, key);
	if (!v || *v != '"')
		return false;
	v++;
	size_t n = 0;
	while (*v && *v != '"' && n < size - 1) {
		char c = *v++;
		if (c == '\\' && *v) {
			switch (*v) {
			case 'n':
				c = '\n';
				break;
			case 't':
				c = '\t';
				break;
			case 'r':
				c = '\r';
				break;
			case '"':
			case '\\':
			case '/':
				c = *v;
				break;
			default: /* \uXXXX stays escaped, good enough */
				c = *v;
				break;
			}
			v++;
		}
		out[n++] = c;
	}
	out[n] = 0;
	return *v == '"';
}

bool json_get_bool(const char *json, const char *key)
{
	const char *v = find_value(json, key);
	if (!v)
		return false;
	if (strncmp(v, "true", 4) == 0)
		return true;
	if (strncmp(v, "\"true\"", 6) == 0)
		return true;
	return false;
}

bool json_get_int(const char *json, const char *key, long *out)
{
	const char *v = find_value(json, key);
	if (!v || *v == '"')
		return false;
	char *end = NULL;
	long val = strtol(v, &end, 10);
	if (end == v)
		return false;
	*out = val;
	return true;
}

bool json_get_double(const char *json, const char *key, double *out)
{
	const char *v = find_value(json, key);
	if (!v || *v == '"')
		return false;
	char *end = NULL;
	double val = strtod(v, &end);
	if (end == v)
		return false;
	*out = val;
	return true;
}

void json_escape_to(char *buf, size_t size, const char *s)
{
	size_t n = 0;
	if (size == 0)
		return;
	buf[n++] = '"';
	for (; *s && n + 6 < size; s++) {
		unsigned char c = (unsigned char)*s;
		if (c == '"' || c == '\\') {
			buf[n++] = '\\';
			buf[n++] = (char)c;
		} else if (c < 0x20) {
			n += (size_t)snprintf(buf + n, size - n, "\\u%04x", c);
		} else {
			buf[n++] = (char)c;
		}
	}
	if (n < size - 1)
		buf[n++] = '"';
	buf[n] = 0;
}
