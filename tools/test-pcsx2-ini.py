#!/usr/bin/env python3
"""Data dir for one headless test PCSX2 (tools/pcsx2-run.sh PINE=<slot>): <data>/PCSX2/inis/PCSX2.ini = the user's PCSX2.ini
with PINE on <slot>, the BIOS folder pointing at the user's, a GS-dump hotkey and no confirmation dialogs.
  test-pcsx2-ini.py <user PCSX2 config dir> <data dir> <slot>"""
import os, sys

SET = {
    "EmuCore": {"EnablePINE": "true", "PINESlot": None, "HostFs": "true", "EnableFastBoot": "true"},
    "UI": {"ConfirmShutdown": "false", "SetupWizardIncomplete": "false", "PauseOnFocusLoss": "false"},
    # tools/test-run.py presses these on the Xvfb display (xdotool): single-frame GS dump into <data>/PCSX2/snaps
    "Hotkeys": {"GSDumpSingleFrame": "Keyboard/F8"},
    "Folders": {"Bios": None},
}


# TURBO=1 (tools/test-run.py --turbo): no speed limit, for functional tests (crossings, soaks) that need no real-time
# pacing. The ELF's frame times / fps come from the emulated clock, so they keep their meaning; the wall clock gets
# shorter. TURBO=<x>: speed limit x (2 = 200 %). Keys of PCSX2 2.x's ini: [Framerate] NominalScalar (0 = unlimited,
# the speed menu's "Unlimited") and [EmuCore/GS] VsyncEnable. A/B 2026-11-02 (world_soak.txt, same ELF): 159.3 s ->
# 104.9 s, 4338 / 4343 frames, the 7 shots' TEST stats equal within noise; check again after a PCSX2 update.
TURBO = os.environ.get("TURBO", "")


def main():
    user, data, slot = sys.argv[1], sys.argv[2], sys.argv[3]
    SET["EmuCore"]["PINESlot"] = slot
    if TURBO and TURBO != "0":
        SET["Framerate"] = {"NominalScalar": "0" if TURBO == "1" else TURBO}
        SET["EmuCore/GS"] = {"VsyncEnable": "false"}
    SET["Folders"]["Bios"] = os.path.join(user, "bios")
    secs, order, cur = {}, [], None
    for line in open(os.path.join(user, "inis", "PCSX2.ini"), encoding="utf-8"):
        line = line.rstrip("\n")
        if line.startswith("[") and line.endswith("]"):
            cur = line[1:-1]
            order.append(cur)
            secs[cur] = []
        elif cur and "=" in line:
            k, v = line.split("=", 1)
            secs[cur].append([k.strip(), v.strip()])
    for sec, kv in SET.items():
        if sec not in secs:
            order.append(sec)
            secs[sec] = []
        for k, v in kv.items():
            row = next((r for r in secs[sec] if r[0] == k), None)
            if row:
                row[1] = v
            else:
                secs[sec].append([k, v])
    root = os.path.join(data, "PCSX2")  # PCSX2 -datapath <dir> keeps its data in <dir>/PCSX2
    os.makedirs(os.path.join(root, "inis"), exist_ok=True)
    with open(os.path.join(root, "inis", "PCSX2.ini"), "w", encoding="utf-8") as f:
        for sec in order:
            f.write(f"[{sec}]\n" + "".join(f"{k} = {v}\n" for k, v in secs[sec]) + "\n")


if __name__ == "__main__":
    main()
