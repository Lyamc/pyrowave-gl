#include "gl_api.hpp"

#include <dlfcn.h>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <string>

GlApiFns gl;

namespace {

constexpr unsigned EGL_OPENGL_API = 0x30A2;
constexpr unsigned EGL_NONE = 0x3038;
constexpr int EGL_SUCCESS = 0x3000;
constexpr unsigned EGL_CONTEXT_MAJOR_VERSION = 0x3098;
constexpr unsigned EGL_CONTEXT_MINOR_VERSION = 0x30FB;
constexpr unsigned EGL_CONTEXT_OPENGL_PROFILE_MASK = 0x30FD;
constexpr unsigned EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT = 0x00000001;
constexpr unsigned EGL_PLATFORM_DEVICE_EXT = 0x313F;
constexpr unsigned EGL_SURFACE_TYPE = 0x3033;
constexpr unsigned EGL_PBUFFER_BIT = 0x0001;
constexpr unsigned EGL_RENDERABLE_TYPE = 0x3040;
constexpr unsigned EGL_OPENGL_BIT = 0x0008;
constexpr unsigned EGL_NONE_DONE = 0x3038;
constexpr unsigned EGL_WIDTH = 0x3057;
constexpr unsigned EGL_HEIGHT = 0x3056;
constexpr unsigned EGL_RED_SIZE = 0x3024;
constexpr unsigned EGL_BLUE_SIZE = 0x3022;
constexpr unsigned EGL_GREEN_SIZE = 0x3023;

using EGLDisplay = void *;
using EGLConfig = void *;
using EGLContext = void *;
using EGLSurface = void *;
using EGLDeviceEXT = void *;
using EGLint = int32_t;
using EGLBoolean = unsigned int;
using EGLAttrib = intptr_t;

constexpr EGLDisplay EGL_NO_DISPLAY = nullptr;
constexpr EGLContext EGL_NO_CONTEXT = nullptr;
constexpr EGLSurface EGL_NO_SURFACE = nullptr;
constexpr EGLConfig EGL_NO_CONFIG = nullptr;

using PFN_eglGetProcAddress = void *(*)(const char *);
using PFN_eglGetDisplay = EGLDisplay (*)(void *);
using PFN_eglInitialize = EGLBoolean (*)(EGLDisplay, EGLint *, EGLint *);
using PFN_eglTerminate = EGLBoolean (*)(EGLDisplay);
using PFN_eglBindAPI = EGLBoolean (*)(unsigned int);
using PFN_eglCreateContext = EGLContext (*)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
using PFN_eglDestroyContext = EGLBoolean (*)(EGLDisplay, EGLContext);
using PFN_eglMakeCurrent = EGLBoolean (*)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
using PFN_eglQueryString = const char *(*)(EGLDisplay, EGLint);
using PFN_eglGetError = EGLint (*)(void);
using PFN_eglQueryDevicesEXT = EGLBoolean (*)(EGLint, EGLDeviceEXT *, EGLint *);
using PFN_eglGetPlatformDisplayEXT = EGLDisplay (*)(EGLint, void *, const EGLAttrib *);
using PFN_eglChooseConfig = EGLBoolean (*)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
using PFN_eglCreatePbufferSurface = EGLSurface (*)(EGLDisplay, EGLConfig, const EGLint *);
using PFN_eglDestroySurface = EGLBoolean (*)(EGLDisplay, EGLSurface);

void *lib = nullptr;
EGLDisplay dpy = nullptr;
EGLContext ctx = nullptr;
EGLSurface surf = nullptr;

PFN_eglTerminate eglTerminate_ = nullptr;
PFN_eglMakeCurrent eglMakeCurrent_ = nullptr;
PFN_eglDestroyContext eglDestroyContext_ = nullptr;
PFN_eglDestroySurface eglDestroySurface_ = nullptr;

void set_err(char *err, size_t n, const char *msg)
{
    if (!err || n == 0) return;
    std::snprintf(err, n, "%s", msg);
}

bool load_gl(PFN_eglGetProcAddress get_proc, char *err, size_t err_len)
{
    struct Item { const char *name; void **dst; };
    Item items[] = {
        {"glCreateShader", (void **)&gl.CreateShader},
        {"glShaderSource", (void **)&gl.ShaderSource},
        {"glCompileShader", (void **)&gl.CompileShader},
        {"glGetShaderiv", (void **)&gl.GetShaderiv},
        {"glGetShaderInfoLog", (void **)&gl.GetShaderInfoLog},
        {"glDeleteShader", (void **)&gl.DeleteShader},
        {"glCreateProgram", (void **)&gl.CreateProgram},
        {"glAttachShader", (void **)&gl.AttachShader},
        {"glLinkProgram", (void **)&gl.LinkProgram},
        {"glGetProgramiv", (void **)&gl.GetProgramiv},
        {"glGetProgramInfoLog", (void **)&gl.GetProgramInfoLog},
        {"glUseProgram", (void **)&gl.UseProgram},
        {"glDeleteProgram", (void **)&gl.DeleteProgram},
        {"glGenBuffers", (void **)&gl.GenBuffers},
        {"glBindBuffer", (void **)&gl.BindBuffer},
        {"glBufferData", (void **)&gl.BufferData},
        {"glBufferSubData", (void **)&gl.BufferSubData},
        {"glGetBufferSubData", (void **)&gl.GetBufferSubData},
        {"glBindBufferBase", (void **)&gl.BindBufferBase},
        {"glMapBufferRange", (void **)&gl.MapBufferRange},
        {"glUnmapBuffer", (void **)&gl.UnmapBuffer},
        {"glDeleteBuffers", (void **)&gl.DeleteBuffers},
        {"glDispatchCompute", (void **)&gl.DispatchCompute},
        {"glMemoryBarrier", (void **)&gl.MemoryBarrier},
        {"glGetError", (void **)&gl.GetError},
        {"glGetString", (void **)&gl.GetString},
        {"glGetIntegerv", (void **)&gl.GetIntegerv},
        {"glClearBufferSubData", (void **)&gl.ClearBufferSubData},
        {"glFinish", (void **)&gl.Finish},
    };
    for (auto &it : items) {
        void *fn = get_proc(it.name);
        if (!fn) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "missing %s", it.name);
            set_err(err, err_len, buf);
            return false;
        }
        *it.dst = fn;
    }
    return true;
}

void *try_dlopen(const char *path)
{
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) return nullptr;
    if (!dlsym(h, "eglGetProcAddress") || !dlsym(h, "eglInitialize")) {
        dlclose(h);
        return nullptr;
    }
    return h;
}

/* NixOS keeps libglvnd off the default linker path. The NVIDIA vendor
 * library's RUNPATH names that store directory. */
void *open_glvnd_from_vendor(const char *vendor_so)
{
    std::ifstream in(vendor_so, std::ios::binary);
    if (!in) return nullptr;
    std::string blob((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::string key = "libglvnd";
    size_t at = blob.find(key);
    if (at == std::string::npos) return nullptr;
    size_t begin = blob.rfind(':', at);
    begin = (begin == std::string::npos) ? 0 : begin + 1;
    size_t end = blob.find(':', at);
    if (end == std::string::npos) end = blob.find('\0', at);
    if (end == std::string::npos || end <= begin) return nullptr;
    std::string dir = blob.substr(begin, end - begin);
    if (dir.empty() || dir[0] != '/') return nullptr;
    return try_dlopen((dir + "/libEGL.so.1").c_str());
}

void *open_egl(void)
{
    if (!std::getenv("__EGL_VENDOR_LIBRARY_FILENAMES")) {
        const char *nvidia_json = "/run/opengl-driver/share/glvnd/egl_vendor.d/10_nvidia.json";
        if (std::ifstream(nvidia_json)) {
            setenv("__EGL_VENDOR_LIBRARY_FILENAMES", nvidia_json, 0);
        }
    }
    if (void *h = try_dlopen("libEGL.so.1")) return h;
    if (void *h = open_glvnd_from_vendor("/run/opengl-driver/lib/libEGL_nvidia.so.0")) return h;
    return nullptr;
}

} // namespace

int gl_init(int adapter_index, int allow_software, char *renderer, size_t renderer_len, char *err, size_t err_len)
{
    if (ctx) return 0;
    lib = open_egl();
    if (!lib) {
        set_err(err, err_len, "dlopen libEGL failed");
        return -1;
    }
    auto get_proc = (PFN_eglGetProcAddress)dlsym(lib, "eglGetProcAddress");
    auto eglGetDisplay_ = (PFN_eglGetDisplay)dlsym(lib, "eglGetDisplay");
    auto eglInitialize_ = (PFN_eglInitialize)dlsym(lib, "eglInitialize");
    auto eglBindAPI_ = (PFN_eglBindAPI)dlsym(lib, "eglBindAPI");
    auto eglCreateContext_ = (PFN_eglCreateContext)dlsym(lib, "eglCreateContext");
    eglMakeCurrent_ = (PFN_eglMakeCurrent)dlsym(lib, "eglMakeCurrent");
    eglTerminate_ = (PFN_eglTerminate)dlsym(lib, "eglTerminate");
    eglDestroyContext_ = (PFN_eglDestroyContext)dlsym(lib, "eglDestroyContext");
    eglDestroySurface_ = (PFN_eglDestroySurface)dlsym(lib, "eglDestroySurface");
    auto eglQueryString_ = (PFN_eglQueryString)dlsym(lib, "eglQueryString");
    auto eglGetError_ = (PFN_eglGetError)dlsym(lib, "eglGetError");
    if (!get_proc || !eglInitialize_ || !eglBindAPI_ || !eglCreateContext_ || !eglMakeCurrent_) {
        set_err(err, err_len, "EGL entry points missing");
        return -1;
    }

    auto query_devices = (PFN_eglQueryDevicesEXT)get_proc("eglQueryDevicesEXT");
    if (!query_devices) query_devices = (PFN_eglQueryDevicesEXT)dlsym(lib, "eglQueryDevicesEXT");
    auto get_platform = (PFN_eglGetPlatformDisplayEXT)get_proc("eglGetPlatformDisplayEXT");
    if (!get_platform) get_platform = (PFN_eglGetPlatformDisplayEXT)dlsym(lib, "eglGetPlatformDisplayEXT");

    if (query_devices && get_platform) {
        EGLDeviceEXT devices[8];
        EGLint ndev = 0;
        if (query_devices(8, devices, &ndev) && ndev > 0) {
            int index = adapter_index < 0 ? 0 : adapter_index;
            if (index >= ndev) {
                set_err(err, err_len, "adapter_index out of range");
                return -1;
            }
            dpy = get_platform(EGL_PLATFORM_DEVICE_EXT, devices[index], nullptr);
        }
    }
    if (!dpy && eglGetDisplay_) dpy = eglGetDisplay_(EGL_NO_DISPLAY);
    if (!dpy) {
        set_err(err, err_len, "egl display failed");
        return -1;
    }
    EGLint major = 0, minor = 0;
    if (!eglInitialize_(dpy, &major, &minor)) {
        set_err(err, err_len, "eglInitialize failed");
        dpy = nullptr;
        return -1;
    }
    if (!eglBindAPI_(EGL_OPENGL_API)) {
        set_err(err, err_len, "eglBindAPI(OpenGL) failed");
        return -1;
    }

    const EGLint attribs[] = {
        EGLint(EGL_CONTEXT_MAJOR_VERSION), 4,
        EGLint(EGL_CONTEXT_MINOR_VERSION), 3,
        EGLint(EGL_CONTEXT_OPENGL_PROFILE_MASK), EGLint(EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT),
        EGLint(EGL_NONE),
    };
    ctx = eglCreateContext_(dpy, EGL_NO_CONFIG, EGL_NO_CONTEXT, attribs);
    if (ctx && eglMakeCurrent_(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
        /* surfaceless core context */
    } else {
        if (ctx && eglDestroyContext_) eglDestroyContext_(dpy, ctx);
        ctx = nullptr;
        auto choose = (PFN_eglChooseConfig)dlsym(lib, "eglChooseConfig");
        auto pbuffer = (PFN_eglCreatePbufferSurface)dlsym(lib, "eglCreatePbufferSurface");
        if (!choose || !pbuffer) {
            set_err(err, err_len, "no surfaceless context and no pbuffer");
            return -1;
        }
        const EGLint cfg_attr[] = {
            EGLint(EGL_SURFACE_TYPE), EGLint(EGL_PBUFFER_BIT),
            EGLint(EGL_RENDERABLE_TYPE), EGLint(EGL_OPENGL_BIT),
            EGLint(EGL_RED_SIZE), 8,
            EGLint(EGL_GREEN_SIZE), 8,
            EGLint(EGL_BLUE_SIZE), 8,
            EGLint(EGL_NONE_DONE),
        };
        EGLConfig cfg = nullptr;
        EGLint ncfg = 0;
        if (!choose(dpy, cfg_attr, &cfg, 1, &ncfg) || ncfg < 1) {
            set_err(err, err_len, "eglChooseConfig failed");
            return -1;
        }
        ctx = eglCreateContext_(dpy, cfg, EGL_NO_CONTEXT, attribs);
        const EGLint pb[] = {EGLint(EGL_WIDTH), 16, EGLint(EGL_HEIGHT), 16, EGLint(EGL_NONE)};
        surf = pbuffer(dpy, cfg, pb);
        if (!ctx || !surf || !eglMakeCurrent_(dpy, surf, surf, ctx)) {
            char buf[80];
            std::snprintf(buf, sizeof(buf), "context failed eglGetError=0x%x", eglGetError_ ? eglGetError_() : 0);
            set_err(err, err_len, buf);
            return -1;
        }
    }

    if (!load_gl(get_proc, err, err_len)) return -1;
    GLint vmaj = 0, vmin = 0;
    gl.GetIntegerv(GL_MAJOR_VERSION, &vmaj);
    gl.GetIntegerv(GL_MINOR_VERSION, &vmin);
    if (vmaj < 4 || (vmaj == 4 && vmin < 3)) {
        set_err(err, err_len, "OpenGL 4.3 is required");
        return -1;
    }
    const char *rend = (const char *)gl.GetString(GL_RENDERER);
    if (!rend) rend = "";
    if (renderer && renderer_len) std::snprintf(renderer, renderer_len, "%s", rend);
    if (!allow_software) {
        if (std::strstr(rend, "llvmpipe") || std::strstr(rend, "softpipe") || std::strstr(rend, "SwiftShader")) {
            set_err(err, err_len, "software renderer rejected");
            return -1;
        }
    }
    (void)EGL_SUCCESS;
    (void)eglQueryString_;
    return 0;
}

void gl_shutdown(void)
{
    if (dpy && eglMakeCurrent_) eglMakeCurrent_(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (dpy && surf && eglDestroySurface_) eglDestroySurface_(dpy, surf);
    if (dpy && ctx && eglDestroyContext_) eglDestroyContext_(dpy, ctx);
    if (dpy && eglTerminate_) eglTerminate_(dpy);
    surf = nullptr;
    ctx = nullptr;
    dpy = nullptr;
    if (lib) dlclose(lib);
    lib = nullptr;
    gl = {};
}
