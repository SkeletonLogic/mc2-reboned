#include "renderer_webgpu_internal.h"

#include <cstring>
#include <cstdlib>

namespace mc2webgpu {

// PORT: BGRA8Unorm, matching the byte order the decoders now produce and the
// order MCLib paints in. The hardware presents it to the shader correctly
// ordered, so nothing in the shaders changes.
wgpu::TextureFormat wgpuFormatFor(gos_TextureFormat fmt)
{
    switch (fmt)
    {
        case gos_Texture_Solid:
        case gos_Texture_Keyed:
        case gos_Texture_Alpha:
        case gos_Texture_Detect:
            return wgpu::TextureFormat::BGRA8Unorm;
        case gos_Texture_Bump:
        case gos_Texture_Normal:
            return wgpu::TextureFormat::BGRA8Unorm;
        default:
            return wgpu::TextureFormat::BGRA8Unorm;
    }
}


    //-- textures ----------------------------------------------------------//
    DWORD WebGPURenderer::NewEmptyTexture(gos_TextureFormat fmt, const char* name, DWORD hw)
    { return Create(name, fmt, hw ? hw : 256, hw ? hw : 256, nullptr, false); }



    DWORD WebGPURenderer::NewTextureFromFile(gos_TextureFormat fmt, const char* fileName,
                             DWORD hints)
    {
        BYTE* bytes = nullptr;
        DWORD size = 0;
        gos_GetFile(fileName, &bytes, &size);
        const DWORD handle = DecodeAndCreate(fileName, fmt, bytes, size, hints);
        if (bytes && !(hints & gosHint_ROMFile)) gos_Free(bytes);
        return handle;
    }



    DWORD WebGPURenderer::NewTextureFromMemory(gos_TextureFormat fmt, const char* name, BYTE* bytes,
                               DWORD size, DWORD hints)
    { return DecodeAndCreate(name, fmt, bytes, size, hints); }



    void WebGPURenderer::DestroyTexture(DWORD handle)
    {
        std::map<DWORD, Texture*>::iterator it = m_textures.find(handle);
        if (it == m_textures.end()) return;
        if (it->second->gpu) it->second->gpu.Destroy();
        delete it->second;
        m_textures.erase(it);
    }



    // PORT: texture-guard diagnostic, MC2_TEXTURE_GUARD=1.
    //
    // MechIcon's merged loop counter ran pDestData off the end of a locked
    // texture and corrupted the heap. That defect is fixed, but heap corruption
    // through __fastfail bypasses the exception filter completely -- no handler,
    // no stack, no report -- so if another write like it exists there is nothing
    // to catch it.
    //
    // With the guard on, every staging buffer gets a padded tail filled with a
    // known pattern. UnLockTexture checks the pattern and names the texture and
    // the overrun distance. That turns an invisible heap kill into a line of
    // text pointing at the culprit. Off by default: the padding changes
    // allocation sizes, and the strict regression measures those.
    static bool textureGuardOn (void)
    {
        static int on = -1;
        if (on < 0)
        {
            const char* want = std::getenv("MC2_TEXTURE_GUARD");
            on = (want && *want && std::strcmp(want, "0")) ? 1 : 0;
        }
        return on != 0;
    }

    static const size_t kGuardBytes = 256;
    static const unsigned char kGuardPattern = 0xA5;

    static void armGuard (std::vector<unsigned char>& pixels, size_t usedBytes)
    {
        if (!textureGuardOn()) return;
        if (pixels.size() < usedBytes + kGuardBytes) pixels.resize(usedBytes + kGuardBytes);
        std::memset(&pixels[usedBytes], kGuardPattern, kGuardBytes);
    }

    static void checkGuard (const std::vector<unsigned char>& pixels, size_t usedBytes,
                            const char* name)
    {
        if (!textureGuardOn()) return;
        if (pixels.size() < usedBytes + kGuardBytes) return;
        size_t clobbered = 0;
        for (size_t i = 0; i < kGuardBytes; ++i)
            if (pixels[usedBytes + i] != kGuardPattern) clobbered = i + 1;
        if (clobbered)
            std::fprintf(stderr,
                         "[webgpu] *** TEXTURE OVERRUN *** '%s' wrote %llu byte(s) past the end "
                         "of its %llu-byte staging buffer\n",
                         name ? name : "(unnamed)",
                         (unsigned long long)clobbered, (unsigned long long)usedBytes);
    }

    void WebGPURenderer::LockTexture(DWORD handle, DWORD mipMapSize, bool readOnly, TEXTUREPTR* info)
    {
        if (!info) return;
        std::memset(info, 0, sizeof(*info));

        std::map<DWORD, Texture*>::iterator it = m_textures.find(handle);
        if (it == m_textures.end())
        {
            std::fprintf(stderr, "[webgpu] LockTexture on unknown handle %lu\n",
                         (unsigned long)handle);
            return;
        }

        Texture* t = it->second;
        t->locked = true;
        if (!readOnly) t->dirty = true;

        // Always hand back the BASE level buffer and advertise the base size, even
        // when a smaller mip is requested. The buffer is then never smaller than
        // the caller's arithmetic expects, which is the property that keeps MCLib
        // from writing past the end. Mip-level locks are counted so the next
        // increment knows whether real mip storage is actually needed.
        if (mipMapSize != 0 && mipMapSize != t->width) ++m_stats.mipLocks;

        armGuard(t->pixels,
                 (size_t)t->width * t->height * kStagingBytesPerPixel);
        info->pTexture = (DWORD*)&t->pixels[0];
        info->Width    = t->width;
        info->Height   = t->height;
        info->Pitch    = t->width; // TEXTUREPTR documents DWORDs, not bytes.
        info->Type     = t->format;
        ++m_stats.locks;
    }



    void WebGPURenderer::UnLockTexture(DWORD handle)
    {
        std::map<DWORD, Texture*>::iterator it = m_textures.find(handle);
        if (it == m_textures.end()) return;

        Texture* t = it->second;
        t->locked = false;
        checkGuard(t->pixels, (size_t)t->width * t->height * kStagingBytesPerPixel,
                   t->name.c_str());
        if (!t->dirty) return;
        t->dirty = false;

        Upload(*t);
    }



    DWORD WebGPURenderer::DecodeAndCreate(const char* name, gos_TextureFormat requested,
                          const BYTE* bytes, DWORD size, DWORD hints)
    {
        mc2gfx::TgaImage image;
        std::string error;
        if (!mc2gfx::DecodeTga(bytes, size, image, error,
                               requested == gos_Texture_Alpha))
        {
            ++m_stats.decodeFailed;
            std::fprintf(stderr, "[webgpu] TGA decode failed for '%s': %s\n",
                         name ? name : "", error.c_str());
            return Create(name, requested == gos_Texture_Detect ? gos_Texture_Alpha : requested,
                          256, 256, nullptr, false);
        }

        const gos_TextureFormat actual =
            mc2gfx::ApplyTgaFormatAndHints(image, requested, hints);
        ++m_stats.decoded;
        if (image.rle) ++m_stats.rleDecoded;
        if (image.sourceBits == 24) ++m_stats.source24;
        if (image.sourceBits == 32) ++m_stats.source32;
        if (image.width != image.height) ++m_stats.nonSquareDecoded;
        if (hints & gosHint_PinkColorKey) ++m_stats.pinkKeyed;
        if (hints & gosHint_ForceAlpha) ++m_stats.forceAlpha;
        m_decodedDimensions.insert(std::make_pair(image.width, image.height));

        if (m_stats.decoded <= 8)
            std::printf("[webgpu] decoded texture '%s': %ux%u %u-bit %s origin=%s%s type=%d hints=0x%lx\n",
                        name ? name : "", image.width, image.height, image.sourceBits,
                        image.rle ? "RLE" : "raw", image.topOrigin ? "top" : "bottom",
                        image.rightOrigin ? "-right" : "-left", (int)actual,
                        (unsigned long)hints);

        return Create(name, actual, image.width, image.height, &image.rgba, true);
    }



    DWORD WebGPURenderer::Create(const char* name, gos_TextureFormat format, DWORD width, DWORD height,
                 const std::vector<unsigned char>* pixels, bool uploadNow)
    {
        Texture* t = new Texture();
        t->name        = name ? name : "";
        t->format      = format;
        t->width       = width ? width : 256;
        t->height      = height ? height : 256;
        t->locked      = false;
        t->dirty       = false;

        if (pixels) t->pixels = *pixels;
        else t->pixels.assign(static_cast<size_t>(t->width) * t->height *
                              kStagingBytesPerPixel, 0);
        ++m_stats.created;

        // The GPU texture is optional by design: if creation fails the staging
        // buffer still satisfies every gos_LockTexture contract, so the game keeps
        // running and the failure is a reported number rather than a crash.
        wgpu::TextureDescriptor td = {};
        td.label         = t->name.empty() ? "gos texture" : t->name.c_str();
        td.usage         = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
        td.dimension     = wgpu::TextureDimension::e2D;
        td.size          = { t->width, t->height, 1 };
        td.format        = wgpuFormatFor(format);
        td.mipLevelCount = 1;
        td.sampleCount   = 1;

        t->gpu = m_device.CreateTexture(&td);
        if (t->gpu) ++m_stats.gpuCreated; else ++m_stats.gpuFailed;
        if (uploadNow) Upload(*t);

        const DWORD handle = m_nextTexture++;
        m_textures[handle] = t;
        return handle;
    }



    // PORT (M8-B19): rebuild every GPU texture after a device loss.
    //
    // This has to be EAGER. The first version of the recovery nulled Texture::gpu
    // and trusted "the lazy upload path" to rebuild it -- but there is no such
    // path. `gpu` is only ever created in Create(), and Upload() early-returns on
    // a null `gpu`; the `dirty` flag is consumed by UnlockTexture alone. The
    // result was a frame with correct geometry, shadows and HUD layout and every
    // texture WHITE, which is what a null texture binds to. Caught by gating the
    // recovered frame against the reference rather than by trusting a "recovery
    // succeeded" log line.
    void WebGPURenderer::RecreateAllTextures()
    {
        size_t rebuilt = 0, failed = 0, skipped = 0;
        for (std::map<DWORD, Texture*>::iterator it = m_textures.begin();
             it != m_textures.end(); ++it)
        {
            Texture* t = it->second;
            if (!t) continue;
            if (t->pixels.empty()) { ++skipped; continue; }

            wgpu::TextureDescriptor td = {};
            td.label         = t->name.empty() ? "gos texture" : t->name.c_str();
            td.usage         = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
            td.dimension     = wgpu::TextureDimension::e2D;
            td.size          = { t->width, t->height, 1 };
            td.format        = wgpuFormatFor(t->format);
            td.mipLevelCount = 1;
            td.sampleCount   = 1;

            t->gpu = m_device.CreateTexture(&td);
            if (!t->gpu) { ++failed; continue; }
            Upload(*t);
            t->dirty = false;
            ++rebuilt;
        }
        std::fprintf(stderr,
                     "[webgpu] device-loss: rebuilt %zu textures, %zu had no CPU "
                     "pixels, %zu failed to create\n", rebuilt, skipped, failed);
        std::fflush(stderr);
    }

    void WebGPURenderer::Upload(Texture& t)
    {
        if (!t.gpu || t.pixels.empty()) return;

        // PORT: gos_Texture_Solid historically used an XRGB hardware format,
        // so alpha bytes written through gos_LockTexture were ignored. BGRA8
        // preserves them; force the documented solid contract before upload.
        // Mission briefing maps rely on this because their TGA packet stores
        // valid terrain RGB under zero alpha.
        if (t.format == gos_Texture_Solid) mc2gfx::ForceOpaqueAlpha(t.pixels);
        // PORT: Empty keyed textures are filled through gos_LockTexture, so they
        // bypass TGA decoding. Apply the keyed matte contract on final upload.
        else if (t.format == gos_Texture_Keyed)
        {
            // PORT: Only unnamed gos_LockTexture products are generated terrain
            // composites; named assets retain their authored keyed colours.
            mc2gfx::ApplyKeyedAlpha(t.pixels, t.name.empty());
        }

        wgpu::TexelCopyTextureInfo dst = {};
        dst.texture  = t.gpu;
        dst.mipLevel = 0;
        dst.origin   = { 0, 0, 0 };
        dst.aspect   = wgpu::TextureAspect::All;

        // queue.WriteTexture stages internally, so bytesPerRow has none of the
        // 256-byte alignment requirement that copyBufferToTexture imposes.
        wgpu::TexelCopyBufferLayout layout = {};
        layout.offset       = 0;
        layout.bytesPerRow  = t.width * kStagingBytesPerPixel;
        layout.rowsPerImage = t.height;

        wgpu::Extent3D size = { t.width, t.height, 1 };

        m_queue.WriteTexture(&dst, t.pixels.data(), t.pixels.size(), &layout, &size);
        ++m_stats.uploads;
        m_stats.uploadedBytes += t.pixels.size();
        if (m_frameOpen) m_currentFrameUploadBytes += t.pixels.size();
    }

} // namespace mc2webgpu
