#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define VGX_VITA_NO_GL_ROUTING
#include <GLES2/gl2.h>
#include <psp2/io/fcntl.h>
#include <vitashark.h>
#include "utils/shacccg_paramquery.h"
#include "frozen-shader-types.h"

#ifdef __cplusplus
extern "C" {
#endif
extern shader* shaders[8];
extern bool is_shark_online;
extern vglSemanticMode glsl_sema_mode;
extern shark_opt compiler_opts;
extern int32_t compiler_fastmath, compiler_fastprecision, compiler_fastint;
extern SceGxmShaderPatcher* gxm_shader_patcher;
#ifdef CACHE_WEAK_ROOT
extern char vgl_shader_cache_path[256] __attribute__((weak));
#else
extern char vgl_shader_cache_path[256];
#endif
extern char* shark_log;
bool start_shader_compiler(void);
void glsl_translator_process(shader* s);
void vgl_free(void* memory);
void* vgl_fast_memcpy(void* destination, const void* source, size_t bytes);
void vgl_log(const char* format, ...);
const char* get_gxm_error_literal(int code);
uint32_t* vglProgramGetParameterBase(const SceGxmProgram* program);
void shader_fixture_gl_error(GLenum error);
void frozen_glShaderSource(GLuint handle, GLsizei count, const GLchar* const* strings, const GLint* lengths);
#ifdef __cplusplus
}
#endif
#define THREAD_SAFE()
#define SET_GL_ERROR(error) { shader_fixture_gl_error(error); return; }
