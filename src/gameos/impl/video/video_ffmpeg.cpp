//---------------------------------------------------------------------------//
// WebM/VP9 + Opus playback through ffmpeg's libraries.
//
// Built only when MC2_ENABLE_FFMPEG is on. One dependency covers all three jobs
// the movies need -- container demux, VP9 video, Opus audio -- which is why it
// was chosen over libvpx + libopus + libwebm: three permissive statics would be
// smaller on disk but three separate things to wire, and libvpx builds awkwardly
// under clang-cl. Linked DYNAMICALLY and deliberately: that is what keeps the
// LGPL simple, and shipping a DLL beside the exe is already how this port
// handles d3dcompiler_47.dll.
//
// The five DLLs are ORDINARY IMPORTS and must sit beside the exe; CMake stages
// them for exactly that reason. Delay-loading them was tried and measured not
// to work with this toolchain (see the note in CMakeLists.txt), so nothing here
// can rescue a process whose imports failed to bind -- a missing DLL is still
// an OS dialog before main(), which is what a ctest run hit on 2026-08-15.
//
// The LoadLibrary probe below is therefore NOT that rescue. It is what gives
// DecoderAvailable() a real answer instead of an assumed one, and it is the
// hook a future explicit GetProcAddress binding would build on.
//
// Output is BGRA because gos_LockTexture hands back bytes in that order on this
// target. swscale is asked for AV_PIX_FMT_BGRA directly rather than converting
// by hand.
//---------------------------------------------------------------------------//

#include "video_decoder.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace mc2video
{
    Decoder* CreateNullDecoder();   // video_null.cpp, the runtime fallback

    namespace
    {
        bool VideoCensusOn()
        {
            static const char* env = std::getenv("MC2_VIDEO_CENSUS");
            static const bool on = env && *env && std::strcmp(env, "0") != 0;
            return on;
        }

        // Probed ONCE, and probed rather than assumed. With static imports this
        // will practically always say yes -- the process could not have reached
        // here otherwise -- but "practically always" is not the same claim as
        // "by construction", and the null decoder is a cheaper answer than a
        // fault if it ever says no.
        bool LibrariesPresent()
        {
#if defined(_WIN32)
            static const bool present = []() -> bool {
                static const char* const names[] = {
                    "avcodec-61.dll", "avformat-61.dll", "avutil-59.dll",
                    "swscale-8.dll",  "swresample-5.dll"
                };
                for (const char* name : names)
                {
                    if (!::LoadLibraryA(name))
                    {
                        if (VideoCensusOn())
                            std::fprintf(stderr, "[video] %s not found -- no decoder\n", name);
                        return false;
                    }
                }
                return true;
            }();
            return present;
#else
            return true;
#endif
        }

        class FfmpegDecoder : public Decoder
        {
        public:
            ~FfmpegDecoder() override { Close(); }

            bool Open(const char* path) override
            {
                Close();
                if (!path || !*path) return false;

                if (avformat_open_input(&m_format, path, nullptr, nullptr) < 0)
                {
                    if (VideoCensusOn())
                        std::fprintf(stderr, "[video] cannot open '%s'\n", path);
                    return false;
                }
                if (avformat_find_stream_info(m_format, nullptr) < 0) { Close(); return false; }

                m_videoStream = av_find_best_stream(m_format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
                if (m_videoStream < 0) { Close(); return false; }
                if (!OpenCodec(m_format, m_videoStream, &m_video)) { Close(); return false; }

                m_width  = m_video->width;
                m_height = m_video->height;
                m_scaler = sws_getContext(m_width, m_height, m_video->pix_fmt,
                                          m_width, m_height, AV_PIX_FMT_BGRA,
                                          SWS_BILINEAR, nullptr, nullptr, nullptr);
                if (!m_scaler) { Close(); return false; }
                m_bgra.assign(static_cast<size_t>(m_width) * m_height * 4, 0);

                // 109 of the 185 movies carry no audio at all, so a missing
                // audio stream is normal and must not fail the open. Only the
                // shape is read here; nothing is decoded until PreloadAudio.
                const int audio = av_find_best_stream(m_format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
                if (audio >= 0)
                {
                    const AVCodecParameters* p = m_format->streams[audio]->codecpar;
                    m_audioChannels = p->ch_layout.nb_channels;
                    m_audioRate     = p->sample_rate;
                    if (m_audioChannels <= 0 || m_audioRate <= 0)
                        m_audioChannels = m_audioRate = 0;
                }

                m_packet = av_packet_alloc();
                m_frame  = av_frame_alloc();
                m_path     = path;
                m_finished = false;
                if (VideoCensusOn())
                    std::fprintf(stderr, "[video] opened '%s' %dx%d audio=%d ch=%d rate=%d\n",
                                 path, m_width, m_height, m_audioChannels > 0 ? 1 : 0,
                                 m_audioChannels, m_audioRate);
                return true;
            }

            bool Advance(double elapsedSeconds) override
            {
                m_frameIsNew = false;
                if (!m_format || m_finished) return false;

                // Present the LAST frame whose stamp is <= the clock. Doing it
                // the other way -- stopping on the first stamp past the clock --
                // runs the picture one whole frame ahead of the caller, which at
                // 15 fps is 67 ms of lead against the sound.
                for (;;)
                {
                    if (!m_pendingValid && !ReceiveNextFrame())
                    {
                        // Nothing more will arrive. Hold the final frame for its
                        // own duration before declaring the movie over.
                        if (!m_hasFrame || elapsedSeconds >= m_frameTime + FrameStep())
                        {
                            m_finished = true;
                            return false;
                        }
                        return true;
                    }

                    if (m_hasFrame && m_pendingTime > elapsedSeconds)
                        break;

                    Present();
                }
                return true;
            }

            bool FrameIsNew() const override { return m_frameIsNew; }

            const unsigned char* Bgra() const override
            { return m_hasFrame ? m_bgra.data() : nullptr; }

            int Width() const override  { return m_width; }
            int Height() const override { return m_height; }
            bool Finished() const override { return m_finished; }

            bool Rewind() override
            {
                if (!m_format || m_videoStream < 0) return false;
                if (av_seek_frame(m_format, m_videoStream, 0, AVSEEK_FLAG_BACKWARD) < 0)
                    return false;
                avcodec_flush_buffers(m_video);
                if (m_pendingValid) { av_frame_unref(m_frame); m_pendingValid = false; }
                m_frameTime = m_pendingTime = 0.0;
                m_hasFrame = m_frameIsNew = false;
                m_flushed = m_finished = false;
                // The audio buffer is NOT re-decoded: PopAudio has already
                // drained it into the caller's own buffer, which is the thing
                // that gets replayed.
                return true;
            }

            // One audio-only pass over the file: a second demuxer, the audio
            // codec, and no video decoding at all. Opus at 48 kHz decodes far
            // faster than real time, so the whole 160 s cinema track is ready
            // before the first frame is drawn.
            int PreloadAudio() override
            {
                if (m_audioChannels <= 0 || m_path.empty()) return 0;
                if (m_audioPreloaded) return static_cast<int>(m_pcm.size());
                m_audioPreloaded = true;

                AVFormatContext* format = nullptr;
                AVCodecContext*  codec  = nullptr;
                SwrContext*      resampler = nullptr;
                AVPacket*        packet = nullptr;
                AVFrame*         frame  = nullptr;
                int              stream = -1;

                if (avformat_open_input(&format, m_path.c_str(), nullptr, nullptr) < 0)
                    return 0;

                if (avformat_find_stream_info(format, nullptr) >= 0 &&
                    (stream = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0)) >= 0 &&
                    OpenCodec(format, stream, &codec))
                {
                    AVChannelLayout out = {};
                    av_channel_layout_default(&out, m_audioChannels);
                    if (swr_alloc_set_opts2(&resampler, &out, AV_SAMPLE_FMT_S16, m_audioRate,
                                            &codec->ch_layout, codec->sample_fmt,
                                            codec->sample_rate, 0, nullptr) >= 0 &&
                        swr_init(resampler) >= 0)
                    {
                        packet = av_packet_alloc();
                        frame  = av_frame_alloc();
                        bool flushed = false;
                        for (;;)
                        {
                            if (!flushed && av_read_frame(format, packet) >= 0)
                            {
                                if (packet->stream_index == stream)
                                    avcodec_send_packet(codec, packet);
                                av_packet_unref(packet);
                            }
                            else if (!flushed)
                            {
                                avcodec_send_packet(codec, nullptr);
                                flushed = true;
                            }

                            while (avcodec_receive_frame(codec, frame) == 0)
                            {
                                AppendResampled(resampler, frame);
                                av_frame_unref(frame);
                            }
                            // Once the flush packet has been drained there is
                            // nothing left to read OR receive, whatever the
                            // last return code was; looping again would spin.
                            if (flushed) break;
                        }
                        // Whatever the resampler is still holding.
                        AppendResampled(resampler, nullptr);
                    }
                    av_channel_layout_uninit(&out);
                }

                if (frame)     av_frame_free(&frame);
                if (packet)    av_packet_free(&packet);
                if (resampler) swr_free(&resampler);
                if (codec)     avcodec_free_context(&codec);
                avformat_close_input(&format);

                if (VideoCensusOn())
                    std::fprintf(stderr, "[video] preloaded %d audio samples (%dch %dHz)\n",
                                 (int)m_pcm.size(), m_audioChannels, m_audioRate);
                return static_cast<int>(m_pcm.size());
            }

            int PopAudio(short* destination, int maxSamples) override
            {
                if (!destination || maxSamples <= 0 || m_pcm.empty()) return 0;
                const int count = static_cast<int>(m_pcm.size()) < maxSamples
                                ? static_cast<int>(m_pcm.size()) : maxSamples;
                std::memcpy(destination, m_pcm.data(), static_cast<size_t>(count) * sizeof(short));
                m_pcm.erase(m_pcm.begin(), m_pcm.begin() + count);
                return count;
            }

            int AudioChannels() const override   { return m_audioChannels; }
            int AudioSampleRate() const override { return m_audioRate; }

            void Close() override
            {
                if (m_scaler)    { sws_freeContext(m_scaler); m_scaler = nullptr; }
                if (m_frame)     { av_frame_free(&m_frame); }
                if (m_packet)    { av_packet_free(&m_packet); }
                if (m_video)     { avcodec_free_context(&m_video); }
                if (m_format)    { avformat_close_input(&m_format); }
                m_bgra.clear();
                m_pcm.clear();
                m_path.clear();
                m_width = m_height = 0;
                m_videoStream = -1;
                m_audioChannels = m_audioRate = 0;
                m_frameTime = m_pendingTime = 0.0;
                m_hasFrame = m_frameIsNew = m_pendingValid = false;
                m_audioPreloaded = m_flushed = false;
                m_finished = true;
            }

        private:
            static bool OpenCodec(AVFormatContext* format, int stream, AVCodecContext** out)
            {
                AVStream* source = format->streams[stream];
                const AVCodec* codec = avcodec_find_decoder(source->codecpar->codec_id);
                if (!codec) return false;
                *out = avcodec_alloc_context3(codec);
                if (!*out) return false;
                if (avcodec_parameters_to_context(*out, source->codecpar) < 0) return false;
                return avcodec_open2(*out, codec, nullptr) >= 0;
            }

            // `frame` null flushes whatever the resampler still holds.
            void AppendResampled(SwrContext* resampler, const AVFrame* frame)
            {
                const int inSamples = frame ? frame->nb_samples : 0;
                const int maxOut = swr_get_out_samples(resampler, inSamples);
                if (maxOut <= 0) return;

                const size_t base = m_pcm.size();
                m_pcm.resize(base + static_cast<size_t>(maxOut) * m_audioChannels);
                uint8_t* out = reinterpret_cast<uint8_t*>(m_pcm.data() + base);
                const int written = swr_convert(resampler, &out, maxOut,
                                                frame ? const_cast<const uint8_t**>(frame->data) : nullptr,
                                                inSamples);
                m_pcm.resize(base + (written > 0
                           ? static_cast<size_t>(written) * m_audioChannels : 0));
            }

            // Scale the frame held in m_frame into m_bgra and make it current.
            void Present()
            {
                uint8_t* planes[4] = { m_bgra.data(), nullptr, nullptr, nullptr };
                int strides[4] = { m_width * 4, 0, 0, 0 };
                sws_scale(m_scaler, m_frame->data, m_frame->linesize, 0, m_height,
                          planes, strides);
                m_frameTime   = m_pendingTime;
                m_hasFrame    = true;
                m_frameIsNew  = true;
                m_pendingValid = false;
                av_frame_unref(m_frame);
            }

            // Leaves the next decoded frame in m_frame with its stamp in
            // m_pendingTime. False only at true end of stream, and the decoder
            // is drained before that is reported -- returning early there threw
            // away every frame the codec was still holding.
            bool ReceiveNextFrame()
            {
                for (;;)
                {
                    const int got = avcodec_receive_frame(m_video, m_frame);
                    if (got == 0)
                    {
                        const AVRational base = m_format->streams[m_videoStream]->time_base;
                        const int64_t stamp = m_frame->best_effort_timestamp;
                        m_pendingTime = (stamp == AV_NOPTS_VALUE)
                                      ? m_frameTime + FrameStep()
                                      : stamp * av_q2d(base);
                        m_pendingValid = true;
                        return true;
                    }
                    if (got != AVERROR(EAGAIN)) return false;   // AVERROR_EOF or a real fault
                    if (m_flushed) return false;

                    if (av_read_frame(m_format, m_packet) < 0)
                    {
                        avcodec_send_packet(m_video, nullptr);  // flush; frames still come out
                        m_flushed = true;
                        continue;
                    }
                    // Audio is not touched here: it is preloaded whole, so a
                    // packet for any other stream is simply dropped.
                    if (m_packet->stream_index == m_videoStream)
                        avcodec_send_packet(m_video, m_packet);
                    av_packet_unref(m_packet);
                }
            }

            double FrameStep() const
            {
                const AVRational rate = m_format->streams[m_videoStream]->avg_frame_rate;
                const double fps = rate.den ? av_q2d(rate) : 15.0;
                return fps > 0.0 ? 1.0 / fps : 1.0 / 15.0;
            }

            AVFormatContext* m_format = nullptr;
            AVCodecContext*  m_video = nullptr;
            SwsContext*      m_scaler = nullptr;
            AVPacket*        m_packet = nullptr;
            AVFrame*         m_frame = nullptr;
            std::vector<unsigned char> m_bgra;
            std::vector<short>         m_pcm;
            std::string m_path;
            int    m_videoStream = -1;
            int    m_width = 0, m_height = 0;
            int    m_audioChannels = 0, m_audioRate = 0;
            double m_frameTime = 0.0;      // stamp of the frame in m_bgra
            double m_pendingTime = 0.0;    // stamp of the frame held in m_frame
            bool   m_hasFrame = false;
            bool   m_frameIsNew = false;
            bool   m_pendingValid = false;
            bool   m_audioPreloaded = false;
            bool   m_flushed = false;
            bool   m_finished = true;
        };
    }

    Decoder* CreateDecoder()
    {
        if (!LibrariesPresent()) return CreateNullDecoder();
        return new FfmpegDecoder();
    }

    bool DecoderAvailable() { return LibrariesPresent(); }

    const char* BackendName() { return LibrariesPresent() ? "ffmpeg" : "none (ffmpeg libraries missing)"; }
}
