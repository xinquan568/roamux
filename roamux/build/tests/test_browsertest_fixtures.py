# SPDX-License-Identifier: Apache-2.0
"""Every test the overlay owns or edits is built AND run by tier-2, or is registered (roam-282).

Grill H14: tier-2 ran roamux_browsertests under --gtest_filter="Roamux*", which silently dropped
the one fixture not named Roamux* (ThreeCarrierTest — the only three-carrier import survival
proof) for months. Grill H15: ~1,040 lines of tests compiled only into targets no workflow builds
(patch-carried importer unit tests in upstream unit_tests, roamux_sparkle_tests never on the
tier-2 line, the settings-about WebUI suite in browser_tests, patch-pinned upstream fixtures).
Grill M29: roamux_enable_sparkle was silent in reference.gn although CI and release build with it
on — and the arg decides which test targets exist at all.

This module binds the three into one hermetic invariant, scoped wider than its (issue-given) name:

  inventory   = overlay gtest sources (*_unittest|*_browsertest|*_test .cc/.mm under roamux/,
                third_party excluded)
              + every build_webui_tests() block's cc_test_files and files
              + every test-named upstream path touched by a roamux/patches/*.patch
                (git apply --numstat — the runhook's own parser; no Chromium checkout needed)
  reachable   = the item is in a roamux/BUILD.gn test() target that tier2_job.sh both BUILDS
                (its autoninja line) and RUNS (a "${OUT}/<target>" line), and every case extracted
                from it is admitted by that run's --gtest_filter (if any) and is not
                DISABLED_/MAYBE_-prefixed (admission is not execution)
  registered  = a well-formed row in roamux/build/ci/unbuilt_tests_register.txt

Every inventory item must be reachable or registered. Rows are REJECTED once their item is
reachable or once tier-2 builds and runs the upstream target they name (re-entry, enforced).
Sources inside `if (roamux_enable_sparkle) { … }` count as built only while reference.gn pins the
arg true (M29 and reachability are one invariant). ci.yml and nightly.yml must both invoke
tier2_job.sh, which is what makes that script the definition of "workflow-invoked".

Filter grammar — a stated, strict subset of gtest's: positive ':'-separated patterns with '*' and
'?', exact against Fixture.Case. Parameterized cases (TEST_P + INSTANTIATE_TEST_SUITE_P) are named
Prefix/Fixture.Case/<index> with the index UNKNOWN; they are proved covered only by a pattern that
ends in '*' and matches the prefix Prefix/Fixture.Case/ (such a pattern matches every index).
Anything else — a '-' negative section, a pattern that leaves a parameterized case unproven — fails
the check as unsupported rather than being approximated. After roam-282 tier-2 carries no filter.
"""

import fnmatch
import pathlib
import re
import shutil
import subprocess
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[3]
CAPABILITY_ARG = "roamux_enable_sparkle"
TIER2_SCRIPT = "roamux/build/ci/tier2_job.sh"
REGISTER = "roamux/build/ci/unbuilt_tests_register.txt"

OVERLAY_TEST_RE = re.compile(r"(_unittest|_browsertest|_test)\.(cc|mm)$")
UPSTREAM_TEST_RE = re.compile(r"(_unittest|_browsertest|_interactive_uitest|_test)\.(cc|mm)$")
CASE_RE = re.compile(
    r"\b(IN_PROC_BROWSER_TEST_[FP]|TYPED_TEST(?:_P)?|TEST_[FP]|TEST)\s*\(\s*(\w+)\s*,\s*(\w+)", re.S)
INSTANTIATE_RE = re.compile(r"\bINSTANTIATE_TEST_SUITE_P\s*\(\s*(\w+)\s*,\s*(\w+)", re.S)
GN_TEST_RE = re.compile(r'^[ \t]*test\("(\w+)"\)\s*\{', re.M)
GN_GATE_RE = re.compile(r"^[ \t]*if\s*\(\s*" + CAPABILITY_ARG + r"\s*\)\s*\{", re.M)
GN_SOURCE_RE = re.compile(r'"([^"]+\.(?:cc|mm))"')
WEBUI_BLOCK_RE = re.compile(r'build_webui_tests\("[^"]*"\)\s*\{')
WEBUI_LIST_RE = re.compile(r"^\s*(cc_test_files|files)\s*=\s*\[(.*?)\]", re.M | re.S)
BUILD_LINE_RE = re.compile(r'^autoninja -C "\$\{OUT\}"\s+(.+?)\s*$', re.M)
RUN_LINE_RE = re.compile(r'^"\$\{OUT\}/(\w+)"(.*)$', re.M)
FILTER_RE = re.compile(r'--gtest_filter="?([^"\s]+)"?')
OWNER_RE = re.compile(r"\broam-\d+\b")


class UnsupportedFilter(ValueError):
    """A --gtest_filter form outside the stated subset."""


def _brace_block(text, open_index):
    """text[open_index] is '{'; return the index one past its matching '}'."""
    depth = 0
    for i in range(open_index, len(text)):
        depth += (text[i] == "{") - (text[i] == "}")
        if depth == 0:
            return i + 1
    raise ValueError("unbalanced braces")


def parse_gn_tests(build_gn_text):
    """{target: {source_path: gated}} from every test("…") block, at any indentation.

    `gated` is True when the source literal sits inside an `if (roamux_enable_sparkle) { … }`
    block (either the whole target or a `sources += […]` addition)."""
    gated_ranges = []
    for m in GN_GATE_RE.finditer(build_gn_text):
        open_i = build_gn_text.index("{", m.start())
        gated_ranges.append((open_i, _brace_block(build_gn_text, open_i)))

    def gated(pos):
        return any(a <= pos < b for a, b in gated_ranges)

    targets = {}
    for m in GN_TEST_RE.finditer(build_gn_text):
        open_i = build_gn_text.index("{", m.start())
        end = _brace_block(build_gn_text, open_i)
        sources = {}
        for s in GN_SOURCE_RE.finditer(build_gn_text, open_i, end):
            sources[s.group(1)] = gated(s.start())
        targets[m.group(1)] = sources
    return targets


def parse_tier2(script_text):
    """(built targets, {run target: filter or None}) from tier2_job.sh."""
    built = set()
    for m in BUILD_LINE_RE.finditer(script_text):
        built.update(m.group(1).split())
    runs = {}
    for m in RUN_LINE_RE.finditer(script_text):
        f = FILTER_RE.search(m.group(2))
        runs[m.group(1)] = f.group(1) if f else None
    return built, runs


def extract_cases(source_text):
    """[(name, parameterized, disabled)] — parameterized names carry a trailing '/' in place of
    the unknown index; an uninstantiated TEST_P yields nothing (gtest runs nothing for it)."""
    instantiations = {}
    for m in INSTANTIATE_RE.finditer(source_text):
        instantiations.setdefault(m.group(2), []).append(m.group(1))
    out = []
    for kind, fixture, case in CASE_RE.findall(source_text):
        disabled = (case.startswith("DISABLED_") or case.startswith("MAYBE_")
                    or fixture.startswith("DISABLED_"))
        if kind.endswith("_P"):
            for prefix in instantiations.get(fixture, []):
                out.append((f"{prefix}/{fixture}.{case}/", True, disabled))
        else:
            out.append((f"{fixture}.{case}", False, disabled))
    return out


def filter_admits(name, parameterized, gtest_filter):
    """Exact for plain cases; for parameterized cases (name ends with '/') a pattern proves
    coverage only if it ends in '*' and matches the prefix. Raises UnsupportedFilter."""
    if gtest_filter is None:
        return True
    if "-" in gtest_filter:
        raise UnsupportedFilter(f"negative section in {gtest_filter!r}")
    patterns = [p for p in gtest_filter.split(":") if p]
    if parameterized:
        return any(p.endswith("*") and fnmatch.fnmatchcase(name, p) for p in patterns)
    return any(fnmatch.fnmatchcase(name, p) for p in patterns)


def _reference_pins_capability(repo):
    text = (repo / "roamux/build/args/reference.gn").read_text()
    for line in text.splitlines():
        s = line.strip()
        if s.startswith("#"):
            continue
        m = re.match(CAPABILITY_ARG + r"\s*=\s*(\S+)", s)
        if m:
            return m.group(1) == "true"
    return False


def _patch_touched_paths(patch):
    r = subprocess.run(["git", "apply", "--numstat", str(patch)], capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"git apply --numstat failed for {patch.name}: {r.stderr.strip()}")
    return [line.split("\t", 2)[2] for line in r.stdout.splitlines() if line.strip()]


class Checker:
    """The invariant, parametrized by a repo root so synthetic trees can exercise every rule."""

    def __init__(self, repo):
        self.repo = pathlib.Path(repo)
        self.overlay = self.repo / "roamux"
        self.capability_on = _reference_pins_capability(self.repo)
        self.targets = parse_gn_tests((self.overlay / "BUILD.gn").read_text())
        self.built, self.runs = parse_tier2((self.repo / TIER2_SCRIPT).read_text())
        self.inventory = self._inventory()          # {repo-or-src-relative path: kind}

    # -- inventory -------------------------------------------------------------------------------
    def _inventory(self):
        items = {}
        for p in sorted(self.overlay.rglob("*")):
            rel = p.relative_to(self.repo).as_posix()
            if "third_party" in p.relative_to(self.overlay).parts:
                continue
            if p.is_file() and OVERLAY_TEST_RE.search(p.name):
                items[rel] = "overlay"
        for gn in sorted(self.overlay.rglob("BUILD.gn")):
            text = gn.read_text()
            for m in WEBUI_BLOCK_RE.finditer(text):
                open_i = text.index("{", m.start())
                block = text[open_i:_brace_block(text, open_i)]
                for _, body in WEBUI_LIST_RE.findall(block):
                    for f in re.findall(r'"([^"]+)"', body):
                        rel = (gn.parent / f).resolve().relative_to(self.repo).as_posix()
                        items.setdefault(rel, "webui")
        patches = self.overlay / "patches"
        if patches.is_dir():
            for patch in sorted(patches.glob("*.patch")):
                for path in _patch_touched_paths(patch):
                    if UPSTREAM_TEST_RE.search(pathlib.PurePosixPath(path).name):
                        items.setdefault(path, "upstream")
        return items

    # -- reachability ----------------------------------------------------------------------------
    def homes(self, repo_rel):
        """Targets whose source list contains this overlay path (gated sources only count while
        the reference config pins the capability on)."""
        try:
            overlay_rel = pathlib.PurePosixPath(repo_rel).relative_to("roamux").as_posix()
        except ValueError:
            return []
        return [t for t, sources in self.targets.items()
                if overlay_rel in sources and (self.capability_on or not sources[overlay_rel])]

    def unreachable_reason(self, path):
        """None when reachable, else a one-line reason."""
        kind = self.inventory.get(path)
        if kind == "upstream" or path.endswith(".ts"):
            return "compiles only into an upstream target no workflow builds"
        homes = self.homes(path)
        if not homes:
            return ("in no roamux/BUILD.gn test() target (or only in a capability-gated one "
                    f"while reference.gn does not pin {CAPABILITY_ARG}=true)")
        reasons = []
        for target in homes:
            if target not in self.built:
                reasons.append(f"target {target} is not on the tier-2 autoninja line")
                continue
            if target not in self.runs:
                reasons.append(f"target {target} is built but never run by tier-2")
                continue
            gtest_filter = self.runs[target]
            dropped = []
            for name, parameterized, disabled in extract_cases((self.repo / path).read_text()):
                if disabled:
                    dropped.append(f"{name} (disabled — never executed)")
                    continue
                try:
                    if not filter_admits(name, parameterized, gtest_filter):
                        dropped.append(f"{name} (not proved covered by {gtest_filter!r})")
                except UnsupportedFilter as e:
                    dropped.append(f"{name} (unsupported filter form: {e})")
            if dropped:
                reasons.append(f"target {target}: " + "; ".join(dropped))
            else:
                return None  # reachable through this target
        return " / ".join(reasons)

    # -- register --------------------------------------------------------------------------------
    def register_rows(self):
        rows = []
        for n, line in enumerate((self.repo / REGISTER).read_text().splitlines(), start=1):
            s = line.strip()
            if not s or s.startswith("#"):
                continue
            rows.append((n, [f.strip() for f in s.split("|")]))
        return rows

    def register_problems(self):
        problems = []
        for n, fields in self.register_rows():
            if len(fields) != 4:
                problems.append(f"line {n}: expected 4 '|'-separated fields, got {len(fields)}")
                continue
            path, target, owner, reentry = fields
            if not OWNER_RE.search(owner):
                problems.append(f"line {n}: owner {owner!r} names no roam-N")
            if not reentry:
                problems.append(f"line {n}: empty re-entry condition")
            if path not in self.inventory:
                problems.append(f"line {n}: {path} is not an inventory item (stale row?)")
                continue
            if self.unreachable_reason(path) is None:
                problems.append(f"line {n}: {path} is now built and run by tier-2 — delete the row")
            if target in self.built and target in self.runs:
                problems.append(f"line {n}: tier-2 now builds and runs {target} — delete the row")
        return problems

    def registered_paths(self):
        return {fields[0] for _, fields in self.register_rows() if fields}

    def reachability_failures(self):
        registered = self.registered_paths()
        return [f"{p}: {reason}" for p in sorted(self.inventory)
                for reason in [self.unreachable_reason(p)]
                if reason is not None and p not in registered]


# --------------------------------------------------------------------------------------------------
class RealTreeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.checker = Checker(REPO)

    def test_reference_gn_pins_the_capability_that_creates_the_sparkle_targets(self):
        # M29 prerequisite: roamux_sparkle_tests (and the update-service test sources) exist only
        # under roamux_enable_sparkle=true; the reference config must match CI/release reality.
        self.assertTrue(self.checker.capability_on,
                        f"roamux/build/args/reference.gn does not pin {CAPABILITY_ARG} = true")

    def test_ci_and_nightly_both_invoke_tier2_job(self):
        for wf in ("ci.yml", "nightly.yml"):
            text = (REPO / ".github/workflows" / wf).read_text()
            self.assertIn(f"bash {TIER2_SCRIPT}", text,
                          f"{wf} no longer runs {TIER2_SCRIPT}; this module's notion of "
                          "'workflow-invoked' would be stale")

    def test_inventory_covers_the_three_sources(self):
        kinds = set(self.checker.inventory.values())
        self.assertEqual({"overlay", "webui", "upstream"}, kinds,
                         "the inventory must draw from overlay sources, WebUI test inputs and "
                         f"patch-touched upstream tests; got {sorted(kinds)}")

    def test_every_owned_or_edited_test_is_built_and_run_or_registered(self):
        failures = self.checker.reachability_failures()
        self.assertEqual([], failures, "\n".join(["unreachable and unregistered:"] + failures))

    def test_register_rows_are_well_formed_and_still_needed(self):
        problems = self.checker.register_problems()
        self.assertEqual([], problems, "\n".join(["register problems:"] + problems))


class FilterGrammarTest(unittest.TestCase):
    def test_no_filter_admits_everything(self):
        self.assertTrue(filter_admits("ThreeCarrierTest.X", False, None))

    def test_prefix_pattern_drops_the_fixture_h14_lost(self):
        self.assertFalse(filter_admits("ThreeCarrierTest.CookieLocalStorageIndexedDbAllSurvive",
                                       False, "Roamux*"))
        self.assertTrue(filter_admits("RoamuxTabUidTest.Case", False, "Roamux*"))

    def test_negative_section_is_unsupported(self):
        with self.assertRaises(UnsupportedFilter):
            filter_admits("A.B", False, "*-A.B")

    def test_parameterized_coverage_needs_a_star_terminated_prefix_match(self):
        name = "AllCombinations/TabStripPinControllerD4Test.TruthTable/"
        self.assertTrue(filter_admits(name, True, "*TruthTable/*"))
        self.assertTrue(filter_admits(name, True, "AllCombinations/*"))
        self.assertFalse(filter_admits(name, True, "*TruthTable/?"),   # admits /0, misses /10-15
                         "a '?' index pattern must be treated as unproven")
        self.assertFalse(filter_admits(name, True, "*TruthTable/0"))

    def test_case_extraction_handles_multiline_macros_and_instantiations(self):
        src = ('IN_PROC_BROWSER_TEST_F(ThreeCarrierTest,\n    Survive) {}\n'
               'TEST_P(PTest, Truth) {}\nINSTANTIATE_TEST_SUITE_P(All, PTest, testing::Range(0, 16));\n'
               'TEST_F(F, DISABLED_Flaky) {}\nTEST(Plain, Case) {}\n')
        self.assertEqual([("ThreeCarrierTest.Survive", False, False),
                          ("All/PTest.Truth/", True, False),
                          ("F.DISABLED_Flaky", False, True),
                          ("Plain.Case", False, False)], extract_cases(src))


class GnParsingTest(unittest.TestCase):
    def test_nested_and_gated_targets_are_seen(self):
        gn = ('if (roamux_enable_sparkle) {\n  test("gated") {\n    sources = [ "test/g_test.mm" ]\n  }\n}\n'
              'test("plain") {\n  sources = [ "test/a_unittest.cc" ]\n'
              '  if (roamux_enable_sparkle) {\n    sources += [ "test/b_unittest.cc" ]\n  }\n}\n')
        self.assertEqual({"gated": {"test/g_test.mm": True},
                          "plain": {"test/a_unittest.cc": False, "test/b_unittest.cc": True}},
                         parse_gn_tests(gn))

    def test_tier2_lines_are_parsed(self):
        sh = ('autoninja -C "${OUT}" a b\n"${OUT}/a" --test-launcher-retry-limit=2\n'
              '"${OUT}/b" --gtest_filter="Roamux*" --test-launcher-retry-limit=2\n')
        self.assertEqual(({"a", "b"}, {"a": None, "b": "Roamux*"}), parse_tier2(sh))


# --------------------------------------------------------------------------------------------------
# Synthetic trees — one per rule, so a regression in any rule is red for a stated reason.
def _fake_repo(tmp, *, build_gn, script, register="", sources=None, patches=None,
               reference="roamux_enable_sparkle = true\n"):
    repo = pathlib.Path(tmp)
    (repo / "roamux/build/ci").mkdir(parents=True)
    (repo / "roamux/build/args").mkdir(parents=True)
    (repo / "roamux/test").mkdir(parents=True)
    (repo / "roamux/patches").mkdir(parents=True)
    (repo / "roamux/BUILD.gn").write_text(build_gn)
    (repo / TIER2_SCRIPT).write_text(script)
    (repo / REGISTER).write_text(register)
    (repo / "roamux/build/args/reference.gn").write_text(reference)
    for name, text in (sources or {}).items():
        (repo / "roamux/test" / name).write_text(text)
    for name, text in (patches or {}).items():
        (repo / "roamux/patches" / name).write_text(text)
    return repo


PLAIN_GN = 'test("t") {\n  sources = [ "test/a_unittest.cc" ]\n}\n'
PLAIN_SH = 'autoninja -C "${OUT}" t\n"${OUT}/t" --test-launcher-retry-limit=2\n'
PLAIN_SRC = {"a_unittest.cc": "TEST(RoamuxA, Case) {}\n"}
UPSTREAM_PATCH = ("diff --git a/chrome/x_unittest.cc b/chrome/x_unittest.cc\n"
                  "--- a/chrome/x_unittest.cc\n+++ b/chrome/x_unittest.cc\n"
                  "@@ -1,1 +1,2 @@\n line\n+added\n")


class SyntheticRegressionTest(unittest.TestCase):
    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp(prefix="roamux-fixtures-"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)

    def test_clean_tree_has_no_failures(self):
        c = Checker(_fake_repo(self.tmp, build_gn=PLAIN_GN, script=PLAIN_SH, sources=PLAIN_SRC))
        self.assertEqual([], c.reachability_failures())
        self.assertEqual([], c.register_problems())

    def test_unregistered_unhomed_source_fails(self):
        srcs = dict(PLAIN_SRC, **{"b_unittest.cc": "TEST(RoamuxB, Case) {}\n"})
        c = Checker(_fake_repo(self.tmp, build_gn=PLAIN_GN, script=PLAIN_SH, sources=srcs))
        self.assertEqual(1, len(c.reachability_failures()))
        self.assertIn("roamux/test/b_unittest.cc: in no roamux/BUILD.gn test() target",
                      c.reachability_failures()[0])

    def test_built_but_not_run_target_fails(self):
        c = Checker(_fake_repo(self.tmp, build_gn=PLAIN_GN, script='autoninja -C "${OUT}" t\n',
                               sources=PLAIN_SRC))
        self.assertIn("built but never run", c.reachability_failures()[0])

    def test_filter_that_drops_a_case_fails(self):
        sh = 'autoninja -C "${OUT}" t\n"${OUT}/t" --gtest_filter="Roamux*" --test-launcher-retry-limit=2\n'
        srcs = {"a_unittest.cc": "TEST(RoamuxA, Case) {}\nTEST(ThreeCarrierTest, Survive) {}\n"}
        c = Checker(_fake_repo(self.tmp, build_gn=PLAIN_GN, script=sh, sources=srcs))
        self.assertIn("ThreeCarrierTest.Survive (not proved covered by 'Roamux*')",
                      c.reachability_failures()[0])

    def test_negative_filter_is_reported_as_unsupported(self):
        sh = 'autoninja -C "${OUT}" t\n"${OUT}/t" --gtest_filter="*-RoamuxA.Case" --test-launcher-retry-limit=2\n'
        c = Checker(_fake_repo(self.tmp, build_gn=PLAIN_GN, script=sh, sources=PLAIN_SRC))
        self.assertIn("unsupported filter form", c.reachability_failures()[0])

    def test_unproven_parameterized_case_fails(self):
        sh = 'autoninja -C "${OUT}" t\n"${OUT}/t" --gtest_filter="All/RoamuxP.Truth/?" --test-launcher-retry-limit=2\n'
        srcs = {"a_unittest.cc": "TEST_P(RoamuxP, Truth) {}\n"
                                 "INSTANTIATE_TEST_SUITE_P(All, RoamuxP, testing::Range(0, 16));\n"}
        c = Checker(_fake_repo(self.tmp, build_gn=PLAIN_GN, script=sh, sources=srcs))
        self.assertIn("All/RoamuxP.Truth/ (not proved covered", c.reachability_failures()[0])

    def test_disabled_case_is_not_execution(self):
        srcs = {"a_unittest.cc": "TEST(RoamuxA, DISABLED_Case) {}\n"}
        c = Checker(_fake_repo(self.tmp, build_gn=PLAIN_GN, script=PLAIN_SH, sources=srcs))
        self.assertIn("disabled — never executed", c.reachability_failures()[0])

    def test_gated_source_counts_only_with_the_reference_pin(self):
        gn = 'test("t") {\n  sources = [ "test/a_unittest.cc" ]\n  if (roamux_enable_sparkle) {\n    sources += [ "test/b_unittest.cc" ]\n  }\n}\n'
        srcs = dict(PLAIN_SRC, **{"b_unittest.cc": "TEST(RoamuxB, Case) {}\n"})
        on = Checker(_fake_repo(self.tmp, build_gn=gn, script=PLAIN_SH, sources=srcs))
        self.assertEqual([], on.reachability_failures())
        off = Checker(_fake_repo(self.tmp / "off", build_gn=gn, script=PLAIN_SH, sources=srcs,
                                 reference="# nothing pinned\n"))
        self.assertFalse(off.capability_on)
        self.assertEqual(1, len(off.reachability_failures()))
        self.assertIn("b_unittest.cc", off.reachability_failures()[0])

    def test_register_row_without_owner_is_rejected(self):
        reg = "chrome/x_unittest.cc | unit_tests | nobody | M41\n"
        c = Checker(_fake_repo(self.tmp, build_gn=PLAIN_GN, script=PLAIN_SH, sources=PLAIN_SRC,
                               register=reg, patches={"0001-x.patch": UPSTREAM_PATCH}))
        self.assertEqual([], c.reachability_failures())
        self.assertIn("names no roam-N", c.register_problems()[0])

    def test_register_row_with_empty_reentry_is_rejected(self):
        reg = "chrome/x_unittest.cc | unit_tests | roam-1 | \n"
        c = Checker(_fake_repo(self.tmp, build_gn=PLAIN_GN, script=PLAIN_SH, sources=PLAIN_SRC,
                               register=reg, patches={"0001-x.patch": UPSTREAM_PATCH}))
        self.assertIn("empty re-entry condition", c.register_problems()[0])

    def test_stale_register_row_is_rejected(self):
        reg = "chrome/gone_unittest.cc | unit_tests | roam-1 | M41\n"
        c = Checker(_fake_repo(self.tmp, build_gn=PLAIN_GN, script=PLAIN_SH, sources=PLAIN_SRC,
                               register=reg))
        self.assertIn("is not an inventory item", c.register_problems()[0])

    def test_register_row_for_a_reachable_item_is_rejected(self):
        reg = "roamux/test/a_unittest.cc | unit_tests | roam-1 | M41\n"
        c = Checker(_fake_repo(self.tmp, build_gn=PLAIN_GN, script=PLAIN_SH, sources=PLAIN_SRC,
                               register=reg))
        self.assertIn("is now built and run by tier-2 — delete the row", c.register_problems()[0])

    def test_register_row_naming_a_built_and_run_target_is_rejected(self):
        # The item itself stays unreachable (an upstream path); only the named target has joined
        # the tier-2 build AND run lines — the M41 re-entry moment.
        sh = 'autoninja -C "${OUT}" t unit_tests\n"${OUT}/t" --test-launcher-retry-limit=2\n"${OUT}/unit_tests" --test-launcher-retry-limit=2\n'
        reg = "chrome/x_unittest.cc | unit_tests | roam-1 | M41\n"
        c = Checker(_fake_repo(self.tmp, build_gn=PLAIN_GN, script=sh, sources=PLAIN_SRC,
                               register=reg, patches={"0001-x.patch": UPSTREAM_PATCH}))
        self.assertEqual(["line 1: tier-2 now builds and runs unit_tests — delete the row"],
                         c.register_problems())
        built_only = Checker(_fake_repo(self.tmp / "b", build_gn=PLAIN_GN, sources=PLAIN_SRC,
                                        script='autoninja -C "${OUT}" t unit_tests\n"${OUT}/t" --test-launcher-retry-limit=2\n',
                                        register=reg, patches={"0001-x.patch": UPSTREAM_PATCH}))
        self.assertEqual([], built_only.register_problems(), "built-but-not-run is not re-entry")


if __name__ == "__main__":
    unittest.main()
