//===========================================================================//
// Win32 audio backend -- waveOut.
//
// winmm is already linked (the timer shims use it), so this adds no dependency
// and does not reimplement DirectSound, which M6-4 explicitly rules out.
//
// One waveOut device per channel. That sounds profligate, but the census says
// the game allocates 20 channels and the mixer is expected to be the OS's, so
// the channel model maps 1:1. The gain does NOT map that way -- see the volume
// note below; that part is ours to do.
//
// Formats to support come from the census, not the header: mono/stereo,
// 8- or 16-bit, 11025/22050/44100. waveOut handles all of them natively, so
// there is no resampling and no conversion path to get wrong.
//
// PROVING IT WITHOUT EARS. MC2_AUDIO_CAPTURE=<path.wav> writes every buffer
// this backend hands to the device into a WAV file, mixed down to one stream.
// That turns "audio plays" from something only a human in the room can confirm
// into a file with a measurable sample count and peak amplitude. It is also how
// this was verified at all, since the agent doing the work cannot hear.
//
// VOLUME IS APPLIED IN SOFTWARE, NOT BY THE DEVICE, and that is a measured
// decision. waveOutSetVolume is documented as per-instance when given a handle,
// but the probe in Open() showed it is not: setting one handle quiet moved a
// SECOND handle's volume to the same value. With one device per voice, every
// per-channel volume MC2 sets -- distance attenuation, music fades -- therefore
// changed the volume of EVERY sound at once. That is what the owner reported on
// 2026-08-10 as other audio quietening when certain weapons fire.
//
// So each voice keeps its unscaled source and submits a scaled copy. MC2 ramps
// volume DURING playback, so a change mid-sound re-derives the remainder from
// the source at the current play cursor and resubmits. Deriving from the source
// rather than rescaling the scaled buffer is what lets a ramp that starts at
// gain 0.0, as the music fade does, come back up at all.
//
// The capture records what was SENT, gain included -- what a listener hears.
//===========================================================================//

#include "audio/audio_backend.h"

#include <windows.h>
#include <mmsystem.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace mc2audio {

namespace {

const int kMaxChannels = 64;

// ---------------------------------------------------------------- capture ---
// Writes a 16-bit stereo 44100 WAV. Buffers arrive in whatever format the game
// used, so they are converted on the way in; this file is evidence, not output.
class CaptureWriter
{
public:
    void Open(const char* path)
    {
        m_file = std::fopen(path, "wb");
        if (!m_file) return;
        unsigned char header[44] = {};
        std::fwrite(header, 1, sizeof(header), m_file);   // patched in Close()
        m_samples = 0;
        m_peak    = 0;
    }

    bool IsOpen() const { return m_file != nullptr; }

    void Append(const Clip& clip)
    {
        if (!m_file || !clip.pcm || clip.bytes == 0) return;

        const int      bytesPerSample = clip.bits / 8;
        const int      frameBytes     = bytesPerSample * clip.channels;
        if (frameBytes <= 0) return;
        const std::size_t frames = clip.bytes / frameBytes;

        for (std::size_t f = 0; f < frames; ++f)
        {
            // Nearest-neighbour rate conversion is fine for evidence.
            const std::size_t srcFrame = clip.rate == 44100
                ? f
                : (std::size_t)((double)f * 44100.0 / (double)clip.rate);
            if (srcFrame >= frames) break;

            for (int c = 0; c < 2; ++c)
            {
                const int srcChan = (clip.channels == 2) ? c : 0;
                const unsigned char* p =
                    clip.pcm + srcFrame * frameBytes + srcChan * bytesPerSample;
                int sample;
                if (clip.bits == 8) sample = ((int)*p - 128) << 8;   // 8-bit is unsigned
                else                sample = (int)*(const short*)p;
                if (sample >  32767) sample =  32767;
                if (sample < -32768) sample = -32768;
                const int mag = sample < 0 ? -sample : sample;
                if (mag > m_peak) m_peak = mag;
                const short out = (short)sample;
                std::fwrite(&out, sizeof(out), 1, m_file);
            }
            ++m_samples;
        }
    }

    void Close()
    {
        if (!m_file) return;
        const unsigned long dataBytes = (unsigned long)(m_samples * 2 * 2);
        unsigned char h[44];
        std::memcpy(h + 0,  "RIFF", 4);
        const unsigned long riff = 36 + dataBytes;
        std::memcpy(h + 4,  &riff, 4);
        std::memcpy(h + 8,  "WAVEfmt ", 8);
        const unsigned long fmtLen = 16; const unsigned short pcm = 1, ch = 2;
        const unsigned long rate = 44100, byteRate = 44100 * 4;
        const unsigned short align = 4, bits = 16;
        std::memcpy(h + 16, &fmtLen, 4); std::memcpy(h + 20, &pcm, 2);
        std::memcpy(h + 22, &ch, 2);     std::memcpy(h + 24, &rate, 4);
        std::memcpy(h + 28, &byteRate, 4); std::memcpy(h + 32, &align, 2);
        std::memcpy(h + 34, &bits, 2);
        std::memcpy(h + 36, "data", 4);  std::memcpy(h + 40, &dataBytes, 4);
        std::fseek(m_file, 0, SEEK_SET);
        std::fwrite(h, 1, sizeof(h), m_file);
        std::fclose(m_file);
        m_file = nullptr;
        std::printf("[audio-win32] capture: %llu frames, peak amplitude %d/32767\n",
                    (unsigned long long)m_samples, m_peak);
    }

    unsigned long long Frames() const { return m_samples; }
    int                Peak()   const { return m_peak; }

private:
    std::FILE*         m_file    = nullptr;
    unsigned long long m_samples = 0;
    int                m_peak    = 0;
};

// ------------------------------------------------------------------ device --
struct Voice
{
    HWAVEOUT   device = nullptr;
    WAVEHDR    header = {};
    std::vector<unsigned char> pcm;    // what is submitted: src with gain applied
    // PORT: the unscaled source is kept so a mid-play volume change can be
    // re-derived rather than compounded. MC2 ramps volume while a sound plays --
    // music starts at 0.0 and fades up -- so scaling the already-scaled buffer
    // would both lose precision and be unable to come back from a gain of zero.
    std::vector<unsigned char> src;
    bool       active = false;
    bool       loop   = false;
    int        bits   = 16;
    int        chans  = 1;
    // PORT: absolute offset into src of the buffer currently submitted.
    // waveOutGetPosition restarts at zero after every waveOutReset, so a
    // resubmit's position is relative to the LAST resubmit, not to the whole
    // sound. Without accumulating it, each volume change re-derived the buffer
    // from offset zero and the music began again -- measured as the submitted
    // size climbing back to the full clip length across a fade.
    std::size_t srcOffset = 0;
    float      gainL  = 1.0f;
    float      gainR  = 1.0f;
    // PORT (M8-B31): the gain the CURRENTLY SUBMITTED buffer was scaled
    // with. ApplyVolume compares against this so a ramp does not reset the
    // device once per step. -1 means "nothing submitted yet".
    float      submittedGainL = -1.0f;
    float      submittedGainR = -1.0f;
};

class Win32AudioBackend : public AudioBackend
{
public:
    const char* Name() const override { return "win32"; }

    bool Open() override
    {
        if (waveOutGetNumDevs() == 0) {
            std::printf("[audio-win32] no waveOut device present; staying silent\n");
            return false;
        }

        // PORT: per-stream volume is an OPTIONAL waveOut capability, and this
        // peer relies on it for every sound: one handle per voice, each with its
        // own waveOutSetVolume. If WAVECAPS_VOLUME is absent, that call has
        // nowhere per-voice to land and the driver may apply it more widely --
        // which is the leading explanation for the owner's report that LRM fire
        // quietens other sounds. Printed once, at open, so the answer is in
        // every census log rather than needing a special run.
        if (const char* c = std::getenv("MC2_AUDIO_CENSUS")) if (*c && std::strcmp(c, "0") != 0) {
            WAVEOUTCAPSA caps = {};
            const MMRESULT cr = waveOutGetDevCapsA((UINT_PTR)WAVE_MAPPER, &caps, sizeof(caps));
            if (cr == MMSYSERR_NOERROR) {
                std::fprintf(stderr,
                    "[audio-win32] device '%s' devices=%u caps=0x%08lX volume=%s lrVolume=%s\n",
                    caps.szPname, (unsigned)waveOutGetNumDevs(), (unsigned long)caps.dwSupport,
                    (caps.dwSupport & WAVECAPS_VOLUME)   ? "YES" : "NO  <== no per-stream volume",
                    (caps.dwSupport & WAVECAPS_LRVOLUME) ? "YES" : "NO");
            } else {
                std::fprintf(stderr, "[audio-win32] waveOutGetDevCaps failed mm=%u\n", (unsigned)cr);
            }
            std::fflush(stderr);
        }
        // PORT: does per-voice volume actually stay per-voice? (M7 B6)
        //
        // Owner report 2026-08-10, refined: it is not only LRMs -- "a lot of
        // sounds are quieting/changing when certain weapons are fired". Many
        // sounds changing at once is what a volume set that is NOT per-stream
        // looks like, and this peer sets volume per voice on every play.
        //
        // WAVECAPS_VOLUME being present is NOT proof of isolation. waveOut is
        // documented as per-instance when given a HANDLE, but on modern Windows
        // it is emulated over a shared per-process audio session, and a driver
        // may honour the call at a level wider than one stream.
        //
        // So measure it instead of arguing about it: open two handles, set one
        // quiet, and read the OTHER back. If the other moved, volume leaks
        // across voices and the fix is to scale samples in software rather than
        // ask the device. MC2_AUDIO_VOLUME_PROBE=1, silent otherwise.
        if (const char* probe = std::getenv("MC2_AUDIO_VOLUME_PROBE"))
        if (*probe && std::strcmp(probe, "0") != 0)
        {
            WAVEFORMATEX wf = {};
            wf.wFormatTag = WAVE_FORMAT_PCM; wf.nChannels = 1;
            wf.nSamplesPerSec = 22050; wf.wBitsPerSample = 16;
            wf.nBlockAlign = 2; wf.nAvgBytesPerSec = 44100;

            HWAVEOUT a = nullptr, b = nullptr;
            const MMRESULT ra = waveOutOpen(&a, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL);
            const MMRESULT rb = waveOutOpen(&b, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL);
            if (ra == MMSYSERR_NOERROR && rb == MMSYSERR_NOERROR)
            {
                waveOutSetVolume(a, 0xFFFFFFFF);
                waveOutSetVolume(b, 0xFFFFFFFF);
                DWORD before = 0; waveOutGetVolume(b, &before);
                waveOutSetVolume(a, 0x20002000);          // quieten A only
                DWORD after = 0;  waveOutGetVolume(b, &after);
                std::fprintf(stderr,
                    "[audio-win32] volume probe: set A=0x20002000, B was 0x%08lX now 0x%08lX -- %s\n",
                    (unsigned long)before, (unsigned long)after,
                    before == after ? "per-voice, isolated"
                                    : "*** VOLUME LEAKS ACROSS VOICES ***");
                std::fflush(stderr);
            }
            else
            {
                std::fprintf(stderr, "[audio-win32] volume probe could not open two devices (%u/%u)\n",
                             (unsigned)ra, (unsigned)rb);
            }
            if (a) waveOutClose(a);
            if (b) waveOutClose(b);
        }

        if (const char* cap = std::getenv("MC2_AUDIO_CAPTURE"))
            if (*cap) m_capture.Open(cap);
        return true;
    }

    void Close() override
    {
        for (int i = 0; i < kMaxChannels; ++i) Stop(i);
        m_capture.Close();
    }

    void Play(int channel, const Clip& clip, float volume, float pan, bool loop) override
    {
        if (channel < 0 || channel >= kMaxChannels) return;
        if (!clip.pcm || clip.bytes == 0) return;

        // PORT: a Play() onto a channel that is STILL SOUNDING cuts it -- Stop()
        // below does waveOutReset and closes the device. That is the one event
        // in this peer that makes an already-audible sound stop early, so it is
        // the thing to count for the owner's report (2026-08-10) that LRM fire
        // quietens other sounds. Counted always, printed only under census: the
        // count is what tells us whether a salvo is stealing or not.
        const bool stole = IsPlaying(channel);
        if (stole) {
            ++m_steals;
            if (const char* c = std::getenv("MC2_AUDIO_CENSUS")) if (*c && std::strcmp(c, "0") != 0) {
                std::fprintf(stderr, "[audio-win32] STEAL ch=%d cut a sound still playing"
                                     " (steals=%ld of %ld plays)\n",
                             channel, m_steals, m_played);
                std::fflush(stderr);
            }
        }

        Stop(channel);
        Voice& v = m_voice[channel];

        WAVEFORMATEX wf = {};
        wf.wFormatTag      = WAVE_FORMAT_PCM;
        wf.nChannels       = (WORD)clip.channels;
        wf.nSamplesPerSec  = (DWORD)clip.rate;
        wf.wBitsPerSample  = (WORD)clip.bits;
        wf.nBlockAlign     = (WORD)(clip.channels * clip.bits / 8);
        wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;

        const MMRESULT openResult = waveOutOpen(&v.device, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL);
        if (openResult != MMSYSERR_NOERROR) {
            v.device = nullptr;
            ++m_openFailures;
            // PORT: this used to be counted and nothing else, so a dropped sound
            // was indistinguishable from one that played. Every voice here opens
            // its OWN waveOut device, and a driver out of handles fails exactly
            // this call -- which presents to the player as other audio going
            // quiet the moment a salvo starts. Owner report, 2026-08-10: LRM and
            // some other weapons mute other sounds.
            //
            // MMSYSERR_ALLOCATED is the code that means "no handles left". Print
            // it rather than infer it: the alternative causes (format the driver
            // will not take, a rate no device supports) have their own codes and
            // would send the search somewhere completely different.
            if (const char* c = std::getenv("MC2_AUDIO_CENSUS")) if (*c && std::strcmp(c, "0") != 0) {
                int live = 0;
                for (int i = 0; i < kMaxChannels; ++i) if (m_voice[i].device) ++live;
                std::fprintf(stderr,
                    "[audio-win32] OPEN FAILED ch=%d mm=%u%s liveDevices=%d rate=%d bits=%d chans=%d"
                    "   *** SOUND DROPPED ***\n",
                    channel, (unsigned)openResult,
                    openResult == MMSYSERR_ALLOCATED ? " (MMSYSERR_ALLOCATED, out of device handles)" : "",
                    live, clip.rate, clip.bits, clip.channels);
                std::fflush(stderr);
            }
            return;
        }

        v.src.assign(clip.pcm, clip.pcm + clip.bytes);
        v.bits  = clip.bits;
        v.chans = clip.channels;
        // PORT: the gain has to be known BEFORE the buffer is scaled, so the
        // channel volume/pan are recorded here rather than through SetVolume
        // after the header is prepared -- which is where they used to be set,
        // back when the device applied the gain and the order did not matter.
        if (volume < 0.0f) volume = 0.0f;
        if (volume > 1.0f) volume = 1.0f;
        if (pan < -1.0f)   pan = -1.0f;
        if (pan >  1.0f)   pan =  1.0f;
        m_volume[channel] = volume;
        m_pan[channel]    = pan;
        ++m_volumeWrites[channel];
        if (volume > m_volumePeak[channel]) m_volumePeak[channel] = volume;

        v.srcOffset = 0;
        ScaleIntoSubmitBuffer(v, channel, 0);
        v.submittedGainL = v.gainL;
        v.submittedGainR = v.gainR;
        v.header = WAVEHDR{};
        v.header.lpData         = (LPSTR)v.pcm.data();
        v.header.dwBufferLength = (DWORD)v.pcm.size();
        v.header.dwLoops        = loop ? 0xFFFFFFFFu : 1u;
        v.header.dwFlags        = loop ? WHDR_BEGINLOOP | WHDR_ENDLOOP : 0;

        if (waveOutPrepareHeader(v.device, &v.header, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
            waveOutClose(v.device); v.device = nullptr; return;
        }
        if (waveOutWrite(v.device, &v.header, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
            waveOutUnprepareHeader(v.device, &v.header, sizeof(WAVEHDR));
            waveOutClose(v.device); v.device = nullptr; return;
        }
        v.active = true;
        v.loop   = loop;
        ++m_played;

        if (const char* c = std::getenv("MC2_AUDIO_CENSUS")) if (*c && std::strcmp(c, "0") != 0) {
            int peak = 0;
            const int step = clip.bits == 16 ? 2 : 1;
            for (std::size_t i = 0; i + step <= clip.bytes; i += (std::size_t)step * 64) {
                const int s = clip.bits == 16 ? (int)*(const short*)(clip.pcm + i)
                                              : (((int)clip.pcm[i] - 128) << 8);
                const int a = s < 0 ? -s : s;
                if (a > peak) peak = a;
            }
            // PORT: stderr, not stdout -- every other census in this port writes
            // to stderr, stdout is the stream the strict bar hashes, and a
            // playtest should leave ONE file to send back rather than two.
            //
            // liveDevices is the number that matters for the salvo report: this
            // peer opens one waveOut device PER VOICE, so it is the quantity
            // that runs out.
            int live = 0;
            for (int i = 0; i < kMaxChannels; ++i) if (m_voice[i].device) ++live;
            std::fprintf(stderr,
                        "[audio-win32] play ch=%d liveDevices=%d bytes=%llu chans=%d rate=%d bits=%d vol=%.4f pan=%.2f srcPeak=%d\n",
                        channel, live, (unsigned long long)clip.bytes, clip.channels,
                        clip.rate, clip.bits, volume, pan, peak);
        }

        // PORT: capture what was SUBMITTED, which now carries the gain.
        Clip sent = clip;
        sent.pcm   = v.pcm.data();
        sent.bytes = v.pcm.size();
        m_capture.Append(sent);
    }

    void Stop(int channel) override
    {
        if (channel < 0 || channel >= kMaxChannels) return;
        Voice& v = m_voice[channel];
        if (!v.device) return;
        waveOutReset(v.device);
        waveOutUnprepareHeader(v.device, &v.header, sizeof(WAVEHDR));
        waveOutClose(v.device);
        v.device = nullptr;
        v.active = false;
        v.pcm.clear();
    }

    void SetVolume(int channel, float volume) override
    {
        if (channel < 0 || channel >= kMaxChannels) return;
        Voice& v = m_voice[channel];
        if (!v.device) return;
        if (volume < 0.0f) volume = 0.0f;
        if (volume > 1.0f) volume = 1.0f;
        m_volume[channel] = volume;
        if (volume > m_volumePeak[channel]) m_volumePeak[channel] = volume;
        ++m_volumeWrites[channel];
        ApplyVolume(channel);
    }

    void SetPan(int channel, float pan) override
    {
        if (channel < 0 || channel >= kMaxChannels) return;
        if (pan < -1.0f) pan = -1.0f;
        if (pan >  1.0f) pan =  1.0f;
        m_pan[channel] = pan;
        ApplyVolume(channel);
    }

    bool IsPlaying(int channel) const override
    {
        if (channel < 0 || channel >= kMaxChannels) return false;
        const Voice& v = m_voice[channel];
        if (!v.device || !v.active) return false;
        return (v.header.dwFlags & WHDR_DONE) == 0;
    }

    void DumpStats() const override
    {
        std::printf("[audio-win32] buffers played=%ld openFailures=%ld capture=%s\n",
                    m_played, m_openFailures, m_capture.IsOpen() ? "on" : "off");
        // The volume envelope, because MC2 ramps it while a sound plays and a
        // single number taken at Play() is not the truth about what was heard.
        for (int i = 0; i < kMaxChannels; ++i)
            if (m_volumeWrites[i])
                std::printf("[audio-win32] channel %d volume: %ld writes, peak %.4f\n",
                            i, m_volumeWrites[i], m_volumePeak[i]);
    }

private:
    // PORT: fill v.pcm from v.src with the channel's gain applied, starting at
    // byteOffset (0 for a fresh play, the play cursor for a mid-play change).
    //
    // This replaces waveOutSetVolume, which was MEASURED to leak across voices
    // on this machine -- see the probe in Open(). Doing the gain here is the
    // only way to make one sound quiet without quietening every other sound,
    // and it is cheap: one multiply per sample on buffers we already copy.
    void ScaleIntoSubmitBuffer(Voice& v, int channel, std::size_t byteOffset)
    {
        const float vol = m_volume[channel];
        const float pan = m_pan[channel];
        v.gainL = vol * (pan > 0.0f ? 1.0f - pan : 1.0f);
        v.gainR = vol * (pan < 0.0f ? 1.0f + pan : 1.0f);

        if (byteOffset > v.src.size()) byteOffset = v.src.size();
        const std::size_t bytes = v.src.size() - byteOffset;
        v.pcm.resize(bytes);
        if (!bytes) return;

        const unsigned char* in = v.src.data() + byteOffset;

        if (v.bits == 16)
        {
            const short* s = reinterpret_cast<const short*>(in);
            short*       d = reinterpret_cast<short*>(v.pcm.data());
            const std::size_t samples = bytes / 2;
            for (std::size_t i = 0; i < samples; ++i)
            {
                // Mono uses the left gain; stereo alternates L,R.
                const float g = (v.chans == 2 && (i & 1)) ? v.gainR : v.gainL;
                int out = (int)(s[i] * g);
                if (out >  32767) out =  32767;
                if (out < -32768) out = -32768;
                d[i] = (short)out;
            }
        }
        else
        {
            // 8-bit waveOut PCM is UNSIGNED, centred on 128, so the gain has to
            // be applied to the signed excursion or quiet sounds drift to zero
            // instead of to silence.
            for (std::size_t i = 0; i < bytes; ++i)
            {
                const float g = (v.chans == 2 && (i & 1)) ? v.gainR : v.gainL;
                int out = (int)(((int)in[i] - 128) * g) + 128;
                if (out > 255) out = 255;
                if (out < 0)   out = 0;
                v.pcm[i] = (unsigned char)out;
            }
        }
    }

    void ApplyVolume(int channel)
    {
        Voice& v = m_voice[channel];
        if (!v.device) return;

        // PORT: a volume change while the voice is playing. MC2 does this
        // constantly -- music fades in over ~60 ramp writes, and SFX are
        // distance-attenuated as units move. Re-derive the remainder from the
        // unscaled source at the current play cursor and resubmit it. Deriving
        // from src rather than rescaling v.pcm is what makes a ramp starting at
        // gain 0.0 able to come back up at all.
        if (!v.active || v.src.empty()) return;

        // PORT (M8-B31): DO NOT RESET THE DEVICE FOR EVERY RAMP STEP.
        //
        // Owner 2026-08-29: "sound stuttering ... when either loading into
        // missions or opening a save file and arriving on the mission
        // selection screen where the video briefing plays". Those are exactly
        // the moments music cross-fades.
        //
        // The resubmit below calls waveOutReset, which STOPS the device and
        // flushes what it had queued. Doing that mid-sound is an audible
        // discontinuity, and the comment above says MC2 ramps volume over
        // ~60 writes per fade.
        //
        // MEASURED, playtest-20260829-140911.err.log, one session:
        //     770 resets total
        //     517 on channel 16 and 249 on channel 17 -- the two MUSIC
        //       streams. Four on everything else combined.
        //     longest unbroken run: 46 resets back to back, one fade.
        //
        // AND THE COMPETING EXPLANATION IS RULED OUT: MC2_SLOW_FRAME_MS=20 was
        // armed for that whole session and logged ZERO slow frames, so the
        // game never missed its frame budget. The stutter is not the game
        // hitching.
        //
        // So skip the resubmit when the change is too small to hear. A fade
        // still rises -- it just steps a handful of times instead of sixty.
        // Silence is exempt: a fade OUT must land exactly on zero or the
        // music would sit quietly under the next screen forever.
        //
        // This REDUCES the resets; it does not remove them. The real answer
        // is one mixed device with a short submit granularity, which is a
        // redesign of this backend and is on the review list.
        // Overridable so the SAME binary can be run with the old behaviour
        // (0 = resubmit on every change) and the new one, which is the only
        // way to attribute a change in the reset count to this and nothing
        // else. It also turns the whole thing off if it ever misbehaves.
        static float kGainEpsilon = -1.0f;
        if (kGainEpsilon < 0.0f)
        {
            kGainEpsilon = 0.05f;
            if (const char* e = std::getenv("MC2_AUDIO_RAMP_EPSILON"))
                if (*e) kGainEpsilon = (float)atof(e);
        }
        // PORT (M8-B37): COMPARE THE REQUESTED GAIN, NOT THE SUBMITTED GAIN
        // AGAINST ITSELF.
        //
        // v.gainL is assigned in exactly one place -- ScaleIntoSubmitBuffer,
        // which runs BELOW this test -- so at this point it still holds the gain
        // the currently submitted buffer was scaled with, which is precisely
        // what submittedGainL holds. The difference was therefore ALWAYS ZERO,
        // and every volume change after the first returned here for the life of
        // the sound. The epsilon was doing far more than it was meant to: it was
        // not damping a ramp, it was ending it.
        //
        // MEASURED, playtest-20260902-001157: 41 music plays and exactly 41
        // resubmits, one each, every one landing at gain 0.0002-0.0056 early in
        // the fade-in and never rising again. Peak sample reaching the device
        // was 4-162 of 32767 while the source peaked at 29,000-31,000 -- about
        // a thousandth of full scale. That is the owner's "I am not hearing the
        // background music" while the sound effects still played.
        //
        // The sliders were checked FIRST and are innocent: DigitalMasterVolume
        // 102 and MusicVolume 128 give 0.400 * 0.500 = 0.200, and the channel's
        // requested volume peaks at 0.1992. The request was right the whole way
        // down; only the comparison was wrong.
        //
        // The epsilon itself stays, and so does its benefit -- a 0 to 0.2 fade
        // now resubmits about four times instead of sixty.
        const float wantVol = m_volume[channel];
        const float wantPan = m_pan[channel];
        const float wantGainL = wantVol * (wantPan > 0.0f ? 1.0f - wantPan : 1.0f);
        const float wantGainR = wantVol * (wantPan < 0.0f ? 1.0f + wantPan : 1.0f);

        const bool goingSilent = (wantGainL <= 0.0001f && wantGainR <= 0.0001f);
        if (!goingSilent && v.submittedGainL >= 0.0f &&
            fabsf(wantGainL - v.submittedGainL) < kGainEpsilon &&
            fabsf(wantGainR - v.submittedGainR) < kGainEpsilon)
            return;

        MMTIME t = {};
        t.wType = TIME_BYTES;
        waveOutGetPosition(v.device, &t, sizeof(t));
        std::size_t played = (t.wType == TIME_BYTES) ? (std::size_t)t.u.cb : 0;

        // Land on a frame boundary or a stereo resubmit swaps left and right
        // for the rest of the sound.
        const std::size_t frame = (std::size_t)(v.bits / 8) * (std::size_t)v.chans;
        if (frame) played -= played % frame;
        v.srcOffset += played;

        waveOutReset(v.device);
        waveOutUnprepareHeader(v.device, &v.header, sizeof(WAVEHDR));

        ScaleIntoSubmitBuffer(v, channel, v.srcOffset);
        v.submittedGainL = v.gainL;
        v.submittedGainR = v.gainR;
        if (v.pcm.empty()) { v.active = false; return; }

        v.header = WAVEHDR{};
        v.header.lpData         = (LPSTR)v.pcm.data();
        v.header.dwBufferLength = (DWORD)v.pcm.size();
        v.header.dwLoops        = v.loop ? 0xFFFFFFFFu : 1u;
        v.header.dwFlags        = v.loop ? WHDR_BEGINLOOP | WHDR_ENDLOOP : 0;
        if (waveOutPrepareHeader(v.device, &v.header, sizeof(WAVEHDR)) == MMSYSERR_NOERROR)
            waveOutWrite(v.device, &v.header, sizeof(WAVEHDR));

        // PORT: the capture file only records what Play() submits, so it cannot
        // see a resubmit -- which made the music fade read as silent when it was
        // simply not being captured. Report the peak of what actually went out, so
        // "the ramp reached 0.1594" can be checked against "and the samples were
        // therefore audible" rather than assumed.
        if (const char* c = std::getenv("MC2_AUDIO_CENSUS")) if (*c && std::strcmp(c, "0") != 0) {
            int peak = 0;
            const int step = v.bits == 16 ? 2 : 1;
            for (std::size_t i = 0; i + step <= v.pcm.size(); i += (std::size_t)step * 64) {
                const int sample = v.bits == 16 ? (int)*(const short*)(v.pcm.data() + i)
                                               : (((int)v.pcm[i] - 128) << 8);
                const int a = sample < 0 ? -sample : sample;
                if (a > peak) peak = a;
            }
            std::fprintf(stderr, "[audio-win32] regain ch=%d gainL=%.4f resubmitPeak=%d bytes=%zu\n",
                         channel, v.gainL, peak, v.pcm.size());
            std::fflush(stderr);
        }
    }

    Voice         m_voice[kMaxChannels];
    float         m_volume[kMaxChannels]      = {};
    float         m_volumePeak[kMaxChannels]  = {};
    long          m_volumeWrites[kMaxChannels] = {};
    float         m_pan[kMaxChannels]         = {};
    CaptureWriter m_capture;
    long          m_played       = 0;
    long          m_steals = 0;
    long          m_openFailures = 0;
};

} // namespace

AudioBackend* MakeWin32AudioBackend()
{
    static Win32AudioBackend backend;
    return &backend;
}

} // namespace mc2audio
