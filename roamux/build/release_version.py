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
import pathlib
import re
import sys

STAGES = {"alpha": 1, "beta": 2, "rc": 3}
_VERSION_FILE = pathlib.Path(__file__).resolve().parent / "VERSION"
_VERSION_KEY = "ROAMUX_VERSION"
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


def version_key(bundle):
    """The bundle encoding as an integer tuple — the order Sparkle's numeric comparator applies
    (roam-285). `bundle_version()` output only; anything else raises ValueError."""
    try:
        return tuple(int(part) for part in bundle.split("."))
    except ValueError as e:
        raise ValueError(f"not a numeric bundle version: {bundle!r}") from e


def at_least(tag, other_tag):
    """True when `tag` orders at or above `other_tag` on the bundle encoding (roam-285, grill M2:
    publish marks `latest` only when the new release is at least the current latest — an older
    tag published later must not re-point the appcast feed backwards). Raises ValueError for an
    unparseable tag on either side; the caller must treat that as an error, not as 'older'."""
    return version_key(bundle_version(tag)) >= version_key(bundle_version(other_tag))


def short_version(tag):
    """Human display string (sparkle:shortVersionString / dialog title)."""
    return tag[1:] if tag.startswith("v") else tag


def read_source_version(path=_VERSION_FILE):
    """The marketing version as checked in (roam-156) — the single source of truth.

    roamux/build/VERSION is read at BUILD time by //roamux/common:version_header
    (process_version) to compile ROAMUX_VERSION_STRING in, and here so the appcast
    advertises the same string. Before roam-156 the marketing version existed only in
    the git tag, so nothing in the running binary knew it.

    The file is KEY=VALUE lines with no comments and no blank lines, because
    //build/util/version.py:FetchValuesFromFile splits EVERY line on '=' and would
    raise on anything else — including an SPDX header. Parse it the same way rather
    than inventing a second grammar.
    """
    for line in pathlib.Path(path).read_text().splitlines():
        key, _, value = line.partition("=")
        if key == _VERSION_KEY:
            return value.strip()
    raise ValueError(f"{path} has no {_VERSION_KEY}= line")


def check_tag(tag, source_version=None):
    """True when `tag` names the version VERSION declares, AND both are well-formed.

    The release job gates on this before it builds anything: a tag that disagrees with
    VERSION would ship a binary whose About page and update dialog contradict each other.
    Comparison is on the marketing string, so it holds for finals ('0.0.2', no stage
    segment) as well as pre-releases.

    Grammar is validated, not assumed. short_version() only strips a leading 'v' — it
    never parses — so comparing short_version(tag) to the file would pass a matching
    pair that _TAG_RE rejects (e.g. 'v0.0.1-gamma.1': not a known stage). That would
    clear this gate and then blow up in bundle_version() further down the release job,
    after the build — exactly the late failure the early gate exists to prevent. Parse
    both sides here so a bad version dies at the gate.
    """
    if source_version is None:
        source_version = read_source_version()
    try:
        # _parse() raises on an unparseable core or an unknown stage. Run it over both
        # sides: the tag may be well-formed while VERSION is not, or vice versa.
        _parse(tag)
        _parse(source_version)
    except ValueError:
        return False
    return short_version(tag) == source_version


def field_value(field, tag=None):
    """Resolve a --field selector. 'source' reads VERSION; the rest parse the tag."""
    if field == "source":
        return read_source_version()
    if tag is None:
        raise ValueError(f"--field {field} requires --tag")
    return bundle_version(tag) if field == "bundle" else short_version(tag)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tag")
    parser.add_argument("--field", choices=["bundle", "short", "source"])
    parser.add_argument(
        "--check-tag", metavar="TAG",
        help="exit non-zero unless TAG matches roamux/build/VERSION (roam-156)")
    parser.add_argument(
        "--at-least", metavar="OTHER_TAG",
        help="with --tag: exit 0 when --tag orders at or above OTHER_TAG on the bundle "
             "encoding, 1 when it is older, 2 when either tag is unparseable (roam-285)")
    args = parser.parse_args()

    if args.at_least is not None:
        # The publish step maps 0/1 to make_latest=true/false and treats ANY other exit as an
        # error (no PATCH), so unparseable input must never look like "older".
        if args.tag is None:
            print("--at-least requires --tag", file=sys.stderr)
            return 2
        try:
            ok = at_least(args.tag, args.at_least)
        except ValueError as e:
            print(f"cannot order {args.tag!r} against {args.at_least!r}: {e}", file=sys.stderr)
            return 2
        print(f"{args.tag} is {'at least' if ok else 'older than'} {args.at_least}")
        return 0 if ok else 1

    if args.check_tag:
        source = read_source_version()
        if not check_tag(args.check_tag, source_version=source):
            # Name both values and the fix: this fires on a pushed tag, so whoever
            # sees it is mid-release and needs to know which side to correct.
            print(f"release tag {args.check_tag!r} does not match "
                  f"{_VERSION_FILE.name} ({_VERSION_KEY}={source!r}).\n"
                  f"Fix: either re-tag as v{source}, or update "
                  f"roamux/build/VERSION to the version you meant to ship "
                  f"and re-tag.", file=sys.stderr)
            return 1
        print(source)
        return 0

    if not args.field:
        parser.error("one of --field or --check-tag is required")
    if args.field != "source" and not args.tag:
        parser.error(f"--field {args.field} requires --tag")
    print(field_value(args.field, args.tag))
    return 0


if __name__ == "__main__":
    sys.exit(main())
