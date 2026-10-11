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
    fields:
      - { name: k, type: u32 }
      - { name: v, union: { by: { lookup: { table: t, key: k } }, cases: [ { when: 1, name: a, type: u8 } ] } }
""", "lookup selectors")
rejects("packed on a non-array", """\
    block: A
    inherits: NiObject
    fields: [ { name: x, type: u32, packed: true } ]
""", "only to arrays")
rejects("packed array holding refs", """\
    block: A
    inherits: NiObject
    fields: [ { name: x, type: "ref[count: u32]", packed: true } ]
""", "can't contain ref")
accepts("packed arrays", """\
    struct: P
    fields: [ { name: a, type: half }, { name: b, type: "u8[3]" } ]
    ---
    block: A
    inherits: NiObject
    fields: [ { name: x, type: "P[count: u16]", packed: true }, { name: y, type: "u16[]", packed: true } ]
""")
accepts("half", """\
    block: A
    inherits: NiObject
    fields: [ { name: x, type: half }, { name: y, type: "half[4]" } ]
""")
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
accepts("named struct reading an enclosing field at each use site", """\
    struct: S
    fields: [ { name: v, type: u8, if: { field: flag, ne: 0 } } ]
    ---
    block: A
    inherits: NiObject
    fields: [ { name: flag, type: u8 }, { name: s, type: S } ]
    ---
    block: B
    inherits: NiObject
    fields: [ { name: pad, type: u16 }, { name: flag, type: u32 }, { name: ss, type: "S[2]" } ]
""")
rejects("named struct used where its field is missing", """\
    struct: S
    fields: [ { name: v, type: u8, if: { field: flag, ne: 0 } } ]
    ---
    block: A
    inherits: NiObject
    fields: [ { name: s, type: S } ]
""", "no earlier field 'flag'")
accepts("union with every kind of case", """\
    block: A
    inherits: NiObject
    fields:
      - { name: kind, type: u32 }
      - name: v
        union:
          by: { field: kind, mask: 3 }
          cases:
            - { when: 1, name: one, type: f32 }
            - { when: [2, 3], name: many, fields: [ { name: a, type: u8 } ] }
          default: { name: none }
""")
rejects("union value in two cases", """\
    block: A
    inherits: NiObject
    fields:
      - { name: kind, type: u32 }
      - { name: v, union: { by: { field: kind }, cases: [ { when: 1, name: a, type: u8 }, { when: [1, 2], name: b, type: u8 } ] } }
""", "appears in two cases")
rejects("union case name twice", """\
    block: A
    inherits: NiObject
    fields:
      - { name: kind, type: u32 }
      - { name: v, union: { by: { field: kind }, cases: [ { when: 1, name: a, type: u8 }, { when: 2, name: a, type: u8 } ] } }
""", "duplicate case name")
rejects("union selector not an integer", """\
    block: A
    inherits: NiObject
    fields:
      - { name: kind, type: f32 }
      - { name: v, union: { by: { field: kind }, cases: [ { when: 1, name: a, type: u8 } ] } }
""", "must be an integer scalar")
rejects("union without cases", """\
    block: A
    inherits: NiObject
    fields:
      - { name: kind, type: u32 }
      - { name: v, union: { by: { field: kind }, cases: [] } }
""", "non-empty 'cases'")
rejects("union case counted by a field", """\
    block: A
    inherits: NiObject
    fields:
      - { name: n, type: u32 }
      - { name: v, union: { by: { field: n }, cases: [ { when: 1, name: a, type: "u8[count: n]" } ] } }
""", "inline struct case")
rejects("rest-consuming union not last", """\
    block: A
    inherits: NiObject
    fields:
      - { name: n, type: u32 }
      - { name: v, union: { by: { field: n }, cases: [ { when: 1, name: a, type: bytes } ] } }
      - { name: after, type: u8 }
""", "only the last field")
accepts("byte-window arrays", """\
    block: A
    inherits: NiObject
    fields:
      - { name: size, type: u32 }
      - { name: xs, type: "u16[]", size: size }
      - { name: ys, type: "u8[]", size: 4 }
""")
rejects("size on a counted array", """\
    block: A
    inherits: NiObject
    fields: [ { name: xs, type: "u8[4]", size: 4 } ]
""", "needs the open suffix")
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
accepts("reference to a conditional field", """\
    block: A
    inherits: NiObject
    fields:
      - { name: f, type: u8 }
      - { name: n, type: u8, if: { field: f, ne: 0 } }
      - { name: xs, type: "u8[count: n]", if: { field: f, ne: 0 } }
""")
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

accepts("computed fields", """    block: A
    inherits: NiObject
    fields:
      - { name: desc, type: u64 }
      - { name: total, type: u32, compute: { size: [items, blob] } }
      - { name: unit, type: u16, compute: { field: desc, mask: 15, times: 4 } }
      - { name: n, type: u8 }
      - { name: items, type: "u16[count: n]" }
      - { name: blob, type: bytes, size: 4 }
""")
rejects("compute on a float", """    block: A
    inherits: NiObject
    fields: [ { name: x, type: f32, compute: { size: [x] } } ]
""", "only to integer fields")
rejects("compute with both forms", """    block: A
    inherits: NiObject
    fields:
      - { name: d, type: u32 }
      - { name: x, type: u32, compute: { size: [d], field: d } }
""", "exactly one of 'size' or 'field'")
rejects("compute size of an unknown field", """    block: A
    inherits: NiObject
    fields: [ { name: x, type: u32, compute: { size: [nope] } } ]
""", "no field 'nope'")
rejects("compute size of itself", """    block: A
    inherits: NiObject
    fields: [ { name: x, type: u32, compute: { size: [x] } } ]
""", "can't sum itself")
rejects("compute size with a mask", """    block: A
    inherits: NiObject
    fields:
      - { name: x, type: u32, compute: { size: [b], mask: 3 } }
      - { name: b, type: bytes, size: 2 }
""", "takes no mask")
accepts("compute from a later field of the struct", """    block: A
    inherits: NiObject
    fields:
      - { name: x, type: u32, compute: { field: d, mask: 15, times: 4 } }
      - { name: d, type: u64 }
""")
rejects("compute from an unknown field", """    block: A
    inherits: NiObject
    fields: [ { name: x, type: u32, compute: { field: nope } } ]
""", "no field 'nope'")
rejects("compute from a non-integer", """    block: A
    inherits: NiObject
    fields:
      - { name: x, type: u32, compute: { field: f } }
      - { name: f, type: f32 }
""", "must be an integer scalar")
rejects("compute with a bad shift", """    block: A
    inherits: NiObject
    fields:
      - { name: d, type: u64 }
      - { name: x, type: u32, compute: { field: d, shift: 64 } }
""", "shift < 64")
rejects("a computed field as a count", """    block: A
    inherits: NiObject
    fields:
      - { name: d, type: u64 }
      - { name: n, type: u8, compute: { field: d, mask: 7 } }
      - { name: xs, type: "u8[count: n]" }
""", "is computed, so it can't also be a count")
rejects("unknown compute key", """    block: A
    inherits: NiObject
    fields:
      - { name: d, type: u64 }
      - { name: x, type: u32, compute: { field: d, plus: 1 } }
""", "unknown key 'plus'")

print("schemagen tests:", "OK" if failures == 0 else f"{failures} failure(s)")
sys.exit(1 if failures else 0)
