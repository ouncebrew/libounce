/* SPDX-License-Identifier: ISC
 * A window's VI2 layer, and the windows of nwindowCreateForDisplay.
 *
 * A window presents to one layer. The layer is attached to a display and
 * selected on the display's endpoint "default". The default window's layer is
 * on the default display; nwindowCreateForDisplay makes a window whose layer
 * is on a display that the caller names. Such a window is like the default
 * window: it has no buffer queue, and a producer presents its own images to
 * the window's layer (nwindowGetVi2Layer).
 * @author kurumuz
 */
#include <string.h>
#include "kernel/mutex.h"
#include "switch2/services/vi2.h"
#include "cpu_window.h"
#include "vi2_window.h"

// The default window is not one of these.
#define NWINDOW_MAX_DISPLAY_WINDOWS 4

typedef struct {
    NWindow* nw;        // NULL while the entry is free.
    Vi2Window vi2;
} NwindowDisplayWindow;

static Mutex g_displayWindowsMutex;
static NwindowDisplayWindow g_displayWindows[NWINDOW_MAX_DISPLAY_WINDOWS];

Result _vi2WindowOpen(Vi2Window* w, const char* display_name)
{
    Result rc = 0;
    if (!w->vi2_initialized) {
        rc = vi2Initialize();
        w->vi2_initialized = R_SUCCEEDED(rc);
    }
    if (R_SUCCEEDED(rc) && !w->display_open) {
        rc = display_name ? vi2OpenDisplay(&w->display, display_name) : vi2OpenDefaultDisplay(&w->display);
        w->display_open = R_SUCCEEDED(rc);
        if (w->display_open)
            vi2CreateEndpoint(&w->endpoint, &w->display, "default");
    }
    if (R_SUCCEEDED(rc) && !w->layer_created) {
        rc = vi2CreateLayer(&w->layer);
        w->layer_created = R_SUCCEEDED(rc);
    }
    if (R_SUCCEEDED(rc) && !w->layer_attached) {
        rc = vi2AttachLayer(&w->layer, &w->display);
        w->layer_attached = R_SUCCEEDED(rc);
    }
    if (R_SUCCEEDED(rc) && !w->layer_selected) {
        rc = vi2SelectLayer(&w->layer, &w->endpoint);
        w->layer_selected = R_SUCCEEDED(rc);
    }
    return rc;
}

Result _vi2WindowCloseLayer(Vi2Window* w)
{
    Result rc = 0;
    if (w->layer_attached) {
        rc = vi2DetachLayer(&w->layer);
        if (R_FAILED(rc))
            return rc;
        w->layer_attached = false;
    }
    if (w->layer_selected) {
        rc = vi2ClearLayerSelection(&w->endpoint);
        if (R_FAILED(rc))
            return rc;
        w->layer_selected = false;
    }
    if (w->layer_created) {
        rc = vi2CloseLayer(&w->layer);
        if (R_FAILED(rc))
            return rc;
        w->layer_created = false;
    }
    return 0;
}

Result _vi2WindowCloseDisplay(Vi2Window* w)
{
    if (w->display_open) {
        vi2CloseEndpoint(&w->endpoint);
        Result rc = vi2CloseDisplay(&w->display);
        if (R_FAILED(rc))
            return rc;
        w->display_open = false;
    }
    if (w->vi2_initialized) {
        vi2Exit();
        w->vi2_initialized = false;
    }
    return 0;
}

// Call with g_displayWindowsMutex held.
static NwindowDisplayWindow* _nwindowFindDisplayWindow(NWindow* nw)
{
    for (u32 i = 0; i < NWINDOW_MAX_DISPLAY_WINDOWS; i++) {
        if (g_displayWindows[i].nw == nw)
            return &g_displayWindows[i];
    }
    return NULL;
}

Vi2Window* _nwindowGetDisplayWindow(NWindow* nw)
{
    if (!nw)
        return NULL;
    mutexLock(&g_displayWindowsMutex);
    NwindowDisplayWindow* entry = _nwindowFindDisplayWindow(nw);
    mutexUnlock(&g_displayWindowsMutex);
    return entry ? &entry->vi2 : NULL;
}

Result nwindowCreateForDisplay(NWindow* nw, const char* display_name)
{
    if (!nw || !display_name)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    mutexLock(&g_displayWindowsMutex);
    NwindowDisplayWindow* entry = _nwindowFindDisplayWindow(NULL);
    if (entry) {
        memset(entry, 0, sizeof(*entry));
        entry->nw = nw;
    }
    mutexUnlock(&g_displayWindowsMutex);
    if (!entry)
        return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);

    _nwindowCreateForCpuWindow(nw);
    nwindowSetDimensions(nw, 1280, 720);

    Result rc = _vi2WindowOpen(&entry->vi2, display_name);
    if (R_FAILED(rc))
        nwindowClose(nw);
    return rc;
}

Result _nwindowCloseDisplayWindow(NWindow* nw)
{
    mutexLock(&g_displayWindowsMutex);
    NwindowDisplayWindow* entry = _nwindowFindDisplayWindow(nw);
    mutexUnlock(&g_displayWindowsMutex);
    if (!nw || !entry)
        return 0;

    Result rc = _vi2WindowCloseLayer(&entry->vi2);
    if (R_SUCCEEDED(rc))
        rc = _vi2WindowCloseDisplay(&entry->vi2);
    if (R_FAILED(rc))
        return rc;

    mutexLock(&g_displayWindowsMutex);
    entry->nw = NULL;
    mutexUnlock(&g_displayWindowsMutex);
    return 0;
}
