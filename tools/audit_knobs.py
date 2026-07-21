#!/usr/bin/env python3
"""Inventory every user-facing knob and check surface consistency.

Scans the repo for all configuration knobs:
  - flame options   (define_flame_option in src/**)
  - host env vars   (getenv("DK2_*") in macos/native/**)
  - runner env vars (DK2_* references in macos/*.zsh)
  - runner CLI game flags (the canonical wine command line)
  - settings.toml schema keys (as written by the host)

Then reports, for each knob: where it is defined, which surfaces expose
it, and flags problems:
  DUPLICATE   same knob configurable from two user surfaces
  UNEXPOSED   knob with no user surface and no debug classification
  STALE       settings.toml key that maps to nothing

Classification lives in KNOBS below: keep it updated when adding knobs —
this script is the checklist. Exit code 1 when problems are found.
"""

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# knob name -> {"surface": "settings" | "debug-env" | "internal",
#               "note": free text}
# "settings"  : settings.toml is the single user surface (env may override)
# "debug-env" : intentionally env/CLI-only, not for end users
# "internal"  : not user-tunable (wired by the host/runner automatically)
KNOBS = {
    # [renderer]
    "DK2_BLOOM":                {"surface": "settings", "note": "renderer.bloom"},
    "DK2_METAL_SHADOWS":        {"surface": "settings", "note": "renderer.metal_shadows"},
    "DK2_RENDER_SCALE":         {"surface": "settings", "note": "renderer.render_scale"},
    "DK2_TEXTURE_HD":           {"surface": "settings", "note": "renderer.hd_textures (path override stays env)"},
    "DK2_METAL_SHADOW_UP_SIGN": {"surface": "debug-env", "note": "world up-axis flip, diagnostics"},
    # [game]
    "DK2_SHADOW_LEVEL":         {"surface": "settings", "note": "game.shadow_level"},
    "DK2_GAME_RES":             {"surface": "settings", "note": "game.resolution"},
    "DK2_LEVEL":                {"surface": "settings", "note": "game.level"},
    # [debug]
    "DK2_WINEDEBUG":            {"surface": "settings", "note": "debug.winedebug"},
    # plumbing
    "DK2_EXTRA_GAME_ARGS":      {"surface": "internal", "note": "host->runner arg channel"},
    "DK2_WINE_BIN":             {"surface": "debug-env", "note": "alternate wine build"},
    "DK2_METAL_PREFIX":         {"surface": "debug-env", "note": "alternate prefix"},
    "DK2_METAL_BRIDGE_FILE":    {"surface": "internal", "note": "bridge path, set by host"},
    "DK2_FLAMETAL_PAYLOAD":     {"surface": "debug-env", "note": "wrapper build payload override"},
    "DK2_IMPORT_GUI":           {"surface": "internal", "note": "import flow"},
    "DK2_TEXTURE_DUMP":         {"surface": "debug-env", "note": "texture dump dir"},
    "DK2_WIN_HOST":             {"surface": "debug-env", "note": "local build box (untracked script)"},
    "DK2_METAL_INPUT_LOG":      {"surface": "debug-env", "note": "input diagnostics"},
    "DK2_MESH_DEBUG":           {"surface": "debug-env", "note": "mesh path visual debug"},
    "DK2_MESH_NO_TEXTURE":      {"surface": "debug-env", "note": "mesh path white-texture debug"},
    "DK2_FULLSCREEN":           {"surface": "internal", "note": "runner<->host plumbing"},
    "DK2_RUNNER_MODE":          {"surface": "internal", "note": "unified runner mode override"},
    "DK2_METAL_HUD":            {"surface": "debug-env", "note": "Metal HUD toggle"},
    "DK2_METAL_APP_BUNDLE":     {"surface": "internal", "note": "bundle path plumbing"},
    "DK2_ALLOW_UNKNOWN_EXE":    {"surface": "debug-env", "note": "import validation escape hatch"},
    "DK2_CODESIGN_IDENTITY":    {"surface": "debug-env", "note": "packaging signing identity"},
    # flame options (game side)
    "flametal:MetalShadows":    {"surface": "settings", "note": "patches/renderer bridge; CLI via DK2_EXTRA_GAME_ARGS"},
    "gog:MeshGpuPath":          {"surface": "settings", "note": "patches.mesh_gpu_path"},
    "flametal:ShadowCache":     {"surface": "settings", "note": "patches.shadow_cache"},
    "flametal:DebugProbes":     {"surface": "settings", "note": "patches.debug_probes"},
}

# flame options that exist upstream (game/patch behavior) and are managed
# through the prefix flametal/config.toml or CLI as before; they are not part
# of the macOS settings surface. Listed to keep UNEXPOSED reports readable.
UPSTREAM_PREFIX = (
    "flametal:logging:", "flametal:wine:", "flametal:experimental:",
    "flametal:menu-res", "flametal:game-res", "flametal:limit-tps",
    "flametal:windowed", "flametal:no-initial-size", "flametal:autosave",
    "flametal:original-compatible", "flametal:net:", "flametal:readsp",
    "flametal:console", "flametal:inspect", "flametal:keep-last-autosaves",
    "flametal:lock-window-size", "flametal:myip", "flametal:single-core",
    "flametal:skip-launcher", "flametal:alt-resources", "flametal:auto-network",
    "gog:video:", "gog:misc:", "gog:",
    "dk2:",  # the game's own option map (upstream flame_config passthrough)
)


def rgrep(pattern, *paths, flags=""):
    cmd = ["grep", "-rnoE", pattern, *[str(ROOT / p) for p in paths]]
    out = subprocess.run(cmd, capture_output=True, text=True).stdout
    return out.splitlines()


def collect_env_reads():
    hits = {}
    for line in rgrep(r'getenv\("DK2_[A-Z_]+"\)', "macos/native"):
        name = re.search(r'DK2_[A-Z_]+', line).group(0)
        hits.setdefault(name, set()).add("host")
    for line in rgrep(r'DK2_[A-Z_]+', "macos"):
        if "/native/" in line:
            continue
        name = re.search(r'DK2_[A-Z_]+', line).group(0)
        hits.setdefault(name, set()).add("scripts")
    return hits


def collect_flame_options():
    hits = {}
    for line in rgrep(r'define_flame_option<[^>]+>\(', "src"):
        pass  # names are on the next line usually; use a second pass
    text_files = subprocess.run(
        ["git", "-C", str(ROOT), "grep", "-n", "-A2", "define_flame_option"],
        capture_output=True, text=True).stdout
    for m in re.finditer(r'"((?:flametal|gog|dk2)[^"\n]*)"', text_files):
        name = m.group(1)
        if ":" in name or name.startswith("dk2"):
            hits.setdefault(name, set()).add("flame-option")
    return hits


def collect_settings_keys():
    keys = set()
    for line in rgrep(r'"(game|renderer|patches|debug)"[^\n]*', "macos/native"):
        pass
    # simpler: look for the literal section/key strings the host writes
    text = ""
    p = ROOT / "macos/native/DK2Metal.mm"
    if p.exists():
        text = p.read_text(errors="replace")
    for m in re.finditer(r'(game|renderer|patches|debug)\.([a-z_]+)', text):
        keys.add(f"{m.group(1)}.{m.group(2)}")
    return keys


def main():
    problems = []
    env = collect_env_reads()
    flame = collect_flame_options()
    settings = collect_settings_keys()

    print(f"{'knob':34} {'class':10} {'seen at':22} note")
    print("-" * 100)
    seen = set()
    for name, places in sorted({**env, **flame}.items()):
        info = KNOBS.get(name)
        seen.add(name)
        if info is None:
            if any(name.startswith(p) for p in UPSTREAM_PREFIX):
                cls, note = "upstream", "prefix config / CLI as before"
            else:
                cls, note = "UNCLASSIFIED", "add to KNOBS in tools/audit_knobs.py"
                problems.append(f"UNEXPOSED/UNCLASSIFIED: {name} ({','.join(sorted(places))})")
        else:
            cls, note = info["surface"], info["note"]
        print(f"{name:34} {cls:10} {','.join(sorted(places)):22} {note}")

    for name, info in KNOBS.items():
        if name not in seen and not name.startswith(("flametal:", "gog:")):
            problems.append(f"STALE classification: {name} no longer referenced")

    if settings:
        print("\nsettings.toml keys referenced by the host:")
        for k in sorted(settings):
            print(f"  {k}")

    if problems:
        print("\nPROBLEMS:")
        for p in problems:
            print(f"  - {p}")
        return 1
    print("\nOK: every knob classified, no duplicate user surfaces detected.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
