# The Lab (Polaris 1.7b)

The player's simulate phase built as a 32-bit host program, driven from Python over a pipe (`tools/lab.py`), so a
gameplay rule or a recorded input trace runs in seconds without PCSX2.

    distrobox-host-exec make -C ps2/proto lab          # build/host/lab (needs a host gcc with -m32)
    python3 tools/lab.py compare build/save/NAME.trace build/save/NAME.trace.state --short

Record a trace on PCSX2 with the test options `fixeddt`, `seed` and `record NAME` (src/frame.h; tests/det_walk*.txt).

What is in it: game data, flags, progress, character stats, items, the player (ESD, combat, physics, status), whole-area
collision (`collision.bin` of an area pack, `COL_TILES=0`), the navmesh, the player's model and animations (no VU1
packets: their layout only). Packs are read from `build/data` with stdio (lab_pack.c).

What is not, yet (Polaris 1.7c): enemies and their AI, events (EMEVD: fog walls, warps), objects (breakables: a pot in
the way blocks the console's player and not the Lab's), sound, the test-script runner (its commands reach enemies,
events and menus). World blocks keep their collision in streamed 16 m tiles, which the Lab does not load: use an area
pack (Firelink's `m10_02_00_00`). The Lab therefore cannot reproduce a fall through a tile that had not loaded.

32-bit because the loaders keep pointers in 32-bit fields of the loaded blobs (TECH_DEBT core-1); when formats move to
chunk streams with base-plus-offset accessors (Polaris phase 3) the Lab can go 64-bit and load into Python directly.

Floating point: SSE scalar math, no fast-math, no contraction (never x87). sinf / cosf / atan2f come from the host's
libm, not the EE's newlib; the first traces agree within 0.04 cm, so a shared implementation waits until a divergence
needs it.

Stubs (lab_stubs.c) count their calls (`stubs` command, and in `lab.py compare`): a simulate module reaching a render,
sound or streaming call shows up there as a layer violation of the Polaris 0.4 boundary map.
