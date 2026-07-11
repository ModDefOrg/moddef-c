# moddef-c

Embedded C runtime for [ModDef](https://github.com/ModDefOrg/moddef) (spec
v0.4) — declarative Modbus device definitions. C99, **no allocation, no
dependencies** (string.h + math.h), builds for Cortex-M.

The runtime parses **binary `.moddef` documents in place**: `md_doc_t` is a
zero-copy view over the serialized bytes wherever they live — memory-mapped
flash, a RAM buffer, or an array baked into the firmware. YAML/JSON stay
host-side (convert with any other ModDef implementation).

```c
#include "moddef/moddef.h"

md_doc_t doc;
md_doc_init(&doc, flash_ptr, flash_len);          /* zero-copy view */

md_dev_t dev;
md_dev_init(&dev, &doc, MD_STR("growatt-sph"), &transport);

md_value_t v;
md_dev_read(&dev, MD_STR("pv1_voltage"), &v);     /* v.v.f64 == 230.5 */
md_dev_write_f64(&dev, MD_STR("stop_soc"), 80.0); /* §11.4 validated */
```

`md_transport_t` is six blocking function pointers over your Modbus stack.

## Generated modules (optional)

`tools/moddef_c_gen.py` embeds the binary document and emits point-index
constants (byte offsets precomputed — no name lookups at runtime), C enums,
and typed inline accessors. Runtime-loaded and generated documents share
every code path.

```c
#include "growatt_sph.h"

md_dev_t inv;
growatt_sph_init(&inv, &transport);

double pv1;
growatt_sph_read_pv1_voltage(&inv, &pv1);
growatt_sph_inverter_run_state_t st;
growatt_sph_read_inverter_status(&inv, &st);      /* real C enum */
growatt_sph_set_active_power_rate(&inv, 80.0);    /* constraints checked */
```

## Semantics

Full codec lockstep with the Go/TS/Rust/Python implementations (shared
vector suite): exact scaling, SunSpec scale factors and discovery
(ID-relative model offsets, §7.3), selector cases, sentinels
(`MD_VAL_UNAVAILABLE` / `MD_ERR_UNAVAILABLE`), strings, BCD, flags, packed
fields, composed values, §11.4-validated writes. The decimal surface is
`double`; `md_decode_raw` is the exact escape hatch and FPU-less targets
can stay on the raw/integer path.

## Integrating

Vendor `include/` + `src/` (five .c files) into your firmware tree — there
is nothing to configure. Tunables: `-DMD_MAX_DISCOVERY=n` (SunSpec base
cache slots, default 4), `-DMD_MAX_POINT_WORDS=n` (stack window, default 64).

## Development

```sh
make test        # host tests: 600+ assertions incl. golden fixtures and
                 # the full 396-point growatt registry profile
make examples    # superloop demo against a stub transport
make thumb       # arm-none-eabi-gcc Cortex-M4 build of runtime + genmodule
```

Tests need sibling checkouts of `moddef` (fixtures), `devices` (registry),
and `moddef-py` with a `.venv` (host-side yaml→binary conversion and the
generator).
