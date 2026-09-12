---
plugin: grill
version: 1.3.0
date: 2026-08-26
target: /Users/xqliu/opt/aispace/claude-projects/eric-tech/010-Roamux/codes/roamux
style: Paranoid Mode (Edge Case Gauntlet)
addons: [Scale stress, Hidden costs, Principle violations, Compact & optimize, Strangler fig, Success metrics, Before vs after, Assumptions audit]
agents: [recon, architecture, error-handling, security, testing, edge-cases]
---

# Roamux — Grill Report (Paranoid Mode)

**Target:** `codes/roamux` @ `16d7327` (PR #278 / roam-277, 2026-08-26)
**Style:** Paranoid Mode — 5 deep-dive agents + recon, all 8 add-on pressure tests.
**Agent status:** architecture, error-handling, security, testing completed normally. The edge-cases agent was cut off by a transient API error just before writing its report; it was resumed with its research context intact and completed. No agent timed out.
**Synthesis-time validation:** one read-only scan of the pinned Chromium checkout's `.xtb` files was run to quantify the edge-cases agent's top finding (H3). Nothing was written, built, or fetched.

Severity legend: `[CRITICAL]` fix immediately · `[HIGH]` this sprint · `[MEDIUM]` next sprint · `[LOW]` when touching the file · `[GOOD]` keep. Effort: `[< 1 day]` `[< 1 week]` `[< 1 month]` `[> 1 month]`. Finding IDs (C1, H1…, M1…, L1…) are referenced by the Fixing Plan. Paths are relative to the repo root unless absolute.

---

## 1. Codebase snapshot (recon)

- Chromium-fork overlay for macOS. `roamux/` is symlinked to `~/chromium/src/roamux` inside a pinned Chromium **149.0.7827.201** checkout that is **shared** between the self-hosted CI runner and the maintainer's local builds on one Mac.
- **457 tracked files.** C++/ObjC++: 12,016 production LOC vs 20,365 test LOC (test = 63% of native). Python 58 files / 9.5k LOC (build channels, gates, 362 hermetic tests). **65 patches** numbered 0001–0070 (retired gaps 0027/0031/0032/0035/0036) touching 124 upstream files. 4 workflows (incl. a 352-line `release.yml`). 134 commits, single author, squash merges.
- Integration channels: patch stack (`apply_patches.py`), one `chromium_src/` include-shadow override, build-time in-place string rebrand (`rebrand_strings.py`) — plus, as the architecture agent found, an undeclared fourth: 49 overlay files compiled into upstream targets via `sources +=`.
- 11 `base::Feature`s, all `FEATURE_ENABLED_BY_DEFAULT`. Sparkle 2.9.4 EdDSA auto-update; unsigned/un-notarized personal-alpha distribution; feed = `releases/latest/download/appcast.xml`.
- CI: hosted `lint`/`governance` (hermetic Python only) + self-hosted `targeted-suite-selfhosted` (`tier2_job.sh`: reset shared base, apply patches, build 3 test targets, run with retry-limit 2). Release on the same runner, triggered by any `v*` tag.

---

## 2. Findings (deduplicated across agents, ranked)

Where two agents reported the same defect, the entry with the strongest evidence is kept and the other is credited in **Source**.

### 2.1 CRITICAL

#### C1. Any `v*` tag push becomes a signed, published update — on a runner shared with PR jobs
- **Source:** security #2 (+ security #3, error-handling #23 for the key-material half)
- **File:** `.github/workflows/release.yml:14-18,29-34,46,60,113-114,243-249,273-289`; `.github/workflows/ci.yml:102-107`; `docs/ci/self-hosted-runner.md:34-45`
- **Observation:** `release.yml` fires on any `v*` tag, pulls `SPARKLE_ED_PRIVATE_KEY` from the `release` environment, signs whatever the tag points at, and PATCHes it to `latest` — the feed every install polls daily. Nothing in the repo shows a tag ruleset or a required reviewer on the environment. The job runs on the **same Mac, as the same login user** that executes same-repo PR code via `tier2_job.sh`; it `source`s `~/roamux-runner/.env`, prepends `${ROAMUX_DEPOT_TOOLS}` (which ships a `python3` shim) to `PATH`, and reconciles the shared checkout with `git clean -fd` (no `-x`), so a PR job can plant persistence that runs inside the next secrets-bearing release job. The staging-validation step additionally imports the **private** key into the runner's login keychain and relies on a bash `EXIT` trap (bypassed by SIGKILL/timeout/sleep) to remove it; `keychain_cleanup.sh` (the `always()` step) does not delete that item. Verification needs only the committed public key.
- **Evidence:**
  ```yaml
  on: { push: { tags: ["v*"] } }
  set -a; . "${env_file}"; set +a                       # release.yml:46
  echo "${ROAMUX_DEPOT_TOOLS}" >> "$GITHUB_PATH"        # :60
  git -C "${CHROMIUM_SRC}" clean -fd -e /roamux         # :114 (no -x)
  trap 'security delete-generic-password -s "https://sparkle-project.org" -a "$acct" …' EXIT  # :281
  "$bindir/generate_keys" --account "$acct" -f "$keyfile"                                      # :289
  ```
  `docs/ci/self-hosted-runner.md:36-37`: "anything that user can read (keychain, SSH keys, `gh` tokens, dotfiles) is reachable by CI-executed code."
- **Why CRITICAL (upgraded from the agent's HIGH):** a single leaked push-capable credential (this project runs `gh` from many places) equals the update-signing key, with a blast radius of every installed user, and the first two mitigations cost under a day.
- **Proposed change:** (1) Repository ruleset restricting `refs/tags/v*` creation to the maintainer, no app bypass. (2) Required reviewer on the `release` environment so a tag alone cannot start signing. (3) Move signing off the shared runner: a dedicated runner user with an empty HOME, or a hosted `macos-14` job that only signs the already-built zip. (4) Replace `. .env` with a strict `KEY=value` parser; use a pristine out-dir (or `clean -fdx`) for release builds. (5) Verify appcasts with the **public** key only (`ed25519_ref.py` / `openssl pkeyutl -verify -pubin`), or use a `$RUNNER_TEMP` throwaway keychain and add the delete to `keychain_cleanup.sh`. (6) Rotate `SPARKLE_ED_PRIVATE_KEY` after isolation.
- **Tradeoff:** one click per release; an artifact hand-off between build and sign jobs; a cold release build if `clean -fdx` is chosen instead of a separate out-dir.
- **Effort:** rulesets + reviewer + public-key verify `[< 1 day]`; runner isolation `[< 1 week]`

### 2.2 HIGH

#### H1. Chromium pin is ~2 months stale with no security-uprev cadence or trigger
- **Source:** security #1
- **File:** `roamux/build/CHROMIUM_PIN:1-3`; `docs/adr/0001-chromium-overlay-strategy.md:16`
- **Observation:** Shipped builds are on 149.0.7827.201 (stable 2026-06-25). On Chrome's 4-week cadence, M150 and M151 have both reached stable since (not verified against the network — inferred from cadence), each with a security bulletin, plus weekly refreshes. The pin comment says "re-pin every milestone cycle"; nothing enforces it, and the last release (alpha.9, 2026-08-04) is on the same pin. The Sparkle channel has nothing newer to deliver.
- **Evidence:** `git log -- roamux/build/CHROMIUM_PIN` → one commit in 134 (the rename). `git tag` newest = `v0.0.1-alpha.9 2026-08-04`.
- **Proposed change:** nightly staleness step (`git ls-remote --tags` in `nightly.yml`) that opens/updates a roam-N issue when a newer stable exists; `docs/security-uprev.md` with an SLA ("re-pin within 14 days of a stable bulletin; 72 h for an in-the-wild CVE"); cut alpha.10 on the first uprev.
- **Tradeoff:** each uprev = 65-patch rebase + ~10 h cold universal build; the SLA makes the recurring cost explicit.
- **Effort:** process `[< 1 day]`; first uprev `[< 1 month]`

#### H2. The uprev playbook has never been exercised; rebase surface is concentrated and includes a `//base` header patch
- **Source:** architecture #3 (+ edge-cases #9 for the M150 time bomb, see M5)
- **File:** `roamux/patches/0056-shutdown-drain-test-diagnostics.patch`; `roamux/patches/README.md:90-91`; `BOOTSTRAP.md:36`; `roamux/patches/0068-new-tab-position-seam.patch:1-62`
- **Observation:** 21/65 patches touch ≥4 files; 39 are seam/registration, 26 behaviour-altering. Hot spots: `about_flags.cc` ×11, `flag-metadata.json` ×11, `flag-never-expire-list.json` ×10, `chrome/browser/ui/tabs/BUILD.gn` ×9, `chrome_command_ids.h` ×5. Patch 0056 edits `base/task/thread_pool/{job_task_source.h,sequence.h,task_tracker.h}` + `base/test/task_environment.h` (test-only accessors; a `//base` header change costs a ~10.5 h cold rebuild). Apply order = filename order; dependencies are prose only ("ORDERING DEPENDENCY after 0066"). Per-pin obligations are scattered across patches, ADR 0002, `check_override_staleness`, and 0010's "re-check on uprev" comment; there is no ordered checklist and no dry-run has ever happened.
- **Evidence:** `0068-new-tab-position-seam.patch: "# UPREV CAVEAT (M150): upstream's #new-tab-adds-to-active-group flags entry expires at M150 …"`
- **Proposed change:** (1) `docs/uprev.md` (or ADR 0004): ordered checklist of every per-pin obligation; dry-run once against M150/M151 in a scratch checkout. (2) Collapse the 10 flag-entry patches into one `00NN-roamux-flags-entries.patch` (3 files, 1 hunk each). (3) Put 0056 behind a test-only GN arg or retire it once roam-195 is closed. (4) Machine-readable `After:` in patch preambles (see M29).
- **Tradeoff:** collapsing flag patches loses per-issue blame inside the patch (keep it in the preamble); the dry-run costs a cold build but is the only way to learn the rebase cost before a CVE forces it.
- **Effort:** playbook + flag collapse `[< 1 week]`; dry-run uprev `[< 1 month]`

#### H3. Rebrand regex has a CJK word-boundary hole — 881 Korean/Chinese strings ship as "Chromium"
- **Source:** edge-cases #1; quantified at synthesis time
- **File:** `roamux/build/rebrand_exclusions.py:53` (`_TOKEN`); release gate `.github/workflows/release.yml:123-129`; fixture `roamux/build/tests/test_rebrand_strings.py:206`
- **Observation:** `_TOKEN = (?<![\w./@:])(?P<w>Chromium|chromium)\b`. Python's Unicode `\b` needs a word→non-word transition; Hangul, ideographs and kana are all `\w`, so `Chromium을` / `Chromium에서는` / `从Chromium中` never match, while `Chromium，` (fullwidth comma, punctuation) does. The release gate greps only the English `chromium_strings.grd` for "Roamux" and never inspects an `.xtb`. **Synthesis-time scan of the pinned checkout (letters-only adjacency, punctuation excluded): 881 hits in 8 files — ko 850 (784 in `chromium_strings_ko.xtb` alone), zh-CN 31; ja/zh-TW/zh-HK are effectively clean because they separate with punctuation/spaces.** Korean particles attach directly to the brand token, so most of the Korean brand surface is affected.
- **Evidence:**
  ```
  shipped regex on 'Chromium을': False   on 'Chromium，': True
  chromium_strings_ko.xtb  id="1040916596585577953">Chromium에서는 이 확장 프로그램을 …
  generated_resources_ko.xtb … 사용하려면 Chromium을 다시 실행한 다음 …
  chromium_strings_zh-CN.xtb id="2847479871509788944">从Chromium中移除…
  ```
- **Proposed change:** replace the trailing `\b` with `(?![A-Za-z0-9_])` (keep the leading guard and `_VETO_AFTER`); add a CJK no-space fixture (`Chromium을`, `从Chromium中`) to `test_rebrand_strings.py`; add a release gate that asserts zero user-visible `Chromium` survivors in `chromium_strings_ko.xtb` and `chromium_strings_zh-CN.xtb` after rebrand.
- **Tradeoff:** widening the boundary must not re-enable `Chromium.Foo`-style identifiers — the leading guard already handles that; verify with the existing exclusion fixtures.
- **Effort:** `[< 1 day]`

#### H4. Re-cutting a tag without a VERSION bump ships a different binary with the same CFBundleVersion — existing users are never offered it
- **Source:** edge-cases #2
- **File:** `.github/workflows/release.yml:81-82,290-339`; `roamux/build/release_version.py:49-52`; `roamux/app/release/rename_bundle.py:52`
- **Observation:** `bundle_version(tag)` is a pure function of the tag string; the re-cut path (roam-124) deletes and recreates the release at the current tag ref; the tag==VERSION gate compares marketing strings only. A hotfix re-cut of `v0.0.1-alpha.9` passes every gate and stamps `0.0.1.1.9` again. Sparkle offers an update only when the feed version is strictly greater, so every user on the first build is stranded on it.
- **Evidence:** `bundle_version` → `f"{core}.{stage}.{n}"`; no artifact diff against the release being deleted.
- **Proposed change:** fail the re-cut step when the tag ref moved but `VERSION` did not; or add a monotonic build counter / short-SHA as a 6th CFBundleVersion component so any re-cut compares greater (keep `CFBundleShortVersionString` unchanged).
- **Tradeoff:** a SHA-suffixed numeric version complicates the roam-156 "compiled string == advertised string" invariant; vary only the numeric encoding.
- **Effort:** `[< 1 week]`

#### H5. No `concurrency:` group — release, tier-2 and nightly all reset the same base, serialized only by the single-runner accident
- **Source:** edge-cases #3
- **File:** `.github/workflows/ci.yml`, `nightly.yml`, `release.yml` (no `concurrency:` key); `release.yml:113-115`; `roamux/build/ci/tier2_job.sh:63-67`
- **Observation:** All three self-hosted jobs `git reset --hard` + `git clean -fd -e /roamux` and re-point `${SRC}/roamux`. They cannot overlap today only because one runner executes one job at a time — an unenforced invariant the runner doc's own roadmap plans to break ("add more runners…"). A second runner, or a duplicate registration, lets a tag release and a `main` tier-2 (or the 18:00 UTC nightly) wipe each other's applied stack and half-built `out/` mid-`autoninja`.
- **Evidence:** `grep -n concurrency .github/workflows/*.yml` → none.
- **Proposed change:** `concurrency: { group: roamux-shared-base, cancel-in-progress: false }` on all three workflows; assert it in `test_workflow_invariants.py`.
- **Tradeoff:** a queued release waits behind a nightly — correct for base integrity.
- **Effort:** `[< 1 day]`

#### H6. `release.yml` re-points the shared overlay symlink and never restores it
- **Source:** error-handling #5
- **File:** `.github/workflows/release.yml:105-106,350-352`; `docs/ci/self-hosted-runner.md:14`
- **Observation:** `ln -sfn "$(pwd)/roamux" "${CHROMIUM_SRC}/roamux"` with no trap and no `always()` restore; the only `always()` step is keychain cleanup; `ROAMUX_CANONICAL_OVERLAY` is never read. After every release, `~/chromium/src/roamux` points at the runner's `_work/…/roamux` — whatever the last job checked out — and the operator's next local build silently compiles it. The runner doc's "restored by an EXIT trap" claim is true only for tier-2. This is a deterministic instance of the symlink-race class that has produced wrong local verdicts before.
- **Evidence:** `release.yml` contains exactly one `ln -sfn`, no `trap`, no `ROAMUX_CANONICAL_OVERLAY`.
- **Proposed change:** final `if: always()` step "Restore canonical overlay" that validates `ROAMUX_CANONICAL_OVERLAY` from `.env` (fail loud if unset, mirroring `tier2_job.sh:36`) and re-links; add the invariant to `test_workflow_invariants.py`.
- **Tradeoff:** none.
- **Effort:** `[< 1 day]`

#### H7. The `ROAMUX_CI_CHROMIUM_RUNNER` kill switch turns the required tier-2 check into a vacuous pass
- **Source:** testing #3
- **File:** `.github/workflows/ci.yml:79-107`; `docs/ci/self-hosted-runner.md:4-6`; `roamux/build/tests/test_workflow_invariants.py:161-163`
- **Observation:** `targeted-suite-selfhosted` (required on `main`) is gated by `if: vars.ROAMUX_CI_CHROMIUM_RUNNER != '' && …`. When the variable is unset or mistyped the job is *skipped*, and branch protection treats an `if:`-skipped job as satisfied. `targeted-suite` prints "SKIPPED" but is contractually "never fails" (asserted by the invariants test). Deleting the variable lets non-fork PRs merge with zero Chromium build/test and nothing red.
- **Evidence:** `self.assertNotIn("exit 1", code, "the announce job never fails")` (invariants :162).
- **Proposed change:** make `targeted-suite` `exit 1` on the `elif [ -z "$CAP" ]` arm for non-fork PRs (fork PRs stay informational), add it to required checks, and flip the invariant to assert exactly that arm fails. Revise the doc's "never red" posture.
- **Tradeoff:** a runner outage blocks merges — which is what "required" means.
- **Effort:** `[< 1 day]`

#### H8. The browser-side Edge import (passwords, cookies, localStorage, IndexedDB) has no production entry point — the picker offers it and it silently does nothing
- **Source:** architecture #1
- **File:** `roamux/browser/importer/roamux_edge_import_driver.h:108-119`, `.cc:117-137`; `roamux/utility/importer/edge_profile_reader.cc:291-298`; `roamux/utility/importer/roamux_edge_importer.cc:17-66`
- **Observation:** `MaybeStartEdgeBrowserSideImport(...)` is documented as "called from the patched `ExternalProcessImporterHost::NotifyImportEnded`" but no patch touches `ExternalProcessImporterHost`/`NotifyImportEnded`; its only callers are unit tests (the driver browsertest constructs `RoamuxEdgeImportDriver` directly). Meanwhile `DetectEdgeSourceProfile` advertises `PASSWORDS | COOKIES` and the utility-process importer handles only HISTORY/FAVORITES/SEARCH_ENGINES/AUTOFILL. Net: the import dialog offers Edge passwords and cookies; ticking them imports nothing; ~2,170 LOC under `roamux/browser/importer/` plus the Keychain decryptor is dead in production and green in tests.
- **Evidence:** `grep -rn 'MaybeStartEdgeBrowserSideImport\|NotifyImportEnded\|ExternalProcessImporterHost' roamux/patches/` → empty. `edge_profile_reader.cc:298: … | user_data_importer::PASSWORDS | user_data_importer::COOKIES;`
- **Proposed change:** (a) land the missing seam — one patch on `external_process_importer_host.cc` masking secrets via `MaskEdgeSecretItemsForUtility` at `StartImportSettings` and calling `MaybeStartEdgeBrowserSideImport` from `NotifyImportEnded`, plus a browsertest driving `ImporterHost` end-to-end; or (b) until then, drop `PASSWORDS | COOKIES` from `services_supported`. Fix the header comment either way.
- **Tradeoff:** (a) adds a patch on a file upstream churns; (b) is honest but ships less than roam-16..20 claim. The status quo is worse than both.
- **Effort:** (b) `[< 1 day]`; (a) `[< 1 week]`

#### H9. The Edge import report is built with a full degraded/blocked taxonomy and then thrown away; `version_supported` gates nothing
- **Source:** error-handling #4
- **File:** `roamux/browser/importer/roamux_edge_import_driver.cc:130-133`; `roamux/browser/importer/edge_import_report.h:19-27,64-66`; `roamux_edge_import_coordinator.cc:72-73`
- **Observation:** The coordinator records `kDegraded` ("Edge keychain unavailable — secrets not imported"), `kBlocked` (Edge running), `version_supported=false`; `EdgeImportReport::any_degraded()` exists so "the user should be told". The only production consumer binds a lambda that drops the report — no log, no UI, no pref. `version_supported` never gates any stage, so an unsupported Edge milestone proceeds with schema-drift soft-fails that read as "empty".
- **Evidence:** `[](std::unique_ptr<RoamuxEdgeImportDriver> owned, base::OnceClosure done, EdgeImportReport report) { std::move(done).Run(); }`
- **Proposed change:** pass the report through (`OnceCallback<void(EdgeImportReport)>`); `LOG(WARNING)` each non-clean `CarrierOutcome`; surface `any_degraded()` in the import-complete UI; decide whether `!version_supported` blocks SQLite carriers or annotates every carrier `kDegraded`.
- **Tradeoff:** UI surfacing needs a string and a patch hunk; logging is a one-liner.
- **Effort:** logging `[< 1 day]`; UI `[< 1 week]`

#### H10. Sparkle `startUpdater:` failure is ignored — scheduled checks and "Check for updates" silently do nothing
- **Source:** error-handling #1
- **File:** `roamux/browser/updates/roamux_update_service.mm:198-203`; `roamux_version_updater.cc:45-49`
- **Observation:** The BOOL result and out-`NSError` are discarded; Sparkle documents this as *the* misconfiguration signal (bad `SUFeedURL`, missing/invalid `SUPublicEDKey`). On failure a dead updater gets `automaticallyChecksForUpdates = YES`, no event reaches the state machine, and the About row stays `kIdle`, which `MapSnapshot` renders as nothing.
- **Evidence:** `NSError* error = nil; [updater_ startUpdater:&error]; updater_.automaticallyChecksForUpdates = YES;`
- **Proposed change:** capture the BOOL; on NO broadcast `UpdateEvent{kError, description+domain+code}` through `NotifySinks` (About row → FAILED, `offer_retry=false`), `LOG(ERROR)` the NSError; add a test injecting a start failure (bundle without `SUPublicEDKey`).
- **Tradeoff:** needs a seam to fake `startUpdater:`; the ObjC owner is currently untestable in isolation.
- **Effort:** `[< 1 day]`

#### H11. A scheduled (non-user-initiated) check that finds an update is dropped by the state machine; its reply block leaks; later clicks become no-ops
- **Source:** error-handling #2, edge-cases #6
- **File:** `roamux/browser/updates/update_state_machine.cc:36-40`; `roamux_update_service.mm:84-98,145-148,203`; pinned by `roamux/test/roamux_update_state_machine_unittest.cc:93`
- **Observation:** Sparkle calls `showUserInitiatedUpdateCheckWithCancellation:` only for user-initiated checks; a scheduled check (enabled: `automaticallyChecksForUpdates = YES`, 86400 s) goes straight to `showUpdateFoundWithAppcastItem:` with the machine still `kIdle`, so `if (status != kChecking) break;` discards it. The stored `foundReply` is never invoked, Sparkle's session stays open, and a later manual check hits `showUpdateInFocus` — an empty method. The behaviour is **pinned by a unit test**, so it will not regress by accident but also encodes the bug.
- **Evidence:** `case UpdateEventType::kUpdateFound: if (snapshot_.status != UpdateStatus::kChecking) { break; }`
- **Proposed change:** either synthesize `kCheckStarted` in the driver when `!state.userInitiated`, or accept `kUpdateFound` from `kIdle`/`kUpToDate`/`kError` (keeping the `skipped_version_` guard); re-broadcast the snapshot from `showUpdateInFocus`; update the unit test at :93; add a browsertest driving `showUpdateFoundWithAppcastItem:` with `userInitiated=NO`.
- **Tradeoff:** (b) loosens the "only a check surfaces an update" invariant; (a) couples the driver to Sparkle's call order.
- **Effort:** `[< 1 day]`

#### H12. Production C++/ObjC++ has zero logging, zero histograms, zero crash keys, zero `[[nodiscard]]`
- **Source:** error-handling #3
- **File:** repo-wide; representative: `roamux/browser/importer/roamux_secret_import_stage.mm:53-70`, `edge_local_storage_reader.cc:60-77,122-124`, `roamux/utility/importer/edge_profile_reader.cc:312-319,356-366` (includes `base/logging.h`, never uses it), `roamux/browser/tab_visit/visits_store.cc:169-190,230-260`, `roamux/browser/tabs/initial_url_refresh_run.cc:139-143`
- **Observation:** `grep -rn -E "LOG\(|VLOG\(|DLOG\(|UmaHistogram"` over production sources returns nothing. Every I/O or parse failure is a silent early return; `--enable-logging=stderr --v=1` yields nothing Roamux-specific. "Feature disabled", "source absent", "source unreadable" and "wrote nothing" are indistinguishable from a user's machine — the exact shape of the project's "still broken" triage incidents.
- **Evidence:** grep result above; `edge_profile_reader.cc:15` includes logging and never calls it.
- **Proposed change:** minimum bar — `DVLOG(1)` at every I/O/parse `return false/0/nullopt` with path/tag/tab-uid context (never secrets); `LOG(ERROR)` for updater start failure, journal razed, IndexedDB publish rollback; `[[nodiscard]]` on `VisitsStore::Open/OpenInMemory/InitSchema` and the stage `Run/Import` bools; one `CrashKeyString` for update state.
- **Tradeoff:** dev-build noise; pair with a "no PII/secrets in logs" review of the importer paths.
- **Effort:** `[< 1 week]`

#### H13. The dominant integration mechanism is an undeclared fourth channel: 49 files (~5k LOC, ~42% of production) compiled into upstream targets via `sources +=`, owned by no `//roamux` target
- **Source:** architecture #2
- **File:** `roamux/patches/0009-tab-uid-hooks.patch:126-131` (and 0010, 0011, 0012, 0015, 0016, 0018, 0019, 0020, 0024, 0033, 0047, 0053, 0054, 0065); `docs/adr/0001-chromium-overlay-strategy.md:12-16`; `scripts/checks/overlay_structure.py:1-20`
- **Observation:** ADR 0001 names three channels. In practice 49 `.cc/.h/.mm` under `roamux/browser/{tab_visit,tabs,ui,bookmarks,updates}` and `roamux/utility/importer` appear in no `roamux/**/BUILD.gn`; they are listed by 15 patches into `//chrome/browser/ui`, `//chrome/browser`, `//chrome/utility`, etc. The pattern is sound for GN acyclicity, but governance cannot see it: nothing asserts every roamux source has exactly one owner; `gn check` visibility is the host target's; the dependency closure belongs to upstream. It also produced the ODR hazard in M27.
- **Evidence:** `for f in roamux/**/*.{cc,h,mm}; do grep -rq "\"$(basename $f)\"" --include=BUILD.gn roamux/ || echo $f; done | wc -l` → 49.
- **Proposed change:** amend ADR 0001 (or ADR 0004) declaring "compiled-into-upstream sources" as a governed channel with its rule; add a hermetic test that every `roamux/**/*.{cc,mm}` is referenced by exactly one of {a `//roamux` BUILD.gn, a patch `sources +=`}; optionally a `roamux_upstream_sources.gni` template so each patch is a one-line hunk.
- **Tradeoff:** the template adds one more overlay mechanism; ADR + test alone are cheap and sufficient.
- **Effort:** `[< 1 week]`

#### H14. tier-2's `--gtest_filter="Roamux*"` silently drops the only three-carrier import survival test
- **Source:** testing #1
- **File:** `roamux/build/ci/tier2_job.sh:129`; `roamux/test/roamux_three_carrier_survival_browsertest.cc:66,91-92`
- **Observation:** Every fixture in the target is `Roamux*` except `ThreeCarrierTest`, whose single test `CookieLocalStorageIndexedDbAllSurvive` is the end-to-end proof that cookie + localStorage + IndexedDB for one origin survive a combined Edge import (roam-16/17/18). It has not run in CI since at least 2026-07-12.
- **Evidence:** `"${OUT}/roamux_browsertests" --gtest_filter="Roamux*" …` vs `IN_PROC_BROWSER_TEST_F(ThreeCarrierTest, …)`.
- **Proposed change:** rename to `RoamuxThreeCarrierTest` and drop the filter (the target already contains only roamux suites); add a hermetic test that every `IN_PROC_BROWSER_TEST_F(<Fixture>` in `roamux/test/` matches whatever filter `tier2_job.sh` passes (or that no filter exists).
- **Tradeoff:** none.
- **Effort:** `[< 1 day]`

#### H15. ~1,040 lines of tests compile only into targets no workflow builds
- **Source:** testing #2, architecture #8
- **File:** `roamux/patches/0013-edge-chromium-importer.patch:29,175`; `0014-edge-secrets-keychain-oscrypt.patch:117-150`; `0033-settings-about-roamux.patch:508-528`; patches 0061/0062; `roamux/BUILD.gn:34-58`; `roamux/build/ci/tier2_job.sh:110`
- **Observation:** tier-2/nightly build exactly `roamux_unittests roamux_browser_unittests roamux_browsertests`. Wired elsewhere and never run: `roamux_edge_bookmarks_integration_unittest.cc` (93 l) and `roamux_edge_importer_unittest.cc` (90 l) → upstream `unit_tests` (0013); `roamux_secret_import_stage_unittest.mm` (221 l — the Keychain password/cookie stage) → `unit_tests` (0014); `roamux_settings_about_browsertest.cc` + two mocha `.ts` files (230 l) → upstream `browser_tests` (0033); `roamux_sparkle_feed_test.mm` (231 l — the only test driving real Sparkle against the signed/tampered/unsigned fixtures) → `roamux_sparkle_tests`, which is not on the tier-2 build line. 0061/0062 edit upstream unit-test fixtures nobody compiles. All cost rebase effort at every pin bump and can rot silently — ADR 0003's situation, on the Roamux-authored side.
- **Evidence:** `autoninja -C "${OUT}" roamux_unittests roamux_browser_unittests roamux_browsertests` (tier2_job.sh:110); `roamux_sparkle_tests` referenced only in `roamux/BUILD.gn:35,42`.
- **Proposed change:** add `roamux_sparkle_tests` to the tier-2 build+run lines; move the three importer unit tests into `roamux_browser_unittests` (removes two test hunks from the stack); for the mocha suite and 0061/0062, a nightly `autoninja unit_tests browser_tests` + filtered run leg (see M42) or an ADR-0003-style register with a re-entry obligation.
- **Tradeoff:** `browser_tests` is hours cold, minutes warm — keep it nightly.
- **Effort:** `[< 1 week]`

#### H16. The release build configuration is first compiled at tag time; no rehearsal path exists
- **Source:** testing #4
- **File:** `.github/workflows/release.yml:29-34,102-184`; `roamux/build/args/release.gn` vs `reference.gn`
- **Observation:** tier-2 builds three test targets in `out/CI` (component, non-official). Release uses `is_official_build=true`, `is_component_build=false`, builds `chrome` + `copy_signing`, universalizer, `rename_bundle.py`, icon/rpath gates, packaging — none of which runs before the tag; a link/visibility/DCHECK-off breakage surfaces ~10 h into a 1440-minute job. `workflow_dispatch` cannot rehearse because "Assert protected ref" rejects non-`v*` refs.
- **Evidence:** `case "${GITHUB_REF}" in refs/tags/v*) …; *) echo "::error::release ran on a non-release ref"; exit 1`
- **Proposed change:** nightly `release-rehearsal` job (arm64 slice only) running `gn gen` with `release.gn` → `autoninja chrome copy_signing` → `rename_bundle` → both gates → `package_roamux.py` → `sign_roamux.py --mode unsigned`, stopping before any `gh`/appcast leg. Factor the steps into `roamux/build/ci/release_build.sh` shared by nightly and `release.yml`; pin the call in `test_workflow_invariants.py`.
- **Tradeoff:** a second warm out-dir (tens of GB) and nightly hours; arm64-only does not prove x64/universal.
- **Effort:** `[< 1 week]`

#### H17. `--test-launcher-retry-limit=2` masks flakes with no signal, no ledger, no artifacts
- **Source:** testing #5, error-handling #13
- **File:** `roamux/build/ci/tier2_job.sh:110-135`; `.github/workflows/ci.yml:104-107`
- **Observation:** All three suites retry twice; a test that fails twice then passes is a green job. No `--test-launcher-summary-output`, no `upload-artifact` on self-hosted jobs, no known-flakes file; `GITHUB_STEP_SUMMARY` gets one line on success only; `SECONDS` is printed only on the green path, so a 6 h timeout cannot be attributed to build vs suite. The documented flake history (roam-195, FullscreenMatrixIsSane display-sleep, hdiutil EAGAIN) is exactly the discarded signal.
- **Evidence:** `grep -n "summary-output\|upload-artifact" tier2_job.sh ci.yml nightly.yml` → none.
- **Proposed change:** `--test-launcher-summary-output="${RUNNER_TEMP}/<suite>.json"` + `2>&1 | tee` per suite (pipefail is on); `if: always()` `upload-artifact`; `roamux/build/ci/flake_report.py` (hermetically tested) that lists tests with >1 iteration to `$GITHUB_STEP_SUMMARY` and fails on retried tests absent from a checked-in `known_flakes.txt` (`Suite.Test # roam-N`); `phase=<name> elapsed=${SECONDS}s` before each phase; extend `test_tier2_job.py`.
- **Tradeoff:** early noise; the ledger needs pruning discipline.
- **Effort:** `[< 1 day]`

### 2.3 MEDIUM

#### M1. Interrupted `rebrand_strings.py` leaves a truncated grd; the only recovery nukes the whole patch stack
- **Source:** edge-cases #4, error-handling #18 — **File:** `roamux/build/rebrand_strings.py:312-323`
- **Observation:** each grd/xtb write is `f.write_text(new)` (truncate-then-write). A kill/Ctrl-C/disk-full mid-write leaves a partial file that passes the regex-based xtb rewrite and fails later inside GRIT; locally the documented recovery is `git reset --hard`, which reverts every patch → full 65-patch re-apply + re-vendor Sparkle. CI self-heals via reconcile.
- **Evidence:** direct `write_text` on the target path.
- **Proposed change:** write to `f.with_suffix(f.suffix + ".tmp")` then `os.replace`; optionally `ET.fromstring` the xtb before replacing.
- **Tradeoff:** none. — **Effort:** `[< 1 day]`

#### M2. `make_latest=true` on every publish re-points `/latest/` backward when an old tag is re-cut
- **Source:** edge-cases #5 — **File:** `.github/workflows/release.yml:338-339`; `roamux/app/sparkle-Info.plist:12`
- **Observation:** the feed URL is `/releases/latest/download/appcast.xml`; re-publishing an older tag (asset fix) makes `latest` serve that single-item appcast to the whole fleet until the next newer release. Sparkle will not downgrade, but new installs and skipped-version users are offered the older build.
- **Evidence:** unconditional `-f make_latest=true`; single-item `TEMPLATE` in `generate_appcast.py:17-34`.
- **Proposed change:** pass `make_latest=true` only when the tag's `bundle_version` ≥ the current latest release's.
- **Tradeoff:** one extra `gh api` read. — **Effort:** `[< 1 day]`

#### M3. `kError` clobbers any state unconditionally; running from the read-only DMG fails every update with a generic message
- **Source:** edge-cases #7, error-handling #9 (empty driver callbacks) — **File:** `roamux/browser/updates/update_state_machine.cc:73-76`; `roamux_update_service.mm:99-102,108-115,130-133,138-140,145-148`
- **Observation:** unsigned distribution makes "run from the mounted DMG" a real path; Sparkle downloads, fails to swap on a read-only volume, emits `kError`, which overwrites `kChecking`/`kDownloading`/`kReadyToInstall` with no guard. `showInstallingUpdateWithApplicationTerminated:retryTerminatingApplication:`, `showUpdateReleaseNotesFailedToDownloadWithError:`, `dismissUpdateInstallation`, `showUpdateInFocus` are empty, so an app that did not terminate for install stays at "NEARLY_UPDATED" forever and an aborted session freezes the snapshot.
- **Evidence:** five consecutive `{}` method bodies; `case kError:` with no status guard.
- **Proposed change:** detect a non-`/Applications`/read-only install location and show "move to Applications first"; invoke `retry()` once in the install callback and emit an `kInstallFailed` event; emit `kError` from release-notes failure; emit an explicit reset on `dismissUpdateInstallation`; do not let a late `kError` erase `kReadyToInstall`.
- **Tradeoff:** two new events and transitions in the pure machine. — **Effort:** `[< 1 week]`

#### M4. `apply_patches.py` simulates with `git apply` outside any repo — ambient git config can diverge from the base and break the byte-exact prefix match
- **Source:** edge-cases #8 — **File:** `roamux/build/apply_patches.py:80-89,124-129,146`
- **Observation:** the scratch simulation runs `git apply` in a `mkdtemp` dir (no `.git`, no `.gitattributes`), so global `core.autocrlf`/`apply.whitespace` apply; the real application runs inside the base repo. The detector compares raw bytes, so a config difference between the runner and the maintainer yields "tree matches no stack state" with no real conflict — the cross-environment false-red class this project has hit repeatedly.
- **Evidence:** `_run(["git","apply",str(patch)], cwd=scratch)`; `worktree == snapshots[k]` over `read_bytes()`.
- **Proposed change:** `git -c core.autocrlf=false -c core.eol=lf -c apply.whitespace=nowarn apply` in the scratch, and `git init -q` it so attribute semantics match the base.
- **Tradeoff:** slightly heavier scratch setup. — **Effort:** `[< 1 day]`

#### M5. M150 uprev time bomb: patches 0067/0068 mode-1 consult an upstream flag that expires
- **Source:** edge-cases #9 — **File:** `roamux/patches/0068-new-tab-position-seam.patch:48-51`; `roamux/browser/tabs/new_tab_placement.cc:19-22`
- **Observation:** the patch header says so: "UPREV CAVEAT (M150): upstream's `#new-tab-adds-to-active-group` flags entry expires at M150 and mode 1 consults that feature." When upstream removes it, either a build break (caught) or — if a stub remains — a silent default-behaviour change for the shipped `kEndOfActiveGroup` default.
- **Evidence:** as quoted.
- **Proposed change:** a roam-N tied to the M150 pin bump collapsing 0067 + the enum into a Roamux-owned switch; a pinned test that fails at the uprev.
- **Tradeoff:** deferred design at the pin bump. — **Effort:** `[< 1 week]` (at uprev)

#### M6. Importer copies whole Edge LevelDB/IndexedDB stores to temp with no size cap
- **Source:** edge-cases #10 (+ security #11 uncapped `Bookmarks` read) — **File:** `roamux/browser/importer/edge_local_storage_reader.cc:59-67`; `roamux_indexed_db_import_stage.cc:100-107`; `roamux/utility/importer/edge_profile_reader.cc:356`
- **Observation:** `Local State` is capped at 1 MiB because it is untrusted input; the `CopyDirectory` of `Local Storage/leveldb`, the IndexedDB copy, and `ReadBookmarks` have no cap. A multi-GB source fills the destination filesystem mid-import.
- **Evidence:** `base::CopyDirectory(source, copy, /*recursive=*/true)` with no size check.
- **Proposed change:** compute aggregate size first and report `kDegraded` "source too large" above a cap; `ReadFileToStringWithMaxSize` (64 MiB) for `Bookmarks`.
- **Tradeoff:** a legitimately huge store will not import. — **Effort:** `[< 1 day]`

#### M7. Secret import runs SQLite + Keychain decrypt synchronously on the UI thread
- **Source:** edge-cases #11 — **File:** `roamux/browser/importer/roamux_edge_import_coordinator.cc:112-121`; `roamux_secret_import_stage.mm:39-104`
- **Observation:** the coordinator documents "blocking work on this UI thread", deferring the offload to roam-20. Thousands of passwords freeze the browser during first-run import, and a macOS keychain prompt resolves on the frozen thread.
- **Evidence:** the F3 note at :112-121.
- **Proposed change:** run the stage on a `MayBlock` pool like the localStorage/IndexedDB stages; reply to UI only for `ProfileWriter` writes.
- **Tradeoff:** async plumbing. — **Effort:** `[< 1 week]`

#### M8. `SparkleOwner::GetOrCreate()` lazy init is unguarded — a first-call race recreates the roam-140 two-owner hang
- **Source:** edge-cases #12 — **File:** `roamux/browser/updates/roamux_update_service.mm:220-224`
- **Observation:** `if (!g_process_owner) g_process_owner = new SparkleOwner();` with no `NoDestructor`/once/sequence check; created both by `InitSparkleUpdater` and lazily by the first per-profile service.
- **Evidence:** bare `SparkleOwner* g_process_owner = nullptr;`
- **Proposed change:** `base::NoDestructor<SparkleOwner>` or a `SequenceChecker` + main-thread `CHECK`.
- **Tradeoff:** none. — **Effort:** `[< 1 day]`

#### M9. tier-2 EXIT trap does not survive cancel/SIGKILL, no `timeout-minutes`, and the restore can misfire
- **Source:** error-handling #6, edge-cases #13 — **File:** `roamux/build/ci/tier2_job.sh:20-23,41-50`; `.github/workflows/ci.yml:97-107`; `nightly.yml`
- **Observation:** the script `exec`s under `caffeinate -sim`; a cancel or the silent 6 h default job timeout ends in a process-tree SIGKILL no trap survives — the symlink stays at the (soon-reclaimed) workspace. If `${SRC}/roamux` were ever a real directory, `ln -sfn` creates `dir/roamux` inside it and still prints "restored" (verified). If `ln` fails in the trap, the job's exit status becomes `ln`'s with a message naming nothing.
- **Evidence:** `restore_overlay() { ln -sfn "${ROAMUX_CANONICAL_OVERLAY}" "${SRC}/roamux"; echo "overlay symlink restored…"; }`
- **Proposed change:** explicit `timeout-minutes` on both self-hosted jobs (with the cold-build number in a comment); make the first action of `tier2_job.sh` and `release.yml` a "reconcile symlink" that asserts `-L "${SRC}/roamux"` and re-points it, so recovery never depends on the previous job's trap; test `-L` before `ln` in the trap and emit `::warning::` with the previous target on failure.
- **Tradeoff:** a hard timeout fails cold rebuilds until the warm-dir strategy is addressed — honest vs a silent 6 h cancel. — **Effort:** `[< 1 day]`

#### M10. `VisitsStore::Open` razes the journal on *any* open/schema failure; the second-open result is discarded
- **Source:** error-handling #7 — **File:** `roamux/browser/tab_visit/visits_store.cc:42-52`; `settled_visit_journal_service.cc:29-36`
- **Observation:** lock contention, EMFILE, disk-full, or a transient `Transaction::Begin` failure inside `InitSchema` all reach `base::DeleteFile(db_path)` — destroying the durable tab-uid MRU the feature exists for. The retry's bool is dropped via `.Then(BindOnce([](bool){}))`; no `set_error_callback`, so mid-session corruption is neither logged nor recovered.
- **Evidence:** `db_.Close(); base::DeleteFile(db_path); return db_.Open(db_path) && InitSchema();`
- **Proposed change:** raze only on `sql::IsErrorCatastrophic` / `GetCompatibleVersionNumber() > kCurrentVersion`; otherwise run degraded in-memory; install an error callback; log the bool and expose `open_failed_` for tests.
- **Tradeoff:** a non-catastrophic-flagged corrupt DB persists until next launch. — **Effort:** `[< 1 day]`

#### M11. Update errors are classified by substring-matching Sparkle's *localized* description; domain/code are thrown away
- **Source:** error-handling #8 — **File:** `roamux/browser/updates/roamux_version_updater.cc:89-105`; `roamux_update_service.mm:108-115`
- **Observation:** `ClassifyUpdateError` looks for "sign"/"validat"/"verif"/"install"/"download" in `localizedDescription`; on a non-English macOS a signature failure (`SUSignatureError 3001`) renders as "Couldn't check… try again" with `offer_retry=true` — the security-relevant class is locale-dependent.
- **Evidence:** `if (lower.find("sign") != npos || lower.find("validat") …`
- **Proposed change:** add `code`/`domain` to `UpdateEvent`; classify on `SUSparkleErrorDomain` codes (3001/3002 signature; 2000/2001 download; 4000-4012 install); keep the string heuristic as fallback; log domain+code.
- **Tradeoff:** couples to Sparkle enum values — stable, vendored, SHA-pinned. — **Effort:** `[< 1 day]`

#### M12. Conventional-Commits CI gate is fail-open when `git rev-list` fails
- **Source:** error-handling #10 — **File:** `.github/workflows/ci.yml:69-77`
- **Observation:** `for sha in $(git rev-list …)` — a failing command substitution in a `for` word list does not trigger errexit (verified); empty/unreachable `BASE`/`HEAD_SHA` → zero iterations → green with no output.
- **Evidence:** the `[ -n "$BASE" ]` guard exists only in the previous step (:51).
- **Proposed change:** `shas="$(git rev-list …)" || { echo "::error::rev-list failed"; exit 1; }`; `[ -n "$shas" ] || exit 1`; `shell: bash` on the job.
- **Tradeoff:** a legitimately empty range now fails — the honest outcome. — **Effort:** `[< 1 day]`

#### M13. `cp -Rc out/Default out/CI` is not restart-safe
- **Source:** error-handling #11 — **File:** `roamux/build/ci/tier2_job.sh:105-108`
- **Observation:** guard is `[ ! -d "${OUT}" ]`; a clone killed by cancel/timeout/sleep leaves a partial `out/CI` the guard treats as complete → confusing failure or a from-scratch rebuild that looks warm.
- **Evidence:** no stamp, no `.partial`, no `build.ninja` check.
- **Proposed change:** clone to `${OUT}.partial`, `mv` atomically; guard on `${OUT}/build.ninja` + `args.gn`; log clone duration.
- **Tradeoff:** none. — **Effort:** `[< 1 day]`

#### M14. `fetch_sparkle.py`: unbounded download, no retry, idempotence not pin-aware
- **Source:** error-handling #12, edge-cases #19 — **File:** `roamux/build/fetch_sparkle.py:57-61,70,92-106`
- **Observation:** `urlretrieve` has no timeout; the no-op check is "framework dir exists", so a bumped `SPARKLE_VERSION`/`SHA256` with an old framework vendored prints `[ok]` and builds stale; an interruption between the framework move and the `bin/` move leaves `sign_update` absent while the check passes — the release fails hours later.
- **Evidence:** `if framework.is_dir() and not args.force: print("[ok] Sparkle.framework already present …")`
- **Proposed change:** `VENDORED_VERSION` stamp (version+sha) written after `bin/`; `urlopen(timeout=60)` with 3 bounded retries; check `bin/sign_update` in `check_sparkle_vendored.py`.
- **Tradeoff:** one extra gitignored file. — **Effort:** `[< 1 day]`

#### M15. Importer collapses distinct failure causes into "empty or unreadable"; `kFailed` is never assigned
- **Source:** error-handling #14 — **File:** `roamux/browser/importer/roamux_secret_import_stage.mm:83-85,94-96,139-148`; `roamux_edge_import_coordinator.cc:152-158`
- **Observation:** invalid URL/realm rows, undecryptable blobs and cookie prefix mismatches are `continue`d with no counter; 300 passwords all failing `DecryptV10` reads identically to an empty `Login Data`.
- **Evidence:** `grep -n kFailed roamux/browser/importer/*` → enum + name only.
- **Proposed change:** `skipped_undecryptable`/`skipped_invalid` counters; `imported == 0 && skipped_undecryptable > 0` → `kFailed` "N entries could not be decrypted"; separate copy/open failure (`kFailed`) from missing table (`kUnsupported`) from 0 rows (`kSkipped`).
- **Tradeoff:** none. — **Effort:** `[< 1 day]`

#### M16. Release partial-failure states: no publish-only path; artifacts not uploaded on failure
- **Source:** error-handling #15 — **File:** `.github/workflows/release.yml:330-348`
- **Observation:** a transient API failure at the publish PATCH leaves a fully-validated draft that a re-run deletes and rebuilds (hours, universal2); `Upload artifacts` is not `if: always()`, so when `verify_appcast.py` or a gate fails the DMG/zip/appcast that explain it stay on the runner's disk.
- **Evidence:** no retry on the PATCH; no `if:` on upload.
- **Proposed change:** `if: always() && env.ROAMUX_APP != ''` on upload; 3-attempt PATCH loop; `workflow_dispatch` input `publish_only=<release_id>` behind the same environment and staging validation.
- **Tradeoff:** a new path in a security-sensitive workflow. — **Effort:** `[< 1 day]`

#### M17. Sparkle hardening keys absent and `delegate:nil`: archives extracted before signature check, no feed/host pinning, appcast unsigned, downgrade check unverified
- **Source:** security #4 — **File:** `roamux/app/sparkle-Info.plist:10-19`; `roamux_update_service.mm:188-204`; `roamux/app/appcast/generate_appcast.py:17-34`
- **Observation:** `SUVerifyUpdateBeforeExtraction` unset (Sparkle 2 default NO → zip/dmg parsers run on unauthenticated bytes); no `SPUUpdaterDelegate`, so no hard feed-URL pin (a user-defaults `SUFeedURL` override is believed honoured — unverified) and no enclosure-host allowlist; only the enclosure is signed, the appcast XML is trusted from TLS+GitHub; whether 2.9.4 cross-checks `CFBundleVersion` against `sparkle:version` (anti-downgrade) could not be confirmed from the repo, and Roamux does no comparison of its own.
- **Evidence:** `initWithHostBundle:… userDriver:driver_ delegate:nil`; plist lacks the key.
- **Proposed change:** add `SUVerifyUpdateBeforeExtraction=true` and assert it in `check_sparkle_bundle.py`; a ~40-line delegate returning the feed URL from a constant and rejecting items whose enclosure is not under `github.com/xinquan568/roamux/releases/download/` or whose `sparkle:version` ≤ running `CFBundleVersion` (`SUStandardVersionComparator`); optionally publish `appcast.xml.sig` and verify with the existing `VerifyAppcastSignature`; confirm the downgrade check in vendored source and record it in an ADR.
- **Tradeoff:** the extraction key requires every item to carry an EdDSA signature — already true. — **Effort:** plist `[< 1 day]`; delegate `[< 1 week]`

#### M18. Initial-URL accepts `javascript:`/`data:`/`file:` and replays it in whatever origin the tab is on, persisted across restarts
- **Source:** security #5 — **File:** `roamux/browser/ui/tabs/edit_initial_url_dialog.cc:25-37`; `roamux/browser/tabs/reload_initial_url_command.cc:50-61`; `tab_initial_url_helper.cc:76-89,169-186`
- **Observation:** `CommitEdit` accepts any `GURL::is_valid()`; the reload command and the Refresh-all chord replay it via `LoadURLWithParams(PAGE_TRANSITION_TYPED)`, which for `javascript:` executes in the tab's *current* document; the value is persisted in session extra-data and re-armed by `SetPendingRestoredInitialUrl` after restart. Classic persisted self-XSS: "paste this into Edit initial URL" → later chord press while the tab is on a bank.
- **Evidence:** `GURL url(text); if (!url.is_valid()) return false; … helper->SetUserInitialUrl(url);`
- **Proposed change:** `url_formatter::FixupURL` then an allowlist of `http`/`https`/`chrome` (and `file` only deliberately); apply the same allowlist in `DecodeExtraData`; unit tests per rejected scheme.
- **Tradeoff:** bookmarklets cannot be initial URLs — never the feature's purpose. — **Effort:** `[< 1 day]`

#### M19. Unsigned distribution consequences (no runtime integrity, no Safe Browsing) are documented only in an entitlements comment for a build that does not ship
- **Source:** security #6 — **File:** `.github/workflows/release.yml:186-229`; `roamux/app/release/entitlements/roamux.entitlements:1-16`; `roamux/build/args/release.gn:8`
- **Observation:** unsigned mode runs zero codesign: no hardened runtime, `DYLD_INSERT_LIBRARIES` works, `Sparkle.framework`/helpers can be rewritten by any same-user process, TCC grants re-prompt after every update; `is_chrome_branded=false` with no API key means Safe Browsing lookups have no key (verify with a network probe on a packaged build). The "Library validation stays ON" comment describes the signed build only.
- **Evidence:** `sign_roamux.py --mode unsigned` prints and exits.
- **Proposed change:** state both consequences in README/About; make #90 (Developer ID + notarization) the exit criterion for leaving alpha; obtain an API key project for Safe Browsing via `google_keys.gni` without `roamux_signin_authorized_build`; fix the entitlements comment.
- **Tradeoff:** paid Apple account; a Google dependency the project has avoided. — **Effort:** docs `[< 1 day]`; signing / API key `[< 1 week]` each

#### M20. `secret_scan.py` cannot detect the secret classes this repo actually handles
- **Source:** security #7 — **File:** `scripts/checks/secret_scan.py:11-19`; `.gitignore:1-12`
- **Observation:** patterns cover PEM, `AKIA`, `xox`, and a quoted `secret|token|password|api_key = "…"` shape; misses a bare 88-char base64 Sparkle private key, `.p12`/`.p8` binaries (no filename rule), GitHub `ghp_`/`github_pat_`/`gho_`/`ghs_`, and `.env` (the runner contract file). History sweep for these classes is clean today.
- **Evidence:** the generic-secret regex requires a quoted value.
- **Proposed change:** add `\b(ghp|gho|ghs|ghu|github_pat)_[A-Za-z0-9_]{20,}`, `SPARKLE[_A-Z]*KEY\s*[:=]\s*\S{40,}`, a high-entropy `[A-Za-z0-9+/]{86,88}={0,2}` near `key|priv|seed`; filename rules for `*.p12 *.p8 *.pem *.key *.keychain-db .env .env.*`; mirror into `.gitignore`.
- **Tradeoff:** small false-positive rate handled by `roamux:allow-secret`. — **Effort:** `[< 1 day]`

#### M21. Runner tarball fetched with no checksum
- **Source:** security #8 — **File:** `roamux/build/ci/provision_runner.sh:18-20,39-44`
- **Observation:** `curl -sSfL … && tar xzf` of `actions-runner-osx-arm64-2.321.0.tar.gz` with no SHA-256, although GitHub publishes it and `fetch_sparkle.py` already does this correctly. This binary executes every CI and release job.
- **Evidence:** as quoted.
- **Proposed change:** `RUNNER_SHA256` beside `RUNNER_VERSION`, `shasum -a 256 -c` before `tar`; assert in `test_provision_runner.py`.
- **Tradeoff:** two-line bumps. — **Effort:** `[< 1 day]`

#### M22. Upstream constants hand-mirrored in three places; `IDC_RELOAD_INITIAL_URL` sits at 34059 with zero headroom
- **Source:** architecture #4 — **File:** `roamux/browser/tabs/shortcut_registry.cc:13-20`; `shortcut_registry_mac.mm:28-30`; `roamux/patches/0010-reload-initial-url-command.patch:8-10`; `roamux/common/roamux_prefs.h:39-45`; `roamux/browser/profiles/external_open_profile.cc:15-22`
- **Observation:** five command ids are literals in `shortcut_registry.cc`, two again in `shortcut_registry_mac.mm` (which already includes chrome headers), and defined a third time in patches; adding a command is a five-site edit. 0010 placed `IDC_RELOAD_INITIAL_URL` at 34059, directly after upstream's vertical-tabs block (34056-34058) and before `IDC_COPY_URL 34060` — the next upstream vertical-tabs command collides. `"vertical_tabs.enabled"`, `"profile-directory"`, `"Guest Profile"`, `"System Profile"` are string mirrors with no test pinning literal↔upstream agreement (the scheme literal in 0028 has one — the right idiom).
- **Evidence:** `shortcut_registry.cc:14 constexpr int kIdcReloadInitialUrl = 34059;`
- **Proposed change:** depend on `//chrome/app:command_ids` from `//roamux/browser/tabs` (or a single `roamux_command_ids.h` in `common` the patches include); renumber 34059 into the 33014+ block (pref-free, no migration); a `RoamuxMirroredConstantsTest` in `roamux_browser_unittests`.
- **Tradeoff:** loosens the "no `//chrome`" purity of the tabs target slightly. — **Effort:** `[< 1 day]`

#### M23. Feature-flag checks are scattered (56 sites), flags gate each other, three comments/test names describe the pre-graduation world, flag-OFF coverage has gaps
- **Source:** architecture #5, testing #20 — **File:** `roamux/common/roamux_features.h:8-10`; `roamux/test/roamux_features_unittest.cc:2-4`; `roamux/test/roamux_smoke_unittest.cc:18`; `roamux/browser/tabs/tab_uid_tab_helper.cc:61-62,118-119`; `roamux/BUILD.gn:193-194`
- **Observation:** all 11 flags are live kill switches with `chrome://flags` entries (good), but checks are 32 direct `IsEnabled` calls in the overlay + 24 inside patches; `kTabStripPosition` alone at 14 sites and, via patch 0060, inside upstream's `tabs::IsVerticalTabsFeatureEnabled()` — load-bearing far beyond the overlay. Flag-OFF explicit coverage: `kTabStripToggleShortcut` 0, `kRefreshAllInitialUrls` 1, `kNewTabPosition` 1. `features.h` says "ships DISABLED"; the smoke test is named `FeatureFlagsDefaultDisabled` while asserting `EXPECT_TRUE` ×9; BUILD.gn says the tier-2 filter is `RoamuxTabStripPosition*` (it is `Roamux*`).
- **Evidence:** grep counts above.
- **Proposed change:** one `bool Is<Feature>Enabled()` accessor per flag (+ composites like `IsDurableTabUidEnabled()`), route overlay and patch sites through them; fix the three stale texts; rename the smoke test `FeatureFlagsShipEnabled`; add a flag-OFF browsertest for `kTabStripToggleShortcut`.
- **Tradeoff:** one-time re-context of ~12 patches. — **Effort:** `[< 1 week]`

#### M24. `chromium_src` channel: a tree-wide include-path edit plus a permanent staleness gate for one inert sample header; patch 0003 "sample marker" ships in the stack
- **Source:** architecture #6 — **File:** `roamux/patches/0002-chromium-src-include-redirect.patch`; `0003-sample-marker.patch`; `roamux/chromium_src/README.md:16-21`; `roamux/build/override_signatures.json`
- **Observation:** 0002 prepends `//roamux/chromium_src` to `default_include_dirs` for every TU; the only override is a byte-identical copy of `chrome_isolated_world_ids.h` plus an inert `#define` consumed only by `roamux_overlay_mechanism_unittest.cc`; `chromium_src/README.md` records the mechanism cannot replace a `.cc` and every real need went to a patch. 0003 adds `#define ROAMUX_SAMPLE_PATCH_ACTIVE 1` to `chrome_constants.h` (hundreds of TUs) to prove the runhook works — `test_apply_patches.py` already proves that.
- **Evidence:** `override_signatures.json` has exactly one entry.
- **Proposed change:** retire the channel (delete 0002, 0003, the override, `override_signatures.json`, `check_override_staleness.py` + its test, the mechanism unittest, tier-2 step :132) and re-introduce with an ADR when a real header override is needed; or keep it and move 0003's marker into the override header.
- **Tradeoff:** loses a one-day-to-re-add escape hatch. — **Effort:** `[< 1 day]`

#### M25. `roamux_version_updater.cc` compiles into two targets that link into one executable (ODR), papered over with `// nogncheck`
- **Source:** architecture #7 — **File:** `roamux/BUILD.gn:150-162`; `roamux/patches/0033-settings-about-roamux.patch:386-392`; `roamux/browser/updates/roamux_version_updater.h:9`
- **Observation:** under `roamux_enable_sparkle=true`, `roamux_browser_unittests` lists the file in its own `sources` *and* depends on `//chrome/browser/ui`, into which 0033 compiles the same file; two object files define `RoamuxVersionUpdater`, `MapSnapshot`, `ClassifyUpdateError` — links by link-order accident.
- **Evidence:** both `sources +=` lines cited.
- **Proposed change:** a sparkle-gated `source_set("version_updater")` in `roamux/browser/updates/BUILD.gn` with an explicit dep (or `allow_circular_includes_from`), depended on by both; drop the `nogncheck`. The H13 ownership test flags this class automatically.
- **Tradeoff:** may need the circular-includes allowance. — **Effort:** `[< 1 day]`

#### M26. ADR fidelity: ADR 0001 forbids the in-place edits `rebrand_strings.py` performs, is silent on `sources +=`, cites a "58-file stack"; four significant decisions have no ADR
- **Source:** architecture #9 — **File:** `docs/adr/0001-chromium-overlay-strategy.md:12-16`; `docs/adr/0003-*.md:137-138`; `roamux/build/rebrand_strings.py:1-40`; `roamux/common/roamux_prefs.h:39-45`
- **Observation:** ADR 0001: "never edited in place" — the rebrand channel rewrites 5 grd units + 80 locales in place. Undocumented decisions meeting the ADR README's own bar: all flags default-ON with flags-entry-as-kill-switch (roam-226), Roamux now *writing* upstream's `vertical_tabs.enabled` pref ("maintainer-authorized 2026-07-20"), test-only `//base` instrumentation (roam-201/0056), rebrand-in-place (roam-132).
- **Evidence:** quotes at the cited lines.
- **Proposed change:** supersede ADR 0001 enumerating all four channels + the reserved `chromium_src` status; short ADRs for roam-226/182/201/132; replace "58-file" with a reference to the inventory.
- **Tradeoff:** documentation only. — **Effort:** `[< 1 week]`

#### M27. Patch inventory is a 64 KB hand-written README-as-changelog; only 6/65 patches self-describe; no machine-readable ordering; retired numbers 0027/0031 unexplained
- **Source:** architecture #10 — **File:** `roamux/patches/README.md:20-94`; `roamux/build/tests/test_patch_inventory.py:14-19`; `roamux/patches/0068-*.patch:1-62`
- **Observation:** the most-churned file in the repo (66 commits) because every feature PR edits a multi-paragraph row; the test asserts only filename presence; 0065-0070 carry a `#` preamble (issue, kind, hunks, ordering, coverage) — the right place — 59 do not; no retirement policy.
- **Evidence:** `for p in roamux/patches/*.patch; do head -1 "$p" | grep -q '^#' && echo x; done | wc -l` → 6.
- **Proposed change:** required preamble fields (`Issue:`, `Kind:`, `Files:`, `After:`, `Coverage:`) enforced by `test_patch_inventory.py`; generate the README table from preambles; permutation test that every patch applies with only its `After:` predecessors; a `RETIRED` list with a never-reuse rule (also closes edge-cases #18, see M36).
- **Tradeoff:** one-time pass writing 59 preambles from existing rows. — **Effort:** `[< 1 week]`

#### M28. Per-window state kept in four process-global `NoDestructor<std::map<Browser*, …>>` registries keyed by raw pointer
- **Source:** architecture #11 — **File:** `roamux/browser/ui/views/tabs/tab_strip_toggle_command.cc:19-30`; `tab_strip_pin_controller_views.cc:31-37`; `roamux/browser/tabs/refresh_all_initial_urls_command.cc:22-28`; `roamux/browser/tab_visit/settled_visit_clear_hook.cc:11-16`; `roamux/browser/signin/signin_inert.cc:9-13`
- **Observation:** the "roam-214 house pattern" depends on every owner erasing in its destructor and is exposed to address reuse; the overlay already uses `BrowserWindowFeatures` members (0020, 0054) for the same need. `signin_inert.cc` compiles three `g_*_for_testing` booleans into production.
- **Evidence:** `std::map<BrowserWindowInterface*, base::RepeatingClosure>& Handlers()`.
- **Proposed change:** hang the toggle hooks and refresh run off `BrowserWindowFeatures`/`BrowserUserData`; keep only `ClearJournalHook` and `SparkleOwner` as globals and say so once; move signin test state to a test support TU.
- **Tradeoff:** one more hunk in a file 0020/0054 already patch. — **Effort:** `[< 1 week]`

#### M29. Three realities for `roamux_enable_sparkle`: default false, `reference.gn` silent, CI `out/Default` true by operator hand, `release.gn` true — and the arg changes which sources and factories exist
- **Source:** architecture #12 — **File:** `roamux/build/buildflags.gni:11-16`; `roamux/build/args/reference.gn`; `release.gn:22`; `roamux/build/ci/tier2_job.sh:71-74`
- **Observation:** a fresh developer following BOOTSTRAP builds a browser with no updater and different KeyedService registration than CI/release; `test_gn_args.py` does not pin it; memory records this biting fresh worktrees.
- **Evidence:** `tier2_job.sh:71 # out/Default carries roamux_enable_sparkle=true …`
- **Proposed change:** `roamux_enable_sparkle = true` in `reference.gn`, pin in `test_gn_args.py`, run `fetch_sparkle.py` from `pre_push.py`/BOOTSTRAP.
- **Tradeoff:** every dev build needs the vendored framework (one hash-verified download). — **Effort:** `[< 1 day]`

#### M30. 79 seconds of fixed wall-clock sleeps in browsertests despite an injectable clock
- **Source:** testing #6 — **File:** `roamux/test/roamux_refresh_all_initial_urls_browsertest.mm:235,281,345,402,466,516,546,607,677`; `roamux_tab_visit_gesture_browsertest.cc:133,223`; `roamux/browser/tabs/initial_url_refresh_run.cc:8`
- **Observation:** `PostDelayedTask(QuitClosure, Seconds(N))` for N = 3,3,12,2,20,12,8,14,2 (×3 with retries), several as negative assertions that are timing-fragile on a builder that also runs local builds; the scheduler takes a `TickClock` but `InitialUrlRefreshRun` hardwires `DefaultTickClock` and the test never uses `InitAndEnableFeatureWithParameters`.
- **Evidence:** `PostDelayedTask(FROM_HERE, loop.QuitClosure(), base::Seconds(20)); loop.Run();`
- **Proposed change:** shrink pacing via feature params (`interval_ms=300`, `min_spacing_ms=50`); `base::test::RunUntil` for positive waits; `SetTickClockForTesting` on `InitialUrlRefreshRun`; `FireDebounceForTesting()` on the coordinator.
- **Tradeoff:** pacing tests less "as shipped" — the pure scheduler test covers shipped constants. — **Effort:** `[< 1 day]`

#### M31. Unbounded poll loop can hang a test until the launcher timeout (×3 with retries)
- **Source:** testing #7 — **File:** `roamux/test/roamux_signin_optin_browsertest.cc:140-142`
- **Observation:** `while (!prefs->GetBoolean(…)) { RunLoop().RunUntilIdle(); }` with no deadline; neighbours already use `base::test::RunUntil`.
- **Proposed change:** `ASSERT_TRUE(base::test::RunUntil([&]{ return prefs->GetBoolean(…); })) << "…never committed";`
- **Tradeoff:** none. — **Effort:** `[< 1 day]`

#### M32. `test_workflow_invariants.py` is blind to several load-bearing release/lint steps
- **Source:** testing #8 — **File:** `roamux/build/tests/test_workflow_invariants.py:200-450`; `release.yml:82,178,225,325`; `ci.yml:34`
- **Observation:** would not catch: dropping `python3 -m unittest discover` from `lint`; removing the tag==VERSION step; removing the icon gate; removing `codesign --verify`; removing the staging validation (`verify_appcast.py`) or reordering it after publish; widening `permissions:`.
- **Evidence:** `grep -n "check-tag\|check_app_icon\|verify_appcast\|codesign" test_workflow_invariants.py` → nothing.
- **Proposed change:** add the five named tests plus a per-workflow `permissions` allowlist; plus the H5/H6 invariants.
- **Tradeoff:** more invariants to touch on legitimate change — the design. — **Effort:** `[< 1 day]`

#### M33. `test_tier2_job.py` proves the symlink/reset discipline by string matching, not behaviour
- **Source:** testing #9 — **File:** `roamux/build/tests/test_tier2_job.py:56-185`
- **Observation:** `assertIn('ln -sfn "${ROAMUX_CANONICAL_OVERLAY}"', code)`; nothing executes the script against a fake checkout — a wrong variable in `restore_overlay`, a trap installed after the flip, or `-e /roamux` on the wrong line would pass. The power-gate and caffeinate tests (:191-338) are behavioural and excellent.
- **Proposed change:** one behavioural case: fake `pmset`/`git`/`autoninja` (exit 1)/python shim; tmp `SRC` with a pre-existing symlink; assert after forced failure that `readlink $SRC/roamux == canonical`, git argv shows reset→clean→apply order, `out/CI` cloned.
- **Tradeoff:** the shim must not shadow the test runner's interpreter. — **Effort:** `[< 1 day]`

#### M34. `apply_patches.py`: four untested edge cases; wrong `--patches` dir exits 0; `git show` failure misreported
- **Source:** testing #10, error-handling #16 — **File:** `roamux/build/tests/test_apply_patches.py:60-186`; `roamux/build/apply_patches.py:42,49-52,108-111`
- **Observation:** untested: binary-patch rejection (bit someone in production), a retired-number gap, an offset-applied patch still matching the prefix, an unrelated dirty file not blocking. `glob("*.patch")` over a nonexistent path → "no patches found" → `return 0`; `_pristine` returns `None` for any `git show` failure, so a non-repo cwd blames the patch.
- **Proposed change:** four tests; `if not args.patches.is_dir(): fail`; empty dir is an error unless `--allow-empty`; distinguish `fatal: path … does not exist` from other git errors.
- **Tradeoff:** none. — **Effort:** `[< 1 day]`

#### M35. `ChordKeyDisplayString` has four fallback tiers; only tier 1 is tested, behind a layout-dependent `GTEST_SKIP`
- **Source:** testing #11 — **File:** `roamux/browser/ui/webui/chord_display.mm:36-67`; `roamux/test/roamux_shortcuts_browsertest.mm:183-190`
- **Observation:** tiers 2-4 and the `IsDisplayableKeyChar` rejections have zero coverage; the one caller test skips on any non-US-ANSI layout, so a builder layout change silently removes the rest. The function is pure.
- **Proposed change:** `roamux/test/chord_display_unittest.mm` in `roamux_unittests`: `kVK_F5→"F5"`, `kVK_ANSI_A→"A"`, out-of-range→`"?"`, the displayable-char table.
- **Tradeoff:** tier 1 stays layout-dependent. — **Effort:** `[< 1 day]`

#### M36. Retired/duplicate patch numbers apply on `sorted(glob)` with no gap or dedup check in the runhook
- **Source:** edge-cases #18 — **File:** `roamux/build/apply_patches.py:108`
- **Observation:** a re-added retired file or two files sharing a number prefix both apply; only `test_patch_inventory.py` guards, and only hermetically.
- **Proposed change:** assert a unique numeric prefix sequence with an explicit allowed-gap set before simulating (folds into M27's retirement list).
- **Tradeoff:** must encode the retired set. — **Effort:** `[< 1 day]`

#### M37. Tier-2 glue has browsertest-only coverage; the pin-controller test warps the real cursor
- **Source:** testing #12 — **File:** `roamux/browser/tab_visit/tab_visit_observer_bridge.cc:48-60`; `roamux_edge_import_coordinator.cc`; `tab_strip_pin_controller_views.cc`; `roamux/test/roamux_tab_strip_toggle_browsertest.mm:78-88`
- **Observation:** the bridge's close-vs-move classification and the coordinator's stage sequencing are asserted only indirectly; `tab_strip_pin_controller_views.cc` is exercised solely by a test that `CGWarpMouseCursorPosition(4000,4000)`s and reads real hover state — the most environment-coupled test in the tree.
- **Evidence:** `grep -l TabVisitObserverBridge roamux/test/*` → nothing; `// The hover model reads the REAL cursor`.
- **Proposed change:** move removal classification into `tab_visit_activation_class.h` with a unit test; a delegate seam on the coordinator; `hover_override_for_testing` on `TabStripPinControllerViews`.
- **Tradeoff:** small production refactors on tier-2-only code. — **Effort:** `[< 1 week]`

#### M38. `lint` runs no linter; `gn format` is a TODO echo
- **Source:** testing #13 — **File:** `.github/workflows/ci.yml:27-34`
- **Observation:** the required check does SPDX presence, two `test -f`, a TODO echo, and the Python suite. No `gn format`, no clang-format (no `.clang-format` in the overlay → local runs fall back to LLVM style), no Python linter, no `tsc`.
- **Proposed change:** pinned `gn format --dry-run` + `clang-format --dry-run -Werror` with a checked-in `.clang-format` (`BasedOnStyle: Chromium`) + `ruff check roamux/build scripts`; drop the TODO.
- **Tradeoff:** 1-2 min on hosted macOS. — **Effort:** `[< 1 day]`

#### M39. pre-push builds in the shared checkout and can grade the CI job's overlay instead of yours
- **Source:** testing #14 — **File:** `scripts/checks/pre_push.py:112-155`; `CONTRIBUTING.md:14`
- **Observation:** `gtest_decision` checks only that `out/Default` exists, then `autoninja` in `~/chromium/src`; a tier-2 job (triggered by the push you are about to make, on the same machine) re-points `${SRC}/roamux` mid-way, so pre-push compiles the CI workspace overlay and reports that as your verdict. It also runs `roamux_unittests` only — CONTRIBUTING's "the touched gtests" overstates it.
- **Proposed change:** resolve `realpath(src/"roamux")` and skip loudly (or fail) when it is not this worktree; print the resolved target; correct CONTRIBUTING; optional `ROAMUX_PRE_PUSH_TARGETS`.
- **Tradeoff:** the local gate is sometimes unavailable during a tier-2 run — better than wrong. — **Effort:** `[< 1 day]`

#### M40. Four `check_*.py` gates are never invoked by any pipeline and have no shipped-artifact test
- **Source:** testing #15 — **File:** `roamux/build/check_settings_logo.py`, `check_sparkle_bundle.py`, `check_toolbar_logo.py`, `check_update_row.py`; `roamux/patches/README.md:56-61`
- **Observation:** only `check_app_icon`/`check_framework_rpath` (release), `check_override_staleness` (tier-2), `check_sparkle_vendored` (GN) run anywhere; dark-logo and omnibox have a `test_shipped_artifacts_pass` case; the four above gate nothing despite the README calling them tier-1 gates.
- **Proposed change:** a `test_shipped_artifacts_pass` case per module, or a `lint` loop over `check_*.py --repo .`.
- **Tradeoff:** none. — **Effort:** `[< 1 day]`

#### M41. Nightly adds nothing Chromium-side over a PR run; ADR-0003 upstream-suite debt has no home
- **Source:** testing #16 — **File:** `.github/workflows/nightly.yml:24-33`; `docs/adr/0003-*.md:133-145`
- **Observation:** `nightly-selfhosted` runs the identical `tier2_job.sh`; the ADR records that upstream suites asserting replaced contracts run nowhere and defers the six-case pin to "if interactive_ui_tests is ever brought into CI".
- **Proposed change:** under `ROAMUX_NIGHTLY=1`, `autoninja unit_tests browser_tests` and filtered runs (`Roamux*:*EdgeImport*`, `RoamuxSettingsAbout*:VerticalTabStrip*`) with a checked-in `upstream_expectations.txt` (`# roam-N` + ADR case id) pinning the six cases. Also hosts the H15 leftovers.
- **Tradeoff:** first link is hours; then incremental. — **Effort:** `[< 1 week]`

#### M42. Sparkle private key handling in the release job (folded into C1) — tracked here so the Fixing Plan has a MEDIUM anchor for the keychain half
- **Source:** security #3, error-handling #23 — **File:** `.github/workflows/release.yml:273-289,350-352`; `roamux/app/release/keychain_cleanup.sh:5-10`
- **Observation / change / tradeoff / effort:** see C1 items (5)-(6). `[< 1 day]`

### 2.4 LOW

| ID | Source | File | Observation → Change | Effort |
|---|---|---|---|---|
| L1 | architecture #13 | `roamux/common/tab_strip_placement.h:4,35-46`, `.cc:56-66` | `common` carries `gfx::Rect` layout geometry and two totality contracts (roam-244 total accessors vs roam-254 deliberate CHECK) in one header → move `ComputeBottomStripLayout` to `browser/ui/tabs`; state the two-contract split in the header comment. | `[< 1 day]` |
| L2 | architecture #14 | `roamux/browser/updates/roamux_update_service.mm:236-266` | Per-profile `UpdateStateMachine`/`skipped_version_` front a process-wide `SparkleOwner`; two profiles show different snapshots after `Skip()` → move the machine into the owner; facades subscribe. | `[< 1 day]` |
| L3 | architecture #15 | git refs; `cliff.toml:18` | Stray lightweight tag `1.0.0.0` → `3107d54` sorts above every real release in `sort -V` → delete locally and remotely after confirming nothing references it. | `[< 1 day]` |
| L4 | architecture #16 | `roamux/build/args/reference.gn:24-35`, `release.gn:24-38` | Three pinned args duplicated with "propagation is manual" comments → `args/common.gni` imported by both; `test_gn_args.py` asserts once. | `[< 1 day]` |
| L5 | error-handling #17 | `verify_appcast.py:52-55`, `sign_update_wrapper.py:25-27`, `check_framework_rpath.py:86-91`, `package_roamux.py:50-61`, `rebrand_strings.py:350-356` | `capture_output=True, check=True` loses stderr on failure; `except Exception as e: print(...)` loses GRIT tracebacks; DMG retries unlogged → shared `run_checked()` printing stderr; log each attempt; `traceback.print_exc()`. | `[< 1 day]` |
| L6 | error-handling #19 | `tier2_job.sh:33-36` vs `release.yml:36-61` | Release validates `.env` keys/dirs; tier-2 defaults `SRC`/`DEPOT` to `$HOME/…` and leaves `RETRY_LIMIT`/`OUT` unvalidated → factor `resolve_machine_env.sh` sourced by both; echo resolved values. | `[< 1 day]` |
| L7 | error-handling #20 | `roamux/app/release/keychain_setup.sh:24-39,58-60`, `keychain_cleanup.sh:7-10` | (Dormant, signed mode) search list mutated before `ROAMUX_SIGNING_KEYCHAIN` reaches `$GITHUB_ENV`; empty `IDENTITY` undetected here → export immediately after `create-keychain`; snapshot/restore the search list; fail on empty identity. | `[< 1 day]` |
| L8 | error-handling #21 | `scripts/checks/check-issue-link.sh:27-35` | Missing `python3` reads as "branch does not contain roam-N" → `command -v python3 || fail "gate could not run"`; check `extract` rc. | `[< 1 day]` |
| L9 | error-handling #22 | `roamux/app/appcast/release_flow.py:24-38` | `require_sparkle_key`/`assert_publishable`/`plan_release` are never executed by `release.yml` → call it from the publish step or delete it. | `[< 1 day]` |
| L10 | security #9 | `ci.yml:16,41,105`, `release.yml:64,342`, `issue-link.yml:16`, `nightly.yml:15,31` | `actions/checkout@v4`, `upload-artifact@v4` by mutable tag inside the `release` environment → pin to commit SHAs; Dependabot for `github-actions` (exclude `dependabot[bot]` from tier-2). | `[< 1 day]` |
| L11 | security #10 | `release.yml:247-250,273-277,287-288,312-313` | Key file created before `chmod 600`; secret exported to the whole draft-release step; jq interpolates `${TAG}` → `umask 077`; scope the secret to a mini-step; `jq --arg`. | `[< 1 day]` |
| L12 | security #11 | `roamux_secret_import_stage.mm:80-81,163-168` | `static_cast<PasswordForm::Scheme/CookieSameSite/…>(ColumnInt())` with no range check → a crafted row reaches `NOTREACHED()` in the browser process → explicit switches with a default that skips the row. | `[< 1 day]` |
| L13 | security #12 | `roamux/browser/importer/edge_secret_decryptor.mm:36-50` | Keychain password left to ordinary destruction; derived key cleared with elidable `std::fill` → `OPENSSL_cleanse` both; scope `password` to the PBKDF2 block. | `[< 1 day]` |
| L14 | security #13 | `README.md`; `fetch_sparkle.py:28` | No `SECURITY.md`; no Sparkle advisory tracking (2.9.4 status unverified offline) → add `SECURITY.md`; extend the H1 staleness job to the Sparkle feed. | `[< 1 day]` |
| L15 | security #14 | `docs/adr/0002-*.md:19-25,54-59`; `release.gn:37` | The ~518 fieldtrial flips were counted, not triaged for security-relevant studies (sandbox, site isolation, PartitionAlloc, V8) → filter the #241 inventory by owner dir; record keep/re-enable per study; re-enable via `FeatureList` overrides. | `[< 1 week]` |
| L16 | testing #17 | `roamux_vertical_strip_placement_browsertest.cc` (16 sites) | `RunUntilIdle` + `DeprecatedLayoutImmediately()` as layout sync; breaks if an uprev makes relayout post/animate → one `SetPlacementAndWait(v)` helper on `RunUntil`; centralize the class-name lookup. | `[< 1 day]` |
| L17 | testing #18 | `roamux_shortcuts_browsertest.mm:189`, `roamux_signin_build_state_unittest.cc:45` | Skips (non-US layout; any Google key set) are log-only; a stray key in the runner `.env` silently drops the keyless-build proof → fail tier-2 on `SKIPPED` once the summary JSON exists; add issue links. | `[< 1 day]` |
| L18 | testing #19 | `roamux_bookmark_subfolder_groups_browsertest.cc:52-53,251-262` | Test globals reset mid-test to run a second scenario under the first's name → split into two tests. | `[< 1 day]` |
| L19 | testing #21 | `test_tier2_job.py:187-188`, `test_workflow_invariants.py` | `unittest.main()` guard placed before later classes; direct execution omits them (discover unaffected) → move to end of file. | `[< 1 day]` |
| L20 | edge-cases #14 | `rebrand_strings.py:184-186` | Duplicate-id abort cannot recognise an already-partially-migrated xtb after an interruption → detect old-id-absent/new-id-present and continue; name colliding ids. | `[< 1 day]` |
| L21 | edge-cases #15 | `roamux/browser/profiles/external_open_profile.cc:53-67` | `kExternalOpenProfile` fed to `Append` with no single-component/parent-ref guard (the importer has `IsSafeProfileName`) → reuse it. | `[< 1 day]` |
| L22 | edge-cases #16 | `refresh_all_initial_urls_command.cc:70-89`; `initial_url_refresh_scheduler.cc:104-108` | A run "finishes" at last-*start*; a second chord immediately re-navigates the active tab while its load is in flight (benign coalesce) → optional: skip a tab whose committed URL already equals its initial URL within the window. | `[< 1 day]` |
| L23 | edge-cases #17 | `rename_bundle.py:52`; `release_version.py:49-52` | 5-component `CFBundleVersion` (`0.0.1.1.10`) exceeds Apple's 3-integer guidance; Sparkle orders it correctly → validate with the notary before #90 lands; consider packing stage+N into the third component before stable. | `[< 1 day]` (at notarization) |

### 2.5 GOOD — keep these (verified, not assumed)

- **G1 `apply_patches.py` design** (architecture #17): forward-simulating the whole stack from `git show HEAD:` and prefix-matching the worktree is the correct answer to adjacent-hunk false negatives; idempotent, fail-loud with diverged file names, binary-patch rejection, 12 hermetic tests. Tier-2's "reset + clean -e /roamux, then runhook" reconcile is the right way to make the shared base CI-owned.
- **G2 Pure-core / chrome-facing split with DI** (architecture #18, testing GOOD): `InitialUrlRefreshScheduler` (Delegate + TickClock, weak-ptr invalidation before out-call, validated params), `TabVisitTraversalCoordinator` (deferred journal load, generation counter, `ReopenFn` injection), `SettledVisitJournalService` (`SequenceBound<VisitsStore>`), `ComputeNewTabPlacement`/`ResolveExternalOpenProfile` (pure with `FunctionRef`), `UpdateStateMachine` (pure, exhaustively tested), scheme alias hooking only `BrowserURLHandler`. Specification-grade unit tests: MOCK_TIME scheduler, v1/v2 journal migration fixtures, the D4 routing table, traversal decoys and NUL/DEL names in `edge_profile_reader_unittest.cc`, `TEST_P` truth tables. No `PlatformThread::Sleep`, no `DISABLED_`/`MAYBE_`, no weak test names.
- **G3 GN graph** (architecture #19): acyclic; overlay never depends on monolithic `//chrome/browser`; `common` clean of browser/chrome includes; `process_version("version_header")` reuse is the ideal zero-patch pattern.
- **G4 Governance as code** (architecture #20, testing GOOD): 29 workflow invariants (trust predicates, no `pull_request_target`, power gating, rebrand-then-gate order, re-cut safety), args pins, ADR shape, inventory presence; `REQUIRE_GRIT`/`SIGNING_PARTS`/`DMG_MOUNT` turn "required but unavailable" into failure, so a green `lint` is a real verdict on release tooling; `Tier2CaffeinateTest` drives the real entry point with a recursion tripwire. ADR 0003's two-part test + named counter-example + re-entry obligation is the model for the rest.
- **G5 Fail-closed gates and recovery** (error-handling #24): `require_ac_power.sh`, `signing_mode.py` partial→exit 2, `release_version.py` tag==VERSION before build, `fetch_sparkle.py` SHA-256 before extraction, `rebrand_strings.py` xtb-first ordering + `--check`, release re-cut idempotency, download-back-and-verify against the committed `SUPublicEDKey`, per-slice `lipo` assertion before the long compile, `renameatx_np(RENAME_EXCL)` IndexedDB publish with rollback, feature params discard-not-clip, bounded WebUI error taxonomy, durable pre-push log. Every histogram declared by 0013/0015/0021 is recorded by upstream code; none undeclared or dead.
- **G6 PR-triggered execution and PR metadata** (security #15): `pull_request` + `head.repo.fork == false` keeps forks off the self-hosted label; tier-2 has no secrets; `issue-link.yml` passes title/body/branch via `env:` to `printf '%s'` → Python (no `eval`, digits-only `N` before `gh api`, `issues: read`); governance reads commit messages from a file. No shell-injection path from attacker-controlled PR fields.
- **G7 `roamux://` confinement and the isolated-world override** (security #17): registered as standard + browser-handled, never web-safe, so `CanRequestURL` blocks renderer-initiated `roamux://` before the rewrite; scheme-only rewrite; display branding gated on `kChromeUIScheme`; the override is byte-identical upstream + an inert define. Suggested: a browsertest pinning that a renderer-initiated `roamux://settings` commits as `about:blank#blocked`.
- **G8 Importer path/symlink/origin discipline** (security #18): single-component profile names with control bytes and `..` rejected; symlinked profile dirs refused; every SQLite/LevelDB source copied to a private temp before opening; `Local State` size-capped; localStorage keys through `StorageKey::DeserializeForLocalStorage` with third-party keys dropped; secrets stripped from the utility mask so they never transit IPC in plaintext.
- **G9 Sign-in inert by construction** (security #19): every entry point intercepted unless keyed **and** `roamux_signin_authorized_build=true`; `google_keys.gni` gitignored; `git log -S` across all refs finds no API keys, tokens, client secrets or private keys.
- **G10 WebUI shortcut handler and external-open resolution validate server-side** (security #20): typed extraction, `CarbonKeycodeFromDomCodeString` negative on unknown, reserved-chord check before pref write, fixed glyph table for echo; designated profile resolved only against registered profiles with Guest/System excluded. Optional `CHECK_EQ(args.size(), 3u)`.
- **G11 Boundary cases verified sound** (edge-cases #20): 0-tab refresh-all, out-of-range/negative/`INT_MAX` feature params, alpha<beta<rc<final and `.9 < .10` ordering under Sparkle's comparator, malformed tags rejected at the gate, `GetNewTabPosition`/`GetTabStripPlacement` total over null/unregistered prefs, `active_index` out of range → append, `TabUidService` re-mints malformed/duplicate restored uids, `VisitsStore` FIFO cap 999 with trim in the same transaction and the roam-28 TOCTOU closed by generation bump.

---

## 3. Edge Case Risk Matrix

Ranked by Risk = Impact × Likelihood (H=3, M=2, L=1). Scenarios discovered by the edge-cases agent; cross-references name the deduplicated finding.

| # | Scenario | Likelihood | Impact | Risk | Component | File | Finding |
|---|---|---|---|---|---|---|---|
| 1 | Korean/zh-CN UI still says "Chromium" — `\b` never fires before Hangul/ideographs; **881 strings in the pinned checkout**; release gate checks only the English grd | High — verified against the checkout | High | 9 | rebrand channel | `roamux/build/rebrand_exclusions.py:53` | H3 |
| 2 | Hotfix re-cut under the same tag → identical `CFBundleVersion` → Sparkle's strictly-greater comparator never offers it → every existing user stranded on the buggy build | Med — a supported, documented one-command path | High | 6 | release / update feed | `.github/workflows/release.yml:290-339` | H4 |
| 3 | No `concurrency:` group; a second runner (roadmapped) or duplicate registration → release and tier-2/nightly reset+clean the same base mid-`autoninja` → release built from a corrupted stack | Med | High | 6 | CI / shared checkout | `.github/workflows/{ci,nightly,release}.yml` | H5 |
| 4 | Ctrl-C / disk-full mid-`write_text` → truncated grd → GRIT parse error → only recovery is `git reset --hard` → full 65-patch re-apply + re-vendor | Med | High (local hours) | 6 | rebrand channel | `roamux/build/rebrand_strings.py:316-323` | M1 |
| 5 | 24 h scheduled check finds alpha.10; `kUpdateFound` arrives in `kIdle` and is discarded; reply block leaks; the About row never shows it | High | Med | 6 | updates | `roamux/browser/updates/update_state_machine.cc:36-50` | H11 |
| 6 | Old tag re-published with `make_latest=true` → `/latest/appcast.xml` advertises the older build to the whole fleet | Med | Med | 4 | release / update feed | `.github/workflows/release.yml:338` | M2 |
| 7 | User runs Roamux from the mounted DMG; install fails on the read-only volume every interval; `kError` clobbers `kReadyToInstall`; generic message | Med — common with unsigned apps | Med | 4 | updates | `roamux/browser/updates/update_state_machine.cc:73-76` | M3 |
| 8 | M150 re-pin removes `#new-tab-adds-to-active-group` → build break, or a stub silently changes the default new-tab position | Med — expiry is certain | Med | 4 | tabs / uprev | `roamux/patches/0068-new-tab-position-seam.patch:48-51` | M5 |
| 9 | Import of thousands of Edge passwords → UI thread blocked in SQLite + Keychain decrypt; keychain prompt stacks on the frozen thread | Med | Med | 4 | importer | `roamux/browser/importer/roamux_edge_import_coordinator.cc:112-121` | M7 |
| 10 | Operator cancels tier-2 → SIGKILL skips the EXIT trap → `${SRC}/roamux` dangles at a reclaimed workspace → next local/CI build fails | Med | Med | 4 | CI / shared checkout | `roamux/build/ci/tier2_job.sh:41-50` | M9 |
| 11 | Global `core.autocrlf`/`apply.whitespace` differs between runner and maintainer → scratch bytes ≠ base bytes → "tree matches no stack state" with no real conflict | Low | High (blocks every build) | 3 | patch runhook | `roamux/build/apply_patches.py:84-89` | M4 |
| 12 | Import from an Edge profile with a multi-GB IndexedDB → uncapped temp copy fills the disk | Low | High | 3 | importer | `roamux/browser/importer/edge_local_storage_reader.cc:59-67` | M6 |
| 13 | A service constructed off-thread or before `InitSparkleUpdater` → two `SPUUpdater`s on the main bundle → the roam-140 second-click hang returns | Low | High | 3 | updates | `roamux/browser/updates/roamux_update_service.mm:220-224` | M8 |
| 14 | Interrupted rebrand partially migrated xtb ids → re-run's re-key collides → whole release aborts | Low | Med | 2 | rebrand channel | `roamux/build/rebrand_strings.py:184-186` | L20 |
| 15 | Hand-edited Local State sets `external_open_profile` to a traversal string → profile resolved outside the user-data dir | Low | Med | 2 | profiles | `roamux/browser/profiles/external_open_profile.cc:53-67` | L21 |
| 16 | A rebase reintroduces a retired patch file or a duplicate number prefix → both apply silently | Low | Med | 2 | patch runhook | `roamux/build/apply_patches.py:108` | M36 |
| 17 | Chord mashed twice → active tab navigated twice (coalesced) | Low | Low | 1 | tabs | `roamux/browser/tabs/refresh_all_initial_urls_command.cc:70-89` | L22 |
| 18 | Notarization enabled; notary rejects the 5-component `CFBundleVersion` | Low | Low | 1 | versioning | `roamux/app/release/rename_bundle.py:52` | L23 |
| 19 | GitHub blip at the Vendor-Sparkle step → hours-long release aborts, no retry | Low | Low | 1 | release | `roamux/build/fetch_sparkle.py:70` | M14 |

---

## 4. Add-on pressure tests

### 4.1 Scale stress — "install base ×100, team doubles: what breaks first?"

Ordered by what breaks first, with the finding that proves it.

1. **The shared checkout on one Mac (breaks on day one of a second maintainer).** Two people pushing means two tier-2 runs, two pre-push builds and possibly a release contending for `~/chromium/src`, `out/Default` and the `roamux` symlink. Today the only serialization is "one runner, one job" (H5); pre-push can already grade the other person's overlay (M39); a cancelled run leaves the symlink dangling for the other person (M9). *Fix before the second maintainer:* H5, M9, M39, H6.
2. **Merge conflicts in two hand-maintained files.** `roamux/patches/README.md` (66 commits, every feature PR edits a multi-paragraph row) and `roamux/BUILD.gn` (61 commits, 92 hand-listed test sources) are the two most-churned files (M27). With two people they conflict on every second PR. *Fix:* M27 (generate the README from preambles) and a per-subsystem `.gni` for test sources.
3. **Release authority.** With two push-capable people, either can produce a signed update (C1) and a same-tag re-cut (H4) or an old-tag republish (M2) affects 100× more users. *Fix:* C1 (ruleset + reviewer), H4, M2.
4. **Support load with zero telemetry.** At ×100 users the "still broken" class (H12) becomes the dominant cost: no logs, no histograms, no crash keys means every report is a reproduction hunt. Korean and Chinese users (H3) will file the same branding bug hundreds of times. *Fix:* H12, H3.
5. **Update-channel silent failures multiply.** H10/H11 (dead updater, dropped scheduled update) are invisible today because the audience is small; at ×100 they become "auto-update doesn't work" as the top issue, with no diagnostic (M11's locale-dependent classification makes non-English reports worse).
6. **Uprev cost.** Every milestone is a 65-patch rebase on files edited by up to 11 patches each (H2) plus a ~10 h cold universal build; with a security SLA (H1) this becomes a monthly multi-day event that currently only one person can do and that has never been rehearsed. *Fix:* H2 playbook + flag-patch collapse, H16 rehearsal job.
7. **CI wall-clock.** tier-2 already runs 79 s of fixed sleeps (M30) and retries every suite twice (H17); with two maintainers the queue doubles and flakes are still invisible. The 6 h default timeout (M9) turns a queued cold rebuild into a silent kill.

### 4.2 Hidden costs

| # | Cost | Where it shows | Evidence |
|---|---|---|---|
| 1 | **Operational:** every CI run is a tax on local development. tier-2 kills racing local builds, re-points the symlink, resets the base; release leaves it pointed at the runner's workspace. | Lost local build hours; wrong local verdicts. | H6, M9, M39; project history of symlink-race incidents. |
| 2 | **Debugging:** no logging in 12k LOC of production code; retry-masked flakes; a 6 h timeout with no phase timing; wrong-verdict investigations caused by the harness rather than the code. | Multi-day "still broken" triage sessions. | H12, H17, M9, M11. |
| 3 | **Onboarding:** BOOTSTRAP + 65 patches + three documented channels (really four, H13) + a `roamux_enable_sparkle` arg with three realities (M29) + a 64 KB README to read before touching a patch. A fresh checkout builds a browser with no updater. | Days before a first correct build; silent divergence from CI. | H13, M29, M27, M26. |
| 4 | **Velocity:** a feature PR touches `about_flags.cc` + `flag-metadata.json` + `flag-never-expire-list.json` (three one-file hunks), the README, BUILD.gn, and hand-mirrored command ids in three places. | Five-site edits for one command; patch re-contexting on every neighbour change. | H2, M22, M27. |
| 5 | **Maintenance with zero signal:** ~1,040 lines of tests that never run (H15), four gates never invoked (M40), a `release_flow.py` invariant module nothing executes (L9), a `chromium_src` channel with one inert override guarded by a permanent staleness gate (M24), and `targeted-suite`, a job that cannot fail (H7). All are rebased, maintained and read — none produce a verdict. | Rebase effort per uprev; false sense of coverage. | H15, M40, L9, M24, H7. |
| 6 | **Deferred uprev interest:** each milestone not taken compounds the rebase (upstream keeps editing the same 11-patch files) and extends the window in which users run a browser with public, patched bugs. | The first uprev will be the most expensive one. | H1, H2, M5. |

### 4.3 Principle violations

**Single Responsibility**
- `roamux/build/ci/tier2_job.sh` (135 l) does nine things: caffeinate re-exec, power gate, symlink flip/restore, base reset, patch apply, Sparkle vendoring, out-dir clone, three builds, three runs, staleness gate. Its tests can only string-match it (M33). → split into `resolve_machine_env.sh`, `reconcile_base.sh`, `build_and_run.sh` (L6, M9).
- `.github/workflows/release.yml` is 352 lines of shell inside YAML with no script file to test or rehearse (H16). → `release_build.sh`.
- `rebrand_strings.py` rewrites, re-keys, and gates in one module with one exception handler (M1, L5, L20).
- `roamux/common/tab_strip_placement.h` mixes pref accessors, dock predicates, RTL translation and rectangle layout, pulling `//ui/gfx/geometry` into `common` (L1).

**Dependency inversion**
- `shortcut_registry.cc` / `shortcut_registry_mac.mm` copy command-id integers rather than depending on the header that defines them (M22).
- `InitialUrlRefreshRun` hardwires `DefaultTickClock` although the scheduler it drives is clock-injected — the tests then sleep 79 s (M30).
- `SparkleOwner` is a bare global with unguarded lazy init (M8); per-window state lives in process-global maps keyed by raw `Browser*` instead of the window's own feature container (M28).
- Roamux code reaches upstream behaviour through 56 scattered `IsEnabled` sites instead of one accessor per flag (M23).

**Least privilege**
- The release job runs as the same user, on the same machine, with the same HOME as PR-triggered jobs; it sources an operator dotfile and puts an operator-controlled `python3` shim on `PATH` (C1).
- The Sparkle **private** key is imported into the login keychain to perform a **public**-key verification (C1/M42).
- The secret is exported to an entire step rather than the one command that needs it; the key file exists world-readable for an instant (L11).
- `git clean -fd` without `-x` lets untracked state survive between PR jobs and the release job (C1).
- Third-party actions run by mutable tag inside the `release` environment (L10).

**Explicit over implicit**
- Single-runner serialization is an accident, not a `concurrency:` declaration (H5).
- `if:`-skipped required check counts as success (H7).
- `roamux_enable_sparkle=true` in CI's `out/Default` is operator hand-state, not a committed arg (M29).

### 4.4 Compact & optimize — code that can be consolidated or eliminated

| # | Target | Action | Saves | Finding |
|---|---|---|---|---|
| 1 | 10 flag-entry patches (0024, 0043-0046, 0048, 0051, 0055, 0064, 0066, 0069 hunks into `about_flags.cc` / `flag-metadata.json` / `flag-never-expire-list.json`) | collapse into one `00NN-roamux-flags-entries.patch` | ~9 patch files; 30 hunks → 3; the largest rebase-conflict magnet | H2 |
| 2 | `chromium_src` channel: patch 0002, patch 0003, the sample override, `override_signatures.json`, `check_override_staleness.py` + `test_override_staleness.py`, `roamux_overlay_mechanism_unittest.cc`, tier-2 step :132 | retire until a real header override exists | 2 patches, 1 gate, 2 test modules, a tree-wide include-path edit and a hundreds-of-TUs `chrome_constants.h` hunk | M24 |
| 3 | `roamux/patches/README.md` (64 KB, hand-edited) | generate from patch preambles | the most-churned file becomes a build artefact | M27 |
| 4 | `roamux/app/appcast/release_flow.py` | wire into the publish step or delete | a module that runs nowhere | L9 |
| 5 | `reference.gn` / `release.gn` duplicated args | `args/common.gni` | three manual-propagation comments | L4 |
| 6 | `--gtest_filter="Roamux*"` | delete | a filter that only ever excluded one real test | H14 |
| 7 | `check_settings_logo.py`, `check_sparkle_bundle.py`, `check_toolbar_logo.py`, `check_update_row.py` | wire into `lint` or delete | four unexecuted gates | M40 |
| 8 | Command-id literals in `shortcut_registry.cc` + `shortcut_registry_mac.mm` + 4 patches | one header | three copies → one | M22 |
| 9 | 56 `IsEnabled` sites | accessors | OFF-semantics defined once | M23 |
| 10 | 79 s of `PostDelayedTask` sleeps | params + `RunUntil` | ~75 s per tier-2 run (×3 on retries) | M30 |
| 11 | Patch 0056 (`//base` test-only accessors, 412 lines, 7 files) | GN-arg gate or retire after roam-195 | a `//base` header patch out of the shipped stack; the 10.5 h cold-rebuild trigger | H2 |
| 12 | `targeted-suite` echo job | make it fail on the non-fork skip arm, or delete it | a job that cannot fail | H7 |
| 13 | ~2,170 LOC browser-side importer (if decision H8-b is taken) | either wire it (H8-a) or delete `roamux/browser/importer/{driver,coordinator,secret_stage,…}` and 0014's unit-test source_set | dead-in-production code that is green in tests | H8 |
| 14 | `roamux/BUILD.gn` 92 hand-listed test sources | per-subsystem `roamux/test/<area>.gni` lists | the second-most-churned file stops being a conflict magnet | M27 (adjacent) |
| 15 | `ThreeCarrierTest` + 6 patch-wired test files | move into `roamux_browser_unittests` | two test hunks out of the patch stack | H15 |

### 4.5 Strangler fig — minimal migration, no big bang

Each strand is independently shippable and leaves the old path in place until the new one is proven.

1. **Trust boundary first (week 1).** Add the tag ruleset and environment reviewer (no code). Add `concurrency:` and the release symlink restore. Switch `verify_appcast.py` to public-key verification and delete the keychain import. *Old path untouched:* the build still happens on the shared runner. *Cut-over:* when a dedicated signing job exists, `release.yml` uploads the unsigned zip as an artifact and the new job signs and publishes; the in-job signing steps are removed only after one successful release through the new path. (C1, H5, H6, M42, L10, L11)
2. **CI truthfulness (week 1-2).** Make `targeted-suite` fail on the non-fork skip arm; drop the gtest filter; add `roamux_sparkle_tests` to the build line; add summary JSON + `flake_report.py` in warn-only mode for two weeks, then flip it to fail-on-unlisted-retry once `known_flakes.txt` is seeded. (H7, H14, H15, H17)
3. **Observability at the boundaries (week 2-3).** Add `DVLOG`/`LOG(ERROR)` only where a `return false` already exists — updater start, state machine drops, importer carriers, journal open — plus the `EdgeImportReport` pass-through. No behaviour change; every site is a one-liner. Histograms come later, one subsystem at a time. (H10, H11, H12, H9, M10, M11, M15)
4. **Governance catch-up (week 3-4).** Land the ownership test and the preamble requirement for *new* patches first; backfill the 59 preambles in two or three batches; generate the README once the last batch lands. Supersede ADR 0001 when the ownership test is green. (H13, M26, M27, M36)
5. **Release rehearsal (week 4-5).** Extract `release_build.sh` from `release.yml` verbatim (no logic change), call it from both `release.yml` and a new nightly arm64 rehearsal job. Add the missing workflow invariants at the same time. (H16, M32)
6. **Uprev (month 2).** Write `docs/uprev.md` from the scattered obligations, collapse the flag patches, then dry-run M150/M151 in a scratch checkout. Fix H3 and H4 **before** the alpha.10 cut that the uprev produces, so the first Korean/Chinese users of alpha.10 see the right brand and a re-cut cannot strand anyone. (H1, H2, H3, H4, M5)
7. **Flag accessors and test-only //base (opportunistic).** Add accessors in `roamux_features.{h,cc}`; migrate each patch's sites the next time that patch is re-contexted by a rebase. Gate 0056 behind a GN arg at the first uprev. (M23, H2)

### 4.6 Success metrics and measurement plan

| Metric | Definition | Baseline (today) | Target (90 days) | How measured |
|---|---|---|---|---|
| **Pin staleness** | days between latest Chromium stable and `CHROMIUM_PIN` | ~62 days (M149 stable 2026-06-25 → 2026-08-26) | ≤ 14 days; ≤ 3 days after an in-the-wild CVE | nightly staleness step (H1) writes the delta to `$GITHUB_STEP_SUMMARY` and an issue |
| **Lead time** | PR opened → merged, non-draft | not measured | median ≤ 1 day for seam-only PRs | `gh pr list --json createdAt,mergedAt` weekly |
| **CI truthfulness** | % of test lines in the repo that execute in some CI job | ~1,040 lines never run (H15) + 1 excluded test (H14) | 100% of `roamux/test/**` and `roamux/build/tests/**` run in PR or nightly | hermetic test asserting every fixture is reachable by a workflow-invoked target + filter |
| **Flake rate** | tests passing only on retry / tests run, per tier-2 run | invisible (retry-limit 2, no summary) | measured within 2 weeks; ≤ 1% within 90 days; every retried test has a `roam-N` in `known_flakes.txt` | `--test-launcher-summary-output` + `flake_report.py` (H17) |
| **MTTR for CI wrong-verdicts** | time from a red/green disagreement to root cause | multi-day in project history | ≤ 4 h | per-suite logs + JSON artifacts uploaded `if: always()`; phase timings |
| **Tier-2 p95 wall-clock** | job duration | unknown; 6 h silent timeout | explicit `timeout-minutes`; p95 ≤ 90 min warm | `phase=… elapsed=` lines aggregated from step summaries |
| **Release p95** | tag push → published, and "release can be rehearsed" (bool) | ~10 h cold; rehearsal impossible | nightly rehearsal green ≥ 90% of nights; release ≤ 6 h | nightly `release-rehearsal` job (H16) |
| **Update delivery** | % of installs on the latest version 7 days after publish; scheduled-check-found-update surfaced (bool) | unknown; scheduled path silently dropped (H11) | ≥ 80% (post-alpha); browsertest for `userInitiated=NO` green | opt-in `sendSystemProfile` stays NO — use GitHub release-asset download counts as the proxy |
| **Escaped defects per release** | issues filed against a release within 14 days that are user-visible | not tracked | trend downward; zero branding-leak issues after H3 | issue label `escaped:<tag>` |
| **Rebrand completeness** | user-visible `Chromium` survivors in ko/zh-CN xtb after rebrand | 881 | 0 | new release gate (H3) |

### 4.7 Before vs after — components and data flow

```
BEFORE (today)
                                   ┌──────────────────────── maintainer's Mac ─────────────────────────┐
  GitHub                           │  ~/chromium/src  (ONE shared checkout)                            │
  ┌─────────────┐  PR/push main    │   ├── roamux -> symlink  ◄── re-pointed by tier-2 (trap-restored) │
  │ same-repo PR│─────────────────►│   │                         and by release (NEVER restored)  H6   │
  │ push main   │   tier2_job.sh   │   ├── out/Default  (operator; roamux_enable_sparkle=true by hand) │
  │ tag v*      │─────────────────►│   ├── out/CI       (cp -Rc clone, not restart-safe) M13          │
  └─────────────┘   release.yml    │   └── out/Release-{arm64,x64}  (first compiled at tag time) H16   │
        │ no ruleset, no reviewer  │                                                                   │
        │ no concurrency group H5  │  same login user for PR jobs AND release; sources ~/.env;        │
        │                          │  Sparkle PRIVATE key imported into login keychain (trap-only)  C1 │
        ▼                          │  local autoninja + pre-push build the same tree            M39    │
  releases/latest/appcast.xml ◄────┤  sign_update → draft → verify → make_latest=true (always) M2      │
        │                          └───────────────────────────────────────────────────────────────────┘
        ▼
  Every install: SPUUpdater, delegate:nil, no SUVerifyUpdateBeforeExtraction (M17)
    scheduled check → kUpdateFound in kIdle → dropped (H11); startUpdater failure ignored (H10)
    zero logs anywhere (H12); ko/zh-CN UI still says "Chromium" (H3)

AFTER (proposed)
  GitHub                                   maintainer's Mac (build only)              signing job (isolated)
  ┌───────────────────┐                    ┌──────────────────────────────┐          ┌──────────────────────┐
  │ tag ruleset: v*   │  concurrency group │ reconcile-first: assert -L   │ unsigned │ hosted macos-14 or   │
  │ env reviewer      │──────────────────► │ symlink, reset, apply, build │ zip+dmg  │ empty-HOME runner    │
  │ (C1)              │  (H5)              │ (release_build.sh, shared    │ artifact │ SPARKLE key only here│
  └───────────────────┘                    │ with nightly rehearsal H16)  │─────────►│ public-key verify    │
       │ PR/push main ──► tier2_job.sh     │ restore symlink always (H6)  │          │ make_latest iff ≥    │
       │   summary JSON + flake ledger H17 │ timeout-minutes (M9)         │          │ current (M2)         │
       │   no gtest filter (H14)           │ + roamux_sparkle_tests (H15) │          │ VERSION-moved gate   │
       │   targeted-suite fails on skip H7 └──────────────────────────────┘          │ (H4)                 │
       ▼                                                                             └──────────┬───────────┘
  nightly: staleness check (H1) · release rehearsal (H16) · unit_tests/browser_tests leg (M41)  │
                                                                                                ▼
  Every install: delegate pins feed + host, SUVerifyUpdateBeforeExtraction (M17)
    kUpdateFound accepted from idle (H11); startUpdater failure → FAILED row + LOG (H10)
    DVLOG/LOG at every boundary (H12); rebrand regex fixed + ko/zh-CN gate (H3)
```

### 4.8 Assumptions audit

| # | Assumption the codebase or its docs rely on | Status | Fastest validation | Finding |
|---|---|---|---|---|
| 1 | Only the operator can push `v*` tags | **Unverified** — no ruleset in the repo | `gh api repos/xinquan568/roamux/rulesets` and environment protection rules | C1 |
| 2 | One runner ⇒ jobs never overlap | **Unenforced** | `gh api repos/…/actions/runners`; add `concurrency:` regardless | H5 |
| 3 | Required checks on `main` are `lint` + `targeted-suite-selfhosted` and a skipped job counts as passing | **Inferred** | `gh api repos/…/branches/main/protection`; push a branch with the var unset and observe | H7 |
| 4 | Sparkle 2.9.4 refuses to install a lower `sparkle:version` than the running `CFBundleVersion` | **Unverified** | read `SPUUpdater`/`SUInstaller` in the vendored source; or a fixture appcast with a lower version against `roamux_sparkle_feed_test.mm` | M17 |
| 5 | A user-defaults `SUFeedURL` override is honoured by Sparkle 2 | **Unverified** | `defaults write <bundle-id> SUFeedURL …` on a packaged build and watch the request | M17 |
| 6 | Sparkle's version comparator orders `0.0.1.1.9 < 0.0.1.1.10` numerically | **Verified** by `test_release_version.py:53` and the header comment | — | G11 |
| 7 | M150/M151 are stable with security bulletins | **Inferred from cadence**, not checked online | `git ls-remote --tags https://chromium.googlesource.com/chromium/src 'refs/tags/15[01].*'` | H1 |
| 8 | The rebrand hole reaches real strings | **Verified** — 881 hits in the pinned checkout (ko 850, zh-CN 31) | rerun the letters-only scan after the regex fix → expect 0 | H3 |
| 9 | Safe Browsing is non-functional in the keyless build | **Plausible, unverified** | packaged build + network capture of `safebrowsing.googleapis.com` responses | M19 |
| 10 | Retried-then-passed tests are the flake signal | **Assumed** | run with `--test-launcher-summary-output` for two weeks and count | H17 |
| 11 | `apply_patches` scratch bytes == base bytes across environments | **Assumed** | run `--check` with `git -c core.autocrlf=input` and `=false` on the same tree | M4 |
| 12 | `MaybeStartEdgeBrowserSideImport` has no caller outside tests | **Verified** by grep over `roamux/patches/` and `chromium_src/` | confirm no upstream hook path by grepping the checkout for the symbol | H8 |
| 13 | `roamux_enable_sparkle=true` is set in the CI `out/Default` | **Operator hand-state** | `cat ~/chromium/src/out/Default/args.gn` | M29 |
| 14 | The Sparkle private key never reaches logs | **Verified** — no `set -x`, `:?` messages carry no values | keep the invariant test | C1/G5 |
| 15 | ADR 0002's 518 flips contain no security-relevant studies | **Untriaged** | filter the #241 inventory by owner directory | L15 |
| 16 | `NOTREACHED()` is fatal at this pin (so L12 is a crash, not memory corruption) | **Assumed** (M149) | check `base/notreached.h` in the checkout | L12 |
| 17 | The 6 h default job timeout is longer than any warm tier-2 run | **Contradicted by history** (a `//base` patch pushed a warm build past it) | set `timeout-minutes` explicitly with the measured cold number | M9 |

---

## 5. Executive summary

### Verdict
The overlay's *code* is in good shape — pure cores with injected seams, disciplined lifetimes, an acyclic GN graph, specification-grade unit tests, and governance that is tested as code. The *system around the code* is where the risk lives: the update channel can be driven by any push-capable credential on a runner shared with PR jobs; the shipped browser is two milestones behind with no uprev trigger and no rehearsed rebase; and CI has three ways to be green without proving anything (a kill switch that vacuously satisfies the required check, a gtest filter that excludes a real test, ~1,040 lines of tests nothing builds). Two product defects reach users today: 881 Korean/Chinese strings still say "Chromium", and the import picker offers password/cookie import that does nothing. Production code has no logging at all, so none of this is diagnosable from a user's machine.

### Top 3 actions
1. **Lock the update channel (≤ 1 week).** Tag ruleset + `release` environment reviewer, `concurrency:` group, release symlink restore, public-key-only verification, `SUVerifyUpdateBeforeExtraction`, then move signing off the shared runner and rotate the key. — C1, H5, H6, M17, M42. *Why first:* it is the only path by which a single mistake or leaked token reaches every installed user, and the first four items are configuration, not code.
2. **Make CI tell the truth (≤ 1 week).** Fail `targeted-suite` on the non-fork skip arm, drop the `Roamux*` filter, build and run `roamux_sparkle_tests`, add summary JSON + a flake ledger, set `timeout-minutes`. — H7, H14, H15, H17, M9. *Why second:* everything else in this report will be verified by CI, and today a green run does not mean what the branch protection assumes it means.
3. **Ship alpha.10 on a fresh pin, correctly (≤ 1 month).** Fix the CJK regex and add the ko/zh-CN gate, add the VERSION-moved gate for re-cuts, write the uprev playbook, collapse the flag patches, dry-run M150/M151, then cut. — H3, H4, H1, H2, M5. *Why third:* the uprev is the single largest deferred cost and the first one will be the hardest; the two release-time defects must land before it so the first release users actually receive is right.

### Confidence
| Recommendation | Confidence | What would raise it |
|---|---|---|
| C1 (tag push ⇒ signed release on shared runner) | **High** — read directly from `release.yml`/`ci.yml`/runner doc | confirming no ruleset/reviewer exists via `gh api` (assumption 1) |
| H3 (CJK rebrand hole) | **High** — regex probed and 881 strings counted in the pinned checkout | building one ko `.pak` after the fix and grepping it |
| H5, H6, H7, H14, H17, M9 (CI truthfulness set) | **High** — workflow/script text, some behaviours verified empirically | a behavioural `test_tier2_job.py` case (M33) |
| H8 (unwired Edge import) | **High** — no caller in patches or `chromium_src`; only tests construct the driver | grep the checkout for any upstream path that could call the symbol (assumption 12) |
| H10/H11 (updater silent failures) | **High** for the code path; **Medium** for Sparkle call-order claims (from headers, not vendored source) | a browsertest driving `showUpdateFoundWithAppcastItem:` with `userInitiated=NO` |
| H4 / M2 (re-cut stranding, `make_latest`) | **High** — pure functions of the tag string; comparator semantics verified in tests | none needed |
| H1 (stale pin, M150/M151 bulletins) | **Medium** — staleness is certain; the specific bulletins are inferred from cadence | one `git ls-remote --tags` (assumption 7) |
| M17 (Sparkle hardening) | **Medium** — plist absence is certain; defaults-override and downgrade behaviour are from memory | read the vendored 2.9.4 source (assumptions 4-5) |
| H13 / M26 / M27 (governance drift) | **High** — counted from BUILD.gn and patches | none needed |
| M19 Safe Browsing claim | **Low-Medium** | packaged-build network probe (assumption 9) |

### Paranoid verdict — the single scariest thing
**C1: a `v*` tag is the whole trust chain.** A leaked `gh` token or PAT from any laptop that runs this project's tooling pushes a tag; the shared runner — which also executes same-repo PR code as the same user, sources an operator dotfile, and has held the Sparkle private key in its login keychain across any job that died mid-step — signs the tag's contents with the real EdDSA key and marks it `latest`; within 24 hours every install offers it and the signature check passes because the signature is genuine. Nothing in the repository, the runner, or the client would notice. The edge-cases agent's own pick was the CJK rebrand hole (H3) for being silent and shipped to real users; the self-inflicted runner-up is H4, where a routine same-tag hotfix strands every existing user on the build it was meant to fix.

---

## Fixing Plan

Every item traces to a finding ID above. Effort in working days (`< 1 day` = 0.5–1, `< 1 week` = 3–5, `< 1 month` = 10–15).

### Phase 1: Critical fixes (do immediately)

- **C1 — tag push ⇒ signed release on the shared runner**
  - **Fix:** (1) repository ruleset restricting `refs/tags/v*` creation to the maintainer, no bypass; (2) required reviewer on the `release` environment; (3) replace `. .env` with a strict `KEY=value` parser; (4) `verify_appcast.py` verifies with the **public** key only (`ed25519_ref.py` or `openssl pkeyutl -verify -pubin`) and the `generate_keys` import + trap are deleted; add `security delete-generic-password -s https://sparkle-project.org -a roamux-release-verify` to `keychain_cleanup.sh` as belt-and-braces; (5) `umask 077` + secret scoped to a mini-step (L11 folded in); (6) split signing into a separate job (hosted `macos-14` or an empty-HOME runner user) consuming the unsigned zip artifact; release builds use a pristine out-dir or `clean -fdx`; (7) rotate `SPARKLE_ED_PRIVATE_KEY` after (6).
  - **Effort:** (1)-(5) 1 day; (6)-(7) 3-4 days
  - **Files:** GitHub settings (rulesets, environment); `.github/workflows/release.yml`; `roamux/app/appcast/verify_appcast.py`; `roamux/app/release/keychain_cleanup.sh`; `roamux/build/tests/test_workflow_invariants.py` (pin the new shape)

### Phase 2: High-priority fixes (this sprint)

| ID | Finding | Fix | Effort | Files |
|---|---|---|---|---|
| H5 | no `concurrency:` group | add `concurrency: {group: roamux-shared-base, cancel-in-progress: false}` to all three workflows; invariant test | 0.5 d | `.github/workflows/{ci,nightly,release}.yml`; `test_workflow_invariants.py` |
| H6 | release never restores the symlink | `if: always()` restore step validating `ROAMUX_CANONICAL_OVERLAY`; invariant test | 0.5 d | `release.yml`; `test_workflow_invariants.py` |
| H7 | kill switch ⇒ vacuous required check | `targeted-suite` exits 1 on the non-fork skip arm; add to required checks; flip invariant; revise doc | 0.5 d | `ci.yml`; `test_workflow_invariants.py:161-163`; `docs/ci/self-hosted-runner.md` |
| H14 | `Roamux*` filter drops ThreeCarrierTest | rename fixture; drop filter; hermetic fixture-vs-filter test | 0.5 d | `tier2_job.sh:129`; `roamux/test/roamux_three_carrier_survival_browsertest.cc`; new `roamux/build/tests/test_browsertest_fixtures.py`; `roamux/BUILD.gn:193` comment |
| H17 | retry masking, no artifacts | summary JSON per suite; `tee` logs; `if: always()` upload; `flake_report.py` + `known_flakes.txt`; phase timings | 1 d | `tier2_job.sh`; `ci.yml`; `nightly.yml`; new `roamux/build/ci/flake_report.py` + test |
| H15 | tests nothing builds | add `roamux_sparkle_tests` to build+run; move 0013/0014 unit tests into `roamux_browser_unittests`; register 0033 mocha + 0061/0062 for the M41 nightly leg | 3 d | `tier2_job.sh:110`; `roamux/BUILD.gn`; patches 0013, 0014 (regenerate); `roamux/patches/README.md` |
| H3 | CJK rebrand hole | `(?![A-Za-z0-9_])` boundary; CJK no-space fixtures; ko/zh-CN release gate | 1 d | `roamux/build/rebrand_exclusions.py:53`; `roamux/build/tests/test_rebrand_strings.py`; `release.yml:123-129` |
| H4 | same-tag re-cut strands users | fail re-cut when tag ref moved but VERSION unchanged; or 6th CFBundleVersion component | 3 d | `release.yml:290-339`; `roamux/build/release_version.py`; `roamux/app/release/rename_bundle.py`; `test_release_version.py` |
| H10 | `startUpdater:` failure ignored | capture BOOL; broadcast `kError`; `LOG(ERROR)`; injected-failure test | 1 d | `roamux/browser/updates/roamux_update_service.mm:198-203`; tests |
| H11 | scheduled update dropped | accept `kUpdateFound` from idle (keep skip guard) or synthesize `kCheckStarted`; re-broadcast in `showUpdateInFocus`; update unit test :93; `userInitiated=NO` browsertest | 1 d | `update_state_machine.cc:36-40`; `roamux_update_service.mm:84-98,145-148`; `roamux/test/roamux_update_state_machine_unittest.cc` |
| H8 | Edge browser-side import unwired | decide (a) land the importer-host seam patch + end-to-end browsertest, or (b) drop `PASSWORDS|COOKIES` from `services_supported`; fix header comment | (b) 1 d / (a) 4 d | `roamux/utility/importer/edge_profile_reader.cc:291-298`; new patch on `external_process_importer_host.cc`; `roamux_edge_import_driver.h:108-119` |
| H9 | import report discarded | pass `EdgeImportReport` through `on_done`; `LOG(WARNING)` non-clean carriers; surface `any_degraded()`; decide `version_supported` gating | 1 d log / 3 d UI | `roamux_edge_import_driver.cc:130-133`; `roamux_edge_import_coordinator.cc:72-73`; import UI patch |
| H12 | zero logging | `DVLOG(1)` at I/O-failure returns; `LOG(ERROR)` at the three never-happen paths; `[[nodiscard]]`; one crash key | 4 d | `roamux/browser/{importer,updates,tab_visit,tabs}/*.cc/.mm`; `roamux/utility/importer/edge_profile_reader.cc` |
| H13 | undeclared `sources +=` channel | ownership test (exactly one owner per source); ADR amendment; optional `roamux_upstream_sources.gni` | 3 d | new `roamux/build/tests/test_source_ownership.py`; `docs/adr/0001-*.md` or `0004`; optionally 15 patches |
| H16 | release never rehearsed | extract `release_build.sh`; nightly arm64 rehearsal job through packaging; invariants | 4 d | new `roamux/build/ci/release_build.sh`; `release.yml`; `nightly.yml`; `test_workflow_invariants.py` |
| H2 | uprev playbook / rebase surface | `docs/uprev.md` checklist; collapse 10 flag patches into one; gate 0056 behind a GN arg or retire; then dry-run M150/M151 in a scratch checkout | 4 d + 10 d dry-run | `docs/uprev.md`; patches 0024,0043-0046,0048,0051,0055,0064,0066,0069 → one; `0056-*.patch`; `roamux/build/buildflags.gni` |
| H1 | stale pin / no SLA | nightly staleness step + issue; `docs/security-uprev.md` SLA; cut alpha.10 on the first uprev | 1 d + uprev (counted in H2) | `nightly.yml`; `docs/security-uprev.md`; `roamux/build/CHROMIUM_PIN` |

### Phase 3: Medium-priority improvements (next sprint)

| ID | Finding | Fix | Effort | Files |
|---|---|---|---|---|
| M1 | non-atomic rebrand writes | `.tmp` + `os.replace`; optional xtb parse check | 0.5 d | `rebrand_strings.py:312-323` |
| M2 | `make_latest` always | conditional on `bundle_version ≥ current latest` | 0.5 d | `release.yml:338`; `test_workflow_invariants.py` |
| M3 | `kError` clobbers; DMG-run install; empty driver callbacks | install-location check + message; `retry()` once + `kInstallFailed`; `kError` on release-notes failure; reset on dismiss | 3 d | `update_state_machine.{h,cc}`; `roamux_update_service.mm:99-148`; state-machine tests |
| M4 | scratch `git apply` outside a repo | `git init -q` scratch; `-c core.autocrlf=false -c core.eol=lf -c apply.whitespace=nowarn` | 0.5 d | `apply_patches.py:80-89`; `test_apply_patches.py` |
| M5 | M150 flag expiry | roam-N tied to the pin bump; pinned test that fails at uprev; collapse 0067+enum | 3 d (at uprev) | `0067-*.patch`, `0068-*.patch`; `new_tab_placement.cc:19-22` |
| M6 | uncapped importer copies | aggregate-size check → `kDegraded`; `ReadFileToStringWithMaxSize` for Bookmarks | 0.5 d | `edge_local_storage_reader.cc:59-67`; `roamux_indexed_db_import_stage.cc:100-107`; `edge_profile_reader.cc:356` |
| M7 | secret stage on UI thread | `MayBlock` pool + UI reply | 3 d | `roamux_edge_import_coordinator.cc:112-121`; `roamux_secret_import_stage.mm` |
| M8 | unguarded `SparkleOwner` | `base::NoDestructor` / `SequenceChecker` + `CHECK` | 0.5 d | `roamux_update_service.mm:220-224` |
| M9 | EXIT trap vs SIGKILL; no timeout; restore misfire | `timeout-minutes` on both self-hosted jobs; reconcile-first symlink assertion; `-L` check + `::warning::` in trap | 0.5 d | `tier2_job.sh:41-50`; `ci.yml:97-107`; `nightly.yml`; `release.yml` |
| M10 | `VisitsStore` raze-on-any-failure | raze only on catastrophic / newer schema; error callback; log + `open_failed_` | 1 d | `visits_store.cc:42-52`; `settled_visit_journal_service.cc:29-36`; `visits_store_unittest.cc` |
| M11 | localized-string error classification | `code`/`domain` on `UpdateEvent`; classify on `SUSparkleErrorDomain` | 0.5 d | `roamux_version_updater.cc:89-105`; `roamux_update_service.mm:108-115`; tests |
| M12 | fail-open commit-msg gate | capture `rev-list` with rc check; non-empty assert; `shell: bash` | 0.5 d | `ci.yml:69-77` |
| M13 | `cp -Rc` not restart-safe | `.partial` + atomic `mv`; guard on `build.ninja` | 0.5 d | `tier2_job.sh:105-108`; `test_tier2_job.py` |
| M14 | `fetch_sparkle` timeout/idempotence | `urlopen(timeout=60)` + 3 retries; `VENDORED_VERSION` stamp; `bin/sign_update` in the vendored check | 0.5 d | `fetch_sparkle.py`; `check_sparkle_vendored.py`; tests |
| M15 | importer failure causes collapsed | skip counters; `kFailed` assignment; three-way status | 0.5 d | `roamux_secret_import_stage.{h,mm}`; `roamux_edge_import_coordinator.cc:152-158` |
| M16 | release partial failure | `if: always()` upload; PATCH retry loop; `publish_only` dispatch input | 1 d | `release.yml:330-348`; `test_workflow_invariants.py` |
| M17 | Sparkle hardening | `SUVerifyUpdateBeforeExtraction`; `check_sparkle_bundle.py` assert; `SPUUpdaterDelegate` pinning feed/host/version; confirm downgrade check in vendored source; ADR note | 0.5 d + 3 d | `roamux/app/sparkle-Info.plist`; `roamux_update_service.mm:188-204`; `check_sparkle_bundle.py`; browsertest |
| M18 | `javascript:` initial URL | `FixupURL` + scheme allowlist in `CommitEdit` and `DecodeExtraData`; per-scheme tests | 0.5 d | `edit_initial_url_dialog.cc:25-37`; `tab_initial_url_helper.cc:169-186`; unit tests |
| M19 | unsigned-distribution consequences | README/About statement; #90 as alpha exit criterion; Safe Browsing API key via `google_keys.gni`; fix entitlements comment | 1 d docs (+ signing/API key later) | `README.md`; `docs/help.md`; `roamux/app/release/entitlements/roamux.entitlements:5-6`; `google_keys.gni.template` |
| M20 | `secret_scan.py` gaps | GitHub-token, Sparkle-key, high-entropy patterns; filename rules; `.gitignore` globs | 0.5 d | `scripts/checks/secret_scan.py:11-19`; `.gitignore`; scanner tests |
| M21 | runner tarball checksum | `RUNNER_SHA256` + `shasum -c` | 0.5 d | `roamux/build/ci/provision_runner.sh:39-44`; `test_provision_runner.py` |
| M22 | hand-mirrored constants; 34059 | depend on `//chrome/app:command_ids` or single header; renumber into 33014+; `RoamuxMirroredConstantsTest` | 1 d | `shortcut_registry.cc:13-20`; `shortcut_registry_mac.mm:28-30`; patch 0010; `roamux/browser/tabs/BUILD.gn`; new unittest |
| M23 | scattered flag checks; stale texts | accessors in `roamux_features.{h,cc}`; migrate 56 sites (patch sites opportunistically); fix 3 stale texts; rename smoke test; `kTabStripToggleShortcut` OFF browsertest | 3 d | `roamux/common/roamux_features.{h,cc}`; 32 overlay sites; ~12 patches; `roamux_smoke_unittest.cc:18`; `roamux_features_unittest.cc:2-4`; `roamux/BUILD.gn:193-194` |
| M24 | `chromium_src` channel + 0003 marker | retire (delete 0002, 0003, override, signatures, staleness gate + tests, mechanism unittest, tier-2 :132) or move the marker into the override | 1 d | `roamux/patches/0002-*`, `0003-*`; `roamux/chromium_src/`; `roamux/build/override_signatures.json`; `check_override_staleness.py`; `test_override_staleness.py`; `roamux_overlay_mechanism_unittest.cc`; `tier2_job.sh:132`; `roamux/BUILD.gn` |
| M25 | `roamux_version_updater.cc` ODR | sparkle-gated `source_set("version_updater")` depended on by both; drop `nogncheck` | 0.5 d | `roamux/browser/updates/BUILD.gn`; `roamux/BUILD.gn:150-162`; patch 0033 (regenerate); `roamux_version_updater.h:9` |
| M26 | ADR fidelity | supersede ADR 0001 (four channels); ADRs for roam-226/182/201/132; fix "58-file" | 3 d | `docs/adr/0001-*.md`; new `docs/adr/0004..0008-*.md`; `docs/adr/0003-*.md:137` |
| M27 | patch inventory by hand | required preamble fields; enforce in `test_patch_inventory.py`; generate README; `After:` permutation test; `RETIRED` list | 4 d | 59 patch files (preambles); `roamux/build/tests/test_patch_inventory.py`; new `roamux/build/gen_patch_readme.py`; `roamux/patches/README.md` (generated) |
| M28 | raw-pointer window registries | `BrowserWindowFeatures`/`BrowserUserData` members; keep two named globals; move signin test state | 3 d | `tab_strip_toggle_command.cc:19-30`; `tab_strip_pin_controller_views.cc:31-37`; `refresh_all_initial_urls_command.cc:22-28`; `signin_inert.cc:9-13`; patches 0020/0054 |
| M29 | `roamux_enable_sparkle` realities | `true` in `reference.gn`; pin in `test_gn_args.py`; `fetch_sparkle.py` from pre-push/BOOTSTRAP | 0.5 d | `roamux/build/args/reference.gn`; `test_gn_args.py`; `scripts/checks/pre_push.py`; `BOOTSTRAP.md` |
| M30 | 79 s of sleeps | feature params + `RunUntil`; `SetTickClockForTesting`; `FireDebounceForTesting` | 1 d | `roamux_refresh_all_initial_urls_browsertest.mm`; `roamux_tab_visit_gesture_browsertest.cc`; `initial_url_refresh_run.{h,cc}`; `tab_visit_traversal_coordinator.h` |
| M31 | unbounded poll loop | `ASSERT_TRUE(base::test::RunUntil(...))` | 0.25 d | `roamux_signin_optin_browsertest.cc:140-142` |
| M32 | invariant blind spots | five named tests + `permissions` allowlist | 0.5 d | `test_workflow_invariants.py` |
| M33 | string-matched tier-2 tests | behavioural fake-checkout case | 1 d | `test_tier2_job.py` |
| M34 | `apply_patches` edge tests; fail-open dir | four tests; `is_dir` check; `git show` error discrimination | 0.5 d | `test_apply_patches.py`; `apply_patches.py:49-52,108-111` |
| M35 | `ChordKeyDisplayString` tiers | `chord_display_unittest.mm` | 0.5 d | new `roamux/test/chord_display_unittest.mm`; `roamux/BUILD.gn` |
| M36 | no gap/dup check in runhook | numeric-prefix uniqueness + allowed-gap set (from M27's `RETIRED`) | 0.25 d | `apply_patches.py:108`; `test_apply_patches.py` |
| M37 | browsertest-only glue; cursor-warping test | removal classification into `tab_visit_activation_class.h` + unit test; coordinator delegate seam; `hover_override_for_testing` | 3 d | `tab_visit_observer_bridge.cc:48-60`; `tab_visit_activation_class.h`; `roamux_edge_import_coordinator.{h,cc}`; `tab_strip_pin_controller_views.{h,cc}`; `roamux_tab_strip_toggle_browsertest.mm:78-88` |
| M38 | `lint` runs no linter | `gn format`, `clang-format` (+ `.clang-format`), `ruff`; drop TODO | 0.5 d | `ci.yml:27-34`; new `.clang-format` |
| M39 | pre-push grades the CI overlay | `realpath` check; print target; correct CONTRIBUTING | 0.5 d | `scripts/checks/pre_push.py:112-155`; `CONTRIBUTING.md:14` |
| M40 | four uninvoked gates | `test_shipped_artifacts_pass` per module or a `lint` loop | 0.5 d | `roamux/build/tests/test_{settings_logo,sparkle_bundle,toolbar_logo,update_row}*.py`; `ci.yml` |
| M41 | nightly adds nothing; ADR-0003 debt | `ROAMUX_NIGHTLY=1` leg building `unit_tests browser_tests` + filtered runs + `upstream_expectations.txt` | 3 d | `tier2_job.sh`; `nightly.yml`; new `roamux/build/ci/upstream_expectations.txt`; `docs/adr/0003-*.md` |
| M42 | Sparkle key in login keychain | (folded into C1 step 4) | — | — |

### Phase 4: Low-priority cleanup (when touching these files)

Grouped by file so a developer opening the file can clear every item at once.

- **`.github/workflows/release.yml`** — L10 pin actions to SHAs; L11 `umask 077`, scope the secret to a mini-step, `jq --arg` for `${TAG}`; L9 call or delete `release_flow.py`.
- **`.github/workflows/{ci,nightly,issue-link}.yml`** — L10 pin actions to SHAs.
- **`roamux/build/ci/tier2_job.sh`** — L6 source a shared `resolve_machine_env.sh`; validate `RETRY_LIMIT`/`OUT`; echo resolved values.
- **`roamux/build/rebrand_strings.py`** — L5 `traceback.print_exc()` in the handler; L20 recognise already-migrated ids on duplicate-id.
- **`roamux/build/apply_patches.py`** — (M34/M36 above).
- **`roamux/app/appcast/verify_appcast.py`, `sign_update_wrapper.py`, `roamux/build/check_framework_rpath.py`, `roamux/app/release/package_roamux.py`** — L5 shared `run_checked()` that prints stderr; log each `hdiutil` attempt.
- **`roamux/app/release/keychain_setup.sh` / `keychain_cleanup.sh`** — L7 export `ROAMUX_SIGNING_KEYCHAIN` right after `create-keychain`; snapshot/restore the search list; fail on empty identity.
- **`scripts/checks/check-issue-link.sh`** — L8 `command -v python3` guard; check `extract` rc.
- **`roamux/build/args/reference.gn`, `release.gn`** — L4 `common.gni`.
- **`roamux/common/tab_strip_placement.{h,cc}`** — L1 move `ComputeBottomStripLayout` to `browser/ui/tabs`; document the two totality contracts.
- **`roamux/browser/updates/roamux_update_service.mm`** — L2 move the state machine into `SparkleOwner`.
- **`roamux/browser/importer/roamux_secret_import_stage.mm`** — L12 explicit enum switches with skip-row defaults.
- **`roamux/browser/importer/edge_secret_decryptor.mm`** — L13 `OPENSSL_cleanse` both buffers; scope `password`.
- **`roamux/browser/profiles/external_open_profile.cc`** — L21 `IsSafeProfileName`-style guard before `Append`.
- **`roamux/browser/tabs/refresh_all_initial_urls_command.cc`** — L22 optional skip of already-at-initial tabs on rapid re-trigger.
- **`roamux/app/release/rename_bundle.py`, `roamux/build/release_version.py`** — L23 validate the 5-component version with the notary when #90 lands.
- **`roamux/test/roamux_vertical_strip_placement_browsertest.cc`** — L16 `SetPlacementAndWait` helper on `RunUntil`.
- **`roamux/test/roamux_shortcuts_browsertest.mm`, `roamux_signin_build_state_unittest.cc`** — L17 issue links on skips; fail tier-2 on `SKIPPED` once H17 lands.
- **`roamux/test/roamux_bookmark_subfolder_groups_browsertest.cc`** — L18 split the two scenarios.
- **`roamux/build/tests/test_tier2_job.py`, `test_workflow_invariants.py`** — L19 move `unittest.main()` to end of file.
- **`docs/adr/0002-*.md`** — L15 triage the 518 flips for security-relevant studies.
- **Repo root** — L14 add `SECURITY.md`; L3 delete the stray `1.0.0.0` tag (local + remote) after confirming nothing references it.

### Dependency graph

- **H17 (summary JSON) before L17** (fail on `SKIPPED`) and before the flake ledger can be enforced — the JSON is the data source.
- **H7 and H5 before any second runner** — otherwise a skipped or overlapping job is indistinguishable from a passing one.
- **C1 steps (1)-(2) before H4/M2** are worth anything — a re-cut gate is moot if anyone can push a tag; conversely **H4 and H3 before the alpha.10 cut** that H1/H2 produce.
- **H2 (flag-patch collapse) before M23** (accessor migration of patch sites) — collapsing first means one re-context instead of ten.
- **M27 (preambles + `RETIRED`) before M36** (runhook gap/dup check) — the allowed-gap set comes from the retirement list.
- **H13 (ownership test) before M25** (version_updater ODR) and before M24 (retire `chromium_src`) — the test is what proves both cleanups are complete.
- **H16 (`release_build.sh`) before M32's release invariants** — the invariants should pin the script call, not 352 lines of YAML.
- **M29 (`roamux_enable_sparkle=true` in `reference.gn`) before H15** (build `roamux_sparkle_tests` in tier-2) — otherwise the target does not exist in a fresh out-dir.
- **M1 (atomic rebrand writes) before L20** (partial-migration detection) — the detection is only needed for the failure mode M1 removes.
- **H8 decision (a/b) before H9's UI surfacing** — there is no import-complete UI to surface into unless the browser-side path is wired.
- **M17 delegate before confirming assumption 4/5** (Sparkle downgrade / feed override) — or, cheaper, read the vendored source first and let the answer size the delegate.

### Estimated total effort

- **Phase 1 (C1):** ~5 days (1 day configuration + verify change; 3-4 days signing-job isolation + key rotation)
- **Phase 2 (H1-H17):** ~30 days of engineering + ~10 days for the first M150/M151 dry-run uprev
- **Phase 3 (M1-M41):** ~48 days (37 items; most are half-day to one-day, six are 3-4 days)
- **Phase 4 (L1-L23):** ~9 days, opportunistic
- **Total:** ~92 days of focused engineering + ~10 days of uprev dry-run. Sequenced per §4.5, the first two strands (~12 days) remove the CRITICAL and every "CI can be green without proving anything" item; the remaining work is a quarter of steady cleanup interleaved with feature work.
