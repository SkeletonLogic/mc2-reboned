#include "renderer_webgpu_internal.h"

namespace mc2webgpu {

bool WebGPURenderer::EnsureSurfaceConfigured()
    {
        if (m_requestedSurfaceWidth == 0 || m_requestedSurfaceHeight == 0)
            return false;
#if !defined(__EMSCRIPTEN__)
        if (!m_nativeWindow || !m_nativeDisplay) return false;
#endif

        if (!m_surface)
        {
#if defined(__EMSCRIPTEN__)
            // PORT: emdawn binds this selector to the canvas supplied by the
            // MC3 shell; the renderer remains otherwise shared with native.
            wgpu::EmscriptenSurfaceSourceCanvasHTMLSelector source;
            source.selector = "#canvas";
#else
            wgpu::SurfaceSourceWindowsHWND source;
            source.hinstance = m_nativeDisplay;
            source.hwnd = m_nativeWindow;
#endif
            wgpu::SurfaceDescriptor descriptor = {};
            descriptor.label = "MechCommander 2 surface";
            descriptor.nextInChain = &source;
            m_surface = m_instance.CreateSurface(&descriptor);
            if (!m_surface)
            {
                std::fprintf(stderr, "[webgpu] CreateSurface failed\n");
                return false;
            }

            wgpu::SurfaceCapabilities capabilities;
            if (!m_surface.GetCapabilities(m_adapter, &capabilities) ||
                capabilities.formatCount == 0)
            {
                std::fprintf(stderr, "[webgpu] surface has no supported formats\n");
                m_surface = nullptr;
                return false;
            }
            m_surfaceFormat = capabilities.formats[0];
            for (size_t i = 0; i < capabilities.formatCount; ++i)
                if (capabilities.formats[i] == wgpu::TextureFormat::BGRA8Unorm)
                    m_surfaceFormat = capabilities.formats[i];

            // PORT (M8-B18): FIFO is vsync, and GetCurrentTexture BLOCKS on it.
            // Measured: the only part of EndFrame that moves when the CPU speeds
            // up is the surface acquire -- encodeSurface went 0.427 -> 0.591 ms
            // when parallel objects were switched on, while submit, present,
            // finish and the arena upload all stayed flat. That is the frame
            // arriving early and waiting for a vblank slot.
            //
            // DEFAULT IS MAILBOX, owner-confirmed in play 2026-08-24. Mailbox
            // does not block on the vblank and, unlike immediate, DOES NOT TEAR.
            //
            // Measured at the heavy repro camera: fifo 120.9/121.1/121.1 fps
            // against mailbox 132.0/130.4/130.5, with the surface acquire
            // dropping from 0.503 ms to 0.019 ms. Measured in the owner's own
            // session: 94,463 frames at 2.193 ms mean against 7.158 and 7.298 ms
            // in the two fifo sessions before it -- those were not the machine's
            // limit, they were the 144 Hz vblank. The rendered picture is
            // pixel-identical to fifo, verified against the framebuffer gate.
            // Their run reported 0 suboptimal, 0 timeout, 0 outdated, 0 lost
            // across all 94,463 presents, and the owner confirmed it looked
            // right.
            //
            // MC2_PRESENT_MODE=fifo|mailbox|immediate overrides. Anything not
            // supported by the surface falls back to fifo, LOUDLY -- a silent
            // fallback would make a null measurement look like a null result.
            wgpu::PresentMode wanted = wgpu::PresentMode::Mailbox;
            const char* modeEnv = std::getenv("MC2_PRESENT_MODE");
            if (modeEnv && *modeEnv)
            {
                if (std::strcmp(modeEnv, "mailbox") == 0)   wanted = wgpu::PresentMode::Mailbox;
                else if (std::strcmp(modeEnv, "immediate") == 0) wanted = wgpu::PresentMode::Immediate;
                else if (std::strcmp(modeEnv, "fifo") != 0)
                    std::fprintf(stderr, "[webgpu] MC2_PRESENT_MODE must be "
                                         "fifo, mailbox or immediate\n");
            }

            m_surfacePresentMode = capabilities.presentModeCount ?
                capabilities.presentModes[0] : wgpu::PresentMode::Fifo;
            bool wantedAvailable = false;
            for (size_t i = 0; i < capabilities.presentModeCount; ++i)
                if (capabilities.presentModes[i] == wanted)
                { m_surfacePresentMode = capabilities.presentModes[i]; wantedAvailable = true; }
            if (!wantedAvailable)
            {
                // Fall back to FIFO, which every implementation must support,
                // and SAY SO -- a silent fallback would make a null measurement
                // look like a null result.
                for (size_t i = 0; i < capabilities.presentModeCount; ++i)
                    if (capabilities.presentModes[i] == wgpu::PresentMode::Fifo)
                        m_surfacePresentMode = capabilities.presentModes[i];
                if (modeEnv && *modeEnv)
                    std::fprintf(stderr, "[webgpu] MC2_PRESENT_MODE=%s is NOT "
                                         "supported here; fell back to fifo\n", modeEnv);
            }

            m_surfaceAlphaMode = capabilities.alphaModeCount ?
                capabilities.alphaModes[0] : wgpu::CompositeAlphaMode::Opaque;
            for (size_t i = 0; i < capabilities.alphaModeCount; ++i)
                if (capabilities.alphaModes[i] == wgpu::CompositeAlphaMode::Opaque)
                    m_surfaceAlphaMode = capabilities.alphaModes[i];

            std::printf("[webgpu] surface ready: format=%u presentMode=%u alphaMode=%u\n",
                        static_cast<unsigned>(m_surfaceFormat),
                        static_cast<unsigned>(m_surfacePresentMode),
                        static_cast<unsigned>(m_surfaceAlphaMode));
        }

        if (m_surfaceConfigured && m_surfaceWidth == m_requestedSurfaceWidth &&
            m_surfaceHeight == m_requestedSurfaceHeight)
            return true;

        wgpu::SurfaceConfiguration configuration = {};
        configuration.device = m_device;
        configuration.format = m_surfaceFormat;
        configuration.usage = wgpu::TextureUsage::RenderAttachment;
        configuration.width = m_requestedSurfaceWidth;
        configuration.height = m_requestedSurfaceHeight;
        configuration.presentMode = m_surfacePresentMode;
        configuration.alphaMode = m_surfaceAlphaMode;
        m_surface.Configure(&configuration);
        m_surfaceWidth = m_requestedSurfaceWidth;
        m_surfaceHeight = m_requestedSurfaceHeight;
        m_surfaceConfigured = true;
        ++m_surfaceConfigurations;
        std::printf("[webgpu] surface configured: %ux%u (#%lu)\n",
                    m_surfaceWidth, m_surfaceHeight, m_surfaceConfigurations);
        return EnsureSurfaceBlitResources();
    }



    bool WebGPURenderer::EnsureSurfaceBlitResources()
    {
        if (!m_surfacePipeline)
        {
            static const char shaderSource[] = R"WGSL(
struct VertexOut {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
};
@group(0) @binding(0) var frameSampler: sampler;
@group(0) @binding(1) var frameTexture: texture_2d<f32>;
@vertex
fn vs_main(@builtin(vertex_index) index: u32) -> VertexOut {
    let positions = array<vec2<f32>, 3>(
        vec2<f32>(-1.0, -1.0), vec2<f32>(3.0, -1.0), vec2<f32>(-1.0, 3.0));
    let p = positions[index];
    var out: VertexOut;
    out.position = vec4<f32>(p, 0.0, 1.0);
    out.uv = vec2<f32>(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);
    return out;
}
@fragment
fn fs_main(in: VertexOut) -> @location(0) vec4<f32> {
    return textureSample(frameTexture, frameSampler, in.uv);
}
)WGSL";
            wgpu::ShaderSourceWGSL wgsl = {};
            wgsl.code = std::string_view(shaderSource);
            wgpu::ShaderModuleDescriptor shaderDescriptor = {};
            shaderDescriptor.label = "MC2 surface blit shader";
            shaderDescriptor.nextInChain = &wgsl;
            const wgpu::ShaderModule shader = m_device.CreateShaderModule(&shaderDescriptor);

            wgpu::BindGroupLayoutEntry entries[2] = {};
            entries[0].binding = 0;
            entries[0].visibility = wgpu::ShaderStage::Fragment;
            entries[0].sampler.type = wgpu::SamplerBindingType::Filtering;
            entries[1].binding = 1;
            entries[1].visibility = wgpu::ShaderStage::Fragment;
            entries[1].texture.sampleType = wgpu::TextureSampleType::Float;
            entries[1].texture.viewDimension = wgpu::TextureViewDimension::e2D;
            wgpu::BindGroupLayoutDescriptor layoutDescriptor = {};
            layoutDescriptor.label = "MC2 surface blit bind layout";
            layoutDescriptor.entryCount = 2;
            layoutDescriptor.entries = entries;
            m_surfaceBindLayout = m_device.CreateBindGroupLayout(&layoutDescriptor);

            wgpu::PipelineLayoutDescriptor pipelineLayoutDescriptor = {};
            pipelineLayoutDescriptor.label = "MC2 surface blit pipeline layout";
            pipelineLayoutDescriptor.bindGroupLayoutCount = 1;
            pipelineLayoutDescriptor.bindGroupLayouts = &m_surfaceBindLayout;
            const wgpu::PipelineLayout pipelineLayout =
                m_device.CreatePipelineLayout(&pipelineLayoutDescriptor);

            wgpu::ColorTargetState target = {};
            target.format = m_surfaceFormat;
            target.writeMask = wgpu::ColorWriteMask::All;
            wgpu::FragmentState fragment = {};
            fragment.module = shader;
            fragment.entryPoint = "fs_main";
            fragment.targetCount = 1;
            fragment.targets = &target;
            wgpu::RenderPipelineDescriptor pipelineDescriptor = {};
            pipelineDescriptor.label = "MC2 surface blit pipeline";
            pipelineDescriptor.layout = pipelineLayout;
            pipelineDescriptor.vertex.module = shader;
            pipelineDescriptor.vertex.entryPoint = "vs_main";
            pipelineDescriptor.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
            pipelineDescriptor.primitive.cullMode = wgpu::CullMode::None;
            pipelineDescriptor.multisample.count = 1;
            pipelineDescriptor.fragment = &fragment;
            m_surfacePipeline = m_device.CreateRenderPipeline(&pipelineDescriptor);

            wgpu::SamplerDescriptor samplerDescriptor = {};
            samplerDescriptor.label = "MC2 surface blit sampler";
            samplerDescriptor.addressModeU = wgpu::AddressMode::ClampToEdge;
            samplerDescriptor.addressModeV = wgpu::AddressMode::ClampToEdge;
            samplerDescriptor.addressModeW = wgpu::AddressMode::ClampToEdge;
            samplerDescriptor.magFilter = wgpu::FilterMode::Linear;
            samplerDescriptor.minFilter = wgpu::FilterMode::Linear;
            m_surfaceSampler = m_device.CreateSampler(&samplerDescriptor);
            if (!m_surfacePipeline || !m_surfaceBindLayout || !m_surfaceSampler)
                return false;
        }

        if (!m_surfaceBindGroup)
        {
            wgpu::BindGroupEntry entries[2] = {};
            entries[0].binding = 0;
            entries[0].sampler = m_surfaceSampler;
            entries[1].binding = 1;
            entries[1].textureView = m_colorView;
            wgpu::BindGroupDescriptor descriptor = {};
            descriptor.label = "MC2 surface blit bind group";
            descriptor.layout = m_surfaceBindLayout;
            descriptor.entryCount = 2;
            descriptor.entries = entries;
            m_surfaceBindGroup = m_device.CreateBindGroup(&descriptor);
        }
        return !!m_surfaceBindGroup;
    }



    bool WebGPURenderer::EncodeSurfaceFrame()
    {
        if (!m_surfaceConfigured || !m_surface || !EnsureSurfaceBlitResources()) return false;

        wgpu::SurfaceTexture acquired = {};
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            m_surface.GetCurrentTexture(&acquired);
            if (acquired.status == wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal ||
                acquired.status == wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal)
            {
                if (acquired.status == wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal)
                    ++m_surfaceSuboptimal;
                break;
            }
            if (acquired.status == wgpu::SurfaceGetCurrentTextureStatus::Timeout)
            { ++m_surfaceTimeouts; return false; }
            if (acquired.status == wgpu::SurfaceGetCurrentTextureStatus::Outdated)
            {
                ++m_surfaceOutdated;
                m_surfaceConfigured = false;
                if (!EnsureSurfaceConfigured()) return false;
                continue;
            }
            if (acquired.status == wgpu::SurfaceGetCurrentTextureStatus::Lost)
            {
                ++m_surfaceLost;
                m_surface = nullptr;
                m_surfaceConfigured = false;
                if (!EnsureSurfaceConfigured()) return false;
                continue;
            }
            std::fprintf(stderr, "[webgpu] GetCurrentTexture failed (status=%u)\n",
                         static_cast<unsigned>(acquired.status));
            return false;
        }
        if (!acquired.texture) return false;

        m_surfaceTexture = acquired.texture;
        const wgpu::TextureView view = m_surfaceTexture.CreateView();
        wgpu::RenderPassColorAttachment color = {};
        color.view = view;
        color.loadOp = wgpu::LoadOp::Clear;
        color.storeOp = wgpu::StoreOp::Store;
        color.clearValue = { 0.0, 0.0, 0.0, 1.0 };
        wgpu::RenderPassDescriptor descriptor = {};
        descriptor.label = "MC2 present blit";
        descriptor.colorAttachmentCount = 1;
        descriptor.colorAttachments = &color;
        wgpu::RenderPassEncoder pass = m_encoder.BeginRenderPass(&descriptor);
        if (!pass) return false;
        pass.SetPipeline(m_surfacePipeline);
        pass.SetBindGroup(0, m_surfaceBindGroup);
        pass.Draw(3);
        pass.End();
        return true;
    }

} // namespace mc2webgpu
