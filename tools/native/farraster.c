/* The far panorama's six views (tools/farpano.py render6, 2026-11-24 build speed): flat-coloured triangles seen from one
 * eye, colour + view depth per pixel. Replaces mapview.render_view for this use (untextured, opaque, no culling, near
 * plane 0.1 m, colour interpolated across the screen, depth perspective-correct, sky colour where nothing is drawn),
 * which was 50 % of farpano's CPU time and most of its 3-4 GB peak (a cold world build: 16 blocks at once swapped and
 * were paused by memwatch for 16 400 job-seconds).
 *
 * fr_render: on the GPU (OpenGL 4.5 core over headless EGL, EGL_EXT_platform_device as tools/pvs_gpu.c: NVIDIA, or Mesa
 * for AMD / Intel) when built with -DFR_GPU and a hardware device comes up, else a C rasteriser with mapview's
 * rules (pixel centres inside or on all three edges, the nearest fragment wins, the earlier triangle on a tie), one view
 * per thread. GPU edge rules are the GPU's (top-left), so its pixels can differ from the C path's along edges.
 *
 *   int fr_render(int n, const float *P, const float *C, const double *eye, int nv, int S, double f, int gpu,
 *                 unsigned char *rgb, double *z, const double *Vc)
 * P: n x 3 corners x xyz, C: n x 3 corners x rgb (0..255, the horizon's baked colours), view k looks along
 * (sin a, 0, cos a), a = 2 pi k / nv, focal length f pixels on an S x S image; Vc (C path, may be NULL): the corners
 * in each view's camera space (nv x n x 3 x xyz: right, up, forward), else computed here; rgb: nv x S x S x 3 (top row
 * first), z: nv x S x S depth along the view axis (INFINITY: nothing). Returns 1 (GPU), 0 (C), -1 (out of memory). */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { double x, y, z, r, g, b; } Vtx;

/* mapview.render_view's colour path for an untextured triangle: geometry colour C * 128 / 150, vertex colour / 128,
 * x the untextured 150 grey (farpano.render6 scales so the result is C again, up to rounding) */
static inline double colour_in(float c) { return c * (128.0 / 150.0) / 128.0; }

static void raster_tri(const Vtx *a, const Vtx *b, const Vtx *c, int S, double f, float *col, double *zb)
{
    double sx0 = S / 2.0 + f * a->x / a->z, sy0 = S / 2.0 - f * a->y / a->z;
    double sx1 = S / 2.0 + f * b->x / b->z, sy1 = S / 2.0 - f * b->y / b->z;
    double sx2 = S / 2.0 + f * c->x / c->z, sy2 = S / 2.0 - f * c->y / c->z;
    double cr = (sx1 - sx0) * (sy2 - sy0) - (sy1 - sy0) * (sx2 - sx0);
    if (!(fabs(cr) > 1e-9))
        return;
    double x0 = floor(fmin(sx0, fmin(sx1, sx2))), x1 = ceil(fmax(sx0, fmax(sx1, sx2)));
    double y0 = floor(fmin(sy0, fmin(sy1, sy2))), y1 = ceil(fmax(sy0, fmax(sy1, sy2)));
    if (x1 < 0 || x0 >= S || y1 < 0 || y0 >= S)
        return;
    int ix0 = x0 < 0 ? 0 : (int)x0, iy0 = y0 < 0 ? 0 : (int)y0;
    int ix1 = x1 > S ? S : (int)x1, iy1 = y1 > S ? S : (int)y1;
    double iz0 = 1.0 / a->z, iz1 = 1.0 / b->z, iz2 = 1.0 / c->z;
    for (int py = iy0; py < iy1; py++) {
        double cy = py + 0.5;
        for (int px = ix0; px < ix1; px++) {
            double cx = px + 0.5;
            double l0 = ((sx1 - cx) * (sy2 - cy) - (sy1 - cy) * (sx2 - cx)) / cr;
            double l1 = ((sx2 - cx) * (sy0 - cy) - (sy2 - cy) * (sx0 - cx)) / cr;
            double l2 = 1 - l0 - l1;
            if (l0 < 0 || l1 < 0 || l2 < 0)
                continue;
            double z = 1.0 / (iz0 * l0 + iz1 * l1 + iz2 * l2);
            size_t p = (size_t)py * S + px;
            if (!(z < zb[p]))
                continue;
            zb[p] = z;
            col[3 * p + 0] = (float)(150.0 * (a->r * l0 + b->r * l1 + c->r * l2));
            col[3 * p + 1] = (float)(150.0 * (a->g * l0 + b->g * l1 + c->g * l2));
            col[3 * p + 2] = (float)(150.0 * (a->b * l0 + b->b * l1 + c->b * l2));
        }
    }
}

static void lerp(Vtx *o, const Vtx *a, const Vtx *b, double t)
{
    o->x = a->x + t * (b->x - a->x), o->y = a->y + t * (b->y - a->y), o->z = a->z + t * (b->z - a->z);
    o->r = a->r + t * (b->r - a->r), o->g = a->g + t * (b->g - a->g), o->b = a->b + t * (b->b - a->b);
}

static void axes(int k, int nv, double fw[3], double rt[3], double up[3])
{
    double yaw = 2 * M_PI * k / nv;
    fw[0] = sin(yaw), fw[1] = 0, fw[2] = cos(yaw);
    double l = sqrt(fw[2] * fw[2] + fw[0] * fw[0]);
    rt[0] = fw[2] / l, rt[1] = 0, rt[2] = -fw[0] / l;
    up[0] = fw[1] * rt[2] - fw[2] * rt[1], up[1] = fw[2] * rt[0] - fw[0] * rt[2], up[2] = fw[0] * rt[1] - fw[1] * rt[0];
}

typedef struct { Vtx v[3]; } Tri;

/* mapview's size bucket of a triangle on screen (its fragments() pass order): 0 for a box of at most 1 pixel, else
 * k for 2^(k-1) < size <= 2^k; -1 when render_view drops it (degenerate, or off screen) */
static int bucket(const Tri *t, int S, double f)
{
    double sx[3], sy[3];
    for (int j = 0; j < 3; j++)
        sx[j] = S / 2.0 + f * t->v[j].x / t->v[j].z, sy[j] = S / 2.0 - f * t->v[j].y / t->v[j].z;
    double cr = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sy[1] - sy[0]) * (sx[2] - sx[0]);
    if (!(fabs(cr) > 1e-9))
        return -1;
    double x0 = floor(fmin(sx[0], fmin(sx[1], sx[2]))), x1 = ceil(fmax(sx[0], fmax(sx[1], sx[2])));
    double y0 = floor(fmin(sy[0], fmin(sy[1], sy[2]))), y1 = ceil(fmax(sy[0], fmax(sy[1], sy[2])));
    if (!(x1 >= 0 && x0 < S && y1 >= 0 && y0 < S))
        return -1;
    x0 = fmax(x0, 0), y0 = fmax(y0, 0), x1 = fmin(x1, S), y1 = fmin(y1, S);
    double size = fmax(x1 - x0, y1 - y0);
    int k = 0;
    for (double sz = 1; size > sz; sz *= 2)
        k++;
    return k;
}

/* one view: the triangles in mapview's order (unclipped ones by index, then the near-clipped pieces), drawn bucket by
 * bucket, smallest first, as its fragments() passes: on an exact depth tie the earlier one keeps the pixel there too */
static int view_cpu(int n, const float *P, const float *C, const double *eye, const double *Vc, int k, int nv,
                    int S, double f, float *col, double *zb)
{
    const double near = 0.1;
    double fw[3], rt[3], up[3];
    axes(k, nv, fw, rt, up);
    Tri *tri = malloc(sizeof(Tri) * (2 * (size_t)n + 1));
    int *bk = malloc(sizeof(int) * (2 * (size_t)n + 1)), *ord = malloc(sizeof(int) * (2 * (size_t)n + 1));
    if (!tri || !bk || !ord) {
        free(tri), free(bk), free(ord);
        return -1;
    }
    size_t nt = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (int t = 0; t < n; t++) {
            Vtx v[3];
            int in = 0;
            for (int j = 0; j < 3; j++) {
                const float *p = P + 9 * (size_t)t + 3 * j, *c = C + 9 * (size_t)t + 3 * j;
                if (Vc) {  /* camera space from the caller (farpano: numpy's own products, so mapview's last bits) */
                    const double *q = Vc + 9 * ((size_t)k * n + t) + 3 * j;
                    v[j].x = q[0], v[j].y = q[1], v[j].z = q[2];
                } else {
                    double d0 = p[0] - eye[0], d1 = p[1] - eye[1], d2 = p[2] - eye[2];
                    v[j].x = d0 * rt[0] + d1 * rt[1] + d2 * rt[2];
                    v[j].y = d0 * up[0] + d1 * up[1] + d2 * up[2];
                    v[j].z = d0 * fw[0] + d1 * fw[1] + d2 * fw[2];
                }
                v[j].r = colour_in(c[0]), v[j].g = colour_in(c[1]), v[j].b = colour_in(c[2]);
                in += v[j].z > near;
            }
            if (in == 3 && pass == 0) {
                tri[nt].v[0] = v[0], tri[nt].v[1] = v[1], tri[nt].v[2] = v[2], nt++;
            } else if (in && in < 3 && pass == 1) {  /* crossing the near plane: clip (Sutherland-Hodgman), fan */
                Vtx poly[4];
                int np = 0;
                for (int j = 0; j < 3; j++) {
                    const Vtx *a = &v[j], *b = &v[(j + 1) % 3];
                    if (a->z > near)
                        poly[np++] = *a;
                    if ((a->z > near) != (b->z > near))
                        lerp(&poly[np++], a, b, (near - a->z) / (b->z - a->z));
                }
                for (int j = 1; j + 1 < np; j++)
                    tri[nt].v[0] = poly[0], tri[nt].v[1] = poly[j], tri[nt].v[2] = poly[j + 1], nt++;
            }
        }
    }
    int nb = 0;
    for (size_t i = 0; i < nt; i++) {
        bk[i] = bucket(&tri[i], S, f);
        if (bk[i] + 1 > nb)
            nb = bk[i] + 1;
    }
    size_t no = 0;
    for (int b = 0; b < nb; b++)  /* stable by bucket */
        for (size_t i = 0; i < nt; i++)
            if (bk[i] == b)
                ord[no++] = (int)i;
    for (size_t i = 0; i < no; i++)
        raster_tri(&tri[ord[i]].v[0], &tri[ord[i]].v[1], &tri[ord[i]].v[2], S, f, col, zb);
    free(tri), free(bk), free(ord);
    return 0;
}

static void store(const float *col, const double *zb, int S, unsigned char *rgb, double *z)
{
    for (size_t p = 0; p < (size_t)S * S; p++) {
        if (isinf(zb[p])) {
            rgb[3 * p] = 190, rgb[3 * p + 1] = 200, rgb[3 * p + 2] = 210;
        } else {
            for (int j = 0; j < 3; j++) {
                float c = col[3 * p + j];
                rgb[3 * p + j] = (unsigned char)(c < 0 ? 0 : c > 255 ? 255 : c);
            }
        }
        z[p] = zb[p];
    }
}

#ifdef FR_GPU
#define GL_GLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>

static int gpu_state;  /* 0 untried, 1 up, -1 unavailable */
static GLuint prog, vao, vbo, fbo, rb_col, rb_dep;
static GLint u_eye, u_ax, u_k;
static int fbo_s;

static const char *VS =
    "#version 450 core\n"
    "layout(location = 0) in vec3 pos;\n"
    "layout(location = 1) in vec3 col;\n"
    "uniform vec3 eye;\n"
    "uniform mat3 ax;\n"   /* rows: right, up, forward */
    "uniform float k;\n"
    "noperspective out vec3 vcol;\n"
    "void main() {\n"
    "    vec3 d = (pos - eye) * transpose(ax);\n"
    "    gl_Position = vec4(k * d.x, k * d.y, d.z - 0.2, d.z);\n"   /* near plane z = 0.1, no far plane */
    "    vcol = col;\n"
    "}\n";
static const char *FS =
    "#version 450 core\n"
    "noperspective in vec3 vcol;\n"
    "layout(location = 0) out vec4 o;\n"
    "void main() { o = vec4(150.0 * vcol, 1.0 / gl_FragCoord.w); }\n";

static GLuint shader(GLenum kind, const char *src)
{
    GLuint s = glCreateShader(kind);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "farraster: shader: %s\n", log);
        return 0;
    }
    return s;
}

static int soft_ok(void)
{
    const char *e = getenv("FAR_GPU_SOFTWARE");
    return e && *e == '1';
}

static int gpu_up(void)
{
    if (gpu_state)
        return gpu_state > 0;
    gpu_state = -1;
    setenv("__GL_YIELD", "USLEEP", 0);  /* NVIDIA: sleep while waiting for the GPU instead of spinning a core */
    PFNEGLQUERYDEVICESEXTPROC qd = (PFNEGLQUERYDEVICESEXTPROC)eglGetProcAddress("eglQueryDevicesEXT");
    PFNEGLGETPLATFORMDISPLAYEXTPROC gpd = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    PFNEGLQUERYDEVICESTRINGEXTPROC qs = (PFNEGLQUERYDEVICESTRINGEXTPROC)eglGetProcAddress("eglQueryDeviceStringEXT");
    if (!qd || !gpd)
        return 0;
    EGLDeviceEXT dev[16];
    EGLint nd = 0;
    if (!qd(16, dev, &nd) || nd < 1)
        return 0;
    int up = 0;
    for (int i = 0; i < nd && !up; i++) {
        const char *ext = qs ? qs(dev[i], EGL_EXTENSIONS) : NULL;
        if (ext && strstr(ext, "EGL_MESA_device_software") && !soft_ok())
            continue;
        EGLDisplay dpy = gpd(EGL_PLATFORM_DEVICE_EXT, dev[i], NULL);
        EGLint maj, min;
        if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &maj, &min) || !eglBindAPI(EGL_OPENGL_API))
            continue;
        static const EGLint ca[] = {EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 5,
                                    EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
        EGLContext ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ca);
        if (ctx != EGL_NO_CONTEXT && eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
            up = 1;
    }
    if (!up)
        return 0;
    const char *ren = (const char *)glGetString(GL_RENDERER);
    if (!ren || ((strstr(ren, "llvmpipe") || strstr(ren, "softpipe")) && !soft_ok()))
        return 0;
    GLuint v = shader(GL_VERTEX_SHADER, VS), f = shader(GL_FRAGMENT_SHADER, FS);
    if (!v || !f)
        return 0;
    prog = glCreateProgram();
    glAttachShader(prog, v), glAttachShader(prog, f);
    glLinkProgram(prog);
    GLint ok;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok)
        return 0;
    u_eye = glGetUniformLocation(prog, "eye"), u_ax = glGetUniformLocation(prog, "ax"), u_k = glGetUniformLocation(prog, "k");
    glGenVertexArrays(1, &vao), glGenBuffers(1, &vbo), glGenFramebuffers(1, &fbo);
    gpu_state = 1;
    return 1;
}

static int view_gpu_all(int n, const float *P, const float *C, const double *eye, int nv, int S, double f,
                        unsigned char *rgb, double *z)
{
    /* vertices relative to the eye (float precision near it, like the C path's doubles) */
    float *vb = malloc(sizeof(float) * 18 * (size_t)(n ? n : 1));
    float *rd = malloc(sizeof(float) * 4 * (size_t)S * S);
    if (!vb || !rd) {
        free(vb), free(rd);
        return -1;
    }
    for (size_t i = 0; i < 3 * (size_t)n; i++) {
        for (int j = 0; j < 3; j++) {
            vb[6 * i + j] = (float)(P[3 * i + j] - eye[j]);
            vb[6 * i + 3 + j] = (float)colour_in(C[3 * i + j]);
        }
    }
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(float) * 18 * (size_t)n), vb, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 24, (void *)0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 24, (void *)12);
    glEnableVertexAttribArray(0), glEnableVertexAttribArray(1);
    free(vb);
    if (fbo_s != S) {
        if (fbo_s)
            glDeleteRenderbuffers(1, &rb_col), glDeleteRenderbuffers(1, &rb_dep);
        glGenRenderbuffers(1, &rb_col), glGenRenderbuffers(1, &rb_dep);
        glBindRenderbuffer(GL_RENDERBUFFER, rb_col);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA32F, S, S);
        glBindRenderbuffer(GL_RENDERBUFFER, rb_dep);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT32F, S, S);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb_col);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rb_dep);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            free(rd);
            return -2;
        }
        fbo_s = S;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, S, S);
    glUseProgram(prog);
    glEnable(GL_DEPTH_TEST), glDepthFunc(GL_LESS), glDisable(GL_CULL_FACE);
    glClampColor(GL_CLAMP_READ_COLOR, GL_FALSE);
    glUniform3f(u_eye, 0, 0, 0);
    glUniform1f(u_k, (float)(2.0 * f / S));
    for (int k = 0; k < nv; k++) {
        double fw[3], rt[3], up[3];
        axes(k, nv, fw, rt, up);
        float ax[9] = {(float)rt[0], (float)rt[1], (float)rt[2], (float)up[0], (float)up[1], (float)up[2],
                       (float)fw[0], (float)fw[1], (float)fw[2]};
        glUniformMatrix3fv(u_ax, 1, GL_TRUE, ax);  /* row-major: rows right, up, forward */
        static const float clear[4] = {0, 0, 0, -1};
        glClearBufferfv(GL_COLOR, 0, clear);
        glClearDepth(1.0), glClear(GL_DEPTH_BUFFER_BIT);
        if (n)
            glDrawArrays(GL_TRIANGLES, 0, 3 * n);
        glReadPixels(0, 0, S, S, GL_RGBA, GL_FLOAT, rd);
        unsigned char *o = rgb + (size_t)k * S * S * 3;
        double *oz = z + (size_t)k * S * S;
        for (int y = 0; y < S; y++) {
            const float *row = rd + 4 * (size_t)(S - 1 - y) * S;  /* GL rows bottom up */
            for (int x = 0; x < S; x++) {
                const float *q = row + 4 * x;
                size_t p = (size_t)y * S + x;
                if (q[3] < 0) {
                    o[3 * p] = 190, o[3 * p + 1] = 200, o[3 * p + 2] = 210, oz[p] = INFINITY;
                } else {
                    for (int j = 0; j < 3; j++)
                        o[3 * p + j] = (unsigned char)(q[j] < 0 ? 0 : q[j] > 255 ? 255 : q[j]);
                    oz[p] = q[3];
                }
            }
        }
    }
    free(rd);
    return glGetError() == GL_NO_ERROR ? 1 : -2;
}
#endif

int fr_render(int n, const float *P, const float *C, const double *eye, int nv, int S, double f, int gpu,
              unsigned char *rgb, double *z, const double *Vc)
{
#ifdef FR_GPU
    if (gpu && gpu_up()) {
        int r = view_gpu_all(n, P, C, eye, nv, S, f, rgb, z);
        if (r == 1)
            return 1;
        fprintf(stderr, "farraster: GPU render failed (%d), C rasteriser\n", r);
    }
#else
    (void)gpu;
#endif
    int fail = 0;
#pragma omp parallel for schedule(dynamic, 1) reduction(| : fail)
    for (int k = 0; k < nv; k++) {
        float *col = calloc((size_t)S * S * 3, sizeof(float));
        double *zb = malloc(sizeof(double) * (size_t)S * S);
        if (!col || !zb) {
            fail = 1;
        } else {
            for (size_t p = 0; p < (size_t)S * S; p++)
                zb[p] = INFINITY;
            if (view_cpu(n, P, C, eye, Vc, k, nv, S, f, col, zb))
                fail = 1;
            store(col, zb, S, rgb + (size_t)k * S * S * 3, z + (size_t)k * S * S);
        }
        free(col), free(zb);
    }
    return fail ? -1 : 0;
}

/* 1 when this build has the GPU path and a hardware OpenGL 4.5 device comes up */
int fr_gpu_ok(void)
{
#ifdef FR_GPU
    return gpu_up();
#else
    return 0;
#endif
}
