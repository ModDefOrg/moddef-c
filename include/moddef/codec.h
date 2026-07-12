// SPDX-License-Identifier: Apache-2.0

/* Codec (spec §8–§15): pure functions over md_point_desc_t, kept in
 * behavioral lockstep with the Go/TS/Rust/Python implementations (shared
 * vector suite in tests/). Allocation-free; strings and bytes decode into
 * caller buffers.
 *
 * The decimal path (§10) computes in double — the same value surface as
 * every other implementation. md_decode_raw is the exact escape hatch for
 * billing counters; FPU-less targets can stay on the raw/integer path. */
#ifndef MODDEF_CODEC_H
#define MODDEF_CODEC_H

#include <stddef.h>

#include "moddef/desc.h"
#include "moddef/err.h"

typedef enum md_val_kind {
    MD_VAL_F64 = 0,     /* scaled DECIMAL / FLOAT */
    MD_VAL_U64,
    MD_VAL_I64,
    MD_VAL_BOOL,
    MD_VAL_FLAGS,       /* raw mask; names via md_flag_iter / md_flags_has */
    MD_VAL_FIELDS,      /* raw window; extract via md_field_value */
    MD_VAL_DATETIME,    /* epoch seconds or millis per the point (§8.5) */
    MD_VAL_UNAVAILABLE  /* §8.4 sentinel; meaning in na_meaning */
} md_val_kind_t;

typedef struct md_value {
    uint8_t kind; /* md_val_kind_t */
    union {
        double f64;
        uint64_t u64;
        int64_t i64;
        bool b;
        uint64_t bits;  /* MD_VAL_FLAGS / MD_VAL_FIELDS */
        int64_t epoch;  /* MD_VAL_DATETIME */
    } v;
    md_str_t na_meaning; /* set for MD_VAL_UNAVAILABLE */
} md_value_t;

static inline md_value_t md_value_f64(double f)
{
    md_value_t v = {MD_VAL_F64, {0}, {0, 0}};
    v.v.f64 = f;
    return v;
}

static inline md_value_t md_value_i64(int64_t i)
{
    md_value_t v = {MD_VAL_I64, {0}, {0, 0}};
    v.v.i64 = i;
    return v;
}

static inline md_value_t md_value_bool(bool b)
{
    md_value_t v = {MD_VAL_BOOL, {0}, {0, 0}};
    v.v.b = b;
    return v;
}

/* Cross-point context: integer values of scale_ref / selector_ref targets
 * (spec §10.4/§10.5). A small array — lookups are O(n) over 1-2 refs. */
typedef struct md_ctx_entry {
    md_str_t id;
    int64_t val;
} md_ctx_entry_t;

typedef struct md_ctx {
    const md_ctx_entry_t *refs;
    uint8_t n;
} md_ctx_t;

#define MD_CTX_EMPTY ((md_ctx_t){0, 0})

/* Decode a point's registers into a typed value. Strings/bytes are not
 * handled here — use md_decode_string / md_decode_bytes. */
md_err_t md_decode(const md_point_desc_t *d, const uint16_t *regs, uint16_t n_regs,
                   const md_ctx_t *ctx, md_value_t *out);

/* Pre-scale integer view (exactness escape hatch). */
md_err_t md_decode_raw(const md_point_desc_t *d, const uint16_t *regs, uint16_t n_regs,
                       uint64_t *raw, uint8_t *bits);

/* Decode a string point (§15) into buf; NUL-terminated, *out_len excludes
 * the NUL. cap must cover the trimmed content + NUL. */
md_err_t md_decode_string(const md_point_desc_t *d, const uint16_t *regs,
                          uint16_t n_regs, char *buf, size_t cap, size_t *out_len);

/* Decode a BYTES_RAW point into buf (*out_len = words * 2). */
md_err_t md_decode_bytes(const md_point_desc_t *d, const uint16_t *regs,
                         uint16_t n_regs, uint8_t *buf, size_t cap, size_t *out_len);

/* Serialize a typed value into out_regs (must be exactly md_point_words). */
md_err_t md_encode(const md_point_desc_t *d, const md_value_t *v, const md_ctx_t *ctx,
                   uint16_t *out_regs, uint16_t n_regs);

md_err_t md_encode_string(const md_point_desc_t *d, md_str_t s, uint16_t *out_regs,
                          uint16_t n_regs);

/* §11.4 write constraint validation (engineering units); returns
 * MD_ERR_CONSTRAINT_* on violation. */
md_err_t md_validate_write(const md_point_desc_t *d, const md_value_t *v);

/* --- flag / field helpers --------------------------------------------------- */

/* Is the named flag set in a decoded MD_VAL_FLAGS mask? */
bool md_flags_has(const md_point_desc_t *d, uint64_t mask, md_str_t name);

/* Extract one named sub-field from a decoded MD_VAL_FIELDS window (§13). */
md_err_t md_field_value(const md_point_desc_t *d, uint64_t window, md_str_t field_id,
                        uint64_t *out);

#endif /* MODDEF_CODEC_H */
