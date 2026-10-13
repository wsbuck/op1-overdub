// Decompile the functions at the given addresses (creating them if needed) and print C to stdout.
// Usage: analyzeHeadless <proj_dir> <proj> -process <prog> -noanalysis -readOnly \
//          -scriptPath tools/ghidra_scripts -postScript Decompile.java 0x1068 0x1388 ...
// @category OP1
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

public class Decompile extends GhidraScript {
    @Override
    protected void run() throws Exception {
        DecompInterface ifc = new DecompInterface();
        ifc.openProgram(currentProgram);
        for (String arg : getScriptArgs()) {
            Address addr = toAddr(arg);
            Function f = getFunctionContaining(addr);
            if (f == null) {
                disassemble(addr);
                new CreateFunctionCmd(addr).applyTo(currentProgram);
                f = getFunctionAt(addr);
            }
            println("==== " + arg + (f == null ? " (no function)" : " in " + f.getName() + " @ " + f.getEntryPoint()));
            if (f == null) continue;
            DecompileResults r = ifc.decompileFunction(f, 120, monitor);
            System.out.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "decompile failed: " + r.getErrorMessage());
        }
    }
}
