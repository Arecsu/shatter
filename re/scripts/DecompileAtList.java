// Headless Ghidra post-script: create a function at every address listed in a TSV (name<TAB>hexaddr) if none exists,
// then decompile each listed function (plus any new callees' entries already present) into one C file.
// Usage: analyzeHeadless <proj> <name> -process <prog> -noanalysis -scriptPath re/scripts
//          -postScript DecompileAtList.java <list.tsv> <out.c>
// @category PhysX

import java.io.*;
import java.util.*;

import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.decompiler.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;

public class DecompileAtList extends GhidraScript {
  @Override
  public void run() throws Exception {
    String[] args = getScriptArgs();
    List<String[]> rows = new ArrayList<>();
    try (BufferedReader r = new BufferedReader(new FileReader(args[0]))) {
      for (String l; (l = r.readLine()) != null;) {
        String[] p = l.split("\t");
        if (p.length >= 2) rows.add(p);
      }
    }
    Listing listing = currentProgram.getListing();
    int created = 0;
    for (String[] p : rows) {
      Address a = toAddr(Long.parseLong(p[1], 16));
      if (listing.getFunctionAt(a) == null) {
        disassemble(a);
        CreateFunctionCmd cmd = new CreateFunctionCmd(a);
        if (cmd.applyTo(currentProgram, monitor)) created++;
      }
      Function f = listing.getFunctionAt(a);
      if (f != null && f.getName().startsWith("FUN_")) f.setName(p[0], ghidra.program.model.symbol.SourceType.USER_DEFINED);
    }
    println("created functions: " + created);
    DecompInterface di = new DecompInterface();
    di.openProgram(currentProgram);
    int n = 0, failed = 0;
    try (PrintWriter w = new PrintWriter(new FileWriter(args[1]))) {
      for (String[] p : rows) {
        Function f = listing.getFunctionAt(toAddr(Long.parseLong(p[1], 16)));
        if (f == null) { failed++; continue; }
        DecompileResults res = di.decompileFunction(f, 60, monitor);
        w.println("// ==== " + f.getEntryPoint() + " " + f.getName());
        if (res.decompileCompleted()) { w.println(res.getDecompiledFunction().getC()); n++; }
        else { w.println("// decompile failed"); failed++; }
      }
    }
    println("decompiled " + n + " functions, " + failed + " failed -> " + args[1]);
  }
}
