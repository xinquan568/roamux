# SPDX-License-Identifier: Apache-2.0
"""Tests for fieldtrial_inventory.py — the ADR 0002 per-pin inventory generator (roam-342).

Two layers:

* Hermetic (tier-1 / pre-push safe): a throwaway git repo tagged like a milestone pin carries a
  tiny ``fieldtrial_testing_config.json`` plus feature sources exercising every branch of the
  ``textual-v1`` resolver — including its four documented limitations, which are part of the
  contract because the M149 oracle depends on them.

* Oracle (checkout-bound, opt-in): ``REQUIRE_FIELDTRIAL_ORACLE=1`` + ``ROAMUX_CHROMIUM_SRC``
  runs the real tool against tag ``149.0.7827.201`` and compares the six bucket lists with the
  committed oracle ``data/fieldtrial_inventory/m149-effective-diff.json`` (issue #241's published
  M149 inventory, preserved here because it used to live only in a local run folder).

  The oracle tag is a PERSISTENT prerequisite of this test, independent of CHROMIUM_PIN: it stays
  ``149.0.7827.201`` through every future uprev. A fresh or shallow runner checkout must fetch it
  (the gate test below prints the command). Tier-2 runs this module fail-not-skip
  (``roamux/build/ci/tier2_job.sh``, phase ``fieldtrial-inventory-oracle``).
"""

import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest

SCRIPT = pathlib.Path(__file__).resolve().parent.parent / "fieldtrial_inventory.py"
DATA = pathlib.Path(__file__).resolve().parent / "data" / "fieldtrial_inventory"
ORACLE_TAG = "149.0.7827.201"
REQUIRE_ORACLE = os.environ.get("REQUIRE_FIELDTRIAL_ORACLE") == "1"
CHROMIUM_SRC = os.environ.get("ROAMUX_CHROMIUM_SRC", "")

CONFIG_REL = "testing/variations/fieldtrial_testing_config.json"


def _git_env():
    return {k: v for k, v in os.environ.items()
            if k not in ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_PREFIX",
                         "GIT_COMMON_DIR", "GIT_OBJECT_DIRECTORY")}


def git(cwd, *args):
    return subprocess.run(["git", "-C", str(cwd), *args], check=True, capture_output=True,
                          text=True, env=_git_env()).stdout


def _study(platforms, *experiments):
    return [{"platforms": platforms, "experiments": list(experiments)}]


def _exp(name, enable=(), disable=(), params=None):
    e = {"name": name}
    if enable:
        e["enable_features"] = list(enable)
    if disable:
        e["disable_features"] = list(disable)
    if params:
        e["params"] = params
    return e


FIXTURE_CONFIG = {
    # no-op (Alpha compiled ENABLED) + the only params-carrying activated experiment
    "AlphaStudy": _study(["mac", "windows"], _exp("Enabled", enable=["Alpha"], params={"x": "1"})),
    # flip via the 2-argument form (name derived from kBeta)
    "BetaStudy": _study(["mac"], _exp("Enabled", enable=["Beta"])),
    # forced-OFF flip via the 3-argument form ("Gamma" string name, kGammaFeature identifier)
    "GammaStudy": _study(["mac"], _exp("Disabled", disable=["Gamma"])),
    # forced-OFF no-op
    "DeltaStudy": _study(["mac"], _exp("Disabled", disable=["Delta"])),
    # limitation 1: bare FEATURE_DISABLED_BY_DEFAULT (namespace base) -> unresolved
    "BareStudy": _study(["mac"], _exp("Enabled", enable=["Bare"])),
    # limitation 1b: base::FeatureState::FEATURE_... spelling -> unresolved
    "StateStudy": _study(["mac"], _exp("Enabled", enable=["State"])),
    # limitation 2: #if inside the macro argument list -> unresolved
    "IfStudy": _study(["mac"], _exp("Enabled", enable=["Iffy"])),
    # no definition anywhere -> unresolved
    "MissingStudy": _study(["mac"], _exp("Enabled", enable=["Missing"])),
    # the literal token is a whole identifier: base::FEATURE_DISABLED_BY_DEFAULT_SUFFIX is not it
    "SuffixStudy": _study(["mac"], _exp("Enabled", enable=["Suffix"])),
    # limitation 3: platform-conditional duplicate; the textually-first (non-mac) branch wins
    "DupStudy": _study(["mac"], _exp("Enabled", enable=["Dup"])),
    # limitation 4: a *_browsertest.cc that sorts first supplies the default
    "CrossStudy": _study(["mac"], _exp("Enabled", enable=["Cross"])),
    # unanchored match: MAKE_STATIC_STORAGE_BASE_FEATURE(...) resolves
    "DohStudy": _study(["mac"], _exp("Enabled", enable=["Doh"])),
    # only the first-listed experiment activates; the second's feature + params are ignored
    "MultiExpStudy": _study(["mac"], _exp("First", enable=["Second0"]),
                            _exp("Second", enable=["SecondIgnored"], params={"y": "2"})),
    # not a mac study at all
    "WinStudy": _study(["windows"], _exp("Enabled", enable=["WinOnly"], params={"z": "3"})),
}

FIXTURE_SOURCES = {
    "chrome/common/chrome_features.cc": """\
// fixture
#include "base/feature.h"
namespace features {
BASE_FEATURE(kAlpha, "Alpha", base::FEATURE_ENABLED_BY_DEFAULT);
BASE_FEATURE(kBeta, base::FEATURE_DISABLED_BY_DEFAULT);
BASE_FEATURE(kGammaFeature, "Gamma", base::FEATURE_ENABLED_BY_DEFAULT);
BASE_FEATURE(kDelta,
             base::FEATURE_DISABLED_BY_DEFAULT);
BASE_FEATURE(kState, base::FeatureState::FEATURE_DISABLED_BY_DEFAULT);
BASE_FEATURE(kIffy,
#if BUILDFLAG(IS_WIN)
             base::FEATURE_ENABLED_BY_DEFAULT);
#else
             base::FEATURE_DISABLED_BY_DEFAULT);
#endif
#if BUILDFLAG(IS_ANDROID)
BASE_FEATURE(kDup, base::FEATURE_DISABLED_BY_DEFAULT);
#else
BASE_FEATURE(kDup, base::FEATURE_ENABLED_BY_DEFAULT);
#endif
BASE_FEATURE(kCross, base::FEATURE_ENABLED_BY_DEFAULT);
BASE_FEATURE(kSecond0, base::FEATURE_ENABLED_BY_DEFAULT);
BASE_FEATURE(kSecondIgnored, base::FEATURE_DISABLED_BY_DEFAULT);
BASE_FEATURE(kWinOnly, base::FEATURE_DISABLED_BY_DEFAULT);
BASE_FEATURE(kSuffix, base::FEATURE_DISABLED_BY_DEFAULT_SUFFIX);
}  // namespace features
""",
    "base/features.cc": """\
// fixture: inside namespace base the token is unqualified
namespace base {
BASE_FEATURE(kBare, FEATURE_DISABLED_BY_DEFAULT);
}  // namespace base
""",
    # sorts BEFORE chrome/... in path order: the test definition wins for Cross
    "a/cross_browsertest.cc": """\
BASE_FEATURE(kCross, base::FEATURE_DISABLED_BY_DEFAULT);
""",
    "net/dns/doh_provider_entry.cc": """\
const DohProviderEntry kEntry{
    "Doh",
    MAKE_STATIC_STORAGE_BASE_FEATURE(kDoh, base::FEATURE_DISABLED_BY_DEFAULT),
};
""",
    # a header carrying a definition must not change any bucket (Alpha is already resolved)
    "chrome/common/chrome_features.h": """\
// BASE_FEATURE(kAlpha, "Alpha", base::FEATURE_DISABLED_BY_DEFAULT);  (a comment; harmless)
""",
}

EXPECTED = {
    "studies": 13,
    "forced_on_directives": 11,
    "forced_off_directives": 2,
    "params_vanishing": 1,
    "forced_on": {
        "effective_flips": ["Beta", "Cross", "Doh", "Dup"],
        "noops": ["Alpha", "Second0"],
        "unresolved": ["Bare", "Iffy", "Missing", "State", "Suffix"],
    },
    "forced_off": {
        "effective_flips": ["Gamma"],
        "noops": ["Delta"],
        "unresolved": [],
    },
}


class _FixtureRepo(unittest.TestCase):
    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp(prefix="roamux-ft-inventory-"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.src = self.tmp / "src"
        self.src.mkdir()
        git(self.src, "init", "-q")
        git(self.src, "config", "user.email", "test@roamux")
        git(self.src, "config", "user.name", "test")
        self._write(CONFIG_REL, json.dumps(FIXTURE_CONFIG, indent=1))
        for rel, text in FIXTURE_SOURCES.items():
            self._write(rel, text)
        git(self.src, "add", ".")
        git(self.src, "commit", "-qm", "pin")
        git(self.src, "tag", "1.0.0.0")
        self.pin_file = self.tmp / "CHROMIUM_PIN"
        self.pin_file.write_text("# pin\n1.0.0.0\n")
        self.out = self.tmp / "out"

    def _write(self, rel, text):
        p = self.src / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text)

    def run_tool(self, *extra, out=None, pin_file=None):
        return subprocess.run(
            [sys.executable, str(SCRIPT), "--chromium-src", str(self.src),
             "--pin-file", str(pin_file or self.pin_file),
             "--out-dir", str(out or self.out), *extra],
            capture_output=True, text=True, env=_git_env())

    def read_json(self, out=None):
        return json.loads(((out or self.out) / "effective-diff.json").read_text())


class ResolverTest(_FixtureRepo):
    def test_buckets_match_the_documented_rule(self):
        r = self.run_tool()
        self.assertEqual(r.returncode, 0, r.stderr)
        got = self.read_json()
        for key in ("studies", "forced_on_directives", "forced_off_directives", "params_vanishing"):
            self.assertEqual(got[key], EXPECTED[key], key)
        self.assertEqual(got["forced_on"], EXPECTED["forced_on"])
        self.assertEqual(got["forced_off"], EXPECTED["forced_off"])
        self.assertEqual(got["pin"], "1.0.0.0")
        self.assertEqual(got["platform"], "mac")
        self.assertEqual(got["rule"], "textual-v1")

    def test_lists_are_sorted_and_output_is_deterministic(self):
        self.assertEqual(self.run_tool().returncode, 0)
        first = (self.out / "effective-diff.json").read_bytes()
        first_md = (self.out / "inventory.md").read_bytes()
        other = self.tmp / "out2"
        self.assertEqual(self.run_tool(out=other).returncode, 0)
        self.assertEqual(first, (other / "effective-diff.json").read_bytes())
        self.assertEqual(first_md, (other / "inventory.md").read_bytes())
        got = self.read_json()
        for direction in ("forced_on", "forced_off"):
            for bucket, names in got[direction].items():
                self.assertEqual(names, sorted(names), f"{direction}.{bucket} not sorted")

    def test_reads_the_tag_not_the_working_tree(self):
        # Mutate the config AND a feature default without committing: the inventory must not move.
        cfg = json.loads((self.src / CONFIG_REL).read_text())
        cfg["ExtraStudy"] = _study(["mac"], _exp("Enabled", enable=["Alpha"]))
        (self.src / CONFIG_REL).write_text(json.dumps(cfg))
        p = self.src / "chrome/common/chrome_features.cc"
        p.write_text(p.read_text().replace(
            "BASE_FEATURE(kBeta, base::FEATURE_DISABLED_BY_DEFAULT);",
            "BASE_FEATURE(kBeta, base::FEATURE_ENABLED_BY_DEFAULT);"))
        r = self.run_tool()
        self.assertEqual(r.returncode, 0, r.stderr)
        got = self.read_json()
        self.assertEqual(got["studies"], EXPECTED["studies"])
        self.assertIn("Beta", got["forced_on"]["effective_flips"])

    def test_inventory_md_states_unresolved_counts_and_limitations(self):
        self.assertEqual(self.run_tool().returncode, 0)
        md = (self.out / "inventory.md").read_text()
        self.assertIn("1.0.0.0", md)
        # The count rows, cell by cell: directives | flips | no-ops | unresolved (bold).
        self.assertIn("| forced-ON | 11 | 4 | 2 | **5** |", md)
        self.assertIn("| forced-OFF | 2 | 1 | 1 | **0** |", md)
        self.assertIn("## Features forced ON — unresolved (5)", md)
        self.assertIn("## Features forced OFF — unresolved (0)", md)
        for phrase in ("textual-v1", "base::FEATURE_", "#if", "first", "test"):
            self.assertIn(phrase, md, f"inventory.md must document the rule's limitations ({phrase})")
        for name in ("Beta", "Gamma", "Bare", "Missing", "Suffix"):
            self.assertIn(name, md)

    def test_wrong_unresolved_count_in_md_is_detected(self):
        # Negative control for the row assertion above: a row with the unresolved cell zeroed must
        # not be present, so the assertion is not satisfiable by a coincidental digit elsewhere.
        self.assertEqual(self.run_tool().returncode, 0)
        md = (self.out / "inventory.md").read_text()
        self.assertNotIn("| forced-ON | 11 | 4 | 2 | **0** |", md)

    def test_summary_on_stdout(self):
        r = self.run_tool()
        self.assertEqual(r.returncode, 0, r.stderr)
        for token in ("forced-ON", "forced-OFF", "unresolved", "1.0.0.0"):
            self.assertIn(token, r.stdout)


class PinHandlingTest(_FixtureRepo):
    def test_unpinned_hard_fails(self):
        self.pin_file.write_text("UNPINNED\n")
        r = self.run_tool()
        self.assertEqual(r.returncode, 1)
        self.assertIn("UNPINNED", r.stderr)
        self.assertFalse(self.out.exists())

    def test_missing_pin_file_hard_fails(self):
        r = self.run_tool(pin_file=self.tmp / "nope")
        self.assertEqual(r.returncode, 1)

    def test_unresolvable_tag_hard_fails_with_fetch_hint(self):
        self.pin_file.write_text("9.9.9.9\n")
        r = self.run_tool()
        self.assertEqual(r.returncode, 1)
        self.assertIn("9.9.9.9", r.stderr)
        self.assertIn("fetch", r.stderr)

    def test_commitish_that_is_not_a_tag_does_not_satisfy_the_pin(self):
        self.pin_file.write_text("HEAD\n")
        r = self.run_tool()
        self.assertEqual(r.returncode, 1)

    def test_tag_flag_overrides_the_pin_file(self):
        self.pin_file.write_text("9.9.9.9\n")
        r = self.run_tool("--tag", "1.0.0.0")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.read_json()["pin"], "1.0.0.0")

    def test_platform_filter(self):
        r = self.run_tool("--platform", "windows")
        self.assertEqual(r.returncode, 0, r.stderr)
        got = self.read_json()
        self.assertEqual(got["platform"], "windows")
        self.assertEqual(got["studies"], 2)  # AlphaStudy + WinStudy
        self.assertEqual(got["params_vanishing"], 2)
        self.assertEqual(got["forced_on"]["effective_flips"], ["WinOnly"])
        self.assertEqual(got["forced_on"]["noops"], ["Alpha"])


class OracleRequirementGateTest(unittest.TestCase):
    """REQUIRE_FIELDTRIAL_ORACLE=1 turns the oracle comparison from skip into a hard run; if the
    checkout or the oracle tag is unusable the gate fails loudly here instead of silently skipping."""

    def test_checkout_and_oracle_tag_present_when_required(self):
        if not REQUIRE_ORACLE:
            self.skipTest("REQUIRE_FIELDTRIAL_ORACLE not set — tier-1 skip behaviour is unchanged")
        self.assertTrue(CHROMIUM_SRC and pathlib.Path(CHROMIUM_SRC).is_dir(),
                        "REQUIRE_FIELDTRIAL_ORACLE=1 but ROAMUX_CHROMIUM_SRC is not a directory")
        r = subprocess.run(["git", "-C", CHROMIUM_SRC, "rev-parse", "--verify",
                            f"refs/tags/{ORACLE_TAG}^{{commit}}"], capture_output=True, text=True,
                           env=_git_env())
        self.assertEqual(
            r.returncode, 0,
            f"REQUIRE_FIELDTRIAL_ORACLE=1 but the oracle tag {ORACLE_TAG} does not resolve in "
            f"{CHROMIUM_SRC}. It is a persistent prerequisite independent of CHROMIUM_PIN; fetch it: "
            f"git -C {CHROMIUM_SRC} fetch origin +refs/tags/{ORACLE_TAG}:refs/tags/{ORACLE_TAG}")


class M149OracleTest(unittest.TestCase):
    """Issue #241's published M149 inventory must be reproduced exactly (roam-342 acceptance)."""

    def setUp(self):
        if not REQUIRE_ORACLE:
            self.skipTest("REQUIRE_FIELDTRIAL_ORACLE not set (checkout-bound; tier-2 runs it)")
        self.tmp = pathlib.Path(tempfile.mkdtemp(prefix="roamux-ft-oracle-"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)

    def test_m149_counts_and_bucket_membership(self):
        oracle = json.loads((DATA / "m149-effective-diff.json").read_text())
        expected = json.loads((DATA / "m149-expected.json").read_text())
        r = subprocess.run(
            [sys.executable, str(SCRIPT), "--chromium-src", CHROMIUM_SRC, "--tag", ORACLE_TAG,
             "--platform", "mac", "--out-dir", str(self.tmp)],
            capture_output=True, text=True, env=_git_env())
        self.assertEqual(r.returncode, 0, r.stderr)
        got = json.loads((self.tmp / "effective-diff.json").read_text())
        for key in ("studies", "forced_on_directives", "forced_off_directives", "params_vanishing"):
            self.assertEqual(got[key], expected[key], key)
        for direction in ("forced_on", "forced_off"):
            for bucket in ("effective_flips", "noops", "unresolved"):
                self.assertEqual(len(got[direction][bucket]), expected[direction][bucket],
                                 f"{direction}.{bucket} size")
                self.assertEqual(set(got[direction][bucket]), set(oracle[direction][bucket]),
                                 f"{direction}.{bucket} membership differs from the M149 oracle")


if __name__ == "__main__":
    unittest.main()
