#!/usr/bin/env python3
"""schemagen validation tests: every rule in docs/schema-language.md that the
generator enforces rejects a schema that breaks it, with a message naming it."""

import sys
import tempfile
import textwrap
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "tools" / "schemagen"))
import schemagen  # noqa: E402

BASE = """\
manifest: t
schemaVersion: "0.0.1"
---
block: NiObject
abstract: true
"""

failures = 0


def build(extra: str):
    with tempfile.TemporaryDirectory() as d:
        (Path(d) / "s.yaml").write_text(BASE + "---\n" + textwrap.dedent(extra), encoding="utf-8")
        s = schemagen.Schema()
        s.load(Path(d))
        s.build()
        schemagen.Tables(s)
        return s


def accepts(name: str, extra: str):
    global failures
    try:
        build(extra)
    except schemagen.SchemaError as e:
        failures += 1
        print(f"FAIL {name}: rejected: {e}")


def rejects(name: str, extra: str, needle: str):
    global failures
    try:
        build(extra)
    except schemagen.SchemaError as e:
        if needle not in str(e):
            failures += 1
            print(f"FAIL {name}: wrong message: {e}")
        return
    failures += 1
    print(f"FAIL {name}: accepted")


accepts("minimal block", """\
    block: A
    inherits: NiObject
    fields: [ { name: x, type: u32 } ]
""")
accepts("outward reference from an inline struct", """\
    block: A
    inherits: NiObject
    fields:
      - { name: flag, type: u8 }
      - name: rows
        array: "count: u8"
        fields: [ { name: v, type: u8, if: { field: flag, ne: 0 } } ]
""")
rejects("unknown key", """\
    block: A
    inherits: NiObject
    colour: blue
""", "unknown key 'colour'")
rejects("reserved construct by name", """\
    block: A
    inherits: NiObject
    fields: [ { name: x, type: u32, union: {} } ]
""", "unions")
rejects("half is reserved", """\
    block: A
    inherits: NiObject
    fields: [ { name: x, type: half } ]
""", "half-precision")
rejects("missing inherits", """\
    block: A
    fields: [ { name: x, type: u32 } ]
""", "'inherits' is required")
rejects("unknown parent", """\
    block: A
    inherits: Nope
""", "inherits unknown block")
rejects("inheritance cycle", """\
    block: A
    inherits: B
    ---
    block: B
    inherits: A
""", "inheritance cycle")
rejects("duplicate field across inheritance", """\
    block: A
    inherits: NiObject
    fields: [ { name: x, type: u32 } ]
    ---
    block: B
    inherits: A
    fields: [ { name: x, type: u8 } ]
""", "inherited fields included")
rejects("rest not last", """\
    block: A
    inherits: NiObject
    fields: [ { name: a, type: bytes }, { name: b, type: u8 } ]
""", "only the last field")
rejects("rest then inherited fields", """\
    block: A
    inherits: NiObject
    fields: [ { name: a, type: bytes } ]
    ---
    block: B
    inherits: A
    fields: [ { name: b, type: u8 } ]
""", "only the last field")
rejects("named struct reaching outward", """\
    struct: S
    fields: [ { name: v, type: u8, if: { field: flag, ne: 0 } } ]
    ---
    block: A
    inherits: NiObject
    fields: [ { name: flag, type: u8 }, { name: s, type: S } ]
""", "no earlier field 'flag'")
rejects("forward reference", """\
    block: A
    inherits: NiObject
    fields: [ { name: xs, type: "u8[count: n]" }, { name: n, type: u8 } ]
""", "no earlier field 'n'")
rejects("count from a non-integer", """\
    block: A
    inherits: NiObject
    fields: [ { name: n, type: f32 }, { name: xs, type: "u8[count: n]" } ]
""", "must be an integer scalar")
rejects("count from a conditional field", """\
    block: A
    inherits: NiObject
    fields:
      - { name: f, type: u8 }
      - { name: n, type: u8, if: { field: f, ne: 0 } }
      - { name: xs, type: "u8[count: n]" }
""", "is conditional")
rejects("two operators", """\
    block: A
    inherits: NiObject
    fields: [ { name: f, type: u8 }, { name: g, type: u8, if: { field: f, eq: 1, ne: 2 } } ]
""", "exactly one operator")
rejects("unknown to target", """\
    block: A
    inherits: NiObject
    fields: [ { name: r, type: ref, to: [Nope] } ]
""", "unknown block 'Nope'")
rejects("to on a non-ref", """\
    block: A
    inherits: NiObject
    fields: [ { name: r, type: u32, to: [A] } ]
""", "'to' applies only to ref/ptr")
rejects("struct size mismatch", """\
    struct: S
    size: 5
    fields: [ { name: a, type: u32 } ]
""", "declared size 5")
rejects("struct contains itself", """\
    struct: S
    fields: [ { name: a, type: u8 }, { name: s, type: S } ]
""", "contains itself")
rejects("bytes array", """\
    block: A
    inherits: NiObject
    fields: [ { name: b, type: "bytes[2]" } ]
""", "bytes takes 'size:'")
rejects("rest-consuming element", """\
    struct: S
    fields: [ { name: a, type: bytes } ]
    ---
    block: A
    inherits: NiObject
    fields: [ { name: xs, type: "S[2]" } ]
""", "can't consume the rest")
rejects("bad field name", """\
    block: A
    inherits: NiObject
    fields: [ { name: Bad, type: u8 } ]
""", "camelCase")
rejects("reserved field name", """\
    block: A
    inherits: NiObject
    fields: [ { name: block, type: u8 } ]
""", "reserved")

print("schemagen tests:", "OK" if failures == 0 else f"{failures} failure(s)")
sys.exit(1 if failures else 0)
