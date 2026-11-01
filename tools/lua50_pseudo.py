"""Readable pseudo-code from Lua 5.0 bytecode (tools/lua50.py chunks): expression tracking per register, statements
for calls / stores, conditional gotos for branches. Not a decompiler (no loops / if-blocks rebuilt); for reading AI
scripts quickly.

Usage: lua50_pseudo.py <luabnd.dcx> <file name filter> [function name filter]"""
import os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import souls
import lua50
from lua50 import OPS, MAXSTACK, decode

BIN = {"ADD": "+", "SUB": "-", "MUL": "*", "DIV": "/", "POW": "^"}


def k(f, x):
    v = f.k[x]
    if isinstance(v, float) and v == int(v):
        return str(int(v))
    return repr(v)


def pseudo(f, name, out, depth=0):
    pad = "  " * depth
    names = {}
    for n, s, e in f.locals:
        names.setdefault(n, (s, e))
    loc = lambda r, pc: next((n for n, (s, e) in [(n, se) for n, se in [(l[0], (l[1], l[2])) for l in f.locals]]
                              if False), None)

    def lname(r, pc):
        # r-th active local at pc
        act = [l for l in f.locals if l[1] <= pc < l[2]]
        return act[r][0] if r < len(act) else None

    reg = {}

    def R(r, pc):
        return reg.get(r) or lname(r, pc) or f"r{r}"

    def RK(x, pc):
        return k(f, x - MAXSTACK) if x >= MAXSTACK else R(x, pc)

    targets = set()
    for pc, ins in enumerate(f.code):
        op, a, b, c, bx, sbx = decode(ins)
        if OPS[op] in ("JMP", "FORLOOP", "TFORPREP"):
            targets.add(pc + 1 + sbx)
    params = [lname(i, 0) or f"a{i}" for i in range(f.nparams)]
    out.append(f"{pad}function {name}({', '.join(params)}{', ...' if f.vararg else ''})")
    pad += "  "
    skip = 0
    for pc, ins in enumerate(f.code):
        if pc in targets:
            out.append(f"{pad[:-2]}L{pc}:")
            reg.clear()
        if skip:
            skip -= 1
            continue
        op, a, b, c, bx, sbx = decode(ins)
        n = OPS[op]

        def setr(r, e):
            ln = lname(r, pc + 1)
            if ln and ln != e:
                out.append(f"{pad}{ln} = {e}")
                reg.pop(r, None)
            else:
                reg[r] = e
        if n == "MOVE":
            setr(a, R(b, pc))
        elif n == "LOADK":
            setr(a, k(f, bx))
        elif n == "LOADBOOL":
            setr(a, "true" if b else "false")
            if c:
                out.append(f"{pad}(skip next)")
        elif n == "LOADNIL":
            for r in range(a, b + 1):
                setr(r, "nil")
        elif n == "GETUPVAL":
            setr(a, f"upval{b}")
        elif n == "GETGLOBAL":
            setr(a, f.k[bx])
        elif n == "GETTABLE":
            key = RK(c, pc)
            setr(a, f"{R(b, pc)}.{key[1:-1]}" if key.startswith("'") else f"{R(b, pc)}[{key}]")
        elif n == "SETGLOBAL":
            out.append(f"{pad}{f.k[bx]} = {R(a, pc)}")
        elif n == "SETUPVAL":
            out.append(f"{pad}upval{b} = {R(a, pc)}")
        elif n == "SETTABLE":
            out.append(f"{pad}{R(a, pc)}[{RK(b, pc)}] = {RK(c, pc)}")
        elif n == "NEWTABLE":
            setr(a, "{}")
        elif n == "SELF":
            obj = R(b, pc)
            reg[a + 1] = obj
            reg[a] = f"{obj}:{RK(c, pc)[1:-1]}"
        elif n in BIN:
            setr(a, f"({RK(b, pc)} {BIN[n]} {RK(c, pc)})")
        elif n == "UNM":
            setr(a, f"-{R(b, pc)}")
        elif n == "NOT":
            setr(a, f"not {R(b, pc)}")
        elif n == "CONCAT":
            setr(a, " .. ".join(R(r, pc) for r in range(b, c + 1)))
        elif n == "JMP":
            out.append(f"{pad}goto L{pc + 1 + sbx}")
        elif n in ("EQ", "LT", "LE"):
            sym = {"EQ": ("==", "~="), "LT": ("<", ">="), "LE": ("<=", ">")}[n]
            # if (RK(B) op RK(C)) ~= A then skip next (the jump); i.e. jump when (cmp == A)
            cond = f"{RK(b, pc)} {sym[0] if a else sym[1]} {RK(c, pc)}"
            nxt = decode(f.code[pc + 1])
            out.append(f"{pad}if {cond} then goto L{pc + 2 + nxt[5]}")
            skip = 1
        elif n == "TEST":
            nxt = decode(f.code[pc + 1])
            out.append(f"{pad}if {'' if c else 'not '}{R(b, pc)} then {'' if a == b else f'r{a} = {R(b, pc)}; '}goto L{pc + 2 + nxt[5]}")
            skip = 1
        elif n in ("CALL", "TAILCALL"):
            fn = R(a, pc)
            if ":" in fn:
                args = [R(r, pc) for r in range(a + 2, a + b)] if b else [R(r, pc) for r in range(a + 2, a + 6)] + ["..."]
            else:
                args = [R(r, pc) for r in range(a + 1, a + b)] if b else ["..."]
            e = f"{fn}({', '.join(args)})"
            if n == "TAILCALL":
                out.append(f"{pad}return {e}")
            elif c == 1:
                out.append(f"{pad}{e}")
            elif c == 2:
                setr(a, e)
            else:
                out.append(f"{pad}r{a}..r{a + c - 2 if c else 'top'} = {e}")
                for r in range(a, a + max(c - 1, 1)):
                    reg.pop(r, None)
        elif n == "RETURN":
            if b == 1 and pc == len(f.code) - 1:
                pass
            else:
                vals = [R(r, pc) for r in range(a, a + b - 1)] if b else [f"r{a}..top"]
                out.append(f"{pad}return {', '.join(vals)}")
        elif n == "FORLOOP":
            out.append(f"{pad}{R(a, pc)} += step; if within limit goto L{pc + 1 + sbx}")
        elif n == "TFORLOOP":
            out.append(f"{pad}tforloop r{a} ({c + 1} vars)")
        elif n == "TFORPREP":
            out.append(f"{pad}tforprep r{a} goto L{pc + 1 + sbx}")
        elif n in ("SETLIST", "SETLISTO"):
            out.append(f"{pad}{R(a, pc)}[...] = r{a + 1}..")
        elif n == "CLOSE":
            pass
        elif n == "CLOSURE":
            setr(a, f"<closure {name}.{bx}>")
            skip = f.p[bx].nups
        else:
            out.append(f"{pad}{n} {a} {b} {c}")
    for i, sub in enumerate(f.p):
        pseudo(sub, f"{name}.{i}", out, depth + 1)


def main():
    files = {souls.basename(n): d for _, n, d in souls.bnd3(souls.read(sys.argv[1]))}
    flt = sys.argv[2]
    for n in sorted(files):
        if flt in n and files[n][:5] == b"\x1bLuaP":
            out = []
            pseudo(lua50.load(files[n]), "main", out)
            print(f"==== {n}")
            print("\n".join(out))


if __name__ == "__main__":
    main()
