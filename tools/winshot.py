#!/usr/bin/env python3
# Usage: winshot.py <caption prefix> <output.png> [--width N] — capture a window via KWin ScreenShot2 (runs on host).
#        winshot.py --list [filter]          — print resourceClass|caption of windows (filter: case-insensitive)
# Same method as ~/.claude/scripts/rs-shot.py, for any window.
import os, sys, subprocess, tempfile, threading
import dbus
from PIL import Image

LIST = sys.argv[1] == "--list"
prefix, out = ("", None) if LIST else (sys.argv[1], sys.argv[2])
maxw = int(sys.argv[sys.argv.index("--width") + 1]) if "--width" in sys.argv else 0
bus = dbus.SessionBus()
# The prefix matches the caption or the window class (PCSX2 titles its window after the running ELF).
js = ('var ws=workspace.windowList();for(var i=0;i<ws.length;i++){var w=ws[i];'
      'if(w.caption.indexOf(%r)===0||String(w.resourceClass).indexOf(%r)===0)'
      'print("WSWIN|"+w.internalId+"|"+w.resourceClass+"|"+w.caption);}' % (prefix, prefix))
with tempfile.NamedTemporaryFile("w", suffix=".js", delete=False) as f:
    f.write(js)
scripting = dbus.Interface(bus.get_object("org.kde.KWin", "/Scripting"), "org.kde.kwin.Scripting")
since = subprocess.run(["date", "+%Y-%m-%d %H:%M:%S"], capture_output=True, text=True).stdout.strip()
sid = scripting.loadScript(f.name, "wsfind", signature="ss")
bus.get_object("org.kde.KWin", f"/Scripting/Script{sid}").run(dbus_interface="org.kde.kwin.Script")
scripting.unloadScript("wsfind")
os.unlink(f.name)
log = subprocess.run(["journalctl", "--user", "--since", since, "-o", "cat", "--no-pager"], capture_output=True, text=True).stdout
found = [l.split("|", 3)[1:] for l in log.splitlines() if "WSWIN|" in l]
if LIST:
    filt = sys.argv[2].lower() if len(sys.argv) > 2 else ""
    for h, cls, cap in found:
        if filt in (cls + cap).lower():
            print(f"{cls}|{cap}")
    sys.exit()
handles = [h.strip() for h, _, _ in found]
if not handles:
    sys.exit(f"no window starting with {prefix!r}")
r, w = os.pipe()
chunks = []
reader = threading.Thread(target=lambda: chunks.extend(iter(lambda: os.read(r, 1 << 20), b"")))
reader.start()
shot = dbus.Interface(bus.get_object("org.kde.KWin", "/org/kde/KWin/ScreenShot2"), "org.kde.KWin.ScreenShot2")
meta = shot.CaptureWindow(handles[-1], {"include-decoration": False, "native-resolution": True}, dbus.types.UnixFd(w))
os.close(w)
reader.join()
os.close(r)
width, height, stride = int(meta["width"]), int(meta["height"]), int(meta["stride"])
raw = "RGBA" if int(meta.get("format", 6)) in (17, 18, 19) else "BGRA"
img = Image.frombuffer("RGBA", (width, height), b"".join(chunks), "raw", raw, stride, 1)
if maxw and width > maxw:
    img = img.resize((maxw, height * maxw // width), Image.LANCZOS)
img.convert("RGB").save(out)
