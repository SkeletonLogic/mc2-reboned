//---------------------------------------------------------------------------//
// M8-B76: the browser movie decoder.
//
// The browser already decodes VP9 + Opus, so this peer hands the .webm bytes to
// it instead of shipping a decoder in wasm:
//
//   picture  a hidden, muted <video> element plays the file in real time; each
//            Advance() draws its current frame into a 2D canvas and copies the
//            pixels out as B,G,R,A (see the pixel-order note in video_decoder.h).
//   sound    decodeAudioData() turns the same bytes into PCM once, at Open(), so
//            the track goes through gosAudio_UserMemory exactly as the ffmpeg
//            peer's does -- same volume slider, same channel, same timing code
//            in MC2Movie. The <video> element stays muted.
//
// Open() waits for the element and the audio decode through Asyncify, because
// the Decoder contract is synchronous. Everything else is plain EM_JS.
//
// The movie clock is MC2Movie's, not the element's: if the two drift more than
// a fraction of a second apart (a paused game, a restart) the element is seeked
// back onto the game clock.
//---------------------------------------------------------------------------//

#include "video_decoder.h"
#include "../vfs/vfs_backend.h"

#include <emscripten/emscripten.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// Returns a handle > 0, or 0 if the element could not load the file. out[] gets
// width, height, audio channels, audio sample rate, audio frames per channel.
EM_ASYNC_JS(int, mc2v_open, (const unsigned char* data, int size, int* out), {
    const bytes = HEAPU8.slice(data, data + size);
    const url = URL.createObjectURL(new Blob([bytes], { type: 'video/webm' }));
    const v = document.createElement('video');
    v.muted = true;
    v.playsInline = true;
    v.preload = 'auto';
    v.style.cssText = 'position:fixed;left:0;top:0;width:2px;height:2px;opacity:0.01;pointer-events:none;z-index:-1';
    document.body.appendChild(v);
    const loaded = await new Promise(resolve => {
        v.onloadeddata = () => resolve(true);
        v.onerror = () => resolve(false);
        setTimeout(() => resolve(false), 15000);
        v.src = url;
    });
    if (!loaded || !v.videoWidth || !v.videoHeight) {
        v.remove(); URL.revokeObjectURL(url);
        return 0;
    }
    let audio = null;
    try {
        const ctx = new OfflineAudioContext(1, 1, 48000);
        audio = await ctx.decodeAudioData(bytes.buffer.slice(0));
    } catch (e) {
        audio = null;              // no track: 109 of the movies carry none
    }
    const w = v.videoWidth, h = v.videoHeight;
    const canvas = new OffscreenCanvas(w, h);
    const g = canvas.getContext('2d', { willReadFrequently: true });
    const M = (Module.mc2Video ||= { next: 1, movies: {} });
    const id = M.next++;
    M.movies[id] = { v, url, canvas, g, audio, lastTime: -1, started: false };
    HEAP32[(out >> 2) + 0] = w;
    HEAP32[(out >> 2) + 1] = h;
    HEAP32[(out >> 2) + 2] = audio ? audio.numberOfChannels : 0;
    HEAP32[(out >> 2) + 3] = audio ? audio.sampleRate : 0;
    HEAP32[(out >> 2) + 4] = audio ? audio.length : 0;
    return id;
});

// Bring the element onto the game clock and copy its frame out if it changed.
// Returns bit 0 = new frame written, bit 1 = finished.
EM_JS(int, mc2v_frame, (int id, double seconds, unsigned char* dst), {
    const m = Module.mc2Video && Module.mc2Video.movies[id];
    if (!m) return 2;
    const v = m.v;
    if (!m.started) {
        m.started = true;
        v.currentTime = 0;
        const p = v.play();
        if (p && p.catch) p.catch(() => {});
    }
    const duration = isFinite(v.duration) ? v.duration : 0;
    if (!v.seeking && Math.abs(v.currentTime - seconds) > 0.35 && (!duration || seconds < duration))
        v.currentTime = seconds;
    if (v.paused && !v.ended && (!duration || seconds < duration)) {
        const p = v.play();
        if (p && p.catch) p.catch(() => {});
    }
    let result = 0;
    if (v.readyState >= 2 && v.currentTime !== m.lastTime) {
        m.lastTime = v.currentTime;
        const w = m.canvas.width, h = m.canvas.height;
        m.g.drawImage(v, 0, 0, w, h);
        const src = new Uint32Array(m.g.getImageData(0, 0, w, h).data.buffer);
        const out = new Uint32Array(HEAPU8.buffer, dst, w * h);
        for (let i = 0; i < src.length; i++) {
            const p = src[i];      // little-endian R,G,B,A -> B,G,R,A
            out[i] = (p & 0xff00ff00) | ((p & 0xff) << 16) | ((p >>> 16) & 0xff);
        }
        result |= 1;
    }
    if (v.ended || (duration && seconds >= duration)) result |= 2;
    return result;
});

// Interleaved 16-bit PCM of the whole track into dst; returns samples written.
EM_JS(int, mc2v_audio, (int id, short* dst, int maxSamples), {
    const m = Module.mc2Video && Module.mc2Video.movies[id];
    if (!m || !m.audio) return 0;
    const a = m.audio, ch = a.numberOfChannels, n = Math.min(a.length, Math.floor(maxSamples / ch));
    const out = new Int16Array(HEAPU8.buffer, dst, n * ch);
    for (let c = 0; c < ch; c++) {
        const s = a.getChannelData(c);
        for (let i = 0; i < n; i++) {
            const x = Math.max(-1, Math.min(1, s[i]));
            out[i * ch + c] = x < 0 ? x * 32768 : x * 32767;
        }
    }
    return n * ch;
});

EM_JS(void, mc2v_rewind, (int id), {
    const m = Module.mc2Video && Module.mc2Video.movies[id];
    if (!m) return;
    m.v.currentTime = 0; m.lastTime = -1; m.started = false;
});

EM_JS(void, mc2v_close, (int id), {
    const M = Module.mc2Video;
    const m = M && M.movies[id];
    if (!m) return;
    m.v.pause(); m.v.removeAttribute('src'); m.v.load(); m.v.remove();
    URL.revokeObjectURL(m.url);
    delete M.movies[id];
});

namespace mc2video
{
    namespace
    {
        class WebDecoder : public Decoder
        {
        public:
            ~WebDecoder() override { Close(); }

            bool Open(const char* path) override
            {
                Close();
                if (!path || !path[0]) return false;
                VfsFile* file = Vfs().Open(path, "rb");
                if (!file) return false;
                std::vector<unsigned char> bytes((size_t)Vfs().Size(file));
                const size_t got = bytes.empty() ? 0 : Vfs().Read(file, bytes.data(), bytes.size());
                Vfs().Close(file);
                if (got == 0 || got != bytes.size()) return false;

                int info[5] = {};
                m_id = mc2v_open(bytes.data(), (int)bytes.size(), info);
                if (m_id <= 0) { m_id = 0; return false; }
                m_width = info[0]; m_height = info[1];
                m_channels = info[2]; m_rate = info[3]; m_audioFrames = info[4];
                m_bgra.assign((size_t)m_width * m_height * 4, 0);
                m_haveFrame = m_new = m_finished = false;

                const char* verbose = std::getenv("MC2_VIDEO_CENSUS");
                if (verbose && *verbose && std::strcmp(verbose, "0") != 0)
                    std::printf("[video] web: '%s' %dx%d audio %d ch @ %d Hz, %d frames\n",
                                path, m_width, m_height, m_channels, m_rate, m_audioFrames);
                return true;
            }

            bool Advance(double elapsedSeconds) override
            {
                m_new = false;
                if (!m_id || m_finished) return false;
                const int r = mc2v_frame(m_id, elapsedSeconds, m_bgra.data());
                if (r & 1) { m_new = true; m_haveFrame = true; }
                if (r & 2) m_finished = true;
                return !m_finished;
            }

            bool FrameIsNew() const override           { return m_new; }
            const unsigned char* Bgra() const override { return m_haveFrame ? m_bgra.data() : nullptr; }
            int Width() const override                 { return m_width; }
            int Height() const override                { return m_height; }
            bool Finished() const override             { return m_finished; }

            bool Rewind() override
            {
                if (!m_id) return false;
                mc2v_rewind(m_id);
                m_finished = false;
                return true;
            }

            int PreloadAudio() override        { m_audioPopped = false; return m_channels > 0 ? m_audioFrames * m_channels : 0; }
            int PopAudio(short* destination, int maxSamples) override
            {
                if (!m_id || m_audioPopped || !destination || maxSamples <= 0) return 0;
                m_audioPopped = true;
                return mc2v_audio(m_id, destination, maxSamples);
            }
            int AudioChannels() const override   { return m_channels; }
            int AudioSampleRate() const override { return m_rate; }

            void Close() override
            {
                if (m_id) mc2v_close(m_id);
                m_id = 0;
                m_bgra.clear();
                m_haveFrame = m_new = false;
                m_finished = true;
            }

        private:
            int m_id = 0;
            int m_width = 0, m_height = 0;
            int m_channels = 0, m_rate = 0, m_audioFrames = 0;
            std::vector<unsigned char> m_bgra;
            bool m_haveFrame = false, m_new = false, m_finished = true, m_audioPopped = false;
        };
    }

    Decoder* CreateDecoder()   { return new WebDecoder(); }
    bool DecoderAvailable()    { return true; }
    const char* BackendName()  { return "web-video"; }
}
