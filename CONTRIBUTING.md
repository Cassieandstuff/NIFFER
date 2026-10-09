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

## Before you open a pull request

- `ctest` passes (`niffer-test`).
- With `-DNIFFER_BUILD_TOOLS=ON`, `niffer-tool roundtrip` and `niffer-tool tri-roundtrip`
  over the full extracted vanilla corpus stay **GREEN**: every SSE file byte-identical, and
  no new unknown blocks or layout mismatches.
- Each block keeps **one** `Sync(Stream&)` that drives both read and write. Never add a
  separate read or write path for one direction.
