"""SPU2 ADPCM (VAG blocks) for the audio exporter: encode / decode / resample, with ps2sdk's ps2adpcm as a cross-check.
    import adpcm
    blocks = adpcm.encode(pcm_int16_mono, loop_start=None)   # bytes, 16 per 28 samples (+ end block for one-shots)
    pcm = adpcm.decode(blocks)
    adpcm.snr(ref, test)                                       # dB
    adpcm.resample(pcm_int16, src_hz, dst_hz)                  # windowed-sinc, per channel
Native part: vagenc.c (built next to this file on first use).
Test: adpcm.py --test [file.fsb]  (encodes every sample of the bank at 22050 Hz, reports SNR, compares with ps2adpcm)"""
import ctypes, os, subprocess, sys, tempfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
_L = None


def lib():
    global _L
    if _L:
        return _L
    so, src = os.path.join(HERE, "libvagenc.so"), os.path.join(HERE, "vagenc.c")
    if not os.path.exists(so) or os.path.getmtime(so) < os.path.getmtime(src):
        tmp = f"{so}.{os.getpid()}"  # built aside and renamed: a parallel process never loads half a library
        if os.system(f"gcc -O2 -ffp-contract=off -shared -fPIC -o '{tmp}' '{src}'"):
            sys.exit("adpcm.py: building libvagenc.so failed")
        os.replace(tmp, so)
    L = ctypes.CDLL(so)
    L.vag_encode.restype = ctypes.c_size_t
    L.vag_encode.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_long]
    L.vag_decode.restype = ctypes.c_size_t
    L.vag_decode.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p]
    L.rs_accum.restype = None
    L.rs_accum.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                           ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p]
    _L = L
    return L


def encode(pcm, loop_start=None):
    pcm = np.ascontiguousarray(pcm, np.int16).reshape(-1)
    out = ctypes.create_string_buffer((len(pcm) // 28 + 2) * 16)
    n = lib().vag_encode(pcm.ctypes.data, len(pcm), out, -1 if loop_start is None else int(loop_start))
    return out.raw[:n]


def decode(blocks):
    nb = len(blocks) // 16
    out = np.zeros(nb * 28, np.int16)
    lib().vag_decode(blocks, nb, out.ctypes.data)
    return out


def snr(ref, test):
    ref = ref.astype(np.float64).reshape(-1)
    test = test.astype(np.float64).reshape(-1)[:len(ref)]
    noise = ((ref - test) ** 2).sum()
    return 99.0 if noise == 0 else 10 * np.log10(max((ref ** 2).sum(), 1e-9) / noise)


def resample(pcm, src, dst, taps=32, native=True):
    """Band-limited resampling (Kaiser-windowed sinc, cutoff 0.95 x the lower Nyquist); int16 (frames, ch) in/out.
    The window weights depend only on an output frame's fractional position, which takes few distinct values (a
    fixed rate ratio repeats them), so they are computed once per distinct value and the multiply-add runs in C
    (vagenc.c rs_accum, same order and rounding: the output is bit for bit the all-numpy form, native=False, which
    took ~50 s for a 5-minute stereo track)."""
    if src == dst:
        return pcm
    x = pcm.astype(np.float32).reshape(len(pcm), -1)
    n_out = int(len(x) * dst / src)
    t = np.arange(n_out) * (src / dst)
    i0 = np.floor(t).astype(np.int64)
    frac = (t - i0).astype(np.float32)
    cut = 0.95 * min(1.0, dst / src)
    k = np.arange(-taps + 1, taps + 1)
    xp = np.vstack([np.zeros((taps, x.shape[1]), np.float32), x, np.zeros((taps + 1, x.shape[1]), np.float32)])

    def weights(j, f):
        d = j - f
        return cut * np.sinc(cut * d) * np.i0(8.0 * np.sqrt(np.clip(1 - (d / taps) ** 2, 0, 1))) / np.i0(8.0)

    if native:
        uf, ph = np.unique(frac, return_inverse=True)
        w = np.stack([weights(j, uf) for j in k], 1)
        if w.dtype == np.float64:  # rs_accum's precision (numpy's promotion of int64 tap - float32 position)
            w = np.ascontiguousarray(w)
            xp = np.ascontiguousarray(xp)
            ph = np.ascontiguousarray(ph.reshape(-1), np.int64)
            i0c = np.ascontiguousarray(i0 + k[0] + taps, np.int64)  # first tap's row in xp
            out = np.empty((n_out, x.shape[1]), np.float32)
            lib().rs_accum(xp.ctypes.data, x.shape[1], i0c.ctypes.data, ph.ctypes.data, n_out, w.ctypes.data,
                           len(k), out.ctypes.data)
            return np.clip(np.round(out), -32768, 32767).astype(np.int16)
    out = np.zeros((n_out, x.shape[1]), np.float32)
    for j in k:  # 2 * taps passes over the output, O(n * taps) memory-light
        out += xp[i0 + j + taps] * weights(j, frac)[:, None]
    return np.clip(np.round(out), -32768, 32767).astype(np.int16)


def ps2sdk_encode(pcm):
    """ps2sdk tools/ps2adpcm (mono raw PCM in, raw ADPCM blocks out); None if the tool is not on PATH"""
    with tempfile.TemporaryDirectory() as d:
        i, o = os.path.join(d, "in.raw"), os.path.join(d, "out.adp")
        np.ascontiguousarray(pcm, np.int16).tofile(i)
        try:
            subprocess.run(["ps2adpcm", i, o], check=True, capture_output=True)
        except (OSError, subprocess.CalledProcessError):
            return None
        return open(o, "rb").read()


def test(fsb_path, rate=22050):
    sys.path.insert(0, os.path.dirname(HERE))
    import fsb4
    b = fsb4.FSB4(fsb_path)
    print(f"{b.name}: {len(b.samples)} samples -> SPU2 ADPCM at {rate} Hz")
    print(f"  {'sample':32} {'src':>6} {'pcm B':>8} {'adpcm B':>8} {'ours dB':>8} {'ps2adpcm dB':>11}")
    for s in b.samples:
        pcm = resample(b.decode(s), s.freq, rate)[:, 0]
        a = encode(pcm)
        ours = snr(pcm, decode(a))
        p = ps2sdk_encode(pcm)
        theirs = f"{snr(pcm, decode(_strip_header(p, len(pcm)))):11.1f}" if p else "          -"
        print(f"  {s.name:32} {s.freq:6} {2 * len(pcm):8} {len(a):8} {ours:8.1f} {theirs}")


def _strip_header(blob, n):
    """ps2adpcm writes bare blocks (no header): sample 0 is in block 0"""
    return blob


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--test":
        test(sys.argv[2])
