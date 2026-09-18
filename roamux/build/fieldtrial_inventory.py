#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Per-pin field-trial feature inventory — the ADR 0002 obligation (roam-342).

ADR 0002 disables Chromium's field-trial testing config in every Roamux build and requires, at every
pin bump, an inventory of what that config would have flipped: "one script over the config JSON + a
``BASE_FEATURE`` harvest", attached to the uprev record. This is that script. The original was never
committed; this one was rebuilt (roam-342) against the surviving M149 output and reproduces issue
#241's published inventory exactly — that reproduction is its acceptance test
(``roamux/build/tests/test_fieldtrial_inventory.py``, opt-in ``REQUIRE_FIELDTRIAL_ORACLE=1``).

What it computes, for one platform (default ``mac``), reading everything from the PRISTINE pin tag
(``git show``/``git grep`` at ``refs/tags/<pin>``, never the patched working tree):

* studies whose ``platforms`` include the platform; the FIRST-listed experiment of each is the one the
  testing config activates (Chromium's ``ChooseExperiment()`` also weighs eligibility/forcing flags;
  the inventory deliberately ignores that);
* that experiment's ``enable_features`` (forced-ON directives) and ``disable_features`` (forced-OFF),
  plus the number of activated experiments carrying ``params`` (those param sets vanish);
* for every directive, the feature's compiled default as the ``textual-v1`` rule sees it, giving three
  buckets per direction: **effective flip** (default differs from the directive), **no-op** (equal),
  **unresolved** (no match).

The ``textual-v1`` rule (FROZEN — the M149 oracle depends on it; improving it changes ADR 0002's
published numbers and is a separate, documented change):

1. scan every tracked ``*.cc``, ``*.mm``, ``*.h`` at the tag;
2. match the UNANCHORED text ``BASE_FEATURE(`` + ``kIdentifier`` + optional ``"Name"`` + the LITERAL
   token ``base::FEATURE_ENABLED_BY_DEFAULT`` / ``base::FEATURE_DISABLED_BY_DEFAULT``, with only
   whitespace between the arguments;
3. the feature name is the string argument when present, else the identifier minus its leading ``k``;
4. the first match wins — path order (git's), then textual order within the file;
5. a directive whose name never matches is unresolved.

Known consequences (documented limitations, reproduced on purpose — see ADR 0002's methodology note):

* a bare ``FEATURE_DISABLED_BY_DEFAULT`` (inside ``namespace base``) or a ``base::FeatureState::``
  spelling does not match -> unresolved, although the default is literal;
* a ``#if`` inside the macro's argument list does not match -> unresolved;
* platform-conditional duplicate definitions resolve to the textually-FIRST branch, which may be a
  branch not compiled for this platform (a false flip or false no-op);
* test sources are scanned, so a ``*_browsertest.cc`` that sorts first can supply the default.

Usage:
  fieldtrial_inventory.py --chromium-src ~/chromium/src --out-dir docs/uprev-records/<pin>/
  fieldtrial_inventory.py --chromium-src ~/chromium/src --tag 149.0.7827.201 --out-dir /tmp/m149

Writes ``<out-dir>/effective-diff.json`` (machine-readable, deterministic) and
``<out-dir>/inventory.md`` (the attachable record) and prints a one-line summary.
"""

import argparse
import json
import pathlib
import re
import subprocess
import sys

RULE = "textual-v1"
CONFIG_REL = "testing/variations/fieldtrial_testing_config.json"
PATHSPECS = ("*.cc", "*.mm", "*.h")
BUCKETS = ("effective_flips", "noops", "unresolved")

# Rule step 2: unanchored, literal base::FEATURE_* token (whole identifier — `..._BY_DEFAULT_X` is not
# the token), whitespace-only between arguments.
_DEFINITION = re.compile(
    rb'BASE_FEATURE\(\s*k(\w+)\s*,\s*(?:"([^"]*)"\s*,\s*)?base::FEATURE_(ENABLED|DISABLED)_BY_DEFAULT(?!\w)')

LIMITATIONS = (
    "a bare `FEATURE_DISABLED_BY_DEFAULT` (inside `namespace base`) or a `base::FeatureState::` spelling "
    "does not match the literal `base::FEATURE_*_BY_DEFAULT` token and lands in **unresolved** even "
    "though its default is literal;",
    "a `#if` inside the macro's argument list does not match and lands in **unresolved**;",
    "platform-conditional duplicate definitions resolve to the textually-**first** branch, which may not "
    "be the branch compiled for this platform (a false flip or a false no-op);",
    "**test** sources are scanned, so a `*_browsertest.cc` that sorts first in path order can supply the "
    "default.",
)


def _read_pin(pin_file):
    if not pin_file.exists():
        return ""
    for line in pin_file.read_text().splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            return line
    return ""


def _git(chromium_src, *args, input_bytes=None):
    return subprocess.run(["git", "-C", str(chromium_src), *args], input=input_bytes,
                          capture_output=True)


def resolve_tag(chromium_src, pin):
    """Strict: only refs/tags/<pin> counts (no HEAD/branch/SHA fallback). Returns an error or None."""
    if not pin or pin == "UNPINNED":
        return "pin is missing or UNPINNED — pin Chromium first (BOOTSTRAP.md step 2)."
    ref = f"refs/tags/{pin}"
    if _git(chromium_src, "rev-parse", "--verify", f"{ref}^{{commit}}").returncode != 0:
        return (f"tag '{pin}' does not resolve in {chromium_src}.\n"
                f"hint: git -C {chromium_src} fetch origin +refs/tags/{pin}:refs/tags/{pin}")
    return None


def load_config(chromium_src, ref):
    r = _git(chromium_src, "show", f"{ref}:{CONFIG_REL}")
    if r.returncode != 0:
        raise RuntimeError(f"{CONFIG_REL} not found at {ref}: {r.stderr.decode(errors='replace').strip()}")
    return json.loads(r.stdout.decode("utf-8"))


def extract_directives(config, platform):
    """Return (studies, forced_on, forced_off, params_vanishing) for the platform."""
    studies = 0
    forced_on, forced_off, params = [], [], 0
    for _name, entries in config.items():
        for entry in entries:
            if platform not in entry.get("platforms", []):
                continue
            experiments = entry.get("experiments") or []
            if not experiments:
                continue
            studies += 1
            first = experiments[0]  # the group the testing config activates
            forced_on.extend(first.get("enable_features", []))
            forced_off.extend(first.get("disable_features", []))
            if first.get("params"):
                params += 1
    return studies, forced_on, forced_off, params


def _blobs_at(chromium_src, ref, rel_paths):
    """Yield (rel_path, bytes) for each path at ref via one cat-file --batch (byte-exact parsing)."""
    request = b"".join(f"{ref}:{p}\n".encode() for p in rel_paths)
    r = _git(chromium_src, "cat-file", "--batch", input_bytes=request)
    if r.returncode != 0:
        raise RuntimeError(f"git cat-file failed: {r.stderr.decode(errors='replace').strip()}")
    out, pos = r.stdout, 0
    for rel in rel_paths:
        nl = out.index(b"\n", pos)
        header = out[pos:nl].split()
        if header[-1] == b"missing":
            pos = nl + 1
            continue
        size = int(header[-1])
        body = out[nl + 1:nl + 1 + size]
        pos = nl + 1 + size + 1  # trailing newline after the blob
        yield rel, body


def harvest_defaults(chromium_src, ref):
    """name -> 'on'|'off' under the textual-v1 rule; first match wins (git path order, then text)."""
    r = _git(chromium_src, "grep", "-l", "-E", r"BASE_FEATURE\(", ref, "--", *PATHSPECS)
    if r.returncode not in (0, 1):  # 1 = no matches
        raise RuntimeError(f"git grep failed: {r.stderr.decode(errors='replace').strip()}")
    prefix = f"{ref}:".encode()
    files = [line[len(prefix):].decode() for line in r.stdout.splitlines() if line.startswith(prefix)]
    defaults = {}
    for _rel, body in _blobs_at(chromium_src, ref, files):
        for m in _DEFINITION.finditer(body):
            name = (m.group(2) if m.group(2) is not None else m.group(1)).decode()
            if name not in defaults:
                defaults[name] = "on" if m.group(3) == b"ENABLED" else "off"
    return defaults


def classify(names, direction, defaults):
    buckets = {b: [] for b in BUCKETS}
    for name in names:
        compiled = defaults.get(name)
        if compiled is None:
            buckets["unresolved"].append(name)
        elif compiled == direction:
            buckets["noops"].append(name)
        else:
            buckets["effective_flips"].append(name)
    return {b: sorted(v) for b, v in buckets.items()}


def build_inventory(chromium_src, pin, platform):
    ref = f"refs/tags/{pin}"
    config = load_config(chromium_src, ref)
    studies, forced_on, forced_off, params = extract_directives(config, platform)
    defaults = harvest_defaults(chromium_src, ref)
    return {
        "pin": pin,
        "platform": platform,
        "rule": RULE,
        "studies": studies,
        "forced_on_directives": len(forced_on),
        "forced_off_directives": len(forced_off),
        "params_vanishing": params,
        "forced_on": classify(forced_on, "on", defaults),
        "forced_off": classify(forced_off, "off", defaults),
    }


def _counts(inv, direction):
    b = inv[direction]
    return (f"{inv[direction + '_directives']} → {len(b['effective_flips'])} effective flips / "
            f"{len(b['noops'])} no-ops / {len(b['unresolved'])} unresolved")


def summary_line(inv):
    return (f"pin {inv['pin']} platform {inv['platform']}: {inv['studies']} studies; "
            f"forced-ON {_counts(inv, 'forced_on')}; forced-OFF {_counts(inv, 'forced_off')}; "
            f"{inv['params_vanishing']} param set(s) vanish")


def render_markdown(inv):
    lines = [
        f"# Field-trial feature inventory — pin `{inv['pin']}`, platform `{inv['platform']}` (ADR 0002)",
        "",
        f"Generated by `roamux/build/fieldtrial_inventory.py` (rule `{inv['rule']}`) from the pristine tag.",
        "",
        "| direction | directives | effective flips | no-ops | **unresolved** |",
        "|---|---|---|---|---|",
    ]
    for label, key in (("forced-ON", "forced_on"), ("forced-OFF", "forced_off")):
        b = inv[key]
        lines.append(f"| {label} | {inv[key + '_directives']} | {len(b['effective_flips'])} | "
                     f"{len(b['noops'])} | **{len(b['unresolved'])}** |")
    lines += [
        "",
        f"- Studies applicable to `{inv['platform']}` (first-listed experiment activates): **{inv['studies']}**",
        f"- Activated experiments whose field-trial params vanish: **{inv['params_vanishing']}**",
        "",
        "## How to read the buckets",
        "",
        "These are `" + inv["rule"] + "` textual-harvest classifications of the pristine tree, not verified "
        "compiled behaviour for this platform (see ADR 0002's methodology note). **Unresolved** names are "
        "potential flips: treat them as flips for decision purposes. A jump in the unresolved count at a new "
        "pin usually means a new definition spelling the rule does not match, not a change in the config. "
        "Known limitations of the rule:",
        "",
    ]
    lines += [f"- {text}" for text in LIMITATIONS]
    lines += ["",
              "The overlay's own patches can change a compiled default (e.g. patch `0067` flips "
              "`NewTabAddsToActiveGroup` to enabled); this inventory reads upstream's pristine defaults."]
    for label, key in (("forced ON", "forced_on"), ("forced OFF", "forced_off")):
        for bucket, title in (("effective_flips", "effective flips"), ("noops", "no-ops"),
                              ("unresolved", "unresolved")):
            names = inv[key][bucket]
            lines += ["", f"## Features {label} — {title} ({len(names)})", ""]
            lines += [f"- {n}" for n in names] or ["- (none)"]
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--chromium-src", required=True, type=pathlib.Path)
    parser.add_argument("--pin-file", type=pathlib.Path,
                        default=pathlib.Path(__file__).resolve().parent / "CHROMIUM_PIN",
                        help="default: roamux/build/CHROMIUM_PIN")
    parser.add_argument("--tag", default=None,
                        help="milestone tag to inventory instead of the pin file's (e.g. the M149 oracle tag)")
    parser.add_argument("--platform", default="mac",
                        help="platform key as used in the config's 'platforms' lists (default: mac; "
                             "only mac is validated against the M149 oracle)")
    parser.add_argument("--out-dir", required=True, type=pathlib.Path,
                        help="where effective-diff.json and inventory.md are written")
    args = parser.parse_args()

    pin = args.tag or _read_pin(args.pin_file)
    if not pin:
        print(f"FAIL: {args.pin_file} is missing or UNPINNED — pin Chromium first (BOOTSTRAP.md step 2).",
              file=sys.stderr)
        return 1
    error = resolve_tag(args.chromium_src, pin)
    if error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1

    try:
        inv = build_inventory(args.chromium_src, pin, args.platform)
    except (RuntimeError, ValueError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1

    args.out_dir.mkdir(parents=True, exist_ok=True)
    (args.out_dir / "effective-diff.json").write_text(json.dumps(inv, indent=2, sort_keys=False) + "\n")
    (args.out_dir / "inventory.md").write_text(render_markdown(inv))
    print(summary_line(inv))
    print(f"wrote {args.out_dir / 'effective-diff.json'} and {args.out_dir / 'inventory.md'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
