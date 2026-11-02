#!/usr/bin/env python3
"""Data dir for one headless test PCSX2 (tools/pcsx2-run.sh PINE=<slot>): <data>/PCSX2/inis/PCSX2.ini = the user's PCSX2.ini
with PINE on <slot>, the BIOS folder pointing at the user's, a GS-dump hotkey and no confirmation dialogs.
  test-pcsx2-ini.py <user PCSX2 config dir> <data dir> <slot>"""
import os, sys

SET = {
    "EmuCore": {"EnablePINE": "true", "PINESlot": None, "HostFs": "true", "EnableFastBoot": "true"},
    "UI": {"ConfirmShutdown": "false", "SetupWizardIncomplete": "false", "PauseOnFocusLoss": "false"},
    # tools/test-run.py presses these on the Xvfb display (xdotool): single-frame GS dump / screenshot into <data>/PCSX2/snaps
    "Hotkeys": {"GSDumpSingleFrame": "Keyboard/F8", "Screenshot": "Keyboard/F9"},
    "Folders": {"Bios": None},
}

# PCSX2_GS="key=value,..." (A/B runs of display / upscaling settings): extra [EmuCore/GS] keys for this instance only,
# e.g. PCSX2_GS="upscale_multiplier=3,deinterlace_mode=0,ScreenshotSize=1" (2.x ini names; deinterlace_mode 0 =
# Automatic, ScreenshotSize 1 = F9 screenshots at internal resolution)
GS = os.environ.get("PCSX2_GS", "")


def main():
    user, data, slot = sys.argv[1], sys.argv[2], sys.argv[3]
    SET["EmuCore"]["PINESlot"] = slot
    SET["Folders"]["Bios"] = os.path.join(user, "bios")
    for kv in filter(None, GS.split(",")):
        k, v = kv.split("=", 1)
        SET.setdefault("EmuCore/GS", {})[k.strip()] = v.strip()
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
