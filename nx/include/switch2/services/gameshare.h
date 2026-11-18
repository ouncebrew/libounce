// SPDX-License-Identifier: MIT
/** @file gameshare.h
 * Local wireless GameShare hosting on Switch 2.
 */
#pragma once
#include <switch/services/sm.h>
#include <switch/services/hid.h>
#include <switch/services/acc.h>
#include <switch/kernel/event.h>
#include <switch/kernel/shmem.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GAMESHARE_LOCAL_MAX_GUESTS 3

typedef enum {
    GameShareLocalStage_Lookup,
    GameShareLocalStage_Acquire,
    GameShareLocalStage_HidConfig,
    GameShareLocalStage_HidMap,
    GameShareLocalStage_Ready,
} GameShareLocalStage;

// Zero-initialize before first use. The client owns its session and mapping.
// Do not copy an open client. Serialize open, query and close on each instance.
typedef struct {
    Service shim;
    SharedMemory hid_memory;
    size_t hid_offset;
} GameShareLocalClient;

// On failure, stage identifies the operation that returned the error. It may be NULL.
Result gameshareLocalOpen(GameShareLocalClient *client, GameShareLocalStage *stage);

// Query the work memory needed for one to three guests in local streaming mode.
// out_size is in bytes and is only written on success.
Result gameshareLocalGetRequiredMemorySize(GameShareLocalClient *client, s32 guest_count, u32 *out_size);

// Safe on a zero-initialized or previously closed client. Retry if unmapping fails.
Result gameshareLocalClose(GameShareLocalClient *client);

typedef struct {
    bool auto_tune;
    float speaker_volume;
    float tv_volume;
    float external_volume;
} GameShareLocalAudio;

typedef struct {
    u32 guest_flags; // Bits 0..2 correspond to guest slots.
    Result result;   // A connection error reported asynchronously by StreamPlay.
} GameShareLocalConnection;

typedef enum {
    GameShareHostIdle,
    GameShareHostInitialized,
    GameShareHostPrepared,
    GameShareHostMatchmaking,
    GameShareHostCancelled,
    GameShareHostSessionOpen,
    GameShareHostStarting,
    GameShareHostStreaming
} GameShareLocalHostState;

typedef struct {
    GameShareLocalClient client;
    Event connection_event;
    void *work;
    size_t work_size;
    const char *operation; // Last host operation attempted, including a failed call.
    u32 session_id;
    s32 joined;
    u8 max_guests;
    GameShareLocalHostState state;
} GameShareLocalHost;

// Start with {0}. Open host.client with gameshareLocalOpen, then prepare, matchmake
// with the application's selected user, and start. All calls on a host are serialized
// by the caller. Always close after the last use, including failed start-up attempts.
Result gameshareLocalHostPrepare(GameShareLocalHost *host, u8 max_guests, const GameShareLocalAudio *audio);
Result gameshareLocalHostMatchmake(GameShareLocalHost *host, AccountUid uid);
Result gameshareLocalHostStart(GameShareLocalHost *host);
Result gameshareLocalHostGetConnection(GameShareLocalHost *host, GameShareLocalConnection *out);
// Closes the StreamPlay session and the embedded client. If memory is still
// borrowed or mapped, the host retains it. Calling close again retries release.
Result gameshareLocalHostClose(GameShareLocalHost *host);

typedef struct {
    bool connected;
    u32 style; // Selected HidNpadStyleTag, zero when there is no standard style.
    HidNpadCommonState state;
} GameShareLocalPad;

// Read the latest standard pad sample. guest_index is 0–2. Supported pad IDs
// are No1, No2, and Handheld. A guest without a sample returns connected=false.
// On LibnxError_Timeout the sample was changing; poll again on the next frame.
Result gameshareLocalReadGuestPad(const GameShareLocalClient *client, u8 guest_index,
                                  HidNpadIdType pad_id, GameShareLocalPad *out);

#ifdef __cplusplus
}
#endif
