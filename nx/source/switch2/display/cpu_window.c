/* SPDX-License-Identifier: ISC
 * A display window for CPU-written images.
 *
 * Switch 1's framebuffer gives nvmap-registered CPU memory to a vi buffer
 * queue. On Switch 2, this window registers the same memory with the GSV
 * GPU worker and imports it as GSV display images. The default window's VI2
 * layer (nwindowGetVi2Layer) presents them. There is no GPU mapping and no
 * GPU work.
 */
#include <string.h>
#include "cpu_window.h"
#include "kernel/mutex.h"
#include "kernel/svc.h"
#include "switch2/kernel/svc.h"
#include "arm/counter.h"
#include "services/nv.h"

// Turing Color2D GOB: 64 bytes by 8 rows of 16-byte lines, in blocks of 16
// GOBs (128 rows). See NIL src/nouveau/nil/tiling.rs and copy.rs
// CopyGOBTuring2D. Layout block format 1 selects Turing sector order.
#define GOB_WIDTH_B        64
#define GOB_HEIGHT         8
#define GOB_SIZE_B         (GOB_WIDTH_B * GOB_HEIGHT)
#define BLOCK_HEIGHT_LOG2  7    // rows: 16 GOBs
#define BLOCK_HEIGHT       (1u << BLOCK_HEIGHT_LOG2)
#define BLOCK_SIZE_B       (GOB_SIZE_B * (BLOCK_HEIGHT / GOB_HEIGHT))

#define IMAGE_FORMAT_RGBA8         1
#define IMAGE_BLOCK_FORMAT_TURING  1
// Image memory type for display images.
#define IMAGE_MEMORY_TYPE          3

// Frame mode a game's startup registers.
#define FRAME_MODE   2
// Main-session slot of the worker.
#define WORKER_SLOT  0
// GPU client name "libnxfb", packed little-endian, for gsvGpuRegisterClient.
#define CLIENT_NAME  0x006266786e62696cull
// SVC 0x80 attribute set on the backing before gsvGpuRegisterMemory.
#define BACKING_PREPARE_BIT  0x20
#define BACKING_ALIGN        0x10000

// The game AS import on /dev/nvhost-as-gpu: version 5, the VM ID at +4 and
// the user interval [0x08000000, 0xc000000000) at +16.
#define AS_IMPORT_IOCTL    0x40284109
#define AS_IMPORT_VERSION  5
#define AS_USER_FIRST      0x08000000ull
#define AS_USER_END        0xc000000000ull

// A present without this flag unregisters the layer's image.
#define PRESENT_IMAGE_FLAG  1ull

#define WAIT_LIMIT_NS  2000000000ull
#define POLL_NS        1000000ull

static CpuWindow g_defaultWindow;
static Mutex g_lifetimeMutex;
static u32 g_sharedWindows;
static Mutex g_retirementMutex;
static Result g_workerRetirementError;

static u32 _cpuwinPitch(u32 width) {
    return (width * 4 + GOB_WIDTH_B - 1) & ~(GOB_WIDTH_B - 1);
}

static u64 _cpuwinImageSize(u32 width, u32 height) {
    const u32 height_aligned = (height + BLOCK_HEIGHT - 1) & ~(BLOCK_HEIGHT - 1);
    return (u64)_cpuwinPitch(width) * height_aligned;
}

static void _cpuwinMakeLayout(GsvImageLayout* out, u32 width, u32 height) {
    gsvMakeImageLayout(out, width, height, BLOCK_HEIGHT_LOG2,
                       _cpuwinPitch(width) / GOB_WIDTH_B, IMAGE_FORMAT_RGBA8,
                       IMAGE_BLOCK_FORMAT_TURING, 0);
}

u32 cpuwinPixelOffset(u32 width, u32 x, u32 y) {
    const u32 x_b = x * 4;
    const u32 blocks_per_row = _cpuwinPitch(width) / GOB_WIDTH_B;
    const u32 block = (y / BLOCK_HEIGHT) * blocks_per_row + x_b / GOB_WIDTH_B;
    const u32 gob = (y % BLOCK_HEIGHT) / GOB_HEIGHT;
    // Inside the GOB: two 32-byte halves, each 4x4 sectors of 16-byte lines.
    const u32 gx = x_b % GOB_WIDTH_B, gy = y % GOB_HEIGHT;
    return block * BLOCK_SIZE_B + gob * GOB_SIZE_B +
           (gx / 32) * 0x100 + (gy / 4) * 0x80 + ((gx % 32) / 16) * 0x40 +
           (gy % 4) * 0x10 + (gx % 16);
}

// A VI2 release: the complemented counter ID at +0, the mode at +4 and the
// threshold at +8. All are zero when unused.
static u32 _cpuwinReleaseToken(const Vi2Dependency* d) {
    u32 token;
    memcpy(&token, d->data, sizeof(token));
    return token;
}

static u64 _cpuwinReleaseThreshold(const Vi2Dependency* d) {
    u64 threshold;
    memcpy(&threshold, d->data + 8, sizeof(threshold));
    return threshold;
}

static bool _cpuwinReleaseEmpty(const Vi2Dependency* d) {
    return !_cpuwinReleaseToken(d) && !d->data[4] && !_cpuwinReleaseThreshold(d);
}

// Waits for a release through the GSV sync counter it names
// (gsvSyncReadCounter).
static Result _cpuwinWaitRelease(CpuWindow* w, const Vi2Dependency* d) {
    if (_cpuwinReleaseEmpty(d))
        return 0;
    const u64 start = armGetSystemTick();
    for (;;) {
        u64 counter = 0;
        u8 mode = 0;
        Result rc = gsvSyncReadCounter(&w->sync, ~_cpuwinReleaseToken(d), &counter, &mode);
        if (R_FAILED(rc))
            return rc;
        if (mode != 1)
            return MAKERESULT(Module_Libnx, LibnxError_BadGfxDequeueBuffer);
        if (counter >= _cpuwinReleaseThreshold(d))
            return 0;
        if (armTicksToNs(armGetSystemTick() - start) > WAIT_LIMIT_NS)
            return KERNELRESULT(TimedOut);
        svcSleepThread(POLL_NS);
    }
}

// Gets a free VI2 slot. Consume the layer's availability notification,
// query, and wait for the next notification.
static Result _cpuwinAcquireSlot(CpuWindow* w, u8* out_slot) {
    const u64 start = armGetSystemTick();
    vi2WaitLayerAvailability(w->layer, 0);
    for (;;) {
        u64 available = 0;
        Result rc = vi2GetLayerAvailability(w->layer, &available);
        if (R_FAILED(rc))
            return rc;
        if (available) {
            *out_slot = (u8)__builtin_ctzll(available);
            return 0;
        }
        if (armTicksToNs(armGetSystemTick() - start) > WAIT_LIMIT_NS)
            return KERNELRESULT(TimedOut);
        vi2WaitLayerAvailability(w->layer, 100 * POLL_NS);
    }
}

// GSV root, nvservices, sync and frame registration, then the GPU session,
// its worker and a client.
static Result _cpuwinOpenGpu(CpuWindow* w) {
    Result rc = gsvInitialize();
    if (R_FAILED(rc))
        return rc;
    rc = nvInitialize();
    if (R_FAILED(rc))
        goto fail_gsv;
    rc = gsvOpenSync(&w->sync);
    if (R_FAILED(rc))
        goto fail_nv;
    rc = gsvRegisterFrame(FRAME_MODE, NULL);
    if (R_FAILED(rc))
        goto fail_sync;
    rc = gsvOpenGpu(&w->gpu);
    if (R_FAILED(rc))
        goto fail_sync;
    rc = gsvGpuCreateSlotEvent(&w->gpu, WORKER_SLOT, &w->slot_event, false);
    if (R_FAILED(rc))
        goto fail_gpu;
    rc = gsvGpuOpenWorker(&w->gpu, WORKER_SLOT, &w->slot_event);
    if (R_FAILED(rc))
        goto fail_slot_event;
    rc = gsvGpuRegisterClient(&w->gpu, CLIENT_NAME, &w->client);
    if (R_FAILED(rc))
        goto fail_slot_event;
    return 0;

fail_slot_event:
    eventClose(&w->slot_event);
fail_gpu:
    gsvGpuClose(&w->gpu);
fail_sync:
    gsvSyncClose(&w->sync);
fail_nv:
    nvExit();
fail_gsv:
    gsvExit();
    return rc;
}

static void _cpuwinCloseGpu(CpuWindow* w) {
    gsvGpuReleaseClient(&w->gpu, w->client);
    gsvSyncClose(&w->sync);
    eventClose(&w->slot_event);
    gsvGpuClose(&w->gpu);
    nvExit();
    gsvExit();
}

// A VM registered with the root and imported on nvhost-as-gpu. Without the
// import, gsvRegisterImageMemory returns 0x280f3.
static Result _cpuwinOpenAddressSpace(CpuWindow* w) {
    u32 vm_id = 0;
    Result rc = gsvGpuCreateVm(&w->gpu, &vm_id);
    GsvGpuVaRange user[4];
    u32 user_count = 0;
    if (R_SUCCEEDED(rc))
        rc = gsvGpuGetVmUserRanges(&w->gpu, vm_id, user, &user_count);
    GsvGpuVaRange gsv_range;
    if (R_SUCCEEDED(rc))
        rc = gsvGpuGetVmGsvRange(&w->gpu, vm_id, &gsv_range);
    if (R_SUCCEEDED(rc))
        rc = gsvRegisterVm(vm_id);
    if (R_SUCCEEDED(rc))
        rc = nvOpen(&w->as_fd, "/dev/nvhost-as-gpu");
    if (R_FAILED(rc))
        return rc;

    struct {
        u32 version;
        u32 vm_id;
        u64 reserved;
        u64 user_first;
        u64 user_end;
        u64 reserved2;
    } argument = { AS_IMPORT_VERSION, vm_id, 0, AS_USER_FIRST, AS_USER_END, 0 };
    _Static_assert(sizeof(argument) == 40, "AS import argument size");
    rc = nvIoctl(w->as_fd, AS_IMPORT_IOCTL, &argument);
    if (R_FAILED(rc))
        nvClose(w->as_fd);
    return rc;
}

Result cpuwinOpen(CpuWindow* w) {
    memset(w, 0, sizeof(*w));
    Result rc = _cpuwinOpenGpu(w);
    if (R_FAILED(rc))
        return rc;
    rc = _cpuwinOpenAddressSpace(w);
    if (R_FAILED(rc))
        goto fail_gpu;
    // The default window owns the layer (default_window.c).
    rc = nwindowGetVi2Layer(nwindowGetDefault(), &w->layer);
    if (R_FAILED(rc))
        goto fail_address_space;
    w->is_open = true;
    return 0;

fail_address_space:
    nvClose(w->as_fd);
fail_gpu:
    _cpuwinCloseGpu(w);
    return rc;
}

// g_lifetimeMutex protects opening, borrowing and closing the default session.
static CpuWindow* _cpuwinGetDefaultLocked(void) {
    if (!g_defaultWindow.is_open && R_FAILED(cpuwinOpen(&g_defaultWindow)))
        return NULL;
    return &g_defaultWindow;
}

Result cpuwinOpenShared(CpuWindow* w, Vi2Layer* layer) {
    // A process gets one GPU session. The window borrows the default window's.
    if (!w || !layer)
        return MAKERESULT(Module_Libnx, LibnxError_BadGfxInit);
    mutexLock(&g_lifetimeMutex);
    CpuWindow* base = _cpuwinGetDefaultLocked();
    if (!base) {
        mutexUnlock(&g_lifetimeMutex);
        return MAKERESULT(Module_Libnx, LibnxError_BadGfxInit);
    }
    memset(w, 0, sizeof(*w));
    w->sync = base->sync;
    w->gpu = base->gpu;
    w->slot_event = base->slot_event;
    w->client = base->client;
    w->as_fd = base->as_fd;
    w->layer = layer;
    w->is_shared = true;
    w->is_open = true;
    ++g_sharedWindows;
    mutexUnlock(&g_lifetimeMutex);
    return 0;
}

void cpuwinClose(CpuWindow* w) {
    if (!w->is_open || w->backing)
        return;
    if (w->is_shared) {
        mutexLock(&g_lifetimeMutex);
        --g_sharedWindows;
        memset(w, 0, sizeof(*w));
        mutexUnlock(&g_lifetimeMutex);
        return;
    }
    nvClose(w->as_fd);
    _cpuwinCloseGpu(w);
    memset(w, 0, sizeof(*w));
}

Result cpuwinGetImageSize(u32 width, u32 height, u64* out_size) {
    if (!width || width > (UINT32_MAX - (GOB_WIDTH_B - 1)) / 4 ||
        !height || height > UINT32_MAX - (BLOCK_HEIGHT - 1) || !out_size)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    const u64 expected_size = _cpuwinImageSize(width, height);
    if (expected_size > UINT32_MAX)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    GsvImageLayout layout;
    _cpuwinMakeLayout(&layout, width, height);
    u64 size = 0;
    Result rc = gsvGetImageSize(&layout, &size);
    if (R_FAILED(rc))
        return rc;
    // cpuwinPixelOffset writes this layout. GSV must agree on its size.
    if (size != expected_size)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    *out_size = size;
    return 0;
}

// Imports one image of the backing: gsvReserveImageMemory,
// gsvRegisterImageMemory, gsvCreateImage and gsvCreateDisplayAlias.
static Result _cpuwinCreateImage(CpuWindow* w, u32 index, u64 image_size) {
    CpuWindowImage* image = &w->images[index];
    GsvImageLayout layout;
    _cpuwinMakeLayout(&layout, w->width, w->height);
    const u64 offset = index * image_size;
    Result rc = gsvReserveImageMemory(IMAGE_MEMORY_TYPE, image_size, &image->memory_id);
    if (R_FAILED(rc))
        goto fail;
    rc = gsvRegisterImageMemory(image->memory_id, (uintptr_t)w->backing + offset,
                                image_size, w->range_id, offset);
    if (R_FAILED(rc))
        goto fail_memory;
    rc = gsvCreateImage(&layout, image->memory_id, &image->image_id);
    if (R_FAILED(rc))
        goto fail_memory;
    rc = gsvCreateDisplayAlias(image->image_id, 1);
    if (R_FAILED(rc))
        goto fail_image;
    return 0;

fail_image:
    w->retirement_error = gsvDestroyImage(image->image_id);
    if (R_FAILED(w->retirement_error)) return rc;
fail_memory:
    w->retirement_error = gsvReleaseImageMemory(image->memory_id);
fail:
    if (R_SUCCEEDED(w->retirement_error)) memset(image, 0, sizeof(*image));
    return rc;
}

static Result _cpuwinReleaseImages(CpuWindow* w) {
    for (u32 i = 0; i < w->num_images; i++) {
        CpuWindowImage* image = &w->images[i];
        Result rc = gsvDestroyDisplayAlias(image->image_id);
        if (R_SUCCEEDED(rc)) rc = gsvDestroyImage(image->image_id);
        if (R_SUCCEEDED(rc)) rc = gsvReleaseImageMemory(image->memory_id);
        if (R_FAILED(rc)) return w->retirement_error = rc;
        memset(image, 0, sizeof(*image));
    }
    w->num_images = 0;
    return 0;
}

// Registers the backing with the worker, CPU-cached. The steps are SVC 0x80
// prepare, then gsvGpuRegisterMemory.
static Result _cpuwinRegisterBacking(CpuWindow* w) {
    Result rc = svcUnknown80(w->backing, w->backing_size, BACKING_PREPARE_BIT, BACKING_PREPARE_BIT);
    if (R_FAILED(rc)) {
        w->backing = NULL;
        return rc;
    }
    rc = gsvGpuRegisterMemory(&w->gpu, (uintptr_t)w->backing, w->backing_size, w->client, &w->range_id);
    if (R_SUCCEEDED(rc) && !w->range_id)
        return w->retirement_error = MAKERESULT(Module_Libnx, LibnxError_InvalidCmifOutHeader);
    if (R_FAILED(rc)) {
        w->retirement_error = svcUnknown80(w->backing, w->backing_size, BACKING_PREPARE_BIT, 0);
        if (R_SUCCEEDED(w->retirement_error)) w->backing = NULL;
    }
    return rc;
}

// The worker signals the slot event when it retires the range. Then this
// function clears the SVC 0x80 attribute.
static Result _cpuwinUnregisterBacking(CpuWindow* w) {
    // All windows share one slot event. A late signal after an error cannot
    // safely be identified as a later request's acknowledgement.
    mutexLock(&g_retirementMutex);
    Result rc = g_workerRetirementError;
    if (R_SUCCEEDED(rc)) {
        rc = eventClear(&w->slot_event);
        if (R_VALUE(rc) == KERNELRESULT(InvalidState)) rc = 0; // Already unsignalled.
        if (R_SUCCEEDED(rc)) {
            rc = gsvGpuUnregisterMemory(&w->gpu, w->range_id, WORKER_SLOT);
            if (R_SUCCEEDED(rc)) rc = eventWait(&w->slot_event, WAIT_LIMIT_NS);
            g_workerRetirementError = rc;
        }
    }
    mutexUnlock(&g_retirementMutex);
    if (R_SUCCEEDED(rc))
        rc = svcUnknown80(w->backing, w->backing_size, BACKING_PREPARE_BIT, 0);
    if (R_FAILED(rc)) return w->retirement_error = rc;
    w->backing = NULL;
    w->backing_size = 0;
    w->range_id = 0;
    w->layer_unregistered = false;
    return rc;
}

Result cpuwinCreateImages(CpuWindow* w, void* backing, u64 backing_size, u32 width, u32 height, u32 num_images) {
    if (!w->is_open || w->backing)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    if (!backing || ((uintptr_t)backing & (BACKING_ALIGN - 1)) || !num_images || num_images > CPUWIN_MAX_IMAGES)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    u64 image_size = 0;
    Result rc = cpuwinGetImageSize(width, height, &image_size);
    if (R_FAILED(rc))
        return rc;
    if (backing_size < num_images * image_size || (backing_size & 0xFFF))
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    w->backing = backing;
    w->backing_size = backing_size;
    w->width = width;
    w->height = height;
    rc = _cpuwinRegisterBacking(w);
    if (R_FAILED(rc))
        return rc;
    for (u32 i = 0; i < num_images && R_SUCCEEDED(rc); i++) {
        rc = _cpuwinCreateImage(w, i, image_size);
        if (R_SUCCEEDED(rc))
            w->num_images = i + 1;
    }
    // No present used these images, so rollback skips VI2 retirement.
    if (R_FAILED(rc) && R_SUCCEEDED(w->retirement_error) && R_SUCCEEDED(_cpuwinReleaseImages(w)))
        _cpuwinUnregisterBacking(w);
    return rc;
}

// Retires the images. The steps are:
// 1. A present without the image flag (layer-wide). Its release must be empty.
// 2. Every image's last release.
// 3. The images and the backing registration.
Result cpuwinDestroyImages(CpuWindow* w) {
    if (R_FAILED(w->retirement_error))
        return w->retirement_error;
    if (!w->backing)
        return 0;
    Result rc = 0;
    if (w->num_images && !w->layer_unregistered) {
        u8 slot = 0;
        rc = _cpuwinAcquireSlot(w, &slot);
        if (R_FAILED(rc)) return rc;
        Vi2PresentOptions options;
        vi2MakeBasicPresentOptions(&options, w->width, w->height, armGetSystemTick());
        options.flags &= ~PRESENT_IMAGE_FLAG;
        u64 available = 0;
        Vi2Dependency release = {0};
        rc = vi2UnregisterLayerImage(w->layer, slot, &options, &available, &release);
        if (R_SUCCEEDED(rc) && !_cpuwinReleaseEmpty(&release))
            rc = MAKERESULT(Module_Libnx, LibnxError_BadGfxQueueBuffer);
        if (R_FAILED(rc)) return w->retirement_error = rc;
        w->layer_unregistered = true;
    }
    for (u32 i = 0; i < w->num_images && R_SUCCEEDED(rc); i++)
        rc = _cpuwinWaitRelease(w, &w->images[i].release);
    // On failure, the display may still read the images. Keep them and
    // their backing.
    if (R_FAILED(rc))
        return rc;

    rc = _cpuwinReleaseImages(w);
    return R_SUCCEEDED(rc) ? _cpuwinUnregisterBacking(w) : rc;
}

Result cpuwinWaitImage(CpuWindow* w, u32 index) {
    if (index >= w->num_images)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    return _cpuwinWaitRelease(w, &w->images[index].release);
}

// VI2 present with an empty producer dependency, because no GPU work
// produced the image. The returned release must be a counter release
// (mode 1).
Result cpuwinPresentImage(CpuWindow* w, u32 index) {
    if (index >= w->num_images)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    CpuWindowImage* image = &w->images[index];
    u8 slot = 0;
    Result rc = _cpuwinAcquireSlot(w, &slot);
    if (R_FAILED(rc))
        return rc;
    Vi2PresentOptions options;
    vi2MakeBasicPresentOptions(&options, w->width, w->height, armGetSystemTick());
    nwindowGetPresentInterval(nwindowGetDefault(), &options.interval_numerator,
                              &options.interval_denominator);
    const Vi2Dependency producer = {0};
    u64 available = 0;
    rc = vi2PresentLayer(w->layer, slot, image->image_id, &options, &producer,
                         &available, &image->release);
    if (R_SUCCEEDED(rc) && image->release.data[4] != 1)
        rc = MAKERESULT(Module_Libnx, LibnxError_BadGfxQueueBuffer);
    return rc;
}

CpuWindow* cpuwinGetDefault(void) {
    mutexLock(&g_lifetimeMutex);
    CpuWindow* w = _cpuwinGetDefaultLocked();
    mutexUnlock(&g_lifetimeMutex);
    return w;
}

bool cpuwinDefaultHasImages(void) {
    return g_defaultWindow.backing != NULL;
}

// Retires a framebuffer left open at exit first. If the display still holds
// an image, everything stays open. Freeing memory that the display reads is
// worse than leaking the window.
bool cpuwinCloseDefault(void) {
    mutexLock(&g_lifetimeMutex);
    const bool closed = !g_sharedWindows && R_SUCCEEDED(cpuwinDestroyImages(&g_defaultWindow));
    if (closed) cpuwinClose(&g_defaultWindow);
    mutexUnlock(&g_lifetimeMutex);
    return closed;
}
