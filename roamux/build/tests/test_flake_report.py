# SPDX-License-Identifier: Apache-2.0
"""roamux/build/ci/flake_report.py — the tier-2 flake signal (roam-283, grill H17).

Tier-2 runs every suite with --test-launcher-retry-limit=2 (roam-195), so a test that passes only
on retry is indistinguishable from a clean pass in the job's verdict. The report reads the
launcher's --test-launcher-summary-output JSON per suite, lists retried / final-non-success /
skipped tests into the step summary, and checks retried tests against the ledger
roamux/build/ci/known_flakes.txt (`mode: warn` — warn on unlisted; `mode: fail` — exit 1).

These tests are hermetic: synthetic summaries shaped like a real one produced by
out/Default/roamux_unittests (five root keys; per_iteration_data = list of {test: [attempt, …]};
attempt dicts carry status / elapsed_time_ms / output_snippet / result_parts; timestamp,
process_num and thread_id are optional). The named synthetic fixture
RoamuxSyntheticFlakyTest.FailsOnceThenPasses (FAILURE → SUCCESS) is the acceptance's
"deliberately flaky fixture".
"""

import io
import json
import pathlib
import shutil
import tempfile
import unittest
from contextlib import redirect_stdout

from roamux.build.ci import flake_report

SUITES = ("roamux_unittests", "roamux_browser_unittests", "roamux_sparkle_tests",
          "roamux_browsertests")
FLAKY = "RoamuxSyntheticFlakyTest.FailsOnceThenPasses"


def attempt(status="SUCCESS", ms=3, snippet="", parts=None, **extra):
    a = {"status": status, "elapsed_time_ms": ms, "output_snippet": snippet,
         "result_parts": parts if parts is not None else []}
    a.update(extra)
    return a


def summary(tests, tags=(), all_tests=None, disabled=()):
    """tests: {name: [attempt, …]} for one iteration (or a list of such dicts for several)."""
    iterations = tests if isinstance(tests, list) else [tests]
    names = sorted({n for it in iterations for n in it})
    return {"all_tests": sorted(all_tests) if all_tests is not None else names,
            "disabled_tests": list(disabled), "global_tags": list(tags),
            "per_iteration_data": iterations, "test_locations": {}}


CLEAN = summary({"RoamuxA.One": [attempt()], "RoamuxA.Two": [attempt(ms=12)]})
LEDGER_WARN = ("# known flakes\nmode: warn\n"
               "RoamuxTabStripToggleTest.* | roam-306 | CHECK(collection_node_)\n"
               "RoamuxNewTabPositionVerticalScrollTest.NewTabAfterActiveIsFullyVisible | roam-307 | timing\n")
LEDGER_FAIL = LEDGER_WARN.replace("mode: warn", "mode: fail")


class Case(unittest.TestCase):
    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp(prefix="roamux-flake-"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.art = self.tmp / "artifacts"
        self.art.mkdir()
        self.ledger = self.tmp / "known_flakes.txt"
        self.summary_file = self.tmp / "summary.md"
        self.ledger.write_text(LEDGER_WARN)

    def write(self, name, doc, log="launcher output\n"):
        if doc is not None:
            (self.art / f"{name}.json").write_text(json.dumps(doc) if not isinstance(doc, str) else doc)
        (self.art / f"{name}.log").write_text(log)

    def all_clean(self, **override):
        for s in SUITES:
            self.write(s, override.get(s, CLEAN))

    def run_report(self, ledger=None, artifacts=None, suites=None):
        args = ["--artifacts", str(artifacts or self.art), "--ledger", str(ledger or self.ledger),
                "--summary", str(self.summary_file)]
        if suites:
            args += ["--suites", ",".join(suites)]
        out = io.StringIO()
        with redirect_stdout(out):
            rc = flake_report.main(args)
        text = self.summary_file.read_text() if self.summary_file.exists() else ""
        return rc, out.getvalue(), text


class HappyPathTest(Case):
    def test_clean_suites_exit_zero_and_render_every_suite(self):
        self.all_clean()
        rc, out, md = self.run_report()
        self.assertEqual(0, rc, out)
        self.assertIn("## Tier-2 flake report", md)
        for s in SUITES:
            self.assertIn(s, md)
        self.assertIn("complete", md)
        self.assertNotIn("::warning::", out)

    def test_real_shape_sample_is_accepted(self):
        # A real run of roamux_unittests (13 cases, one attempt each) captured on 2026-09-06.
        real = summary({f"RoamuxFeaturesTest.Case{i}": [attempt(ms=0, timestamp="2026-09-06T05:38:00Z",
                                                                  process_num=1, thread_id=2,
                                                                  output_snippet_base64="", losless_snippet="")]
                        for i in range(13)})
        self.all_clean(roamux_unittests=real)
        rc, out, _ = self.run_report()
        self.assertEqual(0, rc, out)


class RetryClassificationTest(Case):
    def flaky_run(self, ledger_text=LEDGER_WARN, name=FLAKY):
        self.ledger.write_text(ledger_text)
        tests = {name: [attempt("FAILURE", 10004, "Value of: ok\n  Actual: false"),
                        attempt("SUCCESS", 4670)],
                 "RoamuxA.One": [attempt()]}
        # all_tests is the known universe: include the ledger's seeded names so no row is stale
        # and a failure can only come from the retry itself.
        universe = sorted(set(tests) | {"RoamuxTabStripToggleTest.PinFocusesStrip",
                                        "RoamuxNewTabPositionVerticalScrollTest.NewTabAfterActiveIsFullyVisible"})
        self.all_clean(roamux_browsertests=summary(tests, all_tests=universe))
        return self.run_report()

    def test_named_synthetic_flaky_fixture_appears_with_attempts_and_statuses(self):
        rc, out, md = self.flaky_run()
        for text in (out, md):
            self.assertIn(FLAKY, text)
            self.assertIn("2 attempts", text)
            self.assertIn("FAILURE → SUCCESS", text)
            self.assertIn("10004", text)      # per-attempt elapsed
            self.assertIn("4670", text)
            self.assertIn("Value of: ok", text)  # snippet of the failed attempt

    def test_unlisted_retry_warns_in_warn_mode(self):
        rc, out, _ = self.flaky_run()
        self.assertEqual(0, rc)
        self.assertIn(f"::warning::", out)
        self.assertIn(FLAKY, out)

    def test_unlisted_retry_fails_in_fail_mode(self):
        rc, out, _ = self.flaky_run(LEDGER_FAIL)
        self.assertEqual(1, rc)
        self.assertIn("::error::", out)
        self.assertIn(FLAKY, out)

    def test_listed_retry_is_shown_but_never_fails(self):
        for ledger in (LEDGER_WARN, LEDGER_FAIL):
            with self.subTest(ledger=ledger[:12]):
                rc, out, md = self.flaky_run(ledger, name="RoamuxTabStripToggleTest.PinFocusesStrip")
                self.assertEqual(0, rc, out)
                self.assertIn("RoamuxTabStripToggleTest.PinFocusesStrip", md)
                self.assertIn("roam-306", md)
                self.assertNotIn("::error::", out)
                self.assertNotIn("::warning::", out)

    def test_exact_row_matches_only_that_case(self):
        rc, out, _ = self.flaky_run(LEDGER_FAIL, name="RoamuxNewTabPositionVerticalScrollTest.Other")
        self.assertEqual(1, rc, "an exact row must not cover a sibling case")

    def test_retry_is_detected_per_iteration(self):
        # Two iterations: one attempt each — NOT a retry (the outer list is iterations, not attempts).
        doc = summary([{"RoamuxA.One": [attempt()]}, {"RoamuxA.One": [attempt()]}])
        self.all_clean(roamux_unittests=doc)
        rc, out, md = self.run_report()
        self.assertEqual(0, rc, out)
        self.assertNotIn("2 attempts", md)

    def test_later_iteration_failure_is_not_hidden_by_an_earlier_retry(self):
        # Step-8 F1: iteration 1 passes on retry, iteration 2 crashes — both must render, exit 1.
        doc = summary([{FLAKY: [attempt("FAILURE", 9, "first"), attempt("SUCCESS", 4)]},
                       {FLAKY: [attempt("CRASH", 7, "second time")]}])
        self.all_clean(roamux_browsertests=doc)
        rc, out, md = self.run_report()
        self.assertEqual(1, rc, out)
        self.assertIn("FAILURE → SUCCESS", md)
        self.assertIn("(iteration 1)", md)
        self.assertIn("(iteration 2)", md)
        self.assertIn("CRASH", md)
        self.assertIn("second time", md)
        self.assertIn(f"::error::roamux_browsertests: {FLAKY} final status CRASH", out)

    def test_table_rows_stay_contiguous_and_snippets_follow_escaped(self):
        # Step-8 F4: several flaky tests with multiline snippets; rows must not be interleaved.
        tests = {f"RoamuxM.T{i}": [attempt("FAILURE", 1, f"line one `tick`\nline two {i}"), attempt()] for i in range(3)}
        self.all_clean(roamux_unittests=summary(tests))
        rc, out, md = self.run_report()
        lines = md.splitlines()
        rows = [i for i, l in enumerate(lines) if l.startswith("| `RoamuxM")]
        self.assertEqual(3, len(rows))
        self.assertEqual(rows, list(range(rows[0], rows[0] + 3)), "table rows must be consecutive")
        snippet_lines = [l for l in lines if "line one" in l]
        self.assertEqual(3, len(snippet_lines))
        for l in snippet_lines:
            self.assertNotIn("`tick`", l)          # backticks escaped
            self.assertIn("⏎", l)                  # newlines folded
            self.assertGreater(lines.index(l), rows[-1])

    def test_final_non_success_is_listed_and_exits_one(self):
        doc = summary({"RoamuxA.Dead": [attempt("FAILURE", 5, "boom"), attempt("CRASH", 6, "boom again")]})
        self.all_clean(roamux_unittests=doc)
        rc, out, md = self.run_report()
        self.assertEqual(1, rc)
        self.assertIn("RoamuxA.Dead", md)
        self.assertIn("FAILURE → CRASH", md)
        self.assertIn("boom again", md)


class SkipClassificationTest(Case):
    def test_gtest_skip_is_recognised_via_result_parts(self):
        doc = summary({"RoamuxShortcutsTest.NonUsLayout": [attempt("SUCCESS", 1, parts=[{"type": "skip", "summary": "US layout only"}])]})
        self.all_clean(roamux_browsertests=doc)
        rc, out, md = self.run_report()
        self.assertEqual(0, rc, "skips are informational in roam-283 (L17 enforces later)")
        self.assertIn("skipped", md)
        self.assertIn("RoamuxShortcutsTest.NonUsLayout", md)

    def test_skipped_status_is_recognised_too(self):
        doc = summary({"RoamuxA.Skip": [attempt("SKIPPED")]})
        self.all_clean(roamux_unittests=doc)
        rc, _, md = self.run_report()
        self.assertEqual(0, rc)
        self.assertIn("RoamuxA.Skip", md)


class SummaryStateTest(Case):
    def test_absent_summary_is_an_error_naming_the_log(self):
        self.all_clean()
        (self.art / "roamux_sparkle_tests.json").unlink()
        rc, out, md = self.run_report()
        self.assertEqual(1, rc)
        self.assertIn("absent", md)
        self.assertIn("roamux_sparkle_tests.log", md)
        self.assertIn("::error::", out)
        self.assertIn("roamux_unittests", md, "other suites still render")

    def test_malformed_summaries_are_errors_and_others_still_render(self):
        malformed = {"parse": "{not json", "null_pid": summary({}) | {"per_iteration_data": None},
                     "empty_attempts": summary({"RoamuxA.One": []}),
                     "no_status": summary({"RoamuxA.One": [{"elapsed_time_ms": 1}]})}
        for label, doc in malformed.items():
            with self.subTest(label=label):
                self.all_clean(roamux_browser_unittests=doc)
                rc, out, md = self.run_report()
                self.assertEqual(1, rc, label)
                self.assertIn("malformed", md)
                self.assertIn("roamux_browser_unittests.log", md)
                self.assertIn("roamux_browsertests", md)

    def test_incomplete_summaries_are_errors_without_a_cause(self):
        for label, doc in (("early", summary({"RoamuxA.One": [attempt("NOTRUN", 0)]}, tags=["EARLY_SUMMARY"])),
                           ("notrun", summary({"RoamuxA.One": [attempt("NOTRUN", 0)]}))):
            with self.subTest(label=label):
                self.all_clean(roamux_unittests=doc)
                rc, out, md = self.run_report()
                self.assertEqual(1, rc)
                self.assertIn("incomplete", md)
                self.assertIn("roamux_unittests.log", md)
                self.assertNotIn("launcher was killed", md)   # possibilities are listed, no cause asserted

    def test_missing_artifacts_dir_is_an_error(self):
        rc, out, _ = self.run_report(artifacts=self.tmp / "nope")
        self.assertEqual(1, rc)
        self.assertIn("no artifacts", out)


class LedgerTest(Case):
    def _bad(self, text, needle):
        self.all_clean()
        self.ledger.write_text(text)
        rc, out, _ = self.run_report()
        self.assertEqual(1, rc, text)
        self.assertIn(needle, out)

    def test_missing_invalid_or_duplicate_mode_is_an_error(self):
        self._bad("RoamuxA.One | roam-1 | note\n", "mode")
        self._bad("mode: maybe\n", "mode")
        self._bad("mode: warn\nmode: fail\n", "mode")

    def test_bad_rows_are_errors(self):
        self._bad("mode: warn\nRoamuxA.One | nobody | note\n", "roam-N")
        self._bad("mode: warn\nRoamuxA.One | roam-1 | \n", "note")
        self._bad("mode: warn\nRoamuxA.One | roam-1\n", "fields")
        self._bad("mode: warn\nRoamuxA.One | roam-1 | note | extra\n", "fields")

    def test_stale_row_is_an_error_in_fail_mode_and_a_note_in_warn_mode(self):
        self.all_clean()
        base = "RoamuxGone.Case | roam-9 | gone\n"
        self.ledger.write_text("mode: fail\n" + base)
        rc, out, md = self.run_report()
        self.assertEqual(1, rc)
        self.assertIn("stale", md)
        self.ledger.write_text("mode: warn\n" + base)
        rc, out, md = self.run_report()
        self.assertEqual(0, rc)
        self.assertIn("stale", md)

    def test_listed_but_not_executed_test_is_not_stale_and_is_rendered(self):
        # Present in all_tests (the known universe) but absent from per_iteration_data (executed).
        doc = summary({"RoamuxA.One": [attempt()]}, all_tests=["RoamuxA.One", "RoamuxA.Listed"])
        for mode in ("warn", "fail"):
            with self.subTest(mode=mode):
                self.all_clean(roamux_unittests=doc)
                self.ledger.write_text(f"mode: {mode}\nRoamuxA.Listed | roam-1 | listed\n")
                rc, out, md = self.run_report()
                self.assertEqual(0, rc, out)
                self.assertNotIn("stale", md)
                self.assertIn("### Ledger", md)
                self.assertRegex(md, r"`RoamuxA\.Listed` \| roam-1 \| listed — listed, not executed this run")

    def test_empty_all_tests_still_evaluates_stale_rows(self):
        # Step-8 F3: an available-but-empty universe is an answer; stale rows must still be judged.
        doc = summary({}, all_tests=[])
        for s in SUITES:
            self.write(s, doc)
        self.ledger.write_text("mode: fail\nRoamuxGone.Case | roam-9 | gone\n")
        rc, out, md = self.run_report()
        self.assertEqual(1, rc, out)
        self.assertIn("stale", md)
        self.assertIn("::error::ledger line 2", out)

    def test_all_absent_summaries_do_not_judge_stale_rows(self):
        for s in SUITES:
            (self.art / f"{s}.log").write_text("log\n")
        self.ledger.write_text("mode: fail\nRoamuxGone.Case | roam-9 | gone\n")
        rc, out, md = self.run_report()
        self.assertEqual(1, rc)                          # via the four absent states
        self.assertIn("stale rows not checked", md)
        self.assertNotIn("::error::ledger line", out)


if __name__ == "__main__":
    unittest.main()
