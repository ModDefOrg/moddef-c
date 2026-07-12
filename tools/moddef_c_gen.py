#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Generate a C module from a ModDef document (spec §31).

Because the C runtime parses binary documents directly, generation is
sugar, not architecture: the emitted pair embeds the serialized document
as a byte array plus, per device profile,

  - a point-index enum with the byte offsets precomputed (no name lookups
    at runtime),
  - C enums for the document's enum types (IntEnum-style),
  - `static inline` typed accessors per point (read_<id> / set_<id>),
  - an init helper binding the embedded document to a transport.

Usage: moddef_c_gen.py <doc.moddef[.yaml|.json]> -o <outdir>

Host-side only; uses moddef-py for parsing (sibling checkout or installed).
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent / "moddef-py" / "src"))

from moddef import schema  # noqa: E402
from moddef.document import parse_document, serialize_document  # noqa: E402

S = schema.StorageType
P = schema.PrimitiveType
A = schema.AccessMode


def c_ident(s: str) -> str:
    out = re.sub(r"[^A-Za-z0-9]", "_", s).lower().strip("_") or "x"
    if out[0].isdigit():
        out = "n" + out
    return out


def c_upper(s: str) -> str:
    return c_ident(s).upper()


class Names:
    def __init__(self) -> None:
        self.seen: set[str] = set()

    def claim(self, base: str) -> str:
        name = base
        i = 2
        while name in self.seen:
            name = f"{base}_{i}"
            i += 1
        self.seen.add(name)
        return name


def wire_ranges(blob: bytes) -> list[dict]:
    """Byte ranges of every Point submessage plus its block envelope,
    walking the same field numbers the C runtime uses (spec §27)."""

    def fields(buf: bytes, base: int):
        i = 0
        while i < len(buf):
            key, i = varint(buf, i)
            fnum, wtype = key >> 3, key & 7
            if wtype == 0:
                v, i = varint(buf, i)
                yield fnum, wtype, v, (0, 0)
            elif wtype == 2:
                n, i = varint(buf, i)
                yield fnum, wtype, None, (base + i, n)
                i += n
            elif wtype == 5:
                i += 4
                yield fnum, wtype, None, (0, 0)
            elif wtype == 1:
                i += 8
                yield fnum, wtype, None, (0, 0)
            else:
                raise ValueError(f"unsupported wire type {wtype}")

    def varint(buf: bytes, i: int) -> tuple[int, int]:
        v = shift = 0
        while True:
            b = buf[i]
            i += 1
            v |= (b & 0x7F) << shift
            if not (b & 0x80):
                return v, i
            shift += 7

    out = []
    for fnum, _, _, (off, n) in fields(blob, 0):
        if fnum != 20:  # devices
            continue
        dev = blob[off : off + n]
        slot = 0
        for bf, _, _, (boff, bn) in fields(dev, off):
            if bf != 10:  # blocks
                continue
            block = blob[boff : boff + bn]
            space = 0
            disc = (0, 0)
            for xf, _, xv, (xoff, xn) in fields(block, boff):
                if xf == 3 and xv is not None:
                    space = xv
                elif xf == 7:
                    disc = (xoff, xn)
            this_slot = -1
            if disc != (0, 0):
                this_slot = slot
                slot += 1
            for pf, _, _, (poff, pn) in fields(block, boff):
                if pf != 10:  # points
                    continue
                out.append(
                    {
                        "off": poff,
                        "len": pn,
                        "space": space,
                        "disc_off": disc[0],
                        "disc_len": disc[1],
                        "slot": this_slot,
                    }
                )
    return out


VKIND = {"f64", "i64", "u64", "bool", "flags", "fields", "datetime", "string", "bytes", "enum"}


def point_vkind(p: schema.Point) -> str:
    """Mirror the runtime's kind decision (composed > flags > fields >
    string/bytes > primitive)."""
    if p.storage_type == S.COMPOSED or p.mapping.HasField("composed"):
        return "f64"
    if p.value_type.WhichOneof("kind") == "flags":
        return "flags"
    if p.fields or p.bit_fields:
        return "fields"
    if p.storage_type in (S.STRING_ASCII, S.STRING_UTF8):
        return "string"
    if p.storage_type == S.BYTES_RAW:
        return "bytes"
    prim = p.value_type.primitive if p.value_type.WhichOneof("kind") == "primitive" else 0
    if prim == P.BOOL:
        return "bool"
    if prim == P.DATETIME:
        return "datetime"
    if prim in (P.DECIMAL, P.FLOAT32, P.FLOAT64):
        return "f64"
    if prim in (P.UINT32, P.UINT64):
        return "u64"
    if prim in (P.INT32, P.INT64):
        return "i64"
    return "enum"


def generate(doc: schema.ModDefDocument, blob: bytes, base: str) -> tuple[str, str]:
    guard = f"MODDEF_GEN_{base.upper()}_H"
    names = Names()

    h: list[str] = []
    c: list[str] = []
    doc_sym = f"{base}_doc"

    h.append(f"/* Generated by moddef_c_gen.py from {doc.doc_id!r}; DO NOT EDIT. */")
    h.append(f"#ifndef {guard}")
    h.append(f"#define {guard}")
    h.append("")
    h.append('#include "moddef/moddef.h"')
    h.append("")
    h.append(f"extern const uint8_t {doc_sym}[];")
    h.append(f"extern const uint32_t {doc_sym}_len;")
    h.append("")

    c.append(f"/* Generated by moddef_c_gen.py from {doc.doc_id!r}; DO NOT EDIT. */")
    c.append(f'#include "{base}.h"')
    c.append("")

    # Embedded binary document.
    c.append(f"const uint8_t {doc_sym}[] = {{")
    for i in range(0, len(blob), 16):
        c.append("    " + " ".join(f"0x{b:02x}," for b in blob[i : i + 16]))
    c.append("};")
    c.append(f"const uint32_t {doc_sym}_len = {len(blob)};")
    c.append("")

    # Document enum types as C enums.
    enum_prefix: dict[str, str] = {}
    for e in doc.enums:
        tname = names.claim(f"{base}_{c_ident(e.type_id)}_t")
        enum_prefix[e.type_id] = tname
        h.append(f"/* {e.name or e.type_id} */")
        h.append("typedef enum {")
        seen = Names()
        for v in e.values:
            member = seen.claim(f"{base.upper()}_{c_upper(e.type_id)}_{c_upper(v.name or f'v{v.value}')}")
            h.append(f"    {member} = {v.value},")
        h.append(f"}} {tname};")
        h.append("")

    ranges = wire_ranges(blob)
    ri = 0
    for dev in doc.devices:
        dv = c_ident(dev.device_id)
        prefix = base if len(doc.devices) == 1 else f"{base}_{dv}"
        points = [p for b in dev.blocks for p in b.points]
        n = len(points)
        dev_ranges = ranges[ri : ri + n]
        ri += n

        # Point-index enum.
        h.append(f"/* Point indexes for device profile {dev.device_id!r}. */")
        h.append("typedef enum {")
        idx_names = []
        seen = Names()
        for i, p in enumerate(points):
            nm = seen.claim(f"{prefix.upper()}_{c_upper(p.point_id)}")
            idx_names.append(nm)
            h.append(f"    {nm} = {i},")
        h.append(f"    {prefix.upper()}_POINT_COUNT = {n}")
        h.append(f"}} {prefix}_point_idx_t;")
        h.append("")
        h.append(f"md_err_t {prefix}_init(md_dev_t *dev, const md_transport_t *t);")
        h.append(f"md_err_t {prefix}_desc(uint32_t idx, md_point_desc_t *out);")
        h.append("")

        # Offset table + functions in the .c.
        c.append(f"typedef struct {{ uint32_t off, len, disc_off, disc_len; uint8_t space; int8_t slot; }} {prefix}_pt_t;")
        c.append(f"static const {prefix}_pt_t {prefix}_pts[] = {{")
        for r in dev_ranges:
            c.append(
                f"    {{{r['off']}u, {r['len']}u, {r['disc_off']}u, {r['disc_len']}u, "
                f"{r['space']}, {r['slot']}}},"
            )
        c.append("};")
        c.append("")
        c.append(f"md_err_t {prefix}_desc(uint32_t idx, md_point_desc_t *out)")
        c.append("{")
        c.append(f"    if (idx >= {prefix.upper()}_POINT_COUNT)")
        c.append("        return MD_ERR_NOT_FOUND;")
        c.append(f"    const {prefix}_pt_t *e = &{prefix}_pts[idx];")
        c.append("    md_point_t pt;")
        c.append(f"    pt.raw.p = {doc_sym} + e->off;")
        c.append("    pt.raw.len = e->len;")
        c.append("    pt.block.raw.p = 0;")
        c.append("    pt.block.raw.len = 0;")
        c.append("    pt.block.block_id.p = 0;")
        c.append("    pt.block.block_id.len = 0;")
        c.append("    pt.block.space = e->space;")
        c.append("    pt.block.has_discovery = e->disc_len != 0;")
        c.append(f"    pt.block.discovery.p = e->disc_len ? {doc_sym} + e->disc_off : 0;")
        c.append("    pt.block.discovery.len = e->disc_len;")
        c.append("    pt.block.discovery_slot = e->slot;")
        c.append("    return md_point_parse(&pt, out);")
        c.append("}")
        c.append("")
        c.append(f"md_err_t {prefix}_init(md_dev_t *dev, const md_transport_t *t)")
        c.append("{")
        c.append("    md_doc_t doc;")
        c.append(f"    md_err_t err = md_doc_init(&doc, {doc_sym}, {doc_sym}_len);")
        c.append("    if (err)")
        c.append("        return err;")
        c.append(f'    return md_dev_init(dev, &doc, MD_STR("{dev.device_id}"), t);')
        c.append("}")
        c.append("")

        # Typed accessors.
        fns = Names()
        fns.seen.update({f"{prefix}_init", f"{prefix}_desc"})
        for i, p in enumerate(points):
            kind = point_vkind(p)
            fn = fns.claim(f"{prefix}_read_{c_ident(p.point_id)}")
            sfn = fns.claim(f"{prefix}_set_{c_ident(p.point_id)}")
            readable = p.access in (A.READ_ONLY, A.READ_WRITE, 0)
            writable = p.access in (A.READ_WRITE, A.WRITE_ONLY, A.COMMAND)
            doc_note = p.name or p.point_id

            enum_t = None
            if kind == "enum" and p.value_type.WhichOneof("kind") == "enum_ref":
                enum_t = enum_prefix.get(p.value_type.enum_ref.type_id)

            def emit_read(sig: str, body: list[str]) -> None:
                h.append(f"/* {doc_note} */")
                h.append(f"static inline md_err_t {sig}")
                h.append("{")
                h.append("    md_point_desc_t d;")
                h.append(f"    md_err_t err = {prefix}_desc({idx_names[i]}, &d);")
                h.append("    if (err)")
                h.append("        return err;")
                h.extend(body)
                h.append("}")
                h.append("")

            read_common = [
                "    md_value_t v;",
                "    err = md_dev_read_desc(dev, &d, &v);",
                "    if (err)",
                "        return err;",
                "    if (v.kind == MD_VAL_UNAVAILABLE)",
                "        return MD_ERR_UNAVAILABLE;",
            ]

            if readable and kind == "string":
                emit_read(
                    f"{fn}(md_dev_t *dev, char *buf, size_t cap)",
                    ["    return md_dev_read_string_desc(dev, &d, buf, cap, 0);"],
                )
            elif readable and kind == "bytes":
                pass  # generic md_decode_bytes path; no typed wrapper in v0.1
            elif readable:
                out_t = {
                    "f64": "double",
                    "i64": "int64_t",
                    "u64": "uint64_t",
                    "bool": "bool",
                    "flags": "uint64_t",
                    "fields": "uint64_t",
                    "datetime": "int64_t",
                    "enum": enum_t or "int64_t",
                }[kind]
                pick = {
                    "f64": "*out = v.v.f64;",
                    "i64": "*out = v.v.i64;",
                    "u64": "*out = v.v.u64;",
                    "bool": "*out = v.v.b;",
                    "flags": "*out = v.v.bits;",
                    "fields": "*out = v.v.bits;",
                    "datetime": "*out = v.v.epoch;",
                    "enum": (
                        f"*out = ({enum_t})(v.kind == MD_VAL_I64 ? v.v.i64 : (int64_t)v.v.u64);"
                        if enum_t
                        else "*out = v.kind == MD_VAL_I64 ? v.v.i64 : (int64_t)v.v.u64;"
                    ),
                }[kind]
                emit_read(
                    f"{fn}(md_dev_t *dev, {out_t} *out)",
                    read_common + [f"    {pick}", "    return MD_OK;"],
                )

            if writable and kind in ("f64", "i64", "u64", "bool", "datetime", "enum", "flags"):
                in_t = {
                    "f64": "double",
                    "i64": "int64_t",
                    "u64": "uint64_t",
                    "bool": "bool",
                    "datetime": "int64_t",
                    "enum": enum_t or "int64_t",
                    "flags": "uint64_t",
                }[kind]
                mk = {
                    "f64": "md_value_t v = md_value_f64(val);",
                    "i64": "md_value_t v = md_value_i64(val);",
                    "u64": "md_value_t v = md_value_i64((int64_t)val);",
                    "bool": "md_value_t v = md_value_bool(val);",
                    "datetime": "md_value_t v; v.kind = MD_VAL_DATETIME; v.v.epoch = val;",
                    "enum": "md_value_t v = md_value_i64((int64_t)val);",
                    "flags": "md_value_t v; v.kind = MD_VAL_FLAGS; v.v.bits = val;",
                }[kind]
                h.append(f"/* Write {doc_note} (§11.4 constraints validated). */")
                h.append(f"static inline md_err_t {sfn}(md_dev_t *dev, {in_t} val)")
                h.append("{")
                h.append("    md_point_desc_t d;")
                h.append(f"    md_err_t err = {prefix}_desc({idx_names[i]}, &d);")
                h.append("    if (err)")
                h.append("        return err;")
                h.append(f"    {mk}")
                h.append("    return md_dev_write_desc(dev, &d, &v);")
                h.append("}")
                h.append("")

    h.append(f"#endif /* {guard} */")
    return "\n".join(h) + "\n", "\n".join(c) + "\n"


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("input")
    ap.add_argument("-o", "--outdir", default=".")
    args = ap.parse_args()

    src = Path(args.input)
    fmt = "yaml" if src.suffix in (".yaml", ".yml") else ("json" if src.suffix == ".json" else "binary")
    doc = parse_document(src.read_bytes(), fmt)
    blob = serialize_document(doc, "binary")
    assert isinstance(blob, bytes)

    base = c_ident(doc.doc_id or "moddef_doc")
    header, source = generate(doc, blob, base)
    outdir = Path(args.outdir)
    outdir.mkdir(parents=True, exist_ok=True)
    (outdir / f"{base}.h").write_text(header)
    (outdir / f"{base}.c").write_text(source)
    print(f"{outdir / (base + '.h')}\n{outdir / (base + '.c')}")


if __name__ == "__main__":
    main()
