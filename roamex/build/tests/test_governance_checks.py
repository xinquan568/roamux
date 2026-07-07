# SPDX-License-Identifier: Apache-2.0
"""Hermetic tests for the governance checkers (roam-38) — the single source of truth the hooks AND CI
both invoke. No Chromium checkout, no external deps.
"""

import pathlib
import subprocess
import sys
import tempfile
import unittest

CHECKS = pathlib.Path(__file__).resolve().parents[3] / "scripts" / "checks"


def run_check(script, *args, stdin=None):
    return subprocess.run([sys.executable, str(CHECKS / script), *args],
                          capture_output=True, text=True, input=stdin)


class TmpTree(unittest.TestCase):
    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp(prefix="roamex-gov-"))
        self.addCleanup(__import__("shutil").rmtree, self.tmp, ignore_errors=True)

    def write(self, rel, text):
        f = self.tmp / rel
        f.parent.mkdir(parents=True, exist_ok=True)
        f.write_text(text)
        return str(f)


class SpdxHeaderTest(TmpTree):
    def test_missing_header_rejected(self):
        f = self.write("roamex/common/x.cc", "int main() { return 0; }\n")
        r = run_check("spdx_header.py", f)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("x.cc", r.stdout + r.stderr)

    def test_present_header_accepted(self):
        f = self.write("roamex/common/x.cc", "// SPDX-License-Identifier: Apache-2.0\nint main(){}\n")
        self.assertEqual(run_check("spdx_header.py", f).returncode, 0)

    def test_chromium_src_copy_exempt(self):
        # A full upstream copy keeps the BSD header and must NOT be required to carry ours.
        f = self.write("roamex/chromium_src/chrome/x.h", "// Copyright The Chromium Authors\n")
        self.assertEqual(run_check("spdx_header.py", f).returncode, 0)

    def test_markdown_doc_exempt(self):
        f = self.write("README.md", "# Roamex\nno header here\n")
        self.assertEqual(run_check("spdx_header.py", f).returncode, 0)

    def test_json_data_exempt(self):
        f = self.write("roamex/build/override_signatures.json", '{"pin":"x"}\n')
        self.assertEqual(run_check("spdx_header.py", f).returncode, 0)


class SecretScanTest(TmpTree):
    def test_aws_key_rejected(self):
        # A realistic key (not the documented ...EXAMPLE fake, which is legitimately allowlisted).
        f = self.write("x.py", "KEY = 'AKIAZ7XQ2WPL4NR8YT3D'\n")  # roamex:allow-secret (fixture)
        r = run_check("secret_scan.py", f)
        self.assertNotEqual(r.returncode, 0)

    def test_documented_example_key_allowlisted(self):
        # The AWS-docs fake key must NOT trip the scanner.
        f = self.write("x.py", "KEY = 'AKIAIOSFODNN7EXAMPLE'\n")  # roamex:allow-secret (fixture)
        self.assertEqual(run_check("secret_scan.py", f).returncode, 0)

    def test_pem_private_key_rejected(self):
        f = self.write("x.pem", "-----BEGIN OPENSSH PRIVATE KEY-----\nabc\n")  # roamex:allow-secret (fixture)
        self.assertNotEqual(run_check("secret_scan.py", f).returncode, 0)

    def test_keys_template_placeholder_allowlisted(self):
        f = self.write("roamex/build/google_keys.gni.template",
                       '# google_api_key = "<your key>"\n')
        self.assertEqual(run_check("secret_scan.py", f).returncode, 0)


    def test_allow_marker_suppresses(self):
        f = self.write("x.py", "KEY = 'AKIAZ7XQ2WPL4NR8YT3D'  # roamex:allow-secret\n")
        self.assertEqual(run_check("secret_scan.py", f).returncode, 0)


class CommitMsgTest(TmpTree):
    def test_conventional_accepted(self):
        f = self.write("m", "feat(prefs): add the thing\n")
        self.assertEqual(run_check("commit_msg.py", f).returncode, 0)

    def test_valid_subject_without_roam_n_accepted(self):
        # roam-38 checks SYNTAX ONLY — no roam-N linkage (that is roam-39's gate).
        f = self.write("m", "chore: tidy up\n")
        self.assertEqual(run_check("commit_msg.py", f).returncode, 0)

    def test_non_conventional_rejected(self):
        for bad in ("wip\n", "updated stuff\n", "Fixed the bug\n"):
            f = self.write("m", bad)
            self.assertNotEqual(run_check("commit_msg.py", f).returncode, 0, bad)


class OverlayStructureTest(TmpTree):
    def test_bad_patch_name_rejected(self):
        f = self.write("roamex/patches/bad name.patch", "diff\n")
        self.assertNotEqual(run_check("overlay_structure.py", f).returncode, 0)

    def test_good_patch_name_accepted(self):
        f = self.write("roamex/patches/0005-ok.patch", "diff\n")
        self.assertEqual(run_check("overlay_structure.py", f).returncode, 0)


if __name__ == "__main__":
    unittest.main()
