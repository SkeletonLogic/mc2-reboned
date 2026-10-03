//===========================================================================//
// Null audio backend -- accepts everything, plays nothing.
//
// This is not a placeholder to be deleted later. It is the backend the strict
// fixed-60 regression runs against, and the entire published hash history
// depends on it staying silent AND on its observable behaviour never changing.
//
// The one behaviour that matters: IsPlaying() is always false. The portable
// layer therefore never auto-completes a channel, so gosAudio_GetChannelPlayMode
// keeps returning exactly what the game last wrote -- which is what the silent
// path has always done. A backend that reported completion would change what the
// game observes and move the regression hashes without playing a sound.
//===========================================================================//

#include "audio/audio_backend.h"

namespace mc2audio {

namespace {

class NullAudioBackend : public AudioBackend
{
public:
    const char* Name() const override { return "null"; }

    bool Open() override  { return true; }
    void Close() override {}

    void Play(int, const Clip&, float, float, bool) override {}
    void Stop(int) override {}
    void SetVolume(int, float) override {}
    void SetPan(int, float) override {}

    // Never "finishes", because it never started. See the note above.
    bool IsPlaying(int) const override { return false; }

    // Nothing to emit, so nothing should be read or decoded on our behalf.
    bool WantsSamples() const override { return false; }
};

} // namespace

AudioBackend* MakeNullAudioBackend()
{
    static NullAudioBackend backend;
    return &backend;
}

} // namespace mc2audio
