# Dungeon Keeper 2 Flame for macOS

The current native pipeline keeps the original 32-bit game simulation isolated in Wine and renders it in a separate arm64 AppKit/Metal 4 host:

`DK2 + Flame (i386/Wine) → shared protocol v4 → AppKit + Metal 4 (arm64)`

Flame captures the game's Direct3D 3 command stream without asking WineD3D to render it. The native host owns presentation, scaling, focus, keyboard and mouse input. Absolute pointer coordinates use AppKit, raw relative motion and keyboard state use GameController, and scrolling uses AppKit's precise wheel events.

## Build and run

```sh
./macos/build-metal-host.zsh
./macos/run-metal-game.zsh
```

The Metal launcher defaults to DK2 shadow level 1. It keeps cached character
shadows while avoiding the original level-3 path that rebuilds animated shadow
geometry every frame. Set `DK2_SHADOW_LEVEL=2` or `3` before launching to trade
CPU time for the original dynamic-shadow modes.

The script pins and verifies both downloads. It reads the owned GOG installation from `~/.wine` by default; use `DK2_SOURCE_PREFIX=/path/to/prefix` to select another source. The source installation is never modified.

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
- `LSSupportsGameMode` and the games application category are enabled in `Info.plist`.

Apple's D3DMetal translation path is not used because DK2 is a 32-bit Direct3D 3/DirectDraw title. Metal 4 is used directly by the native renderer, following the lifecycle, display-link, residency, and input patterns in the GPTK 4 beta samples mounted with the toolkit.

References: [Apple Game Porting Toolkit](https://developer.apple.com/games/game-porting-toolkit/), [Apple GPTK repository](https://github.com/apple/game-porting-toolkit/), [Gcenx macOS Wine builds](https://github.com/Gcenx/macOS_Wine_builds), [WineHQ Dungeon Keeper 2 entry](https://appdb.winehq.org/objectManager.php?sClass=version&iId=3696), [Flame](https://github.com/DiaLight/Flame), [macOS Flame fork release](https://github.com/nasedkinpv/Flame/releases/tag/v1.7.0-260718-macos-native).
