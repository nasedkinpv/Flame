# Dungeon Keeper 2 Flame for macOS

This builds an isolated macOS application around the original 32-bit GOG game:

`DirectDraw/Direct3D 2 → WineD3D → OpenGL → macOS graphics driver`

It uses the Wine 11.0 macOS build from Gcenx and a small Flame fork that keeps windowed mode inside a native AppKit window. The fork also keeps DirectDraw presenting while the window loses focus, so switching to another app no longer leaves a black game window with audio still playing. There is no Wine virtual desktop, dgVoodoo, DXMT, or replacement DirectPlay DLL.

## Build and run

```sh
./macos/build-wrapper.zsh
open "dist/Dungeon Keeper 2 Flame.app"
```

The script pins and verifies both downloads. It reads the owned GOG installation from `~/.wine` by default; use `DK2_SOURCE_PREFIX=/path/to/prefix` to select another source. The source installation is never modified.

## Isolation and data

- App: `dist/Dungeon Keeper 2 Flame.app`
- Private prefix: `~/Library/Application Support/Dungeon Keeper 2 Flame/prefix-native`
- Log: `~/Library/Logs/Dungeon Keeper 2 Flame/game.log`

The private prefix exposes only its `C:` drive. Wine's `Z:` drive and links to macOS user folders are removed. Saves stay outside the application bundle, so rebuilding the app preserves them.

## Compatibility choices

- Native Wine/AppKit window, not a Wine virtual desktop.
- 640×480 menus and 1024×768 gameplay in one resizable native window; the split avoids blank DirectDraw startup surfaces.
- Builtin Wine DirectDraw, D3DImm, and DirectInput are forced; bundled GOG compatibility DLLs are preserved but disabled in the private copy.
- Flame's default single-core compatibility mode is retained. Removing it did not improve measured frame rate and made repeated starts less stable.
- WineD3D's default OpenGL renderer is used. Its default command-stream threading is retained; the old WineHQ advice to disable CSMT crashes this build of Wine 11.
- `MouseWarpOverride=disable`, matching the WineHQ AppDB recommendation for this version of the game.
- `LSSupportsGameMode` and the games application category enabled in `Info.plist`.

Vulkan support is present in the bundled Wine, but forcing WineD3D's Vulkan backend is not viable for this title: its windowed DirectDraw path reaches unimplemented drawable-to-texture transfers and produces a black surface. The Apple vendor fallback message from WineD3D is only a capability-profile heuristic, not the cause of the blank window.

Apple's D3DMetal path is not used: Dungeon Keeper 2 is a 32-bit Direct3D 2/DirectDraw title, while current D3DMetal targets newer 64-bit D3D APIs. DXVK, DXMT, dgVoodoo and DDraw-to-D3D9 combinations were also tested; they either lack required MoltenVK features, fail at the Wine macOS driver boundary, or render incorrectly for this game.

References: [Apple Game Porting Toolkit](https://developer.apple.com/games/game-porting-toolkit/), [Apple GPTK repository](https://github.com/apple/game-porting-toolkit/), [Gcenx macOS Wine builds](https://github.com/Gcenx/macOS_Wine_builds), [WineHQ Dungeon Keeper 2 entry](https://appdb.winehq.org/objectManager.php?sClass=version&iId=3696), [Flame](https://github.com/DiaLight/Flame), [macOS Flame fork release](https://github.com/nasedkinpv/Flame/releases/tag/v1.7.0-260718-macos-native).
