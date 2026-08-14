import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.util.task.ConsoleTaskMonitor;

public class Decomp extends GhidraScript {
    public void run() throws Exception {
        String[] want = getScriptArgs();
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        for (String name : want) {
            for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                if (!f.getName().equals(name)) continue;
                println("=== " + name + " @ " + f.getEntryPoint());
                DecompileResults r = di.decompileFunction(f, 120, new ConsoleTaskMonitor());
                if (r.decompileCompleted())
                    println(r.getDecompiledFunction().getC());
                else
                    println("decompile failed: " + r.getErrorMessage());
            }
        }
    }
}
