#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Versioned exclusion policy for the Roamux string-rebrand channel (roam-132).

Single source of truth imported by BOTH rebrand_strings.py and its tests, so
what stays "Chromium" is declared in exactly one place. Two layers:

  1. EXCLUDED_MESSAGE_NAMES — grd ``<message name=...>`` whose whole content must
     never rebrand (legal attribution / copyright / OS license). The channel
     skips these messages entirely: no text edit, no xtb re-key.

  2. guarded_substitute() — the word-boundary Chromium->Roamux (and
     chromium->roamux, for user-visible lowercase) token pass, with in-content
     VETO guards so attribution phrases, the ChromeOS product name, domains,
     chrome:// URLs and code/histogram/policy identifiers keep "Chromium" even
     inside an otherwise-rebranded message.

Schema / governance: bump VERSION on ANY change to the sets or the guard regex,
and pin the new/changed case with a fixture in test_rebrand_strings.py. The
guards are deliberately conservative — a missed rebrand leaves a stale
"Chromium" (cosmetic, caught by the release gate); a wrong rebrand corrupts
legal text (unacceptable), so when in doubt the guard KEEPS "Chromium".
"""

import re

# v2 (roam-132 review): added the components_chromium_strings.grd About/version
# license-attribution message names (IDS_VERSION_UI_LICENSE*).
VERSION = 2

# Layer 1: message names that must never rebrand — they carry legal/attribution
# text about the upstream Chromium PROJECT (not the Roamux product), so the whole
# message stays "Chromium". These live in the target *_strings.grd units.
EXCLUDED_MESSAGE_NAMES = frozenset({
    # chrome/app/chromium_strings.grd — copyright / company / OS license.
    "IDS_ABOUT_VERSION_COMPANY_NAME",   # "The Chromium Authors"
    "IDS_ABOUT_VERSION_COPYRIGHT",      # "Copyright <YEAR> The Chromium Authors. ..."
    "IDS_ABOUT_CROS_VERSION_LICENSE",   # ChromeOS open-source license blurb
    "IDS_ABOUT_VERSION_LICENSE",        # open-source license blurb (defensive)
    "IDS_ABOUT_VERSION_LICENSE_EULA",   # EULA (defensive)
    # components/components_chromium_strings.grd — the About/version license labels:
    # "Chromium is made possible by the Chromium open source project ..." (the
    # attribution links to the Chromium project itself and must read "Chromium").
    "IDS_VERSION_UI_LICENSE",           # "... made possible by the Chromium open source project and other ..."
    "IDS_VERSION_UI_LICENSE_CHROMIUM",  # "... made possible by the Chromium open source project."
    "IDS_VERSION_UI_LICENSE_OTHER",     # "... also made possible by other open source software."
})

# Layer 2a: the rebrandable token. A leading guard keeps the match off dotted /
# slashed / @'d / scheme'd identifiers (org.chromium.foo, //chromium, a@chromium,
# chrome://...). The trailing ``\b`` naturally excludes "ChromiumOS" (no word
# boundary before the "OS"), so only "Chromium OS" (with a space) needs a veto.
_TOKEN = re.compile(r'(?<![\w./@:])(?P<w>Chromium|chromium)\b')

# Layer 2b: text immediately AFTER a matched token that means "keep Chromium".
_VETO_AFTER = re.compile(
    r'''^(?:
          \ OS\b                 # "Chromium OS" — the ChromeOS product name
        | \ Authors\b            # "The Chromium Authors" — attribution
        | \ open\ source         # "Chromium open source project" — attribution
        | \.[A-Za-z0-9]          # chromium.org / Chromium.Histogram / dotted id
        | ://                    # scheme boundary (defensive)
    )''',
    re.VERBOSE)


def is_message_excluded(name):
    """True when a grd message (by name) must never be rebranded."""
    return name in EXCLUDED_MESSAGE_NAMES


def guarded_substitute(text):
    """Rebrand user-visible Chromium->Roamux / chromium->roamux in ``text``.

    Operates only on the substring handed in (the caller restricts this to
    translatable message content / xtb translation text — never attributes,
    placeholder names, comments or ids). Returns ``(new_text, changed_bool)``.
    """
    def repl(m):
        if _VETO_AFTER.match(text[m.end():]):
            return m.group('w')
        return 'Roamux' if m.group('w') == 'Chromium' else 'roamux'

    new = _TOKEN.sub(repl, text)
    return new, (new != text)
