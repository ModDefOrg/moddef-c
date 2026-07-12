// SPDX-License-Identifier: Apache-2.0

/* Zero-copy views over a binary `.moddef` document (spec §4, §27).
 *
 * The document is never copied or materialized: md_doc_t wraps the
 * serialized bytes wherever they live (memory-mapped flash, a RAM buffer,
 * an array baked into the firmware) and navigation walks the protobuf
 * frames. YAML/JSON parsing is host-side only — convert with any other
 * ModDef implementation or the CLI. */
#ifndef MODDEF_DOC_H
#define MODDEF_DOC_H

#include <stddef.h>

#include "moddef/err.h"
#include "moddef/str.h"

/* A whole ModDefDocument. */
typedef struct md_doc {
    md_bytes_t raw;
} md_doc_t;

/* A DeviceProfile submessage. */
typedef struct md_devprof {
    md_bytes_t raw;
    md_str_t device_id;
} md_devprof_t;

/* A RegisterBlock submessage with its decoded envelope fields. */
typedef struct md_block {
    md_bytes_t raw;
    md_str_t block_id;
    uint8_t space;          /* md_space_t */
    bool has_discovery;
    md_bytes_t discovery;   /* Discovery submessage when has_discovery */
    int8_t discovery_slot;  /* index among the profile's discovery blocks, -1 */
} md_block_t;

/* A Point submessage plus its owning block. */
typedef struct md_point {
    md_bytes_t raw;
    md_block_t block;
} md_point_t;

/* Wrap a buffer; verifies the outer frame walks cleanly. */
md_err_t md_doc_init(md_doc_t *doc, const void *buf, size_t len);

md_str_t md_doc_id(const md_doc_t *doc);

/* Find a device profile by id; an empty id selects the first profile. */
md_err_t md_doc_device(const md_doc_t *doc, md_str_t device_id, md_devprof_t *out);

/* Find a point by id across the profile's blocks. */
md_err_t md_devprof_find_point(const md_devprof_t *prof, md_str_t point_id, md_point_t *out);

/* Iterate all points of a profile (catalog walks, generators):
 *   md_point_iter_t it = md_point_iter(prof);
 *   md_point_t pt;
 *   while (md_point_next(&it, &pt)) { ... }                                  */
typedef struct md_point_iter {
    md_bytes_t prof;
    const uint8_t *block_pos;   /* cursor in the profile message */
    md_block_t block;
    const uint8_t *point_pos;   /* cursor in the current block, NULL = advance */
    int8_t next_slot;
} md_point_iter_t;

md_point_iter_t md_point_iter(const md_devprof_t *prof);
bool md_point_next(md_point_iter_t *it, md_point_t *out);

/* Point envelope accessors used by callers (id lives in the wire). */
md_str_t md_point_id(const md_point_t *pt);

#endif /* MODDEF_DOC_H */
