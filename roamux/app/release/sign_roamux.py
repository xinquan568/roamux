# SPDX-License-Identifier: Apache-2.0
"""roam-33 / roam-97 sign driver — resolves the signing mode and, in signed
mode, signs the universal2 Roamux.app inside-out with the Sparkle parts injected
BEFORE the outer app (so the outer seal stays valid), then staples.

Model B (roam-97). Chromium's mac signer (chrome/installer/mac/signing) is
consumed as a LIBRARY, IN-PROCESS — not shelled out to. Three things make the
dormant signed path internally consistent:

  1. Config seam. Chromium's `driver.main` resolves its config solely through
     `signing.config_factory.get_class()` (there is NO CLI flag to inject a
     subclass; the only hook is `--development`). So the Roamux config is
     installed by monkeypatching `config_factory.get_class` to return
     `RoamuxCodeSignConfig` for the duration of the call, then RESTORING the
     original in a `finally`. `RoamuxCodeSignConfig` rebrands only the outer app
     (`app_product` -> "Roamux") and inherits `product` == "Chromium", so the
     nested framework/helper part paths resolve to the on-disk Chromium bundles
     `rename_bundle.py` leaves.

  2. App-signing-only + output contract. Chromium's `pipeline.sign_all` copies
     the app input->work, signs it in the work dir, and its *product* is a
     packaged DMG/PKG in `--output` — the bare signed `.app` is not left usable
     in `--output` by default. Roamux owns packaging (its zip + Sparkle EdDSA)
     and its own notary/staple, so the signer is driven for APP-SIGNING ONLY
     (`--disable-packaging --notarize none`). Under that mode the bare signed
     app lands at `<output>/stable/<app_product>.app` (see
     `signed_app_output_path`); Roamux promotes it onto the release path and
     staples THAT.

  3. CLI contract. `--input` is the DIRECTORY containing Roamux.app (not the
     `.app` path); `--output` is a separate required dir; there is NO
     `--entitlements` flag (entitlements are config/packaging-derived).

The real codesign/notarize/staple only runs in the release job with the
protected credentials; this module is unit-covered for its decision + plan
construction and dry-runnable elsewhere. `--dry-run` routes through a
no-side-effect planning function and never calls the real Sparkle codesign,
Chromium's driver/pipeline, or the stapler.
"""

import argparse
import os
import pathlib
import shutil
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import signing_mode  # noqa: E402
import signing_plan  # noqa: E402
import roamux_signing_config  # noqa: E402

# The subdirectory Chromium's pipeline places the default (unbranded, no-channel)
# distribution's signed app under, when driven with `--disable-packaging
# --notarize none`. Derived from pipeline._intermediate_work_dir_name(default
# Distribution) -> "stable". See signed_app_output_path().
_SIGNER_DIST_SUBDIR = "stable"


def _run(cmd, dry_run):
    print("+ " + " ".join(str(c) for c in cmd))
    if not dry_run:
        subprocess.run(cmd, check=True)


def _chromium_mac_dir(chromium_src):
    return pathlib.Path(chromium_src) / "chrome" / "installer" / "mac"


def _ensure_signing_on_path(chromium_src):
    """Put Chromium's signing package dir on sys.path so `signing.*` imports."""
    mac = str(_chromium_mac_dir(chromium_src))
    if mac not in sys.path:
        sys.path.insert(0, mac)
    return mac


def resolve_roamux_config_class(chromium_src):
    """Return the Model-B RoamuxCodeSignConfig subclass over Chromium's real
    config base, or None if the Chromium signing config is not importable here
    (e.g. a source-only checkout where build_props_config.py — GN-generated — is
    absent)."""
    base = roamux_signing_config.load_chromium_config_base(chromium_src)
    if base is None:
        return None
    return roamux_signing_config.make_roamux_config_class(base)


def signed_app_output_path(output_dir, app_product="Roamux"):
    """Absolute path at which Chromium's pipeline leaves the bare signed `.app`
    when the signer is driven for APP-SIGNING ONLY (`--disable-packaging
    --notarize none`).

    Confirmed against chrome/installer/mac/signing/pipeline.py:
      * `sign_all()` -> `_sign_and_maybe_notarize_distributions()`: with
        `disable_packaging=True` and `config.notarize == NONE`, for the default
        Distribution `do_packaging` is False and `should_notarize()` is False,
        so `dest_dir = paths.output` joined with
        `_intermediate_work_dir_name(dist)`.
      * For the default Distribution (channel=None, no customization) that
        intermediate directory name is the literal "stable".
      * `_customize_and_sign_chrome()` then MOVES the signed bundle to
        `os.path.join(dest_dir, dist_config.app_dir)`, i.e.
        `<output>/stable/<app_product>.app`.
    `model.Paths` abspath()s output, so this returns the absolute path.
    """
    return os.path.join(
        os.path.abspath(str(output_dir)), _SIGNER_DIST_SUBDIR,
        "{}.app".format(app_product))


def _chromium_part_keys(roamux_cfg_cls, chromium_src):
    """Ordered part keys from Chromium's own get_parts, driven with the ROAMUX
    config (Model B: nested keys equal the base's), outer app ('app') last."""
    import importlib
    _ensure_signing_on_path(chromium_src)
    parts_mod = importlib.import_module("signing.parts")
    cfg = roamux_cfg_cls(invoker=lambda c: None, identity="-")
    keys = list(parts_mod.get_parts(cfg).keys())
    # Chromium keys the outer app as 'app'; ensure it is last.
    if "app" in keys:
        keys = [k for k in keys if k != "app"] + ["app"]
    return keys


def _resolve_part_keys(chromium_src):
    """The ordered Chromium part keys, or a preview fallback when the signing
    package (or the GN-generated config) isn't importable here."""
    cfg_cls = resolve_roamux_config_class(chromium_src)
    if cfg_cls is not None:
        try:
            return _chromium_part_keys(cfg_cls, chromium_src)
        except Exception as e:  # noqa: BLE001 — degrade to preview keys
            print("note: could not derive Chromium part keys ({}); using "
                  "preview keys".format(e))
    return ["chromium_framework", "chromium_helpers", "app"]


def _config_identity(chromium_src):
    """Resolve the Model-B config identity for the plan preview.

    Applies and RESTORES the `config_factory.get_class` seam only long enough to
    instantiate RoamuxCodeSignConfig (real build env); degrades to the known
    Model-B constants when Chromium's config isn't importable here."""
    identity = {"product": "Chromium", "app_product": "Roamux",
                "base_bundle_id": "com.roamux.Roamux", "class_name": None}
    cfg_cls = resolve_roamux_config_class(chromium_src)
    if cfg_cls is None:
        return identity
    _ensure_signing_on_path(chromium_src)
    import signing.config_factory as cf
    original_get_class = cf.get_class
    cf.get_class = lambda: cfg_cls
    try:
        cfg = cfg_cls(invoker=lambda c: None, identity="-")
        identity.update(product=cfg.product, app_product=cfg.app_product,
                        base_bundle_id=cfg.base_bundle_id,
                        class_name=cfg_cls.__name__)
    except Exception:  # noqa: BLE001 — keep the constants
        pass
    finally:
        cf.get_class = original_get_class
    return identity


def build_signing_plan(args, identity, chromium_src):
    """Resolve the full signing plan with NO side effects (used by --dry-run and
    as the input to the real signed run)."""
    input_dir = str(pathlib.Path(args.app).resolve().parent)
    release_app = str(pathlib.Path(args.app).resolve())
    output_dir = args.output
    signer_app = signed_app_output_path(output_dir)
    config = _config_identity(chromium_src)
    chromium_keys = _resolve_part_keys(chromium_src)
    ordered = roamux_signing_config.roamux_get_parts({}, chromium_keys)
    outer = chromium_keys[-1]
    # Invariants: outer app sealed last; Sparkle parts before it.
    assert ordered[-1] == outer, "outer app must be signed last"
    for k in roamux_signing_config.sparkle_part_keys():
        assert ordered.index(k) < ordered.index(outer), \
            "Sparkle parts must precede the outer app"
    return {
        "identity": identity,
        "input_dir": input_dir,
        "output_dir": output_dir,
        "release_app": release_app,
        "signer_app": signer_app,
        "chromium_src": chromium_src,
        "config": config,
        "ordered": ordered,
        "outer_key": outer,
    }


def print_plan(plan):
    c = plan["config"]
    cls = c["class_name"] or ("RoamuxCodeSignConfig [constants — Chromium "
                              "config not importable here]")
    print("=== roam-97 signed-release plan (Model B) ===")
    print("config: {}".format(cls))
    print("  product={} app_product={} base_bundle_id={}".format(
        c["product"], c["app_product"], c["base_bundle_id"]))
    print("paths (model.Paths): input={} output={}".format(
        plan["input_dir"], plan["output_dir"]))
    print("  --input is the DIRECTORY containing {}.app; app-signing only "
          "(--disable-packaging --notarize none); NO --entitlements".format(
              c["app_product"]))
    print("sign order: " + " -> ".join(plan["ordered"]))
    print("final signed app (from signer) -> {}".format(plan["signer_app"]))
    print("promoted to release path -> {}".format(plan["release_app"]))


def sign_sparkle_parts(app_path, identity, dry_run):
    """Sign every nested Sparkle bundle deepest-first, same identity, hardened
    runtime. Returns the ordered parts actually planned (for verification)."""
    framework = (pathlib.Path(app_path) / "Contents" / "Frameworks" /
                 "Sparkle.framework")
    parts = signing_plan.discover_sparkle_parts(framework)
    signing_plan.assert_sparkle_fully_planned(framework, parts)
    for part in parts:
        _run(["codesign", "--force", "--sign", identity,
              "--options", "runtime", "--timestamp", str(part)], dry_run)
    return parts


def _invoke_chromium_signer(identity, input_dir, output_dir, chromium_src):
    """Drive Chromium's signer IN-PROCESS for APP-SIGNING ONLY, with the Roamux
    config installed via the config_factory seam (restored in a finally).

    `--disable-packaging --notarize none`: Roamux owns packaging + notary/staple,
    so Chromium's pipeline must not build a DMG/PKG. Returns the installed Roamux
    config class."""
    _ensure_signing_on_path(chromium_src)
    import signing.config_factory as cf
    import signing.driver as driver

    base = cf.get_class()
    roamux_cls = roamux_signing_config.make_roamux_config_class(base)
    original_get_class = cf.get_class
    cf.get_class = lambda: roamux_cls
    try:
        driver.main([
            "--identity", identity,
            "--input", str(input_dir),
            "--output", str(output_dir),
            "--disable-packaging",
            "--notarize", "none",
        ])
    finally:
        cf.get_class = original_get_class
    return roamux_cls


def promote_signed_app(output_dir, release_app):
    """Copy/promote the bare signed app from the signer's output location
    (`<output>/stable/Roamux.app`) onto the release path staple/package
    consume, preserving symlinks."""
    src = signed_app_output_path(output_dir)
    release_app = str(release_app)
    print("+ promote signed app {} -> {}".format(src, release_app))
    if not os.path.exists(src):
        raise FileNotFoundError(
            "signer did not leave a signed app at {} (expected "
            "<output>/stable/<app_product>.app per pipeline.py)".format(src))
    if os.path.lexists(release_app):
        if os.path.islink(release_app) or os.path.isfile(release_app):
            os.remove(release_app)
        else:
            shutil.rmtree(release_app)
    shutil.copytree(src, release_app, symlinks=True)
    return release_app


def run_signed(args, plan):
    """Execute the real signed pipeline (never called under --dry-run)."""
    # 1) Sparkle nested code first (deepest-first) so the outer app seals it.
    sign_sparkle_parts(args.app, plan["identity"], dry_run=False)
    # 2) Chromium's signer (config seam, app-signing only) writes the signed app
    #    to <output>/stable/Roamux.app.
    _invoke_chromium_signer(plan["identity"], plan["input_dir"],
                            plan["output_dir"], plan["chromium_src"])
    # 3) Promote the signed app onto the release path.
    promote_signed_app(plan["output_dir"], plan["release_app"])
    # 4) Roamux owns the staple of the promoted signed app.
    _run(["xcrun", "stapler", "staple", plan["release_app"]], dry_run=False)
    print("signing-mode=signed — Sparkle parts + outer app signed, promoted, "
          "stapled.")
    return 0


def _build_parser():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", required=True,
                        help="the universal2 Roamux.app to sign")
    parser.add_argument("--output", default="",
                        help="output DIRECTORY for the signer (required in "
                             "signed mode); the bare signed app is retrieved "
                             "from <output>/stable/Roamux.app")
    parser.add_argument("--identity", default="",
                        help="Developer ID identity (signed mode)")
    # Notary credentials are Roamux-owned (notary/staple happen outside the
    # Chromium app-signing call); kept for interface stability.
    parser.add_argument("--notary-key", default="")
    parser.add_argument("--notary-key-id", default="")
    parser.add_argument("--notary-issuer", default="")
    parser.add_argument("--mode", choices=("signed", "unsigned"), default="",
                        help="explicit mode (else derived from the env gate)")
    # NOTE (roam-97 finding 3): NO --entitlements. Chromium's driver has no such
    # flag; entitlements are config/packaging-derived.
    parser.add_argument("--dry-run", action="store_true")
    return parser


def main(argv=None):
    parser = _build_parser()
    args = parser.parse_args(argv)

    if args.mode:
        mode = args.mode
    else:
        try:
            mode = signing_mode.resolve_signing_mode(os.environ)
        except signing_mode.PartialSigningSecretsError as e:
            print("::error::{}".format(e), file=sys.stderr)
            return 2

    if mode == "unsigned":
        print("signing-mode=unsigned — deliberate personal-alpha; "
              "skipping Apple codesign/notarize/staple. The Sparkle EdDSA "
              "signature (roam-32) still applies.")
        return 0

    identity = args.identity or os.environ.get("ROAMUX_SIGN_IDENTITY", "")
    if not identity:
        print("::error::signed mode but no signing identity resolved",
              file=sys.stderr)
        return 2

    if not args.output:
        print("::error::signed mode requires --output (the signer output "
              "directory; the signed app is retrieved from "
              "<output>/stable/Roamux.app)", file=sys.stderr)
        return 2

    chromium_src = os.environ.get("CHROMIUM_SRC", ".")
    plan = build_signing_plan(args, identity, chromium_src)
    print_plan(plan)

    if args.dry_run:
        print("dry-run: NO signing performed (Sparkle codesign, Chromium "
              "driver/pipeline, and stapler all skipped).")
        return 0

    return run_signed(args, plan)


if __name__ == "__main__":
    sys.exit(main())
