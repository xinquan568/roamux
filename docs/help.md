<!-- SPDX-License-Identifier: Apache-2.0 -->
# Getting help with Roamux

Roamux is a Chromium-based browser for macOS, built as a small overlay
(`src/roamux`) over upstream Chromium. It adds tab-workflow features (vertical
tab strip placement, durable tab identity, settled-visit navigation), an Edge
profile importer, and Sparkle-based updates, while staying as close to stock
Chromium as possible.

This page is the in-product help target — the "Get help with Roamux" row on
`chrome://settings/help` lands here (roam-161).

## Reporting a problem

Open an issue: **https://github.com/xinquan568/roamux/issues**

Please include:

- the Roamux version from `chrome://settings/help` (both lines — the Roamux
  marketing version and the Chromium base), and
- for update problems, the dimmed grey detail line under the update status (it
  carries the raw Sparkle error while Roamux is in alpha — that line exists
  precisely to make your report actionable).

## Known limitations of the personal-alpha distribution

- **Unsigned:** builds are not Apple-signed or notarized (a deliberate
  personal-alpha decision). macOS Gatekeeper will warn on first launch;
  right-click → Open, or `xattr -dr com.apple.quarantine Roamux.app`.
- **Quarantined documents will not open through Roamux:** with the current
  unsigned, un-notarized builds, a quarantined file — one that arrived by
  AirDrop or download — is refused by Gatekeeper when Roamux is the app
  opening it. The alert names the **document**, not Roamux, so it can look
  like a problem with the document itself; and unlike the first-launch warning
  above it offers no "Open Anyway", only *Move to Trash* or *Done*. The
  right-click → Open escape does not apply here.

  Choose **Done** to leave the file where it is — do **not** choose *Move to
  Trash*. Then clear the quarantine flag from the **document itself** — no
  `-r`, one file at a time. (The recursive `Roamux.app` command above is for
  the application bundle, not for documents.)

  ```sh
  xattr -d com.apple.quarantine "$HOME/Downloads/file.pdf"
  ```

  Substitute your own filename and keep the quotes — document paths often
  contain spaces. This bypasses Gatekeeper's quarantine assessment for that
  file, so use it only if you trust the source. (Other macOS protections, such
  as XProtect, are unaffected.) To avoid the prompt in the first place, keep a
  notarized browser as the default handler for these file types and open local
  files from inside Roamux, rather than ticking *Always Open With → Roamux*
  for downloaded files.
- **Updates:** delivered via Sparkle from the project's appcast. Update
  packages are EdDSA-signed; there is no Mac App Store channel.
- **macOS only**, Apple silicon first.

## More

- [README](../README.md) — project overview and goals
- [CONTRIBUTING](../CONTRIBUTING.md) — building, the overlay/patch discipline,
  and the roam-N issue convention
