#pragma once

#include <cstddef>
#include <cstdint>

/* GL 4.3 entry points this encoder calls. Loaded from EGL so the library
 * does not link libGL.
 */

typedef uint32_t GLenum;
typedef uint32_t GLuint;
typedef int32_t GLint;
typedef int32_t GLsizei;
typedef uint32_t GLbitfield;
typedef char GLchar;
typedef intptr_t GLintptr;
typedef ptrdiff_t GLsizeiptr;
typedef uint8_t GLboolean;
typedef uint8_t GLubyte;

#define GL_COMPUTE_SHADER 0x91B9
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_INFO_LOG_LENGTH 0x8B84
#define GL_SHADER_STORAGE_BUFFER 0x90D2
#define GL_UNIFORM_BUFFER 0x8A11
#define GL_DYNAMIC_DRAW 0x88E8
#define GL_STREAM_READ 0x88E1
#define GL_MAP_READ_BIT 0x0001
#define GL_COPY_READ_BUFFER 0x8F36
#define GL_COPY_WRITE_BUFFER 0x8F37
#define GL_PIXEL_PACK_BUFFER 0x88EB
#define GL_SHADER_STORAGE_BARRIER_BIT 0x00002000
#define GL_BUFFER_UPDATE_BARRIER_BIT 0x00000200
#define GL_R32UI 0x8236
#define GL_RED_INTEGER 0x8D94
#define GL_UNSIGNED_INT 0x1405
#define GL_MAJOR_VERSION 0x821B
#define GL_MINOR_VERSION 0x821C
#define GL_RENDERER 0x1F01
#define GL_VENDOR 0x1F00

struct GlApiFns {
    GLuint (*CreateShader)(GLenum);
    void (*ShaderSource)(GLuint, GLsizei, const GLchar *const *, const GLint *);
    void (*CompileShader)(GLuint);
    void (*GetShaderiv)(GLuint, GLenum, GLint *);
    void (*GetShaderInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
    void (*DeleteShader)(GLuint);
    GLuint (*CreateProgram)(void);
    void (*AttachShader)(GLuint, GLuint);
    void (*LinkProgram)(GLuint);
    void (*GetProgramiv)(GLuint, GLenum, GLint *);
    void (*GetProgramInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
    void (*UseProgram)(GLuint);
    void (*DeleteProgram)(GLuint);
    void (*GenBuffers)(GLsizei, GLuint *);
    void (*BindBuffer)(GLenum, GLuint);
    void (*BufferData)(GLenum, GLsizeiptr, const void *, GLenum);
    void (*BufferSubData)(GLenum, GLintptr, GLsizeiptr, const void *);
    void (*GetBufferSubData)(GLenum, GLintptr, GLsizeiptr, void *);
    void (*CopyBufferSubData)(GLenum, GLenum, GLintptr, GLintptr, GLsizeiptr);
    void (*BindBufferBase)(GLenum, GLuint, GLuint);
    void *(*MapBufferRange)(GLenum, GLintptr, GLsizeiptr, GLbitfield);
    GLboolean (*UnmapBuffer)(GLenum);
    void (*DeleteBuffers)(GLsizei, const GLuint *);
    void (*DispatchCompute)(GLuint, GLuint, GLuint);
    void (*MemoryBarrier)(GLbitfield);
    GLenum (*GetError)(void);
    const GLubyte *(*GetString)(GLenum);
    void (*GetIntegerv)(GLenum, GLint *);
    void (*ClearBufferSubData)(GLenum, GLenum, GLintptr, GLsizeiptr, GLenum, GLenum, const void *);
    void (*Finish)(void);
};

extern GlApiFns gl;

/* Opens a surfaceless GL 4.3 core context. Returns 0 on success. */
int gl_init(int adapter_index, int allow_software, char *renderer, size_t renderer_len, char *err, size_t err_len);
void gl_shutdown(void);
