#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Settings-logo gate (roam-157): assert the branded About logo/glyph are real art, not stubs.

roam-140 shipped roamux_logo.png as a 1x1 transparent placeholder, so the chrome://settings/help
About hero and the settings nav chip rendered blank (roam-157). This checker proves the served assets
are the real Roamux mark — structurally, not by a byte count — so the stub can never silently return.

Served from roamux/app/resources/settings_about/ via the settings_about build_webui bundle at
chrome://settings/roamux_about/*:
  - roamux_logo.svg   the full tile (About hero)
  - roamux_glyph.svg  the transparent-centre glyph (nav chip): the four rays under a luminance mask,
                      no tile rect, no drop shadow.

The check is XML-structural (parses the SVG), so a 1x1 PNG, an empty file, or a colourless placeholder
all fail — a plain colour-literal grep would not. Pure Python, no Chromium build; runs in tier-1.

Usage: check_settings_logo.py --assets-dir <settings_about dir> [--json]
"""

import argparse
import json
import pathlib
import sys
import xml.etree.ElementTree as ET

_SVG = "{http://www.w3.org/2000/svg}"
PALETTE = {"#4285F4", "#EA4335", "#FBBC05", "#34A853"}  # Blue / Red / Yellow / Green
_RAY_STROKE_WIDTH = 90.608

LOGO = "roamux_logo.svg"
GLYPH = "roamux_glyph.svg"


def _tag(el):
    return el.tag.replace(_SVG, "")


def _findall(root, name):
    return [el for el in root.iter() if _tag(el) == name]


def _masked_elements(root):
    """Every element that lives inside a <mask> — the mask's own white field rect and knockout
    circle are mask machinery, not visible art, so the tile/shadow checks must skip them."""
    inside = set()
    for m in _findall(root, "mask"):
        for el in m.iter():
            inside.add(id(el))
    return inside


def _ray_strokes(root):
    """Palette strokes on the four ray <line>s (stroke may sit on the line or an ancestor <g>)."""
    strokes = []
    for line in _findall(root, "line"):
        s = line.get("stroke")
        if s in PALETTE:
            strokes.append(s)
    return strokes


def _check_common(root, errors, name):
    if _tag(root) != "svg":
        errors.append(f"{name}: root is <{_tag(root)}>, not <svg> (a stub PNG or non-SVG?)")
        return
    if not root.get("viewBox"):
        errors.append(f"{name}: <svg> has no viewBox")
    strokes = _ray_strokes(root)
    if set(strokes) != PALETTE:
        errors.append(
            f"{name}: rays carry {sorted(set(strokes))}, expected the 4 Google palette colours "
            f"{sorted(PALETTE)} (degenerate/placeholder art?)")
    if len([el for el in _findall(root, "line")]) < 4:
        errors.append(f"{name}: fewer than 4 ray <line> elements")


def _check_glyph(root, errors):
    _check_common(root, errors, GLYPH)
    # A luminance mask cuts the transparent centre hole; without it the glyph has no negative space.
    masks = _findall(root, "mask")
    if not masks:
        errors.append(f"{GLYPH}: no <mask> — the transparent centre hole would be missing")
    else:
        has_hole = any(_findall(m, "circle") for m in masks)
        if not has_hole:
            errors.append(f"{GLYPH}: <mask> has no <circle> centre knockout")
    # The glyph must NOT carry the tile: no white background rect, no drop-shadow filter. Skip rects
    # inside the <mask> — the luminance mask's white field is legitimate machinery, not a tile.
    masked = _masked_elements(root)
    for rect in _findall(root, "rect"):
        if id(rect) in masked:
            continue
        if (rect.get("fill") or "").upper() == "#FFFFFF":
            errors.append(f"{GLYPH}: has a white tile <rect> — the inline glyph must be transparent")
    if _findall(root, "filter"):
        errors.append(f"{GLYPH}: has a <filter> (drop shadow) — the inline glyph must be flat")


def _check_logo(root, errors):
    _check_common(root, errors, LOGO)
    # The hero tile SHOULD carry the white rounded-square background.
    if not any((r.get("fill") or "").upper() == "#FFFFFF" for r in _findall(root, "rect")):
        errors.append(f"{LOGO}: no white tile <rect> — the hero should be the full app-icon tile")


def check(assets_dir):
    errors = []
    d = pathlib.Path(assets_dir)
    for name, checker in ((LOGO, _check_logo), (GLYPH, _check_glyph)):
        p = d / name
        if not p.exists():
            errors.append(f"{name}: missing from {assets_dir}")
            continue
        try:
            root = ET.fromstring(p.read_bytes())
        except ET.ParseError as e:
            errors.append(f"{name}: not parseable as SVG ({e}) — a 1x1 PNG stub?")
            continue
        checker(root, errors)
    return errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--assets-dir", required=True,
                        help="the settings_about resource dir holding roamux_logo.svg / roamux_glyph.svg")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()

    errors = check(args.assets_dir)
    if args.json:
        print(json.dumps({"ok": not errors, "errors": errors}, indent=2))
    else:
        for e in errors:
            print(f"FAIL: {e}", file=sys.stderr)
        if not errors:
            print("settings logo/glyph OK: both are structural Roamux SVG art (not stubs)")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
