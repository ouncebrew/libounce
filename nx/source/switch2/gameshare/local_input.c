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

static int padBank(HidNpadIdType id) {
    switch (id) {
        case HidNpadIdType_No1: return 0;
        case HidNpadIdType_No2: return 1;
        case HidNpadIdType_Handheld: return 2;
        default: return -1;
    }
}

Result gameshareLocalReadGuestPad(const GameShareLocalClient *client, u8 guest_index,
                                  HidNpadIdType pad_id, GameShareLocalPad *out) {
    if (!client || !out || guest_index >= GAMESHARE_LOCAL_MAX_GUESTS)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    int pad_index = padBank(pad_id);
    if (pad_index < 0) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
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

_Static_assert(sizeof(HidTouchScreenState) + 16 == 0x2a0, "guest touch entry size");
_Static_assert(sizeof(HidSixAxisSensorState) + 16 == 0x70, "guest motion entry size");

static Result readSensorRing(const GameShareLocalClient *client, size_t offset, void *out, size_t size) {
    if (!client->shim.session || !client->hid_memory.map_addr)
        return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    const size_t stride = 16 + size; // sequence and frame precede the HID state
    const size_t mapped = client->hid_memory.size;
    if ((client->hid_offset & 7) || client->hid_offset > mapped ||
        offset + 16 + RingCapacity * stride > mapped - client->hid_offset)
        return MAKERESULT(Module_Libnx, LibnxError_InvalidCmifOutHeader);
    const u8 *ring = (const u8 *)client->hid_memory.map_addr + client->hid_offset + offset;
    const u64 *header = (const u64 *)ring;
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        if (!__atomic_load_n(header + 1, __ATOMIC_ACQUIRE)) {
            memset(out, 0, size);
            return 0;
        }
        const size_t slot = __atomic_load_n(header, __ATOMIC_ACQUIRE) % RingCapacity;
        const u64 *entry = (const u64 *)(ring + 16 + stride * slot);
        const u64 before = __atomic_load_n(entry, __ATOMIC_ACQUIRE);
        if (before & 1) continue;
        memcpy(out, entry + 2, size);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (before == __atomic_load_n(entry, __ATOMIC_RELAXED)) return 0;
    }
    return MAKERESULT(Module_Libnx, LibnxError_Timeout);
}

Result gameshareLocalReadGuestTouch(const GameShareLocalClient *client, u8 guest_index, HidTouchScreenState *out) {
    if (!client || !out || guest_index >= GAMESHARE_LOCAL_MAX_GUESTS)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    HidTouchScreenState sample;
    Result rc = readSensorRing(client, 0x6c000 + 0x6000 * guest_index, &sample, sizeof(sample));
    if (R_FAILED(rc)) return rc;
    if (sample.count < 0 || sample.count > 16) return MAKERESULT(Module_Libnx, LibnxError_InvalidCmifOutHeader);
    *out = sample;
    return 0;
}

Result gameshareLocalReadGuestSixAxis(const GameShareLocalClient *client, u8 guest_index, HidNpadIdType pad_id,
                                      GameShareLocalSensor sensor, HidSixAxisSensorState *out) {
    int bank = padBank(pad_id);
    if (!client || !out || guest_index >= GAMESHARE_LOCAL_MAX_GUESTS || bank < 0 ||
        (unsigned)sensor > GameShareLocalSensor_JoyRight)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    size_t offset = bank * PadStride + guest_index * GuestStride + 0x2490 + sensor * 0xe80;
    HidSixAxisSensorState sample;
    Result rc = readSensorRing(client, offset, &sample, sizeof(sample));
    if (R_SUCCEEDED(rc)) *out = sample;
    return rc;
}
