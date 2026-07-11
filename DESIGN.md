# moddef-c — embedded C runtime for ModDef

Fifth implementation (after Go/TS/Rust/Python), targeting MCUs and small
gateways. C99, no dynamic allocation, no dependencies beyond `<stdint.h>`
`<stdbool.h>` `<string.h>` (and `<math.h>` on the decimal path).

## The one big decision: wire-first

**Embedded targets parse binary `.moddef` documents at runtime.** YAML/JSON
stay host-side (use any other implementation or the CLI to convert); the
protobuf binary form is compact, canonical, and — critically — parseable
**in place** with zero RAM:

- `md_doc_t` is a *view* over the serialized document wherever it lives
  (memory-mapped flash, a RAM buffer, an asset baked into the firmware).
  Nothing is copied or materialized; navigation walks tag/varint frames.
- Point lookup (`md_doc_find_point`) yields an `md_point_t` byte-range
  view; `md_point_parse` expands it into a compact ~120-byte
  `md_point_desc_t` on the stack (scalar fields decoded, repeated
  sections — na_values, selector cases, flags, fields — kept as wire
  ranges the codec iterates on demand).
- Strings on the wire are length-delimited, not NUL-terminated, so all
  identifiers are `md_str_t { const char *p; uint16_t len; }`.

Because the runtime already reads wire format, the **generator becomes
sugar, not architecture**: it embeds the binary document as a byte array
and emits point-index constants (byte offsets precomputed at generation
time, so no name lookups at runtime), C enums for the profile's enum
types, and `static inline` typed accessors. Runtime-loaded and
generator-embedded documents share every code path.

Per-read wire parsing costs a few microseconds on a Cortex-M — noise next
to a Modbus transaction.

## Layout

```
moddef-c/
  include/moddef/
    moddef.h        umbrella
    err.h           md_err_t
    str.h           md_str_t + helpers
    wire.h          tag/varint reader (internal but installable)
    doc.h           md_doc_t / md_device_view_t / md_block_t / md_point_t
    desc.h          md_point_desc_t (compact stack descriptor)
    codec.h         decode/encode/validate + string/bytes buffer APIs
    transport.h     blocking function-pointer table
    device.h        md_dev_t: read/write, SunSpec discovery, ref context
  src/              wire.c doc.c desc.c codec.c device.c
  tools/moddef_c_gen.py    generator (host-side, uses ../moddef-py)
  tests/            host-built (gcc): vectors, doc/fixture, device, generated
  examples/         superloop skeleton
  Makefile          host tests + thumb smoke; sources vendor directly
```

## API sketch

```c
md_doc_t doc;
md_doc_init(&doc, flash_ptr, flash_len);                 // zero-copy view
md_dev_t dev;
md_dev_init(&dev, &doc, MD_STR("growatt-sph"), &transport);

md_value_t v;
md_dev_read(&dev, MD_STR("pv1_voltage"), &v);            // v.f64
md_dev_write_f64(&dev, MD_STR("stop_soc"), 80.0);        // §11.4 validated
char sn[32]; md_dev_read_string(&dev, MD_STR("serial_number"), sn, sizeof sn);
```

`md_transport_t` is a struct of blocking function pointers
(`read_holding(void *ctx, uint16_t off, uint16_t count, uint16_t *out)`
returning `md_err_t`) — adapt to your HAL/RTOS; the core never blocks on
anything else. One in-flight request per transport, caller's discipline.

## Semantics (lockstep with Go/TS/Rust/Py)

- Codec: §8–§15 decode/encode incl. scale_ref POW10/MULTIPLY, selector_ref
  cases with fallback, na sentinels on masked raw, strings/BCD/flags/
  fields/datetime/composed; §11.4 write constraints.
- Decimal path computes in `double` (surface parity with every other
  impl); `md_decode_raw` is the exact escape hatch for billing counters.
  FPU-less targets can stay entirely on the raw/integer path.
- SunSpec discovery: ID-relative model offsets (§7.3), anchors probed,
  chain walked, base cached per block in `md_dev_t` (fixed slots,
  `MD_MAX_DISCOVERY`, default 4, override with -D).
- Wire field numbers come from spec §27 (draft-stable); the conformance
  tests against `moddef/fixtures` golden binaries guard drift.

## Testing

Host-built with gcc (`make test`): shared codec vectors (descriptors
constructed directly), golden fixture parsing (binary triples from
moddef/fixtures), registry profiles (converted yaml→binary at test-build
time via moddef-py), device tests over a mock transport incl. SunSpec,
and generated-code tests. CI adds an `arm-none-eabi-gcc -mcpu=cortex-m4`
compile smoke of the core + a generated profile.

## Milestones

1. wire/doc/desc + codec core; vectors + fixture parsing green.
   ✅ **Done** — 109 vector assertions (self-contained: tests build Point
   wire with a tiny writer, no host tooling needed); 454 doc assertions
   incl. all three golden binaries and the full 396-point growatt profile
   parsing from a 59.7 KB embedded binary.
2. transport + device layer; mock-transport tests incl. SunSpec.
   ✅ **Done** — 25 assertions: scaling, sentinels with meaning,
   constrained writes, SunSpec chain walk with ID-relative offsets and
   anchor caching.
3. generator + generated tests (growatt, sunspec fixture). ✅ **Done** —
   20 assertions: real C enum reads with lossless unknown values, packed
   fields, §11.4 writes, discovery + scale_ref + MD_ERR_UNAVAILABLE
   through typed accessors.
4. example, Makefile polish, CI (org conventions + thumb smoke), README.
   ✅ **Done** — superloop example runs on host; `make thumb`
   cross-compiles runtime + generated module for Cortex-M4 (exercised in
   CI; no arm-none-eabi-gcc on the dev box).
