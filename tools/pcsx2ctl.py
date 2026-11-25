#!/usr/bin/env python3
"""Client for the control socket of pcsx2-dsrtest, the headless PCSX2 test runner (tools/pcsx2-headless/Main.cpp).

  pcsx2ctl.py (--slot N | --sock PATH) shot|gsdump|save <path> [--timeout 20]
  pcsx2ctl.py (--slot N | --sock PATH) quit

One request line, one reply line ("ok <path>" / "ok" / "err <message>"), then the runner closes the connection. The
reply comes once the file is written (the GS thread writes screenshots and dumps at the next frame), so callers never
poll snaps/ for new files. tools/pcsx2-run.sh starts the runner with -ctl /tmp/pcsx2-headless/ctl.<slot>.sock (slot =
its PINE slot, 99 without PINE). Prints the path (or "ok") and exits 0; exits 1 on "err ..." or a dead socket.

As a module: ctl(sock, line, timeout=20) -> reply line (raises CtlError), sock_for(slot) -> socket path."""
import argparse, os, socket, sys

CTL_DIR = "/tmp/pcsx2-headless"


class CtlError(Exception):
    pass


def sock_for(slot):
    return f"{CTL_DIR}/ctl.{slot}.sock"


def ctl(sock, line, timeout=20.0):
    """Send one request; return the reply line. Raises CtlError on an "err ..." reply or a dead / missing socket."""
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(1.0)  # connect: a dead socket fails at once
    try:
        s.connect(sock)
        s.settimeout(timeout + 5.0)  # the runner waits up to its own 20 s per file, then answers "err timeout"
        s.sendall(line.strip().encode() + b"\n")
        buf = b""
        while not buf.endswith(b"\n"):
            c = s.recv(4096)
            if not c:
                break
            buf += c
    except (OSError, socket.timeout) as e:
        raise CtlError(f"{sock}: {e}") from None
    finally:
        s.close()
    reply = buf.decode(errors="replace").strip()
    if not reply.startswith("ok"):
        raise CtlError(reply or f"{sock}: no reply")
    return reply


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--slot", type=int, help="PINE slot of the runner (socket /tmp/pcsx2-headless/ctl.<slot>.sock)")
    g.add_argument("--sock", help="control socket path")
    ap.add_argument("verb", choices=("shot", "gsdump", "save", "quit"))
    ap.add_argument("path", nargs="?")
    ap.add_argument("--timeout", type=float, default=20.0)
    a = ap.parse_args()
    if a.verb != "quit" and not a.path:
        ap.error(f"{a.verb} needs a path")
    line = a.verb if a.verb == "quit" else f"{a.verb} {os.path.abspath(a.path)}"
    try:
        reply = ctl(a.sock or sock_for(a.slot), line, a.timeout)
    except CtlError as e:
        print(f"pcsx2ctl: {e}", file=sys.stderr)
        sys.exit(1)
    print(reply[3:] if reply.startswith("ok ") else reply)


if __name__ == "__main__":
    main()
