# Flametal: Dungeon Keeper 2 native macOS edition

The current native pipeline keeps the original 32-bit game simulation isolated in Wine and renders it in a separate arm64 AppKit/Metal 4 host:

`DK2 + Flame (i386/Wine) → shared protocol v9 → AppKit + Metal 4 (arm64)`

Flame (the underlying [DiaLight/Flame](https://github.com/DiaLight/Flame) patch layer this fork is built on) captures the game's Direct3D 3 command stream without asking WineD3D to render it. The native host owns presentation, scaling, focus, keyboard and mouse input. Absolute pointer coordinates use AppKit, raw relative motion and keyboard state use GameController, and scrolling uses AppKit's precise wheel events.

## Rendering paths

Two rendering paths coexist over the same bridge:

- **Legacy path** (default for everything): the game's original CPU pipeline
  emits pre-transformed screen-space FVF vertices; the host replays them
  through a fixed-function texture-stage combiner ported to Metal (multi-stage
  blending, `D3DTOP_BUMPENVMAP` environment bump mapping, per-draw
  depth/blend/cull state). The 2D overlay is reconstructed from the game's
  black/white matte pair with per-tile dirty tracking, and a tile only
  re-uploads when its composited pixels actually changed.
- **World-space mesh path** (protocol v9, opt-in, under active development):
  object-space or world-space meshes cross the bridge once (or per frame for
  deformed geometry via `DRAW_MESH_INLINE`), together with a per-frame camera,
  the scene light list and the engine's own 256-entry falloff LUT. The Metal
  vertex shader then performs projection and DK2's exact per-vertex point-light
  accumulation, replacing the original engine's per-vertex CPU loop. The first
  rerouted emitter is the deformed/dynamic mesh family (`sub_57B6D0`); enable it
  with `MeshGpuPath = true` in the `[gog]` section of `flame/config.toml` for
  A/B testing. The camera matrix is assembled in closed form from the same
  globals the original projection uses, so GPU output lands in the same clip
  space as legacy draws and z-testing orders the two paths correctly.

## Build and run

```sh
./macos/build-metal-host.zsh
./macos/run-metal-game.zsh
```

To build the self-contained app that can be handed to another Mac user:

```sh
./macos/build-metal-wrapper.zsh
open "dist/Dungeon Keeper II.app"
```

The resulting app contains the native Metal host, the pinned Wine runtime, and
Flame, but no Dungeon Keeper 2 executable, WAD, media, or other game data.

The game itself is never included. On the first run, a native folder picker
asks for the user's original Dungeon Keeper 2 installation, validates the GOG
1.7 executable and required WAD files, and copies it into the isolated prefix.
The selected installation is read-only and remains untouched. To import from a
known path without the picker, run:

```sh
./macos/import-original-game.zsh "/path/to/Dungeon Keeper 2"
```

The first public importer intentionally accepts an already installed original
GOG copy. Automating the GOG offline installer is kept separate so the normal
launch path does not depend on a particular third-party installer version.

The Metal launcher defaults to DK2 shadow level 3. The bounds-safe Flame
rasterizer keeps the original dynamic-shadow mode from writing outside its
32x32 coverage surface. Set `DK2_SHADOW_LEVEL=0`, `1`, or `2` before launching
to select a cheaper mode when profiling older hardware.

The packaging script pins Wine 11 and verifies its SHA-256 checksum. Maintainer
builds take the matching Release Flame payload from the CI cache; set
`DK2_FLAME_PAYLOAD=/path/to/artifact` to select one explicitly. A Developer ID
can be supplied with `DK2_CODESIGN_IDENTITY`; local builds use ad-hoc signing.

## Cursor assets

The game cursor is replayed as a final independent Metal quad, so it no longer
inherits overlay scaling or its black/white matte trail. Original RGBA cursor
sheets can be derived locally from the user's game for future HD replacements:

```sh
python3 tools/extract_dk2_cursors.py "/path/to/Dungeon Keeper 2" scratchpad/cursors --split-frames
```

The command writes the 15 original sheets, split animation frames, hotspots,
dimensions and hashes. These derived game assets are intentionally not stored
in the repository. Runtime cursor textures keep participating in the existing
hash-based `textures-hd` lookup.

## Isolation and data

- Native host: `macos/native/build/Dungeon Keeper II.app`
- Private prefix: `~/Library/Application Support/Dungeon Keeper II Metal/prefix`
- Bridge: `~/Library/Application Support/Dungeon Keeper II Metal/prefix/drive_c/dk2-metal/frame.bin`
- Log: `~/Library/Logs/Dungeon Keeper II Metal/game.log`

The private prefix exposes only its `C:` drive. Wine's `Z:` drive and links to macOS user folders are removed. Saves stay outside the application bundle, so rebuilding the app preserves them.

## Compatibility choices

- Native resizable AppKit window and standard macOS full-screen lifecycle.
- Fixed 4:3 Metal layer with correct letterboxing at every window size.
- Native cursor, absolute UI clicks, raw mouse deltas, wheel zoom, and DIK-compatible keyboard events.
- Focus loss releases all pressed input; a heartbeat also clears stuck state after a host crash.
- Wine is retained only for the original i386 simulation, audio, and resource loading.
- Native full screen stays available, but Game Mode is explicitly disabled with
  `GCSupportsGameMode` and `LSSupportsGameMode`: DK2's Wine simulation and the
  separate Metal replay process schedule more smoothly without Game Mode.

Apple's D3DMetal translation path is not used because DK2 is a 32-bit Direct3D 3/DirectDraw title. Metal 4 is used directly by the native renderer, following the lifecycle, display-link, residency, and input patterns in the GPTK 4 beta samples mounted with the toolkit.

References: [Apple Game Porting Toolkit](https://developer.apple.com/games/game-porting-toolkit/), [Apple GPTK repository](https://github.com/apple/game-porting-toolkit/), [Gcenx macOS Wine builds](https://github.com/Gcenx/macOS_Wine_builds), [WineHQ Dungeon Keeper 2 entry](https://appdb.winehq.org/objectManager.php?sClass=version&iId=3696), [Flame](https://github.com/DiaLight/Flame), [macOS Flame fork release](https://github.com/nasedkinpv/Flame/releases/tag/v1.7.0-260718-macos-native).
