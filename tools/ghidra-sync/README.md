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

## Note

The script targets stable GhidraScript API (`getFunctionAt`, `toAddr`,
`createLabel`, `SymbolTable`, `SourceType`). It could not be compiled in this
repo because there is no Ghidra install here to provide the API jars; it
compiles when placed in a Ghidra script directory (Ghidra compiles scripts
on load).
