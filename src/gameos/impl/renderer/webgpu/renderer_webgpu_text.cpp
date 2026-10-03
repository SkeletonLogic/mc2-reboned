#include "mem/page_guard.h"
#include "renderer_webgpu_internal.h"

#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
// PORT: GetAsyncKeyState for the Ctrl+Alt+S screenshot chord. Nothing else in
// this translation unit needs the Win32 headers, so keep them narrow.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace mc2webgpu {


    //-- text --------------------------------------------------------------//
    HGOSFONT3D WebGPURenderer::LoadFont(const char* fontFile, DWORD startLine, int charCount,
                        DWORD textureHandle)
    {
#if defined(__EMSCRIPTEN__)
        // See m_fontCache in the internal header: identical requests must not pay
        // the decode + mask + grid-scan + texture upload again.
        const std::string cacheKey = std::string(fontFile ? fontFile : "") + '|' +
            std::to_string((unsigned long)startLine) + '|' + std::to_string(charCount) + '|' +
            std::to_string((unsigned long)textureHandle);
        {
            std::map<std::string, FontCacheEntry>::iterator hit = m_fontCache.find(cacheKey);
            if (hit != m_fontCache.end())
            {
                ++hit->second.refs;
                return reinterpret_cast<HGOSFONT3D>(hit->second.font);
            }
        }
#endif
        mc2gfx::FontData* font = new mc2gfx::FontData();
        font->name = fontFile ? fontFile : "";
        mc2gfx::TgaImage image;
        std::string error;

        if (textureHandle)
        {
            std::map<DWORD, Texture*>::iterator found = m_textures.find(textureHandle);
            if (found != m_textures.end())
            {
                image.width = found->second->width;
                image.height = found->second->height;
                image.sourceHasAlpha = found->second->format != gos_Texture_Solid;
                image.rgba = found->second->pixels;
                mc2gfx::MakeFontMask(image);
                font->textureHandle = textureHandle;
            }
            else error = "TextureHandle is unknown";
        }
        else
        {
            BYTE* bytes = nullptr;
            DWORD byteCount = 0;
            gos_GetFile(fontFile ? fontFile : "", &bytes, &byteCount);
            const bool d3f = mc2gfx::DecodeD3f4Font(bytes, byteCount, image, *font, error);
            if (d3f || mc2gfx::DecodeTga(bytes, byteCount, image, error))
            {
                if (!d3f) mc2gfx::MakeFontMask(image);
                font->textureHandle = Create(fontFile, gos_Texture_Alpha,
                                             image.width, image.height, &image.rgba, true);
                font->ownsTexture = true;
            }
            if (bytes) gos_Free(bytes);
        }

        if (!image.rgba.empty() && (font->parsed ||
            mc2gfx::ParseGridFont(image, startLine, charCount, *font, error)))
        {
            ++m_stats.fontsLoaded;
            std::printf("[webgpu] font[%lu] '%s': texture=%lu grid=%dx%d across=%d chars=%d\n",
                        m_stats.fontsLoaded, font->name.c_str(),
                        (unsigned long)font->textureHandle, font->gridWidth,
                        font->gridHeight, font->across, font->charCount);
        }
        else
        {
            ++m_stats.fontLoadFailed;
            if (m_stats.fontLoadFailed <= 8)
                std::fprintf(stderr, "[webgpu] font fallback for '%s': %s\n",
                             font->name.c_str(), error.empty() ? "no font data" : error.c_str());
        }
        m_fonts.insert(font);
#if defined(__EMSCRIPTEN__)
        FontCacheEntry entry; entry.font = font; entry.refs = 1;
        m_fontCache[cacheKey] = entry;
        m_fontCacheKey[font] = cacheKey;
#endif
        return reinterpret_cast<HGOSFONT3D>(font); // Always non-null for game safety.
    }



    void WebGPURenderer::DeleteFont(HGOSFONT3D handle)
    {
        mc2gfx::FontData* font = reinterpret_cast<mc2gfx::FontData*>(handle);
        if (!font || m_fonts.count(font) == 0) return;
#if defined(__EMSCRIPTEN__)
        // A cached font is handed to several callers; only the last release frees
        // it, or the next screen would draw through a destroyed atlas.
        {
            std::map<const mc2gfx::FontData*, std::string>::iterator keyed = m_fontCacheKey.find(font);
            if (keyed != m_fontCacheKey.end())
            {
                std::map<std::string, FontCacheEntry>::iterator entry = m_fontCache.find(keyed->second);
                if (entry != m_fontCache.end() && entry->second.refs > 1) { --entry->second.refs; return; }
                if (entry != m_fontCache.end()) m_fontCache.erase(entry);
                m_fontCacheKey.erase(keyed);
            }
        }
#endif
        m_fonts.erase(font);
        if (m_text.font == font) m_text.font = nullptr;
        if (font == m_proofFont) m_proofFont = nullptr;
        if (font->ownsTexture) DestroyTexture(font->textureHandle);
        delete font;
    }



    void WebGPURenderer::TextSetAttributes(HGOSFONT3D handle, DWORD foreground, float size,
                           bool wordWrap, bool proportional, bool bold, bool italic,
                           DWORD wrapType, bool disableEmbeddedCodes)
    {
        mc2gfx::FontData* font = reinterpret_cast<mc2gfx::FontData*>(handle);
        m_text.font = m_fonts.count(font) ? font : nullptr;
        m_text.foreground = foreground;
        m_text.size = size;
        m_text.wordWrap = wordWrap;
        m_text.proportional = proportional;
        m_text.bold = bold;
        m_text.italic = italic;
        m_text.wrapType = wrapType;
        m_text.disableEmbeddedCodes = disableEmbeddedCodes;
    }



    void WebGPURenderer::TextSetPosition(int x, int y)  { m_text.x = x; m_text.y = y; }



    void WebGPURenderer::TextSetRegion(int l, int t, int r, int b)
    { m_text.left = l; m_text.top = t; m_text.right = r; m_text.bottom = b; }



    void WebGPURenderer::TextDrawBackground(int left, int top, int right, int bottom, DWORD color)
    {
        ++m_stats.textBackgrounds;
        left = std::max(left, m_text.left); top = std::max(top, m_text.top);
        right = std::min(right, m_text.right); bottom = std::min(bottom, m_text.bottom);
        if (right <= left || bottom <= top) return;
        gos_VERTEX quad[4] = {};
        SetQuad(quad, static_cast<float>(left), static_cast<float>(top),
                static_cast<float>(right), static_cast<float>(bottom),
                0.0f, 0.0f, 1.0f, 1.0f, color);
        if (SubmitTextQuads(quad, 4, 0, false)) ++m_stats.textBackgroundsSubmitted;
    }



    void WebGPURenderer::TextStringLength(DWORD* width, DWORD* height, const char* text)
    { m_text.StringLength(width, height, text); }



    void WebGPURenderer::TextDraw(const char* text)
    {
        ++m_stats.textDraws;
        // Row 7 census: does any drawn string carry an embedded slash code?
        // Counting only, silent unless MC2_TEXT_CENSUS=1.
        static const bool textCensus = [] {
            const char* e = std::getenv("MC2_TEXT_CENSUS");
            return e && *e && std::strcmp(e, "0") != 0;
        }();
        if (textCensus && text && std::strchr(text, '\\'))
        {
            if (++m_stats.slashDraws <= 16)
                std::printf("[text-census] slash draw: \"%.120s\"\n", text);
        }
        if (textCensus && text && (m_text.bold || m_text.italic))
        {
            static unsigned long styledLogged = 0;
            if (++styledLogged <= 16)
                std::printf("[text-census] styled draw bold=%d italic=%d font='%s': \"%.120s\"\n",
                            m_text.bold ? 1 : 0, m_text.italic ? 1 : 0,
                            m_text.font ? m_text.font->name.c_str() : "", text);
        }
        const mc2gfx::TextLayout layout = m_text.Layout(text, true);
        if (!m_text.font || !m_text.font->parsed || !m_text.font->textureHandle ||
            layout.glyphs.empty())
            return;

        // M8-B85, owner ruling 2026-10-01 ("Bold looks good"): synthesize
        // bold by striking each glyph a second time one pixel to the right.
        // How the original GameOS drew bold is not on record (the
        // reconstructions only declare the flag), so this is a choice, not a
        // recovery. MC2_TEXT_BOLD=0 turns it off for A/B comparison.
        static const bool synthBold = [] {
            const char* e = std::getenv("MC2_TEXT_BOLD");
            return !(e && std::strcmp(e, "0") == 0);
        }();
        const int strikes = (synthBold && m_text.bold) ? 2 : 1;

        std::vector<gos_VERTEX> vertices;
        vertices.reserve(layout.glyphs.size() * 4 * strikes);
        for (int strike = 0; strike < strikes; ++strike)
        for (const mc2gfx::TextGlyphPlacement& placed : layout.glyphs)
        {
            float x0 = placed.x + strike, y0 = placed.y;
            float x1 = placed.x + placed.width + strike, y1 = placed.y + placed.height;
            float u0 = static_cast<float>(placed.glyph->x) / m_text.font->textureWidth;
            float v0 = static_cast<float>(placed.glyph->y) / m_text.font->textureHeight;
            float u1 = static_cast<float>(placed.glyph->x + placed.glyph->width) /
                       m_text.font->textureWidth;
            float v1 = static_cast<float>(placed.glyph->y + placed.glyph->height) /
                       m_text.font->textureHeight;
            if (!ClipTextQuad(x0, y0, x1, y1, u0, v0, u1, v1)) continue;
            const size_t first = vertices.size();
            vertices.resize(first + 4);
            SetQuad(vertices.data() + first, x0, y0, x1, y1, u0, v0, u1, v1,
                    m_text.foreground);
        }
        if (!vertices.empty() &&
            SubmitTextQuads(vertices.data(), static_cast<DWORD>(vertices.size()),
                            m_text.font->textureHandle, true))
        {
            const unsigned long glyphs =
                static_cast<unsigned long>(vertices.size() / 4 / strikes);
            m_stats.glyphsDrawn += glyphs;
            if (m_text.bold && strikes == 1) m_stats.boldIgnored += glyphs;
            if (m_text.italic) m_stats.italicIgnored += glyphs;
        }
    }



    bool WebGPURenderer::ClipTextQuad(float& x0, float& y0, float& x1, float& y1,
                      float& u0, float& v0, float& u1, float& v1) const
    {
        if (x1 <= m_text.left || x0 >= m_text.right ||
            y1 <= m_text.top || y0 >= m_text.bottom)
            return false;
        const float originalX0 = x0, originalY0 = y0;
        const float originalX1 = x1, originalY1 = y1;
        const float originalU0 = u0, originalV0 = v0;
        const float originalU1 = u1, originalV1 = v1;
        x0 = std::max(x0, static_cast<float>(m_text.left));
        y0 = std::max(y0, static_cast<float>(m_text.top));
        x1 = std::min(x1, static_cast<float>(m_text.right));
        y1 = std::min(y1, static_cast<float>(m_text.bottom));
        u0 = originalU0 + (originalU1 - originalU0) *
             (x0 - originalX0) / (originalX1 - originalX0);
        u1 = originalU0 + (originalU1 - originalU0) *
             (x1 - originalX0) / (originalX1 - originalX0);
        v0 = originalV0 + (originalV1 - originalV0) *
             (y0 - originalY0) / (originalY1 - originalY0);
        v1 = originalV0 + (originalV1 - originalV0) *
             (y1 - originalY0) / (originalY1 - originalY0);
        return x1 > x0 && y1 > y0;
    }



    bool WebGPURenderer::SubmitTextQuads(const gos_VERTEX* vertices, DWORD count, DWORD textureHandle,
                         bool alphaTest)
    {
        if (!m_frameOpen || !vertices || count < 4 || count % 4 != 0) return false;
        const DWORD quadCount = count / 4;
        const PipelineKey pipelineKey = { wgpu::PrimitiveTopology::TriangleList,
            gos_Alpha_AlphaInvAlpha, 0, 0, alphaTest ? 1 : 0,
            gos_BlendModulateAlpha, false };
        SamplerKey samplerKey = { gos_FilterNone, gos_TextureClamp };
        const wgpu::RenderPipeline pipeline = PipelineForKey(pipelineKey);
        SamplerBinding* sampler = SamplerForKey(samplerKey);
        const wgpu::BindGroup texture = TextureBindGroupForHandle(textureHandle);
        const wgpu::BindGroup fog = FogBindGroupForCurrentState();
        if (!EnsureQuadIndices(quadCount) || !pipeline || !sampler ||
            !sampler->bindGroup || !texture || !fog)
            return false;
        const VertexSlice slice = AppendVertices(vertices, count);
        if (!slice.buffer) return false;
        m_pass.SetPipeline(pipeline);
        m_pass.SetBindGroup(0, ViewportBindGroupForCurrentCanvas());
        m_pass.SetBindGroup(1, sampler->bindGroup);
        m_pass.SetBindGroup(2, texture);
        m_pass.SetBindGroup(3, fog);
        m_pass.SetVertexBuffer(0, slice.buffer, slice.offset, count * sizeof(gos_VERTEX));
        m_pass.SetIndexBuffer(m_quadIndexBuffer, wgpu::IndexFormat::Uint16, 0,
                              quadCount * 6 * sizeof(WORD));
        m_pass.DrawIndexed(quadCount * 6);
        m_maxQuadBatch = std::max(m_maxQuadBatch, quadCount);
        return true;
    }



    void WebGPURenderer::DrawTextProof()
    {
        if (m_textProof.empty()) return;
        if (!m_proofFont)
            m_proofFont = reinterpret_cast<mc2gfx::FontData*>(
                LoadFont("assets\\graphics\\arial8.tga", 0, 256, 0));
        if (!m_proofFont || !m_proofFont->parsed) return;

        const mc2gfx::TextMetrics saved = m_text;
        TextSetAttributes(reinterpret_cast<HGOSFONT3D>(m_proofFont), 0xFFFFFFFFu,
                          2.0f, false, true, false, false, 0, true);
        TextSetRegion(32, 32, static_cast<int>(m_targetWidth) - 32, 100);
        TextSetPosition(40, 40);
        DWORD width = 0, height = 0;
        TextStringLength(&width, &height, m_textProof.c_str());
        const mc2gfx::TextLayout layout = m_text.Layout(m_textProof.c_str(), true);
        float inkLeft = 0, inkRight = 0;
        if (!layout.glyphs.empty())
        {
            inkLeft = layout.glyphs.front().x;
            inkRight = layout.glyphs.front().x + layout.glyphs.front().width;
            for (const mc2gfx::TextGlyphPlacement& glyph : layout.glyphs)
            {
                inkLeft = std::min(inkLeft, glyph.x);
                inkRight = std::max(inkRight, glyph.x + glyph.width);
            }
        }
        TextDrawBackground(32, 32, 48 + static_cast<int>(width),
                           48 + static_cast<int>(height), 0xE0202020u);
        TextDraw(m_textProof.c_str());
        if (!m_textProofLogged)
        {
            std::printf("[webgpu] text proof: '%s' position=40,40 metrics=%lux%lu "
                        "predictedInkX=%.1f..%.1f\n", m_textProof.c_str(),
                        (unsigned long)width, (unsigned long)height, inkLeft, inkRight);
            m_textProofLogged = true;
        }
        m_text = saved;
    }

    //-- TEST BRANCH HUD and screenshot hotkey -----------------------------//
    //
    // PORT (2026-08-13): a screenshot pasted into a chat window is not a file.
    // It cannot be re-read, diffed, or measured pixel by pixel, which is
    // exactly what the shop-UI line defects need. Ctrl+Alt+S writes the frame
    // the owner is looking at straight out of the colour target -- the real
    // framebuffer, not a rescaled desktop grab -- so line positions can be
    // counted. Both this and the HUD are off unless MC2_HUD_OVERLAY is set.

    void WebGPURenderer::PollCaptureHotkey()
    {
        if (!m_hudOverlay) return;
#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
        const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
        const bool sKey = (GetAsyncKeyState('S') & 0x8000) != 0;
        const std::filesystem::path requestPath =
            std::filesystem::path(m_shotDir) / "capture.request";
        std::error_code requestError;
        const bool fileRequested = std::filesystem::exists(requestPath, requestError);
        // PORT: F12 is the single-key fallback for unattended test control.
        // Synthesised multi-key chords can be shorter than one render frame,
        // The request file is deterministic when even a single injected key is
        // shorter than one render frame.
        const bool chordDown = (ctrl && alt && sKey) ||
                               ((GetAsyncKeyState(VK_F12) & 0x8000) != 0) ||
                               fileRequested;

        // Edge-triggered: a held chord must not fire a shot every frame.
        if (chordDown && !m_captureKeyWasDown)
        {
            std::error_code dirError;
            std::filesystem::create_directories(m_shotDir, dirError);
            if (fileRequested) std::filesystem::remove(requestPath, requestError);

            const std::time_t now = std::time(nullptr);
            std::tm local = {};
            localtime_s(&local, &now);
            char stamp[64];
            std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);

            char name[512];
            std::snprintf(name, sizeof(name), "%s/shot-%s-%lu.ppm",
                          m_shotDir.c_str(), stamp, ++m_captureCount);
            m_capturePath = name;
            m_captureRequested = true;
        }
        m_captureKeyWasDown = chordDown;
#endif
    }

    void WebGPURenderer::DrawHudOverlay()
    {
        if (!m_hudOverlay) return;

        // FPS over the last quarter second, measured here rather than read from
        // the game's `frameRate`: this counts frames the renderer actually
        // presented, which is the number a screenshot should be labelled with.
        const unsigned long long micros =
            (unsigned long long)std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        ++m_hudFramesSince;
        if (m_hudLastMicros == 0) m_hudLastMicros = micros;
        else if (micros - m_hudLastMicros >= 250000ull)
        {
            m_hudFps = (float)(m_hudFramesSince * 1000000.0 /
                               (double)(micros - m_hudLastMicros));
            m_hudFramesSince = 0;
            m_hudLastMicros = micros;
        }

        if (!m_hudFont)
            m_hudFont = reinterpret_cast<mc2gfx::FontData*>(
                LoadFont("assets\\graphics\\arial8.tga", 0, 256, 0));
        if (!m_hudFont || !m_hudFont->parsed) return;

        // PORT: B1-1. The page guard's state belongs ON SCREEN. Checking whether
        // the instrument is live should not mean grepping a log mid-playtest.
        char guard[64];
        mc2mem::PageGuardStatusLine(guard, sizeof(guard));

        char line[192];
        std::snprintf(line, sizeof(line),
                      "Screenshot Ctrl+Alt+S / F12 | %3.0f FPS | %s",
                      m_hudFps, guard);

        const mc2gfx::TextMetrics saved = m_text;
        TextSetAttributes(reinterpret_cast<HGOSFONT3D>(m_hudFont), 0xFFFFFFFFu,
                          1.0f, false, true, false, false, 0, true);
        TextSetRegion(0, 0, (int)m_targetWidth, (int)m_targetHeight);
        TextSetPosition(6, 3);
        DWORD width = 0, height = 0;
        TextStringLength(&width, &height, line);
        TextDrawBackground(2, 1, 10 + (int)width, 5 + (int)height, 0xA0000000u);
        TextDraw(line);
        m_text = saved;
    }

} // namespace mc2webgpu
