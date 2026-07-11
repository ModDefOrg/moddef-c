#include "moddef/device.h"

/* Discovery message fields (device.proto §7.3). */
#define F_DISC_KIND 1
#define F_DISC_ANCHORS 2
#define F_DISC_MODEL_ID 3
#define DISCOVERY_SUNSPEC 1

/* "SunS" marker as two big-endian 16-bit words. */
#define SUNS_HI 0x5375
#define SUNS_LO 0x6e53

md_err_t md_dev_init(md_dev_t *dev, const md_doc_t *doc, md_str_t device_id,
                     const md_transport_t *t)
{
    dev->t = t;
    dev->base_valid = 0;
    return md_doc_device(doc, device_id, &dev->prof);
}

static md_err_t read_space(md_dev_t *dev, uint8_t space, uint16_t off, uint16_t n,
                           uint16_t *out)
{
    switch (space) {
    case MD_SPACE_HOLDING:
        return dev->t->read_holding(dev->t->ctx, off, n, out);
    case MD_SPACE_INPUT:
        return dev->t->read_input(dev->t->ctx, off, n, out);
    case MD_SPACE_COIL:
    case MD_SPACE_DISCRETE: {
        bool bit = false;
        md_err_t err = space == MD_SPACE_COIL
                           ? dev->t->read_coils(dev->t->ctx, off, 1, &bit)
                           : dev->t->read_discrete(dev->t->ctx, off, 1, &bit);
        if (err)
            return err;
        out[0] = bit ? 1 : 0;
        return MD_OK;
    }
    default:
        return MD_ERR_UNSUPPORTED;
    }
}

/* Probe SunSpec anchors, walk the (model_id, length) chain, and cache the
 * offset of the target model's ID register (spec §7.3). */
static md_err_t resolve_base(md_dev_t *dev, const md_point_desc_t *d, uint16_t *base)
{
    int8_t slot = d->discovery_slot;
    if (slot < 0 || slot >= MD_MAX_DISCOVERY)
        return MD_ERR_DISCOVERY;
    if (dev->base_valid & (uint8_t)(1u << slot)) {
        *base = dev->model_base[slot];
        return MD_OK;
    }

    /* Re-locate the owning block's Discovery message by slot. */
    md_bytes_t disc = {0, 0};
    uint8_t space = MD_SPACE_HOLDING;
    {
        md_point_iter_t it = md_point_iter(&dev->prof);
        md_point_t pt;
        while (md_point_next(&it, &pt)) {
            if (pt.block.discovery_slot == slot) {
                disc = pt.block.discovery;
                space = pt.block.space;
                break;
            }
        }
    }
    if (!disc.len)
        return MD_ERR_DISCOVERY;

    uint64_t v;
    if (!md_wire_find_varint(disc, F_DISC_KIND, &v) || v != DISCOVERY_SUNSPEC)
        return MD_ERR_DISCOVERY;
    uint64_t model_id = 0;
    (void)md_wire_find_varint(disc, F_DISC_MODEL_ID, &model_id);

    /* Anchor candidates: packed uint32 payload, defaults when absent. */
    static const uint16_t defaults[] = {40000, 50000, 0};
    md_bytes_t anchors_wire = {0, 0};
    (void)md_wire_find_len(disc, F_DISC_ANCHORS, &anchors_wire);

    uint16_t anchor = 0;
    bool found = false;
    uint16_t hdr[2];
    if (anchors_wire.len) {
        md_wire_t w = md_wire(anchors_wire);
        uint64_t c;
        while (!found && md_wire_varint(&w, &c)) {
            /* Devices answer exceptions off-anchor; try the next one. */
            if (read_space(dev, space, (uint16_t)c, 2, hdr) == MD_OK &&
                hdr[0] == SUNS_HI && hdr[1] == SUNS_LO) {
                anchor = (uint16_t)c;
                found = true;
            }
        }
    } else {
        for (unsigned i = 0; !found && i < 3; i++) {
            if (read_space(dev, space, defaults[i], 2, hdr) == MD_OK &&
                hdr[0] == SUNS_HI && hdr[1] == SUNS_LO) {
                anchor = defaults[i];
                found = true;
            }
        }
    }
    if (!found)
        return MD_ERR_DISCOVERY;

    /* Walk model headers starting just after the marker. */
    uint32_t off = (uint32_t)anchor + 2;
    for (unsigned i = 0; i < 256; i++) {
        md_err_t err = read_space(dev, space, (uint16_t)off, 2, hdr);
        if (err)
            return err;
        if (hdr[0] == 0xffff)
            break;
        if (hdr[0] == (uint16_t)model_id) {
            /* Base is the model ID register (model_relative_offset 0). */
            dev->model_base[slot] = (uint16_t)off;
            dev->base_valid |= (uint8_t)(1u << slot);
            *base = (uint16_t)off;
            return MD_OK;
        }
        off += 2u + hdr[1];
        if (off > 0xffff)
            return MD_ERR_DISCOVERY;
    }
    return MD_ERR_DISCOVERY;
}

static md_err_t point_offset(md_dev_t *dev, const md_point_desc_t *d, uint16_t *off)
{
    if (d->in_discovery_block) {
        uint16_t base;
        md_err_t err = resolve_base(dev, d, &base);
        if (err)
            return err;
        *off = (uint16_t)(base + d->model_relative_offset);
        return MD_OK;
    }
    *off = d->offset;
    return MD_OK;
}

static md_err_t read_registers(md_dev_t *dev, const md_point_desc_t *d, uint16_t *regs,
                               uint16_t *n)
{
    uint16_t words = md_point_words(d);
    if (words > MD_MAX_POINT_WORDS)
        return MD_ERR_BUFFER;
    uint16_t off;
    md_err_t err = point_offset(dev, d, &off);
    if (err)
        return err;
    err = read_space(dev, d->space, off, words, regs);
    if (err)
        return err;
    *n = words;
    return MD_OK;
}

/* Read the points referenced by d's scale_ref / selector_ref
 * (spec §10.4/§10.5), decoded to integers for the codec context. */
static md_err_t ref_context(md_dev_t *dev, const md_point_desc_t *d,
                            md_ctx_entry_t entries[2], md_ctx_t *ctx)
{
    md_str_t ids[2];
    uint8_t n = 0;
    if (d->has_scale_ref)
        ids[n++] = d->sref_id;
    if (d->has_selector)
        ids[n++] = d->sel_id;

    uint8_t filled = 0;
    for (uint8_t i = 0; i < n; i++) {
        md_point_t rp;
        md_err_t err = md_devprof_find_point(&dev->prof, ids[i], &rp);
        if (err)
            return err;
        md_point_desc_t rd;
        err = md_point_parse(&rp, &rd);
        if (err)
            return err;
        uint16_t regs[MD_MAX_POINT_WORDS];
        uint16_t nr;
        err = read_registers(dev, &rd, regs, &nr);
        if (err)
            return err;
        md_value_t val;
        err = md_decode(&rd, regs, nr, &MD_CTX_EMPTY, &val);
        if (err)
            return err;
        int64_t iv;
        switch (val.kind) {
        case MD_VAL_I64:
            iv = val.v.i64;
            break;
        case MD_VAL_U64:
            iv = (int64_t)val.v.u64;
            break;
        case MD_VAL_BOOL:
            iv = val.v.b;
            break;
        case MD_VAL_F64:
            iv = (int64_t)val.v.f64;
            break;
        default:
            continue; /* unavailable ref: leave unresolved */
        }
        entries[filled].id = ids[i];
        entries[filled].val = iv;
        filled++;
    }
    ctx->refs = entries;
    ctx->n = filled;
    return MD_OK;
}

md_err_t md_dev_read_desc(md_dev_t *dev, const md_point_desc_t *d, md_value_t *out)
{
    uint16_t regs[MD_MAX_POINT_WORDS];
    uint16_t n;
    md_err_t err = read_registers(dev, d, regs, &n);
    if (err)
        return err;
    md_ctx_entry_t entries[2];
    md_ctx_t ctx = MD_CTX_EMPTY;
    if (d->has_scale_ref || d->has_selector) {
        err = ref_context(dev, d, entries, &ctx);
        if (err)
            return err;
    }
    return md_decode(d, regs, n, &ctx, out);
}

md_err_t md_dev_read(md_dev_t *dev, md_str_t point_id, md_value_t *out)
{
    md_point_t pt;
    md_err_t err = md_devprof_find_point(&dev->prof, point_id, &pt);
    if (err)
        return err;
    md_point_desc_t d;
    err = md_point_parse(&pt, &d);
    if (err)
        return err;
    return md_dev_read_desc(dev, &d, out);
}

md_err_t md_dev_read_string_desc(md_dev_t *dev, const md_point_desc_t *d, char *buf,
                                 size_t cap, size_t *out_len)
{
    uint16_t regs[MD_MAX_POINT_WORDS];
    uint16_t n;
    md_err_t err = read_registers(dev, d, regs, &n);
    if (err)
        return err;
    return md_decode_string(d, regs, n, buf, cap, out_len);
}

md_err_t md_dev_read_string(md_dev_t *dev, md_str_t point_id, char *buf, size_t cap,
                            size_t *out_len)
{
    md_point_t pt;
    md_err_t err = md_devprof_find_point(&dev->prof, point_id, &pt);
    if (err)
        return err;
    md_point_desc_t d;
    err = md_point_parse(&pt, &d);
    if (err)
        return err;
    return md_dev_read_string_desc(dev, &d, buf, cap, out_len);
}

md_err_t md_dev_write_desc(md_dev_t *dev, const md_point_desc_t *d, const md_value_t *v)
{
    if (!md_point_writable(d))
        return MD_ERR_NOT_WRITABLE;
    md_err_t err = md_validate_write(d, v);
    if (err)
        return err;

    uint16_t off;
    err = point_offset(dev, d, &off);
    if (err)
        return err;

    if (d->space == MD_SPACE_COIL) {
        bool on = false;
        if (v->kind == MD_VAL_BOOL)
            on = v->v.b;
        else if (v->kind == MD_VAL_F64)
            on = v->v.f64 != 0.0;
        else if (v->kind == MD_VAL_I64)
            on = v->v.i64 != 0;
        else if (v->kind == MD_VAL_U64)
            on = v->v.u64 != 0;
        return dev->t->write_coil(dev->t->ctx, off, on);
    }
    if (d->space != MD_SPACE_HOLDING)
        return MD_ERR_UNSUPPORTED;

    md_ctx_entry_t entries[2];
    md_ctx_t ctx = MD_CTX_EMPTY;
    if (d->has_scale_ref || d->has_selector) {
        err = ref_context(dev, d, entries, &ctx);
        if (err)
            return err;
    }

    uint16_t words = md_point_words(d);
    if (words > MD_MAX_POINT_WORDS)
        return MD_ERR_BUFFER;
    uint16_t regs[MD_MAX_POINT_WORDS];
    err = md_encode(d, v, &ctx, regs, words);
    if (err)
        return err;
    return dev->t->write_holding(dev->t->ctx, off, words, regs);
}

md_err_t md_dev_write(md_dev_t *dev, md_str_t point_id, const md_value_t *v)
{
    md_point_t pt;
    md_err_t err = md_devprof_find_point(&dev->prof, point_id, &pt);
    if (err)
        return err;
    md_point_desc_t d;
    err = md_point_parse(&pt, &d);
    if (err)
        return err;
    return md_dev_write_desc(dev, &d, v);
}
