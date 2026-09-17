#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Roamux patch runhook (execution plan §12.2 mechanism 3, §12.5).

Applies every ``roamux/patches/*.patch`` to a Chromium checkout, in name order.
Idempotent and **stack-aware** (roam-77): the applied-state detector forward-
simulates the whole stack from pristine (``git show HEAD:<path>``) content in a
scratch tree and matches the working tree against the per-patch snapshots —
per-patch ``git apply --reverse --check`` probes misread a fully-applied tree
whenever a later patch inserts lines adjacent to an earlier patch's hunk (the
0008×0020 / 0006×0024 shape). Fails loudly, naming the patch or the diverged
file, when the tree matches no stack state.

Usage:
  apply_patches.py --chromium-src ~/chromium/src [--patches DIR] [--check]
  apply_patches.py --chromium-src ~/chromium/src --reconcile        # CI only — see below

``--check`` verifies (applied or cleanly appliable) without mutating the tree.

``--reconcile`` (roam-341) is CI's base reconcile and **overwrites**: it forces the checkout to the
fully-patched state of the current stack whatever the tree held before (a superseded stack, stray
edits, a prior release's mutations), keeping every guarantee of the former
``git reset --hard HEAD`` + ``git clean -fd -e /roamux`` + apply sequence (roam-175) — but a patched
file whose bytes already equal the stack's target is left untouched, so its mtime survives and the
retained build does not re-invalidate its dependents. It does that by letting git do the work:

  1. simulate the stack from pristine (as always; a stack that does not apply fails loudly here,
     before anything is mutated);
  2. write the target blobs; build the tree "HEAD + targets" in a TEMPORARY index (staged junk in
     the real index must not enter it);
  3. seed only the union entries of the REAL index with those blobs (a stack-deleted path gets HEAD's
     own entry re-seeded so the reset consumes the deletion; an entry that conflicts with the index —
     a staged file where a directory must be, or vice versa — is skipped and repaired by the reset),
     refresh the index so byte-identical files become stat-clean, and remove any real directory
     sitting on a union path (git leaves those behind);
  4. ``git read-tree --reset -u <tree>`` — the HEAD-preserving core of ``reset --hard``: every entry
     absent from the tree is removed, every changed or non-clean entry is checked out (git unlinks
     first: symlinks, obstructions and hardlink aliases are handled by git), every stat-clean entry
     is left alone;
  5. ``git clean -fd -e /roamux`` while the index still equals the tree (patch-added files are
     tracked, so spared; superseded-stack leftovers removed; never ``-x``, never ``-ff``);
  6. ``git read-tree --reset HEAD`` (no ``-u``): index back to pristine, worktree untouched, and — unlike
     ``reset``, which writes ORIG_HEAD and a reflog line — no ref or reflog is touched.

HEAD is never written, so an interruption at any point converges on the next run. Objects written
are content-addressed (an unchanged stack adds none after its first run), unreferenced, and pruned by
the checkout's ordinary gc. Patch-added executables would be seeded as 100644 (no patch adds a file
today). Never run ``--reconcile`` on a checkout whose edits you want to keep.
"""

import argparse
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile


def _run(cmd, **kwargs):
    return subprocess.run(cmd, capture_output=True, **kwargs)


def _touched_paths(patch):
    """Repo-relative paths a patch touches, via git's own parser."""
    result = _run(["git", "apply", "--numstat", str(patch)])
    if result.returncode != 0:
        _fail(patch, result)
        return None
    paths = []
    for line in result.stdout.decode("utf-8").splitlines():
        added, deleted, path = line.split("\t", 2)
        if added == "-" or deleted == "-":
            print(f"FAIL: binary patch unsupported: {patch.name} ({path})",
                  file=sys.stderr)
            return None
        paths.append(path)
    return paths


def _pristine(chromium_src, path):
    """The file's content at HEAD (the pinned base), or None if absent."""
    result = _run(["git", "-C", str(chromium_src), "show", f"HEAD:{path}"])
    return result.stdout if result.returncode == 0 else None


def _read_tree(root, paths):
    """Snapshot {path: bytes-or-None} for the given paths under root."""
    snapshot = {}
    for path in paths:
        file = pathlib.Path(root) / path
        snapshot[path] = file.read_bytes() if file.is_file() else None
    return snapshot


def _write_tree(root, snapshot):
    for path, content in snapshot.items():
        if content is None:
            continue
        file = pathlib.Path(root) / path
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_bytes(content)


def _simulate(chromium_src, patches, union):
    """Forward-apply the stack in a scratch tree from pristine content.

    Returns [state0 (pristine), state1, ..., stateN] — content snapshots over
    the union of touched paths — or None after a loud failure (the stack does
    not apply in order; the same failure a fresh checkout would hit).
    """
    scratch = tempfile.mkdtemp(prefix="roamux-patch-sim-")
    try:
        _write_tree(scratch, {p: _pristine(chromium_src, p) for p in union})
        snapshots = [_read_tree(scratch, union)]
        for patch in patches:
            result = _run(["git", "apply", str(patch)], cwd=scratch)
            if result.returncode != 0:
                _fail(patch, result)
                return None
            snapshots.append(_read_tree(scratch, union))
        return snapshots
    finally:
        shutil.rmtree(scratch, ignore_errors=True)


_INDEX_CONFLICT = b"appears as both a file and as a directory"


def _reconcile(chromium_src, union, target):
    """Force the checkout to `target` (the stack's final snapshot over `union`) through git's own
    read-tree --reset -u; see the module docstring (roam-341). Returns a process exit code."""
    src = str(chromium_src)

    def g(*args, env=None, input_bytes=None):
        return _run(["git", "--literal-pathspecs", "-C", src, *args], env=env, input=input_bytes)

    def must(result, what):
        if result.returncode != 0:
            print(f"FAIL: reconcile: {what}: {result.stderr.decode('utf-8', 'replace').strip()}",
                  file=sys.stderr)
            return False
        return True

    head = g("rev-parse", "HEAD")
    if not must(head, "rev-parse HEAD"):
        return 1
    orig = head.stdout.decode().strip()

    head_entries = {}
    if union:
        listing = g("ls-tree", "-z", orig, "--", *union)
        if not must(listing, "ls-tree"):
            return 1
        for record in listing.stdout.split(b"\0"):
            if record:
                meta, path = record.split(b"\t", 1)
                mode, _kind, blob = meta.decode().split()
                head_entries[path.decode()] = (mode, blob)

    blobs = {}
    for path in union:
        if target.get(path) is not None:
            written = g("hash-object", "-w", "--stdin", input_bytes=target[path])
            if not must(written, f"hash-object {path}"):
                return 1
            blobs[path] = written.stdout.decode().strip()

    def seed(path, env=None, *, for_tree):
        """Index entry for `path`: in the target TREE a stack-deleted path is absent; in the REAL
        index it keeps HEAD's own entry so that read-tree --reset consumes the deletion."""
        if target.get(path) is None:
            if path not in head_entries:  # HEAD never had it: nothing to remove or seed
                return None
            if for_tree:
                return g("update-index", "--force-remove", "--", path, env=env)
            mode, blob = head_entries[path]
        else:
            mode = head_entries.get(path, ("100644", None))[0]
            blob = blobs[path]
        return g("update-index", "--add", "--cacheinfo", f"{mode},{blob},{path}", env=env)

    # The target tree, built from HEAD in a temporary index so staged junk never enters it.
    fd, tmp_index = tempfile.mkstemp(prefix="roamux-reconcile-index-")
    os.close(fd)
    os.unlink(tmp_index)
    tmp_env = {**os.environ, "GIT_INDEX_FILE": tmp_index}
    try:
        if not must(g("read-tree", orig, env=tmp_env), "read-tree into the temporary index"):
            return 1
        for path in union:
            result = seed(path, env=tmp_env, for_tree=True)
            if result is not None and not must(result, f"temporary index: {path}"):
                return 1
        tree = g("write-tree", env=tmp_env)
        if not must(tree, "write-tree"):
            return 1
        tree = tree.stdout.decode().strip()
    finally:
        if os.path.exists(tmp_index):
            os.unlink(tmp_index)

    # Classify what is about to change, for the summary (before the real index is touched).
    status = g("status", "--porcelain", "-z", "--untracked-files=no")
    if not must(status, "status"):
        return 1
    restored = sorted({record[3:].decode("utf-8", "replace").split("\0")[0]
                       for record in status.stdout.split(b"\0") if len(record) > 3}
                      - set(union))

    # Seed the real index leniently, then let git decide which union entries are already clean.
    for path in union:
        result = seed(path, for_tree=False)
        if result is not None and result.returncode != 0 and _INDEX_CONFLICT not in result.stderr:
            if not must(result, f"seeding {path}"):
                return 1
    g("update-index", "-q", "--refresh")  # rc 1 just means "some entries need update"
    for path in union:
        obstruction = pathlib.Path(src) / path
        if obstruction.is_dir() and not obstruction.is_symlink():
            shutil.rmtree(obstruction)
    dirty = g("diff-files", "--name-only", "-z", "--", *union) if union else None
    needs_update = set(dirty.stdout.decode("utf-8", "replace").split("\0")) if dirty else set()
    rewritten = sorted(p for p in union if target.get(p) is not None
                       and (p in needs_update or not (pathlib.Path(src) / p).exists()))
    removed = sorted(p for p in union if target.get(p) is None and (pathlib.Path(src) / p).exists())
    kept = [p for p in union if target.get(p) is not None and p not in rewritten]

    if not must(g("read-tree", "--reset", "-u", tree), "read-tree --reset -u"):
        return 1
    if os.environ.get("ROAMUX_RECONCILE_STOP_AFTER") == "read-tree":  # test hook: interruption
        return 0
    if not must(g("clean", "-fd", "-e", "/roamux"), "clean"):
        return 1
    if not must(g("read-tree", "--reset", orig), "read-tree --reset HEAD"):
        return 1

    for path in rewritten:
        print(f"[rewritten] {path}")
    for path in removed:
        print(f"[removed]   {path}")
    for path in restored:
        print(f"[restored]  {path}")
    print(f"reconcile: {len(kept)} kept, {len(rewritten)} rewritten, {len(removed)} removed, "
          f"{len(restored)} restored")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--chromium-src", required=True, type=pathlib.Path,
                        help="path to the Chromium src/ checkout")
    parser.add_argument("--patches", type=pathlib.Path,
                        default=pathlib.Path(__file__).resolve().parent.parent / "patches",
                        help="directory holding *.patch files (default: roamux/patches)")
    parser.add_argument("--check", action="store_true",
                        help="verify only; do not mutate the tree")
    parser.add_argument("--reconcile", action="store_true",
                        help="CI only: force the tree to the fully-patched state, leaving "
                             "byte-identical patched files untouched (roam-341); OVERWRITES")
    args = parser.parse_args()
    if args.reconcile and args.check:
        parser.error("--reconcile cannot be combined with --check "
                     "(--check never mutates; --reconcile always may)")

    # Resolve now: the scratch simulation runs git apply with a different cwd,
    # so relative --patches paths must be absolute before that (roam-77 review).
    patches = sorted(args.patches.resolve().glob("*.patch"))
    if not patches and not args.reconcile:
        print(f"no patches found under {args.patches}", file=sys.stderr)
        return 0

    union = []
    for patch in patches:
        paths = _touched_paths(patch)
        if paths is None:
            return 1
        union.extend(p for p in paths if p not in union)

    snapshots = _simulate(args.chromium_src, patches, union)
    if snapshots is None:
        return 1

    if args.reconcile:
        return _reconcile(args.chromium_src, union, snapshots[-1])

    worktree = _read_tree(args.chromium_src, union)
    applied_count = None
    for k in range(len(patches), -1, -1):
        if worktree == snapshots[k]:
            applied_count = k
            break
    if applied_count is None:
        diverged = sorted(p for p in union
                          if worktree.get(p) != snapshots[-1].get(p))
        print("FAIL: the tree matches no stack state (neither pristine nor "
              "any applied prefix) — diverged file(s): "
              f"{', '.join(diverged[:5])}", file=sys.stderr)
        print("Rebase or fix the patch (§12.5: patches fail loudly on rebase).",
              file=sys.stderr)
        return 1

    for i, patch in enumerate(patches, start=1):
        if i <= applied_count:
            print(f"[applied]   {patch.name}")
        elif args.check:
            print(f"[appliable] {patch.name}")
        else:
            result = _run(["git", "-C", str(args.chromium_src), "apply",
                           str(patch)])
            if result.returncode != 0:
                _fail(patch, result)
                return 1
            print(f"[apply]     {patch.name}")
    return 0


def _fail(patch, result):
    print(f"FAIL: patch neither applied nor cleanly appliable: {patch.name}", file=sys.stderr)
    stderr = result.stderr
    if stderr:
        if isinstance(stderr, bytes):
            stderr = stderr.decode("utf-8", "replace")
        print(stderr, file=sys.stderr)
    print("Rebase or fix the patch (§12.5: patches fail loudly on rebase).", file=sys.stderr)


if __name__ == "__main__":
    sys.exit(main())
