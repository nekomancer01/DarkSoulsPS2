# Contributing as an AI agent

This file is for AI coding agents (Claude Code, Codex, Cursor, Aider, …) working on this repository, and for the
humans running them. Human contributors should follow it too. Read it before your first edit.

The project ports Dark Souls Remastered (DSR) to the PlayStation 2: a C engine in `ps2/proto` (ps2dev + gsKit,
VU1 microprograms) fed by Python exporters in `tools/` that read the player's own copy of the game.

## 1. Legal: what may never enter the repository

These rules have no exceptions. A change that breaks one is rejected however good the rest of it is.

- **No game files, and nothing derived from them.** That covers textures, models, maps, collision, animations,
  sounds, fonts, params, scripts (ESD, EMEVD, Lua bytecode), the exe and dumps of it, and exported `.bin`/`.pak`
  files. It also covers C headers or tables generated from game data, and screenshots or videos of the game.
  Everything game-derived is generated at build time from the user's copy into the build/asset directory, which is
  git-ignored.
- **No copied decompiled code.** Describe the behaviour in your own words and implement it yourself. You may cite
  facts: function addresses, struct offsets, param field names, constants and formulas, each with its source (for
  example `FUN_140565600`, `NpcParam.turnVellocity`).
- **No personal data.** No home-directory paths, user names, account IDs, save files or machine-specific config.
  Use the game-path config (`DSR_GAME`, see README) and paths relative to the repository.
- **The reference game stays offline.** If you run DSR to compare visuals or behaviour, use a separate copy with
  its own prefix and save files, offline, with no mods ever going online. Never touch the user's real install,
  saves or cloud sync.
- Don't publish, push, open releases or upload anything unless the maintainer explicitly asks.

## 2. Goal: match the original

The target is DSR as it plays, not something "close enough".

- Take values from the game's data (params, MSB, FLVER/MTD, TAE, ESD, Havok, banks) or from the exe. When you must
  guess, mark the value `ASSUMED` in a code comment, say why, and log it in `notes/TECH_DEBT.md`.
- Check visual work against the real game: same spot, same camera, side by side (`tools/dsr-ref*`). "It looks
  better" is not evidence. A measured difference is.
- Every way the port differs from DSR goes into `notes/TECH_DEBT.md`, whether a skipped feature, an
  approximation, a workaround or a budget cut. Each item gets a unique ID (check that the ID isn't taken), a
  severity and a fix path. Move it to "Resolved" when you fix it.

## 3. Whole game, not one area

Firelink Shrine is the test area, not the target.

- Exporters and runtime systems take the area or character ID as input. Values tuned on one area go into a
  per-area override table that defaults to game data, with a comment saying why.
- Before calling a tool done, run it on at least one other area (e.g. `m10_01_00_00`, Undead Burg).

## 4. Done means working, verified code

- Research or a design document alone is not a finished task. Ship code that builds and runs.
- Verify in PCSX2 headless: `tools/pcsx2-run.sh <elf> <seconds> <shot.png>` runs on a virtual display and kills
  only its own instance. Use the full test suite when it applies (`tools/test-all.sh`).
- Report numbers from before and after your change: fps at fixed spots, per-pass ms, free EE RAM and VRAM, and
  file sizes. PCSX2 doesn't time VU1, GS fill or the EE cache, so say when a gain can only be confirmed on real
  hardware.
- **The tree must always build and boot.** Iterate on scratch outputs and copy them in only once a plain build
  boots. Stay within the budgets in `notes/HANDOFF.md` (EE RAM, VRAM, 60 fps).

## 5. Working alongside other agents

Several agents often work in the same tree at once.

- **Job queue:** every heavy job (make, exporters, PCSX2 tests, Ghidra, whole-game scans) runs as `tools/q [-m GB] <cmd>`.
  It runs jobs one at a time, waits for free RAM and caps each job's memory. Exporters must stream per part or area
  instead of loading everything at once.
- **Ownership:** the active assignments in `notes/HANDOFF.md` say who owns which files. Outside your own files,
  make small, local edits only. Re-read the file right before each edit, and never rewrite or reformat a whole
  file someone else owns. If another owner's file needs a real change, ask for it (issue, PR comment or
  coordinator) rather than making it yourself.
- **Merging scratch work:** merge it hunk by hunk into the current files. Never copy an older copy over a file
  someone has changed since.
- **Generated sources:** don't hand-edit generated files. The VU1 programs (`*.vsm`) come from
  `tools/vu1/gen.py`; run the offline tests in `tools/vu1/` after changing them.
- **Handoff note:** before reporting a task done, write `notes/threads/<task>.md` covering what works, the files you
  own, open TECH_DEBT IDs, how to test, and next steps. The next agent starts from `notes/HANDOFF.md` plus that
  note; assume it has none of your context.

## 5a. Context and token budget

Agent context is a shared, limited budget (usage limits stop every agent at once).

- **Logs:** never print a whole build, exporter or emulator log. Redirect it to a file and read the tail or grep
  for errors (`make ... > log 2>&1; tail -20 log`).
- **Images:** look at screenshots at 1024 px wide or less. Make comparison sheets with small panels, and crop to
  the region that matters when checking a detail.
- **Waiting:** run long jobs in the background and wait for the completion notice. Don't poll with sleep loops or
  step-by-step monitors.
- **Large files:** read the part you need (grep, line ranges), not whole exporters, generated `.vsm` or notes.
- **Reports:** milestone and handoff messages carry numbers, paths and open items, not narrative. Keep them under
  about 15 lines.

## 6. Code style

- Match the surrounding code's naming, idiom and comment density. C99 in `ps2/proto`, Python 3 in `tools/`.
- Comments state sources and reasons: which exe function, param row or measurement a number comes from. They don't
  narrate the change.
- Keep binary formats documented at the top of the exporter that writes them, and bump a version field when one
  changes.
- No new dependencies without a note in the README (what it is, its licence, how to install it user-locally).

## 7. Commits and pull requests

- One logical change per commit, with a message that says what changed and why, plus measured numbers where they
  matter.
- Don't include game-derived files or screenshots (run `git status` and check `.gitignore` before committing).
- Mark AI-assisted work with a `Co-Authored-By:` trailer naming the agent.
- PR description: what changed, how it was verified (spots, numbers, the headless command), and the TECH_DEBT IDs
  it opens or resolves.

## 8. Where to start

1. `README.md`: build setup and pointing the build at your copy of the game.
2. `notes/HANDOFF.md`: current state, build/run commands, budgets, active assignments.
3. `notes/TECH_DEBT.md`: everything that doesn't match DSR yet, by severity. It is large: look up rows with
   `grep -n '^| ID-' notes/TECH_DEBT.md` (one row per ID) instead of reading the whole file.
4. `notes/ROADMAP.md` and `notes/PORTING.md`: the plan and the format/engine notes.
5. `notes/research/`: optimisation and tooling research with sources.
