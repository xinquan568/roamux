<!-- SPDX-License-Identifier: Apache-2.0 -->
# `roamex/patches` — minimal upstream patches (roam-2 / plan §12.2)

In-function hook points and generated-table edits that **neither** an additive `//roamex` file **nor** a
`chromium_src` override can reach — e.g. a one-line `ROAMEX_*` macro insert inside `BrowserViewLayout::Layout()`,
an `IDC_*` command-id enum entry, a macOS accelerator table row, the `RegisterProfilePrefs` call
(`roamex::prefs::RegisterProfilePrefs`), or a `BrowsingDataRemover` data-type registration.

Each patch is **tiny, reviewed, and fails loudly on rebase**. Applied by a `gclient` runhook at sync time.
Keep the patch surface minimal — it is the rebase-cost surface tracked by §7.6/§12.5. The full inventory of
which hook is additive vs. `chromium_src` vs. patch lives in plan **§12.2**.
