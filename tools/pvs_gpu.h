/* tools/pvs_gpu.c: pvs_bake's cube-map sampling on the GPU (OpenGL 4.5 over headless EGL: NVIDIA, or Mesa for AMD / Intel) */
#include <stdint.h>
/* 1 when a hardware OpenGL 4.5 context is up: verts = ntri triangles x 9 floats (world space), per chunk its first
 * triangle, triangle count and unit (all kept by the caller) */
int gpu_init(int res, const float *verts, unsigned ntri, const unsigned *first, const unsigned *ntris,
             const unsigned *unit, unsigned nch, unsigned nun);
void gpu_run(int ncam, const float (*eye)[3], const float fbs[6][9], const unsigned *off, const unsigned *idx,
             uint32_t *bits);
int gpu_words(void);
int gpu_probe(void);
