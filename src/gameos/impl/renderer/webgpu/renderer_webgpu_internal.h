#pragma once

#include <algorithm>
#include <chrono>  // PORT: HUD frame-rate window.
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <cstring>
#include <ctime>   // PORT: timestamped screenshot names.
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <webgpu/webgpu_cpp.h>

#include <gameos.hpp>
#include <mc2_port.h>

#include "../../gos_backend.h"
#include "renderer_backend.h"
#include "renderer_state.h"
#include "renderer_tga.h"

namespace mc2webgpu {

// PORT (M8-B52): the size of the fixed-layout UI canvas currently in force.
//
// This USED TO BE HARDCODED 800x600 in two places, which is what broke the
// widescreen loading screen: MC2_SetVirtualCanvas(1672, 941) flipped the flag
// the renderer tests, but the scale and offset it drew with stayed the 800x600
// fit. At 1920x1080 that is scale 1.8 with a 240px offset, so a layout authored
// at 1672 wide was drawn about 2.15x too large and ran off the screen.
//
// Falls back to 800x600 when no canvas is active, so every existing authored
// screen produces byte-identical geometry.
inline void CanvasExtent(float* width, float* height)
{
    unsigned int canvasWidth = 0, canvasHeight = 0;
    MC2_GetVirtualCanvas(&canvasWidth, &canvasHeight);
    if (!canvasWidth || !canvasHeight) { canvasWidth = 800; canvasHeight = 600; }
    if (width)  *width  = static_cast<float>(canvasWidth);
    if (height) *height = static_cast<float>(canvasHeight);
}


// wgpu::StringView is not null-terminated in general; %s would run off the end.
inline std::string_view sv(wgpu::StringView s)
{
    return static_cast<std::string_view>(s);
}

// 32-bit staging for every format, matching the null backend. Deliberately never
// smaller than a caller expects: MCLib writes 16-bit pixels through some paths and
// a tighter guess would let it run off the end of the buffer.
inline constexpr DWORD kStagingBytesPerPixel = 4;
inline constexpr uint32_t kReadbackAlignment = 256;

struct ViewportUniform
{
    float width;
    float height;
    float scale;
    float unused;
    float offsetX;
    float offsetY;
    float padding[2];
};

struct FogUniform
{
    float red;
    float green;
    float blue;
    float padding;
};

struct FogBinding
{
    wgpu::Buffer buffer;
    wgpu::BindGroup bindGroup;
};

struct Texture
{
    std::string                name;
    gos_TextureFormat          format;
    DWORD                      width;
    DWORD                      height;
    std::vector<unsigned char> pixels;      // authoritative; see header comment
    wgpu::Texture              gpu;         // downstream copy, may be null
    wgpu::TextureView          view;        // cached on first bind
    wgpu::BindGroup            bindGroup;   // texture-only group, cached on first bind
    bool                       locked;
    bool                       dirty;
};

struct PipelineKey
{
    wgpu::PrimitiveTopology topology;
    int alphaMode;
    int zCompare;
    int zWrite;
    int alphaTest;
    int textureBlend;
    bool textureSolid;
    bool perspective;
    // PORT (M8-B11): gos_State_Specular, which decides whether the vertex
    // specular channel is added. It has to be part of the key because the two
    // cases are different shaders.
    int specular;
    int fog;

    bool operator<(const PipelineKey& rhs) const
    {
        return std::tie(topology, alphaMode, zCompare, zWrite, alphaTest, textureBlend,
                        textureSolid, perspective, specular, fog) <
               std::tie(rhs.topology, rhs.alphaMode, rhs.zCompare, rhs.zWrite,
                        rhs.alphaTest, rhs.textureBlend, rhs.textureSolid,
                        rhs.perspective, rhs.specular, rhs.fog);
    }
};

struct SamplerKey
{
    int filter;
    int address;

    bool operator<(const SamplerKey& rhs) const
    { return std::tie(filter, address) < std::tie(rhs.filter, rhs.address); }
};

struct SamplerBinding
{
    wgpu::Sampler sampler;
    wgpu::BindGroup bindGroup;
};

struct VertexArenaChunk
{
    wgpu::Buffer gpu;
    std::vector<gos_VERTEX> cpu;
    size_t capacity;
};

struct IndexArenaChunk
{
    wgpu::Buffer gpu;
    std::vector<WORD> cpu;
    size_t capacity;
};

struct VertexSlice
{
    wgpu::Buffer buffer;
    uint64_t offset;
};

struct IndexSlice
{
    wgpu::Buffer buffer;
    uint64_t offset;
};

//---------------------------------------------------------------------------//
// GameOS texture format -> WebGPU format.
//
// All three of Solid/Keyed/Alpha map to RGBA8Unorm today. The original picked
// 565/1555/4444 to save video memory on 2001 hardware; there is no reason to
// reproduce that, and collapsing them means one upload path. Bump/Normal are
// mapped too but the game's use of them is not yet exercised, so they are logged
// once if they appear rather than assumed correct.
//---------------------------------------------------------------------------//
wgpu::TextureFormat wgpuFormatFor(gos_TextureFormat fmt);

class WebGPURenderer : public RendererBackend
{
public:
    WebGPURenderer();

    // Separate from the constructor because failure is expected and recoverable:
    // no GPU, no driver, or a missing d3dcompiler_47.dll all mean "fall back to
    // null", not "abort the process".
    bool Init();

    const char* Name() const override;

    //-- frame boundary ----------------------------------------------------//
    void BeginFrame() override;

    void EndFrame() override;

    void SetWindow(void* nativeWindow, void* nativeDisplay,
                   int clientWidth, int clientHeight) override;

    //-- render state ------------------------------------------------------//
    // Recorded, not yet translated. Translation is per-pipeline and belongs with
    // the pipeline work; recording it here with the SAME tracker the null backend
    // uses is what makes the two directly comparable.
    void SetRenderState(gos_RenderState state, intptr_t value) override;
    void PushRenderStates() override;
    void PopRenderStates() override;

    //-- drawing -----------------------------------------------------------//
    void DrawPoints(gos_VERTEX* v, int n) override;

    void DrawLines(gos_VERTEX* v, int n) override;

    void DrawTriangles(gos_VERTEX* v, int n) override;

    void DrawQuads(gos_VERTEX* v, int n) override;

    void RenderIndexed(gos_VERTEX* v, DWORD nv, WORD* indices, DWORD ni) override;

    void RenderIndexed2UV(gos_VERTEX_2UV* v, DWORD nv, WORD* indices, DWORD ni) override;

    void RenderIndexed3UV(gos_VERTEX_3UV* v, DWORD nv, WORD* indices, DWORD ni) override;

    void RenderIndexedRaw(void* v, DWORD nv, WORD* indices, DWORD ni,
                          gosVERTEXTYPE vertexType, gosPRIMITIVETYPE primitiveType) override;

    //-- textures ----------------------------------------------------------//
    DWORD NewEmptyTexture(gos_TextureFormat fmt, const char* name, DWORD hw) override;

    DWORD NewTextureFromFile(gos_TextureFormat fmt, const char* fileName,
                             DWORD hints) override;

    DWORD NewTextureFromMemory(gos_TextureFormat fmt, const char* name, BYTE* bytes,
                               DWORD size, DWORD hints) override;

    void DestroyTexture(DWORD handle) override;

    void LockTexture(DWORD handle, DWORD mipMapSize, bool readOnly, TEXTUREPTR* info) override;

    void UnLockTexture(DWORD handle) override;

    //-- text --------------------------------------------------------------//
    HGOSFONT3D LoadFont(const char* fontFile, DWORD startLine, int charCount,
                        DWORD textureHandle) override;

    void DeleteFont(HGOSFONT3D handle) override;

    void TextSetAttributes(HGOSFONT3D handle, DWORD foreground, float size,
                           bool wordWrap, bool proportional, bool bold, bool italic,
                           DWORD wrapType, bool disableEmbeddedCodes) override;

    void TextSetPosition(int x, int y) override;

    void TextSetRegion(int l, int t, int r, int b) override;
    void SetViewportBackground(bool fill, DWORD color) override;

    void TextDrawBackground(int left, int top, int right, int bottom, DWORD color) override;

    void TextStringLength(DWORD* width, DWORD* height, const char* text) override;

    void TextDraw(const char* text) override;

    //-- diagnostics -------------------------------------------------------//
    void DumpStats() override;

private:
    bool EnsureSurfaceConfigured();

    bool EnsureSurfaceBlitResources();

    bool EncodeSurfaceFrame();

    bool EnsureTargets(uint32_t width, uint32_t height);

    bool EnsureSharedResources();

    PipelineKey CurrentPipelineKey(wgpu::PrimitiveTopology topology) const;
    // PORT (M8-B11): specular census, gated behind MC2_LIGHT_CENSUS.
    void TallySpecular(const gos_VERTEX* vertices, DWORD count);
    unsigned long m_specDraws[2] = { 0, 0 };
    unsigned long m_specVerts[2] = { 0, 0 };
    unsigned long m_specCarrying[2] = { 0, 0 };
    // PORT (M8-3): per-vertex fog census, gated behind MC2_FOG_CENSUS.
    void TallyFog(const gos_VERTEX* vertices, DWORD count);
    unsigned long m_fogDraws[2] = { 0, 0 };
    unsigned long m_fogVerts[2] = { 0, 0 };
    unsigned long m_fogCarrying[2] = { 0, 0 };
    unsigned char m_fogMinAlpha[2] = { 0xff, 0xff };
    // PORT (M8-3): shade-mode census, gated behind MC2_SHADE_CENSUS.
    void TallyShade(const gos_VERTEX* vertices, DWORD count);
    unsigned long m_shadeDraws[2] = { 0, 0 };
    unsigned long m_shadeVerts[2] = { 0, 0 };
    unsigned long m_shadeTris[2] = { 0, 0 };
    unsigned long m_shadeVaryingTris[2] = { 0, 0 };

    wgpu::RenderPipeline PipelineFor(wgpu::PrimitiveTopology topology);

    wgpu::RenderPipeline PipelineForKey(const PipelineKey& key);

    SamplerBinding* SamplerForCurrentState();

    SamplerBinding* SamplerForKey(SamplerKey key);

    wgpu::BindGroup TextureBindGroupForCurrentState();

    wgpu::BindGroup TextureBindGroupForHandle(DWORD handle);

    wgpu::BindGroup FogBindGroupForCurrentState();

    bool BindDrawState(wgpu::PrimitiveTopology topology);
    wgpu::BindGroup ViewportBindGroupForCurrentCanvas();

    VertexArenaChunk& VertexChunkFor(size_t count);

    IndexArenaChunk& IndexChunkFor(size_t count);

    VertexSlice AppendVertices(const gos_VERTEX* vertices, DWORD count);

    IndexSlice AppendIndices(const WORD* indices, DWORD count);

    void UploadFrameArenas();

    bool SubmitVertices(const gos_VERTEX* vertices, DWORD count,
                        wgpu::PrimitiveTopology topology);

    void AccountSubmittedGeometryDraw();

    bool EnsureQuadIndices(DWORD quadCount);

    bool SubmitQuads(const gos_VERTEX* vertices, DWORD count);

    static void SetQuad(gos_VERTEX* vertices, float x0, float y0, float x1, float y1,
                        float u0, float v0, float u1, float v1, DWORD color);

    bool ClipTextQuad(float& x0, float& y0, float& x1, float& y1,
                      float& u0, float& v0, float& u1, float& v1) const;

    bool SubmitTextQuads(const gos_VERTEX* vertices, DWORD count, DWORD textureHandle,
                         bool alphaTest);

    void DrawTextProof();

    // PORT: TEST BRANCH only -- a top-left HUD ("Screenshot Ctrl+Alt+S | ### FPS")
    // and the hotkey that writes the CURRENT frame out as a .ppm. A screenshot
    // pasted into a chat is not a file and cannot be measured; a defect that
    // needs pixels counted needs a file on disk. Both are off unless
    // MC2_HUD_OVERLAY is set, so the plain launcher is untouched.
    void DrawHudOverlay();
    void PollCaptureHotkey();

    bool SubmitIndexed(const gos_VERTEX* vertices, DWORD vertexCount,
                       const WORD* indices, DWORD indexCount,
                       wgpu::PrimitiveTopology topology);

    static wgpu::PrimitiveTopology TopologyFor(gosPRIMITIVETYPE primitive);

    static void ConvertVertices(const gos_VERTEX_2UV* input, DWORD count,
                                std::vector<gos_VERTEX>& output);

    static void ConvertVertices(const gos_VERTEX_3UV* input, DWORD count,
                                std::vector<gos_VERTEX>& output);

    void DumpReadback(wgpu::Buffer& readback, uint32_t paddedBytesPerRow,
                      uint64_t readbackSize);

    void CountIndexed(DWORD numVerts, DWORD numIndices);

    DWORD DecodeAndCreate(const char* name, gos_TextureFormat requested,
                          const BYTE* bytes, DWORD size, DWORD hints);

    DWORD Create(const char* name, gos_TextureFormat format, DWORD width, DWORD height,
                 const std::vector<unsigned char>* pixels, bool uploadNow);

    void Upload(Texture& t);

    static const char* backendTypeNameRaw(wgpu::BackendType b);

    static std::string backendTypeName(wgpu::BackendType b);

    static const char* adapterTypeName(wgpu::AdapterType t);

    struct Stats
    {
        unsigned long points, lines, triangles, quads, indexed;
        unsigned long submittedPoints, submittedLines, submittedTriangles;
        unsigned long submittedQuads, submittedIndexed;
        unsigned long indexed1UV, indexed2UV, indexed3UV, indexedRaw;
        unsigned long submittedIndexed1UV, submittedIndexed2UV;
        unsigned long submittedIndexed3UV, submittedIndexedRaw;
        unsigned long long indexedVerts, indexedIndices;
        unsigned long textDraws;
        unsigned long created, gpuCreated, gpuFailed;
        unsigned long locks, mipLocks, uploads;
        unsigned long decoded, decodeFailed, rleDecoded, nonSquareDecoded;
        unsigned long source24, source32, pinkKeyed, forceAlpha;
        unsigned long fontsLoaded, fontLoadFailed, glyphsDrawn;
        unsigned long textBackgrounds, textBackgroundsSubmitted;
        unsigned long boldIgnored, italicIgnored;
        unsigned long slashDraws; // MC2_TEXT_CENSUS: draws carrying a backslash
        unsigned long long uploadedBytes;
    };

    // PORT (M8-B19): DEVICE-LOSS RECOVERY.
    //
    // 2026-08-25: the owner paused a mission, walked away, and came back to a
    // black screen with sound still playing. The driver had removed the D3D
    // device (DXGI_ERROR_DEVICE_REMOVED reported out of IDXGISwapChain::Present)
    // and the device-lost callback only PRINTED. The game carried on simulating
    // for roughly 99,000 further ticks, rendering into a dead device, so the
    // session was unrecoverable and the play was wasted.
    //
    // WHY RECOVERY IS POSSIBLE AT ALL, and it is a property of this renderer's
    // design rather than luck: every GPU object here is a CACHE over data the
    // CPU still owns. Texture::pixels is marked authoritative and Texture::gpu
    // is "a downstream copy, may be null"; pipelines, samplers, fog bindings and
    // bind groups are lazily built maps; the render targets, shared resources
    // and surface are all created on demand from BeginFrame. So recovery is:
    // drop every GPU handle, rebuild the device, and let the existing lazy paths
    // repopulate from the CPU-side originals.
    //
    // NOTE ON WHAT DOES **NOT** WORK: toggling vsync cannot revive a lost
    // device. A removed device invalidates every object created from it,
    // including the swapchain, so there is nothing left for a present-mode
    // change to act on. The device itself has to be recreated.
    bool RecoverFromDeviceLoss();
    void RecreateAllTextures();
    void ReleaseAllGpuState();
    unsigned      m_deviceLossRecoveries = 0;   // consecutive, not lifetime
    unsigned long m_lastDeviceLossFrame = 0;
    bool     m_deviceLossFatal = false;

    wgpu::Instance m_instance;
    wgpu::Adapter  m_adapter;
    wgpu::Device   m_device;
    wgpu::Queue    m_queue;
    std::string    m_adapterName;
    std::string    m_backendName;

    mc2gfx::RenderStateTracker m_state;
    mc2gfx::TextMetrics        m_text;
    Stats                      m_stats;

    // PORT (M8-F1 follow-up): what the offscreen colour attachment clears to.
    // Black unless gos_SetupViewport asks for a fill, which the game does only
    // while a mission is active.
    wgpu::Color                m_viewportBackground = { 0.0, 0.0, 0.0, 1.0 };

    std::map<DWORD, Texture*> m_textures;
    std::set<mc2gfx::FontData*> m_fonts;
#if defined(__EMSCRIPTEN__)
    // PORT: the browser re-opens the same few font files on every screen, and an
    // unoptimised wasm build spends about 1.7 s per font re-decoding the atlas,
    // re-running MakeFontMask and re-scanning the glyph grid, plus a fresh texture
    // upload. Measured 2026-08-24: 16 of these cost 25 s on one LOAD GAME click.
    // Cache the finished FontData by file+params and reference-count it. Native is
    // fast enough that it keeps the original one-FontData-per-LoadFont behaviour.
    struct FontCacheEntry { mc2gfx::FontData* font; unsigned refs; };
    std::map<std::string, FontCacheEntry>          m_fontCache;
    std::map<const mc2gfx::FontData*, std::string> m_fontCacheKey;
#endif
    DWORD                     m_nextTexture;
    std::set<std::pair<DWORD, DWORD> > m_decodedDimensions;

    wgpu::Texture      m_colorTarget;
    wgpu::TextureView  m_colorView;
    wgpu::Texture      m_depthTarget;
    wgpu::TextureView  m_depthView;
    uint32_t           m_targetWidth;
    uint32_t           m_targetHeight;

    wgpu::CommandEncoder    m_encoder;
    wgpu::RenderPassEncoder m_pass;
    wgpu::RenderPipeline    m_proofPipeline;
    wgpu::BindGroupLayout   m_proofBindLayout;
    wgpu::BindGroup         m_proofBindGroup;
    wgpu::Buffer            m_viewportBuffer;
    wgpu::Buffer            m_canvasViewportBuffer;
    wgpu::Buffer            m_proofVertexBuffer;
    wgpu::Texture           m_proofTexture;
    wgpu::TextureView       m_proofTextureView;
    wgpu::Sampler           m_proofSampler;

    wgpu::BindGroupLayout m_viewportBindLayout;
    wgpu::BindGroupLayout m_samplerBindLayout;
    wgpu::BindGroupLayout m_textureBindLayout;
    wgpu::BindGroupLayout m_fogBindLayout;
    wgpu::PipelineLayout  m_pipelineLayout;
    wgpu::BindGroup       m_viewportBindGroup;
    wgpu::BindGroup       m_canvasViewportBindGroup;

    // PORT: two runtime switches for diagnosing a reported visual artefact
    // without a rebuild. See PipelineForKey and SamplerForCurrentState.
    bool                  m_noPerspective;
    bool                  m_uiPointSampling;
    unsigned long long    m_uiPointForced;
    unsigned long long    m_uiPointAlreadyPoint;
    wgpu::Texture         m_whiteTexture;
    wgpu::TextureView     m_whiteTextureView;
    wgpu::BindGroup       m_whiteBindGroup;

    std::map<PipelineKey, wgpu::RenderPipeline> m_pipelines;
    std::map<SamplerKey, SamplerBinding>         m_samplers;
    std::map<DWORD, FogBinding>                  m_fogBindings;
    std::vector<VertexArenaChunk>                m_vertexChunks;
    std::vector<IndexArenaChunk>                 m_indexChunks;
    size_t                                       m_activeVertexChunk;
    size_t                                       m_activeIndexChunk;
    size_t                                       m_frameArenaBytes;
    size_t                                       m_peakArenaBytes;
    wgpu::Buffer                                 m_quadIndexBuffer;
    DWORD                                        m_quadIndexCapacity;
    unsigned long                                m_maxQuadBatch;
    unsigned long                                m_textureBindGroupCount;
    unsigned long                                m_unsupportedRawDraws;
    bool                                         m_flatTextures;
    bool                                         m_disableDepth;
    unsigned long                                m_currentFrameDrawCalls;
    unsigned long                                m_peakFrameDrawCalls;
    unsigned long long                           m_totalFrameDrawCalls;
    unsigned long long                           m_geometryDrawsByAlphaMode[5];
    unsigned long long                           m_currentFrameUploadBytes;
    unsigned long long                           m_peakFrameUploadBytes;
    unsigned long long                           m_totalFrameUploadBytes;
    unsigned long                                m_framesWithUploads;
    mc2gfx::FontData*                            m_proofFont;
    std::string                                  m_textProof;
    bool                                         m_textProofLogged;

    void*                    m_nativeWindow;
    void*                    m_nativeDisplay;
    uint32_t                 m_requestedSurfaceWidth;
    uint32_t                 m_requestedSurfaceHeight;
    uint32_t                 m_surfaceWidth;
    uint32_t                 m_surfaceHeight;
    wgpu::TextureFormat      m_surfaceFormat;
    wgpu::PresentMode        m_surfacePresentMode;
    wgpu::CompositeAlphaMode m_surfaceAlphaMode;
    wgpu::Surface            m_surface;
    wgpu::Texture            m_surfaceTexture;
    wgpu::BindGroupLayout    m_surfaceBindLayout;
    wgpu::BindGroup          m_surfaceBindGroup;
    wgpu::Sampler            m_surfaceSampler;
    wgpu::RenderPipeline     m_surfacePipeline;
    bool                     m_surfaceConfigured;
    bool                     m_surfaceFrameAcquired;
    unsigned long            m_surfaceConfigurations;
    unsigned long            m_surfacePresented;
    unsigned long            m_surfaceSuboptimal;
    unsigned long            m_surfaceTimeouts;
    unsigned long            m_surfaceOutdated;
    unsigned long            m_surfaceLost;

    unsigned long m_frameNumber;
    unsigned long m_dumpFrame;
    bool          m_dumped;
    bool          m_frameOpen;
    bool          m_dumpOnAdditive;

    // PORT: HUD + screenshot hotkey. See DrawHudOverlay/PollCaptureHotkey.
    bool               m_hudOverlay;
    bool               m_captureRequested;
    bool               m_captureKeyWasDown;
    unsigned long      m_captureCount;
    std::string        m_shotDir;
    std::string        m_capturePath;
    mc2gfx::FontData*  m_hudFont;
    float              m_hudFps;
    unsigned long long m_hudLastMicros;
    unsigned long      m_hudFramesSince;
};

} // namespace mc2webgpu
