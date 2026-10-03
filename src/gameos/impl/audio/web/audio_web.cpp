//===========================================================================//
// Web Audio peer for the M6-4 audio seam.
//
// The third implementation of mc2audio::AudioBackend, next to null/ and
// win32/. Everything portable is already above the seam -- WAV parsing, the
// MS-ADPCM decoder, clip lifetime, channel bookkeeping and the play-mode the
// game polls 101,380 times per 6000 ticks -- so this file only has to put PCM
// on a device and answer "is it still going".
//
// WHY THIS IS SHORTER THAN THE WIN32 PEER, AND WHY THAT IS NOT AN OVERSIGHT.
//
// audio_win32.cpp carries about 400 lines that exist only to work around
// waveOut: one device per voice because waveOutSetVolume was MEASURED to leak
// across handles (M7-B6), gain applied per sample in software because of it,
// and a resubmit-from-the-play-cursor dance so a mid-sound volume ramp can
// still rise -- which is where M8-B37's silent music came from. The Web Audio
// API has a real mixer: a GainNode per channel is per-voice by construction and
// takes a value change while the source is playing. None of that machinery is
// needed here, and reproducing it would be reproducing a workaround for a
// problem this platform does not have.
//
// AUTOPLAY. A browser AudioContext starts suspended until the page has had a
// user gesture. That is not an error and must not be treated as one: Open()
// still succeeds, sounds submitted while suspended are simply not heard, and
// the shell calls mc2_web_audio_resume() on the first click or keypress. The
// seam's contract already says audio is never allowed to be fatal.
//===========================================================================//

#include "audio_backend.h"

#include <cstdio>
#include <emscripten.h>

namespace mc2audio {

namespace {

// ---------------------------------------------------------------------------
// The JS side. One AudioContext, one GainNode+StereoPannerNode pair per channel
// created on demand, and a per-channel record of what is currently sounding.
// ---------------------------------------------------------------------------

EM_JS(int, mc2_web_audio_open, (), {
    try {
        if (!Module.mc2Audio) {
            var Ctor = window.AudioContext || window.webkitAudioContext;
            if (!Ctor) return 0;
            var ctx = new Ctor();
            Module.mc2Audio = {
                ctx: ctx,
                voices: new Map(),   // channel -> { src, gain, panner, loop }
                plays: 0,
                blocked: 0,
                // Per-channel tally, the browser twin of the native
                // [audio-family] census: plays and the loudest source sample
                // x requested volume handed over on each channel.
                byChannel: {},
                // Every voice goes through one analyser before the speakers, so
                // the shell can measure what actually came OUT, not what was
                // requested. A silent session reads a peak of 0.
                output: null,
                analyser: null
            };
            try {
                var an = ctx.createAnalyser();
                an.fftSize = 2048;
                an.connect(ctx.destination);
                Module.mc2Audio.analyser = an;
                Module.mc2Audio.output = an;
            } catch (e) {}
        }
        return 1;
    } catch (e) {
        return 0;
    }
});

EM_JS(void, mc2_web_audio_close, (), {
    var a = Module.mc2Audio;
    if (!a) return;
    a.voices.forEach(function (v) { try { v.src.stop(); } catch (e) {} });
    a.voices.clear();
    try { a.ctx.close(); } catch (e) {}
    Module.mc2Audio = null;
});

// Called by the shell from a user gesture. Safe to call repeatedly.
EM_JS(void, mc2_web_audio_resume, (), {
    var a = Module.mc2Audio;
    if (a && a.ctx && a.ctx.state === 'suspended') a.ctx.resume();
});

EM_JS(void, mc2_web_audio_play,
      (int channel, int ptr, int bytes, int chans, int rate, int bits,
       float volume, float pan, int loop), {
    var a = Module.mc2Audio;
    if (!a) return;
    try {
        // A play arriving while the context is still suspended is counted, not
        // dropped silently -- "the browser never let us start" and "we never
        // tried" look identical otherwise, and that ambiguity has cost this
        // project sessions before.
        if (a.ctx.state === 'suspended') { a.blocked++; a.ctx.resume(); }

        var frameBytes = (bits >> 3) * chans;
        var frames = frameBytes ? Math.floor(bytes / frameBytes) : 0;
        if (frames <= 0) return;

        var buffer = a.ctx.createBuffer(chans, frames, rate);
        for (var c = 0; c < chans; ++c) {
            var out = buffer.getChannelData(c);
            if (bits === 16) {
                // HEAP16 is little-endian, which is what every WAV in the game
                // is; the common layer never hands us anything else.
                var base = (ptr >> 1) + c;
                for (var i = 0; i < frames; ++i)
                    out[i] = HEAP16[base + i * chans] / 32768;
            } else {
                var base8 = ptr + c;
                for (var j = 0; j < frames; ++j)
                    out[j] = (HEAPU8[base8 + j * chans] - 128) / 128;
            }
        }

        var old = a.voices.get(channel);
        if (old) { try { old.src.stop(); } catch (e) {} }

        var gain = a.ctx.createGain();
        gain.gain.value = volume;
        var node = gain;
        var panner = null;
        if (a.ctx.createStereoPanner) {
            panner = a.ctx.createStereoPanner();
            panner.pan.value = pan;
            gain.connect(panner);
            node = panner;
        }
        node.connect(a.output || a.ctx.destination);

        var src = a.ctx.createBufferSource();
        src.buffer = buffer;
        src.loop = !!loop;
        src.connect(gain);
        var voice = { src: src, gain: gain, panner: panner, loop: !!loop };
        src.onended = function () {
            if (a.voices.get(channel) === voice) a.voices.delete(channel);
        };
        a.voices.set(channel, voice);
        src.start();
        a.plays++;
        var peak = 0;
        for (var pc = 0; pc < chans; ++pc) {
            var d = buffer.getChannelData(pc);
            for (var k = 0; k < frames; k += 7) { var v = d[k] < 0 ? -d[k] : d[k]; if (v > peak) peak = v; }
        }
        var tally = a.byChannel[channel] || (a.byChannel[channel] = { plays: 0, loudest: 0 });
        tally.plays++;
        if (peak * volume > tally.loudest) tally.loudest = peak * volume;
    } catch (e) {
        // Never fatal. A refused buffer is silence, not a crash.
    }
});

EM_JS(void, mc2_web_audio_stop, (int channel), {
    var a = Module.mc2Audio;
    if (!a) return;
    var v = a.voices.get(channel);
    if (!v) return;
    try { v.src.stop(); } catch (e) {}
    a.voices.delete(channel);
});

EM_JS(void, mc2_web_audio_set_volume, (int channel, float volume), {
    var a = Module.mc2Audio;
    if (!a) return;
    var v = a.voices.get(channel);
    // setTargetAtTime rather than a bare assignment: MC2 ramps volume in small
    // steps (music fades, per-weapon distance attenuation) and a stepped
    // assignment on a live GainNode clicks. The time constant is short enough
    // that a fade still tracks.
    if (v) v.gain.gain.setTargetAtTime(volume, a.ctx.currentTime, 0.01);
});

EM_JS(void, mc2_web_audio_set_pan, (int channel, float pan), {
    var a = Module.mc2Audio;
    if (!a) return;
    var v = a.voices.get(channel);
    if (v && v.panner) v.panner.pan.setTargetAtTime(pan, a.ctx.currentTime, 0.01);
});

EM_JS(int, mc2_web_audio_is_playing, (int channel), {
    var a = Module.mc2Audio;
    return (a && a.voices.has(channel)) ? 1 : 0;
});

EM_JS(int, mc2_web_audio_plays, (), {
    return Module.mc2Audio ? Module.mc2Audio.plays : 0;
});

EM_JS(int, mc2_web_audio_blocked, (), {
    return Module.mc2Audio ? Module.mc2Audio.blocked : 0;
});

// ---------------------------------------------------------------------------

class WebAudioBackend : public AudioBackend
{
public:
    const char* Name() const override { return "web"; }

    bool Open() override
    {
        // A suspended context is still open. See the autoplay note at the top.
        return mc2_web_audio_open() != 0;
    }

    void Close() override { mc2_web_audio_close(); }

    void Play(int channel, const Clip& clip, float volume, float pan, bool loop) override
    {
        if (!clip.pcm || !clip.bytes) return;
        mc2_web_audio_play(channel,
                           (int)(std::size_t)clip.pcm, (int)clip.bytes,
                           clip.channels, clip.rate, clip.bits,
                           volume, pan, loop ? 1 : 0);
    }

    void Stop(int channel) override { mc2_web_audio_stop(channel); }

    void SetVolume(int channel, float volume) override
    {
        if (volume < 0.0f) volume = 0.0f;
        if (volume > 1.0f) volume = 1.0f;
        mc2_web_audio_set_volume(channel, volume);
    }

    void SetPan(int channel, float pan) override
    {
        if (pan < -1.0f) pan = -1.0f;
        if (pan > 1.0f) pan = 1.0f;
        mc2_web_audio_set_pan(channel, pan);
    }

    bool IsPlaying(int channel) const override
    {
        return mc2_web_audio_is_playing(channel) != 0;
    }

    void DumpStats() const override
    {
        // `blocked` is the number that answers the question the console cannot:
        // whether the browser's autoplay policy ate the session. Non-zero with
        // plays > 0 means sound was requested before the first gesture.
        std::printf("[audio-web] plays=%d submittedWhileSuspended=%d\n",
                    mc2_web_audio_plays(), mc2_web_audio_blocked());
        std::fflush(stdout);
    }
};

WebAudioBackend g_web;

} // namespace

AudioBackend* MakeWebAudioBackend() { return &g_web; }

// Exported so the shell can lift the autoplay block on the first user gesture.
extern "C" EMSCRIPTEN_KEEPALIVE void mc2_audio_resume(void)
{
    mc2_web_audio_resume();
}

} // namespace mc2audio
