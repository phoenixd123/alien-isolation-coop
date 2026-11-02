// Decompile functions by RVA. Usage: -postScript Decomp.java <outdir> <rva-hex> [<rva-hex> ...]
// Also writes callers of each function to callers_<rva>.txt.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.*;

public class Decomp extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        File out = new File(a[0]); out.mkdirs();
        DecompInterface di = new DecompInterface(); di.openProgram(currentProgram);
        long base = currentProgram.getImageBase().getOffset();
        FunctionManager fm = currentProgram.getFunctionManager();
        for (int i = 1; i < a.length; i++) {
            long rva = Long.parseLong(a[i], 16);
            Address ad = toAddr(base + rva);
            Function f = fm.getFunctionContaining(ad);
            if (f == null) { f = createFunction(ad, null); }
            if (f == null) { println("no function at " + a[i]); continue; }
            DecompileResults r = di.decompileFunction(f, 180, monitor);
            try (PrintWriter w = new PrintWriter(new FileWriter(new File(out, "f_" + a[i] + ".c")))) {
                w.printf("// %s rva %x%n", f.getName(), f.getEntryPoint().getOffset() - base);
                w.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// failed: " + r.getErrorMessage());
            }
            try (PrintWriter w = new PrintWriter(new FileWriter(new File(out, "callers_" + a[i] + ".txt")))) {
                for (Reference ref : getReferencesTo(f.getEntryPoint())) {
                    Function c = fm.getFunctionContaining(ref.getFromAddress());
                    w.printf("%s from %s in %s (rva %s)%n", ref.getReferenceType(), ref.getFromAddress(),
                        c == null ? "?" : c.getName(), c == null ? "?" : Long.toHexString(c.getEntryPoint().getOffset() - base));
                }
            }
        }
    }
}
