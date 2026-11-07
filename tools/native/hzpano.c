/* Panorama render for tools/horizon.py (HZN_PANO): the block's far backdrop as seen from its centre, on a cylinder.
 *
 *   hz_pano(n, scr, uv, tid, light, ntex, pix, toff, tw, th, W, H, rgba, zb)
 *     scr: n x 9 floats, per corner x (texels across, may run past W: wrapped), y (texels down), depth (metres);
 *     uv / tid / light / textures as hz_bake (tools/native/hzbake.c); rgba: H x W x 4 floats (the caller zeroes it),
 *     zb: H x W floats (the caller fills it with a large depth)
 *
 * Nearest surface wins per texel (depth test), texture point sampled (u, v interpolated in screen space: the caller
 * subdivides triangles that span more than a few texels of the cylinder), colour = texel x light / 128, alpha 1 where a
 * texel is drawn. Transparent texels leave what is behind them. */
#include <math.h>

void hz_pano(int n, const float *scr, const float *uv, const int *tid, const float *light, int ntex,
             const unsigned char *pix, const long *toff, const int *tw, const int *th, int W, int H, float *rgba,
             float *zb)
{
    for (int i = 0; i < n; i++) {
        const float *s = scr + 9 * i;
        for (int rep = 0; rep < 2; rep++) {  /* a triangle over the seam: drawn again one turn to the left */
            float x[3], y[3], z[3];
            for (int k = 0; k < 3; k++)
                x[k] = s[3 * k] - rep * W, y[k] = s[3 * k + 1], z[k] = s[3 * k + 2];
            float x0 = fminf(x[0], fminf(x[1], x[2])), x1 = fmaxf(x[0], fmaxf(x[1], x[2]));
            float y0 = fminf(y[0], fminf(y[1], y[2])), y1 = fmaxf(y[0], fmaxf(y[1], y[2]));
            if (x1 < 0 || x0 >= W || y1 < 0 || y0 >= H)
                continue;
            float area = (x[1] - x[0]) * (y[2] - y[0]) - (y[1] - y[0]) * (x[2] - x[0]);
            if (fabsf(area) < 1e-9f)
                continue;
            int ix0 = x0 < 0 ? 0 : (int)x0, ix1 = x1 >= W ? W - 1 : (int)x1;
            int iy0 = y0 < 0 ? 0 : (int)y0, iy1 = y1 >= H ? H - 1 : (int)y1;
            int t = tid[i];
            for (int py = iy0; py <= iy1; py++)
                for (int px = ix0; px <= ix1; px++) {
                    float cx = px + 0.5f, cy = py + 0.5f;
                    float w0 = ((x[1] - cx) * (y[2] - cy) - (y[1] - cy) * (x[2] - cx)) / area;
                    float w1 = ((x[2] - cx) * (y[0] - cy) - (y[2] - cy) * (x[0] - cx)) / area;
                    float w2 = 1 - w0 - w1;
                    if (w0 < 0 || w1 < 0 || w2 < 0)
                        continue;
                    float d = w0 * z[0] + w1 * z[1] + w2 * z[2];
                    long o = (long)py * W + px;
                    if (d >= zb[o])
                        continue;
                    float rgb[3] = {128, 128, 128};
                    if (t >= 0 && t < ntex) {
                        float u = w0 * uv[6 * i] + w1 * uv[6 * i + 2] + w2 * uv[6 * i + 4];
                        float v = w0 * uv[6 * i + 1] + w1 * uv[6 * i + 3] + w2 * uv[6 * i + 5];
                        int tx = (int)floorf(u * tw[t]) % tw[t], ty = (int)floorf(v * th[t]) % th[t];
                        tx += tx < 0 ? tw[t] : 0, ty += ty < 0 ? th[t] : 0;
                        const unsigned char *p = pix + toff[t] + 4 * ((long)ty * tw[t] + tx);
                        if (!p[3])
                            continue;  /* cut out */
                        rgb[0] = p[0], rgb[1] = p[1], rgb[2] = p[2];
                    }
                    for (int k = 0; k < 3; k++) {
                        float l = (w0 * light[9 * i + k] + w1 * light[9 * i + 3 + k] + w2 * light[9 * i + 6 + k]) / 128;
                        float c = rgb[k] * l;
                        rgba[4 * o + k] = c > 255 ? 255 : c;
                    }
                    rgba[4 * o + 3] = 1;
                    zb[o] = d;
                }
        }
    }
}
