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
studies' params. Resolved against compiled defaults, at least 518 forced-ON and 13
forced-OFF features were *effective* flips (116+13 directives unresolved by the textual
harvest — treated as potential flips; full three-bucket inventory attached to issue #241,
regenerate at every pin bump). This experiment soup silently changes at every Chromium pin
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
- Runtime verification on a packaged build (vertical-tabs surfaces: strip creation,
  collapse action, session restore, menus) is owned by the tracked follow-up recorded on
  issue #241, next-packaged-build timing — no packaged build exists in CI.
