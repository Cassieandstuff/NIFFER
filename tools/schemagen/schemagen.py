#!/usr/bin/env python3
"""schemagen — compile NIFFER's YAML schema into schema.json + C++ headers.

    schemagen.py --schema schema --out include/niffer/generated \\
                 --tables hpp/generated/Tables.h --json schema.json
    schemagen.py ... --check        # exit 1 if any output is stale

Emits:
    <--tables>        constexpr tables the engine interprets (niffer::schema::SchemaSet)
    <--out>/Fields.h  typed handles (niffer::FieldRef<T> / StructRef)
    <--out>/Views.h   typed views (thin accessors over niffer::Block)
    <--json>          the resolved schema for data consumers

The descriptor language is specified in docs/schema-language.md. This file is
the reference implementation of its validation rules: anything it accepts the
engine can interpret, and anything outside the current language version is
rejected by name.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

try:
    import yaml
except ImportError:  # pragma: no cover
    sys.exit("schemagen: PyYAML is required (pip install pyyaml)")

LANGUAGE_VERSION = "0.1"

# spelling -> (C++ Kind, C++ value type, byte size)
SCALARS = {
    "u8": ("U8", "uint8_t", 1), "u16": ("U16", "uint16_t", 2),
    "u32": ("U32", "uint32_t", 4), "u64": ("U64", "uint64_t", 8),
    "i8": ("I8", "int8_t", 1), "i16": ("I16", "int16_t", 2),
    "i32": ("I32", "int32_t", 4), "i64": ("I64", "int64_t", 8),
    "f32": ("F32", "float", 4),
}
INTEGER_KINDS = {"U8", "U16", "U32", "U64", "I8", "I16", "I32", "I64"}
PREFIX_LEN = {"u8": "Prefix8", "u16": "Prefix16", "u32": "Prefix32"}
COND_OPS = {"eq": "Eq", "ne": "Ne", "min": "Min", "max": "Max", "mask": "Mask"}

# Constructs specified by the design but not in this language version: reject by name.
NOT_YET = {
    "union": "unions", "half": "half-precision floats", "packed": "packed arrays",
    "enum": "enum hints", "flags": "flag hints", "unit": "unit hints",
}

CPP_KEYWORDS = set("""alignas alignof and and_eq asm auto bitand bitor bool break case catch char
char8_t char16_t char32_t class compl concept const consteval constexpr constinit const_cast
continue co_await co_return co_yield decltype default delete do double dynamic_cast else enum
explicit export extern false float for friend goto if inline int long mutable namespace new
noexcept not not_eq nullptr operator or or_eq private protected public register
reinterpret_cast requires return short signed sizeof static static_assert static_cast struct
switch template this thread_local throw true try typedef typeid typename union unsigned using
virtual void volatile wchar_t while xor xor_eq""".split())
RESERVED_FIELDS = {"self", "block", "cursor", "path", "from"}

BLOCK_KEYS = {"block", "inherits", "abstract", "fields", "verified", "doc"}
STRUCT_KEYS = {"struct", "size", "fields", "doc"}
MANIFEST_KEYS = {"manifest", "schemaVersion"}
FIELD_KEYS = {"name", "type", "fields", "array", "to", "size", "if", "doc"}


class SchemaError(Exception):
    pass


def fail(where: str, msg: str):
    raise SchemaError(f"{where}: {msg}")


def ident(name: str) -> str:
    return name + "_" if name in CPP_KEYWORDS else name


def pascal(name: str) -> str:
    return name[:1].upper() + name[1:]


def check_keys(where: str, d: dict, allowed: set[str]):
    for k in d:
        if k in NOT_YET:
            fail(where, f"'{k}' ({NOT_YET[k]}) is not supported in language {LANGUAGE_VERSION}")
        if k not in allowed:
            fail(where, f"unknown key '{k}' (allowed: {', '.join(sorted(allowed))})")


def check_field_name(where: str, name) -> str:
    if not isinstance(name, str) or not re.fullmatch(r"[a-z][A-Za-z0-9]*", name):
        fail(where, f"field name must be camelCase ([a-z][A-Za-z0-9]*), got {name!r}")
    if name in RESERVED_FIELDS:
        fail(where, f"field name '{name}' is reserved")
    return name


def check_struct_name(where: str, name) -> str:
    if not isinstance(name, str) or not re.fullmatch(r"[A-Z][A-Za-z0-9]*", name):
        fail(where, f"struct name must be PascalCase ([A-Z][A-Za-z0-9]*), got {name!r}")
    return name


def check_block_name(where: str, name) -> str:
    if not isinstance(name, str) or not re.fullmatch(r"[A-Za-z][A-Za-z0-9_]*", name):
        fail(where, f"block name must match [A-Za-z][A-Za-z0-9_]*, got {name!r}")
    return name


# ── Model ─────────────────────────────────────────────────────────────────────

@dataclass
class Type:
    """A resolved type. `key` identifies it for de-duplication in the tables."""
    kind: str                      # C++ Kind enumerator
    key: str
    len: str = "None"              # C++ LenKind enumerator
    count: int = 0
    elem: "Type | None" = None     # Array
    struct: "Struct | None" = None  # Struct
    to: tuple = ()                 # Ref/Ptr target block names
    spelling: str = ""             # scalar/base spelling, for json

    def fixed_size(self) -> int | None:
        if self.kind in ("Ref", "Ptr", "StringRef"):
            return 4
        for k, size in ((v[0], v[2]) for v in SCALARS.values()):
            if self.kind == k:
                return size
        if self.kind == "Bytes" and self.len == "Fixed":
            return self.count
        if self.kind == "Array" and self.len == "Fixed":
            es = self.elem.fixed_size()
            return None if es is None else es * self.count
        if self.kind == "Struct":
            return self.struct.fixed_size()
        return None

    def consumes_rest(self) -> bool:
        if self.kind in ("Bytes", "Array") and self.len == "Rest":
            return True
        if self.kind == "Struct" and self.struct.fields:
            return self.struct.fields[-1].type.consumes_rest()
        return False


@dataclass
class Field:
    name: str
    type: Type
    where: str
    len_ref: tuple | None = None        # (up, index, name)
    cond: tuple | None = None           # (op, up, index, value, fieldName, opSpelling)
    doc: str | None = None
    declared_by: str | None = None      # block that declares it (block layouts)


@dataclass
class Struct:
    name: str
    where: str
    named: bool
    fields: list[Field] = field(default_factory=list)
    resolved: bool = False

    def fixed_size(self) -> int | None:
        total = 0
        for f in self.fields:
            if f.cond is not None:
                return None
            s = f.type.fixed_size()
            if s is None:
                return None
            total += s
        return total


@dataclass
class Block:
    name: str
    where: str
    raw: dict
    parent: str | None = None
    abstract: bool = False
    own: list[Field] = field(default_factory=list)
    layout: list[Field] = field(default_factory=list)
    verified: list = field(default_factory=list)
    doc: str | None = None
    resolved: bool = False
    resolving: bool = False


class Schema:
    def __init__(self):
        self.version: str | None = None
        self.struct_docs: dict[str, tuple[str, dict]] = {}
        self.structs: dict[str, Struct] = {}
        self.inline_structs: list[Struct] = []
        self.blocks: dict[str, Block] = {}
        self._resolving_structs: set[str] = set()

    # ── Loading ───────────────────────────────────────────────────────────────
    def load(self, root: Path):
        files = sorted(p for p in root.rglob("*") if p.suffix in (".yaml", ".yml"))
        if not files:
            raise SchemaError(f"{root}: no schema files")
        for path in files:
            rel = path.relative_to(root).as_posix()
            with path.open(encoding="utf-8") as fh:
                docs = list(yaml.safe_load_all(fh))
            for i, doc in enumerate(docs):
                if doc is None:
                    continue
                where = rel if len(docs) == 1 else f"{rel}[{i}]"
                if not isinstance(doc, dict):
                    fail(where, "a descriptor must be a mapping")
                kinds = [k for k in ("manifest", "block", "struct") if k in doc]
                for k in doc:
                    if k in NOT_YET and k not in FIELD_KEYS:
                        fail(where, f"'{k}' ({NOT_YET[k]}) is not supported in language {LANGUAGE_VERSION}")
                if len(kinds) != 1:
                    fail(where, "a descriptor needs exactly one of: manifest, block, struct")
                getattr(self, f"_load_{kinds[0]}")(where, doc)

    def _load_manifest(self, where: str, d: dict):
        check_keys(where, d, MANIFEST_KEYS)
        if self.version is not None:
            fail(where, "more than one manifest")
        v = d.get("schemaVersion")
        if not isinstance(v, str) or not re.fullmatch(r"\d+\.\d+\.\d+", v):
            fail(where, "schemaVersion must be a quoted \"x.y.z\" string")
        self.version = v

    def _load_struct(self, where: str, d: dict):
        check_keys(where, d, STRUCT_KEYS)
        name = check_struct_name(where, d["struct"])
        if name in self.struct_docs or name in self.blocks:
            fail(where, f"'{name}' is defined twice")
        if not isinstance(d.get("fields"), list) or not d["fields"]:
            fail(where, "a struct needs a non-empty 'fields' list")
        self.struct_docs[name] = (where, d)

    def _load_block(self, where: str, d: dict):
        check_keys(where, d, BLOCK_KEYS)
        name = check_block_name(where, d["block"])
        if name in self.blocks or name in self.struct_docs:
            fail(where, f"'{name}' is defined twice")
        b = Block(name=name, where=f"{where} ({name})", raw=d)
        b.parent = d.get("inherits")
        b.abstract = bool(d.get("abstract", False))
        if "abstract" in d and not isinstance(d["abstract"], bool):
            fail(b.where, "'abstract' must be true or false")
        fields = d.get("fields", [])
        if not isinstance(fields, list):
            fail(b.where, "'fields' must be a list")
        verified = d.get("verified", [])
        if not isinstance(verified, list) or not all(isinstance(v, str) for v in verified):
            fail(b.where, "'verified' must be a list of strings")
        b.verified = verified
        b.doc = d.get("doc")
        self.blocks[name] = b

    # ── Resolution ────────────────────────────────────────────────────────────
    def build(self):
        if self.version is None:
            raise SchemaError("schema has no manifest")
        if "NiObject" not in self.blocks:
            raise SchemaError("schema has no root block 'NiObject'")
        for name, b in self.blocks.items():
            if name == "NiObject":
                if b.parent is not None:
                    fail(b.where, "NiObject is the root and can't inherit")
            elif b.parent is None:
                fail(b.where, "'inherits' is required (every block except NiObject has a parent)")
            elif b.parent not in self.blocks:
                fail(b.where, f"inherits unknown block '{b.parent}'")
        for name in self.struct_docs:
            self.named_struct(name, "schema")
        for b in self.blocks.values():
            self.resolve_block(b)
        # `to:` targets can only be checked once every block is known.
        for t in self.all_types():
            for target in t.to:
                if target not in self.blocks:
                    raise SchemaError(f"'to:' names unknown block '{target}'")

    def named_struct(self, name: str, where: str) -> Struct:
        if name in self.structs:
            s = self.structs[name]
            if not s.resolved:
                fail(where, f"struct '{name}' contains itself")
            return s
        if name not in self.struct_docs:
            fail(where, f"unknown type '{name}'")
        swhere, d = self.struct_docs[name]
        s = Struct(name=name, where=f"{swhere} ({name})", named=True)
        self.structs[name] = s
        self.resolve_fields(s.where, s.name, d["fields"], s.fields, chain=[], named=True)
        self.check_rest(s.where, s.fields)
        s.resolved = True
        if "size" in d:
            fs = s.fixed_size()
            if not isinstance(d["size"], int) or fs != d["size"]:
                fail(s.where, f"declared size {d['size']!r} but the fields add up to "
                              f"{fs if fs is not None else 'a variable size'}")
        return s

    def resolve_block(self, b: Block):
        if b.resolved:
            return
        if b.resolving:
            fail(b.where, "inheritance cycle")
        b.resolving = True
        layout: list[Field] = []
        if b.parent is not None:
            parent = self.blocks[b.parent]
            self.resolve_block(parent)
            layout = list(parent.layout)
        own_start = len(layout)
        self.resolve_fields(b.where, b.name, b.raw.get("fields", []), layout, chain=[], named=False)
        b.own = layout[own_start:]
        for f in b.own:
            f.declared_by = b.name
        self.check_rest(b.where, layout)
        b.layout = layout
        b.resolved = True
        b.resolving = False

    def check_rest(self, where: str, fields: list[Field]):
        for f in fields[:-1]:
            if f.type.consumes_rest():
                fail(f"{where}.{f.name}",
                     "only the last field may consume the rest of the block")

    def resolve_fields(self, where: str, owner: str, raw_fields, out: list[Field],
                       chain: list[list[Field]], named: bool):
        """Resolves `raw_fields` into `out` (which may already hold inherited
        fields). `chain` is the enclosing structs' fields, outermost first."""
        if not isinstance(raw_fields, list):
            fail(where, "'fields' must be a list")
        for i, fd in enumerate(raw_fields):
            fwhere = f"{where}.fields[{i}]"
            if not isinstance(fd, dict):
                fail(fwhere, "a field must be a mapping")
            check_keys(fwhere, fd, FIELD_KEYS)
            name = check_field_name(fwhere, fd.get("name"))
            fwhere = f"{where}.{name}"
            if any(f.name == name for f in out):
                fail(fwhere, "duplicate field name (names are unique across the whole layout, "
                             "inherited fields included)")
            scopes = chain + [out]

            if ("type" in fd) == ("fields" in fd):
                fail(fwhere, "a field needs exactly one of 'type' or 'fields'")
            if "array" in fd and "fields" not in fd:
                fail(fwhere, "'array' is only for inline structs ('fields:'); put the suffix in 'type'")

            if "fields" in fd:
                s = Struct(name=f"{owner}{pascal(name)}", where=fwhere, named=False)
                if s.name in self.struct_docs or s.name in self.blocks or \
                        any(x.name == s.name for x in self.inline_structs):
                    fail(fwhere, f"inline struct name '{s.name}' collides with another type")
                self.inline_structs.append(s)
                self.resolve_fields(fwhere, s.name, fd["fields"], s.fields, chain=scopes, named=False)
                if not s.fields:
                    fail(fwhere, "an inline struct needs fields")
                self.check_rest(fwhere, s.fields)
                s.resolved = True
                base = Type(kind="Struct", key=f"struct:{s.name}", struct=s)
                suffix = fd.get("array")
                if suffix is not None and not isinstance(suffix, str):
                    suffix = str(suffix)
                ftype, len_name = self.apply_suffix(fwhere, base, suffix)
            else:
                ftype, len_name = self.parse_type(fwhere, fd["type"], fd, named)

            f = Field(name=name, type=ftype, where=fwhere, doc=fd.get("doc"))

            # Options that belong to particular types.
            base_t = ftype.elem if ftype.kind == "Array" else ftype
            if "to" in fd and base_t.kind not in ("Ref", "Ptr"):
                fail(fwhere, "'to' applies only to ref/ptr")
            if "size" in fd and ftype.kind != "Bytes":
                fail(fwhere, "'size' applies only to bytes")

            if len_name is not None:
                up, idx, target = self.lookup(fwhere, len_name, scopes, named)
                f.len_ref = (up, idx, len_name)
            if "if" in fd:
                c = fd["if"]
                if not isinstance(c, dict) or "field" not in c:
                    fail(fwhere, "'if' must be a mapping with 'field' and one operator")
                ops = [k for k in c if k != "field"]
                if len(ops) != 1 or ops[0] not in COND_OPS:
                    fail(fwhere, f"'if' needs exactly one operator of: {', '.join(COND_OPS)}")
                operand = c[ops[0]]
                if not isinstance(operand, int) or isinstance(operand, bool):
                    fail(fwhere, "'if' operand must be an integer")
                up, idx, _ = self.lookup(fwhere, c["field"], scopes, named)
                f.cond = (COND_OPS[ops[0]], up, idx, operand, c["field"], ops[0])
            out.append(f)

    def lookup(self, where: str, name, scopes: list[list[Field]], named: bool):
        """Nearest earlier field `name`: the innermost scope first, then outward.
        A named struct may only see its own fields."""
        if not isinstance(name, str):
            fail(where, f"field reference must be a field name, got {name!r}")
        search = scopes[-1:] if named else scopes
        for up, scope in enumerate(reversed(search)):
            for idx, f in enumerate(scope):
                if f.name == name:
                    if f.type.kind not in INTEGER_KINDS:
                        fail(where, f"'{name}' must be an integer scalar to be a count, size or condition")
                    if f.cond is not None:
                        fail(where, f"'{name}' is conditional, so it can't be a count, size or condition")
                    return up, idx, f
        if named and any(f.name == name for s in scopes[:-1] for f in s):
            fail(where, f"'{name}' is outside this named struct; only inline structs may "
                        "reference enclosing fields")
        fail(where, f"no earlier field '{name}' to reference")

    def parse_type(self, where: str, spec, fd: dict, named: bool):
        if not isinstance(spec, str):
            fail(where, f"type must be a string, got {spec!r}")
        m = re.fullmatch(r"\s*([A-Za-z][A-Za-z0-9_]*)\s*(?:\[([^\]]*)\])?\s*", spec)
        if not m:
            fail(where, f"can't parse type {spec!r}")
        base_name, suffix = m.group(1), m.group(2)
        if base_name in NOT_YET:
            fail(where, f"'{base_name}' ({NOT_YET[base_name]}) is not supported in language {LANGUAGE_VERSION}")
        to = fd.get("to")
        if to is not None:
            if not isinstance(to, list) or not to or not all(isinstance(x, str) for x in to):
                fail(where, "'to' must be a non-empty list of block names")
        if base_name in SCALARS:
            kind, _, _ = SCALARS[base_name]
            base = Type(kind=kind, key=base_name, spelling=base_name)
        elif base_name in ("ref", "ptr"):
            targets = tuple(sorted(set(to))) if to else ()
            base = Type(kind=pascal(base_name), key=base_name + ("->" + ",".join(targets) if targets else ""),
                        to=targets, spelling=base_name)
        elif base_name == "stringref":
            base = Type(kind="StringRef", key="stringref", spelling="stringref")
        elif base_name == "sizedstring":
            base = Type(kind="SizedString", key="sizedstring", spelling="sizedstring")
        elif base_name == "bytes":
            if suffix is not None:
                fail(where, "bytes takes 'size:', not an array suffix")
            size = fd.get("size")
            if size is None:
                return Type(kind="Bytes", key="bytes", len="Rest", spelling="bytes"), None
            if isinstance(size, int) and not isinstance(size, bool):
                if size < 0:
                    fail(where, "bytes size must be >= 0")
                return Type(kind="Bytes", key=f"bytes:{size}", len="Fixed", count=size,
                            spelling="bytes"), None
            return Type(kind="Bytes", key="bytes:field", len="Field", spelling="bytes"), size
        else:
            base = Type(kind="Struct", key=f"struct:{base_name}",
                        struct=self.named_struct(base_name, where))
        return self.apply_suffix(where, base, suffix)

    def apply_suffix(self, where: str, base: Type, suffix: str | None):
        """Returns (type, length field name or None)."""
        if suffix is None:
            return base, None
        if base.kind == "Bytes":
            fail(where, "bytes can't be an array element")
        if base.consumes_rest():
            fail(where, "an array element can't consume the rest of the block")
        s = suffix.strip()
        if s == "":
            return Type(kind="Array", key=f"{base.key}[]", len="Rest", elem=base), None
        if re.fullmatch(r"\d+", s):
            n = int(s)
            return Type(kind="Array", key=f"{base.key}[{n}]", len="Fixed", count=n, elem=base), None
        m = re.fullmatch(r"count\s*:\s*([A-Za-z][A-Za-z0-9]*)", s)
        if not m:
            fail(where, f"can't parse array suffix [{suffix}]")
        src = m.group(1)
        if src in PREFIX_LEN:
            return Type(kind="Array", key=f"{base.key}[{src}]", len=PREFIX_LEN[src], elem=base), None
        return Type(kind="Array", key=f"{base.key}[field]", len="Field", elem=base), src

    # ── Iteration helpers ─────────────────────────────────────────────────────
    def all_structs(self) -> list[Struct]:
        return list(self.structs.values()) + self.inline_structs

    def all_types(self):
        seen = set()

        def walk(t: Type):
            if id(t) in seen:
                return
            seen.add(id(t))
            yield t
            if t.elem is not None:
                yield from walk(t.elem)
            if t.struct is not None:
                for f in t.struct.fields:
                    yield from walk(f.type)

        for s in self.all_structs():
            for f in s.fields:
                yield from walk(f.type)
        for b in self.blocks.values():
            for f in b.layout:
                yield from walk(f.type)

    def sorted_blocks(self) -> list[Block]:
        return sorted(self.blocks.values(), key=lambda b: b.name.encode("ascii"))

    def lca(self, names: tuple) -> str:
        """The nearest common ancestor block of `names` (NiObject if none)."""
        if not names:
            return "NiObject"

        def chain(n):
            out = []
            while n is not None:
                out.append(n)
                n = self.blocks[n].parent
            return out

        common = chain(names[0])
        for n in names[1:]:
            c = set(chain(n))
            common = [x for x in common if x in c]
        return common[0] if common else "NiObject"


# ── Tables ────────────────────────────────────────────────────────────────────

class Tables:
    def __init__(self, schema: Schema):
        self.schema = schema
        self.blocks = schema.sorted_blocks()
        self.block_index = {b.name: i for i, b in enumerate(self.blocks)}
        self.types: list[dict] = []
        self.type_index: dict[str, int] = {}
        self.fields: list[tuple] = []
        self.links: list[int] = []
        self.link_index: dict[tuple, int] = {}
        self.struct_type: dict[str, int] = {}
        self.block_type: dict[str, int] = {}
        # Named + inline structs first (each once), then each block's layout.
        for s in schema.all_structs():
            self.intern(Type(kind="Struct", key=f"struct:{s.name}", struct=s))
        for b in self.blocks:
            self.block_type[b.name] = self.emit_struct(f"block:{b.name}", b.name, b.layout)

    def intern(self, t: Type) -> int:
        if t.key in self.type_index:
            return self.type_index[t.key]
        if t.kind == "Struct":
            return self.emit_struct(t.key, t.struct.name, t.struct.fields)
        row = {"kind": t.kind, "len": t.len, "count": t.count, "ref": 0, "refCount": 0,
               "linkFirst": 0, "linkCount": 0, "name": None, "comment": t.key}
        if t.kind == "Array":
            row["ref"] = self.intern(t.elem)
        if t.to:
            targets = tuple(self.block_index[n] for n in t.to)
            if targets not in self.link_index:
                self.link_index[targets] = len(self.links)
                self.links.extend(targets)
            row["linkFirst"] = self.link_index[targets]
            row["linkCount"] = len(targets)
        self.type_index[t.key] = len(self.types)
        self.types.append(row)
        return self.type_index[t.key]

    def emit_struct(self, key: str, name: str, fields: list[Field]) -> int:
        if key in self.type_index:
            return self.type_index[key]
        idx = len(self.types)
        self.type_index[key] = idx
        row = {"kind": "Struct", "len": "None", "count": 0, "ref": 0, "refCount": len(fields),
               "linkFirst": 0, "linkCount": 0, "name": name, "comment": key}
        self.types.append(row)
        ftypes = [self.intern(f.type) for f in fields]
        row["ref"] = len(self.fields)
        for f, ft in zip(fields, ftypes):
            len_up, len_field = (f.len_ref[0], f.len_ref[1]) if f.len_ref else (0, None)
            if f.cond:
                op, cup, cidx, cval = f.cond[0], f.cond[1], f.cond[2], f.cond[3]
            else:
                op, cup, cidx, cval = "None", 0, None, 0
            self.fields.append((f.name, ft, len_up, len_field, op, cup, cidx, cval))
        if key.startswith("struct:"):
            self.struct_type[name] = idx
        return idx


def c_str(s: str | None) -> str:
    return "nullptr" if s is None else json.dumps(s)


def field_or_none(i) -> str:
    return "kNoField" if i is None else str(i)


GENERATED_NOTE = """// GENERATED by tools/schemagen from {src} — do not edit. Regenerate with
// `cmake --build <dir> --target niffer-schemagen` (or tools/schemagen/schemagen.py).
"""


def emit_tables(schema: Schema, tables: Tables, ns: str, src: str) -> str:
    out = ["#pragma once", GENERATED_NOTE.format(src=src), "#include <niffer/Schema.h>", "",
           f"namespace {ns} {{", "", "using namespace ::niffer::schema;", ""]
    out.append("inline constexpr TypeDef kTypes[] = {")
    for i, t in enumerate(tables.types):
        out.append(f"    {{ Kind::{t['kind']}, LenKind::{t['len']}, {t['count']}, {t['ref']}, "
                   f"{t['refCount']}, {t['linkFirst']}, {t['linkCount']}, {c_str(t['name'])} }},"
                   f"  // {i}: {t['comment']}")
    out.append("};")
    out.append("")
    out.append("inline constexpr FieldDef kFields[] = {")
    for i, (name, ft, lup, lf, op, cup, cf, cval) in enumerate(tables.fields):
        out.append(f"    {{ {c_str(name)}, {ft}, {lup}, {field_or_none(lf)}, CondOp::{op}, {cup}, "
                   f"{field_or_none(cf)}, {cval} }},  // {i}")
    out.append("};")
    out.append("")
    if tables.links:
        out.append("inline constexpr uint16_t kLinks[] = { " + ", ".join(map(str, tables.links)) + " };")
        links = "kLinks"
    else:
        links = "std::span<const uint16_t>{}"
    out.append("")
    out.append("inline constexpr BlockDef kBlocks[] = {")
    for i, b in enumerate(tables.blocks):
        parent = tables.block_index[b.parent] if b.parent else -1
        own_first = len(b.layout) - len(b.own)
        out.append(f"    {{ {c_str(b.name)}, {parent}, {tables.block_type[b.name]}, {own_first}, "
                   f"{'true' if b.abstract else 'false'} }},  // {i}")
    out.append("};")
    out.append("")
    out.append(f"inline constexpr SchemaSet kSchema{{ kTypes, kFields, {links}, kBlocks, "
               f"{c_str(schema.version)}, {c_str(LANGUAGE_VERSION)} }};")
    out.append("")
    out.append(f"}}  // namespace {ns}")
    out.append("")
    return "\n".join(out)


# ── C++ value types ───────────────────────────────────────────────────────────

class Cpp:
    def __init__(self, schema: Schema, views_ns: str):
        self.schema = schema
        self.views_ns = views_ns

    def value_type(self, t: Type) -> str | None:
        """The C++ type a FieldRef<T> / accessor uses, or None for struct-shaped
        values (reached through views)."""
        for k, cpp, _ in SCALARS.values():
            if t.kind == k:
                return cpp
        if t.kind in ("Ref", "Ptr"):
            return f"::niffer::NiRef<::{self.views_ns}::{self.schema.lca(t.to)}>"
        if t.kind == "StringRef":
            return "::niffer::NiStringRef"
        if t.kind == "SizedString":
            return "std::string"
        if t.kind == "Bytes":
            return "std::vector<uint8_t>"
        if t.kind == "Array":
            e = self.value_type(t.elem)
            return None if e is None else f"std::vector<{e}>"
        return None


def path_init(idx: list[int]) -> str:
    return "{ { " + ", ".join(map(str, idx)) + f" }}, {len(idx)} }}"


def emit_fields(schema: Schema, tables: Tables, ns: str, views_ns: str, src: str) -> str:
    cpp = Cpp(schema, views_ns)
    out = ["#pragma once", GENERATED_NOTE.format(src=src), "#include <niffer/Schema.h>", "",
           "#include <cstdint>", "#include <string>", "#include <vector>", "",
           f"namespace {views_ns} {{"]
    for b in tables.blocks:
        out.append(f"class {b.name};")
    out.append(f"}}  // namespace {views_ns}")
    out.append("")
    out.append(f"namespace {ns} {{")

    def emit(fields: list[Field], base: list[int], start: int, block_idx: int, indent: str):
        for k, f in enumerate(fields):
            idx = base + [start + k]
            if len(idx) > 8:
                raise SchemaError(f"{f.where}: path deeper than 8 levels")
            t = f.type
            if t.kind == "Struct":
                out.append(f"{indent}namespace {ident(f.name)} {{")
                out.append(f"{indent}    inline constexpr ::niffer::StructRef self{{ {block_idx}, {path_init(idx)} }};")
                emit(t.struct.fields, idx, 0, block_idx, indent + "    ")
                out.append(f"{indent}}}")
                continue
            vt = cpp.value_type(t) or "::niffer::Value"
            out.append(f"{indent}inline constexpr ::niffer::FieldRef<{vt}> {ident(f.name)}"
                       f"{{ {block_idx}, {path_init(idx)} }};")

    for i, b in enumerate(tables.blocks):
        if not b.own:
            continue
        out.append("")
        out.append(f"// {b.name}" + (f" : {b.parent}" if b.parent else ""))
        out.append(f"namespace {b.name} {{")
        emit(b.own, [], len(b.layout) - len(b.own), i, "    ")
        out.append("}")
    out.append("")
    out.append(f"}}  // namespace {ns}")
    out.append("")
    return "\n".join(out)


def emit_views(schema: Schema, tables: Tables, ns: str, src: str) -> str:
    cpp = Cpp(schema, ns)
    q = f"::{ns}::"
    out = ["#pragma once", GENERATED_NOTE.format(src=src), "#include <niffer/Schema.h>", "",
           "#include <cstdint>", "#include <optional>", "#include <string>", "#include <vector>", "",
           f"namespace {ns} {{", ""]
    structs = schema.all_structs()
    for s in sorted(structs, key=lambda s: s.name):
        out.append(f"class {s.name};")
    for b in tables.blocks:
        out.append(f"class {b.name};")
    out.append("")

    def accessors(fields: list[Field], start: int) -> list[str]:
        lines = []
        for k, f in enumerate(fields):
            i = start + k
            acc = pascal(f.name)
            p = f"p_.Child({i})"
            t = f.type
            doc = f"    // {f.name}" + (f" — {f.doc.strip().splitlines()[0]}" if f.doc else "")
            lines.append(doc)
            cond = f.cond is not None
            if t.kind == "Struct":
                sv = q + t.struct.name
                if cond:
                    lines.append(f"    std::optional<{sv}> {acc}() const {{ if (!b_->GetValue({p})) return std::nullopt; return {sv}(*b_, {p}); }}")
                else:
                    lines.append(f"    {sv} {acc}() const {{ return {sv}(*b_, {p}); }}")
                continue
            if t.kind == "Array" and t.elem.kind == "Struct":
                sv = q + t.elem.struct.name
                lines.append(f"    std::vector<{sv}> {acc}() const {{")
                lines.append(f"        std::vector<{sv}> out;")
                lines.append(f"        if (const ::niffer::Value* v = b_->GetValue({p}))")
                lines.append(f"            for (uint16_t e = 0; e < v->items.size(); ++e) out.emplace_back(*b_, {p}.Child(e));")
                lines.append("        return out;")
                lines.append("    }")
                continue
            vt = cpp.value_type(t) or "::niffer::Value"
            if cond:
                lines.append(f"    std::optional<{vt}> {acc}() const {{ return b_->GetAt<{vt}>({p}); }}")
            else:
                lines.append(f"    {vt} {acc}() const {{ return b_->GetAt<{vt}>({p}).value_or({vt}{{}}); }}")
            lines.append(f"    bool Set{acc}(const {vt}& v) const {{ return b_->SetAt<{vt}>({p}, v); }}")
        return lines

    # Struct views, dependencies first (a struct view returns nested ones by value).
    emitted: set[str] = set()

    def emit_struct(s: Struct):
        if s.name in emitted:
            return
        emitted.add(s.name)
        for f in s.fields:
            t = f.type.elem if f.type.kind == "Array" else f.type
            if t.kind == "Struct":
                emit_struct(t.struct)
        out.append(f"class {s.name} {{")
        out.append("public:")
        out.append(f"    {s.name}(::niffer::Block& b, ::niffer::RelPath p) : b_(&b), p_(p) {{}}")
        out.append("    ::niffer::Block& block() const { return *b_; }")
        out.append("    ::niffer::RelPath path() const { return p_; }")
        out.extend(accessors(s.fields, 0))
        out.append("private:")
        out.append("    ::niffer::Block* b_;")
        out.append("    ::niffer::RelPath p_;")
        out.append("};")
        out.append("")

    for s in sorted(structs, key=lambda s: s.name):
        emit_struct(s)

    # Block views, parents first.
    done: set[str] = set()

    def emit_block(b: Block):
        if b.name in done:
            return
        if b.parent:
            emit_block(schema.blocks[b.parent])
        done.add(b.name)
        bi = tables.block_index[b.name]
        base = f" : public {b.parent}" if b.parent else ""
        out.append(f"class {b.name}{base} {{")
        out.append("public:")
        out.append(f"    static constexpr uint16_t kBlock = {bi};")
        out.append(f"    static std::optional<{b.name}> From(::niffer::Block& b) {{")
        out.append(f"        if (b.Modeled() && b.IsA(kBlock)) return {b.name}(b);")
        out.append("        return std::nullopt;")
        out.append("    }")
        if not b.parent:
            out.append("    ::niffer::Block& block() const { return *b_; }")
        out.extend(accessors(b.own, len(b.layout) - len(b.own)))
        out.append("protected:")
        if b.parent:
            out.append(f"    explicit {b.name}(::niffer::Block& b) : {b.parent}(b) {{}}")
        else:
            out.append(f"    explicit {b.name}(::niffer::Block& b) : b_(&b) {{}}")
            out.append("    ::niffer::Block* b_;")
            out.append("    ::niffer::RelPath p_{};")
        out.append("};")
        out.append("")

    for b in tables.blocks:
        emit_block(b)
    out.append(f"}}  // namespace {ns}")
    out.append("")
    return "\n".join(out)


# ── schema.json ───────────────────────────────────────────────────────────────

def type_json(t: Type) -> dict:
    if t.kind == "Struct":
        return {"kind": "struct", "name": t.struct.name,
                "fields": [field_json(f) for f in t.struct.fields]}
    if t.kind == "Array":
        d = {"kind": "array", "element": type_json(t.elem)}
        if t.len == "Fixed":
            d["count"] = t.count
        elif t.len.startswith("Prefix"):
            d["countPrefix"] = {"Prefix8": "u8", "Prefix16": "u16", "Prefix32": "u32"}[t.len]
        return d
    if t.kind == "Bytes":
        d = {"kind": "bytes"}
        if t.len == "Fixed":
            d["size"] = t.count
        return d
    d = {"kind": t.spelling or t.kind.lower()}
    if t.to:
        d["to"] = list(t.to)
    return d


def field_json(f: Field) -> dict:
    d = {"name": f.name, "type": type_json(f.type)}
    if f.len_ref:
        key = "sizeField" if f.type.kind == "Bytes" else "countField"
        d["type"][key] = f.len_ref[2]
    if f.cond:
        d["if"] = {"field": f.cond[4], f.cond[5]: f.cond[3]}
    if f.declared_by:
        d["declaredBy"] = f.declared_by
    if f.doc:
        d["doc"] = f.doc.strip()
    return d


def emit_json(schema: Schema, tables: Tables) -> str:
    doc = {
        "schemaVersion": schema.version,
        "languageVersion": LANGUAGE_VERSION,
        "structs": [{"name": s.name, "fields": [field_json(f) for f in s.fields]}
                    for s in sorted(schema.structs.values(), key=lambda s: s.name)],
        "blocks": [],
    }
    for b in tables.blocks:
        d = {"name": b.name, "parent": b.parent, "abstract": b.abstract,
             "layout": [field_json(f) for f in b.layout]}
        if b.verified:
            d["verified"] = b.verified
        if b.doc:
            d["doc"] = b.doc.strip()
        doc["blocks"].append(d)
    return json.dumps(doc, indent=1) + "\n"


# ── Main ──────────────────────────────────────────────────────────────────────

def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--schema", required=True, type=Path)
    ap.add_argument("--out", required=True, type=Path, help="directory for Fields.h/Views.h")
    ap.add_argument("--tables", required=True, type=Path, help="path for Tables.h")
    ap.add_argument("--json", type=Path, help="path for the schema.json bundle")
    ap.add_argument("--tables-ns", default="niffer::schema::tables")
    ap.add_argument("--fields-ns", default="niffer::fields")
    ap.add_argument("--views-ns", default="niffer::view")
    ap.add_argument("--check", action="store_true", help="verify outputs are up to date")
    args = ap.parse_args(argv)

    schema = Schema()
    try:
        schema.load(args.schema)
        schema.build()
        tables = Tables(schema)
        # Name the schema relative to the repo, so the output doesn't depend on
        # how (or from where) the generator was invoked.
        repo = Path(__file__).resolve().parents[2]
        try:
            src = args.schema.resolve().relative_to(repo).as_posix() + "/"
        except ValueError:
            src = args.schema.name + "/"
        outputs = {
            args.tables: emit_tables(schema, tables, args.tables_ns, src),
            args.out / "Fields.h": emit_fields(schema, tables, args.fields_ns, args.views_ns, src),
            args.out / "Views.h": emit_views(schema, tables, args.views_ns, src),
        }
        if args.json:
            outputs[args.json] = emit_json(schema, tables)
    except SchemaError as e:
        print(f"schemagen: error: {e}", file=sys.stderr)
        return 2

    stale = []
    for path, text in outputs.items():
        current = path.read_text(encoding="utf-8") if path.exists() else None
        if current == text:
            continue
        if args.check:
            stale.append(path)
        else:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8", newline="\n")
            print(f"schemagen: wrote {path}")
    if stale:
        for p in stale:
            print(f"schemagen: stale: {p}", file=sys.stderr)
        print("schemagen: run the niffer-schemagen target and commit the result", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
