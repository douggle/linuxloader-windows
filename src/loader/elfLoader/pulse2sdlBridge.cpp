#if defined(_WIN32) || defined(__MINGW32__)

#include "pulse2sdlBridge.hpp"
#include "symbolResolver.hpp"
#include "../audio/lindbergh/alsa2sdl/alsa2sdl.h"
#include "../log/log.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>

#define MAP(name, func) SymbolResolver::GetInstance().RegisterVTable(name, reinterpret_cast<void *>(func))

// PulseAudio's "simple" API (libpulse-simple.so.0). Big Buck HD Wild plays its
// sound through it (and Angry Birds and The Walking Dead import pa_simple_new /
// pa_simple_free). There is no PulseAudio on Windows, so each call maps onto
// the same SDL audio sink the ALSA bridge uses (alsa2sdl.c): pa_simple is a
// blocking playback stream, exactly what shared_snd_pcm_* already provides.

extern "C"
{
    // pulse/sample.h: pa_sample_spec, only the fields that matter here. The
    // Linux struct is { pa_sample_format_t format; uint32_t rate; uint8_t
    // channels; }, so format is a 32-bit enum at offset 0.
    typedef struct
    {
        int32_t format;
        uint32_t rate;
        uint8_t channels;
    } RtPaSampleSpec;

    // pulse/sample.h: pa_sample_format_t.
    enum
    {
        RT_PA_SAMPLE_U8 = 0,
        RT_PA_SAMPLE_ALAW = 1,
        RT_PA_SAMPLE_ULAW = 2,
        RT_PA_SAMPLE_S16LE = 3,
        RT_PA_SAMPLE_S16BE = 4,
        RT_PA_SAMPLE_FLOAT32LE = 5,
        RT_PA_SAMPLE_FLOAT32BE = 6,
        RT_PA_SAMPLE_S32LE = 7,
        RT_PA_SAMPLE_S32BE = 8,
    };

    // alsa2sdl.c's ALSA format ids (its own enum).
    enum
    {
        RT_SND_PCM_FORMAT_S8 = 0,
        RT_SND_PCM_FORMAT_U8 = 1,
        RT_SND_PCM_FORMAT_S16_LE = 2,
        RT_SND_PCM_FORMAT_S16_BE = 3,
        RT_SND_PCM_FORMAT_FLOAT_LE = 14,
    };

    enum
    {
        RT_SND_PCM_STREAM_PLAYBACK = 0,
        RT_SND_PCM_ACCESS_RW_INTERLEAVED = 3,
    };

    // pulse/def.h: pa_stream_direction_t.
    enum
    {
        RT_PA_STREAM_PLAYBACK = 1,
    };

    typedef struct
    {
        snd_pcm_t *pcm;
        unsigned int rate;
        int frameBytes; // bytes per frame (sample size * channels)
    } RtPaSimple;

    static void paSetError(int *error, int code)
    {
        if (error)
            *error = code;
    }

    // One pa sample format -> an ALSA format alsa2sdl.c plays, and the sample
    // size in bytes. Unknown formats fall back to S16LE.
    static int paFormatToAlsa(int32_t paFormat, int *sampleBytes)
    {
        switch (paFormat)
        {
            case RT_PA_SAMPLE_U8:
                *sampleBytes = 1;
                return RT_SND_PCM_FORMAT_U8;
            case RT_PA_SAMPLE_S16LE:
                *sampleBytes = 2;
                return RT_SND_PCM_FORMAT_S16_LE;
            case RT_PA_SAMPLE_S16BE:
                *sampleBytes = 2;
                return RT_SND_PCM_FORMAT_S16_BE;
            case RT_PA_SAMPLE_FLOAT32LE:
                *sampleBytes = 4;
                return RT_SND_PCM_FORMAT_FLOAT_LE;
            default:
                log_warn("PulseAudio: sample format %d unsupported, using S16LE", (int)paFormat);
                *sampleBytes = 2;
                return RT_SND_PCM_FORMAT_S16_LE;
        }
    }

    // pa_simple_new(server, name, dir, dev, stream_name, ss, map, attr, error)
    static void *rtPaSimpleNew(const char *server, const char *name, int dir, const char *dev, const char *streamName,
                               const RtPaSampleSpec *ss, const void *map, const void *attr, int *error)
    {
        (void)server;
        (void)dev;
        (void)map;
        (void)attr;

        if (dir != RT_PA_STREAM_PLAYBACK)
        {
            log_error("PulseAudio: only playback streams are supported (dir=%d)", dir);
            paSetError(error, 1); // PA_ERR_ACCESS
            return nullptr;
        }
        if (!ss || ss->channels == 0 || ss->rate == 0)
        {
            log_error("PulseAudio: invalid sample spec");
            paSetError(error, 2); // PA_ERR_INVALID
            return nullptr;
        }

        int sampleBytes = 2;
        int alsaFormat = paFormatToAlsa(ss->format, &sampleBytes);

        snd_pcm_t *pcm = nullptr;
        if (shared_snd_pcm_open(&pcm, "default", RT_SND_PCM_STREAM_PLAYBACK, 0) < 0 || !pcm)
        {
            log_error("PulseAudio: could not open the SDL audio sink");
            paSetError(error, 5); // PA_ERR_CONNECTIONREFUSED
            return nullptr;
        }

        snd_pcm_hw_params_t *hw = nullptr;
        shared_snd_pcm_hw_params_malloc(&hw);
        shared_snd_pcm_hw_params_any(pcm, hw);
        shared_snd_pcm_hw_params_set_access(pcm, hw, RT_SND_PCM_ACCESS_RW_INTERLEAVED);
        shared_snd_pcm_hw_params_set_format(pcm, hw, alsaFormat);
        shared_snd_pcm_hw_params_set_channels(pcm, hw, ss->channels);
        unsigned int rate = ss->rate;
        int rdir = 0;
        shared_snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, &rdir);
        shared_snd_pcm_hw_params(pcm, hw);
        shared_snd_pcm_hw_params_free(hw);
        shared_snd_pcm_prepare(pcm);
        shared_snd_pcm_start(pcm);

        RtPaSimple *s = (RtPaSimple *)calloc(1, sizeof(RtPaSimple));
        s->pcm = pcm;
        s->rate = rate;
        s->frameBytes = sampleBytes * ss->channels;

        log_info("PulseAudio: '%s'/'%s' -> SDL sink, %u Hz, %u ch, %d B/frame", name ? name : "", streamName ? streamName : "",
                 rate, (unsigned)ss->channels, s->frameBytes);
        paSetError(error, 0);
        return s;
    }

    // pa_simple_write(s, data, bytes, error): 0 on success, negative on error.
    static int rtPaSimpleWrite(void *simple, const void *data, size_t bytes, int *error)
    {
        RtPaSimple *s = (RtPaSimple *)simple;
        if (!s || !s->pcm || s->frameBytes <= 0)
        {
            paSetError(error, 2); // PA_ERR_INVALID
            return -1;
        }
        if (bytes == 0)
        {
            paSetError(error, 0);
            return 0;
        }

        unsigned long frames = (unsigned long)(bytes / (size_t)s->frameBytes);
        const unsigned char *p = (const unsigned char *)data;
        while (frames > 0)
        {
            snd_pcm_sframes_t wrote = shared_snd_pcm_writei(s->pcm, p, frames);
            if (wrote < 0)
            {
                // Recover from an underrun once, then give up on this write.
                if (shared_snd_pcm_recover(s->pcm, (int)wrote, 1) < 0)
                {
                    paSetError(error, 5); // PA_ERR_CONNECTIONREFUSED
                    return -1;
                }
                continue;
            }
            frames -= (unsigned long)wrote;
            p += (unsigned long)wrote * (unsigned long)s->frameBytes;
        }
        paSetError(error, 0);
        return 0;
    }

    // pa_simple_drain(s, error): play out what is queued. 0 on success.
    static int rtPaSimpleDrain(void *simple, int *error)
    {
        RtPaSimple *s = (RtPaSimple *)simple;
        if (s && s->pcm)
            shared_snd_pcm_drain(s->pcm);
        paSetError(error, 0);
        return 0;
    }

    // pa_simple_flush(s, error): discard what is queued. 0 on success.
    static int rtPaSimpleFlush(void *simple, int *error)
    {
        RtPaSimple *s = (RtPaSimple *)simple;
        if (s && s->pcm)
            shared_snd_pcm_drop(s->pcm);
        paSetError(error, 0);
        return 0;
    }

    // pa_simple_get_latency(s, error): microseconds of audio still queued.
    static uint64_t rtPaSimpleGetLatency(void *simple, int *error)
    {
        RtPaSimple *s = (RtPaSimple *)simple;
        paSetError(error, 0);
        if (!s || !s->pcm || s->rate == 0)
            return 0;
        snd_pcm_sframes_t queued = 0;
        shared_snd_pcm_delay(s->pcm, &queued);
        if (queued < 0)
            queued = 0;
        return (uint64_t)queued * 1000000ULL / (uint64_t)s->rate;
    }

    // pa_simple_read(s, data, bytes, error): no capture, so silence and fail.
    static int rtPaSimpleRead(void *simple, void *data, size_t bytes, int *error)
    {
        (void)simple;
        if (data && bytes)
            memset(data, 0, bytes);
        paSetError(error, 1); // PA_ERR_ACCESS
        return -1;
    }

    static void rtPaSimpleFree(void *simple)
    {
        RtPaSimple *s = (RtPaSimple *)simple;
        if (!s)
            return;
        if (s->pcm)
            shared_snd_pcm_close(s->pcm);
        free(s);
    }

    static const char *rtPaStrerror(int error)
    {
        (void)error;
        return "PulseAudio error";
    }
}

namespace Pulse2SdlBridge
{
    void initBridges()
    {
        MAP("pa_simple_new", rtPaSimpleNew);
        MAP("pa_simple_free", rtPaSimpleFree);
        MAP("pa_simple_write", rtPaSimpleWrite);
        MAP("pa_simple_drain", rtPaSimpleDrain);
        MAP("pa_simple_flush", rtPaSimpleFlush);
        MAP("pa_simple_get_latency", rtPaSimpleGetLatency);
        MAP("pa_simple_read", rtPaSimpleRead);
        MAP("pa_strerror", rtPaStrerror);
    }
} // namespace Pulse2SdlBridge

#endif
