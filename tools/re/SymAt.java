// Print the symbol (or nearest preceding label) for each RVA listed in a file. Usage: -postScript SymAt.java <in> <out>
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.nio.file.*;

public class SymAt extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        long base = currentProgram.getImageBase().getOffset();
        try (PrintWriter w = new PrintWriter(new FileWriter(a[1]))) {
            for (String line : Files.readAllLines(Paths.get(a[0]))) {
                line = line.trim(); if (line.isEmpty()) continue;
                long rva = Long.parseLong(line, 16);
                Address ad = toAddr(base + rva);
                Symbol s = getSymbolAt(ad);
                String name;
                if (s != null) name = s.getName(true);
                else {
                    Symbol p = currentProgram.getSymbolTable().getPrimarySymbol(ad);
                    Address q = ad; int back = 0;
                    while (p == null && back < 0x400) { q = q.subtract(4); back += 4; p = currentProgram.getSymbolTable().getPrimarySymbol(q); }
                    name = p == null ? "?" : p.getName(true) + "+0x" + Integer.toHexString(back);
                }
                w.printf("%s %s%n", line, name);
            }
        }
    }
}
