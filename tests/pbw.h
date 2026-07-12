// SPDX-License-Identifier: Apache-2.0

/* Tiny protobuf wire *writer* for tests: build Point messages in fixed
 * buffers so the codec vectors are self-contained (no host tooling).
 * Field numbers follow spec §27. */
#ifndef MD_TEST_PBW_H
#define MD_TEST_PBW_H

#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "moddef/doc.h"

typedef struct pb {
    uint8_t *p;
    uint32_t len, cap;
} pb_t;

#define PB(name, capacity)            \
    uint8_t name##_storage[capacity]; \
    pb_t name = {name##_storage, 0, capacity}

static inline void pb_raw(pb_t *b, uint8_t byte)
{
    assert(b->len < b->cap);
    b->p[b->len++] = byte;
}

static inline void pb_varint(pb_t *b, uint64_t v)
{
    do {
        uint8_t byte = v & 0x7f;
        v >>= 7;
        pb_raw(b, v ? (byte | 0x80) : byte);
    } while (v);
}

/* varint field (wire type 0) — emitted even for zero (tests want that). */
static inline void pb_v(pb_t *b, uint32_t field, uint64_t v)
{
    pb_varint(b, ((uint64_t)field << 3) | 0);
    pb_varint(b, v);
}

/* int64 field: negative values are 64-bit two's-complement varints. */
static inline void pb_i(pb_t *b, uint32_t field, int64_t v)
{
    pb_v(b, field, (uint64_t)v);
}

static inline void pb_bytes(pb_t *b, uint32_t field, const void *data, uint32_t n)
{
    pb_varint(b, ((uint64_t)field << 3) | 2);
    pb_varint(b, n);
    assert(b->len + n <= b->cap);
    memcpy(b->p + b->len, data, n);
    b->len += n;
}

static inline void pb_str(pb_t *b, uint32_t field, const char *s)
{
    pb_bytes(b, field, s, (uint32_t)strlen(s));
}

static inline void pb_msg(pb_t *b, uint32_t field, const pb_t *sub)
{
    pb_bytes(b, field, sub->p, sub->len);
}

/* --- composite recipes -------------------------------------------------- */

/* Rational submessage into `field`. */
static inline void pb_rational(pb_t *b, uint32_t field, int64_t num, int64_t den)
{
    PB(r, 24);
    pb_i(&r, 1, num);
    pb_i(&r, 2, den);
    pb_msg(b, field, &r);
}

/* Point header: point_id(1), access(10), storage_type(11). */
static inline void tp_head(pb_t *b, const char *id, uint64_t access, uint64_t storage)
{
    pb_str(b, 1, id);
    if (access)
        pb_v(b, 10, access);
    pb_v(b, 11, storage);
}

/* value_type(12){primitive(1)} */
static inline void tp_prim(pb_t *b, uint64_t prim)
{
    PB(vt, 8);
    pb_v(&vt, 1, prim);
    pb_msg(b, 12, &vt);
}

/* mapping(20){space(1), offset(2), length_words(3), byte_order(5), word_order(6)} */
static inline void tp_mapping(pb_t *b, uint64_t space, uint64_t offset, uint64_t lw,
                              uint64_t byte_order, uint64_t word_order)
{
    PB(m, 32);
    if (space)
        pb_v(&m, 1, space);
    if (offset)
        pb_v(&m, 2, offset);
    if (lw)
        pb_v(&m, 3, lw);
    if (byte_order)
        pb_v(&m, 5, byte_order);
    if (word_order)
        pb_v(&m, 6, word_order);
    pb_msg(b, 20, &m);
}

/* transform(30){scale(1), offset(2)} — pass den 0 to skip either. */
static inline void tp_scale(pb_t *b, int64_t sn, int64_t sd, int64_t on, int64_t od)
{
    PB(t, 64);
    if (sd)
        pb_rational(&t, 1, sn, sd);
    if (od)
        pb_rational(&t, 2, on, od);
    pb_msg(b, 30, &t);
}

/* transform(30){scale_ref(4){point_id(1), mode(2)}} */
static inline void tp_scale_ref(pb_t *b, const char *ref_id, uint64_t mode)
{
    PB(sr, 40);
    pb_str(&sr, 1, ref_id);
    if (mode)
        pb_v(&sr, 2, mode);
    PB(t, 48);
    pb_msg(&t, 4, &sr);
    pb_msg(b, 30, &t);
}

/* na_values(62){raw(1), meaning(2)} */
static inline void tp_na(pb_t *b, int64_t raw, const char *meaning)
{
    PB(na, 48);
    pb_i(&na, 1, raw);
    if (meaning && meaning[0])
        pb_str(&na, 2, meaning);
    pb_msg(b, 62, &na);
}

/* Wrap finished Point bytes as an md_point_t in a synthetic block. */
static inline md_point_t tp_point(const pb_t *b, uint8_t block_space)
{
    md_point_t pt;
    pt.raw.p = b->p;
    pt.raw.len = b->len;
    pt.block.raw.p = 0;
    pt.block.raw.len = 0;
    pt.block.block_id.p = 0;
    pt.block.block_id.len = 0;
    pt.block.space = block_space;
    pt.block.has_discovery = false;
    pt.block.discovery.p = 0;
    pt.block.discovery.len = 0;
    pt.block.discovery_slot = -1;
    return pt;
}

#endif
