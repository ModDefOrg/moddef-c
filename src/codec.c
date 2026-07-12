// SPDX-License-Identifier: Apache-2.0

#include "moddef/codec.h"

#include <math.h>
#include <string.h>

/* Wire enum values used here. */
#define SCALE_MODE_MULTIPLY 2
#define PADDING_NULL 1
#define PADDING_SPACE 2
#define TERMINATION_NULL 2

/* --- register/byte assembly (§9) ------------------------------------------- */

/* Byte at logical big-endian position i of the normalized stream. */
static uint8_t byte_at(const uint16_t *regs, uint16_t n, uint32_t i, bool byte_big,
                       bool word_big)
{
    uint32_t w = i / 2;
    uint16_t reg = regs[word_big ? w : n - 1 - w];
    bool hi = byte_big ? (i % 2 == 0) : (i % 2 == 1);
    return hi ? (uint8_t)(reg >> 8) : (uint8_t)reg;
}

/* Last 8 normalized bytes as a big-endian u64. */
static uint64_t assemble_u64(const uint16_t *regs, uint16_t n, bool byte_big,
                             bool word_big)
{
    uint32_t total = (uint32_t)n * 2;
    uint32_t start = total > 8 ? total - 8 : 0;
    uint64_t v = 0;
    for (uint32_t i = start; i < total; i++)
        v = (v << 8) | byte_at(regs, n, i, byte_big, word_big);
    return v;
}

static uint64_t mask_for(uint8_t bits)
{
    return bits >= 64 ? UINT64_MAX : ((uint64_t)1 << bits) - 1;
}

static int64_t sign_extend(uint64_t v, uint8_t bits)
{
    if (bits >= 64)
        return (int64_t)v;
    uint64_t sign = (uint64_t)1 << (bits - 1);
    if (v & sign)
        return (int64_t)(v | ~mask_for(bits));
    return (int64_t)v;
}

static uint8_t storage_bits(uint8_t st, uint16_t words)
{
    switch (st) {
    case MD_ST_BIT:
        return 1;
    case MD_ST_U16:
    case MD_ST_S16:
        return 16;
    case MD_ST_U24:
        return 24;
    case MD_ST_U32:
    case MD_ST_S32:
        return 32;
    case MD_ST_U48:
    case MD_ST_S48:
        return 48;
    case MD_ST_U64:
    case MD_ST_S64:
        return 64;
    default:
        if (words == 0)
            return 16;
        return words * 16 > 64 ? 64 : (uint8_t)(words * 16);
    }
}

static bool is_signed(uint8_t st)
{
    return st == MD_ST_S16 || st == MD_ST_S32 || st == MD_ST_S48 || st == MD_ST_S64;
}

/* Split a raw integer back into registers (inverse of assemble_u64). */
static void words_from_u64(uint64_t raw, uint16_t *out, uint16_t n, bool byte_big,
                           bool word_big)
{
    memset(out, 0, (size_t)n * 2);
    uint32_t total = (uint32_t)n * 2;
    for (uint32_t i = total; i-- > 0;) {
        uint8_t b = (uint8_t)raw;
        raw >>= 8;
        uint32_t w = i / 2;
        uint32_t reg = word_big ? w : n - 1 - w;
        bool hi = byte_big ? (i % 2 == 0) : (i % 2 == 1);
        out[reg] |= hi ? (uint16_t)b << 8 : b;
    }
}

/* --- transform pipeline (§10) ----------------------------------------------- */

static double pow10d(int64_t exp)
{
    double r = 1.0;
    int64_t e = exp < 0 ? -exp : exp;
    if (e > 308)
        e = 308;
    while (e--)
        r *= 10.0;
    return exp < 0 ? 1.0 / r : r;
}

static bool ctx_get(const md_ctx_t *ctx, md_str_t id, int64_t *out)
{
    if (!ctx)
        return false;
    for (uint8_t i = 0; i < ctx->n; i++) {
        if (md_str_eq(ctx->refs[i].id, id)) {
            *out = ctx->refs[i].val;
            return true;
        }
    }
    return false;
}

static md_err_t apply_scale(const md_point_desc_t *d, int64_t raw_int,
                            const md_ctx_t *ctx, double *out)
{
    double r = (double)raw_int;

    /* §10.5: a matching selector case replaces the point's own transform;
     * unresolved selector or unmatched case falls through. */
    if (d->has_selector) {
        int64_t key;
        if (ctx_get(ctx, d->sel_id, &key)) {
            md_kv_iter_t it = md_case_iter(d);
            int64_t k;
            md_rational_t cs, co;
            while (md_case_next(&it, &k, &cs, &co)) {
                if (k != key)
                    continue;
                if (cs.den != 0)
                    r = r * (double)cs.num / (double)cs.den;
                if (co.den != 0)
                    r += (double)co.num / (double)co.den;
                *out = r;
                return MD_OK;
            }
        }
    }

    if (d->has_scale_ref) {
        int64_t sf;
        if (!ctx_get(ctx, d->sref_id, &sf))
            return MD_ERR_UNRESOLVED_REF;
        if (d->sref_mode == SCALE_MODE_MULTIPLY) {
            int64_t den = d->sref_den ? d->sref_den : 1;
            r = r * (double)sf / (double)den;
        } else {
            r *= pow10d(sf);
        }
    } else if (d->scale.den != 0 || d->scale.num != 0) {
        if (d->scale.den == 0)
            return MD_ERR_ZERO_SCALE_DEN;
        r = r * (double)d->scale.num / (double)d->scale.den;
    }

    if (d->offset_add.den != 0)
        r += (double)d->offset_add.num / (double)d->offset_add.den;
    *out = r;
    return MD_OK;
}

static double apply_float_scale(const md_point_desc_t *d, double f)
{
    if (d->scale.den != 0)
        f = f * (double)d->scale.num / (double)d->scale.den;
    if (d->offset_add.den != 0)
        f += (double)d->offset_add.num / (double)d->offset_add.den;
    return f;
}

/* Signed integer from a sub-window (§14 composed mantissa/exponent). */
static int64_t sub_int(const md_point_desc_t *d, const uint16_t *regs, uint16_t n,
                       uint16_t off, uint8_t words)
{
    if (words == 0)
        words = 1;
    if ((uint32_t)off + words > n)
        return 0;
    uint64_t raw = assemble_u64(regs + off, words, d->byte_big, d->word_big);
    uint8_t bits = (uint8_t)(words * 16 > 64 ? 64 : words * 16);
    return sign_extend(raw & mask_for(bits), bits);
}

static int64_t bcd_to_int(const uint16_t *regs, uint16_t n, bool byte_big, bool word_big)
{
    int64_t v = 0;
    for (uint32_t i = 0; i < (uint32_t)n * 2; i++) {
        uint8_t b = byte_at(regs, n, i, byte_big, word_big);
        v = v * 100 + (b >> 4) * 10 + (b & 0x0f);
    }
    return v;
}

/* --- decode ------------------------------------------------------------------ */

md_err_t md_decode(const md_point_desc_t *d, const uint16_t *regs, uint16_t n_regs,
                   const md_ctx_t *ctx, md_value_t *out)
{
    uint16_t words = md_point_words(d);
    if (n_regs < words)
        return MD_ERR_SHORT_READ;
    n_regs = words;
    memset(out, 0, sizeof *out);

    /* §14 composed mantissa/exponent over the window. */
    if (d->vkind == MD_VK_COMPOSED) {
        if (d->comp_base == 0)
            return MD_ERR_COMPOSED_BASE;
        int64_t mant = sub_int(d, regs, n_regs, d->comp_moff, d->comp_mwords);
        int64_t exp = sub_int(d, regs, n_regs, d->comp_eoff, d->comp_ewords);
        double r = (double)mant;
        int64_t e = exp < 0 ? -exp : exp;
        if (e > 64)
            e = 64;
        while (e--)
            r = exp >= 0 ? r * (double)d->comp_base : r / (double)d->comp_base;
        out->kind = MD_VAL_F64;
        out->v.f64 = r;
        return MD_OK;
    }

    /* IEEE754 floats decode straight from the normalized byte stream. */
    if (d->storage == MD_ST_F32) {
        uint32_t raw = (uint32_t)assemble_u64(regs, n_regs, d->byte_big, d->word_big);
        float f;
        memcpy(&f, &raw, 4);
        out->kind = MD_VAL_F64;
        out->v.f64 = apply_float_scale(d, (double)f);
        return MD_OK;
    }
    if (d->storage == MD_ST_F64) {
        uint64_t raw = assemble_u64(regs, n_regs, d->byte_big, d->word_big);
        double f;
        memcpy(&f, &raw, 8);
        out->kind = MD_VAL_F64;
        out->v.f64 = apply_float_scale(d, f);
        return MD_OK;
    }

    /* Integer-backed value. */
    uint8_t bits = storage_bits(d->storage, n_regs);
    uint64_t raw = assemble_u64(regs, n_regs, d->byte_big, d->word_big) & mask_for(bits);

    /* §8.4 sentinel check on the masked raw integer. */
    if (d->has_na) {
        md_kv_iter_t it = md_na_iter(d);
        int64_t na_raw;
        md_str_t meaning;
        while (md_na_next(&it, &na_raw, &meaning)) {
            if (((uint64_t)na_raw & mask_for(bits)) == raw) {
                out->kind = MD_VAL_UNAVAILABLE;
                out->na_meaning = meaning;
                return MD_OK;
            }
        }
    }

    /* §13.2 flags / §13 fields surface the raw window. */
    if (d->vkind == MD_VK_FLAGS) {
        out->kind = MD_VAL_FLAGS;
        out->v.bits = raw;
        return MD_OK;
    }
    if (d->vkind == MD_VK_FIELDS) {
        out->kind = MD_VAL_FIELDS;
        out->v.bits = raw;
        return MD_OK;
    }

    if (d->storage == MD_ST_BCD) {
        out->kind = MD_VAL_I64;
        out->v.i64 = bcd_to_int(regs, n_regs, d->byte_big, d->word_big);
        return MD_OK;
    }

    bool sgn = is_signed(d->storage);
    int64_t raw_int = sgn ? sign_extend(raw, bits) : (int64_t)raw;

    switch (d->vkind) {
    case MD_VK_BOOL:
        out->kind = MD_VAL_BOOL;
        out->v.b = raw != 0;
        return MD_OK;
    case MD_VK_DATETIME:
        out->kind = MD_VAL_DATETIME;
        out->v.epoch = (int64_t)raw;
        return MD_OK;
    case MD_VK_DECIMAL: {
        out->kind = MD_VAL_F64;
        return apply_scale(d, raw_int, ctx, &out->v.f64);
    }
    case MD_VK_UINT:
        out->kind = MD_VAL_U64;
        out->v.u64 = raw;
        return MD_OK;
    case MD_VK_INT:
        out->kind = MD_VAL_I64;
        out->v.i64 = raw_int;
        return MD_OK;
    default:
        /* Enum-backed and anything else integer-shaped: raw integer,
         * signed if the storage is (parity with the other ports). */
        if (sgn) {
            out->kind = MD_VAL_I64;
            out->v.i64 = raw_int;
        } else {
            out->kind = MD_VAL_U64;
            out->v.u64 = raw;
        }
        return MD_OK;
    }
}

md_err_t md_decode_raw(const md_point_desc_t *d, const uint16_t *regs, uint16_t n_regs,
                       uint64_t *raw, uint8_t *bits)
{
    uint16_t words = md_point_words(d);
    if (n_regs < words)
        return MD_ERR_SHORT_READ;
    *bits = storage_bits(d->storage, words);
    *raw = assemble_u64(regs, words, d->byte_big, d->word_big) & mask_for(*bits);
    return MD_OK;
}

md_err_t md_decode_string(const md_point_desc_t *d, const uint16_t *regs,
                          uint16_t n_regs, char *buf, size_t cap, size_t *out_len)
{
    uint16_t words = md_point_words(d);
    if (n_regs < words)
        return MD_ERR_SHORT_READ;
    size_t total = (size_t)words * 2;
    if (cap < total + 1)
        return MD_ERR_BUFFER;
    for (uint32_t i = 0; i < total; i++)
        buf[i] = (char)byte_at(regs, words, i, d->byte_big, d->word_big);

    size_t end = total;
    if (d->str_termination == TERMINATION_NULL) {
        for (size_t i = 0; i < end; i++) {
            if (buf[i] == '\0') {
                end = i;
                break;
            }
        }
    }
    char pad = 0;
    bool has_pad = false;
    if (d->str_padding == PADDING_NULL) {
        pad = '\0';
        has_pad = true;
    } else if (d->str_padding == PADDING_SPACE) {
        pad = ' ';
        has_pad = true;
    }
    if (has_pad) {
        while (end > 0 && buf[end - 1] == pad)
            end--;
    }
    buf[end] = '\0';
    if (out_len)
        *out_len = end;
    return MD_OK;
}

md_err_t md_decode_bytes(const md_point_desc_t *d, const uint16_t *regs,
                         uint16_t n_regs, uint8_t *buf, size_t cap, size_t *out_len)
{
    uint16_t words = md_point_words(d);
    if (n_regs < words)
        return MD_ERR_SHORT_READ;
    size_t total = (size_t)words * 2;
    if (cap < total)
        return MD_ERR_BUFFER;
    for (uint32_t i = 0; i < total; i++)
        buf[i] = byte_at(regs, words, i, d->byte_big, d->word_big);
    if (out_len)
        *out_len = total;
    return MD_OK;
}

/* --- encode ------------------------------------------------------------------ */

static int64_t round_half_away(double v)
{
    return v >= 0.0 ? (int64_t)floor(v + 0.5) : (int64_t)ceil(v - 0.5);
}

static bool value_as_f64(const md_value_t *v, double *out)
{
    switch (v->kind) {
    case MD_VAL_F64:
        *out = v->v.f64;
        return true;
    case MD_VAL_U64:
        *out = (double)v->v.u64;
        return true;
    case MD_VAL_I64:
        *out = (double)v->v.i64;
        return true;
    case MD_VAL_BOOL:
        *out = v->v.b ? 1.0 : 0.0;
        return true;
    default:
        return false;
    }
}

static bool value_as_i64(const md_value_t *v, int64_t *out)
{
    switch (v->kind) {
    case MD_VAL_I64:
        *out = v->v.i64;
        return true;
    case MD_VAL_U64:
        *out = (int64_t)v->v.u64;
        return true;
    case MD_VAL_BOOL:
        *out = v->v.b ? 1 : 0;
        return true;
    case MD_VAL_F64:
        *out = (int64_t)v->v.f64;
        return true;
    case MD_VAL_DATETIME:
        *out = v->v.epoch;
        return true;
    case MD_VAL_FLAGS:
    case MD_VAL_FIELDS:
        *out = (int64_t)v->v.bits;
        return true;
    default:
        return false;
    }
}

/* Inverse transform pipeline: raw = (value - offset) / scale. */
static md_err_t encode_scaled(const md_point_desc_t *d, double val, const md_ctx_t *ctx,
                              int64_t *out)
{
    if (d->offset_add.den != 0)
        val -= (double)d->offset_add.num / (double)d->offset_add.den;
    if (d->has_scale_ref) {
        int64_t sf;
        if (!ctx_get(ctx, d->sref_id, &sf))
            return MD_ERR_UNRESOLVED_REF;
        if (d->sref_mode == SCALE_MODE_MULTIPLY) {
            int64_t den = d->sref_den ? d->sref_den : 1;
            val = val * (double)den / (double)sf;
        } else {
            val /= pow10d(sf);
        }
    } else if (d->scale.den != 0 || d->scale.num != 0) {
        if (d->scale.den == 0)
            return MD_ERR_ZERO_SCALE_DEN;
        val = val * (double)d->scale.den / (double)d->scale.num;
    }
    *out = round_half_away(val);
    return MD_OK;
}

static uint64_t int_to_bcd(int64_t v)
{
    uint64_t raw = 0;
    unsigned shift = 0;
    while (v > 0) {
        raw |= (uint64_t)(v % 10) << shift;
        shift += 4;
        v /= 10;
    }
    return raw;
}

md_err_t md_encode(const md_point_desc_t *d, const md_value_t *v, const md_ctx_t *ctx,
                   uint16_t *out_regs, uint16_t n_regs)
{
    uint16_t words = md_point_words(d);
    if (n_regs < words)
        return MD_ERR_BUFFER;

    if (d->vkind == MD_VK_COMPOSED)
        return MD_ERR_UNSUPPORTED;
    if (d->vkind == MD_VK_STRING || d->vkind == MD_VK_BYTES)
        return MD_ERR_WRONG_TYPE; /* use md_encode_string */
    if (d->vkind == MD_VK_FIELDS)
        return MD_ERR_UNSUPPORTED; /* packed windows are read-oriented */

    if (d->storage == MD_ST_F32) {
        double f;
        if (!value_as_f64(v, &f))
            return MD_ERR_WRONG_TYPE;
        float f32 = (float)f;
        uint32_t bits32;
        memcpy(&bits32, &f32, 4);
        words_from_u64(bits32, out_regs, words, d->byte_big, d->word_big);
        return MD_OK;
    }
    if (d->storage == MD_ST_F64) {
        double f;
        if (!value_as_f64(v, &f))
            return MD_ERR_WRONG_TYPE;
        uint64_t bits64;
        memcpy(&bits64, &f, 8);
        words_from_u64(bits64, out_regs, words, d->byte_big, d->word_big);
        return MD_OK;
    }

    uint8_t bits = storage_bits(d->storage, words);
    uint64_t raw;

    switch (d->vkind) {
    case MD_VK_FLAGS:
        if (v->kind != MD_VAL_FLAGS)
            return MD_ERR_WRONG_TYPE;
        raw = v->v.bits & mask_for(bits);
        break;
    case MD_VK_BOOL: {
        int64_t i;
        if (!value_as_i64(v, &i))
            return MD_ERR_WRONG_TYPE;
        raw = i != 0;
        break;
    }
    case MD_VK_DATETIME: {
        int64_t i;
        if (!value_as_i64(v, &i))
            return MD_ERR_WRONG_TYPE;
        raw = (uint64_t)i & mask_for(bits);
        break;
    }
    case MD_VK_DECIMAL: {
        double f;
        if (!value_as_f64(v, &f))
            return MD_ERR_WRONG_TYPE;
        int64_t i;
        md_err_t err = encode_scaled(d, f, ctx, &i);
        if (err)
            return err;
        raw = (uint64_t)i & mask_for(bits);
        break;
    }
    default: {
        int64_t i;
        if (!value_as_i64(v, &i))
            return MD_ERR_WRONG_TYPE;
        raw = (uint64_t)i & mask_for(bits);
        break;
    }
    }

    if (d->storage == MD_ST_BCD)
        raw = int_to_bcd((int64_t)raw);

    words_from_u64(raw, out_regs, words, d->byte_big, d->word_big);
    return MD_OK;
}

md_err_t md_encode_string(const md_point_desc_t *d, md_str_t s, uint16_t *out_regs,
                          uint16_t n_regs)
{
    uint16_t words = md_point_words(d);
    if (n_regs < words)
        return MD_ERR_BUFFER;
    memset(out_regs, 0, (size_t)words * 2);
    uint32_t total = (uint32_t)words * 2;
    for (uint32_t i = 0; i < total; i++) {
        uint8_t b = i < s.len ? (uint8_t)s.p[i] : 0;
        uint32_t w = i / 2;
        uint32_t reg = d->word_big ? w : (uint32_t)words - 1 - w;
        bool hi = d->byte_big ? (i % 2 == 0) : (i % 2 == 1);
        out_regs[reg] |= hi ? (uint16_t)b << 8 : b;
    }
    return MD_OK;
}

/* --- §11.4 write constraints --------------------------------------------------- */

md_err_t md_validate_write(const md_point_desc_t *d, const md_value_t *v)
{
    if (!d->has_constraints)
        return MD_OK;

    double num;
    bool numeric = value_as_f64(v, &num);

    if (d->w_allowed.len) {
        if (!numeric)
            return MD_ERR_CONSTRAINT_ALLOWED;
        int64_t iv = round_half_away(num);
        md_wire_t w = md_wire(d->w_allowed);
        int64_t a;
        while (md_allowed_next(&w, &a)) {
            if (a == iv)
                return MD_OK;
        }
        return MD_ERR_CONSTRAINT_ALLOWED;
    }
    if (!numeric)
        return MD_OK;

    double base = 0.0;
    if (d->w_min.den != 0) {
        double lo = (double)d->w_min.num / (double)d->w_min.den;
        base = lo;
        if (num < lo)
            return MD_ERR_CONSTRAINT_MIN;
    }
    if (d->w_max.den != 0) {
        double hi = (double)d->w_max.num / (double)d->w_max.den;
        if (num > hi)
            return MD_ERR_CONSTRAINT_MAX;
    }
    if (d->w_step.den != 0 && d->w_step.num != 0) {
        double step = (double)d->w_step.num / (double)d->w_step.den;
        double k = (num - base) / step;
        if (fabs(k - (double)round_half_away(k)) > 1e-9)
            return MD_ERR_CONSTRAINT_STEP;
    }
    return MD_OK;
}

/* --- flag / field helpers ------------------------------------------------------- */

bool md_flags_has(const md_point_desc_t *d, uint64_t mask, md_str_t name)
{
    md_kv_iter_t it = md_flag_iter(d);
    uint32_t bit;
    md_str_t n;
    while (md_flag_next(&it, &bit, &n)) {
        if (md_str_eq(n, name))
            return bit < 64 && (mask & ((uint64_t)1 << bit)) != 0;
    }
    return false;
}

md_err_t md_field_value(const md_point_desc_t *d, uint64_t window, md_str_t field_id,
                        uint64_t *out)
{
    md_field_iter_t it = md_field_iter(d);
    md_str_t id;
    uint32_t off, len;
    while (md_field_next(&it, &id, &off, &len)) {
        if (md_str_eq(id, field_id)) {
            uint64_t m = len >= 64 ? UINT64_MAX : ((uint64_t)1 << len) - 1;
            *out = (window >> off) & m;
            return MD_OK;
        }
    }
    return MD_ERR_NOT_FOUND;
}

const char *md_err_str(md_err_t err)
{
    switch (err) {
    case MD_OK:
        return "MD_OK";
    case MD_ERR_TRANSPORT:
        return "MD_ERR_TRANSPORT";
    case MD_ERR_PARSE:
        return "MD_ERR_PARSE";
    case MD_ERR_NOT_FOUND:
        return "MD_ERR_NOT_FOUND";
    case MD_ERR_SHORT_READ:
        return "MD_ERR_SHORT_READ";
    case MD_ERR_BUFFER:
        return "MD_ERR_BUFFER";
    case MD_ERR_UNRESOLVED_REF:
        return "MD_ERR_UNRESOLVED_REF";
    case MD_ERR_ZERO_SCALE_DEN:
        return "MD_ERR_ZERO_SCALE_DEN";
    case MD_ERR_COMPOSED_BASE:
        return "MD_ERR_COMPOSED_BASE";
    case MD_ERR_NOT_WRITABLE:
        return "MD_ERR_NOT_WRITABLE";
    case MD_ERR_WRONG_TYPE:
        return "MD_ERR_WRONG_TYPE";
    case MD_ERR_UNSUPPORTED:
        return "MD_ERR_UNSUPPORTED";
    case MD_ERR_CONSTRAINT_MIN:
        return "MD_ERR_CONSTRAINT_MIN";
    case MD_ERR_CONSTRAINT_MAX:
        return "MD_ERR_CONSTRAINT_MAX";
    case MD_ERR_CONSTRAINT_STEP:
        return "MD_ERR_CONSTRAINT_STEP";
    case MD_ERR_CONSTRAINT_ALLOWED:
        return "MD_ERR_CONSTRAINT_ALLOWED";
    case MD_ERR_DISCOVERY:
        return "MD_ERR_DISCOVERY";
    case MD_ERR_UNAVAILABLE:
        return "MD_ERR_UNAVAILABLE";
    }
    return "MD_ERR_?";
}
