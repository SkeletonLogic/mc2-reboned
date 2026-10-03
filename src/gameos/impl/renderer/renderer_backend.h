//===========================================================================//
// RendererBackend -- the seam between GameOS's renderer API and whatever
// actually draws.
//
// WHY THIS EXISTS
//
// The null renderer previously implemented the gos_* entry points directly.
// That was right for Milestone 1 (fastest path to a booting process) but it
// makes a second implementation impossible: WebGPU would have to REPLACE the
// file wholesale, the two could never coexist, and there would be no way to run
// them side by side to find where they disagree.
//
// With this interface:
//   * gos_* becomes a thin dispatcher (renderer_dispatch.cpp).
//   * null and WebGPU are peers, selected at build or run time.
//   * the null renderer's state/draw counters become a VALIDATION layer that
//     can wrap the real backend, not just a stub -- which matters because the
//     thing being ported is a D3D8-era fixed-function state machine and the
//     only way to know WebGPU reproduces it is to compare.
//
// SCOPE. This covers exactly the ~18 renderer entry points the linker said the
// game requires (docs/GAMEOS_REQUIRED_SYMBOLS.txt). Text and texture live here
// too because they are drawing concerns and share the backend's device; audio
// deliberately does NOT -- it was only ever in the same file because both were
// stubbed in the same pass.
//
// MEASURED, NOT GUESSED. At boot the game issues 30 indexed draws, 114
// gos_SetRenderState calls, and touches 15 DISTINCT render states. Phase 0
// quoted "691 gos_SetRenderState call sites" as the headline difficulty; that
// is the call count, not the state space. The state space is what an
// implementation must reproduce, and it is what this interface exposes.
//===========================================================================//

#pragma once

#include <gameos.hpp>

class RendererBackend
{
public:
    virtual ~RendererBackend() {}

    // Human-readable name, used in logs and by the harness.
    virtual const char* Name() const = 0;

    //-- frame boundary ----------------------------------------------------//
    // GameOS owns the main loop. These bracket Environment.UpdateRenderers(),
    // the one callback in which the game is allowed to issue draw calls.
    virtual void BeginFrame() = 0;
    virtual void EndFrame() = 0;
    // Opaque native window/display handles keep platform headers out of this
    // seam. Null ignores them; WebGPU uses them to create/configure a surface.
    virtual void SetWindow(void* nativeWindow, void* nativeDisplay,
                           int clientWidth, int clientHeight) = 0;

    //-- render state ------------------------------------------------------//
    // The D3D8-era fixed-function state machine. Push/Pop bracket nested state
    // changes; the game relies on them being a true stack.
    virtual void SetRenderState(gos_RenderState state, intptr_t value) = 0;
    virtual void PushRenderStates() = 0;
    virtual void PopRenderStates() = 0;

    //-- drawing -----------------------------------------------------------//
    virtual void DrawPoints(gos_VERTEX* v, int count) = 0;
    virtual void DrawLines(gos_VERTEX* v, int count) = 0;
    virtual void DrawTriangles(gos_VERTEX* v, int count) = 0;
    virtual void DrawQuads(gos_VERTEX* v, int count) = 0;

    virtual void RenderIndexed(gos_VERTEX* v, DWORD numVerts, WORD* indices, DWORD numIndices) = 0;
    virtual void RenderIndexed2UV(gos_VERTEX_2UV* v, DWORD numVerts, WORD* indices, DWORD numIndices) = 0;
    virtual void RenderIndexed3UV(gos_VERTEX_3UV* v, DWORD numVerts, WORD* indices, DWORD numIndices) = 0;
    virtual void RenderIndexedRaw(void* v, DWORD numVerts, WORD* indices, DWORD numIndices,
                                  gosVERTEXTYPE vertexType, gosPRIMITIVETYPE primType) = 0;

    //-- textures ----------------------------------------------------------//
    // Lock MUST return writable memory of the advertised size. MCLib's texture
    // manager writes pixels through it and reads them back -- TerrTxm.cpp
    // combines terrain overlays in RAM and then LZ-compresses the result. A
    // backend that returns null here crashes during DATA LOAD, long before
    // anything is drawn.
    virtual DWORD NewEmptyTexture(gos_TextureFormat fmt, const char* name, DWORD heightWidth) = 0;
    virtual DWORD NewTextureFromFile(gos_TextureFormat fmt, const char* fileName,
                                     DWORD hints) = 0;
    virtual DWORD NewTextureFromMemory(gos_TextureFormat fmt, const char* name,
                                       BYTE* bitmap, DWORD size, DWORD hints) = 0;
    virtual void  DestroyTexture(DWORD handle) = 0;
    virtual void  LockTexture(DWORD handle, DWORD mipMapSize, bool readOnly, TEXTUREPTR* info) = 0;
    virtual void  UnLockTexture(DWORD handle) = 0;

    //-- text --------------------------------------------------------------//
    virtual HGOSFONT3D LoadFont(const char* fontFile, DWORD startLine, int charCount, DWORD texHandle) = 0;
    virtual void DeleteFont(HGOSFONT3D font) = 0;
    virtual void TextSetAttributes(HGOSFONT3D font, DWORD foreground, float size,
                                   bool wordWrap, bool proportional, bool bold, bool italic,
                                   DWORD wrapType, bool disableEmbeddedCodes) = 0;
    virtual void TextSetPosition(int x, int y) = 0;
    virtual void TextSetRegion(int left, int top, int right, int bottom) = 0;
    virtual void TextDrawBackground(int left, int top, int right, int bottom, DWORD color) = 0;
    // Must return a plausible size even when nothing is rendered: returning
    // zero collapses every layout that sizes itself from text.
    virtual void TextStringLength(DWORD* width, DWORD* height, const char* text) = 0;
    virtual void TextDraw(const char* text) = 0;

    //-- viewport ----------------------------------------------------------//
    // PORT (M8-F1 follow-up): retail's viewport BACKGROUND FILL, which this
    // port never implemented -- gos_SetupViewport threw FillBG and BGColor
    // away and every frame cleared to hard black.
    //
    // That is the black above the sky. MechCmd2.cpp has ALWAYS passed
    // eye->fogColor here while a mission is active, and the fog colour is the
    // haze the distant terrain already blends into, so honouring the two
    // parameters puts the sky's own tone behind the sky instead of a hole.
    // Menus keep their black: the game passes 0 there, deliberately, because
    // the logistics art leaves transparent gaps for a black backplate.
    //
    // Non-pure with a no-op default so a backend that cannot clear to a
    // colour simply keeps its old behaviour.
    virtual void SetViewportBackground(bool /*fill*/, DWORD /*color*/) {}

    //-- diagnostics -------------------------------------------------------//
    // What the game actually exercised. Implementations that cannot answer
    // meaningfully may no-op.
    virtual void DumpStats() {}
};

//---------------------------------------------------------------------------//
// Backend selection.
//
// One backend is active at a time. gos_* dispatches through Renderer().
// CreateNullRenderer is always available and cannot fail.
//---------------------------------------------------------------------------//
RendererBackend* CreateNullRenderer();

#if defined(MC2_ENABLE_WEBGPU)
// Returns nullptr if the GPU device could not be created -- no adapter, no
// driver, or a missing d3dcompiler_47.dll. That is a normal outcome, not an
// error to abort on, so the caller falls back to null. Declared with no WebGPU
// types in the signature so this header stays free of Dawn (and of C++20, which
// Dawn's headers require and the rest of the port does not use).
RendererBackend* CreateWebGPURenderer();
#endif

RendererBackend& Renderer();
void             SetRenderer(RendererBackend* backend);

// Applies the selection policy once, at startup. Lives in renderer_select.cpp so
// that policy is not mixed into either the dispatch table or a backend.
void gosRenderer_Select();
