#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void* GLHooks_GetProcAddress(const char* procName);

// LL_GL_DEBUG=1: the GL diagnostics are on (see rtGlDebugStart).
int llGlDebug(void);
// A diagnostic line under LL_GL_DEBUG, at most limit times per tag.
void llGlDebugLog(const char *tag, int limit, const char *fmt, ...);
// A depth texture's upload, after it (its error), under LL_GL_DEBUG.
void llGlDebugTexImage(unsigned int target, int internalformat, int width, int height, unsigned int format,
                       unsigned int type);

#ifdef __cplusplus
}
#endif
