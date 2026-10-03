#pragma once

//---------------------------------------------------------------------------//
// M8-1 video playback seam.
//
// The game must not know what decodes a movie. Retail MC2 called Bink directly
// and Microsoft stripped that decoder before releasing the source, leaving
// MC2Movie an empty shell -- BltMovieFrame() did nothing, render() drew
// nothing, and stillPlaying was never set true. Rather than wire a second
// vendor API straight into game code, everything goes through this interface:
//
//   video_null.cpp     always built. Open() fails, so the game behaves exactly
//                      as it does today. This is what keeps the tree building
//                      and the strict bar unmoved while the real decoder is
//                      unavailable. CreateDecoder() also falls back to it if the
//                      ffmpeg libraries cannot be loaded -- though note that the
//                      DLLs are ordinary imports, not delay-loaded, so in an
//                      ffmpeg build they are a startup requirement rather than
//                      something this fallback can cover.
//   video_ffmpeg.cpp   built when MC2_ENABLE_FFMPEG is on. WebM demux, VP9
//                      video and Opus audio, all from one dependency.
//   (browser)          emscripten will supply its own, because a hidden
//                      <video> element decodes VP9+Opus for free there.
//
// PIXEL ORDER IS BGRA, not RGBA, and that is deliberate: gos_LockTexture hands
// back `unsigned long*` whose bytes are B,G,R,A on this little-endian target,
// and the texture pipeline was moved to BGRA precisely because that is the
// order the game paints in. A decoder that hands back RGBA here produces
// blue-for-red movies.
//---------------------------------------------------------------------------//

namespace mc2video
{
    class Decoder
    {
    public:
        virtual ~Decoder() {}

        // path is a resolved filename. Returns false if the file is missing,
        // unreadable, or carries no video stream -- callers must treat a false
        // here as "no movie" and carry on, never as a fatal error.
        virtual bool Open(const char* path) = 0;

        // Decode forward until the frame whose presentation time COVERS
        // `elapsedSeconds` is current -- that is the last frame whose stamp is
        // <= the clock, never the one after it. Returns false once the stream
        // ends AND the final frame has had its full duration on screen.
        virtual bool Advance(double elapsedSeconds) = 0;

        // True when the most recent Advance() actually replaced the picture.
        // The caller uploads to GPU tiles, which at 60 Hz against a 15 fps
        // movie is otherwise four identical uploads per movie frame.
        virtual bool FrameIsNew() const = 0;

        // Current frame, Width()*Height()*4 bytes of B,G,R,A. Null until the
        // first successful Advance().
        virtual const unsigned char* Bgra() const = 0;

        virtual int Width() const = 0;
        virtual int Height() const = 0;

        // True once the stream is exhausted; MC2Movie ends playback on this.
        virtual bool Finished() const = 0;

        // Back to the first frame, for MC2Movie::restart(). Without this a
        // "restart" only reset the caller's clock and the picture stayed
        // wherever the decoder had already reached.
        virtual bool Rewind() = 0;

        // AUDIO IS PRELOADED, NOT STREAMED, and that is forced by the sink
        // rather than chosen: gosAudio_UserMemory takes one pointer to PCM that
        // must stay valid for the life of the resource (audio_common.cpp:308),
        // so there is nothing to push samples into frame by frame. PreloadAudio
        // decodes the whole track in one audio-only pass -- no video decoding --
        // and returns the number of interleaved 16-bit samples waiting, 0 for
        // the 109 movies that carry no track at all. The caller then drains it
        // with PopAudio into a buffer it owns and keeps alive.
        //
        // The video path deliberately never touches audio. When it did, packets
        // accumulated for the whole movie with nothing draining them: cinema1
        // is 160.574 s of 48 kHz mono, which is 15.4 MB retained by the end.
        //
        // The owner ruled on 2026-08-15 that the sound stays inside the movie
        // file rather than shipping 152.7 MiB of sidecar WAV.
        virtual int PreloadAudio() = 0;
        virtual int PopAudio(short* destination, int maxSamples) = 0;
        virtual int AudioChannels() const = 0;
        virtual int AudioSampleRate() const = 0;

        virtual void Close() = 0;
    };

    // Never returns null. Without a real backend -- either not built in, or
    // built in but with the shared libraries missing at runtime -- this hands
    // back a decoder whose Open() fails, and the game carries on silently.
    Decoder* CreateDecoder();

    // True when a real decoder is available RIGHT NOW, libraries and all. The
    // .bik/.webm existence gates in MissionSelectionScreen and ForceGroupBar
    // consult this so they do not offer a movie nothing can play.
    bool DecoderAvailable();

    // Names the backend for logs, so a run says which one it actually got
    // rather than leaving it to be inferred.
    const char* BackendName();
}
