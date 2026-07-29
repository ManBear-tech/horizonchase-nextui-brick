/*
 * egl_shim.h -- adaptive EGL/GLES wrapper backed by SDL2
 */

#ifndef __EGL_SHIM_H__
#define __EGL_SHIM_H__

#include <stdint.h>

typedef void *EGLDisplay;
typedef void *EGLSurface;
typedef void *EGLContext;
typedef void *EGLConfig;
typedef void *EGLNativeDisplayType;
typedef void *EGLNativeWindowType;
typedef unsigned int EGLBoolean;
typedef int32_t EGLint;

#define EGL_TRUE 1
#define EGL_FALSE 0
#define EGL_SUCCESS 0x3000
#define EGL_DEFAULT_DISPLAY ((EGLNativeDisplayType)0)
#define EGL_NO_DISPLAY ((EGLDisplay)0)
#define EGL_NO_CONTEXT ((EGLContext)0)
#define EGL_NO_SURFACE ((EGLSurface)0)

// EGL attribute constants
#define EGL_OPENGL_ES_API 0x30A0
#define EGL_OPENGL_ES2_BIT 0x0004
#define EGL_CONTEXT_CLIENT_VERSION 0x3098

EGLDisplay egl_shim_GetDisplay(EGLNativeDisplayType display_id);
EGLBoolean egl_shim_Initialize(EGLDisplay dpy, EGLint *major, EGLint *minor);
EGLBoolean egl_shim_Terminate(EGLDisplay dpy);
EGLBoolean egl_shim_ChooseConfig(EGLDisplay dpy, const EGLint *attrib_list,
                                  EGLConfig *configs, EGLint config_size,
                                  EGLint *num_config);
EGLSurface egl_shim_CreateWindowSurface(EGLDisplay dpy, EGLConfig config,
                                         EGLNativeWindowType win,
                                         const EGLint *attrib_list);
EGLSurface egl_shim_CreatePbufferSurface(EGLDisplay dpy, EGLConfig config,
                                          const EGLint *attrib_list);
EGLContext egl_shim_CreateContext(EGLDisplay dpy, EGLConfig config,
                                  EGLContext share_context,
                                  const EGLint *attrib_list);
EGLBoolean egl_shim_MakeCurrent(EGLDisplay dpy, EGLSurface draw,
                                 EGLSurface read, EGLContext ctx);
EGLBoolean egl_shim_SwapBuffers(EGLDisplay dpy, EGLSurface surface);
EGLBoolean egl_shim_DestroySurface(EGLDisplay dpy, EGLSurface surface);
EGLBoolean egl_shim_DestroyContext(EGLDisplay dpy, EGLContext ctx);
EGLBoolean egl_shim_QuerySurface(EGLDisplay dpy, EGLSurface surface,
                                  EGLint attribute, EGLint *value);
EGLBoolean egl_shim_GetConfigAttrib(EGLDisplay dpy, EGLConfig config,
                                     EGLint attribute, EGLint *value);
EGLint egl_shim_GetError(void);
void *egl_shim_GetProcAddress(const char *procname);
EGLBoolean egl_shim_BindAPI(unsigned int api);
const char *egl_shim_QueryString(EGLDisplay dpy, EGLint name);
EGLBoolean egl_shim_SwapInterval(EGLDisplay dpy, EGLint interval);
EGLContext egl_shim_GetCurrentContext(void);
EGLSurface egl_shim_GetCurrentSurface(EGLint readdraw);
EGLBoolean egl_shim_SurfaceAttrib(EGLDisplay dpy, EGLSurface s, EGLint a,
                                  EGLint v);

// Access SDL window from outside (for ANativeWindow shim)
struct SDL_Window *egl_shim_get_window(void);
void egl_shim_get_size(int *width, int *height);

// Pre-create the SDL window+context from main thread
void egl_shim_create_window(void);

// Mutex hooks — call from pthread_mutex_lock/unlock wrappers
// to detect outermost EndCriticalSectionGL and release GL
void egl_shim_on_mutex_post_lock(void *mutex_id);
void egl_shim_on_mutex_pre_unlock(void *mutex_id);
int egl_shim_ensure_current(void);

#endif
