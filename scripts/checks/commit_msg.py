#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Conventional Commits SYNTAX check (roam-38, §7.9). Validates the subject shape ONLY. It performs no
roam-N linkage of any kind — the roam-<N> issue-link gate is roam-39's (I-7.2) exclusive scope.

Usage: commit_msg.py <path-to-commit-message-file>
"""

import pathlib
import re
import sys

TYPES = "feat|fix|chore|docs|test|refactor|perf|build|ci|style|revert"
SUBJECT_RE = re.compile(rf"^(?:{TYPES})(?:\([^)]+\))?!?: .+")


def main(argv):
    if len(argv) != 1:
        print("commit_msg: expected one argument (the commit message file)", file=sys.stderr)
        return 2
    lines = pathlib.Path(argv[0]).read_text(errors="replace").splitlines()
    subject = next((l for l in lines if l.strip() and not l.startswith("#")), "")
    if not SUBJECT_RE.match(subject):
        print(f"commit-msg: subject is not Conventional Commits: {subject!r}\n"
              f"  expected: <type>(<scope>): <subject>   type in {{{TYPES}}}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
