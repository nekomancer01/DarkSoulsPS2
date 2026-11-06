"""VU1 microcode model for ps2/proto: instruction semantics, a cycle counter for .vsm files and a list scheduler with
register allocation for generated programs (tools/vu1/gen.py).

Timing model (VU1 at 294.912 MHz, from the EE/VU user's manuals):
  FMAC results (upper ops, LQ, MOVE, MR32, MFIR, ITOF/FTOI): usable 4 cycles after issue, hardware stalls before that.
  ACC: accumulate chains (MULA / MADDA ... MADD) issue back to back (latency 1).
  DIV / SQRT: Q written 7 cycles after issue (RSQRT 13); WAITQ stalls until then; a second DIV stalls while one runs.
  MAC / CLIP flags: a flag read at cycle c sees the newest FMAC / CLIP issued at <= c - 4 (no interlock).
  VI: integer ALU results next cycle; ILW treated as 4 (conservative); a branch reads a VI written >= 2 cycles before.
The scheduler only emits code that never stalls in this model, so 'count' on generated code = its cycle count.
"""
import re

FIELDS = 'xyzw'
LAT_FMAC = 4
LAT_DIV = 7
LAT_RSQRT = 13
LAT_EFU = {'erleng': 24, 'ersqrt': 18, 'ercpr': 12, 'esum': 12, 'eleng': 18, 'esqrt': 12}  # EFU (VU1 only), not pipelined


class Ins:
    """One upper or lower instruction. Registers are 'VFnn', 'VInn', 'ACC', 'Q', 'MAC', 'CLIP' or virtual names
    ('%name' = VF, '&name' = VI). reads / writes: lists of (reg, fields)."""

    def __init__(self, pipe, text):
        self.pipe, self.text = pipe, text.strip()
        self.reads, self.writes = [], []
        self.lat = 1
        self.flag_w = None   # 'MAC' / 'CLIP' when this op writes those flags
        self.flag_r = None
        self.mem_r = self.mem_w = None
        self.branch = self.xgkick = self.waitq = self.div = self.efu = self.waitp = False
        self.ebit = False
        self.label = None
        self.prio_boost = 0
        self._parse()

    def __repr__(self):
        return self.text

    def _parse(self):
        t = self.text
        if t.upper().startswith('NOP'):
            self.mnem, self.ops = 'nop', []
            if '[E]' in t.upper():
                self.ebit = True
            return
        m = re.match(r'(\S+)\s*(.*)$', t)
        mnem, rest = m.group(1), m.group(2)
        if '[E]' in mnem.upper():
            self.ebit = True
            mnem = mnem.replace('[E]', '').replace('[e]', '')
        self.mnem = mnem.lower()
        self.ops = [o.strip() for o in rest.split(',')] if rest.strip() else []
        (self._upper if self.pipe == 'U' else self._lower)()

    # ---- helpers
    @staticmethod
    def reg(op):
        """'VF10x' -> ('VF10', 'x'); 'VF16xyz' -> ('VF16', 'xyz'); '%pa:x' -> ('%pa', 'x'); 'ACC' -> ('ACC', '')"""
        op = op.strip()
        if op.startswith('%') or op.startswith('&'):
            if ':' in op:
                n, f = op.split(':')
                return n, f
            return op, ''
        m = re.match(r'(VF\d\d|VI\d\d|ACC|Q|I|R|P)([xyzw]*)$', op, re.I)
        if not m:
            raise ValueError('bad operand %r' % op)
        return m.group(1).upper(), m.group(2).lower()

    def r(self, reg, f):
        if reg in ('VF00', 'VI00', 'I', 'R'):
            return
        self.reads.append((reg, f or 'xyzw'))

    def w(self, reg, f):
        if reg in ('VF00', 'VI00'):
            return
        self.writes.append((reg, f or 'xyzw'))

    @staticmethod
    def isvi(reg):
        return reg.startswith('VI') or reg.startswith('&')

    def _upper(self):
        self._upper_ops()
        if self.flag_w and ('MAC' if self.flag_w == 'MAC' else 'CLIP', 'x') not in self.writes:
            self.writes.append((self.flag_w, 'x'))

    def _upper_ops(self):
        mn = self.mnem
        base, _, dest = mn.partition('.')
        dest = dest or 'xyzw'
        self.lat = LAT_FMAC
        for b in ('opmula', 'opmsub'):
            if base == b:
                d, s, tt = self.ops
                self.r(self.reg(s)[0], 'xyz'), self.r(self.reg(tt)[0], 'xyz')
                if b == 'opmsub':
                    self.r('ACC', 'xyz')
                self.w(self.reg(d)[0], 'xyz')
                self.flag_w = 'MAC'
                return
        m = re.match(r'(ftoi(0|4|12|15)|itof(0|4|12|15)|abs)$', base)
        if m:
            d, s = self.ops
            self.r(self.reg(s)[0], dest), self.w(self.reg(d)[0], dest)
            return
        if base.startswith('clip'):
            s, tt = self.ops
            self.r(self.reg(s)[0], 'xyz'), self.r(self.reg(tt)[0], 'w')
            self.r('CLIP', 'x')  # shifts the previous flags: clips stay in program order
            self.w('CLIP', 'x')
            self.flag_w = 'CLIP'
            return
        for b in ('msuba', 'madda', 'mula', 'adda', 'suba', 'msub', 'madd', 'mini', 'max', 'mul', 'add', 'sub'):
            if base.startswith(b):
                bc = base[len(b):]
                if bc not in ('', 'x', 'y', 'z', 'w', 'q', 'i'):
                    continue
                d, s, tt = self.ops
                self.r(self.reg(s)[0], dest)
                treg, tf = self.reg(tt)
                if (b.endswith('a')) != (d.upper() == 'ACC'):
                    raise ValueError('ACC form / operand mismatch: %r' % self.text)
                if tf and bc != tf:
                    raise ValueError('broadcast operand without matching mnemonic: %r' % self.text)
                if bc == 'q':
                    self.r('Q', 'x')
                elif bc == 'i':
                    pass
                elif bc:
                    self.r(treg, bc)
                else:
                    self.r(treg, dest)
                if b in ('msuba', 'madda', 'msub', 'madd'):
                    self.r('ACC', dest)
                self.w(self.reg(d)[0], dest)
                if b not in ('mini', 'max'):
                    self.flag_w = 'MAC'
                return
        raise ValueError('unknown upper op %r' % self.text)

    def _lower(self):
        mn = self.mnem
        base, _, dest = mn.partition('.')
        dest = dest or 'xyzw'
        ops = self.ops

        def addr(a):
            m = re.match(r'(.*)\((.+)\)$', a)
            off = eval(m.group(1) or '0', {}, {})
            return off, self.reg(m.group(2))[0]

        if base == 'lq':
            off, b = addr(ops[1])
            self.r(b, 'x'), self.w(self.reg(ops[0])[0], dest)
            self.lat, self.mem_r = LAT_FMAC, (b, off)
        elif base == 'sq':
            off, b = addr(ops[1])
            self.r(self.reg(ops[0])[0], dest), self.r(b, 'x')
            self.mem_w = (b, off)
        elif base == 'ilw':
            off, b = addr(ops[1])
            self.r(b, 'x'), self.w(self.reg(ops[0])[0], 'x')
            self.lat, self.mem_r = 4, (b, off)
        elif base == 'isw':
            off, b = addr(ops[1])
            self.r(self.reg(ops[0])[0], 'x'), self.r(b, 'x')
            self.mem_w = (b, off)
        elif base in ('iaddiu', 'isubiu', 'iaddi'):
            self.r(self.reg(ops[1])[0], 'x'), self.w(self.reg(ops[0])[0], 'x')
        elif base in ('iadd', 'isub', 'iand', 'ior'):
            self.r(self.reg(ops[1])[0], 'x'), self.r(self.reg(ops[2])[0], 'x'), self.w(self.reg(ops[0])[0], 'x')
        elif base == 'mtir':
            rr, f = self.reg(ops[1])
            self.r(rr, f), self.w(self.reg(ops[0])[0], 'x')
        elif base == 'mfir':
            self.r(self.reg(ops[1])[0], 'x'), self.w(self.reg(ops[0])[0], dest)
            self.lat = LAT_FMAC
        elif base in ('move', 'mr32'):
            self.r(self.reg(ops[1])[0], dest if base == 'move' else 'xyzw'), self.w(self.reg(ops[0])[0], dest)
            self.lat = LAT_FMAC
        elif base in ('div', 'rsqrt', 'sqrt'):
            for o in ops[1:]:
                rr, f = self.reg(o)
                self.r(rr, f)
            self.w('Q', 'x')
            self.lat = LAT_RSQRT if base == 'rsqrt' else LAT_DIV
            self.div = True
        elif base in LAT_EFU:  # EFU: result in P after the latency; the unit is busy until then
            for o in ops[1:]:
                rr, f = self.reg(o)
                self.r(rr, f or ('xyz' if base in ('erleng', 'eleng', 'esum') else 'x'))
            self.w('P', 'x')
            self.lat = LAT_EFU[base]
            self.efu = True
        elif base == 'mfp':
            self.r('P', 'x'), self.w(self.reg(ops[0])[0], dest)
            self.lat = LAT_FMAC
        elif base == 'waitp':
            self.r('P', 'x')
            self.waitp = True
        elif base == 'waitq':
            self.r('Q', 'x')
            self.waitq = True
        elif base in ('fcand', 'fcor', 'fceq'):
            self.r('CLIP', 'x'), self.w(self.reg(ops[0])[0], 'x')
            self.flag_r = 'CLIP'
        elif base == 'fcget':
            self.r('CLIP', 'x'), self.w(self.reg(ops[0])[0], 'x')
            self.flag_r = 'CLIP'
        elif base == 'fcset':
            self.w('CLIP', 'x')
            self.flag_w = 'CLIP'
        elif base in ('fmand', 'fmor', 'fmeq'):
            self.r('MAC', 'x'), self.r(self.reg(ops[1])[0], 'x'), self.w(self.reg(ops[0])[0], 'x')
            self.flag_r = 'MAC'
        elif base in ('xtop', 'xitop'):
            self.w(self.reg(ops[0])[0], 'x')
        elif base == 'xgkick':
            self.r(self.reg(ops[0])[0], 'x')
            self.xgkick = True
        elif base in ('b', 'bal'):
            self.branch, self.label = True, ops[-1]
            if base == 'bal':
                self.w(self.reg(ops[0])[0], 'x')
        elif base in ('jr', 'jalr'):
            self.branch = True
            self.r(self.reg(ops[-1])[0], 'x')
            if base == 'jalr':
                self.w(self.reg(ops[0])[0], 'x')
        elif base in ('ibne', 'ibeq'):
            self.branch, self.label = True, ops[2]
            self.r(self.reg(ops[0])[0], 'x'), self.r(self.reg(ops[1])[0], 'x')
        elif base in ('ibltz', 'ibgez', 'ibgtz', 'iblez'):
            self.branch, self.label = True, ops[1]
            self.r(self.reg(ops[0])[0], 'x')
        elif base == 'nop':
            pass
        else:
            raise ValueError('unknown lower op %r' % self.text)


def overlap(a, b):
    return any(c in b for c in a)


# ---------------------------------------------------------------- .vsm parsing (macros, labels) and cycle counting
def expand(text):
    """Expand .macro / .if / .else / .endif; returns a flat list of (kind, payload): ('label', name), ('pair', (u, l))."""
    lines = [l.split(';', 1)[0].rstrip() for l in text.splitlines()]
    macros = {}
    out = []

    def run(lines, env):
        i = 0
        stack = [True]
        while i < len(lines):
            l = lines[i]
            for k, v in env.items():
                l = l.replace('\\' + k, v)
            s = l.strip()
            i += 1
            if not s:
                continue
            tok = s.split()
            if tok[0] == '.macro':
                name = tok[1].rstrip(',')
                args = [a.strip() for a in s[len('.macro'):].strip()[len(name):].split(',') if a.strip()]
                body = []
                while lines[i].strip() != '.endm':
                    body.append(lines[i])
                    i += 1
                i += 1
                macros[name] = (args, body)
                continue
            if tok[0] == '.if':
                stack.append(stack[-1] and bool(eval(s[3:].strip(), {}, {})))
                continue
            if tok[0] == '.else':
                stack[-1] = (not stack[-1]) and stack[-2]
                continue
            if tok[0] == '.endif':
                stack.pop()
                continue
            if not stack[-1]:
                continue
            if s.startswith('.'):
                continue
            m = re.match(r'^([A-Za-z_0-9]+):\s*(.*)$', s)
            if m:
                out.append(('label', m.group(1)))
                s = m.group(2).strip()
                if not s:
                    continue
                tok = s.split()
            if tok[0] in macros:
                args, body = macros[tok[0]]
                vals = [v.strip() for v in s[len(tok[0]):].split(',')] if len(tok) > 1 else []
                run(body, dict(zip(args, vals)))
                continue
            out.append(('pair', split_pair(s)))

    run(lines, {})
    return out


def split_pair(s):
    tok = s.split()
    u = tok[0]
    k = 1
    if not u.upper().startswith('NOP'):
        u += ' ' + tok[1]
        k = 2
    l = ' '.join(tok[k:])
    lm = l.split()[0].lower() if l else 'nop'
    return u, l


def parse_program(text):
    """-> (pairs [(Ins U, Ins L)], labels {name: index}); numeric local labels resolved for '1b' / '1f' refs."""
    items = expand(text)
    pairs, labels, local = [], {}, []
    for kind, p in items:
        if kind == 'label':
            labels[p] = len(pairs)
            if p.isdigit():
                local.append((p, len(pairs)))
            continue
        u, l = p
        pairs.append((Ins('U', u), Ins('L', l or 'nop')))
    for idx, (u, l) in enumerate(pairs):
        if l.label and re.match(r'^\d+[bf]$', l.label):
            n, d = l.label[:-1], l.label[-1]
            cands = [i for name, i in local if name == n]
            l.target = max(i for i in cands if i <= idx) if d == 'b' else min(i for i in cands if i > idx)
        elif l.label:
            l.target = labels[l.label]
    return pairs, labels


class Timer:
    """In-order issue with the stall rules above; returns issue cycles."""

    def __init__(self):
        self.ready = {}      # (reg, field) -> cycle
        self.cycle = -1
        self.div_busy = 0
        self.efu_busy = 0
        self.stalls = 0

    def issue(self, u, l):
        t = self.cycle + 1
        need = t
        for ins in (u, l):
            for reg, f in ins.reads:
                if reg in ('MAC', 'CLIP'):
                    continue
                if reg == 'Q' and not ins.waitq:
                    continue
                for c in f:
                    need = max(need, self.ready.get((reg, c), 0))
            if ins.div:
                need = max(need, self.div_busy)
            if ins.efu:
                need = max(need, self.efu_busy)
        self.stalls += need - t
        t = need
        for ins in (u, l):
            for reg, f in ins.writes:
                lat = ins.lat
                if reg == 'ACC':
                    lat = 1
                if self.isvi(reg) and ins.mnem.split('.')[0] not in ('ilw',):
                    lat = 1
                for c in f:
                    self.ready[(reg, c)] = t + lat
            if ins.div:
                self.div_busy = t + ins.lat
            if ins.efu:
                self.efu_busy = t + ins.lat
        self.cycle = t
        return t

    isvi = staticmethod(Ins.isvi)


def count(pairs, start, stop_at=None, take_branch=lambda i, n: True, max_pairs=100000):
    """Run from pair index start following branches (take_branch(idx, times) decides conditional ones) until an
    E-bit pair + its delay pair, or stop_at; returns (cycles, trace [(idx, cycle)])."""
    tm = Timer()
    i, trace, times = start, [], {}
    pending = None
    end_after = None
    while len(trace) < max_pairs:
        u, l = pairs[i]
        c = tm.issue(u, l)
        trace.append((i, c))
        if end_after is not None:
            break
        nxt = i + 1
        if pending is not None:
            nxt, pending = pending, None
        if l.branch:
            times[i] = times.get(i, 0) + 1
            uncond = l.mnem in ('b', 'bal')
            if uncond or take_branch(i, times[i]):
                pending = l.target
        if u.ebit:
            end_after = True
        if stop_at is not None and nxt == stop_at:
            trace.append((nxt, tm.cycle + 1))
            break
        i = nxt
    return tm.cycle + 1, trace, tm.stalls


# ---------------------------------------------------------------- list scheduler + register allocation
class Block:
    """Straight-line code in program order -> scheduled pairs. Virtual registers %x (VF) / &x (VI) are allocated
    afterwards from the given pools."""

    def __init__(self, code, vf_pool, vi_pool):
        self.ins = []
        for line in code:
            line = line.strip()
            if not line or line.startswith(';'):
                continue
            pipe, text = line.split(None, 1)
            self.ins.append(Ins(pipe, text))
        self.vf_pool, self.vi_pool = list(vf_pool), list(vi_pool)

    def acc_chains(self):
        """ACC accumulate chains: from an op that writes ACC without reading it to the op that reads ACC into a VF
        register. The scheduler may reorder whole chains but never interleaves two."""
        self.chain = [None] * len(self.ins)
        cur, nid = None, 0
        for i, x in enumerate(self.ins):
            r = any(reg == 'ACC' for reg, f in x.reads)
            w = any(reg == 'ACC' for reg, f in x.writes)
            if not (r or w):
                continue
            if w and not r:
                cur, nid = nid, nid + 1
            self.chain[i] = cur
            if r and not w:
                cur = None

    def deps(self):
        self.acc_chains()
        n = len(self.ins)
        preds = [dict() for _ in range(n)]   # j -> min distance (issue_i - issue_j >= d)
        last_w = {}       # (reg, f) -> j
        reads_since = {}  # (reg, f) -> [j] readers since last write
        last_mem_w, last_mem_r = {}, {}
        last_efu = {}

        def add(i, j, d):
            if j is None or j == i:
                return
            preds[i][j] = max(preds[i].get(j, -99), d)

        self.flag_src = {}
        for i, ins in enumerate(self.ins):
            if ins.flag_r:
                self.flag_src[i] = last_w.get((ins.flag_r, 'x'))
            for reg, f in ins.reads:
                for c in f:
                    j = last_w.get((reg, c))
                    if j is None:
                        continue
                    src = self.ins[j]
                    if reg == 'ACC' and self.chain[i] != self.chain[j]:
                        continue  # other chain: ordered by the scheduler's ACC lock
                    if reg == 'ACC':
                        d = 1
                    elif reg in ('MAC', 'CLIP'):
                        d = 1 if ins.flag_w == reg else 4
                    elif reg == 'Q':
                        d = src.lat if not ins.waitq else src.lat
                    elif Ins.isvi(reg):
                        d = 4 if src.mnem.startswith('ilw') else 1
                        if ins.branch:
                            d = max(d, 2)
                    else:
                        d = src.lat
                    add(i, j, d)
            for reg, f in ins.writes:
                for c in f:
                    if reg in ('MAC', 'CLIP'):
                        continue  # flag anti-deps: checked in verify()
                    for j in reads_since.get((reg, c), []):
                        if reg == 'ACC' and self.chain[i] != self.chain[j]:
                            continue
                        add(i, j, 1 if reg != 'Q' else 1)
                    j = last_w.get((reg, c))
                    if j is not None and not (reg == 'ACC' and self.chain[i] != self.chain[j]):
                        lj = 1 if (reg == 'ACC' or (Ins.isvi(reg) and not self.ins[j].mnem.startswith('ilw'))) \
                            else self.ins[j].lat
                        li = 1 if (reg == 'ACC' or (Ins.isvi(reg) and not ins.mnem.startswith('ilw'))) else ins.lat
                        add(i, j, max(1, lj - li + 1))
            if ins.efu:  # one EFU op at a time
                add(i, last_efu.get('e'), self.ins[last_efu['e']].lat if 'e' in last_efu else 0)
                last_efu['e'] = i
            if ins.xgkick:  # the GIF reads what the program stored: after every store so far
                for j in range(i):
                    if self.ins[j].mem_w:
                        add(i, j, 2)
            if ins.mem_r:
                add(i, last_mem_w.get(ins.mem_r), 1)
                last_mem_r.setdefault(ins.mem_r, []).append(i)
            if ins.mem_w:
                add(i, last_mem_w.get(ins.mem_w), 1)
                for j in last_mem_r.get(ins.mem_w, []):
                    add(i, j, 1)
                last_mem_w[ins.mem_w] = i
                last_mem_r[ins.mem_w] = []
            # waitq / xgkick / branches keep program order with everything that touches the same resource only
            for reg, f in ins.reads:
                for c in f:
                    reads_since.setdefault((reg, c), []).append(i)
            for reg, f in ins.writes:
                for c in f:
                    last_w[(reg, c)] = i
                    reads_since[(reg, c)] = []
            # Q readers must not overtake a later div: handled by WAR on Q above (div writes Q)
        return preds

    def schedule(self, limit_vf=None, limit_vi=None, reserve=0, window=10 ** 9):
        """List scheduling by latency-weighted height. Register pressure: an instruction that starts a new virtual
        value may use the last `reserve` registers of a pool only if it is within `window` of the oldest
        unscheduled instruction (program order), so work far ahead cannot starve the oldest vertex."""
        ins = self.ins
        n = len(ins)
        preds = self.deps()
        succs = [dict() for _ in range(n)]
        for i in range(n):
            for j, d in preds[i].items():
                succs[j][i] = d
        # height: longest path to the end (latency-weighted)
        height = [0] * n
        for i in reversed(range(n)):
            h = 1
            for k, d in succs[i].items():
                h = max(h, d + height[k])
            height[i] = h + ins[i].prio_boost
        for i in range(n):
            if ins[i].flag_r:
                height[i] += 10000
        # virtual value bookkeeping for register pressure
        vreaders = {}
        for i, x in enumerate(ins):
            for reg, f in x.reads:
                if reg[0] in '%&':
                    vreaders.setdefault(reg, set()).add(i)
        limit = {'%': limit_vf or len(self.vf_pool), '&': limit_vi or len(self.vi_pool)}
        members, ext = {}, {}
        for i, c in enumerate(self.chain):
            if c is not None:
                members.setdefault(c, []).append(i)
        for c, ms in members.items():
            ext[c] = {j for i in ms for j in preds[i] if self.chain[j] != c}
        acc_owner = None
        live = {'%': set(), '&': set()}
        done_readers = {}
        issue = [None] * n
        cycle = 0
        remaining = set(range(n))
        while remaining:
            frees = []
            for pipe in ('L', 'U'):
                best = None
                for i in remaining:
                    x = ins[i]
                    if x.pipe != pipe:
                        continue
                    ok = True
                    for j, d in preds[i].items():
                        if issue[j] is None or issue[j] + d > cycle:
                            ok = False
                            break
                        if issue[j] == cycle and d <= 0 and ins[j].pipe == pipe:
                            ok = False
                            break
                    if not ok:
                        continue
                    c = self.chain[i]
                    if c is not None and acc_owner != c:
                        if acc_owner is not None or i != members[c][0] or \
                                any(issue[j] is None for j in ext[c]):
                            continue
                    # register pressure
                    newdefs = {'%': set(), '&': set()}
                    for reg, f in x.writes:
                        if reg[0] in '%&' and reg not in live[reg[0]]:
                            newdefs[reg[0]].add(reg)
                    over = False
                    oldest = min(remaining)
                    for k in '%&':
                        if newdefs[k]:
                            need = len(live[k]) + len(newdefs[k])
                            if need > limit[k] or (need > limit[k] - reserve and i > oldest + window and
                                                   not (c is not None and acc_owner == c)):
                                over = True
                    if over:
                        continue
                    if best is None or height[i] > height[best] or (height[i] == height[best] and i < best):
                        best = i
                if best is not None:
                    issue[best] = cycle
                    remaining.discard(best)
                    x = ins[best]
                    c = self.chain[best]
                    if c is not None:
                        acc_owner = None if best == members[c][-1] else c
                    for reg, f in x.writes:
                        if reg[0] in '%&':
                            live[reg[0]].add(reg)
                    for reg, f in x.reads:
                        if reg[0] in '%&':
                            dr = done_readers.setdefault(reg, set())
                            dr.add(best)
                            if dr >= vreaders[reg]:
                                frees.append(reg)
            for reg in frees:
                live[reg[0]].discard(reg)
            cycle += 1
            if cycle > 30 * n + 500:
                raise RuntimeError('scheduler stuck: live %r' % (live,))
        self.issue = issue
        self.length = max(issue) + 1
        return issue

    def check_flags(self):
        """Each flag reader must see its source-order writer: issued >= 4 cycles before it, and no other writer of
        that flag issued between the two."""
        bad = []
        for r, w in self.flag_src.items():
            if w is None:
                continue
            c, kind = self.issue[r], self.ins[r].flag_r
            if self.issue[w] > c - 4:
                bad.append('%s: writer %s only %d cycles before' % (self.ins[r], self.ins[w], c - self.issue[w]))
            for x, ins in enumerate(self.ins):
                if x != w and ins.flag_w == kind and self.issue[w] < self.issue[x] <= c - 4:
                    bad.append('%s: %s overwrites %s flags of %s' % (self.ins[r], ins, kind, self.ins[w]))
        return bad

    def allocate(self, fixed_last_use=None):
        """Interval colouring of the virtual registers over the schedule; renames in place."""
        ins, issue = self.ins, self.issue
        iv = {}
        for i, x in enumerate(ins):
            for reg, f in x.writes + x.reads:
                if reg[0] in '%&':
                    a, b = iv.get(reg, (issue[i], issue[i]))
                    iv[reg] = (min(a, issue[i]), max(b, issue[i]))
        names = {}
        for kind, pool in (('%', self.vf_pool), ('&', self.vi_pool)):
            regs = sorted((r for r in iv if r[0] == kind), key=lambda r: iv[r])
            free_at = {p: -1 for p in pool}   # phys -> last use cycle
            for r in regs:
                a, b = iv[r]
                cand = [p for p in pool if free_at[p] < a]
                if not cand:
                    raise RuntimeError('out of %s registers at cycle %d (%s)' % (kind, a, r))
                p = min(cand, key=lambda p: (free_at[p], p))  # reuse the longest-free register
                names[r] = p
                free_at[p] = b
        self.names = names

        def ren(text):
            def sub(m):
                name, f = m.group(1), m.group(2)
                return names[name] + (f or '')
            return re.sub(r'([%&][A-Za-z_0-9]+)(?::([xyzw]+))?', sub, text)

        for x in ins:
            x.text = ren(x.text)
        return names

    def pairs(self, branch=None, label=None):
        """Scheduled pairs as text; branch: lower op appended so that it issues in the second-last pair (with its
        delay slot the last). Returns list of (u, l) strings."""
        L = self.length
        slots = [['NOP', 'NOP'] for _ in range(L)]
        for i, x in enumerate(self.ins):
            s = slots[self.issue[i]]
            k = 0 if x.pipe == 'U' else 1
            assert s[k] in ('NOP',), 'slot clash'
            s[k] = x.text
        if branch:
            # place in the last free lower slot b with b >= L - 2 (delay slot = b + 1), at least 2 cycles after the
            # last write of a VI the branch reads (hardware: a VI written by the pair right before a branch is read
            # old, e.g. a loop counter decremented there runs the loop once more)
            br_vi = {r for r, _ in Ins('L', branch).reads if r.startswith('VI')}
            last_w = max([self.issue[i] for i, x in enumerate(self.ins)
                          if any(r in br_vi for r, _ in x.writes)], default=-9)
            b = max(L - 2, last_w + 2, 0)
            while b < len(slots) and slots[b][1] != 'NOP':
                b += 1
            while len(slots) < b + 2:
                slots.append(['NOP', 'NOP'])
            slots[b][1] = branch
        return slots


def verify(pairs_text, loop=True, iters=3):
    """Check flag timing and stall-freedom of a scheduled body (text pairs) by running it iters times back to back.
    Returns (cycles per iteration, stalls, problems)."""
    seq = []
    for _ in range(iters if loop else 1):
        seq += [(Ins('U', u), Ins('L', l)) for u, l in pairs_text]
    tm = Timer()
    cyc = []
    for u, l in seq:
        cyc.append(tm.issue(u, l))
    problems = []
    n = len(pairs_text)
    per = (cyc[-1] - cyc[n - 1]) / (iters - 1) if loop and iters > 1 else cyc[-1] + 1
    return per, tm.stalls, problems
