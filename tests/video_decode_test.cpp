//---------------------------------------------------------------------------//
// M8-1: prove the video seam decodes real pixels, not that it merely links.
//
// This drives the SAME Decoder interface MC2Movie drives, on a real converted
// movie, and writes the frame it gets to a .ppm. The comparison against
// ffmpeg's own extraction of the same frame is done outside, by the harness
// that runs this -- which is the point: if this decoder were wrong, a check
// written inside it would very likely be wrong the same way.
//
// It SKIPS rather than fails when there is no decoder or no converted movie,
// because both are legitimate states of this tree: the converted set is
// retail-derived and never committed, and a build without ffmpeg deliberately
// carries the null decoder. ctest must stay green in both.
//
//   usage: video_decode_test <movie.webm> <seconds> <out.ppm>
//---------------------------------------------------------------------------//

#include "video_decoder.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char** argv)
{
    const char* path    = argc > 1 ? argv[1] : nullptr;
    const double when   = argc > 2 ? std::atof(argv[2]) : 1.0;
    const char* outPath = argc > 3 ? argv[3] : nullptr;

    std::printf("video backend: %s\n", mc2video::BackendName());

    if (!path)
    {
        std::printf("SKIP: no movie given\n");
        return 0;
    }

    mc2video::Decoder* decoder = mc2video::CreateDecoder();
    if (!decoder)
    {
        std::printf("FAIL: CreateDecoder returned null, which it never may\n");
        return 1;
    }

    if (!decoder->Open(path))
    {
        // No ffmpeg in this build, or no converted assets on this machine.
        std::printf("SKIP: cannot open '%s' with backend '%s'\n",
                    path, mc2video::BackendName());
        delete decoder;
        return 0;
    }

    if (!decoder->Advance(when))
    {
        std::printf("FAIL: stream ended before %.3fs\n", when);
        delete decoder;
        return 1;
    }

    const unsigned char* bgra = decoder->Bgra();
    const int width = decoder->Width();
    const int height = decoder->Height();
    if (!bgra || width <= 0 || height <= 0)
    {
        std::printf("FAIL: Advance() succeeded but produced no frame\n");
        delete decoder;
        return 1;
    }

    // A frame that is entirely one colour is the classic shape of a decode that
    // "worked" and produced nothing usable, so it is called out here rather
    // than left for a human to notice on screen.
    bool uniform = true;
    for (int i = 4; i < width * height * 4 && uniform; i += 4)
        if (std::memcmp(bgra, bgra + i, 4) != 0) uniform = false;

    // The audio track is decoded whole, up front, because that is the shape
    // gosAudio_UserMemory needs. Reported here so a run says how many samples a
    // movie actually yielded rather than only that it declared a track.
    const int audioSamples = decoder->PreloadAudio();

    std::printf("decoded %dx%d at %.3fs audio=%dch/%dHz samples=%d uniform=%s\n",
                width, height, when, decoder->AudioChannels(),
                decoder->AudioSampleRate(), audioSamples,
                uniform ? "YES (suspicious)" : "no");

    if (decoder->AudioChannels() > 0 && audioSamples <= 0)
    {
        std::printf("FAIL: movie declares %d audio channels but preloaded nothing\n",
                    decoder->AudioChannels());
        delete decoder;
        return 1;
    }

    if (outPath)
    {
        FILE* out = std::fopen(outPath, "wb");
        if (!out)
        {
            std::printf("FAIL: cannot write '%s'\n", outPath);
            delete decoder;
            return 1;
        }
        std::fprintf(out, "P6\n%d %d\n255\n", width, height);
        for (int i = 0; i < width * height; ++i)
        {
            // BGRA in, RGB out -- see the header on why the decoder is BGRA.
            const unsigned char rgb[3] = { bgra[i * 4 + 2], bgra[i * 4 + 1], bgra[i * 4 + 0] };
            std::fwrite(rgb, 1, 3, out);
        }
        std::fclose(out);
        std::printf("wrote %s\n", outPath);
    }

    delete decoder;
    return uniform ? 1 : 0;
}
