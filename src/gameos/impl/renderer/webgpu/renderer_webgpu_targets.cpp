#include "renderer_webgpu_internal.h"

namespace mc2webgpu {


    bool WebGPURenderer::EnsureTargets(uint32_t width, uint32_t height)
    {
        if (m_colorTarget && m_depthTarget && width == m_targetWidth && height == m_targetHeight)
            return true;

        if (m_colorTarget) m_colorTarget.Destroy();
        if (m_depthTarget) m_depthTarget.Destroy();
        m_colorView = nullptr;
        m_depthView = nullptr;
        m_colorTarget = nullptr;
        m_depthTarget = nullptr;

        wgpu::TextureDescriptor colorDesc = {};
        colorDesc.label = "MC2 offscreen colour";
        colorDesc.usage = wgpu::TextureUsage::RenderAttachment |
                          wgpu::TextureUsage::CopySrc |
                          wgpu::TextureUsage::TextureBinding;
        colorDesc.dimension = wgpu::TextureDimension::e2D;
        colorDesc.size = { width, height, 1 };
        colorDesc.format = wgpu::TextureFormat::RGBA8Unorm;
        colorDesc.mipLevelCount = 1;
        colorDesc.sampleCount = 1;
        m_colorTarget = m_device.CreateTexture(&colorDesc);

        wgpu::TextureDescriptor depthDesc = {};
        depthDesc.label = "MC2 offscreen depth";
        depthDesc.usage = wgpu::TextureUsage::RenderAttachment;
        depthDesc.dimension = wgpu::TextureDimension::e2D;
        depthDesc.size = { width, height, 1 };
        depthDesc.format = wgpu::TextureFormat::Depth24Plus;
        depthDesc.mipLevelCount = 1;
        depthDesc.sampleCount = 1;
        m_depthTarget = m_device.CreateTexture(&depthDesc);

        if (!m_colorTarget || !m_depthTarget)
        {
            std::fprintf(stderr, "[webgpu] failed to create %ux%u offscreen targets\n",
                         width, height);
            return false;
        }

        m_colorView = m_colorTarget.CreateView();
        m_surfaceBindGroup = nullptr;
        m_depthView = m_depthTarget.CreateView();
        if (!m_colorView || !m_depthView)
        {
            std::fprintf(stderr, "[webgpu] failed to create offscreen target views\n");
            return false;
        }

        m_targetWidth = width;
        m_targetHeight = height;
        std::printf("[webgpu] offscreen targets ready: %ux%u RGBA8Unorm + Depth24Plus\n",
                    width, height);
        return true;
    }



    void WebGPURenderer::DumpReadback(wgpu::Buffer& readback, uint32_t paddedBytesPerRow,
                      uint64_t readbackSize)
    {
        wgpu::MapAsyncStatus mapStatus = wgpu::MapAsyncStatus::Error;
        std::string mapMessage;
        const wgpu::Future future = readback.MapAsync(
            wgpu::MapMode::Read, 0, readbackSize, wgpu::CallbackMode::WaitAnyOnly,
            [&](wgpu::MapAsyncStatus status, wgpu::StringView message)
            {
                mapStatus = status;
                mapMessage = std::string(sv(message));
            });

        const wgpu::WaitStatus waitStatus = m_instance.WaitAny(future, UINT64_MAX);
        if (waitStatus != wgpu::WaitStatus::Success ||
            mapStatus != wgpu::MapAsyncStatus::Success)
        {
            std::fprintf(stderr, "[webgpu] frame readback map failed (wait=%u map=%u): %.*s\n",
                         (unsigned)waitStatus, (unsigned)mapStatus,
                         (int)mapMessage.size(), mapMessage.data());
            return;
        }

        const unsigned char* pixels = static_cast<const unsigned char*>(
            readback.GetConstMappedRange(0, readbackSize));
        if (!pixels)
        {
            std::fprintf(stderr, "[webgpu] frame readback returned no mapped bytes\n");
            readback.Unmap();
            return;
        }

        // PORT: the Ctrl+Alt+S hotkey sets m_capturePath; MC2_DUMP_FRAME keeps
        // the original name in the run directory.
        char fileName[512];
        if (!m_capturePath.empty())
            std::snprintf(fileName, sizeof(fileName), "%s", m_capturePath.c_str());
        else
            std::snprintf(fileName, sizeof(fileName), "mc2-frame-%lu.ppm", m_frameNumber);
        FILE* image = std::fopen(fileName, "wb");
        if (!image)
        {
            std::fprintf(stderr, "[webgpu] could not open %s for frame dump\n", fileName);
            readback.Unmap();
            return;
        }

        std::fprintf(image, "P6\n%u %u\n255\n", m_targetWidth, m_targetHeight);
        std::vector<unsigned char> rgb(static_cast<size_t>(m_targetWidth) * 3);
        for (uint32_t y = 0; y < m_targetHeight; ++y)
        {
            const unsigned char* source = pixels + static_cast<size_t>(y) * paddedBytesPerRow;
            for (uint32_t x = 0; x < m_targetWidth; ++x)
            {
                rgb[x * 3 + 0] = source[x * 4 + 0];
                rgb[x * 3 + 1] = source[x * 4 + 1];
                rgb[x * 3 + 2] = source[x * 4 + 2];
            }
            std::fwrite(rgb.data(), 1, rgb.size(), image);
        }
        std::fclose(image);

        const auto printPixel = [&](uint32_t x, uint32_t y)
        {
            const unsigned char* p = pixels + static_cast<size_t>(y) * paddedBytesPerRow + x * 4;
            std::printf("[webgpu] readback pixel (%u,%u)=RGBA(%u,%u,%u,%u)\n",
                        x, y, p[0], p[1], p[2], p[3]);
        };
        printPixel(10, 10);
        printPixel(325, 275);
        printPixel(475, 275);
        printPixel(325, 325);
        printPixel(475, 325);

        std::error_code pathError;
        const std::filesystem::path absolute = std::filesystem::absolute(fileName, pathError);
        std::printf("[webgpu] dumped frame %lu to %s (%ux%u, bytesPerRow=%u)\n",
                    m_frameNumber, pathError ? fileName : absolute.string().c_str(),
                    m_targetWidth, m_targetHeight, paddedBytesPerRow);
        readback.Unmap();
    }

} // namespace mc2webgpu
