//===========================================================================//
// Emscripten JPEG peer using the toolchain's maintained libjpeg port.
// Decoding is synchronous from memory and returns the port's canonical BGRA8.
//===========================================================================//

#include "../renderer_image_decode.h"

#include <csetjmp>
#include <cstdio>
#include <limits>

extern "C" {
#include <jpeglib.h>
}

namespace mc2gfx {
namespace {

struct JpegError
{
    jpeg_error_mgr base;
    std::jmp_buf jump;
};

void onJpegError(j_common_ptr common)
{
    JpegError* error = reinterpret_cast<JpegError*>(common->err);
    longjmp(error->jump, 1);
}

} // namespace

bool DecodeJpeg(const unsigned char* bytes, size_t size, JpegImage& image)
{
    image = JpegImage{};
    if (!bytes || !size || size > std::numeric_limits<unsigned long>::max()) return false;

    jpeg_decompress_struct decoder = {};
    JpegError error = {};
    decoder.err = jpeg_std_error(&error.base);
    error.base.error_exit = onJpegError;
    if (setjmp(error.jump))
    {
        jpeg_destroy_decompress(&decoder);
        image = JpegImage{};
        return false;
    }

    jpeg_create_decompress(&decoder);
    jpeg_mem_src(&decoder, bytes, static_cast<unsigned long>(size));
    if (jpeg_read_header(&decoder, TRUE) != JPEG_HEADER_OK)
    {
        jpeg_destroy_decompress(&decoder);
        return false;
    }
    decoder.out_color_space = JCS_RGB;
    jpeg_start_decompress(&decoder);
    const size_t width = decoder.output_width;
    const size_t height = decoder.output_height;
    if (!width || !height || width > std::numeric_limits<size_t>::max() / height / 4u)
    {
        jpeg_destroy_decompress(&decoder);
        return false;
    }

    image.width = static_cast<unsigned long>(width);
    image.height = static_cast<unsigned long>(height);
    image.rgba.resize(width * height * 4u);
    std::vector<unsigned char> row(width * 3u);
    while (decoder.output_scanline < decoder.output_height)
    {
        JSAMPROW source = row.data();
        jpeg_read_scanlines(&decoder, &source, 1);
        unsigned char* destination = image.rgba.data() +
            (static_cast<size_t>(decoder.output_scanline) - 1u) * width * 4u;
        for (size_t x = 0; x < width; ++x)
        {
            // PORT: libjpeg produces RGB, but the renderer's canonical texture
            // storage is BGRA (aec15fc).  The native WIC peer was changed with
            // that fix; this browser-only peer was missed, which made the JPEG
            // terrain composite blue while TGA/UI surfaces remained correct.
            destination[x * 4u + 0u] = row[x * 3u + 2u];
            destination[x * 4u + 1u] = row[x * 3u + 1u];
            destination[x * 4u + 2u] = row[x * 3u + 0u];
            destination[x * 4u + 3u] = 255;
        }
    }
    jpeg_finish_decompress(&decoder);
    jpeg_destroy_decompress(&decoder);
    return true;
}

} // namespace mc2gfx
