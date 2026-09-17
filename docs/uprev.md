<!-- SPDX-License-Identifier: Apache-2.0 -->
# Uprevving the Chromium pin

Roamux tracks the latest stable Chromium milestone and re-pins every milestone cycle
(`docs/adr/0001-chromium-overlay-strategy.md`: "A per-milestone re-pin (Q(i4)-A) tracks the latest stable
Chromium"). This document is the procedure for doing that, and the register of everything that must be
re-checked when the pin moves.

> **Status: not yet rehearsed end to end on a new pin.** Every step — or, in a compound step, each sub-step —
> is tagged:
>
> - **[verified on M149]** — executed against pin `149.0.7827.201`, overlay revision `79f8532`, on
>   2026-09-13, with the result recorded inline;
> - **[prospective]** — derived from the repository's own sources but not yet exercised.
>
> The first real uprev (roam-293) owns the end-to-end rehearsal. Correct this document from that
> experience, and replace each **[prospective]** tag with a dated **[verified on M1xx]** as it is proven.
> Until then, read a [prospective] step as a careful plan, not a runbook.

## Before you start

- **Name your two checkouts.** Commands below use `SRC` for the Chromium checkout and run from the root of
  the Roamux checkout on the uprev branch:

  ```sh
  SRC=~/chromium/src              # or the scratch checkout
  cd /abs/path/to/codes/roamux    # the Roamux checkout, uprev branch
  ```

  `$SRC/roamux` must be the symlink to *this* checkout's `roamux/`. On the shared base it usually is not: a
  CI job links the base to its own checkout while it runs, and its exit trap restores the runner's
  `ROAMUX_CANONICAL_OVERLAY` — the operator checkout, on whatever branch that checkout has (an interrupted
  job can leave its own link behind). Check `readlink "$SRC/roamux"` before validating locally. A fresh scratch checkout has no link, no
  Sparkle and no build directory: set it up with `BOOTSTRAP.md` §2 (fetch), §3 (the overlay link and
  `fetch_sparkle.py`; its patch and rebrand commands are steps 3 and 6 here) and §4 (args and build).
- **Serialize with CI.** `~/chromium/src` is shared with the self-hosted runner. Tier-2, nightly and release
  jobs reconcile that base to HEAD plus their own stack (`apply_patches.py --reconcile`, roam-341)
  (`docs/ci/self-hosted-runner.md` §Cache model), which will reset an uprev in progress. Make sure no job is
  running and none will start: stop the runner's listener for the duration.
- **Prefer a scratch checkout for the first pass** (the approach roam-293 names), so a failed uprev never
  leaves the shared warm base half-migrated.
- **Start from clean tracked sources.** Uncommitted edits in the base do not survive a CI reconcile, and they
  also make `apply_patches.py`'s prefix check fail.

## Protocol

### 1. Obtain and sync the target revision [prospective]

From `BOOTSTRAP.md`:

```sh
git -C "$SRC" fetch --tags
git -C "$SRC" checkout <TAG>        # the new stable milestone tag
(cd "$SRC" && gclient sync -D)      # sync deps to that tag; vendors gn/ninja
```

**Editing `roamux/build/CHROMIUM_PIN` does not move the checkout.** The pin file is read by tooling; the
checkout moves only through the commands above.

### 2. Update the pin, then prove the checkout matches it

Record the tag in `roamux/build/CHROMIUM_PIN` **[prospective]**, then confirm that HEAD is the pinned commit
**before running any validation** **[verified on M149]**:

```sh
test "$(git -C "$SRC" rev-parse HEAD)" = \
     "$(git -C "$SRC" rev-parse "refs/tags/$(grep -v '^#' roamux/build/CHROMIUM_PIN | sed '/^$/d')^{commit}")"
```

This matters because the two tools below resolve different references: `apply_patches.py` simulates against
the checkout's **HEAD**, while `check_override_staleness.py` resolves **`refs/tags/<pin>`** strictly, with no
HEAD fallback. Both can pass while looking at different revisions.

*Verified on M149 (the equality check):* HEAD equalled `refs/tags/149.0.7827.201^{commit}`; exit 0.

### 3. Apply the patch stack

```sh
python3 roamux/build/apply_patches.py --chromium-src "$SRC" --check   # verify only
python3 roamux/build/apply_patches.py --chromium-src "$SRC"           # apply
```

- **`--check` verifies and does not apply.** It simulates the stack from HEAD, accepts the tree only if it
  matches pristine or an applied prefix, and reports each patch as `[applied]` or `[appliable]`. A green
  `--check` does not by itself mean the stack is applied. **[verified on M149]** — exit 0, all **67** patch
  files (numbered through `0072`) reported `[applied]`.
- **Application on a new pin [prospective].** Expect conflicts; that is the rebase signal the runhook is
  designed to raise. Triage in stack order, because later patches may depend on earlier context
  (`roamux/patches/README.md` records ordering dependencies per patch).
- After any rebase or retirement decision, **re-run `--check`** before moving on.
- **`--reconcile` is CI's mode, and it overwrites.** It forces the checkout to HEAD plus the current stack
  (leaving byte-identical patched files untouched — roam-341). Never run it on a checkout whose edits you want.

### 4. Override staleness

```sh
python3 roamux/build/check_override_staleness.py --chromium-src "$SRC"            # gate
python3 roamux/build/check_override_staleness.py --chromium-src "$SRC" --update   # after review
```

Every file under `roamux/chromium_src/` shadows an upstream file. The gate compares a recorded hash of the
*pristine* upstream counterpart at the pin against the new pin, and fails loudly when it changed.
**[verified on M149]** — exit 0, "1 override(s) fresh at 149.0.7827.201". Re-reviewing flagged overrides and
running `--update` on a new pin is **[prospective]**. Context: roam-300 proposes retiring the override
channel altogether.

### 5. Work the per-pin obligation register [prospective]

See [Per-pin obligation register](#per-pin-obligation-register) below. Do this before building: several
obligations change what is built.

### 6. Rebrand re-run [prospective]

Re-run the rebrand and its locale gate — see `docs/release.md` §Locale rebrand gate (roam-284). A new pin
brings new upstream strings.

### 7. `AboutFlagsTest`, locally [prospective]

`AboutFlagsTest` lives in upstream `unit_tests`, which **no Roamux job builds** — it is invisible to CI. It
enforces two things any Roamux `chrome://flags` entry must respect:

- **global alphabetical order** in `flag-metadata.json` (Roamux entries sort among all entries, not together);
- **`expiry_milestone: -1` ⊆ `flag-never-expire-list.json`**.

`unit_tests` reaches Sparkle through patch `0026`'s `//roamux/browser/updates:update_service` dependency, so
launching it has required pointing `DYLD_FRAMEWORK_PATH` at the vendored framework
(`roamux/third_party/sparkle`). That launch detail is unverified at this pin.

If roam-340 (collapsing the flag-entry patches) has landed, this step becomes more load-bearing, not less.

### 8. Field-trial feature inventory (ADR 0002) [prospective]

`docs/adr/0002-disable-fieldtrial-testing-config-in-shipped-builds.md` requires, at every pin bump:
"regenerate the inventory (one script over the config JSON + a `BASE_FEATURE` harvest) … and attach it to
the uprev record."

The generator is `roamux/build/fieldtrial_inventory.py` (roam-342). It classifies every feature named in a
mac study's first-listed experiment's `enable_features` or `disable_features`, separately for forced-ON and
forced-OFF directives, into **effective flips**, **no-ops** and **unresolved**, and counts the activated
experiments whose field-trial params vanish. Those buckets are the generator's `textual-v1` harvest
classifications of the pristine tag — **not verified macOS compiled behaviour** (ADR 0002 §Consequences,
"Methodology note": a non-mac `#if` branch or a test source can supply the default, and some literal
defaults are spelled in a form the rule does not match). Treat *unresolved* as potential flips, and read a
jump in that count as "new spelling the rule missed", not as a config change. The historical M149 numbers
stand.

```sh
python3 roamux/build/fieldtrial_inventory.py --chromium-src "$SRC" --out-dir docs/uprev-records/<TAG>/
```

It reads the pin from `roamux/build/CHROMIUM_PIN` (or `--tag <TAG>`), resolves `refs/tags/<pin>` strictly
like the staleness gate, and writes `effective-diff.json` and `inventory.md` in about five seconds. **Attach
both**: commit them under `docs/uprev-records/<TAG>/` in the uprev PR and link them from its body — that
directory is the "uprev record" ADR 0002 refers to. The inventory reads upstream's pristine defaults; the
overlay's own default flips (today patch `0067`, `NewTabAddsToActiveGroup`) are a separate, known delta.

**[verified on M149]** — the generator reproduces issue #241's inventory exactly: 653 studies; forced-ON
747 → 518 / 113 / 116; forced-OFF 47 → 13 / 21 / 13; 197 param sets (`REQUIRE_FIELDTRIAL_ORACLE=1
ROAMUX_CHROMIUM_SRC=$SRC python3 -m unittest roamux.build.tests.test_fieldtrial_inventory`; tier-2 runs it
fail-not-skip). Running it on a *new* pin is what stays [prospective].

**Standing prerequisite, independent of the pin:** that acceptance test always needs tag `149.0.7827.201`
resolvable in the runner's checkout, at every future pin. A fresh or shallow checkout must fetch it —
`git -C "$SRC" fetch origin +refs/tags/149.0.7827.201:refs/tags/149.0.7827.201` — and the tier-2 gate fails,
not skips, when it is absent.

Separately, `roamux/build/tests/test_gn_args.py` checks that `disable_fieldtrial_testing_config = true` is
present in both args templates. **[verified on M149]** — 3 tests pass. It validates **template text only**,
not the effective build configuration, and says nothing about what flips at a new pin.

### 9. Upstream-contract triage (ADR 0003) [prospective]

Apply `docs/adr/0003-upstream-tests-of-revoked-surfaces.md`: its two-part applicability test, and its register
of known divergent upstream tests, matched **by failure signature, not by test name**.

### 10. Build, test and rehearse [prospective]

Build the Roamux targets and run the Roamux suites locally.

**Tier-2 does not move to the new pin by itself.** It runs against the runner's configured checkout
(`ROAMUX_CHROMIUM_SRC`, default `~/chromium/src`), and its reconcile (`apply_patches.py --reconcile`) forces the checkout to HEAD plus the stack: it never
checks out `CHROMIUM_PIN` and never asserts HEAD against it (`roamux/build/ci/tier2_job.sh`). An uprev
proven in a scratch checkout therefore proves nothing in CI until the runner's checkout moves too. Before
resuming the runner for the uprev PR:

1. run step 1 against the runner's checkout, with the target tag;
2. run step 2's equality check there, against the PR branch's `CHROMIUM_PIN`;
3. record that checkout's `git rev-parse HEAD` in the PR alongside the tier-2 result.

A tier-2 run applies the patch stack of the revision **it** checked out (the PR's merge commit), and a re-run
keeps that original revision. So once the shared base is on the new pin, any run whose revision predates the
uprev — a queued run, a re-run, or a PR not yet updated past the uprev merge — applies an old-pin stack to a
new-pin base and fails. Landing the uprev does not fix those runs by itself: before resuming normal traffic,
cancel stale queued runs and start fresh ones from revisions that include the uprev.

Then follow
`docs/release.md` §Rehearsal obligation before the next release is cut from the new pin.

## Per-pin obligation register

### Rule: a guard never replaces a stated manual obligation

Some obligations are backed by a test that fails loudly at uprev. Those guards check **current state**, not
**completeness**: a newly added upstream feature, a new trigger, or a changed probe can leave a guard green
while the obligation it backs is unmet. Where a source states a manual obligation, it stays manual even when a
guard exists — the guard is a backstop.

### Manual audits

These fail silently if skipped. Perform each one.

| Obligation | Source | Action |
|---|---|---|
| Ctrl+Cmd+R accelerator collision re-audit | `roamux/patches/0010-reload-initial-url-command.patch` — "Re-check on uprev" | Re-audit Ctrl+Cmd chords for a `VKEY_R` conflict. |
| Ctrl+Opt+Cmd+R collision audit | `roamux/patches/0065-refresh-all-initial-urls-command.patch` — "RE-RUN ON UPREV" | Re-run the audit it describes. Known, accepted limitation recorded there: a stored custom binding can shadow this command silently. Guard backstop: `ChordIsNotReservedElsewhere`. |
| WebUI-toolbar suppression set | `roamux/test/support/roamux_browser_test.h` — "re-derive from the pin's" | Re-derive from the pin's `IsWebUIToolbarEnabled()` (`chrome/browser/ui/ui_features.cc`) **and** its independent triggers — currently `kWebUIToolbarProcessOverheadExperiment` (`chrome/browser/ui/waap/initial_web_ui_manager.cc`). Guard backstop: the env test below. |
| roam-240 guard's probe feature | `roamux/test/roamux_test_env_browsertest.cc` — `FieldTrialTestingConfigIsOffInOverlayTests` | Confirm the probe `tabs::kVerticalTabs` is still default-off **and** enabled by the testing config at the new pin. If not, choose a new probe — otherwise the guard passes vacuously. |
| mac deployment target vs `Assets.car` | `roamux/app/resources/icons/mac/README.md` — "re-check at uprev" | If `mac_deployment_target` in `build/config/mac/mac_sdk.gni` changed, recompile `Assets.car` with a matching `--minimum-deployment-target` using that README's recipe, then verify with `roamux/build/check_app_icon.py`. |
| mac app icon on conflict | `roamux/patches/README.md` — patch `0029` row | Upstream is mid-migration on mac iconography: re-point the icon bundle-data targets at the Roamux payloads on **both** channels. |
| Field-trial feature inventory | ADR 0002 — "regenerate the inventory" | Run protocol step 8 and commit both outputs under `docs/uprev-records/<TAG>/`. Its oracle test guards the *tool*, not the pin: it cannot tell you what flipped. |
| Oracle tag `149.0.7827.201` reachable in the runner's checkout | `roamux/build/tests/test_fieldtrial_inventory.py` — "PERSISTENT prerequisite" | Independent of the pin: before resuming tier-2 on a fresh or shallow checkout, fetch the tag (command in protocol step 8). Guard backstop: the gate test fails, not skips, under `REQUIRE_FIELDTRIAL_ORACLE=1`. |
| `#new-tab-adds-to-active-group` expiry at M150 | `roamux/patches/0068-new-tab-position-seam.patch` — "UPREV CAVEAT (M150)" (with `0067`) | Decide whether patches `0067` and `0068` collapse into a single Roamux-owned switch, or keep both. |
| WebUI location-bar icon mapping | `roamux/patches/README.md` — patch `0039` row | *Conditional product obligation:* if an uprev enables `kWebUILocationBar`, add the Roamux icon mapping, because the native handler CHECKs unmapped icons. Overlay tests pin that surface off, which is exactly how the incompatibility would stay hidden. |
| `chromium_src` override staleness | `roamux/chromium_src/README.md` — "On a milestone uprev" | Protocol step 4: re-review, then `--update`. |
| Patch `0056` rebase | Maintainer decision recorded on roam-291 (2026-09-13) | Rebase its seven `//base` files. Kept deliberately; reopen only on a measured per-run invalidation share, real rebase burden at an uprev, or an upstream equivalent. Since roam-341 the reconcile leaves its headers untouched while their existing regular-file entries already have the stack's bytes and mode; they are rewritten (and their dependents rebuilt) when a hunk changes or a repair is needed — a stray edit, a missing file, a type or mode change. |
| Patch `0072` retirement | `roamux/patches/0072-worker-thread-hang-watch-background-hint-gate.patch` — "at every uprev, check whether the pinned" | Read the **pristine** file at the target tag — the applied tree already carries the backport: `git -C "$SRC" show <TAG>:base/task/thread_pool/worker_thread.cc`. If `watch_for_hangs` in `WorkerThread::RunWorker` tests `thread_type_hint_` rather than `GetDesiredThreadType()`, delete the patch and its README row. Guard backstop: the `apply_patches.py` row below. |

### Guard-signalled

These announce themselves as test or runhook failures during the uprev. Each failure names its action.

| Guard | Fails when | Action |
|---|---|---|
| `roamux/test/roamux_test_env_browsertest.cc` — WebUI-toolbar wait guard | the initial-paint wait gate becomes reachable again | Re-derive the suppression set (manual row above). |
| `roamux/test/roamux_test_env_browsertest.cc` — feedback-uploader guard (roam-223) | "the suppression hook moves or the factory is reshaped" | Re-point `SuppressFeedbackUploaderForTesting` (`roamux/test/support/roamux_browser_test.cc`). |
| `roamux/test/roamux_test_env_browsertest.cc` — `FieldTrialTestingConfigIsOffInOverlayTests` (roam-240) | "the switch is lost in a refactor or the mechanism changes at an uprev" | Re-establish `--disable-field-trial-config` in `RoamuxBrowserTest::SetUpCommandLine`. Also check the probe (manual row above). |
| `roamux/test/roamux_refresh_all_initial_urls_browsertest.mm` — `ChordIsNotReservedElsewhere` | upstream introduces a browser-owned Ctrl+Opt+Cmd+R | Resolve the collision; see the manual Ctrl+Opt+Cmd+R audit. |
| `roamux/test/roamux_external_open_profile_browsertest.mm` — deterministic tab-count assertions (roam-213) | "If an uprev changes this" | Re-decide the external-open stance. |
| `apply_patches.py` on patch `0072` | its context changes | Do the manual `0072` retirement audit above. Retire only on that evidence — upstream carrying the change is one cause of this failure, and any unrelated context change fails the same way. A clean apply does **not** mean the audit can be skipped. |

### How this register was built

On 2026-09-13, against pin `149.0.7827.201`:

```sh
grep -rn -i -E 'uprev|re-?pin(ned|ning)?\b|pin bump|per-milestone|each milestone|every milestone' \
  roamux docs scripts .github BOOTSTRAP.md CONTRIBUTING.md \
  | grep -v -e '^docs/grill/' -e '^docs/uprev.md:' -e '__pycache__' \
  | grep -v -E '^roamux/patches/[0-9]{4}-[^:]*\.patch:[0-9]+:[-+ @]'
```

The filters drop the grill reports, this document, generated caches, and patch diff-body lines (patch
preambles stay in). Two earlier sweeps built from lists of specific phrasings each missed obligations worded
differently, so **re-run this superset form and classify every hit** rather than searching for particular
wording. The 24 files hit on M149 were classified as:

- **register rows** — the two tables above, sourced from (roam-342 later added two hits, both register rows:
  `roamux/build/fieldtrial_inventory.py` restates the ADR 0002 obligation and
  `roamux/build/tests/test_fieldtrial_inventory.py` states the oracle-tag prerequisite): patch preambles `0065`, `0067`, `0068`, `0072`
  (patch `0010`'s "Re-check on uprev" is a comment inside its diff body, so it surfaces through its README row);
  `roamux/patches/README.md` rows `0010`, `0029`, `0039`, `0065`, `0067`, `0068`, `0072`;
  `roamux/app/resources/icons/mac/README.md`; `roamux/chromium_src/README.md`; ADR 0002;
  `roamux/test/roamux_test_env_browsertest.cc`; `roamux/test/support/roamux_browser_test.h`;
  `roamux/test/roamux_refresh_all_initial_urls_browsertest.mm`; `roamux/test/roamux_external_open_profile_browsertest.mm`;
- **protocol** — `BOOTSTRAP.md` (the fetch-and-checkout commands step 1 reuses; its other hit is a historical
  note on the first build's re-pin); `roamux/build/CHROMIUM_PIN` (the pin's own policy comment, step 2);
  `roamux/build/check_override_staleness.py` (step 4); ADR 0003 (step 9 — its per-re-pin obligation is the
  patch failures `apply_patches.py` already raises);
- **context** — `docs/adr/0001-chromium-overlay-strategy.md` (the re-pin policy);
  `roamux/build/tests/test_override_staleness.py` (the staleness gate's own tests, which simulate an uprev);
  `roamux/patches/README.md` rows `0059` (how the field-trial studies mask the collapse-action crash —
  covered by explicit-disable regression tests at any pin), `0060` (why widening the predicate absorbs callers
  upstream adds at future pin bumps) and `0062` (why `histograms.xml` is not patched: rebase cost);
- **self-maintaining** — `roamux/test/support/roamux_browser_test.cc` uses an upstream constant for its
  switch, so a rename is picked up automatically and a removal breaks the build loudly;
- **out of scope** — `roamux/third_party/sparkle/README.md` describes a *Sparkle* version uprev, a separate
  pin with its own procedure;
- **false positives** — tab-strip pinning (`roamux/browser/ui/views/tabs/tab_strip_pin_controller.cc`,
  `roamux/test/roamux_tab_strip_toggle_browsertest.mm`), feature-flag pinning in a fixture
  (`roamux/test/settled_visit_journal_service_unittest.cc`), and re-anchoring a workflow-text assertion
  (`roamux/build/tests/test_workflow_invariants.py`).

## Build cost model

Patched upstream files, especially widely included headers, cost rebuild time. Four cases behave differently:

| Case | Does a patched header force rebuilds? |
|---|---|
| **Local steady state** — base untouched between builds | No. `apply_patches.py` skips already-applied patches and nothing rewrites the files. |
| **Same-pin CI reconcile** — every tier-2 run, `out/CI` retained | **No** (since roam-341). The runhook's `--reconcile` leaves a patched path untouched when its existing regular-file entry already has the stack's bytes and mode; a changed hunk, a repaired stray edit, a missing file, a type or mode change, or a prior mutation is rewritten and invalidates its dependents — correctly. Before roam-341, `git reset --hard` plus re-apply gave every patched file a new timestamp on every run. |
| **Empty `out/`** | Everything compiles regardless. |
| **Pin change** | Large invalidation regardless of any one patch. Removing a patch cannot promise to avoid it, and removing one itself invalidates its dependents once. |

No per-patch share of the recurring cost has been measured; decisions about individual patches (such as
`0056`) should wait for a measurement rather than rest on an estimate.

## Rehearsal obligation

This document has not been proven on a new pin. roam-293 performs the first uprev: follow this procedure,
record where it was wrong or incomplete, and fix it in the same change. A [prospective] step that worked
becomes **[verified on M1xx]** with its date and the command result, in the form used in steps 2–4 and 8.
