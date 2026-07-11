/* Minimal protobuf wire reader (spec §27). Read-only, allocation-free:
 * documents are navigated in place wherever they live (memory-mapped
 * flash, RAM). Only what the ModDef schema uses: varint (0),
 * length-delimited (2), and skippable fixed32/64. */
#ifndef MODDEF_WIRE_H
#define MODDEF_WIRE_H

#include <stdbool.h>
#include <stdint.h>

#include "moddef/str.h"

typedef struct md_wire {
    const uint8_t *p;
    const uint8_t *end;
} md_wire_t;

static inline md_wire_t md_wire(md_bytes_t b)
{
    md_wire_t w = {b.p, b.p + b.len};
    return w;
}

static inline bool md_wire_done(const md_wire_t *w)
{
    return w->p >= w->end;
}

/* Read the next field tag; false at end of message or on malformed data. */
bool md_wire_tag(md_wire_t *w, uint32_t *field, uint8_t *wtype);

bool md_wire_varint(md_wire_t *w, uint64_t *v);

/* Length-delimited payload (strings, bytes, submessages, packed scalars). */
bool md_wire_len(md_wire_t *w, md_bytes_t *out);

/* Skip a field of the given wire type. */
bool md_wire_skip(md_wire_t *w, uint8_t wtype);

/* --- convenience over a whole (sub)message --------------------------------- */

/* First occurrence of a length-delimited field; false if absent. */
bool md_wire_find_len(md_bytes_t msg, uint32_t field, md_bytes_t *out);

/* First occurrence of a varint field; false if absent (proto3 default). */
bool md_wire_find_varint(md_bytes_t msg, uint32_t field, uint64_t *out);

static inline md_str_t md_wire_str(md_bytes_t b)
{
    md_str_t s = {(const char *)b.p, (uint16_t)b.len};
    return s;
}

/* int64 fields arrive as 64-bit two's-complement varints. */
static inline int64_t md_wire_i64(uint64_t v)
{
    return (int64_t)v;
}

#endif /* MODDEF_WIRE_H */
