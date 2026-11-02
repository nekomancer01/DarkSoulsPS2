"""Image.resize with a fixed pass order, so exports give the same pixels with every Pillow version and platform.

Why (TECH_DEBT build-7): when both axes change, Pillow resamples in two passes through an 8-bit intermediate. Up to
12.x it always ran the horizontal pass first; 13.0 picks the order per shape (vertical first for e.g. 8x58 -> 2x17),
so the rounding of the intermediate, and the exported glyphs / sprites / textures, depended on the Pillow installed
(the in-browser converter has 10.2). resize() here runs the passes itself, horizontal then vertical, on the
premultiplied image as Pillow does for RGBA / LA: the same pixels Pillow <= 12 gave in one call.

    resize(img, (w, h), resample) -> PIL image, like img.resize((w, h), resample)
"""

_PREMUL = {"RGBA": "RGBa", "LA": "La"}


def resize(img, size, resample):
    from PIL import Image
    w, h = int(size[0]), int(size[1])
    if (w, h) == img.size:
        return img.copy()
    if resample == Image.NEAREST or img.size[0] == w or img.size[1] == h:
        return img.resize((w, h), resample)  # one pass (or none to order): already the same everywhere
    mode = img.mode
    im = img.convert(_PREMUL[mode]) if mode in _PREMUL else img
    im = im.resize((w, im.size[1]), resample)  # horizontal pass
    im = im.resize((w, h), resample)  # vertical pass
    return im.convert(mode) if mode in _PREMUL else im
