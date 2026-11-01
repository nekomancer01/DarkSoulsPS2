#!/usr/bin/env python3
"""PCSX2 PINE client: read / write EE memory by ELF symbol, savestates, and the test mailbox of src/testscript.c.

PINE (https://github.com/GovanifY/pine, PCSX2 pcsx2/PINE.cpp) is a Unix socket: $XDG_RUNTIME_DIR/pcsx2.sock.<slot>.
tools/pcsx2-run.sh (PINE=<slot>) starts the headless flatpak with XDG_RUNTIME_DIR = /run/user/<uid>/app/net.pcsx2.PCSX2,
the per-app runtime dir that flatpak shares with the host, so the socket is reachable from here. No pad input in PINE.

  pine.py [--slot N] [--elf proto.elf] status | version
  pine.py read <symbol|0xaddr> [bytes]          hex dump (default: the symbol's size, else 16)
  pine.py read32 / write32 <symbol|0xaddr>[+off] [value]
  pine.py save <slot> | load <slot>             savestate slot (PCSX2 sstates folder of the test instance)
  pine.py mailbox                               decode test_mb (src/testscript.h)
  pine.py watch out.csv [--hz 10]               log the mailbox to CSV until the script reports done / PCSX2 exits

Symbols come from the ELF's .symtab (unstripped build: `make EE_STRIP=true`, which tools/test-all.sh uses); a stripped
ELF falls back to the "TEST mailbox 0x..." line testscript.c prints to the EE log (--log)."""
import argparse, csv, os, re, socket, struct, sys, time

MSG_READ = {1: 0, 2: 1, 4: 2, 8: 3}
MSG_WRITE = {1: 4, 2: 5, 4: 6, 8: 7}
MSG_VERSION, MSG_SAVE, MSG_LOAD, MSG_STATUS = 8, 9, 0xA, 0xF
STATUS = {0: "running", 1: "paused", 2: "shutdown"}
DEFAULT_SLOT = 28011

# src/testscript.h TestMailbox, version 1
MAILBOX_FMT = "<6I32si4f5f2i5i5i2iI3f3i"
MAILBOX_FIELDS = ("magic version frame state shot_seq shot_ack shot_name line x y z yaw fps frame_ms map_ms chr_ms vuw_ms "
                  "ram_free vram_free chain_map chain_map_peak chain_chr chain_chr_peak segments tris vu1_batches ee_tris "
                  "enemies_active enemies_drawn hp deaths seg_frames seg_ms_avg seg_ms_max seg_map_max seg_ram_min "
                  "seg_chain_map_max seg_chain_chr_max").split()
MAILBOX_MAGIC = 0x4D545354
SHOT_ACK_OFF = 20
SHOT_BUSY = 0x80000000  # shot_ack | this: host is capturing, the ELF waits up to 60 s instead of 3 s


def sock_path(slot):
    run = f"/run/user/{os.getuid()}/app/net.pcsx2.PCSX2"
    return f"{run}/pcsx2.sock" + ("" if slot == DEFAULT_SLOT else f".{slot}")


class Pine:
    def __init__(self, slot=DEFAULT_SLOT, path=None, timeout=5.0):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.settimeout(timeout)
        self.s.connect(path or sock_path(slot))

    def close(self):
        self.s.close()

    def _recv(self, n):
        b = b""
        while len(b) < n:
            c = self.s.recv(n - len(b))
            if not c:
                raise ConnectionError("PINE socket closed")
            b += c
        return b

    def call(self, body):
        """One request (any number of concatenated commands); returns the reply payload after the status byte."""
        self.s.sendall(struct.pack("<I", len(body) + 4) + body)
        size = struct.unpack("<I", self._recv(4))[0]
        rep = self._recv(size - 4)
        if rep[0] != 0:
            raise IOError("PINE command failed")
        return rep[1:]

    def read(self, addr, size=4):
        r = self.call(struct.pack("<BI", MSG_READ[size], addr))
        return int.from_bytes(r[:size], "little")

    def write(self, addr, value, size=4):
        self.call(struct.pack("<BI", MSG_WRITE[size], addr) + int(value).to_bytes(size, "little"))

    def read_bytes(self, addr, n):
        """n bytes from addr: batched Read64 (8-byte aligned window), 4096 commands per request."""
        a0, a1 = addr & ~7, (addr + n + 7) & ~7
        out = b""
        for base in range(a0, a1, 8 * 4096):
            cnt = min(4096, (a1 - base) // 8)
            out += self.call(b"".join(struct.pack("<BI", 3, base + 8 * i) for i in range(cnt)))
        return out[addr - a0: addr - a0 + n]

    def version(self):
        r = self.call(bytes([MSG_VERSION]))
        return r[4:4 + struct.unpack_from("<I", r)[0]].rstrip(b"\0").decode()

    def status(self):
        return STATUS.get(struct.unpack("<I", self.call(bytes([MSG_STATUS])))[0], "?")

    def save_state(self, slot):
        self.call(bytes([MSG_SAVE, slot]))

    def load_state(self, slot):
        self.call(bytes([MSG_LOAD, slot]))


def elf_symbols(path):
    """{name: (addr, size)} from an ELF32 little-endian .symtab ({} when stripped)."""
    b = open(path, "rb").read()
    if b[:4] != b"\x7fELF" or b[4] != 1:
        raise ValueError(f"{path}: not an ELF32")
    shoff, = struct.unpack_from("<I", b, 0x20)
    shentsize, shnum, _ = struct.unpack_from("<HHH", b, 0x2E)
    secs = [struct.unpack_from("<10I", b, shoff + i * shentsize) for i in range(shnum)]
    out = {}
    for s in secs:
        if s[1] != 2:  # SHT_SYMTAB
            continue
        strtab = secs[s[6]]
        for o in range(s[4], s[4] + s[5], 16):
            name, value, size, info, _, _ = struct.unpack_from("<IIIBBH", b, o)
            if name and (info & 0xF) in (1, 2):  # OBJECT, FUNC
                n = b[strtab[4] + name: b.index(b"\0", strtab[4] + name)].decode()
                out[n] = (value, size)
    return out


def log_mailbox(log):
    """Address of test_mb from the newest "TEST mailbox 0x..." line of an EE log (stripped ELFs)."""
    try:
        m = re.findall(r"TEST mailbox 0x([0-9a-f]+)", open(log, errors="replace").read())
    except OSError:
        return None
    return int(m[-1], 16) if m else None


def resolve(spec, syms, log=None):
    """'name', 'name+off', '0xaddr' -> (addr, size or None)"""
    base, _, off = spec.partition("+")
    off = int(off, 0) if off else 0
    if base.startswith("0x"):
        return int(base, 16) + off, None
    if base in syms:
        return syms[base][0] + off, syms[base][1]
    if base == "test_mb" and log and log_mailbox(log) is not None:
        return log_mailbox(log) + off, struct.calcsize(MAILBOX_FMT)
    raise KeyError(f"symbol {base} not found (stripped ELF? build with `make EE_STRIP=true`)")


def read_mailbox(p, addr):
    v = struct.unpack(MAILBOX_FMT, p.read_bytes(addr, struct.calcsize(MAILBOX_FMT)))
    d = dict(zip(MAILBOX_FIELDS, v))
    d["shot_name"] = d["shot_name"].split(b"\0")[0].decode(errors="replace")
    if d["magic"] != MAILBOX_MAGIC:
        raise ValueError(f"no test mailbox at 0x{addr:08x} (magic 0x{d['magic']:08x})")
    return d


def ack_shot(p, addr, seq):
    p.write(addr + SHOT_ACK_OFF, seq)


def connect(slot, wait=0.0):
    t0 = time.time()
    while True:
        try:
            return Pine(slot)
        except (FileNotFoundError, ConnectionRefusedError):
            if time.time() - t0 >= wait:
                raise
            time.sleep(0.2)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--slot", type=int, default=int(os.environ.get("PINE", DEFAULT_SLOT)))
    ap.add_argument("--elf", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "../ps2/proto/proto.elf"))
    ap.add_argument("--log", help="EE log with the 'TEST mailbox' line (stripped ELFs)")
    ap.add_argument("cmd")
    ap.add_argument("args", nargs="*")
    ap.add_argument("--hz", type=float, default=10)
    a = ap.parse_args()
    p = connect(a.slot)
    syms = elf_symbols(a.elf) if os.path.exists(a.elf) else {}
    if a.cmd == "status":
        print(p.status())
    elif a.cmd == "version":
        print(p.version())
    elif a.cmd == "read":
        addr, size = resolve(a.args[0], syms, a.log)
        n = int(a.args[1], 0) if len(a.args) > 1 else size or 16
        data = p.read_bytes(addr, n)
        for o in range(0, n, 16):
            print(f"{addr + o:08x}  {data[o:o + 16].hex(' ')}")
    elif a.cmd in ("read32", "write32"):
        addr, _ = resolve(a.args[0], syms, a.log)
        if a.cmd == "write32":
            p.write(addr, int(a.args[1], 0))
        print(f"{addr:08x} = 0x{p.read(addr):08x}")
    elif a.cmd in ("save", "load"):
        (p.save_state if a.cmd == "save" else p.load_state)(int(a.args[0]))
    elif a.cmd == "mailbox":
        addr, _ = resolve("test_mb", syms, a.log)
        for k, v in read_mailbox(p, addr).items():
            print(f"{k:18} {v:.3f}" if isinstance(v, float) else f"{k:18} {v}")
    elif a.cmd == "watch":
        addr, _ = resolve("test_mb", syms, a.log)
        with open(a.args[0], "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["t"] + MAILBOX_FIELDS)
            t0, last = time.time(), -1
            while True:
                try:
                    d = read_mailbox(p, addr)
                except (ConnectionError, OSError):
                    break
                if d["frame"] != last:
                    w.writerow([f"{time.time() - t0:.2f}"] + [d[k] for k in MAILBOX_FIELDS])
                    last = d["frame"]
                if d["state"] == 2:
                    break
                time.sleep(1 / a.hz)
    else:
        sys.exit(f"unknown command {a.cmd}")


if __name__ == "__main__":
    main()
