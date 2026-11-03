/* dsr_light.load_probes_uncached's cube decode: a GI_EnvDif DDS (BC6H UF16, 6 faces) -> per face R x R cells of the
   mean linear rgb, summed in double in the Python loop's order (block rows, blocks, the block's 16 texels). */
#define BCDEC_IMPLEMENTATION
#define BCDEC_STATIC
#include "../native/bcdec.h"
#include "mapx.h"

/* dds: the file from offset 148 on (the first face's first block); w, h: the face size; out: 6 x R x R x 3 */
MX_API void mx_probe_faces(const unsigned char *blocks, int w, int h, int R, double *out)
{
    int step = w / R;
    float buf[48];
    double n = (double)(step * step);
    for (int f = 0; f < 6; f++) {
        double *acc = out + (long long)f * R * R * 3;
        for (int k = 0; k < R * R * 3; k++)
            acc[k] = 0.0;
        for (int by = 0; by < h / 4; by++)
            for (int bx = 0; bx < w / 4; bx++) {
                bcdec_bc6h_float(blocks, buf, 12, 0);
                blocks += 16;
                for (int i = 0; i < 16; i++) {
                    int y = by * 4 + i / 4, x = bx * 4 + i % 4;
                    double *cell = acc + ((long long)(y / step) * R + x / step) * 3;
                    for (int c = 0; c < 3; c++)
                        cell[c] += (double)buf[i * 3 + c];
                }
            }
        for (int k = 0; k < R * R * 3; k++)
            acc[k] = acc[k] / n;
    }
}
