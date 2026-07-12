// SPDX-License-Identifier: Apache-2.0

/* Length-delimited strings. Protobuf strings are not NUL-terminated, and
 * document views point straight into the serialized buffer, so every
 * identifier in the API is an (pointer, length) pair. */
#ifndef MODDEF_STR_H
#define MODDEF_STR_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef struct md_str {
    const char *p;
    uint16_t len;
} md_str_t;

/* String literal → md_str_t (compile-time length). */
#define MD_STR(lit) ((md_str_t){(lit), (uint16_t)(sizeof(lit) - 1)})

static inline md_str_t md_str_cstr(const char *s)
{
    md_str_t out = {s, (uint16_t)(s ? strlen(s) : 0)};
    return out;
}

static inline bool md_str_eq(md_str_t a, md_str_t b)
{
    return a.len == b.len && (a.len == 0 || memcmp(a.p, b.p, a.len) == 0);
}

/* Raw byte range inside a serialized document. */
typedef struct md_bytes {
    const uint8_t *p;
    uint32_t len;
} md_bytes_t;

#endif /* MODDEF_STR_H */
