#!/bin/bash
# Usage: [NOSTRIP=1] ps2-iso.sh <proto.elf> <data dir> <out.iso> [boot id=DSRP_000.01]   (NOSTRIP keeps .symtab)
# Bootable PS2 disc image: SYSTEM.CNF + the ELF under its boot id + every pack under DATA\, ISO9660
# level 1 (upper case 8.3 names with ;1, what the BIOS CDVD driver reads). Pack names follow pack.c pack_path():
# '_' dropped, upper case, names cut to their last 8 chars ("data/m10_01_00_00/c1000.pak" -> DATA/10010000/C1000.PAK).
# Sector order (xorriso sort_weight, TECH_DEBT world-6): SYSTEM.CNF + ELF first, then the boot packs (menu, world,
# common, player), then the world blocks of data/world.pak in neighbour order from the boot block (breadth first over
# its neighbour masks), each block's pack followed by its character packs (top-level c*.pak for the boot block,
# data/<block>/ for the others), then every other area pack with its directory. Without world.pak: the boot packs,
# then each area pack with its directory, by name. Boots in PCSX2 (tools/ps2-pcsx2.sh iso) and in OPL / on a burnt disc with a modchip (untested).
# xorriso: XORRISO, else the one on PATH, else a user-local install in ~/tools/xorriso.
set -e
ELF=$(realpath "$1"); DATA=$(realpath "$2"); OUT=$(realpath -m "$3"); ID=${4:-DSRP_000.01}
XORRISO=${XORRISO:-$(command -v xorriso || echo "$HOME/tools/xorriso/bin/xorriso")}
STRIP=${STRIP:-mips64r5900el-ps2-elf-strip}
[ -f "$ELF" ] && [ -d "$DATA" ] || { echo "usage: $0 <elf> <data dir> <out.iso> [boot id]" >&2; exit 1; }
[[ $ID =~ ^[A-Z]{4}_[0-9]{3}\.[0-9]{2}$ ]] || { echo "boot id must look like ABCD_123.45" >&2; exit 1; }
STAGE=$(mktemp -d "${TMPDIR:-/tmp}/ps2iso.XXXXXX"); trap 'rm -rf "$STAGE"' EXIT
iso_name() {  # one path component -> ISO9660 level 1 name (same rule as pack.c)
    local b=${1%%.*} e=; [[ $1 == *.* ]] && e=.${1#*.}
    b=$(tr -d _ <<<"$b" | tr a-z A-Z); e=$(tr a-z A-Z <<<"$e")
    [ ${#b} -gt 8 ] && b=${b: -8}
    printf '%s%s' "$b" "${e:0:4}"
}
printf 'BOOT2 = cdrom0:\\%s;1\r\nVER = 1.00\r\nVMODE = NTSC\r\n' "$ID" > "$STAGE/SYSTEM.CNF"
if [ -n "$NOSTRIP" ]; then cp "$ELF" "$STAGE/$ID"; else "$STRIP" --strip-all -o "$STAGE/$ID" "$ELF"; fi
# xorriso native mode: its mkisofs emulation turns Rock Ridge on and ignores -sort weights. Rock Ridge makes every
# directory record ~120 bytes, and the BIOS CDVD driver reads only a directory's first 2048-byte sector: with 15 packs
# DATA\ spilled into a second sector and DATA\MENU.PAK could not be opened (seen in PCSX2, SCPH-77001 BIOS).
WEIGHTS=(-find /SYSTEM.CNF -exec sort_weight 100000 -- -find "/$ID" -exec sort_weight 99999 --)
mkdir -p "$STAGE/DATA"
w=90000
while IFS= read -r f; do
    rel=${f#$DATA/}; dst=DATA
    IFS=/ read -ra parts <<<"$rel"
    for p in "${parts[@]}"; do dst=$dst/$(iso_name "$p"); done
    [ -e "$STAGE/$dst" ] && { echo "ISO name clash: $rel -> $dst" >&2; exit 1; }
    mkdir -p "$(dirname "$STAGE/$dst")"; cp "$f" "$STAGE/$dst"
    WEIGHTS+=(-find "/$dst" -exec sort_weight $w --); w=$((w - 1))
    echo "  $rel -> $dst ($(stat -c %s "$f") B)"
done < <(python3 -I - "$DATA" <<'PY'
import os, struct, sys
data = sys.argv[1]
def lz4(src, n):  # LZ4 block format (tools/pack.py entries)
    out, i = bytearray(), 0
    while i < len(src):
        t = src[i]; i += 1
        l = t >> 4
        if l == 15:
            while True:
                b = src[i]; i += 1; l += b
                if b != 255: break
        out += src[i:i + l]; i += l
        if i >= len(src): break
        off = src[i] | src[i + 1] << 8; i += 2
        m = t & 15
        if m == 15:
            while True:
                b = src[i]; i += 1; m += b
                if b != 255: break
        for _ in range(m + 4): out.append(out[-off])
    return bytes(out[:n])
def world_blocks():  # data/world.pak (tools/export_world.py world.bin): [(pack name, neighbour mask)], boot first
    try:
        d = open(os.path.join(data, 'world.pak'), 'rb').read()
    except OSError:
        return []
    off, raw, _, blocks, bs = struct.unpack_from('<5I', d, 16 + 32)
    cs, p, w = struct.unpack_from(f'<{blocks}I', d, off), off + 4 * blocks, b''
    for c in cs:
        k = c & 0x7fffffff
        w += d[p:p + k] if c >> 31 else lz4(d[p:p + k], min(bs, raw - len(w))); p += k
    assert w[:4] == b'DSW1', 'world.pak: no world.bin'
    fmt = '<24s16s4f2I' if struct.unpack_from('<I', w, 4)[0] == 2 else '<16s16s6f2I'  # version 3: height range
    rows = [struct.unpack_from(fmt, w, 16 + 64 * i) for i in range(struct.unpack_from('<I', w, 8)[0])]
    return [(r[0].rstrip(b'\0').decode(), r[-2] & 1, r[-1]) for r in rows]
paks = sorted(os.path.relpath(os.path.join(r, f), data) for r, _, fs in os.walk(data) for f in fs if f.endswith('.pak'))
top = [p for p in paks if '/' not in p]
order = [p for p in ('menu.pak', 'world.pak', 'common.pak', 'c0000.pak') if p in top]
def area(name, chrs):
    order.extend(p for p in [name + '.pak'] + chrs if p in paks and p not in order)
blocks = world_blocks()
boot = next((i for i, b in enumerate(blocks) if b[1]), None)
if boot is not None:
    seen, queue = {boot}, [boot]
    while queue:
        i = queue.pop(0)
        name = blocks[i][0]
        area(name, [p for p in top if p[0] == 'c'] if i == boot else [p for p in paks if p.startswith(name + '/')])
        for k in range(len(blocks)):
            if blocks[i][2] >> k & 1 and k not in seen:
                seen.add(k), queue.append(k)
for p in paks:  # other areas (single-area builds' packs), each with its directory; then anything left
    if '/' not in p and p not in order:
        area(p[:-4], [q for q in paks if q.startswith(p[:-4] + '/')] if p[0] == 'm' else [])
order += [p for p in paks if p not in order]
print('\n'.join(os.path.join(data, p) for p in order))
PY
)
rm -f "$OUT"
"$XORRISO" -report_about WARNING -outdev "$OUT" -rockridge off -joliet off -compliance iso_9660_level=1 \
    -volid DSRPS2 -system_id PLAYSTATION -application_id "DSR PS2 PROTO" -map "$STAGE" / "${WEIGHTS[@]}" -commit \
    2>&1 | grep -v "^xorriso .* : RockRidge filesystem manipulator" | grep -v '^$' || true
# Check what the BIOS can read: every directory in one sector, file names with ;1
python3 -I - "$OUT" <<'PY'
import struct, sys
f = open(sys.argv[1], 'rb')
def sec(n, c=1):
    f.seek(n * 2048); return f.read(2048 * c)
def walk(lba, size, path):
    if size > 2048:
        sys.exit(f"ISO check: directory {path or '/'} is {size} B > one sector (the BIOS reads only the first)")
    d, i = sec(lba), 0
    while i < len(d) and d[i]:
        r = d[i:i + d[i]]; i += d[i]
        ext, sz, flags, name = struct.unpack('<I', r[2:6])[0], struct.unpack('<I', r[10:14])[0], r[25], r[33:33 + r[32]]
        if name in (b'\0', b'\1'): continue
        if flags & 2: walk(ext, sz, path + '/' + name.decode())
        elif not name.endswith(b';1'): sys.exit(f"ISO check: {path}/{name} has no ;1 version")
root = sec(16)[156:190]
walk(struct.unpack('<I', root[2:6])[0], struct.unpack('<I', root[10:14])[0], '')
print("ISO check: every directory fits one sector, names are ISO9660 level 1 with ;1")
PY
echo "ISO: $OUT ($(stat -c %s "$OUT") B), boot cdrom0:\\$ID;1"
