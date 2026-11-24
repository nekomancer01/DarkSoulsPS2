#!/usr/bin/env python3
"""Sample rate pick for the sound exporter (tools/export_audio.py): the band loss of a sample at each candidate rate, and
the greedy per-sample rate choice that fits the counted packs into a byte budget. Pure functions, no game data.

Kept out of export_audio.py so its encoder cache key (inspect.getsource(_encode)) does not change."""
import heapq
from dataclasses import dataclass
from typing import Callable

import numpy as np

RATES = (22050, 16000, 11025, 8000)  # candidate rates, highest first; export_audio.py imports it from here


def band_loss(pcm, src, rates=RATES, frame=2048, max_s=10.0):
    """pcm int16 (n, ch). Share of the energy (mono mix, first max_s seconds, Hann frames of `frame`, power averaged)
    above r/2, per r < src; r >= src -> 0.0; silence -> 0.0 for every r. The energy above the new Nyquist is what a
    resampler to r removes, so no resampler runs here."""
    x = np.asarray(pcm, dtype=np.float64)
    if x.ndim == 2:
        x = x.mean(axis=1)
    x = x[:int(max_s * src)]
    out = {r: 0.0 for r in rates}
    if len(x) == 0:
        return out
    if len(x) < frame:
        x = np.pad(x, (0, frame - len(x)))
    n = len(x) // frame
    win = np.hanning(frame)
    power = np.zeros(frame // 2 + 1)
    for i in range(n):
        power += np.abs(np.fft.rfft(x[i * frame:(i + 1) * frame] * win)) ** 2
    total = power.sum()
    if not total > 0:
        return out
    freqs = np.fft.rfftfreq(frame, 1.0 / src)
    for r in rates:
        if r < src:
            out[r] = float(power[freqs > r / 2.0].sum() / total)
    return out


@dataclass
class Item:
    pack: str                       # pack name: "main", "m10", "c2230", ...
    src: int                        # source rate
    weight: float
    loss: dict                      # band_loss
    size: Callable[[int], int]      # bytes at a rate


def _next_step(it, cur, rates):
    """the next lower rate than cur whose size is strictly smaller (64-byte rounding can make a step save nothing), or None"""
    s = it.size(cur)
    for r in rates:
        if r < cur and it.size(r) < s:
            return r
    return None


def pick_rates(items, budget, counted, rates=RATES):
    """items {key: Item} -> {key: rate}: start every key at the highest rate <= its source (else the lowest rate), then
    step the key with the least weight * added loss per byte saved, among the packs `counted(pack_bytes)` names, until
    those packs fit `budget`. Steps in a pack that later drops out of the counted set are not undone (greedy).
    ValueError when every counted key is at its lowest step and the sum is still over budget."""
    rates = tuple(sorted(rates, reverse=True))
    cur, order, pack_bytes, heaps = {}, {}, {}, {}

    def push(key):
        it = items[key]
        nxt = _next_step(it, cur[key], rates)
        if nxt is None:
            return
        saved = it.size(cur[key]) - it.size(nxt)  # > 0 by _next_step
        cost = it.weight * (it.loss.get(nxt, 0.0) - it.loss.get(cur[key], 0.0))
        heapq.heappush(heaps[it.pack], (cost / saved, order[key], key, nxt))

    for i, (key, it) in enumerate(items.items()):
        cur[key] = max((r for r in rates if r <= it.src), default=rates[-1])
        order[key] = i
        pack_bytes[it.pack] = pack_bytes.get(it.pack, 0) + it.size(cur[key])
        heaps.setdefault(it.pack, [])
    for key in items:
        push(key)

    while True:
        c = counted(pack_bytes)
        if sum(pack_bytes[p] for p in c if p in pack_bytes) <= budget:
            return cur
        best = None
        for p in c:
            h = heaps.get(p)
            if h and (best is None or h[0][:2] < heaps[best][0][:2]):
                best = p
        if best is None:
            raise ValueError(f"every counted sample is at its lowest rate and {sum(pack_bytes[p] for p in c if p in pack_bytes)} "
                             f"bytes are still over the budget of {budget}")
        _, _, key, nxt = heapq.heappop(heaps[best])
        it = items[key]
        pack_bytes[best] -= it.size(cur[key]) - it.size(nxt)
        cur[key] = nxt
        push(key)
