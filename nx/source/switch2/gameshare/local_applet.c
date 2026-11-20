// SPDX-License-Identifier: MIT
#include "local_applet.h"

enum {
    // Bananza passes the full ID to AM. On 23.0.1 AM maps its low byte
    // to ns.applet!splay_applet_id when resolving the Splay program.
    LocalMatchmakeAppletId = 0x01000024,
    LocalMatchmakeAppletVersion = 0x10000,
    MatchmakeCancelled = MAKERESULT(287, 30),
};

typedef struct {
    u32 session_value;
    s32 joined;
    u8 remaining[0xa8];
} MatchmakeReply;

_Static_assert(sizeof(MatchmakeReply) == 0xb0, "StreamPlay applet reply size");

Result gameshareLocalShowMatchmake(const u8 args[128], u32 *session_value, s32 *joined) {
    if (session_value) *session_value = 0;
    if (joined) *joined = 0;
    if (!args || !session_value || !joined)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    AppletHolder holder = {0};
    Result rc = appletCreateLibraryApplet(&holder, (AppletId)LocalMatchmakeAppletId,
                                          LibAppletMode_AllForeground);
    if (R_SUCCEEDED(rc)) {
        LibAppletArgs common;
        libappletArgsCreate(&common, LocalMatchmakeAppletVersion);
        rc = libappletArgsPush(&common, &holder);
    }
    if (R_SUCCEEDED(rc)) rc = libappletPushInData(&holder, args, 0x80);
    if (R_SUCCEEDED(rc)) rc = appletHolderStart(&holder);
    if (R_SUCCEEDED(rc)) {
        appletHolderJoin(&holder);
        LibAppletExitReason reason = appletHolderGetExitReason(&holder);
        if (reason == LibAppletExitReason_Canceled) rc = MatchmakeCancelled;
        else if (reason != LibAppletExitReason_Normal)
            rc = MAKERESULT(Module_Libnx, LibnxError_LibAppletBadExit);
    }
    if (R_SUCCEEDED(rc)) {
        MatchmakeReply reply = {0};
        size_t received = 0;
        rc = libappletPopOutData(&holder, &reply, sizeof(reply), &received);
        if (R_SUCCEEDED(rc)) {
            if (received != sizeof(reply))
                rc = MAKERESULT(Module_Libnx, LibnxError_BadAppletReceiveMessage);
            else if (reply.joined <= 0)
                rc = MatchmakeCancelled;
            else {
                *session_value = reply.session_value;
                *joined = reply.joined;
            }
        }
    }
    appletHolderClose(&holder);
    return rc;
}
