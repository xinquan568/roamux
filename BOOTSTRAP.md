<!-- SPDX-License-Identifier: Apache-2.0 -->
# Roamex — bootstrap & build (E0 foundation)

Roamex is a **Chromium/C++ overlay** — the code in this repo (`roamex/`) is layered onto an upstream
Chromium checkout; the Chromium source is **not** committed here (it's fetched with `depot_tools`).
See the execution plan §11–§12 (`docs/discussion/2026-07-05-roamex-browser-features/FINAL.html`).

## Prerequisites (build machine)

| Need | Why |
|---|---|
| **≥ ~150 GB free disk** | Chromium checkout (~40 GB) + build output (~60–100 GB). **This is the gating requirement.** |
| **Xcode (FULL — not just CLT) + Command Line Tools** | Chromium's macOS build runs `xcodebuild`, which the Command Line Tools alone do **not** provide (`gn gen` fails at `sdk_info.py` otherwise). Install full Xcode from the App Store or developer.apple.com, then: `sudo xcode-select -s /Applications/Xcode.app/Contents/Developer && sudo xcodebuild -license accept && sudo xcodebuild -runFirstLaunch`. Verify: `xcodebuild -version`. |
| **`depot_tools` on `PATH`** | `gclient`, `gn`, `autoninja`, `fetch`. Installed by the bootstrap step below. |
| **macOS on Apple silicon** | Target is macOS-only (universal2 later; §11.10). |

## 1. depot_tools (done once)

```sh
git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git ~/depot_tools
echo 'export PATH="$HOME/depot_tools:$PATH"' >> ~/.zshrc && source ~/.zshrc
gclient --version    # sanity
```
> In this workspace, depot_tools was cloned to `./depot_tools` and added to `~/.zshrc`. `gn`/`autoninja`
> resolve to the copy Chromium vendors into `src/` after the first sync — until then `gn --version` on the
> bare `depot_tools` reports "not found", which is expected.

## 2. Fetch Chromium (heavy — ~40 GB, hours; run OUTSIDE this repo)

```sh
mkdir -p ~/chromium && cd ~/chromium
fetch --no-history chromium          # downloads the tree, runs the first gclient sync
cd src
git fetch --tags
git checkout <LATEST_STABLE_MILESTONE_TAG>   # Q(i4)-A: latest stable milestone; re-pin every cycle
gclient sync -D                      # sync deps to the pinned tag; vendors gn/ninja
```
Record the exact tag in `roamex/build/CHROMIUM_PIN` when you pick it.

## 3. Place the Roamex overlay into the Chromium tree + wire it into the build graph

The overlay is `codes/roamex/roamex/` → it must appear as `src/roamex/` in the checkout, **and** a root
target must reference it (GN only loads `//roamex/BUILD.gn` if something depends on it):
```sh
ln -s /abs/path/to/codes/roamex/roamex ~/chromium/src/roamex
# The §12.4 top-level hook (confirmed): adds //roamex + //roamex:roamex_unittests to gn_all (is_mac).
git -C ~/chromium/src apply /abs/path/to/codes/roamex/roamex/patches/0001-gn-all-add-roamex-targets.patch
```
(A symlink keeps the git repo as the source of truth; a `gclient` custom-solution/DEPS entry is the
alternative once the overlay stabilizes. The `patches/` entry becomes a `gclient` runhook later, §12.5.)

## 4. Configure + build (targeted — do NOT build `all`)

```sh
cd ~/chromium/src
gn gen out/Default --args="$(tr '\n' ' ' < roamex/build/args/reference.gn)"
autoninja -C out/Default roamex_unittests   # the E0 hello-world test target (roam-1); first build is hours
out/Default/roamex_unittests                # should pass
```
> **First-run validation:** wiring `//roamex` into Chromium's build graph may need a small top-level hook
> (a `deps` entry so GN reaches `//roamex`, plan §12.4). Validate/adjust on the first `gn gen`; capture the
> exact steps back into this file. Full Chromium builds are hours — always build the **touched target + its
> test target**, never `all` (per the CI trust tiers, §12.6).

## Status of this repo (E0 scaffolding)

Progress on the bootstrap machine so far:
- ✅ depot_tools installed; Chromium **fetched** (checked out `152.0.7936.0` — this is `main`/tip; **re-pin to
  the latest *stable* milestone tag** per Q(i4)-A and record it in `roamex/build/CHROMIUM_PIN`).
- ✅ Overlay symlinked into `src/roamex`; the `//roamex` build-graph wiring found and captured as
  `roamex/patches/0001-gn-all-add-roamex-targets.patch`.
- ⛔ **Blocked at `gn gen`: full Xcode is missing** (only the Command Line Tools are installed, so `xcodebuild`
  fails). **Reinstall full Xcode** (prereqs table) and re-run steps 4 → the build. `roam-1` acceptance
  (`autoninja roamex_unittests` green) is pending that.

Once it builds green, E7 (governance) and the feature epics can run via `/issue2pr chain --label <epic>`.
