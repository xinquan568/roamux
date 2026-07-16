#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Roamux string-rebrand channel (execution plan §12.2 governed channel; roam-132).

Rebrands user-visible "Chromium" -> "Roamux" across the upstream GRIT string
sources (grd/grdp) and their locale .xtb translations, then RE-KEYS every
rebranded message's xtb id so existing translations stay bound. This is a
declared, governed BUILD channel — like apply_patches.py / fetch_sparkle.py —
NOT a stack of per-string patches (there are far too many strings across 80+
locales to hand-patch). It runs at build time (after apply_patches.py, before
the grit/resource compile) and the release workflow gates on the result.

Why re-key: GRIT derives a message's numeric id from a hash of its PRESENTABLE
source text (+ meaning), unless ``use_name_for_id`` is set. Substituting
"Chromium"->"Roamux" changes that id, so the existing .xtb translations (keyed
by the OLD id) silently stop binding and the message reverts to English. So for
every rebranded message we compute the NEW id via GRIT's own authoritative path
(``tclib.GenerateMessageId`` over the rebranded presentable content) and re-key
the matching xtb entries — proven bound by the fixtures.

Properties (mirroring apply_patches.py's contract):
  * structure-aware — substitutes only within translatable message content (grd
    ``<message>`` text runs; xtb ``<translation>`` text), never in attributes,
    ``name=``/ids, ``<ph>`` placeholder names, or comments.
  * idempotent — an already-rebranded tree is a strict no-op (no "Roamuxium").
  * fail-loud — any parse error / structural anomaly aborts, naming the file.
  * xtb-first ordering — xtb re-key precedes the grd edit so an interrupted run
    is completed (never left with a rebranded grd and detached translations).

Exclusions (legal/attribution, ChromeOS, domains, URLs, code/histogram/policy
identifiers) are versioned and test-pinned in rebrand_exclusions.py.

Usage:
  rebrand_strings.py --chromium-src ~/chromium/src [--check]

``--check`` reports (non-zero) whether a rebrand is pending; mutates nothing.
"""

import argparse
import collections
import os
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import rebrand_exclusions as _excl

# Re-exported so callers/tests share ONE substitution + exclusion policy.
guarded_substitute = _excl.guarded_substitute
is_message_excluded = _excl.is_message_excluded

# The upstream targets (relative to --chromium-src). grd_reader follows each
# grd's ``<part file=...>`` grdp includes; the physical grdp/xtb files are
# discovered from the grd itself (discover_grdp_files / discover_xtb_files).
# The *_chromium_strings.grd units are the Chromium-BRANDED string sources (their
# google_chrome_strings.grd siblings ship only in official Google builds, which
# Roamux never produces) — they carry the bulk of the literal product mentions.
TARGET_GRDS = (
    "chrome/app/chromium_strings.grd",
    "chrome/app/generated_resources.grd",
    "components/components_strings.grd",
    "components/components_chromium_strings.grd",
    "components/privacy_sandbox_strings.grd",
)


def rebrand_text(text):
    """Public alias for the guarded token substitution. Returns (new, changed)."""
    return _excl.guarded_substitute(text)


def _import_grit(chromium_src):
    """Import GRIT read-only from the checkout. Returns (tclib, grd_reader)."""
    grit_path = os.path.join(str(chromium_src), "tools", "grit")
    if grit_path not in sys.path:
        sys.path.insert(0, grit_path)
    from grit.extern import tclib
    from grit import grd_reader
    return tclib, grd_reader


# ---------------------------------------------------------------------------
# grd/grdp: structure-aware, streaming rewrite of translatable text runs only.
# ---------------------------------------------------------------------------
_GRD_TOKEN = re.compile(r'<!--.*?-->|<[^>]*>|[^<]+', re.DOTALL)
_OPEN_NAME = re.compile(r'<\s*([\w:.-]+)')
_CLOSE_NAME = re.compile(r'</\s*([\w:.-]+)')
_ATTR_NAME = re.compile(r'\bname\s*=\s*"([^"]*)"')


def rewrite_grd_text(raw):
    """Rebrand only the translatable text inside non-excluded ``<message>``
    elements, skipping ``<ph>`` subtrees, comments, tags and attributes.

    Returns ``(new_raw, n_messages_changed)``. Pure — no GRIT needed.
    """
    out = []
    changed = 0
    msg_active = False
    msg_excluded = False
    ph_depth = 0
    for m in _GRD_TOKEN.finditer(raw):
        tok = m.group(0)
        if tok.startswith("<!--"):
            out.append(tok)                      # comment — verbatim
            continue
        if tok.startswith("<"):
            out.append(tok)                      # tag — verbatim
            if tok.startswith("<?") or tok.startswith("<!"):
                continue                         # declaration / doctype
            self_closing = tok.rstrip().endswith("/>")
            if tok.startswith("</"):
                cm = _CLOSE_NAME.match(tok)
                name = cm.group(1) if cm else ""
                if name == "message":
                    msg_active, msg_excluded, ph_depth = False, False, 0
                elif name == "ph" and ph_depth > 0:
                    ph_depth -= 1
            elif not self_closing:
                om = _OPEN_NAME.match(tok)
                name = om.group(1) if om else ""
                if name == "message":
                    am = _ATTR_NAME.search(tok)
                    msg_active = True
                    msg_excluded = _excl.is_message_excluded(am.group(1) if am else "")
                    ph_depth = 0
                elif name == "ph":
                    ph_depth += 1
            continue
        # Text run.
        if msg_active and not msg_excluded and ph_depth == 0:
            new, did = _excl.guarded_substitute(tok)
            if did:
                changed += 1
            out.append(new)
        else:
            out.append(tok)
    return "".join(out), changed


# ---------------------------------------------------------------------------
# xtb: re-key mapped ids + rebrand their translation text (structure-aware).
# ---------------------------------------------------------------------------
_TRANS_RE = re.compile(
    r'(<translation\b[^>]*?\bid=")([^"]*)("[^>]*?>)(.*?)(</translation>)',
    re.DOTALL)
_TRANS_ID_RE = re.compile(r'<translation\b[^>]*?\bid="([^"]*)"')
_XTB_TAG = re.compile(r'<[^>]*>', re.DOTALL)


def _rebrand_xtb_content(content):
    """Rebrand only the text nodes of a translation, leaving ``<ph .../>`` /
    ``<branch>`` markup and XML entities intact."""
    out = []
    last = 0
    for tm in _XTB_TAG.finditer(content):
        out.append(_excl.guarded_substitute(content[last:tm.start()])[0])
        out.append(tm.group(0))
        last = tm.end()
    out.append(_excl.guarded_substitute(content[last:])[0])
    return "".join(out)


def rewrite_xtb_text(raw, id_map):
    """Re-key every ``<translation id=...>`` whose OLD id is in ``id_map`` to its
    new id and rebrand its text. Entries not in the map are untouched.

    Returns ``(new_raw, n_entries_changed)``. Fails loud on a re-key that would
    duplicate an existing id. Pure — no GRIT needed."""
    changed = 0

    def repl(m):
        nonlocal changed
        old_id = m.group(2)
        if old_id not in id_map:
            return m.group(0)
        changed += 1
        return (m.group(1) + id_map[old_id] + m.group(3)
                + _rebrand_xtb_content(m.group(4)) + m.group(5))

    new_raw = _TRANS_RE.sub(repl, raw)
    ids = _TRANS_ID_RE.findall(new_raw)
    if len(ids) != len(set(ids)):
        dupes = sorted({i for i in ids if ids.count(i) > 1})
        raise ValueError(f"xtb re-key produced duplicate translation id(s): {dupes}")
    return new_raw, changed


# ---------------------------------------------------------------------------
# Message enumeration (authoritative ids via GRIT) + file discovery.
# ---------------------------------------------------------------------------
class _PermissiveDefines(dict):
    """A grit ``defines`` map that reports EVERY name as defined (unknown -> False).

    The real grds gate ``<part>``/``<if>`` on many build variables (use_titlecase,
    _google_chrome, enable_*, ...) whose values come from ``gn gen`` at compile
    time; GRIT asserts on any undefined name. We enumerate EVERY message across
    ALL branches (a superset — see compute_id_map), so the branch a variable
    selects is irrelevant to us; we only need Parse not to abort. Real platform
    names (is_win/is_macosx/...) are handled by GRIT before this map is consulted.
    """

    def __contains__(self, key):
        return True

    def __missing__(self, key):
        return False


def compute_id_map(grd_path, chromium_src, target_platform="darwin"):
    """Walk a grd's messages via GRIT (following grdp includes) and return the
    ``{old_id: new_id}`` map for messages that rebrand. Excluded messages and
    no-op messages (nothing to rebrand) are omitted. ``use_name_for_id``
    messages keep their id (the map still lists them so their xtb TEXT rebrands).

    Enumeration is a PREORDER walk over every ``<if>`` branch (not just the ones
    active for ``target_platform``): xtb ids are global content hashes shared
    across platforms, so re-keying the superset never misses an active message's
    translation, and inactive-branch messages (no rebrand, or no xtb) are inert.
    """
    tclib, grd_reader = _import_grit(chromium_src)
    grd_path = pathlib.Path(grd_path)
    root = grd_reader.Parse(str(grd_path), dir=str(grd_path.parent),
                            defines=_PermissiveDefines(),
                            target_platform=target_platform,
                            skip_validation_checks=True)

    id_map = {}
    for node in root:                            # preorder: all branches
        if node.name != "message":
            continue
        if _excl.is_message_excluded(node.attrs.get("name", "")):
            continue
        cliques = node.GetCliques()
        if not cliques:
            continue
        msg = cliques[0].GetMessage()
        presentable = msg.GetPresentableContent()
        new_presentable, did = _excl.guarded_substitute(presentable)
        if not did:
            continue
        old_id = msg.GetId()
        if node.attrs.get("use_name_for_id") == "true":
            new_id = old_id                      # id derives from name, not text
        else:
            new_id = tclib.GenerateMessageId(new_presentable, msg.GetMeaning())
        id_map[old_id] = new_id
    return id_map


_PART_RE = re.compile(r'<part\b[^>]*\bfile="([^"]+)"')
_XTB_FILE_RE = re.compile(r'<file\b[^>]*\bpath="([^"]+\.xtb)"')


def discover_grdp_files(grd_path):
    """Physical grdp files reachable from a grd via ``<part file=...>`` (recursive)."""
    grd_path = pathlib.Path(grd_path)
    found = []
    scanned = set()
    stack = [grd_path]
    while stack:
        f = stack.pop()
        if f in scanned or not f.is_file():
            continue
        scanned.add(f)
        raw = f.read_text(encoding="utf-8")
        for rel in _PART_RE.findall(raw):
            p = (f.parent / rel).resolve()
            if p.suffix == ".grdp" and p not in found:
                found.append(p)
                stack.append(p)
    return found


def discover_xtb_files(grd_path):
    """Physical xtb files a grd references via ``<translations><file path=...>``."""
    grd_path = pathlib.Path(grd_path)
    raw = grd_path.read_text(encoding="utf-8")
    return [(grd_path.parent / rel).resolve() for rel in _XTB_FILE_RE.findall(raw)]


Result = collections.namedtuple("Result", "would_change changed_files id_map")


def run_on_grd_unit(grd_path, chromium_src, check=False):
    """Rebrand one grd + its grdp includes + its xtb locales as a unit.

    ``check=True`` computes what WOULD change and mutates nothing. Otherwise the
    xtb files are re-keyed FIRST (interruption-resilient), then the grd/grdps."""
    grd_path = pathlib.Path(grd_path)
    grd_files = [grd_path] + discover_grdp_files(grd_path)
    xtb_files = [f for f in discover_xtb_files(grd_path) if f.is_file()]
    id_map = compute_id_map(grd_path, chromium_src)

    would_change = False
    for f in grd_files:
        raw = f.read_text(encoding="utf-8")
        if rewrite_grd_text(raw)[0] != raw:
            would_change = True
    if id_map:
        for f in xtb_files:
            raw = f.read_text(encoding="utf-8")
            if rewrite_xtb_text(raw, id_map)[0] != raw:
                would_change = True

    if check:
        return Result(would_change, [], id_map)

    changed_files = []
    if id_map:
        for f in xtb_files:
            raw = f.read_text(encoding="utf-8")
            new, _ = rewrite_xtb_text(raw, id_map)
            if new != raw:
                f.write_text(new, encoding="utf-8")
                changed_files.append(f)
    for f in grd_files:
        raw = f.read_text(encoding="utf-8")
        new, _ = rewrite_grd_text(raw)
        if new != raw:
            f.write_text(new, encoding="utf-8")
            changed_files.append(f)
    return Result(would_change, changed_files, id_map)


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--chromium-src", required=True, type=pathlib.Path,
                        help="path to the Chromium src/ checkout")
    parser.add_argument("--check", action="store_true",
                        help="report pending rebrand (non-zero); mutate nothing")
    args = parser.parse_args()

    src = args.chromium_src
    if not src.is_dir():
        print(f"FAIL: --chromium-src {src} is not a directory", file=sys.stderr)
        return 1

    pending = False
    for rel in TARGET_GRDS:
        grd = src / rel
        if not grd.is_file():
            print(f"FAIL: target grd missing: {grd}", file=sys.stderr)
            print("The rebrand channel runs against a synced Chromium checkout "
                  "(§12.2).", file=sys.stderr)
            return 1
        try:
            result = run_on_grd_unit(grd, src, check=args.check)
        except Exception as e:  # noqa: BLE001 — fail loud, name the file
            print(f"FAIL: {grd}: {e}", file=sys.stderr)
            print("Rebase or fix the rebrand channel (§12.2: the channel fails "
                  "loudly on any structural change).", file=sys.stderr)
            return 1
        if args.check:
            if result.would_change:
                pending = True
                print(f"[pending]   {rel} — rebrand not applied")
            else:
                print(f"[ok]        {rel} — already rebranded")
        elif result.changed_files:
            print(f"[rebrand]   {rel} — {len(result.changed_files)} file(s) updated")
        else:
            print(f"[ok]        {rel} — already rebranded")

    if args.check and pending:
        print("FAIL: user-visible strings still read 'Chromium' — run "
              "rebrand_strings.py before the resource compile.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
