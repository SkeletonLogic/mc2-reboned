#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

#include "renderer_font.h"

namespace {

int checks = 0;
int failures = 0;

void check(bool condition, const char* message)
{
    ++checks;
    if (!condition) { ++failures; std::printf("FAIL: %s\n", message); }
}

void pixel(mc2gfx::TgaImage& image, int x, int y, unsigned char alpha = 255)
{
    const size_t offset = (static_cast<size_t>(y) * image.width + x) * 4;
    image.rgba[offset + 0] = image.rgba[offset + 1] = image.rgba[offset + 2] = 255;
    image.rgba[offset + 3] = alpha;
}

} // namespace

int main(int argc, char** argv)
{
    mc2gfx::TgaImage image;
    image.width = 16; image.height = 12; image.sourceHasAlpha = true;
    image.rgba.assign(static_cast<size_t>(image.width) * image.height * 4, 0);

    // 4x6 measurement box in cell zero.
    for (int x = 0; x < 4; ++x) { pixel(image, x, 0); pixel(image, x, 5); }
    for (int y = 0; y < 6; ++y) { pixel(image, 0, y); pixel(image, 3, y); }
    // ASCII 33 in cell one: one occupied column with one leading pixel.
    for (int y = 1; y <= 4; ++y) pixel(image, 5, y);
    // ASCII 34 in cell two: three pixels wide with no leading space.
    for (int x = 8; x <= 10; ++x) pixel(image, x, 2);

    mc2gfx::FontData font;
    std::string error;
    check(mc2gfx::ParseGridFont(image, 0, 35, font, error), "synthetic font parses");
    check(font.gridWidth == 4 && font.gridHeight == 6, "measurement box sets 4x6 grid");
    check(font.across == 4, "grid has four cells across");
    check(font.glyphs[33].valid && font.glyphs[33].leading == 1 &&
          font.glyphs[33].width == 1 && font.glyphs[33].advance == 2,
          "proportional scan records leading, ink width, and advance");

    mc2gfx::TextMetrics text;
    text.font = &font; text.size = 2.0f; text.proportional = true;
    DWORD width = 0, height = 0;
    text.StringLength(&width, &height, "!!");
    check(width == 8 && height == 12, "positive scale metrics use proportional advances");
    text.proportional = false;
    text.StringLength(&width, &height, "!!");
    check(width == 16 && height == 12, "fixed pitch metrics use full grid cells");
    text.proportional = true; text.size = -12.0f;
    text.StringLength(&width, &height, "!");
    check(width == 4 && height == 12, "negative size selects absolute pixel height");
    text.StringLength(&width, &height, "");
    check(width == 0 && height == 12, "empty text retains non-zero line height");
    text.StringLength(&width, &height, "A");
    check(width == 4 && height == 12, "missing glyph retains non-zero fallback advance");

    text.size = 1.0f; text.wordWrap = true; text.left = 0; text.right = 4;
    text.StringLength(&width, &height, "!!!");
    check(width == 4 && height == 12, "wrapped metrics agree with two lines");

    // PORT (M8-B24): WHY aFont::height COULD NOT COUNT LINES IN THIS PORT.
    //
    // It measured the string inside the panel and then decided whether to run
    // its own wrap counter with
    //
    //     if ( width > areaWidth - 1 )   ...count lines...
    //     return lineHeightOfA * lineCount;      // lineCount starts at 1
    //
    // That assumes the measurement returns the UNWRAPPED width, which is what
    // retail's did. Here StringLength runs the same Layout that draws, so the
    // width it returns is the widest WRAPPED line, bounded by the region.
    //
    // MEASURED with the synthetic font above, region 40:
    //
    //     "one\ntwo\nthree\nfour"   width 10   height 24  (4 lines)   test: no
    //     "!! !! !! ..."           width 34   height 12  (2 lines)   test: no
    //
    // The test never fires, so a four-line string was sized as ONE line -- while
    // the right answer sat in the height returned by the very same call. These
    // pin both halves so it cannot regress.
    text.left = 0; text.right = 40;
    text.StringLength(&width, &height, "one\ntwo\nthree\nfour");
    check(width == 10, "wrapped width is the widest LINE, not the whole string");
    check(!(width > 40 - 1), "so aFont::height's old wrap test could not fire");
    check(height == 24, "the height from that same call is the true four-line height");

    text.StringLength(&width, &height, "!! !! !! !! !! !! !! !! !! !!");
    check(width == 34 && height == 12, "long text wraps to two lines inside the region");

    // PORT (M8-B26): THE OTHER HALF OF THE SAME MISTAKE, AND THE ONE THAT WAS
    // STILL CUTTING THE BRIEFINGS AFTER M8-B24.
    //
    // Layout wraps ONLY when wordWrap is set. aFont::height set it FALSE while
    // aFont::render set it TRUE, so a panel was sized in explicit newlines and
    // drawn wrapped, and ClipTextQuad dropped every line the wrap added.
    //
    // This pins the gap with a briefing-shaped string: two newline-separated
    // segments, each long enough to wrap once inside the region.
    {
        const char* briefingShaped =
            "!! !! !! !! !! !! !! !! !! !!\n!! !! !! !! !! !! !! !! !! !!";
        text.wordWrap = false;
        text.StringLength(&width, &height, briefingShaped);
        check(height == 12, "wrap OFF measures only the newline count -- two lines");
        text.wordWrap = true;
        text.StringLength(&width, &height, briefingShaped);
        check(height == 24, "wrap ON measures what is actually drawn -- four lines");
    }

    mc2gfx::FontData malformed;
    mc2gfx::TgaImage empty;
    check(!mc2gfx::ParseGridFont(empty, 0, 256, malformed, error),
          "empty font texture is rejected");

    // Minimal D3F4: six metric tables and a 4x4 alpha atlas.
    std::vector<unsigned char> d3f(89 + 6 * 256 + 16, 0);
    std::memcpy(d3f.data(), "D3F4", 4);
    d3f[81] = 4;
    d3f[89 + 256 + 29] = 1; // x for printable character '!'
    d3f[89 + 512 + 29] = 0; // y
    d3f[89 + 768 + 29] = 2; // advance
    d3f[89 + 6 * 256 + 1] = 255;
    d3f[89 + 6 * 256 + 5] = 255;
    mc2gfx::TgaImage d3fImage;
    mc2gfx::FontData d3fFont;
    check(mc2gfx::DecodeD3f4Font(d3f.data(), d3f.size(), d3fImage, d3fFont, error),
          "D3F4 font decodes");
    check(d3fImage.width == 4 && d3fImage.height == 4 && d3fFont.gridHeight == 4,
          "D3F4 atlas and line height decode");
    check(d3fFont.glyphs[33].valid && d3fFont.glyphs[33].advance == 2 &&
          d3fFont.glyphs[33].width == 1 && d3fFont.glyphs[33].height == 2,
          "D3F4 glyph metrics scan the atlas cell");

    if (argc > 1)
    {
        std::ifstream input(argv[1], std::ios::binary);
        std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(input)),
                                         std::istreambuf_iterator<char>());
        mc2gfx::TgaImage actualImage;
        mc2gfx::FontData actualFont;
        check(input.good() || input.eof(), "actual font file opens");
        check(mc2gfx::DecodeTga(bytes.data(), bytes.size(), actualImage, error),
              "actual font TGA decodes");
        mc2gfx::MakeFontMask(actualImage);
        check(mc2gfx::ParseGridFont(actualImage, 0, 256, actualFont, error),
              "actual GameOS grid font parses");
        if (actualFont.parsed)
        {
            mc2gfx::TextMetrics actualText;
            actualText.font = &actualFont; actualText.size = 2.0f;
            actualText.StringLength(&width, &height, "M3-4 TEXT METRICS 0123456789");
            std::printf("actual: %s texture=%dx%d grid=%dx%d across=%d metrics=%lux%lu\n",
                        argv[1], actualFont.textureWidth, actualFont.textureHeight,
                        actualFont.gridWidth, actualFont.gridHeight, actualFont.across,
                        static_cast<unsigned long>(width), static_cast<unsigned long>(height));
        }
    }

    std::printf("%d check(s), %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
