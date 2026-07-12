// SPDX-License-Identifier: Apache-2.0

/* Device driver layer (spec §32.4): binds a transport to one device profile
 * inside a parsed document view for point reads/writes.
 *
 * SunSpec model_relative_offset resolves against the model *ID register*
 * (offset 0 = model id, 1 = length, data at 2+) per spec §7.3 — the same
 * convention as every other ModDef implementation. Resolved bases are
 * cached per discovery block in fixed slots (MD_MAX_DISCOVERY, default 4;
 * override with -DMD_MAX_DISCOVERY=n).
 *
 * Register windows read into a fixed stack buffer of MD_MAX_POINT_WORDS
 * registers (default 64 — larger than any registry point; strings cap at
 * MD_MAX_POINT_WORDS words too). */
#ifndef MODDEF_DEVICE_H
#define MODDEF_DEVICE_H

#include "moddef/codec.h"
#include "moddef/doc.h"
#include "moddef/transport.h"

#ifndef MD_MAX_DISCOVERY
#define MD_MAX_DISCOVERY 4
#endif

#ifndef MD_MAX_POINT_WORDS
#define MD_MAX_POINT_WORDS 64
#endif

typedef struct md_dev {
    md_devprof_t prof;
    const md_transport_t *t;
    uint16_t model_base[MD_MAX_DISCOVERY];
    uint8_t base_valid; /* bitmask over slots */
} md_dev_t;

/* Bind a transport to the named profile in doc (empty id = first). */
md_err_t md_dev_init(md_dev_t *dev, const md_doc_t *doc, md_str_t device_id,
                     const md_transport_t *t);

/* Read and decode one point by id (numeric/bool/flags/fields/datetime). */
md_err_t md_dev_read(md_dev_t *dev, md_str_t point_id, md_value_t *out);

/* Read a string point into buf (NUL-terminated). */
md_err_t md_dev_read_string(md_dev_t *dev, md_str_t point_id, char *buf, size_t cap,
                            size_t *out_len);

/* Encode and write a value; validates access mode and §11.4 constraints. */
md_err_t md_dev_write(md_dev_t *dev, md_str_t point_id, const md_value_t *v);

static inline md_err_t md_dev_write_f64(md_dev_t *dev, md_str_t point_id, double f)
{
    md_value_t v = md_value_f64(f);
    return md_dev_write(dev, point_id, &v);
}

static inline md_err_t md_dev_write_i64(md_dev_t *dev, md_str_t point_id, int64_t i)
{
    md_value_t v = md_value_i64(i);
    return md_dev_write(dev, point_id, &v);
}

/* Pre-parsed descriptor variants: skip the by-id document walk when you
 * hold a descriptor already (generated code, hot loops). */
md_err_t md_dev_read_desc(md_dev_t *dev, const md_point_desc_t *d, md_value_t *out);
md_err_t md_dev_write_desc(md_dev_t *dev, const md_point_desc_t *d, const md_value_t *v);
md_err_t md_dev_read_string_desc(md_dev_t *dev, const md_point_desc_t *d, char *buf,
                                 size_t cap, size_t *out_len);

#endif /* MODDEF_DEVICE_H */
