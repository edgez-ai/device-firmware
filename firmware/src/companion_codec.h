/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
struct reader { const uint8_t *p; size_t n; };
struct writer { uint8_t *p; size_t n, cap; bool failed; };
static inline int number(struct reader *r, uint64_t *value)
{
	*value = 0;
	for (unsigned i = 0; i < 10; ++i) {
		if (!r->n) return -EBADMSG;
		uint8_t b = *r->p++; --r->n;
		if (i == 9 && b > 1) return -EBADMSG;
		*value |= (uint64_t)(b & 127) << (7 * i);
		if (!(b & 128)) return 0;
	}
	return -EBADMSG;
}
static inline int next(struct reader *r, uint32_t *tag, uint64_t *value, struct reader *bytes)
{
	uint64_t key;
	if (number(r, &key) || !(key >> 3) || key >> 3 > 0x1fffffff) return -EBADMSG;
	*tag = key >> 3; *value = 0; *bytes = (struct reader){0};
	switch (key & 7) {
	case 0: return number(r, value);
	case 1: if (r->n < 8) return -EBADMSG; r->p += 8; r->n -= 8; return 1;
	case 2:
		if (number(r, value) || *value > r->n) return -EBADMSG;
		*bytes = (struct reader){r->p, *value}; r->p += *value; r->n -= *value; return 2;
	case 5: if (r->n < 4) return -EBADMSG; *bytes = (struct reader){r->p, 4}; r->p += 4; r->n -= 4; return 5;
	default: return -EBADMSG;
	}
}
static inline void put(struct writer *w, uint64_t value)
{
	do {
		if (w->n >= w->cap) { w->failed = true; return; }
		uint8_t b = value & 127; value >>= 7;
		w->p[w->n++] = b | (value ? 128 : 0);
	} while (value);
}
static inline void integer(struct writer *w, uint32_t tag, uint64_t value)
{
	put(w, (uint64_t)tag << 3); put(w, value);
}
static inline void blob(struct writer *w, uint32_t tag, const void *data, size_t size)
{
	put(w, ((uint64_t)tag << 3) | 2); put(w, size);
	if (w->failed || size > w->cap - w->n) { w->failed = true; return; }
	if (size) memcpy(w->p + w->n, data, size);
	w->n += size;
}
static inline void text(struct writer *w, uint32_t tag, const char *value)
{
	if (value && *value) blob(w, tag, value, strlen(value));
}
