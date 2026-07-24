// MaterializeSgmapFunctions.java
//
// Companion to ImportSgmapSymbols.java. After the names from
// mapping/DKII_EXE_v170.sgmap have been imported, a meaningful number of the
// named addresses are NOT real Ghidra Function objects: they carry the correct
// name only as a data-typed label, because they are reached ONLY via indirect
// (C++ vtable) calls that Ghidra's auto-analyzer never traced as code, so the
// bytes were left as raw/defined data instead of being disassembled into a
// function body.
//
// Symptom: get_function_by_address / decompile_function on such an address
// fails with "No function found", even though the address clearly IS a
// function and already has the right name. Two confirmed real examples from a
// live session:
//   CEngineStaticHeightField::appendToSceneObject2EList @ 0x00587060
//   CDefaultPlayerInterface_onMouseAction              @ 0x00406530
// Both had the correct name but no Function until create_function was called.
//
// This script walks the same .sgmap entries ImportSgmapSymbols.java does and,
// for every entry that (a) already carries a non-default (previously-imported
// or human) symbol and (b) sits in an EXECUTABLE memory block and (c) is not
// already a Function, calls createFunction() to materialize it. The existing
// label name is preserved. Data globals (which live in non-executable blocks)
// are never touched.
//
// Run this INTERACTIVELY from the running GUI via the Script Manager, right
// after ImportSgmapSymbols (or any time later, after re-importing new names).
// It operates on `currentProgram` through Ghidra's normal transaction/undo
// system. It does NOT touch project files on disk and does NOT require a
// headless run. See tools/ghidra-sync/README.md for exact steps.
//
// Safe to re-run (idempotent): materializing an already-materialized function
// is a no-op (counted as "already a function").
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
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolTable;

public class MaterializeSgmapFunctions extends GhidraScript {

	// -------------------------------------------------------------------------
	// Configuration
	// -------------------------------------------------------------------------

	// Build C++-style qualified names (Class::method) for entries that carry a
	// `member_of` struct reference, mirroring ImportSgmapSymbols so we can
	// recognise the name a prior import applied. Only used for logging here;
	// materialization keeps whatever symbol already exists at the address.
	private static final boolean QUALIFY_WITH_CLASS = true;

	// How many failure examples to echo in the final summary.
	private static final int MAX_EXAMPLES = 20;

	// -------------------------------------------------------------------------
	// Counters
	// -------------------------------------------------------------------------

	private int candidatesExamined = 0;  // executable + already-named, i.e. plausible functions
	private int materialized = 0;        // createFunction succeeded
	private int alreadyFunction = 0;     // getFunctionAt != null, nothing to do
	private int failed = 0;              // createFunction failed / threw
	private int notCandidate = 0;        // data global / no prior name / not executable
	private int noAddress = 0;           // address not present in this program

	private final List<String> failureExamples = new ArrayList<>();

	// -------------------------------------------------------------------------

	@Override
	public void run() throws Exception {
		long startNanos = System.nanoTime();

		if (currentProgram == null) {
			println("[sgmap-mat] No program is open. Open the DKII program first.");
			return;
		}

		File sgmapFile = resolveSgmapFile();
		if (sgmapFile == null) {
			println("[sgmap-mat] No .sgmap file selected. Aborting.");
			return;
		}
		println("[sgmap-mat] Reading: " + sgmapFile.getAbsolutePath());

		Map<String, String> structNameById = new HashMap<>();
		List<GlobalEntry> globals = new ArrayList<>();
		parse(sgmapFile, structNameById, globals);
		println("[sgmap-mat] Parsed " + structNameById.size() + " structs, "
				+ globals.size() + " global entries.");

		// Script Manager already runs this inside a Ghidra transaction, so the
		// whole materialization pass is a single undoable operation.
		SymbolTable symbolTable = currentProgram.getSymbolTable();
		monitor.initialize(globals.size());
		int processed = 0;
		for (GlobalEntry g : globals) {
			if (monitor.isCancelled()) {
				println("[sgmap-mat] Cancelled by user after " + processed + " entries.");
				break;
			}
			monitor.incrementProgress(1);
			processed++;
			process(g, structNameById, symbolTable);
		}

		double seconds = (System.nanoTime() - startNanos) / 1_000_000_000.0;
		printSummary(seconds);
	}

	// -------------------------------------------------------------------------
	// Resolve the .sgmap path (default guess, else prompt)
	// -------------------------------------------------------------------------

	private File resolveSgmapFile() {
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
		try {
			return askFile("Select DKII_EXE_v170.sgmap", "Materialize");
		}
		catch (Exception e) {
			return null;
		}
	}

	// -------------------------------------------------------------------------
	// Parsing (mirrors ImportSgmapSymbols.java exactly)
	// -------------------------------------------------------------------------

	private void parse(File file, Map<String, String> structNameById,
			List<GlobalEntry> globals) throws Exception {
		try (BufferedReader reader = new BufferedReader(new FileReader(file))) {
			String line;
			while ((line = reader.readLine()) != null) {
				if (line.isEmpty() || line.charAt(0) == '#') {
					continue; // comment / blank
				}
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
	// Process one entry
	// -------------------------------------------------------------------------

	private void process(GlobalEntry g, Map<String, String> structNameById,
			SymbolTable symbolTable) {
		Address addr;
		try {
			addr = toAddr(Long.parseLong(g.va, 16));
		}
		catch (Exception e) {
			return; // bad va; ImportSgmapSymbols already reports these
		}
		if (addr == null || !currentProgram.getMemory().contains(addr)) {
			noAddress++;
			return;
		}

		// Already a real function? Nothing to do (idempotent path).
		Function func = getFunctionAt(addr);
		if (func != null) {
			alreadyFunction++;
			return;
		}

		// Only consider addresses that look like a function START:
		//  1. they sit in an EXECUTABLE memory block (.text / cseg), and
		//  2. they already carry a non-default symbol (a prior successful
		//     ImportSgmapSymbols run, or a human, named this address) -- this
		//     is our proxy for "this .sgmap entry is a function, not data".
		// Everything else (data globals in non-exec blocks, un-named addresses)
		// is deliberately left alone.
		MemoryBlock block = currentProgram.getMemory().getBlock(addr);
		if (block == null || !block.isExecute()) {
			notCandidate++;
			return;
		}
		Symbol primary = symbolTable.getPrimarySymbol(addr);
		if (primary == null || primary.getSource() == SourceType.DEFAULT) {
			notCandidate++;
			return;
		}

		candidatesExamined++;

		String label = buildName(g, structNameById);
		try {
			// Passing null name preserves the existing (already-correct) symbol
			// as the function's name. createFunction disassembles the entry
			// point as needed; if defined data is blocking it, clear it first
			// and retry once.
			Function created = createFunction(addr, null);
			if (created == null) {
				Data data = getDataAt(addr);
				if (data != null) {
					clearListing(addr);
				}
				disassemble(addr);
				created = createFunction(addr, null);
			}
			if (created != null) {
				materialized++;
			}
			else {
				recordFailure(addr + "  " + label + "  (createFunction returned null)");
			}
		}
		catch (Exception e) {
			recordFailure(addr + "  " + label + "  (" + e.getClass().getSimpleName()
					+ ": " + e.getMessage() + ")");
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

	// -------------------------------------------------------------------------
	// Reporting helpers
	// -------------------------------------------------------------------------

	private void recordFailure(String msg) {
		failed++;
		if (failureExamples.size() < MAX_EXAMPLES) {
			failureExamples.add(msg);
		}
	}

	private void printSummary(double seconds) {
		println("");
		println("========== sgmap materialize summary ==========");
		println("  candidates examined     : " + candidatesExamined);
		println("  materialized (created)  : " + materialized);
		println("  already a function      : " + alreadyFunction);
		println("  not a candidate         : " + notCandidate
				+ " (data / unnamed / non-executable)");
		println("  not in program          : " + noAddress);
		println("  failed to materialize   : " + failed);
		println(String.format("  runtime                 : %.2f s", seconds));
		if (!failureExamples.isEmpty()) {
			println("  --- first failures (addr  name  reason) ---");
			for (String s : failureExamples) {
				println("    " + s);
			}
		}
		println("===============================================");
	}

	// -------------------------------------------------------------------------

	private static class GlobalEntry {
		String va;
		String name;
		String memberOf;
	}
}
