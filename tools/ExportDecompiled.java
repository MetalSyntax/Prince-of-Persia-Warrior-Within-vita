// Ghidra headless post-script: dumps pseudo-C of every function to the path given as script arg.
// Usage: analyzeHeadless <proj_dir> <proj> -import POP -postScript ExportDecompiled.java <out.c>
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import java.io.PrintWriter;

public class ExportDecompiled extends GhidraScript {
	@Override
	public void run() throws Exception {
		String out = getScriptArgs().length > 0 ? getScriptArgs()[0] : "POP.c";
		DecompInterface ifc = new DecompInterface();
		ifc.openProgram(currentProgram);
		int ok = 0, fail = 0;
		try (PrintWriter w = new PrintWriter(out, "UTF-8")) {
			for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
				if (monitor.isCancelled()) break;
				w.printf("// ==== %s @ %s ====%n", f.getName(), f.getEntryPoint());
				DecompileResults r = ifc.decompileFunction(f, 60, monitor);
				if (r != null && r.decompileCompleted()) {
					w.println(r.getDecompiledFunction().getC());
					ok++;
				} else {
					w.println("// decompile failed: " + (r != null ? r.getErrorMessage() : "null"));
					fail++;
				}
			}
		}
		ifc.dispose();
		println("Exported " + ok + " functions, " + fail + " failed -> " + out);
	}
}
