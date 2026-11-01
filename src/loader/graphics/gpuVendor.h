int getGPUVendorID();

#ifdef _WIN32
// opengl32's own entry points, for the loader's glad: glXGetProcAddressARB
// gives the GL hooks' wrappers for the game, which call through glad's
// pointers, so loading glad from it points each wrapper at itself.
void *gladGlLoader(const char *name);
#endif