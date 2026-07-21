# Flametal

Flametal is a fork of [DiaLight/Flame](https://github.com/DiaLight/Flame), which modifies the Dungeon Keeper 2 code to fix bugs found in both single and multiplayer and works with the Disk, Steam and GOG versions of the game. The goal of this fork is preservation: keeping Dungeon Keeper 2 running natively on current hardware, starting with Apple Silicon Macs and, longer term, iOS.

Warning: Saves and network sessions between Flame/Flametal and non-Flame Dungeon Keeper 2 versions are [incompatible](https://github.com/DiaLight/Flame/issues/57).
But you can use `-original-compatible` flag to disable some patches that breaks compatibility.

## Credits

All of the original decompilation work, the DLL function-replacement approach, and the multiplayer/singleplayer bug fixes are [DiaLight](https://github.com/DiaLight)'s, from the upstream [Flame](https://github.com/DiaLight/Flame) project. Flametal builds the native macOS edition described below on top of that foundation; it isn't a rewrite or a replacement of DiaLight's work.

## Native macOS edition

This fork adds a self-contained Apple Silicon app that imports the user's original GOG 1.7 game into an isolated Wine prefix, then streams its Direct3D 3 command buffer to a native AppKit/Metal 4 renderer instead of going through WineD3D. Highlights:

- **Rendering bridge**: DK2's D3D3/DirectDraw4 calls are captured over a shared-memory ring buffer and replayed by a native Metal 4 host - including a from-scratch fixed-function texture-stage combiner (multi-stage texture blending, `D3DTOP_BUMPENVMAP` environment bump mapping for water/lava) and content-compare dirty tracking for the 2D UI overlay (a tile re-uploads only when its composited pixels actually changed).
- **GPU mesh pipeline (in progress)**: a second, opt-in rendering path (bridge protocol v9) moves DK2's per-vertex CPU work - projection and point-light accumulation - into the Metal vertex shader. Meshes cross the bridge in object/world space with a per-frame camera, light list and the engine's own falloff LUT; the lighting model is a bit-exact port of the original. First rerouted emitter: the deformed/dynamic mesh family (`MeshGpuPath = true` in `[gog]`).
- **Native windowing and input**: real AppKit window/fullscreen lifecycle, Retina/high-resolution output, native cursor and keyboard/text input routing, one-command launch and first-run game import.
- **Performance**: a large ongoing campaign translating hot original x87 engine paths (vertex/matrix math, lighting, mesh and animation traversal, spatial queries, shadows) into SSE2 C++, plus CPU-side frame telemetry on both sides of the bridge to find the next hotspot.
- **Textures**: optional HD texture replacement, a texture dump/curation pipeline (collage/sprite detection, batch image upscaling), and 16-bit bump-map pixel format support.

See [macos/README.md](macos/README.md) for the build, packaging, import and run instructions. No copyrighted game data is included.

## How to report a bug

1) If you have any bugs in the game, please describe them in the [GitHub issues](https://github.com/nasedkinpv/Flametal/issues).
2) It helps a lot if you include steps how to reproduce found bug
3) Attaching a good test map is welcome

If you are reporting several bugs, please open separate issues for each one. Please, be sure to have followed the recommended installation steps.

### What bugs are in priority to fix?
Imagine you are playing through a storyline campaign and there are moments
that are extremely frustrating or simply prevent you from progressing further in the game.
These bugs, I consider them critical, and those are the ones I will focus on fixing.

You can vote for an bug that you consider critical at your discretion by placing a rocket emoji(?) on the corresponding issue


## How to install
1) Go to the [releases](https://github.com/DiaLight/Flame/releases) page and download the Flame-1.7.0-*.zip file of the newest release
2) Extract the zip file into your Dungeon Keeper 2 game directory
3) That's it. Now you can run `DKII-DX.exe` as usual

Note: It is possible to find newer test builds on [github actions](https://github.com/DiaLight/Flame/actions)

Note 2: The `Data` directory are not required for this to work, but are recommended.

## Files explained

The `Data` folder in the zip file contains patches by Quuz for level editor

# For Software Developers

## How it is done

Flametal is a new approach to modifying the compiled code of Dungeon Keeper 2

Flametal recompiles some functions of `DKII.EXE` into a separate `flametal/Flametal.dll` file.
`DKII-DX.EXE` depends on `PATCH.dll`. I decompiled whole `PATCH.dll` functional and included it into `flametal/Flametal.dll`.
Flametal comes with its own `PATCH.dll` which handles loading `flametal/Flametal.dll` and replacing the references to
the original functions with the references to recompiled functions.
Recompiled functions are supplemented with switchable changes that fix some game bugs and add some functionality

### History

[Earlier](https://github.com/DiaLight/Flame/tree/93e04efaba41bb3a574b33a0a8d91d2f63d4b31d "Exe merge approach"), this project implemented an approach to recompiles some functions of `DKII.EXE` into a separate `.exe` file.
Then it merges this file with the original `.exe` file, replacing the references to the original functions with the references to recompiled functions. Due to development and debug complexity the exe merge method was replaced by the dll dynamic loading with function replacement.

Also [Earlier](https://github.com/DiaLight/Flame/tree/46e5b0c1df93060bd01a83bb6d14d064e9c8c3dc "Full relinking approach"), this project implemented an approach to fully relinking `DKII.EXE`,
which contains false positive references that caused new bugs. Due to problems with false positive references, the relinking method was replaced by the exe merge method.

## Build requirements
- CMake 3.25 or higher https://cmake.org/download/
- Visual Studio 2022
- Dungeon Keeper II v1.70 (GOG/Steam version)
- Python 3 https://www.python.org/downloads/windows/

## How to build
cmd (instructions is not for powershell):
- `mkdir build && cd build`
- `"D:\Program Files\Visual Studio Community\2022\VC\Auxiliary\Build\vcvars32.bat"`
- `cmake -DCMAKE_BUILD_TYPE=Debug -GNinja -DCMAKE_INSTALL_PREFIX=../install ..`
- `cmake --build .`
- `cmake --install .`
- `copy /Y "..\install\PATCH.dll" "<Dungeon Keeper2 dir>\PATCH.dll"`
- `copy /Y "..\install\flametal" "<Dungeon Keeper2 dir>\flametal"`
