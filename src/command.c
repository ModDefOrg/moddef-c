// SPDX-License-Identifier: Apache-2.0

#include "moddef/command.h"

#include <string.h>

/* Field numbers, spec §27 (§11.7 command messages). */
#define F_DEV_COMMANDS 30

#define F_CMD_ID 1
#define F_CMD_PARAMS 10
#define F_CMD_STEPS 20
#define F_CMD_RESULTS 30

#define F_CP_FIELD 1
#define F_CP_STORAGE 2
#define F_CP_MAPPING 4
#define F_CP_REQUIRED 5

#define F_STEP_WRITE 2
#define F_STEP_POLL 3
#define F_STEP_READ 4

#define F_WS_PARAM 1
#define F_WS_TRIGGER 2

#define F_TW_POINT_ID 1
#define F_TW_VALUE 2

#define F_PS_POINT_ID 1
#define F_PS_UNTIL 2
#define F_PS_INTERVAL 3
#define F_PS_TIMEOUT 4

#define F_RS_POINT_ID 1
#define F_RS_INTO 2

#define F_CR_FIELD 1
#define F_CR_FROM 2

#define F_COND_OP 1
#define F_COND_VALUE 2
#define F_COND_MASK 3
#define F_COND_MIN 4
#define F_COND_MAX 5

#define COND_EQ 1
#define COND_NE 2
#define COND_MASK 3
#define COND_RANGE 4

/* Mapping submessage fields (mirrors desc.c's private view). */
#define F_MAP_SPACE 1
#define F_MAP_OFFSET 2
#define F_MAP_LENGTH_WORDS 3
#define F_MAP_BYTE_ORDER 5
#define F_MAP_WORD_ORDER 6
#define BYTE_ORDER_LITTLE 2
#define WORD_ORDER_LITTLE 2

/* Yield the next `field` submessage from a wire cursor. */
static bool next_msg(md_wire_t *w, uint32_t field, md_bytes_t *out)
{
    uint32_t f;
    uint8_t t;
    while (md_wire_tag(w, &f, &t)) {
        if (f == field && t == 2)
            return md_wire_len(w, out);
        if (!md_wire_skip(w, t))
            return false;
    }
    return false;
}

static const md_cmd_param_t *find_param_value(const md_cmd_params_t *params, md_str_t field)
{
    for (uint8_t i = 0; params && i < params->n; i++) {
        if (md_str_eq(params->items[i].field, field))
            return &params->items[i];
    }
    return NULL;
}

/* Locate the CommandParam message named `field` in the command. */
static bool find_param_msg(md_bytes_t cmd, md_str_t field, md_bytes_t *out)
{
    md_wire_t w = md_wire(cmd);
    md_bytes_t p;
    while (next_msg(&w, F_CMD_PARAMS, &p)) {
        md_bytes_t name;
        if (md_wire_find_len(p, F_CP_FIELD, &name) && md_str_eq(md_wire_str(name), field)) {
            *out = p;
            return true;
        }
    }
    return false;
}

/* Minimal descriptor for a CommandParam: storage + mapping envelope. The
 * §11.7 write path needs no transform/value-kind machinery — params encode
 * raw (numeric via MD_VK_INT, strings/bytes via the byte packers). */
static void param_desc(md_bytes_t param_msg, md_point_desc_t *d)
{
    memset(d, 0, sizeof *d);
    d->byte_big = true;
    d->word_big = true;
    d->space = MD_SPACE_HOLDING;
    d->access = MD_ACCESS_WO;
    d->vkind = MD_VK_INT;

    uint64_t v;
    md_bytes_t b;
    if (md_wire_find_len(param_msg, F_CP_FIELD, &b))
        d->id = md_wire_str(b);
    if (md_wire_find_varint(param_msg, F_CP_STORAGE, &v))
        d->storage = (uint8_t)v;

    md_bytes_t mapping;
    if (md_wire_find_len(param_msg, F_CP_MAPPING, &mapping)) {
        if (md_wire_find_varint(mapping, F_MAP_SPACE, &v) && v != 0)
            d->space = (uint8_t)v;
        if (md_wire_find_varint(mapping, F_MAP_OFFSET, &v))
            d->offset = (uint16_t)v;
        if (md_wire_find_varint(mapping, F_MAP_LENGTH_WORDS, &v))
            d->length_words = (uint16_t)v;
        if (md_wire_find_varint(mapping, F_MAP_BYTE_ORDER, &v) && v == BYTE_ORDER_LITTLE)
            d->byte_big = false;
        if (md_wire_find_varint(mapping, F_MAP_WORD_ORDER, &v) && v == WORD_ORDER_LITTLE)
            d->word_big = false;
    }
}

/* Write a zero-padded byte payload over the declared window in ≤123-word
 * chunks. Chunking assumes word-big order (checked by the caller). */
static md_err_t write_bytes_chunked(md_dev_t *dev, uint8_t space, uint16_t off,
                                    uint16_t total_words, const uint8_t *data,
                                    uint32_t data_len, bool byte_big)
{
    uint16_t chunk[MD_CMD_MAX_WRITE_WORDS];
    uint16_t done = 0;
    while (done < total_words) {
        uint16_t n = (uint16_t)(total_words - done);
        if (n > MD_CMD_MAX_WRITE_WORDS)
            n = MD_CMD_MAX_WRITE_WORDS;
        for (uint16_t i = 0; i < n; i++) {
            uint32_t at = ((uint32_t)done + i) * 2;
            uint8_t b0 = at < data_len ? data[at] : 0;
            uint8_t b1 = at + 1 < data_len ? data[at + 1] : 0;
            chunk[i] = byte_big ? (uint16_t)((uint16_t)b0 << 8 | b1)
                                : (uint16_t)((uint16_t)b1 << 8 | b0);
        }
        md_err_t err = md_dev_write_raw(dev, space, (uint16_t)(off + done), n, chunk);
        if (err)
            return err;
        done = (uint16_t)(done + n);
    }
    return MD_OK;
}

static md_err_t fail(md_cmd_exec_t *ex, md_err_t err)
{
    ex->err = err;
    ex->status = MD_CMD_ERROR;
    return err;
}

static md_err_t do_write_param(md_cmd_exec_t *ex, md_str_t field)
{
    md_bytes_t pmsg;
    if (!find_param_msg(ex->cmd_raw, field, &pmsg))
        return MD_ERR_STEP_REF;
    const md_cmd_param_t *pv = find_param_value(&ex->params, field);
    if (!pv)
        return MD_OK; /* optional param not supplied — skip its write */

    md_point_desc_t d;
    param_desc(pmsg, &d);
    uint16_t words = md_point_words(&d);

    if (pv->kind == MD_CMD_PARAM_STR || pv->kind == MD_CMD_PARAM_BYTES) {
        if (!d.word_big && words > MD_CMD_MAX_WRITE_WORDS)
            return MD_ERR_UNSUPPORTED; /* word-little cannot chunk */
        const uint8_t *data =
            pv->kind == MD_CMD_PARAM_STR ? (const uint8_t *)pv->str.p : pv->bytes.p;
        uint32_t len = pv->kind == MD_CMD_PARAM_STR ? pv->str.len : pv->bytes.len;
        if (!d.word_big) {
            /* Small word-little window: pack fully, single write. */
            uint16_t regs[MD_CMD_MAX_WRITE_WORDS];
            md_str_t s = {(const char *)data, (uint16_t)len};
            md_err_t err = md_encode_string(&d, s, regs, words);
            if (err)
                return err;
            return md_dev_write_raw(ex->dev, d.space, d.offset, words, regs);
        }
        return write_bytes_chunked(ex->dev, d.space, d.offset, words, data, len,
                                   d.byte_big);
    }

    uint16_t regs[8]; /* numeric params are at most 4 words */
    if (words > 8)
        return MD_ERR_UNSUPPORTED;
    md_err_t err = md_encode(&d, &pv->value, &MD_CTX_EMPTY, regs, words);
    if (err)
        return err;
    return md_dev_write_raw(ex->dev, d.space, d.offset, words, regs);
}

static md_err_t point_desc_by_id(md_cmd_exec_t *ex, md_str_t point_id,
                                 md_point_desc_t *d)
{
    md_point_t pt;
    md_err_t err = md_devprof_find_point(&ex->dev->prof, point_id, &pt);
    if (err)
        return MD_ERR_STEP_REF;
    return md_point_parse(&pt, d);
}

static md_err_t do_write_trigger(md_cmd_exec_t *ex, md_bytes_t tr)
{
    md_bytes_t b;
    if (!md_wire_find_len(tr, F_TW_POINT_ID, &b))
        return MD_ERR_STEP_REF;
    uint64_t v = 0;
    (void)md_wire_find_varint(tr, F_TW_VALUE, &v);

    md_point_desc_t d;
    md_err_t err = point_desc_by_id(ex, md_wire_str(b), &d);
    if (err)
        return err;

    /* Trigger values are raw register values (§11.7): encode via the
     * storage/mapping envelope only, bypassing transform/value_type. */
    md_point_desc_t raw = d;
    raw.vkind = MD_VK_INT;
    raw.scale.den = 0;
    raw.offset_add.den = 0;
    raw.has_scale_ref = false;
    raw.has_selector = false;

    uint16_t words = md_point_words(&raw);
    uint16_t regs[8];
    if (words > 8)
        return MD_ERR_UNSUPPORTED;
    md_value_t val = md_value_i64(md_wire_i64(v));
    err = md_encode(&raw, &val, &MD_CTX_EMPTY, regs, words);
    if (err)
        return err;
    uint16_t off;
    err = md_dev_point_offset(ex->dev, &d, &off);
    if (err)
        return err;
    return md_dev_write_raw(ex->dev, d.space, off, words, regs);
}

static md_err_t do_write(md_cmd_exec_t *ex, md_bytes_t w)
{
    md_bytes_t b;
    if (md_wire_find_len(w, F_WS_PARAM, &b))
        return do_write_param(ex, md_wire_str(b));
    if (md_wire_find_len(w, F_WS_TRIGGER, &b))
        return do_write_trigger(ex, b);
    return MD_ERR_STEP_REF;
}

static md_err_t do_read(md_cmd_exec_t *ex, md_bytes_t r)
{
    md_bytes_t b;
    if (!md_wire_find_len(r, F_RS_POINT_ID, &b))
        return MD_ERR_STEP_REF;
    md_point_desc_t d;
    md_err_t err = point_desc_by_id(ex, md_wire_str(b), &d);
    if (err)
        return err;

    md_str_t into = {0, 0};
    md_bytes_t ib;
    if (md_wire_find_len(r, F_RS_INTO, &ib))
        into = md_wire_str(ib);
    if (into.len == 0)
        return MD_OK; /* unbound read: side effect only */
    if (ex->n_bindings >= MD_CMD_MAX_BINDINGS)
        return MD_ERR_BUFFER;
    md_cmd_binding_t *bind = &ex->bindings[ex->n_bindings];
    memset(bind, 0, sizeof *bind);
    bind->name = into;

    if (d.vkind == MD_VK_STRING || d.vkind == MD_VK_BYTES) {
        uint16_t words;
        err = md_dev_point_words(ex->dev, &d, &words);
        if (err)
            return err;
        if (!d.word_big && words > MD_MAX_POINT_WORDS)
            return MD_ERR_UNSUPPORTED; /* word-little cannot chunk */
        if ((uint32_t)words * 2 > ex->data_cap - ex->data_used)
            return MD_ERR_BUFFER;
        uint16_t off;
        err = md_dev_point_offset(ex->dev, &d, &off);
        if (err)
            return err;
        uint8_t *dst = ex->data + ex->data_used;
        uint16_t done = 0;
        uint16_t chunk[MD_MAX_POINT_WORDS];
        while (done < words) {
            uint16_t n = (uint16_t)(words - done);
            if (n > MD_MAX_POINT_WORDS)
                n = MD_MAX_POINT_WORDS;
            err = md_dev_read_raw(ex->dev, d.space, (uint16_t)(off + done), n, chunk);
            if (err)
                return err;
            for (uint16_t i = 0; i < n; i++) {
                uint16_t reg = d.word_big ? chunk[i] : chunk[n - 1 - i];
                uint32_t at = ((uint32_t)done + i) * 2;
                dst[at] = d.byte_big ? (uint8_t)(reg >> 8) : (uint8_t)reg;
                dst[at + 1] = d.byte_big ? (uint8_t)reg : (uint8_t)(reg >> 8);
            }
            done = (uint16_t)(done + n);
        }
        bind->is_data = true;
        bind->data_off = ex->data_used;
        bind->data_len = (uint32_t)words * 2;
        ex->data_used += bind->data_len;
    } else {
        err = md_dev_read_desc(ex->dev, &d, &bind->value);
        if (err)
            return err;
    }
    ex->n_bindings++;
    return MD_OK;
}

/* Evaluate a §11.7 poll exit condition against a raw integer. */
static bool cond_met(md_bytes_t cond, int64_t raw)
{
    uint64_t v;
    int64_t op = 0, val = 0, mask = 0, min = 0, max = 0;
    if (md_wire_find_varint(cond, F_COND_OP, &v))
        op = (int64_t)v;
    if (md_wire_find_varint(cond, F_COND_VALUE, &v))
        val = md_wire_i64(v);
    if (md_wire_find_varint(cond, F_COND_MASK, &v))
        mask = md_wire_i64(v);
    if (md_wire_find_varint(cond, F_COND_MIN, &v))
        min = md_wire_i64(v);
    if (md_wire_find_varint(cond, F_COND_MAX, &v))
        max = md_wire_i64(v);
    switch (op) {
    case COND_EQ:
        return raw == val;
    case COND_NE:
        return raw != val;
    case COND_MASK:
        return (raw & mask) == val;
    case COND_RANGE:
        return raw >= min && raw <= max;
    default:
        return false;
    }
}

/* One poll read: the raw (pre-transform) integer of the polled point. */
static md_err_t poll_read(md_cmd_exec_t *ex, md_bytes_t poll, int64_t *raw)
{
    md_bytes_t b;
    if (!md_wire_find_len(poll, F_PS_POINT_ID, &b))
        return MD_ERR_STEP_REF;
    md_point_desc_t d;
    md_err_t err = point_desc_by_id(ex, md_wire_str(b), &d);
    if (err)
        return err;
    md_point_desc_t rd = d;
    rd.vkind = MD_VK_ENUM; /* raw integer path, no transform */
    rd.scale.den = 0;
    rd.offset_add.den = 0;
    rd.has_scale_ref = false;
    rd.has_selector = false;
    rd.has_na = false;
    md_value_t v;
    err = md_dev_read_desc(ex->dev, &rd, &v);
    if (err)
        return err;
    switch (v.kind) {
    case MD_VAL_I64:
        *raw = v.v.i64;
        return MD_OK;
    case MD_VAL_U64:
        *raw = (int64_t)v.v.u64;
        return MD_OK;
    case MD_VAL_BOOL:
        *raw = v.v.b;
        return MD_OK;
    default:
        return MD_ERR_WRONG_TYPE;
    }
}

md_err_t md_cmd_begin(md_cmd_exec_t *ex, md_dev_t *dev, md_str_t command_id,
                      const md_cmd_params_t *params, uint8_t *data_buf,
                      size_t data_cap)
{
    memset(ex, 0, sizeof *ex);
    ex->dev = dev;
    if (params)
        ex->params = *params;
    ex->data = data_buf;
    ex->data_cap = (uint32_t)data_cap;

    /* Locate the Command message by id. */
    md_wire_t w = md_wire(dev->prof.raw);
    md_bytes_t cmd;
    bool found = false;
    while (next_msg(&w, F_DEV_COMMANDS, &cmd)) {
        md_bytes_t idb;
        if (md_wire_find_len(cmd, F_CMD_ID, &idb) &&
            md_str_eq(md_wire_str(idb), command_id)) {
            found = true;
            break;
        }
    }
    if (!found) {
        ex->status = MD_CMD_ERROR;
        ex->err = MD_ERR_COMMAND_NOT_FOUND;
        return ex->err;
    }
    ex->cmd_raw = cmd;

    /* Every required param must be supplied (MDE504's runtime mirror). */
    md_wire_t pw = md_wire(cmd);
    md_bytes_t pmsg;
    while (next_msg(&pw, F_CMD_PARAMS, &pmsg)) {
        uint64_t req = 0;
        (void)md_wire_find_varint(pmsg, F_CP_REQUIRED, &req);
        if (!req)
            continue;
        md_bytes_t name;
        if (!md_wire_find_len(pmsg, F_CP_FIELD, &name))
            continue;
        if (!find_param_value(&ex->params, md_wire_str(name))) {
            ex->status = MD_CMD_ERROR;
            ex->err = MD_ERR_PARAM_MISSING;
            return ex->err;
        }
    }

    ex->steps = md_wire(cmd);
    ex->status = MD_CMD_RUNNING;
    return MD_OK;
}

md_cmd_status_t md_cmd_tick(md_cmd_exec_t *ex, uint32_t now_ms)
{
    if (ex->status == MD_CMD_DONE || ex->status == MD_CMD_ERROR)
        return (md_cmd_status_t)ex->status;

    for (;;) {
        if (ex->in_poll) {
            if (!ex->poll_first && (int32_t)(now_ms - ex->poll_due_ms) < 0) {
                ex->status = MD_CMD_WAITING_POLL;
                return MD_CMD_WAITING_POLL;
            }
            md_bytes_t cond = {0, 0};
            (void)md_wire_find_len(ex->poll_raw, F_PS_UNTIL, &cond);
            int64_t raw;
            md_err_t err = poll_read(ex, ex->poll_raw, &raw);
            if (err) {
                fail(ex, err);
                return MD_CMD_ERROR;
            }
            if (cond_met(cond, raw)) {
                ex->in_poll = false;
                continue;
            }
            uint64_t timeout = 0, interval = 0;
            (void)md_wire_find_varint(ex->poll_raw, F_PS_TIMEOUT, &timeout);
            (void)md_wire_find_varint(ex->poll_raw, F_PS_INTERVAL, &interval);
            if (timeout > 0 && now_ms - ex->poll_started_ms >= (uint32_t)timeout) {
                fail(ex, MD_ERR_POLL_TIMEOUT);
                return MD_CMD_ERROR;
            }
            if (interval == 0)
                interval = MD_CMD_DEFAULT_INTERVAL_MS;
            ex->poll_due_ms = now_ms + (uint32_t)interval;
            ex->poll_first = false;
            ex->status = MD_CMD_WAITING_POLL;
            return MD_CMD_WAITING_POLL;
        }

        md_bytes_t step;
        if (!next_msg(&ex->steps, F_CMD_STEPS, &step)) {
            ex->status = MD_CMD_DONE;
            return MD_CMD_DONE;
        }

        md_bytes_t body;
        md_err_t err;
        if (md_wire_find_len(step, F_STEP_WRITE, &body)) {
            err = do_write(ex, body);
        } else if (md_wire_find_len(step, F_STEP_POLL, &body)) {
            ex->in_poll = true;
            ex->poll_first = true;
            ex->poll_raw = body;
            ex->poll_started_ms = now_ms;
            continue;
        } else if (md_wire_find_len(step, F_STEP_READ, &body)) {
            err = do_read(ex, body);
        } else {
            err = MD_ERR_STEP_REF;
        }
        if (err) {
            fail(ex, err);
            return MD_CMD_ERROR;
        }
    }
}

md_err_t md_cmd_error(const md_cmd_exec_t *ex)
{
    return ex->status == MD_CMD_ERROR ? ex->err : MD_OK;
}

static const md_cmd_binding_t *result_binding(const md_cmd_exec_t *ex, md_str_t field)
{
    md_wire_t w = md_wire(ex->cmd_raw);
    md_bytes_t res;
    while (next_msg(&w, F_CMD_RESULTS, &res)) {
        md_bytes_t b;
        if (!md_wire_find_len(res, F_CR_FIELD, &b) || !md_str_eq(md_wire_str(b), field))
            continue;
        md_str_t from = {0, 0};
        if (md_wire_find_len(res, F_CR_FROM, &b))
            from = md_wire_str(b);
        for (uint8_t i = 0; i < ex->n_bindings; i++) {
            if (md_str_eq(ex->bindings[i].name, from))
                return &ex->bindings[i];
        }
        return NULL;
    }
    return NULL;
}

md_err_t md_cmd_result(const md_cmd_exec_t *ex, md_str_t field, md_value_t *out)
{
    const md_cmd_binding_t *b = result_binding(ex, field);
    if (!b)
        return MD_ERR_NOT_FOUND;
    if (b->is_data)
        return MD_ERR_WRONG_TYPE; /* use md_cmd_result_data */
    *out = b->value;
    return MD_OK;
}

md_err_t md_cmd_result_data(const md_cmd_exec_t *ex, md_str_t field,
                            const uint8_t **p, size_t *len)
{
    const md_cmd_binding_t *b = result_binding(ex, field);
    if (!b)
        return MD_ERR_NOT_FOUND;
    if (!b->is_data)
        return MD_ERR_WRONG_TYPE; /* use md_cmd_result */
    *p = ex->data + b->data_off;
    *len = b->data_len;
    return MD_OK;
}
