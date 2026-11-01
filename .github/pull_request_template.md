## What

<!-- What changed and why. One logical change per PR (AGENTS.md section 7). -->

## Verification

<!-- AGENTS.md section 4. The GitHub checks cover hygiene, the VU1 tests and a data-free compile; everything below
needs the game data and PCSX2, so it is run on a PC and reported here. -->

- [ ] Plain build boots: `make -C ps2/proto` (and `MAP=m10_01_00_00` when area code changed)
- [ ] Headless run: `tools/pcsx2-run.sh <elf> <seconds> <shot.png>` or the relevant `ps2/proto/tests/*.txt` script
- [ ] `tools/test-all.sh` step summary pasted below (render-base is informational until the baseline is re-accepted)
- [ ] Budgets hold: 30 fps, at least 1.5 MB free EE RAM in the heaviest spot, no "Not enough VRAM" in the log

| Spot / test | fps min | frame ms max | free EE RAM min | before → after |
|---|---|---|---|---|
|  |  |  |  |  |

<!-- Say when a gain can only be confirmed on real hardware (PCSX2 doesn't time VU1, GS fill or the EE cache). -->

## TECH_DEBT

<!-- IDs opened or resolved in notes/TECH_DEBT.md, or "none". -->

## Checklist

- [ ] No game data, game-derived files, screenshots or personal paths (AGENTS.md section 1)
- [ ] Values from game data or the exe cite their source; guesses are marked `ASSUMED` and logged in TECH_DEBT
- [ ] Ran on at least one area besides Firelink, if the change touches exporters or area-dependent code
- [ ] Generated files regenerated, not hand-edited (`tools/vu1/gen.py` for `*.vsm`)
- [ ] AI-assisted commits carry a `Co-Authored-By:` trailer
