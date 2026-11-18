// SPDX-License-Identifier: MIT
#pragma once

#include <switch.h>

// The local StreamPlay service prepares the input. The applet blocks until
// matchmaking completes or the user cancels. Outputs are zero on failure.
Result gameshareLocalShowMatchmake(const u8 args[128], u32 *session_value, s32 *joined);
