#include "renderer_webgpu_internal.h"
#include <atomic>

namespace mc2webgpu {

WebGPURenderer::WebGPURenderer()
        : m_state("webgpu"), m_nextTexture(1), m_targetWidth(0), m_targetHeight(0),
          m_frameNumber(0), m_dumpFrame(0), m_dumped(false), m_frameOpen(false),
          m_dumpOnAdditive(false),
          m_activeVertexChunk(0), m_activeIndexChunk(0), m_frameArenaBytes(0),
          m_peakArenaBytes(0), m_quadIndexCapacity(0), m_maxQuadBatch(0),
          m_textureBindGroupCount(0), m_unsupportedRawDraws(0), m_flatTextures(false),
          m_disableDepth(false), m_currentFrameDrawCalls(0), m_peakFrameDrawCalls(0),
          m_totalFrameDrawCalls(0), m_currentFrameUploadBytes(0),
          m_peakFrameUploadBytes(0), m_totalFrameUploadBytes(0), m_framesWithUploads(0),
          m_proofFont(nullptr), m_textProofLogged(false), m_nativeWindow(nullptr),
          m_nativeDisplay(nullptr), m_requestedSurfaceWidth(0),
          m_requestedSurfaceHeight(0), m_surfaceWidth(0), m_surfaceHeight(0),
          m_surfaceFormat(wgpu::TextureFormat::Undefined),
          m_surfacePresentMode(wgpu::PresentMode::Fifo),
          m_surfaceAlphaMode(wgpu::CompositeAlphaMode::Opaque),
          m_surfaceConfigured(false), m_surfaceFrameAcquired(false),
          m_surfaceConfigurations(0), m_surfacePresented(0), m_surfaceSuboptimal(0),
          m_surfaceTimeouts(0), m_surfaceOutdated(0), m_surfaceLost(0),
          m_hudOverlay(false), m_captureRequested(false), m_captureKeyWasDown(false),
          m_captureCount(0), m_hudFont(nullptr), m_hudFps(0.0f), m_hudLastMicros(0),
          m_hudFramesSince(0)
    {
        std::memset(&m_stats, 0, sizeof(m_stats));
        std::memset(m_geometryDrawsByAlphaMode, 0, sizeof(m_geometryDrawsByAlphaMode));

        const char* requestedDump = std::getenv("MC2_DUMP_FRAME");
        if (requestedDump && *requestedDump)
        {
            char* end = nullptr;
            const unsigned long parsed = std::strtoul(requestedDump, &end, 10);
            if (end != requestedDump && *end == '\0' && parsed > 0)
                m_dumpFrame = parsed;
            else
                std::fprintf(stderr, "[webgpu] MC2_DUMP_FRAME must be a positive frame number\n");
        }
        const char* flatTextures = std::getenv("MC2_FLAT_TEXTURES");
        m_flatTextures = flatTextures && *flatTextures && std::strcmp(flatTextures, "0") != 0;
        const char* disableDepth = std::getenv("MC2_DISABLE_DEPTH");
        m_disableDepth = disableDepth && *disableDepth && std::strcmp(disableDepth, "0") != 0;
        if (m_disableDepth)
            std::fprintf(stderr, "[webgpu] MC2_DISABLE_DEPTH active -- depth compare/write forced off\n");
        // PORT: TEST BRANCH HUD and screenshot hotkey (2026-08-13). MC2_SHOT_DIR
        // says where Ctrl+Alt+S writes; it defaults to the run directory so a
        // missing variable still produces a file rather than nothing.
        const char* hudOverlay = std::getenv("MC2_HUD_OVERLAY");
        m_hudOverlay = hudOverlay && *hudOverlay && std::strcmp(hudOverlay, "0") != 0;
        const char* shotDir = std::getenv("MC2_SHOT_DIR");
        m_shotDir = (shotDir && *shotDir) ? shotDir : ".";
        if (m_hudOverlay)
            std::fprintf(stderr, "[webgpu] MC2_HUD_OVERLAY active -- HUD top-left, "
                                 "Ctrl+Alt+S writes a .ppm to %s\n", m_shotDir.c_str());

        const char* textProof = std::getenv("MC2_TEXT_PROOF");
        if (textProof && *textProof) m_textProof = textProof;
        const char* dumpOnAdditive = std::getenv("MC2_DUMP_ON_ADDITIVE");
        m_dumpOnAdditive = dumpOnAdditive && *dumpOnAdditive &&
                           std::strcmp(dumpOnAdditive, "0") != 0;

        const char* noPerspective = std::getenv("MC2_NO_PERSPECTIVE");
        m_noPerspective = noPerspective && *noPerspective &&
                          std::strcmp(noPerspective, "0") != 0;
        if (m_noPerspective)
            std::fprintf(stderr, "[webgpu] MC2_NO_PERSPECTIVE active -- u,v back to "
                                 "affine screen-space interpolation\n");

        const char* uiPointSampling = std::getenv("MC2_UI_POINT_SAMPLING");
        m_uiPointSampling = uiPointSampling && *uiPointSampling &&
                            std::strcmp(uiPointSampling, "0") != 0;
        m_uiPointForced = m_uiPointAlreadyPoint = 0;
        if (m_uiPointSampling)
            std::fprintf(stderr, "[webgpu] MC2_UI_POINT_SAMPLING active -- nearest "
                                 "filtering for canvas-scaled draws\n");
    }



    // Separate from the constructor because failure is expected and recoverable:
    // no GPU, no driver, or a missing d3dcompiler_47.dll all mean "fall back to
    // null", not "abort the process".
    // PORT (M8-B19): set from Dawn's device-lost callback, which can fire on a
    // thread of Dawn's choosing. Consumed on the render thread in BeginFrame.
    std::atomic<bool> g_webgpuDeviceLost{false};

    bool WebGPURenderer::Init()
    {
        // TimedWaitAny is required for a WaitAny with a non-zero timeout, which is
        // how the async adapter request is made synchronous here.
        const wgpu::InstanceFeatureName wanted[] = { wgpu::InstanceFeatureName::TimedWaitAny };
        wgpu::InstanceDescriptor idesc = {};
        idesc.requiredFeatureCount = 1;
        idesc.requiredFeatures     = wanted;

        m_instance = wgpu::CreateInstance(&idesc);
        if (!m_instance)
        {
            std::fprintf(stderr, "[webgpu] CreateInstance failed\n");
            return false;
        }

        wgpu::RequestAdapterOptions aopts = {};
        aopts.powerPreference = wgpu::PowerPreference::HighPerformance;

        // Non-capturing lambda + typed userdata: Dawn only supports capturing
        // lambdas for callbacks that carry a CallbackMode, and passing the slot
        // explicitly is clearer than allocating a closure.
        wgpu::Future f = m_instance.RequestAdapter(
            &aopts, wgpu::CallbackMode::WaitAnyOnly,
            [](wgpu::RequestAdapterStatus status, wgpu::Adapter adapter,
               wgpu::StringView message, wgpu::Adapter* out)
            {
                if (status == wgpu::RequestAdapterStatus::Success)
                {
                    *out = std::move(adapter);
                }
                else
                {
                    const std::string_view m = sv(message);
                    std::fprintf(stderr, "[webgpu] RequestAdapter failed (status=%u): %.*s\n",
                                 (unsigned)status, (int)m.size(), m.data());
                }
            },
            &m_adapter);

        // Success == 1, not 0. See the enum trap note at the top of this file.
        const wgpu::WaitStatus ws = m_instance.WaitAny(f, UINT64_MAX);
        if (ws != wgpu::WaitStatus::Success || !m_adapter)
        {
            std::fprintf(stderr, "[webgpu] no adapter (WaitAny status=%u)\n", (unsigned)ws);
            return false;
        }

        wgpu::AdapterInfo info;
        m_adapter.GetInfo(&info);
        m_adapterName = std::string(sv(info.device));
        m_backendName = backendTypeName(info.backendType);
        std::printf("[webgpu] adapter '%s' (vendor '%.*s', backend %s, type %s)\n",
                    m_adapterName.c_str(),
                    (int)sv(info.vendor).size(), sv(info.vendor).data(),
                    m_backendName.c_str(), adapterTypeName(info.adapterType));

        wgpu::DeviceDescriptor ddesc = {};
        ddesc.label = "MechCommander 2";
        ddesc.SetUncapturedErrorCallback(
            [](const wgpu::Device&, wgpu::ErrorType type, wgpu::StringView message)
            {
                const std::string_view m = sv(message);
                std::fprintf(stderr, "[webgpu] ERROR (type=%u): %.*s\n",
                             (unsigned)type, (int)m.size(), m.data());
            });
        ddesc.SetDeviceLostCallback(
            wgpu::CallbackMode::AllowSpontaneous,
            [](const wgpu::Device&, wgpu::DeviceLostReason reason, wgpu::StringView message)
            {
                const std::string_view m = sv(message);
                std::fprintf(stderr, "[webgpu] DEVICE LOST (reason=%u): %.*s\n",
                             (unsigned)reason, (int)m.size(), m.data());
                std::fflush(stderr);
                // PORT (M8-B19): Dawn may raise this from another thread, and
                // the renderer must not be touched from here. Raise a flag; the
                // next BeginFrame does the work on the render thread.
                //
                // Reason 2 is Destroyed -- our own shutdown -- and must NOT
                // trigger recovery or the teardown path fights itself.
                if (reason != wgpu::DeviceLostReason::Destroyed)
                    g_webgpuDeviceLost.store(true, std::memory_order_release);
            });

#if defined(__EMSCRIPTEN__)
        // PORT: Browser WebGPU exposes device creation asynchronously. Asyncify
        // lets the legacy synchronous initialization seam wait without moving
        // the whole game to a pthread worker.
        const wgpu::Future deviceFuture = m_adapter.RequestDevice(
            &ddesc, wgpu::CallbackMode::WaitAnyOnly,
            [](wgpu::RequestDeviceStatus status, wgpu::Device device,
               wgpu::StringView message, wgpu::Device* out)
            {
                if (status == wgpu::RequestDeviceStatus::Success)
                    *out = std::move(device);
                else
                {
                    const std::string_view m = sv(message);
                    std::fprintf(stderr, "[webgpu] RequestDevice failed (status=%u): %.*s\n",
                                 (unsigned)status, (int)m.size(), m.data());
                }
            }, &m_device);
        const wgpu::WaitStatus deviceWait = m_instance.WaitAny(deviceFuture, UINT64_MAX);
        if (deviceWait != wgpu::WaitStatus::Success) m_device = nullptr;
#else
        // Native Dawn offers synchronous device creation.
        m_device = m_adapter.CreateDevice(&ddesc);
#endif
        if (!m_device)
        {
            std::fprintf(stderr,
                         "[webgpu] CreateDevice failed. If the message above mentions "
                         "d3dcompiler_47.dll, that DLL must sit NEXT TO the executable "
                         "(System32 is not enough -- see the header comment).\n");
            return false;
        }

        m_queue = m_device.GetQueue();
        if (!m_queue)
        {
            std::fprintf(stderr, "[webgpu] GetQueue failed\n");
            return false;
        }

        std::printf("[webgpu] device and queue ready (surface optional)\n");
        return true;
    }



    //-- device-loss recovery ----------------------------------------------//
    //
    // See the header for why this is possible: every GPU object below is a cache
    // over data the CPU still holds, so dropping the lot and rebuilding the
    // device is enough. Anything missed here becomes a stale handle owned by a
    // dead device, which is a crash on next use rather than a visible bug --
    // hence the exhaustive list rather than a tidy subset.
    void WebGPURenderer::ReleaseAllGpuState()
    {
        // In-flight frame objects first; the pass must not outlive the device.
        m_pass = nullptr;
        m_encoder = nullptr;
        m_frameOpen = false;

        // Textures: keep `pixels`, `name`, `format` and the dimensions, which
        // are the authoritative copy. Drop only the downstream GPU objects, and
        // mark dirty so the existing lazy upload path re-sends them.
        for (std::map<DWORD, Texture*>::iterator it = m_textures.begin();
             it != m_textures.end(); ++it)
        {
            Texture* t = it->second;
            if (!t) continue;
            t->gpu = nullptr;
            t->view = nullptr;
            t->bindGroup = nullptr;
            t->dirty = true;
            // A texture left `locked` across a loss would never be unlocked by
            // the game, because the lock belongs to a frame that no longer
            // exists.
            t->locked = false;
        }

        // Lazily-built caches. All are rebuilt on demand.
        m_pipelines.clear();
        m_samplers.clear();
        m_fogBindings.clear();

        // Vertex/index arenas. DROP THE CHUNKS ENTIRELY, do not merely null the
        // buffers -- that was the first version of this and it crashed on the
        // very first forced-loss test. VertexChunkFor only creates a buffer when
        // the active index runs off the end of the vector, so a chunk that keeps
        // its `capacity` gets REUSED with a null `gpu`, and UploadFrameArenas
        // then calls Queue::WriteBuffer on nothing. Clearing the vectors makes
        // the existing lazy path rebuild both the chunk and its buffer.
        m_vertexChunks.clear();
        m_indexChunks.clear();
        m_activeVertexChunk = 0;
        m_activeIndexChunk = 0;
        m_quadIndexBuffer = nullptr;
        m_quadIndexCapacity = 0;

        // Offscreen targets. Zeroing the size forces EnsureTargets to rebuild
        // rather than short-circuit on a matching width/height.
        m_colorTarget = nullptr; m_colorView = nullptr;
        m_depthTarget = nullptr; m_depthView = nullptr;
        m_targetWidth = 0; m_targetHeight = 0;

        // Shared resources built by EnsureSharedResources.
        m_proofPipeline = nullptr; m_proofBindLayout = nullptr; m_proofBindGroup = nullptr;
        m_proofVertexBuffer = nullptr; m_proofTexture = nullptr;
        m_proofTextureView = nullptr; m_proofSampler = nullptr;
        m_viewportBuffer = nullptr; m_canvasViewportBuffer = nullptr;
        m_viewportBindLayout = nullptr; m_samplerBindLayout = nullptr;
        m_textureBindLayout = nullptr; m_fogBindLayout = nullptr;
        m_pipelineLayout = nullptr;
        m_viewportBindGroup = nullptr; m_canvasViewportBindGroup = nullptr;
        m_whiteTexture = nullptr; m_whiteTextureView = nullptr; m_whiteBindGroup = nullptr;

        // Surface and its blit resources.
        m_surfaceTexture = nullptr;
        m_surfaceBindGroup = nullptr; m_surfaceBindLayout = nullptr;
        m_surfaceSampler = nullptr; m_surfacePipeline = nullptr;
        m_surface = nullptr;
        m_surfaceConfigured = false;
        m_surfaceFrameAcquired = false;

        // Finally the device itself and everything it came from.
        m_queue = nullptr;
        m_device = nullptr;
        m_adapter = nullptr;
        m_instance = nullptr;
    }

    bool WebGPURenderer::RecoverFromDeviceLoss()
    {
        ++m_deviceLossRecoveries;
        std::fprintf(stderr,
                     "[webgpu] device-loss recovery attempt %u: rebuilding the "
                     "device and every GPU object\n", m_deviceLossRecoveries);
        std::fflush(stderr);

        ReleaseAllGpuState();

        if (!Init())
        {
            std::fprintf(stderr,
                         "[webgpu] device-loss recovery FAILED -- could not "
                         "recreate the device.\n");
            std::fflush(stderr);
            return false;
        }

        // Pipelines, samplers, targets, shared resources and the surface rebuild
        // lazily from here: BeginFrame calls EnsureTargets and
        // EnsureSharedResources, and EnsureSurfaceConfigured re-attaches to the
        // same native window.
        //
        // TEXTURES DO NOT. There is no create-on-bind path for them, so they are
        // rebuilt eagerly here -- see RecreateAllTextures for what happens if you
        // assume otherwise.
        RecreateAllTextures();

        std::fprintf(stderr, "[webgpu] device-loss recovery %u succeeded\n",
                     m_deviceLossRecoveries);
        std::fflush(stderr);
        return true;
    }

    const char* WebGPURenderer::Name() const  { return "webgpu"; }



    //-- frame boundary ----------------------------------------------------//
    void WebGPURenderer::SetViewportBackground(bool fill, DWORD color)
    {
        if (!fill)
        {
            m_viewportBackground = { 0.0, 0.0, 0.0, 1.0 };
            return;
        }
        // MC2 packs this as 0x00RRGGBB -- Camera::fogColor is built as
        // (r << 16) + (g << 8) + b -- so there is no alpha to read.
        m_viewportBackground.r = static_cast<double>((color >> 16) & 0xFF) / 255.0;
        m_viewportBackground.g = static_cast<double>((color >>  8) & 0xFF) / 255.0;
        m_viewportBackground.b = static_cast<double>((color      ) & 0xFF) / 255.0;
        m_viewportBackground.a = 1.0;
    }

    void WebGPURenderer::BeginFrame()
    {
        ++m_frameNumber;
        m_frameOpen = false;

        // PORT (M8-B19): FORCE A LOSS ON DEMAND, so the recovery path is
        // testable without waiting for a driver fault. MC2_FORCE_DEVICE_LOSS=N
        // raises the flag on frame N, and "N,M,..." raises it on each listed
        // frame so repeated recovery can be exercised too.
        //
        // WHAT THIS DOES AND DOES NOT PROVE. It exercises the whole teardown,
        // rebuild and lazy-repopulate path, which is where the bugs live -- a
        // missed handle here is a stale pointer into a dead device. It does NOT
        // reproduce a driver-side removal, where the adapter itself may be gone
        // and Init() can legitimately fail; that path is covered by the retry
        // cap, not by this switch.
        {
            static bool parsed = false;
            static std::vector<unsigned long> forceFrames;
            if (!parsed)
            {
                parsed = true;
                if (const char* spec = std::getenv("MC2_FORCE_DEVICE_LOSS"))
                    for (const char* p = spec; *p; )
                    {
                        char* end = nullptr;
                        const unsigned long v = std::strtoul(p, &end, 10);
                        if (end == p) break;
                        forceFrames.push_back(v);
                        p = (*end == ',') ? end + 1 : end;
                    }
            }
            for (size_t i = 0; i < forceFrames.size(); ++i)
                if (forceFrames[i] == m_frameNumber)
                {
                    std::fprintf(stderr,
                                 "[webgpu] MC2_FORCE_DEVICE_LOSS: simulating a "
                                 "device loss at frame %lu\n", m_frameNumber);
                    std::fflush(stderr);
                    g_webgpuDeviceLost.store(true, std::memory_order_release);
                }
        }

        // PORT (M8-B19): consume a device loss here, on the render thread.
        //
        // THE CAP IS THE POINT. Without it, a GPU that is genuinely gone (pulled
        // eGPU, driver uninstalled mid-session) turns into an infinite
        // rebuild-fail loop that is exactly as useless as the silent black
        // screen this replaces, and harder to diagnose. After the cap the
        // renderer stops pretending: it says so once, loudly, and asks the game
        // to quit rather than keep simulating a session nobody can see.
        if (g_webgpuDeviceLost.exchange(false, std::memory_order_acq_rel) &&
            !m_deviceLossFatal)
        {
            // THE CAP COUNTS CONSECUTIVE LOSSES, NOT LIFETIME ONES. A machine
            // that loses the device once an hour and recovers cleanly every
            // time must keep recovering forever -- that is the whole point of
            // the feature. What must not be tolerated is a REBUILD LOOP, where
            // the device dies again immediately after each rebuild. So a loss
            // that arrives well clear of the last one resets the count, and only
            // a tight cluster trips the limit.
            const unsigned kMaxRecoveries = 4;
            const unsigned long kQuietFrames = 600;   // a few seconds of real play
            if (m_deviceLossRecoveries &&
                m_frameNumber - m_lastDeviceLossFrame > kQuietFrames)
            {
                std::fprintf(stderr,
                             "[webgpu] %lu frames rendered cleanly since the last "
                             "device loss; treating this as a new one\n",
                             m_frameNumber - m_lastDeviceLossFrame);
                m_deviceLossRecoveries = 0;
            }
            m_lastDeviceLossFrame = m_frameNumber;

            if (m_deviceLossRecoveries >= kMaxRecoveries || !RecoverFromDeviceLoss())
            {
                m_deviceLossFatal = true;
                std::fprintf(stderr,
                             "[webgpu] giving up after %u device losses in quick "
                             "succession. The graphics device cannot be kept alive; "
                             "ending the session rather than running blind.\n",
                             m_deviceLossRecoveries);
                std::fflush(stderr);
                MC2_PortMessageBox(nullptr,
                    "The graphics device was lost and could not be restored.\n\n"
                    "MechCommander 2 has to close. Your last save is intact.",
                    "MechCommander 2", 0);
                g_gosQuitRequested = true;
            }
        }
        if (m_deviceLossFatal) return;

        const int requestedWidth = Environment.screenWidth;
        const int requestedHeight = Environment.screenHeight;
        if (requestedWidth <= 0 || requestedHeight <= 0)
        {
            std::fprintf(stderr, "[webgpu] invalid frame size %dx%d\n",
                         requestedWidth, requestedHeight);
            return;
        }

        const uint32_t width = static_cast<uint32_t>(requestedWidth);
        const uint32_t height = static_cast<uint32_t>(requestedHeight);
        if (!EnsureTargets(width, height) || !EnsureSharedResources()) return;
#if defined(__EMSCRIPTEN__)
        EnsureSurfaceConfigured();
#else
        if (m_nativeWindow) EnsureSurfaceConfigured();
#endif

        for (VertexArenaChunk& chunk : m_vertexChunks) chunk.cpu.clear();
        for (IndexArenaChunk& chunk : m_indexChunks) chunk.cpu.clear();
        m_activeVertexChunk = 0;
        m_activeIndexChunk = 0;
        m_frameArenaBytes = 0;
        m_currentFrameDrawCalls = 0;
        m_currentFrameUploadBytes = 0;

        // PORT: mission terrain and the 800x600 loading UI can be submitted in
        // the same render pass. Keep distinct immutable-per-frame uniforms so
        // each draw binds the coordinate system active at that draw, rather
        // than every draw consuming whichever canvas existed at BeginFrame.
        // PORT (M8-B52): the canvas extent is whatever is in force, NOT a
        // hardcoded 800x600 -- see CanvasExtent() for what that cost.
        float canvasW = 800.0f, canvasH = 600.0f;
        CanvasExtent(&canvasW, &canvasH);
        const float canvasScaleX = static_cast<float>(width) / canvasW;
        const float canvasScaleY = static_cast<float>(height) / canvasH;
        const float canvasScale = std::min(canvasScaleX, canvasScaleY);
        const float canvasOffsetX = (static_cast<float>(width) - canvasW * canvasScale) * 0.5f;
        const float canvasOffsetY = (static_cast<float>(height) - canvasH * canvasScale) * 0.5f;
        const ViewportUniform viewport = {
            static_cast<float>(width), static_cast<float>(height),
            1.0f, 0.0f, 0.0f, 0.0f, { 0.0f, 0.0f }
        };
        m_queue.WriteBuffer(m_viewportBuffer, 0, &viewport, sizeof(viewport));
        const ViewportUniform canvasViewport = {
            static_cast<float>(width), static_cast<float>(height),
            canvasScale, 0.0f, canvasOffsetX, canvasOffsetY, { 0.0f, 0.0f }
        };
        m_queue.WriteBuffer(m_canvasViewportBuffer, 0, &canvasViewport, sizeof(canvasViewport));

        wgpu::CommandEncoderDescriptor encoderDesc = {};
        encoderDesc.label = "MC2 offscreen frame encoder";
        m_encoder = m_device.CreateCommandEncoder(&encoderDesc);
        if (!m_encoder) return;

        wgpu::RenderPassColorAttachment color = {};
        color.view = m_colorView;
        color.loadOp = wgpu::LoadOp::Clear;
        color.storeOp = wgpu::StoreOp::Store;
        // PORT: The logistics art deliberately leaves transparent gaps for a
        // black backplate. The old #112233 bring-up clear leaked through every
        // screen and made those authored black regions look navy. So black
        // stays the default and ONLY an explicit gos_SetupViewport fill moves
        // it -- which is what the game asks for, and only while a mission is
        // active (MechCmd2.cpp passes eye->fogColor there and 0 elsewhere).
        color.clearValue = m_viewportBackground;

        wgpu::RenderPassDepthStencilAttachment depth = {};
        depth.view = m_depthView;
        depth.depthLoadOp = wgpu::LoadOp::Clear;
        depth.depthStoreOp = wgpu::StoreOp::Store;
        depth.depthClearValue = 1.0f;
        depth.depthReadOnly = false;
        depth.stencilLoadOp = wgpu::LoadOp::Undefined;
        depth.stencilStoreOp = wgpu::StoreOp::Undefined;

        wgpu::RenderPassDescriptor passDesc = {};
        passDesc.label = "MC2 offscreen frame";
        passDesc.colorAttachmentCount = 1;
        passDesc.colorAttachments = &color;
        passDesc.depthStencilAttachment = &depth;

        m_pass = m_encoder.BeginRenderPass(&passDesc);
        if (!m_pass) return;
        m_frameOpen = true;
    }



    // PORT (M8-B18): SPLIT EndFrame. It is 1.03-1.07 ms of an 8.3 ms frame and
    // is the last unexamined block. The frame is NOT GPU fill-bound -- measured
    // by taking the offscreen target from 1280x720 to 2560x1440, four times the
    // pixels, for 8.391 ms against 8.301 ms -- so if this ms is GPU-facing it is
    // submit or present, not rasterisation. Armed by MC2_RENDER_SPLIT.
    namespace
    {
        double g_efPassEndMs = 0.0, g_efEncodeMs = 0.0, g_efFinishMs = 0.0;
        double g_efUploadMs = 0.0, g_efSubmitMs = 0.0, g_efPresentMs = 0.0;
        long   g_efFrames = 0;
        bool   g_efOn = false, g_efChecked = false;

        void EndFrameSplitSummary()
        {
            if (!g_efOn || g_efFrames <= 0) return;
            const double n = static_cast<double>(g_efFrames);
            std::printf("[endframe] over %ld frames, mean ms: passEnd=%.3f "
                        "encodeSurface=%.3f finish=%.3f uploadArenas=%.3f "
                        "submit=%.3f present=%.3f (sum %.3f)\n",
                        g_efFrames, g_efPassEndMs / n, g_efEncodeMs / n,
                        g_efFinishMs / n, g_efUploadMs / n, g_efSubmitMs / n,
                        g_efPresentMs / n,
                        (g_efPassEndMs + g_efEncodeMs + g_efFinishMs +
                         g_efUploadMs + g_efSubmitMs + g_efPresentMs) / n);
            std::fflush(stdout);
        }

        bool EndFrameSplitArmed()
        {
            if (!g_efChecked)
            {
                g_efChecked = true;
                const char* e = std::getenv("MC2_RENDER_SPLIT");
                g_efOn = e && *e && std::strcmp(e, "0") != 0;
                if (g_efOn) std::atexit(EndFrameSplitSummary);
            }
            return g_efOn;
        }

        typedef std::chrono::steady_clock EFClock;
    }

    void WebGPURenderer::EndFrame()
    {
        if (!m_frameOpen) return;
        const bool efOn = EndFrameSplitArmed();
        EFClock::time_point efT = efOn ? EFClock::now() : EFClock::time_point();
        if (efOn) ++g_efFrames;
        DrawTextProof();
        // PORT: polled while the pass is still open. The HUD is SKIPPED on the
        // frame being captured, so a shot taken for measurement does not have
        // this overlay burnt into its top-left corner. One frame without it is
        // not perceptible.
        PollCaptureHotkey();
        if (!m_captureRequested) DrawHudOverlay();
        m_totalFrameDrawCalls += m_currentFrameDrawCalls;
        m_peakFrameDrawCalls = std::max(m_peakFrameDrawCalls, m_currentFrameDrawCalls);
        m_totalFrameUploadBytes += m_currentFrameUploadBytes;
        m_peakFrameUploadBytes = std::max(m_peakFrameUploadBytes, m_currentFrameUploadBytes);
        if (m_currentFrameUploadBytes) ++m_framesWithUploads;
        m_pass.End();
        m_frameOpen = false;
        if (efOn) { const EFClock::time_point t = EFClock::now();
            g_efPassEndMs += std::chrono::duration<double, std::milli>(t - efT).count();
            efT = t; }

        m_surfaceFrameAcquired = EncodeSurfaceFrame();
        if (efOn) { const EFClock::time_point t = EFClock::now();
            g_efEncodeMs += std::chrono::duration<double, std::milli>(t - efT).count();
            efT = t; }

        // PORT: m_dumped makes MC2_DUMP_FRAME a one-shot. The hotkey must stay
        // repeatable, so it rides a separate flag and does not set m_dumped.
        const bool dumpThisFrame = (!m_dumped && m_dumpFrame == m_frameNumber) ||
                                   m_captureRequested;
        wgpu::Buffer readback;
        uint32_t paddedBytesPerRow = 0;
        uint64_t readbackSize = 0;

        if (dumpThisFrame)
        {
            const uint32_t unpadded = m_targetWidth * 4;
            paddedBytesPerRow = (unpadded + kReadbackAlignment - 1) & ~(kReadbackAlignment - 1);
            readbackSize = static_cast<uint64_t>(paddedBytesPerRow) * m_targetHeight;

            wgpu::BufferDescriptor bufferDesc = {};
            bufferDesc.label = "MC2 frame readback";
            bufferDesc.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
            bufferDesc.size = readbackSize;
            readback = m_device.CreateBuffer(&bufferDesc);

            if (readback)
            {
                wgpu::TexelCopyTextureInfo source = {};
                source.texture = m_colorTarget;
                source.mipLevel = 0;
                source.origin = { 0, 0, 0 };
                source.aspect = wgpu::TextureAspect::All;

                wgpu::TexelCopyBufferInfo destination = {};
                destination.buffer = readback;
                destination.layout.offset = 0;
                destination.layout.bytesPerRow = paddedBytesPerRow;
                destination.layout.rowsPerImage = m_targetHeight;

                const wgpu::Extent3D copySize = { m_targetWidth, m_targetHeight, 1 };
                m_encoder.CopyTextureToBuffer(&source, &destination, &copySize);
            }
        }

        wgpu::CommandBuffer commands = m_encoder.Finish();
        if (efOn) { const EFClock::time_point t = EFClock::now();
            g_efFinishMs += std::chrono::duration<double, std::milli>(t - efT).count();
            efT = t; }
        UploadFrameArenas();
        if (efOn) { const EFClock::time_point t = EFClock::now();
            g_efUploadMs += std::chrono::duration<double, std::milli>(t - efT).count();
            efT = t; }
        m_queue.Submit(1, &commands);
        if (efOn) { const EFClock::time_point t = EFClock::now();
            g_efSubmitMs += std::chrono::duration<double, std::milli>(t - efT).count();
            efT = t; }

        if (m_surfaceFrameAcquired)
        {
#if defined(__EMSCRIPTEN__)
            // PORT: Browser WebGPU presents the canvas texture when the frame
            // returns to requestAnimationFrame; emdawn rejects Surface::Present.
            ++m_surfacePresented;
#else
            if (m_surface.Present()) ++m_surfacePresented;
            else std::fprintf(stderr, "[webgpu] surface present failed\n");
            if (efOn) { const EFClock::time_point t = EFClock::now();
                g_efPresentMs += std::chrono::duration<double, std::milli>(t - efT).count();
                efT = t; }
#endif
            m_surfaceTexture = nullptr;
            m_surfaceFrameAcquired = false;
        }

        if (dumpThisFrame && readback)
        {
            DumpReadback(readback, paddedBytesPerRow, readbackSize);
            // PORT: only the env-driven one-shot latches; the hotkey clears its
            // own request so the next Ctrl+Alt+S works too.
            if (!m_captureRequested) m_dumped = true;
            m_captureRequested = false;
            m_capturePath.clear();
        }
        else if (m_captureRequested)
        {
            std::fprintf(stderr, "[webgpu] screenshot requested but the readback "
                                 "buffer could not be created -- nothing written\n");
            m_captureRequested = false;
            m_capturePath.clear();
        }
    }



    void WebGPURenderer::SetWindow(void* nativeWindow, void* nativeDisplay,
                   int clientWidth, int clientHeight)
    {
        if (nativeWindow != m_nativeWindow || nativeDisplay != m_nativeDisplay)
        {
            if (m_surfaceConfigured && m_surface) m_surface.Unconfigure();
            m_surface = nullptr;
            m_surfacePipeline = nullptr;
            m_surfaceBindGroup = nullptr;
            m_surfaceConfigured = false;
            m_nativeWindow = nativeWindow;
            m_nativeDisplay = nativeDisplay;
        }
        if (clientWidth > 0 && clientHeight > 0)
        {
            m_requestedSurfaceWidth = static_cast<uint32_t>(clientWidth);
            m_requestedSurfaceHeight = static_cast<uint32_t>(clientHeight);
        }
    }



    //-- render state ------------------------------------------------------//
    // Recorded, not yet translated. Translation is per-pipeline and belongs with
    // the pipeline work; recording it here with the SAME tracker the null backend
    // uses is what makes the two directly comparable.
    void WebGPURenderer::SetRenderState(gos_RenderState state, intptr_t value)
    {
        if (state == gos_State_Fog)
        {
            // GameOS defines this state as a pointer to a DWORD colour. Copy it
            // now: most callers pass a stack local which is gone by draw time.
            // Bit 24 records enablement so black remains a valid fog colour.
            const int tracked = value ? static_cast<int>(
                (*reinterpret_cast<const DWORD*>(value) & 0x00ffffffu) | 0x01000000u) : 0;
            m_state.Set(state, tracked);
            return;
        }
        m_state.Set(state, static_cast<int>(value));
    }


    void WebGPURenderer::PushRenderStates()  { m_state.Push(); }


    void WebGPURenderer::PopRenderStates()   { m_state.Pop(); }



    //-- diagnostics -------------------------------------------------------//
    void WebGPURenderer::DumpStats()
    {
        std::printf("[webgpu] adapter='%s' backend=%s\n",
                    m_adapterName.c_str(), m_backendName.c_str());
        std::printf("[webgpu] textures=%u created=%lu gpuCreated=%lu gpuFailed=%lu\n",
                    (unsigned)m_textures.size(), m_stats.created,
                    m_stats.gpuCreated, m_stats.gpuFailed);
        std::printf("[webgpu] locks=%lu mipLocks=%lu uploads=%lu uploadedBytes=%llu\n",
                    m_stats.locks, m_stats.mipLocks, m_stats.uploads,
                    (unsigned long long)m_stats.uploadedBytes);
        std::printf("[webgpu] texture decode: decoded=%lu failed=%lu raw24=%lu raw32=%lu "
                    "rle=%lu nonSquare=%lu pinkKey=%lu forceAlpha=%lu dimensions=%u\n",
                    m_stats.decoded, m_stats.decodeFailed, m_stats.source24,
                    m_stats.source32, m_stats.rleDecoded, m_stats.nonSquareDecoded,
                    m_stats.pinkKeyed, m_stats.forceAlpha,
                    (unsigned)m_decodedDimensions.size());
        std::printf("[webgpu] decoded dimensions:");
        for (const std::pair<DWORD, DWORD>& dimensions : m_decodedDimensions)
            std::printf(" %lux%lu", (unsigned long)dimensions.first,
                        (unsigned long)dimensions.second);
        if (m_decodedDimensions.empty()) std::printf(" none");
        std::printf("\n");
        std::printf("[webgpu] text: fonts=%lu failed=%lu glyphs=%lu "
                    "backgrounds=%lu/%lu boldIgnored=%lu italicIgnored=%lu\n",
                    m_stats.fontsLoaded, m_stats.fontLoadFailed, m_stats.glyphsDrawn,
                    m_stats.textBackgroundsSubmitted, m_stats.textBackgrounds,
                    m_stats.boldIgnored, m_stats.italicIgnored);
        if (const char* e = std::getenv("MC2_TEXT_CENSUS"); e && *e && std::strcmp(e, "0") != 0)
            std::printf("[text-census] textDraws=%lu slashDraws=%lu glyphs=%lu fonts=%lu "
                        "failed=%lu bold=%lu italic=%lu\n",
                        m_stats.textDraws, m_stats.slashDraws, m_stats.glyphsDrawn,
                        m_stats.fontsLoaded, m_stats.fontLoadFailed,
                        m_stats.boldIgnored, m_stats.italicIgnored);
        std::printf("[webgpu] draws submitted/accounted: points=%lu/%lu lines=%lu/%lu "
                    "tris=%lu/%lu quads=%lu/%lu indexed=%lu/%lu "
                    "(verts=%llu idx=%llu) textDraws=%lu\n",
                    m_stats.submittedPoints, m_stats.points,
                    m_stats.submittedLines, m_stats.lines,
                    m_stats.submittedTriangles, m_stats.triangles,
                    m_stats.submittedQuads, m_stats.quads,
                    m_stats.submittedIndexed, m_stats.indexed,
                    (unsigned long long)m_stats.indexedVerts,
                    (unsigned long long)m_stats.indexedIndices,
                    m_stats.textDraws);
        std::printf("[webgpu] indexed formats submitted/accounted: "
                    "1UV=%lu/%lu 2UV=%lu/%lu 3UV=%lu/%lu raw=%lu/%lu\n",
                    m_stats.submittedIndexed1UV, m_stats.indexed1UV,
                    m_stats.submittedIndexed2UV, m_stats.indexed2UV,
                    m_stats.submittedIndexed3UV, m_stats.indexed3UV,
                    m_stats.submittedIndexedRaw, m_stats.indexedRaw);
        std::printf("[webgpu] draw resources: pipelines=%u samplers=%u textureBindGroups=%lu "
                    "peakVertexArenaBytes=%llu maxQuadBatch=%lu unsupportedRaw=%lu flatTextures=%s\n",
                    (unsigned)m_pipelines.size(), (unsigned)m_samplers.size(),
                    m_textureBindGroupCount, (unsigned long long)m_peakArenaBytes,
                    m_maxQuadBatch, m_unsupportedRawDraws, m_flatTextures ? "yes" : "no");
        const unsigned long long frameCount = m_frameNumber ? m_frameNumber : 1;
        std::printf("[webgpu] per-frame: drawCalls avg=%.2f peak=%lu "
                    "textureUploadBytes avg=%.2f peak=%llu activeFrames=%lu/%lu\n",
                    static_cast<double>(m_totalFrameDrawCalls) / frameCount,
                    m_peakFrameDrawCalls,
                    static_cast<double>(m_totalFrameUploadBytes) / frameCount,
                    m_peakFrameUploadBytes, m_framesWithUploads, m_frameNumber);
        std::printf("[webgpu] submitted geometry draws by alpha: "
                    "1(OneZero)=%llu 2(OneOne)=%llu 3(AlphaInvAlpha)=%llu "
                    "4(OneInvAlpha)=%llu unknown=%llu\n",
                    m_geometryDrawsByAlphaMode[1], m_geometryDrawsByAlphaMode[2],
                    m_geometryDrawsByAlphaMode[3], m_geometryDrawsByAlphaMode[4],
                    m_geometryDrawsByAlphaMode[0]);
        if (m_state.StackErrors())
            std::printf("[webgpu] render state stack errors: %lu\n", m_state.StackErrors());

        if (std::getenv("MC2_FOG_CENSUS"))
        {
            std::fprintf(stderr,
                "[fog-census] FINAL state ON draws=%lu verts=%lu carrying=%lu minAlpha=%u | "
                "state OFF draws=%lu verts=%lu carrying=%lu minAlpha=%u\n",
                m_fogDraws[1], m_fogVerts[1], m_fogCarrying[1],
                static_cast<unsigned>(m_fogMinAlpha[1]),
                m_fogDraws[0], m_fogVerts[0], m_fogCarrying[0],
                static_cast<unsigned>(m_fogMinAlpha[0]));
        }

        m_state.DumpCensus("webgpu");

        std::printf("[webgpu] offscreen frames=%lu target=%ux%u dumpRequested=%lu dumped=%s\n",
                    m_frameNumber, m_targetWidth, m_targetHeight, m_dumpFrame,
                    m_dumped ? "yes" : "no");
        std::printf("[webgpu] surface: requested=%s configured=%s size=%ux%u "
                    "configurations=%lu presented=%lu suboptimal=%lu timeouts=%lu "
                    "outdated=%lu lost=%lu\n",
                    m_nativeWindow ? "yes" : "no", m_surfaceConfigured ? "yes" : "no",
                    m_surfaceWidth, m_surfaceHeight, m_surfaceConfigurations,
                    m_surfacePresented, m_surfaceSuboptimal, m_surfaceTimeouts,
                    m_surfaceOutdated, m_surfaceLost);
        std::printf("[webgpu] state mapping: Dither and Clipping recorded but ignored "
                    "(no WebGPU equivalents); culling remains off.\n");
        std::printf("[webgpu] NOTE: game geometry, decoded textures, and font glyphs are "
                    "rendered offscreen; an optional native surface presents that image.\n");
    }



    const char* WebGPURenderer::backendTypeNameRaw(wgpu::BackendType b)
    {
        switch (b)
        {
            case wgpu::BackendType::Null:     return "Null";
            case wgpu::BackendType::WebGPU:   return "WebGPU";
            case wgpu::BackendType::D3D11:    return "D3D11";
            case wgpu::BackendType::D3D12:    return "D3D12";
            case wgpu::BackendType::Metal:    return "Metal";
            case wgpu::BackendType::Vulkan:   return "Vulkan";
            case wgpu::BackendType::OpenGL:   return "OpenGL";
            case wgpu::BackendType::OpenGLES: return "OpenGLES";
            default:                          return "Undefined";
        }
    }



    std::string WebGPURenderer::backendTypeName(wgpu::BackendType b) { return backendTypeNameRaw(b); }



    const char* WebGPURenderer::adapterTypeName(wgpu::AdapterType t)
    {
        switch (t)
        {
            case wgpu::AdapterType::DiscreteGPU:   return "DiscreteGPU";
            case wgpu::AdapterType::IntegratedGPU: return "IntegratedGPU";
            case wgpu::AdapterType::CPU:           return "CPU";
            default:                                return "Unknown";
        }
    }

} // namespace mc2webgpu
