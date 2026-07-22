
# Translate confident x87/integer leaf functions → modern instructions (SSE2/scalar) with bit-exact differential tests

## Context

Flametal replaces DKII.EXE functions at runtime via the Flametal loader. The proven confidence method is the **differential test**: rewrite the x87/integer leaf in modern C++ (SSE2 or scalar), then a one-file `*_difftest.cpp` compares the rewrite against a scalar reference over exhaustive inputs (incl. aliasing, denormals, inf, nan). Bit-exact equality holds for leaf functions: integer ops are identical everywhere; x87 float (one op per component, store to m32) rounds identically to SSE single-precision.

**Proven references (do not deviate from this pattern):**

- Rewrite: `src/dk2/Vec3f.cpp`, `src/dk2/Mat3x3f.cpp`
- Difftest: `tests/vec3f_difftest/vec3f_difftest.cpp` + local shim `tests/vec3f_difftest/dk2/utils/Vec3f.h` + `tests/vec3f_difftest/dk2_functions.h`
- Auto-gen struct prototypes w/ addresses: `libs/dkii_exe/api/dk2/utils/*.h`

## Disassembly (extract the original x87/integer to confirm semantics)

```
objdump -d --start-address=0xADDR --stop-address=0xEND -M intel libs/dkii_exe/DKII.EXE
```

Address `0xADDR` is the `/*XXXXXXXX*/` from the api header. Read enough bytes to find the next `ret`/`ret N` + padding. **Confirm it is a leaf**: no `call`/`e8` to other code, no global writes outside the struct args. If it calls other functions or touches globals, SKIP it (not a confident leaf) and pick the next.

## Per-iteration unit (ONE function, fully done)

1. **Pick** the next target from the ranked list below.
2. **Disasm** from DKII.EXE, confirm leaf + exact semantics (operation order, rounding, return register/ABI, whether output may alias `this`/args).
3. **Rewrite** in `src/dk2/<Type>.cpp` (create file if new type; append if type exists). SSE2 where it helps (`<emmintrin.h>`), scalar otherwise. Match the exact ABI (return value, `__cdecl` for free functions). Add a leading `// 0xADDR: ...` comment describing the original.
4. **Difftest**: create `tests/<type>_difftest/` containing:
   - `dk2/utils/<Type>.h` — clean shim matching the struct layout + only the methods under test (copy style from the Vec3f shim). NO windows headers.
   - `<type>_difftest.cpp` — `#include "../../src/dk2/<Type>.cpp"`, a scalar `ref*` reference, `bitEq` (compare as uint32, nan-aware), exhaustive value grid + **all aliasing combos** (output==this, output==right, this==right). Print `OK: N combinations` on success.
5. **Build & run** (must succeed): `clang++ -arch x86_64 -O2 -std=c++17 -I tests/<type>_difftest -o /tmp/<type>_difftest tests/<type>_difftest/<type>_difftest.cpp && /tmp/<type>_difftest`
6. **Wire** `src/CMakeLists.txt`: add the new `dk2/<Type>.cpp` to the source list (next to the existing `dk2/Mat3x3f.cpp`/`dk2/Vec3f.cpp` entries). Do NOT edit the auto-gen api header.
7. **Commit**: `feat: native SSE2/scalar <Type>::<method> (DKII 0xADDR) + difftest`. One function = one commit.

## Ranked confident targets (leaf, bit-exact-trivial)

Integer first (zero FP-rounding risk), then x87 FP:

1. **Vec3i::add** `0x00437FE0` — int3 add. Validates the whole pipeline. (struct: `int x,y,z`, 0xC bytes)
2. **Vec3d::addVec3d** `0x0040F680` — fields are `int` despite the name; integer add. (struct 0xC)
3. **Pos2i::shiftDiv** `0x004D1EC0` — int divide/shift (2-compo). Confirm signed rounding (C truncation vs arithmetic shift). (struct: `int x,y`, 0x8)
4. **AABB::contains** `0x0052D3A0` + **isIntersects** `0x005B7050` — integer compares, BOOL. Do both in one AABB batch. (struct: `int minX,minY,maxX,maxY`, 0x10)
5. **AABB::intersection** `0x005B6FD0` + **getOuter** `0x005B7090` — min/max.
6. **AABB::appendPoint** `0x00556590` + **move** `0x005DC2D0`.
7. **Vec3i::calcLength** `0x00555990` — likely integer sqrt; confirm exact algorithm from disasm before trusting.
8. **Mat3x3f difftests** (implementations already exist in `src/dk2/Mat3x3f.cpp`, but UNTESTED): write `tests/mat3x3f_difftest/` covering sub_594CB0 (matmul), multiplyVec/sub_594E10/fun_594E70 (mat-vec, rows vs columns), multiply (scalar), sub_594F30 (transpose). This hardens existing FP rewrites — high value, no new rewrite needed.

If any target turns out non-leaf (calls out / touches globals), note it and move to the next. Do not force a non-confident rewrite.

## Definition of done (per iteration)

- New/updated `src/dk2/*.cpp` rewrite present
- Difftest builds with the clang++ command above AND prints `OK: N combinations`
- `src/CMakeLists.txt` updated
- Committed with conventional message
- Report: function, address, one-line semantics, test count, any surprise from the disasm

## Hard rules

- Never edit `libs/dkii_exe/api/**` (auto-generated).
- Never skip the difftest — it is the only confidence gate.
- If `objdump` shows the function is NOT a clean leaf, abandon it and pick the next; say so in the report.
- Keep rewrites minimal: SSE2 only where it genuinely vectorizes; otherwise plain scalar. No abstractions.

## Progress log

### Iteration 1 — DONE

- [x] **Vec3i::add** `0x00437FE0` — committed `420a6f0`, then **param-order fix** `7c2af69`.
  - Pure integer leaf: `output = this + right`, two's-complement wraparound (uint32).
  - **BUG CAUGHT & FIXED:** original ABI is **param1=output (written, returned), param2=right (read)** — verified from disasm AND the Vec3f::mulV convention. First impl had them swapped; value commutes so only distinct-pointer callers were affected. Difftest now also asserts `right` is untouched, so a swap fails the test (verified by building a deliberately-swapped impl — it asserts).
- [x] **Vec3d::addVec3d** `0x0040F680` — committed `9493630`. Same pattern (fields are `int` despite the name). 2.98M combos.

### ⚠ CRITICAL CONVENTION (applies to ALL future iterations)

DKII member-fn ABI for "output + operand" methods: **param1 = output (written + returned), param2(+) = operands**. Confirmed via Vec3f::mulV/substractAssign disasm. Always verify from disasm which stack slot is written vs read. Difftests MUST assert the non-output operand is untouched when distinct, else a param-order bug hides behind a self-consistent test.

### Next: item 3 — Pos2i::shiftDiv `0x004D1EC0`

### Iteration 2 — DONE

- [x] **Pos2i::shiftDiv** `0x004D1EC0` — committed `4a5a39f`. 5415 combos.
  - Surprise: NOT a plain shift — it's **16.16 fixed-point division**: `output.c = ((int64)c << 16) / divisor` (truncate toward zero). The `shl eax,0x10 / sar edx,0x10 / idiv` idiom == `(int64)c << 16`, verified bit-exact incl. negatives via a standalone check.
  - x86 `idiv` faults (#DE) on divisor==0 or quotient overflow; test stays in `|v|<=32767` so quotients fit int32 (both orig & rewrite fault identically outside that).

### Next: item 4 — AABB::contains `0x0052D3A0` + isIntersects `0x005B7050`

### Iteration 3 — DONE

- [x] **AABB::contains** `0x0052D3A0` + **isIntersects** `0x005B7050` — committed `a39446c`. 390706 box-pairs.
  - Both pure integer predicates, BOOL (0/1), ret 4, read-only (no output/aliasing concern). contains = full enclosure (this.min≤other.min && this.max≥other.max per axis); isIntersects = inclusive overlap (touching edges count). Confirmed signed `jg`/`jl` → `<=`/`>=` mapping from disasm.
  - Grid allows degenerate boxes (min>max) and INT32 extremes to stress the comparisons.

### Next: item 5 — AABB::intersection `0x005B6FD0` + getOuter `0x005B7090` (min/max)

### Iteration 4 — DONE

- [x] **AABB::intersection** `0x005B6FD0` + **getOuter** `0x005B7090` — committed `23238f9`. 390625 box-pairs.
  - Both min/max integer leaves, param1=output/param2=other. intersection clamps `max = max(max, min)` per axis (disjoint boxes → zero-size boundary box); getOuter is plain union (no clamp). Traced the pointer-swap min/max selection trick carefully.
  - Difftest extended to all 4 AABB methods with output==this/output==other aliasing + operand-untouched assertions.

### Next: item 6 — AABB::appendPoint `0x00556590` + move `0x005DC2D0`
