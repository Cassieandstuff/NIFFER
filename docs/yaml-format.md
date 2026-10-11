# NIFFER YAML

Format version **1**. This is the normative description of the text that `niffer::yaml::Decompile`
writes and `niffer::yaml::Compile` reads (target `niffer::yaml`, CLI `niffer-yaml`). It is a
view of the schema (spec: [schema-language.md](schema-language.md)): field names, case names and
layouts all come from `schema/`. There are two modes; the `niffer.format` key says which, and
Compile reads both.

- **Faithful** is byte-exact. Compiling the decompiled text saves the original file's bytes,
  and every field is written as stored: counts, header string-table indices and block indices
  included. Nothing is derived on compile except what the container always re-measures (block
  count, sizes and type indices). An edit that leaves the file inconsistent is refused, not
  repaired. Most of this document describes faithful mode.
- **Authoring** is for editing by hand: blocks are keyed by ids, refs name ids, string refs
  are strings, and counts, sizes and the header's tables are derived on compile. See
  [Authoring mode](#authoring-mode).

## Document

One YAML document, a mapping with these keys (any other key is an error):

```yaml
niffer: {format: faithful, version: 1, schema: 0.2.0}
header:
  versionLine: "Gamebryo File Format, Version 20.2.0.7"
  version: 0x14020007
  endian: 1
  userVersion: 12
  bsVersion: 100
  author: "mcarofano\0"
  processScript: "\0"
  exportScript: " PE Static Art\0"
  blockTypes:
    - "BSFadeNode"  # 0
    - "BSXFlags"  # 1
  maxStringLength: 10
  strings:
    - "Barrel01"  # 0
  groups: []
blocks:
  - BSFadeNode:  # 0
      name: 0  # "Barrel01"
      extraData:
        - 1  # BSXFlags
      ...
  - BSXFlags:  # 1
      name: 1  # "BSX"
      integerData: 130
roots: [0]
trailing: "0102"        # only when the file has bytes past the footer
```

- `niffer`: `format` must be `faithful` and `version` 1. `schema` records the schema version
  the text was written with; it is informational.
- `header`: the header's stored facts, all required, re-emitted verbatim. Not written, because
  the container re-measures them on save: the block count, the per-block sizes, and the
  per-block type indices. A block's type index is the position of its class in `blockTypes`
  (the first position, if a class were listed twice); a class that isn't in `blockTypes` is an
  error.
- `blocks`: a sequence, in file order (a block's position is its index, which every ref uses).
  Each element is a mapping with exactly one key, the block's class:
  - a **modeled** block's value is a mapping of its layout's fields (inherited fields first),
    by schema field name;
  - a **raw** block (no descriptor, or a decode the schema refused) is a hex string of its
    bytes. On compile, raw bytes are loaded exactly as a file's would be, so a class that has
    since gained a descriptor comes back modeled.
- `roots`: the footer's root refs. `trailing`: bytes past the footer, as hex; omitted when
  there are none.

## Values

A value is read against its schema type. Style is free: flow or block collections, quoting,
key order within a mapping, comments and anchors don't change what compiles.

| Schema type | YAML |
|---|---|
| `u8` … `u64` | an unsigned integer, decimal or `0x` hex; must fit the type |
| `i8` … `i64` | a signed integer, decimal or `0x` hex; must fit the type |
| `ref`, `ptr` | the block index, `-1` for null. Decompile adds a comment with the target's class |
| `stringref` | the header string-table index, `-1` for empty (`4294967295` is accepted too). Decompile adds a comment with the string |
| `f32` | the shortest decimal that reads back to the same bits (`-0`, `1e-30`, `.inf`, `-.inf`); a NaN is `!bits 0x7FC01234` (the exact bit pattern) |
| `half` | likewise: the decimal of its exact value, or `!bits 0x7E01` for a NaN. A decimal that isn't exactly a half is an error |
| `sizedstring` | a double-quoted string (below) |
| `bytes` | a double-quoted hex string, two digits per byte. Long runs wrap with escaped line breaks (`\` at the end of a line), which YAML joins without spaces |
| struct | a mapping of its fields. A field whose `if:` condition is false is **omitted**; every other field is required |
| array | a sequence of its elements. A count or size field is a separate field and must match |
| union | a mapping with one key, the selected case's name; its value is the case's value, or `~` for an empty case. The case must be the one the selector field picks |

### Strings

Strings keep their raw bytes, NULs included. A string that is valid UTF-8 is written
double-quoted, as is, with these escaped: `\"`, `\\`, `\0`, `\t`, `\n`, `\r`, other control
characters as `\xNN`, and the code points YAML treats as line breaks or byte-order marks
(U+0080–U+009F, U+2028, U+2029, U+FEFF) as `\uNNNN`. YAML decodes every escape to UTF-8, which
gives back the original bytes. A string that isn't valid UTF-8 (a Latin-1 path, say) can't be
written that way, so it is written as `!hex "636166e9"`.

### Comments

Decompile writes comments that help a reader: each block's index, a ref's target class, a string
ref's string, each table entry's index, and why a raw block is raw. They are ignored on compile;
they are not checked against the data.

## Compile

Compile parses the text (rapidyaml), reads each modeled block's mapping into a value tree, and
builds the block with `Block::Build`. Build encodes the tree and decodes the bytes again, and
accepts the block only if the decode gives back exactly the tree. So a tree that contradicts
itself is refused, naming the first differing field:

- a count or size field that doesn't match its array or bytes;
- a field given where its condition is false, or missing where it holds;
- a union case the selector doesn't pick;
- a value that doesn't fit its type.

Every error carries the line and column of the YAML node it is about: a field name that isn't in
the layout, a value of the wrong shape, a class missing from `blockTypes`, a refused block.

## Authoring mode

```yaml
niffer: {format: authoring, version: 1, schema: 0.2.0}
header:
  author: "mcarofano"
  processScript: ""
  exportScript: " PE Static Art"
blocks:
  Barrel01: !BSFadeNode
    name: "Barrel01"
    extraData:
      - BSX
    controller: ~
    flags: 524302
    transform:
      translation: {x: 0, y: 0, z: 0}
      rotation: {m: [1, 0, 0, 0, 1, 0, 0, 0, 1]}
      scale: 1
    collisionObject: bhkCollisionObject#0
    children:
      - "Barrel01:0"
    effects: []
  BSX: !BSXFlags
    name: "BSX"
    integerData: 130
  ...
roots: [Barrel01]
```

Values are written as in faithful mode, with these differences.

- **Blocks** are a mapping from each block's **id** to its value, tagged with its class
  (`id: !Class`). File order is the order written; moving an entry moves the block. An id is
  any string, unique in the file. Decompile uses the block's name (its root `name` string ref)
  when that is non-empty, unique in the file and printable UTF-8, and `<Class>#<n>` otherwise
  (`n` counts the blocks of that class from 0). A raw block is `id: !Class "<hex>"`. Refs
  inside a raw block are opaque indices: reordering blocks around one breaks them.
- **Refs** (`ref`, `ptr`) and `roots` name ids; `~` is null.
- **String refs** are the strings themselves (double-quoted, or `!hex`); `~` is "no string",
  and `""` is a reference to an empty string. Compile builds the string table from them in
  first-use order: blocks in order, each block's fields in layout order.
- **Derived fields** are left out wherever they can be derived (schema-language.md §6):
  - a **length field**, one the schema names as the element count or byte size of a later
    array or bytes field (`count:` / `size:` in the descriptor). Compile sets an omitted length
    field to the length of the present containers it sizes; when one field sizes several, they
    must agree. A length field that sizes nothing present is written explicitly;
  - a **computed field** (`compute:` in the descriptor), such as BSTriShape's `dataSize` (the
    byte size of its vertices and triangles) or NiSkinPartition's `vertexSize`. Compile sets an
    omitted one to its computation.

  Either is written explicitly when its stored value doesn't match what it would derive to.
- **Header:** only `author`, `processScript` and `exportScript` (each written without the NUL
  that ends it in every vanilla file; `!verbatim "..."` writes one exactly as stored), and
  `groups` when it isn't empty. All keys are optional. Everything else is derived on compile:
  the version facts (SSE's), the block-type table (first-use order), the string table (above),
  `maxStringLength` (the longest string), and the block count and sizes. The SE corpus follows
  these conventions without exception, except that 1,751 of its 22,760 files order their string
  tables differently (animation sequences list their strings in another order).

So compiling an authoring decompile gives **the same file up to the string table's order**:
byte-identical where the file used first-use order, otherwise the same blocks, values and
strings with the string refs renumbered. Decompile refuses (an error, not text) a file
authoring mode can't express: a ref or string ref that points at nothing.

## Gate

`niffer-yaml roundtrip [--authoring] <dir>` decompiles and compiles every SSE NIF under `<dir>`.
Faithful mode requires the saved bytes to equal the file and the same blocks to be modeled.
Authoring mode requires the same file up to the string table's order (and counts how many are
byte-identical), and that decompiling the compiled file gives the same text again. CTest's
`niffer-yaml-test` covers the awkward cases on a hand-built file: NaN payloads (f32 and half),
negative zero, non-UTF-8 and NUL-carrying strings, a raw block, a packed vertex array, ids,
edits, and refusals in both modes.

## Linking

`niffer::yaml` compiles rapidyaml 0.15.2 (MIT) from the vendored single header
`third_party/rapidyaml/rapidyaml.hpp`, so it defines rapidyaml's symbols. Don't link it into a
binary that also links another copy of rapidyaml.
