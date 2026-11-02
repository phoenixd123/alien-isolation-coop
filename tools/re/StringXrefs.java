// For each string given in the script args (or in strings.txt next to the output), find defined/raw
// occurrences in memory, every reference to them, and decompile the referencing functions.
// Usage (headless): -postScript StringXrefs.java <outdir> <string1> <string2> ...
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.*;

public class StringXrefs extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        File out = new File(a[0]); out.mkdirs();
        DecompInterface di = new DecompInterface(); di.openProgram(currentProgram);
        Memory mem = currentProgram.getMemory();
        ReferenceManager rm = currentProgram.getReferenceManager();
        FunctionManager fm = currentProgram.getFunctionManager();
        long base = currentProgram.getImageBase().getOffset();
        PrintWriter idx = new PrintWriter(new FileWriter(new File(out, "index.txt"), true));
        for (int i = 1; i < a.length; i++) {
            byte[] pat = (a[i] + "\0").getBytes(StandardCharsets.US_ASCII);
            Address at = mem.getMinAddress();
            Set<Function> funcs = new LinkedHashSet<>();
            while (at != null) {
                at = mem.findBytes(at, pat, null, true, monitor);
                if (at == null) break;
                idx.printf("STRING %s @ %s%n", a[i], at);
                for (Reference r : rm.getReferencesTo(at)) {
                    Function f = fm.getFunctionContaining(r.getFromAddress());
                    idx.printf("  ref from %s (rva %x) in %s%n", r.getFromAddress(), r.getFromAddress().getOffset() - base,
                               f == null ? "?" : f.getName() + " rva " + Long.toHexString(f.getEntryPoint().getOffset() - base));
                    if (f != null) funcs.add(f);
                }
                // pointer tables: look for 4-byte little-endian pointers to the string
                byte[] ptr = new byte[]{(byte) at.getOffset(), (byte) (at.getOffset() >> 8), (byte) (at.getOffset() >> 16), (byte) (at.getOffset() >> 24)};
                Address p = mem.getMinAddress();
                int n = 0;
                while (p != null && n < 20) {
                    p = mem.findBytes(p, ptr, null, true, monitor);
                    if (p == null) break;
                    idx.printf("  pointer @ %s (rva %x)%n", p, p.getOffset() - base);
                    for (Reference r : rm.getReferencesTo(p)) {
                        Function f = fm.getFunctionContaining(r.getFromAddress());
                        idx.printf("    ptr ref from %s in %s%n", r.getFromAddress(), f == null ? "?" : f.getName());
                        if (f != null) funcs.add(f);
                    }
                    p = p.add(1); n++;
                }
                at = at.add(1);
            }
            for (Function f : funcs) {
                DecompileResults res = di.decompileFunction(f, 120, monitor);
                String name = a[i].replaceAll("[^A-Za-z0-9_]", "_") + "__" + f.getName() + ".c";
                try (PrintWriter w = new PrintWriter(new FileWriter(new File(out, name)))) {
                    w.printf("// %s rva %x, refs string \"%s\"%n", f.getName(), f.getEntryPoint().getOffset() - base, a[i]);
                    w.println(res.decompileCompleted() ? res.getDecompiledFunction().getC() : "// decompile failed: " + res.getErrorMessage());
                }
            }
            idx.flush();
        }
        idx.close();
    }
}
