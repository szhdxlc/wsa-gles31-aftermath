// glesprobe: dump what the guest GLES driver actually reports.
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES3/gl3.h>
#include <stdio.h>
#include <string.h>

#ifndef GL_MAJOR_VERSION
#define GL_MAJOR_VERSION 0x821B
#endif
#ifndef GL_MINOR_VERSION
#define GL_MINOR_VERSION 0x821C
#endif
#ifndef GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS
#define GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS 0x90EB
#endif
#ifndef GL_MAX_SHADER_STORAGE_BLOCK_SIZE
#define GL_MAX_SHADER_STORAGE_BLOCK_SIZE 0x90DE
#endif

int main(void) {
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY) { printf("no display\n"); return 1; }
    EGLint maj, min;
    if (!eglInitialize(dpy, &maj, &min)) { printf("eglInitialize failed 0x%x\n", eglGetError()); return 1; }
    printf("EGL version: %d.%d\n", maj, min);
    printf("EGL_VENDOR: %s\n", eglQueryString(dpy, EGL_VENDOR));
    printf("EGL_VERSION: %s\n", eglQueryString(dpy, EGL_VERSION));
    printf("EGL_CLIENT_APIS: %s\n", eglQueryString(dpy, EGL_CLIENT_APIS));

    EGLint cfgAttr[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
                        EGL_NONE};
    EGLConfig cfg; EGLint n = 0;
    if (!eglChooseConfig(dpy, cfgAttr, &cfg, 1, &n) || n < 1) {
        printf("eglChooseConfig failed\n"); return 1;
    }
    EGLint ctxAttr[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxAttr);
    if (ctx == EGL_NO_CONTEXT) { printf("eglCreateContext failed 0x%x\n", eglGetError()); return 1; }
    EGLint pbAttr[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pbAttr);
    if (surf == EGL_NO_SURFACE) { printf("pbuffer failed 0x%x\n", eglGetError()); return 1; }
    if (!eglMakeCurrent(dpy, surf, surf, ctx)) {
        printf("eglMakeCurrent(surf) failed 0x%x\n", eglGetError()); return 1;
    }
    printf("GL_VENDOR: %s\n", (const char *)glGetString(GL_VENDOR));
    printf("GL_RENDERER: %s\n", (const char *)glGetString(GL_RENDERER));
    printf("GL_VERSION: %s\n", (const char *)glGetString(GL_VERSION));
    printf("GL_SHADING_LANGUAGE_VERSION: %s\n", (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION));

    GLint v = -1;
    glGetIntegerv(GL_MAJOR_VERSION, &v); printf("GL_MAJOR_VERSION: %d (err 0x%x)\n", v, glGetError());
    v = -1;
    glGetIntegerv(GL_MINOR_VERSION, &v); printf("GL_MINOR_VERSION: %d (err 0x%x)\n", v, glGetError());
    v = -1;
    glGetIntegerv(GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS, &v);
    printf("GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS: %d (err 0x%x)\n", v, glGetError());
    v = -1;
    glGetIntegerv(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, &v);
    printf("GL_MAX_SHADER_STORAGE_BLOCK_SIZE: %d (err 0x%x)\n", v, glGetError());
    v = -1;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &v);
    printf("GL_MAX_TEXTURE_SIZE: %d\n", v);

    const char *ext = (const char *)glGetString(GL_EXTENSIONS);
    printf("EXT count: %d\n", ext ? (int)(strlen(ext)) : -1);
    return 0;
}
