#include "moddef/doc.h"

#include "moddef/wire.h"

/* Field numbers, spec §27 (draft-stable; guarded by fixture conformance). */
#define F_DOC_DOC_ID 1
#define F_DOC_DEVICES 20
#define F_DEV_DEVICE_ID 1
#define F_DEV_BLOCKS 10
#define F_BLK_BLOCK_ID 1
#define F_BLK_SPACE 3
#define F_BLK_DISCOVERY 7
#define F_BLK_POINTS 10
#define F_PT_POINT_ID 1

md_err_t md_doc_init(md_doc_t *doc, const void *buf, size_t len)
{
    doc->raw.p = (const uint8_t *)buf;
    doc->raw.len = (uint32_t)len;
    /* Sanity: the outer message must walk cleanly end to end. */
    md_wire_t w = md_wire(doc->raw);
    uint32_t f;
    uint8_t t;
    while (md_wire_tag(&w, &f, &t)) {
        if (!md_wire_skip(&w, t))
            return MD_ERR_PARSE;
    }
    return md_wire_done(&w) ? MD_OK : MD_ERR_PARSE;
}

md_str_t md_doc_id(const md_doc_t *doc)
{
    md_bytes_t b;
    if (md_wire_find_len(doc->raw, F_DOC_DOC_ID, &b))
        return md_wire_str(b);
    return (md_str_t){0, 0};
}

md_err_t md_doc_device(const md_doc_t *doc, md_str_t device_id, md_devprof_t *out)
{
    md_wire_t w = md_wire(doc->raw);
    uint32_t f;
    uint8_t t;
    while (md_wire_tag(&w, &f, &t)) {
        if (f == F_DOC_DEVICES && t == 2) {
            md_bytes_t prof;
            if (!md_wire_len(&w, &prof))
                return MD_ERR_PARSE;
            md_bytes_t idb = {0, 0};
            (void)md_wire_find_len(prof, F_DEV_DEVICE_ID, &idb);
            md_str_t id = md_wire_str(idb);
            if (device_id.len == 0 || md_str_eq(id, device_id)) {
                out->raw = prof;
                out->device_id = id;
                return MD_OK;
            }
        } else if (!md_wire_skip(&w, t)) {
            return MD_ERR_PARSE;
        }
    }
    return MD_ERR_NOT_FOUND;
}

static void block_envelope(md_bytes_t raw, int8_t slot_counter, md_block_t *out)
{
    out->raw = raw;
    out->block_id = (md_str_t){0, 0};
    out->space = 0;
    out->has_discovery = false;
    out->discovery = (md_bytes_t){0, 0};
    out->discovery_slot = -1;

    md_bytes_t b;
    uint64_t v;
    if (md_wire_find_len(raw, F_BLK_BLOCK_ID, &b))
        out->block_id = md_wire_str(b);
    if (md_wire_find_varint(raw, F_BLK_SPACE, &v))
        out->space = (uint8_t)v;
    if (md_wire_find_len(raw, F_BLK_DISCOVERY, &b)) {
        out->has_discovery = true;
        out->discovery = b;
        out->discovery_slot = slot_counter;
    }
}

md_point_iter_t md_point_iter(const md_devprof_t *prof)
{
    md_point_iter_t it;
    it.prof = prof->raw;
    it.block_pos = prof->raw.p;
    it.point_pos = 0;
    it.next_slot = 0;
    it.block.raw = (md_bytes_t){0, 0};
    return it;
}

/* Advance to the next block submessage; false when exhausted. */
static bool next_block(md_point_iter_t *it)
{
    md_wire_t w = {it->block_pos, it->prof.p + it->prof.len};
    uint32_t f;
    uint8_t t;
    while (md_wire_tag(&w, &f, &t)) {
        if (f == F_DEV_BLOCKS && t == 2) {
            md_bytes_t blk;
            if (!md_wire_len(&w, &blk))
                return false;
            it->block_pos = w.p;
            block_envelope(blk, it->next_slot, &it->block);
            if (it->block.has_discovery)
                it->next_slot++;
            it->point_pos = blk.p;
            return true;
        }
        if (!md_wire_skip(&w, t))
            return false;
    }
    return false;
}

bool md_point_next(md_point_iter_t *it, md_point_t *out)
{
    for (;;) {
        if (it->point_pos) {
            md_wire_t w = {it->point_pos, it->block.raw.p + it->block.raw.len};
            uint32_t f;
            uint8_t t;
            while (md_wire_tag(&w, &f, &t)) {
                if (f == F_BLK_POINTS && t == 2) {
                    md_bytes_t pt;
                    if (!md_wire_len(&w, &pt))
                        return false;
                    it->point_pos = w.p;
                    out->raw = pt;
                    out->block = it->block;
                    return true;
                }
                if (!md_wire_skip(&w, t))
                    return false;
            }
            it->point_pos = 0; /* block exhausted */
        }
        if (!next_block(it))
            return false;
    }
}

md_err_t md_devprof_find_point(const md_devprof_t *prof, md_str_t point_id, md_point_t *out)
{
    md_point_iter_t it = md_point_iter(prof);
    md_point_t pt;
    while (md_point_next(&it, &pt)) {
        if (md_str_eq(md_point_id(&pt), point_id)) {
            *out = pt;
            return MD_OK;
        }
    }
    return MD_ERR_NOT_FOUND;
}

md_str_t md_point_id(const md_point_t *pt)
{
    md_bytes_t b;
    if (md_wire_find_len(pt->raw, F_PT_POINT_ID, &b))
        return md_wire_str(b);
    return (md_str_t){0, 0};
}
