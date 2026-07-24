# ghidra-sync — import `.sgmap` names into a live Ghidra session

`ImportSgmapSymbols.java` is a **GhidraScript** that reads the hand-curated
symbol map `mapping/DKII_EXE_v170.sgmap` and pushes its function/global names
into the program **currently open in the Ghidra GUI**, so the disassembly and
decompiler show meaningful names such as

```
CEngineStaticHeightField::appendToSceneObject2EList
```

instead of `FUN_00587060` / `DAT_00xxxxxx`. It replaces the manual, per-symbol
cross-referencing of the `.sgmap` file by hand.

This is a **one-way** push (`.sgmap` → Ghidra). It never writes back to
`.sgmap`.

## What it does

- Parses every top-level `global: va=<hex>,name=<name>[,size=..][,member_of=<structId>]`
  line and every `struct: id=..,name=..` line (using the same simple
  comma-separated `key=value` format as `tools/genapi/sgmap.py`).
- For each global it resolves the Ghidra address with `toAddr(va)`:
  - if a **function** exists there → `function.setName(...)`,
  - otherwise → `createLabel(...)` for the data/global address.
- When a global has a `member_of` struct reference, the name is qualified as
  `Class::method` (this both matches the C++ view and makes otherwise-duplicate
  names like `constructor` / `appendToSceneObject2EList` unique). Names that are
  already qualified in `.sgmap` are used as-is.
- Everything runs inside the single transaction the Script Manager already
  opens around a script, so the whole import is **one undo** (Edit → Undo).

## Safety model (what gets overwritten vs skipped)

Default (`OVERWRITE_EXISTING = false`, top of the script):

- **Overwrites only Ghidra's own placeholder names** — symbols whose
  `SourceType` is `DEFAULT` (`FUN_`, `LAB_`, `DAT_`, `SUB_`, …) and addresses
  with no symbol at all.
- **Skips and logs** any address that already has a *real* name — one a human
  typed, or one Ghidra's analysis recovered from RTTI/debug info
  (`USER_DEFINED`, `ANALYSIS`, `IMPORTED`, …). Your manual work is never
  clobbered.
- Bad/duplicate/invalid names are caught and logged, never fatal — the run
  continues.

Flip `OVERWRITE_EXISTING = true` only once you consider the `.sgmap`
authoritative and want it to win over everything.

**Idempotent / safe to re-run.** Names that already match are counted as
"already correct" and left alone; re-running never duplicates or corrupts
symbols.

## How to run it (against your live, already-open project)

You do **not** need to close Ghidra, and you must **not** run `analyzeHeadless`
against the open project — that would fight the GUI over the locked project
files. Run this script interactively instead:

1. In the running Ghidra GUI, with the DKII program open, open
   **Window → Script Manager**.
2. Click the **Manage Script Directories** button (top-right, looks like a
   bulleted-list icon) and **add** the path to this folder
   (`.../dungeonkeeper2/tools/ghidra-sync`). *(Alternatively copy/symlink
   `ImportSgmapSymbols.java` into your `~/ghidra_scripts` directory.)*
3. Back in the Script Manager, find **`ImportSgmapSymbols`** (category `DK2`;
   use the filter box). Select it and click **Run** (green arrow).
4. If the script can't auto-find `mapping/DKII_EXE_v170.sgmap` via a relative
   path, it prompts you with a file chooser — point it at
   `mapping/DKII_EXE_v170.sgmap` in this repo.
5. Read the summary printed in the **Console – Scripting** window: functions
   renamed, labels created, already-correct, skipped-as-conflict,
   not-in-program, and failed (with the first several examples of each).

If anything looks wrong, **Edit → Undo** reverts the entire import in one step.

## `MaterializeSgmapFunctions.java` — turn vtable-only names into real functions

`ImportSgmapSymbols` applies the *name* to every `.sgmap` address, but a
meaningful number of those addresses never become real Ghidra **`Function`**
objects. They are reached **only** through indirect (C++ vtable) calls, which
Ghidra's auto-analyzer never traced as code, so it left the bytes as
raw/defined **data** carrying just a label. The name is correct, but
`get_function_by_address` / `decompile_function` on such an address fails with
**"No function found"** until a function is explicitly created there.

Confirmed real examples:

```
CEngineStaticHeightField::appendToSceneObject2EList @ 0x00587060
CDefaultPlayerInterface_onMouseAction              @ 0x00406530
```

Both had the right name but no function body until `createFunction` was called.

`MaterializeSgmapFunctions.java` fixes this in bulk. It re-reads the same
`.sgmap` and, for every entry that

- is **not already a function** (`getFunctionAt` returns null),
- sits in an **executable** memory block (`.text` / `cseg`), and
- **already carries a non-default symbol** (proof that a prior
  `ImportSgmapSymbols` run — or a human — named it, i.e. it is a function, not
  a data global),

calls `createFunction(addr, null)` to disassemble and materialize the function,
**preserving the existing name**. If defined data is blocking disassembly it
clears that unit and retries once. Data globals (which live in non-executable
blocks, or carry no imported name) are never touched. Every failure is caught
and logged, never fatal, and the whole pass runs in one undoable transaction.

The final summary reports: candidates examined, materialized, already-a-function
(skipped), not-a-candidate, not-in-program, failed (with the first ~20
addresses + reasons), and total runtime.

### When / how to run it

Run it **right after `ImportSgmapSymbols`** — or any time later, whenever new
names get imported. Same Script Manager flow: find **`MaterializeSgmapFunctions`**
(category `DK2`) next to `ImportSgmapSymbols`, select it, click **Run**, and
point it at `mapping/DKII_EXE_v170.sgmap` if it prompts.

**Idempotent / safe to re-run.** Materializing an already-materialized function
is a no-op (counted as "already a function"). **Edit → Undo** reverts the whole
pass in one step.

## Note

The script targets stable GhidraScript API (`getFunctionAt`, `toAddr`,
`createLabel`, `SymbolTable`, `SourceType`). It could not be compiled in this
repo because there is no Ghidra install here to provide the API jars; it
compiles when placed in a Ghidra script directory (Ghidra compiles scripts
on load).
