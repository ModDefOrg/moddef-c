// SPDX-License-Identifier: Apache-2.0

/* Compact point descriptor (spec §7–§15): the codec's view of one point,
 * expanded from the wire by md_point_parse into ~130 stack bytes. Scalar
 * fields are decoded eagerly; repeated sections (na_values, selector
 * cases, flags, packed fields, allowed_values) stay as wire ranges with
 * on-demand iterators — nothing is allocated.
 *
 * Kind/precedence decisions mirror the Go/TS/Rust/Py implementations:
 * composed > flags > fields > string/bytes > primitive (default: raw
 * enum integer, signed if the storage is). */
#ifndef MODDEF_DESC_H
#define MODDEF_DESC_H

#include "moddef/doc.h"
#include "moddef/err.h"
#include "moddef/str.h"
#include "moddef/wire.h"

/* §7 address spaces (wire values). */
typedef enum md_space {
    MD_SPACE_COIL = 1,
    MD_SPACE_DISCRETE = 2,
    MD_SPACE_INPUT = 3,
    MD_SPACE_HOLDING = 4
} md_space_t;

/* §8.2 storage types (wire values). */
typedef enum md_storage {
    MD_ST_UNSPECIFIED = 0,
    MD_ST_BIT = 1,
    MD_ST_U16 = 2,
    MD_ST_S16 = 3,
    MD_ST_U32 = 4,
    MD_ST_S32 = 5,
    MD_ST_U64 = 6,
    MD_ST_S64 = 7,
    MD_ST_F32 = 8,
    MD_ST_F64 = 9,
    MD_ST_STRING_ASCII = 10,
    MD_ST_STRING_UTF8 = 11,
    MD_ST_BYTES_RAW = 12,
    MD_ST_BCD = 13,
    MD_ST_FIXED_POINT = 14,
    MD_ST_COMPOSED = 15,
    MD_ST_U24 = 16,
    MD_ST_U48 = 17,
    MD_ST_S48 = 18
} md_storage_t;

/* §11.1 access modes (wire values). */
typedef enum md_access {
    MD_ACCESS_RO = 1,
    MD_ACCESS_WO = 2,
    MD_ACCESS_RW = 3,
    MD_ACCESS_CMD = 4
} md_access_t;

/* Logical value interpretation (derived, not a wire enum). */
typedef enum md_vkind {
    MD_VK_DECIMAL = 0, /* scaled numeric (§10) */
    MD_VK_BOOL,
    MD_VK_UINT,
    MD_VK_INT,
    MD_VK_ENUM,        /* raw integer; caller maps via the profile enum */
    MD_VK_FLAGS,       /* §13.2 */
    MD_VK_FIELDS,      /* §13/§13.1 packed window */
    MD_VK_STRING,      /* §15 — use md_decode_string */
    MD_VK_BYTES,
    MD_VK_DATETIME,    /* §8.5 */
    MD_VK_COMPOSED     /* §14 mantissa/exponent */
} md_vkind_t;

typedef struct md_rational {
    int64_t num;
    int64_t den; /* den == 0 → unset */
} md_rational_t;

typedef struct md_point_desc {
    md_str_t id;
    md_bytes_t point_raw;  /* the Point message; iterators re-walk it */

    uint8_t space;         /* effective (mapping's, else the block's) */
    uint8_t storage;       /* md_storage_t */
    uint8_t access;        /* md_access_t; 0 → treated as read-only */
    uint8_t vkind;         /* md_vkind_t */

    uint16_t offset;
    uint16_t model_relative_offset;
    uint8_t length_words;
    bool byte_big;         /* §9.1: big-endian unless LITTLE_ENDIAN */
    bool word_big;         /* §9.2 */

    md_rational_t scale;      /* §10.2 */
    md_rational_t offset_add;

    bool has_scale_ref;       /* §10.4 */
    md_str_t sref_id;
    uint8_t sref_mode;        /* 2 = MULTIPLY, else POW10 */
    int64_t sref_den;

    bool has_selector;        /* §10.5; cases iterated from the wire */
    md_str_t sel_id;
    md_bytes_t sel_raw;       /* SelectorRef message */

    bool has_na;              /* §8.4; entries iterated from point_raw */

    uint8_t str_padding;      /* §15 wire Padding (0 = unset → none) */
    uint8_t str_termination;  /* wire Termination (0 = unset → fixed) */
    uint8_t dt_encoding;      /* §8.5 wire DateTimeEncoding (0 → epoch_s) */

    bool has_composed;        /* §14 */
    int64_t comp_base;
    uint16_t comp_moff, comp_eoff;
    uint8_t comp_mwords, comp_ewords;

    bool has_constraints;     /* §11.4 */
    md_rational_t w_min, w_max, w_step;
    md_bytes_t w_allowed;     /* packed int64 payload; may be empty */

    bool in_discovery_block;  /* resolve offset via the block's discovery */
    int8_t discovery_slot;
} md_point_desc_t;

/* Expand a point view into a descriptor. */
md_err_t md_point_parse(const md_point_t *pt, md_point_desc_t *out);

/* Register count to read/write (mapping length or storage default). */
uint16_t md_point_words(const md_point_desc_t *d);

bool md_point_readable(const md_point_desc_t *d);
bool md_point_writable(const md_point_desc_t *d);

/* --- on-demand iterators over the wire ranges ------------------------------ */

typedef struct md_kv_iter {
    md_wire_t w;
    uint32_t field;
} md_kv_iter_t;

/* §8.4 sentinels: (raw, meaning) pairs. */
md_kv_iter_t md_na_iter(const md_point_desc_t *d);
bool md_na_next(md_kv_iter_t *it, int64_t *raw, md_str_t *meaning);

/* §13.2 flags: (bit, name) pairs, wire order. */
md_kv_iter_t md_flag_iter(const md_point_desc_t *d);
bool md_flag_next(md_kv_iter_t *it, uint32_t *bit, md_str_t *name);

/* §13 packed sub-fields: legacy bit_fields first, then fields. */
typedef struct md_field_iter {
    md_wire_t w;
    uint8_t pass; /* 0 = bit_fields (60), 1 = fields (61) */
    md_bytes_t msg;
} md_field_iter_t;

md_field_iter_t md_field_iter(const md_point_desc_t *d);
bool md_field_next(md_field_iter_t *it, md_str_t *field_id, uint32_t *bit_offset,
                   uint32_t *bit_length);

/* §10.5 selector cases: (key, scale, offset). */
md_kv_iter_t md_case_iter(const md_point_desc_t *d);
bool md_case_next(md_kv_iter_t *it, int64_t *key, md_rational_t *scale,
                  md_rational_t *offset);

/* §11.4 allowed_values (packed int64). */
bool md_allowed_next(md_wire_t *w, int64_t *v);

#endif /* MODDEF_DESC_H */
