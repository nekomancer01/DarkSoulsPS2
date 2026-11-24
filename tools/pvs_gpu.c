/* GPU sampling for tools/pvs_bake.c (2026-11-15, build speed): the same cube-map test as its sample(), drawn with OpenGL
 * 4.5 (+ ARB_shader_draw_parameters) on a headless EGL context (EGL_EXT_platform_device, no window or X server): NVIDIA's
 * driver, or Mesa's for AMD (radeonsi) and Intel (iris).
 *
 * Per camera and cube face: the occluder chunks write 1 / depth into an R32F image with MAX blending (raster() mode 0:
 * the buffer keeps the nearest surface); then every block-0 chunk drawn from there is rasterised again (conservative
 * rasterisation with PVS_GPU_CONSERVATIVE=1) and each fragment that is no
 * farther than 1 % + 2 cm behind that image (raster() mode 1's bias) sets its unit's bit for the camera. The bits come
 * back per camera. Edge coverage follows the GPU's rules, not raster()'s pixel centres, so the result is close to the
 * CPU bake's rather than the same bits: pvs_bake's --validate check (still the CPU raster) measures what is missed.
 *
 * pvs_bake picks this when PVS_GPU is not 0 and gpu_init succeeds; otherwise the CPU path runs as before. */
#define GL_GLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "pvs_gpu.h"

typedef struct { GLuint count, inst, first, base; } DrawCmd;

static EGLDisplay dpy;
static EGLContext ctx;
static GLuint prog_occ, prog_test, vao, vbo, unit_ssbo, bits_ssbo, cmd_buf, fbo_occ, fbo_test, tex;
static int R, W, conservative;
static unsigned nchunks;
static const unsigned *c_first, *c_ntri;
static size_t cmd_cap, bits_cap;
static GLint u_occ_e, u_occ_fb, u_test_e, u_test_fb, u_test_cam;

static const char *VS =
    "#version 450 core\n"
    "#extension GL_ARB_shader_draw_parameters : require\n"
    "layout(location = 0) in vec3 pos;\n"
    "uniform vec3 eye;\n"
    "uniform float fb[9];\n"
    "layout(std430, binding = 0) readonly buffer Units { uint unit[]; };\n"
    "flat out uint vunit;\n"
    "void main() {\n"
    "    vec3 d = pos - eye;\n"
    "    float x = d.x * fb[3] + d.y * fb[4] + d.z * fb[5];\n"
    "    float y = d.x * fb[6] + d.y * fb[7] + d.z * fb[8];\n"
    "    float z = d.x * fb[0] + d.y * fb[1] + d.z * fb[2];\n"
    "    gl_Position = vec4(x, y, z - 0.2, z);  /* near plane at z = 0.1 (raster()'s), no far plane */\n"
    "    vunit = unit[gl_BaseInstanceARB];\n"
    "}\n";
/* layered batches (2026-11-15): a whole batch of cameras x 6 faces in one draw per pass. Each draw command's base
 * instance points at its (chunk, layer, camera) in an SSBO; the vertex shader takes the camera's eye and the face's
 * axes from there and writes gl_Layer (ARB_shader_viewport_layer_array), so every camera face gets its own layer of
 * a 2D array texture. The same per-pixel work as the per-face draws, about 350 000 fewer driver calls per block. */
static const char *VS_L =
    "#version 450 core\n"
    "#extension GL_ARB_shader_draw_parameters : require\n"
    "#extension GL_ARB_shader_viewport_layer_array : require\n"
    "layout(location = 0) in vec3 pos;\n"
    "layout(std430, binding = 0) readonly buffer Units { uint unit[]; };\n"
    "layout(std430, binding = 2) readonly buffer Info { uvec4 info[]; };\n"   /* chunk, layer, camera */
    "layout(std430, binding = 3) readonly buffer Cams { vec4 eye[]; };\n"     /* per layer / 6 */
    "uniform float fbs[54];\n"
    "flat out uint vunit, vcam;\n"
    "flat out int vlayer;\n"
    "void main() {\n"
    "    uvec4 k = info[gl_BaseInstanceARB];\n"
    "    uint layer = k.y + uint(gl_InstanceID);\n"   /* instanced: one command draws all 6 faces */
    "    int f = int(layer % 6u), o = 9 * f;\n"
    "    vec3 d = pos - eye[layer / 6u].xyz;\n"
    "    float x = d.x * fbs[o + 3] + d.y * fbs[o + 4] + d.z * fbs[o + 5];\n"
    "    float y = d.x * fbs[o + 6] + d.y * fbs[o + 7] + d.z * fbs[o + 8];\n"
    "    float z = d.x * fbs[o + 0] + d.y * fbs[o + 1] + d.z * fbs[o + 2];\n"
    "    gl_Position = vec4(x, y, z - 0.2, z);\n"
    "    gl_Layer = int(layer);\n"
    "    vlayer = int(layer), vunit = unit[k.x], vcam = k.z;\n"
    "}\n";
static const char *FS_TEST_L =
    "#version 450 core\n"
    "layout(binding = 0) uniform sampler2DArray occ;\n"
    "layout(std430, binding = 1) buffer Bits { uint bits[]; };\n"
    "uniform uint words;\n"
    "flat in uint vunit, vcam;\n"
    "flat in int vlayer;\n"
    "void main() {\n"
    "    float z = texelFetch(occ, ivec3(ivec2(gl_FragCoord.xy), vlayer), 0).r, iz = gl_FragCoord.w;\n"
    "    if (z == 0.0 || iz >= z / (1.01 + 0.02 * z))\n"
    "        atomicOr(bits[vcam * words + (vunit >> 5)], 1u << (vunit & 31u));\n"
    "}\n";
static const char *FS_OCC_L =
    "#version 450 core\n"
    "layout(location = 0) out float iz;\n"
    "flat in uint vunit, vcam;\n"
    "flat in int vlayer;\n"
    "void main() { iz = gl_FragCoord.w; }\n";
#define LCAMS 64  /* cameras per layered draw (x 6 layers) */
static int layered, instanced;
static GLuint prog_occ_l, prog_test_l, tex_l, fbo_occ_l, fbo_test_l, info_buf, cams_buf;
static GLint u_occ_l_fb, u_test_l_fb, u_test_l_words;
static size_t info_cap;

static const char *FS_OCC =
    "#version 450 core\n"
    "layout(location = 0) out float iz;\n"
    "void main() { iz = gl_FragCoord.w; }\n";
static const char *FS_TEST =
    "#version 450 core\n"
    "layout(binding = 0) uniform sampler2D occ;\n"
    "layout(std430, binding = 1) buffer Bits { uint bits[]; };\n"
    "uniform uint cambase;\n"
    "flat in uint vunit;\n"
    "void main() {\n"
    "    float z = texelFetch(occ, ivec2(gl_FragCoord.xy), 0).r, iz = gl_FragCoord.w;\n"
    "    if (z == 0.0 || iz >= z / (1.01 + 0.02 * z))\n"
    "        atomicOr(bits[cambase + (vunit >> 5)], 1u << (vunit & 31u));\n"
    "}\n";

static GLuint shader(GLenum kind, const char *src)
{
    GLuint s = glCreateShader(kind);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "pvs_gpu: shader: %s\n", log);
        return 0;
    }
    return s;
}

static GLuint program2(const char *vs, const char *fs)
{
    GLuint v = shader(GL_VERTEX_SHADER, vs), f = shader(GL_FRAGMENT_SHADER, fs);
    if (!v || !f)
        return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, v), glAttachShader(p, f);
    glLinkProgram(p);
    GLint ok;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(p, sizeof log, NULL, log);
        fprintf(stderr, "pvs_gpu: link: %s\n", log);
        return 0;
    }
    return p;
}
static GLuint program(const char *fs) { return program2(VS, fs); }

/* PVS_GPU_SOFTWARE=1 (testing only): accept a software renderer (Mesa llvmpipe), to run this code without a GPU */
static int soft_ok(void)
{
    const char *e = getenv("PVS_GPU_SOFTWARE");
    return e && *e == '1';
}

/* Wait for the GPU without spinning a core (2026-11-24 build profile: NVIDIA's driver busy-waits inside the blocking
 * read-back, so pvs_bake burned clock_gettime + libnvidia-eglcore for ~8 % of a world build's CPU samples while the GPU
 * drew; the export steps beside it lost those cores): a fence polled with short sleeps, then the read-back finds the
 * work done. PVS_GPU_SPIN=1: the old blocking wait. */
static void gpu_wait(void)
{
    static int spin = -1;
    if (spin < 0) {
        const char *e = getenv("PVS_GPU_SPIN");
        spin = e && *e == '1';
    }
    if (spin)
        return;
    GLsync fs = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    struct timespec ts = {0, 200000};
    while (glClientWaitSync(fs, 0, 0) == GL_TIMEOUT_EXPIRED)
        nanosleep(&ts, NULL);
    glDeleteSync(fs);
}

static int egl_up(void)
{
    if (!getenv("PVS_GPU_SPIN"))
        setenv("__GL_YIELD", "USLEEP", 0);  /* NVIDIA: its other internal waits sleep too */
    PFNEGLQUERYDEVICESEXTPROC qd = (PFNEGLQUERYDEVICESEXTPROC)eglGetProcAddress("eglQueryDevicesEXT");
    PFNEGLGETPLATFORMDISPLAYEXTPROC gpd = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    PFNEGLQUERYDEVICESTRINGEXTPROC qs = (PFNEGLQUERYDEVICESTRINGEXTPROC)eglGetProcAddress("eglQueryDeviceStringEXT");
    if (!qd || !gpd)
        return 0;
    EGLDeviceEXT dev[16];
    EGLint nd = 0;
    if (!qd(16, dev, &nd) || nd < 1)
        return 0;
    for (int i = 0; i < nd; i++) {  /* hardware devices only (software renderers list themselves too) */
        const char *ext = qs ? qs(dev[i], EGL_EXTENSIONS) : NULL;
        if (ext && strstr(ext, "EGL_MESA_device_software") && !soft_ok())
            continue;
        dpy = gpd(EGL_PLATFORM_DEVICE_EXT, dev[i], NULL);
        EGLint maj, min;
        if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &maj, &min))
            continue;
        if (!eglBindAPI(EGL_OPENGL_API))
            continue;
        static const EGLint ca[] = {EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 5,
                                    EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
        ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ca);
        if (ctx == EGL_NO_CONTEXT)
            continue;
        if (eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
            return 1;
    }
    return 0;
}

int gpu_init(int res, const float *verts, unsigned ntri, const unsigned *first, const unsigned *ntris,
             const unsigned *unit, unsigned nch, unsigned nun)
{
    if (!egl_up())
        return 0;
    const char *ren = (const char *)glGetString(GL_RENDERER);
    if (!ren || ((strstr(ren, "llvmpipe") || strstr(ren, "softpipe")) && !soft_ok()))
        return 0;
    R = res, W = (int)((nun + 31) / 32), nchunks = nch, c_first = first, c_ntri = ntris;
    if (!(prog_occ = program(FS_OCC)) || !(prog_test = program(FS_TEST)))
        return 0;
    u_occ_e = glGetUniformLocation(prog_occ, "eye"), u_occ_fb = glGetUniformLocation(prog_occ, "fb");
    u_test_e = glGetUniformLocation(prog_test, "eye"), u_test_fb = glGetUniformLocation(prog_test, "fb");
    u_test_cam = glGetUniformLocation(prog_test, "cambase");
    glCreateVertexArrays(1, &vao);
    glCreateBuffers(1, &vbo);
    glNamedBufferStorage(vbo, (GLsizeiptr)ntri * 36, verts, 0);
    glVertexArrayVertexBuffer(vao, 0, vbo, 0, 12);
    glEnableVertexArrayAttrib(vao, 0);
    glVertexArrayAttribFormat(vao, 0, 3, GL_FLOAT, GL_FALSE, 0);
    glVertexArrayAttribBinding(vao, 0, 0);
    glCreateBuffers(1, &unit_ssbo);
    glNamedBufferStorage(unit_ssbo, (GLsizeiptr)(nch ? nch : 1) * 4, unit, 0);
    glCreateBuffers(1, &bits_ssbo), glCreateBuffers(1, &cmd_buf);
    glCreateTextures(GL_TEXTURE_2D, 1, &tex);
    glTextureStorage2D(tex, 1, GL_R32F, R, R);
    glCreateFramebuffers(1, &fbo_occ);
    glNamedFramebufferTexture(fbo_occ, GL_COLOR_ATTACHMENT0, tex, 0);
    glCreateFramebuffers(1, &fbo_test);  /* no attachments: the test pass only writes the bits */
    glNamedFramebufferParameteri(fbo_test, GL_FRAMEBUFFER_DEFAULT_WIDTH, R);
    glNamedFramebufferParameteri(fbo_test, GL_FRAMEBUFFER_DEFAULT_HEIGHT, R);
    if (glCheckNamedFramebufferStatus(fbo_occ, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE ||
        glCheckNamedFramebufferStatus(fbo_test, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return 0;
    GLint next = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &next);
    const char *lv = getenv("PVS_GPU_LAYERED");
    GLint maxl = 0;
    glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &maxl);
    for (GLint i = 0; i < next && !(lv && *lv == '0') && maxl >= 6 * LCAMS; i++)
        if (!strcmp((const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i), "GL_ARB_shader_viewport_layer_array"))
            layered = 1;
    const char *iv = getenv("PVS_GPU_INSTANCED");
    if (layered && (!(prog_occ_l = program2(VS_L, FS_OCC_L)) || !(prog_test_l = program2(VS_L, FS_TEST_L))))
        layered = 0;
    if (layered) {
        u_occ_l_fb = glGetUniformLocation(prog_occ_l, "fbs"), u_test_l_fb = glGetUniformLocation(prog_test_l, "fbs");
        u_test_l_words = glGetUniformLocation(prog_test_l, "words");
        glCreateTextures(GL_TEXTURE_2D_ARRAY, 1, &tex_l);
        glTextureStorage3D(tex_l, 1, GL_R32F, R, R, 6 * LCAMS);
        glCreateFramebuffers(1, &fbo_occ_l);
        glNamedFramebufferTexture(fbo_occ_l, GL_COLOR_ATTACHMENT0, tex_l, 0);  /* (layered: gl_Layer picks the face) */
        glCreateFramebuffers(1, &fbo_test_l);
        glNamedFramebufferParameteri(fbo_test_l, GL_FRAMEBUFFER_DEFAULT_WIDTH, R);
        glNamedFramebufferParameteri(fbo_test_l, GL_FRAMEBUFFER_DEFAULT_HEIGHT, R);
        glNamedFramebufferParameteri(fbo_test_l, GL_FRAMEBUFFER_DEFAULT_LAYERS, 6 * LCAMS);
        glCreateBuffers(1, &info_buf), glCreateBuffers(1, &cams_buf);
        glNamedBufferStorage(cams_buf, sizeof(float) * 4 * LCAMS, NULL, GL_DYNAMIC_STORAGE_BIT);
        if (glCheckNamedFramebufferStatus(fbo_occ_l, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE ||
            glCheckNamedFramebufferStatus(fbo_test_l, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            layered = 0;
    }
    /* instancing (every chunk into all 6 faces, the GPU clipping the rest) is off by default since 2026-11-17: the 6x
     * vertex work made it GPU-bound (Burg b 33 -> 22 s wall, same bits); the CPU's per-face lists cost ~25 CPU-s more.
     * PVS_GPU_INSTANCED=1: instanced */
    instanced = layered && iv && *iv == '1';
    /* PVS_GPU_CONSERVATIVE=1: conservative rasterisation for the test pass (GL_NV_conservative_raster): misses fewer
       pixels but marks many more units seen (Burg b: 349 instead of 291 per cell), so the runtime draws more; off */
    const char *cv = getenv("PVS_GPU_CONSERVATIVE");
    for (GLint i = 0; i < next && cv && *cv == '1'; i++)
        if (!strcmp((const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i), "GL_NV_conservative_raster"))
            conservative = 1;
    glBindVertexArray(vao);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, unit_ssbo);
    glDisable(GL_DEPTH_TEST), glDisable(GL_CULL_FACE), glDisable(GL_DITHER);
    glViewport(0, 0, R, R);
    printf("pvs_gpu: %s, %d px faces%s%s\n", ren, R, instanced ? ", layered + instanced batches" : layered ? ", layered batches" : "", conservative ? ", conservative rasterisation" : "");
    return 1;
}

/* ncam cameras: eyes, and per camera 6 faces x 2 lists (occluders, then block-0 chunks to test) of chunk indices:
 * list k of camera c is idx[off[12 c + k] .. off[12 c + k + 1]). bits: ncam x W words, the units each camera saw. */
void gpu_run(int ncam, const float (*eye)[3], const float fbs[6][9], const unsigned *off, const unsigned *idx,
             uint32_t *bits)
{
    size_t ncmd = off[12 * (size_t)ncam];
    if (ncmd > cmd_cap) {
        cmd_cap = ncmd + ncmd / 2 + 1024;
        glDeleteBuffers(1, &cmd_buf), glCreateBuffers(1, &cmd_buf);
        glNamedBufferStorage(cmd_buf, (GLsizeiptr)(cmd_cap * sizeof(DrawCmd)), NULL, GL_DYNAMIC_STORAGE_BIT);
    }
    DrawCmd *cmd = malloc(sizeof *cmd * (ncmd ? ncmd : 1));
    if (!cmd) {
        fprintf(stderr, "pvs_gpu: out of memory for %zu draw commands (address space cap?)\n", ncmd);
        exit(3);
    }
    for (size_t k = 0; k < ncmd; k++) {
        unsigned i = idx[k];
        cmd[k].count = 3 * c_ntri[i], cmd[k].inst = 1, cmd[k].first = 3 * c_first[i], cmd[k].base = i;
    }
    glNamedBufferSubData(cmd_buf, 0, (GLsizeiptr)(ncmd * sizeof *cmd), cmd);
    free(cmd);
    size_t nb = (size_t)ncam * (size_t)W;
    if (nb > bits_cap) {
        bits_cap = nb;
        glDeleteBuffers(1, &bits_ssbo), glCreateBuffers(1, &bits_ssbo);
        glNamedBufferStorage(bits_ssbo, (GLsizeiptr)(bits_cap * 4 + 4), NULL, GL_DYNAMIC_STORAGE_BIT);
    }
    static const GLuint zero = 0;
    glClearNamedBufferSubData(bits_ssbo, GL_R32UI, 0, (GLsizeiptr)(nb * 4), GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, bits_ssbo);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, cmd_buf);
    static const float clear0[4] = {0, 0, 0, 0};
    for (int c = 0; c < ncam; c++)
        for (int f = 0; f < 6; f++) {
            const unsigned *o = off + 12 * (size_t)c + 2 * f;
            /* occluders: max of 1 / depth */
            glBindFramebuffer(GL_FRAMEBUFFER, fbo_occ);
            glClearNamedFramebufferfv(fbo_occ, GL_COLOR, 0, clear0);
            glUseProgram(prog_occ);
            glUniform3fv(u_occ_e, 1, eye[c]), glUniform1fv(u_occ_fb, 9, fbs[f]);
            glEnable(GL_BLEND), glBlendEquation(GL_MAX), glBlendFunc(GL_ONE, GL_ONE);
            if (o[1] > o[0])
                glMultiDrawArraysIndirect(GL_TRIANGLES, (const void *)(sizeof(DrawCmd) * (size_t)o[0]), (GLsizei)(o[1] - o[0]), 0);
            glDisable(GL_BLEND);
            if (o[2] == o[1])
                continue;
            /* the tests read that image */
            glTextureBarrier();
            glBindFramebuffer(GL_FRAMEBUFFER, fbo_test);
            glUseProgram(prog_test);
            glBindTextureUnit(0, tex);
            glUniform3fv(u_test_e, 1, eye[c]), glUniform1fv(u_test_fb, 9, fbs[f]), glUniform1ui(u_test_cam, (GLuint)(c * W));
            if (conservative)
                glEnable(GL_CONSERVATIVE_RASTERIZATION_NV);
            glMultiDrawArraysIndirect(GL_TRIANGLES, (const void *)(sizeof(DrawCmd) * (size_t)o[1]), (GLsizei)(o[2] - o[1]), 0);
            if (conservative)
                glDisable(GL_CONSERVATIVE_RASTERIZATION_NV);
            glBindTextureUnit(0, 0);
        }
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
    gpu_wait();
    glGetNamedBufferSubData(bits_ssbo, 0, (GLsizeiptr)(nb * 4), bits);
}

static int pend_ncam;

/* gpu_run in two halves, so the caller can make the next batch's lists while the GPU draws this one: gpu_submit
 * queues the draws (layered batches when the GPU has them, else the per-face loop and its wait), gpu_collect waits
 * for them and returns the bits. Same arguments and results as gpu_run. */
void gpu_submit(int ncam, const float (*eye)[3], const float fbs[6][9], const unsigned *off, const unsigned *idx)
{
    pend_ncam = ncam;
    if (!layered) {
        static uint32_t *tmp;  /* the per-face path collects at once */
        static size_t cap;
        if ((size_t)ncam * W > cap)
            cap = (size_t)ncam * W, tmp = realloc(tmp, cap * 4);
        gpu_run(ncam, eye, fbs, off, idx, tmp);
        return;
    }
    size_t ncmd = off[12 * (size_t)ncam];
    if (ncmd > cmd_cap) {
        cmd_cap = ncmd + ncmd / 2 + 1024;
        glDeleteBuffers(1, &cmd_buf), glCreateBuffers(1, &cmd_buf);
        glNamedBufferStorage(cmd_buf, (GLsizeiptr)(cmd_cap * sizeof(DrawCmd)), NULL, GL_DYNAMIC_STORAGE_BIT);
    }
    if (ncmd > info_cap) {
        info_cap = ncmd + ncmd / 2 + 1024;
        glDeleteBuffers(1, &info_buf), glCreateBuffers(1, &info_buf);
        glNamedBufferStorage(info_buf, (GLsizeiptr)(info_cap * 16), NULL, GL_DYNAMIC_STORAGE_BIT);
    }
    size_t nb = (size_t)ncam * (size_t)W;
    if (nb > bits_cap) {
        bits_cap = nb;
        glDeleteBuffers(1, &bits_ssbo), glCreateBuffers(1, &bits_ssbo);
        glNamedBufferStorage(bits_ssbo, (GLsizeiptr)(bits_cap * 4 + 4), NULL, GL_DYNAMIC_STORAGE_BIT);
    }
    /* commands grouped per sub-batch of LCAMS cameras: its occluders, then its tests */
    DrawCmd *cmd = malloc(sizeof *cmd * (ncmd ? ncmd : 1));
    GLuint (*info)[4] = malloc(16 * (ncmd ? ncmd : 1));
    size_t nsub = ((size_t)ncam + LCAMS - 1) / LCAMS, *sb = malloc(sizeof(size_t) * (2 * nsub + 1)), w = 0;
    if (!cmd || !info || !sb) {
        fprintf(stderr, "pvs_gpu: out of memory for %zu draw commands (address space cap?)\n", ncmd);
        exit(3);
    }
    for (size_t sbi = 0; sbi < nsub; sbi++)
        for (int pass = 0; pass < 2; pass++) {
            sb[2 * sbi + pass] = w;
            for (int c = (int)(sbi * LCAMS); c < ncam && c < (int)((sbi + 1) * LCAMS); c++)
                for (int f = 0; f < 6; f++) {
                    const unsigned *o = off + 12 * (size_t)c + 2 * f;
                    for (unsigned k = o[pass]; k < o[pass + 1]; k++, w++) {
                        unsigned i = idx[k];
                        cmd[w].count = 3 * c_ntri[i], cmd[w].inst = instanced ? 6 : 1, cmd[w].first = 3 * c_first[i], cmd[w].base = (GLuint)w;
                        info[w][0] = i, info[w][1] = (GLuint)(6 * (c % LCAMS) + f), info[w][2] = (GLuint)c, info[w][3] = 0;
                    }
                }
        }
    sb[2 * nsub] = w;
    glNamedBufferSubData(cmd_buf, 0, (GLsizeiptr)(w * sizeof *cmd), cmd);
    glNamedBufferSubData(info_buf, 0, (GLsizeiptr)(w * 16), info);
    free(cmd), free(info);
    static const GLuint zero = 0;
    glClearNamedBufferSubData(bits_ssbo, GL_R32UI, 0, (GLsizeiptr)(nb * 4), GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, bits_ssbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, info_buf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, cams_buf);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, cmd_buf);
    glUseProgram(prog_occ_l), glUniform1fv(u_occ_l_fb, 54, &fbs[0][0]);
    glUseProgram(prog_test_l), glUniform1fv(u_test_l_fb, 54, &fbs[0][0]), glUniform1ui(u_test_l_words, (GLuint)W);
    for (size_t sbi = 0; sbi < nsub; sbi++) {
        float e4[LCAMS][4] = {{0}};
        for (int c = (int)(sbi * LCAMS); c < ncam && c < (int)((sbi + 1) * LCAMS); c++)
            memcpy(e4[c % LCAMS], eye[c], 12);
        glNamedBufferSubData(cams_buf, 0, sizeof e4, e4);
        static const float z4[4] = {0, 0, 0, 0};
        glClearTexImage(tex_l, 0, GL_RED, GL_FLOAT, z4);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo_occ_l);
        glUseProgram(prog_occ_l);
        glEnable(GL_BLEND), glBlendEquation(GL_MAX), glBlendFunc(GL_ONE, GL_ONE);
        if (sb[2 * sbi + 1] > sb[2 * sbi])
            glMultiDrawArraysIndirect(GL_TRIANGLES, (const void *)(sizeof(DrawCmd) * sb[2 * sbi]), (GLsizei)(sb[2 * sbi + 1] - sb[2 * sbi]), 0);
        glDisable(GL_BLEND);
        glTextureBarrier();
        glBindFramebuffer(GL_FRAMEBUFFER, fbo_test_l);
        glUseProgram(prog_test_l);
        glBindTextureUnit(0, tex_l);
        if (conservative)
            glEnable(GL_CONSERVATIVE_RASTERIZATION_NV);
        if (sb[2 * sbi + 2] > sb[2 * sbi + 1])
            glMultiDrawArraysIndirect(GL_TRIANGLES, (const void *)(sizeof(DrawCmd) * sb[2 * sbi + 1]), (GLsizei)(sb[2 * sbi + 2] - sb[2 * sbi + 1]), 0);
        if (conservative)
            glDisable(GL_CONSERVATIVE_RASTERIZATION_NV);
        glBindTextureUnit(0, 0);
    }
    free(sb);
    glFlush();
}

void gpu_collect(uint32_t *bits)
{
    size_t nb = (size_t)pend_ncam * (size_t)W;
    if (!layered) {  /* gpu_submit already ran it into its buffer: the bits are still in bits_ssbo */
        glGetNamedBufferSubData(bits_ssbo, 0, (GLsizeiptr)(nb * 4), bits);
        return;
    }
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
    gpu_wait();
    glGetNamedBufferSubData(bits_ssbo, 0, (GLsizeiptr)(nb * 4), bits);
}

int gpu_words(void) { return W; }

/* 1: gpu_submit draws every command into all 6 faces of its camera (instancing; the GPU clips what a face does not
 * see), so the caller puts each chunk once, in face 0's lists, and skips the per-face box tests */
int gpu_instanced(void) { return instanced; }

/* pvs_bake --gpu-probe: 0 (and the renderer on stdout) when a hardware OpenGL 4.5 context comes up, else 1 */
int gpu_probe(void)
{
    if (!egl_up())
        return 1;
    const char *ren = (const char *)glGetString(GL_RENDERER);
    if (!ren || ((strstr(ren, "llvmpipe") || strstr(ren, "softpipe")) && !soft_ok()))
        return 1;
    printf("%s\n", ren);
    return 0;
}
