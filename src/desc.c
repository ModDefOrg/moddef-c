// SPDX-License-Identifier: Apache-2.0

#include "moddef/desc.h"

/* Field numbers, spec §27. */
#define F_PT_POINT_ID 1
#define F_PT_ACCESS 10
#define F_PT_STORAGE 11
#define F_PT_VALUE_TYPE 12
#define F_PT_MAPPING 20
#define F_PT_TRANSFORM 30
#define F_PT_BIT_FIELDS 60
#define F_PT_FIELDS 61
#define F_PT_NA_VALUES 62
#define F_PT_DATETIME 63
#define F_PT_SELECTOR 66
#define F_PT_WRITE 80

#define F_MAP_SPACE 1
#define F_MAP_OFFSET 2
#define F_MAP_LENGTH_WORDS 3
#define F_MAP_BYTE_ORDER 5
#define F_MAP_WORD_ORDER 6
#define F_MAP_MODEL_REL 9
#define F_MAP_COMPOSED 10
#define F_MAP_STRING_ENC 12
#define F_MAP_STORAGE_TYPE 13 /* §14 sub-mapping storage (signedness/width) */
#define F_MAP_BIT_OFFSET 14   /* §14.2 bit window */
#define F_MAP_BIT_LENGTH 15
#define F_MAP_LENGTH_REF 16   /* §11.7.1 dynamic read length (PointRef) */

#define F_PREF_POINT_ID 1

#define F_COMP_MANTISSA 1
#define F_COMP_EXPONENT 2
#define F_COMP_BASE 3

#define F_TR_SCALE 1
#define F_TR_OFFSET 2
#define F_TR_SCALE_REF 4

#define F_RAT_NUM 1
#define F_RAT_DEN 2

#define F_SREF_POINT_ID 1
#define F_SREF_MODE 2
#define F_SREF_DEN 3

#define F_SEL_POINT_ID 1
#define F_SEL_CASES 2
#define F_CASE_SCALE 1
#define F_CASE_OFFSET 2

#define F_VT_PRIMITIVE 1
#define F_VT_FLAGS 5
#define F_FLAGSET_BITS 1

#define F_SE_PADDING 2
#define F_SE_TERMINATION 3

#define F_DT_ENCODING 1

#define F_NA_RAW 1
#define F_NA_MEANING 2

#define F_BF_FIELD_ID 1
#define F_BF_BIT_OFFSET 3
#define F_BF_BIT_LENGTH 4

#define F_WR_CONSTRAINTS 4
#define F_WC_MIN 1
#define F_WC_MAX 2
#define F_WC_STEP 3
#define F_WC_ALLOWED 4

/* Wire primitive values (types.proto). */
#define PRIM_BOOL 1
#define PRIM_INT32 2
#define PRIM_UINT32 3
#define PRIM_INT64 4
#define PRIM_UINT64 5
#define PRIM_FLOAT32 6
#define PRIM_FLOAT64 7
#define PRIM_DECIMAL 8
#define PRIM_DATETIME 11

#define BYTE_ORDER_LITTLE 2
#define WORD_ORDER_LITTLE 2

static md_rational_t parse_rational(md_bytes_t msg)
{
    md_rational_t r = {0, 0};
    uint64_t v;
    if (md_wire_find_varint(msg, F_RAT_NUM, &v))
        r.num = md_wire_i64(v);
    if (md_wire_find_varint(msg, F_RAT_DEN, &v))
        r.den = md_wire_i64(v);
    return r;
}

static bool has_field(md_bytes_t msg, uint32_t field)
{
    md_wire_t w = md_wire(msg);
    uint32_t f;
    uint8_t t;
    while (md_wire_tag(&w, &f, &t)) {
        if (f == field)
            return true;
        if (!md_wire_skip(&w, t))
            return false;
    }
    return false;
}

/* Sub-mapping of a composed value: offset, words, storage, and the §14.2
 * bit window (embedded decade exponent). */
static void composed_sub(md_bytes_t comp, uint32_t field, md_composed_sub_t *out)
{
    memset(out, 0, sizeof *out);
    out->words = 1;
    md_bytes_t sub;
    if (!md_wire_find_len(comp, field, &sub))
        return;
    uint64_t v;
    if (md_wire_find_varint(sub, F_MAP_OFFSET, &v))
        out->offset = (uint16_t)v;
    if (md_wire_find_varint(sub, F_MAP_LENGTH_WORDS, &v) && v > 0)
        out->words = (uint8_t)v;
    if (md_wire_find_varint(sub, F_MAP_STORAGE_TYPE, &v))
        out->storage = (uint8_t)v;
    if (md_wire_find_varint(sub, F_MAP_BIT_OFFSET, &v))
        out->bit_offset = (uint8_t)v;
    if (md_wire_find_varint(sub, F_MAP_BIT_LENGTH, &v))
        out->bit_length = (uint8_t)v;
}

md_err_t md_point_parse(const md_point_t *pt, md_point_desc_t *out)
{
    memset(out, 0, sizeof *out);
    out->point_raw = pt->raw;
    out->byte_big = true;
    out->word_big = true;
    out->space = pt->block.space;
    out->in_discovery_block = pt->block.has_discovery;
    out->discovery_slot = pt->block.discovery_slot;

    md_bytes_t b;
    uint64_t v;

    if (md_wire_find_len(pt->raw, F_PT_POINT_ID, &b))
        out->id = md_wire_str(b);
    if (md_wire_find_varint(pt->raw, F_PT_ACCESS, &v))
        out->access = (uint8_t)v;
    if (md_wire_find_varint(pt->raw, F_PT_STORAGE, &v))
        out->storage = (uint8_t)v;

    /* Mapping (§7, §9, §14, §15). */
    md_bytes_t mapping = {0, 0};
    bool has_composed_mapping = false;
    if (md_wire_find_len(pt->raw, F_PT_MAPPING, &mapping)) {
        if (md_wire_find_varint(mapping, F_MAP_SPACE, &v) && v != 0)
            out->space = (uint8_t)v;
        if (md_wire_find_varint(mapping, F_MAP_OFFSET, &v))
            out->offset = (uint16_t)v;
        if (md_wire_find_varint(mapping, F_MAP_LENGTH_WORDS, &v))
            out->length_words = (uint16_t)v;
        md_bytes_t lref;
        if (md_wire_find_len(mapping, F_MAP_LENGTH_REF, &lref)) {
            out->has_length_ref = true;
            if (md_wire_find_len(lref, F_PREF_POINT_ID, &b))
                out->lref_id = md_wire_str(b);
        }
        if (md_wire_find_varint(mapping, F_MAP_MODEL_REL, &v))
            out->model_relative_offset = (uint16_t)v;
        if (md_wire_find_varint(mapping, F_MAP_BYTE_ORDER, &v) && v == BYTE_ORDER_LITTLE)
            out->byte_big = false;
        if (md_wire_find_varint(mapping, F_MAP_WORD_ORDER, &v) && v == WORD_ORDER_LITTLE)
            out->word_big = false;

        md_bytes_t comp;
        if (md_wire_find_len(mapping, F_MAP_COMPOSED, &comp)) {
            has_composed_mapping = true;
            out->has_composed = true;
            if (md_wire_find_varint(comp, F_COMP_BASE, &v))
                out->comp_base = md_wire_i64(v);
            composed_sub(comp, F_COMP_MANTISSA, &out->comp_m);
            composed_sub(comp, F_COMP_EXPONENT, &out->comp_e);
        }

        md_bytes_t senc;
        if (md_wire_find_len(mapping, F_MAP_STRING_ENC, &senc)) {
            if (md_wire_find_varint(senc, F_SE_PADDING, &v))
                out->str_padding = (uint8_t)v;
            if (md_wire_find_varint(senc, F_SE_TERMINATION, &v))
                out->str_termination = (uint8_t)v;
        }
    }

    /* Transform (§10). */
    md_bytes_t tr;
    if (md_wire_find_len(pt->raw, F_PT_TRANSFORM, &tr)) {
        if (md_wire_find_len(tr, F_TR_SCALE, &b))
            out->scale = parse_rational(b);
        if (md_wire_find_len(tr, F_TR_OFFSET, &b))
            out->offset_add = parse_rational(b);
        md_bytes_t sref;
        if (md_wire_find_len(tr, F_TR_SCALE_REF, &sref)) {
            out->has_scale_ref = true;
            if (md_wire_find_len(sref, F_SREF_POINT_ID, &b))
                out->sref_id = md_wire_str(b);
            if (md_wire_find_varint(sref, F_SREF_MODE, &v))
                out->sref_mode = (uint8_t)v;
            if (md_wire_find_varint(sref, F_SREF_DEN, &v))
                out->sref_den = md_wire_i64(v);
        }
    }

    /* Selector (§10.5). */
    if (md_wire_find_len(pt->raw, F_PT_SELECTOR, &out->sel_raw)) {
        out->has_selector = true;
        if (md_wire_find_len(out->sel_raw, F_SEL_POINT_ID, &b))
            out->sel_id = md_wire_str(b);
    }

    out->has_na = has_field(pt->raw, F_PT_NA_VALUES);

    /* DateTime spec (§8.5). */
    md_bytes_t dt;
    if (md_wire_find_len(pt->raw, F_PT_DATETIME, &dt)) {
        if (md_wire_find_varint(dt, F_DT_ENCODING, &v))
            out->dt_encoding = (uint8_t)v;
    }

    /* Write constraints (§11.4). */
    md_bytes_t wr;
    if (md_wire_find_len(pt->raw, F_PT_WRITE, &wr)) {
        md_bytes_t wc;
        if (md_wire_find_len(wr, F_WR_CONSTRAINTS, &wc)) {
            out->has_constraints = true;
            if (md_wire_find_len(wc, F_WC_MIN, &b))
                out->w_min = parse_rational(b);
            if (md_wire_find_len(wc, F_WC_MAX, &b))
                out->w_max = parse_rational(b);
            if (md_wire_find_len(wc, F_WC_STEP, &b))
                out->w_step = parse_rational(b);
            (void)md_wire_find_len(wc, F_WC_ALLOWED, &out->w_allowed);
        }
    }

    /* Value kind: composed > flags > fields > string/bytes > primitive. */
    md_bytes_t vt = {0, 0};
    (void)md_wire_find_len(pt->raw, F_PT_VALUE_TYPE, &vt);

    if (out->storage == MD_ST_COMPOSED || has_composed_mapping) {
        out->vkind = MD_VK_COMPOSED;
        out->has_composed = true;
        return MD_OK;
    }
    if (vt.len && has_field(vt, F_VT_FLAGS)) {
        out->vkind = MD_VK_FLAGS;
        return MD_OK;
    }
    if (has_field(pt->raw, F_PT_FIELDS) || has_field(pt->raw, F_PT_BIT_FIELDS)) {
        out->vkind = MD_VK_FIELDS;
        return MD_OK;
    }
    if (out->storage == MD_ST_STRING_ASCII || out->storage == MD_ST_STRING_UTF8) {
        out->vkind = MD_VK_STRING;
        return MD_OK;
    }
    if (out->storage == MD_ST_BYTES_RAW) {
        out->vkind = MD_VK_BYTES;
        return MD_OK;
    }

    uint64_t prim = 0;
    if (vt.len)
        (void)md_wire_find_varint(vt, F_VT_PRIMITIVE, &prim);
    switch (prim) {
    case PRIM_BOOL:
        out->vkind = MD_VK_BOOL;
        break;
    case PRIM_DATETIME:
        out->vkind = MD_VK_DATETIME;
        break;
    case PRIM_DECIMAL:
    case PRIM_FLOAT32:
    case PRIM_FLOAT64:
        out->vkind = MD_VK_DECIMAL;
        break;
    case PRIM_UINT32:
    case PRIM_UINT64:
        out->vkind = MD_VK_UINT;
        break;
    case PRIM_INT32:
    case PRIM_INT64:
        out->vkind = MD_VK_INT;
        break;
    default:
        /* enum_ref / struct_ref / no primitive: raw enum integer. */
        out->vkind = MD_VK_ENUM;
        break;
    }
    return MD_OK;
}

uint16_t md_point_words(const md_point_desc_t *d)
{
    if (d->length_words)
        return d->length_words;
    switch (d->storage) {
    case MD_ST_U32:
    case MD_ST_S32:
    case MD_ST_F32:
    case MD_ST_U24:
        return 2;
    case MD_ST_U48:
    case MD_ST_S48:
        return 3;
    case MD_ST_U64:
    case MD_ST_S64:
    case MD_ST_F64:
        return 4;
    default:
        return 1;
    }
}

bool md_point_readable(const md_point_desc_t *d)
{
    return d->access == MD_ACCESS_RO || d->access == MD_ACCESS_RW || d->access == 0;
}

bool md_point_writable(const md_point_desc_t *d)
{
    return d->access == MD_ACCESS_RW || d->access == MD_ACCESS_WO || d->access == MD_ACCESS_CMD;
}

/* --- iterators ---------------------------------------------------------- */

static md_kv_iter_t kv_iter(md_bytes_t msg, uint32_t field)
{
    md_kv_iter_t it;
    it.w = md_wire(msg);
    it.field = field;
    return it;
}

/* Yield the next submessage of `it.field`; false when exhausted. */
static bool kv_next_msg(md_kv_iter_t *it, md_bytes_t *out)
{
    uint32_t f;
    uint8_t t;
    while (md_wire_tag(&it->w, &f, &t)) {
        if (f == it->field && t == 2)
            return md_wire_len(&it->w, out);
        if (!md_wire_skip(&it->w, t))
            return false;
    }
    return false;
}

md_kv_iter_t md_na_iter(const md_point_desc_t *d)
{
    return kv_iter(d->point_raw, F_PT_NA_VALUES);
}

bool md_na_next(md_kv_iter_t *it, int64_t *raw, md_str_t *meaning)
{
    md_bytes_t msg;
    if (!kv_next_msg(it, &msg))
        return false;
    uint64_t v = 0;
    *raw = md_wire_find_varint(msg, F_NA_RAW, &v) ? md_wire_i64(v) : 0;
    md_bytes_t m = {0, 0};
    (void)md_wire_find_len(msg, F_NA_MEANING, &m);
    *meaning = md_wire_str(m);
    return true;
}

md_kv_iter_t md_flag_iter(const md_point_desc_t *d)
{
    /* value_type(12).flags(5).bits(1): map entries {key=1, value=2}. */
    md_bytes_t vt, fl;
    if (md_wire_find_len(d->point_raw, F_PT_VALUE_TYPE, &vt) &&
        md_wire_find_len(vt, F_VT_FLAGS, &fl))
        return kv_iter(fl, F_FLAGSET_BITS);
    return kv_iter((md_bytes_t){0, 0}, F_FLAGSET_BITS);
}

bool md_flag_next(md_kv_iter_t *it, uint32_t *bit, md_str_t *name)
{
    md_bytes_t entry;
    if (!kv_next_msg(it, &entry))
        return false;
    uint64_t k = 0;
    (void)md_wire_find_varint(entry, 1, &k);
    *bit = (uint32_t)k;
    md_bytes_t n = {0, 0};
    (void)md_wire_find_len(entry, 2, &n);
    *name = md_wire_str(n);
    return true;
}

md_field_iter_t md_field_iter(const md_point_desc_t *d)
{
    md_field_iter_t it;
    it.msg = d->point_raw;
    it.w = md_wire(d->point_raw);
    it.pass = 0;
    return it;
}

bool md_field_next(md_field_iter_t *it, md_str_t *field_id, uint32_t *bit_offset,
                   uint32_t *bit_length)
{
    for (;;) {
        uint32_t want = it->pass == 0 ? F_PT_BIT_FIELDS : F_PT_FIELDS;
        uint32_t f;
        uint8_t t;
        while (md_wire_tag(&it->w, &f, &t)) {
            if (f == want && t == 2) {
                md_bytes_t msg;
                if (!md_wire_len(&it->w, &msg))
                    return false;
                md_bytes_t idb = {0, 0};
                (void)md_wire_find_len(msg, F_BF_FIELD_ID, &idb);
                *field_id = md_wire_str(idb);
                uint64_t v = 0;
                *bit_offset = md_wire_find_varint(msg, F_BF_BIT_OFFSET, &v) ? (uint32_t)v : 0;
                v = 0;
                *bit_length = md_wire_find_varint(msg, F_BF_BIT_LENGTH, &v) ? (uint32_t)v : 0;
                return true;
            }
            if (!md_wire_skip(&it->w, t))
                return false;
        }
        if (it->pass == 1)
            return false;
        it->pass = 1;
        it->w = md_wire(it->msg);
    }
}

md_kv_iter_t md_case_iter(const md_point_desc_t *d)
{
    if (d->has_selector)
        return kv_iter(d->sel_raw, F_SEL_CASES);
    return kv_iter((md_bytes_t){0, 0}, F_SEL_CASES);
}

bool md_case_next(md_kv_iter_t *it, int64_t *key, md_rational_t *scale,
                  md_rational_t *offset)
{
    md_bytes_t entry;
    if (!kv_next_msg(it, &entry))
        return false;
    uint64_t k = 0;
    (void)md_wire_find_varint(entry, 1, &k); /* map key */
    *key = md_wire_i64(k);
    scale->num = scale->den = 0;
    offset->num = offset->den = 0;
    md_bytes_t val;
    if (md_wire_find_len(entry, 2, &val)) { /* map value: SelectorCase */
        md_bytes_t r;
        if (md_wire_find_len(val, F_CASE_SCALE, &r))
            *scale = parse_rational(r);
        if (md_wire_find_len(val, F_CASE_OFFSET, &r))
            *offset = parse_rational(r);
    }
    return true;
}

bool md_allowed_next(md_wire_t *w, int64_t *v)
{
    uint64_t raw;
    if (md_wire_done(w) || !md_wire_varint(w, &raw))
        return false;
    *v = md_wire_i64(raw);
    return true;
}
