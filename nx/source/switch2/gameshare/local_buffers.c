// SPDX-License-Identifier: MIT
#include "local_buffers.h"
#include <stddef.h>
#include <string.h>

typedef struct {
    u64 common;
    u64 first;
    u64 third;
    u64 second;
} BufferSizes;

typedef struct {
    u32 index;
    u32 reserved;
    u64 sizes[3];
} GuestBuffers;

typedef struct {
    u8 option;
    u8 guests;
    u8 flags[2];
    u8 reserved[8];
    u16 styles;
    u16 reserved2;
    AccountUid uid;
} MatchmakeConfig;

_Static_assert(sizeof(BufferSizes) == 32, "Unexpected size reply layout");
_Static_assert(sizeof(GuestBuffers) == 32 && offsetof(GuestBuffers, sizes) == 8, "Unexpected guest buffer layout");
_Static_assert(sizeof(MatchmakeConfig) == 32 && offsetof(MatchmakeConfig, uid) == 16, "Unexpected matchmake layout");

static bool validRange(const void *buffer, size_t size) {
    uintptr_t start = (uintptr_t)buffer;
    return buffer && size && !(start & 0xfff) && !(size & 0xfff) && size <= UINTPTR_MAX - start;
}

// Empty slices are represented by a zero handle and a zero size in the SDK.
static Result createSlice(Handle *handle, u8 **cursor, size_t size) {
    if (!size) return 0;
    Result rc = svcCreateTransferMemory(handle, *cursor, size, Perm_None);
    if (R_SUCCEEDED(rc)) *cursor += size;
    return rc;
}

Result gameshareRegisterLocalBuffers(Service *shim, void *buffer, size_t size, u8 mode, u8 guests) {
    if (!shim || !serviceIsActive(shim) || !validRange(buffer, size) || guests < 1 || guests > GAMESHARE_LOCAL_MAX_GUESTS)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    const u64 type_and_mode = (u64)mode << 8;
    BufferSizes sizes;
    Result rc = serviceDispatchInOut(shim, 9, type_and_mode, sizes, .context = 1);
    if (R_FAILED(rc)) return rc;

    GuestBuffers guest = {.sizes = {sizes.first, sizes.second, sizes.third}};
    if ((sizes.common | sizes.first | sizes.second | sizes.third) & 0xfff)
        return MAKERESULT(Module_Libnx, LibnxError_InvalidCmifOutHeader);
    if (sizes.common > size) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    size_t guest_capacity = (size - sizes.common) / guests;
    for (size_t i = 0; i < 3; ++i) {
        if (guest.sizes[i] > guest_capacity) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        guest_capacity -= guest.sizes[i];
    }

    u8 *cursor = buffer;
    Handle common = INVALID_HANDLE;
    rc = createSlice(&common, &cursor, sizes.common);
    if (R_FAILED(rc)) return rc;
    rc = serviceDispatchIn(shim, 505, sizes.common, .context = 1, .in_num_handles = 1, .in_handles = {common});
    if (common != INVALID_HANDLE) svcCloseHandle(common);
    if (R_FAILED(rc)) return rc;

    for (guest.index = 0; guest.index < guests; ++guest.index) {
        Handle handles[3] = {INVALID_HANDLE, INVALID_HANDLE, INVALID_HANDLE};
        for (size_t i = 0; i < 3; ++i) {
            rc = createSlice(&handles[i], &cursor, guest.sizes[i]);
            if (R_FAILED(rc)) break;
        }
        if (R_SUCCEEDED(rc)) {
            rc = serviceDispatchIn(shim, 506, guest, .context = 1, .in_num_handles = 3,
                                   .in_handles = {handles[0], handles[1], handles[2]});
        }
        for (size_t i = 0; i < 3; ++i)
            if (handles[i] != INVALID_HANDLE) svcCloseHandle(handles[i]);
        if (R_FAILED(rc)) return rc;
    }
    return 0;
}

Result gameshareMakeLocalAppletArg(Service *shim, AccountUid uid, u8 guests, u8 out[128]) {
    if (!shim || !serviceIsActive(shim) || !accountUidIsValid(&uid) || guests < 1 || guests > GAMESHARE_LOCAL_MAX_GUESTS || !out)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    const MatchmakeConfig config = {.option = 1, .guests = guests, .flags = {1, 1}, .styles = 3, .uid = uid};
    struct { u8 bytes[128]; } argument;
    Result rc = serviceDispatchInOut(shim, 504, config, argument, .context = 1);
    if (R_SUCCEEDED(rc)) memcpy(out, argument.bytes, sizeof(argument.bytes));
    return rc;
}

Result gameshareWaitLocalBuffers(void *buffer, size_t size) {
    if (!validRange(buffer, size)) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    const uintptr_t end = (uintptr_t)buffer + size;
    uintptr_t cursor = (uintptr_t)buffer;
    // The service can release its last copy after the close response. Check
    // entire kernel regions, including pages following the common buffer.
    for (int attempt = 0; attempt <= 100; ++attempt) {
        while (cursor < end) {
            MemoryInfo info;
            u32 page_info;
            Result rc = svcQueryMemory(&info, &page_info, cursor);
            if (R_FAILED(rc)) return rc;
            if ((info.attr & MemAttr_IsBorrowed) || info.perm != Perm_Rw) break;
            if (info.addr > cursor || cursor - info.addr >= info.size)
                return MAKERESULT(Module_Libnx, LibnxError_BadQueryMemory);
            const u64 remaining = info.size - (cursor - info.addr);
            cursor += remaining < end - cursor ? remaining : end - cursor;
        }
        if (cursor == end) return 0;
        if (attempt < 100) svcSleepThread(10000000);
    }
    return MAKERESULT(Module_Libnx, LibnxError_Timeout);
}
