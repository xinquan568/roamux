#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Overlay-structure check (roam-38, §7.9 / §12.2) — a STRUCTURAL APPROXIMATION only. A single commit
cannot know the whole §12.2 hook inventory, so this enforces cheap shape rules; the roam-2 staleness
gate and the reviewer are the deeper enforcement. Hook + CI both call it.

Rules: files under roamex/patches/ match NNNN-slug.patch (or are README.md); nothing else.
"""

import pathlib
import re
import sys

PATCH_RE = re.compile(r"^\d{4}-[a-z0-9][a-z0-9-]*\.patch$")


def main(argv):
    violations = []
    for path in argv:
        p = path.replace("\\", "/")
        if "/roamex/patches/" in p or p.startswith("roamex/patches/"):
            name = pathlib.PurePath(p).name
            if name in ("README.md",) or name.startswith("."):
                continue
            if not PATCH_RE.match(name):
                violations.append(f"{path}: patch name must match NNNN-slug.patch")
    for v in violations:
        print(f"overlay-structure: {v}", file=sys.stderr)
    return 1 if violations else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
