/* Whole-image BC1/BC2/BC3/BC7 -> RGBA8 decode for tools/souls.py (ctypes). bcdec.h: github.com/iOrange/bcdec (MIT). */
#define BCDEC_IMPLEMENTATION
#include "bcdec.h"

/* format: 1 = BC1 (DXT1), 2 = BC2 (DXT3), 3 = BC3 (DXT5), 7 = BC7. dst is w*h*4 bytes. Returns 0 on success. */
int decode_image(const unsigned char *src, int w, int h, int format, unsigned char *dst)
{
    int bsize = format == 1 ? 8 : 16;
    unsigned char block[4 * 4 * 4];
    for (int by = 0; by < (h + 3) / 4; by++)
        for (int bx = 0; bx < (w + 3) / 4; bx++) {
            switch (format) {
            case 1: bcdec_bc1(src, block, 16); break;
            case 2: bcdec_bc2(src, block, 16); break;
            case 3: bcdec_bc3(src, block, 16); break;
            case 7: bcdec_bc7(src, block, 16); break;
            default: return -1;
            }
            src += bsize;
            for (int y = 0; y < 4; y++)
                for (int x = 0; x < 4; x++) {
                    int px = bx * 4 + x, py = by * 4 + y;
                    if (px < w && py < h)
                        for (int c = 0; c < 4; c++)
                            dst[(py * w + px) * 4 + c] = block[(y * 4 + x) * 4 + c];
                }
        }
    return 0;
}
