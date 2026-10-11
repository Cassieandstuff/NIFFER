# Contributing to NIFFER

## Contributor License Agreement (required)

NIFFER is published under GPL-3.0-or-later, and its copyright holder also makes it available
under other terms. For that to stay possible, every contribution must be covered by a
Contributor License Agreement that lets the copyright holder relicense it.

**Pull requests cannot be merged until their author has signed the CLA.** This applies to
every external contribution, no matter how small. The CLA text and signing process will be
linked here.

<!-- TODO(owner): choose the CLA document (it must grant relicensing rights, not only an
     inbound GPL license) and wire up signing (e.g. the CLA Assistant GitHub app). -->

## Clean-room rule

NIFFER is a clean-room implementation, and that is a legal requirement, not a style
preference. When you add or change a block layout:

- **Do not** consult or copy from `nif.xml`, nifly, NifSkope, PyFFI, niflib or any other NIF
  library's source or schema.
- **Do** derive the layout from shipped game bytes. Record the file and block index you
  confirmed it against in the layout's comment, for example
  `// confirmed against clutter/barrel01.nif block 7`.
- A layout that only *size-matches* is not verified. A wrong layout that consumes the right
  byte count still round-trips, so check that the field *meanings* are right, not just the
  byte counts.

## Changing the schema

Block layouts live in `schema/` (see [docs/schema-language.md](docs/schema-language.md)). After
editing a descriptor, regenerate the outputs with the `niffer-schemagen` target (Python 3 +
PyYAML) and commit them together with the YAML. Never edit generated files by hand.

`to:` targets, `ref` vs `ptr`, and field meanings are claims about the data. Add one only when
`niffer-tool roundtrip` confirms it over the vanilla corpus; `--refs` shows the target
classes each untyped reference field actually has.

## Before you open a pull request

- `ctest` passes: `niffer-test`, the generator tests, and the generated-output freshness checks.
- With `-DNIFFER_BUILD_TOOLS=ON`, `niffer-tool roundtrip` and `niffer-tool tri-roundtrip` over
  the full extracted vanilla corpus stay **GREEN**: every SSE file byte-identical, no new raw or
  refused blocks, and no reference or string violations. If you touched the engine, the
  language or `yaml/`, `niffer-yaml roundtrip` over the corpus stays GREEN too.
- Third-party code goes only in `third_party/`, only for an optional target, and only under a
  permissive license; NIFFER's own sources never contain any.
- One description drives both read and write: one descriptor per block class, one `Sync(Stream&)`
  for the header and for TRI. Never add a separate read or write path for one direction.
