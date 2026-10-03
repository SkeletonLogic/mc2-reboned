//===========================================================================//
// Small TGA decoder shared by the null and WebGPU renderers.
//
// GameOS documents exactly four accepted forms for gos_NewTextureFrom*: 24/32
// bit true-colour TGA, uncompressed or RLE. Keeping that decoder here avoids an
// image dependency and, more importantly, keeps the null and GPU data paths on
// identical dimensions, origin handling, alpha classification, and hints.
//===========================================================================//

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <gameos.hpp>

namespace mc2gfx {

struct TgaImage
{
    uint32_t width = 0;
    uint32_t height = 0;
    uint8_t sourceBits = 0;
    bool rle = false;
    bool topOrigin = false;
    bool rightOrigin = false;
    bool sourceHasAlpha = false;
    // PORT: BGRA byte order, NOT RGBA, despite the name. This is the D3D
    // convention MCLib was written against: a pixel read as a DWORD is
    // 0x00RRGGBB, which little-endian stores as B,G,R,A.
    //
    // It has to be this way round because the game PAINTS textures through
    // gos_LockTexture using exactly that DWORD form -- mech paint schemes and
    // the MechIcon damage cards -- while other code COPIES pixels between
    // textures verbatim. Only one byte order can satisfy both, and it is the
    // game's. M4-3 originally reordered TGA's native BGRA into RGBA, which fixed
    // the loaded surfaces and left every painted surface with red and blue
    // exchanged. TGA is already BGRA, so this is now a straight copy.
    //
    // The name is left alone deliberately: renaming it touches every consumer
    // and this comment is what actually prevents the mistake.
    std::vector<unsigned char> rgba;
};

inline uint16_t TgaU16(const unsigned char* p)
{
    return static_cast<uint16_t>(p[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8);
}

inline bool DecodeTga(const unsigned char* bytes, size_t size,
                      TgaImage& image, std::string& error,
                      bool acceptUnmarkedAlpha = false)
{
    image = TgaImage();
    error.clear();
    if (!bytes || size < 18)
    {
        error = "shorter than the 18-byte TGA header";
        return false;
    }

    const uint8_t idLength = bytes[0];
    const uint8_t colorMapType = bytes[1];
    const uint8_t imageType = bytes[2];
    const uint16_t width = TgaU16(bytes + 12);
    const uint16_t height = TgaU16(bytes + 14);
    const uint8_t bits = bytes[16];
    const uint8_t descriptor = bytes[17];

    if (colorMapType != 0)
    {
        error = "colour-mapped TGA is outside the documented GameOS format";
        return false;
    }
    if (imageType != 2 && imageType != 10)
    {
        error = "TGA image type is not true-colour uncompressed (2) or RLE (10)";
        return false;
    }
    if (bits != 24 && bits != 32)
    {
        error = "TGA pixel depth is not 24 or 32 bits";
        return false;
    }
    if (width == 0 || height == 0)
    {
        error = "TGA dimensions are zero";
        return false;
    }

    const size_t pixelCount = static_cast<size_t>(width) * height;
    if (pixelCount > (SIZE_MAX / 4))
    {
        error = "TGA dimensions overflow RGBA storage";
        return false;
    }
    const size_t bytesPerPixel = bits / 8;
    size_t cursor = 18u + idLength;
    if (cursor > size)
    {
        error = "TGA ID field extends past the blob";
        return false;
    }

    image.width = width;
    image.height = height;
    image.sourceBits = bits;
    image.rle = imageType == 10;
    image.topOrigin = (descriptor & 0x20) != 0;
    image.rightOrigin = (descriptor & 0x10) != 0;
    // PORT: MC2's animated water-detail TGAs are 32-bit and store meaningful
    // fourth-channel alpha, but their legacy descriptor declares zero attribute
    // bits. Preserve those bytes only when the caller explicitly requests an
    // alpha texture; Detect/Keyed/Solid retain the descriptor-driven contract.
    image.sourceHasAlpha = bits == 32 &&
        ((descriptor & 0x0F) != 0 || acceptUnmarkedAlpha);
    image.rgba.assign(pixelCount * 4, 0);

    size_t emitted = 0;
    while (emitted < pixelCount)
    {
        size_t packetCount = 1;
        bool repeated = false;
        if (image.rle)
        {
            if (cursor >= size)
            {
                error = "RLE packet header extends past the blob";
                return false;
            }
            const uint8_t packet = bytes[cursor++];
            packetCount = static_cast<size_t>(packet & 0x7F) + 1;
            repeated = (packet & 0x80) != 0;
            if (packetCount > pixelCount - emitted)
            {
                error = "RLE packet emits past the declared image size";
                return false;
            }
        }

        unsigned char repeatedPixel[4] = {};
        if (repeated)
        {
            if (cursor + bytesPerPixel > size)
            {
                error = "RLE pixel extends past the blob";
                return false;
            }
            // PORT: straight copy -- TGA stores B,G,R,A and so do we now.
            repeatedPixel[0] = bytes[cursor + 0];
            repeatedPixel[1] = bytes[cursor + 1];
            repeatedPixel[2] = bytes[cursor + 2];
            repeatedPixel[3] = image.sourceHasAlpha ? bytes[cursor + 3] : 255;
            cursor += bytesPerPixel;
        }

        for (size_t p = 0; p < packetCount; ++p)
        {
            unsigned char rgba[4];
            if (repeated)
            {
                for (int c = 0; c < 4; ++c) rgba[c] = repeatedPixel[c];
            }
            else
            {
                if (cursor + bytesPerPixel > size)
                {
                    error = "pixel data extends past the blob";
                    return false;
                }
                // PORT: straight copy -- TGA stores B,G,R,A and so do we now.
                rgba[0] = bytes[cursor + 0];
                rgba[1] = bytes[cursor + 1];
                rgba[2] = bytes[cursor + 2];
                rgba[3] = image.sourceHasAlpha ? bytes[cursor + 3] : 255;
                cursor += bytesPerPixel;
            }

            const size_t sourceX = emitted % width;
            const size_t sourceY = emitted / width;
            const size_t x = image.rightOrigin ? width - 1u - sourceX : sourceX;
            const size_t y = image.topOrigin ? sourceY : height - 1u - sourceY;
            const size_t destination = (y * width + x) * 4;
            for (int c = 0; c < 4; ++c) image.rgba[destination + c] = rgba[c];
            ++emitted;
        }
    }
    return true;
}

inline gos_TextureFormat ClassifyTgaAlpha(const TgaImage& image)
{
    bool anyNonOpaque = false;
    bool binaryRangeOnly = true;
    for (size_t i = 3; i < image.rgba.size(); i += 4)
    {
        const unsigned char alpha = image.rgba[i];
        if (alpha != 255) anyNonOpaque = true;
        if (alpha > 25 && alpha < 230) binaryRangeOnly = false;
    }
    if (!anyNonOpaque) return gos_Texture_Solid;
    return binaryRangeOnly ? gos_Texture_Keyed : gos_Texture_Alpha;
}

inline void ForceOpaqueAlpha(std::vector<unsigned char>& pixels)
{
    for (size_t i = 3; i < pixels.size(); i += 4) pixels[i] = 255;
}

// PORT: gos_Texture_Keyed is a pixel contract, not merely a storage format.
// Memory-generated terrain transitions can contain opaque 24-bit hot-pink
// matte, so restore the legacy colour key before they reach BGRA8 WebGPU.
inline void ApplyKeyedAlpha(std::vector<unsigned char>& pixels,
                            bool removePinkFringe = false)
{
    for (size_t i = 0; i + 3 < pixels.size(); i += 4)
    {
        const bool hotPink = pixels[i + 0] == 255 && pixels[i + 1] == 0 &&
                             pixels[i + 2] == 255;
        const bool retailMatte = pixels[i + 0] == 127 && pixels[i + 1] == 0 &&
                                 pixels[i + 2] == 226;
        // PORT: The terrain combiner anti-aliases road pixels against its
        // hot-pink source matte before creating an unnamed keyed texture.
        // Legacy 1555 conversion discarded that fringe with the key; BGRA8
        // retains it unless the strongly pink edge colours are keyed too.
        const bool pinkFringe = removePinkFringe && pixels[i + 0] >= 160 &&
                                pixels[i + 2] >= 160 && pixels[i + 1] <= 96 &&
                                pixels[i + 0] >= pixels[i + 1] + 80 &&
                                pixels[i + 2] >= pixels[i + 1] + 80;
        pixels[i + 3] = (hotPink || retailMatte || pinkFringe ||
                         pixels[i + 3] < 128) ? 0 : 255;
    }
}

inline gos_TextureFormat ApplyTgaFormatAndHints(TgaImage& image,
                                                gos_TextureFormat requested,
                                                DWORD hints)
{
    const bool pinkKey = (hints & gosHint_PinkColorKey) != 0;
    const bool forceAlpha = requested == gos_Texture_Detect &&
                            (hints & gosHint_ForceAlpha) != 0;

    if (pinkKey)
    {
        for (size_t i = 0; i < image.rgba.size(); i += 4)
            image.rgba[i + 3] = image.rgba[i] == 255 && image.rgba[i + 1] == 0 &&
                                image.rgba[i + 2] == 255 ? 0 : 255;
        return gos_Texture_Keyed;
    }

    if (forceAlpha && ClassifyTgaAlpha(image) == gos_Texture_Solid)
    {
        // PORT: still the BLUE channel, which is now index 0, not index 2.
        for (size_t i = 0; i < image.rgba.size(); i += 4)
            image.rgba[i + 3] = image.rgba[i];
        const gos_TextureFormat classified = ClassifyTgaAlpha(image);
        return classified == gos_Texture_Solid ? gos_Texture_Alpha : classified;
    }

    if (requested == gos_Texture_Detect) return ClassifyTgaAlpha(image);
    if (requested == gos_Texture_Solid)
        ForceOpaqueAlpha(image.rgba);
    else if (requested == gos_Texture_Keyed || requested == gos_Texture_Alpha)
        for (size_t i = 0; i < image.rgba.size(); i += 4)
        {
            // PORT: MC2's retail interface atlases use 0xE2007F as their
            // transparent matte, including pixels whose stored TGA alpha is
            // opaque. The original 1555 keyed path discarded that matte; if
            // it survives, it appears as the thin magenta seams between
            // logistics-screen atlas pieces. Some custom UI meters and icons
            // request Alpha instead of Keyed, so discard the exact matte for
            // both transparent formats while leaving solid textures alone.
            const bool retailMatte = image.rgba[i + 0] == 127 &&
                                     image.rgba[i + 1] == 0 &&
                                     image.rgba[i + 2] == 226;
            const bool hotPink = image.rgba[i + 0] == 255 &&
                                 image.rgba[i + 1] == 0 &&
                                 image.rgba[i + 2] == 255;
            if (retailMatte || (requested == gos_Texture_Keyed && hotPink))
                image.rgba[i + 3] = 0;
            else if (requested == gos_Texture_Keyed)
                image.rgba[i + 3] = image.rgba[i + 3] < 128 ? 0 : 255;
        }
    return requested;
}

} // namespace mc2gfx
