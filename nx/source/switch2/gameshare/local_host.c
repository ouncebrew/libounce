// SPDX-License-Identifier: MIT
#include <malloc.h>
#include <stddef.h>
#include <stdlib.h>
#include <switch.h>
#include "local_applet.h"
#include "local_buffers.h"

enum {
    Initialize = 2, Finalize = 3, StartGuest = 4, CloseSession = 5,
    GetConnectionEvent = 7, GetConnectionState = 8, SetPadStyles = 25,
    SetJoyHoldType = 27, SetTouchRange = 120, SetHidFeatures = 160, StartStreaming = 500,
    GetGuestCapacity = 501, GetStreamType = 502, GetStreamingMode = 503,
    ReleaseGuestBuffers = 507, EndMatchmake = 509, SetAudio = 510,
    // A nonzero libounce trace token selects CMIF v1 / RequestWithContext.
    TraceContext = 1,
};

typedef struct {
    u8 type;
    u8 mode;
    u16 reserved;
    s32 guest_count;
    u64 pid_placeholder;
} InitRequest;

_Static_assert(sizeof(InitRequest) == 16 && offsetof(InitRequest, pid_placeholder) == 8, "init ABI");
_Static_assert(sizeof(GameShareLocalConnection) == 8, "connection ABI");

static Result badInput(void) {
    return MAKERESULT(Module_Libnx, LibnxError_BadInput);
}

static Result invalidReply(void) {
    return MAKERESULT(Module_Libnx, LibnxError_InvalidCmifOutHeader);
}

static void rememberError(Result *first, Result rc) {
    if (R_SUCCEEDED(*first)) *first = rc;
}

static bool validGain(float gain) {
    return gain >= 0.0f && gain <= 1.0f;
}

static Result verifyLocalMode(GameShareLocalHost *host) {
    Service *shim = &host->client.shim;
    u8 type = 0xff, mode = 0xff;
    host->operation = "read StreamPlay type";
    Result rc = serviceDispatchOut(shim, GetStreamType, type, .context = TraceContext);
    if (R_FAILED(rc)) return rc;
    if (type != 0) return invalidReply();
    host->operation = "read streaming mode";
    rc = serviceDispatchOut(shim, GetStreamingMode, mode, .context = TraceContext);
    if (R_FAILED(rc)) return rc;
    return mode == host->mode ? 0 : invalidReply();
}

Result gameshareLocalHostPrepare(GameShareLocalHost *host, u8 max_guests, const GameShareLocalAudio *audio) {
    return gameshareLocalHostPrepareEx(host, max_guests, audio, 0);
}

Result gameshareLocalHostPrepareEx(GameShareLocalHost *host, u8 max_guests, const GameShareLocalAudio *audio,
                                   u8 mode) {
    if (!host || !audio || max_guests < 1 || max_guests > GAMESHARE_LOCAL_MAX_GUESTS || !validGain(audio->speaker_volume) ||
        !validGain(audio->tv_volume) || !validGain(audio->external_volume)) return badInput();
    if (host->work || host->state != GameShareHostIdle) return MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized);
    host->operation = "query local work memory";
    u32 size = 0;
    Result rc = gameshareLocalGetRequiredMemorySizeEx(&host->client, max_guests, mode, &size);
    if (R_FAILED(rc)) return rc;
    if (size == 0 || (size & 0xfff)) return invalidReply();

    host->operation = "allocate local work memory";
    host->work = memalign(0x1000, size);
    if (!host->work) return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    host->work_size = size;
    host->max_guests = max_guests;
    host->mode = mode;

    Service *shim = &host->client.shim;
    const InitRequest request = {.mode = mode, .guest_count = max_guests};
    host->operation = "initialize local StreamPlay";
    rc = serviceDispatchIn(shim, Initialize, request, .context = TraceContext, .in_send_pid = true);
    if (R_FAILED(rc)) return rc;
    host->state = GameShareHostInitialized;

    host->operation = "get connection event";
    Handle event = INVALID_HANDLE;
    rc = serviceDispatch(shim, GetConnectionEvent, .context = TraceContext,
                         .out_handle_attrs = {SfOutHandleAttr_HipcCopy}, .out_handles = &event);
    if (R_FAILED(rc)) return rc;
    if (!event) return invalidReply();
    eventLoadRemote(&host->connection_event, event, false);

    struct { u8 auto_tune; u8 reserved[3]; float speaker, tv, external; } mix = {
        .auto_tune = audio->auto_tune, .speaker = audio->speaker_volume,
        .tv = audio->tv_volume, .external = audio->external_volume
    };
    host->operation = "configure stream audio";
    rc = serviceDispatchIn(shim, SetAudio, mix, .context = TraceContext);
    if (R_FAILED(rc)) return rc;
    host->operation = "configure guest pad styles";
    const u32 styles = 0x1f;
    rc = serviceDispatchIn(shim, SetPadStyles, styles, .context = TraceContext);
    if (R_FAILED(rc)) return rc;
    host->operation = "configure guest Joy-Con hold";
    const u32 hold = 1;
    rc = serviceDispatchIn(shim, SetJoyHoldType, hold, .context = TraceContext);
    if (R_FAILED(rc)) return rc;
    host->operation = "configure guest HID features";
    const u32 features = 0x0d;
    rc = serviceDispatchIn(shim, SetHidFeatures, features, .context = TraceContext);
    if (R_SUCCEEDED(rc)) host->state = GameShareHostPrepared;
    return rc;
}

Result gameshareLocalHostEnableSensors(GameShareLocalHost *host, s32 width, s32 height) {
    if (!host || host->state != GameShareHostPrepared || width <= 0 || height <= 0) return badInput();
    const u32 features = 0x0f;
    const struct { s32 width, height; } range = {width, height};
    host->operation = "enable guest sensors";
    Result rc = serviceDispatchIn(&host->client.shim, SetHidFeatures, features, .context = TraceContext);
    if (R_FAILED(rc)) return rc;
    host->operation = "set guest touch range";
    return serviceDispatchIn(&host->client.shim, SetTouchRange, range, .context = TraceContext);
}

Result gameshareLocalHostMatchmake(GameShareLocalHost *host, AccountUid uid) {
    if (!host || !accountUidIsValid(&uid)) return badInput();
    if (host->state != GameShareHostPrepared) return badInput();
    Result rc = verifyLocalMode(host);
    if (R_FAILED(rc)) return rc;
    Service *shim = &host->client.shim;
    host->operation = "register local work buffers";
    host->state = GameShareHostMatchmaking;
    rc = gameshareRegisterLocalBuffers(shim, host->work, host->work_size, host->mode, host->max_guests);
    if (R_FAILED(rc)) return rc;
    u8 arg[128];
    host->operation = "build local matchmaking argument";
    rc = gameshareMakeLocalAppletArg(shim, uid, host->max_guests, arg);
    if (R_FAILED(rc)) return rc;
    host->operation = "show local GameShare applet";
    rc = gameshareLocalShowMatchmake(arg, &host->session_id, &host->joined);
    if (rc == MAKERESULT(287, 30)) host->state = GameShareHostCancelled;
    if (R_FAILED(rc)) return rc;
    host->state = GameShareHostSessionOpen;
    if (host->joined < 1 || host->joined > host->max_guests) return invalidReply();
    return 0;
}

Result gameshareLocalHostStart(GameShareLocalHost *host) {
    if (!host) return badInput();
    if (host->state == GameShareHostStreaming) return 0;
    if (host->state != GameShareHostSessionOpen || host->joined < 1 || host->joined > host->max_guests)
        return badInput();
    Result rc = verifyLocalMode(host);
    if (R_FAILED(rc)) return rc;
    Service *shim = &host->client.shim;
    s32 capacity = 0;
    host->operation = "get streaming guest capacity";
    rc = serviceDispatchOut(shim, GetGuestCapacity, capacity, .context = TraceContext);
    if (R_FAILED(rc)) return rc;
    if (capacity < host->joined) return invalidReply();

    host->state = GameShareHostStarting;
    host->operation = "start guest stream";
    for (s32 guest = 0; guest < host->max_guests && guest < capacity; ++guest) {
        rc = serviceDispatchIn(shim, StartGuest, guest, .context = TraceContext);
        if (R_FAILED(rc)) return rc;
    }
    host->operation = "start local streaming";
    rc = serviceDispatch(shim, StartStreaming, .context = TraceContext);
    if (R_SUCCEEDED(rc)) host->state = GameShareHostStreaming;
    return rc;
}

Result gameshareLocalHostGetConnection(GameShareLocalHost *host, GameShareLocalConnection *out) {
    if (!host || !out || host->state == GameShareHostIdle) return badInput();
    host->operation = "read connection state";
    return serviceDispatchOut(&host->client.shim, GetConnectionState, *out, .context = TraceContext);
}

Result gameshareLocalHostClose(GameShareLocalHost *host) {
    if (!host) return badInput();
    Result rc = 0;
    Service *shim = &host->client.shim;
    if (host->state >= GameShareHostSessionOpen)
        rememberError(&rc, serviceDispatch(shim, CloseSession, .context = TraceContext));
    if (host->state == GameShareHostCancelled) {
        rc = serviceDispatch(shim, EndMatchmake, .context = TraceContext);
        if (R_SUCCEEDED(rc)) rc = serviceDispatch(shim, ReleaseGuestBuffers, .context = TraceContext);
    }
    if (host->state != GameShareHostIdle)
        rememberError(&rc, serviceDispatch(shim, Finalize, .context = TraceContext));
    if (eventActive(&host->connection_event)) eventClose(&host->connection_event);
    rememberError(&rc, gameshareLocalClose(&host->client));
    // Closing the scoped interface also releases its service-side resources.
    // Keep the allocation for a subsequent close if a remote handle still owns pages.
    if (host->work) {
        Result released = gameshareWaitLocalBuffers(host->work, host->work_size);
        rememberError(&rc, released);
        if (R_SUCCEEDED(released)) {
            free(host->work);
            host->work = NULL;
            host->work_size = 0;
        }
    }
    host->state = GameShareHostIdle;
    host->joined = 0;
    host->session_id = 0;
    host->max_guests = 0;
    host->mode = 0;
    return rc;
}
