#include <stdlib.h>
#include <string.h>
#include "types.h"
#include "result.h"
#include "arm/cache.h"
#include "services/nv.h"
#include "services/vi.h"
#include "runtime/diag.h"
#include "display/binder.h"
#include "display/buffer_producer.h"
#include "display/native_window.h"
#include "display/framebuffer.h"
#include "nvidia/graphic_buffer.h"
#include "../runtime/alloc.h"
#if defined(__SWITCH2__)
#include "../switch2/display/cpu_window.h"
#endif

#if defined(__SWITCH2__)
// Framebuffers are GSV display images (switch2/display/cpu_window.c):
// RGBA8, Turing 2D block-linear. framebufferEnd converts the linear shadow
// buffer from the requested format. This function refuses RGB_888 and
// RGBA_5551, as on Switch 1, where they do not work.
static u32 _framebufferBytesPerPixel(u32 format)
{
    switch (format) {
        case PIXEL_FORMAT_RGBA_8888:
        case PIXEL_FORMAT_RGBX_8888:
        case PIXEL_FORMAT_BGRA_8888:
            return 4;
        case PIXEL_FORMAT_RGB_565:
        case PIXEL_FORMAT_RGBA_4444:
            return 2;
        default:
            return 0;
    }
}

static u32 _framebufferExpand(u32 value, u32 bits)
{
    return (value << (8 - bits)) | (value >> (2 * bits - 8));
}

// One pixel of the linear buffer as RGBA8 (R in the low byte).
static u32 _framebufferToRgba8(const u8* in, u32 format)
{
    u32 v32;
    u16 v16;
    switch (format) {
        case PIXEL_FORMAT_RGBA_8888:
            memcpy(&v32, in, 4);
            return v32;
        case PIXEL_FORMAT_RGBX_8888:
            memcpy(&v32, in, 4);
            return v32 | 0xFF000000;
        case PIXEL_FORMAT_BGRA_8888:
            memcpy(&v32, in, 4);
            return (v32 & 0xFF00FF00) | ((v32 >> 16) & 0xFF) | ((v32 & 0xFF) << 16);
        case PIXEL_FORMAT_RGB_565:
            memcpy(&v16, in, 2);
            return _framebufferExpand(v16 >> 11, 5) | _framebufferExpand((v16 >> 5) & 0x3F, 6) << 8 |
                   _framebufferExpand(v16 & 0x1F, 5) << 16 | 0xFF000000;
        default: // PIXEL_FORMAT_RGBA_4444 (A4B4G4R4)
            memcpy(&v16, in, 2);
            return (v16 & 0xF) * 0x11 | ((v16 >> 4) & 0xF) * 0x11 << 8 |
                   ((v16 >> 8) & 0xF) * 0x11 << 16 | (u32)(v16 >> 12) * 0x11 << 24;
    }
}

// Converts the linear shadow buffer into a display image. Four RGBA8 pixels
// starting at a multiple of 4 are one contiguous 16-byte GOB line.
static void _framebufferConvertToImage(u8* image, const u8* linear, u32 stride, u32 width, u32 height, u32 format)
{
    const u32 bpp = _framebufferBytesPerPixel(format);
    for (u32 y = 0; y < height; y++) {
        const u8* row = linear + y*stride;
        for (u32 x = 0; x < width; x += 4) {
            u32 line[4];
            const u32 count = width - x < 4 ? width - x : 4;
            for (u32 i = 0; i < count; i++)
                line[i] = _framebufferToRgba8(row + (x + i)*bpp, format);
            memcpy(image + cpuwinPixelOffset(width, x, y), line, count*4);
        }
    }
}
#else
static const NvColorFormat g_nvColorFmtTable[] = {
    NvColorFormat_A8B8G8R8, // PIXEL_FORMAT_RGBA_8888
    NvColorFormat_X8B8G8R8, // PIXEL_FORMAT_RGBX_8888
    NvColorFormat_R8_G8_B8, // PIXEL_FORMAT_RGB_888   <-- doesn't work
    NvColorFormat_R5G6B5,   // PIXEL_FORMAT_RGB_565
    NvColorFormat_A8R8G8B8, // PIXEL_FORMAT_BGRA_8888
    NvColorFormat_R5G5B5A1, // PIXEL_FORMAT_RGBA_5551 <-- doesn't work
    NvColorFormat_A4B4G4R4, // PIXEL_FORMAT_RGBA_4444
};
#endif

#if defined(__SWITCH2__)
// Only the default window. Its display layer opens here on first
// use, with GSV images registered from this CPU memory. See cpu_window.c.
Result framebufferCreate(Framebuffer* fb, NWindow *win, u32 width, u32 height, u32 format, u32 num_fbs)
{
    const u32 bytes_per_pixel = _framebufferBytesPerPixel(format);
    if (!fb || win != nwindowGetDefault() || !width || !height || !bytes_per_pixel || num_fbs < 1 || num_fbs > CPUWIN_MAX_IMAGES)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    CpuWindow* cw = cpuwinGetDefault();
    if (!cw)
        return MAKERESULT(Module_Libnx, LibnxError_BadGfxInit);
    if (cw->backing)
        return MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized);

    u64 image_size = 0;
    Result rc = cpuwinGetImageSize(width, height, &image_size);
    if (R_FAILED(rc))
        return rc;

    memset(fb, 0, sizeof(*fb));
    const u64 buf_size = (num_fbs*image_size + 0xFFF) &~ 0xFFF;
    fb->buf = __libnx_aligned_alloc(0x10000, buf_size);
    if (!fb->buf)
        return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    memset(fb->buf, 0, buf_size);
    armDCacheFlush(fb->buf, buf_size);

    rc = cpuwinCreateImages(cw, fb->buf, buf_size, width, height, num_fbs);
    if (R_FAILED(rc)) {
        // Rollback can fail while the worker still owns the backing.
        if (cw->backing != fb->buf)
            __libnx_free(fb->buf);
        fb->buf = NULL;
        return rc;
    }

    const u32 width_aligned_bytes = (width*bytes_per_pixel + 63) &~ 63;
    fb->has_init = true;
    fb->win = win;
    fb->num_fbs = num_fbs;
    fb->stride = width_aligned_bytes;
    fb->width_aligned = width_aligned_bytes / bytes_per_pixel;
    fb->height_aligned = (height + 127) &~ 127;
    fb->fb_size = image_size;
    win->width = width;
    win->height = height;
    win->format = format;
    win->cur_slot = num_fbs - 1;
    return 0;
}
#else
Result framebufferCreate(Framebuffer* fb, NWindow *win, u32 width, u32 height, u32 format, u32 num_fbs)
{
    Result rc = 0;
    if (!fb || !nwindowIsValid(win) || !width || !height || format < PIXEL_FORMAT_RGBA_8888 || format > PIXEL_FORMAT_RGBA_4444 || num_fbs < 1 || num_fbs > 3)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    rc = nwindowSetDimensions(win, width, height);
    if (R_FAILED(rc))
        return rc;

    rc = nvInitialize();
    if (R_SUCCEEDED(rc)) {
        rc = nvMapInit();
        if (R_SUCCEEDED(rc)) {
            rc = nvFenceInit();
            if (R_FAILED(rc))
                nvMapExit();
        }
        if (R_FAILED(rc))
            nvExit();
    }

    if (R_FAILED(rc))
        return rc;

    memset(fb, 0, sizeof(*fb));
    fb->has_init = true;
    fb->win = win;
    fb->num_fbs = num_fbs;

    const NvColorFormat colorfmt = g_nvColorFmtTable[format-PIXEL_FORMAT_RGBA_8888];
    const u32 bytes_per_pixel = ((u64)colorfmt >> 3) & 0x1F;
    const u32 block_height_log2 = 4; // According to TRM this is the optimal value (SIXTEEN_GOBS)
    const u32 block_height = 8 * (1U << block_height_log2);

    NvGraphicBuffer grbuf = {0};
    grbuf.header.num_ints = (sizeof(NvGraphicBuffer) - sizeof(NativeHandle)) / 4;
    grbuf.unk0 = -1;
    grbuf.magic = 0xDAFFCAFF;
    grbuf.pid = 42;
    grbuf.usage = GRALLOC_USAGE_HW_COMPOSER | GRALLOC_USAGE_HW_RENDER | GRALLOC_USAGE_HW_TEXTURE;
    grbuf.format = format;
    grbuf.ext_format = format;
    grbuf.num_planes = 1;
    grbuf.planes[0].width = width;
    grbuf.planes[0].height = height;
    grbuf.planes[0].color_format = colorfmt;
    grbuf.planes[0].layout = NvLayout_BlockLinear;
    grbuf.planes[0].kind = NvKind_Generic_16BX2;
    grbuf.planes[0].block_height_log2 = block_height_log2;

    // Calculate buffer dimensions and sizes
    const u32 width_aligned_bytes = (width*bytes_per_pixel + 63) &~ 63; // GOBs are 64 bytes wide
    const u32 width_aligned = width_aligned_bytes / bytes_per_pixel;
    const u32 height_aligned = (height + block_height - 1) &~ (block_height - 1);
    const u32 fb_size = width_aligned_bytes*height_aligned;
    const u32 buf_size = (num_fbs*fb_size + 0xFFF) &~ 0xFFF; // needs to be page aligned

    fb->buf = __libnx_aligned_alloc(0x1000, buf_size);
    if (!fb->buf)
        rc = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);

    if (R_SUCCEEDED(rc))
        rc = nvMapCreate(&fb->map, fb->buf, buf_size, 0x20000, NvKind_Pitch, true);

    if (R_SUCCEEDED(rc)) {
        grbuf.nvmap_id = nvMapGetId(&fb->map);
        grbuf.stride = width_aligned;
        grbuf.total_size = fb_size;
        grbuf.planes[0].pitch = width_aligned_bytes;
        grbuf.planes[0].size = fb_size;

        for (u32 i = 0; i < num_fbs; i ++) {
            grbuf.planes[0].offset = i*fb_size;
            rc = nwindowConfigureBuffer(win, i, &grbuf);
            if (R_FAILED(rc))
                break;
        }
    }

    if (R_SUCCEEDED(rc)) {
        fb->stride = width_aligned_bytes;
        fb->width_aligned = width_aligned;
        fb->height_aligned = height_aligned;
        fb->fb_size = fb_size;
    }

    if (R_FAILED(rc))
        framebufferClose(fb);

    return rc;
}
#endif

Result framebufferMakeLinear(Framebuffer* fb)
{
    if (!fb || !fb->has_init)
        return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    if (fb->buf_linear)
        return MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized);

    u32 height = (fb->win->height + 7) &~ 7; // GOBs are 8 rows tall
    fb->buf_linear = __libnx_alloc(fb->stride*height);
    if (!fb->buf_linear)
        return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);

    memset(fb->buf_linear, 0, fb->stride*height);
    return 0;
}

#if defined(__SWITCH2__)
// Waits for the display to release every image before freeing it.
// If that fails the images stay registered and their memory is not freed.
// Then it releases the window's GPU sessions and VI2 layer, as libnx's
// framebufferClose ends its nv sessions. They hold the process's GSV root,
// which delegates nvservices only once: a GPU user that opens after the
// framebuffer, such as a Vulkan device, needs a new root.
void framebufferClose(Framebuffer* fb)
{
    if (!fb || !fb->has_init)
        return;

    if (fb->buf_linear)
        __libnx_free(fb->buf_linear);

    bool images_released = false;
    if (fb->buf && R_SUCCEEDED(cpuwinDestroyImages(cpuwinGetDefault()))) {
        __libnx_free(fb->buf);
        images_released = true;
    }

    memset(fb, 0, sizeof(*fb));
    if (images_released)
        nwindowReleaseVi2Layer(nwindowGetDefault());
}
#else
void framebufferClose(Framebuffer* fb)
{
    if (!fb || !fb->has_init)
        return;

    if (fb->buf_linear)
        __libnx_free(fb->buf_linear);

    if (fb->buf) {
        nwindowReleaseBuffers(fb->win);
        nvMapClose(&fb->map);
        __libnx_free(fb->buf);
    }

    memset(fb, 0, sizeof(*fb));
    nvFenceExit();
    nvMapExit();
    nvExit();
}
#endif

#if defined(__SWITCH2__)
// Waits until the display releases the next image. Returns only the
// linear shadow buffer. See framebufferMakeLinear.
void* framebufferBegin(Framebuffer* fb, u32* out_stride)
{
    if (!fb->has_init || !fb->buf_linear)
        return NULL;

    const s32 slot = (fb->win->cur_slot + 1) % (s32)fb->num_fbs;
    Result rc = cpuwinWaitImage(cpuwinGetDefault(), slot);
    if (R_FAILED(rc))
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_BadGfxDequeueBuffer));
    fb->win->cur_slot = slot;

    if (out_stride)
        *out_stride = fb->stride;

    return fb->buf_linear;
}
#else
void* framebufferBegin(Framebuffer* fb, u32* out_stride)
{
    if (!fb->has_init)
        return NULL;

    s32 slot;
    Result rc = nwindowDequeueBuffer(fb->win, &slot, NULL);
    if (R_FAILED(rc))
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_BadGfxDequeueBuffer));

    if (out_stride)
        *out_stride = fb->stride;

    if (fb->buf_linear)
        return fb->buf_linear;

    return (u8*)fb->buf + slot*fb->fb_size;
}
#endif

#if !defined(__SWITCH2__)
static void _convertGobTo16Bx2(u8* outgob, const u8* ingob, u32 stride)
{
    // GOB byte offsets can be expressed with 9 bits:
    //   yyyxxxxxx  where 'x' is the horizontal position and 'y' is the vertical position
    // 16Bx2 sector ordering basically applies swizzling to the upper 5 bits:
    //   iiiiioooo  where 'o' doesn't change and 'i' gets swizzled
    // This swizzling of the 'i' field can be expressed the following way:
    //   43210 -> 14302  to go from unswizzled to swizzled offset
    //   32041 <- 43210  to go from swizzled to unswizzled offset
    // Here, we iterate through each of the 32 sequential swizzled positions and
    // calculate the actual X and Y positions in the unswizzled source image.
    // Since the 'o' bits aren't swizzled, we can copy the whole thing as a single 128-bit unit.

    for (u32 i = 0; i < 32; i ++) {
        const u32 y = ((i>>1)&0x06) | ( i    &0x01);
        const u32 x = ((i<<3)&0x10) | ((i<<1)&0x20);
        *(u128*)outgob = *(u128*)(ingob + y*stride + x);
        outgob += sizeof(u128);
    }
}

static void _convertToBlocklinear(void* outbuf, const void* inbuf, u32 stride, u32 height, u32 block_height_log2)
{
    const u32 block_height_gobs = 1U << block_height_log2;
    const u32 block_height_px = 8U << block_height_log2;

    const u32 width_blocks = stride >> 6;
    const u32 height_blocks = (height + block_height_px - 1) >> (3 + block_height_log2);
    u8* outgob = (u8*)outbuf;

    for (u32 block_y = 0; block_y < height_blocks; block_y ++) {
        for (u32 block_x = 0; block_x < width_blocks; block_x ++) {
            for (u32 gob_y = 0; gob_y < block_height_gobs; gob_y ++) {
                const u32 x = block_x*64;
                const u32 y = block_y*block_height_px + gob_y*8;
                if (y < height) {
                    const u8* ingob = (u8*)inbuf + y*stride + x;
                    _convertGobTo16Bx2(outgob, ingob, stride);
                }
                outgob += 512;
            }
        }
    }
}
#endif

#if defined(__SWITCH2__)
// Converts the linear shadow buffer into the image, cleans it from
// the data cache and presents it through VI2.
void framebufferEnd(Framebuffer* fb)
{
    if (!fb->has_init || !fb->buf_linear)
        return;

    u8* image = (u8*)fb->buf + fb->win->cur_slot*fb->fb_size;
    _framebufferConvertToImage(image, fb->buf_linear, fb->stride, fb->win->width, fb->win->height, fb->win->format);
    armDCacheFlush(image, fb->fb_size);

    Result rc = cpuwinPresentImage(cpuwinGetDefault(), fb->win->cur_slot);
    if (R_FAILED(rc))
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_BadGfxQueueBuffer));
}
#else
void framebufferEnd(Framebuffer* fb)
{
    if (!fb->has_init)
        return;

    void* buf = (u8*)fb->buf + fb->win->cur_slot*fb->fb_size;
    if (fb->buf_linear)
        _convertToBlocklinear(buf, fb->buf_linear, fb->stride, fb->win->height, 4);

    armDCacheFlush(buf, fb->fb_size);

    Result rc = nwindowQueueBuffer(fb->win, fb->win->cur_slot, NULL);
    if (R_FAILED(rc))
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_BadGfxQueueBuffer));
}
#endif
