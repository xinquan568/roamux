<!-- SPDX-License-Identifier: Apache-2.0 -->
# `roamex/patches` — minimal upstream patches (roam-2 / plan §12.2)

In-function hook points and generated-table edits that **neither** an additive `//roamex` file **nor** a
`chromium_src` override can reach — e.g. a one-line `ROAMEX_*` macro insert, an `IDC_*` command-id enum
entry, a macOS accelerator table row, `RegisterProfilePrefs`, or a `BrowsingDataRemover` registration.

## The runhook (roam-2)

```sh
python3 roamex/build/apply_patches.py --chromium-src ~/chromium/src            # apply (idempotent)
python3 roamex/build/apply_patches.py --chromium-src ~/chromium/src --check   # verify, no mutation
```

Patches apply in **name order** (`NNNN-slug.patch`, `git diff` format). Already-applied patches are
skipped (`[applied]`); one that neither is applied nor applies cleanly **fails loudly naming the patch**
(§12.5 — the rebase signal). §12.4's target state invokes this at `gclient sync` time; until then it
runs manually / from the build gate.

## Current inventory

| Patch | Kind | What it does |
|---|---|---|
| `0001-gn-all-add-roamex-targets.patch` | **persistent** | wires `//roamex` + the `//roamex:roamex_tests` group into `gn_all` (§12.4) — new test targets join the group, never this patch |
| `0002-chromium-src-include-redirect.patch` | **persistent** | enables `chromium_src` shadowing (one `include_dirs` line, §12.2 mechanism 2) |
| `0003-sample-marker.patch` | **sample** | one-line inert marker proving the runhook end-to-end (roam-2 test) |
| `0004-register-profile-prefs.patch` | **persistent** | the §12.2 registrar hook (roam-3): `roamex::prefs::RegisterProfilePrefs` call + include in `browser_prefs.cc`, plus the `//roamex/common` dep edge on its owning GN target |

Each patch is **tiny, reviewed, and fails loudly on rebase**. Keep the surface minimal — it is the
rebase-cost surface tracked by §7.6/§12.5; the authoritative hook inventory is plan **§12.2**.
