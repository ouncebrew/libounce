// SPDX-License-Identifier: MIT
#include <switch.h>
#include <stddef.h>
#include <string.h>

enum {
    GuestStride = 0xc000,
    PadStride = 3 * GuestStride,
    FirstPadRing = 0x28,
    RingCapacity = 33,
};

typedef struct {
    u64 sequence;
    u64 frame;
    HidNpadCommonState state;
} PadEntry;

typedef struct {
    u64 latest;
    u64 count;
    PadEntry entries[RingCapacity];
} PadRing;

_Static_assert(offsetof(PadRing, entries) == 0x10 && sizeof(PadEntry) == 0x38 &&
               offsetof(PadEntry, state) == 0x10 && sizeof(PadRing) == 0x748,
               "Unexpected guest pad ring layout");

Result gameshareLocalReadGuestPad(const GameShareLocalClient *client, u8 guest_index,
                                  HidNpadIdType pad_id, GameShareLocalPad *out) {
    if (!client || !out || guest_index >= GAMESHARE_LOCAL_MAX_GUESTS)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    size_t pad_index;
    switch (pad_id) {
        case HidNpadIdType_No1:      pad_index = 0; break;
        case HidNpadIdType_No2:      pad_index = 1; break;
        case HidNpadIdType_Handheld: pad_index = 2; break;
        default: return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }
    if (!client->shim.session || !client->hid_memory.map_addr)
        return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);

    size_t offset = pad_index * PadStride + guest_index * GuestStride;
    if ((client->hid_offset & 7) || client->hid_offset > client->hid_memory.size ||
        offset + FirstPadRing + 5 * sizeof(PadRing) > client->hid_memory.size - client->hid_offset)
        return MAKERESULT(Module_Libnx, LibnxError_InvalidCmifOutHeader);
    const u8 *pad = (const u8 *)client->hid_memory.map_addr + client->hid_offset + offset;
    const u32 *styles = (const u32 *)pad;

    // Each ring writer sets its entry sequence odd during a write and even
    // after publishing. Bound retries so an unavailable sample cannot stall a frame.
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        u32 style_set = __atomic_load_n(styles, __ATOMIC_ACQUIRE) & HidNpadStyleSet_NpadStandard;
        if (!style_set) {
            *out = (GameShareLocalPad){0};
            return 0;
        }
        unsigned style_index = (unsigned)__builtin_ctz(style_set);
        const PadRing *ring = (const PadRing *)(pad + FirstPadRing) + style_index;
        u64 count = __atomic_load_n(&ring->count, __ATOMIC_ACQUIRE);
        if (!count) {
            *out = (GameShareLocalPad){.style = 1u << style_index};
            return 0;
        }
        u64 latest = __atomic_load_n(&ring->latest, __ATOMIC_ACQUIRE);
        const PadEntry *entry = &ring->entries[latest % RingCapacity];
        u64 before = __atomic_load_n(&entry->sequence, __ATOMIC_ACQUIRE);
        if (before & 1) continue;
        HidNpadCommonState state;
        memcpy(&state, &entry->state, sizeof(state));
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (before != __atomic_load_n(&entry->sequence, __ATOMIC_RELAXED) ||
            style_set != (__atomic_load_n(styles, __ATOMIC_ACQUIRE) & HidNpadStyleSet_NpadStandard))
            continue;
        *out = (GameShareLocalPad){
            .connected = (state.attributes & HidNpadAttribute_IsConnected) != 0,
            .style = 1u << style_index,
            .state = state,
        };
        return 0;
    }
    return MAKERESULT(Module_Libnx, LibnxError_Timeout);
}
