#!/bin/bash
# Usage: [NOSTRIP=1] ps2-iso.sh <proto.elf> <data dir> <out.iso> [boot id=DSRP_000.01]   (NOSTRIP keeps .symtab)
# Bootable PS2 disc image (tools-hwperf): SYSTEM.CNF + the ELF under its boot id + every pack under DATA\, ISO9660
# level 1 (upper case 8.3 names with ;1, what the BIOS CDVD driver reads). Pack names follow pack.c pack_path():
# '_' dropped, upper case, names cut to their last 8 chars ("data/m10_01_00_00/c1000.pak" -> DATA/10010000/C1000.PAK).
# Sector order (xorriso sort_weight): SYSTEM.CNF + ELF first, then the player / menu / area packs, then the rest, each
# area's packs contiguous. Boots in PCSX2 (tools/ps2-pcsx2.sh iso) and in OPL / on a burnt disc with a modchip (untested).
# xorriso: user-local in ~/tools/xorriso (Ubuntu xorriso 1.5.6 debs, see notes/threads/tools-hwperf.md).
set -e
ELF=$(realpath "$1"); DATA=$(realpath "$2"); OUT=$(realpath -m "$3"); ID=${4:-DSRP_000.01}
XORRISO=${XORRISO:-$HOME/tools/xorriso/bin/xorriso}
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
# order: top-level packs (player, menu, the default area) first, then each area directory together
done < <(find "$DATA" -maxdepth 1 -name '*.pak' | sort; find "$DATA" -mindepth 2 -name '*.pak' | sort)
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
