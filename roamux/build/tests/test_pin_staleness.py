# SPDX-License-Identifier: Apache-2.0
"""Hermetic tests for roamux/build/ci/pin_staleness.py (roam-292, grill H1).

The tool is imported lazily inside a helper so that, before the module exists, every named test
fails on its own (the RED artifact lists the behaviours, not one import error).

Fixtures under data/pin_staleness/ are real VersionHistory / ChromiumDash responses captured on
2026-09-18 for the M149 pin 149.0.7827.201:
  * versionhistory-mac-stable-since-pin-fraction1.json — filter=version>=<pin>,fraction>=1, starttime asc (27 records);
  * versionhistory-mac-stable-serving.json           — filter=endtime=none (six records; 153.0.8010.53 at
    fraction 1 in group 194, four 153.x + 154.0.8037.44 sharing group 195 — the mixed-group case);
  * chromiumdash-milestone-schedule-149-154.json.
"""

import datetime
import importlib
import io
import json
import pathlib
import unittest

DATA = pathlib.Path(__file__).resolve().parent / "data" / "pin_staleness"
PIN = "149.0.7827.201"
TODAY = datetime.date(2026, 9, 18)


def tool():
    return importlib.import_module("roamux.build.ci.pin_staleness")


def fixture(name):
    return json.loads((DATA / name).read_text())


def history():
    return fixture("versionhistory-mac-stable-since-pin-fraction1.json")["releases"]


def serving():
    return fixture("versionhistory-mac-stable-serving.json")["releases"]


def rec(version, start, fraction=1, group="1", end=None):
    r = {"name": f"chrome/platforms/mac/channels/stable/versions/{version}/releases/{start}",
         "version": version, "fraction": fraction, "fractionGroup": group,
         "serving": {"startTime": f"{start}T10:00:00Z"}}
    if end:
        r["serving"]["endTime"] = f"{end}T10:00:00Z"
    return r


class PinParsingTest(unittest.TestCase):
    def test_parses_version_ignoring_comments_and_blanks(self):
        self.assertEqual(tool().parse_pin("# comment\n\n149.0.7827.201\n"), PIN)

    def test_malformed_pin_raises(self):
        with self.assertRaises(tool().PinError):
            tool().parse_pin("# only a comment\n")
        with self.assertRaises(tool().PinError):
            tool().parse_pin("UNPINNED\n")

    def test_version_ordering_and_milestone(self):
        t = tool()
        self.assertLess(t.version_tuple("149.0.7827.201"), t.version_tuple("150.0.7871.47"))
        self.assertLess(t.version_tuple("153.0.8010.9"), t.version_tuple("153.0.8010.53"))
        self.assertEqual(t.milestone("154.0.8037.44"), 154)


class QualificationTest(unittest.TestCase):
    def test_release_dates_take_earliest_qualifying_interval_per_version(self):
        dates = tool().release_dates([rec("150.0.1.1", "2026-07-05"), rec("150.0.1.1", "2026-07-02"),
                                      rec("150.0.1.1", "2026-07-09", fraction=0.5)])
        self.assertEqual(dates["150.0.1.1"].date, datetime.date(2026, 7, 2))

    def test_records_below_fraction_one_do_not_qualify(self):
        dates = tool().release_dates([rec("150.0.1.1", "2026-07-02", fraction=0.5)])
        self.assertNotIn("150.0.1.1", dates)

    def test_missing_fraction_counts_as_one(self):
        r = rec("150.0.1.1", "2026-07-02")
        del r["fraction"]
        self.assertIn("150.0.1.1", tool().release_dates([r]))

    def test_group_is_retained(self):
        dates = tool().release_dates([rec("150.0.1.1", "2026-07-02", group="194")])
        self.assertEqual(dates["150.0.1.1"].group, "194")

    def test_selection_on_real_fixtures(self):
        sel = tool().select(history(), serving(), PIN)
        self.assertEqual(sel.pin_date, datetime.date(2026, 6, 25))
        self.assertEqual(sel.first_newer.version, "150.0.7871.47")
        self.assertEqual(sel.first_newer.date, datetime.date(2026, 7, 2))
        self.assertEqual(sel.latest.version, "153.0.8010.53")
        self.assertEqual(sel.latest.group, "194")
        rolling = {r.version for r in sel.rolling}
        self.assertIn("154.0.8037.44", rolling)          # serving at 0.005 in group 195: rolling, not qualifying
        self.assertNotIn("153.0.8010.53", rolling)

    def test_pin_without_qualifying_record(self):
        sel = tool().select([rec("150.0.1.1", "2026-07-02")], [], PIN)
        self.assertIsNone(sel.pin_date)
        self.assertEqual(sel.first_newer.version, "150.0.1.1")

    def test_empty_history_means_current(self):
        sel = tool().select([], [], PIN)
        self.assertIsNone(sel.first_newer)
        self.assertIsNone(sel.latest)


class MetricsTest(unittest.TestCase):
    def _sel(self, pin_date, newer):
        recs = ([rec(PIN, pin_date.isoformat())] if pin_date else []) + [rec(v, d) for v, d in newer]
        return tool().select(recs, [], PIN)

    def test_real_fixture_numbers_on_2026_09_18(self):
        m = tool().metrics(tool().select(history(), serving(), PIN), TODAY)
        self.assertEqual((m.milestones_behind, m.pin_age_days, m.release_lag_days, m.stale_days), (4, 85, 84, 78))

    def test_current_pin_has_zero_stale_days(self):
        m = tool().metrics(self._sel(datetime.date(2026, 9, 1), []), TODAY)
        self.assertEqual(m.stale_days, 0)
        self.assertEqual(m.milestones_behind, 0)
        self.assertEqual(m.release_lag_days, 0)

    def test_stale_days_counts_from_the_earliest_newer_qualifying_release(self):
        sel = self._sel(datetime.date(2026, 6, 25), [("151.0.1.1", "2026-08-01"), ("150.0.1.1", "2026-07-10")])
        m = tool().metrics(sel, TODAY)
        self.assertEqual(m.stale_days, (TODAY - datetime.date(2026, 7, 10)).days)
        self.assertEqual(m.milestones_behind, 2)

    def test_same_day_newer_release_gives_zero_lag(self):
        sel = self._sel(datetime.date(2026, 9, 1), [("150.0.1.1", "2026-09-01")])
        self.assertEqual(tool().metrics(sel, TODAY).release_lag_days, 0)

    def test_pin_without_record_reports_unavailable_age_and_lag(self):
        m = tool().metrics(self._sel(None, [("150.0.1.1", "2026-07-10")]), TODAY)
        self.assertIsNone(m.pin_age_days)
        self.assertIsNone(m.release_lag_days)
        self.assertEqual(m.stale_days, (TODAY - datetime.date(2026, 7, 10)).days)

    def test_threshold_boundary(self):
        t = tool()
        sel14 = self._sel(datetime.date(2026, 8, 1), [("150.0.1.1", (TODAY - datetime.timedelta(days=14)).isoformat())])
        sel15 = self._sel(datetime.date(2026, 8, 1), [("150.0.1.1", (TODAY - datetime.timedelta(days=15)).isoformat())])
        self.assertFalse(t.measure(PIN, sel14, TODAY, 14).breach)
        self.assertTrue(t.measure(PIN, sel15, TODAY, 14).breach)
        self.assertEqual(t.measure(PIN, sel14, TODAY, 14).status, "behind")

    def test_current_status(self):
        self.assertEqual(tool().measure(PIN, self._sel(datetime.date(2026, 9, 1), []), TODAY, 14).status, "current")


class TrackerDecisionTest(unittest.TestCase):
    def _m(self, status, breach):
        return tool().Measurement(status=status, breach=breach, metrics=None, selection=None)

    def _tr(self, number, state, pin):
        return tool().Tracker(number=number, state=state, pin=pin)

    def test_breach_with_no_tracker_creates(self):
        self.assertEqual(tool().decide_actions(self._m("behind", True), [], PIN), [("create", None)])

    def test_breach_with_open_tracker_updates(self):
        self.assertEqual(tool().decide_actions(self._m("behind", True), [self._tr(7, "OPEN", PIN)], PIN), [("update", 7)])

    def test_breach_with_closed_tracker_is_left_alone(self):
        self.assertEqual(tool().decide_actions(self._m("behind", True), [self._tr(7, "CLOSED", PIN)], PIN), [("leave-closed", 7)])

    def test_superseded_open_trackers_are_closed_first_whatever_the_new_status(self):
        old = [self._tr(3, "OPEN", "148.0.1.1"), self._tr(4, "OPEN", "147.0.1.1")]
        for status, breach, tail in (("current", False, []), ("behind", False, []), ("behind", True, [("create", None)])):
            actions = tool().decide_actions(self._m(status, breach), old, PIN)
            self.assertEqual(actions[:2], [("close", 3), ("close", 4)], (status, breach))
            self.assertEqual(actions[2:], tail, (status, breach))

    def test_closed_superseded_trackers_are_not_touched(self):
        self.assertEqual(tool().decide_actions(self._m("current", False), [self._tr(3, "CLOSED", "148.0.1.1")], PIN), [])

    def test_within_sla_again_closes_the_current_tracker(self):
        self.assertEqual(tool().decide_actions(self._m("behind", False), [self._tr(7, "OPEN", PIN)], PIN), [("close", 7)])
        self.assertEqual(tool().decide_actions(self._m("current", False), [self._tr(7, "OPEN", PIN)], PIN), [("close", 7)])

    def test_tracker_key_and_title(self):
        t = tool()
        self.assertEqual(t.tracker_key(PIN), f"roamux-pin-staleness/{PIN}")
        self.assertIn(PIN, t.tracker_title(PIN))

    def test_discover_verifies_the_key_client_side(self):
        t = tool()
        listing = json.dumps([
            {"number": 292, "state": "OPEN", "body": "Nightly: Chromium pin staleness check (no key here)"},
            {"number": 400, "state": "OPEN", "body": f"...\nTracker key: {t.tracker_key(PIN)}\n"},
            {"number": 401, "state": "CLOSED", "body": "Tracker key: roamux-pin-staleness/148.0.1.1"},
        ])
        trackers = t.discover_trackers(lambda argv: listing, "xinquan568/roamux")
        self.assertEqual([(x.number, x.state, x.pin) for x in trackers],
                         [(400, "OPEN", PIN), (401, "CLOSED", "148.0.1.1")])

    def test_discover_rejects_inline_quotes_placeholders_punctuation_and_conflicts(self):
        t = tool()
        listing = json.dumps([
            {"number": 1, "state": "OPEN", "body": f"see the line `Tracker key: {t.tracker_key(PIN)}` in trackers"},   # inline quotation
            {"number": 2, "state": "OPEN", "body": "Tracker key: roamux-pin-staleness/<pin>"},                            # placeholder
            {"number": 3, "state": "OPEN", "body": f"Tracker key: {t.tracker_key(PIN)}."},                              # trailing punctuation
            {"number": 4, "state": "OPEN", "body": f"Tracker key: {t.tracker_key(PIN)}\nTracker key: roamux-pin-staleness/148.0.1.1"},  # conflict
            {"number": 5, "state": "OPEN", "body": f"  Tracker key: {t.tracker_key(PIN)}  \n"},                        # standalone (whitespace trimmed)
        ])
        trackers = t.discover_trackers(lambda argv: listing, "xinquan568/roamux")
        self.assertEqual([(x.number, x.pin) for x in trackers], [(5, PIN)])

    def test_discover_fails_closed_on_empty_output_non_array_bad_items_and_truncation(self):
        t = tool()
        bad_items = ('[{"number": "7", "state": "OPEN", "body": ""}]',     # string number
                     '[{"number": true, "state": "OPEN", "body": ""}]',    # boolean number
                     '[{"number": 0, "state": "OPEN", "body": ""}]',       # non-positive number
                     '[{"number": 7, "state": "MERGED", "body": ""}]',     # unknown state
                     '[{"number": 7, "state": "OPEN", "body": false}]',    # non-string bodies ...
                     '[{"number": 7, "state": "OPEN", "body": 0}]',
                     '[{"number": 7, "state": "OPEN", "body": []}]',
                     '[{"number": 7, "state": "OPEN", "body": {}}]',
                     '[{"number": 7, "state": "OPEN"}]',                   # missing body
                     '["not an object"]')
        for out in ("", "   ", "{}", "null", *bad_items):
            with self.assertRaises(t.LookupError_, msg=repr(out)):
                t.discover_trackers(lambda argv, o=out: o, "xinquan568/roamux")
        two = json.dumps([{"number": 1, "state": "OPEN", "body": "x"}, {"number": 2, "state": "OPEN", "body": "y"}])
        with self.assertRaises(t.LookupError_):
            t.discover_trackers(lambda argv: two, "xinquan568/roamux", limit=2)
        self.assertEqual(t.discover_trackers(lambda argv: "[]", "xinquan568/roamux"), [])


class RenderingTest(unittest.TestCase):
    def test_summary_markdown_and_stdout_annotations(self):
        t = tool()
        m = t.measure(PIN, t.select(history(), serving(), PIN), TODAY, 14)
        md = t.render_summary(m, publication=None, milestone_dates={150: "2026-06-30"})
        self.assertIn("78", md)
        self.assertIn("153.0.8010.53", md)
        self.assertIn("194", md)                     # the group is printed
        self.assertNotIn("::warning::", md)          # annotations never go into the Markdown sink
        self.assertIn("::warning::", t.annotations(m, publication=None))

    def test_issue_body_carries_the_key_and_links(self):
        t = tool()
        m = t.measure(PIN, t.select(history(), serving(), PIN), TODAY, 14)
        body = t.render_issue_body(m, PIN, milestone_dates={}, remediation_issue=293)
        self.assertIn(t.tracker_key(PIN), body)
        self.assertIn("#293", body)
        self.assertIn("(https://github.com/xinquan568/roamux/blob/main/docs/security-uprev.md)", body)
        self.assertIn("150.0.7871.47` (2026-07-02, fraction group 194)", body)
        self.assertIn("154.0.8037.44` (0.005, group 195)", body)


class FetchTest(unittest.TestCase):
    def test_pagination_follows_next_page_token(self):
        pages = {0: {"releases": [rec("150.0.1.1", "2026-07-02")], "nextPageToken": "T"},
                 1: {"releases": [rec("151.0.1.1", "2026-08-01")]}}
        calls = []

        def fetch(url):
            calls.append(url)
            return pages[1] if "pageToken=T" in url else pages[0]

        hist, serv = tool().fetch_versionhistory(fetch, "mac", PIN)
        self.assertEqual([r["version"] for r in hist], ["150.0.1.1", "151.0.1.1"])
        self.assertTrue(any("pageToken=T" in c for c in calls))

    def test_fetch_failure_is_undeterminable_after_one_retry(self):
        calls = []

        def fetch(url):
            calls.append(url)
            raise OSError("boom")

        with self.assertRaises(tool().Undeterminable):
            tool().fetch_versionhistory(fetch, "mac", PIN, retry_delay=0)
        self.assertEqual(len(calls), 2)

    def test_unexpected_shape_is_undeterminable(self):
        with self.assertRaises(tool().Undeterminable):
            tool().fetch_versionhistory(lambda url: {"unexpected": True}, "mac", PIN, retry_delay=0)


class MainTest(unittest.TestCase):
    """End-to-end through main() with injected seams; a temp pin file and summary sink."""

    def setUp(self):
        import tempfile
        self.tmp = pathlib.Path(tempfile.mkdtemp())
        self.addCleanup(lambda: __import__("shutil").rmtree(self.tmp, ignore_errors=True))
        (self.tmp / "CHROMIUM_PIN").write_text(f"# pin\n{PIN}\n")
        self.summary = self.tmp / "summary.md"

    def _fetch_ok(self, url):
        if "endtime=none" in url:
            return fixture("versionhistory-mac-stable-serving.json")
        if "chromiumdash" in url:
            return fixture("chromiumdash-milestone-schedule-149-154.json")["153"]
        return fixture("versionhistory-mac-stable-since-pin-fraction1.json")

    def _run(self, *extra, fetch=None, gh=None):
        out = io.StringIO()
        rc = tool().main(["--pin-file", str(self.tmp / "CHROMIUM_PIN"), "--today", TODAY.isoformat(),
                          "--summary", str(self.summary), "--repo", "xinquan568/roamux", *extra],
                         fetch=fetch or self._fetch_ok, gh=gh, stdout=out)
        return rc, out.getvalue()

    def test_dry_run_never_calls_gh_and_exits_zero_on_breach(self):
        gh_calls = []
        rc, out = self._run("--dry-run", gh=lambda argv: gh_calls.append(argv) or "[]")
        self.assertEqual(rc, 0)
        self.assertEqual(gh_calls, [])
        self.assertIn("::warning::", out)
        self.assertIn("78", self.summary.read_text())

    def test_undeterminable_exits_one_writes_summary_and_never_publishes(self):
        gh_calls = []

        def fetch(url):
            raise OSError("down")

        rc, out = self._run("--publish", "--retry-delay", "0", fetch=fetch, gh=lambda argv: gh_calls.append(argv) or "[]")
        self.assertEqual(rc, 1)
        self.assertEqual(gh_calls, [])
        self.assertIn("::error::", out)
        self.assertIn("undeterminable", self.summary.read_text())

    def test_publish_creates_a_tracker_on_breach(self):
        gh_calls = []

        def gh(argv):
            gh_calls.append(argv)
            if argv[:2] == ["issue", "list"]:
                return "[]"
            if argv[:2] == ["issue", "create"]:
                return "https://github.com/xinquan568/roamux/issues/500\n"
            return ""

        rc, _ = self._run("--publish", gh=gh)
        self.assertEqual(rc, 0)
        kinds = [c[:2] for c in gh_calls]
        self.assertIn(["issue", "create"], kinds)
        create = next(c for c in gh_calls if c[:2] == ["issue", "create"])
        self.assertIn("--label", create)
        self.assertIn("--milestone", create)
        self.assertIn("500", self.summary.read_text())

    def test_lookup_failure_publishes_nothing_and_exits_one(self):
        gh_calls = []

        def gh(argv):
            gh_calls.append(argv)
            if argv[:2] == ["issue", "list"]:
                raise RuntimeError("gh: HTTP 500")
            return ""

        rc, out = self._run("--publish", gh=gh)
        self.assertEqual(rc, 1)
        self.assertEqual([c[:2] for c in gh_calls], [["issue", "list"]])
        self.assertIn("lookup-failed", self.summary.read_text())
        self.assertIn("::error::", out)

    def test_mutation_failure_keeps_the_summary_and_exits_one(self):
        def gh(argv):
            if argv[:2] == ["issue", "list"]:
                return "[]"
            raise RuntimeError("gh: create failed")

        rc, out = self._run("--publish", gh=gh)
        self.assertEqual(rc, 1)
        text = self.summary.read_text()
        self.assertIn("mutation-failed", text)
        self.assertIn("78", text)
        self.assertIn("::error::", out)

    def test_malformed_records_are_undeterminable_through_main(self):
        for bad in ([None], [{"version": "150.0.1.1"}], [{"version": "not-a-version", "serving": {"startTime": "2026-07-02T00:00:00Z"}}],
                    [{"version": "150.0.1.1", "fraction": "lots", "serving": {"startTime": "2026-07-02T00:00:00Z"}}],
                    [{"version": "150.0.1.1", "serving": {"startTime": "yesterday"}}]):
            gh_calls = []

            def fetch(url, bad=bad):
                return {"releases": bad}

            self.summary.write_text("")
            rc, out = self._run("--publish", "--retry-delay", "0", fetch=fetch, gh=lambda argv: gh_calls.append(argv) or "[]")
            self.assertEqual(rc, 1, bad)
            self.assertEqual(gh_calls, [], bad)
            self.assertIn("::error::", out, bad)
            self.assertIn("undeterminable", self.summary.read_text(), bad)

    def test_create_rechecks_and_updates_when_an_overlapping_run_created_the_tracker(self):
        t = tool()
        calls = []
        listings = iter(["[]", json.dumps([{"number": 77, "state": "OPEN", "body": f"Tracker key: {t.tracker_key(PIN)}"}])])

        def gh(argv):
            calls.append(argv)
            if argv[:2] == ["issue", "list"]:
                return next(listings)
            return ""

        rc, _ = self._run("--publish", gh=gh)
        self.assertEqual(rc, 0)
        kinds = [c[:2] for c in calls]
        self.assertNotIn(["issue", "create"], kinds)
        self.assertIn(["issue", "edit"], kinds)
        self.assertIn("update #77", self.summary.read_text())

    def test_create_is_suppressed_when_the_recheck_fails(self):
        calls = []
        listings = iter(["[]"])

        def gh(argv):
            calls.append(argv)
            if argv[:2] == ["issue", "list"]:
                try:
                    return next(listings)
                except StopIteration:
                    raise RuntimeError("gh: HTTP 502")
            return ""

        rc, out = self._run("--publish", gh=gh)
        self.assertEqual(rc, 1)
        self.assertNotIn(["issue", "create"], [c[:2] for c in calls])
        self.assertIn("lookup-failed", self.summary.read_text())
        self.assertIn("::error::", out)

    def test_duplicate_episodes_are_marked_in_the_summary(self):
        t = tool()
        listing = json.dumps([{"number": 70, "state": "OPEN", "body": f"Tracker key: {t.tracker_key(PIN)}"},
                              {"number": 71, "state": "OPEN", "body": f"Tracker key: {t.tracker_key(PIN)}"}])
        rc, _ = self._run("--publish", gh=lambda argv: listing if argv[:2] == ["issue", "list"] else "")
        self.assertEqual(rc, 0)
        self.assertIn("DUPLICATE episode", self.summary.read_text())

    def test_no_summary_sink_is_fine(self):
        out = io.StringIO()
        rc = tool().main(["--pin-file", str(self.tmp / "CHROMIUM_PIN"), "--today", TODAY.isoformat(),
                          "--repo", "xinquan568/roamux", "--dry-run"], fetch=self._fetch_ok, gh=None, stdout=out)
        self.assertEqual(rc, 0)


SLA_SENTENCE_RE = __import__("re").compile(r"The pin is at most (\d+) days behind stable")


def documented_threshold(doc_text):
    m = SLA_SENTENCE_RE.search(doc_text)
    return int(m.group(1)) if m else None


class DocConsistencyTest(unittest.TestCase):
    def test_doc_states_the_tools_default_threshold_in_the_sla_sentence(self):
        doc = (pathlib.Path(__file__).resolve().parents[3] / "docs" / "security-uprev.md").read_text()
        self.assertEqual(documented_threshold(doc), tool().DEFAULT_THRESHOLD_DAYS)

    def test_control_the_cve_clause_does_not_satisfy_the_check(self):
        # A doc whose only "N days" is the CVE clause must NOT match the pin SLA sentence.
        self.assertIsNone(documented_threshold("A fixed release within 3 days of an in-the-wild CVE."))
        self.assertEqual(documented_threshold("**The pin is at most 21 days behind stable.**"), 21)


if __name__ == "__main__":
    unittest.main()
