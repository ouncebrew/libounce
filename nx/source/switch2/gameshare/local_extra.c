// SPDX-License-Identifier: MIT
#include <stdint.h>
#include <string.h>
#include <switch.h>

enum {
    SetPadStyles = 25, GetPadStyles = 26,
    SetJoyHoldType = 27, GetJoyHoldType = 28,
    GetGuestAudioCapacity = 41, GetGuestAudioFree = 42, GetGuestAudioFilled = 43,
    WriteGuestAudio = 40,
    DisconnectGuest = 70,
    SendVibration = 141,
    SetHidFeatures = 160, GetHidFeatures = 161,
    GetNickname = 200, GetProfileImage = 201,
    // A nonzero libounce trace token selects CMIF v1 / RequestWithContext.
    TraceContext = 1,
};

static Result badInput(void) {
    return MAKERESULT(Module_Libnx, LibnxError_BadInput);
}

static Service *live(GameShareLocalHost *host) {
    return host && host->state != GameShareHostIdle ? &host->client.shim : NULL;
}

static Service *streaming(GameShareLocalHost *host) {
    return host && host->state == GameShareHostStreaming ? &host->client.shim : NULL;
}

static bool validGuest(const GameShareLocalHost *host, u8 guest) {
    return guest < host->max_guests;
}

Event *gameshareLocalHostGetConnectionEvent(GameShareLocalHost *host) {
    return host && eventActive(&host->connection_event) ? &host->connection_event : NULL;
}

Result gameshareLocalHostDisconnectGuest(GameShareLocalHost *host, u8 guest) {
    Service *shim = streaming(host);
    if (!shim || !validGuest(host, guest)) return badInput();
    host->operation = "disconnect guest";
    return serviceDispatchIn(shim, DisconnectGuest, guest, .context = TraceContext);
}

Result gameshareLocalHostGetGuestNickname(GameShareLocalHost *host, u8 guest, char out[33]) {
    Service *shim = streaming(host);
    if (!shim || !out || !validGuest(host, guest)) return badInput();
    char name[33] = {0};
    host->operation = "read guest nickname";
    Result rc = serviceDispatchInOut(shim, GetNickname, guest, name, .context = TraceContext);
    if (R_SUCCEEDED(rc)) {
        name[32] = 0;
        memcpy(out, name, sizeof(name));
    }
    return rc;
}

Result gameshareLocalHostGetGuestProfileImage(GameShareLocalHost *host, u8 guest, void *buffer, size_t size,
                                              size_t *out_size) {
    Service *shim = streaming(host);
    if (!shim || !buffer || !size || !out_size || !validGuest(host, guest)) return badInput();
    u64 written = 0;
    host->operation = "read guest profile image";
    Result rc = serviceDispatchInOut(shim, GetProfileImage, guest, written, .context = TraceContext,
        .buffer_attrs = {SfBufferAttr_Out | SfBufferAttr_HipcAutoSelect},
        .buffers = {{buffer, size}});
    if (R_SUCCEEDED(rc)) *out_size = written > size ? size : (size_t)written;
    return rc;
}

Result gameshareLocalHostWriteGuestAudio(GameShareLocalHost *host, u8 guest, const void *frames, s32 frame_count,
                                         s32 *out_written) {
    Service *shim = streaming(host);
    if (!shim || !frames || frame_count <= 0 || !out_written || !validGuest(host, guest)) return badInput();
    struct { u8 guest; u8 reserved[3]; u32 frames; } in = {.guest = guest, .frames = (u32)frame_count};
    u32 written = 0;
    host->operation = "write guest audio";
    Result rc = serviceDispatchInOut(shim, WriteGuestAudio, in, written, .context = TraceContext,
        .buffer_attrs = {SfBufferAttr_In | SfBufferAttr_HipcAutoSelect},
        .buffers = {{frames, (size_t)frame_count * GAMESHARE_LOCAL_AUDIO_FRAME_SIZE}});
    if (R_FAILED(rc)) return rc;
    if (written > (u32)frame_count) return MAKERESULT(Module_Libnx, LibnxError_InvalidCmifOutHeader);
    *out_written = (s32)written;
    return 0;
}

Result gameshareLocalHostGetGuestAudioCapacity(GameShareLocalHost *host, s32 *out_frames) {
    Service *shim = streaming(host);
    if (!shim || !out_frames) return badInput();
    u32 frames = 0;
    host->operation = "read guest audio capacity";
    Result rc = serviceDispatchOut(shim, GetGuestAudioCapacity, frames, .context = TraceContext);
    if (R_FAILED(rc)) return rc;
    if (frames > INT32_MAX) return MAKERESULT(Module_Libnx, LibnxError_InvalidCmifOutHeader);
    *out_frames = (s32)frames;
    return 0;
}

static Result audioCount(GameShareLocalHost *host, u8 guest, u32 command, s32 *out, const char *operation) {
    Service *shim = streaming(host);
    if (!shim || !out || !validGuest(host, guest)) return badInput();
    u32 frames = 0;
    host->operation = operation;
    Result rc = serviceDispatchInOut(shim, command, guest, frames, .context = TraceContext);
    if (R_FAILED(rc)) return rc;
    if (frames > INT32_MAX) return MAKERESULT(Module_Libnx, LibnxError_InvalidCmifOutHeader);
    *out = (s32)frames;
    return 0;
}

Result gameshareLocalHostGetGuestAudioFree(GameShareLocalHost *host, u8 guest, s32 *out_frames) {
    return audioCount(host, guest, GetGuestAudioFree, out_frames, "read guest audio free frames");
}

Result gameshareLocalHostGetGuestAudioFilled(GameShareLocalHost *host, u8 guest, s32 *out_frames) {
    return audioCount(host, guest, GetGuestAudioFilled, out_frames, "read guest audio filled frames");
}

// A guest's vibration handle is the pad id, the guest, the style's tag index
// and the device number packed into 64 bits.
s32 gameshareLocalMakeVibrationHandles(GameShareLocalVibrationHandle *out, s32 max, u8 guest,
                                       HidNpadIdType pad_id, u32 style) {
    if (!out || max < 1 || guest >= GAMESHARE_LOCAL_MAX_GUESTS || !style || (style & (style - 1))) return 0;
    if (pad_id != HidNpadIdType_No1 && pad_id != HidNpadIdType_No2 && pad_id != HidNpadIdType_Handheld) return 0;
    s32 count;
    if (style & (BIT(0) | BIT(1) | BIT(2) | BIT(12))) count = 2;
    else if (style & (BIT(3) | BIT(13) | BIT(4) | BIT(14))) count = 1;
    else return 0;
    if (count > max) count = max;
    // Tags 12 to 14 are the system variants of tags 2 to 4.
    u32 tag = style;
    if (tag & BIT(12)) tag = (tag & ~BIT(12)) | BIT(2);
    if (tag & BIT(13)) tag = (tag & ~BIT(13)) | BIT(3);
    if (tag & BIT(14)) tag = (tag & ~BIT(14)) | BIT(4);
    const u64 base = (u64)(u32)pad_id | (u64)guest << 32 | (u64)__builtin_ctz(tag) << 40;
    for (s32 i = 0; i < count; ++i) out[i] = base | (u64)i << 48;
    return count;
}

Result gameshareLocalHostSendVibration(GameShareLocalHost *host, GameShareLocalVibrationHandle handle,
                                       const HidVibrationValue *value) {
    Service *shim = streaming(host);
    if (!shim || !value) return badInput();
    struct { HidVibrationValue value; u64 handle; } in = {*value, handle};
    host->operation = "send guest vibration";
    return serviceDispatchIn(shim, SendVibration, in, .context = TraceContext);
}

static Result setU32(GameShareLocalHost *host, u32 command, u32 value, const char *operation) {
    Service *shim = live(host);
    if (!shim) return badInput();
    host->operation = operation;
    return serviceDispatchIn(shim, command, value, .context = TraceContext);
}

static Result getU32(GameShareLocalHost *host, u32 command, u32 *out, const char *operation) {
    Service *shim = live(host);
    if (!shim || !out) return badInput();
    host->operation = operation;
    return serviceDispatchOut(shim, command, *out, .context = TraceContext);
}

Result gameshareLocalHostSetPadStyles(GameShareLocalHost *host, u32 styles) {
    return setU32(host, SetPadStyles, styles, "set guest pad styles");
}

Result gameshareLocalHostGetPadStyles(GameShareLocalHost *host, u32 *out) {
    return getU32(host, GetPadStyles, out, "read guest pad styles");
}

Result gameshareLocalHostSetJoyHoldType(GameShareLocalHost *host, u32 hold_type) {
    return setU32(host, SetJoyHoldType, hold_type, "set guest Joy-Con hold type");
}

Result gameshareLocalHostGetJoyHoldType(GameShareLocalHost *host, u32 *out) {
    return getU32(host, GetJoyHoldType, out, "read guest Joy-Con hold type");
}

Result gameshareLocalHostSetHidFeatures(GameShareLocalHost *host, u32 features) {
    return setU32(host, SetHidFeatures, features, "set guest input features");
}

Result gameshareLocalHostGetHidFeatures(GameShareLocalHost *host, u32 *out) {
    return getU32(host, GetHidFeatures, out, "read guest input features");
}
