# Flametal: Dungeon Keeper 2 native macOS edition

The current native pipeline keeps the original 32-bit game simulation isolated in Wine and renders it in a separate arm64 AppKit/Metal 4 host:

`DK2 + Flametal (i386/Wine) → shared protocol v13 → AppKit + Metal 4 (arm64)`

Flametal (the underlying [DiaLight/Flame](https://github.com/DiaLight/Flame) patch layer this fork is built on) captures the game's Direct3D 3 command stream without asking WineD3D to render it. The native host owns presentation, scaling, focus, keyboard and mouse input. Absolute pointer coordinates use AppKit, raw relative motion and keyboard state use GameController, and scrolling uses AppKit's precise wheel events.

## Rendering paths

Two rendering paths coexist over the same bridge:

- **Legacy path** (default for everything): the game's original CPU pipeline
  emits pre-transformed screen-space FVF vertices; the host replays them
  through a fixed-function texture-stage combiner ported to Metal (multi-stage
  blending, `D3DTOP_BUMPENVMAP` environment bump mapping, per-draw
  depth/blend/cull state). The 2D overlay is reconstructed from the game's
  black/white matte pair with per-tile dirty tracking, and a tile only
  re-uploads when its composited pixels actually changed.
- **World-space mesh path** (introduced in protocol v9, retained protocol in
  v13): static and dynamic topology, normals, local UVs and indices cross the
  bridge once through `MESH_REGISTER`. Static/dynamic draws then carry only a
  transform, atlas UV transform, material and the engine's exact per-object
  light indices. Animated topology is also retained; `DRAW_MESH_DEFORMED`
  sends only interpolated `float3` positions each frame. The Metal vertex
  shader performs projection and DK2's per-vertex point-light accumulation
  with the original 256-entry falloff LUT. Opaque repeats of one topology can
  be emitted as one indexed instanced draw; transparent/additive order is
  preserved. The camera matrix is assembled from the same globals as the
  original projection, so GPU and legacy draws share clip/depth space.

Dynamic shadows retain DK2's original per-light selection, projection,
receiver decal, UVs, blend and depth behavior. With `metal_shadows = true`,
only the hot 256x256-subpixel silhouette rasterizer and its 8x8 reduction move
to Metal; the result is resolved into the same 32x32 atlas slot the original
draw samples. Turning it off live returns that one step to the bounds-safe CPU
rasterizer. `shadow_cache` is automatically bypassed while the Metal
rasterizer is active, because caching the deliberately blank CPU scratch mask
would suppress the projected triangles sent to the GPU.

The old inline experiment was slower than the translated SSE2 CPU transform,
because it recopied every vertex and issued roughly 1300 small draws each
frame. Protocol v13 removes that feeding cost and is now the default. DK2's
terrain subdivision still produces mostly distinct topologies, so instancing
is opportunistic rather than the main win; retained upload, compact animated
positions and exact per-draw light selection are the important reductions.
Set `mesh_gpu_path = false` only for a restart-based A/B against the legacy CPU
transform path.

The renderer deliberately uses one `CAMetalLayer`. World geometry and the
legacy/UI overlay stay ordered in the same scene render pass; only real data
dependencies get separate passes (shadow-mask resolve and the bloom chain).
This follows Apple's tile-rendering guidance: splitting UI/world into Core
Animation layers or extra passes would add attachment load/store traffic and
would not improve texture ownership.

Texture residency is frame-local. Three residency sets mirror the three
in-flight frame slots and contain only textures referenced by that slot's
argument tables; replacing an atlas therefore cannot keep every historical
version resident. Protocol v13 also retires the host texture, its dynamic ring,
and named-atlas metadata when the owning DirectDraw surface is destroyed.
Metal I/O remains a future loading optimization, not a lifetime manager;
sparse textures are intentionally not used for DK2's small 256→1024 atlas
pages, where tile mapping and access-counter LRU would cost more than they save.

## Settings

`~/Library/Application Support/Dungeon Keeper II/settings.toml` is the one
user-facing config file: OS-neutral, hand-editable, and parsed with
[toml11](https://github.com/ToruNiina/toml11) (header-only, vendored at
`libs/Toml11-4.4.0`). It's created with commented defaults the first time the
host runs, migrating `MetalShadows`/`ShadowCache`/`DebugProbes`/`MeshGpuPath`
out of the prefix's `flametal/config.toml` if that already exists from an
older build. From then on, that prefix config is host-generated CLI passthrough
plumbing, not something players edit.

```toml
[game]
shadow_level = 3       # 0 (off) - 3 (full detail, cached silhouettes)
resolution = "1600x1200"  # widescreen (e.g. 1800x1200) is experimental: the
                           # HUD lays out for 4:3 and shows black gaps beside
                           # the bottom toolbar at wider aspects
movies = false
level = ""              # non-empty skips the normal menu and loads this level

[renderer]              # all four keys apply live, no restart needed
bloom = true
metal_shadows = true
render_scale = 1.0       # fraction of the display's native backing resolution, 0.375-1.0
hd_textures = true

[patches]                # applied on next launch, passed to the game as
mesh_gpu_path = true      # -gog:MeshGpuPath=/-flametal:ShadowCache=/
shadow_cache = true       # -flametal:DebugProbes= CLI flags; CPU shadows only
debug_probes = false

[debug]
winedebug = "-all"
```

Edit the file directly, or open **Settings…** (Cmd-,) in the game host for a
native AppKit editor over the same file — every control saves immediately
(single atomic-replace writer), there is no separate Apply/OK step. `[renderer]`
controls are marked as applying immediately in the window; everything else is
marked "restart required" since it only takes effect the next time the game
(not the host) launches, and the window keeps that in mind rather than
pretending a live in-place change happened.

The host watches the file (a `DISPATCH_SOURCE_TYPE_VNODE` source, re-armed
across the atomic-replace renames a save performs) and live-applies bloom,
metal_shadows, render_scale and hd_textures the moment the file changes,
whether edited by hand or through Settings. Toggling `hd_textures` marks every
mapped atlas page dirty: off rebuilds pages from their stored original pixels,
and on recomposes them from the named pack. Rebuilds are budgeted across frames,
so the visible transition is live but may take several frames.

**Env overrides are a debug-only layer**, documented here once instead of
scattered across the source: any `DK2_*` variable already present in the
environment when the host starts wins over the matching settings.toml key,
on every key the host manages. This exists for quick one-off profiling
without touching the config file, not for regular use:

- `DK2_BLOOM`, `DK2_METAL_SHADOWS`, `DK2_RENDER_SCALE` — pin a `[renderer]`
  value for the process; the file (and Settings window) stop reaching it.
- `DK2_SHADOW_LEVEL`, `DK2_GAME_RES`, `DK2_LEVEL`, `DK2_WINEDEBUG`, `DK2_MOVIES`,
  `DK2_EXTRA_GAME_ARGS` — the same environment the host composes for
  `dk2-runner.zsh` from `[game]`/`[patches]`/`[debug]`; set any of these
  before launching (or export them in a dev shell) to override that one key.
- `DK2_TEXTURE_HD` selects the HD texture directory (default
  `.../Dungeon Keeper II/textures-hd`) for legacy whole-texture hash matches.
- `DK2_RESOURCE_PACK_DIR` selects the named atlas pack (default
  `.../Dungeon Keeper II/resource-pack/textures`). A file such as
  `Slab Floor.png` replaces that named resource wherever DK2 packs it, while
  unmapped pixels preserve the original page. Both directories obey the same
  live `hd_textures` toggle, which has no environment override.
- `DK2_METAL_INPUT_LOG`, `DK2_MESH_DEBUG`, `DK2_MESH_NO_TEXTURE`,
  `DK2_TEXTURE_DUMP`, `DK2_METAL_HUD`, `DK2_FULLSCREEN`
  remain plain debug/dev knobs with no settings.toml equivalent — see their
  definitions in `macos/native/DK2Metal.mm` and `macos/run-metal-game.zsh`.

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
Flametal, but no Dungeon Keeper 2 executable, WAD, media, or other game data.

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

The Metal launcher defaults to DK2 shadow level 3 (`settings.toml` [game]
`shadow_level`). The Metal rasterizer reproduces the original projected mask;
the bounds-safe Flametal CPU implementation remains the live fallback. Both
keep dynamic shadows inside their 32x32 coverage surface. Set the level to
`0`, `1`, or `2` for a cheaper mode when profiling older hardware.

The packaging script pins Wine 11 and verifies its SHA-256 checksum. Maintainer
builds take the matching Release Flametal payload from the CI cache; set
`DK2_FLAMETAL_PAYLOAD=/path/to/artifact` to select one explicitly. A Developer ID
can be supplied with `DK2_CODESIGN_IDENTITY`; local builds use ad-hoc signing.

## The runner script

`macos/dk2-runner.zsh` is the single script that launches Wine, both in a repo
checkout and inside the packaged `.app` (where `build-metal-wrapper.zsh` copies
it in as `Contents/Resources/dk2-game-runner`). It auto-detects which mode it's
in from its own location (a bundled `Resources/wine` next to it means packaged
mode: first-run import flow, Flametal payload sync, and error dialogs via
`osascript`; otherwise dev mode: `.cache/`-relative Wine, plain log output).
`macos/dk2-wine-runner.zsh` still exists as a deprecated one-line shim that
execs `dk2-runner.zsh`, so old muscle-memory invocations keep working.

The host composes this script's environment from `settings.toml` before
spawning it (see Settings above): `DK2_LEVEL`, `DK2_SHADOW_LEVEL`,
`DK2_GAME_RES`, `DK2_WINEDEBUG`, `DK2_MOVIES`, `DK2_EXTRA_GAME_ARGS`.
`DK2_WINE_BIN`/`DK2_METAL_PREFIX` are dev-flow-only path overrides with no
settings.toml equivalent.

The removed `macos/build-wrapper.zsh` and `macos/dk2-flametal.zsh` (and the
`macos/Info.plist` that only they used) were the legacy single-executable,
windowed-only packaging path, superseded by `build-metal-wrapper.zsh` +
the native Metal host.

The packaged app removes Wine's unused Mono and Gecko installers, DirectDraw/
WineD3D frontends, OpenGL/Vulkan modules, and MoltenVK from its staged runtime.
The source Wine archive and `.cache` runtime stay intact, so dev builds retain
the explicit `DK2_HEADLESS_DDRAW=0` A/B fallback. Packaged builds are native-
surface-only and reject that fallback instead of silently loading a graphics
stack the app no longer ships.

## Flametal DLL build options

The `FLAMETAL_WELCOME_WINDOW` CMake option (default `OFF`) controls the
upstream [Flame](https://github.com/DiaLight/Flame) ImGui welcome/options
window. We always launch with `-skip-launcher`, so this window never runs in
practice; when the option is off, its sources are excluded from the `flametal`
target and ImGui/D3DX9 are not linked into the DLL at all. The code is kept
around (guarded by `#ifdef FLAMETAL_WELCOME_WINDOW`) for future merges with
upstream. Pass `-DFLAMETAL_WELCOME_WINDOW=ON` to build it in.

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
- Private prefix: `~/Library/Application Support/Dungeon Keeper II/prefix`
- Bridge: `~/Library/Application Support/Dungeon Keeper II/prefix/drive_c/dk2-metal/frame.bin`
- Log: `~/Library/Logs/Dungeon Keeper II/game.log`

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

References: [Apple Game Porting Toolkit](https://developer.apple.com/games/game-porting-toolkit/), [Metal residency sets](https://developer.apple.com/documentation/metal/mtlresidencyset), [Metal resource loading](https://developer.apple.com/documentation/metal/resource-loading), [Metal sparse texture memory](https://developer.apple.com/documentation/metal/managing-sparse-texture-memory), [Apple GPTK repository](https://github.com/apple/game-porting-toolkit/), [Gcenx macOS Wine builds](https://github.com/Gcenx/macOS_Wine_builds), [WineHQ Dungeon Keeper 2 entry](https://appdb.winehq.org/objectManager.php?sClass=version&iId=3696), [Flame](https://github.com/DiaLight/Flame), [Flametal releases](https://github.com/nasedkinpv/Flametal/releases).
