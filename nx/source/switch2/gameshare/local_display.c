// SPDX-License-Identifier: MIT
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>
#include "../display/cpu_window.h"

enum { ImageCount = 2 };

struct GameShareLocalGuestDisplay {
    Vi2Display display;
    Vi2Endpoint endpoint;
    Vi2Layer layer;
    CpuWindow window;
    void *backing;
    u64 image_size;
    u32 width, height, next;
    bool vi2, display_open, layer_created, attached, selected, closing;
    Result close_error;
};

static const char *g_lastStep = "none";

const char *gameshareLocalGuestDisplayLastStep(void) {
    return g_lastStep;
}

static Result badInput(void) {
    return MAKERESULT(Module_Libnx, LibnxError_BadInput);
}

Result gameshareLocalGuestDisplayClose(GameShareLocalGuestDisplay *d) {
    if (!d) return badInput();
    if (R_FAILED(d->close_error)) return d->close_error;
    d->closing = true;
    Result rc;
    if (d->window.is_open) {
        rc = cpuwinDestroyImages(&d->window);
        if (R_FAILED(rc)) return rc;
        cpuwinClose(&d->window);
    }
    free(d->backing);
    d->backing = NULL;
    if (d->selected) {
        rc = vi2ClearLayerSelection(&d->endpoint);
        if (R_FAILED(rc)) goto park;
    }
    if (d->attached) {
        rc = vi2DetachLayer(&d->layer);
        if (R_FAILED(rc)) goto park;
    }
    if (d->layer_created) {
        rc = vi2CloseLayer(&d->layer);
        if (R_FAILED(rc)) goto park;
    }
    if (d->display_open) {
        rc = vi2CloseDisplay(&d->display);
        if (R_FAILED(rc)) goto park;
        vi2CloseEndpoint(&d->endpoint);
    }
    if (d->vi2) vi2Exit();
    free(d);
    return 0;

park:
    // A teardown command might have completed without an acknowledgement.
    d->close_error = rc;
    return rc;
}

Result gameshareLocalGuestDisplayOpen(GameShareLocalGuestDisplay **out, u8 guest, u32 width, u32 height) {
    g_lastStep = "validate the guest display";
    if (!out || guest >= GAMESHARE_LOCAL_MAX_GUESTS || !width || !height) return badInput();
    g_lastStep = "allocate the guest display";
    GameShareLocalGuestDisplay *d = calloc(1, sizeof(*d));
    if (!d) return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    d->width = width;
    d->height = height;

    // The firmware names each guest's display player1, player2 and player3.
    char name[8] = {'p', 'l', 'a', 'y', 'e', 'r', (char)('1' + guest), 0};
    g_lastStep = "start VI2";
    Result rc = vi2Initialize();
    if (R_FAILED(rc)) goto fail;
    d->vi2 = true;
    g_lastStep = "register the guest display";
    rc = vi2OpenDisplay(&d->display, name);
    if (R_FAILED(rc)) goto fail;
    d->display_open = true;
    vi2CreateEndpoint(&d->endpoint, &d->display, "default");
    g_lastStep = "create the guest layer";
    rc = vi2CreateLayer(&d->layer);
    if (R_FAILED(rc)) goto fail;
    d->layer_created = true;
    g_lastStep = "attach the guest layer";
    rc = vi2AttachLayer(&d->layer, &d->display);
    if (R_FAILED(rc)) goto fail;
    d->attached = true;
    g_lastStep = "select the guest layer";
    rc = vi2SelectLayer(&d->layer, &d->endpoint);
    if (R_FAILED(rc)) goto fail;
    d->selected = true;
    g_lastStep = "share the GPU session";
    rc = cpuwinOpenShared(&d->window, &d->layer);
    if (R_FAILED(rc)) goto fail;

    g_lastStep = "size the guest image";
    rc = cpuwinGetImageSize(width, height, &d->image_size);
    if (R_FAILED(rc)) goto fail;
    const u64 backing_size = (ImageCount * d->image_size + 0xfff) & ~0xfffull;
    g_lastStep = "allocate the guest images";
    d->backing = memalign(0x10000, backing_size);
    if (!d->backing) {
        rc = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
        goto fail;
    }
    memset(d->backing, 0, backing_size);
    armDCacheFlush(d->backing, backing_size);
    g_lastStep = "register the guest images";
    rc = cpuwinCreateImages(&d->window, d->backing, backing_size, width, height, ImageCount);
    if (R_FAILED(rc)) goto fail;

    g_lastStep = "open";
    *out = d;
    return 0;

fail:
    gameshareLocalGuestDisplayClose(d);
    return rc;
}

Result gameshareLocalGuestDisplayPresent(GameShareLocalGuestDisplay *d, const u32 *rgba, u32 stride_pixels) {
    if (!d || d->closing || !rgba || stride_pixels < d->width) return badInput();
    const u32 index = d->next;
    Result rc = cpuwinWaitImage(&d->window, index);
    if (R_FAILED(rc)) return rc;

    u8 *image = (u8 *)d->backing + index * d->image_size;
    // Four pixels starting at a multiple of four share one 16-byte GOB line.
    for (u32 y = 0; y < d->height; ++y) {
        const u32 *row = rgba + (size_t)y * stride_pixels;
        for (u32 x = 0; x < d->width; x += 4) {
            const u32 count = d->width - x < 4 ? d->width - x : 4;
            memcpy(image + cpuwinPixelOffset(d->width, x, y), row + x, count * 4);
        }
    }
    armDCacheFlush(image, d->image_size);
    rc = cpuwinPresentImage(&d->window, index);
    if (R_SUCCEEDED(rc)) d->next = (index + 1) % ImageCount;
    return rc;
}
