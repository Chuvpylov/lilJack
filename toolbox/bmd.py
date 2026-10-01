# toolbox/bmd.py (vendored from the the trainer project, corpus/bmd.py; stdlib only)
"""BMD reader/writer — the knowledge store's knowledge format, from the trainer side.

Deliberately a *reimplementation of the format*, not of the knowledge store. the knowledge store owns the
brain (HDF5) and the oracle; this module only reads and writes `.bmd` text so
training data can be authored, converted and ingested as locons. Files produced
here must open unchanged in the knowledge store.

Structure
    [[header]]      key: value lines (document metadata)
    [[/header]]
    [[pasem: name]] a named, individually-referenceable section
      [[footer]]    assertions scoped to that pasem
      [[/footer]]
    [[/pasem]]
    [[footer]]      document-level assertions
    [[/footer]]

Assertions — five forms (the knowledge store logic-dsl v0.2):
    fact(<pred>, <subj> [, <obj>]) [| confidence=0..1] [| source=] [| ts=]
    neg(<pred>, <subj> [, <obj>])  [| confidence=0..1]
    stmt(<pred>, <subj> [, <obj>]) | actor= | trust=0..1 [| ts=]
    rule(<head>) :- <body1>, <body2>, …            [| weight=0..1]
    edge(<relation>, <from>, <to>)

Parse-or-reject is the point: a malformed assertion raises rather than being
silently coerced, so a model's proposal is a syntax error instead of a plausible
falsehood. `parse_assertion` returns None only for blank/comment lines.

Pure stdlib — the Den imports this too.
"""
from __future__ import annotations

import re
from dataclasses import dataclass, field

FORMS = ("fact", "neg", "stmt", "rule", "edge")

# The DSL's own terminals, reserved as single tokens from spm_v5 on, so that
# grammar-constrained decoding (parse_assertion(canonical_only=True)) can mask
# on a terminal the model can actually emit in one step.
#
# ⚠ Only DISTINCTIVE terminals belong here. `|` and `=>` were tried and dropped:
# they are ubiquitous in markdown tables and ordinary code, so reserving them
# spends vocab to pin a token that was already one token, and forces that
# tokenisation everywhere else in the corpus. The `form(` openers and `:-` are
# rare enough outside a claim that pinning them is nearly free.
DSL_TERMINALS = [f"{f}(" for f in FORMS] + [":-", "⟦DSL⟧", "⟦/DSL⟧"]

_HEADER_OPEN = "[[header]]"
_HEADER_CLOSE = "[[/header]]"
_FOOTER_OPEN = "[[footer]]"
_FOOTER_CLOSE = "[[/footer]]"
_PASEM_OPEN = re.compile(r"^\[\[pasem:\s*([^\]]+?)\s*\]\]\s*$")
_PASEM_CLOSE = "[[/pasem]]"
_BLOCK_ANY = re.compile(r"^\[\[/?[a-z]+")


class BmdError(ValueError):
    """Malformed BMD or assertion. Raised, never swallowed."""


# ── assertions ───────────────────────────────────────────────────────────────

@dataclass
class Assertion:
    form: str                      # fact | neg | stmt | rule | edge
    pred: str = ""                 # predicate / relation; for rule: the head
    args: list = field(default_factory=list)
    attrs: dict = field(default_factory=dict)
    body: list = field(default_factory=list)   # rule only: body terms

    def render(self) -> str:
        """Emit attributes INSIDE the parens — that is what the knowledge store's own files
        do, and what the knowledge store reads back. Rules keep attrs after the body."""
        attrs = "".join(f" | {k}={v}" for k, v in self.attrs.items())
        if self.form == "rule":
            s = f"rule({self.pred})"
            if self.body:
                s += " :- " + ", ".join(self.body)
            return s + attrs
        inner = ", ".join([self.pred] + [str(a) for a in self.args])
        return f"{self.form}({inner}{attrs})"

    @property
    def canonical(self) -> bool:
        """True if this is one of the five documented DSL forms."""
        return self.form in FORMS

    def key(self) -> tuple:
        """Identity for dedup: form + predicate + args (attrs excluded)."""
        return (self.form, self.pred, tuple(str(a) for a in self.args))


_ATTR_SPLIT = re.compile(r"\s*\|\s*")
# greedy inner so it binds to the LAST ')' — attributes live inside the parens
# in real the knowledge store files, so a non-greedy match would truncate the arg list.
# [a-z_] because real braids also contain calls like wiki_extract(...).
_CALL = re.compile(r"^([a-z_]+)\s*\((.*)\)\s*(.*)$", re.S)


def _split_top(s: str, sep: str = ",") -> list:
    """Split on `sep` at paren depth 0 and outside quotes.

    Quote-awareness is required, not cosmetic: free text carries commas
    ("establish ring level from physical accessibility, not capability") and
    would otherwise be shredded into extra arguments. the knowledge store quotes such
    strings too — see wiki_extract("...", "...").
    """
    out, depth, cur, quote = [], 0, "", None
    for ch in s:
        if quote:
            cur += ch
            if ch == quote:
                quote = None
            continue
        if ch in "\"'" and _opens_quote(cur):
            quote = ch
            cur += ch
            continue
        if ch in "([":
            depth += 1
        elif ch in ")]":
            depth -= 1
        if ch == sep and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def _opens_quote(cur: str) -> bool:
    """A quote character starts a quoted value only at the START of a segment
    or right after `=` — never inside a word. Otherwise the apostrophe in
    `en=I'm fine | uk=…` opens a quote that never closes and swallows every
    attribute after it (found 2026-08-27 in the trio dictionary)."""
    t = cur.rstrip()
    return t == "" or t.endswith("=") or t.endswith(",")


def qval(s) -> str:
    """Quote a value if it carries a separator the grammar uses."""
    t = str(s).replace("\n", " ").strip()
    if any(c in t for c in ',|"'):
        return '"' + t.replace('"', "'") + '"'
    if "'" in t:
        return '"' + t + '"'          # an apostrophe mid-word must not read as a quote
    return t


def _take_attrs(text: str):
    """Split ' | k=v | k=v' off the end of `text`. Returns (rest, attrs).

    Attributes live INSIDE the parens in every real the knowledge store file —
    `fact(builds, acme, rockets | confidence=0.95 | source=press)` — even though
    logic-dsl.bmd documents them outside. Both are accepted; the files are the
    ground truth and are what we must round-trip.
    """
    # Quote-aware, for the same reason arg splitting is: qval() quotes any value
    # containing '|', and a raw split on '|' would shred it back apart —
    # match="comput|software" became two attributes, the second one junk.
    # Empty segments are kept here (unlike _split_top): an empty leading segment
    # means "no body, attributes only", and dropping it would promote the first
    # attribute into the body slot.
    parts = _split_keep_empty(text, "|")
    attrs = {}
    for a in parts[1:]:
        k, sep, v = a.partition("=")
        if not sep or not k.strip():
            raise BmdError(f"bad attribute {a!r}")
        attrs[k.strip()] = _unquote(v.strip())
    return parts[0].strip(), attrs


def _split_keep_empty(s: str, sep: str) -> list:
    """_split_top, but empty segments survive. See _take_attrs for why."""
    out, depth, cur, quote = [], 0, "", None
    for ch in s:
        if quote:
            cur += ch
            if ch == quote:
                quote = None
            continue
        if ch in "\"'" and _opens_quote(cur):
            quote = ch
            cur += ch
            continue
        if ch in "([":
            depth += 1
        elif ch in ")]":
            depth -= 1
        if ch == sep and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    out.append(cur.strip())
    return out


def _unquote(v: str) -> str:
    if len(v) >= 2 and v[0] == v[-1] and v[0] in "\"'":
        return v[1:-1]
    return v


def parse_assertion(line: str, canonical_only: bool = False):
    """One assertion line → Assertion. None for blank/comment. Raises on junk.

    `canonical_only=True` additionally rejects forms outside FORMS — use it to
    validate MODEL output (parse-or-reject). Reading the knowledge store files leaves it
    False, because real braids also carry calls like wiki_extract(...).
    """
    line = line.strip()
    if not line or line.startswith(("#", "//", "%")):
        return None

    # rule: rule(head) :- body1, body2 [| attrs]   (attrs may sit either side)
    if line.startswith("rule("):
        head_part, sep, rest = line.partition(":-")
        if sep:
            body_txt, attrs = _take_attrs(rest)
        else:
            head_part, body_txt, attrs = head_part, "", {}
        m = _CALL.match(head_part.strip())
        if not m or m.group(1) != "rule":
            raise BmdError(f"malformed rule head: {line!r}")
        inner, inner_attrs = _take_attrs(m.group(2).strip())
        trailing = m.group(3).strip()
        if trailing:
            _, tail_attrs = _take_attrs("x " + trailing)
            inner_attrs = {**inner_attrs, **tail_attrs}
        attrs = {**inner_attrs, **attrs}
        if not inner:
            raise BmdError(f"rule with empty head: {line!r}")
        body = _split_top(body_txt) if body_txt else []
        return Assertion("rule", pred=inner, body=body, attrs=attrs)

    m = _CALL.match(line)
    if not m:
        raise BmdError(f"not an assertion: {line!r}")
    form, inner, trailing = m.group(1), m.group(2), m.group(3).strip()
    inner, inner_attrs = _take_attrs(inner)     # real form: attrs before ')'
    outer_attrs = {}
    if trailing:                                # spec form: attrs after ')'
        _, outer_attrs = _take_attrs("x " + trailing)
    attrs = {**inner_attrs, **outer_attrs}

    if canonical_only and form not in FORMS:
        raise BmdError(f"unknown form {form!r} (expected one of {FORMS})")
    args = _split_top(inner)
    if not args or not args[0]:
        raise BmdError(f"{form} needs a predicate: {line!r}")
    pred, rest_args = args[0], args[1:]
    if form == "edge" and len(rest_args) != 2:
        raise BmdError(f"edge needs exactly (relation, from, to): {line!r}")
    if form in ("fact", "neg", "stmt") and not 1 <= len(rest_args) <= 2:
        raise BmdError(f"{form} needs (pred, subj[, obj]): {line!r}")
    return Assertion(form, pred=pred, args=rest_args, attrs=attrs)


# ── documents ────────────────────────────────────────────────────────────────

@dataclass
class Pasem:
    name: str
    text: str = ""
    assertions: list = field(default_factory=list)


@dataclass
class BmdDoc:
    header: dict = field(default_factory=dict)
    pasems: list = field(default_factory=list)
    assertions: list = field(default_factory=list)   # document-level footer
    preamble: str = ""                               # content before any block

    # -- access ---------------------------------------------------------------
    def pasem(self, name: str):
        return next((p for p in self.pasems if p.name == name), None)

    def all_assertions(self) -> list:
        out = list(self.assertions)
        for p in self.pasems:
            out += p.assertions
        return out

    def facts(self, pred: str | None = None) -> list:
        return [a for a in self.all_assertions()
                if a.form == "fact" and (pred is None or a.pred == pred)]

    def edges(self, rel: str | None = None) -> list:
        return [a for a in self.all_assertions()
                if a.form == "edge" and (rel is None or a.pred == rel)]

    def prose(self) -> str:
        """Human-readable body: preamble + every pasem's text."""
        parts = [self.preamble.strip()] if self.preamble.strip() else []
        parts += [p.text.strip() for p in self.pasems if p.text.strip()]
        return "\n\n".join(parts)

    # -- render ---------------------------------------------------------------
    def render(self) -> str:
        out = []
        if self.header:
            out.append(_HEADER_OPEN)
            for k, v in self.header.items():
                out.append(f"{k}: {v}")
            out.append(_HEADER_CLOSE)
            out.append("")
        if self.preamble.strip():
            out.append(self.preamble.strip())
            out.append("")
        for p in self.pasems:
            out.append(f"[[pasem: {p.name}]]")
            if p.text.strip():
                out.append(p.text.strip())
            if p.assertions:
                out.append(_FOOTER_OPEN)
                out += [a.render() for a in p.assertions]
                out.append(_FOOTER_CLOSE)
            out.append(_PASEM_CLOSE)
            out.append("")
        if self.assertions:
            out.append(_FOOTER_OPEN)
            out += [a.render() for a in self.assertions]
            out.append(_FOOTER_CLOSE)
        return "\n".join(out).rstrip() + "\n"


def parse(text: str, strict: bool = True) -> BmdDoc:
    """Parse BMD source. strict=False downgrades bad assertions to skips."""
    doc = BmdDoc()
    lines = text.splitlines()
    i, n = 0, len(lines)
    pre = []
    cur_pasem = None
    in_fence = False

    while i < n:
        raw = lines[i]
        line = raw.strip()

        # never interpret block markers inside a ``` code fence
        if line.startswith("```"):
            in_fence = not in_fence
            if cur_pasem is not None:
                cur_pasem.text += raw + "\n"
            else:
                pre.append(raw)
            i += 1
            continue
        if in_fence:
            if cur_pasem is not None:
                cur_pasem.text += raw + "\n"
            else:
                pre.append(raw)
            i += 1
            continue

        if line == _HEADER_OPEN:
            i += 1
            while i < n and lines[i].strip() != _HEADER_CLOSE:
                k, sep, v = lines[i].partition(":")
                if sep and k.strip():
                    doc.header[k.strip()] = v.strip().strip('"')
                i += 1
            i += 1
            continue

        m = _PASEM_OPEN.match(line)
        if m:
            cur_pasem = Pasem(name=m.group(1))
            doc.pasems.append(cur_pasem)
            i += 1
            continue

        if line == _PASEM_CLOSE:
            cur_pasem = None
            i += 1
            continue

        if line == _FOOTER_OPEN:
            i += 1
            target = cur_pasem.assertions if cur_pasem is not None else doc.assertions
            while i < n and lines[i].strip() != _FOOTER_CLOSE:
                try:
                    a = parse_assertion(lines[i])
                except BmdError:
                    if strict:
                        raise
                    a = None
                if a is not None:
                    target.append(a)
                i += 1
            i += 1
            continue

        if cur_pasem is not None:
            cur_pasem.text += raw + "\n"
        else:
            pre.append(raw)
        i += 1

    doc.preamble = "\n".join(pre).strip()
    return doc


def load(path, strict: bool = True) -> BmdDoc:
    from pathlib import Path
    return parse(Path(path).read_text(errors="replace"), strict=strict)


def save(doc: BmdDoc, path):
    from pathlib import Path
    p = Path(path)
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(doc.render())
    return p


# ── helpers for building documents ───────────────────────────────────────────

def fact(pred, *args, **attrs) -> Assertion:
    return Assertion("fact", pred, list(args), _clean(attrs))


def neg(pred, *args, **attrs) -> Assertion:
    return Assertion("neg", pred, list(args), _clean(attrs))


def stmt(pred, *args, **attrs) -> Assertion:
    return Assertion("stmt", pred, list(args), _clean(attrs))


def edge(relation, frm, to) -> Assertion:
    return Assertion("edge", relation, [frm, to], {})


def rule(head, body=None, **attrs) -> Assertion:
    return Assertion("rule", head, [], _clean(attrs), list(body or []))


def _clean(attrs: dict) -> dict:
    """Drop Nones and normalise trailing-underscore keys (ts_ → ts)."""
    return {k.rstrip("_"): v for k, v in attrs.items() if v is not None}


def slug(text: str, maxlen: int = 48) -> str:
    """Filesystem/predicate-safe slug — the knowledge store slugs are lowercase-hyphenated."""
    s = re.sub(r"[^a-zA-Z0-9]+", "-", str(text)).strip("-").lower()
    return (s[:maxlen].rstrip("-")) or "unnamed"
