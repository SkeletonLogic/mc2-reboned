//===========================================================================//
// GameOS audio -- the portable layer.
//
// Everything that is not "put samples on a device" lives here, above the
// backend seam, exactly as vfs_resolve.cpp sits above VfsBackend: resource
// lifetime and handle issue, the channel table, volume/pan bookkeeping, reading
// streamed files through the VFS, and the call census.
//
// That split is what makes a second backend cheap. A WebAudio peer has to
// implement Play/Stop/SetVolume/SetPan/IsPlaying and nothing else -- it never
// sees a HGOSAUDIO, never parses a WAV, never touches the VFS.
//
// SILENT PATH IS SACRED. Under the null backend this file must produce exactly
// the observable behaviour the port has always had, because the strict fixed-60
// regression and every published hash depend on it. Two rules enforce that:
// nothing here prints unless MC2_AUDIO_CENSUS is set, and GetChannelPlayMode
// only consults the backend when the backend claims a channel is still going --
// null always says false, so the game keeps reading back exactly what it wrote.
//
// NOTE, unchanged from the original stub and still true: MC2 plays its MUSIC
// through soundSystem->playDigitalStream() over gosAudio_, NOT through GameOS's
// gosMusic_ API -- the game calls zero gosMusic_ functions.
//===========================================================================//

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <gameos.hpp>

#include "audio/audio_backend.h"
#include "vfs/vfs_resolve.h"

namespace {

const int kMaxChannels = 64;

struct Resource
{
    std::vector<unsigned char> owned;    // streamed files we read ourselves
    mc2audio::Clip             clip;
    std::string                path;     // streamed source, decoded lazily
    bool                       inUse = false;
};

struct ChannelState
{
    HGOSAUDIO         resource = 0;
    gosAudio_PlayMode mode     = gosAudio_Stop;
    float             volume   = 1.0f;
    float             pan      = 0.0f;
    bool              started  = false;
};

std::map<intptr_t, Resource> g_resources;
ChannelState                 g_channel[kMaxChannels];
int                          g_liveResources = 0;
int                          g_nextHandle    = 0;

//------------------------------------------------------------------ census ---
struct AudioCensus
{
    long createFromFile = 0, createFromHandle = 0, destroy = 0;
    long allocateSliders = 0, assignToChannel = 0, setSlider = 0, getSlider = 0;
    long setPlayMode = 0, getPlayMode = 0, playRequests = 0, stopRequests = 0;
    long peakLiveResources = 0;
    long long totalPcmBytes = 0;
    int  largestPcmBytes = 0;
    std::map<std::string, long> resourceNames, formats;
    std::map<int, long>         sliderProperty, resourceType;

    // Per-channel tally of what actually reached the device. See the family
    // table below for why the channel number is enough to name the family.
    long      channelPlays[kMaxChannels] = {};
    long long channelBytes[kMaxChannels] = {};
};

//------------------------------------------------------------------ family ---
// M8-2, 2026-08-20. The gap list (docs/gpt-tasks/M8-2-AUDIO-GAP.md) measured the
// gosAudio_ API surface and the shipped formats as fully covered, which leaves
// exactly one open question for row 8: WHICH FAMILIES OF SOUND HAVE ACTUALLY
// BEEN HEARD. Nobody has established that Betty speaks, that mission control is
// audible, or that the music crossfade ever uses its second stream.
//
// MC2 hard-codes its channel layout, so the channel number IS the family, and
// the tally costs one increment at the point samples go to the device. The map
// is restated here rather than included because gameos must not depend on a
// game header -- src/game/MCLib/Soundsys.h is the source, and these are its own
// constants:
//
//   RADIO_CHANNEL 0 . findOpenChannel(1, SUPPORT_CHANNEL) 1..12 .
//   SUPPORT_CHANNEL 13 . BETTY_CHANNEL 14 . FIRE_CHANNEL 15 .
//
// FIRE_CHANNEL IS NOT WEAPONS FIRE. That label was wrong in the first version
// of this table and the owner playtest of 2026-08-20 exposed it: the row read
// zero after a mission full of combat. Its only three references in the whole
// tree are the isRaining block in SoundSystem::update (Soundsys.cpp:421-444),
// so channel 15 is the RAIN AMBIENCE LOOP and retail simply named it badly.
// Weapons fire goes through playDigitalSample -> findOpenChannel(1,
// SUPPORT_CHANNEL), i.e. channels 1..12 with every other effect.
// Measured both ways over ten missions at 500 ticks: mc2_01..mc2_09 report
// zero on channel 15 and mc2_10 reports one play of 298,168 bytes.
//   STREAM1CHANNEL 16 . STREAM2CHANNEL 17 . STREAM3CHANNEL 18
//
// A family reporting ZERO after a mission that should have exercised it is a
// defect with a name; every family non-zero closes the coverage axis. Rows are
// printed even when zero on purpose -- an absent line is one the reader misses,
// and zero is the result this instrument exists to report.
struct FamilyRow { const char* name; int lo, hi; };

const FamilyRow kFamilies[] = {
    { "radio / pilot chatter",      0,  0  },
    { "sound effects + weapons",    1,  12 },
    { "support / mission control",  13, 13 },
    { "Betty",                      14, 14 },
    { "rain loop (FIRE_CHANNEL)",   15, 15 },
    { "music stream 1",             16, 16 },
    { "music stream 2 (crossfade)", 17, 17 },
    { "voiceover / movie STREAM3",  18, 18 },
    { "unmapped (>= 19)",           19, kMaxChannels - 1 },
};

AudioCensus& census() { static AudioCensus c; return c; }

bool censusEnabled()
{
    const char* want = std::getenv("MC2_AUDIO_CENSUS");
    return want && *want && std::strcmp(want, "0") != 0;
}

void noteLive()
{
    if (g_liveResources > census().peakLiveResources)
        census().peakLiveResources = g_liveResources;
}

Resource* find(HGOSAUDIO h)
{
    if (!h) return nullptr;
    std::map<intptr_t, Resource>::iterator it = g_resources.find((intptr_t)h);
    return it == g_resources.end() ? nullptr : &it->second;
}

// PORT: MS-ADPCM decoder. This sits ABOVE the backend seam on purpose: MC2's
// streamed music is not PCM, and every backend would otherwise need its own
// copy. Measured from the shipped data -- data\sound\music01.wav is
// wFormatTag=2 (MS-ADPCM), 4-bit, stereo, 44100, nBlockAlign=2048 -- which is
// why waveOut rejected it outright with WAVE_FORMAT_PCM (openFailures=2,
// buffers played=0) before this existed.
//
// The alternative was Win32's ACM (msacm32), but that would have locked music
// decoding to the native peer and left the browser peer to solve it again.
const int kAdaptTable[16] = {
    230, 230, 230, 230, 307, 409, 512, 614,
    768, 614, 512, 409, 307, 230, 230, 230
};
const int kAdaptCoef1[7] = { 256, 512, 0, 192, 240, 460, 392 };
const int kAdaptCoef2[7] = { 0, -256, 0, 64, 0, -208, -232 };

inline short clampSample(int v)
{
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return (short)v;
}

struct AdpcmChannelState { int coef1, coef2, delta, sample1, sample2; };

inline short adpcmNibble(AdpcmChannelState& s, int nibble)
{
    // Sign-extend the 4-bit nibble.
    const int signedNibble = (nibble & 0x08) ? (nibble - 16) : nibble;
    int predictor = (s.sample1 * s.coef1 + s.sample2 * s.coef2) / 256;
    predictor += signedNibble * s.delta;
    const short out = clampSample(predictor);
    s.sample2 = s.sample1;
    s.sample1 = out;
    s.delta = (kAdaptTable[nibble] * s.delta) / 256;
    if (s.delta < 16) s.delta = 16;
    return out;
}

// Decodes whole blocks into interleaved 16-bit PCM. Returns false rather than
// guessing if the stream is malformed -- a silent channel is always preferable
// to noise.
bool decodeMsAdpcm(const unsigned char* data, std::size_t bytes, int channels,
                   int blockAlign, const short* coefs, int numCoef,
                   std::vector<unsigned char>& outPcm)
{
    if (channels < 1 || channels > 2 || blockAlign <= 0 || !coefs || numCoef <= 0) return false;
    const int headerBytes = 7 * channels;
    if (blockAlign <= headerBytes) return false;

    std::vector<short> samples;
    for (std::size_t base = 0; base + (std::size_t)blockAlign <= bytes; base += (std::size_t)blockAlign)
    {
        const unsigned char* b = data + base;
        AdpcmChannelState st[2];
        int idx = 0;
        bool ok = true;
        for (int c = 0; c < channels; ++c) {
            const int pred = b[idx++];
            if (pred >= numCoef) { ok = false; break; }
            st[c].coef1 = coefs[pred * 2 + 0];
            st[c].coef2 = coefs[pred * 2 + 1];
        }
        if (!ok) break;
        for (int c = 0; c < channels; ++c) { st[c].delta   = (short)(b[idx] | (b[idx+1] << 8)); idx += 2; }
        for (int c = 0; c < channels; ++c) { st[c].sample1 = (short)(b[idx] | (b[idx+1] << 8)); idx += 2; }
        for (int c = 0; c < channels; ++c) { st[c].sample2 = (short)(b[idx] | (b[idx+1] << 8)); idx += 2; }

        // The block header carries the two most recent samples, oldest first.
        for (int c = 0; c < channels; ++c) samples.push_back(st[c].sample2);
        for (int c = 0; c < channels; ++c) samples.push_back(st[c].sample1);

        for (int p = idx; p < blockAlign; ++p) {
            const unsigned char byte = b[p];
            samples.push_back(adpcmNibble(st[0], (byte >> 4) & 0x0F));
            samples.push_back(adpcmNibble(st[channels > 1 ? 1 : 0], byte & 0x0F));
        }
    }
    if (samples.empty()) return false;
    outPcm.resize(samples.size() * sizeof(short));
    std::memcpy(outPcm.data(), samples.data(), outPcm.size());
    return true;
}

// PORT: streamed resources name a file instead of handing over memory. Read it
// through the VFS here so no backend has to know about game storage. Only the
// WAV container is understood, which is all the census says the game asks for.
bool loadWav(const char* path, Resource& res)
{
    // gos_GetFile is the whole-file read GameOS exposes, and it is already the
    // path everything else in the port uses -- so streamed audio picks up the
    // VFS's separator normalisation and case-insensitive fallback for free.
    BYTE* image = nullptr;
    DWORD size  = 0;
    gos_GetFile(path, &image, &size);
    if (!image) return false;
    // gos_GetFile hands back a gos_Malloc'd buffer and the caller owns it.
    // Copying then freeing keeps exactly one copy of the file alive; leaving it
    // to the GC that does not exist leaked 954 KB per streamed track.
    std::vector<unsigned char> raw(image, image + size);
    gos_Free(image);
    if (size <= 44) return false;
    if (std::memcmp(raw.data(), "RIFF", 4) != 0 || std::memcmp(raw.data() + 8, "WAVE", 4) != 0)
        return false;

    // Walk the chunk list rather than assuming the canonical 44-byte layout --
    // MC2's music carries a 50-byte fmt chunk with the ADPCM coefficient table.
    std::size_t pos = 12;
    int channels = 0, rate = 0, bits = 0, formatTag = 0, blockAlign = 0, numCoef = 0;
    std::vector<short> coefs;
    while (pos + 8 <= raw.size())
    {
        const char* id = (const char*)raw.data() + pos;
        unsigned long len = 0;
        std::memcpy(&len, raw.data() + pos + 4, 4);
        const std::size_t body = pos + 8;
        if (std::memcmp(id, "fmt ", 4) == 0 && body + 16 <= raw.size())
        {
            unsigned short tag = 0, c = 0, b = 0, align = 0; unsigned long r = 0;
            std::memcpy(&tag,   raw.data() + body +  0, 2);
            std::memcpy(&c,     raw.data() + body +  2, 2);
            std::memcpy(&r,     raw.data() + body +  4, 4);
            std::memcpy(&align, raw.data() + body + 12, 2);
            std::memcpy(&b,     raw.data() + body + 14, 2);
            formatTag = tag; channels = c; rate = (int)r; bits = b; blockAlign = align;

            if (formatTag == 2 && body + 22 <= raw.size()) {
                unsigned short n = 0;
                std::memcpy(&n, raw.data() + body + 20, 2);
                numCoef = n;
                if (body + 22 + (std::size_t)numCoef * 4 <= raw.size()) {
                    coefs.resize((std::size_t)numCoef * 2);
                    std::memcpy(coefs.data(), raw.data() + body + 22, coefs.size() * sizeof(short));
                }
            }
        }
        else if (std::memcmp(id, "data", 4) == 0 && channels && rate)
        {
            const std::size_t avail = raw.size() - body;
            const std::size_t take  = len < avail ? (std::size_t)len : avail;

            if (formatTag == 1) {
                res.owned.assign(raw.begin() + (long)body, raw.begin() + (long)(body + take));
            } else if (formatTag == 2) {
                if (!decodeMsAdpcm(raw.data() + body, take, channels, blockAlign,
                                   coefs.empty() ? nullptr : coefs.data(), numCoef, res.owned))
                    return false;
                bits = 16;              // decoded output is always 16-bit PCM
            } else {
                return false;           // unknown container; stay silent, do not guess
            }

            res.clip.pcm      = res.owned.data();
            res.clip.bytes    = res.owned.size();
            res.clip.channels = channels;
            res.clip.rate     = rate;
            res.clip.bits     = bits;
            return true;
        }
        pos = body + len + (len & 1);
    }
    return false;
}

void startChannel(int ch)
{
    ChannelState& c = g_channel[ch];
    Resource* r = find(c.resource);
    if (!r) return;
    if (!mc2audio::Audio().WantsSamples()) return;
    if (!r->clip.pcm && !r->path.empty()) {
        if (!loadWav(r->path.c_str(), *r)) { r->path.clear(); return; }
    }
    if (!r->clip.pcm) return;
    const bool loop = (c.mode == gosAudio_Loop);
    mc2audio::Audio().Play(ch, r->clip, c.volume, c.pan, loop);
    // Counted HERE and nowhere earlier: this is the only line in the port at
    // which samples are handed to a device. A tally taken at the request sites
    // would count sounds the null backend never emits and sounds dropped for a
    // missing or unreadable resource, which is exactly the confusion the
    // coverage question exists to remove.
    if (ch >= 0 && ch < kMaxChannels) {
        ++census().channelPlays[ch];
        census().channelBytes[ch] += (long long)r->clip.bytes;
    }
    c.started = true;
}

} // namespace

//---------------------------------------------------------------- resources --

void __stdcall gosAudio_CreateResource(HGOSAUDIO* hgosaudio, enum gosAudio_ResourceType type,
                                       const char* file_name, gosAudio_Format* ga_wf,
                                       void* data, int size, bool /*only2D*/)
{
    AudioCensus& cs = census();
    ++cs.createFromFile;
    cs.resourceNames[file_name && *file_name ? file_name : "(unnamed)"]++;
    cs.resourceType[(int)type]++;
    if (ga_wf) {
        char key[64];
        std::snprintf(key, sizeof(key), "ch%d rate%lu bits%d",
                      (int)ga_wf->nChannels, (unsigned long)ga_wf->nSamplesPerSec,
                      (int)ga_wf->wBitsPerSample);
        cs.formats[key]++;
    } else {
        cs.formats["(no format)"]++;
    }
    if (size > 0) {
        cs.totalPcmBytes += size;
        if (size > cs.largestPcmBytes) cs.largestPcmBytes = size;
    }

    if (!hgosaudio) return;
    const intptr_t handle = ++g_nextHandle;
    Resource& res = g_resources[handle];
    res.inUse = true;

    if (data && size > 0 && ga_wf) {
        res.clip.pcm      = (const unsigned char*)data;
        res.clip.bytes    = (std::size_t)size;
        res.clip.channels = ga_wf->nChannels ? ga_wf->nChannels : 1;
        res.clip.rate     = ga_wf->nSamplesPerSec ? (int)ga_wf->nSamplesPerSec : 22050;
        res.clip.bits     = ga_wf->wBitsPerSample ? ga_wf->wBitsPerSample : 16;
    } else if (file_name && *file_name) {
        // PORT: remember the path, decode on first play. Loading here instead
        // cost 4.4 MB of resident PCM in runs that never make a sound, which
        // moved the published 2000-tick memory figures (47,096,997 ->
        // 51,457,385 liveBytes) under the SILENT backend. Audio that is never
        // heard must not cost anything.
        res.path = file_name;
    }

    ++g_liveResources;
    noteLive();
    *hgosaudio = (HGOSAUDIO)handle;
}

void __stdcall gosAudio_CreateResource(HGOSAUDIO* hgosaudio, const char* identifier_name,
                                       HGOSFILE /*file*/, DWORD /*offset*/, bool /*only2D*/)
{
    AudioCensus& cs = census();
    ++cs.createFromHandle;
    cs.resourceNames[identifier_name && *identifier_name ? identifier_name : "(unnamed)"]++;
    if (!hgosaudio) return;
    const intptr_t handle = ++g_nextHandle;
    g_resources[handle].inUse = true;
    ++g_liveResources;
    noteLive();
    *hgosaudio = (HGOSAUDIO)handle;
}

void __stdcall gosAudio_DestroyResource(HGOSAUDIO* hgosaudio)
{
    ++census().destroy;
    if (!hgosaudio || !*hgosaudio) return;
    for (int ch = 0; ch < kMaxChannels; ++ch)
        if (g_channel[ch].resource == *hgosaudio) {
            mc2audio::Audio().Stop(ch);
            g_channel[ch].resource = 0;
            g_channel[ch].started  = false;
            // A channel with nothing on it is STOPPED, and must say so. See the
            // note on AssignResourceToChannel: leaving the retired sound's mode
            // behind makes this channel read as busy forever.
            g_channel[ch].mode     = gosAudio_Stop;
        }
    g_resources.erase((intptr_t)*hgosaudio);
    --g_liveResources;
    *hgosaudio = 0;
}

//----------------------------------------------------------------- channels --

void __stdcall gosAudio_AllocateChannelSliders(int /*Channel*/, DWORD /*properties*/)
{
    ++census().allocateSliders;
}

void __stdcall gosAudio_AssignResourceToChannel(int Channel, HGOSAUDIO hgosaudio)
{
    ++census().assignToChannel;
    if (Channel < 0 || Channel >= kMaxChannels) return;
    mc2audio::Audio().Stop(Channel);
    g_channel[Channel].resource = hgosaudio;
    g_channel[Channel].started  = false;

    // A NEWLY ASSIGNED RESOURCE IS NOT PLAYING, and the channel has to report
    // that. This line is the M8-B4 fix.
    //
    // The mode was left at whatever the PREVIOUS sound finished with, and
    // GetChannelPlayMode can only retire a channel whose `started` is true --
    // which a fresh assign has just cleared -- so it kept handing back the old
    // gosAudio_PlayOnce. MC2 tests exactly that value to decide whether a
    // channel is free: SoundSystem::update starts a stream only
    // `if (!stream3Active && sampleStatus != gosAudio_PlayOnce)`. Reading back a
    // stale PlayOnce made that test false forever, so SetChannelPlayMode was
    // never called again, startChannel never ran, and the assigned resource sat
    // silent.
    //
    // Measured, 2026-08-16: in a full session only the FIRST STREAM3 sound ever
    // reached the device -- the intro, on a channel still at its initial Stop.
    // The mission-select video (node_04, 1,364,637 samples) and the comms video
    // (mc2_04a, 238,237 samples) were both assigned with zero refusals and
    // produced no `play ch=18` line at all. This affects every STREAM3 user, not
    // just movies: in-mission objective voiceovers go through the same path.
    g_channel[Channel].mode = gosAudio_Stop;
}

void __stdcall gosAudio_SetChannelSlider(int Channel, enum gosAudio_Properties property,
                                         float v1, float /*v2*/, float /*v3*/)
{
    ++census().setSlider;
    census().sliderProperty[(int)property]++;
    if (censusEnabled())
        std::printf("[audio-trace] setSlider ch=%d prop=%d v=%.4f\n", Channel, (int)property, v1);
    if (Channel < 0 || Channel >= kMaxChannels) return;
    if (property == gosAudio_Volume) {
        g_channel[Channel].volume = v1;
        mc2audio::Audio().SetVolume(Channel, v1);
    } else if (property == gosAudio_Panning) {
        g_channel[Channel].pan = v1;
        mc2audio::Audio().SetPan(Channel, v1);
    }
}

void __stdcall gosAudio_GetChannelSlider(int Channel, enum gosAudio_Properties property,
                                         float* v1, float* v2, float* v3)
{
    ++census().getSlider;
    // Unchanged from the silent stub: report zeroes. The game writes these and
    // does not depend on reading its own values back.
    (void)Channel; (void)property;
    if (v1) *v1 = 0.0f;
    if (v2) *v2 = 0.0f;
    if (v3) *v3 = 0.0f;
}

void __stdcall gosAudio_SetChannelPlayMode(int Channel, enum gosAudio_PlayMode ga_pm)
{
    ++census().setPlayMode;
    if (ga_pm == gosAudio_Stop) ++census().stopRequests;
    else                        ++census().playRequests;

    if (Channel < 0 || Channel >= kMaxChannels) return;
    g_channel[Channel].mode = ga_pm;

    switch (ga_pm)
    {
        case gosAudio_PlayOnce:
        case gosAudio_Loop:
            startChannel(Channel);
            break;
        case gosAudio_Stop:
            mc2audio::Audio().Stop(Channel);
            g_channel[Channel].started = false;
            break;
        case gosAudio_Pause:
            mc2audio::Audio().Stop(Channel);   // waveOut has no true pause here
            break;
        case gosAudio_Continue:
            if (!g_channel[Channel].started) startChannel(Channel);
            break;
        default:
            break;
    }
}

gosAudio_PlayMode __stdcall gosAudio_GetChannelPlayMode(int Channel)
{
    ++census().getPlayMode;
    if (Channel < 0 || Channel >= kMaxChannels) return gosAudio_Stop;

    ChannelState& c = g_channel[Channel];
    // Only a backend that actually plays can retire a channel. Null never does,
    // so the silent path keeps returning exactly what the game last wrote --
    // which is what keeps the strict regression byte-identical.
    //
    // PORT: retire ANY finished non-looping channel, not only one whose mode is
    // still literally gosAudio_PlayOnce.
    //
    // Owner report 2026-08-10: LRM fire quietens other sounds. Measured in that
    // session -- 596 plays, median clip 1362 ms, and 18 runs of three or more
    // back-to-back plays on the SAME channel, longest twelve on channel 1.
    // Play() stops whatever is on a channel before starting, so every one of
    // those cut a sound that was still going.
    //
    // MC2 finds a free channel by asking here. A channel that has finished but
    // still reads as busy is one MC2 will not choose, so it crowds onto the few
    // it believes are free and cuts them. Telling the truth for every
    // non-looping mode hands back the channels it already has.
    //
    // gosAudio_Loop is excluded deliberately: a loop between buffers is not
    // finished, and retiring it would silence the music.
    const bool nonLooping = (c.mode != gosAudio_Loop) && (c.mode != gosAudio_Stop);
    if (c.started && nonLooping && !mc2audio::Audio().IsPlaying(Channel)) {
        c.mode    = gosAudio_Stop;
        c.started = false;
    }
    return c.mode;
}

//-------------------------------------------------------------------- census --

void gosAudioNull_DumpStats()
{
    if (!censusEnabled()) return;

    const AudioCensus& c = census();
    std::printf("[audio-census] backend=%s\n", mc2audio::Audio().Name());
    std::printf("[audio-census] createFromFile=%ld createFromHandle=%ld destroy=%ld\n",
                c.createFromFile, c.createFromHandle, c.destroy);
    std::printf("[audio-census] allocateSliders=%ld assignToChannel=%ld setSlider=%ld getSlider=%ld\n",
                c.allocateSliders, c.assignToChannel, c.setSlider, c.getSlider);
    std::printf("[audio-census] setPlayMode=%ld (play=%ld stop=%ld) getPlayMode=%ld\n",
                c.setPlayMode, c.playRequests, c.stopRequests, c.getPlayMode);
    std::printf("[audio-census] liveResources=%d peakLive=%ld issued=%d distinctNames=%d\n",
                g_liveResources, c.peakLiveResources, g_nextHandle, (int)c.resourceNames.size());
    std::printf("[audio-census] pcmBytesTotal=%lld largestPcm=%d\n",
                c.totalPcmBytes, c.largestPcmBytes);
    for (std::map<int, long>::const_iterator it = c.resourceType.begin(); it != c.resourceType.end(); ++it)
        std::printf("[audio-census] resourceType[%d]=%ld\n", it->first, it->second);
    for (std::map<std::string, long>::const_iterator it = c.formats.begin(); it != c.formats.end(); ++it)
        std::printf("[audio-census] format '%s' x%ld\n", it->first.c_str(), it->second);
    for (std::map<int, long>::const_iterator it = c.sliderProperty.begin(); it != c.sliderProperty.end(); ++it)
        std::printf("[audio-census] sliderProperty[%d]=%ld\n", it->first, it->second);
    for (std::map<std::string, long>::const_iterator it = c.resourceNames.begin(); it != c.resourceNames.end(); ++it)
        std::printf("[audio-census] resource '%s' x%ld\n", it->first.c_str(), it->second);

    // M8-2 coverage tally. Read this against the family table in
    // docs/gpt-tasks/M8-2-AUDIO-GAP.md. ZERO IS A RESULT, not a missing line.
    long totalPlays = 0;
    for (int ch = 0; ch < kMaxChannels; ++ch) totalPlays += c.channelPlays[ch];
    std::printf("[audio-family] ---- what reached the device (%ld plays total) ----\n", totalPlays);
    const int familyCount = (int)(sizeof(kFamilies) / sizeof(kFamilies[0]));
    for (int f = 0; f < familyCount; ++f)
    {
        const FamilyRow& row = kFamilies[f];
        long      plays = 0;
        long long bytes = 0;
        for (int ch = row.lo; ch <= row.hi && ch < kMaxChannels; ++ch) {
            plays += c.channelPlays[ch];
            bytes += c.channelBytes[ch];
        }
        std::printf("[audio-family] %-28s ch %2d-%-2d  plays=%-6ld bytes=%-12lld %s\n",
                    row.name, row.lo, row.hi, plays, bytes,
                    plays ? "" : "<-- NOTHING EVER PLAYED");
    }
    // The null backend never reaches the Play() this tally counts, so a silent
    // run reports every family at zero. That is correct and is not a finding.
    if (!mc2audio::Audio().WantsSamples())
        std::printf("[audio-family] backend emits no samples -- all-zero above is expected, not a result\n");

    mc2audio::Audio().DumpStats();
}
