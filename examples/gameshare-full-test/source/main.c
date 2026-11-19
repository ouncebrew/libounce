// SPDX-License-Identifier: MIT
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <malloc.h>
#include <switch.h>

enum { Width = 1280, Height = 720 };
enum { SampleRate = 48000, ChunkFrames = 2048, HostBuffers = 4 };
// Guest feed pacing: frames kept queued ahead of playback, and the most credit it may bank.
enum { GuestPrefill = 4096, GuestMaxCredit = 8192 };

static const HidNpadIdType pad_ids[] = {HidNpadIdType_No1, HidNpadIdType_No2, HidNpadIdType_Handheld};

static const char log_path[] = "sdmc:/gameshare-full-test.txt";
static const char tv_path[] = "sdmc:/gameshare/tv.wav";
static const char *const guest_paths[GAMESHARE_LOCAL_MAX_GUESTS] = {
    "sdmc:/gameshare/gamepad.wav", "sdmc:/gameshare/guest2.wav", "sdmc:/gameshare/guest3.wav",
};

typedef struct {
    FILE *file;
    bool failed;
} Report;

static void record(Report *log, const char *format, ...) {
    va_list args;
    va_start(args, format);
    if (vfprintf(log->file, format, args) < 0) log->failed = true;
    va_end(args);
    if (fputc('\n', log->file) == EOF || fflush(log->file) || fsync(fileno(log->file))) log->failed = true;
}

static void showStatus(const char *message) {
    ErrorApplicationConfig dialog;
    if (R_SUCCEEDED(errorApplicationCreate(&dialog, message, message))) errorApplicationShow(&dialog);
}

static void fill(u32 *pixels, u32 stride, int x, int y, int w, int h, u32 color) {
    for (int row = 0; row < h; ++row) {
        u32 *line = pixels + (size_t)(y + row) * stride + x;
        for (int col = 0; col < w; ++col) line[col] = color;
    }
}

static void text(u32 *pixels, u32 stride, int limit, int x, int y, const char *s, int scale, u32 color) {
    const ConsoleFont *font = &consoleGetDefault()->font;
    const u16 *glyphs = font->gfx;
    for (; *s && x + 16 * scale <= limit; ++s, x += 16 * scale) {
        unsigned char c = (unsigned char)*s;
        if (c < 32 || c > 126) c = '?';
        const u16 *glyph = glyphs + (c - font->asciiOffset) * 16;
        for (int row = 0; row < 16; ++row)
            for (int col = 0; col < 16; ++col)
                if (glyph[row] & (0x8000u >> col)) fill(pixels, stride, x + col * scale, y + row * scale, scale, scale, color);
    }
}

// ---- Audio tracks ----

typedef struct {
    FILE *file;
    long data_start;
    u32 data_len, played;
    float phase, hz;
    bool tone;
} Track;

static u32 rd32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
static u16 rd16(const u8 *p) { return p[0] | p[1] << 8; }

static void trackOpen(Track *t, Report *log, const char *name, const char *path, float tone_hz) {
    memset(t, 0, sizeof(*t));
    t->hz = tone_hz;
    t->file = fopen(path, "rb");
    if (!t->file) {
        record(log, "%s: %s is missing. Playing a %.0f Hz tone", name, path, tone_hz);
        t->tone = true;
        return;
    }
    u8 head[12];
    bool ok = fread(head, 1, 12, t->file) == 12 && !memcmp(head, "RIFF", 4) && !memcmp(head + 8, "WAVE", 4);
    bool format_ok = false;
    for (; ok;) {
        u8 chunk[8];
        if (fread(chunk, 1, 8, t->file) != 8) { ok = false; break; }
        const u32 size = rd32(chunk + 4);
        if (!memcmp(chunk, "fmt ", 4) && size >= 16) {
            u8 fmt[16];
            if (fread(fmt, 1, 16, t->file) != 16) { ok = false; break; }
            format_ok = rd16(fmt) == 1 && rd16(fmt + 2) == 2 && rd32(fmt + 4) == SampleRate && rd16(fmt + 14) == 16;
            if (fseek(t->file, (long)(size - 16 + (size & 1)), SEEK_CUR)) ok = false;
        } else if (!memcmp(chunk, "data", 4)) {
            t->data_start = ftell(t->file);
            t->data_len = size & ~3u;
            break;
        } else if (fseek(t->file, (long)size + (size & 1), SEEK_CUR)) {
            ok = false;
        }
    }
    if (!ok || !format_ok || t->data_start < 0 || !t->data_len) {
        record(log, "%s: %s must be 48 kHz stereo 16-bit PCM. Playing a %.0f Hz tone instead", name, path, tone_hz);
        fclose(t->file);
        t->file = NULL;
        t->tone = true;
        return;
    }
    record(log, "%s: playing %s (%u frames)", name, path, (unsigned)(t->data_len / 4));
}

static void trackRead(Track *t, s16 *out, int frames) {
    if (t->tone) {
        for (int i = 0; i < frames; ++i) {
            const s16 v = (s16)(sinf(t->phase) * 8000.0f);
            out[2 * i] = out[2 * i + 1] = v;
            t->phase += 6.2831853f * t->hz / SampleRate;
            if (t->phase > 6.2831853f) t->phase -= 6.2831853f;
        }
        return;
    }
    int done = 0;
    while (done < frames) {
        u32 left = (t->data_len - t->played) / 4;
        if (!left) {
            t->played = 0;
            if (fseek(t->file, t->data_start, SEEK_SET)) break;
            continue;
        }
        const int want = left < (u32)(frames - done) ? (int)left : frames - done;
        const size_t got = fread(out + 2 * done, 4, want, t->file);
        if (!got) break;
        done += (int)got;
        t->played += (u32)got * 4;
    }
    if (done < frames) memset(out + 2 * done, 0, (size_t)(frames - done) * 4);
}

// Frame position of the next read inside the looping track.
static u32 trackPosition(const Track *t) { return t->tone ? 0 : t->played / 4; }

static u32 trackLength(const Track *t) { return t->tone || !t->data_len ? 0 : t->data_len / 4; }

// Move the next read to `frame`, wrapped into the track. A tone has no position, so only its phase moves.
static void trackSeek(Track *t, s64 frame) {
    if (t->tone) {
        t->phase = fmodf(6.2831853f * t->hz * (float)(frame % SampleRate) / SampleRate, 6.2831853f);
        if (t->phase < 0.0f) t->phase += 6.2831853f;
        return;
    }
    const s64 length = trackLength(t);
    if (!length) return;
    frame %= length;
    if (frame < 0) frame += length;
    if (fseek(t->file, t->data_start + (long)frame * 4, SEEK_SET)) return;
    t->played = (u32)frame * 4;
}

typedef struct {
    Track track;
    AudioOutBuffer buffers[HostBuffers];
    u64 frames_queued; // Total frames handed to audout. This is the host's audio clock.
    bool initialized, started, playing;
} HostAudio;

static Result queueHostAudio(HostAudio *a, AudioOutBuffer *buffer) {
    trackRead(&a->track, buffer->buffer, ChunkFrames);
    a->frames_queued += ChunkFrames;
    armDCacheFlush(buffer->buffer, buffer->data_size);
    return audoutAppendAudioOutBuffer(buffer);
}

static Result hostAudioOpen(HostAudio *a) {
    Result rc = audoutInitialize();
    if (R_FAILED(rc)) return rc;
    a->initialized = true;
    rc = audoutStartAudioOut();
    if (R_FAILED(rc)) return rc;
    a->started = true;

    const size_t bytes = (ChunkFrames * GAMESHARE_LOCAL_AUDIO_FRAME_SIZE + 0xfff) & ~0xfffu;
    for (int i = 0; i < HostBuffers; ++i) {
        void *memory = memalign(0x1000, bytes);
        if (!memory) return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
        a->buffers[i] = (AudioOutBuffer){.buffer = memory, .buffer_size = bytes,
                                        .data_size = ChunkFrames * GAMESHARE_LOCAL_AUDIO_FRAME_SIZE};
    }
    return 0;
}

static Result hostAudioPrime(HostAudio *a) {
    for (int i = 0; i < HostBuffers; ++i) {
        Result rc = queueHostAudio(a, &a->buffers[i]);
        if (R_FAILED(rc)) return rc;
    }
    a->playing = true;
    return 0;
}

static Result hostAudioPump(HostAudio *a) {
    for (int i = 0; i < HostBuffers; ++i) {
        AudioOutBuffer *released = NULL;
        u32 count = 0;
        Result rc = audoutGetReleasedAudioOutBuffer(&released, &count);
        if (R_FAILED(rc) || !count) return rc;
        int j = 0;
        while (j < HostBuffers && released != &a->buffers[j]) ++j;
        if (j == HostBuffers) return MAKERESULT(Module_Libnx, LibnxError_InvalidCmifOutHeader);
        rc = queueHostAudio(a, released);
        if (R_FAILED(rc)) return rc;
    }
    return 0;
}

static void hostAudioClose(HostAudio *a, Report *log) {
    if (a->started) {
        Result rc = audoutStopAudioOut();
        record(log, "Stop host audio -> 0x%08x", (unsigned)rc);
    }
    if (a->initialized) audoutExit();
    for (int i = 0; i < HostBuffers; ++i) free(a->buffers[i].buffer);
    if (a->track.file) fclose(a->track.file);
    *a = (HostAudio){0};
}

// ---- Setup ----

static Result getUser(Report *log, AccountUid *uid) {
    record(log, "Getting the last opened user");
    Result rc = accountInitialize(AccountServiceType_Application);
    if (R_FAILED(rc)) return rc;
    rc = accountGetLastOpenedUser(uid);
    if (R_FAILED(rc) || !accountUidIsValid(uid)) {
        const PselUserSelectionSettings options = {0};
        rc = pselShowUserSelector(uid, &options);
    }
    accountExit();
    return rc;
}

static const char *openStep(GameShareLocalStage stage) {
    switch (stage) {
        case GameShareLocalStage_Lookup: return "look up stpl:u";
        case GameShareLocalStage_Acquire: return "acquire StreamPlay interface";
        case GameShareLocalStage_HidConfig: return "get guest input memory";
        case GameShareLocalStage_HidMap: return "map guest input memory";
        default: return "open local client";
    }
}

typedef struct {
    Track track;
    bool audio_enabled, first_write;
    int audio_offset, audio_pending;
    s64 credit;            // Frames the guest feed may still write: one host buffer's worth per host buffer played.
    u64 host_mark;         // host_audio.frames_queued when credit was last topped up.
    u64 stat_tick, stat_written, stat_host_mark;
    s16 chunk[ChunkFrames * 2];
    char audio_status[72], info[96], rumble_status[72], queue_status[80];
    GameShareLocalVibrationHandle rumble_handles[2 * sizeof(pad_ids) / sizeof(pad_ids[0])];
    size_t rumble_count;
    u64 rumble_started;
} Guest;

typedef struct {
    Report *log;
    GameShareLocalHost host;
    Framebuffer fb;
    HostAudio host_audio;
    Guest guests[GAMESHARE_LOCAL_MAX_GUESTS];
    u32 frame;
    unsigned session_number;
    u8 count, selected;
    bool separate;
    GameShareLocalGuestDisplay *displays[GAMESHARE_LOCAL_MAX_GUESTS];
    char status[100];
} App;

static void showGuestInfo(App *app, u8 slot) {
    Guest *guest = &app->guests[slot];
    const unsigned number = slot + 1;
    char nickname[33] = {0};
    record(app->log, "Guest %u: read nickname", number);
    Result nick = gameshareLocalHostGetGuestNickname(&app->host, slot, nickname);
    record(app->log, "Guest %u: nickname -> 0x%08x", number, (unsigned)nick);

    const size_t capacity = 0x20000;
    void *image = malloc(capacity);
    size_t size = 0;
    record(app->log, "Guest %u: read profile image", number);
    Result pic = image ? gameshareLocalHostGetGuestProfileImage(&app->host, slot, image, capacity, &size)
                       : MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    record(app->log, "Guest %u: profile image -> 0x%08x (%zu bytes)", number, (unsigned)pic, size);
    free(image);
    if (R_FAILED(nick) || R_FAILED(pic))
        snprintf(guest->info, sizeof(guest->info), "Guest info: nickname 0x%08x, image 0x%08x", (unsigned)nick, (unsigned)pic);
    else
        snprintf(guest->info, sizeof(guest->info), "Guest: %s (%zu B profile image)", nickname, size);
}

static void showAudioQueue(App *app, u8 slot) {
    Guest *guest = &app->guests[slot];
    const unsigned number = slot + 1;
    s32 capacity = 0, free_frames = 0, filled = 0;
    record(app->log, "Guest %u: query audio capacity", number);
    Result cap_rc = gameshareLocalHostGetGuestAudioCapacity(&app->host, &capacity);
    record(app->log, "Guest %u: audio capacity -> 0x%08x (%d frames)", number, (unsigned)cap_rc, (int)capacity);
    record(app->log, "Guest %u: query free audio frames", number);
    Result free_rc = gameshareLocalHostGetGuestAudioFree(&app->host, slot, &free_frames);
    record(app->log, "Guest %u: free audio -> 0x%08x (%d frames)", number, (unsigned)free_rc, (int)free_frames);
    record(app->log, "Guest %u: query filled audio frames", number);
    Result filled_rc = gameshareLocalHostGetGuestAudioFilled(&app->host, slot, &filled);
    record(app->log, "Guest %u: filled audio -> 0x%08x (%d frames)", number, (unsigned)filled_rc, (int)filled);
    if (R_FAILED(cap_rc) || R_FAILED(free_rc) || R_FAILED(filled_rc))
        snprintf(guest->queue_status, sizeof(guest->queue_status), "Audio queue: cap 0x%08x, free 0x%08x, filled 0x%08x",
                 (unsigned)cap_rc, (unsigned)free_rc, (unsigned)filled_rc);
    else
        snprintf(guest->queue_status, sizeof(guest->queue_status), "Audio queue: %d capacity, %d free, %d filled (frames)",
                 (int)capacity, (int)free_frames, (int)filled);
}

static void toggleGuestAudio(App *app, u8 slot) {
    Guest *guest = &app->guests[slot];
    guest->audio_enabled = !guest->audio_enabled;
    guest->audio_pending = 0;
    guest->first_write = guest->audio_enabled;
    if (guest->audio_enabled) {
        // Start the guest stem at the point of the song the host is playing now, so the two line up.
        // The host has read played/4 frames, of which about 3.5 buffers are still queued. The guest
        // feed keeps GuestPrefill frames queued ahead of what the guest hears.
        HostAudio *host = &app->host_audio;
        if (host->playing) {
            const s64 heard = (s64)trackPosition(&host->track) - (HostBuffers * ChunkFrames - ChunkFrames / 2);
            trackSeek(&guest->track, heard + GuestPrefill);
            guest->host_mark = host->frames_queued;
        } else {
            guest->host_mark = armTicksToNs(armGetSystemTick());
        }
        guest->credit = GuestPrefill;
        guest->stat_tick = armGetSystemTick();
        guest->stat_written = 0;
        guest->stat_host_mark = host->frames_queued;
    }
    snprintf(guest->audio_status, sizeof(guest->audio_status), "Guest audio feed %s", guest->audio_enabled ? "on (mixed)" : "off");
    record(app->log, "Guest %u: %s", (unsigned)slot + 1, guest->audio_status);
}

// Bank one frame of credit per frame the host has played, so the guest feed runs on the host's audio clock
// and never faster. Without a host clock (host audio failed), fall back to the system tick.
static void topUpGuestCredit(App *app, Guest *guest) {
    const HostAudio *host = &app->host_audio;
    u64 delta;
    if (host->playing) {
        delta = host->frames_queued - guest->host_mark;
        guest->host_mark = host->frames_queued;
    } else {
        const u64 now = armTicksToNs(armGetSystemTick());
        delta = (now - guest->host_mark) * SampleRate / 1000000000ull;
        guest->host_mark += delta * 1000000000ull / SampleRate;
    }
    guest->credit += (s64)delta;
    if (guest->credit > GuestMaxCredit) guest->credit = GuestMaxCredit;
}

static void logGuestAudioRate(App *app, u8 slot) {
    Guest *guest = &app->guests[slot];
    const u64 now = armGetSystemTick();
    const u64 ns = armTicksToNs(now - guest->stat_tick);
    if (ns < 1000000000ull) return;
    s32 free_frames = -1, filled = -1;
    gameshareLocalHostGetGuestAudioFree(&app->host, slot, &free_frames);
    gameshareLocalHostGetGuestAudioFilled(&app->host, slot, &filled);
    const u64 host_frames = app->host_audio.frames_queued - guest->stat_host_mark;
    record(app->log, "Guest %u audio over %llu ms: wrote %llu frames (%llu/s), host played %llu (%llu/s), free %d filled %d credit %lld",
           (unsigned)slot + 1, (unsigned long long)(ns / 1000000ull), (unsigned long long)guest->stat_written,
           (unsigned long long)(guest->stat_written * 1000000000ull / ns), (unsigned long long)host_frames,
           (unsigned long long)(host_frames * 1000000000ull / ns), (int)free_frames, (int)filled, (long long)guest->credit);
    guest->stat_tick = now;
    guest->stat_written = 0;
    guest->stat_host_mark = app->host_audio.frames_queued;
}

static void pumpGuestAudio(App *app, u8 slot) {
    Guest *guest = &app->guests[slot];
    topUpGuestCredit(app, guest);
    s32 free_frames = 0;
    Result rc = gameshareLocalHostGetGuestAudioFree(&app->host, slot, &free_frames);
    if (R_SUCCEEDED(rc) && free_frames > 0 && (guest->audio_pending || guest->credit > 0)) {
        if (!guest->audio_pending) {
            guest->audio_offset = 0;
            s64 want = guest->credit < free_frames ? guest->credit : free_frames;
            if (want > ChunkFrames) want = ChunkFrames;
            guest->audio_pending = (int)want;
            trackRead(&guest->track, guest->chunk, guest->audio_pending);
        }
        const int frames = free_frames < guest->audio_pending ? free_frames : guest->audio_pending;
        s32 written = 0;
        if (guest->first_write) record(app->log, "Guest %u: write the first %d audio frames", (unsigned)slot + 1, frames);
        rc = gameshareLocalHostWriteGuestAudio(&app->host, slot, guest->chunk + 2 * guest->audio_offset, frames, &written);
        if (guest->first_write) {
            record(app->log, "Guest %u: first audio write -> 0x%08x (%d frames)", (unsigned)slot + 1, (unsigned)rc, (int)written);
            guest->first_write = false;
        }
        if (R_SUCCEEDED(rc)) {
            guest->audio_offset += written;
            guest->audio_pending -= written;
            guest->credit -= written;
            guest->stat_written += (u64)written;
        }
    }
    if (R_FAILED(rc)) {
        snprintf(guest->audio_status, sizeof(guest->audio_status), "Guest audio stopped: 0x%08x", (unsigned)rc);
        record(app->log, "Guest %u: %s", (unsigned)slot + 1, guest->audio_status);
        guest->audio_enabled = false;
        return;
    }
    logGuestAudioRate(app, slot);
}

static void stopRumble(App *app, u8 slot) {
    Guest *guest = &app->guests[slot];
    const HidVibrationValue off = {.freq_low = 160.0f, .freq_high = 320.0f};
    for (size_t i = 0; i < guest->rumble_count; ++i) {
        Result rc = gameshareLocalHostSendVibration(&app->host, guest->rumble_handles[i], &off);
        if (R_FAILED(rc)) record(app->log, "Guest %u: stop vibration -> 0x%08x", (unsigned)slot + 1, (unsigned)rc);
    }
    guest->rumble_count = 0;
}

static void rumble(App *app, u8 slot) {
    Guest *guest = &app->guests[slot];
    stopRumble(app, slot);
    const HidVibrationValue strong = {.amp_low = 0.8f, .freq_low = 160.0f, .amp_high = 0.8f, .freq_high = 320.0f};
    unsigned failed = 0;
    for (size_t i = 0; i < sizeof(pad_ids) / sizeof(pad_ids[0]); ++i) {
        GameShareLocalPad pad;
        if (R_FAILED(gameshareLocalReadGuestPad(&app->host.client, slot, pad_ids[i], &pad)) || !pad.connected) continue;
        GameShareLocalVibrationHandle handles[2];
        const s32 n = gameshareLocalMakeVibrationHandles(handles, 2, slot, pad_ids[i], pad.style);
        for (s32 j = 0; j < n; ++j) {
            record(app->log, "Guest %u: vibrate pad %u device %d", (unsigned)slot + 1, (unsigned)pad_ids[i], (int)j);
            Result rc = gameshareLocalHostSendVibration(&app->host, handles[j], &strong);
            record(app->log, "Guest %u: vibrate -> 0x%08x", (unsigned)slot + 1, (unsigned)rc);
            if (R_SUCCEEDED(rc)) guest->rumble_handles[guest->rumble_count++] = handles[j];
            else ++failed;
        }
    }
    snprintf(guest->rumble_status, sizeof(guest->rumble_status), "Vibration: %zu accepted, %u failed (check guest)", guest->rumble_count, failed);
    guest->rumble_started = armGetSystemTick();
}

static void captureInput(App *app, u8 slot) {
    const GameShareLocalClient *client = &app->host.client;
    const unsigned guest = slot + 1;
    record(app->log, "Guest %u: capture pad, touch and gyro inputs", guest);
    HidTouchScreenState touch = {0};
    Result rc = gameshareLocalReadGuestTouch(client, slot, &touch);
    record(app->log, "Guest %u: touch -> 0x%08x; sample %llu; contacts %d", guest, (unsigned)rc,
           (unsigned long long)touch.sampling_number, (int)touch.count);
    if (R_SUCCEEDED(rc)) {
        for (s32 i = 0; i < touch.count; ++i)
            record(app->log, "Guest %u: touch [%d] at %u,%u", guest, (int)i, (unsigned)touch.touches[i].x, (unsigned)touch.touches[i].y);
    }
    for (size_t i = 0; i < sizeof(pad_ids) / sizeof(pad_ids[0]); ++i) {
        const HidNpadIdType id = pad_ids[i];
        GameShareLocalPad pad = {0};
        rc = gameshareLocalReadGuestPad(client, slot, id, &pad);
        record(app->log, "Guest %u pad %u: pad -> 0x%08x; connected %d; style 0x%x; buttons 0x%llx", guest,
               (unsigned)id, (unsigned)rc, pad.connected, (unsigned)pad.style, (unsigned long long)pad.state.buttons);
        if (pad.connected) {
            for (unsigned sensor = GameShareLocalSensor_FullKey; sensor <= GameShareLocalSensor_JoyRight; ++sensor) {
                HidSixAxisSensorState gyro = {0};
                rc = gameshareLocalReadGuestSixAxis(client, slot, id, (GameShareLocalSensor)sensor, &gyro);
                record(app->log, "Guest %u pad %u: gyro %u -> 0x%08x; sample %llu; velocity %.2f %.2f %.2f", guest,
                       (unsigned)id, sensor, (unsigned)rc, (unsigned long long)gyro.sampling_number,
                       gyro.angular_velocity.x, gyro.angular_velocity.y, gyro.angular_velocity.z);
            }
        }
    }
    snprintf(app->status, sizeof(app->status), "Guest %u input snapshot appended to SD log.", guest);
}

static void drawGuestInput(App *app, u8 slot, u32 *p, u32 stride, u32 white) {
    char line[96];
    for (size_t i = 0; i < sizeof(pad_ids) / sizeof(pad_ids[0]); ++i) {
        GameShareLocalPad pad;
        Result rc = gameshareLocalReadGuestPad(&app->host.client, slot, pad_ids[i], &pad);
        const char *name = pad_ids[i] == HidNpadIdType_Handheld ? "Handheld" : i == 0 ? "Pad 1" : "Pad 2";
        if (R_FAILED(rc))
            snprintf(line, sizeof(line), "%s: read 0x%08x", name, (unsigned)rc);
        else if (pad.connected)
            snprintf(line, sizeof(line), "%s: style %02x   buttons %llx   left stick %d,%d",
                     name, (unsigned)pad.style, (unsigned long long)pad.state.buttons,
                     pad.state.analog_stick_l.x, pad.state.analog_stick_l.y);
        else
            snprintf(line, sizeof(line), "%s: waiting for input", name);
        const int y = 352 + (int)i * 48;
        text(p, stride, Width, 40, y, line, 1, white);
        if (R_FAILED(rc) || !pad.connected) continue;
        GameShareLocalSensor sensor;
        switch (pad.style) {
            case HidNpadStyleTag_NpadFullKey:  sensor = GameShareLocalSensor_FullKey; break;
            case HidNpadStyleTag_NpadHandheld: sensor = GameShareLocalSensor_Handheld; break;
            case HidNpadStyleTag_NpadJoyDual:  sensor = GameShareLocalSensor_JoyDualLeft; break;
            case HidNpadStyleTag_NpadJoyLeft:  sensor = GameShareLocalSensor_JoyLeft; break;
            case HidNpadStyleTag_NpadJoyRight: sensor = GameShareLocalSensor_JoyRight; break;
            default: continue;
        }
        HidSixAxisSensorState motion;
        rc = gameshareLocalReadGuestSixAxis(&app->host.client, slot, pad_ids[i], sensor, &motion);
        if (R_SUCCEEDED(rc) && sensor == GameShareLocalSensor_JoyDualLeft && !motion.sampling_number)
            rc = gameshareLocalReadGuestSixAxis(&app->host.client, slot, pad_ids[i], GameShareLocalSensor_JoyDualRight, &motion);
        if (R_FAILED(rc))
            snprintf(line, sizeof(line), "Gyro: read 0x%08x", (unsigned)rc);
        else if (motion.sampling_number)
            snprintf(line, sizeof(line), "Gyro #%llu: %.2f  %.2f  %.2f", (unsigned long long)motion.sampling_number,
                     motion.angular_velocity.x, motion.angular_velocity.y, motion.angular_velocity.z);
        else
            snprintf(line, sizeof(line), "Gyro: waiting for sample");
        text(p, stride, Width, 64, y + 20, line, 1, white);
    }
    HidTouchScreenState touch;
    Result rc = gameshareLocalReadGuestTouch(&app->host.client, slot, &touch);
    if (R_FAILED(rc))
        snprintf(line, sizeof(line), "Touch: read 0x%08x", (unsigned)rc);
    else if (touch.count)
        snprintf(line, sizeof(line), "Touch: %d contact(s), first at %u,%u", (int)touch.count,
                 (unsigned)touch.touches[0].x, (unsigned)touch.touches[0].y);
    else
        snprintf(line, sizeof(line), "Touch: no contacts");
    text(p, stride, Width, 40, 502, line, 1, white);
}


static Result drawGuest(App *app, u8 slot) {
    static u32 pixels[Width * Height];
    const u32 colors[] = {
        RGBA8_MAXALPHA(163, 89, 10), RGBA8_MAXALPHA(100, 42, 135), RGBA8_MAXALPHA(16, 108, 69),
    };
    const u32 white = RGBA8_MAXALPHA(245, 245, 245);
    fill(pixels, Width, 0, 0, Width, Height, colors[slot]);
    char label[24];
    snprintf(label, sizeof(label), "GUEST %u", (unsigned)slot + 1);
    text(pixels, Width, Width, 64, 65, label, 3, white);
    drawGuestInput(app, slot, pixels, Width, white);
    fill(pixels, Width, 64 + (int)(app->frame % 120) * 8, 600, 48, 48, white);
    return gameshareLocalGuestDisplayPresent(app->displays[slot], pixels, Width);
}

static void drawHost(App *app, u32 guest_mask, bool connection_read) {
    u32 bytes;
    u32 *p = framebufferBegin(&app->fb, &bytes);
    if (!p) return;
    const u32 stride = bytes / 4, white = RGBA8_MAXALPHA(245, 245, 245);
    fill(p, stride, 0, 0, Width, Height, RGBA8_MAXALPHA(18, 52, 118));
    text(p, stride, Width, 40, 24, "GameShare Test Suite", 3, white);
    const char *lines[] = {
        "Left/Right  select guest    +  end session",
        "A  mixed audio    Y  vibrate    L  nickname/profile    R  audio queue",
        "X  log guest input samples",
    };
    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); ++i)
        text(p, stride, Width, 40, 90 + (int)i * 25, lines[i], 1, white);

    char info[100];
    for (u8 slot = 0; slot < app->count; ++slot) {
        const bool selected = slot == app->selected;
        const bool connected = (guest_mask & (1u << slot)) != 0;
        const int x = 40 + slot * 396;
        fill(p, stride, x, 170, 372, 42, selected ? RGBA8_MAXALPHA(20, 125, 133) : RGBA8_MAXALPHA(12, 35, 82));
        snprintf(info, sizeof(info), "%sGuest %u %s", selected ? "> " : "", (unsigned)slot + 1,
                 !connection_read ? "unknown" : connected ? "connected" : "offline");
        text(p, stride, x + 366, x + 12, 183, info, 1, white);
    }
    const Guest *guest = &app->guests[app->selected];
    snprintf(info, sizeof(info), "Host audio %s   %s", app->host_audio.playing ? "on" : "off",
             guest->audio_status[0] ? guest->audio_status : "Guest audio feed off");
    text(p, stride, Width, 40, 226, info, 1, white);
    text(p, stride, Width, 40, 253, guest->queue_status, 1, white);
    text(p, stride, Width, 40, 280, guest->info, 1, white);
    text(p, stride, Width, 40, 307, guest->rumble_status, 1, white);
    if (connection_read && (guest_mask & (1u << app->selected)))
        drawGuestInput(app, app->selected, p, stride, white);
    text(p, stride, Width, 40, 552, app->status, 1, white);
    text(p, stride, Width, 40, 585, app->separate ? "HOST SCREEN (separate views)" : "HOST SCREEN (mirroring)", 1, white);
    fill(p, stride, 40 + (int)(app->frame % 120) * 8, 650, 48, 48, RGBA8_MAXALPHA(94, 217, 206));
    framebufferEnd(&app->fb);
}

static void drawMenu(App *app) {
    u32 bytes;
    u32 *p = framebufferBegin(&app->fb, &bytes);
    if (!p) return;
    const u32 stride = bytes / 4, white = RGBA8_MAXALPHA(245, 245, 245);
    fill(p, stride, 0, 0, Width, Height, RGBA8_MAXALPHA(18, 52, 118));
    text(p, stride, Width, 60, 80, "GameShare Test Suite", 3, white);
    text(p, stride, Width, 60, 188, "Select maximum guests", 2, white);
    for (u8 count = 1; count <= GAMESHARE_LOCAL_MAX_GUESTS; ++count) {
        char number[20];
        snprintf(number, sizeof(number), "%u guest%s", (unsigned)count, count == 1 ? "" : "s");
        const int x = 60 + (count - 1) * 370;
        fill(p, stride, x, 270, 334, 102, count == app->count ? RGBA8_MAXALPHA(20, 125, 133) : RGBA8_MAXALPHA(12, 35, 82));
        text(p, stride, x + 334, x + 30, 307, number, 2, white);
    }
    text(p, stride, Width, 60, 430, "Left/Right  choose    A  start matchmaking    +  quit", 1, white);
    text(p, stride, Width, 60, 480, app->status, 1, white);
    text(p, stride, Width, 60, 530, app->separate ? "X  Mode: separate views" : "X  Mode: mirror host", 1, white);
    text(p, stride, Width, 60, 580, "Session logs: sd:/gameshare-full-test.txt", 1, white);
    framebufferEnd(&app->fb);
}

static bool run(App *app, PadState *pad) {
    u32 connected = 0;
    Result last_result = 0, last_query = 0;
    bool first_query = true, first_state = true;
    Event *event = gameshareLocalHostGetConnectionEvent(&app->host);
    record(app->log, "Connection event handle %s", event ? "ready" : "unavailable");
    for (; appletMainLoop(); ++app->frame) {
        padUpdate(pad);
        const u64 down = padGetButtonsDown(pad);
        if (down & HidNpadButton_Plus) return true;
        if (down & HidNpadButton_Left) app->selected = (app->selected + app->count - 1) % app->count;
        if (down & HidNpadButton_Right) app->selected = (app->selected + 1) % app->count;

        if (event && R_SUCCEEDED(eventWait(event, 0))) {
            Result rc = eventClear(event);
            record(app->log, "Connection event signalled, clear -> 0x%08x", (unsigned)rc);
            if (R_FAILED(rc)) event = NULL;
        }
        GameShareLocalConnection info;
        Result query = gameshareLocalHostGetConnection(&app->host, &info);
        if (first_query || query != last_query) {
            record(app->log, "Query connection state -> 0x%08x", (unsigned)query);
            last_query = query;
            first_query = false;
        }
        const bool connection_read = R_SUCCEEDED(query);
        if (connection_read && (first_state || info.guest_flags != connected || info.result != last_result)) {
            record(app->log, "Connected guest mask 0x%x, connection result 0x%08x", (unsigned)info.guest_flags, (unsigned)info.result);
            for (u8 slot = 0; slot < app->count; ++slot) {
                if ((connected & (1u << slot)) && !(info.guest_flags & (1u << slot))) {
                    Guest *guest = &app->guests[slot];
                    guest->rumble_count = 0;
                    guest->info[0] = guest->queue_status[0] = guest->rumble_status[0] = 0;
                    if (guest->audio_enabled) toggleGuestAudio(app, slot);
                }
            }
            connected = info.guest_flags;
            last_result = info.result;
            first_state = false;
        }
        const u8 slot = app->selected;
        if (connection_read && (connected & (1u << slot))) {
            if (down & HidNpadButton_A) toggleGuestAudio(app, slot);
            if (down & HidNpadButton_Y) rumble(app, slot);
            if (down & HidNpadButton_L) showGuestInfo(app, slot);
            if (down & HidNpadButton_R) showAudioQueue(app, slot);
            if (down & HidNpadButton_X) captureInput(app, slot);
        }
        if (connection_read) {
            for (u8 active = 0; active < app->count; ++active) {
                if (!(connected & (1u << active))) continue;
                Guest *guest = &app->guests[active];
                if (guest->rumble_count && armTicksToNs(armGetSystemTick() - guest->rumble_started) >= 500000000ull)
                    stopRumble(app, active);
                if (guest->audio_enabled) pumpGuestAudio(app, active);
            }
        }
        if (app->host_audio.playing) {
            Result rc = hostAudioPump(&app->host_audio);
            if (R_FAILED(rc)) {
                record(app->log, "Host audio update -> 0x%08x", (unsigned)rc);
                hostAudioClose(&app->host_audio, app->log);
            }
        }
        if (app->separate && connection_read) {
            for (u8 slot = 0; slot < app->count; ++slot) {
                if (!(connected & BIT(slot))) continue;
                Result rc = drawGuest(app, slot);
                if (R_FAILED(rc)) {
                    record(app->log, "Present guest %u -> 0x%08x", (unsigned)slot + 1, (unsigned)rc);
                    snprintf(app->status, sizeof(app->status), "Guest display returned 0x%08x.", (unsigned)rc);
                    return true;
                }
            }
        }
        drawHost(app, connected, connection_read);
    }
    return false;
}

static Result startSession(App *app, const char **step) {
    Report *log = app->log;
    *step = "release prior session";
    if (app->host.work || app->host.client.hid_memory.handle || app->host.client.hid_memory.map_addr) {
        Result rc = gameshareLocalHostClose(&app->host);
        record(log, "Retry previous host release -> 0x%08x", (unsigned)rc);
        if (R_FAILED(rc)) return rc;
    }

    *step = "select application user";
    AccountUid uid = {0};
    Result rc = getUser(log, &uid);
    if (R_FAILED(rc)) return rc;

    GameShareLocalStage stage;
    record(log, "Open local StreamPlay client");
    rc = gameshareLocalOpen(&app->host.client, &stage);
    if (R_FAILED(rc)) {
        *step = openStep(stage);
        return rc;
    }

    trackOpen(&app->host_audio.track, log, "Host", tv_path, 440.0f);
    for (u8 slot = 0; slot < app->count; ++slot) {
        char name[20];
        snprintf(name, sizeof(name), "Guest %u", (unsigned)slot + 1);
        trackOpen(&app->guests[slot].track, log, name, guest_paths[slot], 660.0f + slot * 220.0f);
    }
    record(log, "Open host audio output");
    Result audio_rc = hostAudioOpen(&app->host_audio);
    record(log, "Open host audio output -> 0x%08x", (unsigned)audio_rc);
    if (R_FAILED(audio_rc)) hostAudioClose(&app->host_audio, log);

    const GameShareLocalAudio audio = {.speaker_volume = 1.0f, .tv_volume = 1.0f, .external_volume = 1.0f};
    *step = "prepare host";
    record(log, "Prepare local mode %d for up to %u guests", app->separate, (unsigned)app->count);
    rc = gameshareLocalHostPrepareEx(&app->host, app->count, &audio, app->separate ? 1 : 0);
    if (R_FAILED(rc)) { *step = app->host.operation ? app->host.operation : *step; return rc; }
    record(log, "Local work memory allocated: %zu bytes", app->host.work_size);

    record(log, "Enable guest touch and gyro");
    rc = gameshareLocalHostEnableSensors(&app->host, Width, Height);
    if (R_FAILED(rc)) { *step = app->host.operation ? app->host.operation : "enable sensors"; return rc; }

    if (app->separate) {
        for (u8 slot = 0; slot < app->count; ++slot) {
            record(log, "Open player%u display", (unsigned)slot + 1);
            rc = gameshareLocalGuestDisplayOpen(&app->displays[slot], slot, Width, Height);
            if (R_FAILED(rc)) {
                *step = gameshareLocalGuestDisplayLastStep();
                return rc;
            }
        }
    }

    *step = "matchmake";
    record(log, "Start local matchmaking");
    rc = gameshareLocalHostMatchmake(&app->host, uid);
    if (R_FAILED(rc)) { *step = app->host.operation ? app->host.operation : *step; return rc; }
    record(log, "Matchmaking returned %d guest(s)", (int)app->host.joined);

    *step = "start stream";
    if (app->separate) {
        for (u8 slot = 0; slot < app->host.joined; ++slot) {
            record(log, "Submit initial player%u frame", (unsigned)slot + 1);
            rc = drawGuest(app, slot);
            if (R_FAILED(rc) && R_VALUE(rc) != KERNELRESULT(TimedOut)) {
                *step = "present initial guest frame";
                return rc;
            }
        }
    }
    record(log, "Start local streaming");
    rc = gameshareLocalHostStart(&app->host);
    if (R_FAILED(rc)) { *step = app->host.operation ? app->host.operation : *step; return rc; }
    record(log, "Stream started");

    if (app->host_audio.started) {
        record(log, "Queue host audio");
        audio_rc = hostAudioPrime(&app->host_audio);
        record(log, "Queue host audio -> 0x%08x", (unsigned)audio_rc);
        if (R_FAILED(audio_rc)) hostAudioClose(&app->host_audio, log);
    }
    return 0;
}

static Result closeSession(App *app) {
    for (u8 slot = 0; slot < app->count; ++slot) {
        stopRumble(app, slot);
        if (app->guests[slot].track.file) fclose(app->guests[slot].track.file);
    }
    memset(app->guests, 0, sizeof(app->guests));
    app->selected = 0;
    app->frame = 0;
    hostAudioClose(&app->host_audio, app->log);
    record(app->log, "Close local StreamPlay host");
    Result rc = gameshareLocalHostClose(&app->host);
    record(app->log, "Host close -> 0x%08x", (unsigned)rc);
    for (u8 slot = 0; slot < GAMESHARE_LOCAL_MAX_GUESTS; ++slot) {
        if (!app->displays[slot]) continue;
        Result display_rc = gameshareLocalGuestDisplayClose(app->displays[slot]);
        record(app->log, "Close player%u -> 0x%08x", (unsigned)slot + 1, (unsigned)display_rc);
        if (R_SUCCEEDED(display_rc)) app->displays[slot] = NULL;
        else if (R_SUCCEEDED(rc)) rc = display_rc;
    }
    return rc;
}

int main(void) {
    Report log = {.file = fopen(log_path, "a")};
    if (!log.file) {
        char message[144];
        snprintf(message, sizeof(message), "Could not open %s (errno %d).", log_path, errno);
        showStatus(message);
        return EXIT_FAILURE;
    }

    static App app;
    app.log = &log;
    app.count = 1;
    app.separate = true;
    record(&log, "--- GameShare Test Suite. Applet type %d ---", (int)appletGetAppletType());
    Result rc = framebufferCreate(&app.fb, nwindowGetDefault(), Width, Height, PIXEL_FORMAT_RGBA_8888, 2);
    if (R_SUCCEEDED(rc)) rc = framebufferMakeLinear(&app.fb);
    if (R_FAILED(rc)) {
        snprintf(app.status, sizeof(app.status), "Create host framebuffer failed (0x%08x).", (unsigned)rc);
        record(&log, "%s", app.status);
    } else {
        PadState pad;
        padConfigureInput(1, HidNpadStyleSet_NpadStandard);
        padInitializeDefault(&pad);
        while (appletMainLoop()) {
            padUpdate(&pad);
            const u64 down = padGetButtonsDown(&pad);
            if (down & HidNpadButton_Plus) break;
            if (down & HidNpadButton_X) app.separate = !app.separate;
            if (down & HidNpadButton_Left)
                app.count = app.count == 1 ? GAMESHARE_LOCAL_MAX_GUESTS : app.count - 1;
            if (down & HidNpadButton_Right)
                app.count = app.count == GAMESHARE_LOCAL_MAX_GUESTS ? 1 : app.count + 1;
            if (!(down & HidNpadButton_A)) {
                drawMenu(&app);
                continue;
            }

            record(&log, "--- Session %u: max guests %u ---", ++app.session_number, (unsigned)app.count);
            snprintf(app.status, sizeof(app.status), "Starting session %u...", app.session_number);
            drawMenu(&app);
            const char *step;
            Result session_rc = startSession(&app, &step);
            bool keep_running = true;
            if (session_rc == MAKERESULT(287, 30)) {
                snprintf(app.status, sizeof(app.status), "Matchmaking cancelled.");
            } else if (R_FAILED(session_rc)) {
                snprintf(app.status, sizeof(app.status), "Failed to %s (0x%08x).", step, (unsigned)session_rc);
            } else {
                app.status[0] = 0;
                keep_running = run(&app, &pad);
                if (!app.status[0]) snprintf(app.status, sizeof(app.status), "Session %u ended.", app.session_number);
            }
            record(&log, "Session %u: %s", app.session_number, app.status);
            Result close_rc = closeSession(&app);
            if (R_FAILED(close_rc))
                snprintf(app.status, sizeof(app.status), "Host cleanup returned 0x%08x. See SD log.", (unsigned)close_rc);
            if (!keep_running || R_FAILED(close_rc)) break;
        }
    }
    if (app.fb.has_init) framebufferClose(&app.fb);
    record(&log, "Suite ended after %u session attempt(s).", app.session_number);
    if (fclose(log.file)) log.failed = true;
    if (R_FAILED(fsdevCommitDevice("sdmc"))) log.failed = true;
    if (log.failed) showStatus("Could not save the complete SD log.");
    else if (R_FAILED(rc)) showStatus(app.status);
    return R_FAILED(rc) ? EXIT_FAILURE : EXIT_SUCCESS;
}
