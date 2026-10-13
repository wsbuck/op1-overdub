// Create functions at every address listed (one hex address per line) in the file given as the script arg.
// @category OP1
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import java.nio.file.Files;
import java.nio.file.Paths;

public class SeedFunctions extends GhidraScript {
    @Override
    protected void run() throws Exception {
        int made = 0;
        for (String line : Files.readAllLines(Paths.get(getScriptArgs()[0]))) {
            line = line.trim();
            if (line.isEmpty()) continue;
            Address a = toAddr(line);
            if (getFunctionAt(a) != null) continue;
            disassemble(a);
            if (new CreateFunctionCmd(a).applyTo(currentProgram)) made++;
        }
        println("SeedFunctions: created " + made);
    }
}
