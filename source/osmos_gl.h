/* osmos_gl.h -- see osmos_gl.c. MIT licensed. */
#ifndef OSMOS_GL_H
#define OSMOS_GL_H

#include <GLES2/gl2.h>
#include "config.h"

#if !OSMOS_DIAG
/* Diagnostics off: the import table resolves straight to the real GL entry
 * points. Not a cheaper wrapper -- no wrapper at all, so there is no per-draw
 * cost. The tracing that found the FBO bug put a call on every glDrawArrays,
 * which at 60 batches a frame is exactly the kind of thing that stutters. */
#define glViewport_diag          glViewport
#define glBindFramebuffer_diag   glBindFramebuffer
#define glColorMask_diag         glColorMask
#define glClearColor_diag        glClearColor
#define glClear_diag             glClear
#define glDrawArrays_diag        glDrawArrays
#define glUniformMatrix4fv_diag  glUniformMatrix4fv
#define glUniform4f_diag         glUniform4f
#define glUniform4fv_diag        glUniform4fv
#define glBlendFunc_diag         glBlendFunc
#define glEnable_diag            glEnable
#define glDisable_diag           glDisable

#define osmos_gl_frame()         ((void)0)

#else

void glViewport_diag(GLint x, GLint y, GLsizei w, GLsizei h);
void glBindFramebuffer_diag(GLenum target, GLuint fb);
void glColorMask_diag(GLboolean r, GLboolean g, GLboolean b, GLboolean a);
void glClearColor_diag(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
void glClear_diag(GLbitfield mask);
void glDrawArrays_diag(GLenum mode, GLint first, GLsizei count);

void glUniformMatrix4fv_diag(GLint loc, GLsizei n, GLboolean transpose,
                             const GLfloat *v);
void glUniform4f_diag(GLint loc, GLfloat x, GLfloat y, GLfloat z, GLfloat w);
void glUniform4fv_diag(GLint loc, GLsizei n, const GLfloat *v);
void glBlendFunc_diag(GLenum src, GLenum dst);
void glEnable_diag(GLenum cap);
void glDisable_diag(GLenum cap);

void osmos_gl_frame(void);   /* after each nativeRender, before the swap        */

#endif  /* !OSMOS_DIAG */

#endif  /* OSMOS_GL_H */
