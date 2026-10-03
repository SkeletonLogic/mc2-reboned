//---------------------------------------------------------------------------//
// The decoder that plays nothing.
//
// Always built. When MC2_ENABLE_FFMPEG is off this is what CreateDecoder()
// returns, so the game behaves exactly as it has since Microsoft stripped Bink:
// playMovie() asks, Open() declines, nothing plays, nothing crashes. That
// matters for more than tidiness -- the strict bar runs a null-renderer build
// and must not move because video work is in progress.
//---------------------------------------------------------------------------//

#include "video_decoder.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mc2video
{
    namespace
    {
        class NullDecoder : public Decoder
        {
        public:
            bool Open(const char* path) override
            {
                // Reported once, and only when asked for, so a run that expects
                // movies says why it has none instead of failing silently.
                static bool reported = false;
                const char* verbose = std::getenv("MC2_VIDEO_CENSUS");
                if (!reported && verbose && *verbose && std::strcmp(verbose, "0") != 0)
                {
                    reported = true;
                    std::fprintf(stderr,
                        "[video] no decoder in this build -- '%s' will not play. "
                        "Rebuild with MC2_ENABLE_FFMPEG=ON to get video.\n",
                        path ? path : "(null)");
                    std::fflush(stderr);
                }
                return false;
            }

            bool Advance(double) override            { return false; }
            bool FrameIsNew() const override         { return false; }
            const unsigned char* Bgra() const override { return nullptr; }
            int Width() const override               { return 0; }
            int Height() const override              { return 0; }
            bool Finished() const override           { return true; }
            bool Rewind() override                   { return false; }
            int PreloadAudio() override              { return 0; }
            int PopAudio(short*, int) override       { return 0; }
            int AudioChannels() const override       { return 0; }
            int AudioSampleRate() const override     { return 0; }
            void Close() override                    {}
        };
    }

#if !defined(MC2_ENABLE_FFMPEG)
    Decoder* CreateDecoder()   { return new NullDecoder(); }
    bool DecoderAvailable()    { return false; }
    const char* BackendName()  { return "none"; }
#else
    // The ffmpeg translation unit defines CreateDecoder/DecoderAvailable/
    // BackendName; this one supplies the decoder it hands back when its
    // LoadLibrary probe comes up empty.
    Decoder* CreateNullDecoder() { return new NullDecoder(); }
#endif
}
