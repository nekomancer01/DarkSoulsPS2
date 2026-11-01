// Prints memory and code at addresses. Each arg is <kind>:<hex address>[:<count>]:
//   f = floats, i = int32s, h = uint16s, b = bytes, q = qwords with the symbol at each target (vtables, pointer tables),
//   d = disassembly (count instructions), r = references to the address, with the containing function and the
//       nearest preceding label (e.g. which vftable slot points at a function).
//@category DSR
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.Symbol;

public class Peek extends GhidraScript {
	@Override
	public void run() throws Exception {
		for (String arg : getScriptArgs()) {
			String[] p = arg.split(":");
			String k = p[0];
			Address a = toAddr(Long.parseUnsignedLong(p[1].replace("0x", ""), 16));
			int n = p.length > 2 ? Integer.parseInt(p[2]) : 1;
			println("## " + arg);
			for (int i = 0; i < n || k.equals("r"); i++) {
				switch (k) {
				case "f": println(a.add(4 * i) + "  " + getFloat(a.add(4 * i))); break;
				case "i": println(a.add(4 * i) + "  " + getInt(a.add(4 * i))); break;
				case "h": println(a.add(2 * i) + "  " + (getShort(a.add(2 * i)) & 0xffff)); break;
				case "b": println(a.add(i) + "  " + (getByte(a.add(i)) & 0xff)); break;
				case "q": {
					long v = getLong(a.add(8 * i));
					Address t = toAddr(v);
					Symbol s = getSymbolAt(t);
					Function f = getFunctionAt(t);
					println(a.add(8 * i) + "  +0x" + Integer.toHexString(8 * i) + "  " + Long.toHexString(v) + "  "
						+ (s != null ? s.getName(true) : "") + (f != null && s == null ? f.getName() : ""));
					break;
				}
				case "d": {
					Instruction ins = getInstructionAt(a);
					if (ins == null) ins = getInstructionAfter(a);
					for (int j = 0; j < n && ins != null; j++) {
						println(ins.getAddress() + "  " + ins);
						ins = ins.getNext();
					}
					i = n;
					break;
				}
				case "r": {
					for (Reference r : getReferencesTo(a)) {
						Address from = r.getFromAddress();
						Function f = getFunctionContaining(from);
						Symbol lab = null;
						Address back = from;
						for (int j = 0; j < 0x1000 && lab == null; j += 8) {
							Symbol s = getSymbolAt(back.subtract(j));
							if (s != null && s.getName().contains("vftable")) lab = s;
							if (f != null) break;
						}
						String where = f != null ? f.getEntryPoint() + " " + f.getName()
							: (lab != null ? lab.getName(true) + " @" + lab.getAddress() + " slot +0x"
								+ Long.toHexString(from.subtract(lab.getAddress())) : "?");
						println(a + " <- " + from + " " + r.getReferenceType() + " in " + where);
					}
					k = "";
					i = n;
					break;
				}
				default: i = n;
				}
			}
		}
	}
}
