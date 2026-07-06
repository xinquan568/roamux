<!-- SPDX-License-Identifier: Apache-2.0 -->
# `roamex/chromium_src` — upstream overrides (roam-2 / plan §12.2)

Whole-file / header **overrides** that shadow an upstream Chromium file via an include-path redirect.
Mirror the upstream path exactly, e.g.
`roamex/chromium_src/chrome/browser/ui/views/frame/browser_view_layout.cc`.

**Use sparingly** — an override carries a full copy of the upstream file (or the `#define`-redirect +
subclass trick at a class boundary) and is the **highest rebase-tax surface**. Prefer **additive `//roamex`
code**; use an override only when a whole file/header must be replaced, and record it in the §12.2
upstream-hook inventory. An **override-staleness check** (roam-2) flags overrides whose upstream signature
changed on a Chromium uprev.
