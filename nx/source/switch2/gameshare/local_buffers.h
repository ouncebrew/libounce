// SPDX-License-Identifier: MIT
#pragma once
#include <switch.h>

// Internal helpers for a single initialized local StreamPlay shim.
// After any registration attempt, release the session and use the wait helper
// before accessing or freeing the application-owned pages.
Result gameshareRegisterLocalBuffers(Service *shim, void *buffer, size_t size, u8 mode, u8 guests);
Result gameshareMakeLocalAppletArg(Service *shim, AccountUid uid, u8 guests, u8 out[128]);
Result gameshareWaitLocalBuffers(void *buffer, size_t size);
