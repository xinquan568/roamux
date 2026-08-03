# SPDX-License-Identifier: Apache-2.0
"""Hermetic tests for the roam-33 release signing/packaging logic — no Apple
credentials, no real codesign (that is the release-job E2E). Covers the pure
seams: the signing-mode gate, the inside-out combined sign order, Sparkle
nested-code discovery, and packaging symlink/exec-bit preservation."""

import contextlib
import importlib
import inspect
import io
import os
import pathlib
import plistlib
import re
import stat
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

REL = pathlib.Path(__file__).resolve().parent.parent.parent / "app" / "release"
sys.path.insert(0, str(REL))

import package_roamux  # noqa: E402
import rename_bundle  # noqa: E402
import signing_mode  # noqa: E402
import roamux_signing_config  # noqa: E402
import signing_plan  # noqa: E402
import sign_roamux  # noqa: E402


# ---------------------------------------------------------------------------
# roam-97: checkout-backed signing-package gate. The parts-path + config-seam
# tests exercise Chromium's real `signing.*` package. Tier-1 CI has no checkout,
# so those tests SKIP there. A runner WITH the checkout sets
# REQUIRE_SIGNING_PARTS=1 to turn the skip into a HARD RUN (mirrors roam-132's
# REQUIRE_GRIT). The checkout-free config-shape/derivation tests always run.
# ---------------------------------------------------------------------------
def _resolve_chromium_src():
    src = os.environ.get("ROAMUX_CHROMIUM_SRC") or os.path.expanduser(
        "~/chromium/src")
    return pathlib.Path(src)


CHROMIUM_SRC = _resolve_chromium_src()
_SIGNING_MAC = CHROMIUM_SRC / "chrome" / "installer" / "mac"


def _signing_skip_reason():
    """None when Chromium's signing package imports, else an honest reason."""
    if not (_SIGNING_MAC / "signing" / "parts.py").is_file():
        return ("no Chromium signing package at {} (set ROAMUX_CHROMIUM_SRC)"
                .format(_SIGNING_MAC))
    if str(_SIGNING_MAC) not in sys.path:
        sys.path.insert(0, str(_SIGNING_MAC))
    try:
        for m in ("signing.parts", "signing.config", "signing.model",
                  "signing.config_factory", "signing.driver",
                  "signing.pipeline"):
            importlib.import_module(m)
    except Exception as e:  # noqa: BLE001 — any import failure is an honest skip
        return "signing package not importable from {}: {}".format(
            _SIGNING_MAC, e)
    return None


SIGNING_SKIP = _signing_skip_reason()
REQUIRE_SIGNING_PARTS = os.environ.get("REQUIRE_SIGNING_PARTS") == "1"
_SKIP_PARTS = bool(SIGNING_SKIP) and not REQUIRE_SIGNING_PARTS


def _import_signing():
    """Import and return (parts, config, model) from the real checkout."""
    _signing_skip_reason()  # ensures _SIGNING_MAC is on sys.path
    return (importlib.import_module("signing.parts"),
            importlib.import_module("signing.config"),
            importlib.import_module("signing.model"))


def _make_chromium_base(config_mod):
    """A concrete Chromium-like base over the REAL `signing.config.CodeSignConfig`
    with the unbranded values (product == app_product == "Chromium"). No-arg
    friendly (stub invoker + ad-hoc identity) so get_parts can be driven without
    a build; Model B's RoamuxCodeSignConfig subclasses this."""

    class _ChromiumLikeBase(config_mod.CodeSignConfig):
        def __init__(self, **kwargs):
            kwargs.setdefault("invoker", lambda cfg: None)
            kwargs.setdefault("identity", "-")
            super().__init__(**kwargs)

        @staticmethod
        def is_chrome_branded():
            return False

        @property
        def enable_updater(self):
            return False

        @property
        def app_product(self):
            return "Chromium"

        @property
        def product(self):
            return "Chromium"

        @property
        def version(self):
            return "149.0.7827.201"

        @property
        def base_bundle_id(self):
            return "org.chromium.Chromium"

    return _ChromiumLikeBase


class _StubChromiumBase:
    """Mirrors ONLY the path-deriving computed properties of Chromium's
    signing/config.py CodeSignConfig, so Model-B derivation can be checked
    checkout-free. `product` is the real unbranded value "Chromium"."""

    @property
    def product(self):
        return "Chromium"

    @property
    def app_product(self):
        return "Chromium"

    @property
    def base_bundle_id(self):
        return "org.chromium.Chromium"

    @property
    def app_dir(self):  # config.py: '{.app_product}.app'
        return "{.app_product}.app".format(self)

    @property
    def framework_dir(self):  # config.py formula
        return ("{0.app_dir}/Contents/Frameworks/"
                "{0.product} Framework.framework").format(self)


def _make_renamed_bundle(root):
    """Build an on-disk bundle matching rename_bundle.py's Model-B output: the
    outer app is Roamux.app; the inner framework + helper apps keep their
    Chromium names. Returns the Roamux.app path."""
    app = root / "Roamux.app"
    fw = app / "Contents" / "Frameworks" / "Chromium Framework.framework"
    helpers = fw / "Helpers"
    libs = fw / "Libraries"
    (app / "Contents" / "MacOS").mkdir(parents=True)
    _exe(app / "Contents" / "MacOS" / "Roamux")
    helpers.mkdir(parents=True)
    libs.mkdir(parents=True)
    for h in ("Chromium Helper.app", "Chromium Helper (Renderer).app",
              "Chromium Helper (GPU).app", "Chromium Helper (Alerts).app"):
        (helpers / h / "Contents" / "MacOS").mkdir(parents=True)
        _exe(helpers / h / "Contents" / "MacOS" / h[:-len(".app")])
    for f in ("chrome_crashpad_handler", "app_mode_loader",
              "web_app_shortcut_copier"):
        _exe(helpers / f)
    for lib in ("libEGL.dylib", "libGLESv2.dylib", "libvk_swiftshader.dylib"):
        _exe(libs / lib)
    return app


SECRETS = [
    "ROAMUX_DEVELOPER_ID_CERT_P12",
    "ROAMUX_DEVELOPER_ID_CERT_PASSWORD",
    "ROAMUX_NOTARY_KEY_ID",
    "ROAMUX_NOTARY_ISSUER_ID",
    "ROAMUX_NOTARY_PRIVATE_KEY",
]


class SigningModeTest(unittest.TestCase):
    def test_all_secrets_is_signed(self):
        env = {k: "x" for k in SECRETS}
        self.assertEqual(signing_mode.resolve_signing_mode(env), "signed")

    def test_no_secrets_is_unsigned(self):
        self.assertEqual(signing_mode.resolve_signing_mode({}), "unsigned")

    def test_partial_secrets_fail_fast_naming_missing(self):
        env = {SECRETS[0]: "x", SECRETS[1]: "x"}  # cert but no notary
        with self.assertRaises(signing_mode.PartialSigningSecretsError) as ctx:
            signing_mode.resolve_signing_mode(env)
        msg = str(ctx.exception)
        self.assertIn("ROAMUX_NOTARY_KEY_ID", msg)
        self.assertNotIn("ROAMUX_DEVELOPER_ID_CERT_P12", msg)

    def test_blank_values_count_as_absent(self):
        env = {k: "" for k in SECRETS}
        self.assertEqual(signing_mode.resolve_signing_mode(env), "unsigned")


class SparkleDiscoveryTest(unittest.TestCase):
    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp(prefix="roamux-fw-"))
        self.addCleanup(_rmtree, self.tmp)
        fw = self.tmp / "Sparkle.framework" / "Versions" / "B"
        (fw / "XPCServices" / "Installer.xpc" / "Contents" / "MacOS").mkdir(
            parents=True)
        (fw / "XPCServices" / "Downloader.xpc" / "Contents" / "MacOS").mkdir(
            parents=True)
        (fw / "Updater.app" / "Contents" / "MacOS").mkdir(parents=True)
        _exe(fw / "XPCServices" / "Installer.xpc" / "Contents" / "MacOS" / "Installer")
        _exe(fw / "XPCServices" / "Downloader.xpc" / "Contents" / "MacOS" / "Downloader")
        _exe(fw / "Updater.app" / "Contents" / "MacOS" / "Updater")
        _exe(fw / "Autoupdate")
        _exe(fw / "Sparkle")
        self.framework = self.tmp / "Sparkle.framework"

    def test_discovers_all_nested_code_deepest_first(self):
        parts = signing_plan.discover_sparkle_parts(self.framework)
        names = [p.name for p in parts]
        for n in ("Installer.xpc", "Downloader.xpc", "Updater.app",
                  "Autoupdate"):
            self.assertIn(n, names, f"{n} not discovered")
        # Deepest-first: nested code precedes the framework itself.
        self.assertEqual(parts[-1].name, "Sparkle.framework")

    def test_autoupdate_must_be_planned(self):
        # A plan missing the loose Autoupdate executable is rejected.
        plan = [p for p in signing_plan.discover_sparkle_parts(self.framework)
                if p.name != "Autoupdate"]
        with self.assertRaises(signing_plan.UnplannedSparkleCodeError):
            signing_plan.assert_sparkle_fully_planned(self.framework, plan)

    def test_full_discovery_satisfies_the_plan_check(self):
        parts = signing_plan.discover_sparkle_parts(self.framework)
        signing_plan.assert_sparkle_fully_planned(self.framework, parts)

    def test_missing_nested_code_fails_the_plan_check(self):
        plan = [self.framework]  # framework only, XPC/app omitted
        with self.assertRaises(signing_plan.UnplannedSparkleCodeError):
            signing_plan.assert_sparkle_fully_planned(self.framework, plan)


class RoamuxSigningConfigTest(unittest.TestCase):
    def test_config_is_model_b_outer_only_rebrand(self):
        # roam-97 Model B: only the OUTER app rebrands. app_product -> "Roamux"
        # and base_bundle_id -> "com.roamux.Roamux", but `product` is INHERITED
        # from the base (unbranded Chromium == "Chromium") and MUST NOT be
        # overridden — else get_parts() would look for nonexistent "Roamux
        # Framework.framework"/"Roamux Helper*.app".
        cls = roamux_signing_config.make_roamux_config_class(_StubChromiumBase)
        cfg = cls()
        self.assertEqual(cfg.app_product, "Roamux")
        self.assertEqual(cfg.base_bundle_id, "com.roamux.Roamux")
        self.assertEqual(cfg.product, "Chromium", "product must be inherited")
        self.assertNotEqual(cfg.product, "Roamux",
                            "regression: product must NOT be overridden (Model B)")

    def test_get_parts_injects_sparkle_before_outer_app(self):
        keys = ["chromium_framework", "chromium_helper", "app"]  # app last
        ordered = roamux_signing_config.roamux_get_parts({}, keys)
        self.assertEqual(ordered[-1], "app")
        sparkle = [k for k in ordered if k.startswith("sparkle:")]
        self.assertTrue(sparkle, "no Sparkle parts injected")
        for k in sparkle:
            self.assertLess(ordered.index(k), ordered.index("app"))
        # Autoupdate + the framework are represented.
        self.assertIn("sparkle:Autoupdate", ordered)
        self.assertIn("sparkle:Sparkle.framework", ordered)

    def test_load_base_returns_none_without_chromium_checkout(self):
        # No signing package on a bare dir -> None (plan-preview fallback).
        tmp = pathlib.Path(tempfile.mkdtemp(prefix="roamux-noc-"))
        self.addCleanup(_rmtree, tmp)
        self.assertIsNone(
            roamux_signing_config.load_chromium_config_base(tmp))

    def test_config_class_reports_roamux_over_a_stub_chromium_base(self):
        class StubChromiumConfig:
            @property
            def app_product(self):
                return "Chromium"
        cls = roamux_signing_config.make_roamux_config_class(StubChromiumConfig)
        self.assertEqual(cls().app_product, "Roamux")


class CombinedOrderTest(unittest.TestCase):
    def test_outer_app_is_last_no_nested_after(self):
        chromium_parts = ["app/Contents/Frameworks/Chromium Framework.framework",
                          "app/Contents/Frameworks/Chromium Helper.app"]
        sparkle_parts = ["Sparkle.framework/Versions/B/XPCServices/Installer.xpc",
                         "Sparkle.framework"]
        outer = "app"
        plan = signing_plan.combined_sign_plan(chromium_parts, sparkle_parts,
                                               outer)
        self.assertEqual(plan[-1], outer)
        outer_idx = plan.index(outer)
        for p in chromium_parts + sparkle_parts:
            self.assertLess(plan.index(p), outer_idx,
                            f"{p} must be signed before the outer app")


class RenameBundleTest(unittest.TestCase):
    def test_chromium_app_becomes_roamux_app(self):
        import plistlib
        tmp = pathlib.Path(tempfile.mkdtemp(prefix="roamux-rn-"))
        self.addCleanup(_rmtree, tmp)
        contents = tmp / "Chromium.app" / "Contents"
        (contents / "MacOS").mkdir(parents=True)
        _exe(contents / "MacOS" / "Chromium")
        with open(contents / "Info.plist", "wb") as f:
            plistlib.dump({"CFBundleExecutable": "Chromium",
                           "CFBundleName": "Chromium",
                           "CFBundleIdentifier": "org.chromium.Chromium"}, f)
        new = rename_bundle.rename_bundle(tmp / "Chromium.app")
        self.assertEqual(new.name, "Roamux.app")
        self.assertTrue((new / "Contents" / "MacOS" / "Roamux").is_file())
        with open(new / "Contents" / "Info.plist", "rb") as f:
            plist = plistlib.load(f)
        self.assertEqual(plist["CFBundleExecutable"], "Roamux")
        self.assertEqual(plist["CFBundleName"], "Roamux")
        self.assertEqual(plist["CFBundleIdentifier"], "com.roamux.Roamux")

    def _make_chromium_app(self, tmp):
        import plistlib
        contents = tmp / "Chromium.app" / "Contents"
        (contents / "MacOS").mkdir(parents=True)
        _exe(contents / "MacOS" / "Chromium")
        with open(contents / "Info.plist", "wb") as f:
            plistlib.dump({"CFBundleExecutable": "Chromium",
                           "CFBundleName": "Chromium",
                           "CFBundleIdentifier": "org.chromium.Chromium",
                           "CFBundleVersion": "7827.201",
                           "CFBundleShortVersionString": "149.0.7827.201"}, f)
        return tmp / "Chromium.app"

    def test_bundle_version_stamped_from_tag(self):
        # roam-120: the appcast advertises the tag version; the installed bundle must
        # carry the SAME scheme in CFBundleVersion, or Sparkle's standard comparator
        # ranks Chromium's 7827.x above every tag version and never offers an update.
        import plistlib
        tmp = pathlib.Path(tempfile.mkdtemp(prefix="roamux-rn-"))
        self.addCleanup(_rmtree, tmp)
        new = rename_bundle.rename_bundle(self._make_chromium_app(tmp),
                                          bundle_version="0.0.1-alpha.2")
        with open(new / "Contents" / "Info.plist", "rb") as f:
            plist = plistlib.load(f)
        self.assertEqual(plist["CFBundleVersion"], "0.0.1-alpha.2",
                         "CFBundleVersion must carry the tag-derived version")
        self.assertEqual(plist["CFBundleShortVersionString"], "149.0.7827.201",
                         "Chromium provenance stays in the short version string")

    def test_without_bundle_version_plist_version_is_untouched(self):
        import plistlib
        tmp = pathlib.Path(tempfile.mkdtemp(prefix="roamux-rn-"))
        self.addCleanup(_rmtree, tmp)
        new = rename_bundle.rename_bundle(self._make_chromium_app(tmp))
        with open(new / "Contents" / "Info.plist", "rb") as f:
            plist = plistlib.load(f)
        self.assertEqual(plist["CFBundleVersion"], "7827.201",
                         "no stamp requested -> no version rewrite")


class PackagingTest(unittest.TestCase):
    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp(prefix="roamux-pkg-"))
        self.addCleanup(_rmtree, self.tmp)
        app = self.tmp / "Roamux.app" / "Contents"
        (app / "Frameworks" / "F.framework" / "Versions" / "B").mkdir(
            parents=True)
        os.symlink("B", app / "Frameworks" / "F.framework" / "Versions" /
                   "Current")
        _exe(app / "MacOS" / "Roamux")
        self.app = self.tmp / "Roamux.app"

    def _assert_preserved(self, extracted_app):
        cur = (extracted_app / "Contents" / "Frameworks" / "F.framework" /
               "Versions" / "Current")
        self.assertTrue(cur.is_symlink(), "framework symlink lost")
        exe = extracted_app / "Contents" / "MacOS" / "Roamux"
        self.assertTrue(os.access(exe, os.X_OK), "exec bit lost")

    def test_zip_preserves_symlinks_and_exec_bits(self):
        out = self.tmp / "Roamux.zip"
        package_roamux.package_zip(self.app, out)
        dest = self.tmp / "unzip"
        dest.mkdir()
        subprocess.run(["ditto", "-x", "-k", str(out), str(dest)], check=True)
        self._assert_preserved(dest / "Roamux.app")

    def test_dmg_preserves_symlinks_and_exec_bits(self):
        if not _has("hdiutil"):
            self.skipTest("hdiutil unavailable (non-macOS)")
        out = self.tmp / "Roamux.dmg"
        package_roamux.package_dmg(self.app, out, volname="Roamux")
        # roam-233: cleanup is registered BEFORE attach and keyed by the image
        # path, so a partially-successful attach (device created, mount step
        # failed, non-zero exit — the mountpoint never learned) still gets
        # detached. _detach_until_gone re-enumerates this exact DMG path until
        # empty and raises otherwise — the run-scoped leak postcondition; on
        # the shared dev/CI host nothing outside this test's own image is ever
        # consulted or touched.
        # roam-233 anticipated that exact partial-attach shape but only at
        # TEARDOWN, i.e. after the retry loop had already exhausted every
        # attempt against the leaked device. roam-259 moves the same recovery
        # in between attempts; this cleanup remains the final backstop.
        self.addCleanup(_detach_until_gone,
                        lambda: _hdiutil_representatives(out),
                        _hdiutil_detach)
        mountpoint = self.tmp / "mnt"
        mountpoint.mkdir()
        # roam-259: recovery (detach this image) runs between attempts, so a
        # leaked device from a failed mount cannot make every retry collide.
        mount, tries, recovery_errors = _attach_dmg_with_recovery(
            out, mountpoint)
        if mount.returncode != 0:
            try:
                state = "attachments of this image at failure: %r" % (
                    _hdiutil_representatives(out),)
            except (RuntimeError, subprocess.CalledProcessError) as exc:
                state = "attachment-state query failed: %s" % exc
            if recovery_errors:
                state += "\n  recovery errors: %s" % "; ".join(recovery_errors)
            self.fail(
                "hdiutil attach failed %d time(s) (last rc=%d): %s\n%s\n%s"
                % (len(tries), mount.returncode, mount.args,
                   "\n".join("  try %d stderr: %s" % (
                       i + 1, (t.stderr or "").strip())
                       for i, t in enumerate(tries)),
                   state))
        self._assert_preserved(mountpoint / "Roamux.app")


class DmgMountHygieneTest(unittest.TestCase):
    """roam-233: pure seams for the DMG mount cleanup — representative-device
    selection over `hdiutil info -plist` data and the bounded detach-until-gone
    policy. Fixture-driven; hdiutil is never invoked."""

    @staticmethod
    def _info(images):
        return plistlib.dumps({"images": images})

    @staticmethod
    def _image(path, *devs, **kw):
        record = {"image-path": path}
        if kw.get("entities", True):
            record["system-entities"] = [{"dev-entry": d} for d in devs]
        return record

    DMG = "/tmp/roamux-pkg-abc/Roamux.dmg"

    # ---- _representative_devices ----

    def test_one_representative_per_multi_entity_attachment(self):
        # One attachment exposing parent disk, a slice, and a synthesized
        # device must yield exactly ONE representative (the whole-disk node) —
        # the round-1-blocker fixture: one attachment => one detach.
        data = self._info([self._image(
            self.DMG, "/dev/disk4", "/dev/disk4s1", "/dev/disk5")])
        self.assertEqual(_representative_devices(data, self.DMG),
                         ["/dev/disk4"])

    def test_each_matching_record_yields_its_own_representative(self):
        data = self._info([
            self._image(self.DMG, "/dev/disk4", "/dev/disk4s1"),
            self._image(self.DMG, "/dev/disk6", "/dev/disk6s1"),
        ])
        self.assertEqual(_representative_devices(data, self.DMG),
                         ["/dev/disk4", "/dev/disk6"])

    def test_non_matching_and_empty_yield_nothing(self):
        data = self._info([self._image("/elsewhere/Other.dmg", "/dev/disk9")])
        self.assertEqual(_representative_devices(data, self.DMG), [])
        self.assertEqual(
            _representative_devices(self._info([]), self.DMG), [])

    def test_no_whole_disk_entry_falls_back_to_first_dev_entry(self):
        # Frozen-plan fallback: when a record exposes no whole-disk node
        # (/dev/diskN), the representative is the record's FIRST dev-entry —
        # not the shortest child (deterministic but arbitrary ordering would
        # detach a slice picked by string length).
        data = self._info([self._image(
            self.DMG, "/dev/disk10s2", "/dev/disk4s1")])
        self.assertEqual(_representative_devices(data, self.DMG),
                         ["/dev/disk10s2"])

    def test_matching_record_without_usable_dev_entry_raises(self):
        # A record hdiutil says is attached but cannot be enumerated must NOT
        # read as "no attachment" — cleanup has to fail loudly instead.
        for record in (self._image(self.DMG, entities=False),
                       self._image(self.DMG)):
            with self.assertRaises(RuntimeError) as ctx:
                _representative_devices(self._info([record]), self.DMG)
            self.assertIn(self.DMG, str(ctx.exception))

    # ---- _detach_until_gone ----

    @staticmethod
    def _script(*batches):
        """enumerate_fn returning successive batches (last repeats)."""
        state = {"i": 0}

        def enumerate_fn():
            batch = batches[min(state["i"], len(batches) - 1)]
            state["i"] += 1
            return list(batch)
        return enumerate_fn

    def test_no_attachment_is_a_noop(self):
        calls = []
        _detach_until_gone(self._script([]),
                           lambda dev, force: calls.append(dev) or (0, ""))
        self.assertEqual(calls, [])

    def test_teardown_on_first_detach_touches_no_siblings(self):
        calls = []

        def detach(dev, force):
            calls.append((dev, force))
            return 0, ""
        _detach_until_gone(self._script(["/dev/disk4"], []), detach)
        self.assertEqual(calls, [("/dev/disk4", False)])

    def test_busy_then_force_succeeds(self):
        calls = []

        def detach(dev, force):
            calls.append((dev, force))
            return (1, "Resource busy") if not force else (0, "")
        _detach_until_gone(
            self._script(["/dev/disk4"], ["/dev/disk4"], []), detach)
        self.assertEqual(calls,
                         [("/dev/disk4", False), ("/dev/disk4", True)])

    def test_survivors_after_max_passes_raise_with_stderr(self):
        with self.assertRaises(RuntimeError) as ctx:
            _detach_until_gone(self._script(["/dev/disk4"]),
                               lambda dev, force: (1, "Resource busy"))
        self.assertIn("/dev/disk4", str(ctx.exception))
        self.assertIn("Resource busy", str(ctx.exception))

    # ---- _attach_with_retry ----

    class _Result:
        def __init__(self, rc):
            self.returncode = rc

    def test_attach_first_try_success_never_sleeps(self):
        sleeps = []
        res, tries = _attach_with_retry(
            lambda: self._Result(0), sleep_fn=sleeps.append)
        self.assertEqual(res.returncode, 0)
        self.assertEqual(len(tries), 1)
        self.assertEqual(sleeps, [])

    def test_attach_transient_failure_retries_after_settle(self):
        # The EAGAIN case observed live during the roam-233 gates: attach
        # fails transiently with a clean attachment table, then succeeds.
        sleeps = []
        outcomes = [self._Result(1), self._Result(0)]
        res, tries = _attach_with_retry(
            lambda: outcomes[len(sleeps)], sleep_fn=sleeps.append)
        self.assertEqual(res.returncode, 0)
        self.assertEqual(len(tries), 2)
        self.assertEqual(sleeps, [2])

    def test_attach_persistent_failure_returns_last_result(self):
        sleeps = []
        res, tries = _attach_with_retry(
            lambda: self._Result(1), delays=(2, 4), sleep_fn=sleeps.append)
        self.assertEqual(res.returncode, 1)
        self.assertEqual(len(tries), 3)
        self.assertEqual(sleeps, [2, 4])

    def test_default_retry_schedule_rides_out_long_windows(self):
        # CI evidence (PR #237 head 6ba9bfa): five EAGAINs across ~20 s —
        # windows outlast short settling. The default schedule must wait
        # ~90 s total across 7 attempts, and stay bounded.
        self.assertEqual(_RETRY_DELAYS, (2, 4, 8, 15, 30, 30))
        sleeps = []
        res, tries = _attach_with_retry(
            lambda: self._Result(1), sleep_fn=sleeps.append)
        self.assertEqual(res.returncode, 1)
        self.assertEqual(len(tries), 7)
        self.assertEqual(sleeps, [2, 4, 8, 15, 30, 30])

    # ---- roam-259: recover (detach the leaked device) before retrying ----

    def test_attach_recovers_after_detaching_the_leaked_device(self):
        # The roam-259 mechanism: `attach -mountpoint` attaches, THEN mounts.
        # A failed mount returns rc=1 with the device already attached, so
        # every later attempt collides with the leak until it is detached.
        # Sleeping cannot clear it; recovery must, and must run BEFORE the
        # settle so the window also covers post-detach settling.
        events = []
        leaked = {"present": True}

        def run_fn():
            events.append("attach")
            return self._Result(1 if leaked["present"] else 0)

        def recover_fn():
            events.append("recover")
            leaked["present"] = False

        res, tries = _attach_with_retry(
            run_fn, sleep_fn=lambda d: events.append("sleep:%s" % d),
            recover_fn=recover_fn)
        self.assertEqual(res.returncode, 0)
        self.assertEqual(len(tries), 2)
        self.assertEqual(events, ["attach", "recover", "sleep:2", "attach"])

    def test_attach_first_try_success_never_recovers(self):
        recoveries = []
        res, tries = _attach_with_retry(
            lambda: self._Result(0), sleep_fn=lambda d: None,
            recover_fn=lambda: recoveries.append(1))
        self.assertEqual(res.returncode, 0)
        self.assertEqual(len(tries), 1)
        self.assertEqual(recoveries, [])

    def test_recovery_failure_does_not_abort_the_retry_loop(self):
        # Recovery is best-effort: _detach_until_gone raises on survivors,
        # _hdiutil_representatives raises CalledProcessError (check=True) and
        # _representative_devices raises on a record with no dev-entry. None of
        # those may abandon the remaining attempts or destroy the attach
        # stderr the call site reports from `tries`.
        def recover_fn():
            raise RuntimeError("detach failed to clear attachments")

        res, tries = _attach_with_retry(
            lambda: self._Result(1), delays=(2, 4), sleep_fn=lambda d: None,
            recover_fn=recover_fn)
        self.assertEqual(res.returncode, 1)
        self.assertEqual(len(tries), 3)

    def test_recovery_not_attempted_after_the_final_attempt(self):
        # No retry follows the last failure, and the call site's addCleanup
        # owns final teardown — recovering there would be pointless work.
        recoveries = []
        res, tries = _attach_with_retry(
            lambda: self._Result(1), delays=(2, 4), sleep_fn=lambda d: None,
            recover_fn=lambda: recoveries.append(1))
        self.assertEqual(res.returncode, 1)
        self.assertEqual(len(tries), 3)
        self.assertEqual(len(recoveries), 2)

    def test_attach_with_explicit_recover_fn_none_behaves_as_before(self):
        sleeps = []
        res, tries = _attach_with_retry(
            lambda: self._Result(1), delays=(2, 4), sleep_fn=sleeps.append,
            recover_fn=None)
        self.assertEqual(res.returncode, 1)
        self.assertEqual(len(tries), 3)
        self.assertEqual(sleeps, [2, 4])

    def test_recovery_errors_are_collected_for_diagnostics(self):
        # Swallowing must not hide a broken detach path: the caller gets the
        # collected reasons AND every original attach stderr.
        errors = []

        def run_fn():
            r = self._Result(1)
            r.stderr = "Resource temporarily unavailable"
            return r

        def recover_fn():
            raise RuntimeError("detach failed to clear attachments")

        res, tries = _attach_with_retry(
            run_fn, delays=(2,), sleep_fn=lambda d: None,
            recover_fn=recover_fn, recovery_errors=errors)
        self.assertEqual(res.returncode, 1)
        self.assertEqual(len(tries), 2)
        self.assertEqual(len(errors), 1)
        self.assertIn("detach failed to clear attachments", errors[0])
        self.assertTrue(
            all("Resource temporarily unavailable" in t.stderr for t in tries))

    def test_dmg_attach_helper_wires_recovery_into_the_mount_path(self):
        # The seam tests above prove the loop behaves; this proves the DMG
        # mount path's own helper actually assembles run_fn + recover_fn.
        calls = []
        leaked = {"present": True}

        def attach_fn():
            calls.append("attach")
            return self._Result(1 if leaked["present"] else 0)

        def recover_fn():
            calls.append("recover")
            leaked["present"] = False
            raise RuntimeError("detach complained but cleared it")

        res, tries, errors = _attach_dmg_with_recovery(
            "/nonexistent/Roamux.dmg", "/nonexistent/mnt",
            attach_fn=attach_fn, recover_fn=recover_fn, sleep_fn=lambda d: None)
        self.assertEqual(res.returncode, 0)
        self.assertEqual(len(tries), 2)
        self.assertEqual(calls, ["attach", "recover", "attach"])
        self.assertEqual(len(errors), 1)
        self.assertIn("detach complained", errors[0])

    def test_dmg_attach_helper_defaults_to_detaching_this_image(self):
        # The test above supplies recover_fn, so it cannot notice if the
        # helper's DEFAULT recovery were removed — which would restore the
        # original defect on the real call path while staying green. Pin the
        # default wiring: no recover_fn, real helpers mocked out.
        events = []
        leaked = {"present": True}

        def fake_reps(image_path):
            events.append("enumerate")
            return ["/dev/disk9"] if leaked["present"] else []

        def fake_detach(device, force):
            events.append("detach:%s" % device)
            leaked["present"] = False
            return 0, ""

        def attach_fn():
            events.append("attach")
            return self._Result(1 if leaked["present"] else 0)

        mod = sys.modules[__name__]
        with mock.patch.object(mod, "_hdiutil_representatives", fake_reps), \
                mock.patch.object(mod, "_hdiutil_detach", fake_detach):
            res, tries, errors = _attach_dmg_with_recovery(
                "/nonexistent/Roamux.dmg", "/nonexistent/mnt",
                attach_fn=attach_fn, sleep_fn=lambda d: None)
        self.assertEqual(res.returncode, 0)
        self.assertEqual(len(tries), 2)
        # The trailing enumerate is _detach_until_gone's re-query: it treats a
        # fresh enumeration, not the detach's exit code, as the source of
        # truth that the table is clear (roam-233).
        self.assertEqual(events, ["attach", "enumerate", "detach:/dev/disk9",
                                  "enumerate", "attach"])
        self.assertEqual(errors, [])

    def test_packaging_mount_path_uses_the_recovery_helper(self):
        # Wiring check: the helper above is only worth anything if the real
        # mount path calls it. That path runs hdiutil and is skip-guarded off
        # macOS, so it cannot be executed hermetically to observe the wiring —
        # assert on its source instead (same idiom as test_tier2_job.py).
        src = inspect.getsource(
            PackagingTest.test_dmg_preserves_symlinks_and_exec_bits)
        self.assertIn("_attach_dmg_with_recovery", src)
        self.assertNotIn("_attach_with_retry(", src)


class DmgCreateRetryTest(unittest.TestCase):
    """roam-259 secondary: package_dmg's 4-attempt `hdiutil create` retry had
    no sleep on any path — the same latent shape as the attach bug, on the
    release path rather than the test path. `create` has no leaked-device mode
    to clear, so the settle schedule is short (unlike _RETRY_DELAYS)."""

    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp(prefix="roamux-dmgcreate-"))
        self.addCleanup(_rmtree, self.tmp)

    def _package(self, create_rcs, **kw):
        """Drive package_dmg with `hdiutil create` scripted to the given
        return codes. `ditto` always succeeds; no real hdiutil is invoked.
        Returns the count of `hdiutil create` invocations so the attempt
        contract can be pinned, not just the delays."""
        creates = []

        def fake_run(cmd, **kwargs):
            if cmd[0] == "ditto":
                return subprocess.CompletedProcess(cmd, 0, "", "")
            creates.append(cmd)
            rc = create_rcs.pop(0)
            return subprocess.CompletedProcess(
                cmd, rc, "", "hdiutil: create failed" if rc else "")

        with mock.patch.object(package_roamux.subprocess, "run", fake_run):
            out = package_roamux.package_dmg(
                self.tmp / "Roamux.app", self.tmp / "Roamux.dmg", **kw)
        return out, creates

    def test_create_retries_with_backoff_between_attempts(self):
        sleeps = []
        _, creates = self._package([1, 1, 0], sleep_fn=sleeps.append)
        self.assertEqual(sleeps, [2, 4])
        self.assertEqual(len(creates), 3)

    def test_create_does_not_sleep_after_the_final_failure(self):
        # Pins BOTH halves of the contract: exactly 4 attempts (a 3-attempt
        # implementation sleeping after each failure would also produce 3
        # sleeps) and no settle after the last one.
        sleeps = []
        rcs = [1, 1, 1, 1]
        creates = []

        def fake_run(cmd, **kwargs):
            if cmd[0] == "ditto":
                return subprocess.CompletedProcess(cmd, 0, "", "")
            creates.append(cmd)
            return subprocess.CompletedProcess(
                cmd, rcs.pop(0), "", "hdiutil: create failed")

        with mock.patch.object(package_roamux.subprocess, "run", fake_run):
            with self.assertRaises(RuntimeError) as cm:
                package_roamux.package_dmg(
                    self.tmp / "Roamux.app", self.tmp / "Roamux.dmg",
                    sleep_fn=sleeps.append)
        self.assertIn("hdiutil create failed after retries", str(cm.exception))
        self.assertEqual(len(creates), 4, "the 4-attempt contract is preserved")
        self.assertEqual(rcs, [], "every scripted failure must be consumed")
        self.assertEqual(sleeps, [2, 4, 8])

    def test_create_first_try_success_never_sleeps(self):
        sleeps = []
        _, creates = self._package([0], sleep_fn=sleeps.append)
        self.assertEqual(sleeps, [])
        self.assertEqual(len(creates), 1)

    def test_create_schedule_is_shorter_than_the_attach_schedule(self):
        # `create` has no leaked-device mode to clear, so it must not inherit
        # the attach path's ~89 s settle and stall a release job.
        self.assertEqual(package_roamux._CREATE_RETRY_DELAYS, (2, 4, 8))
        self.assertLess(sum(package_roamux._CREATE_RETRY_DELAYS),
                        sum(_RETRY_DELAYS))

    def test_create_default_path_sleeps_without_an_injected_sleep_fn(self):
        # Without this, every test above could pass over a production path
        # that still has zero backoff: they all inject sleep_fn. Note the
        # deliberate asymmetry with _attach_with_retry, where sleep_fn=None
        # means "do not sleep"; here None resolves to the real time.sleep.
        slept = []
        with mock.patch.object(package_roamux.time, "sleep", slept.append):
            self._package([1, 1, 0])
        self.assertEqual(slept, [2, 4])


class RoamuxPartsPathTest(unittest.TestCase):
    """roam-97 Done: derive Chromium's part paths for the Model-B Roamux config
    and prove they resolve against the post-rename on-disk bundle — the hermetic
    guard that finding 1 (product/app_product coupling) cannot recur."""

    # ---- checkout-free: the derived path STRINGS (config.py formulas) ----
    def test_derived_paths_are_model_b(self):
        cfg = roamux_signing_config.make_roamux_config_class(
            _StubChromiumBase)()
        self.assertEqual(cfg.app_dir, "Roamux.app")  # app_product -> Roamux
        self.assertEqual(  # product inherited "Chromium"
            cfg.framework_dir,
            "Roamux.app/Contents/Frameworks/Chromium Framework.framework")
        helper = "{0.framework_dir}/Helpers/{0.product} Helper.app".format(cfg)
        self.assertEqual(
            helper,
            "Roamux.app/Contents/Frameworks/Chromium Framework.framework/"
            "Helpers/Chromium Helper.app")

    def test_derived_paths_exist_on_renamed_bundle(self):
        tmp = pathlib.Path(tempfile.mkdtemp(prefix="roamux-parts-"))
        self.addCleanup(_rmtree, tmp)
        _make_renamed_bundle(tmp)
        cfg = roamux_signing_config.make_roamux_config_class(
            _StubChromiumBase)()
        self.assertTrue((tmp / cfg.app_dir).is_dir())
        self.assertTrue((tmp / cfg.framework_dir).is_dir())
        helper = "{0.framework_dir}/Helpers/{0.product} Helper.app".format(cfg)
        self.assertTrue((tmp / helper).is_dir())

    # ---- checkout-backed: Chromium's REAL parts.get_parts() resolves ----
    @unittest.skipIf(_SKIP_PARTS, SIGNING_SKIP or "signing package unavailable")
    def test_real_get_parts_paths_resolve_on_fixture(self):
        parts_mod, config_mod, _model = _import_signing()
        base = _make_chromium_base(config_mod)
        cfg = roamux_signing_config.make_roamux_config_class(base)()
        parts = parts_mod.get_parts(cfg)
        # Model B: outer app rebrands, nested parts keep Chromium names.
        self.assertEqual(parts["app"].path, "Roamux.app")
        self.assertEqual(
            parts["framework"].path,
            "Roamux.app/Contents/Frameworks/Chromium Framework.framework")
        self.assertEqual(
            parts["helper-app"].path,
            "Roamux.app/Contents/Frameworks/Chromium Framework.framework/"
            "Helpers/Chromium Helper.app")
        tmp = pathlib.Path(tempfile.mkdtemp(prefix="roamux-realparts-"))
        self.addCleanup(_rmtree, tmp)
        _make_renamed_bundle(tmp)
        for name, part in parts.items():
            self.assertTrue(
                (tmp / part.path).exists(),
                "nested part {} missing on renamed bundle: {}".format(
                    name, part.path))


class RoamuxSignerSeamTest(unittest.TestCase):
    """roam-97 findings 1/2/3: the config seam actually governs the signer, the
    invocation is well-formed (input DIRECTORY + output, no --entitlements,
    app-signing only), and a usable signed app is promoted."""

    @unittest.skipIf(_SKIP_PARTS, SIGNING_SKIP or "signing package unavailable")
    def test_config_seam_and_output_contract(self):
        parts_mod, config_mod, model_mod = _import_signing()
        import signing.config_factory as cf

        fake_base = _make_chromium_base(config_mod)
        fake_get_class = lambda: fake_base  # noqa: E731
        original_get_class = cf.get_class
        cf.get_class = fake_get_class
        self.addCleanup(setattr, cf, "get_class", original_get_class)

        tmp = pathlib.Path(tempfile.mkdtemp(prefix="roamux-seam-"))
        self.addCleanup(_rmtree, tmp)
        input_dir = tmp / "in"
        input_dir.mkdir()
        app = _make_renamed_bundle(input_dir)               # in/Roamux.app
        output_dir = tmp / "out"
        # Simulate the signer leaving the bare signed app where pipeline.py does.
        _make_renamed_bundle(output_dir / "stable")         # out/stable/Roamux.app
        # A BUILT signing package under the --input dir (build_props_config.py
        # present) so _resolve_signing_pkg_dir picks it — the source tree lacks
        # that generated file (roam-97 Fix 1).
        built_sig = input_dir / "Chromium Packaging" / "signing"
        built_sig.mkdir(parents=True)
        (built_sig / "build_props_config.py").write_text("# generated\n")

        buf = io.StringIO()
        with mock.patch.dict(os.environ,
                             {"ROAMUX_CHROMIUM_OUT": "", "CHROMIUM_OUT": ""}), \
             mock.patch.object(sign_roamux, "sign_sparkle_parts") as sp, \
             mock.patch.object(sign_roamux, "_run") as run_mock, \
             mock.patch("signing.driver._show_tool_versions"), \
             mock.patch("signing.pipeline.sign_all",
                        new_callable=mock.AsyncMock) as sign_all, \
             contextlib.redirect_stdout(buf):
            rc = sign_roamux.main([
                "--mode", "signed", "--identity", "ABCDEF",
                "--app", str(app), "--output", str(output_dir)])

        self.assertEqual(rc, 0)
        self.assertTrue(sign_all.called, "Chromium pipeline was not driven")
        paths_arg = sign_all.call_args.args[0]
        config_arg = sign_all.call_args.args[1]

        # (a) the config reaching the pipeline is the Roamux subclass (Model B).
        self.assertEqual(type(config_arg).__name__, "RoamuxCodeSignConfig")
        self.assertEqual(config_arg.app_product, "Roamux")
        self.assertEqual(config_arg.product, "Chromium")   # inherited
        self.assertEqual(config_arg.base_bundle_id, "com.roamux.Roamux")

        # (b) model.Paths use an --input DIRECTORY + --output; packaging
        #     disabled; notarize none.
        self.assertEqual(pathlib.Path(paths_arg.input).resolve(),
                         input_dir.resolve())
        self.assertEqual(pathlib.Path(paths_arg.output).resolve(),
                         output_dir.resolve())
        self.assertNotEqual(pathlib.Path(paths_arg.input).resolve(),
                            app.resolve(), "--input must be the DIR, not .app")
        self.assertTrue(sign_all.call_args.kwargs["disable_packaging"])
        self.assertEqual(config_arg.notarize,
                         model_mod.NotarizeAndStapleLevel.NONE)

        # (d) the promoted final signed-app path (staple/package consume it)
        #     exists.
        self.assertTrue(app.exists(), "signed app was not promoted")

        # seam restored; Sparkle signed first.
        self.assertIs(cf.get_class, fake_get_class, "get_class not restored")
        self.assertTrue(sp.called, "Sparkle parts not signed first")

        # roam-97 Fix 2: notarization + stapling deferred to #90 — NO stapler
        # runs here, and the deferral is logged.
        run_mock.assert_not_called()  # _run only wraps codesign/stapler
        out_text = buf.getvalue()
        self.assertIn("#90", out_text)
        self.assertIn("DEFERRED", out_text)
        self.assertNotIn("stapler staple", out_text)


class RoamuxDryRunTest(unittest.TestCase):
    """roam-97 finding 3: production --dry-run does NO real signing — no
    driver/pipeline, no codesign, no stapler — and prints the resolved plan."""

    def test_dry_run_performs_no_real_signing(self):
        tmp = pathlib.Path(tempfile.mkdtemp(prefix="roamux-dry-"))
        self.addCleanup(_rmtree, tmp)
        app = tmp / "Roamux.app"
        (app / "Contents" / "MacOS").mkdir(parents=True)
        _exe(app / "Contents" / "MacOS" / "Roamux")
        out = tmp / "out"

        buf = io.StringIO()
        with mock.patch.dict(os.environ,
                             {"ROAMUX_CHROMIUM_OUT": "", "CHROMIUM_OUT": ""}), \
             mock.patch.object(sign_roamux, "_invoke_chromium_signer") as inv, \
             mock.patch.object(sign_roamux, "sign_sparkle_parts") as sp, \
             mock.patch.object(sign_roamux, "promote_signed_app") as promote, \
             mock.patch.object(sign_roamux, "_run") as run_mock, \
             contextlib.redirect_stdout(buf):
            rc = sign_roamux.main([
                "--mode", "signed", "--identity", "ABCDEF",
                "--app", str(app), "--output", str(out), "--dry-run"])

        self.assertEqual(rc, 0)
        inv.assert_not_called()      # no driver.main / pipeline
        sp.assert_not_called()       # no Sparkle codesign
        promote.assert_not_called()  # no filesystem promote
        run_mock.assert_not_called()  # no codesign / stapler shell-out

        text = buf.getvalue()
        self.assertIn("product=Chromium", text)
        self.assertIn("app_product=Roamux", text)
        self.assertIn("base_bundle_id=com.roamux.Roamux", text)
        # the resolved model.Paths (input dir + output) are previewed
        self.assertIn(str(app.parent.resolve()), text)
        self.assertIn(str(out), text)
        # the final promoted signed-app path is previewed
        final = os.path.join(os.path.abspath(str(out)), "stable", "Roamux.app")
        self.assertIn(final, text)
        # Sparkle parts previewed before the outer app in the sign order
        order_line = next(l for l in text.splitlines()
                          if l.startswith("sign order:"))
        self.assertIn("sparkle:", order_line)
        self.assertLess(order_line.index("sparkle:"),
                        order_line.rindex("app"))


class SignRoamuxCLITest(unittest.TestCase):
    """roam-97 finding 3 regression: the malformed CLI is gone."""

    def test_output_present_and_no_entitlements_flag(self):
        parser = sign_roamux._build_parser()
        opts = set()
        for action in parser._actions:
            opts.update(action.option_strings)
        self.assertIn("--output", opts)
        self.assertNotIn(
            "--entitlements", opts,
            "--entitlements is not a Chromium driver flag (roam-97)")

    def test_sign_chrome_shell_out_is_retired(self):
        src = pathlib.Path(sign_roamux.__file__).read_text()
        self.assertNotIn(
            "sign_chrome.py", src,
            "the sign_chrome.py shell-out must be retired (in-process signer)")

    def test_unsigned_mode_returns_zero_without_output(self):
        rc = sign_roamux.main(
            ["--mode", "unsigned", "--app", "/nonexistent/Roamux.app"])
        self.assertEqual(rc, 0)

    def test_signed_mode_requires_output(self):
        rc = sign_roamux.main(
            ["--mode", "signed", "--identity", "X",
             "--app", "/nonexistent/Roamux.app"])
        self.assertEqual(rc, 2)


class SignerPackageResolutionTest(unittest.TestCase):
    """roam-97 Fix 1: the signer must import the BUILT signing package (with the
    GN-generated build_props_config.py under `<...>/Chromium Packaging/signing/`),
    NOT the source tree (which lacks it and fails with ModuleNotFoundError).
    Hermetic — no checkout needed; only the resolution logic is exercised."""

    def _make_built_pkg(self, root):
        sig = root / "Chromium Packaging" / "signing"
        sig.mkdir(parents=True)
        (sig / "build_props_config.py").write_text("# generated\n")
        (sig / "config_factory.py").write_text("# copied\n")
        return str(root / "Chromium Packaging")

    def test_env_out_dir_is_resolved(self):
        tmp = pathlib.Path(tempfile.mkdtemp(prefix="roamux-pkg-"))
        self.addCleanup(_rmtree, tmp)
        out = tmp / "out"
        pkg = self._make_built_pkg(out)
        self.assertEqual(
            sign_roamux._resolve_signing_pkg_dir(
                str(tmp / "input"), env={"ROAMUX_CHROMIUM_OUT": str(out)}),
            pkg)

    def test_input_dir_fallback_is_resolved(self):
        tmp = pathlib.Path(tempfile.mkdtemp(prefix="roamux-pkg-"))
        self.addCleanup(_rmtree, tmp)
        inp = tmp / "input"
        pkg = self._make_built_pkg(inp)
        self.assertEqual(
            sign_roamux._resolve_signing_pkg_dir(str(inp), env={}), pkg)

    def test_env_out_beats_input_dir(self):
        tmp = pathlib.Path(tempfile.mkdtemp(prefix="roamux-pkg-"))
        self.addCleanup(_rmtree, tmp)
        out = tmp / "out"
        env_pkg = self._make_built_pkg(out)
        inp = tmp / "input"
        self._make_built_pkg(inp)
        self.assertEqual(
            sign_roamux._resolve_signing_pkg_dir(
                str(inp), env={"CHROMIUM_OUT": str(out)}),
            env_pkg)

    def test_source_tree_layout_is_not_picked(self):
        # A source-like layout (signing/ WITHOUT the generated build_props_config.py)
        # must NOT resolve — that is exactly the ModuleNotFoundError trap.
        tmp = pathlib.Path(tempfile.mkdtemp(prefix="roamux-pkg-"))
        self.addCleanup(_rmtree, tmp)
        src = tmp / "input" / "Chromium Packaging" / "signing"
        src.mkdir(parents=True)
        (src / "config_factory.py").write_text("# copied, no build_props\n")
        self.assertIsNone(
            sign_roamux._resolve_signing_pkg_dir(str(tmp / "input"), env={}))

    def test_degrades_cleanly_when_absent(self):
        tmp = pathlib.Path(tempfile.mkdtemp(prefix="roamux-pkg-"))
        self.addCleanup(_rmtree, tmp)
        self.assertIsNone(
            sign_roamux._resolve_signing_pkg_dir(str(tmp / "input"), env={}))


class SigningPartsRequirementGateTest(unittest.TestCase):
    """roam-97 finding 2 (CI gate): the checkout-backed parts + seam tests must
    not silently skip everywhere. On a runner WITH the checkout the tier-2 job
    sets REQUIRE_SIGNING_PARTS=1 to make this a HARD RUN (mirrors REQUIRE_GRIT);
    this self-enforcing guard fails loudly if that contract is violated."""

    def test_signing_package_present_when_required(self):
        if REQUIRE_SIGNING_PARTS:
            self.assertIsNone(
                SIGNING_SKIP,
                "REQUIRE_SIGNING_PARTS=1 but the Chromium signing package is "
                "unavailable ({}) — the load-bearing parts-path + config-seam "
                "tests would not execute. The tier-2 CI gate must run with a "
                "synced Chromium checkout (ROAMUX_CHROMIUM_SRC).".format(
                    SIGNING_SKIP))
        else:
            self.skipTest("REQUIRE_SIGNING_PARTS not set — tier-1 skip "
                          "behaviour is unchanged")


def _exe(path):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("#!/bin/sh\nexit 0\n")
    path.chmod(path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP |
               stat.S_IXOTH)


def _has(tool):
    return subprocess.run(["which", tool], capture_output=True).returncode == 0


def _representative_devices(plist_bytes, image_path):
    """roam-233: ONE representative device per `images[]` record attached from
    image_path. A single attachment exposes several system entities (parent
    whole-disk node, slices, APFS synthesized devices); detaching any one tears
    the whole attachment down, so cleanup must be per-attachment, never
    per-dev-entry. The whole-disk node is the shortest dev-entry. A matching
    record with no usable dev-entry must fail loudly — hdiutil says it is
    attached, so "nothing to detach" would be a silent leak."""
    reps = []
    for image in plistlib.loads(plist_bytes).get("images", []):
        if image.get("image-path") != image_path:
            continue
        devs = [e["dev-entry"] for e in image.get("system-entities", [])
                if e.get("dev-entry")]
        if not devs:
            raise RuntimeError(
                "hdiutil reports an attachment of %s with no usable dev-entry:"
                " %r" % (image_path, image))
        whole = [d for d in devs if re.fullmatch(r"/dev/disk\d+", d)]
        reps.append(min(whole, key=lambda d: (len(d), d)) if whole
                    else devs[0])
    return reps


def _detach_until_gone(enumerate_fn, detach_fn, max_passes=3):
    """roam-233: bounded detach loop with re-query as the source of truth
    (already-gone devices are success, not error). Pass 1 detaches plain,
    later passes -force; survivors after max_passes raise with the collected
    stderr so the failure self-explains."""
    errors = []
    for attempt in range(max_passes):
        devices = enumerate_fn()
        if not devices:
            return
        force = attempt > 0
        for dev in devices:
            rc, stderr = detach_fn(dev, force)
            if rc != 0:
                errors.append("%s%s: %s" % (
                    dev, " -force" if force else "", (stderr or "").strip()))
    if enumerate_fn():
        raise RuntimeError(
            "hdiutil detach failed to clear attachments after %d passes: %s"
            % (max_passes, "; ".join(errors) or "no stderr captured"))


# Two DISTINCT attach failure modes share this schedule — do not conflate them:
#
#   1. create-then-attach leak (roam-259, the common one). The trigger is
#      `hdiutil create` (UDZO conversion inside package_dmg) immediately
#      followed by `attach` of the just-created image; machine load raises the
#      rate but is NOT required. `attach -mountpoint` attaches then mounts, so
#      a failed mount leaves the device attached and every later attempt
#      collides with the leak. Settling cannot fix this — detach-before-retry
#      can, and does (measured 0/63 -> 62/63). This corrects roam-233, which
#      recorded the trigger as rapid attach/detach cycles.
#
#   2. clean-table transient (roam-233's original mode). Enumeration finds
#      nothing to detach and the window genuinely outlasts short settling —
#      tier-2 CI on PR #237 saw five failures across ~20 s while a probe
#      minutes later was healthy.
#
# The capped-exponential schedule below (7 attempts, ~89 s total) is retained
# CONSERVATIVELY FOR MODE 2, whose evidence is independent of roam-259 and
# still stands. It is not the fix for mode 1 and must not be read as such: with
# recovery in place mode 1 clears on attempt 2 after a single 2 s sleep, so the
# full 89 s is only ever paid when every attempt genuinely fails.
_RETRY_DELAYS = (2, 4, 8, 15, 30, 30)


def _attach_with_retry(run_fn, delays=_RETRY_DELAYS, sleep_fn=None,
                       recover_fn=None, recovery_errors=None):
    """Bounded `hdiutil attach` retry over the delay schedule (attempts =
    len(delays)+1); returns (last_result, all_tries) so a persistent failure
    still self-explains with every stderr.

    roam-259: a failed attempt does not imply a clean attachment table.
    `attach -mountpoint` attaches and THEN mounts, so a failed *mount* returns
    non-zero with the device already attached — the image is partially
    attached, and every later attempt collides with what the previous one
    leaked. Waiting cannot clear that; `recover_fn` (detach this image) runs
    before the settle, so the sleep also covers post-detach settling.

    Recovery is best-effort by design: it is skipped after the final failed
    attempt (no retry follows; the caller's addCleanup owns teardown), and any
    exception it raises is collected into `recovery_errors` rather than
    propagated. `_detach_until_gone` raises on survivors,
    `_hdiutil_representatives` raises CalledProcessError (check=True) and
    `_representative_devices` raises for a record with no dev-entry; letting
    any of those escape would abandon the remaining attempts and destroy the
    attach stderr the caller reports from `tries`. The next attach is the real
    signal."""
    tries = []
    for attempt in range(len(delays) + 1):
        res = run_fn()
        tries.append(res)
        if res.returncode == 0:
            return res, tries
        if attempt < len(delays):
            if recover_fn is not None:
                try:
                    recover_fn()
                except Exception as exc:  # never BaseException
                    if recovery_errors is not None:
                        recovery_errors.append(
                            "attempt %d recovery: %s" % (attempt + 1, exc))
            if sleep_fn is not None:
                sleep_fn(delays[attempt])
    return tries[-1], tries


def _attach_dmg_with_recovery(image_path, mountpoint, attach_fn=None,
                              recover_fn=None, sleep_fn=time.sleep):
    """roam-259: the DMG mount path's attach, with leak recovery wired in.

    Exists so that "recovery reaches the real mount path" is a testable
    property rather than an inline lambda: the seams default to the real
    `hdiutil attach` and to detaching every attachment of THIS image (the same
    two helpers the caller already registers with addCleanup). Returns
    (result, tries, recovery_errors)."""
    if attach_fn is None:
        def attach_fn():
            return subprocess.run(
                ["hdiutil", "attach", str(image_path), "-nobrowse", "-readonly",
                 "-mountpoint", str(mountpoint)],
                capture_output=True, text=True)
    if recover_fn is None:
        def recover_fn():
            _detach_until_gone(lambda: _hdiutil_representatives(image_path),
                               _hdiutil_detach)
    recovery_errors = []
    res, tries = _attach_with_retry(
        attach_fn, sleep_fn=sleep_fn, recover_fn=recover_fn,
        recovery_errors=recovery_errors)
    return res, tries, recovery_errors


def _hdiutil_representatives(image_path):
    info = subprocess.run(["hdiutil", "info", "-plist"],
                          capture_output=True, check=True)
    return _representative_devices(info.stdout, str(image_path))


def _hdiutil_detach(device, force):
    cmd = ["hdiutil", "detach", device] + (["-force"] if force else [])
    res = subprocess.run(cmd, capture_output=True, text=True)
    return res.returncode, res.stderr


def _rmtree(p):
    import shutil
    shutil.rmtree(p, ignore_errors=True)


if __name__ == "__main__":
    unittest.main()
