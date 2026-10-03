#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "renderer_tga.h"

namespace {

int checks = 0;
int failures = 0;

void check(bool condition, const char* message)
{
    ++checks;
    if (!condition)
    {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
}

uint64_t fnv1a(const std::vector<unsigned char>& bytes)
{
    uint64_t hash = 1469598103934665603ull;
    for (unsigned char byte : bytes)
    {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

void pixel(const mc2gfx::TgaImage& image, uint32_t x, uint32_t y,
           unsigned& r, unsigned& g, unsigned& b, unsigned& a)
{
    // PORT: the decoder now emits BGRA, the D3D order MCLib paints in, so blue
    // is index 0 and red is index 2. Every assertion below is written in terms
    // of r/g/b and is unchanged -- only the unpacking moved.
    const size_t offset = (static_cast<size_t>(y) * image.width + x) * 4;
    b = image.rgba[offset + 0]; g = image.rgba[offset + 1];
    r = image.rgba[offset + 2]; a = image.rgba[offset + 3];
}

bool writePpm(const char* path, const mc2gfx::TgaImage& image)
{
    std::FILE* output = std::fopen(path, "wb");
    if (!output) return false;
    std::fprintf(output, "P6\n%u %u\n255\n", image.width, image.height);
    // PPM is RGB, the buffer is BGRA -- unpack rather than blitting three bytes.
    for (size_t i = 0; i < image.rgba.size(); i += 4)
    {
        const unsigned char rgb[3] = { image.rgba[i + 2], image.rgba[i + 1], image.rgba[i] };
        std::fwrite(rgb, 1, 3, output);
    }
    std::fclose(output);
    return true;
}

void syntheticTests()
{
    {
        std::vector<unsigned char> pixels = {
            1, 2, 3, 0,
            4, 5, 6, 127,
            7, 8, 9, 255
        };
        mc2gfx::ForceOpaqueAlpha(pixels);
        check(pixels[3] == 255 && pixels[7] == 255 && pixels[11] == 255,
              "solid textures force every alpha byte opaque");
        check(pixels[0] == 1 && pixels[4] == 4 && pixels[8] == 7,
              "forcing solid alpha preserves color channels");
    }

    // 2x2, uncompressed 24-bit, bottom-left origin. File order is bottom row
    // (blue, yellow), then top row (red, green), all stored as BGR.
    std::vector<unsigned char> raw(18, 0);
    raw[2] = 2; raw[12] = 2; raw[14] = 2; raw[16] = 24;
    const unsigned char pixels[] = {
        255, 0, 0,   0, 255, 255,
        0, 0, 255,   0, 255, 0
    };
    raw.insert(raw.end(), pixels, pixels + sizeof(pixels));
    mc2gfx::TgaImage image;
    std::string error;
    check(mc2gfx::DecodeTga(raw.data(), raw.size(), image, error),
          "decode bottom-origin raw TGA");
    unsigned r, g, b, a;
    pixel(image, 0, 0, r, g, b, a);
    check(r == 255 && g == 0 && b == 0 && a == 255,
          "bottom-origin image is normalized to top-left RGBA");
    pixel(image, 1, 1, r, g, b, a);
    check(r == 255 && g == 255 && b == 0 && a == 255,
          "bottom-right yellow survives BGR conversion");

    // 3x1 top-origin RLE packet, 32-bit with eight attribute bits.
    std::vector<unsigned char> rle(18, 0);
    rle[2] = 10; rle[12] = 3; rle[14] = 1; rle[16] = 32; rle[17] = 0x28;
    const unsigned char packet[] = { 0x82, 30, 20, 10, 40 };
    rle.insert(rle.end(), packet, packet + sizeof(packet));
    check(mc2gfx::DecodeTga(rle.data(), rle.size(), image, error),
          "decode 32-bit RLE TGA");
    check(image.rle && image.sourceHasAlpha && image.width == 3 && image.height == 1,
          "RLE metadata is preserved");
    pixel(image, 2, 0, r, g, b, a);
    check(r == 10 && g == 20 && b == 30 && a == 40,
          "RLE BGRA packet expands to RGBA");

    // MC2 retail water detail: meaningful fourth-channel bytes in a 32-bit
    // TGA whose descriptor declares zero attribute bits. The default remains
    // descriptor-driven; an explicit Alpha request opts into the stored bytes.
    std::vector<unsigned char> unmarkedAlpha(18, 0);
    unmarkedAlpha[2] = 2; unmarkedAlpha[12] = 1; unmarkedAlpha[14] = 1;
    unmarkedAlpha[16] = 32; unmarkedAlpha[17] = 0;
    const unsigned char unmarkedPixel[] = { 255, 255, 255, 37 };
    unmarkedAlpha.insert(unmarkedAlpha.end(), unmarkedPixel,
                         unmarkedPixel + sizeof(unmarkedPixel));
    check(mc2gfx::DecodeTga(unmarkedAlpha.data(), unmarkedAlpha.size(), image, error) &&
          !image.sourceHasAlpha && image.rgba[3] == 255,
          "unmarked fourth channel stays ignored by default");
    check(mc2gfx::DecodeTga(unmarkedAlpha.data(), unmarkedAlpha.size(), image, error, true) &&
          image.sourceHasAlpha && image.rgba[3] == 37,
          "explicit alpha request preserves an unmarked fourth channel");

    std::vector<unsigned char> keyed(18, 0);
    keyed[2] = 2; keyed[12] = 1; keyed[14] = 1; keyed[16] = 24; keyed[17] = 0x20;
    const unsigned char pink[] = { 255, 0, 255 };
    keyed.insert(keyed.end(), pink, pink + sizeof(pink));
    check(mc2gfx::DecodeTga(keyed.data(), keyed.size(), image, error),
          "decode colour-key source");
    check(mc2gfx::ApplyTgaFormatAndHints(image, gos_Texture_Detect,
                                         gosHint_PinkColorKey) == gos_Texture_Keyed &&
          image.rgba[3] == 0,
          "pink hint produces keyed transparent magenta");

    mc2gfx::TgaImage retailKey;
    retailKey.width = 3; retailKey.height = 1; retailKey.sourceHasAlpha = true;
    retailKey.rgba = {
        127, 0, 226, 255, // MC2 retail interface matte, stored as BGRA
        20, 30, 40, 127,  // existing keyed-alpha transparency
        20, 30, 40, 255   // ordinary opaque texel
    };
    check(mc2gfx::ApplyTgaFormatAndHints(retailKey, gos_Texture_Keyed, 0) ==
              gos_Texture_Keyed &&
          retailKey.rgba[3] == 0 && retailKey.rgba[7] == 0 &&
          retailKey.rgba[11] == 255,
          "keyed textures discard the retail MC2 matte and preserve alpha keying");

    std::vector<unsigned char> keyedMemory = {
        255, 0, 255, 255, // 24-bit hot-pink matte expanded to opaque BGRA
        127, 0, 226, 255, // MC2 retail interface matte
        20, 30, 40, 127,  // existing keyed-alpha transparency
        20, 30, 40, 255
    };
    mc2gfx::ApplyKeyedAlpha(keyedMemory);
    check(keyedMemory[3] == 0 && keyedMemory[7] == 0 &&
          keyedMemory[11] == 0 && keyedMemory[15] == 255,
          "memory keyed textures discard both mattes and quantize alpha");

    std::vector<unsigned char> terrainComposite = {
        254, 0, 254, 255, // near-key fringe from terrain overlay composition
        216, 23, 225, 255,
        180, 81, 206, 255,
        159, 0, 255, 255, // outside the measured terrain-fringe envelope
        180, 97, 206, 255
    };
    mc2gfx::ApplyKeyedAlpha(terrainComposite, true);
    check(terrainComposite[3] == 0 && terrainComposite[7] == 0 &&
          terrainComposite[11] == 0 && terrainComposite[15] == 255 &&
          terrainComposite[19] == 255,
          "generated terrain keyed textures discard measured pink fringe only");

    retailKey.rgba = {
        127, 0, 226, 255, // opaque retail matte becomes transparent
        20, 30, 40, 117,  // continuous alpha remains continuous
        20, 30, 40, 255
    };
    check(mc2gfx::ApplyTgaFormatAndHints(retailKey, gos_Texture_Alpha, 0) ==
              gos_Texture_Alpha &&
          retailKey.rgba[3] == 0 && retailKey.rgba[7] == 117 &&
          retailKey.rgba[11] == 255,
          "alpha textures discard only the retail MC2 matte");

    std::vector<unsigned char> forced(18, 0);
    forced[2] = 2; forced[12] = 1; forced[14] = 1;
    forced[16] = 24; forced[17] = 0x20;
    const unsigned char blueAlpha[] = { 64, 20, 10 };
    forced.insert(forced.end(), blueAlpha, blueAlpha + sizeof(blueAlpha));
    check(mc2gfx::DecodeTga(forced.data(), forced.size(), image, error),
          "decode force-alpha source");
    check(mc2gfx::ApplyTgaFormatAndHints(image, gos_Texture_Detect,
                                         gosHint_ForceAlpha) == gos_Texture_Alpha &&
          image.rgba[3] == 64,
          "force-alpha hint copies the blue channel into alpha");
}

} // namespace

int main(int argc, char** argv)
{
    syntheticTests();

    if (argc >= 2)
    {
        std::ifstream input(argv[1], std::ios::binary);
        std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(input)),
                                         std::istreambuf_iterator<char>());
        check(bytes.size() >= 18, "input TGA has a full header");
        if (bytes.size() >= 18)
        {
            const unsigned headerWidth = mc2gfx::TgaU16(bytes.data() + 12);
            const unsigned headerHeight = mc2gfx::TgaU16(bytes.data() + 14);
            std::printf("header: type=%u width=%u height=%u bits=%u descriptor=0x%02x bytes=%u\n",
                        bytes[2], headerWidth, headerHeight, bytes[16], bytes[17],
                        (unsigned)bytes.size());

            mc2gfx::TgaImage image;
            std::string error;
            check(mc2gfx::DecodeTga(bytes.data(), bytes.size(), image, error),
                  "decode input TGA");
            if (!image.rgba.empty())
            {
                check(image.width == headerWidth && image.height == headerHeight,
                      "decoded dimensions match hand-read header");
                std::printf("decoded: width=%u height=%u bits=%u rle=%s origin=%s%s "
                            "alpha=%s fnv1a64=%016llX\n",
                            image.width, image.height, image.sourceBits,
                            image.rle ? "yes" : "no",
                            image.topOrigin ? "top" : "bottom",
                            image.rightOrigin ? "-right" : "-left",
                            image.sourceHasAlpha ? "yes" : "no",
                            (unsigned long long)fnv1a(image.rgba));
                const uint32_t xs[] = { 0, image.width - 1, 0, image.width - 1,
                                        image.width / 2 };
                const uint32_t ys[] = { 0, 0, image.height - 1, image.height - 1,
                                        image.height / 2 };
                const char* names[] = { "top-left", "top-right", "bottom-left",
                                        "bottom-right", "center" };
                for (int i = 0; i < 5; ++i)
                {
                    unsigned r, g, b, a;
                    pixel(image, xs[i], ys[i], r, g, b, a);
                    std::printf("pixel %s (%u,%u)=RGBA(%u,%u,%u,%u)\n",
                                names[i], xs[i], ys[i], r, g, b, a);
                }
                if (argc >= 3)
                {
                    check(writePpm(argv[2], image), "write decoded PPM");
                    std::printf("ppm: %s\n", argv[2]);
                }
            }
            else if (!error.empty())
                std::fprintf(stderr, "decode error: %s\n", error.c_str());
        }
    }

    std::printf("%d check(s), %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
