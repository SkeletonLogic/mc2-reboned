//===========================================================================//
// Audio backend seam.
//
// Cut the same way the VFS and renderer seams were cut: everything portable --
// resource lifetime, channel bookkeeping, volume/pan state, the play-mode the
// game reads back -- lives ABOVE this interface in audio_common.cpp. What is
// below is only "put these samples on the device".
//
// SIZED BY MEASUREMENT, NOT BY THE HEADER. GameOS.HPP declares ~60 gosAudio_*
// entry points. The M6-4 census of what MechCommander 2 actually calls, over a
// 6000-tick mc2_01 run, is:
//
//   createResource   107   106 x gosAudio_UserMemory (raw PCM already in memory)
//                            1 x gosAudio_StreamedFile ('data\sound\music01.wav')
//   formats                 63 x mono 22050/16, 30 x mono 44100/16,
//                           11 x mono 11025/8,   2 x stereo 22050/16
//   allocateSliders   20   channels
//   setChannelSlider  63   62 x gosAudio_Volume, 1 x gosAudio_Panning
//   assignToChannel    1
//   setPlayMode        1   (the music stream; no effect ever fires headless)
//   getPlayMode   101380   polled every frame on every channel
//
// So the device surface is small: play a PCM buffer or a WAV file on a channel
// with a volume and a pan, stop it, and answer "is it still going". Everything
// else the header offers is unused and stays unimplemented rather than guessed.
//===========================================================================//

#pragma once

#include <cstddef>

namespace mc2audio {

// A resource as the portable layer holds it. Raw PCM only -- the game hands us
// already-decoded WAV data for UserMemory, and streamed files are read by the
// common layer so backends never touch the VFS.
struct Clip
{
    const unsigned char* pcm      = nullptr;
    std::size_t          bytes    = 0;
    int                  channels = 1;
    int                  rate     = 22050;
    int                  bits     = 16;
};

class AudioBackend
{
public:
    virtual ~AudioBackend() {}

    virtual const char* Name() const = 0;

    // Open/close the device. Open() returning false must leave the game running
    // silently -- audio is never allowed to be fatal.
    virtual bool Open()  = 0;
    virtual void Close() = 0;

    // volume 0..1, pan -1..+1 (GameOS's documented ranges).
    virtual void Play(int channel, const Clip& clip, float volume, float pan, bool loop) = 0;
    virtual void Stop(int channel) = 0;
    virtual void SetVolume(int channel, float volume) = 0;
    virtual void SetPan(int channel, float pan) = 0;

    // Whether this backend has any use for decoded samples. The null backend
    // says no, and the portable layer then never reads or decodes a streamed
    // track at all -- silent runs must not pay for audio they cannot emit. Not
    // an optimisation: decoding under the null peer moved the published
    // 2000-tick memory figures (47,096,997 -> 51,457,385 liveBytes).
    virtual bool WantsSamples() const { return true; }

    // True while the device still has samples queued for this channel. The null
    // backend answers false forever, which is what keeps the silent path's
    // observable behaviour -- and therefore the strict regression -- unchanged.
    virtual bool IsPlaying(int channel) const = 0;

    // Called once per shutdown dump; backends may report device-side counters.
    virtual void DumpStats() const {}
};

// Selection only, no policy. MC2_AUDIO=null|win32 (default: win32 natively,
// null everywhere else and whenever the device will not open).
AudioBackend& Audio();
void          AudioSelect();
void          AudioShutdown();

AudioBackend* MakeNullAudioBackend();
#if defined(_WIN32)
AudioBackend* MakeWin32AudioBackend();
#endif
#if defined(__EMSCRIPTEN__)
// C2 (owner ruling 2026-09-02): the browser peer. Web Audio needs none of the
// per-sample gain machinery win32 carries -- a GainNode is per-voice by
// construction and accepts a change while the source plays.
AudioBackend* MakeWebAudioBackend();
#endif

} // namespace mc2audio
