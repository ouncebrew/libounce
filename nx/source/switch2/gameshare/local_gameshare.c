// SPDX-License-Identifier: MIT
#include <switch.h>
#include <stddef.h>
#include <string.h>

enum {
    CreateShim = 0,
    GetMemorySize = 1,
    GetHidSharedMemory = 300,
    ShimVersion = 3,
    TlsCommandBufferSize = 0x100,
};

typedef struct {
    u8 type;
    u8 mode;
    u16 reserved;
    s32 guest_count;
} MemoryRequest;

typedef struct {
    u64 size;
    u64 offset;
} HidConfig;

_Static_assert(sizeof(MemoryRequest) == 8 && offsetof(MemoryRequest, guest_count) == 4,
               "Unexpected memory query layout");
_Static_assert(sizeof(HidConfig) == 16, "Unexpected HID configuration layout");

static Result invalidReply(void) {
    return MAKERESULT(Module_Libnx, LibnxError_InvalidCmifOutHeader);
}

// All three SDK calls are non-domain, descriptor-free input with CMIF v1,
// including when the current thread has the default inline context of zero.
static Result dispatch(Service *service, u32 command, const void *in, u32 in_size,
                       void *out, u32 out_size, Handle *out_copy, Handle *out_move) {
    void *tls = armGetTls();
    HipcRequest request = hipcMakeRequestInline(tls,
        .type = CmifCommandType_RequestWithContext,
        .num_data_words = (16 + sizeof(CmifInHeader) + in_size + 3) / 4);
    CmifInHeader *header = cmifGetAlignedDataStart(request.data_words, tls);
    *header = (CmifInHeader){.magic = CMIF_IN_HEADER_MAGIC, .version = 1, .command_id = command};
    if (in_size) memcpy(header + 1, in, in_size);
    Result rc = svcSendSyncRequest(service->session);
    if (R_FAILED(rc)) return rc;

    HipcResponse reply = hipcParseResponse(tls);
    CmifOutHeader *cmif = cmifGetAlignedDataStart(reply.data_words, tls);
    size_t data_offset = (uintptr_t)reply.data_words - (uintptr_t)tls;
    size_t data_bytes = reply.num_data_words * sizeof(u32);
    size_t padding = (uintptr_t)cmif - (uintptr_t)reply.data_words;
    if (data_offset <= TlsCommandBufferSize && data_bytes <= TlsCommandBufferSize - data_offset &&
        data_bytes >= padding + sizeof(*cmif) && cmif->magic == CMIF_OUT_HEADER_MAGIC) {
        rc = cmif->result;
        if (R_FAILED(rc)) goto discard_handles;
        if (!reply.num_statics && data_bytes >= padding + sizeof(*cmif) + out_size &&
            reply.num_copy_handles == (u32)(out_copy != NULL) &&
            reply.num_move_handles == (u32)(out_move != NULL) &&
            (!out_copy || reply.copy_handles[0] != INVALID_HANDLE) &&
            (!out_move || reply.move_handles[0] != INVALID_HANDLE)) {
            if (out_size) memcpy(out, cmif + 1, out_size);
            if (out_copy) *out_copy = reply.copy_handles[0];
            if (out_move) *out_move = reply.move_handles[0];
            return 0;
        }
    }
    rc = invalidReply();
discard_handles:
    for (u32 i = 0; i < reply.num_copy_handles; ++i) svcCloseHandle(reply.copy_handles[i]);
    for (u32 i = 0; i < reply.num_move_handles; ++i) svcCloseHandle(reply.move_handles[i]);
    return rc;
}

Result gameshareLocalOpen(GameShareLocalClient *client, GameShareLocalStage *stage) {
    GameShareLocalStage unused;
    if (!stage) stage = &unused;
    *stage = GameShareLocalStage_Lookup;
    if (!client) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    if (serviceIsActive(&client->shim) || client->hid_memory.handle)
        return MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized);

    Service root = {0};
    Result rc = smGetService(&root, "stpl:u");
    if (R_FAILED(rc)) return rc;

    *stage = GameShareLocalStage_Acquire;
    const u32 version = ShimVersion;
    Handle child = INVALID_HANDLE;
    rc = dispatch(&root, CreateShim, &version, sizeof(version), NULL, 0, NULL, &child);
    if (R_SUCCEEDED(rc)) serviceCreateNonDomainSubservice(&client->shim, &root, child);
    serviceClose(&root);
    if (R_FAILED(rc)) return rc;

    *stage = GameShareLocalStage_HidConfig;
    HidConfig config;
    Handle memory = INVALID_HANDLE;
    rc = dispatch(&client->shim, GetHidSharedMemory, NULL, 0, &config, sizeof(config), &memory, NULL);
    if (R_SUCCEEDED(rc)) {
        if (!config.size || (config.size & 0xfff) || config.offset >= config.size) {
            svcCloseHandle(memory);
            rc = invalidReply();
        } else {
            *stage = GameShareLocalStage_HidMap;
            shmemLoadRemote(&client->hid_memory, memory, config.size, Perm_R);
            rc = shmemMap(&client->hid_memory);
        }
    }
    if (R_FAILED(rc)) {
        gameshareLocalClose(client);
        return rc;
    }
    client->hid_offset = config.offset;
    *stage = GameShareLocalStage_Ready;
    return 0;
}

Result gameshareLocalGetRequiredMemorySize(GameShareLocalClient *client, s32 guest_count, u32 *out_size) {
    if (!client || !out_size || guest_count < 1 || guest_count > GAMESHARE_LOCAL_MAX_GUESTS)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    if (!serviceIsActive(&client->shim))
        return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    const MemoryRequest request = {.type = 0, .mode = 0, .guest_count = guest_count};
    return dispatch(&client->shim, GetMemorySize, &request, sizeof(request), out_size, sizeof(*out_size), NULL, NULL);
}

Result gameshareLocalClose(GameShareLocalClient *client) {
    if (!client) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    Result rc = shmemClose(&client->hid_memory);
    serviceClose(&client->shim);
    if (R_SUCCEEDED(rc)) *client = (GameShareLocalClient){0};
    return rc;
}
