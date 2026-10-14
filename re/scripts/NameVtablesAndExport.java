// Headless Ghidra post-script:
//  1. For every RTTI-recovered "vftable" symbol, name unnamed slot targets
//     <Class>::vf<N> (first class wins; shared thunks keep the first name).
//  2. Decompile every function and write one big C file + an index TSV.
// Usage: analyzeHeadless <proj> <name> -process <prog> -noanalysis
//          -scriptPath re/scripts -postScript NameVtablesAndExport.java <outdir>
// @category PhysX

import java.io.*;
import java.util.*;

import ghidra.app.decompiler.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.symbol.*;

public class NameVtablesAndExport extends GhidraScript {
  @Override
  public void run() throws Exception {
    String[] args = getScriptArgs();
    File outDir = new File(args.length > 0 ? args[0] : "re/out");
    outDir.mkdirs();

    int renamed = nameVtables(new File(outDir, "vtables.tsv"));
    println("renamed vtable slot functions: " + renamed);

    DecompInterface di = new DecompInterface();
    DecompileOptions opts = new DecompileOptions();
    di.setOptions(opts);
    di.openProgram(currentProgram);
    int n = 0, fail = 0;
    try (PrintWriter c = new PrintWriter(new FileWriter(new File(outDir, "decomp.c")));
         PrintWriter idx = new PrintWriter(new FileWriter(new File(outDir, "functions.tsv")))) {
      idx.println("entry\tname\tsize\tcallers");
      for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
        if (monitor.isCancelled()) break;
        if (f.isThunk() || f.isExternal()) continue;
        DecompileResults r = di.decompileFunction(f, 60, monitor);
        String body = (r != null && r.decompileCompleted()) ? r.getDecompiledFunction().getC() : null;
        c.println("// ==== " + f.getEntryPoint() + " " + f.getName(true));
        if (body == null) { c.println("// decompile failed\n"); fail++; }
        else c.println(body);
        idx.println(f.getEntryPoint() + "\t" + f.getName(true) + "\t" + f.getBody().getNumAddresses() + "\t"
            + f.getSymbol().getReferenceCount());
        n++;
      }
    } finally {
      di.dispose();
    }
    println("decompiled " + n + " functions, " + fail + " failed -> " + outDir);
  }

  private int nameVtables(File tsv) throws Exception {
    SymbolTable st = currentProgram.getSymbolTable();
    Memory mem = currentProgram.getMemory();
    FunctionManager fm = currentProgram.getFunctionManager();
    int renamed = 0;
    try (PrintWriter out = new PrintWriter(new FileWriter(tsv))) {
      out.println("class\tvftable\tslot\ttarget\tname");
      SymbolIterator it = st.getSymbolIterator("vftable", true);
      while (it.hasNext()) {
        Symbol s = it.next();
        String cls = s.getParentNamespace().getName(true);
        Address a = s.getAddress();
        for (int slot = 0; slot < 1024; slot++) {
          Address p = a.add(slot * 4L);
          if (slot > 0 && st.getPrimarySymbol(p) != null && st.getPrimarySymbol(p).getName().contains("vftable")) break;
          int v;
          try { v = mem.getInt(p); } catch (MemoryAccessException e) { break; }
          Address t = a.getNewAddress(Integer.toUnsignedLong(v));
          MemoryBlock b = mem.getBlock(t);
          if (b == null || !b.isExecute()) break;
          Function f = fm.getFunctionAt(t);
          if (f == null) {
            try { f = createFunction(t, null); } catch (Exception e) { f = null; }
          }
          String name = f != null ? f.getName(true) : "?";
          if (f != null && f.getName().startsWith("FUN_")) {
            try {
              f.setName(cls.replace("::", "_") + "_vf" + slot, SourceType.ANALYSIS);
              renamed++;
              name = f.getName(true);
            } catch (Exception e) { /* duplicate etc. */ }
          }
          out.println(cls + "\t" + a + "\t" + slot + "\t" + t + "\t" + name);
        }
      }
    }
    return renamed;
  }
}
