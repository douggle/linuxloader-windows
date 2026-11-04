#if defined(_WIN32) || defined(__MINGW32__)

#include "alsa2sdlBridge.hpp"
#include "symbolResolver.hpp"
#include "../audio/lindbergh/alsa2sdl/alsa2sdl.h"

#define MAP(name, func) SymbolResolver::GetInstance().RegisterVTable(name, reinterpret_cast<void *>(func))

#include <cstdlib>
#include <cstring>

#include "memoryManager.hpp"

extern "C"
{
    static char rtSndDefaultHint;
    static void *rtSndHints[2] = {&rtSndDefaultHint, nullptr};

    static int rtSndDeviceNameHint(int card, const char *iface, void ***hints)
    {
        (void)card;
        (void)iface;
        *hints = rtSndHints;
        return 0;
    }
    // Strings the caller frees with free(): the game's allocator's.
    static char *gameStrdup(const char *s) { return MemoryManager::customStrdup(s); }

    static char *rtSndDeviceNameGetHint(const void *hint, const char *id)
    {
        (void)hint;
        if (id && !strcmp(id, "NAME"))
            return gameStrdup("default");
        if (id && !strcmp(id, "DESC"))
            return gameStrdup("Default Audio Device");
        if (id && !strcmp(id, "IOID"))
            return gameStrdup("Output");
        return nullptr;
    }
    static int rtSndDeviceNameFreeHint(void **hints)
    {
        (void)hints;
        return 0;
    }
    // The bridge's parameter objects are its own (alsa2sdl.c); callers that
    // allocate them on the stack (snd_pcm_hw_params_alloca) get room enough.
    static size_t rtSndHwParamsSizeof(void) { return 256; }
    static size_t rtSndSwParamsSizeof(void) { return 64; }
    static int rtSndHwParamsSetRate(void *pcm, snd_pcm_hw_params_t *params, unsigned int val, int dir)
    {
        unsigned int v = val;
        return shared_snd_pcm_hw_params_set_rate_near((snd_pcm_t *)pcm, params, &v, &dir);
    }
    static int rtSndHwParamsTestRate(void *pcm, void *params, unsigned int val, int dir)
    {
        (void)pcm;
        (void)params;
        (void)dir;
        return val >= 8000 && val <= 192000 ? 0 : -22; // -EINVAL
    }
    static int rtSndPcmNonblock(void *pcm, int nonblock)
    {
        (void)pcm;
        (void)nonblock;
        return 0;
    }
    static long rtSndPcmReadi(void *pcm, void *buf, unsigned long frames)
    {
        (void)pcm;
        (void)buf;
        (void)frames;
        return -5; // -EIO: no capture
    }
    static const char *rtSndAsoundlibVersion(void) { return "1.1.3"; }
}

namespace Alsa2SdlBridge
{
    void initBridges()
    {
        MAP("snd_pcm_open", shared_snd_pcm_open);
        MAP("snd_pcm_close", shared_snd_pcm_close);

        MAP("snd_pcm_hw_params_malloc", shared_snd_pcm_hw_params_malloc);
        MAP("snd_pcm_hw_params_free", shared_snd_pcm_hw_params_free);
        MAP("snd_pcm_hw_params_any", shared_snd_pcm_hw_params_any);
        MAP("snd_pcm_hw_params_set_access", shared_snd_pcm_hw_params_set_access);
        MAP("snd_pcm_hw_params_set_format", shared_snd_pcm_hw_params_set_format);
        MAP("snd_pcm_hw_params_set_channels", shared_snd_pcm_hw_params_set_channels);
        MAP("snd_pcm_hw_params_set_rate_near", shared_snd_pcm_hw_params_set_rate_near);
        MAP("snd_pcm_hw_params_set_periods", shared_snd_pcm_hw_params_set_periods);
        MAP("snd_pcm_hw_params_set_buffer_size", shared_snd_pcm_hw_params_set_buffer_size);
        MAP("snd_pcm_hw_params_set_buffer_size_near", shared_snd_pcm_hw_params_set_buffer_size_near);
        MAP("snd_pcm_hw_params_set_period_size_near", shared_snd_pcm_hw_params_set_period_size_near);
        MAP("snd_pcm_hw_params", shared_snd_pcm_hw_params);
        MAP("snd_pcm_hw_params_copy", shared_snd_pcm_hw_params_copy);
        MAP("snd_pcm_hw_params_set_periods_near", shared_snd_pcm_hw_params_set_periods_near);
        MAP("snd_pcm_hw_params_get_buffer_size", shared_snd_pcm_hw_params_get_buffer_size);
        MAP("snd_pcm_hw_params_get_channels", shared_snd_pcm_hw_params_get_channels);
        MAP("snd_pcm_hw_params_get_period_size", shared_snd_pcm_hw_params_get_period_size);
        MAP("snd_pcm_hw_params_get_periods", shared_snd_pcm_hw_params_get_periods);
        MAP("snd_pcm_recover", shared_snd_pcm_recover);
        MAP("snd_pcm_wait", shared_snd_pcm_wait);

        MAP("snd_pcm_sw_params_malloc", shared_snd_pcm_sw_params_malloc);
        MAP("snd_pcm_sw_params_free", shared_snd_pcm_sw_params_free);
        MAP("snd_pcm_sw_params_current", shared_snd_pcm_sw_params_current);
        MAP("snd_pcm_sw_params_set_avail_min", shared_snd_pcm_sw_params_set_avail_min);
        MAP("snd_pcm_sw_params_set_start_threshold", shared_snd_pcm_sw_params_set_start_threshold);
        MAP("snd_pcm_sw_params_set_xfer_align", shared_snd_pcm_sw_params_set_xfer_align);
        MAP("snd_pcm_sw_params", shared_snd_pcm_sw_params);

        MAP("snd_pcm_prepare", shared_snd_pcm_prepare);
        MAP("snd_pcm_start", shared_snd_pcm_start);
        MAP("snd_pcm_drop", shared_snd_pcm_drop);
        MAP("snd_pcm_resume", shared_snd_pcm_resume);
        MAP("snd_pcm_drain", shared_snd_pcm_drain);
        MAP("snd_pcm_delay", shared_snd_pcm_delay);
        MAP("snd_pcm_writei", shared_snd_pcm_writei);
        MAP("snd_pcm_avail_update", shared_snd_pcm_avail_update);
        MAP("snd_pcm_state", shared_snd_pcm_state);
        MAP("snd_pcm_format_width", shared_snd_pcm_format_width);
        MAP("snd_strerror", shared_snd_strerror);

        // Device enumeration and the rest FMOD asks for (Cruis'n Blast): one
        // playback device, "default", which the calls above play to.
        MAP("snd_device_name_hint", rtSndDeviceNameHint);
        MAP("snd_device_name_get_hint", rtSndDeviceNameGetHint);
        MAP("snd_device_name_free_hint", rtSndDeviceNameFreeHint);
        MAP("snd_pcm_hw_params_sizeof", rtSndHwParamsSizeof);
        MAP("snd_pcm_sw_params_sizeof", rtSndSwParamsSizeof);
        MAP("snd_pcm_hw_params_set_rate", rtSndHwParamsSetRate);
        MAP("snd_pcm_hw_params_test_rate", rtSndHwParamsTestRate);
        MAP("snd_pcm_nonblock", rtSndPcmNonblock);
        MAP("snd_pcm_readi", rtSndPcmReadi);
        MAP("snd_asoundlib_version", rtSndAsoundlibVersion);
    }
} // namespace Alsa2SdlBridge

#endif