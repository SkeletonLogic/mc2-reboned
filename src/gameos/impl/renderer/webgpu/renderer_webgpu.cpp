#include "renderer_webgpu_internal.h"

//---------------------------------------------------------------------------//
// Deliberately heap-allocated and never freed.
//
// The null backend is a function-local static, which is fine because it owns
// nothing. This one owns a GPU device, and releasing that during static
// destruction -- after the game's clean TerminateGameEngine teardown -- would
// still run Dawn shutdown from global/static destruction, where ordering against
// other process-lifetime objects is unspecified. Leaking at exit avoids that
// ordering hazard and remains a bounded, one-object leak in a process that is
// ending.
//---------------------------------------------------------------------------//
RendererBackend* CreateWebGPURenderer()
{
    static mc2webgpu::WebGPURenderer* instance = nullptr;
    static bool            tried    = false;

    if (tried) return instance;
    tried = true;

    mc2webgpu::WebGPURenderer* r = new mc2webgpu::WebGPURenderer();
    if (!r->Init())
    {
        std::fprintf(stderr, "[webgpu] initialisation failed; caller should fall back\n");
        // Not deleted: Dawn objects partially created during a failed Init are
        // safer left alone than torn down in an unknown state.
        return nullptr;
    }

    instance = r;
    return instance;
}
