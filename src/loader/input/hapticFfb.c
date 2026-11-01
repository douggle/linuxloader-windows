// Force feedback for the games whose wheel motor the loader drives itself
// (Raw Thrills, Namco: see evdevFfb.h), on Windows: DirectInput on the first
// attached controller with force feedback, in place of the evdev device the
// Linux build uses (evdevFfb.c). Same effects: a constant force, a damper (or
// friction) and a centering spring.
//
// Straight on DirectInput, not SDL3's haptics (which are DirectInput's too):
// through SDL every effect on a G29 failed ("Unable to create effect"), and
// SDL keeps the reason (the HRESULT) to itself. DirectInput is a COM object
// and its force feedback needs exclusive access, given to a window: one
// thread of its own makes the device, its window and every effect; the calls
// below hand it their latest values.

#if defined(_WIN32)

#define DIRECTINPUT_VERSION 0x0800
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <dinput.h>

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "evdevFfb.h"
#include "../log/log.h"

typedef struct
{
    const GUID *guid;
    IDirectInputEffect *effect;
    int level; // last level sent, INT_MIN: none
} Effect;

static const char *ffbWho = "FFB";

// The device and its effects: the force feedback thread's only.
static IDirectInput8A *dinput;
static IDirectInputDevice8A *device;
static HWND window;
static char deviceName[MAX_PATH];
static int ffbHasSpring, ffbHasDamper, ffbHasFriction;
static Effect constantEffect = {&GUID_ConstantForce, NULL, INT_MIN};
static Effect springEffect = {&GUID_Spring, NULL, INT_MIN};
static Effect damperEffect = {&GUID_Damper, NULL, INT_MIN};

// What the game asked for last (INT_MIN: nothing yet), for the thread.
enum
{
    WANT_CONSTANT,
    WANT_SPRING,
    WANT_DAMPER,
    WANTS
};
static volatile LONG wanted[WANTS] = {INT_MIN, INT_MIN, INT_MIN};
static HANDLE ffbThread, ffbWake, ffbOpened;
static volatile LONG ffbReady; // 1: a device is open, -1: none

static BOOL CALLBACK firstDevice(LPCDIDEVICEINSTANCEA instance, LPVOID ref)
{
    *(GUID *)ref = instance->guidInstance;
    snprintf(deviceName, sizeof(deviceName), "%s", instance->tszProductName);
    return DIENUM_STOP;
}

static BOOL CALLBACK effectSupported(LPCDIEFFECTINFOA info, LPVOID ref)
{
    (void)ref;
    if (IsEqualGUID(&info->guid, &GUID_Spring))
        ffbHasSpring = 1;
    else if (IsEqualGUID(&info->guid, &GUID_Damper))
        ffbHasDamper = 1;
    else if (IsEqualGUID(&info->guid, &GUID_Friction))
        ffbHasFriction = 1;
    return DIENUM_CONTINUE;
}

static void closeDevice(void)
{
    Effect *effects[] = {&constantEffect, &springEffect, &damperEffect};
    for (int i = 0; i < 3; i++)
        if (effects[i]->effect)
        {
            IDirectInputEffect_Release(effects[i]->effect);
            effects[i]->effect = NULL;
        }
    if (device)
    {
        IDirectInputDevice8_Unacquire(device);
        IDirectInputDevice8_Release(device);
        device = NULL;
    }
}

static int openDevice(void)
{
    GUID guid;
    HRESULT hr;

    hr = DirectInput8Create(GetModuleHandleA(NULL), DIRECTINPUT_VERSION, &IID_IDirectInput8A, (void **)&dinput, NULL);
    if (FAILED(hr))
    {
        log_warn("%s: no DirectInput (0x%08lx)", ffbWho, (unsigned long)hr);
        return 0;
    }
    memset(&guid, 0, sizeof(guid));
    hr = IDirectInput8_EnumDevices(dinput, DI8DEVCLASS_GAMECTRL, firstDevice, &guid,
                                   DIEDFL_ATTACHEDONLY | DIEDFL_FORCEFEEDBACK);
    if (FAILED(hr) || !deviceName[0])
    {
        log_warn("%s: no force feedback device", ffbWho);
        return 0;
    }
    hr = IDirectInput8_CreateDevice(dinput, &guid, &device, NULL);
    if (FAILED(hr))
    {
        log_error("%s: %s: CreateDevice failed (0x%08lx)", ffbWho, deviceName, (unsigned long)hr);
        return 0;
    }
    // Exclusive access is given to a top-level window: a hidden one of this
    // thread's, kept in the background mode so the game's focus does not
    // matter.
    window = CreateWindowExA(0, "STATIC", "linuxloader force feedback", WS_POPUP, 0, 0, 1, 1, NULL, NULL,
                             GetModuleHandleA(NULL), NULL);
    if ((hr = IDirectInputDevice8_SetDataFormat(device, &c_dfDIJoystick2)) != DI_OK ||
        (hr = IDirectInputDevice8_SetCooperativeLevel(device, window, DISCL_EXCLUSIVE | DISCL_BACKGROUND)) != DI_OK)
    {
        log_error("%s: %s: exclusive access refused (0x%08lx)", ffbWho, deviceName, (unsigned long)hr);
        closeDevice();
        return 0;
    }
    // The game centers the wheel itself.
    DIPROPDWORD autocenter;
    memset(&autocenter, 0, sizeof(autocenter));
    autocenter.diph.dwSize = sizeof(autocenter);
    autocenter.diph.dwHeaderSize = sizeof(DIPROPHEADER);
    autocenter.diph.dwHow = DIPH_DEVICE;
    autocenter.dwData = DIPROPAUTOCENTER_OFF;
    IDirectInputDevice8_SetProperty(device, DIPROP_AUTOCENTER, &autocenter.diph);
    hr = IDirectInputDevice8_Acquire(device);
    if (FAILED(hr))
    {
        log_error("%s: %s: Acquire failed (0x%08lx)", ffbWho, deviceName, (unsigned long)hr);
        closeDevice();
        return 0;
    }
    log_info("%s: %s acquired (0x%08lx)", ffbWho, deviceName, (unsigned long)hr);
    IDirectInputDevice8_EnumEffects(device, effectSupported, NULL, DIEFT_CONDITION);
    if (!ffbHasDamper && ffbHasFriction)
        damperEffect.guid = &GUID_Friction;
    printf("%s: force feedback (wheel%s%s) on %s\n", ffbWho, ffbHasSpring ? ", spring" : "",
           ffbHasDamper ? ", damper" : ffbHasFriction ? ", friction" : "", deviceName);
    return 1;
}

// The game's window: the process's visible top-level window that is not
// ours (SDL's, for the game).
static BOOL CALLBACK gameWindowOf(HWND hwnd, LPARAM ref)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId() || hwnd == window || !IsWindowVisible(hwnd))
        return TRUE;
    *(HWND *)ref = hwnd;
    return FALSE;
}

// Exclusive access refused on the hidden window: SDL's joystick holds the
// wheel exclusively too, with a window of its own. Tried again with the
// game's window, which is the one in front while it is played. 1 if taken.
static int takeGameWindow(void)
{
    static int tried;
    HWND game = NULL;
    HRESULT hr;

    if (tried)
        return 0;
    tried = 1;
    EnumWindows(gameWindowOf, (LPARAM)&game);
    if (!game)
        return 0;
    IDirectInputDevice8_Unacquire(device);
    hr = IDirectInputDevice8_SetCooperativeLevel(device, game, DISCL_EXCLUSIVE | DISCL_BACKGROUND);
    if (SUCCEEDED(hr))
        hr = IDirectInputDevice8_Acquire(device);
    log_info("%s: %s: exclusive access with the game's window (0x%08lx)", ffbWho, deviceName, (unsigned long)hr);
    return SUCCEEDED(hr);
}

// The effect's parameters on the steering axis (X): a constant force of
// level (positive pulling to the left), or a centered condition as hard both
// ways. Created once, then updated in place; plays until replaced.
static void play(Effect *e, int level)
{
    DWORD axes[1] = {DIJOFS_X};
    LONG direction[1] = {1}; // where the force comes from: the right
    DICONSTANTFORCE constant;
    DICONDITION condition;
    DIEFFECT params;
    HRESULT hr;

    if (level == e->level)
        return;
    memset(&params, 0, sizeof(params));
    params.dwSize = sizeof(params);
    params.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
    params.dwDuration = INFINITE;
    params.dwGain = DI_FFNOMINALMAX;
    params.dwTriggerButton = DIEB_NOTRIGGER;
    params.cAxes = 1;
    params.rgdwAxes = axes;
    params.rglDirection = direction;
    if (e == &constantEffect)
    {
        // DirectInput's -10000..10000 from -32767..32767.
        constant.lMagnitude = (LONG)level * DI_FFNOMINALMAX / 0x7fff;
        params.cbTypeSpecificParams = sizeof(constant);
        params.lpvTypeSpecificParams = &constant;
    }
    else
    {
        memset(&condition, 0, sizeof(condition));
        condition.lPositiveCoefficient = condition.lNegativeCoefficient = (LONG)level * DI_FFNOMINALMAX / 0x7fff;
        condition.dwPositiveSaturation = condition.dwNegativeSaturation = DI_FFNOMINALMAX;
        params.cbTypeSpecificParams = sizeof(condition);
        params.lpvTypeSpecificParams = &condition;
    }

    if (!e->effect)
    {
        hr = IDirectInputDevice8_CreateEffect(device, e->guid, &params, &e->effect, NULL);
        if (hr == DIERR_NOTEXCLUSIVEACQUIRED && takeGameWindow())
            hr = IDirectInputDevice8_CreateEffect(device, e->guid, &params, &e->effect, NULL);
        if (FAILED(hr))
        {
            static int reported;
            if (reported++ < 3)
                log_error("%s: %s: force feedback effect failed (0x%08lx)", ffbWho, deviceName, (unsigned long)hr);
            e->effect = NULL;
            return;
        }
        hr = IDirectInputEffect_Start(e->effect, 1, 0);
    }
    else
        hr = IDirectInputEffect_SetParameters(e->effect, &params, DIEP_TYPESPECIFICPARAMS | DIEP_START);
    if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED || hr == DIERR_NOTEXCLUSIVEACQUIRED)
    {
        // Taken away (another program, the device replugged): taken back,
        // and sent again next time.
        IDirectInputDevice8_Acquire(device);
        return;
    }
    if (FAILED(hr))
    {
        static int reported;
        if (reported++ < 3)
            log_error("%s: %s: force feedback update failed (0x%08lx)", ffbWho, deviceName, (unsigned long)hr);
        return;
    }
    e->level = level;
}

static DWORD WINAPI ffbMain(LPVOID arg)
{
    (void)arg;
    HRESULT com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    int open = openDevice();
    InterlockedExchange(&ffbReady, open ? 1 : -1);
    SetEvent(ffbOpened);
    while (open)
    {
        // The window's messages (DirectInput's), and the game's new values.
        MsgWaitForMultipleObjects(1, &ffbWake, FALSE, 50, QS_ALLINPUT);
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE))
            DispatchMessageA(&msg);
        LONG constant = wanted[WANT_CONSTANT], spring = wanted[WANT_SPRING], damper = wanted[WANT_DAMPER];
        if (constant != INT_MIN)
            play(&constantEffect, (int)constant);
        if (spring != INT_MIN && ffbHasSpring)
            play(&springEffect, (int)spring);
        if (damper != INT_MIN && (ffbHasDamper || ffbHasFriction))
            play(&damperEffect, (int)damper);
    }
    closeDevice();
    if (SUCCEEDED(com))
        CoUninitialize();
    return 0;
}

int evdevFfbOpen(const char *who)
{
    if (!ffbThread)
    {
        ffbWho = who;
        ffbWake = CreateEventA(NULL, FALSE, FALSE, NULL);
        ffbOpened = CreateEventA(NULL, TRUE, FALSE, NULL);
        ffbThread = CreateThread(NULL, 0, ffbMain, NULL, 0, NULL);
        if (!ffbThread)
        {
            log_error("%s: force feedback thread not started", ffbWho);
            return 0;
        }
        WaitForSingleObject(ffbOpened, 5000);
    }
    return ffbReady == 1;
}

int evdevFfbOpenPath(const char *who, const char *path)
{
    (void)path; // a Linux device path: Windows has the first device instead
    return evdevFfbOpen(who);
}

int evdevFfbWheel(void)
{
    return ffbReady == 1;
}

static float clamp(float v, float lo, float hi)
{
    if (!(v >= lo)) // NaN too
        return v > hi ? hi : lo;
    return v > hi ? hi : v;
}

static void want(int which, int level)
{
    if (ffbReady != 1 || InterlockedExchange(&wanted[which], level) == level)
        return;
    SetEvent(ffbWake);
}

void evdevFfbConstant(float force)
{
    want(WANT_CONSTANT, (int)lrintf(clamp(force, -1.0f, 1.0f) * 0x7fff));
}

void evdevFfbSpring(float strength)
{
    want(WANT_SPRING, (int)lrintf(clamp(strength, 0.0f, 1.0f) * 0x7fff));
}

void evdevFfbDamper(float strength)
{
    want(WANT_DAMPER, (int)lrintf(clamp(strength, 0.0f, 1.0f) * 0x7fff));
}

#endif
