// The SDL3 the loader links (statically), by name: what a Linux
// sdl2-compat (Cruis'n Blast's libSDL2-2.0.so.0, under its sdl12-compat)
// finds when it opens libSDL3.so.0 (see redirections/libcShared.c).
// SDL3's functions are cdecl and its structures have the same layout on
// both systems. Generated from the SDL3 headers for Windows; listing them
// here also links all of them in.

#if defined(_WIN32) || defined(__MINGW32__)

#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "sdl3Exports.h"
#include "linuxCallback.h"
#include "gamePath.h"
#include "../elfLoader/glHooks.hpp"
#include "../rawthrills/rawthrills.h"
#include "../log/log.h"

static bool SDLCALL setHint(const char *name, const char *value);
static bool SDLCALL setHintWithPriority(const char *name, const char *value, SDL_HintPriority priority);
static int SDLCALL setenvUnsafe(const char *name, const char *value, int overwrite);
static bool SDLCALL setEnvironmentVariable(SDL_Environment *env, const char *name, const char *value, bool overwrite);

static SDL_FunctionPointer SDLCALL glGetProcAddress(const char *proc);
static SDL_GLContext SDLCALL glCreateContext(SDL_Window *window);
static SDL_IOStream *SDLCALL ioFromFile(const char *file, const char *mode);
static void *SDLCALL loadFile(const char *file, size_t *datasize);
static bool SDLCALL saveFile(const char *file, const void *data, size_t datasize);
static bool SDLCALL getPathInfo(const char *path, SDL_PathInfo *info);

// Callbacks: see the definitions below.
static bool SDLCALL addEventWatch(SDL_EventFilter filter, void *userdata);
static void SDLCALL removeEventWatch(SDL_EventFilter filter, void *userdata);
static void SDLCALL setEventFilter(SDL_EventFilter filter, void *userdata);
static bool SDLCALL getEventFilter(SDL_EventFilter *filter, void **userdata);
static void SDLCALL filterEvents(SDL_EventFilter filter, void *userdata);
static bool SDLCALL addHintCallback(const char *name, SDL_HintCallback callback, void *userdata);
static void SDLCALL removeHintCallback(const char *name, SDL_HintCallback callback, void *userdata);
static SDL_TimerID SDLCALL addTimer(Uint32 interval, SDL_TimerCallback callback, void *userdata);
static SDL_TimerID SDLCALL addTimerNS(Uint64 interval, SDL_NSTimerCallback callback, void *userdata);
static SDL_Thread *SDLCALL createThreadRuntime(SDL_ThreadFunction fn, const char *name, void *data,
                                               SDL_FunctionPointer pfnBeginThread, SDL_FunctionPointer pfnEndThread);
static SDL_Thread *SDLCALL createThreadWithPropertiesRuntime(SDL_PropertiesID props, SDL_FunctionPointer pfnBeginThread,
                                                             SDL_FunctionPointer pfnEndThread);
static void SDLCALL setLogOutputFunction(SDL_LogOutputFunction callback, void *userdata);
static void SDLCALL getLogOutputFunction(SDL_LogOutputFunction *callback, void **userdata);
static SDL_AudioStream *SDLCALL openAudioDeviceStream(SDL_AudioDeviceID devid, const SDL_AudioSpec *spec,
                                                      SDL_AudioStreamCallback callback, void *userdata);
static bool SDLCALL setAudioStreamGetCallback(SDL_AudioStream *stream, SDL_AudioStreamCallback callback, void *userdata);
static bool SDLCALL setAudioStreamPutCallback(SDL_AudioStream *stream, SDL_AudioStreamCallback callback, void *userdata);
static bool SDLCALL setAudioPostmixCallback(SDL_AudioDeviceID devid, SDL_AudioPostmixCallback callback, void *userdata);
static bool SDLCALL setWindowHitTest(SDL_Window *window, SDL_HitTest callback, void *callback_data);
static bool SDLCALL runOnMainThread(SDL_MainThreadCallback callback, void *userdata, bool wait_complete);
static void SDLCALL qsortAligned(void *base, size_t nmemb, size_t size, SDL_CompareCallback compare);
static void SDLCALL qsortRAligned(void *base, size_t nmemb, size_t size, SDL_CompareCallback_r compare, void *userdata);
static void *SDLCALL bsearchAligned(const void *key, const void *base, size_t nmemb, size_t size,
                                    SDL_CompareCallback compare);
static void *SDLCALL bsearchRAligned(const void *key, const void *base, size_t nmemb, size_t size,
                                     SDL_CompareCallback_r compare, void *userdata);

typedef struct
{
    const char *name;
    void *address;
} Sdl3Export;

static const Sdl3Export sdl3Exports[] = {
    {"SDL_AcquireCameraFrame", (void *)(SDL_AcquireCameraFrame)},
    {"SDL_AcquireGPUCommandBuffer", (void *)(SDL_AcquireGPUCommandBuffer)},
    {"SDL_AcquireGPUSwapchainTexture", (void *)(SDL_AcquireGPUSwapchainTexture)},
    {"SDL_AddAtomicInt", (void *)(SDL_AddAtomicInt)},
    {"SDL_AddAtomicU32", (void *)(SDL_AddAtomicU32)},
    {"SDL_AddEventWatch", (void *)(addEventWatch)}, // callback: see linuxCallback.h
    {"SDL_AddGamepadMapping", (void *)(SDL_AddGamepadMapping)},
    {"SDL_AddGamepadMappingsFromFile", (void *)(SDL_AddGamepadMappingsFromFile)},
    {"SDL_AddGamepadMappingsFromIO", (void *)(SDL_AddGamepadMappingsFromIO)},
    {"SDL_AddHintCallback", (void *)(addHintCallback)}, // callback: see linuxCallback.h
    {"SDL_AddSurfaceAlternateImage", (void *)(SDL_AddSurfaceAlternateImage)},
    {"SDL_AddTimer", (void *)(addTimer)}, // callback: see linuxCallback.h
    {"SDL_AddTimerNS", (void *)(addTimerNS)}, // callback: see linuxCallback.h
    {"SDL_AddVulkanRenderSemaphores", (void *)(SDL_AddVulkanRenderSemaphores)},
    {"SDL_AsyncIOFromFile", (void *)(SDL_AsyncIOFromFile)},
    {"SDL_AttachVirtualJoystick", (void *)(SDL_AttachVirtualJoystick)},
    {"SDL_AudioDevicePaused", (void *)(SDL_AudioDevicePaused)},
    {"SDL_AudioStreamDevicePaused", (void *)(SDL_AudioStreamDevicePaused)},
    {"SDL_BeginGPUComputePass", (void *)(SDL_BeginGPUComputePass)},
    {"SDL_BeginGPUCopyPass", (void *)(SDL_BeginGPUCopyPass)},
    {"SDL_BeginGPURenderPass", (void *)(SDL_BeginGPURenderPass)},
    {"SDL_BindAudioStream", (void *)(SDL_BindAudioStream)},
    {"SDL_BindAudioStreams", (void *)(SDL_BindAudioStreams)},
    {"SDL_BindGPUComputePipeline", (void *)(SDL_BindGPUComputePipeline)},
    {"SDL_BindGPUComputeSamplers", (void *)(SDL_BindGPUComputeSamplers)},
    {"SDL_BindGPUComputeStorageBuffers", (void *)(SDL_BindGPUComputeStorageBuffers)},
    {"SDL_BindGPUComputeStorageTextures", (void *)(SDL_BindGPUComputeStorageTextures)},
    {"SDL_BindGPUFragmentSamplers", (void *)(SDL_BindGPUFragmentSamplers)},
    {"SDL_BindGPUFragmentStorageBuffers", (void *)(SDL_BindGPUFragmentStorageBuffers)},
    {"SDL_BindGPUFragmentStorageTextures", (void *)(SDL_BindGPUFragmentStorageTextures)},
    {"SDL_BindGPUGraphicsPipeline", (void *)(SDL_BindGPUGraphicsPipeline)},
    {"SDL_BindGPUIndexBuffer", (void *)(SDL_BindGPUIndexBuffer)},
    {"SDL_BindGPUVertexBuffers", (void *)(SDL_BindGPUVertexBuffers)},
    {"SDL_BindGPUVertexSamplers", (void *)(SDL_BindGPUVertexSamplers)},
    {"SDL_BindGPUVertexStorageBuffers", (void *)(SDL_BindGPUVertexStorageBuffers)},
    {"SDL_BindGPUVertexStorageTextures", (void *)(SDL_BindGPUVertexStorageTextures)},
    {"SDL_BlitGPUTexture", (void *)(SDL_BlitGPUTexture)},
    {"SDL_BlitSurface", (void *)(SDL_BlitSurface)},
    {"SDL_BlitSurface9Grid", (void *)(SDL_BlitSurface9Grid)},
    {"SDL_BlitSurfaceScaled", (void *)(SDL_BlitSurfaceScaled)},
    {"SDL_BlitSurfaceTiled", (void *)(SDL_BlitSurfaceTiled)},
    {"SDL_BlitSurfaceTiledWithScale", (void *)(SDL_BlitSurfaceTiledWithScale)},
    {"SDL_BlitSurfaceUnchecked", (void *)(SDL_BlitSurfaceUnchecked)},
    {"SDL_BlitSurfaceUncheckedScaled", (void *)(SDL_BlitSurfaceUncheckedScaled)},
    {"SDL_BroadcastCondition", (void *)(SDL_BroadcastCondition)},
    {"SDL_CalculateGPUTextureFormatSize", (void *)(SDL_CalculateGPUTextureFormatSize)},
    {"SDL_CancelGPUCommandBuffer", (void *)(SDL_CancelGPUCommandBuffer)},
    {"SDL_CaptureMouse", (void *)(SDL_CaptureMouse)},
    {"SDL_ClaimWindowForGPUDevice", (void *)(SDL_ClaimWindowForGPUDevice)},
    {"SDL_CleanupTLS", (void *)(SDL_CleanupTLS)},
    {"SDL_ClearAudioStream", (void *)(SDL_ClearAudioStream)},
    {"SDL_ClearClipboardData", (void *)(SDL_ClearClipboardData)},
    {"SDL_ClearComposition", (void *)(SDL_ClearComposition)},
    {"SDL_ClearError", (void *)(SDL_ClearError)},
    {"SDL_ClearProperty", (void *)(SDL_ClearProperty)},
    {"SDL_ClearSurface", (void *)(SDL_ClearSurface)},
    {"SDL_ClickTrayEntry", (void *)(SDL_ClickTrayEntry)},
    {"SDL_CloseAsyncIO", (void *)(SDL_CloseAsyncIO)},
    {"SDL_CloseAudioDevice", (void *)(SDL_CloseAudioDevice)},
    {"SDL_CloseCamera", (void *)(SDL_CloseCamera)},
    {"SDL_CloseGamepad", (void *)(SDL_CloseGamepad)},
    {"SDL_CloseHaptic", (void *)(SDL_CloseHaptic)},
    {"SDL_CloseIO", (void *)(SDL_CloseIO)},
    {"SDL_CloseJoystick", (void *)(SDL_CloseJoystick)},
    {"SDL_CloseSensor", (void *)(SDL_CloseSensor)},
    {"SDL_CloseStorage", (void *)(SDL_CloseStorage)},
    {"SDL_CompareAndSwapAtomicInt", (void *)(SDL_CompareAndSwapAtomicInt)},
    {"SDL_CompareAndSwapAtomicPointer", (void *)(SDL_CompareAndSwapAtomicPointer)},
    {"SDL_CompareAndSwapAtomicU32", (void *)(SDL_CompareAndSwapAtomicU32)},
    {"SDL_ComposeCustomBlendMode", (void *)(SDL_ComposeCustomBlendMode)},
    {"SDL_ConvertAudioSamples", (void *)(SDL_ConvertAudioSamples)},
    {"SDL_ConvertEventToRenderCoordinates", (void *)(SDL_ConvertEventToRenderCoordinates)},
    {"SDL_ConvertPixels", (void *)(SDL_ConvertPixels)},
    {"SDL_ConvertPixelsAndColorspace", (void *)(SDL_ConvertPixelsAndColorspace)},
    {"SDL_ConvertSurface", (void *)(SDL_ConvertSurface)},
    {"SDL_ConvertSurfaceAndColorspace", (void *)(SDL_ConvertSurfaceAndColorspace)},
    {"SDL_CopyFile", (void *)(SDL_CopyFile)},
    {"SDL_CopyGPUBufferToBuffer", (void *)(SDL_CopyGPUBufferToBuffer)},
    {"SDL_CopyGPUTextureToTexture", (void *)(SDL_CopyGPUTextureToTexture)},
    {"SDL_CopyProperties", (void *)(SDL_CopyProperties)},
    {"SDL_CopyStorageFile", (void *)(SDL_CopyStorageFile)},
    {"SDL_CreateAnimatedCursor", (void *)(SDL_CreateAnimatedCursor)},
    {"SDL_CreateAsyncIOQueue", (void *)(SDL_CreateAsyncIOQueue)},
    {"SDL_CreateAudioStream", (void *)(SDL_CreateAudioStream)},
    {"SDL_CreateColorCursor", (void *)(SDL_CreateColorCursor)},
    {"SDL_CreateCondition", (void *)(SDL_CreateCondition)},
    {"SDL_CreateCursor", (void *)(SDL_CreateCursor)},
    {"SDL_CreateDirectory", (void *)(SDL_CreateDirectory)},
    {"SDL_CreateEnvironment", (void *)(SDL_CreateEnvironment)},
    {"SDL_CreateGPUBuffer", (void *)(SDL_CreateGPUBuffer)},
    {"SDL_CreateGPUComputePipeline", (void *)(SDL_CreateGPUComputePipeline)},
    {"SDL_CreateGPUDevice", (void *)(SDL_CreateGPUDevice)},
    {"SDL_CreateGPUDeviceWithProperties", (void *)(SDL_CreateGPUDeviceWithProperties)},
    {"SDL_CreateGPUGraphicsPipeline", (void *)(SDL_CreateGPUGraphicsPipeline)},
    {"SDL_CreateGPURenderState", (void *)(SDL_CreateGPURenderState)},
    {"SDL_CreateGPURenderer", (void *)(SDL_CreateGPURenderer)},
    {"SDL_CreateGPUSampler", (void *)(SDL_CreateGPUSampler)},
    {"SDL_CreateGPUShader", (void *)(SDL_CreateGPUShader)},
    {"SDL_CreateGPUTexture", (void *)(SDL_CreateGPUTexture)},
    {"SDL_CreateGPUTransferBuffer", (void *)(SDL_CreateGPUTransferBuffer)},
    {"SDL_CreateHapticEffect", (void *)(SDL_CreateHapticEffect)},
    {"SDL_CreateMutex", (void *)(SDL_CreateMutex)},
    {"SDL_CreatePalette", (void *)(SDL_CreatePalette)},
    {"SDL_CreatePopupWindow", (void *)(SDL_CreatePopupWindow)},
    {"SDL_CreateProcess", (void *)(SDL_CreateProcess)},
    {"SDL_CreateProcessWithProperties", (void *)(SDL_CreateProcessWithProperties)},
    {"SDL_CreateProperties", (void *)(SDL_CreateProperties)},
    {"SDL_CreateRWLock", (void *)(SDL_CreateRWLock)},
    {"SDL_CreateRenderer", (void *)(SDL_CreateRenderer)},
    {"SDL_CreateRendererWithProperties", (void *)(SDL_CreateRendererWithProperties)},
    {"SDL_CreateSemaphore", (void *)(SDL_CreateSemaphore)},
    {"SDL_CreateSoftwareRenderer", (void *)(SDL_CreateSoftwareRenderer)},
    {"SDL_CreateStorageDirectory", (void *)(SDL_CreateStorageDirectory)},
    {"SDL_CreateSurface", (void *)(SDL_CreateSurface)},
    {"SDL_CreateSurfaceFrom", (void *)(SDL_CreateSurfaceFrom)},
    {"SDL_CreateSurfacePalette", (void *)(SDL_CreateSurfacePalette)},
    {"SDL_CreateSystemCursor", (void *)(SDL_CreateSystemCursor)},
    {"SDL_CreateTexture", (void *)(SDL_CreateTexture)},
    {"SDL_CreateTextureFromSurface", (void *)(SDL_CreateTextureFromSurface)},
    {"SDL_CreateTextureWithProperties", (void *)(SDL_CreateTextureWithProperties)},
    {"SDL_CreateThreadRuntime", (void *)(createThreadRuntime)}, // callback: see linuxCallback.h
    {"SDL_CreateThreadWithPropertiesRuntime", (void *)(createThreadWithPropertiesRuntime)}, // callback: see linuxCallback.h
    {"SDL_CreateTray", (void *)(SDL_CreateTray)},
    {"SDL_CreateTrayMenu", (void *)(SDL_CreateTrayMenu)},
    {"SDL_CreateTraySubmenu", (void *)(SDL_CreateTraySubmenu)},
    {"SDL_CreateTrayWithProperties", (void *)(SDL_CreateTrayWithProperties)},
    {"SDL_CreateWindow", (void *)(SDL_CreateWindow)},
    {"SDL_CreateWindowAndRenderer", (void *)(SDL_CreateWindowAndRenderer)},
    {"SDL_CreateWindowWithProperties", (void *)(SDL_CreateWindowWithProperties)},
    {"SDL_CursorVisible", (void *)(SDL_CursorVisible)},
    {"SDL_DateTimeToTime", (void *)(SDL_DateTimeToTime)},
    {"SDL_Delay", (void *)(SDL_Delay)},
    {"SDL_DelayNS", (void *)(SDL_DelayNS)},
    {"SDL_DelayPrecise", (void *)(SDL_DelayPrecise)},
    {"SDL_DestroyAsyncIOQueue", (void *)(SDL_DestroyAsyncIOQueue)},
    {"SDL_DestroyAudioStream", (void *)(SDL_DestroyAudioStream)},
    {"SDL_DestroyCondition", (void *)(SDL_DestroyCondition)},
    {"SDL_DestroyCursor", (void *)(SDL_DestroyCursor)},
    {"SDL_DestroyEnvironment", (void *)(SDL_DestroyEnvironment)},
    {"SDL_DestroyGPUDevice", (void *)(SDL_DestroyGPUDevice)},
    {"SDL_DestroyGPURenderState", (void *)(SDL_DestroyGPURenderState)},
    {"SDL_DestroyHapticEffect", (void *)(SDL_DestroyHapticEffect)},
    {"SDL_DestroyMutex", (void *)(SDL_DestroyMutex)},
    {"SDL_DestroyPalette", (void *)(SDL_DestroyPalette)},
    {"SDL_DestroyProcess", (void *)(SDL_DestroyProcess)},
    {"SDL_DestroyProperties", (void *)(SDL_DestroyProperties)},
    {"SDL_DestroyRWLock", (void *)(SDL_DestroyRWLock)},
    {"SDL_DestroyRenderer", (void *)(SDL_DestroyRenderer)},
    {"SDL_DestroySemaphore", (void *)(SDL_DestroySemaphore)},
    {"SDL_DestroySurface", (void *)(SDL_DestroySurface)},
    {"SDL_DestroyTexture", (void *)(SDL_DestroyTexture)},
    {"SDL_DestroyTray", (void *)(SDL_DestroyTray)},
    {"SDL_DestroyWindow", (void *)(SDL_DestroyWindow)},
    {"SDL_DestroyWindowSurface", (void *)(SDL_DestroyWindowSurface)},
    {"SDL_DetachThread", (void *)(SDL_DetachThread)},
    {"SDL_DetachVirtualJoystick", (void *)(SDL_DetachVirtualJoystick)},
    {"SDL_DisableScreenSaver", (void *)(SDL_DisableScreenSaver)},
    {"SDL_DispatchGPUCompute", (void *)(SDL_DispatchGPUCompute)},
    {"SDL_DispatchGPUComputeIndirect", (void *)(SDL_DispatchGPUComputeIndirect)},
    {"SDL_DownloadFromGPUBuffer", (void *)(SDL_DownloadFromGPUBuffer)},
    {"SDL_DownloadFromGPUTexture", (void *)(SDL_DownloadFromGPUTexture)},
    {"SDL_DrawGPUIndexedPrimitives", (void *)(SDL_DrawGPUIndexedPrimitives)},
    {"SDL_DrawGPUIndexedPrimitivesIndirect", (void *)(SDL_DrawGPUIndexedPrimitivesIndirect)},
    {"SDL_DrawGPUPrimitives", (void *)(SDL_DrawGPUPrimitives)},
    {"SDL_DrawGPUPrimitivesIndirect", (void *)(SDL_DrawGPUPrimitivesIndirect)},
    {"SDL_DuplicateSurface", (void *)(SDL_DuplicateSurface)},
    {"SDL_EGL_GetCurrentConfig", (void *)(SDL_EGL_GetCurrentConfig)},
    {"SDL_EGL_GetCurrentDisplay", (void *)(SDL_EGL_GetCurrentDisplay)},
    {"SDL_EGL_GetProcAddress", (void *)(SDL_EGL_GetProcAddress)},
    {"SDL_EGL_GetWindowSurface", (void *)(SDL_EGL_GetWindowSurface)},
    {"SDL_EGL_SetAttributeCallbacks", (void *)(SDL_EGL_SetAttributeCallbacks)},
    {"SDL_EnableScreenSaver", (void *)(SDL_EnableScreenSaver)},
    {"SDL_EndGPUComputePass", (void *)(SDL_EndGPUComputePass)},
    {"SDL_EndGPUCopyPass", (void *)(SDL_EndGPUCopyPass)},
    {"SDL_EndGPURenderPass", (void *)(SDL_EndGPURenderPass)},
    {"SDL_EnumerateDirectory", (void *)(SDL_EnumerateDirectory)},
    {"SDL_EnumerateProperties", (void *)(SDL_EnumerateProperties)},
    {"SDL_EnumerateStorageDirectory", (void *)(SDL_EnumerateStorageDirectory)},
    {"SDL_EventEnabled", (void *)(SDL_EventEnabled)},
    {"SDL_FillSurfaceRect", (void *)(SDL_FillSurfaceRect)},
    {"SDL_FillSurfaceRects", (void *)(SDL_FillSurfaceRects)},
    {"SDL_FilterEvents", (void *)(filterEvents)}, // callback: see linuxCallback.h
    {"SDL_FlashWindow", (void *)(SDL_FlashWindow)},
    {"SDL_FlipSurface", (void *)(SDL_FlipSurface)},
    {"SDL_FlushAudioStream", (void *)(SDL_FlushAudioStream)},
    {"SDL_FlushEvent", (void *)(SDL_FlushEvent)},
    {"SDL_FlushEvents", (void *)(SDL_FlushEvents)},
    {"SDL_FlushIO", (void *)(SDL_FlushIO)},
    {"SDL_FlushRenderer", (void *)(SDL_FlushRenderer)},
    {"SDL_GL_CreateContext", (void *)(glCreateContext)}, // LL_GL_DEBUG: see below
    {"SDL_GL_DestroyContext", (void *)(SDL_GL_DestroyContext)},
    {"SDL_GL_ExtensionSupported", (void *)(SDL_GL_ExtensionSupported)},
    {"SDL_GL_GetAttribute", (void *)(SDL_GL_GetAttribute)},
    {"SDL_GL_GetCurrentContext", (void *)(SDL_GL_GetCurrentContext)},
    {"SDL_GL_GetCurrentWindow", (void *)(SDL_GL_GetCurrentWindow)},
    {"SDL_GL_GetProcAddress", (void *)(glGetProcAddress)}, // the cdecl wrappers: see below
    {"SDL_GL_GetSwapInterval", (void *)(SDL_GL_GetSwapInterval)},
    {"SDL_GL_LoadLibrary", (void *)(SDL_GL_LoadLibrary)},
    {"SDL_GL_MakeCurrent", (void *)(SDL_GL_MakeCurrent)},
    {"SDL_GL_ResetAttributes", (void *)(SDL_GL_ResetAttributes)},
    {"SDL_GL_SetAttribute", (void *)(SDL_GL_SetAttribute)},
    {"SDL_GL_SetSwapInterval", (void *)(SDL_GL_SetSwapInterval)},
    {"SDL_GL_SwapWindow", (void *)(SDL_GL_SwapWindow)},
    {"SDL_GL_UnloadLibrary", (void *)(SDL_GL_UnloadLibrary)},
    {"SDL_GPUSupportsProperties", (void *)(SDL_GPUSupportsProperties)},
    {"SDL_GPUSupportsShaderFormats", (void *)(SDL_GPUSupportsShaderFormats)},
    {"SDL_GPUTextureFormatTexelBlockSize", (void *)(SDL_GPUTextureFormatTexelBlockSize)},
    {"SDL_GPUTextureSupportsFormat", (void *)(SDL_GPUTextureSupportsFormat)},
    {"SDL_GPUTextureSupportsSampleCount", (void *)(SDL_GPUTextureSupportsSampleCount)},
    {"SDL_GUIDToString", (void *)(SDL_GUIDToString)},
    {"SDL_GamepadConnected", (void *)(SDL_GamepadConnected)},
    {"SDL_GamepadEventsEnabled", (void *)(SDL_GamepadEventsEnabled)},
    {"SDL_GamepadHasAxis", (void *)(SDL_GamepadHasAxis)},
    {"SDL_GamepadHasButton", (void *)(SDL_GamepadHasButton)},
    {"SDL_GamepadHasSensor", (void *)(SDL_GamepadHasSensor)},
    {"SDL_GamepadSensorEnabled", (void *)(SDL_GamepadSensorEnabled)},
    {"SDL_GenerateMipmapsForGPUTexture", (void *)(SDL_GenerateMipmapsForGPUTexture)},
    {"SDL_GetAppMetadataProperty", (void *)(SDL_GetAppMetadataProperty)},
    {"SDL_GetAssertionHandler", (void *)(SDL_GetAssertionHandler)},
    {"SDL_GetAssertionReport", (void *)(SDL_GetAssertionReport)},
    {"SDL_GetAsyncIOResult", (void *)(SDL_GetAsyncIOResult)},
    {"SDL_GetAsyncIOSize", (void *)(SDL_GetAsyncIOSize)},
    {"SDL_GetAtomicInt", (void *)(SDL_GetAtomicInt)},
    {"SDL_GetAtomicPointer", (void *)(SDL_GetAtomicPointer)},
    {"SDL_GetAtomicU32", (void *)(SDL_GetAtomicU32)},
    {"SDL_GetAudioDeviceChannelMap", (void *)(SDL_GetAudioDeviceChannelMap)},
    {"SDL_GetAudioDeviceFormat", (void *)(SDL_GetAudioDeviceFormat)},
    {"SDL_GetAudioDeviceGain", (void *)(SDL_GetAudioDeviceGain)},
    {"SDL_GetAudioDeviceName", (void *)(SDL_GetAudioDeviceName)},
    {"SDL_GetAudioDriver", (void *)(SDL_GetAudioDriver)},
    {"SDL_GetAudioFormatName", (void *)(SDL_GetAudioFormatName)},
    {"SDL_GetAudioPlaybackDevices", (void *)(SDL_GetAudioPlaybackDevices)},
    {"SDL_GetAudioRecordingDevices", (void *)(SDL_GetAudioRecordingDevices)},
    {"SDL_GetAudioStreamAvailable", (void *)(SDL_GetAudioStreamAvailable)},
    {"SDL_GetAudioStreamData", (void *)(SDL_GetAudioStreamData)},
    {"SDL_GetAudioStreamDevice", (void *)(SDL_GetAudioStreamDevice)},
    {"SDL_GetAudioStreamFormat", (void *)(SDL_GetAudioStreamFormat)},
    {"SDL_GetAudioStreamFrequencyRatio", (void *)(SDL_GetAudioStreamFrequencyRatio)},
    {"SDL_GetAudioStreamGain", (void *)(SDL_GetAudioStreamGain)},
    {"SDL_GetAudioStreamInputChannelMap", (void *)(SDL_GetAudioStreamInputChannelMap)},
    {"SDL_GetAudioStreamOutputChannelMap", (void *)(SDL_GetAudioStreamOutputChannelMap)},
    {"SDL_GetAudioStreamProperties", (void *)(SDL_GetAudioStreamProperties)},
    {"SDL_GetAudioStreamQueued", (void *)(SDL_GetAudioStreamQueued)},
    {"SDL_GetBasePath", (void *)(SDL_GetBasePath)},
    {"SDL_GetBooleanProperty", (void *)(SDL_GetBooleanProperty)},
    {"SDL_GetCPUCacheLineSize", (void *)(SDL_GetCPUCacheLineSize)},
    {"SDL_GetCameraDriver", (void *)(SDL_GetCameraDriver)},
    {"SDL_GetCameraFormat", (void *)(SDL_GetCameraFormat)},
    {"SDL_GetCameraID", (void *)(SDL_GetCameraID)},
    {"SDL_GetCameraName", (void *)(SDL_GetCameraName)},
    {"SDL_GetCameraPermissionState", (void *)(SDL_GetCameraPermissionState)},
    {"SDL_GetCameraPosition", (void *)(SDL_GetCameraPosition)},
    {"SDL_GetCameraProperties", (void *)(SDL_GetCameraProperties)},
    {"SDL_GetCameraSupportedFormats", (void *)(SDL_GetCameraSupportedFormats)},
    {"SDL_GetCameras", (void *)(SDL_GetCameras)},
    {"SDL_GetClipboardData", (void *)(SDL_GetClipboardData)},
    {"SDL_GetClipboardMimeTypes", (void *)(SDL_GetClipboardMimeTypes)},
    {"SDL_GetClipboardText", (void *)(SDL_GetClipboardText)},
    {"SDL_GetClosestFullscreenDisplayMode", (void *)(SDL_GetClosestFullscreenDisplayMode)},
    {"SDL_GetCurrentAudioDriver", (void *)(SDL_GetCurrentAudioDriver)},
    {"SDL_GetCurrentCameraDriver", (void *)(SDL_GetCurrentCameraDriver)},
    {"SDL_GetCurrentDirectory", (void *)(SDL_GetCurrentDirectory)},
    {"SDL_GetCurrentDisplayMode", (void *)(SDL_GetCurrentDisplayMode)},
    {"SDL_GetCurrentDisplayOrientation", (void *)(SDL_GetCurrentDisplayOrientation)},
    {"SDL_GetCurrentRenderOutputSize", (void *)(SDL_GetCurrentRenderOutputSize)},
    {"SDL_GetCurrentThreadID", (void *)(SDL_GetCurrentThreadID)},
    {"SDL_GetCurrentTime", (void *)(SDL_GetCurrentTime)},
    {"SDL_GetCurrentVideoDriver", (void *)(SDL_GetCurrentVideoDriver)},
    {"SDL_GetCursor", (void *)(SDL_GetCursor)},
    {"SDL_GetDXGIOutputInfo", (void *)(SDL_GetDXGIOutputInfo)},
    {"SDL_GetDateTimeLocalePreferences", (void *)(SDL_GetDateTimeLocalePreferences)},
    {"SDL_GetDayOfWeek", (void *)(SDL_GetDayOfWeek)},
    {"SDL_GetDayOfYear", (void *)(SDL_GetDayOfYear)},
    {"SDL_GetDaysInMonth", (void *)(SDL_GetDaysInMonth)},
    {"SDL_GetDefaultAssertionHandler", (void *)(SDL_GetDefaultAssertionHandler)},
    {"SDL_GetDefaultCursor", (void *)(SDL_GetDefaultCursor)},
    {"SDL_GetDefaultLogOutputFunction", (void *)(SDL_GetDefaultLogOutputFunction)},
    {"SDL_GetDefaultTextureScaleMode", (void *)(SDL_GetDefaultTextureScaleMode)},
    {"SDL_GetDesktopDisplayMode", (void *)(SDL_GetDesktopDisplayMode)},
    {"SDL_GetDirect3D9AdapterIndex", (void *)(SDL_GetDirect3D9AdapterIndex)},
    {"SDL_GetDisplayBounds", (void *)(SDL_GetDisplayBounds)},
    {"SDL_GetDisplayContentScale", (void *)(SDL_GetDisplayContentScale)},
    {"SDL_GetDisplayForPoint", (void *)(SDL_GetDisplayForPoint)},
    {"SDL_GetDisplayForRect", (void *)(SDL_GetDisplayForRect)},
    {"SDL_GetDisplayForWindow", (void *)(SDL_GetDisplayForWindow)},
    {"SDL_GetDisplayName", (void *)(SDL_GetDisplayName)},
    {"SDL_GetDisplayProperties", (void *)(SDL_GetDisplayProperties)},
    {"SDL_GetDisplayUsableBounds", (void *)(SDL_GetDisplayUsableBounds)},
    {"SDL_GetDisplays", (void *)(SDL_GetDisplays)},
    {"SDL_GetEnvironment", (void *)(SDL_GetEnvironment)},
    {"SDL_GetEnvironmentVariable", (void *)(SDL_GetEnvironmentVariable)},
    {"SDL_GetEnvironmentVariables", (void *)(SDL_GetEnvironmentVariables)},
    {"SDL_GetError", (void *)(SDL_GetError)},
    {"SDL_GetEventDescription", (void *)(SDL_GetEventDescription)},
    {"SDL_GetEventFilter", (void *)(getEventFilter)}, // callback: see linuxCallback.h
    {"SDL_GetFloatProperty", (void *)(SDL_GetFloatProperty)},
    {"SDL_GetFullscreenDisplayModes", (void *)(SDL_GetFullscreenDisplayModes)},
    {"SDL_GetGPUDeviceDriver", (void *)(SDL_GetGPUDeviceDriver)},
    {"SDL_GetGPUDeviceProperties", (void *)(SDL_GetGPUDeviceProperties)},
    {"SDL_GetGPUDriver", (void *)(SDL_GetGPUDriver)},
    {"SDL_GetGPURendererDevice", (void *)(SDL_GetGPURendererDevice)},
    {"SDL_GetGPUShaderFormats", (void *)(SDL_GetGPUShaderFormats)},
    {"SDL_GetGPUSwapchainTextureFormat", (void *)(SDL_GetGPUSwapchainTextureFormat)},
    {"SDL_GetGPUTextureFormatFromPixelFormat", (void *)(SDL_GetGPUTextureFormatFromPixelFormat)},
    {"SDL_GetGamepadAppleSFSymbolsNameForAxis", (void *)(SDL_GetGamepadAppleSFSymbolsNameForAxis)},
    {"SDL_GetGamepadAppleSFSymbolsNameForButton", (void *)(SDL_GetGamepadAppleSFSymbolsNameForButton)},
    {"SDL_GetGamepadAxis", (void *)(SDL_GetGamepadAxis)},
    {"SDL_GetGamepadAxisFromString", (void *)(SDL_GetGamepadAxisFromString)},
    {"SDL_GetGamepadBindings", (void *)(SDL_GetGamepadBindings)},
    {"SDL_GetGamepadButton", (void *)(SDL_GetGamepadButton)},
    {"SDL_GetGamepadButtonFromString", (void *)(SDL_GetGamepadButtonFromString)},
    {"SDL_GetGamepadButtonLabel", (void *)(SDL_GetGamepadButtonLabel)},
    {"SDL_GetGamepadButtonLabelForType", (void *)(SDL_GetGamepadButtonLabelForType)},
    {"SDL_GetGamepadConnectionState", (void *)(SDL_GetGamepadConnectionState)},
    {"SDL_GetGamepadFirmwareVersion", (void *)(SDL_GetGamepadFirmwareVersion)},
    {"SDL_GetGamepadFromID", (void *)(SDL_GetGamepadFromID)},
    {"SDL_GetGamepadFromPlayerIndex", (void *)(SDL_GetGamepadFromPlayerIndex)},
    {"SDL_GetGamepadGUIDForID", (void *)(SDL_GetGamepadGUIDForID)},
    {"SDL_GetGamepadID", (void *)(SDL_GetGamepadID)},
    {"SDL_GetGamepadJoystick", (void *)(SDL_GetGamepadJoystick)},
    {"SDL_GetGamepadMapping", (void *)(SDL_GetGamepadMapping)},
    {"SDL_GetGamepadMappingForGUID", (void *)(SDL_GetGamepadMappingForGUID)},
    {"SDL_GetGamepadMappingForID", (void *)(SDL_GetGamepadMappingForID)},
    {"SDL_GetGamepadMappings", (void *)(SDL_GetGamepadMappings)},
    {"SDL_GetGamepadName", (void *)(SDL_GetGamepadName)},
    {"SDL_GetGamepadNameForID", (void *)(SDL_GetGamepadNameForID)},
    {"SDL_GetGamepadPath", (void *)(SDL_GetGamepadPath)},
    {"SDL_GetGamepadPathForID", (void *)(SDL_GetGamepadPathForID)},
    {"SDL_GetGamepadPlayerIndex", (void *)(SDL_GetGamepadPlayerIndex)},
    {"SDL_GetGamepadPlayerIndexForID", (void *)(SDL_GetGamepadPlayerIndexForID)},
    {"SDL_GetGamepadPowerInfo", (void *)(SDL_GetGamepadPowerInfo)},
    {"SDL_GetGamepadProduct", (void *)(SDL_GetGamepadProduct)},
    {"SDL_GetGamepadProductForID", (void *)(SDL_GetGamepadProductForID)},
    {"SDL_GetGamepadProductVersion", (void *)(SDL_GetGamepadProductVersion)},
    {"SDL_GetGamepadProductVersionForID", (void *)(SDL_GetGamepadProductVersionForID)},
    {"SDL_GetGamepadProperties", (void *)(SDL_GetGamepadProperties)},
    {"SDL_GetGamepadSensorData", (void *)(SDL_GetGamepadSensorData)},
    {"SDL_GetGamepadSensorDataRate", (void *)(SDL_GetGamepadSensorDataRate)},
    {"SDL_GetGamepadSerial", (void *)(SDL_GetGamepadSerial)},
    {"SDL_GetGamepadSteamHandle", (void *)(SDL_GetGamepadSteamHandle)},
    {"SDL_GetGamepadStringForAxis", (void *)(SDL_GetGamepadStringForAxis)},
    {"SDL_GetGamepadStringForButton", (void *)(SDL_GetGamepadStringForButton)},
    {"SDL_GetGamepadStringForType", (void *)(SDL_GetGamepadStringForType)},
    {"SDL_GetGamepadTouchpadFinger", (void *)(SDL_GetGamepadTouchpadFinger)},
    {"SDL_GetGamepadType", (void *)(SDL_GetGamepadType)},
    {"SDL_GetGamepadTypeForID", (void *)(SDL_GetGamepadTypeForID)},
    {"SDL_GetGamepadTypeFromString", (void *)(SDL_GetGamepadTypeFromString)},
    {"SDL_GetGamepadVendor", (void *)(SDL_GetGamepadVendor)},
    {"SDL_GetGamepadVendorForID", (void *)(SDL_GetGamepadVendorForID)},
    {"SDL_GetGamepads", (void *)(SDL_GetGamepads)},
    {"SDL_GetGlobalMouseState", (void *)(SDL_GetGlobalMouseState)},
    {"SDL_GetGlobalProperties", (void *)(SDL_GetGlobalProperties)},
    {"SDL_GetGrabbedWindow", (void *)(SDL_GetGrabbedWindow)},
    {"SDL_GetHapticEffectStatus", (void *)(SDL_GetHapticEffectStatus)},
    {"SDL_GetHapticFeatures", (void *)(SDL_GetHapticFeatures)},
    {"SDL_GetHapticFromID", (void *)(SDL_GetHapticFromID)},
    {"SDL_GetHapticID", (void *)(SDL_GetHapticID)},
    {"SDL_GetHapticName", (void *)(SDL_GetHapticName)},
    {"SDL_GetHapticNameForID", (void *)(SDL_GetHapticNameForID)},
    {"SDL_GetHaptics", (void *)(SDL_GetHaptics)},
    {"SDL_GetHint", (void *)(SDL_GetHint)},
    {"SDL_GetHintBoolean", (void *)(SDL_GetHintBoolean)},
    {"SDL_GetIOProperties", (void *)(SDL_GetIOProperties)},
    {"SDL_GetIOSize", (void *)(SDL_GetIOSize)},
    {"SDL_GetIOStatus", (void *)(SDL_GetIOStatus)},
    {"SDL_GetJoystickAxis", (void *)(SDL_GetJoystickAxis)},
    {"SDL_GetJoystickAxisInitialState", (void *)(SDL_GetJoystickAxisInitialState)},
    {"SDL_GetJoystickBall", (void *)(SDL_GetJoystickBall)},
    {"SDL_GetJoystickButton", (void *)(SDL_GetJoystickButton)},
    {"SDL_GetJoystickConnectionState", (void *)(SDL_GetJoystickConnectionState)},
    {"SDL_GetJoystickFirmwareVersion", (void *)(SDL_GetJoystickFirmwareVersion)},
    {"SDL_GetJoystickFromID", (void *)(SDL_GetJoystickFromID)},
    {"SDL_GetJoystickFromPlayerIndex", (void *)(SDL_GetJoystickFromPlayerIndex)},
    {"SDL_GetJoystickGUID", (void *)(SDL_GetJoystickGUID)},
    {"SDL_GetJoystickGUIDForID", (void *)(SDL_GetJoystickGUIDForID)},
    {"SDL_GetJoystickGUIDInfo", (void *)(SDL_GetJoystickGUIDInfo)},
    {"SDL_GetJoystickHat", (void *)(SDL_GetJoystickHat)},
    {"SDL_GetJoystickID", (void *)(SDL_GetJoystickID)},
    {"SDL_GetJoystickName", (void *)(SDL_GetJoystickName)},
    {"SDL_GetJoystickNameForID", (void *)(SDL_GetJoystickNameForID)},
    {"SDL_GetJoystickPath", (void *)(SDL_GetJoystickPath)},
    {"SDL_GetJoystickPathForID", (void *)(SDL_GetJoystickPathForID)},
    {"SDL_GetJoystickPlayerIndex", (void *)(SDL_GetJoystickPlayerIndex)},
    {"SDL_GetJoystickPlayerIndexForID", (void *)(SDL_GetJoystickPlayerIndexForID)},
    {"SDL_GetJoystickPowerInfo", (void *)(SDL_GetJoystickPowerInfo)},
    {"SDL_GetJoystickProduct", (void *)(SDL_GetJoystickProduct)},
    {"SDL_GetJoystickProductForID", (void *)(SDL_GetJoystickProductForID)},
    {"SDL_GetJoystickProductVersion", (void *)(SDL_GetJoystickProductVersion)},
    {"SDL_GetJoystickProductVersionForID", (void *)(SDL_GetJoystickProductVersionForID)},
    {"SDL_GetJoystickProperties", (void *)(SDL_GetJoystickProperties)},
    {"SDL_GetJoystickSerial", (void *)(SDL_GetJoystickSerial)},
    {"SDL_GetJoystickType", (void *)(SDL_GetJoystickType)},
    {"SDL_GetJoystickTypeForID", (void *)(SDL_GetJoystickTypeForID)},
    {"SDL_GetJoystickVendor", (void *)(SDL_GetJoystickVendor)},
    {"SDL_GetJoystickVendorForID", (void *)(SDL_GetJoystickVendorForID)},
    {"SDL_GetJoysticks", (void *)(SDL_GetJoysticks)},
    {"SDL_GetKeyFromName", (void *)(SDL_GetKeyFromName)},
    {"SDL_GetKeyFromScancode", (void *)(SDL_GetKeyFromScancode)},
    {"SDL_GetKeyName", (void *)(SDL_GetKeyName)},
    {"SDL_GetKeyboardFocus", (void *)(SDL_GetKeyboardFocus)},
    {"SDL_GetKeyboardNameForID", (void *)(SDL_GetKeyboardNameForID)},
    {"SDL_GetKeyboardState", (void *)(SDL_GetKeyboardState)},
    {"SDL_GetKeyboards", (void *)(SDL_GetKeyboards)},
    {"SDL_GetLogOutputFunction", (void *)(getLogOutputFunction)}, // callback: see linuxCallback.h
    {"SDL_GetLogPriority", (void *)(SDL_GetLogPriority)},
    {"SDL_GetMasksForPixelFormat", (void *)(SDL_GetMasksForPixelFormat)},
    {"SDL_GetMaxHapticEffects", (void *)(SDL_GetMaxHapticEffects)},
    {"SDL_GetMaxHapticEffectsPlaying", (void *)(SDL_GetMaxHapticEffectsPlaying)},
    {"SDL_GetMemoryFunctions", (void *)(SDL_GetMemoryFunctions)},
    {"SDL_GetMice", (void *)(SDL_GetMice)},
    {"SDL_GetModState", (void *)(SDL_GetModState)},
    {"SDL_GetMouseFocus", (void *)(SDL_GetMouseFocus)},
    {"SDL_GetMouseNameForID", (void *)(SDL_GetMouseNameForID)},
    {"SDL_GetMouseState", (void *)(SDL_GetMouseState)},
    {"SDL_GetNaturalDisplayOrientation", (void *)(SDL_GetNaturalDisplayOrientation)},
    {"SDL_GetNumAllocations", (void *)(SDL_GetNumAllocations)},
    {"SDL_GetNumAudioDrivers", (void *)(SDL_GetNumAudioDrivers)},
    {"SDL_GetNumCameraDrivers", (void *)(SDL_GetNumCameraDrivers)},
    {"SDL_GetNumGPUDrivers", (void *)(SDL_GetNumGPUDrivers)},
    {"SDL_GetNumGamepadTouchpadFingers", (void *)(SDL_GetNumGamepadTouchpadFingers)},
    {"SDL_GetNumGamepadTouchpads", (void *)(SDL_GetNumGamepadTouchpads)},
    {"SDL_GetNumHapticAxes", (void *)(SDL_GetNumHapticAxes)},
    {"SDL_GetNumJoystickAxes", (void *)(SDL_GetNumJoystickAxes)},
    {"SDL_GetNumJoystickBalls", (void *)(SDL_GetNumJoystickBalls)},
    {"SDL_GetNumJoystickButtons", (void *)(SDL_GetNumJoystickButtons)},
    {"SDL_GetNumJoystickHats", (void *)(SDL_GetNumJoystickHats)},
    {"SDL_GetNumLogicalCPUCores", (void *)(SDL_GetNumLogicalCPUCores)},
    {"SDL_GetNumRenderDrivers", (void *)(SDL_GetNumRenderDrivers)},
    {"SDL_GetNumVideoDrivers", (void *)(SDL_GetNumVideoDrivers)},
    {"SDL_GetNumberProperty", (void *)(SDL_GetNumberProperty)},
    {"SDL_GetOriginalMemoryFunctions", (void *)(SDL_GetOriginalMemoryFunctions)},
    {"SDL_GetPathInfo", (void *)(getPathInfo)}, // the cabinet's paths: see below
    {"SDL_GetPenDeviceType", (void *)(SDL_GetPenDeviceType)},
    {"SDL_GetPerformanceCounter", (void *)(SDL_GetPerformanceCounter)},
    {"SDL_GetPerformanceFrequency", (void *)(SDL_GetPerformanceFrequency)},
    {"SDL_GetPixelFormatDetails", (void *)(SDL_GetPixelFormatDetails)},
    {"SDL_GetPixelFormatForMasks", (void *)(SDL_GetPixelFormatForMasks)},
    {"SDL_GetPixelFormatFromGPUTextureFormat", (void *)(SDL_GetPixelFormatFromGPUTextureFormat)},
    {"SDL_GetPixelFormatName", (void *)(SDL_GetPixelFormatName)},
    {"SDL_GetPlatform", (void *)(SDL_GetPlatform)},
    {"SDL_GetPointerProperty", (void *)(SDL_GetPointerProperty)},
    {"SDL_GetPowerInfo", (void *)(SDL_GetPowerInfo)},
    {"SDL_GetPrefPath", (void *)(SDL_GetPrefPath)},
    {"SDL_GetPreferredLocales", (void *)(SDL_GetPreferredLocales)},
    {"SDL_GetPrimaryDisplay", (void *)(SDL_GetPrimaryDisplay)},
    {"SDL_GetPrimarySelectionText", (void *)(SDL_GetPrimarySelectionText)},
    {"SDL_GetProcessInput", (void *)(SDL_GetProcessInput)},
    {"SDL_GetProcessOutput", (void *)(SDL_GetProcessOutput)},
    {"SDL_GetProcessProperties", (void *)(SDL_GetProcessProperties)},
    {"SDL_GetPropertyType", (void *)(SDL_GetPropertyType)},
    {"SDL_GetRGB", (void *)(SDL_GetRGB)},
    {"SDL_GetRGBA", (void *)(SDL_GetRGBA)},
    {"SDL_GetRealGamepadType", (void *)(SDL_GetRealGamepadType)},
    {"SDL_GetRealGamepadTypeForID", (void *)(SDL_GetRealGamepadTypeForID)},
    {"SDL_GetRectAndLineIntersection", (void *)(SDL_GetRectAndLineIntersection)},
    {"SDL_GetRectAndLineIntersectionFloat", (void *)(SDL_GetRectAndLineIntersectionFloat)},
    {"SDL_GetRectEnclosingPoints", (void *)(SDL_GetRectEnclosingPoints)},
    {"SDL_GetRectEnclosingPointsFloat", (void *)(SDL_GetRectEnclosingPointsFloat)},
    {"SDL_GetRectIntersection", (void *)(SDL_GetRectIntersection)},
    {"SDL_GetRectIntersectionFloat", (void *)(SDL_GetRectIntersectionFloat)},
    {"SDL_GetRectUnion", (void *)(SDL_GetRectUnion)},
    {"SDL_GetRectUnionFloat", (void *)(SDL_GetRectUnionFloat)},
    {"SDL_GetRelativeMouseState", (void *)(SDL_GetRelativeMouseState)},
    {"SDL_GetRenderClipRect", (void *)(SDL_GetRenderClipRect)},
    {"SDL_GetRenderColorScale", (void *)(SDL_GetRenderColorScale)},
    {"SDL_GetRenderDrawBlendMode", (void *)(SDL_GetRenderDrawBlendMode)},
    {"SDL_GetRenderDrawColor", (void *)(SDL_GetRenderDrawColor)},
    {"SDL_GetRenderDrawColorFloat", (void *)(SDL_GetRenderDrawColorFloat)},
    {"SDL_GetRenderDriver", (void *)(SDL_GetRenderDriver)},
    {"SDL_GetRenderLogicalPresentation", (void *)(SDL_GetRenderLogicalPresentation)},
    {"SDL_GetRenderLogicalPresentationRect", (void *)(SDL_GetRenderLogicalPresentationRect)},
    {"SDL_GetRenderMetalCommandEncoder", (void *)(SDL_GetRenderMetalCommandEncoder)},
    {"SDL_GetRenderMetalLayer", (void *)(SDL_GetRenderMetalLayer)},
    {"SDL_GetRenderOutputSize", (void *)(SDL_GetRenderOutputSize)},
    {"SDL_GetRenderSafeArea", (void *)(SDL_GetRenderSafeArea)},
    {"SDL_GetRenderScale", (void *)(SDL_GetRenderScale)},
    {"SDL_GetRenderTarget", (void *)(SDL_GetRenderTarget)},
    {"SDL_GetRenderTextureAddressMode", (void *)(SDL_GetRenderTextureAddressMode)},
    {"SDL_GetRenderVSync", (void *)(SDL_GetRenderVSync)},
    {"SDL_GetRenderViewport", (void *)(SDL_GetRenderViewport)},
    {"SDL_GetRenderWindow", (void *)(SDL_GetRenderWindow)},
    {"SDL_GetRenderer", (void *)(SDL_GetRenderer)},
    {"SDL_GetRendererFromTexture", (void *)(SDL_GetRendererFromTexture)},
    {"SDL_GetRendererName", (void *)(SDL_GetRendererName)},
    {"SDL_GetRendererProperties", (void *)(SDL_GetRendererProperties)},
    {"SDL_GetRevision", (void *)(SDL_GetRevision)},
    {"SDL_GetSIMDAlignment", (void *)(SDL_GetSIMDAlignment)},
    {"SDL_GetSandbox", (void *)(SDL_GetSandbox)},
    {"SDL_GetScancodeFromKey", (void *)(SDL_GetScancodeFromKey)},
    {"SDL_GetScancodeFromName", (void *)(SDL_GetScancodeFromName)},
    {"SDL_GetScancodeName", (void *)(SDL_GetScancodeName)},
    {"SDL_GetSemaphoreValue", (void *)(SDL_GetSemaphoreValue)},
    {"SDL_GetSensorData", (void *)(SDL_GetSensorData)},
    {"SDL_GetSensorFromID", (void *)(SDL_GetSensorFromID)},
    {"SDL_GetSensorID", (void *)(SDL_GetSensorID)},
    {"SDL_GetSensorName", (void *)(SDL_GetSensorName)},
    {"SDL_GetSensorNameForID", (void *)(SDL_GetSensorNameForID)},
    {"SDL_GetSensorNonPortableType", (void *)(SDL_GetSensorNonPortableType)},
    {"SDL_GetSensorNonPortableTypeForID", (void *)(SDL_GetSensorNonPortableTypeForID)},
    {"SDL_GetSensorProperties", (void *)(SDL_GetSensorProperties)},
    {"SDL_GetSensorType", (void *)(SDL_GetSensorType)},
    {"SDL_GetSensorTypeForID", (void *)(SDL_GetSensorTypeForID)},
    {"SDL_GetSensors", (void *)(SDL_GetSensors)},
    {"SDL_GetSilenceValueForFormat", (void *)(SDL_GetSilenceValueForFormat)},
    {"SDL_GetStorageFileSize", (void *)(SDL_GetStorageFileSize)},
    {"SDL_GetStoragePathInfo", (void *)(SDL_GetStoragePathInfo)},
    {"SDL_GetStorageSpaceRemaining", (void *)(SDL_GetStorageSpaceRemaining)},
    {"SDL_GetStringProperty", (void *)(SDL_GetStringProperty)},
    {"SDL_GetSurfaceAlphaMod", (void *)(SDL_GetSurfaceAlphaMod)},
    {"SDL_GetSurfaceBlendMode", (void *)(SDL_GetSurfaceBlendMode)},
    {"SDL_GetSurfaceClipRect", (void *)(SDL_GetSurfaceClipRect)},
    {"SDL_GetSurfaceColorKey", (void *)(SDL_GetSurfaceColorKey)},
    {"SDL_GetSurfaceColorMod", (void *)(SDL_GetSurfaceColorMod)},
    {"SDL_GetSurfaceColorspace", (void *)(SDL_GetSurfaceColorspace)},
    {"SDL_GetSurfaceImages", (void *)(SDL_GetSurfaceImages)},
    {"SDL_GetSurfacePalette", (void *)(SDL_GetSurfacePalette)},
    {"SDL_GetSurfaceProperties", (void *)(SDL_GetSurfaceProperties)},
    {"SDL_GetSystemPageSize", (void *)(SDL_GetSystemPageSize)},
    {"SDL_GetSystemRAM", (void *)(SDL_GetSystemRAM)},
    {"SDL_GetSystemTheme", (void *)(SDL_GetSystemTheme)},
    {"SDL_GetTLS", (void *)(SDL_GetTLS)},
    {"SDL_GetTextInputArea", (void *)(SDL_GetTextInputArea)},
    {"SDL_GetTextureAlphaMod", (void *)(SDL_GetTextureAlphaMod)},
    {"SDL_GetTextureAlphaModFloat", (void *)(SDL_GetTextureAlphaModFloat)},
    {"SDL_GetTextureBlendMode", (void *)(SDL_GetTextureBlendMode)},
    {"SDL_GetTextureColorMod", (void *)(SDL_GetTextureColorMod)},
    {"SDL_GetTextureColorModFloat", (void *)(SDL_GetTextureColorModFloat)},
    {"SDL_GetTexturePalette", (void *)(SDL_GetTexturePalette)},
    {"SDL_GetTextureProperties", (void *)(SDL_GetTextureProperties)},
    {"SDL_GetTextureScaleMode", (void *)(SDL_GetTextureScaleMode)},
    {"SDL_GetTextureSize", (void *)(SDL_GetTextureSize)},
    {"SDL_GetThreadID", (void *)(SDL_GetThreadID)},
    {"SDL_GetThreadName", (void *)(SDL_GetThreadName)},
    {"SDL_GetThreadState", (void *)(SDL_GetThreadState)},
    {"SDL_GetTicks", (void *)(SDL_GetTicks)},
    {"SDL_GetTicksNS", (void *)(SDL_GetTicksNS)},
    {"SDL_GetTouchDeviceName", (void *)(SDL_GetTouchDeviceName)},
    {"SDL_GetTouchDeviceType", (void *)(SDL_GetTouchDeviceType)},
    {"SDL_GetTouchDevices", (void *)(SDL_GetTouchDevices)},
    {"SDL_GetTouchFingers", (void *)(SDL_GetTouchFingers)},
    {"SDL_GetTrayEntries", (void *)(SDL_GetTrayEntries)},
    {"SDL_GetTrayEntryChecked", (void *)(SDL_GetTrayEntryChecked)},
    {"SDL_GetTrayEntryEnabled", (void *)(SDL_GetTrayEntryEnabled)},
    {"SDL_GetTrayEntryLabel", (void *)(SDL_GetTrayEntryLabel)},
    {"SDL_GetTrayEntryParent", (void *)(SDL_GetTrayEntryParent)},
    {"SDL_GetTrayMenu", (void *)(SDL_GetTrayMenu)},
    {"SDL_GetTrayMenuParentEntry", (void *)(SDL_GetTrayMenuParentEntry)},
    {"SDL_GetTrayMenuParentTray", (void *)(SDL_GetTrayMenuParentTray)},
    {"SDL_GetTraySubmenu", (void *)(SDL_GetTraySubmenu)},
    {"SDL_GetUserFolder", (void *)(SDL_GetUserFolder)},
    {"SDL_GetVersion", (void *)(SDL_GetVersion)},
    {"SDL_GetVideoDriver", (void *)(SDL_GetVideoDriver)},
    {"SDL_GetWindowAspectRatio", (void *)(SDL_GetWindowAspectRatio)},
    {"SDL_GetWindowBordersSize", (void *)(SDL_GetWindowBordersSize)},
    {"SDL_GetWindowDisplayScale", (void *)(SDL_GetWindowDisplayScale)},
    {"SDL_GetWindowFlags", (void *)(SDL_GetWindowFlags)},
    {"SDL_GetWindowFromEvent", (void *)(SDL_GetWindowFromEvent)},
    {"SDL_GetWindowFromID", (void *)(SDL_GetWindowFromID)},
    {"SDL_GetWindowFullscreenMode", (void *)(SDL_GetWindowFullscreenMode)},
    {"SDL_GetWindowICCProfile", (void *)(SDL_GetWindowICCProfile)},
    {"SDL_GetWindowID", (void *)(SDL_GetWindowID)},
    {"SDL_GetWindowKeyboardGrab", (void *)(SDL_GetWindowKeyboardGrab)},
    {"SDL_GetWindowMaximumSize", (void *)(SDL_GetWindowMaximumSize)},
    {"SDL_GetWindowMinimumSize", (void *)(SDL_GetWindowMinimumSize)},
    {"SDL_GetWindowMouseGrab", (void *)(SDL_GetWindowMouseGrab)},
    {"SDL_GetWindowMouseRect", (void *)(SDL_GetWindowMouseRect)},
    {"SDL_GetWindowOpacity", (void *)(SDL_GetWindowOpacity)},
    {"SDL_GetWindowParent", (void *)(SDL_GetWindowParent)},
    {"SDL_GetWindowPixelDensity", (void *)(SDL_GetWindowPixelDensity)},
    {"SDL_GetWindowPixelFormat", (void *)(SDL_GetWindowPixelFormat)},
    {"SDL_GetWindowPosition", (void *)(SDL_GetWindowPosition)},
    {"SDL_GetWindowProgressState", (void *)(SDL_GetWindowProgressState)},
    {"SDL_GetWindowProgressValue", (void *)(SDL_GetWindowProgressValue)},
    {"SDL_GetWindowProperties", (void *)(SDL_GetWindowProperties)},
    {"SDL_GetWindowRelativeMouseMode", (void *)(SDL_GetWindowRelativeMouseMode)},
    {"SDL_GetWindowSafeArea", (void *)(SDL_GetWindowSafeArea)},
    {"SDL_GetWindowSize", (void *)(SDL_GetWindowSize)},
    {"SDL_GetWindowSizeInPixels", (void *)(SDL_GetWindowSizeInPixels)},
    {"SDL_GetWindowSurface", (void *)(SDL_GetWindowSurface)},
    {"SDL_GetWindowSurfaceVSync", (void *)(SDL_GetWindowSurfaceVSync)},
    {"SDL_GetWindowTitle", (void *)(SDL_GetWindowTitle)},
    {"SDL_GetWindows", (void *)(SDL_GetWindows)},
    {"SDL_GlobDirectory", (void *)(SDL_GlobDirectory)},
    {"SDL_GlobStorageDirectory", (void *)(SDL_GlobStorageDirectory)},
    {"SDL_HapticEffectSupported", (void *)(SDL_HapticEffectSupported)},
    {"SDL_HapticRumbleSupported", (void *)(SDL_HapticRumbleSupported)},
    {"SDL_HasARMSIMD", (void *)(SDL_HasARMSIMD)},
    {"SDL_HasAVX", (void *)(SDL_HasAVX)},
    {"SDL_HasAVX2", (void *)(SDL_HasAVX2)},
    {"SDL_HasAVX512F", (void *)(SDL_HasAVX512F)},
    {"SDL_HasAltiVec", (void *)(SDL_HasAltiVec)},
    {"SDL_HasClipboardData", (void *)(SDL_HasClipboardData)},
    {"SDL_HasClipboardText", (void *)(SDL_HasClipboardText)},
    {"SDL_HasEvent", (void *)(SDL_HasEvent)},
    {"SDL_HasEvents", (void *)(SDL_HasEvents)},
    {"SDL_HasGamepad", (void *)(SDL_HasGamepad)},
    {"SDL_HasJoystick", (void *)(SDL_HasJoystick)},
    {"SDL_HasKeyboard", (void *)(SDL_HasKeyboard)},
    {"SDL_HasLASX", (void *)(SDL_HasLASX)},
    {"SDL_HasLSX", (void *)(SDL_HasLSX)},
    {"SDL_HasMMX", (void *)(SDL_HasMMX)},
    {"SDL_HasMouse", (void *)(SDL_HasMouse)},
    {"SDL_HasNEON", (void *)(SDL_HasNEON)},
    {"SDL_HasPrimarySelectionText", (void *)(SDL_HasPrimarySelectionText)},
    {"SDL_HasProperty", (void *)(SDL_HasProperty)},
    {"SDL_HasRectIntersection", (void *)(SDL_HasRectIntersection)},
    {"SDL_HasRectIntersectionFloat", (void *)(SDL_HasRectIntersectionFloat)},
    {"SDL_HasSSE", (void *)(SDL_HasSSE)},
    {"SDL_HasSSE2", (void *)(SDL_HasSSE2)},
    {"SDL_HasSSE3", (void *)(SDL_HasSSE3)},
    {"SDL_HasSSE41", (void *)(SDL_HasSSE41)},
    {"SDL_HasSSE42", (void *)(SDL_HasSSE42)},
    {"SDL_HasScreenKeyboardSupport", (void *)(SDL_HasScreenKeyboardSupport)},
    {"SDL_HideCursor", (void *)(SDL_HideCursor)},
    {"SDL_HideWindow", (void *)(SDL_HideWindow)},
    {"SDL_IOFromConstMem", (void *)(SDL_IOFromConstMem)},
    {"SDL_IOFromDynamicMem", (void *)(SDL_IOFromDynamicMem)},
    {"SDL_IOFromFile", (void *)(ioFromFile)}, // the cabinet's paths: see below
    {"SDL_IOFromMem", (void *)(SDL_IOFromMem)},
    {"SDL_IOprintf", (void *)(SDL_IOprintf)},
    {"SDL_IOvprintf", (void *)(SDL_IOvprintf)},
    {"SDL_Init", (void *)(SDL_Init)},
    {"SDL_InitHapticRumble", (void *)(SDL_InitHapticRumble)},
    {"SDL_InitSubSystem", (void *)(SDL_InitSubSystem)},
    {"SDL_InsertGPUDebugLabel", (void *)(SDL_InsertGPUDebugLabel)},
    {"SDL_InsertTrayEntryAt", (void *)(SDL_InsertTrayEntryAt)},
    {"SDL_IsAudioDevicePhysical", (void *)(SDL_IsAudioDevicePhysical)},
    {"SDL_IsAudioDevicePlayback", (void *)(SDL_IsAudioDevicePlayback)},
    {"SDL_IsGamepad", (void *)(SDL_IsGamepad)},
    {"SDL_IsJoystickHaptic", (void *)(SDL_IsJoystickHaptic)},
    {"SDL_IsJoystickVirtual", (void *)(SDL_IsJoystickVirtual)},
    {"SDL_IsMainThread", (void *)(SDL_IsMainThread)},
    {"SDL_IsMouseHaptic", (void *)(SDL_IsMouseHaptic)},
    {"SDL_IsPhone", (void *)(SDL_IsPhone)},
    {"SDL_IsTV", (void *)(SDL_IsTV)},
    {"SDL_IsTablet", (void *)(SDL_IsTablet)},
    {"SDL_JoystickConnected", (void *)(SDL_JoystickConnected)},
    {"SDL_JoystickEventsEnabled", (void *)(SDL_JoystickEventsEnabled)},
    {"SDL_KillProcess", (void *)(SDL_KillProcess)},
    {"SDL_LoadBMP", (void *)(SDL_LoadBMP)},
    {"SDL_LoadBMP_IO", (void *)(SDL_LoadBMP_IO)},
    {"SDL_LoadFile", (void *)(loadFile)}, // the cabinet's paths: see below
    {"SDL_LoadFileAsync", (void *)(SDL_LoadFileAsync)},
    {"SDL_LoadFile_IO", (void *)(SDL_LoadFile_IO)},
    {"SDL_LoadFunction", (void *)(SDL_LoadFunction)},
    {"SDL_LoadObject", (void *)(SDL_LoadObject)},
    {"SDL_LoadPNG", (void *)(SDL_LoadPNG)},
    {"SDL_LoadPNG_IO", (void *)(SDL_LoadPNG_IO)},
    {"SDL_LoadSurface", (void *)(SDL_LoadSurface)},
    {"SDL_LoadSurface_IO", (void *)(SDL_LoadSurface_IO)},
    {"SDL_LoadWAV", (void *)(SDL_LoadWAV)},
    {"SDL_LoadWAV_IO", (void *)(SDL_LoadWAV_IO)},
    {"SDL_LockAudioStream", (void *)(SDL_LockAudioStream)},
    {"SDL_LockJoysticks", (void *)(SDL_LockJoysticks)},
    {"SDL_LockMutex", (void *)(SDL_LockMutex)},
    {"SDL_LockProperties", (void *)(SDL_LockProperties)},
    {"SDL_LockRWLockForReading", (void *)(SDL_LockRWLockForReading)},
    {"SDL_LockRWLockForWriting", (void *)(SDL_LockRWLockForWriting)},
    {"SDL_LockSpinlock", (void *)(SDL_LockSpinlock)},
    {"SDL_LockSurface", (void *)(SDL_LockSurface)},
    {"SDL_LockTexture", (void *)(SDL_LockTexture)},
    {"SDL_LockTextureToSurface", (void *)(SDL_LockTextureToSurface)},
    {"SDL_Log", (void *)(SDL_Log)},
    {"SDL_LogCritical", (void *)(SDL_LogCritical)},
    {"SDL_LogDebug", (void *)(SDL_LogDebug)},
    {"SDL_LogError", (void *)(SDL_LogError)},
    {"SDL_LogInfo", (void *)(SDL_LogInfo)},
    {"SDL_LogMessage", (void *)(SDL_LogMessage)},
    {"SDL_LogMessageV", (void *)(SDL_LogMessageV)},
    {"SDL_LogTrace", (void *)(SDL_LogTrace)},
    {"SDL_LogVerbose", (void *)(SDL_LogVerbose)},
    {"SDL_LogWarn", (void *)(SDL_LogWarn)},
    {"SDL_MapGPUTransferBuffer", (void *)(SDL_MapGPUTransferBuffer)},
    {"SDL_MapRGB", (void *)(SDL_MapRGB)},
    {"SDL_MapRGBA", (void *)(SDL_MapRGBA)},
    {"SDL_MapSurfaceRGB", (void *)(SDL_MapSurfaceRGB)},
    {"SDL_MapSurfaceRGBA", (void *)(SDL_MapSurfaceRGBA)},
    {"SDL_MaximizeWindow", (void *)(SDL_MaximizeWindow)},
    {"SDL_MemoryBarrierAcquireFunction", (void *)(SDL_MemoryBarrierAcquireFunction)},
    {"SDL_MemoryBarrierReleaseFunction", (void *)(SDL_MemoryBarrierReleaseFunction)},
    {"SDL_Metal_CreateView", (void *)(SDL_Metal_CreateView)},
    {"SDL_Metal_DestroyView", (void *)(SDL_Metal_DestroyView)},
    {"SDL_Metal_GetLayer", (void *)(SDL_Metal_GetLayer)},
    {"SDL_MinimizeWindow", (void *)(SDL_MinimizeWindow)},
    {"SDL_MixAudio", (void *)(SDL_MixAudio)},
    {"SDL_OnApplicationDidEnterBackground", (void *)(SDL_OnApplicationDidEnterBackground)},
    {"SDL_OnApplicationDidEnterForeground", (void *)(SDL_OnApplicationDidEnterForeground)},
    {"SDL_OnApplicationDidReceiveMemoryWarning", (void *)(SDL_OnApplicationDidReceiveMemoryWarning)},
    {"SDL_OnApplicationWillEnterBackground", (void *)(SDL_OnApplicationWillEnterBackground)},
    {"SDL_OnApplicationWillEnterForeground", (void *)(SDL_OnApplicationWillEnterForeground)},
    {"SDL_OnApplicationWillTerminate", (void *)(SDL_OnApplicationWillTerminate)},
    {"SDL_OpenAudioDevice", (void *)(SDL_OpenAudioDevice)},
    {"SDL_OpenAudioDeviceStream", (void *)(openAudioDeviceStream)}, // callback: see linuxCallback.h
    {"SDL_OpenCamera", (void *)(SDL_OpenCamera)},
    {"SDL_OpenFileStorage", (void *)(SDL_OpenFileStorage)},
    {"SDL_OpenGamepad", (void *)(SDL_OpenGamepad)},
    {"SDL_OpenHaptic", (void *)(SDL_OpenHaptic)},
    {"SDL_OpenHapticFromJoystick", (void *)(SDL_OpenHapticFromJoystick)},
    {"SDL_OpenHapticFromMouse", (void *)(SDL_OpenHapticFromMouse)},
    {"SDL_OpenIO", (void *)(SDL_OpenIO)},
    {"SDL_OpenJoystick", (void *)(SDL_OpenJoystick)},
    {"SDL_OpenSensor", (void *)(SDL_OpenSensor)},
    {"SDL_OpenStorage", (void *)(SDL_OpenStorage)},
    {"SDL_OpenTitleStorage", (void *)(SDL_OpenTitleStorage)},
    {"SDL_OpenURL", (void *)(SDL_OpenURL)},
    {"SDL_OpenUserStorage", (void *)(SDL_OpenUserStorage)},
    {"SDL_OutOfMemory", (void *)(SDL_OutOfMemory)},
    {"SDL_PauseAudioDevice", (void *)(SDL_PauseAudioDevice)},
    {"SDL_PauseAudioStreamDevice", (void *)(SDL_PauseAudioStreamDevice)},
    {"SDL_PauseHaptic", (void *)(SDL_PauseHaptic)},
    {"SDL_PeepEvents", (void *)(SDL_PeepEvents)},
    {"SDL_PlayHapticRumble", (void *)(SDL_PlayHapticRumble)},
    {"SDL_PollEvent", (void *)(SDL_PollEvent)},
    {"SDL_PopGPUDebugGroup", (void *)(SDL_PopGPUDebugGroup)},
    {"SDL_PremultiplyAlpha", (void *)(SDL_PremultiplyAlpha)},
    {"SDL_PremultiplySurfaceAlpha", (void *)(SDL_PremultiplySurfaceAlpha)},
    {"SDL_PumpEvents", (void *)(SDL_PumpEvents)},
    {"SDL_PushEvent", (void *)(SDL_PushEvent)},
    {"SDL_PushGPUComputeUniformData", (void *)(SDL_PushGPUComputeUniformData)},
    {"SDL_PushGPUDebugGroup", (void *)(SDL_PushGPUDebugGroup)},
    {"SDL_PushGPUFragmentUniformData", (void *)(SDL_PushGPUFragmentUniformData)},
    {"SDL_PushGPUVertexUniformData", (void *)(SDL_PushGPUVertexUniformData)},
    {"SDL_PutAudioStreamData", (void *)(SDL_PutAudioStreamData)},
    {"SDL_PutAudioStreamDataNoCopy", (void *)(SDL_PutAudioStreamDataNoCopy)},
    {"SDL_PutAudioStreamPlanarData", (void *)(SDL_PutAudioStreamPlanarData)},
    {"SDL_QueryGPUFence", (void *)(SDL_QueryGPUFence)},
    {"SDL_Quit", (void *)(SDL_Quit)},
    {"SDL_QuitSubSystem", (void *)(SDL_QuitSubSystem)},
    {"SDL_RaiseWindow", (void *)(SDL_RaiseWindow)},
    {"SDL_ReadAsyncIO", (void *)(SDL_ReadAsyncIO)},
    {"SDL_ReadIO", (void *)(SDL_ReadIO)},
    {"SDL_ReadProcess", (void *)(SDL_ReadProcess)},
    {"SDL_ReadS16BE", (void *)(SDL_ReadS16BE)},
    {"SDL_ReadS16LE", (void *)(SDL_ReadS16LE)},
    {"SDL_ReadS32BE", (void *)(SDL_ReadS32BE)},
    {"SDL_ReadS32LE", (void *)(SDL_ReadS32LE)},
    {"SDL_ReadS64BE", (void *)(SDL_ReadS64BE)},
    {"SDL_ReadS64LE", (void *)(SDL_ReadS64LE)},
    {"SDL_ReadS8", (void *)(SDL_ReadS8)},
    {"SDL_ReadStorageFile", (void *)(SDL_ReadStorageFile)},
    {"SDL_ReadSurfacePixel", (void *)(SDL_ReadSurfacePixel)},
    {"SDL_ReadSurfacePixelFloat", (void *)(SDL_ReadSurfacePixelFloat)},
    {"SDL_ReadU16BE", (void *)(SDL_ReadU16BE)},
    {"SDL_ReadU16LE", (void *)(SDL_ReadU16LE)},
    {"SDL_ReadU32BE", (void *)(SDL_ReadU32BE)},
    {"SDL_ReadU32LE", (void *)(SDL_ReadU32LE)},
    {"SDL_ReadU64BE", (void *)(SDL_ReadU64BE)},
    {"SDL_ReadU64LE", (void *)(SDL_ReadU64LE)},
    {"SDL_ReadU8", (void *)(SDL_ReadU8)},
    {"SDL_RegisterEvents", (void *)(SDL_RegisterEvents)},
    {"SDL_ReleaseCameraFrame", (void *)(SDL_ReleaseCameraFrame)},
    {"SDL_ReleaseGPUBuffer", (void *)(SDL_ReleaseGPUBuffer)},
    {"SDL_ReleaseGPUComputePipeline", (void *)(SDL_ReleaseGPUComputePipeline)},
    {"SDL_ReleaseGPUFence", (void *)(SDL_ReleaseGPUFence)},
    {"SDL_ReleaseGPUGraphicsPipeline", (void *)(SDL_ReleaseGPUGraphicsPipeline)},
    {"SDL_ReleaseGPUSampler", (void *)(SDL_ReleaseGPUSampler)},
    {"SDL_ReleaseGPUShader", (void *)(SDL_ReleaseGPUShader)},
    {"SDL_ReleaseGPUTexture", (void *)(SDL_ReleaseGPUTexture)},
    {"SDL_ReleaseGPUTransferBuffer", (void *)(SDL_ReleaseGPUTransferBuffer)},
    {"SDL_ReleaseWindowFromGPUDevice", (void *)(SDL_ReleaseWindowFromGPUDevice)},
    {"SDL_ReloadGamepadMappings", (void *)(SDL_ReloadGamepadMappings)},
    {"SDL_RemoveEventWatch", (void *)(removeEventWatch)}, // callback: see linuxCallback.h
    {"SDL_RemoveHintCallback", (void *)(removeHintCallback)}, // callback: see linuxCallback.h
    {"SDL_RemovePath", (void *)(SDL_RemovePath)},
    {"SDL_RemoveStoragePath", (void *)(SDL_RemoveStoragePath)},
    {"SDL_RemoveSurfaceAlternateImages", (void *)(SDL_RemoveSurfaceAlternateImages)},
    {"SDL_RemoveTimer", (void *)(SDL_RemoveTimer)},
    {"SDL_RemoveTrayEntry", (void *)(SDL_RemoveTrayEntry)},
    {"SDL_RenamePath", (void *)(SDL_RenamePath)},
    {"SDL_RenameStoragePath", (void *)(SDL_RenameStoragePath)},
    {"SDL_RenderClear", (void *)(SDL_RenderClear)},
    {"SDL_RenderClipEnabled", (void *)(SDL_RenderClipEnabled)},
    {"SDL_RenderCoordinatesFromWindow", (void *)(SDL_RenderCoordinatesFromWindow)},
    {"SDL_RenderCoordinatesToWindow", (void *)(SDL_RenderCoordinatesToWindow)},
    {"SDL_RenderDebugText", (void *)(SDL_RenderDebugText)},
    {"SDL_RenderDebugTextFormat", (void *)(SDL_RenderDebugTextFormat)},
    {"SDL_RenderFillRect", (void *)(SDL_RenderFillRect)},
    {"SDL_RenderFillRects", (void *)(SDL_RenderFillRects)},
    {"SDL_RenderGeometry", (void *)(SDL_RenderGeometry)},
    {"SDL_RenderGeometryRaw", (void *)(SDL_RenderGeometryRaw)},
    {"SDL_RenderLine", (void *)(SDL_RenderLine)},
    {"SDL_RenderLines", (void *)(SDL_RenderLines)},
    {"SDL_RenderPoint", (void *)(SDL_RenderPoint)},
    {"SDL_RenderPoints", (void *)(SDL_RenderPoints)},
    {"SDL_RenderPresent", (void *)(SDL_RenderPresent)},
    {"SDL_RenderReadPixels", (void *)(SDL_RenderReadPixels)},
    {"SDL_RenderRect", (void *)(SDL_RenderRect)},
    {"SDL_RenderRects", (void *)(SDL_RenderRects)},
    {"SDL_RenderTexture", (void *)(SDL_RenderTexture)},
    {"SDL_RenderTexture9Grid", (void *)(SDL_RenderTexture9Grid)},
    {"SDL_RenderTexture9GridTiled", (void *)(SDL_RenderTexture9GridTiled)},
    {"SDL_RenderTextureAffine", (void *)(SDL_RenderTextureAffine)},
    {"SDL_RenderTextureRotated", (void *)(SDL_RenderTextureRotated)},
    {"SDL_RenderTextureTiled", (void *)(SDL_RenderTextureTiled)},
    {"SDL_RenderViewportSet", (void *)(SDL_RenderViewportSet)},
    {"SDL_ReportAssertion", (void *)(SDL_ReportAssertion)},
    {"SDL_ResetAssertionReport", (void *)(SDL_ResetAssertionReport)},
    {"SDL_ResetHint", (void *)(SDL_ResetHint)},
    {"SDL_ResetHints", (void *)(SDL_ResetHints)},
    {"SDL_ResetKeyboard", (void *)(SDL_ResetKeyboard)},
    {"SDL_ResetLogPriorities", (void *)(SDL_ResetLogPriorities)},
    {"SDL_RestoreWindow", (void *)(SDL_RestoreWindow)},
    {"SDL_ResumeAudioDevice", (void *)(SDL_ResumeAudioDevice)},
    {"SDL_ResumeAudioStreamDevice", (void *)(SDL_ResumeAudioStreamDevice)},
    {"SDL_ResumeHaptic", (void *)(SDL_ResumeHaptic)},
    {"SDL_RotateSurface", (void *)(SDL_RotateSurface)},
    {"SDL_RumbleGamepad", (void *)(SDL_RumbleGamepad)},
    {"SDL_RumbleGamepadTriggers", (void *)(SDL_RumbleGamepadTriggers)},
    {"SDL_RumbleJoystick", (void *)(SDL_RumbleJoystick)},
    {"SDL_RumbleJoystickTriggers", (void *)(SDL_RumbleJoystickTriggers)},
    {"SDL_RunHapticEffect", (void *)(SDL_RunHapticEffect)},
    {"SDL_RunOnMainThread", (void *)(runOnMainThread)}, // callback: see linuxCallback.h
    {"SDL_SaveBMP", (void *)(SDL_SaveBMP)},
    {"SDL_SaveBMP_IO", (void *)(SDL_SaveBMP_IO)},
    {"SDL_SaveFile", (void *)(saveFile)}, // the cabinet's paths: see below
    {"SDL_SaveFile_IO", (void *)(SDL_SaveFile_IO)},
    {"SDL_SavePNG", (void *)(SDL_SavePNG)},
    {"SDL_SavePNG_IO", (void *)(SDL_SavePNG_IO)},
    {"SDL_ScaleSurface", (void *)(SDL_ScaleSurface)},
    {"SDL_ScreenKeyboardShown", (void *)(SDL_ScreenKeyboardShown)},
    {"SDL_ScreenSaverEnabled", (void *)(SDL_ScreenSaverEnabled)},
    {"SDL_SeekIO", (void *)(SDL_SeekIO)},
    {"SDL_SendGamepadEffect", (void *)(SDL_SendGamepadEffect)},
    {"SDL_SendJoystickEffect", (void *)(SDL_SendJoystickEffect)},
    {"SDL_SendJoystickVirtualSensorData", (void *)(SDL_SendJoystickVirtualSensorData)},
    {"SDL_SetAppMetadata", (void *)(SDL_SetAppMetadata)},
    {"SDL_SetAppMetadataProperty", (void *)(SDL_SetAppMetadataProperty)},
    {"SDL_SetAssertionHandler", (void *)(SDL_SetAssertionHandler)},
    {"SDL_SetAtomicInt", (void *)(SDL_SetAtomicInt)},
    {"SDL_SetAtomicPointer", (void *)(SDL_SetAtomicPointer)},
    {"SDL_SetAtomicU32", (void *)(SDL_SetAtomicU32)},
    {"SDL_SetAudioDeviceGain", (void *)(SDL_SetAudioDeviceGain)},
    {"SDL_SetAudioPostmixCallback", (void *)(setAudioPostmixCallback)}, // callback: see linuxCallback.h
    {"SDL_SetAudioStreamFormat", (void *)(SDL_SetAudioStreamFormat)},
    {"SDL_SetAudioStreamFrequencyRatio", (void *)(SDL_SetAudioStreamFrequencyRatio)},
    {"SDL_SetAudioStreamGain", (void *)(SDL_SetAudioStreamGain)},
    {"SDL_SetAudioStreamGetCallback", (void *)(setAudioStreamGetCallback)}, // callback: see linuxCallback.h
    {"SDL_SetAudioStreamInputChannelMap", (void *)(SDL_SetAudioStreamInputChannelMap)},
    {"SDL_SetAudioStreamOutputChannelMap", (void *)(SDL_SetAudioStreamOutputChannelMap)},
    {"SDL_SetAudioStreamPutCallback", (void *)(setAudioStreamPutCallback)}, // callback: see linuxCallback.h
    {"SDL_SetBooleanProperty", (void *)(SDL_SetBooleanProperty)},
    {"SDL_SetClipboardData", (void *)(SDL_SetClipboardData)},
    {"SDL_SetClipboardText", (void *)(SDL_SetClipboardText)},
    {"SDL_SetCurrentThreadPriority", (void *)(SDL_SetCurrentThreadPriority)},
    {"SDL_SetCursor", (void *)(SDL_SetCursor)},
    {"SDL_SetDefaultTextureScaleMode", (void *)(SDL_SetDefaultTextureScaleMode)},
    {"SDL_SetEnvironmentVariable", (void *)(setEnvironmentVariable)}, // filtered: see llIsLinuxOnlySdlDriver
    {"SDL_SetError", (void *)(SDL_SetError)},
    {"SDL_SetErrorV", (void *)(SDL_SetErrorV)},
    {"SDL_SetEventEnabled", (void *)(SDL_SetEventEnabled)},
    {"SDL_SetEventFilter", (void *)(setEventFilter)}, // callback: see linuxCallback.h
    {"SDL_SetFloatProperty", (void *)(SDL_SetFloatProperty)},
    {"SDL_SetGPUAllowedFramesInFlight", (void *)(SDL_SetGPUAllowedFramesInFlight)},
    {"SDL_SetGPUBlendConstants", (void *)(SDL_SetGPUBlendConstants)},
    {"SDL_SetGPUBufferName", (void *)(SDL_SetGPUBufferName)},
    {"SDL_SetGPURenderState", (void *)(SDL_SetGPURenderState)},
    {"SDL_SetGPURenderStateFragmentUniforms", (void *)(SDL_SetGPURenderStateFragmentUniforms)},
    {"SDL_SetGPURenderStateSamplerBindings", (void *)(SDL_SetGPURenderStateSamplerBindings)},
    {"SDL_SetGPURenderStateStorageBuffers", (void *)(SDL_SetGPURenderStateStorageBuffers)},
    {"SDL_SetGPURenderStateStorageTextures", (void *)(SDL_SetGPURenderStateStorageTextures)},
    {"SDL_SetGPUScissor", (void *)(SDL_SetGPUScissor)},
    {"SDL_SetGPUStencilReference", (void *)(SDL_SetGPUStencilReference)},
    {"SDL_SetGPUSwapchainParameters", (void *)(SDL_SetGPUSwapchainParameters)},
    {"SDL_SetGPUTextureName", (void *)(SDL_SetGPUTextureName)},
    {"SDL_SetGPUViewport", (void *)(SDL_SetGPUViewport)},
    {"SDL_SetGamepadEventsEnabled", (void *)(SDL_SetGamepadEventsEnabled)},
    {"SDL_SetGamepadLED", (void *)(SDL_SetGamepadLED)},
    {"SDL_SetGamepadMapping", (void *)(SDL_SetGamepadMapping)},
    {"SDL_SetGamepadPlayerIndex", (void *)(SDL_SetGamepadPlayerIndex)},
    {"SDL_SetGamepadSensorEnabled", (void *)(SDL_SetGamepadSensorEnabled)},
    {"SDL_SetHapticAutocenter", (void *)(SDL_SetHapticAutocenter)},
    {"SDL_SetHapticGain", (void *)(SDL_SetHapticGain)},
    {"SDL_SetHint", (void *)(setHint)}, // filtered: see llIsLinuxOnlySdlDriver
    {"SDL_SetHintWithPriority", (void *)(setHintWithPriority)}, // filtered: see llIsLinuxOnlySdlDriver
    {"SDL_SetInitialized", (void *)(SDL_SetInitialized)},
    {"SDL_SetJoystickEventsEnabled", (void *)(SDL_SetJoystickEventsEnabled)},
    {"SDL_SetJoystickLED", (void *)(SDL_SetJoystickLED)},
    {"SDL_SetJoystickPlayerIndex", (void *)(SDL_SetJoystickPlayerIndex)},
    {"SDL_SetJoystickVirtualAxis", (void *)(SDL_SetJoystickVirtualAxis)},
    {"SDL_SetJoystickVirtualBall", (void *)(SDL_SetJoystickVirtualBall)},
    {"SDL_SetJoystickVirtualButton", (void *)(SDL_SetJoystickVirtualButton)},
    {"SDL_SetJoystickVirtualHat", (void *)(SDL_SetJoystickVirtualHat)},
    {"SDL_SetJoystickVirtualTouchpad", (void *)(SDL_SetJoystickVirtualTouchpad)},
    {"SDL_SetLogOutputFunction", (void *)(setLogOutputFunction)}, // callback: see linuxCallback.h
    {"SDL_SetLogPriorities", (void *)(SDL_SetLogPriorities)},
    {"SDL_SetLogPriority", (void *)(SDL_SetLogPriority)},
    {"SDL_SetLogPriorityPrefix", (void *)(SDL_SetLogPriorityPrefix)},
    {"SDL_SetMemoryFunctions", (void *)(SDL_SetMemoryFunctions)},
    {"SDL_SetModState", (void *)(SDL_SetModState)},
    {"SDL_SetNumberProperty", (void *)(SDL_SetNumberProperty)},
    {"SDL_SetPaletteColors", (void *)(SDL_SetPaletteColors)},
    {"SDL_SetPointerProperty", (void *)(SDL_SetPointerProperty)},
    {"SDL_SetPointerPropertyWithCleanup", (void *)(SDL_SetPointerPropertyWithCleanup)},
    {"SDL_SetPrimarySelectionText", (void *)(SDL_SetPrimarySelectionText)},
    {"SDL_SetRelativeMouseTransform", (void *)(SDL_SetRelativeMouseTransform)},
    {"SDL_SetRenderClipRect", (void *)(SDL_SetRenderClipRect)},
    {"SDL_SetRenderColorScale", (void *)(SDL_SetRenderColorScale)},
    {"SDL_SetRenderDrawBlendMode", (void *)(SDL_SetRenderDrawBlendMode)},
    {"SDL_SetRenderDrawColor", (void *)(SDL_SetRenderDrawColor)},
    {"SDL_SetRenderDrawColorFloat", (void *)(SDL_SetRenderDrawColorFloat)},
    {"SDL_SetRenderLogicalPresentation", (void *)(SDL_SetRenderLogicalPresentation)},
    {"SDL_SetRenderScale", (void *)(SDL_SetRenderScale)},
    {"SDL_SetRenderTarget", (void *)(SDL_SetRenderTarget)},
    {"SDL_SetRenderTextureAddressMode", (void *)(SDL_SetRenderTextureAddressMode)},
    {"SDL_SetRenderVSync", (void *)(SDL_SetRenderVSync)},
    {"SDL_SetRenderViewport", (void *)(SDL_SetRenderViewport)},
    {"SDL_SetScancodeName", (void *)(SDL_SetScancodeName)},
    {"SDL_SetStringProperty", (void *)(SDL_SetStringProperty)},
    {"SDL_SetSurfaceAlphaMod", (void *)(SDL_SetSurfaceAlphaMod)},
    {"SDL_SetSurfaceBlendMode", (void *)(SDL_SetSurfaceBlendMode)},
    {"SDL_SetSurfaceClipRect", (void *)(SDL_SetSurfaceClipRect)},
    {"SDL_SetSurfaceColorKey", (void *)(SDL_SetSurfaceColorKey)},
    {"SDL_SetSurfaceColorMod", (void *)(SDL_SetSurfaceColorMod)},
    {"SDL_SetSurfaceColorspace", (void *)(SDL_SetSurfaceColorspace)},
    {"SDL_SetSurfacePalette", (void *)(SDL_SetSurfacePalette)},
    {"SDL_SetSurfaceRLE", (void *)(SDL_SetSurfaceRLE)},
    {"SDL_SetTLS", (void *)(SDL_SetTLS)},
    {"SDL_SetTextInputArea", (void *)(SDL_SetTextInputArea)},
    {"SDL_SetTextureAlphaMod", (void *)(SDL_SetTextureAlphaMod)},
    {"SDL_SetTextureAlphaModFloat", (void *)(SDL_SetTextureAlphaModFloat)},
    {"SDL_SetTextureBlendMode", (void *)(SDL_SetTextureBlendMode)},
    {"SDL_SetTextureColorMod", (void *)(SDL_SetTextureColorMod)},
    {"SDL_SetTextureColorModFloat", (void *)(SDL_SetTextureColorModFloat)},
    {"SDL_SetTexturePalette", (void *)(SDL_SetTexturePalette)},
    {"SDL_SetTextureScaleMode", (void *)(SDL_SetTextureScaleMode)},
    {"SDL_SetTrayEntryCallback", (void *)(SDL_SetTrayEntryCallback)},
    {"SDL_SetTrayEntryChecked", (void *)(SDL_SetTrayEntryChecked)},
    {"SDL_SetTrayEntryEnabled", (void *)(SDL_SetTrayEntryEnabled)},
    {"SDL_SetTrayEntryLabel", (void *)(SDL_SetTrayEntryLabel)},
    {"SDL_SetTrayIcon", (void *)(SDL_SetTrayIcon)},
    {"SDL_SetTrayTooltip", (void *)(SDL_SetTrayTooltip)},
    {"SDL_SetWindowAlwaysOnTop", (void *)(SDL_SetWindowAlwaysOnTop)},
    {"SDL_SetWindowAspectRatio", (void *)(SDL_SetWindowAspectRatio)},
    {"SDL_SetWindowBordered", (void *)(SDL_SetWindowBordered)},
    {"SDL_SetWindowFillDocument", (void *)(SDL_SetWindowFillDocument)},
    {"SDL_SetWindowFocusable", (void *)(SDL_SetWindowFocusable)},
    {"SDL_SetWindowFullscreen", (void *)(SDL_SetWindowFullscreen)},
    {"SDL_SetWindowFullscreenMode", (void *)(SDL_SetWindowFullscreenMode)},
    {"SDL_SetWindowHitTest", (void *)(setWindowHitTest)}, // callback: see linuxCallback.h
    {"SDL_SetWindowIcon", (void *)(SDL_SetWindowIcon)},
    {"SDL_SetWindowKeyboardGrab", (void *)(SDL_SetWindowKeyboardGrab)},
    {"SDL_SetWindowMaximumSize", (void *)(SDL_SetWindowMaximumSize)},
    {"SDL_SetWindowMinimumSize", (void *)(SDL_SetWindowMinimumSize)},
    {"SDL_SetWindowModal", (void *)(SDL_SetWindowModal)},
    {"SDL_SetWindowMouseGrab", (void *)(SDL_SetWindowMouseGrab)},
    {"SDL_SetWindowMouseRect", (void *)(SDL_SetWindowMouseRect)},
    {"SDL_SetWindowOpacity", (void *)(SDL_SetWindowOpacity)},
    {"SDL_SetWindowParent", (void *)(SDL_SetWindowParent)},
    {"SDL_SetWindowPosition", (void *)(SDL_SetWindowPosition)},
    {"SDL_SetWindowProgressState", (void *)(SDL_SetWindowProgressState)},
    {"SDL_SetWindowProgressValue", (void *)(SDL_SetWindowProgressValue)},
    {"SDL_SetWindowRelativeMouseMode", (void *)(SDL_SetWindowRelativeMouseMode)},
    {"SDL_SetWindowResizable", (void *)(SDL_SetWindowResizable)},
    {"SDL_SetWindowShape", (void *)(SDL_SetWindowShape)},
    {"SDL_SetWindowSize", (void *)(SDL_SetWindowSize)},
    {"SDL_SetWindowSurfaceVSync", (void *)(SDL_SetWindowSurfaceVSync)},
    {"SDL_SetWindowTitle", (void *)(SDL_SetWindowTitle)},
    {"SDL_SetWindowsMessageHook", (void *)(SDL_SetWindowsMessageHook)},
    {"SDL_SetX11EventHook", (void *)(SDL_SetX11EventHook)},
    {"SDL_ShouldInit", (void *)(SDL_ShouldInit)},
    {"SDL_ShouldQuit", (void *)(SDL_ShouldQuit)},
    {"SDL_ShowCursor", (void *)(SDL_ShowCursor)},
    {"SDL_ShowFileDialogWithProperties", (void *)(SDL_ShowFileDialogWithProperties)},
    {"SDL_ShowMessageBox", (void *)(SDL_ShowMessageBox)},
    {"SDL_ShowOpenFileDialog", (void *)(SDL_ShowOpenFileDialog)},
    {"SDL_ShowOpenFolderDialog", (void *)(SDL_ShowOpenFolderDialog)},
    {"SDL_ShowSaveFileDialog", (void *)(SDL_ShowSaveFileDialog)},
    {"SDL_ShowSimpleMessageBox", (void *)(SDL_ShowSimpleMessageBox)},
    {"SDL_ShowWindow", (void *)(SDL_ShowWindow)},
    {"SDL_ShowWindowSystemMenu", (void *)(SDL_ShowWindowSystemMenu)},
    {"SDL_SignalAsyncIOQueue", (void *)(SDL_SignalAsyncIOQueue)},
    {"SDL_SignalCondition", (void *)(SDL_SignalCondition)},
    {"SDL_SignalSemaphore", (void *)(SDL_SignalSemaphore)},
    {"SDL_StartTextInput", (void *)(SDL_StartTextInput)},
    {"SDL_StartTextInputWithProperties", (void *)(SDL_StartTextInputWithProperties)},
    {"SDL_StepBackUTF8", (void *)(SDL_StepBackUTF8)},
    {"SDL_StepUTF8", (void *)(SDL_StepUTF8)},
    {"SDL_StopHapticEffect", (void *)(SDL_StopHapticEffect)},
    {"SDL_StopHapticEffects", (void *)(SDL_StopHapticEffects)},
    {"SDL_StopHapticRumble", (void *)(SDL_StopHapticRumble)},
    {"SDL_StopTextInput", (void *)(SDL_StopTextInput)},
    {"SDL_StorageReady", (void *)(SDL_StorageReady)},
    {"SDL_StretchSurface", (void *)(SDL_StretchSurface)},
    {"SDL_StringToGUID", (void *)(SDL_StringToGUID)},
    {"SDL_SubmitGPUCommandBuffer", (void *)(SDL_SubmitGPUCommandBuffer)},
    {"SDL_SubmitGPUCommandBufferAndAcquireFence", (void *)(SDL_SubmitGPUCommandBufferAndAcquireFence)},
    {"SDL_SurfaceHasAlternateImages", (void *)(SDL_SurfaceHasAlternateImages)},
    {"SDL_SurfaceHasColorKey", (void *)(SDL_SurfaceHasColorKey)},
    {"SDL_SurfaceHasRLE", (void *)(SDL_SurfaceHasRLE)},
    {"SDL_SyncWindow", (void *)(SDL_SyncWindow)},
    {"SDL_TellIO", (void *)(SDL_TellIO)},
    {"SDL_TextInputActive", (void *)(SDL_TextInputActive)},
    {"SDL_TimeFromWindows", (void *)(SDL_TimeFromWindows)},
    {"SDL_TimeToDateTime", (void *)(SDL_TimeToDateTime)},
    {"SDL_TimeToWindows", (void *)(SDL_TimeToWindows)},
    {"SDL_TryLockJoysticks", (void *)(SDL_TryLockJoysticks)},
    {"SDL_TryLockMutex", (void *)(SDL_TryLockMutex)},
    {"SDL_TryLockRWLockForReading", (void *)(SDL_TryLockRWLockForReading)},
    {"SDL_TryLockRWLockForWriting", (void *)(SDL_TryLockRWLockForWriting)},
    {"SDL_TryLockSpinlock", (void *)(SDL_TryLockSpinlock)},
    {"SDL_TryWaitSemaphore", (void *)(SDL_TryWaitSemaphore)},
    {"SDL_UCS4ToUTF8", (void *)(SDL_UCS4ToUTF8)},
    {"SDL_UnbindAudioStream", (void *)(SDL_UnbindAudioStream)},
    {"SDL_UnbindAudioStreams", (void *)(SDL_UnbindAudioStreams)},
    {"SDL_UnloadObject", (void *)(SDL_UnloadObject)},
    {"SDL_UnlockAudioStream", (void *)(SDL_UnlockAudioStream)},
    {"SDL_UnlockJoysticks", (void *)(SDL_UnlockJoysticks)},
    {"SDL_UnlockMutex", (void *)(SDL_UnlockMutex)},
    {"SDL_UnlockProperties", (void *)(SDL_UnlockProperties)},
    {"SDL_UnlockRWLock", (void *)(SDL_UnlockRWLock)},
    {"SDL_UnlockSpinlock", (void *)(SDL_UnlockSpinlock)},
    {"SDL_UnlockSurface", (void *)(SDL_UnlockSurface)},
    {"SDL_UnlockTexture", (void *)(SDL_UnlockTexture)},
    {"SDL_UnmapGPUTransferBuffer", (void *)(SDL_UnmapGPUTransferBuffer)},
    {"SDL_UnsetEnvironmentVariable", (void *)(SDL_UnsetEnvironmentVariable)},
    {"SDL_UpdateGamepads", (void *)(SDL_UpdateGamepads)},
    {"SDL_UpdateHapticEffect", (void *)(SDL_UpdateHapticEffect)},
    {"SDL_UpdateJoysticks", (void *)(SDL_UpdateJoysticks)},
    {"SDL_UpdateNVTexture", (void *)(SDL_UpdateNVTexture)},
    {"SDL_UpdateSensors", (void *)(SDL_UpdateSensors)},
    {"SDL_UpdateTexture", (void *)(SDL_UpdateTexture)},
    {"SDL_UpdateTrays", (void *)(SDL_UpdateTrays)},
    {"SDL_UpdateWindowSurface", (void *)(SDL_UpdateWindowSurface)},
    {"SDL_UpdateWindowSurfaceRects", (void *)(SDL_UpdateWindowSurfaceRects)},
    {"SDL_UpdateYUVTexture", (void *)(SDL_UpdateYUVTexture)},
    {"SDL_UploadToGPUBuffer", (void *)(SDL_UploadToGPUBuffer)},
    {"SDL_UploadToGPUTexture", (void *)(SDL_UploadToGPUTexture)},
    {"SDL_WaitAndAcquireGPUSwapchainTexture", (void *)(SDL_WaitAndAcquireGPUSwapchainTexture)},
    {"SDL_WaitAsyncIOResult", (void *)(SDL_WaitAsyncIOResult)},
    {"SDL_WaitCondition", (void *)(SDL_WaitCondition)},
    {"SDL_WaitConditionTimeout", (void *)(SDL_WaitConditionTimeout)},
    {"SDL_WaitEvent", (void *)(SDL_WaitEvent)},
    {"SDL_WaitEventTimeout", (void *)(SDL_WaitEventTimeout)},
    {"SDL_WaitForGPUFences", (void *)(SDL_WaitForGPUFences)},
    {"SDL_WaitForGPUIdle", (void *)(SDL_WaitForGPUIdle)},
    {"SDL_WaitForGPUSwapchain", (void *)(SDL_WaitForGPUSwapchain)},
    {"SDL_WaitProcess", (void *)(SDL_WaitProcess)},
    {"SDL_WaitSemaphore", (void *)(SDL_WaitSemaphore)},
    {"SDL_WaitSemaphoreTimeout", (void *)(SDL_WaitSemaphoreTimeout)},
    {"SDL_WaitThread", (void *)(SDL_WaitThread)},
    {"SDL_WarpMouseGlobal", (void *)(SDL_WarpMouseGlobal)},
    {"SDL_WarpMouseInWindow", (void *)(SDL_WarpMouseInWindow)},
    {"SDL_WasInit", (void *)(SDL_WasInit)},
    {"SDL_WindowHasSurface", (void *)(SDL_WindowHasSurface)},
    {"SDL_WindowSupportsGPUPresentMode", (void *)(SDL_WindowSupportsGPUPresentMode)},
    {"SDL_WindowSupportsGPUSwapchainComposition", (void *)(SDL_WindowSupportsGPUSwapchainComposition)},
    {"SDL_WriteAsyncIO", (void *)(SDL_WriteAsyncIO)},
    {"SDL_WriteIO", (void *)(SDL_WriteIO)},
    {"SDL_WriteS16BE", (void *)(SDL_WriteS16BE)},
    {"SDL_WriteS16LE", (void *)(SDL_WriteS16LE)},
    {"SDL_WriteS32BE", (void *)(SDL_WriteS32BE)},
    {"SDL_WriteS32LE", (void *)(SDL_WriteS32LE)},
    {"SDL_WriteS64BE", (void *)(SDL_WriteS64BE)},
    {"SDL_WriteS64LE", (void *)(SDL_WriteS64LE)},
    {"SDL_WriteS8", (void *)(SDL_WriteS8)},
    {"SDL_WriteStorageFile", (void *)(SDL_WriteStorageFile)},
    {"SDL_WriteSurfacePixel", (void *)(SDL_WriteSurfacePixel)},
    {"SDL_WriteSurfacePixelFloat", (void *)(SDL_WriteSurfacePixelFloat)},
    {"SDL_WriteU16BE", (void *)(SDL_WriteU16BE)},
    {"SDL_WriteU16LE", (void *)(SDL_WriteU16LE)},
    {"SDL_WriteU32BE", (void *)(SDL_WriteU32BE)},
    {"SDL_WriteU32LE", (void *)(SDL_WriteU32LE)},
    {"SDL_WriteU64BE", (void *)(SDL_WriteU64BE)},
    {"SDL_WriteU64LE", (void *)(SDL_WriteU64LE)},
    {"SDL_WriteU8", (void *)(SDL_WriteU8)},
    {"SDL_abs", (void *)(SDL_abs)},
    {"SDL_acos", (void *)(SDL_acos)},
    {"SDL_acosf", (void *)(SDL_acosf)},
    {"SDL_aligned_alloc", (void *)(SDL_aligned_alloc)},
    {"SDL_aligned_free", (void *)(SDL_aligned_free)},
    {"SDL_asin", (void *)(SDL_asin)},
    {"SDL_asinf", (void *)(SDL_asinf)},
    {"SDL_asprintf", (void *)(SDL_asprintf)},
    {"SDL_atan", (void *)(SDL_atan)},
    {"SDL_atan2", (void *)(SDL_atan2)},
    {"SDL_atan2f", (void *)(SDL_atan2f)},
    {"SDL_atanf", (void *)(SDL_atanf)},
    {"SDL_atof", (void *)(SDL_atof)},
    {"SDL_atoi", (void *)(SDL_atoi)},
    {"SDL_bsearch", (void *)(bsearchAligned)}, // callback: see linuxCallback.h
    {"SDL_bsearch_r", (void *)(bsearchRAligned)}, // callback: see linuxCallback.h
    {"SDL_calloc", (void *)(SDL_calloc)},
    {"SDL_ceil", (void *)(SDL_ceil)},
    {"SDL_ceilf", (void *)(SDL_ceilf)},
    {"SDL_copysign", (void *)(SDL_copysign)},
    {"SDL_copysignf", (void *)(SDL_copysignf)},
    {"SDL_cos", (void *)(SDL_cos)},
    {"SDL_cosf", (void *)(SDL_cosf)},
    {"SDL_crc16", (void *)(SDL_crc16)},
    {"SDL_crc32", (void *)(SDL_crc32)},
    {"SDL_exp", (void *)(SDL_exp)},
    {"SDL_expf", (void *)(SDL_expf)},
    {"SDL_fabs", (void *)(SDL_fabs)},
    {"SDL_fabsf", (void *)(SDL_fabsf)},
    {"SDL_floor", (void *)(SDL_floor)},
    {"SDL_floorf", (void *)(SDL_floorf)},
    {"SDL_fmod", (void *)(SDL_fmod)},
    {"SDL_fmodf", (void *)(SDL_fmodf)},
    {"SDL_free", (void *)(SDL_free)},
    {"SDL_getenv", (void *)(SDL_getenv)},
    {"SDL_getenv_unsafe", (void *)(SDL_getenv_unsafe)},
    {"SDL_hid_ble_scan", (void *)(SDL_hid_ble_scan)},
    {"SDL_hid_close", (void *)(SDL_hid_close)},
    {"SDL_hid_device_change_count", (void *)(SDL_hid_device_change_count)},
    {"SDL_hid_enumerate", (void *)(SDL_hid_enumerate)},
    {"SDL_hid_exit", (void *)(SDL_hid_exit)},
    {"SDL_hid_free_enumeration", (void *)(SDL_hid_free_enumeration)},
    {"SDL_hid_get_device_info", (void *)(SDL_hid_get_device_info)},
    {"SDL_hid_get_feature_report", (void *)(SDL_hid_get_feature_report)},
    {"SDL_hid_get_indexed_string", (void *)(SDL_hid_get_indexed_string)},
    {"SDL_hid_get_input_report", (void *)(SDL_hid_get_input_report)},
    {"SDL_hid_get_manufacturer_string", (void *)(SDL_hid_get_manufacturer_string)},
    {"SDL_hid_get_product_string", (void *)(SDL_hid_get_product_string)},
    {"SDL_hid_get_properties", (void *)(SDL_hid_get_properties)},
    {"SDL_hid_get_report_descriptor", (void *)(SDL_hid_get_report_descriptor)},
    {"SDL_hid_get_serial_number_string", (void *)(SDL_hid_get_serial_number_string)},
    {"SDL_hid_init", (void *)(SDL_hid_init)},
    {"SDL_hid_open", (void *)(SDL_hid_open)},
    {"SDL_hid_open_path", (void *)(SDL_hid_open_path)},
    {"SDL_hid_read", (void *)(SDL_hid_read)},
    {"SDL_hid_read_timeout", (void *)(SDL_hid_read_timeout)},
    {"SDL_hid_send_feature_report", (void *)(SDL_hid_send_feature_report)},
    {"SDL_hid_set_nonblocking", (void *)(SDL_hid_set_nonblocking)},
    {"SDL_hid_write", (void *)(SDL_hid_write)},
    {"SDL_iconv", (void *)(SDL_iconv)},
    {"SDL_iconv_close", (void *)(SDL_iconv_close)},
    {"SDL_iconv_open", (void *)(SDL_iconv_open)},
    {"SDL_iconv_string", (void *)(SDL_iconv_string)},
    {"SDL_isalnum", (void *)(SDL_isalnum)},
    {"SDL_isalpha", (void *)(SDL_isalpha)},
    {"SDL_isblank", (void *)(SDL_isblank)},
    {"SDL_iscntrl", (void *)(SDL_iscntrl)},
    {"SDL_isdigit", (void *)(SDL_isdigit)},
    {"SDL_isgraph", (void *)(SDL_isgraph)},
    {"SDL_isinf", (void *)(SDL_isinf)},
    {"SDL_isinff", (void *)(SDL_isinff)},
    {"SDL_islower", (void *)(SDL_islower)},
    {"SDL_isnan", (void *)(SDL_isnan)},
    {"SDL_isnanf", (void *)(SDL_isnanf)},
    {"SDL_isprint", (void *)(SDL_isprint)},
    {"SDL_ispunct", (void *)(SDL_ispunct)},
    {"SDL_isspace", (void *)(SDL_isspace)},
    {"SDL_isupper", (void *)(SDL_isupper)},
    {"SDL_isxdigit", (void *)(SDL_isxdigit)},
    {"SDL_itoa", (void *)(SDL_itoa)},
    {"SDL_lltoa", (void *)(SDL_lltoa)},
    {"SDL_log", (void *)(SDL_log)},
    {"SDL_log10", (void *)(SDL_log10)},
    {"SDL_log10f", (void *)(SDL_log10f)},
    {"SDL_logf", (void *)(SDL_logf)},
    {"SDL_lround", (void *)(SDL_lround)},
    {"SDL_lroundf", (void *)(SDL_lroundf)},
    {"SDL_ltoa", (void *)(SDL_ltoa)},
    {"SDL_malloc", (void *)(SDL_malloc)},
    {"SDL_memcmp", (void *)(SDL_memcmp)},
    {"SDL_memcpy", (void *)(SDL_memcpy)},
    {"SDL_memmove", (void *)(SDL_memmove)},
    {"SDL_memset", (void *)(SDL_memset)},
    {"SDL_memset4", (void *)(SDL_memset4)},
    {"SDL_modf", (void *)(SDL_modf)},
    {"SDL_modff", (void *)(SDL_modff)},
    {"SDL_murmur3_32", (void *)(SDL_murmur3_32)},
    {"SDL_pow", (void *)(SDL_pow)},
    {"SDL_powf", (void *)(SDL_powf)},
    {"SDL_qsort", (void *)(qsortAligned)}, // callback: see linuxCallback.h
    {"SDL_qsort_r", (void *)(qsortRAligned)}, // callback: see linuxCallback.h
    {"SDL_rand", (void *)(SDL_rand)},
    {"SDL_rand_bits", (void *)(SDL_rand_bits)},
    {"SDL_rand_bits_r", (void *)(SDL_rand_bits_r)},
    {"SDL_rand_r", (void *)(SDL_rand_r)},
    {"SDL_randf", (void *)(SDL_randf)},
    {"SDL_randf_r", (void *)(SDL_randf_r)},
    {"SDL_realloc", (void *)(SDL_realloc)},
    {"SDL_round", (void *)(SDL_round)},
    {"SDL_roundf", (void *)(SDL_roundf)},
    {"SDL_scalbn", (void *)(SDL_scalbn)},
    {"SDL_scalbnf", (void *)(SDL_scalbnf)},
    {"SDL_setenv_unsafe", (void *)(setenvUnsafe)}, // filtered: see llIsLinuxOnlySdlDriver
    {"SDL_sin", (void *)(SDL_sin)},
    {"SDL_sinf", (void *)(SDL_sinf)},
    {"SDL_snprintf", (void *)(SDL_snprintf)},
    {"SDL_sqrt", (void *)(SDL_sqrt)},
    {"SDL_sqrtf", (void *)(SDL_sqrtf)},
    {"SDL_srand", (void *)(SDL_srand)},
    {"SDL_sscanf", (void *)(SDL_sscanf)},
    {"SDL_strcasecmp", (void *)(SDL_strcasecmp)},
    {"SDL_strcasestr", (void *)(SDL_strcasestr)},
    {"SDL_strchr", (void *)(SDL_strchr)},
    {"SDL_strcmp", (void *)(SDL_strcmp)},
    {"SDL_strdup", (void *)(SDL_strdup)},
    {"SDL_strlcat", (void *)(SDL_strlcat)},
    {"SDL_strlcpy", (void *)(SDL_strlcpy)},
    {"SDL_strlen", (void *)(SDL_strlen)},
    {"SDL_strlwr", (void *)(SDL_strlwr)},
    {"SDL_strncasecmp", (void *)(SDL_strncasecmp)},
    {"SDL_strncmp", (void *)(SDL_strncmp)},
    {"SDL_strndup", (void *)(SDL_strndup)},
    {"SDL_strnlen", (void *)(SDL_strnlen)},
    {"SDL_strnstr", (void *)(SDL_strnstr)},
    {"SDL_strpbrk", (void *)(SDL_strpbrk)},
    {"SDL_strrchr", (void *)(SDL_strrchr)},
    {"SDL_strrev", (void *)(SDL_strrev)},
    {"SDL_strstr", (void *)(SDL_strstr)},
    {"SDL_strtod", (void *)(SDL_strtod)},
    {"SDL_strtok_r", (void *)(SDL_strtok_r)},
    {"SDL_strtol", (void *)(SDL_strtol)},
    {"SDL_strtoll", (void *)(SDL_strtoll)},
    {"SDL_strtoul", (void *)(SDL_strtoul)},
    {"SDL_strtoull", (void *)(SDL_strtoull)},
    {"SDL_strupr", (void *)(SDL_strupr)},
    {"SDL_swprintf", (void *)(SDL_swprintf)},
    {"SDL_tan", (void *)(SDL_tan)},
    {"SDL_tanf", (void *)(SDL_tanf)},
    {"SDL_tolower", (void *)(SDL_tolower)},
    {"SDL_toupper", (void *)(SDL_toupper)},
    {"SDL_trunc", (void *)(SDL_trunc)},
    {"SDL_truncf", (void *)(SDL_truncf)},
    {"SDL_uitoa", (void *)(SDL_uitoa)},
    {"SDL_ulltoa", (void *)(SDL_ulltoa)},
    {"SDL_ultoa", (void *)(SDL_ultoa)},
    {"SDL_unsetenv_unsafe", (void *)(SDL_unsetenv_unsafe)},
    {"SDL_utf8strlcpy", (void *)(SDL_utf8strlcpy)},
    {"SDL_utf8strlen", (void *)(SDL_utf8strlen)},
    {"SDL_utf8strnlen", (void *)(SDL_utf8strnlen)},
    {"SDL_vasprintf", (void *)(SDL_vasprintf)},
    {"SDL_vsnprintf", (void *)(SDL_vsnprintf)},
    {"SDL_vsscanf", (void *)(SDL_vsscanf)},
    {"SDL_vswprintf", (void *)(SDL_vswprintf)},
    {"SDL_wcscasecmp", (void *)(SDL_wcscasecmp)},
    {"SDL_wcscmp", (void *)(SDL_wcscmp)},
    {"SDL_wcsdup", (void *)(SDL_wcsdup)},
    {"SDL_wcslcat", (void *)(SDL_wcslcat)},
    {"SDL_wcslcpy", (void *)(SDL_wcslcpy)},
    {"SDL_wcslen", (void *)(SDL_wcslen)},
    {"SDL_wcsncasecmp", (void *)(SDL_wcsncasecmp)},
    {"SDL_wcsncmp", (void *)(SDL_wcsncmp)},
    {"SDL_wcsnlen", (void *)(SDL_wcsnlen)},
    {"SDL_wcsnstr", (void *)(SDL_wcsnstr)},
    {"SDL_wcsstr", (void *)(SDL_wcsstr)},
    {"SDL_wcstol", (void *)(SDL_wcstol)},
};

static int compareExport(const void *key, const void *entry)
{
    return strcmp((const char *)key, ((const Sdl3Export *)entry)->name);
}

// A request for an SDL video or audio driver that only Linux builds have
// (sdl2-compat asks for x11 when the program links Xlib, as Cruis'n Blast
// does): ignored, so the Windows drivers are used. The loader and the game
// share this SDL3, so the loader's own SDL_Init() would fail on it too.
int llIsLinuxOnlySdlDriver(const char *name, const char *value)
{
    static const char *const names[] = {"SDL_VIDEO_DRIVER", "SDL_VIDEODRIVER", "SDL_AUDIO_DRIVER", "SDL_AUDIODRIVER"};
    static const char *const linuxDrivers[] = {"x11",  "wayland", "kmsdrm", "vivante", "rpi",  "alsa",
                                               "pulse", "pipewire", "jack",  "sndio",   "dsp", "esd", "arts"};
    int isDriver = 0;

    if (!name || !value)
        return 0;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (!strcmp(name, names[i]))
            isDriver = 1;
    if (!isDriver)
        return 0;
    for (size_t i = 0; i < sizeof(linuxDrivers) / sizeof(linuxDrivers[0]); i++)
        if (SDL_strcasestr(value, linuxDrivers[i]))
        {
            log_info("SDL3: %s=%s ignored (a Linux driver), the Windows one is used", name, value);
            return 1;
        }
    return 0;
}

static bool SDLCALL setHint(const char *name, const char *value)
{
    if (llIsLinuxOnlySdlDriver(name, value))
        return true;
    return SDL_SetHint(name, value);
}

static bool SDLCALL setHintWithPriority(const char *name, const char *value, SDL_HintPriority priority)
{
    if (llIsLinuxOnlySdlDriver(name, value))
        return true;
    return SDL_SetHintWithPriority(name, value, priority);
}

static int SDLCALL setenvUnsafe(const char *name, const char *value, int overwrite)
{
    if (llIsLinuxOnlySdlDriver(name, value))
        return 0;
    return SDL_setenv_unsafe(name, value, overwrite);
}

static bool SDLCALL setEnvironmentVariable(SDL_Environment *env, const char *name, const char *value, bool overwrite)
{
    if (llIsLinuxOnlySdlDriver(name, value))
        return true;
    return SDL_SetEnvironmentVariable(env, name, value, overwrite);
}

// SDL3 functions only other systems' builds have (sdl2-compat looks up all
// of SDL3's at startup and gives up if one is missing): the Linux thread
// priority calls succeed (Windows priorities are set by SDL itself), and any
// other one does nothing and answers 0 (false, NULL).
static bool SDLCALL setLinuxThreadPriority(Sint64 threadID, int priority)
{
    (void)threadID;
    (void)priority;
    return true;
}

static bool SDLCALL setLinuxThreadPriorityAndPolicy(Sint64 threadID, int priority, int policy)
{
    (void)threadID;
    (void)priority;
    (void)policy;
    return true;
}

static int SDLCALL otherSystemsFunction(void)
{
    return 0;
}

// opengl32's functions are stdcall: they pop their own arguments, and a Linux
// caller, which pops them again, comes back with its stack pointer off.
// sdl12-compat's LoadOpenGLFunctions called glGetString so, and the GL
// version it read and its stack canary were then taken from the wrong slots
// ("stack smashing detected"). The GL hooks' cdecl wrappers instead, as
// glXGetProcAddress gives (graphics/glxBridge.c).
static SDL_FunctionPointer SDLCALL glGetProcAddress(const char *proc)
{
    return proc ? (SDL_FunctionPointer)GLHooks_GetProcAddress(proc) : NULL;
}

// The game's GL context, current once made: the driver's reports go on there
// (LL_GL_DEBUG, rtGlDebugStart).
static SDL_GLContext SDLCALL glCreateContext(SDL_Window *window)
{
    SDL_GLContext context = SDL_GL_CreateContext(window);
    if (context)
        rtGlDebugStart();
    return context;
}

// Files the game opens through SDL (Cruis'n Blast's sound effects, by
// SDL_mixer under sdl12-compat) name the cabinet's paths (/pm/g5/race/...):
// mapped to the game folder, as the loader's open() and fopen() do.
static SDL_IOStream *SDLCALL ioFromFile(const char *file, const char *mode)
{
    char buf[1024];
    return SDL_IOFromFile(file ? gameRedirectPath(file, buf, sizeof(buf)) : NULL, mode);
}

static void *SDLCALL loadFile(const char *file, size_t *datasize)
{
    char buf[1024];
    return SDL_LoadFile(file ? gameRedirectPath(file, buf, sizeof(buf)) : NULL, datasize);
}

static bool SDLCALL saveFile(const char *file, const void *data, size_t datasize)
{
    char buf[1024];
    return SDL_SaveFile(file ? gameRedirectPath(file, buf, sizeof(buf)) : NULL, data, datasize);
}

static bool SDLCALL getPathInfo(const char *path, SDL_PathInfo *info)
{
    char buf[1024];
    return SDL_GetPathInfo(path ? gameRedirectPath(path, buf, sizeof(buf)) : NULL, info);
}

// SDL3 calls these functions' callbacks from its own (Windows) code, on a
// stack only 4-byte aligned: the Linux callbacks get a thunk that realigns
// it (linuxCallback.h). sdl2-compat's event watch faulted without one.
#define CB(type, fn) ((type)linuxCallback((void *)(fn)))

static bool SDLCALL addEventWatch(SDL_EventFilter filter, void *userdata)
{
    return SDL_AddEventWatch(CB(SDL_EventFilter, filter), userdata);
}

static void SDLCALL removeEventWatch(SDL_EventFilter filter, void *userdata)
{
    SDL_RemoveEventWatch(CB(SDL_EventFilter, filter), userdata);
}

static void SDLCALL setEventFilter(SDL_EventFilter filter, void *userdata)
{
    SDL_SetEventFilter(CB(SDL_EventFilter, filter), userdata);
}

static bool SDLCALL getEventFilter(SDL_EventFilter *filter, void **userdata)
{
    bool ok = SDL_GetEventFilter(filter, userdata);
    if (filter)
        *filter = (SDL_EventFilter)linuxCallbackTarget((void *)*filter);
    return ok;
}

static void SDLCALL filterEvents(SDL_EventFilter filter, void *userdata)
{
    SDL_FilterEvents(CB(SDL_EventFilter, filter), userdata);
}

static bool SDLCALL addHintCallback(const char *name, SDL_HintCallback callback, void *userdata)
{
    return SDL_AddHintCallback(name, CB(SDL_HintCallback, callback), userdata);
}

static void SDLCALL removeHintCallback(const char *name, SDL_HintCallback callback, void *userdata)
{
    SDL_RemoveHintCallback(name, CB(SDL_HintCallback, callback), userdata);
}

static SDL_TimerID SDLCALL addTimer(Uint32 interval, SDL_TimerCallback callback, void *userdata)
{
    return SDL_AddTimer(interval, CB(SDL_TimerCallback, callback), userdata);
}

static SDL_TimerID SDLCALL addTimerNS(Uint64 interval, SDL_NSTimerCallback callback, void *userdata)
{
    return SDL_AddTimerNS(interval, CB(SDL_NSTimerCallback, callback), userdata);
}

static SDL_Thread *SDLCALL createThreadRuntime(SDL_ThreadFunction fn, const char *name, void *data,
                                               SDL_FunctionPointer pfnBeginThread, SDL_FunctionPointer pfnEndThread)
{
    return SDL_CreateThreadRuntime(CB(SDL_ThreadFunction, fn), name, data, pfnBeginThread, pfnEndThread);
}

static SDL_Thread *SDLCALL createThreadWithPropertiesRuntime(SDL_PropertiesID props, SDL_FunctionPointer pfnBeginThread,
                                                             SDL_FunctionPointer pfnEndThread)
{
    void *fn = SDL_GetPointerProperty(props, SDL_PROP_THREAD_CREATE_ENTRY_FUNCTION_POINTER, NULL);
    if (fn)
        SDL_SetPointerProperty(props, SDL_PROP_THREAD_CREATE_ENTRY_FUNCTION_POINTER, linuxCallback(fn));
    return SDL_CreateThreadWithPropertiesRuntime(props, pfnBeginThread, pfnEndThread);
}

static void SDLCALL setLogOutputFunction(SDL_LogOutputFunction callback, void *userdata)
{
    SDL_SetLogOutputFunction(CB(SDL_LogOutputFunction, callback), userdata);
}

static void SDLCALL getLogOutputFunction(SDL_LogOutputFunction *callback, void **userdata)
{
    SDL_GetLogOutputFunction(callback, userdata);
    if (callback)
        *callback = (SDL_LogOutputFunction)linuxCallbackTarget((void *)*callback);
}

static SDL_AudioStream *SDLCALL openAudioDeviceStream(SDL_AudioDeviceID devid, const SDL_AudioSpec *spec,
                                                      SDL_AudioStreamCallback callback, void *userdata)
{
    return SDL_OpenAudioDeviceStream(devid, spec, CB(SDL_AudioStreamCallback, callback), userdata);
}

static bool SDLCALL setAudioStreamGetCallback(SDL_AudioStream *stream, SDL_AudioStreamCallback callback, void *userdata)
{
    return SDL_SetAudioStreamGetCallback(stream, CB(SDL_AudioStreamCallback, callback), userdata);
}

static bool SDLCALL setAudioStreamPutCallback(SDL_AudioStream *stream, SDL_AudioStreamCallback callback, void *userdata)
{
    return SDL_SetAudioStreamPutCallback(stream, CB(SDL_AudioStreamCallback, callback), userdata);
}

static bool SDLCALL setAudioPostmixCallback(SDL_AudioDeviceID devid, SDL_AudioPostmixCallback callback, void *userdata)
{
    return SDL_SetAudioPostmixCallback(devid, CB(SDL_AudioPostmixCallback, callback), userdata);
}

static bool SDLCALL setWindowHitTest(SDL_Window *window, SDL_HitTest callback, void *callback_data)
{
    return SDL_SetWindowHitTest(window, CB(SDL_HitTest, callback), callback_data);
}

static bool SDLCALL runOnMainThread(SDL_MainThreadCallback callback, void *userdata, bool wait_complete)
{
    return SDL_RunOnMainThread(CB(SDL_MainThreadCallback, callback), userdata, wait_complete);
}

static void SDLCALL qsortAligned(void *base, size_t nmemb, size_t size, SDL_CompareCallback compare)
{
    SDL_qsort(base, nmemb, size, CB(SDL_CompareCallback, compare));
}

static void SDLCALL qsortRAligned(void *base, size_t nmemb, size_t size, SDL_CompareCallback_r compare, void *userdata)
{
    SDL_qsort_r(base, nmemb, size, CB(SDL_CompareCallback_r, compare), userdata);
}

static void *SDLCALL bsearchAligned(const void *key, const void *base, size_t nmemb, size_t size,
                                    SDL_CompareCallback compare)
{
    return SDL_bsearch(key, base, nmemb, size, CB(SDL_CompareCallback, compare));
}

static void *SDLCALL bsearchRAligned(const void *key, const void *base, size_t nmemb, size_t size,
                                     SDL_CompareCallback_r compare, void *userdata)
{
    return SDL_bsearch_r(key, base, nmemb, size, CB(SDL_CompareCallback_r, compare), userdata);
}

void *sdl3Export(const char *name)
{
    const Sdl3Export *e;
    if (!name)
        return NULL;
    e = (const Sdl3Export *)bsearch(name, sdl3Exports, sizeof(sdl3Exports) / sizeof(sdl3Exports[0]),
                                    sizeof(sdl3Exports[0]), compareExport);
    if (e)
        return e->address;
    if (!strcmp(name, "SDL_SetLinuxThreadPriority"))
        return (void *)setLinuxThreadPriority;
    if (!strcmp(name, "SDL_SetLinuxThreadPriorityAndPolicy"))
        return (void *)setLinuxThreadPriorityAndPolicy;
    if (!strncmp(name, "SDL_", 4))
    {
        log_warn("SDL3: %s is not in the Windows SDL3, it will do nothing", name);
        return (void *)otherSystemsFunction;
    }
    return NULL;
}

#endif
