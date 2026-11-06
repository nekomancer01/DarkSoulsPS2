"""Functional VU1 emulator for the ops ps2/proto's programs use, with the timing effects that matter for scheduled
code: Q lands 7 cycles after DIV (reads before that see the old Q unless WAITQ), MAC / CLIP flag reads see the
writers issued >= 4 cycles earlier, a conditional branch reads a VI written by the pair right before it as it was
before that write. VF / VI / ACC behave in program order otherwise (hardware interlocks). XGKICK packets are
decoded into vertex lists so two programs can be compared on the same input (tools/vu1/test_*.py)."""
import struct
from vucore import parse_program, Timer, Ins

F = lambda u: struct.unpack('<f', struct.pack('<I', u & 0xFFFFFFFF))[0]


def U(f):
    try:
        return struct.unpack('<I', struct.pack('<f', f))[0]
    except OverflowError:
        return 0x7F7FFFFF if f > 0 else 0xFF7FFFFF


def r32(f):
    return F(U(f))


IDX = {'x': 0, 'y': 1, 'z': 2, 'w': 3}


class VU1:
    def __init__(self, code_text):
        self.pairs, self.labels = parse_program(code_text)
        self.mem = [[0, 0, 0, 0] for _ in range(1024)]
        self.vf = [[0, 0, 0, 0] for _ in range(32)]
        self.vf[0] = [U(0), U(0), U(0), U(1.0)]
        self.vi = [0] * 16
        self.acc = [0.0] * 4
        self.q = 0.0
        self.q_pending = []  # (land cycle, value)
        self.p = 0.0
        self.p_pending = []
        self.mac_hist = []   # (issue cycle, flags)
        self.clip_hist = []  # (issue cycle, clip register after)
        self.clip = 0
        self.top = 0
        self.kicks = []

    # ---------------- memory helpers
    def wq(self, addr, vals, f=False):
        self.mem[addr & 1023] = [U(v) if f else v & 0xFFFFFFFF for v in vals]

    def rqf(self, addr):
        return [F(u) for u in self.mem[addr & 1023]]

    def getf(self, reg):
        n = int(reg[2:])
        return [F(u) for u in self.vf[n]]

    def setf(self, reg, vals, fields):
        n = int(reg[2:])
        if n == 0:
            return
        for c in fields:
            v = vals[IDX[c]]
            self.vf[n][IDX[c]] = v if isinstance(v, int) else U(r32(v))

    def geti(self, reg):
        return self.vi[int(reg[2:])] & 0xFFFF

    def seti(self, reg, v):
        n = int(reg[2:])
        if n:
            self.vi_wrote = getattr(self, 'vi_wrote', []) + [(n, self.vi[n])]
            self.vi[n] = v & 0xFFFF

    def geti_branch(self, reg):
        """a conditional branch reads a VI written by the pair right before it as it was before that write (VU1
        hardware / PCSX2 microVU: the branch's VI read is a cycle ahead)"""
        n = int(reg[2:])
        for k, old in getattr(self, 'vi_last', []):
            if k == n:
                return old & 0xFFFF
        return self.geti(reg)

    # ---------------- run
    def run(self, entry, max_pairs=200000):
        pc = self.labels[entry] if isinstance(entry, str) else entry
        tm = Timer()
        pending_branch = None
        end = False
        steps = 0
        while steps < max_pairs:
            steps += 1
            u, l = self.pairs[pc]
            c = tm.issue(u, l)
            self.cycle = c
            # land Q values
            for lc, v in list(self.p_pending):
                if lc <= c or l.waitp:
                    self.p = v
                    self.p_pending.remove((lc, v))
            for lc, v in list(self.q_pending):
                if lc <= c:
                    self.q = v
                    self.q_pending.remove((lc, v))
            # upper reads happen before lower writes of the same pair: evaluate upper, then lower, then commit
            uw = self.exec_upper(u, c)
            lw, br = self.exec_lower(l, c)
            self.vi_wrote = []
            for fn in uw + lw:
                fn()
            self.vi_last = self.vi_wrote
            nxt = pc + 1
            if pending_branch is not None:
                nxt, pending_branch = pending_branch, None
            if br is not None:
                pending_branch = br
            if end:
                return c + 1
            if u.ebit:
                end = True
            pc = nxt
        raise RuntimeError('no end')

    def ops_f(self, op):
        reg, f = Ins.reg(op)
        return reg, f

    def exec_upper(self, ins, c):
        mn = ins.mnem
        if mn == 'nop':
            return []
        base, _, dest = mn.partition('.')
        dest = dest or 'xyzw'
        ops = ins.ops
        if base.startswith('ftoi') or base.startswith('itof'):
            d, s = ops
            sreg = Ins.reg(s)[0]
            n = int(sreg[2:])
            raw = list(self.vf[n])
            k = int(base[4:])
            if base.startswith('ftoi'):
                vals = []
                for x in raw:
                    v = int(F(x) * (1 << k))  # truncation toward zero
                    v = max(-0x80000000, min(0x7FFFFFFF, v))
                    vals.append(v & 0xFFFFFFFF)
            else:
                vals = []
                for x in raw:
                    xi = x - (1 << 32) if x & 0x80000000 else x
                    vals.append(r32(xi / (1 << k)))
            dreg = Ins.reg(d)[0]
            return [lambda: self.setf(dreg, vals, dest)]
        if base == 'abs':
            d, sreg = ops
            vals = [abs(x) for x in self.getf(Ins.reg(sreg)[0])]
            dreg = Ins.reg(d)[0]
            return [lambda: self.setf(dreg, vals, dest)]
        if base.startswith('clip'):
            s, t = ops
            sv = self.getf(Ins.reg(s)[0])
            w = abs(self.getf(Ins.reg(t)[0])[3])
            fl = 0
            for k, comp in enumerate(sv[:3]):
                if comp > w:
                    fl |= 1 << (2 * k)
                if comp < -w:
                    fl |= 1 << (2 * k + 1)

            def go():
                self.clip = ((self.clip << 6) | fl) & 0xFFFFFF
                self.clip_hist.append((c, self.clip))
            return [go]
        if base in ('opmula', 'opmsub'):
            d, s, t = ops
            a = self.getf(Ins.reg(s)[0])
            b = self.getf(Ins.reg(t)[0])
            prod = [a[1] * b[2], a[2] * b[0], a[0] * b[1], 0]
            if base == 'opmula':
                def go():
                    for k in range(3):
                        self.acc[k] = r32(prod[k])
                    self.mac(c, prod, 'xyz')
                return [go]
            res = [r32(self.acc[k] - (a[1 + k - 3 if k == 2 else k + 1] * 0)) for k in range(3)]
            # opmsub: fd = ACC - (fs.yzx * ft.zxy)
            res = [self.acc[0] - a[1] * b[2], self.acc[1] - a[2] * b[0], self.acc[2] - a[0] * b[1], 0]
            dreg = Ins.reg(d)[0]

            def go():
                self.setf(dreg, res, 'xyz')
                self.mac(c, res, 'xyz')
            return [go]
        for b in ('msuba', 'madda', 'mula', 'adda', 'suba', 'msub', 'madd', 'mini', 'max', 'mul', 'add', 'sub'):
            if base.startswith(b) and base[len(b):] in ('', 'x', 'y', 'z', 'w', 'q', 'i'):
                bc = base[len(b):]
                d, s, t = ops
                sv = self.getf(Ins.reg(s)[0])
                if bc == 'q':
                    tv = [self.q] * 4
                elif bc:
                    tv = [self.getf(Ins.reg(t)[0])[IDX[bc]]] * 4
                else:
                    tv = self.getf(Ins.reg(t)[0])
                core = b.rstrip('a') if b not in ('madda', 'msuba', 'adda', 'suba', 'mula') else b[:-1]
                if b in ('mini',):
                    core = 'mini'
                res = [0.0] * 4
                for k in range(4):
                    x, y = sv[k], tv[k]
                    if core == 'add':
                        r = x + y
                    elif core == 'sub':
                        r = x - y
                    elif core == 'mul':
                        r = x * y
                    elif core == 'madd':
                        r = self.acc[k] + x * y
                    elif core == 'msub':
                        r = self.acc[k] - x * y
                    elif core == 'max':
                        r = max(x, y)
                    elif core == 'mini':
                        r = min(x, y)
                    else:
                        raise ValueError(core)
                    res[k] = r32(r)
                to_acc = d.upper() == 'ACC'
                dreg = None if to_acc else Ins.reg(d)[0]

                def go(res=res, to_acc=to_acc, dreg=dreg, core=core):
                    if to_acc:
                        for ch in dest:
                            self.acc[IDX[ch]] = res[IDX[ch]]
                    else:
                        self.setf(dreg, res, dest)
                    if core not in ('max', 'mini'):
                        self.mac(c, res, dest)
                return [go]
        raise ValueError('emu: upper %r' % ins.text)

    def mac(self, c, res, dest):
        fl = 0
        for ch in dest:
            k = IDX[ch]
            bit = 3 - k
            if res[k] == 0:
                fl |= 1 << bit
            if res[k] < 0:
                fl |= 1 << (bit + 4)
        self.mac_hist.append((c, fl))

    def flag_at(self, hist, c, default=0):
        v = default
        for ic, f in hist:
            if ic <= c - 4:
                v = f
        return v

    def exec_lower(self, ins, c):
        mn = ins.mnem
        if mn == 'nop':
            return [], None
        base, _, dest = mn.partition('.')
        dest = dest or 'xyzw'
        ops = ins.ops

        def addr(a):
            off, b = a.split('(')
            return eval(off or '0', {}, {}) + self.geti(b.rstrip(')'))
        if base == 'lq':
            a = addr(ops[1])
            vals = list(self.mem[a & 1023])
            d = Ins.reg(ops[0])[0]
            return [lambda: self.setf(d, vals, dest)], None
        if base == 'sq':
            a = addr(ops[1])
            n = int(Ins.reg(ops[0])[0][2:])
            vals = list(self.vf[n])

            def go():
                for ch in dest:
                    self.mem[a & 1023][IDX[ch]] = vals[IDX[ch]]
            return [go], None
        if base == 'ilw':
            a = addr(ops[1])
            v = self.mem[a & 1023][IDX[dest[0]]] & 0xFFFF
            d = ops[0]
            return [lambda: self.seti(d, v)], None
        if base == 'isw':
            a = addr(ops[1])
            v = self.geti(ops[0])

            def go():
                for ch in dest:
                    self.mem[a & 1023][IDX[ch]] = v
            return [go], None
        if base in ('iaddiu', 'iaddi'):
            v = self.geti(ops[1]) + eval(ops[2], {}, {})
            return [lambda: self.seti(ops[0], v)], None
        if base == 'isubiu':
            v = self.geti(ops[1]) - eval(ops[2], {}, {})
            return [lambda: self.seti(ops[0], v)], None
        if base in ('iadd', 'isub', 'iand', 'ior'):
            a, b = self.geti(ops[1]), self.geti(ops[2])
            v = {'iadd': a + b, 'isub': a - b, 'iand': a & b, 'ior': a | b}[base]
            return [lambda: self.seti(ops[0], v)], None
        if base == 'mtir':
            reg, f = Ins.reg(ops[1])
            v = self.vf[int(reg[2:])][IDX[f]] & 0xFFFF
            return [lambda: self.seti(ops[0], v)], None
        if base == 'div':
            s, sf = Ins.reg(ops[1])
            t, tf = Ins.reg(ops[2])
            num, den = self.getf(s)[IDX[sf]], self.getf(t)[IDX[tf]]
            v = num / den if den != 0 else (3.4e38 if num >= 0 else -3.4e38)
            self.q_pending.append((c + 7, r32(v)))
            return [], None
        if base in ('erleng', 'ersqrt'):
            reg, f = Ins.reg(ops[1])
            vv = self.getf(reg)
            if base == 'erleng':
                x = vv[0] * vv[0] + vv[1] * vv[1] + vv[2] * vv[2]
            else:
                x = vv[IDX[f]]
            r = 1 / x ** 0.5 if x > 0 else 3.4e38
            self.p_pending.append((c + ins.lat, r32(r)))
            return [], None
        if base == 'mfp':
            v = U(self.p)
            d = Ins.reg(ops[0])[0]
            return [lambda: self.setf(d, [v] * 4, dest)], None
        if base == 'waitp':
            return [], None
        if base == 'waitq':
            if self.q_pending:
                self.q = self.q_pending[-1][1]
                self.q_pending = []
            return [], None
        if base == 'fcand':
            v = 1 if self.flag_at(self.clip_hist, c) & eval(ops[1], {}, {}) else 0
            return [lambda: self.seti(ops[0], v)], None
        if base == 'fcor':
            v = 1 if (self.flag_at(self.clip_hist, c) | eval(ops[1], {}, {})) == 0xFFFFFF else 0
            return [lambda: self.seti(ops[0], v)], None
        if base == 'move':
            n = int(Ins.reg(ops[1])[0][2:])
            vals = list(self.vf[n])
            d = Ins.reg(ops[0])[0]
            return [lambda: self.setf(d, vals, dest)], None
        if base == 'fmand':
            v = self.flag_at(self.mac_hist, c) & self.geti(ops[1])
            return [lambda: self.seti(ops[0], v)], None
        if base == 'xtop':
            v = self.top
            return [lambda: self.seti(ops[0], v)], None
        if base == 'xgkick':
            self.kick(self.geti(ops[0]))
            return [], None
        if base in ('ibne', 'ibeq'):
            a, b = self.geti_branch(ops[0]), self.geti_branch(ops[1])
            take = (a != b) if base == 'ibne' else (a == b)
            return [], (ins.target if take else None)
        if base in ('ibltz', 'ibgez', 'ibgtz', 'iblez'):
            a = self.geti_branch(ops[0])
            a = a - 0x10000 if a & 0x8000 else a
            take = {'ibltz': a < 0, 'ibgez': a >= 0, 'ibgtz': a > 0, 'iblez': a <= 0}[base]
            return [], (ins.target if take else None)
        if base == 'b':
            return [], ins.target
        raise ValueError('emu: lower %r' % ins.text)

    def kick(self, addr):
        """Decode GIF packets (PACKED) from addr until EOP; collect primitives' vertices."""
        self.kick_addrs = getattr(self, 'kick_addrs', []) + [addr & 1023]
        out = []
        while True:
            t = self.mem[addr & 1023]
            lo = t[0] | (t[1] << 32)
            hi = t[2] | (t[3] << 32)
            nloop, eop = lo & 0x7FFF, (lo >> 15) & 1
            pre, prim, flg, nreg = (lo >> 46) & 1, (lo >> 47) & 0x7FF, (lo >> 58) & 3, (lo >> 60) & 15
            nreg = nreg or 16
            regs = [(hi >> (4 * k)) & 15 for k in range(nreg)]
            addr += 1
            for _ in range(nloop):
                for r in regs:
                    q = self.mem[addr & 1023]
                    out.append((r, tuple(q), prim if pre else None))
                    addr += 1
            if eop:
                break
        self.kicks.append(out)


def vertices(kicks):
    """-> list of dicts per vertex from STQ (2) / RGBAQ (1) / XYZF2 (4) register writes; A+D (0xE) skipped."""
    vs, cur = [], {}
    for k in kicks:
        for r, q, prim in k:
            if r == 2:
                cur['stq'] = tuple(F(u) for u in q[:3])
            elif r == 1:
                cur['rgba'] = tuple(x & 0xFF for x in q)
            elif r == 4:
                cur['x'], cur['y'] = q[0] & 0xFFFF, q[1] & 0xFFFF
                cur['z'] = (q[2] >> 4) & 0xFFFFFF
                cur['f'] = (q[3] >> 4) & 0xFF
                cur['adc'] = (q[3] >> 15) & 1
                vs.append(cur)
                cur = {}
    return vs
