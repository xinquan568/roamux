#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Secret scan (roam-38, §7.9). High-signal patterns only, with an allowlist for this repo's known-safe
strings (the google_keys template placeholders, obvious example/fixture tokens). Hook + CI both call it.
"""

import pathlib
import re
import sys

PATTERNS = [
    ("private-key-block", re.compile(r"-----BEGIN [A-Z ]*PRIVATE KEY-----")),
    ("aws-access-key", re.compile(r"\bAKIA[0-9A-Z]{16}\b")),
    ("slack-token", re.compile(r"\bxox[baprs]-[0-9A-Za-z-]{10,}")),
    ("generic-secret", re.compile(
        r"""(?i)\b(secret|token|password|api[_-]?key|client[_-]?secret)\b\s*[:=]\s*["'][^"'\s]{16,}["']""")),
]
# Substrings that make a match a known-safe placeholder/example rather than a real secret.
ALLOWLIST = ("EXAMPLE", "<your", "your-", "placeholder", "REPLACE", "dummy", "fake", "xxxx")
# An explicit, review-visible per-line opt-out for legitimate fixtures/examples (e.g. the scanner's
# own test data). Same idea as detect-secrets' `pragma: allowlist secret` / bandit's `nosec`.
ALLOW_MARKER = "roamex:allow-secret"


def main(argv):
    violations = []
    for path in argv:
        try:
            text = pathlib.Path(path).read_text(errors="replace")
        except (FileNotFoundError, IsADirectoryError):
            continue
        for lineno, line in enumerate(text.splitlines(), 1):
            if ALLOW_MARKER in line:
                continue
            for name, pat in PATTERNS:
                if pat.search(line) and not any(a.lower() in line.lower() for a in ALLOWLIST):
                    violations.append(f"{path}:{lineno}: possible {name}")
    for v in violations:
        print(f"secret-scan: {v}", file=sys.stderr)
    return 1 if violations else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
