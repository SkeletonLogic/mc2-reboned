//===========================================================================//
// Renderer backend selection policy.
//
// Kept out of renderer_dispatch.cpp (which is forwarding only, by design) and out
// of the backends themselves (a backend should not know what its alternatives
// are). This is the one place that decides which implementation runs.
//
// POLICY
//   MC2_RENDERER=null     force the null backend
//   MC2_RENDERER=webgpu   force WebGPU; if it cannot start, say so and use null
//   unset                 WebGPU when compiled in, else null
//
// The fallback is deliberate. A missing GPU, a headless session, or a missing
// d3dcompiler_47.dll must not stop the game from booting -- headless bring-up is
// how nearly every bug in this port has been found, and losing it to a graphics
// problem would be a bad trade. The choice is always logged, because "which
// renderer was actually active" is the first question about any rendering report.
//===========================================================================//

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "renderer_backend.h"

namespace {

// Case-insensitive compare without pulling in a platform header.
bool sameName(const char* a, const char* b)
{
    if (!a || !b) return false;
    for (; *a && *b; ++a, ++b)
    {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return *a == *b;
}

} // namespace

// PORT (M8-B49): does the game have a real backend this run?
//
// The loading-bar pump must be INERT headless, or it would add frames the strict
// bar has never counted. Recording the choice here is the only honest way to ask:
// a caller cannot tell a null backend from a real one through RendererBackend.
static bool g_rendererIsNull = true;

bool MC2_RendererIsNull() { return g_rendererIsNull; }

void gosRenderer_Select()
{
    const char* want = std::getenv("MC2_RENDERER");

    if (want && *want && sameName(want, "null"))
    {
        std::printf("[renderer] MC2_RENDERER=null -- using the null backend\n");
        g_rendererIsNull = true; SetRenderer(CreateNullRenderer());
        return;
    }

#if defined(MC2_ENABLE_WEBGPU)
    if (!want || !*want || sameName(want, "webgpu"))
    {
        RendererBackend* gpu = CreateWebGPURenderer();
        if (gpu)
        {
            g_rendererIsNull = false; SetRenderer(gpu);
            return;
        }
        std::fprintf(stderr,
                     "[renderer] WebGPU unavailable -- falling back to the null backend. "
                     "The game will run but nothing will be drawn.\n");
        g_rendererIsNull = true; SetRenderer(CreateNullRenderer());
        return;
    }
    std::fprintf(stderr, "[renderer] unknown MC2_RENDERER='%s' (want null|webgpu); using null\n",
                 want);
#else
    if (want && *want && !sameName(want, "null"))
        std::fprintf(stderr,
                     "[renderer] MC2_RENDERER='%s' but this binary was built without "
                     "MC2_ENABLE_WEBGPU; using null\n", want);
#endif

    g_rendererIsNull = true; SetRenderer(CreateNullRenderer());
}
