//===========================================================================//
// VFS backend dispatch. Selection is deliberately isolated from resolution
// policy and storage implementations so a browser peer can replace native I/O.
//===========================================================================//

#include "vfs_backend.h"

VfsBackend& Vfs()
{
#if defined(__EMSCRIPTEN__)
    static VfsBackend* backend = CreateEmscriptenVfsBackend();
#else
    static VfsBackend* backend = CreateNativeVfsBackend();
#endif
    return *backend;
}
