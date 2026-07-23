# src/shared/dk2_core

Shared DK2 logic compiled into BOTH build targets:

- **Guest** — the Flametal x86 DLL (MSVC), injected into DKII.EXE.
- **Host** — the native ARM64 Metal host (`DK2Metal`, clang).

This is the structural home for the **native render core** mirror primitives
(host-side scene/cull/light logic that must produce bit-identical results to
the guest's x87/SSE2 computation). See `.porting/roadmap-native.md`,
"Native render core" / "Portable-logic" sections.

## Hard rules (enforced, not aspirational)

1. **Zero OS / Windows / x86 dependencies.** No `<windows.h>`, no DirectX types,
   no `<emmintrin.h>` / `__m128` / SSE intrinsics. The host is ARM64 — x86 SIMD
   literally will not compile there, which is the forcing function: a stray
   `__m128` in shared code breaks the arm64 difftest build immediately instead
   of silently diverging in semantics (the `sub_57BBF0` double-translation /
   LNK2005 bug was exactly this class of error).
   Allowed headers: `<cstdint>`, `<cstring>`, plain scalar C++.
2. **One canonical translation per DKII function.** Before translating a
   function, grep `dk2_core` and the wider tree — do not reimplement what
   already lives here. Every function keeps its difftest.
3. **Float determinism across architectures.** SSE2 (guest x86 MSVC, `/fp:precise`)
   and ARM64 scalar (host clang) produce identical IEEE-754 results for plain
   `+ - * /` **without FMA contraction and without fast-math**. Build both with
   `-ffp-contract=off` explicitly (MSVC is already `/fp:precise`; do not switch
   to `/fp:fast`). The only iron proof that this holds in practice is the dual
   difftest below — theory is not enough.
4. **Difftest twice: `-arch x86_64` AND `-arch arm64`.** Same source, both archs,
   both must pass bit-exactly. The arm64 run is cheap on Apple Silicon (native)
   and is the cross-arch-determinism + no-SIMD gate.

## Calling-convention note

Some functions carry `__fastcall` / `__thiscall` to match the DKII.EXE mangled
symbol the delinker externalizes (guest x86 MSVC only). On the host (arm64)
these specifiers are ignored (one fixed calling convention) and emit a benign
`-Wignored-attributes` warning under the difftest build. When the host actually
links `dk2_core` (Phase 1 mirror), gate the convention behind a macro that is
empty off MSVC-x86 so the `-Werror` host build stays clean.

## Residents

- `sub_57BBF0.cpp` — DKII 0x0057BBF0, batched sphere-cull bitmask (first
  resident; was `src/dk2/sub_57BBF0.cpp`). Difftest: `tests/sub_57BBF0_difftest/`.

## Candidates (move here when self-contained)

- frustum/sphere cull `Vec3f_static_sub_575D70` / `_575F10` (currently inlined
  in `CEngineStaticMeshAdd.cpp` — extract the pure-math core here if it does
  not depend on Windows types).
- bounding-sphere math (`sub_57AA40` center+radius), once translated.
- future mirror primitives (visibility, cell-walk) for the native render core.

## Build wiring

- Guest DLL: `src/CMakeLists.txt` lists `shared/dk2_core/<file>.cpp`.
- Host binary: NOT linked yet (deferred to Phase 1 — the host does not consume
  `dk2_core` until the scene mirror exists; linking unused code into the
  `-Werror -Wextra` host build now would trip unused-symbol warnings). The dual
  difftest already enforces the rules above in the meantime.
