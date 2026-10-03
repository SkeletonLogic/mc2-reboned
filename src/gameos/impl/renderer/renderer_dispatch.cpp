//===========================================================================//
// The GameOS renderer entry points, as thin dispatchers onto RendererBackend.
//
// Every gos_* renderer function in the game's link surface lives here and does
// nothing but forward. Keeping the dispatch separate from any implementation is
// what lets null and WebGPU be peers rather than one replacing the other.
//
// There is deliberately no logic in this file. Anything that looks like a
// decision belongs in a backend.
//===========================================================================//

#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <string> // PORT (M8-B10 tail): text is formatted without a fixed cap.

#include <gameos.hpp>

#include "renderer_backend.h"

namespace {

RendererBackend* g_renderer = nullptr;

// PORT (row 9 census): how often each rarely-seen draw path is entered, printed
// at exit when MC2_PATH_CENSUS=1. Counting only -- the forwards are unchanged.
// `indexed` and `quads` are the controls: a line with both at 0 means the run
// drew nothing, not that the other paths are unused. Each counter's FIRST hit
// also prints at once, because a browser tab closing never runs this destructor.
struct PathCensus {
    unsigned long long indexed = 0, quads = 0, idx2uv = 0, idx3uv = 0, raw = 0, points = 0;
    unsigned long long tex2Set = 0, tex3Set = 0, multiSet = 0;
    ~PathCensus()
    {
        const char* e = std::getenv("MC2_PATH_CENSUS");
        if (!e || *e != '1') return;
        std::fprintf(stderr,
            "[path-census] indexed=%llu quads=%llu 2uv=%llu 3uv=%llu raw=%llu points=%llu "
            "texture2Set=%llu texture3Set=%llu multitextureSet=%llu\n",
            indexed, quads, idx2uv, idx3uv, raw, points, tex2Set, tex3Set, multiSet);
        std::fflush(stderr);
    }
} g_census;

inline void Hit(unsigned long long& counter, const char* name)
{
    if (counter++ != 0) return;
    const char* e = std::getenv("MC2_PATH_CENSUS");
    if (e && *e == '1') { std::fprintf(stderr, "[path-census] first %s\n", name); std::fflush(stderr); }
}

} // namespace

RendererBackend& Renderer()
{
    // Default to null rather than crashing: the renderer is used during startup
    // before anything has had a chance to choose a backend, and a null draw is
    // always a safe answer.
    if (!g_renderer) g_renderer = CreateNullRenderer();
    return *g_renderer;
}

void SetRenderer(RendererBackend* backend)
{
    g_renderer = backend;
    if (backend)
        std::fprintf(stderr, "[renderer] backend = %s\n", backend->Name());
}

//---------------------------------------------------------------------------//
// Render state
//---------------------------------------------------------------------------//
void __stdcall gos_SetRenderState(gos_RenderState RenderState, intptr_t Value)
{
    if (Value) {
        if (RenderState == gos_State_Texture2) Hit(g_census.tex2Set, "texture2Set");
        else if (RenderState == gos_State_Texture3) Hit(g_census.tex3Set, "texture3Set");
        else if (RenderState == gos_State_Multitexture) Hit(g_census.multiSet, "multitextureSet");
    }
    Renderer().SetRenderState(RenderState, Value);
}

void __stdcall gos_PushRenderStates() { Renderer().PushRenderStates(); }
void __stdcall gos_PopRenderStates()  { Renderer().PopRenderStates(); }

//---------------------------------------------------------------------------//
// Drawing
//---------------------------------------------------------------------------//
void __stdcall gos_DrawPoints(gos_VERTEX* v, int n)    { Hit(g_census.points, "points"); Renderer().DrawPoints(v, n); }
void __stdcall gos_DrawLines(gos_VERTEX* v, int n)     { Renderer().DrawLines(v, n); }
void __stdcall gos_DrawTriangles(gos_VERTEX* v, int n) { Renderer().DrawTriangles(v, n); }
void __stdcall gos_DrawQuads(gos_VERTEX* v, int n)     { Hit(g_census.quads, "quads"); Renderer().DrawQuads(v, n); }

void __stdcall gos_RenderIndexedArray(gos_VERTEX* v, DWORD nv, WORD* idx, DWORD ni)
{ Hit(g_census.indexed, "indexed"); Renderer().RenderIndexed(v, nv, idx, ni); }

void __stdcall gos_RenderIndexedArray(gos_VERTEX_2UV* v, DWORD nv, WORD* idx, DWORD ni)
{ Hit(g_census.idx2uv, "2uv"); Renderer().RenderIndexed2UV(v, nv, idx, ni); }

void __stdcall gos_RenderIndexedArray(gos_VERTEX_3UV* v, DWORD nv, WORD* idx, DWORD ni)
{ Hit(g_census.idx3uv, "3uv"); Renderer().RenderIndexed3UV(v, nv, idx, ni); }

void __stdcall gos_RenderIndexedArray(void* v, DWORD nv, WORD* idx, DWORD ni,
                                      gosVERTEXTYPE vt, gosPRIMITIVETYPE pt)
{ Hit(g_census.raw, "raw"); Renderer().RenderIndexedRaw(v, nv, idx, ni, vt, pt); }

//---------------------------------------------------------------------------//
// Textures
//---------------------------------------------------------------------------//
DWORD __stdcall gos_NewEmptyTexture(gos_TextureFormat Format, const char* Name, DWORD HeightWidth,
                                    DWORD /*Hints*/, gos_RebuildFunction /*pFunc*/, void* /*pInstance*/)
{ return Renderer().NewEmptyTexture(Format, Name, HeightWidth); }

DWORD __stdcall gos_NewTextureFromFile(gos_TextureFormat Format, const char* FileName,
                                       DWORD Hints, gos_RebuildFunction /*pFunc*/, void* /*pInstance*/)
{ return Renderer().NewTextureFromFile(Format, FileName, Hints); }

DWORD __stdcall gos_NewTextureFromMemory(gos_TextureFormat Format, const char* FileName,
                                         BYTE* pBitmap, DWORD Size, DWORD Hints,
                                         gos_RebuildFunction /*pFunc*/, void* /*pInstance*/)
{ return Renderer().NewTextureFromMemory(Format, FileName, pBitmap, Size, Hints); }

void __stdcall gos_DestroyTexture(DWORD Handle)
{ Renderer().DestroyTexture(Handle); }

void __stdcall gos_LockTexture(DWORD Handle, DWORD MipMapSize, bool ReadOnly, TEXTUREPTR* TextureInfo)
{ Renderer().LockTexture(Handle, MipMapSize, ReadOnly, TextureInfo); }

void __stdcall gos_UnLockTexture(DWORD Handle)
{ Renderer().UnLockTexture(Handle); }

//---------------------------------------------------------------------------//
// Text
//
// The two variadic entry points are formatted here rather than in a backend:
// varargs cannot be forwarded through a virtual call, and every backend would
// otherwise repeat the same vsnprintf.
//---------------------------------------------------------------------------//
HGOSFONT3D __stdcall gos_LoadFont(const char* FontFile, DWORD StartLine, int CharCount, DWORD TextureHandle)
{ return Renderer().LoadFont(FontFile, StartLine, CharCount, TextureHandle); }

void __stdcall gos_DeleteFont(HGOSFONT3D Fonthandle)
{ Renderer().DeleteFont(Fonthandle); }

void __stdcall gos_TextSetAttributes(HGOSFONT3D FontHandle, DWORD Foreground, float Size,
                                     bool WordWrap, bool Proportional, bool Bold, bool Italic,
                                     DWORD WrapType, bool DisableEmbeddedCodes)
{ Renderer().TextSetAttributes(FontHandle, Foreground, Size, WordWrap, Proportional,
                               Bold, Italic, WrapType, DisableEmbeddedCodes); }

// PORT (M8-F1 follow-up): gos_SetupViewport lives in gos_system.cpp, which
// does not see the renderer. This is the one hop across.
void __stdcall gos_SetViewportBackground(bool Fill, DWORD Color)
{ Renderer().SetViewportBackground(Fill, Color); }

void __stdcall gos_TextSetPosition(int XPosition, int YPosition)
{ Renderer().TextSetPosition(XPosition, YPosition); }

void __stdcall gos_TextSetRegion(int Left, int Top, int Right, int Bottom)
{ Renderer().TextSetRegion(Left, Top, Right, Bottom); }

void __stdcall gos_TextDrawBackground(int Left, int Top, int Right, int Bottom, DWORD Color)
{ Renderer().TextDrawBackground(Left, Top, Right, Bottom, Color); }

// PORT (M8-B10 tail): both of these formatted into `char buffer[2048]`, so
// any string over 2047 characters was silently cut at the very last step --
// the same defect M8-B10 fixed in the resource loader, one layer further
// down, and just as silent. The retail table's longest string is 2000
// characters, so today's content fits with 47 to spare; that is a margin,
// not a design. Measure the formatted length first and give it exactly the
// room it needs.
//
// The printf semantics are UNCHANGED and deliberately so: Message is still a
// format string. 79 retail strings contain a '%', which is a real hazard for
// any caller that passes one straight through with no arguments -- but that
// is pre-existing behaviour the whole GUI is built on, and narrowing it here
// would be a silent change of contract rather than a bug fix.
static void FormatGosText(std::string& out, const char* message, va_list args)
{
    va_list probe;
    va_copy(probe, args);
    const int needed = vsnprintf(NULL, 0, message ? message : "", probe);
    va_end(probe);

    if (needed <= 0) { out.clear(); return; }

    out.resize(static_cast<size_t>(needed) + 1);
    vsnprintf(&out[0], out.size(), message ? message : "", args);
    out.resize(static_cast<size_t>(needed));
}

void __stdcall gos_TextStringLength(DWORD* Width, DWORD* Height, const char* Message, ...)
{
    std::string buffer;
    va_list args;
    va_start(args, Message);
    FormatGosText(buffer, Message, args);
    va_end(args);
    Renderer().TextStringLength(Width, Height, buffer.c_str());
}

void __stdcall gos_TextDraw(const char* Message, ...)
{
    std::string buffer;
    va_list args;
    va_start(args, Message);
    FormatGosText(buffer, Message, args);
    va_end(args);
    Renderer().TextDraw(buffer.c_str());
}

//---------------------------------------------------------------------------//
// Diagnostics, kept at the old name so gos_platform.cpp's shutdown call and
// gos_backend.h do not need to change.
//---------------------------------------------------------------------------//
void gosRenderNull_DumpStats()
{
    Renderer().DumpStats();
}
