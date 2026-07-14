#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Map a release tag to a Sparkle-orderable CFBundleVersion + a human display string
(roam-141).

Sparkle's SUStandardVersionComparator only orders NUMERIC, dot-separated versions —
it reads `0.0.1-alpha.3` as just `0.0.1` and drops the pre-release suffix, so every
alpha compares equal and no update is ever offered. So the value Sparkle compares
(CFBundleVersion, and the appcast's sparkle:version) must be a numeric encoding, while
the marketing string (`0.0.1-alpha.3`) lives in sparkle:shortVersionString / the dialog
title.

Encoding: `MAJOR.MINOR.PATCH.STAGE.N`, STAGE ∈ {alpha:1, beta:2, rc:3, final:9}. A final
release (no pre-release segment) is `M.m.p.9.0`, which sorts above every pre-release of
the same patch and below the next patch's alpha. Verified against the vendored Sparkle
framework: 0.0.1.1.9 < 0.0.1.1.10 < 0.0.1.2.1 < 0.0.1.9.0 < 0.0.2.1.1.
"""

import argparse
import re
import sys

STAGES = {"alpha": 1, "beta": 2, "rc": 3}
_FINAL_STAGE = 9
_TAG_RE = re.compile(
    r"^v?(?P<core>\d+\.\d+\.\d+)(?:-(?P<stage>[a-z]+)\.(?P<n>\d+))?$")


def _parse(tag):
    m = _TAG_RE.match(tag.strip())
    if not m:
        raise ValueError(
            f"unparseable release tag {tag!r} "
            "(expected vMAJOR.MINOR.PATCH or vMAJOR.MINOR.PATCH-STAGE.N)")
    core = m.group("core")
    stage = m.group("stage")
    if stage is None:
        return core, _FINAL_STAGE, 0
    if stage not in STAGES:
        raise ValueError(
            f"unknown pre-release stage {stage!r} in {tag!r} "
            f"(known: {', '.join(sorted(STAGES))})")
    return core, STAGES[stage], int(m.group("n"))


def bundle_version(tag):
    """CFBundleVersion / appcast sparkle:version — numeric, Sparkle-orderable."""
    core, stage, n = _parse(tag)
    return f"{core}.{stage}.{n}"


def short_version(tag):
    """Human display string (sparkle:shortVersionString / dialog title)."""
    return tag[1:] if tag.startswith("v") else tag


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--field", required=True, choices=["bundle", "short"])
    args = parser.parse_args()
    print(bundle_version(args.tag) if args.field == "bundle"
          else short_version(args.tag))
    return 0


if __name__ == "__main__":
    sys.exit(main())
