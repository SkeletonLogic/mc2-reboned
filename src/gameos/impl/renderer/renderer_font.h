//===========================================================================//
// GameOS regular-grid TGA font parsing and layout, shared by both renderers.
//
// The public GameOS header documents the format: cell zero is a one-pixel box
// that encodes the grid size, cell one is ASCII 33, and later cells increment
// through CharCount.  Keeping parsing and layout here makes null metrics exactly
// match the WebGPU glyph placement instead of maintaining two approximations.
//===========================================================================//

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <gameos.hpp>

#include "renderer_tga.h"

namespace mc2gfx {

struct FontGlyph
{
    int x = 0, y = 0, width = 0, height = 0;
    int leading = 0, topOffset = 0, advance = 0;
    bool valid = false;
};

struct FontData
{
    std::string name;
    DWORD textureHandle = 0;
    bool ownsTexture = false;
    bool parsed = false;
    int textureWidth = 0, textureHeight = 0;
    int gridWidth = 8, gridHeight = 10;
    int across = 1;
    int charCount = 256;
    FontGlyph glyphs[256];
};

inline unsigned char FontMaskAt(const TgaImage& image, size_t pixel)
{
    const size_t offset = pixel * 4;
    if (image.sourceHasAlpha) return image.rgba[offset + 3];
    return std::max(image.rgba[offset],
                    std::max(image.rgba[offset + 1], image.rgba[offset + 2]));
}

inline void MakeFontMask(TgaImage& image)
{
    const size_t pixels = static_cast<size_t>(image.width) * image.height;
    for (size_t pixel = 0; pixel < pixels; ++pixel)
    {
        const unsigned char alpha = FontMaskAt(image, pixel);
        const size_t offset = pixel * 4;
        image.rgba[offset + 0] = 255;
        image.rgba[offset + 1] = 255;
        image.rgba[offset + 2] = 255;
        image.rgba[offset + 3] = alpha;
    }
    image.sourceHasAlpha = true;
}

// MC2's production UI uses GameOS D3F4 files rather than the documented grid
// TGA form. The file contains six 256-byte metric tables followed by a square
// 8-bit glyph atlas. Tables 1/2/3 are atlas x, atlas y, and cell advance.
inline bool DecodeD3f4Font(const unsigned char* bytes, size_t size,
                           TgaImage& image, FontData& font, std::string& error)
{
    static const size_t kHeaderBytes = 89;
    static const size_t kMetricBytes = 6 * 256;
    static const size_t kAtlasOffset = kHeaderBytes + kMetricBytes;
    error.clear();
    if (!bytes || size < kAtlasOffset || std::memcmp(bytes, "D3F4", 4) != 0)
    {
        error = "not a GameOS D3F4 font";
        return false;
    }

    const size_t atlasBytes = size - kAtlasOffset;
    const uint32_t side = static_cast<uint32_t>(std::sqrt(static_cast<double>(atlasBytes)));
    if (side == 0 || static_cast<size_t>(side) * side != atlasBytes)
    {
        error = "D3F4 glyph atlas is not square";
        return false;
    }
    const int lineHeight = bytes[81];
    if (lineHeight <= 0 || lineHeight > static_cast<int>(side))
    {
        error = "D3F4 line height is invalid";
        return false;
    }

    image = TgaImage();
    image.width = side;
    image.height = side;
    image.sourceBits = 8;
    image.topOrigin = true;
    image.sourceHasAlpha = true;
    image.rgba.resize(atlasBytes * 4);
    for (size_t pixel = 0; pixel < atlasBytes; ++pixel)
    {
        const unsigned char alpha = bytes[kAtlasOffset + pixel];
        image.rgba[pixel * 4 + 0] = 255;
        image.rgba[pixel * 4 + 1] = 255;
        image.rgba[pixel * 4 + 2] = 255;
        image.rgba[pixel * 4 + 3] = alpha;
    }

    const unsigned char* atlasX = bytes + kHeaderBytes + 256;
    const unsigned char* atlasY = bytes + kHeaderBytes + 512;
    const unsigned char* advances = bytes + kHeaderBytes + 768;
    font.textureWidth = static_cast<int>(side);
    font.textureHeight = static_cast<int>(side);
    font.gridHeight = lineHeight;
    font.gridWidth = 1;
    font.across = 1;
    font.charCount = 256;
    font.parsed = true;

    for (int code = 0; code < 256; ++code)
    {
        // D3F4's printable metric table has four reserved slots: table entry
        // 29 describes character 33 ('!'). Space has no atlas glyph.
        const int metric = code >= 33 ? code - 4 : code;
        const int cellX = atlasX[metric];
        const int cellY = atlasY[metric];
        const int advance = advances[metric];
        FontGlyph& glyph = font.glyphs[code];
        glyph.advance = std::max(1, advance);
        if (code == 32) continue;
        if (advance <= 0 || cellX + advance > static_cast<int>(side) ||
            cellY + lineHeight > static_cast<int>(side))
            continue;

        int minX = advance, minY = lineHeight, maxX = -1, maxY = -1;
        for (int y = 0; y < lineHeight; ++y)
            for (int x = 0; x < advance; ++x)
            {
                const size_t alpha =
                    (static_cast<size_t>(cellY + y) * side + cellX + x) * 4 + 3;
                if (image.rgba[alpha] <= 8) continue;
                minX = std::min(minX, x); minY = std::min(minY, y);
                maxX = std::max(maxX, x); maxY = std::max(maxY, y);
            }
        if (maxX < minX || maxY < minY) continue;
        glyph.x = cellX + minX;
        glyph.y = cellY + minY;
        glyph.width = maxX - minX + 1;
        glyph.height = maxY - minY + 1;
        glyph.leading = minX;
        glyph.topOffset = minY;
        glyph.valid = true;
    }
    return true;
}

inline bool ParseGridFont(const TgaImage& image, DWORD startLine, int charCount,
                          FontData& font, std::string& error)
{
    error.clear();
    if (image.width == 0 || image.height == 0 || image.rgba.empty())
    {
        error = "font texture is empty";
        return false;
    }
    if (startLine >= image.height)
    {
        error = "StartLine is outside the font texture";
        return false;
    }

    const auto occupied = [&](uint32_t x, uint32_t y)
    {
        return image.rgba[(static_cast<size_t>(y) * image.width + x) * 4 + 3] > 8;
    };

    int gridWidth = 0;
    while (gridWidth < static_cast<int>(image.width) &&
           occupied(static_cast<uint32_t>(gridWidth), startLine))
        ++gridWidth;
    int gridHeight = 0;
    while (startLine + static_cast<DWORD>(gridHeight) < image.height &&
           occupied(0, startLine + static_cast<DWORD>(gridHeight)))
        ++gridHeight;
    if (gridWidth < 2 || gridHeight < 2)
    {
        error = "top-left measurement box is missing or too small";
        return false;
    }

    font.textureWidth = static_cast<int>(image.width);
    font.textureHeight = static_cast<int>(image.height);
    font.gridWidth = gridWidth;
    font.gridHeight = gridHeight;
    font.across = std::max(1, static_cast<int>(image.width) / gridWidth);
    font.charCount = std::max(33, std::min(256, charCount));
    font.parsed = true;

    // ASCII 32 is represented by the measurement cell, not a drawable glyph.
    // Give it a useful proportional advance without ever sampling the box.
    font.glyphs[32].advance = std::max(1, gridWidth / 2);

    for (int code = 33; code < font.charCount; ++code)
    {
        const int cell = code - 32;
        const int cellX = (cell % font.across) * gridWidth;
        const int cellY = static_cast<int>(startLine) + (cell / font.across) * gridHeight;
        if (cellX + gridWidth > font.textureWidth ||
            cellY + gridHeight > font.textureHeight)
            break;

        int minX = gridWidth, minY = gridHeight, maxX = -1, maxY = -1;
        for (int y = 0; y < gridHeight; ++y)
            for (int x = 0; x < gridWidth; ++x)
                if (occupied(static_cast<uint32_t>(cellX + x),
                             static_cast<uint32_t>(cellY + y)))
                {
                    minX = std::min(minX, x); minY = std::min(minY, y);
                    maxX = std::max(maxX, x); maxY = std::max(maxY, y);
                }

        FontGlyph& glyph = font.glyphs[code];
        if (maxX >= minX && maxY >= minY)
        {
            glyph.x = cellX + minX;
            glyph.y = cellY + minY;
            glyph.width = maxX - minX + 1;
            glyph.height = maxY - minY + 1;
            glyph.leading = minX;
            glyph.topOffset = minY;
            glyph.advance = std::max(1, minX + glyph.width);
            glyph.valid = true;
        }
        else
        {
            glyph.advance = std::max(1, gridWidth / 2);
        }
    }
    return true;
}

struct TextGlyphPlacement
{
    const FontGlyph* glyph = nullptr;
    float x = 0, y = 0, width = 0, height = 0;
};

struct TextLayout
{
    std::vector<TextGlyphPlacement> glyphs;
    float width = 0;
    float height = 0;
};

struct TextMetrics
{
    FontData* font;
    int x, y;
    int left, top, right, bottom;
    float size;
    DWORD foreground;
    bool wordWrap, proportional, bold, italic;
    DWORD wrapType;
    bool disableEmbeddedCodes;

    TextMetrics()
        : font(nullptr), x(0), y(0), left(0), top(0), right(640), bottom(480),
          size(1.0f), foreground(0xFFFFFFFFu), wordWrap(false), proportional(true),
          bold(false), italic(false), wrapType(0), disableEmbeddedCodes(false) {}

    float Scale() const
    {
        if (!font || !font->parsed) return size < -1.0f ? -size / 10.0f :
                                             (size > 0.0f ? size : 1.0f);
        return size < 0.0f ? std::max(1.0f, -size) / font->gridHeight :
                             (size > 0.0f ? size : 1.0f);
    }

    float LineHeight() const
    { return (font && font->parsed ? font->gridHeight : 10) * Scale(); }

    float Advance(unsigned char code) const
    {
        const float scale = Scale();
        if (!font || !font->parsed)
            return std::max(1.0f, LineHeight() * 0.5f);
        const FontGlyph* glyph = code < 256 ? &font->glyphs[code] : nullptr;
        const int fallback = std::max(1, font->gridWidth / 2);
        return (proportional ? (glyph && glyph->advance ? glyph->advance : fallback) :
                               font->gridWidth) * scale;
    }

    TextLayout Layout(const char* text, bool includeGlyphs) const
    {
        struct Line { std::vector<unsigned char> chars; float width = 0; };
        std::vector<Line> lines(1);
        const float limit = static_cast<float>(std::max(1, right - left));
        const auto recalculate = [&](Line& line)
        {
            line.width = 0;
            for (unsigned char code : line.chars) line.width += Advance(code);
        };
        if (text)
            for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p; ++p)
            {
                if (*p == '\r') continue;
                if (*p == '\n') { lines.push_back(Line()); continue; }
                const float advance = Advance(*p);
                if (wordWrap && !lines.back().chars.empty() &&
                    lines.back().width + advance > limit)
                {
                    Line& current = lines.back();
                    std::vector<unsigned char>::reverse_iterator space =
                        std::find(current.chars.rbegin(), current.chars.rend(),
                                  static_cast<unsigned char>(' '));
                    if (space != current.chars.rend())
                    {
                        const size_t split = current.chars.size() - 1 -
                                             static_cast<size_t>(space - current.chars.rbegin());
                        Line next;
                        next.chars.assign(current.chars.begin() + split + 1,
                                          current.chars.end());
                        current.chars.erase(current.chars.begin() + split,
                                            current.chars.end());
                        recalculate(current);
                        recalculate(next);
                        lines.push_back(next);
                    }
                    else lines.push_back(Line());
                }
                if (wordWrap && lines.back().chars.empty() && *p == ' ') continue;
                lines.back().chars.push_back(*p);
                lines.back().width += advance;
            }

        TextLayout result;
        const float lineHeight = std::max(1.0f, LineHeight());
        result.height = lineHeight * lines.size();
        for (const Line& line : lines) result.width = std::max(result.width, line.width);
        if (!includeGlyphs || !font || !font->parsed) return result;

        float baseY = static_cast<float>(y);
        if (wrapType == 3)
            baseY = top + (static_cast<float>(bottom - top) - result.height) * 0.5f;
        const float scale = Scale();
        for (size_t lineIndex = 0; lineIndex < lines.size(); ++lineIndex)
        {
            const Line& line = lines[lineIndex];
            float cursor = static_cast<float>(lineIndex ? left : x);
            if (wrapType == 1) cursor = static_cast<float>(right) - line.width;
            else if (wrapType == 2 || wrapType == 3)
                cursor = left + (static_cast<float>(right - left) - line.width) * 0.5f;
            const float lineY = baseY + lineIndex * lineHeight;
            for (unsigned char code : line.chars)
            {
                const FontGlyph& glyph = font->glyphs[code];
                if (glyph.valid)
                {
                    TextGlyphPlacement placed;
                    placed.glyph = &glyph;
                    placed.x = cursor + glyph.leading * scale;
                    placed.y = lineY + glyph.topOffset * scale;
                    placed.width = glyph.width * scale;
                    placed.height = glyph.height * scale;
                    result.glyphs.push_back(placed);
                }
                cursor += Advance(code);
            }
        }
        return result;
    }

    void StringLength(DWORD* width, DWORD* height, const char* text) const
    {
        const TextLayout layout = Layout(text, false);
        if (width) *width = static_cast<DWORD>(std::ceil(std::max(0.0f, layout.width)));
        if (height) *height = static_cast<DWORD>(std::ceil(std::max(1.0f, layout.height)));
    }
};

} // namespace mc2gfx
