#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <gbm.h>
#define EGL_EGLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#define GL_GLEXT_PROTOTYPES
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

/* ── Linear path (dumb buffer / Xorg / FBC) ─────────────────────────── */

static int dump_linear(int fd, drmModeFB2 *fb)
{
    struct drm_mode_map_dumb map = {};
    map.handle = fb->handles[0];
    if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map)) {
        perror("MAP_DUMB"); return 1;
    }

    size_t size = (size_t)fb->pitches[0] * fb->height;
    void *ptr = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, map.offset);
    if (ptr == MAP_FAILED) { perror("mmap"); return 1; }

    FILE *f = fopen("/tmp/scanout.raw", "wb");
    fwrite(ptr, 1, size, f);
    fclose(f);
    munmap(ptr, size);

    printf("method=linear raw_fmt=drm raw_pitch=%d\n", fb->pitches[0]);
    printf("dumped %zu bytes to /tmp/scanout.raw\n", size);
    return 0;
}

/* ── EGL path (CCS / tiled / Wayland) ───────────────────────────────── */

static const char *vert_src =
    "#version 300 es\n"
    "in vec2 pos;\n"
    "out vec2 uv;\n"
    "void main() {\n"
    "    uv = pos * 0.5 + 0.5;\n"
    "    uv.y = 1.0 - uv.y;\n"
    "    gl_Position = vec4(pos, 0.0, 1.0);\n"
    "}\n";

static const char *frag_src =
    "#version 300 es\n"
    "#extension GL_OES_EGL_image_external_essl3 : require\n"
    "precision mediump float;\n"
    "uniform samplerExternalOES tex;\n"
    "in vec2 uv;\n"
    "out vec4 color;\n"
    "void main() {\n"
    "    color = texture(tex, uv);\n"
    "}\n";

static GLuint compile_shader(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        fprintf(stderr, "shader compile error: %s\n", log);
    }
    return s;
}

typedef void (*PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)(GLenum, GLeglImageOES);

static int dump_egl(int fd, drmModeFB2 *fb)
{
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES =
        (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
        eglGetProcAddress("glEGLImageTargetTexture2DOES");
    if (!glEGLImageTargetTexture2DOES) {
        fprintf(stderr, "glEGLImageTargetTexture2DOES not available\n");
        return 1;
    }
    int render_fd = open("/dev/dri/renderD128", O_RDWR);
    if (render_fd < 0) { perror("open renderD128"); return 1; }

    struct gbm_device *gbm = gbm_create_device(render_fd);
    if (!gbm) { fprintf(stderr, "gbm_create_device failed\n"); return 1; }

    EGLDisplay dpy = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm, NULL);
    if (dpy == EGL_NO_DISPLAY) {
        fprintf(stderr, "eglGetPlatformDisplay failed: 0x%x\n", eglGetError());
        return 1;
    }

    EGLint major, minor;
    if (!eglInitialize(dpy, &major, &minor)) {
        fprintf(stderr, "eglInitialize failed: 0x%x\n", eglGetError());
        return 1;
    }

    eglBindAPI(EGL_OPENGL_ES_API);

    static const EGLint ctx_attrs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 3,
        EGL_NONE
    };
    EGLContext ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attrs);
    if (ctx == EGL_NO_CONTEXT) {
        fprintf(stderr, "eglCreateContext failed: 0x%x\n", eglGetError());
        return 1;
    }

    if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
        fprintf(stderr, "eglMakeCurrent failed: 0x%x\n", eglGetError());
        return 1;
    }

    /* Export GEM handle as dma-buf */
    int prime_fd;
    if (drmPrimeHandleToFD(fd, fb->handles[0], DRM_CLOEXEC, &prime_fd)) {
        perror("drmPrimeHandleToFD"); return 1;
    }

    /* Import dma-buf as EGL image (GPU handles CCS decompression + detiling).
     * All planes (main surface + CCS + clear color) are in the same BO,
     * so the same prime_fd is used with different offsets. */
    EGLAttrib img_attrs[64];
    int ai = 0;
    EGLAttrib mod_lo = (EGLAttrib)(fb->modifier & 0xffffffff);
    EGLAttrib mod_hi = (EGLAttrib)(fb->modifier >> 32);

    static const EGLenum plane_fd[]    = { EGL_DMA_BUF_PLANE0_FD_EXT,          EGL_DMA_BUF_PLANE1_FD_EXT,          EGL_DMA_BUF_PLANE2_FD_EXT          };
    static const EGLenum plane_offset[]= { EGL_DMA_BUF_PLANE0_OFFSET_EXT,      EGL_DMA_BUF_PLANE1_OFFSET_EXT,      EGL_DMA_BUF_PLANE2_OFFSET_EXT      };
    static const EGLenum plane_pitch[] = { EGL_DMA_BUF_PLANE0_PITCH_EXT,       EGL_DMA_BUF_PLANE1_PITCH_EXT,       EGL_DMA_BUF_PLANE2_PITCH_EXT       };
    static const EGLenum plane_mod_lo[]= { EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT };
    static const EGLenum plane_mod_hi[]= { EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT };

    img_attrs[ai++] = EGL_WIDTH;              img_attrs[ai++] = (EGLAttrib)fb->width;
    img_attrs[ai++] = EGL_HEIGHT;             img_attrs[ai++] = (EGLAttrib)fb->height;
    img_attrs[ai++] = EGL_LINUX_DRM_FOURCC_EXT; img_attrs[ai++] = (EGLAttrib)fb->pixel_format;

    for (int i = 0; i < 3; i++) {
        if (!fb->pitches[i] && i > 0) break;
        img_attrs[ai++] = plane_fd[i];     img_attrs[ai++] = (EGLAttrib)prime_fd;
        img_attrs[ai++] = plane_offset[i]; img_attrs[ai++] = (EGLAttrib)fb->offsets[i];
        img_attrs[ai++] = plane_pitch[i];  img_attrs[ai++] = (EGLAttrib)fb->pitches[i];
        img_attrs[ai++] = plane_mod_lo[i]; img_attrs[ai++] = mod_lo;
        img_attrs[ai++] = plane_mod_hi[i]; img_attrs[ai++] = mod_hi;
    }
    img_attrs[ai] = EGL_NONE;
    EGLImage img = eglCreateImage(dpy, EGL_NO_CONTEXT,
                                  EGL_LINUX_DMA_BUF_EXT, NULL, img_attrs);
    if (img == EGL_NO_IMAGE) {
        fprintf(stderr, "eglCreateImage failed: 0x%x\n", eglGetError());
        return 1;
    }

    /* Bind EGL image as an external OES texture */
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, (GLeglImageOES)img);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    /* Create linear RGBA8 FBO as render target */
    GLuint fbo, rb;
    glGenFramebuffers(1, &fbo);
    glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, fb->width, fb->height);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "FBO not complete\n"); return 1;
    }

    /* Compile and link shaders */
    GLuint prog = glCreateProgram();
    glAttachShader(prog, compile_shader(GL_VERTEX_SHADER,   vert_src));
    glAttachShader(prog, compile_shader(GL_FRAGMENT_SHADER, frag_src));
    glLinkProgram(prog);
    glUseProgram(prog);
    glUniform1i(glGetUniformLocation(prog, "tex"), 0);

    /* Draw fullscreen quad → GPU decodes CCS into linear FBO */
    glViewport(0, 0, fb->width, fb->height);
    static const float verts[] = { -1,-1, 1,-1, -1,1, 1,1 };
    GLint loc = glGetAttribLocation(prog, "pos");
    glEnableVertexAttribArray(loc);
    glVertexAttribPointer(loc, 2, GL_FLOAT, GL_FALSE, 0, verts);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glFinish();

    /* Read linear pixels back to CPU */
    size_t size = (size_t)fb->width * fb->height * 4;
    void *pixels = malloc(size);
    glReadPixels(0, 0, fb->width, fb->height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    FILE *f = fopen("/tmp/scanout.raw", "wb");
    fwrite(pixels, 1, size, f);
    fclose(f);
    free(pixels);
    close(prime_fd);

    printf("method=egl raw_fmt=rgba raw_pitch=%d\n", fb->width * 4);
    printf("dumped %zu bytes to /tmp/scanout.raw\n", size);
    return 0;
}

/* ── Main ────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <card_name> <crtc_id>\n", argv[0]);
        return 1;
    }

    char path[64];
    snprintf(path, sizeof(path), "/dev/dri/%s", argv[1]);
    int fd = open(path, O_RDWR);
    if (fd < 0) { perror("open"); return 1; }

    drmModeCrtc *crtc = drmModeGetCrtc(fd, atoi(argv[2]));
    if (!crtc) { fprintf(stderr, "GetCrtc failed: %s\n", strerror(errno)); return 1; }
    printf("fb_id=%d size=%dx%d\n", crtc->buffer_id, crtc->width, crtc->height);

    drmModeFB2 *fb = drmModeGetFB2(fd, crtc->buffer_id);
    if (!fb) { fprintf(stderr, "GetFB2 failed: %s\n", strerror(errno)); return 1; }
    printf("fb: %dx%d pitch=%d handle=%d fmt=%c%c%c%c modifier=0x%llx\n",
           fb->width, fb->height, fb->pitches[0], fb->handles[0],
           (fb->pixel_format >>  0) & 0xff, (fb->pixel_format >>  8) & 0xff,
           (fb->pixel_format >> 16) & 0xff, (fb->pixel_format >> 24) & 0xff,
           (unsigned long long)fb->modifier);
    for (int i = 0; i < 4; i++) {
        if (fb->handles[i] || fb->pitches[i])
            printf("  plane[%d] handle=%d pitch=%d offset=%d\n",
                   i, fb->handles[i], fb->pitches[i], fb->offsets[i]);
    }
    fflush(stdout);

    if (fb->handles[0] == 0) {
        fprintf(stderr, "handle=0, need DRM master\n");
        return 1;
    }

    int ret;
    if (fb->modifier == DRM_FORMAT_MOD_LINEAR || fb->modifier == DRM_FORMAT_MOD_INVALID) {
        printf("path=linear (dumb buffer / FBC)\n");
        ret = dump_linear(fd, fb);
    } else {
        printf("path=egl (CCS/tiled modifier)\n");
        ret = dump_egl(fd, fb);
    }

    drmModeFreeFB2(fb);
    drmModeFreeCrtc(crtc);
    close(fd);
    return ret;
}
