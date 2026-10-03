#include "platform_backend.h"

PlatformBackend& Platform()
{
#if defined(__EMSCRIPTEN__)
    static PlatformBackend* backend = CreateEmscriptenPlatformBackend();
#else
    static PlatformBackend* backend = CreateWin32PlatformBackend();
#endif
    return *backend;
}
