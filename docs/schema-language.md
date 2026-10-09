# NIFFER schema language — specification

**Language version 0.2.** This document is normative. It defines what a descriptor in `schema/`
means, so that any implementation can decode and encode Skyrim SE NIF blocks exactly as
NIFFER's engine does. When this document and the code disagree, that is a bug in one of them.
`tools/schemagen` is the reference validator, and `niffer::Block` is the reference engine. The
design rationale is in [schema-design.md](schema-design.md).

The language deliberately follows ESPER's (github.com/Cassieandstuff/ESPER,
`docs/schema-language.md`) wherever the two formats agree. Plugin records are subrecord streams;
NIF blocks are flat layouts whose size the file header declares. So this language has no
signatures, groups or ordinal rule, and adds inheritance and block references.

## 1. Files

A schema is a directory tree of YAML files (`*.yaml` / `*.yml`). Each file holds one or more
YAML documents, and each document is one descriptor. The descriptor's kind is set by which of
these keys it has (exactly one):

| Key | Descriptor |
|---|---|
| `manifest` | schema metadata; exactly one per schema: `schemaVersion: "x.y.z"` |
| `block` | a NIF block class (§5) |
| `struct` | a named, reusable byte layout (§4) |

`enum` is reserved for a later version (§10). Unknown keys are errors. A key that is reserved
for a later version is rejected by name.

By convention each block and struct lives in its own file, named after it:
`schema/blocks/<area>/<Block>.yaml`, `schema/structs/<Struct>.yaml`.

## 2. Names

- **Block names** are the class names exactly as they appear in a NIF header's block-type table
  (`NiNode`, `BSXFlags`, `bhkRigidBody`): `[A-Za-z][A-Za-z0-9_]*`. They also name the generated
  view types, so they are unique across blocks and structs.
- **Struct names** are PascalCase: `[A-Z][A-Za-z0-9]*`. Inline structs are named by
  concatenating their owner's name with the PascalCased field name, for example `NiSkinDataBone`.
- **Field names** are camelCase: `[a-z][A-Za-z0-9]*`. `self`, `block` and `cursor` are reserved.

## 3. Types

A `type` is a base type, optionally followed by an array suffix.

### 3.1 Base types

| Type | Encoding (little-endian) | Options |
|---|---|---|
| `u8 u16 u32 u64` | unsigned integer of that width | |
| `i8 i16 i32 i64` | two's-complement integer | |
| `f32` | IEEE-754 single; carried as raw bits, so every bit pattern round-trips | |
| `half` | IEEE-754 half (2 bytes); carried as raw bits, converted at the edge | |
| `ref` | i32 block index; −1 is null. An **owning** link: a parent to its child | `to: [Block, …]` (§8) |
| `ptr` | i32 block index; −1 is null. A **non-owning** link back or across the graph (a controller's target, a skin's bones) | `to: [Block, …]` (§8) |
| `stringref` | u32 index into the header's string table; `0xFFFFFFFF` is empty | |
| `sizedstring` | u32 byte length, then that many bytes, kept verbatim (including any NUL) | |
| `bytes` | opaque bytes: `size` bytes, otherwise the rest of the block | `size: N` or `size: <field>` (§3.3) |
| *StructName* | the named struct's layout (§4) | |

### 3.2 Array suffixes

| Suffix | Meaning |
|---|---|
| `T[]` | elements to the end of the block |
| `T[N]` | exactly N elements |
| `T[count: u8]`, `T[count: u16]`, `T[count: u32]` | an element count of that width, then the elements |
| `T[count: <field>]` | the element count is the value of an earlier field (§3.3) |
| `T[]` with `size: N` or `size: <field>` | elements until exactly that many **bytes** are consumed (a byte window); for formats that store a byte size rather than a count |

The field's options (for example `to:` on `"ref[count: u32]"`) apply to the element type. An
element that consumes zero bytes is an error at decode time; otherwise a `T[]` array would loop
forever.

### 3.3 Field references

`T[count: <field>]`, `size: <field>`, `if:` (§4.2) and a union's `by:` (§4.3) name another
field. The name resolves to the **nearest earlier field**: first among the fields before this one
in the same struct, then among the fields before the enclosing field in each enclosing struct,
outward to the block. So an array element can read a count or flag that lives on the block.

The referenced field must be an integer scalar (`u8`–`u64`, `i8`–`i64`) and must not itself be
conditional.

A **named** struct may also reference a field of the structs that enclose it, such as a vertex
layout reading its shape's `vertexDesc`. Such a reference is resolved **at each use site**,
against that site's fields: a use site where the name resolves to no earlier field is an error.

`T[count: <field>]` and `size: <field>` describe a field's own length, so they appear only on a
field's top-level type, never on an array's element type or a union case's type.

## 4. Structs

```yaml
struct: Vec3
size: 12            # optional cross-check
fields:
  - { name: x, type: f32 }
  - { name: y, type: f32 }
  - { name: z, type: f32 }
```

- Fields are laid out in order, with no implicit padding.
- **No byte is ever dropped.** Padding and unknown bytes must be named fields.
- `size`, when present, must equal the sum of the field sizes. A struct whose size varies
  (strings, arrays without a fixed length, conditional fields) can't declare a size.
- **Rest of the block:** only the **last** field of a block may consume the rest of the block
  (`T[]`, or `bytes` without `size`), directly or through a struct whose own last field does.
- A struct can't contain itself.
- A field may define an inline struct with `fields:` instead of `type:`. It may still carry an
  array suffix through `array:` (`{ name: bones, array: "count: numBones", fields: [...] }`).

### 4.1 Field keys

| Key | Meaning |
|---|---|
| `name` | required |
| `type` / `fields` / `union` | exactly one: a type (§3), an inline struct, or a union (§4.3) |
| `array` | with `fields:` only: an array suffix without brackets (`"count: u32"`, `"count: numKeys"`, `"4"`, `""`) |
| `to` | `ref`/`ptr` (or arrays of them) only: allowed target blocks (§8) |
| `size` | `bytes` (§3.1) and `T[]` byte windows (§3.2) |
| `if` | a condition (§4.2) |
| `doc` | free text: what the field means and how it was verified. Ignored by engines. |

### 4.2 Conditional fields

```yaml
  - { name: interpolation, type: u32, if: { field: numKeys, min: 1 } }
```

`if: { field: <name>, <op>: <integer> }` with exactly one operator:

| Operator | Present when the field's value… |
|---|---|
| `eq` | equals the operand |
| `ne` | differs from the operand |
| `min` | is ≥ the operand |
| `max` | is ≤ the operand |
| `mask` | ANDed with the operand is non-zero |

Values compare as signed 64-bit for signed types and unsigned 64-bit otherwise. A field whose
condition is false is **absent**: it consumes no bytes and has no value.

### 4.3 Unions

```yaml
  - name: data
    union:
      by: { field: interpolation }            # optionally: mask: <integer>
      cases:
        - { when: 1, name: linear, fields: [ { name: value, type: f32 } ] }
        - { when: [2, 6], name: quadratic, type: QuadraticFloat }
      default: { name: other, type: bytes }   # optional
```

A union is one of a closed set of layouts, chosen by an earlier integer field, the **selector**.

- The selector's value (ANDed with `mask` when given) is compared with each case's `when`, a
  value or a list of values; a value may appear in only one case. Signed selectors compare
  signed, unless masked.
- No matching case: the `default` case, if any. Otherwise the block is refused.
- A case has a `name` (camelCase, unique in the union) and `type:` (§3), `fields:` (an inline
  struct), or neither: an **empty** case consumes no bytes.
- A case's type can't take its length from a field (`count:` / `size:` a field); use an inline
  struct case for that.
- A union whose case consumes the rest of the block must be the last field.
- A union field may also carry `if:`.

In a value, a union is its selected case. Paths name the case after the union field
(`data/linear/value`), and a case that isn't selected has no value.

## 5. Blocks

```yaml
block: NiNode
inherits: NiAVObject
fields:
  - { name: children, type: "ref[count: u32]", to: [NiAVObject] }
  - { name: effects,  type: "ref[count: u32]" }
verified: [ "clutter/barrel01.nif#0" ]
```

| Key | Meaning |
|---|---|
| `block` | the class name (§2) |
| `inherits` | the parent block. Required on every block except `NiObject`, the root. |
| `abstract: true` | the class never appears in a file. It exists to share fields and to be a `to:` target. |
| `fields` | this class's own fields, which follow all inherited fields |
| `verified` | informational: the files and block indices the layout was confirmed against (`"<path>#<index>"`) |
| `doc` | free text |

- A block's **layout** is its ancestors' fields, root first, followed by its own. Field names
  must be unique across that whole layout.
- Every non-abstract block is **concrete**: a block in a file whose type name equals it is
  decoded with its layout.
- A block whose type name has no concrete descriptor is **raw** (§7.1).

## 6. Derived lengths

A field that a later array or `bytes` uses as its count or size (`count: <field>`,
`size: <field>`) is a **length field**. Its value is part of the data and is decoded and
re-encoded like any other. When an array or byte run is **replaced through the editing API**, the
engine sets its length field to the new length. A length field is never recomputed from an array
that is absent.

## 7. Semantics

### 7.1 Decoding a block

A block is decoded from exactly the bytes the header declares for it.

1. Find the concrete descriptor named by the block's type. If there is none, the block is
   **raw**.
2. Decode its layout field by field. A conditional field is decoded only if its condition holds
   (§4.2).
3. **Exact accounting.** Decoding must consume exactly the block's declared size. If it reads
   past the end, meets an invalid length, or stops short, the block is **refused**: it is kept
   raw with its bytes and position, and the reason is recorded.

Refusal is normal, not an error. Raw blocks keep their bytes and their index.

### 7.2 Encoding a block

- **Raw and unmodified blocks** are written from their original bytes.
- **Modified blocks** are written from their value: each field in layout order, absent
  conditional fields skipped. A conditional field whose condition holds but which has no value is
  written as its zero value. A union writes the case its selector picks now: if the value holds
  another case, the picked case's zero value is written.

An implementation that verifies its layouts must also support a **forced** encode, which writes
every decoded block from its value. For every SSE file of the vanilla game, this must reproduce
the original bytes. It is NIFFER's real-data gate.

### 7.3 Editing

- **No-op sets:** setting a value equal to the current one doesn't modify anything.
- **Derived lengths:** see §6. A byte-window array's size field is set to the encoded size of
  its elements.
- **Unions:** the selector field decides the case. Storing a value into a case that isn't
  selected fails.

## 8. Block references

- Only `ref` and `ptr` values are block references. They are file-native indices into the block
  list; −1 is null.
- **`to: [Block, …]`** declares which blocks a reference may point to. A target matches if its
  class is one of the listed blocks or inherits from one. Without `to:`, any block may be the
  target. `to:` is a claim about the data: add it only when the real-data gate confirms it.
- **`ref` vs `ptr`** is also a claim about the data. Following only `ref` links from the file's
  roots never revisits a block on the current path; that is, the `ref` graph is acyclic. The gate
  checks this.
- `stringref` values index the header's string table. They are not block references.

## 9. Outputs

`tools/schemagen` validates a schema and emits these outputs. They are committed to the repo, so
consumers never run the generator.

| Output | Content |
|---|---|
| `schema.json` | The resolved schema for data consumers: `schemaVersion`, `languageVersion`, `structs[]` and `blocks[]`. Each block has `name`, `parent`, `abstract`, and its full `layout` (inherited fields first; each field names the block that declares it in `declaredBy`), plus `verified` and `doc` when present. Every field has `name` and a resolved `type` (`kind`, and `fields`, `element`, `count`, `countPrefix`, `countField`, `size`, `sizeField`, `to`, `by` and `cases` as applicable), plus `if` when conditional. Named structs appear expanded at each use. |
| `hpp/generated/Tables.h` | the engine's constexpr tables (`niffer::schema::SchemaSet`). Private to the library. |
| `include/niffer/generated/Fields.h` | typed handles: `niffer::fields::<Block>::<field>[::<field>]`; struct fields expose `::self` |
| `include/niffer/generated/Views.h` | typed views (`niffer::view::<Name>`): accessors only, over the generic engine. Block views inherit as the blocks do. A union field gets `<Field>Case()` (the selected case's index, or −1) and one accessor per case, `<Field><Case>()`, which is empty unless that case is selected. |

## 10. Reserved for later versions

These constructs come from the design in [schema-design.md](schema-design.md). The current
language version rejects them by name.

| Construct | Purpose |
|---|---|
| `lookup` union selector | a case chosen through a table, keyed by a field |
| `packed: true` on arrays | fixed-size elements kept as one byte run and read in place, for vertex and triangle data |
| `enum:`, `flags:`, `unit:` and `enum` descriptors | rendering hints |

## 11. Versioning

- `schemaVersion` (in the manifest) follows semver for the schema's content. Adding blocks,
  fields or cases is a minor change. Renaming or removing a field, or changing a layout, is a
  major change.
- `languageVersion` is this document's version. It changes only when a construct is added or its
  meaning changes.

| Version | Change |
|---|---|
| 0.1 | Initial: scalars, `ref`/`ptr`, `stringref`, `sizedstring`, `bytes`, structs, arrays (fixed, rest, count prefix, count field), `if:` conditions, block inheritance, `to:` |
| 0.2 | adds `half`, `union:` (selector field with optional mask, `when` values, `default`, empty cases), byte-window arrays (`T[]` + `size:`), and named structs that read enclosing fields (resolved per use site) |
