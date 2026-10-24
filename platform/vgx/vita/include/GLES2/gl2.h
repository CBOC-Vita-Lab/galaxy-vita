/* <GLES2/gl2.h> for the PS Vita build of vgx: vitaGL in place of a GLES2
 * driver. The renderer's sources include this header unchanged; everything
 * vitaGL lacks or does differently is dealt with here and in
 * vgx_host_vita.cpp, so vgx/src needs no platform conditionals.
 *
 * Three GL entry points are routed through the Vita host layer:
 *   glEnable/glDisable the host shadows the scissor enable, which its present
 *                      blit has to switch off and put back, and drops
 *                      GL_DITHER, which vitaGL does not know
 *   glVertexAttrib4f   vitaGL keeps the constants of disabled attribute
 *                      arrays in a ring it reuses; the host re-issues them in
 *                      time (see vgxvita_glVertexAttrib4f)
 *   glCompressedTexImage2D, glGenerateMipmap
 *                      vitaGL hands these to the GPU's transfer unit and goes on
 *                      without waiting; the host waits for the transfer (see
 *                      vgxvita_glCompressedTexImage2D)
 *   glScissor          the host keeps the rectangle and, while the scissor is off,
 *                      parks vitaGL's copy at (0,0,1,1): see vgxvita_glScissor
 *   glCreateShader, glShaderSource
 *                      optional RAM-only shader identity annotations; normal builds forward
 *                      directly. Cache directories are prepared at actual write misses.
 *   glDepthFunc, glDepthMask, glDrawElements, glDrawArrays, glClear
 *                      only for GXV_VITA_MASK_GUARD (off by default; one byte
 *                      is tested per draw): see vgxvita_maskGuard
 * (The present itself is the back end's present hook, see vgx_host_vita.cpp.)
 */
#pragma once
#include <vitaGL.h>

#ifndef GL_DITHER
#define GL_DITHER 0x0BD0
#endif
#ifndef GL_DEPTH_COMPONENT24_OES
#define GL_DEPTH_COMPONENT24_OES GL_DEPTH_COMPONENT24
#endif

#ifdef __cplusplus
extern "C" {
#endif
void vgxvita_glEnable(GLenum cap);
void vgxvita_glDisable(GLenum cap);
void vgxvita_glVertexAttrib4f(GLuint index, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3);
void vgxvita_glCompressedTexImage2D(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLint border,
                                    GLsizei imageSize, const void* data);
void vgxvita_glGenerateMipmap(GLenum target);
void vgxvita_glScissor(GLint x, GLint y, GLsizei width, GLsizei height);
void vgxvita_glDepthFunc(GLenum func);
void vgxvita_glDepthMask(GLboolean flag);
void vgxvita_maskGuard(void);
extern unsigned char vgxvita_maskGuardOn;
extern unsigned char vgxvita_maskGuardDue; /* set by the back end when vitaGL is about to redraw its scissor mask (or 2: before every draw) */
GLuint vgxvita_glCreateShader(GLenum type);
void vgxvita_glShaderSource(GLuint shader, GLsizei count, const GLchar* const* string, const GLint* length);
void vgxvita_cacheFolderStats(unsigned* made, unsigned* ms);
#ifdef __cplusplus
}
#endif

#ifndef VGX_VITA_NO_GL_ROUTING
#define glEnable vgxvita_glEnable
#define glDisable vgxvita_glDisable
#define glVertexAttrib4f vgxvita_glVertexAttrib4f
#define glCompressedTexImage2D vgxvita_glCompressedTexImage2D
#define glGenerateMipmap vgxvita_glGenerateMipmap
#define glScissor vgxvita_glScissor
#define glDepthFunc vgxvita_glDepthFunc
#define glDepthMask vgxvita_glDepthMask
#define glCreateShader vgxvita_glCreateShader
#define glShaderSource vgxvita_glShaderSource
/* (a function-like macro is not expanded again inside its own replacement: the inner names are vitaGL's) */
#define glDrawElements(mode, count, type, indices) (((vgxvita_maskGuardOn && vgxvita_maskGuardDue) ? vgxvita_maskGuard() : (void)0), glDrawElements(mode, count, type, indices))
#define glDrawArrays(mode, first, count) (((vgxvita_maskGuardOn && vgxvita_maskGuardDue) ? vgxvita_maskGuard() : (void)0), glDrawArrays(mode, first, count))
#define glClear(bits) (((vgxvita_maskGuardOn && vgxvita_maskGuardDue) ? vgxvita_maskGuard() : (void)0), glClear(bits))
#endif
