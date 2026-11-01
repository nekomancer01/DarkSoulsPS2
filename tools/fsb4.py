"""FSB4 (FMOD Ex sample bank) reader and decoder for DSR sound/*.fsb.

Header (48 B): "FSB4", u32 nsamples, u32 sample-header bytes, u32 data bytes, u32 version (0x40000), u32 mode
(0x40 = MPEG frames padded), u8[8], u8[16] hash. Then one header per sample (u16 size, normally 80):
  char name[30], u32 length (samples), u32 compressed bytes, u32 loop start, u32 loop end, u32 mode,
  u32 frequency, u16 volume, i16 pan, u16 priority, u16 channels, f32 min distance, f32 max distance, ...
Sample data follows back to back (each already a multiple of 32 bytes).
Sample mode bits used by DSR: 0x10 16-bit, 0x20 mono, 0x40 stereo, 0x100 signed, 0x200 MPEG, 0x2000 2D,
0x100000 3D, 0x4000000 multichannel (interleaved MPEG streams), 0x10000000 MPEG layer 3, 0x02 loop normal.
Decoding: MPEG through libmpg123 (system library, the decoder vgmstream also uses), PCM16 directly.
Usage: fsb4.py <file.fsb> [--wav outdir]"""
import ctypes, ctypes.util, os, struct, sys, wave

import numpy as np

MODE_LOOP = 0x02
MODE_MPEG = 0x200
MODE_MULTICH = 0x4000000
MODE_3D = 0x100000


class Sample:
    def __init__(self, bank, i, name, length, csize, loop_start, loop_end, mode, freq, vol, pan, pri, ch, mind, maxd,
                 offset):
        self.bank, self.index, self.name = bank, i, name
        self.length, self.csize, self.loop_start, self.loop_end = length, csize, loop_start, loop_end
        self.mode, self.freq, self.volume, self.pan, self.priority = mode, freq, vol, pan, pri
        self.channels, self.min_distance, self.max_distance, self.offset = ch, mind, maxd, offset

    @property
    def codec(self):
        return "mp3" if self.mode & MODE_MPEG else "pcm16" if self.mode & 0x10 else f"mode{self.mode:#x}"

    @property
    def loops(self):
        return bool(self.mode & MODE_LOOP)

    def __repr__(self):
        return (f"#{self.index} {self.name} {self.codec} {self.freq} Hz {self.channels} ch {self.length} smp "
                f"{self.csize} B{' loop' if self.loops else ''}{' 3D' if self.mode & MODE_3D else ''}")


class FSB4:
    def __init__(self, path):
        self.path = path
        self.name = os.path.splitext(os.path.basename(path))[0]
        self.f = open(path, "rb")
        h = self.f.read(48)
        magic, n, shsz, dsz, self.version, self.mode = struct.unpack_from("<4s5I", h)
        if magic != b"FSB4":
            raise ValueError(f"{path}: not FSB4")
        sh = self.f.read(shsz)
        self.samples, o, data = [], 0, 48 + shsz
        for i in range(n):
            size, = struct.unpack_from("<H", sh, o)
            name = sh[o + 2:o + 32].split(b"\0")[0].decode("latin-1")
            v = struct.unpack_from("<6I HhHH ff", sh, o + 32)
            self.samples.append(Sample(self.name, i, name, *v, data))
            data += v[1]
            o += size
        self.by_name = {os.path.splitext(s.name)[0].lower(): s for s in self.samples}

    def raw(self, s):
        self.f.seek(s.offset)
        return self.f.read(s.csize)

    def decode(self, s):
        """int16 array, shape (frames, channels)"""
        d = self.raw(s)
        if s.mode & MODE_MPEG:
            pcm = decode_mpeg(d, s.channels)
        elif s.mode & 0x10:
            pcm = np.frombuffer(d, "<i2").reshape(-1, s.channels)
        else:
            raise ValueError(f"{s}: unsupported codec")
        if len(pcm) >= s.length:  # MP3 rounds up to whole frames
            return pcm[:s.length]
        return np.vstack([pcm, np.zeros((s.length - len(pcm), s.channels), np.int16)])


_mpg = None


def mpg123():
    global _mpg
    if _mpg:
        return _mpg
    cands = [ctypes.util.find_library("mpg123"), "libmpg123.so.0", "/run/host/usr/lib/libmpg123.so.0",
             "/run/host/usr/lib/x86_64-linux-gnu/libmpg123.so.0"]
    for c in cands:
        if not c:
            continue
        try:
            L = ctypes.CDLL(c)
            break
        except OSError:
            continue
    else:
        sys.exit("fsb4.py: libmpg123 not found (system package mpg123 / libmpg123)")
    L.mpg123_init()
    L.mpg123_new.restype = ctypes.c_void_p
    L.mpg123_new.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_int)]
    for fn in ("mpg123_open_feed", "mpg123_delete", "mpg123_format_none"):
        getattr(L, fn).argtypes = [ctypes.c_void_p]
    L.mpg123_format.argtypes = [ctypes.c_void_p, ctypes.c_long, ctypes.c_int, ctypes.c_int]
    L.mpg123_param.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_long, ctypes.c_double]
    L.mpg123_decode.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t,
                                ctypes.POINTER(ctypes.c_size_t)]
    _mpg = L
    return L


def decode_mp3(data, channels):
    L = mpg123()
    err = ctypes.c_int()
    h = L.mpg123_new(None, ctypes.byref(err))
    L.mpg123_param(h, 2, 0x20 | 0x40000, 0.0)  # MPG123_ADD_FLAGS: QUIET | (no gapless guessing)
    L.mpg123_format_none(h)
    for rate in (8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000):
        L.mpg123_format(h, rate, channels, 0xD0)  # MPG123_ENC_SIGNED_16
    L.mpg123_open_feed(h)
    out = ctypes.create_string_buffer(1 << 16)
    done = ctypes.c_size_t()
    chunks = []
    r = L.mpg123_decode(h, data, len(data), out, len(out), ctypes.byref(done))
    while True:
        if done.value:
            chunks.append(out.raw[:done.value])
        if r not in (0, -11):  # MPG123_OK, MPG123_NEW_FORMAT; NEED_MORE (-10) = input used up
            break
        r = L.mpg123_decode(h, None, 0, out, len(out), ctypes.byref(done))
    L.mpg123_delete(h)
    return np.frombuffer(b"".join(chunks), "<i2").reshape(-1, channels)


def mp3_frame_len(b, o):
    h = b[o] << 24 | b[o + 1] << 16 | b[o + 2] << 8 | b[o + 3]
    if h >> 21 != 0x7FF:
        return 0
    ver, layer, br, sr, pad = (h >> 19) & 3, (h >> 17) & 3, (h >> 12) & 15, (h >> 10) & 3, (h >> 9) & 1
    if layer != 1 or br in (0, 15) or sr == 3:
        return 0
    rates = {3: (44100, 48000, 32000), 2: (22050, 24000, 16000), 0: (11025, 12000, 8000)}[ver]
    kbps = ((0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320) if ver == 3 else
            (0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160))[br]
    return (144 if ver == 3 else 72) * kbps * 1000 // rates[sr] + pad


def mp3_frames(data):
    """the MPEG frames of an FSB sample without FMOD's padding between them (mode 0x40 pads every frame)"""
    o, out = 0, []
    while o + 4 <= len(data):
        n = mp3_frame_len(data, o)
        if not n or o + n > len(data):
            o += 1
            continue
        out.append(data[o:o + n])
        o += n
    return out


def decode_mpeg(data, channels):
    """FMOD MPEG sample: mono / stereo is one stream; multichannel interleaves one frame per stereo pair
    (channels / 2 streams) in turn. Decodes every stream and interleaves the channels."""
    frames = mp3_frames(data)
    if channels <= 2:
        return decode_mp3(b"".join(frames), channels)
    nstreams = (channels + 1) // 2
    pcm = [decode_mp3(b"".join(frames[k::nstreams]), 2) for k in range(nstreams)]
    n = min(len(p) for p in pcm)
    return np.hstack([p[:n] for p in pcm])[:, :channels]


def write_wav(path, pcm, freq):
    with wave.open(path, "wb") as w:
        w.setnchannels(pcm.shape[1]); w.setsampwidth(2); w.setframerate(freq)
        w.writeframes(np.ascontiguousarray(pcm).tobytes())


def main():
    b = FSB4(sys.argv[1])
    print(f"{b.name}: {len(b.samples)} samples, mode {b.mode:#x}")
    for s in b.samples:
        print(" ", s)
    if "--wav" in sys.argv:
        out = sys.argv[sys.argv.index("--wav") + 1]
        os.makedirs(out, exist_ok=True)
        for s in b.samples:
            write_wav(os.path.join(out, os.path.splitext(s.name)[0] + ".wav"), b.decode(s), s.freq)


if __name__ == "__main__":
    main()
