// ImportSgmapSymbols.java
//
// One-way importer: pushes the function/global names recovered in the
// hand-curated symbol map (mapping/DKII_EXE_v170.sgmap) INTO the program that
// is currently open in the Ghidra GUI, so the disassembly/decompiler show
// meaningful names (e.g. CEngineStaticHeightField::appendToSceneObject2EList)
// instead of FUN_00587060 / DAT_xxxxxxxx.
//
// Run this INTERACTIVELY from the already-running GUI via the Script Manager.
// It operates on `currentProgram` through Ghidra's normal transaction/undo
// system. It does NOT touch project files on disk and does NOT require a
// headless run. See tools/ghidra-sync/README.md for exact steps.
//
// Safe to re-run (idempotent): re-running only re-confirms names it already
// applied; it never duplicates or corrupts symbols.
//
//@category DK2

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolTable;

public class ImportSgmapSymbols extends GhidraScript {

	// -------------------------------------------------------------------------
	// Configuration
	// -------------------------------------------------------------------------

	// When false (the SAFE default) we only overwrite Ghidra's own
	// auto-generated placeholder names (FUN_/LAB_/DAT_/SUB_ ... i.e. symbols
	// whose SourceType is DEFAULT, plus addresses with no symbol at all).
	// Any name that a human already typed, or that Ghidra's analysis recovered
	// from RTTI/debug info, is left untouched and logged as "conflict, skipped".
	//
	// Flip to true only once you trust the .sgmap as authoritative and want to
	// force-overwrite everything it names.
	private static final boolean OVERWRITE_EXISTING = false;

	// Build C++-style qualified names (Class::method) for entries that carry a
	// `member_of` struct reference. This is what makes duplicate simple names
	// like "constructor" / "appendToSceneObject2EList" unique and readable.
	private static final boolean QUALIFY_WITH_CLASS = true;

	// How many error/conflict examples to echo in the final summary.
	private static final int MAX_EXAMPLES = 15;

	// -------------------------------------------------------------------------
	// Counters
	// -------------------------------------------------------------------------

	private int functionsRenamed = 0;
	private int labelsCreated = 0;
	private int alreadyCorrect = 0;   // name already matches -> nothing to do
	private int skippedConflict = 0;  // existing non-default name, not overwritten
	private int failed = 0;
	private int noAddress = 0;        // address not present in this program

	private final List<String> conflictExamples = new ArrayList<>();
	private final List<String> errorExamples = new ArrayList<>();

	// -------------------------------------------------------------------------

	@Override
	public void run() throws Exception {
		if (currentProgram == null) {
			println("[sgmap] No program is open. Open the DKII program first.");
			return;
		}

		File sgmapFile = resolveSgmapFile();
		if (sgmapFile == null) {
			println("[sgmap] No .sgmap file selected. Aborting.");
			return;
		}
		println("[sgmap] Reading: " + sgmapFile.getAbsolutePath());

		// Pass 1: parse the file into a struct-id -> class-name map and a flat
		// list of global entries. We read the whole file first so struct
		// definitions are all known before we resolve member_of references,
		// regardless of ordering.
		Map<String, String> structNameById = new HashMap<>();
		List<GlobalEntry> globals = new ArrayList<>();
		parse(sgmapFile, structNameById, globals);
		println("[sgmap] Parsed " + structNameById.size() + " structs, "
				+ globals.size() + " global entries.");

		// Pass 2: apply. Script Manager already runs this inside a Ghidra
		// transaction, so the whole import is a single undoable operation.
		SymbolTable symbolTable = currentProgram.getSymbolTable();
		monitor.initialize(globals.size());
		int processed = 0;
		for (GlobalEntry g : globals) {
			if (monitor.isCancelled()) {
				println("[sgmap] Cancelled by user after " + processed + " entries.");
				break;
			}
			monitor.incrementProgress(1);
			processed++;
			apply(g, structNameById, symbolTable);
		}

		printSummary();
	}

	// -------------------------------------------------------------------------
	// Resolve the .sgmap path (default guess, else prompt)
	// -------------------------------------------------------------------------

	private File resolveSgmapFile() {
		// Try a few relative guesses first (working dir is wherever Ghidra was
		// launched from, so these may or may not hit).
		String[] guesses = new String[] {
			"mapping/DKII_EXE_v170.sgmap",
			"../mapping/DKII_EXE_v170.sgmap",
			"../../mapping/DKII_EXE_v170.sgmap",
		};
		for (String guess : guesses) {
			File f = new File(guess);
			if (f.isFile()) {
				return f.getAbsoluteFile();
			}
		}
		// Fall back to prompting the human for the file.
		try {
			return askFile("Select DKII_EXE_v170.sgmap", "Import");
		}
		catch (Exception e) {
			// CancelledException or similar.
			return null;
		}
	}

	// -------------------------------------------------------------------------
	// Parsing
	// -------------------------------------------------------------------------

	private void parse(File file, Map<String, String> structNameById,
			List<GlobalEntry> globals) throws Exception {
		try (BufferedReader reader = new BufferedReader(new FileReader(file))) {
			String line;
			while ((line = reader.readLine()) != null) {
				if (line.isEmpty() || line.charAt(0) == '#') {
					continue; // comment / blank
				}
				// Only top-level (col 0) struct:/global: headers matter; every
				// nested detail line (fields, types) is indented with spaces
				// and is ignored here.
				if (line.startsWith("struct: ")) {
					Map<String, String> props = parseProps(line.substring("struct: ".length()));
					String id = props.get("id");
					String name = props.get("name");
					if (id != null && name != null) {
						structNameById.put(id, name);
					}
				}
				else if (line.startsWith("global: ")) {
					Map<String, String> props = parseProps(line.substring("global: ".length()));
					String va = props.get("va");
					String name = props.get("name");
					if (va == null || name == null) {
						continue;
					}
					GlobalEntry g = new GlobalEntry();
					g.va = va;
					g.name = name;
					g.memberOf = props.get("member_of"); // may be null
					globals.add(g);
				}
				// anything else (indented detail, unknown headers) -> skip
			}
		}
	}

	// Parse a comma-separated list of key=value pairs. Names, ids, paths and
	// hex/decimal values in this format never contain commas or '=', so a plain
	// split is safe (this mirrors the Python parser in tools/genapi/sgmap.py,
	// _parse_short).
	private Map<String, String> parseProps(String s) {
		Map<String, String> map = new HashMap<>();
		String[] parts = s.split(",");
		for (String part : parts) {
			int eq = part.indexOf('=');
			if (eq <= 0) {
				continue;
			}
			String key = part.substring(0, eq);
			String value = part.substring(eq + 1);
			map.put(key, value);
		}
		return map;
	}

	// -------------------------------------------------------------------------
	// Apply one entry
	// -------------------------------------------------------------------------

	private void apply(GlobalEntry g, Map<String, String> structNameById,
			SymbolTable symbolTable) {
		Address addr;
		try {
			// va is an 8-hex-digit image virtual address; for a PE loaded at
			// its native image base this equals the Ghidra address.
			addr = toAddr(Long.parseLong(g.va, 16));
		}
		catch (Exception e) {
			recordError("bad va " + g.va + " for " + g.name);
			return;
		}
		if (addr == null || !currentProgram.getMemory().contains(addr)) {
			noAddress++;
			return;
		}

		String targetName = buildName(g, structNameById);
		if (targetName == null || targetName.isEmpty()) {
			return;
		}

		Function func = getFunctionAt(addr);
		if (func != null) {
			applyToFunction(func, targetName);
		}
		else {
			applyAsLabel(addr, targetName, symbolTable);
		}
	}

	private String buildName(GlobalEntry g, Map<String, String> structNameById) {
		String name = g.name;
		if (QUALIFY_WITH_CLASS && g.memberOf != null && !name.contains("::")) {
			String className = structNameById.get(g.memberOf);
			if (className != null && !className.isEmpty()) {
				return className + "::" + name;
			}
		}
		return name;
	}

	private void applyToFunction(Function func, String targetName) {
		Symbol sym = func.getSymbol();
		String current = func.getName();
		if (current.equals(targetName)) {
			alreadyCorrect++;
			return;
		}
		boolean isPlaceholder = (sym == null) || (sym.getSource() == SourceType.DEFAULT);
		if (!isPlaceholder && !OVERWRITE_EXISTING) {
			skippedConflict++;
			addConflict("function @ " + func.getEntryPoint() + " keeps '" + current
					+ "', .sgmap wanted '" + targetName + "'");
			return;
		}
		try {
			func.setName(targetName, SourceType.USER_DEFINED);
			functionsRenamed++;
		}
		catch (Exception e) {
			recordError("function @ " + func.getEntryPoint() + " -> '" + targetName
					+ "': " + e.getClass().getSimpleName() + " " + e.getMessage());
		}
	}

	private void applyAsLabel(Address addr, String targetName, SymbolTable symbolTable) {
		Symbol primary = symbolTable.getPrimarySymbol(addr);
		if (primary != null) {
			if (primary.getName().equals(targetName)) {
				alreadyCorrect++;
				return;
			}
			boolean isPlaceholder = (primary.getSource() == SourceType.DEFAULT);
			if (!isPlaceholder && !OVERWRITE_EXISTING) {
				skippedConflict++;
				addConflict("data @ " + addr + " keeps '" + primary.getName()
						+ "', .sgmap wanted '" + targetName + "'");
				return;
			}
		}
		try {
			// createLabel adds a USER_DEFINED symbol and makes it primary; a
			// pre-existing DEFAULT dynamic (DAT_) symbol is superseded, not
			// duplicated. Re-running with the same name is a no-op (caught by
			// the alreadyCorrect check above).
			createLabel(addr, targetName, true, SourceType.USER_DEFINED);
			labelsCreated++;
		}
		catch (Exception e) {
			recordError("label @ " + addr + " -> '" + targetName + "': "
					+ e.getClass().getSimpleName() + " " + e.getMessage());
		}
	}

	// -------------------------------------------------------------------------
	// Reporting helpers
	// -------------------------------------------------------------------------

	private void addConflict(String msg) {
		if (conflictExamples.size() < MAX_EXAMPLES) {
			conflictExamples.add(msg);
		}
	}

	private void recordError(String msg) {
		failed++;
		if (errorExamples.size() < MAX_EXAMPLES) {
			errorExamples.add(msg);
		}
	}

	private void printSummary() {
		println("");
		println("========== sgmap import summary ==========");
		println("  functions renamed : " + functionsRenamed);
		println("  labels created    : " + labelsCreated);
		println("  already correct   : " + alreadyCorrect);
		println("  skipped (conflict): " + skippedConflict
				+ (OVERWRITE_EXISTING ? " [OVERWRITE_EXISTING=true]" : ""));
		println("  not in program    : " + noAddress);
		println("  failed/errored    : " + failed);
		println("  overwrite mode    : " + OVERWRITE_EXISTING);
		if (!conflictExamples.isEmpty()) {
			println("  --- first conflicts (existing names preserved) ---");
			for (String s : conflictExamples) {
				println("    " + s);
			}
		}
		if (!errorExamples.isEmpty()) {
			println("  --- first errors ---");
			for (String s : errorExamples) {
				println("    " + s);
			}
		}
		println("==========================================");
	}

	// -------------------------------------------------------------------------

	private static class GlobalEntry {
		String va;
		String name;
		String memberOf;
	}
}
