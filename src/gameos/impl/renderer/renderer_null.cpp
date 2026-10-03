//===========================================================================//
// NullRenderer -- accepts and MEASURES every call, draws nothing.
//
// Not a stub in the throwaway sense. Two jobs:
//
//  1. Let the game run without a graphics device, which is what Milestone 1 is
//     defined as and what keeps headless bring-up possible afterwards.
//
//  2. MEASURE the fixed-function state space the game actually uses. Phase 0
//     called mapping D3D8-era render state onto WebGPU the single largest work
//     item in the port and sized it by "691 gos_SetRenderState call sites".
//     That is the wrong number: what an implementation must reproduce is the
//     set of DISTINCT states and values, which at boot is 15. This class is how
//     that number is obtained, and it stays useful after WebGPU exists as a
//     comparison layer.
//
// TEXTURES GET REAL MEMORY. MCLib writes pixels through gos_LockTexture and
// reads them back -- TerrTxm.cpp combines terrain overlays in RAM then
// LZ-compresses the result. Returning null would crash during DATA LOAD, long
// before a pixel is drawn.
//===========================================================================//

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <vector>
#include <string>

#include <gameos.hpp>

#include "renderer_backend.h"
#include "renderer_state.h"
#include "renderer_tga.h"

namespace {

struct Texture
{
    std::string                name;
    gos_TextureFormat          format;
    DWORD                      width;
    DWORD                      height;
    std::vector<unsigned char> pixels;
    bool                       locked;
};

// 32-bit is the safe assumption: never smaller than a caller expects, so code
// writing 16-bit pixels cannot run off the end.
DWORD bytesPerPixelFor(DWORD /*format*/) { return 4; }

// M8-B12, 2026-08-20. The owner reports roads rendering as a flat untextured
// band. MC2 does not draw those roads from an authored tile: the terrain
// combiner COMPOSES them, writing pixels through gos_LockTexture into an
// unnamed keyed texture. That is the same family M7-B14 already had to repair
// once (9fe6287, hot-pink matte and its antialias fringe left opaque).
//
// So the first question is not about the renderer at all: IS THE COMPOSITE
// FLAT WHEN THE GAME FINISHES WRITING IT? This dumps it at UnLockTexture,
// under the NULL renderer, where nothing has yet applied the keyed-alpha pass
// that runs on the WebGPU upload path. That separates "generated wrong" from
// "generated right and then flattened or mis-sampled downstream" -- two
// different faults that look identical on screen.
//
// Costs the owner nothing: it runs headless.
//
// MC2_TEXTURE_DUMP=<dir> writes each unnamed keyed texture as a 32-bit TGA
// and prints one stats line per texture. MC2_TEXTURE_DUMP_MAX caps the count
// (default 40) so a mission does not fill the disk.
const char* textureDumpDir()
{
    static const char* dir = std::getenv("MC2_TEXTURE_DUMP");
    return (dir && *dir) ? dir : nullptr;
}

int textureDumpMax()
{
    static const int cap = []() -> int {
        const char* env = std::getenv("MC2_TEXTURE_DUMP_MAX");
        const int value = (env && *env) ? std::atoi(env) : 40;
        return value > 0 ? value : 40;
    }();
    return cap;
}

void dumpGeneratedTexture(DWORD handle, const Texture& t)
{
    const char* dir = textureDumpDir();
    if (!dir) return;
    // Only the generated composites. A named texture came from an authored
    // asset and is not what this is looking for.
    if (!t.name.empty()) return;
    if (t.pixels.empty() || t.width == 0 || t.height == 0) return;

    static int written = 0;
    if (written >= textureDumpMax()) return;

    // Stats first, because they answer the question on their own: a composite
    // that is genuinely flat has one colour, and no image viewer is needed to
    // say so.
    unsigned char minC[4] = { 255, 255, 255, 255 };
    unsigned char maxC[4] = { 0, 0, 0, 0 };
    std::map<unsigned long, int> colours;
    for (std::size_t i = 0; i + 3 < t.pixels.size(); i += 4)
    {
        for (int c = 0; c < 4; ++c)
        {
            const unsigned char v = t.pixels[i + c];
            if (v < minC[c]) minC[c] = v;
            if (v > maxC[c]) maxC[c] = v;
        }
        if (colours.size() < 4096)
        {
            unsigned long key = ((unsigned long)t.pixels[i + 0] << 24) |
                                ((unsigned long)t.pixels[i + 1] << 16) |
                                ((unsigned long)t.pixels[i + 2] << 8) |
                                 (unsigned long)t.pixels[i + 3];
            colours[key] = 1;
        }
    }

    char path[1024];
    std::snprintf(path, sizeof(path), "%s/gen-%03d-h%lu-%lux%lu-fmt%d.tga",
                  dir, written, (unsigned long)handle,
                  (unsigned long)t.width, (unsigned long)t.height, (int)t.format);

    std::fprintf(stderr,
        "[tex-dump] %s colours=%s%d B %d-%d G %d-%d R %d-%d A %d-%d\n",
        path, colours.size() >= 4096 ? ">=" : "", (int)colours.size(),
        (int)minC[0], (int)maxC[0], (int)minC[1], (int)maxC[1],
        (int)minC[2], (int)maxC[2], (int)minC[3], (int)maxC[3]);

    // Uncompressed 32-bit TGA, bottom-up, so anything can open it.
    std::FILE* f = std::fopen(path, "wb");
    if (!f) { std::fflush(stderr); return; }
    unsigned char header[18] = {};
    header[2]  = 2;                                   // uncompressed true-colour
    header[12] = (unsigned char)(t.width & 0xff);
    header[13] = (unsigned char)((t.width >> 8) & 0xff);
    header[14] = (unsigned char)(t.height & 0xff);
    header[15] = (unsigned char)((t.height >> 8) & 0xff);
    header[16] = 32;
    header[17] = 8;                                   // 8 alpha bits
    std::fwrite(header, 1, sizeof(header), f);
    std::fwrite(&t.pixels[0], 1, t.pixels.size(), f);
    std::fclose(f);
    ++written;
    std::fflush(stderr);
}

class NullRenderer : public RendererBackend
{
public:
    NullRenderer()
        : m_state("null-renderer"), m_nextTexture(1), m_texCreated(0), m_texDestroyed(0), m_decoded(0), m_decodeFailed(0)
    {
        std::memset(&m_stats, 0, sizeof(m_stats));
    }

    ~NullRenderer() override
    {
        for (std::set<mc2gfx::FontData*>::iterator it = m_fonts.begin();
             it != m_fonts.end(); ++it)
            delete *it;
        for (std::map<DWORD, Texture*>::iterator it = m_textures.begin();
             it != m_textures.end(); ++it)
            delete it->second;
    }

    const char* Name() const override { return "null"; }

    void BeginFrame() override {}
    void EndFrame() override {}
    void SetWindow(void*, void*, int, int) override {}

    //-- render state ------------------------------------------------------//
    // The tracker is shared with every other backend (renderer_state.h) so that
    // a difference in the census between two backends is evidence about the
    // backends, not about two separate copies of the bookkeeping.
    void SetRenderState(gos_RenderState state, intptr_t value) override
    { m_state.Set(state, static_cast<int>(value)); }
    void PushRenderStates() override { m_state.Push(); }
    void PopRenderStates()  override { m_state.Pop(); }

    //-- drawing -----------------------------------------------------------//
    void DrawPoints(gos_VERTEX*, int n) override    { if (n > 0) ++m_stats.points; }
    void DrawLines(gos_VERTEX*, int n) override     { if (n > 0) ++m_stats.lines; }
    void DrawTriangles(gos_VERTEX*, int n) override { if (n > 0) ++m_stats.triangles; }
    void DrawQuads(gos_VERTEX*, int n) override     { if (n > 0) ++m_stats.quads; }

    void RenderIndexed(gos_VERTEX*, DWORD, WORD*, DWORD) override
    { ++m_stats.indexed; ++m_stats.indexed1UV; }
    void RenderIndexed2UV(gos_VERTEX_2UV*, DWORD, WORD*, DWORD) override
    { ++m_stats.indexed; ++m_stats.indexed2UV; }
    void RenderIndexed3UV(gos_VERTEX_3UV*, DWORD, WORD*, DWORD) override
    { ++m_stats.indexed; ++m_stats.indexed3UV; }
    void RenderIndexedRaw(void*, DWORD, WORD*, DWORD,
                          gosVERTEXTYPE, gosPRIMITIVETYPE) override
    { ++m_stats.indexed; ++m_stats.indexedRaw; }

    //-- textures ----------------------------------------------------------//
    DWORD NewEmptyTexture(gos_TextureFormat fmt, const char* name, DWORD hw) override
    { return Create(name, fmt, hw ? hw : 256, hw ? hw : 256, nullptr); }

    DWORD NewTextureFromFile(gos_TextureFormat fmt, const char* fileName,
                             DWORD hints) override
    {
        BYTE* bytes = nullptr;
        DWORD size = 0;
        gos_GetFile(fileName, &bytes, &size);
        const DWORD handle = DecodeAndCreate(fileName, fmt, bytes, size, hints);
        if (bytes && !(hints & gosHint_ROMFile)) gos_Free(bytes);
        return handle;
    }

    DWORD NewTextureFromMemory(gos_TextureFormat fmt, const char* name, BYTE* bytes,
                               DWORD size, DWORD hints) override
    { return DecodeAndCreate(name, fmt, bytes, size, hints); }

    void DestroyTexture(DWORD handle) override
    {
        std::map<DWORD, Texture*>::iterator it = m_textures.find(handle);
        if (it == m_textures.end()) return;
        delete it->second;
        ++m_texDestroyed;
        m_textures.erase(it);
    }

    void LockTexture(DWORD handle, DWORD, bool, TEXTUREPTR* info) override
    {
        if (!info) return;
        std::memset(info, 0, sizeof(*info));

        std::map<DWORD, Texture*>::iterator it = m_textures.find(handle);
        if (it == m_textures.end())
        {
            std::fprintf(stderr, "[null-renderer] LockTexture on unknown handle %lu\n",
                         (unsigned long)handle);
            return;
        }

        Texture* t = it->second;
        t->locked = true;
        info->pTexture = (DWORD*)&t->pixels[0];
        info->Width    = t->width;
        info->Height   = t->height;
        info->Pitch    = t->width; // TEXTUREPTR documents DWORDs, not bytes.
        info->Type     = t->format;
    }

    void UnLockTexture(DWORD handle) override
    {
        std::map<DWORD, Texture*>::iterator it = m_textures.find(handle);
        if (it != m_textures.end())
        {
            it->second->locked = false;
            // PORT: mirror the XRGB semantics enforced by the WebGPU peer.
            if (it->second->format == gos_Texture_Solid)
                mc2gfx::ForceOpaqueAlpha(it->second->pixels);
            // M8-B12: capture the generated composite as the game wrote it.
            dumpGeneratedTexture(handle, *it->second);
        }
    }

    //-- text --------------------------------------------------------------//
    HGOSFONT3D LoadFont(const char* fontFile, DWORD startLine, int charCount,
                        DWORD textureHandle) override
    {
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
                                             image.width, image.height, &image.rgba);
                font->ownsTexture = true;
            }
            if (bytes) gos_Free(bytes);
        }

        if (!image.rgba.empty() && (font->parsed ||
            mc2gfx::ParseGridFont(image, startLine, charCount, *font, error)))
            ++m_stats.fontsLoaded;
        else
        {
            ++m_stats.fontLoadFailed;
            if (m_stats.fontLoadFailed <= 8)
                std::fprintf(stderr, "[null-renderer] font fallback for '%s': %s\n",
                             font->name.c_str(), error.empty() ? "no font data" : error.c_str());
        }
        m_fonts.insert(font);
        return reinterpret_cast<HGOSFONT3D>(font); // Always non-null for game safety.
    }

    void DeleteFont(HGOSFONT3D handle) override
    {
        mc2gfx::FontData* font = reinterpret_cast<mc2gfx::FontData*>(handle);
        if (!font || m_fonts.erase(font) == 0) return;
        if (m_text.font == font) m_text.font = nullptr;
        if (font->ownsTexture) DestroyTexture(font->textureHandle);
        delete font;
    }

    void TextSetAttributes(HGOSFONT3D handle, DWORD foreground, float size,
                           bool wordWrap, bool proportional, bool bold, bool italic,
                           DWORD wrapType, bool disableEmbeddedCodes) override
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

    void TextSetPosition(int x, int y) override { m_text.x = x; m_text.y = y; }

    void TextSetRegion(int l, int t, int r, int b) override
    { m_text.left = l; m_text.top = t; m_text.right = r; m_text.bottom = b; }

    void TextDrawBackground(int, int, int, int, DWORD) override
    { ++m_stats.textBackgrounds; }

    void TextStringLength(DWORD* width, DWORD* height, const char* text) override
    {
        m_text.StringLength(width, height, text);
    }

    void TextDraw(const char* text) override
    {
        ++m_stats.textDraws;
        m_stats.glyphs += static_cast<unsigned long>(m_text.Layout(text, true).glyphs.size());
    }

    //-- diagnostics -------------------------------------------------------//
    void DumpStats() override
    {
        std::printf("[null-renderer] draws: points=%lu lines=%lu tris=%lu quads=%lu indexed=%lu\n",
                    m_stats.points, m_stats.lines, m_stats.triangles,
                    m_stats.quads, m_stats.indexed);
        std::printf("[null-renderer] indexed formats: 1UV=%lu 2UV=%lu 3UV=%lu raw=%lu\n",
                    m_stats.indexed1UV, m_stats.indexed2UV,
                    m_stats.indexed3UV, m_stats.indexedRaw);
        std::printf("[null-renderer] SetRenderState=%lu textDraws=%lu textures=%u stackErrors=%lu\n",
                    m_state.SetCalls(), m_stats.textDraws,
                    (unsigned)m_textures.size(), m_state.StackErrors());
        // M8-B69. OPT-IN, because this line lands on the strict bar's hashed
        // stdout: printing it unconditionally re-baselines a reference that has
        // stood since August, and a moved reference looks exactly like a
        // regression to the next session. That cost one bar run to notice.
        {
            const char* churn = std::getenv("MC2_TEXTURE_CHURN");
            if (churn && churn[0] && std::strcmp(churn, "0"))
            std::printf("[null-renderer] texture churn: created=%lu destroyed=%lu\n",
                        m_texCreated, m_texDestroyed);
        }
        std::printf("[null-renderer] texture decode: decoded=%lu failed=%lu dimensions=%u\n",
                    m_decoded, m_decodeFailed, (unsigned)m_dimensions.size());
        std::printf("[null-renderer] text: fonts=%lu failed=%lu glyphs=%lu backgrounds=%lu\n",
                    m_stats.fontsLoaded, m_stats.fontLoadFailed,
                    m_stats.glyphs, m_stats.textBackgrounds);

        // The measurement that actually sizes the WebGPU work: which states, and
        // how many distinct values each takes.
        m_state.DumpCensus("null-renderer");
    }

private:
    DWORD DecodeAndCreate(const char* name, gos_TextureFormat requested,
                          const BYTE* bytes, DWORD size, DWORD hints)
    {
        mc2gfx::TgaImage image;
        std::string error;
        if (!mc2gfx::DecodeTga(bytes, size, image, error,
                               requested == gos_Texture_Alpha))
        {
            ++m_decodeFailed;
            std::fprintf(stderr, "[null-renderer] TGA decode failed for '%s': %s\n",
                         name ? name : "", error.c_str());
            return Create(name, requested == gos_Texture_Detect ? gos_Texture_Alpha : requested,
                          256, 256, nullptr);
        }
        const gos_TextureFormat actual =
            mc2gfx::ApplyTgaFormatAndHints(image, requested, hints);
        ++m_decoded;
        return Create(name, actual, image.width, image.height, &image.rgba);
    }

    DWORD Create(const char* name, gos_TextureFormat format, DWORD width, DWORD height,
                 const std::vector<unsigned char>* pixels)
    {
        Texture* t = new Texture();
        t->name        = name ? name : "";
        t->format      = format;
        t->width       = width ? width : 256;
        t->height      = height ? height : 256;
        t->locked      = false;

        if (pixels) t->pixels = *pixels;
        else t->pixels.assign(static_cast<size_t>(t->width) * t->height *
                              bytesPerPixelFor(format), 0);
        m_dimensions.insert(std::make_pair(t->width, t->height));

        DWORD handle = m_nextTexture++;
        ++m_texCreated;
        m_textures[handle] = t;
        return handle;
    }

    struct DrawStats
    {
        unsigned long points, lines, triangles, quads, indexed;
        unsigned long indexed1UV, indexed2UV, indexed3UV, indexedRaw;
        unsigned long textDraws;
        unsigned long fontsLoaded, fontLoadFailed, glyphs, textBackgrounds;
    };

    mc2gfx::RenderStateTracker m_state;
    mc2gfx::TextMetrics        m_text;
    DrawStats                  m_stats;
    std::map<DWORD, Texture*>  m_textures;
    std::set<mc2gfx::FontData*> m_fonts;
    DWORD                      m_nextTexture;
    // M8-B69: "textures=" counts what is still LIVE at teardown, which cannot
    // separate "made fewer" from "destroyed more". These two can.
    unsigned long              m_texCreated;
    unsigned long              m_texDestroyed;
    unsigned long              m_decoded;
    unsigned long              m_decodeFailed;
    std::set<std::pair<DWORD, DWORD> > m_dimensions;
};

} // namespace

RendererBackend* CreateNullRenderer()
{
    static NullRenderer instance;
    return &instance;
}
