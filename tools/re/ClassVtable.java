// Dump and decompile vftable entries of CATHODE classes.
// Usage: -postScript ClassVtable.java <outdir> <maxSlots> <Class1> <Class2> ...   (class names inside CATHODE::)
// Writes <Class>.txt (slot -> function, shared with base marked) and <Class>_slotNN.c for slots unique to that class.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.util.*;

public class ClassVtable extends GhidraScript {
    Map<Long, Integer> useCount = new HashMap<>();

    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        File out = new File(a[0]); out.mkdirs();
        int max = Integer.parseInt(a[1]);
        long base = currentProgram.getImageBase().getOffset();
        DecompInterface di = new DecompInterface(); di.openProgram(currentProgram);
        SymbolTable st = currentProgram.getSymbolTable();
        FunctionManager fm = currentProgram.getFunctionManager();
        // Count how many CATHODE vftables use each function (to spot base-class defaults).
        List<Symbol> all = new ArrayList<>();
        for (Symbol s : st.getSymbols("vftable")) all.add(s);
        for (Symbol s : all) {
            Address v = s.getAddress();
            for (int i = 0; i < max; i++) {
                long p = getInt(v.add(4L * i)) & 0xffffffffL;
                useCount.merge(p, 1, Integer::sum);
            }
        }
        for (int c = 2; c < a.length; c++) {
            Symbol vs = null;
            for (Symbol s : all) if (s.getParentNamespace().getName(true).equals("CATHODE::" + a[c])) { vs = s; break; }
            try (PrintWriter w = new PrintWriter(new FileWriter(new File(out, a[c] + ".txt")))) {
                if (vs == null) { w.println("no vftable for CATHODE::" + a[c]); continue; }
                w.printf("CATHODE::%s vftable @ %s (rva %x)%n", a[c], vs.getAddress(), vs.getAddress().getOffset() - base);
                for (int i = 0; i < max; i++) {
                    Address slot = vs.getAddress().add(4L * i);
                    long p = getInt(slot) & 0xffffffffL;
                    Function f = fm.getFunctionAt(toAddr(p));
                    if (f == null && getSymbolAt(slot) != null && i > 0 && getSymbolAt(slot).getName().contains("vftable")) break;
                    int uses = useCount.getOrDefault(p, 0);
                    w.printf("slot %3d (+0x%03x): %08x %s uses=%d%n", i, i * 4, p, f == null ? "(not a function)" : f.getName(), uses);
                    if (f != null && uses <= 2) {
                        DecompileResults r = di.decompileFunction(f, 120, monitor);
                        try (PrintWriter fw = new PrintWriter(new FileWriter(new File(out, String.format("%s_slot%03d.c", a[c], i))))) {
                            fw.printf("// CATHODE::%s slot %d (+0x%x) = %s rva %x%n", a[c], i, i * 4, f.getName(), p - base);
                            fw.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// failed");
                        }
                    }
                }
            }
        }
    }
}
