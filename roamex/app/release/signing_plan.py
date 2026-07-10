# SPDX-License-Identifier: Apache-2.0
"""The roam-33 inside-out signing plan + Sparkle nested-code discovery.

The outer app must be signed LAST — mutating any nested code (Chromium helpers
or the Sparkle framework/XPC) after the outer signature invalidates the outer
seal. So Sparkle parts are injected into the combined parts list BEFORE the
outer app, and a discovery check guarantees every piece of nested Sparkle code
is represented in the plan (guards fixture drift vs the real fetched framework).
"""

import pathlib


class UnplannedSparkleCodeError(RuntimeError):
    """Nested Sparkle code exists that the signing plan does not cover."""


def discover_sparkle_parts(framework_dir):
    """Nested code under Sparkle.framework, deepest-first, framework last.

    Returns pathlib.Path objects: the XPC services and Updater.app (nested
    bundles), then the framework directory itself last. codesign signs a
    bundle's own nested Mach-O via --deep-free recursion, so enumerating the
    bundle roots deepest-first is sufficient for the ordering invariant.
    """
    framework_dir = pathlib.Path(framework_dir)
    versions = framework_dir / "Versions"
    current = versions / "B"
    if not current.is_dir():
        # Follow Current if B is not the concrete version dir.
        current = (versions / "Current").resolve()
    nested = []
    xpc_dir = current / "XPCServices"
    if xpc_dir.is_dir():
        nested += sorted(xpc_dir.glob("*.xpc"))
    updater = current / "Updater.app"
    if updater.is_dir():
        nested.append(updater)
    # Deepest-first: nested bundles, then the framework itself.
    return nested + [framework_dir]


def _all_nested_code(framework_dir):
    framework_dir = pathlib.Path(framework_dir)
    found = set()
    for p in framework_dir.rglob("*"):
        if p.suffix in (".xpc", ".app") and p.is_dir():
            found.add(p.name)
    return found


def assert_sparkle_fully_planned(framework_dir, plan):
    """Fail if any nested .xpc/.app under the framework is absent from `plan`."""
    planned_names = {pathlib.Path(p).name for p in plan}
    for name in _all_nested_code(framework_dir):
        if name not in planned_names:
            raise UnplannedSparkleCodeError(
                f"nested Sparkle code not in the signing plan: {name}")


def combined_sign_plan(chromium_parts, sparkle_parts, outer_app):
    """Chromium nested parts, then Sparkle parts, then the outer app LAST."""
    plan = list(chromium_parts) + list(sparkle_parts)
    if outer_app in plan:
        raise ValueError("outer app must not appear among nested parts")
    plan.append(outer_app)
    return plan
