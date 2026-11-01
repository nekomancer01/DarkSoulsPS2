"""EzState (.esd) reader and expression decompiler for DSR (long format "fsSL", 64-bit offsets).
Layout from SoulsFormats ESD.cs; expression bytecode from ESDLang EzInfixor.cs.
Usage: esd.py <file.esd[.dcx]> [out.txt]  — writes every state group as readable text."""
import struct, sys

sys.path.insert(0, __import__("os").path.dirname(__file__))
import souls

OPS = {0x8C: "+", 0x8E: "-", 0x8F: "*", 0x90: "/", 0x91: "<=", 0x92: ">=", 0x93: "<", 0x94: ">", 0x95: "==",
       0x96: "!=", 0x98: "&&", 0x99: "||"}


def expr(b):
    """Bytecode -> infix string. Function calls print as f<id>(args)."""
    st, i = [], 0
    while i < len(b):
        o = b[i]
        if o <= 0x7F:
            st.append(str(o - 64))
        elif o == 0x80:
            st.append(f"{struct.unpack_from('<f', b, i + 1)[0]:g}")
            i += 4
        elif o == 0x81:
            st.append(f"{struct.unpack_from('<d', b, i + 1)[0]:g}")
            i += 8
        elif o == 0x82:
            st.append(str(struct.unpack_from("<i", b, i + 1)[0]))
            i += 4
        elif o == 0xA5:
            j = i + 1
            while b[j:j + 2] != b"\0\0":
                j += 2
            st.append('"' + b[i + 1:j].decode("utf-16le") + '"')
            i = j + 1
        elif 0x84 <= o <= 0x8A:
            n = o - 0x84
            args = st[len(st) - n:] if n else []
            del st[len(st) - n:]
            st.append(f"f{st.pop()}({', '.join(args)})")
        elif o in OPS:
            r, l = st.pop(), st.pop()
            st.append(f"({l} {OPS[o]} {r})")
        elif o == 0x8D:
            st.append(f"-{st.pop()}")
        elif o == 0x9A:
            st.append(f"!{st.pop()}")
        elif o == 0xA6:
            st[-1] += " ~continue"
        elif 0xA7 <= o <= 0xAE:
            st.append(f"SetREG{o - 0xA7}({st.pop()})")
        elif 0xAF <= o <= 0xB6:
            st.append(f"REG{o - 0xAF}")
        elif o == 0xB7:
            st[-1] += " ~abort"
        elif o == 0xB8:
            st.append(f"Arg({st.pop()})")
        elif o == 0xB9:
            st.append("CallResult")
        elif o == 0xBA:
            st.append("CallOngoing")
        elif o == 0xA1:
            pass
        else:
            st.append(f"?{o:02x}")
        i += 1
    return " ; ".join(st)


class ESD:
    def __init__(self, b):
        assert b[:4] == b"fsSL", "only long-format (DSR) ESD"
        self.b = b
        n_groups = struct.unpack_from("<i", b, 0x28)[0]
        n_states, n_conds = struct.unpack_from("<i", b, 0x30)[0], struct.unpack_from("<i", b, 0x38)[0]
        self.ds = ds = 0x6C
        pos = ds + 0x18  # int 1, four unknown ints, int 0
        groups_off = struct.unpack_from("<q", b, pos)[0]
        pos = ds + groups_off
        self.groups = {}
        for _ in range(n_groups):
            gid, s_off, s_count, _ = struct.unpack_from("<4q", b, pos)
            pos += 0x20
            self.groups[gid] = (s_off, s_count)
        # states follow the group headers; each group's state list has one extra copy of state 0 after it
        self.states = {}
        for _ in range(n_states):
            self.states[pos - ds] = self._state(pos)
            pos += 0x48
        self.conds = {}
        for _ in range(n_conds):
            self.conds[pos - ds] = self._cond(pos)
            pos += 0x38

    def q(self, off, n):
        return struct.unpack_from(f"<{n}q", self.b, off)

    def _cmds(self, off, n):
        out = []
        for k in range(n):
            bank, cid, a_off, a_n = struct.unpack_from("<iiqq", self.b, self.ds + off + 0x18 * k)
            args = [expr(self.b[self.ds + ao:self.ds + ao + al]) for ao, al in
                    (self.q(self.ds + a_off + 0x10 * j, 2) for j in range(a_n))]
            out.append((bank, cid, args))
        return out

    def _state(self, p):
        sid, c_off, c_n, en_off, en_n, ex_off, ex_n, wh_off, wh_n = self.q(p, 9)
        return {"id": sid, "conds": list(self.q(self.ds + c_off, c_n)) if c_n else [],
                "entry": self._cmds(en_off, en_n), "exit": self._cmds(ex_off, ex_n), "while": self._cmds(wh_off, wh_n)}

    def _cond(self, p):
        target, pc_off, pc_n, sc_off, sc_n, ev_off, ev_len = self.q(p, 7)
        return {"target": target, "pass": self._cmds(pc_off, pc_n),
                "sub": list(self.q(self.ds + sc_off, sc_n)) if sc_n else [],
                "eval": expr(self.b[self.ds + ev_off:self.ds + ev_off + ev_len])}

    def group_states(self, gid):
        s_off, s_count = self.groups[gid]
        return {s_off + 0x48 * k: self.states[s_off + 0x48 * k] for k in range(s_count)}


def cmd_str(c):
    bank, cid, args = c
    return f"c{bank}_{cid}({', '.join(args)})"


def dump(e, out):
    for gid in sorted(e.groups):
        states = e.group_states(gid)
        sid_of = {off: s["id"] for off, s in states.items()}
        out.write(f"\n######## group {gid}: {len(states)} states\n")

        def cond_lines(coff, depth):
            c = e.conds[coff]
            tgt = f" -> state {sid_of.get(c['target'], c['target'])}" if c["target"] != -1 else ""
            out.write(f"{'  ' * depth}if {c['eval']}{tgt}\n")
            for pc in c["pass"]:
                out.write(f"{'  ' * depth}  do {cmd_str(pc)}\n")
            for sc in c["sub"]:
                cond_lines(sc, depth + 1)

        for off, s in sorted(states.items(), key=lambda kv: kv[1]["id"]):
            out.write(f"\nstate {s['id']}\n")
            for k in ("entry", "while", "exit"):
                for c in s[k]:
                    out.write(f"  {k}: {cmd_str(c)}\n")
            for coff in s["conds"]:
                cond_lines(coff, 1)


if __name__ == "__main__":
    e = ESD(souls.read(sys.argv[1]))
    with open(sys.argv[2], "w") if len(sys.argv) > 2 else sys.stdout as f:
        dump(e, f)
