## What

<!-- What changed and why. One logical change per PR (AGENTS.md section 7). -->

## Verification

<!-- AGENTS.md section 4. The GitHub checks cover hygiene, the VU1 tests and a data-free compile. The PCSX2 checks below
need the game data, so they run on a PC and are reported here. Scale the run to the change: a full all-area
tools/test-all.sh is for releases / public exports or a nightly run, not for every PR. -->

Every PR:
- [ ] Incremental build boots: `make -C ps2/proto` (plus `MAP=<area>` for each area the change touches); exports stay cached
- [ ] Scoped smoke run: `tools/test-all.sh --no-build --areas <touched areas> --max 20`, step summary pasted below
- [ ] Budgets hold at the spots tested: 30 fps, at least 1.5 MB free EE RAM, no "Not enough VRAM" in the log

Only when the change calls for it (tick or delete):
- [ ] Streaming / world / enemy code: `ps2/proto/tests/world_*.txt` through `tools/test-run.py`
- [ ] Exporter or file-format change: `make` re-runs the changed exporters (they are Makefile dependencies) for the touched areas and one other area (AGENTS.md section 3), then the scoped run
- [ ] Rendering change: render-last diff reviewed (render-base is informational until the baseline is re-accepted with `--accept`)

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
