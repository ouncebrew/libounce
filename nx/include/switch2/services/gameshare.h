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
/// Same, for a given StreamingMode. 0 mirrors the host screen to the guests, 1 gives each guest its own display.
Result gameshareLocalGetRequiredMemorySizeEx(GameShareLocalClient *client, s32 guest_count, u8 mode, u32 *out_size);

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
    u8 mode; // StreamingMode passed to prepare.
    GameShareLocalHostState state;
} GameShareLocalHost;

// Start with {0}. Open host.client with gameshareLocalOpen, then prepare, matchmake
// with the application's selected user, and start. All calls on a host are serialized
// by the caller. Always close after the last use, including failed start-up attempts.
Result gameshareLocalHostPrepare(GameShareLocalHost *host, u8 max_guests, const GameShareLocalAudio *audio);
/// Prepare with an explicit StreamingMode: 0 mirrors the host screen, 1 gives each guest its own display.
Result gameshareLocalHostPrepareEx(GameShareLocalHost *host, u8 max_guests, const GameShareLocalAudio *audio, u8 mode);
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

typedef enum {
    GameShareLocalSensor_FullKey,
    GameShareLocalSensor_Handheld,
    GameShareLocalSensor_JoyDualLeft,
    GameShareLocalSensor_JoyDualRight,
    GameShareLocalSensor_JoyLeft,
    GameShareLocalSensor_JoyRight,
} GameShareLocalSensor;

// Call after prepare and before matchmaking. Touch positions use the supplied dimensions.
Result gameshareLocalHostEnableSensors(GameShareLocalHost *host, s32 width, s32 height);

// Reads the latest sample for guest_index (0–2). An empty ring returns a zeroed
// state. If the writer is busy, poll again on LibnxError_Timeout.
Result gameshareLocalReadGuestTouch(const GameShareLocalClient *client, u8 guest_index, HidTouchScreenState *out);
Result gameshareLocalReadGuestSixAxis(const GameShareLocalClient *client, u8 guest_index, HidNpadIdType pad_id,
                                      GameShareLocalSensor sensor, HidSixAxisSensorState *out);

// ---- Guest session ----

/// Sends the connection-change event to wait on. NULL until the host is prepared.
Event *gameshareLocalHostGetConnectionEvent(GameShareLocalHost *host);

/// Disconnects a guest. The host must be streaming.
Result gameshareLocalHostDisconnectGuest(GameShareLocalHost *host, u8 guest);

/// out holds the nickname as a NUL-terminated UTF-8 string.
Result gameshareLocalHostGetGuestNickname(GameShareLocalHost *host, u8 guest, char out[33]);
Result gameshareLocalHostGetGuestProfileImage(GameShareLocalHost *host, u8 guest, void *buffer, size_t size,
                                              size_t *out_size);

// ---- Guest input settings ----
// Call these after prepare and before matchmaking.

Result gameshareLocalHostSetPadStyles(GameShareLocalHost *host, u32 styles);
Result gameshareLocalHostGetPadStyles(GameShareLocalHost *host, u32 *out);
Result gameshareLocalHostSetJoyHoldType(GameShareLocalHost *host, u32 hold_type);
Result gameshareLocalHostGetJoyHoldType(GameShareLocalHost *host, u32 *out);
Result gameshareLocalHostSetHidFeatures(GameShareLocalHost *host, u32 features);
Result gameshareLocalHostGetHidFeatures(GameShareLocalHost *host, u32 *out);

// ---- Guest audio ----
// Frames are interleaved signed 16-bit stereo. In local mirror mode, the
// guest feed can mix with the host audio.

#define GAMESHARE_LOCAL_AUDIO_FRAME_SIZE 4

Result gameshareLocalHostWriteGuestAudio(GameShareLocalHost *host, u8 guest, const void *frames, s32 frame_count,
                                         s32 *out_written);
Result gameshareLocalHostGetGuestAudioCapacity(GameShareLocalHost *host, s32 *out_frames);
Result gameshareLocalHostGetGuestAudioFree(GameShareLocalHost *host, u8 guest, s32 *out_frames);
Result gameshareLocalHostGetGuestAudioFilled(GameShareLocalHost *host, u8 guest, s32 *out_frames);

// ---- Guest rumble ----

typedef u64 GameShareLocalVibrationHandle;

/// Fills out with the vibration devices of one guest pad and returns how many there are.
/// style is a single HidNpadStyleTag bit.
s32 gameshareLocalMakeVibrationHandles(GameShareLocalVibrationHandle *out, s32 max, u8 guest,
                                       HidNpadIdType pad_id, u32 style);
Result gameshareLocalHostSendVibration(GameShareLocalHost *host, GameShareLocalVibrationHandle handle,
                                       const HidVibrationValue *value);

// ---- Per-guest displays ----
// Prepare the host in separate-views mode (gameshareLocalHostPrepareEx, mode 1), open a display
// for each guest before matchmaking, and present to it while streaming.

/// A display whose image goes to one guest instead of the host's screen.
typedef struct GameShareLocalGuestDisplay GameShareLocalGuestDisplay;

/// Names the step the last open reached. Serialize display opens to use this diagnostic.
const char *gameshareLocalGuestDisplayLastStep(void);

Result gameshareLocalGuestDisplayOpen(GameShareLocalGuestDisplay **out, u8 guest, u32 width, u32 height);
/// Presents a linear RGBA8 image of the size given at open.
Result gameshareLocalGuestDisplayPresent(GameShareLocalGuestDisplay *display, const u32 *rgba, u32 stride_pixels);
/// Releases the display on success. On failure the handle is retained. Stop
/// presenting and retry close later. If retirement cannot be confirmed, the
/// library retains the resources until the process exits.
Result gameshareLocalGuestDisplayClose(GameShareLocalGuestDisplay *display);

#ifdef __cplusplus
}
#endif
