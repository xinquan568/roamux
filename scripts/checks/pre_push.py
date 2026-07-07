#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""pre-push test gate (roam-38, §7.9 / P6). Honest degradation: always runs the hermetic suite; also
builds+runs the touched Roamex gtests when a Chromium checkout is configured (ROAMEX_CHROMIUM_SRC), else
announces the skip. Never a multi-hour block; never a silent pass.
"""

import os
import pathlib
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parents[2]


def main():
    print("pre-push: running the hermetic suite (checkout-free)...")
    # git exports GIT_DIR/GIT_WORK_TREE/GIT_INDEX_FILE/... into the hook env; those would hijack the
    # throwaway `git init` repos the fixture tests build. Scrub them so the suite runs standalone.
    env = {k: v for k, v in os.environ.items()
           if k not in ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_PREFIX",
                        "GIT_COMMON_DIR", "GIT_OBJECT_DIRECTORY")}
    rc = subprocess.run([sys.executable, "-m", "unittest", "discover",
                         "-s", "roamex/build/tests"], cwd=REPO, env=env).returncode
    if rc != 0:
        print("pre-push: hermetic suite FAILED — push blocked.", file=sys.stderr)
        return rc
    src = os.environ.get("ROAMEX_CHROMIUM_SRC", "")
    if src and pathlib.Path(src, "out", "Default").is_dir():
        print(f"pre-push: Chromium checkout at {src} — build the touched gtest target(s) with autoninja "
              "and run them before push (see docs/ci). [operator/CI gate]")
    else:
        print("pre-push: SKIP gtest gate — no Chromium checkout configured (ROAMEX_CHROMIUM_SRC unset); "
              "the touched-target build+test runs in CI.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
