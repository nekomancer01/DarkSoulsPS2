/* GPU sampling for tools/pvs_bake.c (2026-11-15, build speed): the same cube-map test as its sample(), drawn with OpenGL
 * 4.6 on a headless EGL context (EGL_EXT_platform_device, no window or X server).
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
    "#version 460 core\n"
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
    "    vunit = unit[gl_BaseInstance];\n"
    "}\n";
static const char *FS_OCC =
    "#version 460 core\n"
    "layout(location = 0) out float iz;\n"
    "void main() { iz = gl_FragCoord.w; }\n";
static const char *FS_TEST =
    "#version 460 core\n"
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

static GLuint program(const char *fs)
{
    GLuint v = shader(GL_VERTEX_SHADER, VS), f = shader(GL_FRAGMENT_SHADER, fs);
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

static int egl_up(void)
{
    PFNEGLQUERYDEVICESEXTPROC qd = (PFNEGLQUERYDEVICESEXTPROC)eglGetProcAddress("eglQueryDevicesEXT");
    PFNEGLGETPLATFORMDISPLAYEXTPROC gpd = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    PFNEGLQUERYDEVICESTRINGEXTPROC qs = (PFNEGLQUERYDEVICESTRINGEXTPROC)eglGetProcAddress("eglQueryDeviceStringEXT");
    if (!qd || !gpd)
        return 0;
    EGLDeviceEXT dev[16];
    EGLint nd = 0;
    if (!qd(16, dev, &nd) || nd < 1)
        return 0;
    for (int i = 0; i < nd; i++) {  /* a hardware device first (software renderers list themselves too) */
        const char *ext = qs ? qs(dev[i], EGL_EXTENSIONS) : NULL;
        if (ext && strstr(ext, "EGL_MESA_device_software"))
            continue;
        dpy = gpd(EGL_PLATFORM_DEVICE_EXT, dev[i], NULL);
        EGLint maj, min;
        if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &maj, &min))
            continue;
        if (!eglBindAPI(EGL_OPENGL_API))
            continue;
        static const EGLint ca[] = {EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 6,
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
    if (!ren || strstr(ren, "llvmpipe") || strstr(ren, "softpipe"))
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
    printf("pvs_gpu: %s, %d px faces%s\n", ren, R, conservative ? ", conservative rasterisation" : "");
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
    glGetNamedBufferSubData(bits_ssbo, 0, (GLsizeiptr)(nb * 4), bits);
}

int gpu_words(void) { return W; }
