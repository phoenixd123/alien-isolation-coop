// List references to data/code addresses (by RVA) and decompile each referencing function.
// Usage: -postScript RefsTo.java <outdir> <rva-hex> ...
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.util.*;

public class RefsTo extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        File out = new File(a[0]); out.mkdirs();
        long base = currentProgram.getImageBase().getOffset();
        DecompInterface di = new DecompInterface(); di.openProgram(currentProgram);
        FunctionManager fm = currentProgram.getFunctionManager();
        for (int i = 1; i < a.length; i++) {
            Address ad = toAddr(base + Long.parseLong(a[i], 16));
            Set<Function> fs = new LinkedHashSet<>();
            try (PrintWriter w = new PrintWriter(new FileWriter(new File(out, "refs_" + a[i] + ".txt")))) {
                for (Reference r : getReferencesTo(ad)) {
                    Function f = fm.getFunctionContaining(r.getFromAddress());
                    w.printf("%s %s in %s%n", r.getReferenceType(), r.getFromAddress(),
                        f == null ? "?" : f.getName() + " rva " + Long.toHexString(f.getEntryPoint().getOffset() - base));
                    if (f != null) fs.add(f);
                }
            }
            int n = 0;
            for (Function f : fs) {
                if (n++ >= 12) break;
                DecompileResults r = di.decompileFunction(f, 120, monitor);
                try (PrintWriter w = new PrintWriter(new FileWriter(new File(out, "r_" + a[i] + "_" + Long.toHexString(f.getEntryPoint().getOffset() - base) + ".c")))) {
                    w.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// failed");
                }
            }
        }
    }
}
