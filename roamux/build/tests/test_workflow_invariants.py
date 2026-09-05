# SPDX-License-Identifier: Apache-2.0
"""Hermetic CI-workflow invariants (roam-5, plan §12.6) — no Chromium checkout, no external deps.

These encode the tier-1 + release posture structurally, so every future workflow edit is gated:
  1. No workflow uses `pull_request_target` (the fork-secrets foot-gun).
  2. ci.yml has the stable `lint` job and it references no secrets (fork/tier-1 boundary is
     structural: fork isolation is GitHub's platform guarantee, preserved by structure).
  3. Chromium-dependent jobs are explicitly marked (ROAMUX_CHROMIUM_DEPENDENT) and gated: in ci.yml
     on BOTH the capability variable AND the non-fork condition (R15 — fork PRs stay tier-1 even
     after a capable runner exists); in nightly.yml on the capability variable.
  4. release.yml binds `environment: release` and triggers only on v* tags / manual dispatch.
  5. nightly.yml is scheduled.
  6. release.yml resolves machine-specific paths from the runner machine-env file contract
     (~/roamux-runner/.env: ROAMUX_CHROMIUM_SRC / ROAMUX_DEPOT_TOOLS) — the file is required and
     sourced unconditionally, every value is validated fail-loud, and no workspace-relative
     Chromium path may appear (roam-108).
  7. release.yml declares an explicit, generous job timeout — without one, GitHub's default 6h cap
     applies (self-hosted runners included) and a cold universal2 build is service-cancelled
     mid-compile (roam-110; run 29195822447 died at exactly 6h00m).
  8. release.yml creates the universal out dir (out/Release) before invoking Chromium's
     universalizer — the upstream script os.mkdir()s its output bundle and requires the parent to
     exist; a fresh builder has neither (roam-112; run 29214000668 died post-compile).
  9. release.yml passes GN args by import(), never newline-flattened — a tr-flattened commented
     args file is ONE line whose first '#' comments out every arg, so slices silently build pure
     GN defaults (wrong arch, debug, component, no Sparkle); and it canary-checks the args took
     effect after gn gen plus asserts each slice's arch after compile (roam-114; run 29218872385).
  10. release.yml gates the assembled bundle's dyld resolution — every @rpath dependency of the
      framework must resolve via its own LC_RPATHs to a file inside the bundle (roam-121; the
      v0.0.1-alpha.1 install aborted at first launch on an unresolvable Sparkle rpath).
  11. release.yml stamps the tag-derived version into the renamed bundle (rename_bundle
      --bundle-version from GITHUB_REF_NAME) so the installed CFBundleVersion and the appcast
      sparkle:version share one comparable scheme (roam-120; Chromium's 7827.x otherwise outranks
      every tag version and Sparkle never offers an update).  12. release.yml's publish leg is re-cut-safe — any prior release for the tag is deleted before
      drafting, the fresh draft is tracked by id, and publish PATCHes that id (never edit-by-tag,
      which republished the stale release while the fixed draft stayed invisible; roam-124,
      run 29261520357).  13. ci.yml's Conventional-Commits check enumerates PR commits with --no-merges — GitHub's
      "Update branch" button injects a merge commit whose subject is not Conventional, and the
      check's contract is each AUTHORED commit, never platform merges (roam-134).  14. release.yml derives the stamped CFBundleVersion and the appcast sparkle:version from the tag
      via release_version.py (numeric, Sparkle-orderable) — never the raw tag string, which
      Sparkle's comparator cannot order across pre-releases (roam-141).
  15. release.yml runs the rebrand channel (rebrand_strings.py, roam-132) AFTER apply_patches.py
      and BEFORE the grit/resource compile (autoninja), followed by a post-substitution
      validation gate — a representative product string is rebranded, legal attribution stays
      "Chromium", and the channel is idempotent (--check clean) — or the release fails.
  16. release.yml reconciles the shared warm base to pristine (reset --hard HEAD +
      clean -fd -e /roamux) BEFORE its apply_patches.py runhook (roam-175) — the stack
      simulator matches only prefixes of the current stack, so a base left at a superseded
      stack (any patch-rewriting/deleting change since the last release) or carrying a prior
      release's rebrand mutations would otherwise fail — or silently contaminate — the build.
  17. nightly.yml's hosted suite opts into the disk-image mount (REQUIRE_DMG_MOUNT=1, roam-261).
      The mount is off the push path and off the required `lint` gate, so its only homes are
      tier-2 and this scheduled hosted run — and tier-2 is conditional on the capability var
      and unreachable for fork PRs (R15). Without this opt-in the one test covering the shipped
      Roamux.dmg's symlink/exec-bit preservation would depend on a single conditional job.
  18. The three self-hosted jobs (ci targeted-suite-selfhosted, nightly-selfhosted, release
      build-sign-package) each declare the job-level concurrency group `roamux-shared-base`
      (cancel-in-progress: false, queue: max): they reset, re-patch and re-point the SAME shared
      warm base, and until roam-279 their serialization was an accident of having one runner. No
      workflow-level group and no hosted job may be serialized (grill H5).
  19. release.yml validates ROAMUX_CANONICAL_OVERLAY in the machine-env step and ends with an
      always() step that restores the base's overlay symlink to it — refusing to link into a real
      directory and verifying with readlink; the restore step's three exit paths AND the
      machine-env step's guards/export are executed by tests, from scripts extracted out of the
      workflow (grill H6; tier2_job.sh already restores via its EXIT trap, release never did).
  20. Every self-hosted job declares timeout-minutes above GitHub's silent 6h default — a cold
      tier-2 exceeds it (nightly run 29827734729 was service-cancelled at exactly 6h00m on
      2026-07-21, roam-110's tier-2 twin); invariant 7 generalized (grill M9).
  21. The REQUIRED targeted-suite-selfhosted check is never skipped by the capability switch: an
      empty ROAMUX_CI_CHROMIUM_RUNNER selects a hosted runner (runs-on expression) and the job's
      first step fails red there. Branch protection treats a skipped required check as satisfied
      ("successful, skipped, or neutral"), so the old if:-gate was a vacuous pass. Forks stay
      skipped by the trust predicate (R15); nightly keeps its arm (not a required check)
      (roam-281 / grill H7).
"""

import os
import pathlib
import re
import shutil
import subprocess
import tempfile
import unittest

WORKFLOWS = pathlib.Path(__file__).resolve().parents[3] / ".github" / "workflows"

CAPABILITY_VAR = "ROAMUX_CI_CHROMIUM_RUNNER"
# roam-281: the required tier-2 job selects its runner by expression — the fixed label triple when
# the capability switch is non-empty, a hosted runner when it is empty (where the first step fails
# red). Pinned exactly; `self-hosted` must stay on the runs-on line for the enumeration helpers.
SELFHOSTED_RUNS_ON_EXPR = ("${{ vars.ROAMUX_CI_CHROMIUM_RUNNER != '' && "
                           "fromJSON('[\"self-hosted\", \"macos\", \"chromium-builder\"]') || 'macos-14' }}")
MARKER = "ROAMUX_CHROMIUM_DEPENDENT"
FORK_CONDITION = "head.repo.fork"


def _read(name):
    path = WORKFLOWS / name
    if not path.exists():
        return None
    return path.read_text()


def _job_block(text, job_id):
    """The lines of one named job, independent of where it sits in the mapping.

    A new top-level job key (two-space indent, ends with ':') closes the block —
    the same rule _marked_job_blocks uses."""
    lines, block, capturing = text.splitlines(), [], False
    for line in lines:
        if line.rstrip() == f"  {job_id}:":
            capturing = True
            continue
        if capturing:
            if (line.startswith("  ") and not line.startswith("   ")
                    and line.rstrip().endswith(":")):
                break
            block.append(line)
    return "\n".join(block) if block else None


def _marked_job_blocks(text):
    """Split a workflow into chunks per marked (Chromium-dependent) job, marker line included."""
    blocks, current, capturing = [], [], False
    for line in text.splitlines():
        if MARKER in line:
            if capturing and current:
                blocks.append("\n".join(current))
            current, capturing = [line], True
            continue
        if capturing:
            # A new top-level job key (two-space indent, ends with ':') closes the block.
            if line.startswith("  ") and not line.startswith("   ") and line.rstrip().endswith(":"):
                blocks.append("\n".join(current))
                current, capturing = [], False
            else:
                current.append(line)
    if capturing and current:
        blocks.append("\n".join(current))
    return blocks


class WorkflowInvariantsTest(unittest.TestCase):
    def test_no_pull_request_target_anywhere(self):
        self.assertTrue(WORKFLOWS.is_dir(), f"missing {WORKFLOWS}")
        for wf in sorted(WORKFLOWS.glob("*.yml")):
            self.assertNotIn("pull_request_target", wf.read_text(),
                             f"{wf.name} uses the pull_request_target foot-gun")

    def test_ci_lint_job_exists_and_references_no_secrets(self):
        text = _read("ci.yml")
        self.assertIsNotNone(text, "ci.yml missing")
        self.assertIn("\n  lint:", text, "the stable `lint` job is load-bearing (required check)")
        # The tier-1 boundary is structural: nothing in ci.yml may reference secrets at all.
        self.assertNotIn("secrets.", text,
                         "ci.yml runs for fork PRs — it must reference no secrets")

    @staticmethod
    def _runs_on_selfhosted(block):
        return any("runs-on:" in l and "self-hosted" in l for l in block.splitlines())

    def _selfhosted_blocks(self, text):
        return [b for b in _marked_job_blocks(text) if self._runs_on_selfhosted(b)]

    def _hosted_blocks(self, text):
        return [b for b in _marked_job_blocks(text) if not self._runs_on_selfhosted(b)]

    def test_ci_hosted_announce_job_is_fork_aware(self):
        text = _read("ci.yml")
        self.assertIsNotNone(text, "ci.yml missing")
        blocks = self._hosted_blocks(text)
        self.assertTrue(blocks, "ci.yml must keep the hosted announce job (visible tier-2 status)")
        for block in blocks:
            self.assertIn(CAPABILITY_VAR, block, "announce job must reflect the capability state")
            self.assertIn(FORK_CONDITION, block, "announce job must be fork-aware (R15)")
            code = "\n".join(l for l in block.splitlines() if not l.strip().startswith("#"))
            fork_branch = code.find('if [ "$IS_FORK" = "true" ]')
            self.assertNotEqual(fork_branch, -1, "announce run body must branch on IS_FORK first")
            self.assertNotIn("exit 1", code,
                             "the announce job never fails — tier-2 work happens on self-hosted")

    def test_ci_selfhosted_job_has_exact_trust_predicate(self):
        # roam-36 (S5-1): the tier-2 predicate enumerates its arms exactly — protected-main pushes
        # and same-repo (non-fork) PRs. A broad non-PR catch-all must never appear.
        text = _read("ci.yml")
        self.assertIsNotNone(text, "ci.yml missing")
        blocks = self._selfhosted_blocks(text)
        self.assertTrue(blocks, "ci.yml must contain the self-hosted targeted-suite job (roam-36)")
        for block in blocks:
            # roam-281 (grill H7): the label triple is pinned INSIDE the runner-selection expression
            # — the fixed triple when the capability switch is non-empty, a hosted runner when it is
            # empty, so the required check can go red instead of being skipped. `self-hosted` stays
            # on the runs-on line, which is what every enumeration helper keys on.
            runs_on = next((l for l in block.splitlines() if l.strip().startswith("runs-on:")), "")
            self.assertIn(SELFHOSTED_RUNS_ON_EXPR, runs_on,
                          "self-hosted job must select the exact label triple by expression, with "
                          "a hosted fallback when the switch is empty (roam-281)")
            self.assertIn(CAPABILITY_VAR, block, "missing the capability variable")
            if_line = next((l for l in block.splitlines() if l.strip().startswith("if:")), "")
            self.assertNotIn(CAPABILITY_VAR, if_line,
                             "the capability switch must NOT skip the required job — a skipped "
                             "required check is a vacuous pass (roam-281 / H7)")
            self.assertIn("github.event_name == 'push' && github.ref == 'refs/heads/main'", block,
                          "missing the protected-main push arm")
            self.assertIn("github.event.pull_request.head.repo.fork == false", block,
                          "missing the same-repo (non-fork) PR arm (R15)")
            self.assertNotIn("event_name != 'pull_request'", block,
                             "broad non-PR catch-all is forbidden (S5-1)")

    def test_nightly_selfhosted_job_has_exact_trust_predicate(self):
        text = _read("nightly.yml")
        self.assertIsNotNone(text, "nightly.yml missing")
        blocks = self._selfhosted_blocks(text)
        self.assertTrue(blocks, "nightly.yml must contain the self-hosted nightly job (roam-36)")
        for block in blocks:
            self.assertIn("[self-hosted, macos, chromium-builder]", block,
                          "self-hosted job must pin the exact label triple")
            self.assertIn(CAPABILITY_VAR, block, "missing the capability-variable arm")
            self.assertIn("github.event_name == 'schedule'", block, "missing the schedule arm")
            self.assertIn("github.event_name == 'workflow_dispatch' && github.ref == 'refs/heads/main'",
                          block, "manual dispatch must be main-only")
            self.assertNotIn("event_name != 'pull_request'", block,
                             "broad non-PR catch-all is forbidden (S5-1)")

    def test_half_provisioned_placeholders_are_retired(self):
        for name in ("ci.yml", "nightly.yml"):
            text = _read(name)
            self.assertIsNotNone(text, f"{name} missing")
            self.assertNotIn("not wired yet", text,
                             f"{name}: roam-5's loud placeholder must be retired by roam-36")

    def test_nightly_scheduled_and_gated(self):
        text = _read("nightly.yml")
        self.assertIsNotNone(text, "nightly.yml missing")
        self.assertIn("schedule:", text, "nightly must be scheduled")
        blocks = _marked_job_blocks(text)
        self.assertTrue(blocks, "nightly.yml must mark its Chromium-dependent work")
        for block in blocks:
            self.assertIn(CAPABILITY_VAR, block,
                          "nightly Chromium work lacks the capability gate")

    def test_nightly_hosted_suite_opts_into_the_dmg_mount(self):
        # Invariant 17 (roam-261). The hosted nightly run is the mount's only
        # home that is NOT conditional on vars.ROAMUX_CI_CHROMIUM_RUNNER, so
        # dropping this opt-in would quietly reduce coverage of the shipped
        # Roamux.dmg to a single conditional job.
        text = _read("nightly.yml")
        self.assertIsNotNone(text, "nightly.yml missing")
        block = _job_block(text, "hermetic-suite")
        self.assertIsNotNone(block, "nightly.yml has no hermetic-suite job")
        # Assert ONE logical run command carries both — asserting the two
        # strings independently would pass with the opt-in parked on an
        # unrelated step, where the unittest process never sees it. Job order
        # is irrelevant because the block is keyed by name, not position.
        runs = [l for l in block.replace("\\\n", " ").splitlines()
                if "unittest discover" in l]
        self.assertEqual(1, len(runs), f"expected 1 discovery run, got {runs}")
        self.assertIn("REQUIRE_DMG_MOUNT=1", runs[0],
                      "nightly's hosted suite must opt into the disk-image "
                      "mount on the discovery command itself (roam-261)")

    def test_release_binds_environment_and_tag_triggers_only(self):
        text = _read("release.yml")
        self.assertIsNotNone(text, "release.yml missing")
        self.assertIn("environment: release", text,
                      "release job must bind the protected Environment")
        # Structural on:-block parsing (S8-2): triggers are ONLY push.tags v* + workflow_dispatch.
        lines = text.splitlines()
        on_start = next(i for i, l in enumerate(lines) if l.rstrip() == "on:")
        on_block = []
        for line in lines[on_start + 1:]:
            if line.strip() and not line.startswith(" "):
                break  # next top-level key ends the on: block
            on_block.append(line)
        on_text = "\n".join(on_block)
        triggers = [l.strip().rstrip(":") for l in on_block
                    if l.startswith("  ") and not l.startswith("   ") and l.strip().endswith(":")]
        self.assertEqual(sorted(triggers), ["push", "workflow_dispatch"],
                         f"release triggers must be exactly push+workflow_dispatch, got {triggers}")
        self.assertIn("tags:", on_text, "release push trigger must be tag-scoped")
        self.assertNotIn("branches", on_text, "release must not trigger on branch pushes")
        tag_patterns = [l.strip().lstrip("- ").strip('"') for l in on_block if l.strip().startswith("- ")]
        self.assertEqual(tag_patterns, ["v*"], f"release tags must be exactly v*, got {tag_patterns}")

    def test_release_reconciles_base_before_runhook(self):
        # roam-175 (invariant 16): the runhook's stack simulator matches only prefixes
        # of the CURRENT stack, so release must reconcile the shared base first —
        # reset --hard HEAD (not `checkout -- .`: that restores from a possibly-staged
        # index) + clean -fd -e /roamux (drops files a superseded stack added; spares
        # the overlay symlink; no -x, so out dirs and ignored caches survive; single
        # -f never descends into nested git repos/submodules). A base left at a
        # superseded stack — or carrying a prior release's rebrand mutations — would
        # otherwise fail, or silently contaminate, a tag-time build.
        text = _read("release.yml")
        self.assertIsNotNone(text, "release.yml missing")
        reset = text.find("reset --hard HEAD")
        clean = text.find("clean -fd -e /roamux")
        runhook = text.find("apply_patches.py")
        self.assertNotEqual(reset, -1, "release.yml: no reset --hard reconcile")
        self.assertNotEqual(clean, -1, "release.yml: no clean reconcile")
        self.assertLess(reset, runhook, "reconcile must precede the runhook")
        self.assertLess(clean, runhook, "reconcile must precede the runhook")
        self.assertNotIn("clean -fdx", text, "clean -x would nuke the warm out dirs")
        self.assertNotIn("clean -ffd", text, "clean -ff would enter submodules")

    def test_release_resolves_chromium_src_from_machine_env(self):
        # roam-108: machine paths come from the runner machine-env file contract
        # (~/roamux-runner/.env), required + sourced unconditionally, fail-loud — never a
        # workspace-relative Chromium path (actions/checkout git-cleans the workspace, so a
        # checkout inside it cannot durably exist on the v1 personal-machine builder).
        text = _read("release.yml")
        self.assertIsNotNone(text, "release.yml missing")
        self.assertNotIn("${{ github.workspace }}/chromium", text,
                         "CHROMIUM_SRC must not be workspace-relative (roam-108)")
        self.assertIn('env_file="${HOME}/roamux-runner/.env"', text,
                      "the machine-env file contract must be pinned to the runbook path")
        lines = text.splitlines()
        self.assertTrue(any("-f" in l and "env_file" in l for l in lines),
                        "the .env existence check must be explicit")
        self.assertTrue(any("::error::" in l and "env_file" in l for l in lines),
                        "the missing-file case must fail loudly")
        self.assertIn('. "${env_file}"', text,
                      "the machine env must be sourced explicitly")
        self.assertFalse(any("ROAMUX_CHROMIUM_SRC:-" in l and "env_file" in l for l in lines),
                         "sourcing must be unconditional, not guarded on an injected variable")
        self.assertTrue(any("::error::" in l and "ROAMUX_CHROMIUM_SRC" in l for l in lines),
                        "missing/invalid ROAMUX_CHROMIUM_SRC must fail loudly")
        self.assertTrue(any("CHROMIUM_SRC=" in l and "GITHUB_ENV" in l for l in lines),
                        "the resolved CHROMIUM_SRC must be exported via GITHUB_ENV")

    def test_release_puts_depot_tools_on_path(self):
        # roam-108: gn/autoninja need depot_tools; the workflow must put ROAMUX_DEPOT_TOOLS on
        # PATH mechanically (tier2_job.sh:14 is the sibling consumer) and fail loudly without it.
        text = _read("release.yml")
        self.assertIsNotNone(text, "release.yml missing")
        lines = text.splitlines()
        self.assertTrue(any("::error::" in l and "ROAMUX_DEPOT_TOOLS" in l for l in lines),
                        "missing/invalid ROAMUX_DEPOT_TOOLS must fail loudly")
        self.assertTrue(any("ROAMUX_DEPOT_TOOLS" in l and "GITHUB_PATH" in l for l in lines),
                        "depot_tools must reach later steps via GITHUB_PATH")

    def test_release_job_declares_generous_timeout(self):
        # roam-110: with no explicit timeout-minutes, GitHub's default 6h job cap applies on
        # self-hosted runners too, and a cold universal2 build is service-cancelled mid-compile
        # (run 29195822447 died at exactly 6h00m). Pin an explicit, generous bound.
        text = _read("release.yml")
        self.assertIsNotNone(text, "release.yml missing")
        m = re.search(r"^\s*timeout-minutes:\s*(\d+)\s*$", text, re.M)
        self.assertIsNotNone(m, "build-sign-package must declare timeout-minutes explicitly "
                                "(GitHub's silent default is 6h — too short for a cold build)")
        self.assertGreaterEqual(int(m.group(1)), 720,
                                "release timeout must comfortably exceed the 6h default")

    def test_release_creates_universal_out_dir_before_universalizer(self):
        # roam-112: chrome/installer/mac/universalizer.py os.mkdir()s its output bundle path, so
        # its PARENT (out/Release) must exist; a fresh builder has neither, and run 29214000668
        # died with FileNotFoundError after both arch slices had built successfully. The workflow
        # must create the dir before the invocation (workflow-side fix — the upstream script is
        # not ours to edit in place).
        text = _read("release.yml")
        self.assertIsNotNone(text, "release.yml missing")
        lines = text.splitlines()
        mk = next((i for i, l in enumerate(lines)
                   if "mkdir -p" in l and "/out/Release\"" in l), None)
        self.assertIsNotNone(
            mk, "the universal out dir must be created (mkdir -p .../out/Release)")
        uni = next((i for i, l in enumerate(lines) if "universalizer.py" in l), None)
        self.assertIsNotNone(uni, "universalizer invocation missing from release.yml")
        self.assertLess(mk, uni, "out/Release must be created BEFORE the universalizer runs")

    def test_release_gn_args_are_imported_not_flattened(self):
        # roam-114: tr-flattening a commented GN args file yields ONE line whose first '#'
        # comments out every argument INCLUDING the appended target_cpu — run 29218872385 built
        # both slices as arm64 debug component builds with Sparkle off, undetected until lipo
        # refused to merge two same-arch binaries hours later. The args must be import()ed
        # (newlines survive), canary-checked right after gn gen (fail in seconds, not hours),
        # and each slice's arch asserted after compile.
        text = _read("release.yml")
        self.assertIsNotNone(text, "release.yml missing")
        self.assertNotIn("tr '\\n' ' '", text,
                         "GN args must never be newline-flattened — a leading comment swallows "
                         "the whole flattened line")
        lines = text.splitlines()
        self.assertTrue(any("import(" in l and "release.gn" in l for l in lines),
                        "gn gen must import() the release args file from the source tree")
        self.assertTrue(any("is_official_build" in l for l in lines),
                        "args canary missing: is_official_build must be verified after gn gen")
        self.assertTrue(any("lipo -archs" in l and "Release-${cpu}" in l for l in lines),
                        "per-slice arch assert missing: each slice must prove its target_cpu")

    def test_release_gates_framework_rpath(self):
        # roam-121: an unresolvable @rpath dependency on the framework aborts the
        # installed app at first launch (dyld: "no LC_RPATH's found"); the pipeline
        # must gate dyld resolution right after bundle assembly, not on a user's Mac.
        text = _read("release.yml")
        self.assertIsNotNone(text, "release.yml missing")
        self.assertIn("check_framework_rpath.py", text,
                      "the framework rpath gate must run in the release pipeline")

    def test_release_stamps_tag_version_into_bundle(self):
        # roam-120: Sparkle compares appcast sparkle:version against the installed
        # CFBundleVersion; without the tag stamp, Chromium's 7827.x outranks every
        # tag version and no update is ever offered. (roam-141: the stamped value is
        # now the numeric encoding release_version.py derives from GITHUB_REF_NAME.)
        text = _read("release.yml")
        self.assertIsNotNone(text, "release.yml missing")
        lines = text.splitlines()
        self.assertTrue(any("rename_bundle.py" in l and "--bundle-version" in l
                            for l in lines),
                        "rename_bundle must stamp a tag-derived CFBundleVersion")
        self.assertTrue(any("release_version.py" in l and "GITHUB_REF_NAME" in l
                            for l in lines),
                        "the stamped version must derive from the pushed tag")

    def test_release_publish_is_recut_safe(self):
        # roam-124: on a re-cut, gh release create spawns a SECOND draft while
        # edit-by-tag publishes the OLD release — the run goes green and ships
        # stale artifacts. The publish leg must be id-addressed end to end.
        text = _read("release.yml")
        self.assertIsNotNone(text, "release.yml missing")
        self.assertNotIn("gh release edit", text,
                         "publish must address the release by id, never by tag")
        lines = text.splitlines()
        self.assertTrue(any("-X DELETE" in l and "releases/" in l for l in lines),
                        "a prior release for the tag must be deleted before drafting")
        self.assertTrue(any("RELEASE_ID" in l and "GITHUB_ENV" in l for l in lines),
                        "the draft id must be captured for the publish step")
        self.assertTrue(any("make_latest=true" in l for l in lines),
                        "publish must mark latest (K2 feed contract)")
        # roam-126: gh api prints the 404 error BODY to stdout (--jq unapplied) while
        # exiting nonzero — the prior-release capture must be exit-code-gated and the
        # id numerically guarded, or fresh tags DELETE a garbage URL and fail.
        self.assertTrue(any("*[!0-9]*" in l for l in lines),
                        "the prior-release id must be numerically guarded")

    def test_ci_commit_check_skips_merge_commits(self):
        # roam-134: GitHub's Update-branch button injects "Merge branch 'main' into ..."
        # commits; the Conventional-Commits check must enumerate authored commits only.
        text = _read("ci.yml")
        self.assertIsNotNone(text, "ci.yml missing")
        lines = text.splitlines()
        rev_list_lines = [l for l in lines if "rev-list" in l]
        self.assertTrue(rev_list_lines, "the commit check must enumerate via rev-list")
        for l in rev_list_lines:
            self.assertIn("--no-merges", l,
                          "rev-list must skip merge commits (Update-branch friction)")

    def test_release_versions_are_numeric_via_release_version(self):
        # roam-141: Sparkle only orders numeric versions; the stamped CFBundleVersion and the
        # appcast sparkle:version must come from release_version.py, not the raw tag string.
        text = _read("release.yml")
        self.assertIsNotNone(text, "release.yml missing")
        lines = text.splitlines()
        self.assertTrue(any("release_version.py" in l and "--field bundle" in l for l in lines),
                        "the stamped bundle version must be derived numerically")
        self.assertFalse(any("--bundle-version" in l and "GITHUB_REF_NAME#v" in l for l in lines),
                         "rename_bundle must not stamp the raw tag string (roam-141)")
        self.assertFalse(any("generate_appcast.py" in l and "TAG#v" in l for l in lines),
                         "the appcast must not advertise the raw tag string (roam-141)")

    def test_release_runs_rebrand_channel_and_gate_in_order(self):
        # roam-132: the rebrand channel is a governed build step. It must run AFTER
        # apply_patches.py (patches land first) and BEFORE the grit/resource compile
        # (autoninja bakes the .pak strings), and be followed by a validation gate that
        # proves a product string rebranded, legal attribution survived, and the pass is
        # idempotent (--check clean). Ordering + gate are pinned structurally.
        text = _read("release.yml")
        self.assertIsNotNone(text, "release.yml missing")
        lines = text.splitlines()

        def _first(pred):
            return next((i for i, l in enumerate(lines) if pred(l)), None)

        apply_i = _first(lambda l: "apply_patches.py" in l)
        rebrand_i = _first(lambda l: "rebrand_strings.py" in l and "--check" not in l)
        compile_i = _first(lambda l: "autoninja -C" in l)  # the compile command, not a comment
        self.assertIsNotNone(rebrand_i, "release.yml must invoke the rebrand channel "
                                        "(rebrand_strings.py)")
        self.assertIsNotNone(apply_i, "apply_patches.py invocation missing")
        self.assertIsNotNone(compile_i, "autoninja (resource compile) missing")
        self.assertLess(apply_i, rebrand_i,
                        "rebrand must run AFTER apply_patches.py")
        self.assertLess(rebrand_i, compile_i,
                        "rebrand must run BEFORE the grit/resource compile (autoninja)")

        # Post-substitution validation gate.
        check_i = _first(lambda l: "rebrand_strings.py" in l and "--check" in l)
        self.assertIsNotNone(check_i, "the rebrand gate must assert idempotency via --check")
        self.assertLess(rebrand_i, check_i, "the --check gate runs after the rebrand pass")
        self.assertLess(check_i, compile_i, "the rebrand gate must pass before the compile")
        self.assertTrue(any("The Chromium Authors" in l for l in lines),
                        "the gate must assert legal attribution stays 'Chromium'")
        self.assertTrue(any("::error::rebrand gate" in l for l in lines),
                        "the rebrand gate must fail the release loudly (::error::)")

    def test_release_signed_invocation_passes_input_dir_and_output(self):
        # roam-97: sign_roamux.py signed mode now requires --output (a separate
        # dir); Chromium's signer takes an --input DIRECTORY (sign_roamux derives
        # it from --app's parent) and leaves the bare signed app at
        # <output>/stable/Roamux.app. The signed release invocation must pass both
        # --app (whose parent is the signer input dir) and --output, or it returns
        # 2 and never reaches the fixed path.
        text = _read("release.yml")
        self.assertIsNotNone(text, "release.yml missing")
        # Reconstruct backslash-continued shell commands, then find the signed
        # sign_roamux.py invocation.
        cmds, cur = [], []
        for line in text.splitlines():
            s = line.strip()
            cur.append(s)
            if not s.endswith("\\"):
                cmds.append(" ".join(c.rstrip("\\").strip() for c in cur))
                cur = []
        signed = [c for c in cmds
                  if "sign_roamux.py" in c and "--mode signed" in c]
        self.assertTrue(signed,
                        "release.yml must invoke sign_roamux.py in signed mode")
        for c in signed:
            self.assertIn("--app", c,
                          "signed invocation must pass --app (its parent is the "
                          "signer --input directory)")
            self.assertIn("--output", c,
                          "roam-97: signed invocation must pass --output "
                          "(Chromium's signer requires a separate output dir)")
            self.assertIn("RUNNER_TEMP", c,
                          "the signer --output dir should live under $RUNNER_TEMP")

    def test_release_builds_and_points_at_signing_package(self):
        # roam-97: config_factory.get_class() needs the GN-generated
        # build_props_config.py, which lives ONLY in the built
        # "<out>/Chromium Packaging/signing/" produced by
        # chrome/installer/mac:copy_signing (never the source tree). The release
        # build must (a) build that target, and (b) point ROAMUX_CHROMIUM_OUT at
        # a PER-SLICE build dir (out/Release-<cpu>) that actually contains it —
        # NOT the universalized out/Release, which holds only the merged .app.
        text = _read("release.yml")
        self.assertIsNotNone(text, "release.yml missing")
        self.assertIn("chrome/installer/mac:copy_signing", text,
                      "roam-97: the signed release build must build "
                      "chrome/installer/mac:copy_signing so the signing package "
                      "(build_props_config.py) exists for sign_roamux")
        # ROAMUX_CHROMIUM_OUT must be a per-slice dir (…/out/Release-<cpu>), not
        # the universalized …/out/Release.
        m = re.search(r'ROAMUX_CHROMIUM_OUT="([^"]*)"', text)
        self.assertIsNotNone(
            m, "release.yml must export ROAMUX_CHROMIUM_OUT for the signer")
        self.assertRegex(
            m.group(1), r"out/Release-(arm64|x64)",
            "roam-97: ROAMUX_CHROMIUM_OUT must point at a per-slice build dir "
            "(out/Release-<cpu>) that contains the built Chromium Packaging, "
            "not the universalized out/Release")

    def test_workflows_carry_spdx(self):
        for wf in sorted(WORKFLOWS.glob("*.yml")):
            head = "\n".join(wf.read_text().splitlines()[:3])
            self.assertIn("SPDX-License-Identifier: Apache-2.0", head,
                          f"{wf.name} missing SPDX header")


if __name__ == "__main__":
    unittest.main()


def _runs_on_is_selfhosted(lines):
    """True when a job's `runs-on:` names the self-hosted label, whether inline
    (`runs-on: [self-hosted, ...]`) or as a multiline label list."""
    live = [l for l in lines if not l.strip().startswith("#")]
    for i, line in enumerate(live):
        if not line.strip().startswith("runs-on:"):
            continue
        rest = line.split("runs-on:", 1)[1].strip()
        if rest:
            return "self-hosted" in rest
        indent = len(line) - len(line.lstrip())
        for cont in live[i + 1:]:
            if not cont.strip():
                continue
            if len(cont) - len(cont.lstrip()) <= indent:
                break
            if "self-hosted" in cont:
                return True
        return False
    return False


def _executable_text(body):
    """Drop echoed text so a log line cannot masquerade as an invocation:
    `run: echo "bash .../require_ac_power.sh"` prints a command, it does not run
    one. Deliberately narrow — truncating at `echo` rather than stripping all
    quoted text, because real commands legitimately carry quoted arguments
    (e.g. "${CHROMIUM_SRC}/.../universalizer.py"). Erring toward truncation is
    safe here: it can only cause a missing-protection failure, never a false
    pass. Comments are stripped by the caller."""
    out = []
    for line in body.splitlines():
        m = re.search(r"(?:^|[\s|&;(])echo(?:\s|$)", line)
        out.append(line[:m.start()] if m else line)
    return "\n".join(out)


# An INVOCATION, not a mention: `caffeinate` must be followed by flags and a
# command, and the gate must actually be run. A bare substring anywhere in the
# job (a comment stripped earlier, a log line, an unrelated wrapped command)
# must not count as protection.
_RE_TIER2 = re.compile(r"(?:^|[\s|])bash\s+\S*tier2_job\.sh\b", re.M)
_RE_GATE = re.compile(r"(?:^|[\s|])bash\s+\S*require_ac_power\.sh\b", re.M)
_RE_CAFFEINATE = re.compile(r"(?:^|[\s|])caffeinate\s+-\S+\s+\S", re.M)


class SelfHostedPowerProtectionTest(unittest.TestCase):
    """roam-258: every self-hosted job must be power-protected.

    The builder idle-sleeps after ~1 minute on battery, which kills long jobs with NO failing step
    and looks nearly identical to a builder network drop. Protection means BOTH: a fail-fast
    pre-flight so a battery start is refused in seconds, and a caffeinate assertion over the long
    commands so an unplug mid-run is survivable. Enumeration is deliberately independent of the
    ROAMUX_CHROMIUM_DEPENDENT marker — release.yml's job carries no such marker, so a
    marker-based scan would silently skip the longest job of the three.
    """

    GATE = "require_ac_power.sh"

    def _selfhosted_jobs(self):
        """{(workflow, job_name): job_text} for every job whose runs-on is self-hosted.
        Hoisted to the module-level _selfhosted_jobs() by roam-279 (its invariants
        enumerate the same three jobs); the parse is unchanged."""
        return _selfhosted_jobs()

    def test_enumeration_finds_the_known_selfhosted_jobs(self):
        # Non-vacuous: a broken parser must fail here rather than pass the
        # coverage test below by finding nothing.
        found = {(w, j) for (w, j) in self._selfhosted_jobs()}
        for expected in (("ci.yml", "targeted-suite-selfhosted"),
                         ("nightly.yml", "nightly-selfhosted"),
                         ("release.yml", "build-sign-package")):
            self.assertIn(expected, found, f"parser missed {expected}; found {sorted(found)}")

    def test_every_selfhosted_job_is_power_protected(self):
        for (wf, job), text in self._selfhosted_jobs().items():
            with self.subTest(f"{wf}:{job}"):
                body = _executable_text("\n".join(
                    l for l in text.splitlines()
                    if not l.strip().startswith("#")))
                if _RE_TIER2.search(body):
                    # Protection is proven behaviourally in test_tier2_job.py
                    # (re-exec under caffeinate + the gate before any side effect).
                    continue
                # Otherwise the job must carry BOTH halves itself, as real
                # invocations — a bare occurrence could wrap something
                # incidental while the actual long body stays exposed.
                self.assertRegex(body, _RE_GATE,
                                 f"{wf}:{job} does not INVOKE the power pre-flight")
                self.assertRegex(body, _RE_CAFFEINATE,
                                 f"{wf}:{job} does not wrap a command in caffeinate")

    def test_release_long_builds_are_caffeinated(self):
        text = (WORKFLOWS / "release.yml").read_text()
        body = _executable_text("\n".join(
            l for l in text.splitlines() if not l.strip().startswith("#")))
        for cmd in ("autoninja", "universalizer.py"):
            idx = body.find(cmd)
            self.assertGreater(idx, 0, f"{cmd} not found")
            line_start = body.rfind("\n", 0, idx) + 1
            self.assertIn("caffeinate", body[line_start:idx],
                          f"the long `{cmd}` invocation must be wrapped in caffeinate")

    def test_release_power_gate_is_the_first_step_after_checkout(self):
        # The frozen invariant. Parse STEPS so an intervening step fails this,
        # and assert checkout exists — otherwise a missing checkout would make
        # a find()-based check pass vacuously.
        lines = (WORKFLOWS / "release.yml").read_text().splitlines()
        steps, cur = [], None
        for line in lines:
            if line.strip().startswith("#"):
                continue
            if re.match(r"^      - (name|uses):", line):
                if cur is not None:
                    steps.append("\n".join(cur))
                cur = [line]
            elif cur is not None:
                cur.append(line)
        if cur is not None:
            steps.append("\n".join(cur))
        idx = [i for i, st in enumerate(steps) if "actions/checkout" in st]
        self.assertTrue(idx, "release.yml must check out the overlay")
        after = _executable_text(steps[idx[-1] + 1])
        self.assertRegex(after, _RE_GATE,
                         "the power gate must be the FIRST step after checkout, "
                         f"but that step is: {after.splitlines()[0].strip()}")

    def test_release_power_gate_precedes_the_expensive_steps(self):
        body = "\n".join(l for l in (WORKFLOWS / "release.yml").read_text().splitlines()
                          if not l.strip().startswith("#"))
        gate = body.find(self.GATE)
        self.assertGreater(gate, 0, "release.yml must run the power pre-flight")
        for expensive in ("fetch_sparkle.py", "apply_patches.py", "autoninja",
                          "universalizer.py"):
            at = body.find(expensive)
            self.assertGreater(at, 0, f"{expensive} vanished from release.yml")
            self.assertLess(gate, at, f"the power gate must precede `{expensive}`")


# ---------------------------------------------------------------------------------------------
# roam-279 (grill H5 / H6 / M9-timeout). Three self-hosted jobs reset, re-patch and re-point the
# SAME shared warm base; the invariants below make their mutual exclusion, the release symlink
# restore and an explicit job timeout structural instead of an accident of having one runner.

SHARED_BASE_GROUP = "roamux-shared-base"
# Queue depth the group declares. "max" keeps every waiting job (up to 100) instead of GitHub's
# default single pending slot, where a NEWER arrival cancels the older pending job — with tier-2 a
# required check, a busy day would turn into re-run duty. This is the single knob of the documented
# fallback: set it to None if GitHub ever rejects `queue:` at job level and the assertion flips to
# "no queue key" (the runner doc and the acceptance wording change with it — roam-279 plan §5).
SHARED_BASE_QUEUE = "max"
# Strictly above GitHub's silent 6h (360 min) default, which applies to self-hosted runners too:
# a cold tier-2 exceeds it (nightly run 29827734729 was service-cancelled at exactly 6h00m on
# 2026-07-21 — the tier-2 twin of roam-110's release case).
SELFHOSTED_TIMEOUT_FLOOR = 420
KNOWN_SELFHOSTED_JOBS = (("ci.yml", "targeted-suite-selfhosted"),
                         ("nightly.yml", "nightly-selfhosted"),
                         ("release.yml", "build-sign-package"))
KNOWN_HOSTED_JOBS = (("ci.yml", "lint"), ("ci.yml", "governance"), ("ci.yml", "targeted-suite"),
                     ("nightly.yml", "hermetic-suite"), ("issue-link.yml", "check-issue-link"))


def _jobs(text):
    """{job_name: job_text} for every job of one workflow, in file order. A job key is exactly
    two-space indented and ends with ':'; everything up to the next job key is its text
    (comment lines included — consumers strip what they must)."""
    jobs, name, buf, in_jobs = {}, None, [], False
    for line in text.splitlines():
        if line.startswith("jobs:"):
            in_jobs = True
            continue
        if not in_jobs:
            continue
        stripped = line.strip()
        if (line.startswith("  ") and not line.startswith("   ")
                and stripped.endswith(":") and not stripped.startswith("#")):
            if name is not None:
                jobs[name] = "\n".join(buf)
            name, buf = stripped[:-1], []
        elif name is not None:
            buf.append(line)
    if name is not None:
        jobs[name] = "\n".join(buf)
    return jobs


def _all_jobs():
    """{(workflow, job_name): job_text} across every workflow file."""
    out = {}
    for wf in sorted(list(WORKFLOWS.glob("*.yml")) + list(WORKFLOWS.glob("*.yaml"))):
        for name, text in _jobs(wf.read_text()).items():
            out[(wf.name, name)] = text
    return out


def _selfhosted_jobs():
    """{(workflow, job_name): job_text} for every job whose runs-on DECLARATION is self-hosted.
    Parsed structurally — `targeted-suite` merely echoes "self-hosted" in a log line and is
    hosted — for inline `[a, b]` and multiline `- a` label lists alike."""
    return {k: t for k, t in _all_jobs().items() if _runs_on_is_selfhosted(t.splitlines())}


def _job_key_block(job_text, key):
    """The lines of one 4-space-indented mapping key inside a job (key line included), or None.
    Comment lines are skipped; the block ends at the first line indented 4 spaces or less."""
    lines = job_text.splitlines()
    for i, line in enumerate(lines):
        if line.rstrip() == f"    {key}:":
            block = [line]
            for cont in lines[i + 1:]:
                if not cont.strip() or cont.strip().startswith("#"):
                    continue
                if len(cont) - len(cont.lstrip()) <= 4:
                    break
                block.append(cont)
            return "\n".join(block)
    return None


def _job_has_key(job_text, key):
    """True when a job declares `key` at job level in ANY form — scalar (`    key: value`), inline
    mapping (`    key: { … }`) or block (`    key:` + indented lines). A commented-out line never
    counts. The hosted-job guard needs this breadth: a scalar `concurrency: <name>` serializes a
    job exactly as the block form does."""
    return re.search(rf"(?m)^    {re.escape(key)}:(?:\s|$)", job_text) is not None


def _job_scalar(job_text, key):
    """The value of a 4-space-indented scalar key (`    key: value`) inside a job, or None."""
    m = re.search(rf"^    {re.escape(key)}:[ \t]*([^#\n]+?)[ \t]*(?:#.*)?$", job_text, re.M)
    return m.group(1) if m else None


def _release_steps():
    """release.yml's step blocks with comment lines removed — the same splitter
    test_release_power_gate_is_the_first_step_after_checkout applies inline."""
    lines = (WORKFLOWS / "release.yml").read_text().splitlines()
    steps, cur = [], None
    for line in lines:
        if line.strip().startswith("#"):
            continue
        if re.match(r"^      - (name|uses):", line):
            if cur is not None:
                steps.append("\n".join(cur))
            cur = [line]
        elif cur is not None:
            cur.append(line)
    if cur is not None:
        steps.append("\n".join(cur))
    return steps


def _step_run_script(step_text):
    """The dedented body of a step's `run: |` block, or None when the step has none."""
    lines = step_text.splitlines()
    for i, line in enumerate(lines):
        if re.match(r"^\s*run:\s*\|\s*$", line):
            indent = len(line) - len(line.lstrip())
            body = []
            for cont in lines[i + 1:]:
                if cont.strip() and len(cont) - len(cont.lstrip()) <= indent:
                    break
                body.append(cont)
            nonblank = [l for l in body if l.strip()]
            if not nonblank:
                return None
            cut = min(len(l) - len(l.lstrip()) for l in nonblank)
            return "\n".join(l[cut:] if l.strip() else "" for l in body) + "\n"
    return None


class SharedBaseConcurrencyTest(unittest.TestCase):
    """Invariant 18 (roam-279 / H5): the three self-hosted jobs are mutually exclusive BY
    DECLARATION — one job-level concurrency group shared across all three workflows — and nothing
    else is serialized (a workflow-level group would queue every PR's hosted lint behind a
    50-minute tier-2 and expose it to the pending-queue rule)."""

    def test_enumeration_finds_the_known_selfhosted_jobs(self):
        # Non-vacuous: a broken parser must fail here, not pass the coverage tests by finding nothing.
        found = set(_selfhosted_jobs())
        for expected in KNOWN_SELFHOSTED_JOBS:
            self.assertIn(expected, found, f"parser missed {expected}; found {sorted(found)}")

    def test_every_selfhosted_job_joins_the_shared_base_group(self):
        for (wf, job), text in sorted(_selfhosted_jobs().items()):
            with self.subTest(f"{wf}:{job}"):
                block = _job_key_block(text, "concurrency")
                self.assertIsNotNone(block, f"{wf}:{job} declares no job-level concurrency — it "
                                            "mutates the shared base and must join the "
                                            f"{SHARED_BASE_GROUP} group (roam-279 / H5)")
                self.assertRegex(block, rf"(?m)^\s+group:\s*{re.escape(SHARED_BASE_GROUP)}\s*$",
                                 f"{wf}:{job} must use the shared group name")
                self.assertRegex(block, r"(?m)^\s+cancel-in-progress:\s*false\s*$",
                                 f"{wf}:{job}: a running build must never be killed by a newcomer")
                if SHARED_BASE_QUEUE is None:
                    self.assertNotRegex(block, r"(?m)^\s+queue:",
                                        f"{wf}:{job}: the fallback configuration declares no queue key")
                else:
                    self.assertRegex(block, rf"(?m)^\s+queue:\s*{re.escape(SHARED_BASE_QUEUE)}\s*$",
                                     f"{wf}:{job}: waiting jobs must queue (queue: {SHARED_BASE_QUEUE}) "
                                     "instead of cancelling the older pending job")

    def test_all_selfhosted_jobs_share_one_group(self):
        groups = set()
        for text in _selfhosted_jobs().values():
            block = _job_key_block(text, "concurrency") or ""
            m = re.search(r"(?m)^\s+group:\s*(\S+)\s*$", block)
            if m:
                groups.add(m.group(1))
        self.assertEqual(groups, {SHARED_BASE_GROUP},
                         "mutual exclusion needs ONE group name across all three workflows; "
                         f"got {sorted(groups)}")

    def test_no_workflow_level_concurrency(self):
        for wf in sorted(WORKFLOWS.glob("*.yml")):
            top = [l for l in wf.read_text().splitlines()
                   if l and not l[0].isspace() and not l.startswith("#")]
            self.assertFalse(any(l.split(":")[0] == "concurrency" for l in top),
                             f"{wf.name}: a workflow-level concurrency group would serialize the "
                             "hosted jobs too — declare it per self-hosted job")

    def test_hosted_jobs_are_not_serialized(self):
        selfhosted = set(_selfhosted_jobs())
        hosted = {k: t for k, t in _all_jobs().items() if k not in selfhosted}
        for expected in KNOWN_HOSTED_JOBS:  # non-vacuous
            self.assertIn(expected, hosted, f"parser missed hosted job {expected}")
        for (wf, job), text in sorted(hosted.items()):
            with self.subTest(f"{wf}:{job}"):
                self.assertFalse(_job_has_key(text, "concurrency"),
                                 f"{wf}:{job} is hosted and touches no shared base — "
                                 "it must not be serialized (in any concurrency form)")

    def test_job_level_concurrency_is_detected_in_every_form(self):
        # Regression for the hosted guard (Step-8 review): a scalar or an inline mapping is a
        # valid declaration that serializes a hosted job exactly as the block form does, and the
        # block-only parser let both through.
        for label, snippet in (
            ("scalar", "    runs-on: macos-14\n    concurrency: roamux-shared-base\n    steps:\n"),
            ("inline", "    runs-on: macos-14\n    concurrency: { group: roamux-shared-base, "
                       "cancel-in-progress: false }\n    steps:\n"),
            ("block", "    runs-on: macos-14\n    concurrency:\n      group: roamux-shared-base\n"
                      "    steps:\n"),
        ):
            with self.subTest(label):
                self.assertTrue(_job_has_key(snippet, "concurrency"), f"{label} form not detected")
        for label, snippet in (
            ("comment", "    runs-on: macos-14\n    # concurrency: roamux-shared-base\n    steps:\n"),
            ("nested", "    runs-on: macos-14\n    with:\n      concurrency: 3\n    steps:\n"),
            ("prefix", "    concurrency-note: x\n    steps:\n"),
        ):
            with self.subTest(label):
                self.assertFalse(_job_has_key(snippet, "concurrency"), f"{label} must not count")


class ReleaseOverlayRestoreTest(unittest.TestCase):
    """Invariant 19 (roam-279 / H6), structural: release re-points the shared base's overlay
    symlink at its own checkout; it must validate the canonical restore target up front and
    restore it in a FINAL always() step, whatever happened in between."""

    def test_machine_env_validates_and_exports_canonical_overlay(self):
        # Scoped to the machine-env step's EXECUTABLE script: _release_steps drops comment lines,
        # so a commented-out guard or export is simply absent here and fails (Step-8 review — the
        # whole-file substring version passed with every guard commented out).
        steps = _release_steps()
        env_i = next((i for i, st in enumerate(steps)
                      if 'env_file="${HOME}/roamux-runner/.env"' in st), None)
        self.assertIsNotNone(env_i, "release.yml has no machine-env step (roam-108)?")
        script = _step_run_script(steps[env_i])
        self.assertIsNotNone(script, "the machine-env step must carry an inline `run: |` script")
        guards = (r'-z "\$\{ROAMUX_CANONICAL_OVERLAY:-\}"',
                  r'! -d "\$\{ROAMUX_CANONICAL_OVERLAY\}"')
        for guard in guards:
            m = re.search(rf"(?ms)^if \[ {guard} \]; then\n(.*?)^fi$", script)
            self.assertIsNotNone(m, f"machine-env step lacks the executable guard `if [ {guard} ]` "
                                    "(tier2_job.sh:36 already requires the variable)")
            body = m.group(1)
            self.assertIn("::error::", body, "the guard must fail loudly")
            self.assertIn("ROAMUX_CANONICAL_OVERLAY", body, "the error must name the variable")
            self.assertRegex(body, r"\bexit 1\b", "the guard must exit non-zero")
        self.assertRegex(script,
                         r'(?m)^echo "CANONICAL_OVERLAY=\$\{ROAMUX_CANONICAL_OVERLAY\}" >> "\$GITHUB_ENV"$',
                         "the validated restore target must be exported via GITHUB_ENV as an "
                         "executable statement — the final step reads CANONICAL_OVERLAY from it")
        export_at = script.find('echo "CANONICAL_OVERLAY=')
        for guard in guards:
            self.assertLess(script.find(guard.replace("\\", "")), export_at,
                            "both guards must run BEFORE the export")
        flip_i = next((i for i, st in enumerate(steps) if 'ln -sfn "$(pwd)/roamux"' in st), None)
        self.assertIsNotNone(flip_i, "release.yml no longer flips the overlay symlink?")
        self.assertLess(env_i, flip_i, "the restore target must be validated BEFORE the flip")

    def test_final_step_always_restores_the_canonical_overlay(self):
        steps = _release_steps()
        self.assertTrue(steps, "release.yml has no steps?")
        flip = next((i for i, st in enumerate(steps) if 'ln -sfn "$(pwd)/roamux"' in st), None)
        self.assertIsNotNone(flip, "release.yml no longer flips the overlay symlink?")
        last = steps[-1]
        self.assertRegex(last, r"(?m)^\s*if:\s*always\(\)\s*$",
                         "the LAST step must be the always() restore — "
                         f"got: {last.splitlines()[0].strip()}")
        script = _step_run_script(last)
        self.assertIsNotNone(script, "the restore step must carry an inline `run: |` script")
        ln = 'ln -sfn "${CANONICAL_OVERLAY}"'
        self.assertEqual(script.count(ln), 1, "exactly one restore link, after both guards")
        self.assertIn("${CHROMIUM_SRC}/roamux", script,
                      "the restore must target the base's overlay path")
        at = script.find(ln)
        self.assertIn("-L", script[:at], "test for a symlink BEFORE linking — ln -sfn into a real "
                                        "directory creates <dir>/roamux and reports success (M9)")
        self.assertIn("readlink", script[at:], "verify the restore with readlink AFTER linking")
        self.assertLess(flip, len(steps) - 1, "the flip must precede the restore")


class ReleaseOverlayRestoreBehaviourTest(unittest.TestCase):
    """Invariant 19, EXECUTED: the restore script is extracted from release.yml (never a copy,
    so the test cannot drift from the workflow) and run under /bin/bash — macOS ships 3.2 and the
    hosted `lint` job runs these tests — against temp-dir fixtures, one per exit path of the
    step's contract: P1 unresolved env → notice, exit 0, nothing touched; P2 a real directory at
    the link path → error, exit 1, nothing created inside it; P3 restore → exit 0 and readlink
    equality. Same fixture style as test_tier2_job.py's power-gate tests."""

    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp(prefix="roamux-restore-"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.src = self.tmp / "chromium" / "src"
        self.src.mkdir(parents=True)
        self.canonical = self.tmp / "codes" / "roamux" / "roamux"
        self.canonical.mkdir(parents=True)
        steps = _release_steps()
        self.script = _step_run_script(steps[-1]) if steps else None

    def _run(self, **env):
        self.assertIsNotNone(self.script,
                             "release.yml has no final `run: |` restore step to execute")
        e = {"PATH": "/usr/bin:/bin", "HOME": str(self.tmp)}
        e.update(env)
        return subprocess.run(["/bin/bash", "-c", self.script], capture_output=True, text=True,
                              env=e, cwd=str(self.tmp), timeout=30)

    def test_unresolved_env_is_a_notice_and_a_noop(self):
        for label, env in (("both unset", {}),
                           ("both empty", {"CHROMIUM_SRC": "", "CANONICAL_OVERLAY": ""}),
                           ("target unset", {"CHROMIUM_SRC": str(self.src)})):
            with self.subTest(label):
                r = self._run(**env)
                self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
                self.assertIn("::notice::", r.stdout + r.stderr)
                link = self.src / "roamux"
                self.assertFalse(link.exists() or link.is_symlink(),
                                 "nothing may be linked when the env step never resolved")

    def test_real_directory_is_refused_without_linking(self):
        real = self.src / "roamux"
        real.mkdir()
        (real / "marker").write_text("keep me\n")
        r = self._run(CHROMIUM_SRC=str(self.src), CANONICAL_OVERLAY=str(self.canonical))
        self.assertNotEqual(r.returncode, 0, "a real directory at the link path must fail the step")
        self.assertIn("::error::", r.stdout + r.stderr)
        self.assertTrue(real.is_dir() and not real.is_symlink(), "the directory must be left alone")
        self.assertFalse((real / "roamux").exists(),
                         "ln -sfn into a real directory would create roamux/roamux")
        self.assertTrue((real / "marker").exists())

    def test_stale_symlink_is_repointed_and_verified(self):
        stale = self.tmp / "_work" / "roamux" / "roamux"
        stale.mkdir(parents=True)
        link = self.src / "roamux"
        link.symlink_to(stale)
        r = self._run(CHROMIUM_SRC=str(self.src), CANONICAL_OVERLAY=str(self.canonical))
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertTrue(link.is_symlink())
        self.assertEqual(os.readlink(link), str(self.canonical))
        self.assertIn(str(self.canonical), r.stdout, "the restored target must be logged")
        # A dangling link (the previous job's workspace was reclaimed) is the same case.
        shutil.rmtree(stale)
        link.unlink()
        link.symlink_to(stale)
        r = self._run(CHROMIUM_SRC=str(self.src), CANONICAL_OVERLAY=str(self.canonical))
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(os.readlink(link), str(self.canonical))
        # No link at all (a fresh base): created, not skipped.
        link.unlink()
        r = self._run(CHROMIUM_SRC=str(self.src), CANONICAL_OVERLAY=str(self.canonical))
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(os.readlink(link), str(self.canonical))


class ReleaseMachineEnvBehaviourTest(unittest.TestCase):
    """Invariant 19, EXECUTED for the machine-env half: the step's script (extracted from
    release.yml) must refuse a .env without ROAMUX_CANONICAL_OVERLAY, refuse a non-directory,
    and export CANONICAL_OVERLAY next to CHROMIUM_SRC when valid — otherwise the restore step
    would find nothing to restore to and skip via P1 after the flip (Step-8 review)."""

    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp(prefix="roamux-machine-env-"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        steps = _release_steps()
        env_step = next((st for st in steps if 'env_file="${HOME}/roamux-runner/.env"' in st), None)
        self.script = _step_run_script(env_step) if env_step else None
        self.home = self.tmp / "home"
        (self.home / "roamux-runner").mkdir(parents=True)
        self.src = self.tmp / "chromium" / "src"
        self.src.mkdir(parents=True)
        self.depot = self.tmp / "depot_tools"
        self.depot.mkdir()
        self.canonical = self.tmp / "codes" / "roamux" / "roamux"
        self.canonical.mkdir(parents=True)
        self.github_env = self.tmp / "github_env"
        self.github_path = self.tmp / "github_path"

    def _base_env(self):
        return [f"ROAMUX_CHROMIUM_SRC={self.src}", f"ROAMUX_DEPOT_TOOLS={self.depot}"]

    def _run(self, env_lines):
        self.assertIsNotNone(self.script,
                             "release.yml has no machine-env `run: |` step to execute")
        (self.home / "roamux-runner" / ".env").write_text("".join(l + "\n" for l in env_lines))
        self.github_env.write_text("")
        self.github_path.write_text("")
        e = {"PATH": "/usr/bin:/bin", "HOME": str(self.home),
             "GITHUB_ENV": str(self.github_env), "GITHUB_PATH": str(self.github_path)}
        return subprocess.run(["/bin/bash", "-c", self.script], capture_output=True, text=True,
                              env=e, cwd=str(self.tmp), timeout=30)

    def test_missing_canonical_overlay_fails_loudly_before_export(self):
        r = self._run(self._base_env())
        self.assertNotEqual(r.returncode, 0, r.stdout + r.stderr)
        out = r.stdout + r.stderr
        self.assertIn("::error::", out)
        self.assertIn("ROAMUX_CANONICAL_OVERLAY", out)
        self.assertNotIn("CANONICAL_OVERLAY=", self.github_env.read_text(),
                         "nothing may be exported when the .env contract is unsatisfied")

    def test_non_directory_canonical_overlay_fails_loudly(self):
        r = self._run(self._base_env() + [f"ROAMUX_CANONICAL_OVERLAY={self.tmp}/does-not-exist"])
        self.assertNotEqual(r.returncode, 0, r.stdout + r.stderr)
        out = r.stdout + r.stderr
        self.assertIn("::error::", out)
        self.assertIn("ROAMUX_CANONICAL_OVERLAY", out)
        self.assertNotIn("CANONICAL_OVERLAY=", self.github_env.read_text())

    def test_valid_env_exports_canonical_overlay_and_chromium_src(self):
        r = self._run(self._base_env() + [f"ROAMUX_CANONICAL_OVERLAY={self.canonical}"])
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        exported = self.github_env.read_text()
        self.assertIn(f"CANONICAL_OVERLAY={self.canonical}\n", exported,
                      "the restore step reads CANONICAL_OVERLAY from GITHUB_ENV")
        self.assertIn(f"CHROMIUM_SRC={self.src}\n", exported)
        self.assertIn(str(self.depot), self.github_path.read_text())


class SelfHostedTimeoutTest(unittest.TestCase):
    """Invariant 20 (roam-279 / M9): every self-hosted job declares timeout-minutes above
    GitHub's silent 6h default. Generalizes invariant 7 (release only) to the two tier-2 jobs,
    whose cold rebuild was service-cancelled at exactly 6h00m with no failing step of its own."""

    def test_every_selfhosted_job_declares_timeout(self):
        jobs = _selfhosted_jobs()
        for expected in KNOWN_SELFHOSTED_JOBS:  # non-vacuous
            self.assertIn(expected, set(jobs), f"parser missed {expected}")
        for (wf, job), text in sorted(jobs.items()):
            with self.subTest(f"{wf}:{job}"):
                value = _job_scalar(text, "timeout-minutes")
                self.assertIsNotNone(value, f"{wf}:{job} relies on GitHub's silent 6h default "
                                            "(nightly run 29827734729 died at exactly 6h00m)")
                self.assertRegex(value, r"^\d+$",
                                 f"{wf}:{job}: timeout-minutes must be a literal integer")
                self.assertGreaterEqual(int(value), SELFHOSTED_TIMEOUT_FLOOR,
                                        f"{wf}:{job}: the bound must exceed the 6h default it replaces")


# ---------------------------------------------------------------------------------------------
# roam-281 (grill H7): the kill switch must be RED, not a skip.


def _job_steps(job_text):
    """A job's step blocks (comment lines dropped), split at `      - name:` / `      - uses:`."""
    steps, cur = [], None
    for line in job_text.splitlines():
        if line.strip().startswith("#"):
            continue
        if re.match(r"^      - (name|uses):", line):
            if cur is not None:
                steps.append("\n".join(cur))
            cur = [line]
        elif cur is not None:
            cur.append(line)
    if cur is not None:
        steps.append("\n".join(cur))
    return steps


class KillSwitchIsRedTest(unittest.TestCase):
    """Invariant 21 (roam-281 / H7). Branch protection on main requires `targeted-suite-selfhosted`;
    GitHub treats a required check in `skipped` status as satisfied. So gating that job on the
    capability variable made an empty variable a vacuous pass. The job must instead run for every
    same-repo event, pick a hosted runner when the variable is empty, and fail red in its first step
    there; the announce job says so; nightly (not a required check) keeps its arm on purpose."""

    JOB = "targeted-suite-selfhosted"

    def _job(self):
        text = _read("ci.yml")
        self.assertIsNotNone(text, "ci.yml missing")
        block = _job_block(text, self.JOB)
        self.assertIsNotNone(block, f"ci.yml has no {self.JOB} job")
        return block

    def test_required_job_is_not_skippable_by_the_switch(self):
        block = self._job()
        if_line = next((l for l in block.splitlines() if l.strip().startswith("if:")), None)
        self.assertIsNotNone(if_line, "the job must keep an explicit trust predicate")
        self.assertNotIn(CAPABILITY_VAR, if_line,
                         "an if:-skipped required check is a vacuous pass — the switch must not gate it")
        self.assertIn("github.event.pull_request.head.repo.fork == false", if_line,
                      "forks must stay excluded by the trust predicate (R15)")
        self.assertIn("github.event_name == 'push' && github.ref == 'refs/heads/main'", if_line)

    def test_runner_falls_back_to_hosted_when_switch_is_empty(self):
        block = self._job()
        runs_on = next((l for l in block.splitlines() if l.strip().startswith("runs-on:")), "")
        self.assertIn(SELFHOSTED_RUNS_ON_EXPR, runs_on,
                      "runs-on must pick the fixed triple when the switch is non-empty and a hosted "
                      "runner when it is empty")
        self.assertIn("self-hosted", runs_on, "the enumeration helpers key on this substring")

    def test_first_step_fails_red_when_switch_is_empty(self):
        steps = _job_steps(self._job())
        self.assertGreaterEqual(len(steps), 3, f"expected kill-switch, checkout, tier-2 steps; got {steps}")
        first, rest = steps[0], "\n".join(steps[1:])
        self.assertIn(CAPABILITY_VAR, first, "the FIRST step must read the switch")
        self.assertIn("exit 1", first, "the first step must fail when the switch is empty")
        self.assertIn("::error::", first, "the failure must be loud")
        self.assertNotIn("actions/checkout", first)
        self.assertNotIn("tier2_job.sh", first)
        self.assertIn("actions/checkout", rest, "checkout must come AFTER the kill-switch step")
        self.assertIn("tier2_job.sh", rest, "the tier-2 script must come AFTER the kill-switch step")

    def test_kill_switch_step_script_exits_1_when_empty(self):
        # BEHAVIOURAL: the first step's script, extracted from ci.yml (never a copy), run the way
        # GitHub runs a `run:` step (bash -e) with the switch empty and then set.
        first = _job_steps(self._job())[0]
        script = _step_run_script(first)
        self.assertIsNotNone(script, "the kill-switch step must carry an inline `run: |` script")
        tmp = pathlib.Path(tempfile.mkdtemp(prefix="roamux-killswitch-"))
        self.addCleanup(shutil.rmtree, tmp, ignore_errors=True)
        summary = tmp / "summary"

        def run(cap):
            summary.write_text("")
            e = {"PATH": "/usr/bin:/bin", "CAP": cap, "GITHUB_STEP_SUMMARY": str(summary)}
            return subprocess.run(["/bin/bash", "-e", "-c", script], capture_output=True,
                                  text=True, env=e, timeout=30)

        r = run("")
        out = r.stdout + r.stderr
        self.assertEqual(r.returncode, 1, out)
        self.assertIn("::error::", out)
        self.assertIn(CAPABILITY_VAR, out, "the error must name the variable")
        self.assertIn(CAPABILITY_VAR, summary.read_text(), "the summary must say why the check is red")
        r = run("roamux-builder-1")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_announce_names_the_red_consequence(self):
        text = _read("ci.yml")
        block = _job_block(text, "targeted-suite")
        self.assertIsNotNone(block, "ci.yml has no announce job")
        code = "\n".join(l for l in block.splitlines() if not l.strip().startswith("#"))
        self.assertNotIn("SKIPPED: no capable", code,
                         "the announce job must no longer describe the off state as a skip")
        self.assertIn("RED", code, "the announce job must say the required check goes red")
        self.assertIn(CAPABILITY_VAR, code)
        self.assertNotIn("exit 1", code, "the announce job stays informational (never fails)")

    def test_nightly_keeps_its_capability_arm(self):
        # Deliberate asymmetry, pinned so it is not "fixed" by accident: nightly-selfhosted is not a
        # required check, and its schedule/dispatch arms are a different contract.
        text = _read("nightly.yml")
        self.assertIsNotNone(text, "nightly.yml missing")
        block = _job_block(text, "nightly-selfhosted")
        self.assertIsNotNone(block)
        if_line = next((l for l in block.splitlines() if l.strip().startswith("if:")), "")
        self.assertIn(CAPABILITY_VAR, if_line, "nightly keeps the capability arm (not a required check)")
