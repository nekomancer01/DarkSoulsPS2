#!/usr/bin/env python3
"""The sound exporter's rate pick without game data (tools/ci/offline.sh; needs numpy).

  1 band_loss: a 1 kHz sine loses nothing at any rate, white noise loses ~ 1 - r/src, r >= src and silence lose 0
  2 pick_rates: counted packs fit the budget, equal-size steps never divide by zero, a low-pass sample goes down before a
    broadband one, uncounted packs and the floor are never stepped past, an unreachable budget raises ValueError, and
    2000 items take under 2 s
Exit code = number of failed checks."""
import os
import sys
import time

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from audio_rate import Item, RATES, band_loss, pick_rates  # noqa: E402

FAILS = []


def check(ok, what):
    print(("ok   " if ok else "FAIL ") + what)
    if not ok:
        FAILS.append(what)


SRC = 44100
t = np.arange(SRC * 2) / SRC
sine = (np.sin(2 * np.pi * 1000 * t) * 20000).astype(np.int16)[:, None]
loss = band_loss(sine, SRC)
check(all(loss[r] < 0.001 for r in RATES), f"1 kHz sine: loss < 0.001 at every rate ({max(loss.values()):.5f})")

noise = (np.random.RandomState(1).standard_normal(SRC * 2) * 4000).astype(np.int16)[:, None]
loss = band_loss(noise, SRC)
check(all(abs(loss[r] - (1 - r / SRC)) < 0.05 for r in RATES), "white noise: loss ~ 1 - r/src within 0.05")
check(band_loss(noise, 16000)[22050] == 0.0 and band_loss(noise, 16000)[16000] == 0.0, "r >= src: loss 0")
check(all(v == 0.0 for v in band_loss(np.zeros((5000, 2), np.int16), SRC).values()), "silence: loss 0 at every rate")


def sz(src_bytes):
    """size at a rate: bytes scale with the rate, rounded up to 64"""
    return lambda r: (src_bytes * r // SRC + 63) // 64 * 64


def packbytes(items, rates):
    out = {}
    for k, it in items.items():
        out[it.pack] = out.get(it.pack, 0) + it.size(rates[k])
    return out


lp = {22050: 0.0, 16000: 0.0, 11025: 0.0, 8000: 0.3}
bb = {22050: 0.2, 16000: 0.4, 11025: 0.6, 8000: 0.8}
items = {"bb": Item("main", SRC, 1.0, bb, sz(88200)), "lp": Item("main", SRC, 1.0, lp, sz(88200))}
full = sum(i.size(22050) for i in items.values())
res = pick_rates(items, full - 1, lambda pb: {"main"})
check(res["lp"] < 22050 and res["bb"] == 22050, "low-pass sample steps down before the broadband one of the same size")
check(sum(packbytes(items, res).values()) <= full - 1, "counted pack fits the budget")

# sizes that round equal at two rates: no division by zero, the step is skipped
flat = Item("main", SRC, 1.0, {22050: 0.0, 16000: 0.0, 11025: 0.1, 8000: 0.2}, lambda r: 64)
try:
    pick_rates({"a": flat}, 64, lambda pb: {"main"})
    ok = True
except ZeroDivisionError:
    ok = False
check(ok, "equal sizes at two rates: no ZeroDivisionError")
step = Item("main", SRC, 1.0, {22050: 0.0, 16000: 0.0, 11025: 0.1, 8000: 0.2}, lambda r: 128 if r >= 16000 else 64)
check(pick_rates({"a": step}, 64, lambda pb: {"main"})["a"] == 11025, "a step that saves nothing is skipped to the next that does")

# uncounted pack untouched, floor kept, ValueError when out of reach
two = {"a": Item("main", SRC, 1.0, bb, sz(88200)), "b": Item("m10", SRC, 1.0, bb, sz(88200))}
try:
    pick_rates(two, 1, lambda pb: {"main"})
    check(False, "unreachable budget raises ValueError")
except ValueError:
    check(True, "unreachable budget raises ValueError")
small = sz(88200)(8000) + sz(88200)(22050)
res = pick_rates(two, small, lambda pb: {"main"})
check(res["b"] == 22050, "uncounted pack never stepped")
check(packbytes(two, res)["main"] <= small, "counted pack fits")
check(all(r in RATES for r in res.values()) and min(res.values()) >= RATES[-1], "no rate below RATES[-1]")
low = {"a": Item("main", 8000, 1.0, {r: 0.0 for r in RATES}, sz(88200))}
check(pick_rates(low, 10 ** 9, lambda pb: {"main"})["a"] == 8000, "source at the floor keeps the floor")
low2 = {"a": Item("main", 5000, 1.0, {r: 0.0 for r in RATES}, sz(88200))}
check(pick_rates(low2, 10 ** 9, lambda pb: {"main"})["a"] == RATES[-1], "source below every rate starts at RATES[-1]")

rng = np.random.RandomState(2)
big = {}
for i in range(2000):
    ls = sorted(rng.uniform(0, 1, 4))
    big[i] = Item("main" if i % 3 else "m10", SRC, float(rng.uniform(0.5, 2)), dict(zip(RATES, [0.0] + list(ls[1:]))),
                  sz(int(rng.randint(2000, 200000))))
tot = sum(packbytes(big, {k: 22050 for k in big}).values())
t0 = time.time()
res = pick_rates(big, tot // 2, lambda pb: {"main", "m10"})
dt = time.time() - t0
check(dt < 2.0 and sum(packbytes(big, res).values()) <= tot // 2, f"2000 items: fits the budget in {dt:.2f} s (< 2 s)")

sys.exit(len(FAILS))
