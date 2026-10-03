#pragma once

#include <cstddef>
#include <vector>

namespace mc2gfx {

// Platform-selected JPEG decoder contract.
//
// Pixels are returned as canonical RGBA8, matching renderer_tga.h. This is not a
// free choice: the TGA decoder explicitly swaps BGR->RGB on load, and WebGPU
// textures are created RGBA8Unorm, so RGBA is the port's single texture-memory
// representation. An earlier revision returned BGRA here on the assumption that
// it was "the legacy GameOS representation"; the effect was that every
// JPEG-sourced texture had red and blue exchanged. It showed up as teal terrain
// in mc2_01 -- the colour map averaged RGB(41,75,82), where swapping the channels
// gives RGB(82,75,41), the tan the map should be.
//
// A decoder peer that can only produce BGRA must swap before returning.
struct JpegImage
{
    unsigned long width = 0;
    unsigned long height = 0;
    std::vector<unsigned char> rgba;
};

bool DecodeJpeg(const unsigned char* bytes, size_t size, JpegImage& image);

} // namespace mc2gfx
