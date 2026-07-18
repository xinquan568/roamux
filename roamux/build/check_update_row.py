#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Native update-row gate (roam-160): the Sparkle VersionUpdater patch surface stays sane.

Phase `shape` (T4 gate): patch 0040 exists, touches ONLY its three declared files
(version_updater_mac.mm + about_handler.{cc,h}), its Create() hunk is flag-guarded, and patch
0033's added lines carry NO support.google.com reference into the branded About surface.
Phase `full` (default; tier-1 CI): shape + demolition honesty — no reference to the retired
update-card artifacts (roamux_update_card*, the card's browser_proxy import, fake_update_page,
roamux/mojom / update_page.mojom) survives anywhere in the overlay sources or the patch stack.
Retained roamux_about surfaces (roamux_logo.svg, roamux_glyph.svg, the chip/menu suites) are
exempt by design (the step-6 F3 narrowing).

Usage: check_update_row.py [--phase shape|full] [--overlay <dir>] [--json]
"""

import argparse
import json
import pathlib
import re
import sys

P0040 = "0040-sparkle-version-updater.patch"
P0033 = "0033-settings-about-roamux.patch"
ALLOWED_0040 = {
    "chrome/browser/ui/webui/help/version_updater_mac.mm",
    "chrome/browser/ui/webui/settings/about_handler.cc",
    "chrome/browser/ui/webui/settings/about_handler.h",
}
# The retired card's artifacts — none may be referenced anywhere.
BANNED = ("roamux_update_card", "fake_update_page",
          "roamux/mojom", "update_page.mojom",
          "roamux_about/browser_proxy")
RETIRED_PATCHES = ("0032-", "0035-", "0036-")
SCAN_SUFFIXES = (".cc", ".h", ".mm", ".ts", ".gn", ".gni", ".patch", ".html")


def _sections(patch_text):
    sections = {}
    current = None
    for line in patch_text.splitlines():
        if line.startswith("diff --git "):
            current = line.split(" b/", 1)[-1].strip()
            sections[current] = []
        elif current and line.startswith("+") and not line.startswith("+++"):
            sections[current].append(line[1:])
    return sections


def check_shape(patches_dir):
    failures = []
    p40 = patches_dir / P0040
    if not p40.is_file():
        return [f"{p40}: missing — the native update row has not shipped"]
    sections = _sections(p40.read_text())
    extra = set(sections) - ALLOWED_0040
    if extra:
        failures.append(f"{p40}: beyond the declared surface: {sorted(extra)}")
    mac = sections.get("chrome/browser/ui/webui/help/version_updater_mac.mm")
    if mac is None:
        failures.append(f"{p40}: no version_updater_mac.mm section")
    else:
        if not any("ROAMUX_ENABLE_SPARKLE" in line for line in mac):
            failures.append(f"{p40}: Create() interposition is not flag-guarded")
        if not any("RoamuxVersionUpdater" in line for line in mac):
            failures.append(f"{p40}: Create() does not select the Roamux updater")
    p33 = patches_dir / P0033
    if p33.is_file():
        for path, added in _sections(p33.read_text()).items():
            for line in added:
                if "support.google.com" not in line:
                    continue
                # Comments RECORDING the ban are fine; live markup/URLs fail.
                if line.lstrip().startswith(("#", "//", "*", "<!--")):
                    continue
                failures.append(
                    f"{p33}: adds a support.google.com reference in {path}")
    for stem in RETIRED_PATCHES:
        for stale in patches_dir.glob(stem + "*.patch"):
            failures.append(f"{stale}: retired patch still present")
    return failures


def check_demolition(overlay_dir):
    failures = []
    for path in sorted(pathlib.Path(overlay_dir).rglob("*")):
        if not path.is_file() or path.suffix not in SCAN_SUFFIXES:
            continue
        if "third_party/sparkle" in str(path):
            continue
        try:
            text = path.read_text(errors="ignore")
        except OSError:
            continue
        for marker in BANNED:
            for match in re.finditer(re.escape(marker), text):
                line_no = text.count("\n", 0, match.start()) + 1
                line = text.splitlines()[line_no - 1].strip()
                # Prose mentions in comments that RECORD the retirement are
                # fine; live references (imports, deps, paths, sources) fail.
                if line.lstrip().startswith(("#", "//", "*", "<!--")):
                    continue
                failures.append(
                    f"{path}:{line_no}: live reference to retired artifact"
                    f" '{marker}': {line[:80]}")
    return failures


def main(argv=None):
    root = pathlib.Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--phase", choices=("shape", "full"), default="full")
    parser.add_argument("--overlay", type=pathlib.Path, default=root)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)
    patches = args.overlay / "patches"
    failures = check_shape(patches)
    if args.phase == "full":
        failures += check_demolition(args.overlay)
    if args.json:
        print(json.dumps({"ok": not failures, "failures": failures}, indent=2))
    else:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        if not failures:
            print(f"update-row check ({args.phase}): OK")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
