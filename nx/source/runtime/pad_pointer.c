/* SPDX-License-Identifier: ISC
 * Original libounce-nx Switch 2 pointer wrapper for pad.h.
 *
 * A PadPointer reads the Joy-Con pointers (Nmouse, switch2/services/nmouse.h)
 * of the controllers of a PadState, as padUpdate reads their buttons. It
 * follows one Joy-Con: the one that last moved, scrolled or changed a
 * button. The Nmouse memory belongs to the process, so it is registered by
 * the first PadPointer and released by the last.
 */
#if defined(__SWITCH2__)
#include <string.h>
#include "kernel/mutex.h"
#include "runtime/pad.h"

// Samples read per Joy-Con and update: the service keeps 16.
#define PAD_POINTER_HISTORY 16

// Npads that a PadState can read: No1 to No8, then Handheld.
#define PAD_POINTER_NPAD_COUNT 9

// How much leeway to give Gyroscope for the pad
#define PAD_MOUSE_GRIP_MIN_ACCEL 0.85f
#define PAD_MOUSE_GRIP_MAX_ACCEL 1.15f

static Mutex g_padPointerMutex;
static u32 g_padPointerUsers;

Result padPointerInitialize(PadPointer* pointer, s32 width, s32 height) {
    memset(pointer, 0, sizeof(*pointer));

    mutexLock(&g_padPointerMutex);
    Result rc = 0;
    if (g_padPointerUsers == 0)
        rc = hidInitializeNmouse(NULL, 0, width, height, HidNmousePointerType_Default);
    if (R_SUCCEEDED(rc))
        g_padPointerUsers++;
    mutexUnlock(&g_padPointerMutex);
    return rc;
}

void padPointerExit(PadPointer* pointer) {
    mutexLock(&g_padPointerMutex);
    if (g_padPointerUsers > 0 && --g_padPointerUsers == 0)
        hidFinalizeNmouse();
    mutexUnlock(&g_padPointerMutex);
    memset(pointer, 0, sizeof(*pointer));
}

/* This is done to ensure the Joy-Con 2 stay upright. Then only can mouse mode be reasonably used */
HidSixAxisSensorState readSixAxis(HidSixAxisSensorHandle targetHandle) {
    HidSixAxisSensorState state = { 0 };
    /* Get the six axis states*/
    hidGetSixAxisSensorStates(targetHandle, &state, 1);

    /* If the state is invalid, get the states a different method */
    if (state.acceleration.x == 0.f && state.acceleration.y == 0.f && state.acceleration.z == -1.f) {
        u64 aruid = appletGetAppletResourceUserId();
        serviceDispatchIn(hidsysGetServiceSession(), 500, aruid);

        hidGetSixAxisSensorStates(targetHandle, &state, 1);
    }

    return state;
}

bool padPointerIsBeingUsed(HidSixAxisSensorHandle masterHandle[]) {
    /* Get the sensor states */
    HidSixAxisSensorState left  = readSixAxis(masterHandle[2]);
    HidSixAxisSensorState right = readSixAxis(masterHandle[3]);

    /* Get the accelerations */
    float lx = left.acceleration.x;
    float rx = right.acceleration.x;

    /* Verify the accelerations*/
    bool leftGrip  = lx >= PAD_MOUSE_GRIP_MIN_ACCEL && lx <= PAD_MOUSE_GRIP_MAX_ACCEL;
    bool rightGrip = rx >= -PAD_MOUSE_GRIP_MAX_ACCEL && rx <= -PAD_MOUSE_GRIP_MIN_ACCEL;

    /* Return the verified accelerations*/
    return leftGrip || rightGrip;
}

// The Joy-Con pointers on the pad's Npads, at most PAD_POINTER_MAX_DEVICES.
static u32 _padPointerListHandles(const PadState* pad, HidNmouseHandle* handles) {
    u32 count = 0;
    for (u32 i = 0; i < PAD_POINTER_NPAD_COUNT && count < PAD_POINTER_MAX_DEVICES; i++) {
        const HidNpadIdType id = i < 8 ? (HidNpadIdType)(HidNpadIdType_No1 + i) : HidNpadIdType_Handheld;
        const bool selected = i < 8 ? (pad->id_mask & BIT(i)) != 0 : pad->read_handheld;
        if (!selected)
            continue;

        s32 found = 0;
        if (R_SUCCEEDED(hidGetNmouseHandles(&handles[count], PAD_POINTER_MAX_DEVICES - count, id, &found)))
            count += found;
    }
    return count;
}

// The tracked entry of a handle, or NULL.
static const PadPointerDevice* _padPointerFindDevice(const PadPointer* pointer, HidNmouseHandle handle) {
    for (u32 i = 0; i < pointer->device_count; i++) {
        if (pointer->devices[i].handle.handle == handle.handle)
            return &pointer->devices[i];
    }
    return NULL;
}

// What one Joy-Con did since the previous update.
typedef struct {
    bool has_sample;            // The Joy-Con has a sample to show.
    bool activity;              // It moved, scrolled or changed a button.
    s32 x;
    s32 y;
    s32 delta_x;
    s32 delta_y;
    s32 wheel_delta_y;
} PadPointerReading;

// Reads a Joy-Con's samples newer than the device's newest, oldest first. A
// newly seen Joy-Con gives its position without movement.
static void _padPointerReadDevice(PadPointerDevice* device, bool known, PadPointerReading* reading) {
    memset(reading, 0, sizeof(*reading));

    HidNmouseStateWithDelta states[PAD_POINTER_HISTORY];
    size_t total = 0;
    if (R_FAILED(hidGetNmouseStatesWithDelta(device->handle, states, PAD_POINTER_HISTORY, &total)) || total == 0)
        return;

    reading->has_sample = true;
    reading->x = states[0].x;
    reading->y = states[0].y;

    if (known) {
        for (size_t i = total; i-- > 0;) {
            const HidNmouseStateWithDelta* state = &states[i];
            if (state->sampling_number <= device->sampling_number)
                continue;
            reading->delta_x += state->delta_x;
            reading->delta_y += state->delta_y;
            reading->wheel_delta_y += state->wheel_delta_y;
            if (state->delta_x || state->delta_y || state->wheel_delta_y || state->buttons != device->buttons)
                reading->activity = true;
            device->buttons = state->buttons;
        }
    } else {
        device->buttons = states[0].buttons;
    }
    device->sampling_number = states[0].sampling_number;
}

static PadPointerSide _padPointerSide(HidNmouseHandle handle) {
    u32 type_set = 0;
    if (R_FAILED(hidGetNmouseTypeSet(handle, &type_set)))
        return PadPointerSide_None;
    if (type_set & HidNmouseType_JoyLeft)
        return PadPointerSide_Left;
    if (type_set & HidNmouseType_JoyRight)
        return PadPointerSide_Right;
    return PadPointerSide_None;
}

void padPointerUpdate(PadPointer* pointer, const PadState* pad) {
    pointer->buttons_old = pointer->buttons_cur;
    pointer->delta_x = 0;
    pointer->delta_y = 0;
    pointer->wheel_delta_y = 0;

    HidNmouseHandle handles[PAD_POINTER_MAX_DEVICES];
    const u32 count = _padPointerListHandles(pad, handles);

    // Read each Joy-Con, keeping the state of the ones already tracked.
    PadPointerDevice devices[PAD_POINTER_MAX_DEVICES];
    PadPointerReading readings[PAD_POINTER_MAX_DEVICES];
    s32 current = -1;
    s32 last_active = -1;
    for (u32 i = 0; i < count; i++) {
        const PadPointerDevice* tracked = _padPointerFindDevice(pointer, handles[i]);
        devices[i] = tracked ? *tracked : (PadPointerDevice){ .handle = handles[i] };
        _padPointerReadDevice(&devices[i], tracked != NULL, &readings[i]);
        if (!readings[i].has_sample)
            continue;
        if (readings[i].activity)
            last_active = i;
        if (handles[i].handle == pointer->current.handle && pointer->active)
            current = i;
    }
    memcpy(pointer->devices, devices, count * sizeof(devices[0]));
    pointer->device_count = count;

    // Follow the Joy-Con that last did something; else keep the current
    // one; else take the first with a sample.
    if (last_active >= 0)
        current = last_active;
    for (u32 i = 0; current < 0 && i < count; i++) {
        if (readings[i].has_sample)
            current = i;
    }

    if (current < 0) {
        pointer->active = false;
        pointer->side = PadPointerSide_None;
        pointer->buttons_cur = 0;
        return;
    }

    const PadPointerReading* reading = &readings[current];
    pointer->active = true;
    pointer->current = handles[current];
    pointer->side = _padPointerSide(handles[current]);
    pointer->x = reading->x;
    pointer->y = reading->y;
    pointer->delta_x = reading->delta_x;
    pointer->delta_y = reading->delta_y;
    pointer->wheel_delta_y = reading->wheel_delta_y;
    pointer->buttons_cur = devices[current].buttons;
}
#endif
