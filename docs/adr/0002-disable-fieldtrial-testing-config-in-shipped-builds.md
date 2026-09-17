<!-- SPDX-License-Identifier: Apache-2.0 -->
# 0002. Disable the fieldtrial testing config in shipped builds

## Status
Accepted (roam-241, 2026-07-30 — maintainer decision recorded on issue #241)

## Context

Chromium compiles `testing/variations/fieldtrial_testing_config.json` into any build where
(`components/variations/service/BUILD.gn`):

```gn
fieldtrial_testing_enabled =
    force_enable_fieldtrial_testing_config ||
    (!disable_fieldtrial_testing_config && !(is_android && is_chrome_branded))
```

Roamux ships unbranded macOS builds, so `is_official_build` is irrelevant to this gate and
shipped users ran whatever experiment groups the testing config picked: at pin M149/mac,
653 studies issuing 747 force-enable and 47 force-disable feature directives plus 197
studies' params. Resolved against compiled defaults: forced-ON 747 = **518
effective flips / 113 no-ops / 116 unresolved**; forced-OFF 47 = **13 effective flips /
21 no-ops / 13 unresolved** (unresolved = conditional/macro-defined defaults the textual
harvest cannot resolve — treated as potential flips; full three-bucket inventory attached
to issue #241, regenerate at every pin bump). This experiment soup silently changes at every Chromium pin
and is exactly what masked the E1 read-side gate-mismatch family: roam-234's startup crash
and roam-239's ten bare-predicate call sites were invisible until fixtures explicitly
disabled the studies. Chromium derivatives conventionally disable the testing config so
shipped behavior is deterministic.

Prerequisites, deliberately sequenced: roam-239 (the widened
`IsVerticalTabsFeatureEnabled()` — without it this flip would have shipped the read-side
inconsistency) and roam-240 (the test-harness de-mask — after which the full overlay
browser-test sweep already runs, green, on exactly the post-flip flag reality).

## Decision

We will set `disable_fieldtrial_testing_config = true` in **both** shipped-args templates —
`roamux/build/args/release.gn` and `roamux/build/args/reference.gn` — so dev/CI builds
follow shipped (one flag reality everywhere; tests are covered separately by roam-240's
harness switch). Upstream features Roamux wants ON are enabled deliberately (flags,
prefs, or explicit args), never inherited from the experiment config.

The single arg per file is the revert point. The hermetic invariant suite
(`roamux/build/tests/test_gn_args.py`) pins the assignment present-unique-uncommented in
both files.

## Consequences

- Shipped, dev, CI, and test builds all run the same flag baseline: compiled defaults plus
  deliberate overrides. A pin bump dropping or adding studies can no longer flip shipped
  behavior implicitly — completing the three-layer de-masking this chain built (roam-239
  product correctness, roam-240 test reality, roam-241 shipped reality).
- Users lose ~518 (+up to 116 unresolved) study-enabled features and 197 param sets
  relative to previous shipped builds; the per-pin inventory on issue #241 is the record
  of what changed. Any regression report traceable to a reverted feature has a one-line
  diagnosis path (was it in the inventory?) and, if wanted, a deliberate re-enable path.
- **Obligation**: regenerate the inventory (one script over the config JSON + a
  `BASE_FEATURE` harvest) at every Chromium pin bump and attach it to the uprev record.
  The script is `roamux/build/fieldtrial_inventory.py` (roam-342); `docs/uprev.md` step 8
  is the procedure.
- **Methodology note (roam-342, 2026-09-17).** The numbers above are what the committed
  generator's `textual-v1` rule produces, and the generator's acceptance test reproduces them
  exactly from the pristine M149 tag; they are unchanged. They are **textual-harvest
  classifications, not verified macOS compiled behaviour**: the harvest matches only the
  literal `base::FEATURE_ENABLED_BY_DEFAULT` / `base::FEATURE_DISABLED_BY_DEFAULT` token with
  whitespace-only argument lists, and the first match in path-then-text order wins. So
  (a) a bare `FEATURE_DISABLED_BY_DEFAULT` (inside `namespace base`) or a
  `base::FeatureState::` spelling counts as *unresolved* although its default is literal;
  (b) a `#if` inside the macro's argument list counts as *unresolved*; (c) a
  platform-conditional duplicate resolves to the textually-first branch, which can be a
  non-mac branch — some "effective flips" and "no-ops" are therefore false; (d) test
  sources are scanned. *Unresolved* remains "treat as a potential flip". Improving the rule
  would move roughly thirty names between buckets and is a separate change that amends
  these numbers; it must not happen silently.
- Runtime verification on a packaged build (vertical-tabs surfaces: strip creation,
  collapse action, session restore, menus) is owned by the tracked follow-up recorded on
  issue #241, next-packaged-build timing — no packaged build exists in CI.
