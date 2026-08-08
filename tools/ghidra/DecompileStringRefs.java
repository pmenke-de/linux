// Decompile functions that reference matching strings.
// Usage:
//   -postScript DecompileStringRefs.java pattern1 pattern2 ...

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.util.task.ConsoleTaskMonitor;

public class DecompileStringRefs extends GhidraScript {

	private static class RefInfo {
		Address stringAddr;
		String text;
		Address fromAddr;

		RefInfo(Address stringAddr, String text, Address fromAddr) {
			this.stringAddr = stringAddr;
			this.text = text;
			this.fromAddr = fromAddr;
		}
	}

	@Override
	protected void run() throws Exception {
		String[] patterns = getScriptArgs();
		if (patterns == null || patterns.length == 0) {
			println("No patterns provided");
			return;
		}

		Map<Function, List<RefInfo>> funcs = new LinkedHashMap<>();
		int stringMatches = 0;

		DataIterator it = currentProgram.getListing().getDefinedData(true);
		while (it.hasNext()) {
			Data data = it.next();
			Object value = data.getValue();
			if (value == null) {
				continue;
			}

			String text = value.toString();
			if (text == null || text.isEmpty()) {
				continue;
			}

			boolean matched = false;
			for (String pat : patterns) {
				if (text.contains(pat)) {
					matched = true;
					break;
				}
			}
			if (!matched) {
				continue;
			}

			stringMatches++;
			Reference[] refs = getReferencesTo(data.getAddress());
			for (Reference ref : refs) {
				Function func = getFunctionContaining(ref.getFromAddress());
				if (func == null) {
					continue;
				}
				funcs.computeIfAbsent(func, k -> new ArrayList<>())
					.add(new RefInfo(data.getAddress(), text, ref.getFromAddress()));
			}
		}

		println("Matched " + stringMatches + " strings");
		println("Matched " + funcs.size() + " functions");

		DecompInterface ifc = new DecompInterface();
		ifc.openProgram(currentProgram);
		ConsoleTaskMonitor taskMonitor = new ConsoleTaskMonitor();

		for (Map.Entry<Function, List<RefInfo>> entry : funcs.entrySet()) {
			Function func = entry.getKey();
			println("=== FUNCTION " + func.getName() + " " + func.getEntryPoint() + " ===");
			for (RefInfo ref : entry.getValue()) {
				println("REF " + ref.fromAddr + " -> " + ref.stringAddr);
				println("STR " + ref.text);
			}

			DecompileResults res = ifc.decompileFunction(func, 60, taskMonitor);
			if (!res.decompileCompleted()) {
				println("DECOMPILATION FAILED: " + res.getErrorMessage());
				continue;
			}
			println(res.getDecompiledFunction().getC());
			println("");
		}

		ifc.dispose();
	}
}
