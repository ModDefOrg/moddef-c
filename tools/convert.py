#!/usr/bin/env python3
"""Convert a .moddef.yaml / .moddef.json document to binary .moddef using
moddef-py (host-side tooling; the C runtime parses only the binary form)."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent / "moddef-py" / "src"))

from moddef.document import parse_document, serialize_document  # noqa: E402


def main() -> None:
    if len(sys.argv) != 3:
        print("usage: convert.py <in.moddef.yaml|json> <out.moddef>", file=sys.stderr)
        raise SystemExit(2)
    src, dst = Path(sys.argv[1]), Path(sys.argv[2])
    fmt = "yaml" if src.suffix in (".yaml", ".yml") else "json"
    doc = parse_document(src.read_bytes(), fmt)
    data = serialize_document(doc, "binary")
    assert isinstance(data, bytes)
    dst.write_bytes(data)
    print(f"{dst} ({len(data)} bytes)")


if __name__ == "__main__":
    main()
