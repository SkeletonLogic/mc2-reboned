//---------------------------------------------------------------------------//
// M8-B4: a channel must not report the PREVIOUS sound's play mode.
//
// This is the defect that made every movie after the first one silent, and it
// is invisible in play: the resource is created, assigned and reported as
// started, and simply never sounds. It cost three rounds of owner playtesting
// to find, so it gets a test that fails without the fix.
//
// THE CONTRACT, as MC2 itself uses it: the game decides whether a channel is
// free by comparing gosAudio_GetChannelPlayMode against gosAudio_PlayOnce --
// SoundSystem::update starts a stream only when the channel does NOT read
// PlayOnce (Soundsys.cpp:478). So a channel that has been emptied, or freshly
// loaded with a new resource, MUST NOT still read PlayOnce; if it does, the
// game never starts it again and the sound is lost with no error anywhere.
//
// Runs against whatever backend is compiled in, including the silent one --
// none of this depends on samples actually reaching a device.
//---------------------------------------------------------------------------//

#include <cstdio>
#include <cstring>

#include <gameos.hpp>

namespace {

int failures = 0;

void check(bool ok, const char* what)
{
    std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

const char* modeName(gosAudio_PlayMode m)
{
    switch (m) {
        case gosAudio_PlayOnce: return "PlayOnce";
        case gosAudio_Loop:     return "Loop";
        case gosAudio_Pause:    return "Pause";
        case gosAudio_Continue: return "Continue";
        case gosAudio_Stop:     return "Stop";
    }
    return "?";
}

// 16-bit PCM, one channel. Content is irrelevant; only the bookkeeping is
// under test, and gosAudio_UserMemory keeps the pointer rather than a copy.
short g_pcmA[256];
short g_pcmB[256];

HGOSAUDIO makeResource(void* pcm, int bytes)
{
    gosAudio_Format format;
    std::memset(&format, 0, sizeof(format));
    format.wFormatTag      = 1;
    format.nChannels       = 1;
    format.nSamplesPerSec  = 48000;
    format.wBitsPerSample  = 16;
    format.nBlockAlign     = 2;
    format.nAvgBytesPerSec = 48000 * 2;

    HGOSAUDIO h = 0;
    gosAudio_CreateResource(&h, gosAudio_UserMemory, "test clip", &format, pcm, bytes);
    return h;
}

} // namespace

int main()
{
    const int channel = 18;      // STREAM3CHANNEL, where the movies play.

    // First sound of the "session" -- the case that always worked, because the
    // channel starts at Stop.
    HGOSAUDIO first = makeResource(g_pcmA, sizeof(g_pcmA));
    check(first != 0, "first resource created");

    gosAudio_AssignResourceToChannel(channel, first);
    check(gosAudio_GetChannelPlayMode(channel) != gosAudio_PlayOnce,
          "a freshly assigned channel does not read PlayOnce");

    gosAudio_SetChannelPlayMode(channel, gosAudio_PlayOnce);
    check(gosAudio_GetChannelPlayMode(channel) == gosAudio_PlayOnce,
          "after SetChannelPlayMode the channel reads PlayOnce");

    // Tear it down the way MC2Movie does, through stopSupportSample.
    gosAudio_DestroyResource(&first);
    const gosAudio_PlayMode afterDestroy = gosAudio_GetChannelPlayMode(channel);
    std::printf("     after destroy the channel reads %s\n", modeName(afterDestroy));
    check(afterDestroy != gosAudio_PlayOnce,
          "an emptied channel does not read PlayOnce");

    // THE REGRESSION. Second sound on the same channel: this is the mission
    // select video, the comms video, and every objective voiceover after the
    // first. Before the fix this read PlayOnce and the game never started it.
    HGOSAUDIO second = makeResource(g_pcmB, sizeof(g_pcmB));
    gosAudio_AssignResourceToChannel(channel, second);
    const gosAudio_PlayMode afterReassign = gosAudio_GetChannelPlayMode(channel);
    std::printf("     after reassign the channel reads %s\n", modeName(afterReassign));
    check(afterReassign != gosAudio_PlayOnce,
          "a channel reused for a second sound does not read PlayOnce");

    // And the game's own start test must now pass, which is the thing that
    // actually matters -- the two lines above are only its ingredients.
    const bool gameWouldStartIt = (gosAudio_GetChannelPlayMode(channel) != gosAudio_PlayOnce);
    check(gameWouldStartIt,
          "SoundSystem::update would start the second sound");

    gosAudio_DestroyResource(&second);

    std::printf("%s\n", failures ? "FAILED" : "all channel-state checks passed");
    return failures ? 1 : 0;
}
