// Webcams on Windows (Cruis'n Blast's player photos, the Namco ES1 cameras):
// the games' /dev/videoN, emulated. They drive their cameras with V4L2
// themselves (QUERYCAP, S_FMT, REQBUFS, QUERYBUF and mmap, QBUF, STREAMON,
// DQBUF, the picture controls); Windows has no V4L2, so the loader answers
// those calls here, the pictures coming from SDL3's cameras (Media
// Foundation): /dev/videoN is the Nth camera SDL lists.
//
// The game's descriptor is a real one (of NUL), so that its numbers do not
// collide with the files' and sockets'; what it is, is kept here. The
// buffers it maps are the loader's, filled from the camera's frame at each
// DQBUF, scaled and converted to the size and format the game asked for.
// The structures are V4L2's on i386 Linux (the sizes in the ioctl numbers).

#if defined(_WIN32)

#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <SDL3/SDL.h>

#include "webcam.h"
#include "../log/log.h"
#include "../namco/namcoEs1.h"
#include "../rawthrills/rawthrills.h"

#define VIDEO_PREFIX "/dev/video"
#define MAX_CAMERA_FDS 32
#define MAX_BUFFERS 8

// V4L2, i386 Linux.
#define V4L2_BUF_TYPE_VIDEO_CAPTURE 1
#define V4L2_MEMORY_MMAP 1
#define V4L2_FIELD_NONE 1
#define V4L2_COLORSPACE_SRGB 8
#define V4L2_CAP_VIDEO_CAPTURE 0x00000001
#define V4L2_CAP_STREAMING 0x04000000
#define V4L2_CAP_DEVICE_CAPS 0x80000000
#define V4L2_BUF_FLAG_MAPPED 0x00000001
#define V4L2_BUF_FLAG_QUEUED 0x00000002
#define V4L2_BUF_FLAG_DONE 0x00000004
#define V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC 0x00002000
#define V4L2_CTRL_TYPE_INTEGER 1
#define V4L2_FRMSIZE_TYPE_DISCRETE 1
#define V4L2_FRMIVAL_TYPE_DISCRETE 1
#define V4L2_INPUT_TYPE_CAMERA 2
#define FOURCC(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
#define V4L2_PIX_FMT_YUYV FOURCC('Y', 'U', 'Y', 'V')
#define V4L2_PIX_FMT_BGR24 FOURCC('B', 'G', 'R', '3')

#define VIDIOC_QUERYCAP 0x80685600
#define VIDIOC_ENUM_FMT 0xc0405602
#define VIDIOC_G_FMT 0xc0cc5604
#define VIDIOC_S_FMT 0xc0cc5605
#define VIDIOC_REQBUFS 0xc0145608
#define VIDIOC_QUERYBUF 0xc0445609
#define VIDIOC_QBUF 0xc044560f
#define VIDIOC_DQBUF 0xc0445611
#define VIDIOC_STREAMON 0x40045612
#define VIDIOC_STREAMOFF 0x40045613
#define VIDIOC_G_PARM 0xc0cc5615
#define VIDIOC_S_PARM 0xc0cc5616
#define VIDIOC_ENUMINPUT 0xc04c561a
#define VIDIOC_G_CTRL 0xc008561b
#define VIDIOC_S_CTRL 0xc008561c
#define VIDIOC_QUERYCTRL 0xc0445624
#define VIDIOC_G_INPUT 0x80045626
#define VIDIOC_S_INPUT 0xc0045627
#define VIDIOC_CROPCAP 0xc02c563a
#define VIDIOC_G_CROP 0xc014563b
#define VIDIOC_S_CROP 0x4014563c
#define VIDIOC_TRY_FMT 0xc0cc5640
#define VIDIOC_ENUM_FRAMESIZES 0xc02c564a
#define VIDIOC_ENUM_FRAMEINTERVALS 0xc034564b

#pragma pack(push, 4)
typedef struct
{
    uint8_t driver[16];
    uint8_t card[32];
    uint8_t bus_info[32];
    uint32_t version, capabilities, device_caps, reserved[3];
} V4l2Capability; // 0x68

typedef struct
{
    uint32_t index, type, flags;
    uint8_t description[32];
    uint32_t pixelformat, mbus_code, reserved[3];
} V4l2FmtDesc; // 0x40

typedef struct
{
    uint32_t width, height, pixelformat, field, bytesperline, sizeimage, colorspace, priv, flags, ycbcr_enc,
        quantization, xfer_func;
} V4l2PixFormat;

typedef struct
{
    uint32_t type;
    union
    {
        V4l2PixFormat pix;
        uint8_t raw[200];
    } fmt;
} V4l2Format; // 0xcc

typedef struct
{
    uint32_t count, type, memory, capabilities;
    uint8_t flags, reserved[3];
} V4l2RequestBuffers; // 0x14

typedef struct
{
    uint32_t index, type, bytesused, flags, field;
    int32_t tv_sec, tv_usec;
    uint32_t timecode[4];
    uint32_t sequence, memory, offset, length, reserved2, request_fd;
} V4l2Buffer; // 0x44

typedef struct
{
    uint32_t id;
    int32_t value;
} V4l2Control; // 0x08

typedef struct
{
    uint32_t id, type;
    uint8_t name[32];
    int32_t minimum, maximum, step, default_value;
    uint32_t flags, reserved[2];
} V4l2QueryCtrl; // 0x44

typedef struct
{
    uint32_t type;
    union
    {
        struct
        {
            uint32_t capability, capturemode, numerator, denominator, extendedmode, readbuffers, reserved[4];
        } capture;
        uint8_t raw[200];
    } parm;
} V4l2StreamParm; // 0xcc

typedef struct
{
    uint32_t index;
    uint8_t name[32];
    uint32_t type, audioset, tuner;
    uint32_t std[2];
    uint32_t status, capabilities, reserved[3];
} V4l2Input; // 0x4c

typedef struct
{
    uint32_t index, pixel_format, type, width, height, stepwise[4], reserved[2];
} V4l2FrmSizeEnum; // 0x2c

typedef struct
{
    uint32_t index, pixel_format, width, height, type, numerator, denominator, stepwise[4], reserved[2];
} V4l2FrmIvalEnum; // 0x34
#pragma pack(pop)

// The controls the games read and set (picture settings): kept here, the
// camera's own picture left as it is (as the Linux build restores it).
#define MAX_CONTROLS 16

typedef struct
{
    int fd;           // the game's descriptor, -1: free slot
    int camera;       // which of SDL's cameras
    int nonblock;     // opened O_NONBLOCK
    int streaming;
    SDL_Camera *device;
    V4l2PixFormat format;
    int bufferCount;
    uint8_t *buffers[MAX_BUFFERS];
    int queued[MAX_BUFFERS];
    uint32_t sequence;
    uint32_t controlIds[MAX_CONTROLS];
    int32_t controlValues[MAX_CONTROLS];
} CameraFd;

static CameraFd cameras[MAX_CAMERA_FDS];
static SRWLOCK cameraLock = SRWLOCK_INIT;
static int camerasInitialized;

static int sdlCameras(void)
{
    int count = 0;
    // LL_NO_CAMERA=1: no camera at all, as on a PC without one.
    const char *off = getenv("LL_NO_CAMERA");
    if (off && *off && strcmp(off, "0") != 0)
        return 0;
    if (!SDL_WasInit(SDL_INIT_CAMERA) && !SDL_InitSubSystem(SDL_INIT_CAMERA))
    {
        log_warn("Webcam: no SDL cameras: %s", SDL_GetError());
        return 0;
    }
    SDL_CameraID *ids = SDL_GetCameras(&count);
    SDL_free(ids);
    return count;
}

static SDL_CameraID cameraId(int n)
{
    int count = 0;
    SDL_CameraID id = 0;
    SDL_CameraID *ids = SDL_GetCameras(&count);
    if (ids && n >= 0 && n < count)
        id = ids[n];
    SDL_free(ids);
    return id;
}

static CameraFd *find(int fd)
{
    if (fd < 0 || !camerasInitialized)
        return NULL;
    for (int i = 0; i < MAX_CAMERA_FDS; i++)
        if (cameras[i].fd == fd)
            return &cameras[i];
    return NULL;
}

static void setFormat(V4l2PixFormat *f, uint32_t width, uint32_t height, uint32_t pixelformat)
{
    if (pixelformat != V4L2_PIX_FMT_BGR24)
        pixelformat = V4L2_PIX_FMT_YUYV;
    width = width < 32 ? 640 : width > 3840 ? 3840 : width & ~1u;
    height = height < 32 ? 480 : height > 2160 ? 2160 : height;
    memset(f, 0, sizeof(*f));
    f->width = width;
    f->height = height;
    f->pixelformat = pixelformat;
    f->field = V4L2_FIELD_NONE;
    f->bytesperline = width * (pixelformat == V4L2_PIX_FMT_BGR24 ? 3 : 2);
    f->sizeimage = f->bytesperline * height;
    f->colorspace = V4L2_COLORSPACE_SRGB;
}

static void freeBuffers(CameraFd *c)
{
    for (int i = 0; i < c->bufferCount; i++)
    {
        VirtualFree(c->buffers[i], 0, MEM_RELEASE);
        c->buffers[i] = NULL;
        c->queued[i] = 0;
    }
    c->bufferCount = 0;
}

static void stopStreaming(CameraFd *c)
{
    if (c->device)
    {
        SDL_CloseCamera(c->device);
        c->device = NULL;
    }
    c->streaming = 0;
    for (int i = 0; i < c->bufferCount; i++)
        c->queued[i] = 0;
}

int webcamIsPath(const char *path)
{
    const char *n = path + sizeof(VIDEO_PREFIX) - 1;
    if ((!isRawThrillsGame() && !isNamcoEs1Game()) || strncmp(path, VIDEO_PREFIX, sizeof(VIDEO_PREFIX) - 1) || !*n)
        return 0;
    for (; *n; n++)
        if (*n < '0' || *n > '9')
            return 0;
    return 1;
}

int webcamHasCamera(void)
{
    return sdlCameras() > 0;
}

int webcamOpen(const char *path, int flags, int mode, int (*realOpen)(const char *, int, ...))
{
    (void)mode;
    (void)realOpen;
    int n = atoi(path + sizeof(VIDEO_PREFIX) - 1), count = sdlCameras();

    // The Namco ES1 games open /dev/video0 only: the first camera.
    if (isNamcoEs1Game() && n >= count && count > 0)
        n = 0;
    if (n < 0 || n >= count)
    {
        errno = ENOENT;
        return -1;
    }
    int fd = _open("NUL", _O_RDWR | _O_BINARY);
    if (fd < 0)
        return -1;

    AcquireSRWLockExclusive(&cameraLock);
    if (!camerasInitialized)
    {
        for (int i = 0; i < MAX_CAMERA_FDS; i++)
            cameras[i].fd = -1;
        camerasInitialized = 1;
    }
    CameraFd *c = NULL;
    for (int i = 0; i < MAX_CAMERA_FDS && !c; i++)
        if (cameras[i].fd < 0)
            c = &cameras[i];
    if (c)
    {
        memset(c, 0, sizeof(*c));
        c->fd = fd;
        c->camera = n;
        c->nonblock = (flags & 04000) != 0; // O_NONBLOCK, Linux's value
        setFormat(&c->format, 640, 480, V4L2_PIX_FMT_YUYV);
    }
    ReleaseSRWLockExclusive(&cameraLock);
    if (!c)
    {
        _close(fd);
        errno = EMFILE;
        return -1;
    }
    return fd;
}

int webcamIsFd(int fd)
{
    return find(fd) != NULL;
}

int webcamIsPictureSetting(unsigned int id)
{
    (void)id;
    return 0;
}

void webcamClose(int fd)
{
    AcquireSRWLockExclusive(&cameraLock);
    CameraFd *c = find(fd);
    if (c)
    {
        stopStreaming(c);
        freeBuffers(c);
        c->fd = -1;
    }
    ReleaseSRWLockExclusive(&cameraLock);
}

void *webcamMmap(int fd, size_t length, uint32_t offset)
{
    void *p = NULL;
    AcquireSRWLockExclusive(&cameraLock);
    CameraFd *c = find(fd);
    // The buffers' offsets are their index times their (page rounded) size.
    if (c && c->bufferCount)
    {
        uint32_t stride = (c->format.sizeimage + 0xfff) & ~0xfffu;
        uint32_t index = offset / stride;
        if (index < (uint32_t)c->bufferCount && length <= stride)
            p = c->buffers[index];
    }
    ReleaseSRWLockExclusive(&cameraLock);
    return p;
}

int webcamMunmap(void *addr)
{
    int ours = 0;
    AcquireSRWLockShared(&cameraLock);
    for (int i = 0; i < MAX_CAMERA_FDS && camerasInitialized && !ours; i++)
        for (int b = 0; cameras[i].fd >= 0 && b < cameras[i].bufferCount; b++)
            if (cameras[i].buffers[b] == addr)
                ours = 1;
    ReleaseSRWLockShared(&cameraLock);
    return ours; // freed with the buffers (REQBUFS 0, close)
}

static uint8_t clampByte(int v)
{
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

// The frame, at the game's size and in its format, into buffer. The camera
// is opened in XRGB8888 (SDL converts its own formats to that); the game's
// YUYV and BGR are made here: SDL's YUV conversions faulted on the camera's
// frames (an SSE read at -1).
static void convertFrame(SDL_Surface *frame, const V4l2PixFormat *f, uint8_t *buffer)
{
    SDL_Surface *rgb = frame->format == SDL_PIXELFORMAT_XRGB8888 ? frame
                                                                  : SDL_ConvertSurface(frame, SDL_PIXELFORMAT_XRGB8888);
    if (!rgb)
        return;
    if ((uint32_t)rgb->w != f->width || (uint32_t)rgb->h != f->height)
    {
        SDL_Surface *scaled = SDL_ScaleSurface(rgb, (int)f->width, (int)f->height, SDL_SCALEMODE_LINEAR);
        if (rgb != frame)
            SDL_DestroySurface(rgb);
        if (!(rgb = scaled))
            return;
    }
    for (uint32_t y = 0; y < f->height; y++)
    {
        const uint32_t *src = (const uint32_t *)((const uint8_t *)rgb->pixels + (size_t)y * rgb->pitch);
        uint8_t *dst = buffer + (size_t)y * f->bytesperline;
        if (f->pixelformat == V4L2_PIX_FMT_BGR24)
        {
            for (uint32_t x = 0; x < f->width; x++, dst += 3)
            {
                dst[0] = (uint8_t)src[x];       // B
                dst[1] = (uint8_t)(src[x] >> 8); // G
                dst[2] = (uint8_t)(src[x] >> 16); // R
            }
            continue;
        }
        // YUYV (BT.601, limited range): two pixels share their U and V.
        for (uint32_t x = 0; x + 1 < f->width; x += 2, dst += 4)
        {
            int r0 = src[x] >> 16 & 0xff, g0 = src[x] >> 8 & 0xff, b0 = src[x] & 0xff;
            int r1 = src[x + 1] >> 16 & 0xff, g1 = src[x + 1] >> 8 & 0xff, b1 = src[x + 1] & 0xff;
            int r = (r0 + r1) / 2, g = (g0 + g1) / 2, b = (b0 + b1) / 2;
            dst[0] = clampByte(((66 * r0 + 129 * g0 + 25 * b0 + 128) >> 8) + 16);
            dst[1] = clampByte(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
            dst[2] = clampByte(((66 * r1 + 129 * g1 + 25 * b1 + 128) >> 8) + 16);
            dst[3] = clampByte(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
        }
    }
    if (rgb != frame)
        SDL_DestroySurface(rgb);
}

// DQBUF: the next frame of the camera into the first buffer queued. Waits
// for it (the games' capture threads block there), up to a few seconds.
static int dequeue(CameraFd *c, V4l2Buffer *b)
{
    int index = -1;
    for (int i = 0; i < c->bufferCount && index < 0; i++)
        if (c->queued[i])
            index = i;
    if (!c->streaming || !c->device || index < 0)
    {
        errno = EINVAL;
        return -1;
    }
    Uint64 deadline = SDL_GetTicks() + 3000;
    for (;;)
    {
        Uint64 ns = 0;
        SDL_Surface *frame = SDL_AcquireCameraFrame(c->device, &ns);
        if (frame)
        {
            convertFrame(frame, &c->format, c->buffers[index]);
            SDL_ReleaseCameraFrame(c->device, frame);
            break;
        }
        if (c->nonblock)
        {
            errno = EAGAIN;
            return -1;
        }
        if (SDL_GetTicks() > deadline)
        {
            log_warn("Webcam: no picture from the camera");
            errno = EIO;
            return -1;
        }
        // Not under the lock: the game may close or stop meanwhile.
        ReleaseSRWLockExclusive(&cameraLock);
        SDL_Delay(2);
        AcquireSRWLockExclusive(&cameraLock);
        if (!c->streaming || !c->device || !c->queued[index])
        {
            errno = EINVAL;
            return -1;
        }
    }
    c->queued[index] = 0;
    Uint64 now = SDL_GetTicksNS();
    memset(b, 0, sizeof(*b));
    b->index = (uint32_t)index;
    b->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    b->bytesused = c->format.sizeimage;
    b->flags = V4L2_BUF_FLAG_MAPPED | V4L2_BUF_FLAG_DONE | V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
    b->field = V4L2_FIELD_NONE;
    b->tv_sec = (int32_t)(now / 1000000000);
    b->tv_usec = (int32_t)(now / 1000 % 1000000);
    b->sequence = c->sequence++;
    b->memory = V4L2_MEMORY_MMAP;
    b->offset = (uint32_t)index * ((c->format.sizeimage + 0xfff) & ~0xfffu);
    b->length = c->format.sizeimage;
    return 0;
}

static int32_t *control(CameraFd *c, uint32_t id)
{
    for (int i = 0; i < MAX_CONTROLS; i++)
        if (c->controlIds[i] == id)
            return &c->controlValues[i];
    for (int i = 0; i < MAX_CONTROLS; i++)
        if (!c->controlIds[i])
        {
            c->controlIds[i] = id;
            c->controlValues[i] = 128;
            return &c->controlValues[i];
        }
    return NULL;
}

int webcamIoctl(int fd, unsigned long request, void *arg)
{
    int ret = 0;
    AcquireSRWLockExclusive(&cameraLock);
    CameraFd *c = find(fd);
    if (!c || (!arg && request != VIDIOC_STREAMON && request != VIDIOC_STREAMOFF))
    {
        ReleaseSRWLockExclusive(&cameraLock);
        errno = c ? EFAULT : EBADF;
        return -1;
    }
    switch (request)
    {
    case VIDIOC_QUERYCAP:
    {
        V4l2Capability *cap = arg;
        const char *name = SDL_GetCameraName(cameraId(c->camera));
        memset(cap, 0, sizeof(*cap));
        snprintf((char *)cap->driver, sizeof(cap->driver), "uvcvideo");
        snprintf((char *)cap->card, sizeof(cap->card), "%s", name ? name : "Webcam");
        snprintf((char *)cap->bus_info, sizeof(cap->bus_info), "usb-windows-%d", c->camera);
        cap->version = 0x00050f00;
        cap->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
        cap->capabilities = cap->device_caps | V4L2_CAP_DEVICE_CAPS;
        break;
    }
    case VIDIOC_ENUM_FMT:
    {
        V4l2FmtDesc *d = arg;
        uint32_t index = d->index, type = d->type;
        if (index > 1)
        {
            errno = EINVAL;
            ret = -1;
            break;
        }
        memset(d, 0, sizeof(*d));
        d->index = index;
        d->type = type;
        d->pixelformat = index == 0 ? V4L2_PIX_FMT_YUYV : V4L2_PIX_FMT_BGR24;
        snprintf((char *)d->description, sizeof(d->description), index == 0 ? "YUYV 4:2:2" : "24-bit BGR 8-8-8");
        break;
    }
    case VIDIOC_G_FMT:
        ((V4l2Format *)arg)->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ((V4l2Format *)arg)->fmt.pix = c->format;
        break;
    case VIDIOC_S_FMT:
    case VIDIOC_TRY_FMT:
    {
        V4l2Format *f = arg;
        V4l2PixFormat chosen;
        if (request == VIDIOC_S_FMT && c->streaming)
        {
            errno = EBUSY;
            ret = -1;
            break;
        }
        setFormat(&chosen, f->fmt.pix.width, f->fmt.pix.height, f->fmt.pix.pixelformat);
        f->fmt.pix = chosen;
        if (request == VIDIOC_S_FMT)
            c->format = chosen;
        break;
    }
    case VIDIOC_REQBUFS:
    {
        V4l2RequestBuffers *r = arg;
        if (r->memory != V4L2_MEMORY_MMAP || c->streaming)
        {
            errno = c->streaming ? EBUSY : EINVAL;
            ret = -1;
            break;
        }
        freeBuffers(c);
        uint32_t count = r->count > MAX_BUFFERS ? MAX_BUFFERS : r->count;
        uint32_t stride = (c->format.sizeimage + 0xfff) & ~0xfffu;
        for (uint32_t i = 0; i < count; i++)
        {
            if (!(c->buffers[i] = VirtualAlloc(NULL, stride, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)))
                break;
            c->bufferCount++;
        }
        r->count = (uint32_t)c->bufferCount;
        r->capabilities = 1; // V4L2_BUF_CAP_SUPPORTS_MMAP
        break;
    }
    case VIDIOC_QUERYBUF:
    case VIDIOC_QBUF:
    {
        V4l2Buffer *b = arg;
        uint32_t index = b->index;
        if (index >= (uint32_t)c->bufferCount)
        {
            errno = EINVAL;
            ret = -1;
            break;
        }
        if (request == VIDIOC_QBUF)
            c->queued[index] = 1;
        memset(b, 0, sizeof(*b));
        b->index = index;
        b->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b->flags = V4L2_BUF_FLAG_MAPPED | (c->queued[index] ? V4L2_BUF_FLAG_QUEUED : 0);
        b->field = V4L2_FIELD_NONE;
        b->memory = V4L2_MEMORY_MMAP;
        b->offset = index * ((c->format.sizeimage + 0xfff) & ~0xfffu);
        b->length = c->format.sizeimage;
        break;
    }
    case VIDIOC_DQBUF:
        ret = dequeue(c, arg);
        break;
    case VIDIOC_STREAMON:
        if (!c->streaming)
        {
            // XRGB8888 at the game's size: SDL converts and scales the
            // camera's own pictures to it.
            SDL_CameraSpec spec;
            memset(&spec, 0, sizeof(spec));
            spec.format = SDL_PIXELFORMAT_XRGB8888;
            spec.colorspace = SDL_COLORSPACE_SRGB;
            spec.width = (int)c->format.width;
            spec.height = (int)c->format.height;
            spec.framerate_numerator = 30;
            spec.framerate_denominator = 1;
            c->device = SDL_OpenCamera(cameraId(c->camera), &spec);
            if (!c->device)
            {
                log_warn("Webcam: camera %d not opened: %s", c->camera, SDL_GetError());
                errno = EIO;
                ret = -1;
                break;
            }
            c->streaming = 1;
            c->sequence = 0;
            log_info("Webcam: %s streaming at %ux%u", SDL_GetCameraName(cameraId(c->camera)), c->format.width,
                     c->format.height);
        }
        break;
    case VIDIOC_STREAMOFF:
        stopStreaming(c);
        break;
    case VIDIOC_G_PARM:
    case VIDIOC_S_PARM:
    {
        V4l2StreamParm *p = arg;
        memset(&p->parm, 0, sizeof(p->parm));
        p->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        p->parm.capture.capability = 0x1000; // V4L2_CAP_TIMEPERFRAME
        p->parm.capture.numerator = 1;
        p->parm.capture.denominator = 30;
        p->parm.capture.readbuffers = 2;
        break;
    }
    case VIDIOC_ENUMINPUT:
    {
        V4l2Input *in = arg;
        uint32_t index = in->index;
        if (index)
        {
            errno = EINVAL;
            ret = -1;
            break;
        }
        memset(in, 0, sizeof(*in));
        snprintf((char *)in->name, sizeof(in->name), "Camera 1");
        in->type = V4L2_INPUT_TYPE_CAMERA;
        break;
    }
    case VIDIOC_G_INPUT:
        *(uint32_t *)arg = 0;
        break;
    case VIDIOC_S_INPUT:
        if (*(uint32_t *)arg)
        {
            errno = EINVAL;
            ret = -1;
        }
        break;
    case VIDIOC_QUERYCTRL:
    {
        // Any control the game asks about: 0..255, as a UVC camera's.
        V4l2QueryCtrl *q = arg;
        uint32_t id = q->id;
        if (id & 0x80000000) // V4L2_CTRL_FLAG_NEXT_CTRL: no enumeration
        {
            errno = EINVAL;
            ret = -1;
            break;
        }
        memset(q, 0, sizeof(*q));
        q->id = id;
        q->type = V4L2_CTRL_TYPE_INTEGER;
        snprintf((char *)q->name, sizeof(q->name), "Control %08x", id);
        q->maximum = 255;
        q->step = 1;
        q->default_value = 128;
        break;
    }
    case VIDIOC_G_CTRL:
    case VIDIOC_S_CTRL:
    {
        V4l2Control *ctl = arg;
        int32_t *value = control(c, ctl->id);
        if (value && request == VIDIOC_S_CTRL)
            *value = ctl->value;
        else if (value)
            ctl->value = *value;
        break;
    }
    case VIDIOC_CROPCAP:
    {
        uint32_t *cap = arg; // type, bounds, defrect, pixelaspect
        cap[3] = cap[7] = c->format.width;
        cap[4] = cap[8] = c->format.height;
        cap[1] = cap[2] = cap[5] = cap[6] = 0;
        cap[9] = cap[10] = 1;
        break;
    }
    case VIDIOC_G_CROP:
    case VIDIOC_S_CROP:
        break;
    case VIDIOC_ENUM_FRAMESIZES:
    {
        V4l2FrmSizeEnum *s = arg;
        static const uint32_t sizes[][2] = {{640, 480}, {1280, 720}, {320, 240}};
        if (s->index >= sizeof(sizes) / sizeof(sizes[0]))
        {
            errno = EINVAL;
            ret = -1;
            break;
        }
        s->type = V4L2_FRMSIZE_TYPE_DISCRETE;
        s->width = sizes[s->index][0];
        s->height = sizes[s->index][1];
        break;
    }
    case VIDIOC_ENUM_FRAMEINTERVALS:
    {
        V4l2FrmIvalEnum *v = arg;
        if (v->index)
        {
            errno = EINVAL;
            ret = -1;
            break;
        }
        v->type = V4L2_FRMIVAL_TYPE_DISCRETE;
        v->numerator = 1;
        v->denominator = 30;
        break;
    }
    default:
        log_info("Webcam: ioctl 0x%08lx not answered", request);
        errno = EINVAL;
        ret = -1;
        break;
    }
    ReleaseSRWLockExclusive(&cameraLock);
    return ret;
}

#endif
