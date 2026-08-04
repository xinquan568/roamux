#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""pre-push test gate (roam-38, §7.9 / P6). Honest degradation: always runs the hermetic suite; when a
Chromium checkout is configured (ROAMUX_CHROMIUM_SRC with an out/Default) it also builds+runs the touched
Roamux gtest target. Never a silent pass; never a full-tree build.
"""

import os
import pathlib
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parents[2]
GTEST_TARGET = "roamux_unittests"


def _clean_git_env():
    # git exports GIT_DIR/GIT_INDEX_FILE/... into hooks; strip them so the fixture tests' throwaway
    # `git -C <tmp>` repos are not hijacked into the real repo.
    #
    # REQUIRE_DMG_MOUNT (roam-261): stripped so `git push` NEVER mounts a disk image, whatever the
    # developer has exported. The mount is opt-in per tier and this tier does not opt in — it was
    # 35-45% of this gate's wall clock. Deliberate: a shell-level opt-in reaching this child would
    # silently restore the cost that roam-261 removed. Direct local runs
    # (`REQUIRE_DMG_MOUNT=1 python3 -m unittest ...`) and both CI opt-ins are unaffected.
    return {k: v for k, v in os.environ.items()
            if k not in ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_PREFIX",
                         "GIT_COMMON_DIR", "GIT_OBJECT_DIRECTORY",
                         "REQUIRE_DMG_MOUNT")}


def _log_path():
    """roam-259: where a blocked push leaves its evidence.

    NOT `REPO/.git/...` — in a linked worktree `.git` is a FILE, and this
    repo's own /issue2pr pipeline runs every task inside a worktree, so the
    naive path would fail exactly where it is used most. Ask git for the real
    git dir (per-worktree), resolving a relative answer against REPO. Returns
    None if no usable location can be determined; logging is best-effort and
    never blocks a push."""
    try:
        out = subprocess.run(["git", "rev-parse", "--git-dir"], cwd=REPO,
                             env=_clean_git_env(), capture_output=True,
                             text=True)
        if out.returncode == 0 and out.stdout.strip():
            git_dir = pathlib.Path(out.stdout.strip())
            if not git_dir.is_absolute():
                git_dir = REPO / git_dir
            if git_dir.is_dir():
                return git_dir / "roamux-pre-push.log"
    except OSError:
        pass
    fallback = REPO / ".git"
    return fallback / "roamux-pre-push.log" if fallback.is_dir() else None


def _run_teed(cmd, log, **kwargs):
    """Run `cmd`, streaming its output live AND appending it to `log`.

    stderr is merged into stdout (one pipe — two would risk a fill-and-block
    deadlock), so the failure text lands in the durable copy alongside the
    progress that explains it. Any logging failure degrades to a warning: a
    gate that failed because its own logging failed would be worse than the
    bug this fixes."""
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, **kwargs)
    sink = None
    if log is not None:
        try:
            sink = open(log, "a")
        except OSError as exc:
            print(f"pre-push: no durable log ({exc}); live output only.",
                  file=sys.stderr, flush=True)

    def _close(handle):
        try:
            handle.close()
        except OSError as exc:
            # Close can flush buffered bytes and fail (full disk, dead NFS).
            # That must NOT escape: proc.wait() below is what propagates the
            # child's verdict, and losing it would fail a green gate.
            print(f"pre-push: durable log close failed ({exc}).",
                  file=sys.stderr, flush=True)

    try:
        for line in proc.stdout:
            sys.stdout.write(line)
            sys.stdout.flush()
            if sink is not None:
                try:
                    sink.write(line)
                    sink.flush()  # a crashed push must still leave the lines
                except OSError as exc:
                    print(f"pre-push: durable log disabled ({exc}); "
                          "live output only.", file=sys.stderr, flush=True)
                    _close(sink)
                    sink = None
    finally:
        try:
            proc.stdout.close()
        except OSError:
            pass
        if sink is not None:
            _close(sink)
    return proc.wait()


def gtest_decision(environ):
    """Return ('run', src) when a usable checkout is configured, else ('skip', reason). Pure — testable."""
    src = environ.get("ROAMUX_CHROMIUM_SRC", "")
    if not src:
        return ("skip", "no Chromium checkout configured (ROAMUX_CHROMIUM_SRC unset)")
    if not (pathlib.Path(src) / "out" / "Default").is_dir():
        return ("skip", f"ROAMUX_CHROMIUM_SRC={src} has no out/Default build dir")
    return ("run", src)


def main():
    log = _log_path()
    if log is not None:
        print(f"pre-push: logging this run to {log}", flush=True)
    else:
        # Say so out loud: silently losing the durable evidence would recreate
        # exactly the failure mode roam-259 is about.
        print("pre-push: no durable log location could be resolved; "
              "live output only.", file=sys.stderr, flush=True)
    print("pre-push: running the hermetic suite (checkout-free)...", flush=True)
    rc = _run_teed([sys.executable, "-m", "unittest", "discover", "-s", "roamux/build/tests"],
                   log, cwd=REPO, env=_clean_git_env())
    if rc != 0:
        print("pre-push: hermetic suite FAILED — push blocked.", file=sys.stderr, flush=True)
        return rc

    action, detail = gtest_decision(os.environ)
    if action == "skip":
        print(f"pre-push: SKIP gtest gate — {detail}; the touched-target build+test runs in CI.",
              flush=True)
        return 0

    src = detail
    print(f"pre-push: Chromium checkout at {src} — building + running {GTEST_TARGET} (incremental)...",
          flush=True)
    build_rc = _run_teed(["autoninja", "-C", "out/Default", GTEST_TARGET], log, cwd=src)
    if build_rc != 0:
        print("pre-push: autoninja FAILED — push blocked.", file=sys.stderr, flush=True)
        return build_rc
    test_rc = _run_teed([str(pathlib.Path(src) / "out" / "Default" / GTEST_TARGET)], log, cwd=src)
    if test_rc != 0:
        print(f"pre-push: {GTEST_TARGET} FAILED — push blocked.", file=sys.stderr, flush=True)
        return test_rc
    print(f"pre-push: {GTEST_TARGET} green.", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
