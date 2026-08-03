# SPDX-License-Identifier: Apache-2.0
"""roam-33 packaging — .zip and .dmg that PRESERVE framework symlinks + exec
bits (Sparkle deltas and codesign break on a flattened framework). `ditto`
preserves symlinks, xattrs, and mode bits; `hdiutil` images a staged dir."""

import argparse
import pathlib
import shutil
import subprocess
import sys
import tempfile
import time

# roam-259: `hdiutil create` retried with ZERO backoff — no sleep on any path,
# so four attempts burned in a few ms and rode out nothing. Short capped
# schedule (3 delays for the 4 attempts): unlike the attach path there is no
# leaked-device mode to clear here, so this only rides out genuine contention
# and must not stall a release job.
_CREATE_RETRY_DELAYS = (2, 4, 8)


def package_zip(app_path, out_path):
    app_path = pathlib.Path(app_path)
    out_path = pathlib.Path(out_path)
    # ditto -c -k --sequesterRsrc --keepParent: a symlink/xattr/mode-preserving
    # zip that keeps the .app as the archive's top-level entry.
    subprocess.run(
        ["ditto", "-c", "-k", "--sequesterRsrc", "--keepParent",
         str(app_path), str(out_path)],
        check=True)
    return out_path


def package_dmg(app_path, out_path, volname="Roamux", sleep_fn=None):
    app_path = pathlib.Path(app_path)
    out_path = pathlib.Path(out_path)
    with tempfile.TemporaryDirectory(prefix="roamux-dmg-") as stage:
        # ditto into the staging dir preserves the bundle exactly.
        subprocess.run(["ditto", str(app_path),
                        str(pathlib.Path(stage) / app_path.name)], check=True)
        if out_path.exists():
            out_path.unlink()
        # hdiutil can transiently fail under concurrent /dev/disk pressure;
        # a bounded retry keeps the real release job robust. roam-259: settle
        # between attempts — sleep_fn=None resolves to the real time.sleep, so
        # the PRODUCTION path backs off (note the deliberate asymmetry with
        # test_release_signing._attach_with_retry, where None means no sleep).
        sleep = time.sleep if sleep_fn is None else sleep_fn
        last = None
        for attempt in range(4):
            r = subprocess.run(
                ["hdiutil", "create", "-volname", volname, "-srcfolder",
                 stage, "-ov", "-format", "UDZO", str(out_path)],
                capture_output=True, text=True)
            if r.returncode == 0:
                break
            last = r.stderr
            if attempt < len(_CREATE_RETRY_DELAYS):
                sleep(_CREATE_RETRY_DELAYS[attempt])
        else:
            raise RuntimeError(f"hdiutil create failed after retries: {last}")
    return out_path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", required=True)
    parser.add_argument("--zip")
    parser.add_argument("--dmg")
    parser.add_argument("--volname", default="Roamux")
    args = parser.parse_args()
    if args.zip:
        package_zip(args.app, args.zip)
        print(f"[ok] zip: {args.zip}")
    if args.dmg:
        package_dmg(args.app, args.dmg, args.volname)
        print(f"[ok] dmg: {args.dmg}")
    if not args.zip and not args.dmg:
        print("nothing to do (pass --zip and/or --dmg)", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
