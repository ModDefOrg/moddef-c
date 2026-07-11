#include "moddef/wire.h"

bool md_wire_varint(md_wire_t *w, uint64_t *v)
{
    uint64_t out = 0;
    for (unsigned shift = 0; shift < 64; shift += 7) {
        if (w->p >= w->end)
            return false;
        uint8_t b = *w->p++;
        out |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) {
            *v = out;
            return true;
        }
    }
    return false; /* > 10 bytes: malformed */
}

bool md_wire_tag(md_wire_t *w, uint32_t *field, uint8_t *wtype)
{
    if (md_wire_done(w))
        return false;
    uint64_t key;
    if (!md_wire_varint(w, &key))
        return false;
    *field = (uint32_t)(key >> 3);
    *wtype = (uint8_t)(key & 7);
    return *field != 0;
}

bool md_wire_len(md_wire_t *w, md_bytes_t *out)
{
    uint64_t n;
    if (!md_wire_varint(w, &n))
        return false;
    if (n > (uint64_t)(w->end - w->p))
        return false;
    out->p = w->p;
    out->len = (uint32_t)n;
    w->p += n;
    return true;
}

bool md_wire_skip(md_wire_t *w, uint8_t wtype)
{
    uint64_t v;
    md_bytes_t b;
    switch (wtype) {
    case 0: /* varint */
        return md_wire_varint(w, &v);
    case 1: /* fixed64 */
        if (w->end - w->p < 8)
            return false;
        w->p += 8;
        return true;
    case 2: /* length-delimited */
        return md_wire_len(w, &b);
    case 5: /* fixed32 */
        if (w->end - w->p < 4)
            return false;
        w->p += 4;
        return true;
    default: /* groups (3/4) and unknown types are not used by ModDef */
        return false;
    }
}

bool md_wire_find_len(md_bytes_t msg, uint32_t field, md_bytes_t *out)
{
    md_wire_t w = md_wire(msg);
    uint32_t f;
    uint8_t t;
    while (md_wire_tag(&w, &f, &t)) {
        if (f == field && t == 2)
            return md_wire_len(&w, out);
        if (!md_wire_skip(&w, t))
            return false;
    }
    return false;
}

bool md_wire_find_varint(md_bytes_t msg, uint32_t field, uint64_t *out)
{
    md_wire_t w = md_wire(msg);
    uint32_t f;
    uint8_t t;
    while (md_wire_tag(&w, &f, &t)) {
        if (f == field && t == 0)
            return md_wire_varint(&w, out);
        if (!md_wire_skip(&w, t))
            return false;
    }
    return false;
}
