#include <string.h>
#include "types.h"
#include "result.h"
#include "services/vi.h"
#include "runtime/diag.h"
#include "display/binder.h"
#include "display/buffer_producer.h"
#include "display/native_window.h"
#include "nvidia/graphic_buffer.h"
#if defined(__SWITCH2__)
#include "services/applet.h"
#include "../switch2/display/cpu_window.h"
#include "../switch2/display/vi2_window.h"
#include "../switch2/display/window_policy.h"
#endif

#if defined(__SWITCH2__)
// The default window's VI2 layer, opened on first use.
static Vi2Window g_vi2Window;
#else
static ViDisplay g_viDisplay;
static ViLayer g_viLayer;
#endif
static NWindow g_defaultWin;

NWindow* nwindowGetDefault(void)
{
    return &g_defaultWin;
}

#if defined(__SWITCH2__)
// A game gets VI2 through GSV (switch2/services/vi2.h). A game's
// service policy does not allow vi:u, vi:s or vi:m. VI2 has no binder
// buffer queue.
// The default window owns the process's VI2 layer (nwindowGetVi2Layer). Its
// producers present to that layer: the framebuffer
// (switch2/display/cpu_window.c) or a Vulkan swapchain (NXVK's VI surface on
// this window). The layer opens on first use, not here. A program that
// never presents thus opens no layer. The window itself is valid for its
// state, so window setup (nwindowSetDimensions) succeeds as on Switch 1.
void __nx_win_init(void)
{
    _nwindowCreateForCpuWindow(&g_defaultWin);
    nwindowSetDimensions(&g_defaultWin, 1280, 720);
}

// The default window's size per operation mode and frame rate
// per refresh rate.
static struct {
    Mutex mutex;

    bool mode_dimensions;
    u32 handheld_width, handheld_height;
    u32 docked_width, docked_height;
    bool resolution_event_open;
    Event resolution_event;
    s32 default_width, default_height;

    bool frame_rates;
    u32 fps_at_120hz, fps_at_60hz;
    bool vrr_declined;  // nwindowSetVrr(false). The window requests VRR by default.
    // The capabilities sent to VI2 for the window's current display.
    bool sent_120hz, sent_vrr;
    bool refresh_event_open;
    Vi2DisplayEvent refresh_event;
    u32 refresh_hz;
} g_windowPolicy;

static void _nwindowApplyRefreshCapabilities(void);

// Rereads the default resolution after AM's change event. On docking, AM
// sends the operation-mode message before the new value is readable. The
// event comes when the value is readable. Call with g_windowPolicy.mutex held.
static void _nwindowUpdateDefaultResolution(void)
{
    if (!g_windowPolicy.resolution_event_open ||
        R_FAILED(eventWait(&g_windowPolicy.resolution_event, 0)))
        return;
    s32 width = 0, height = 0;
    if (R_SUCCEEDED(appletGetDefaultDisplayResolution(&width, &height))) {
        g_windowPolicy.default_width = width;
        g_windowPolicy.default_height = height;
    }
}

void _nwindowApplyModeDimensions(NWindow* nw)
{
    if (nw != &g_defaultWin)
        return;
    mutexLock(&g_windowPolicy.mutex);
    if (g_windowPolicy.mode_dimensions) {
        _nwindowUpdateDefaultResolution();
        const bool docked = appletGetOperationMode() == AppletOperationMode_Console;
        u32 width = docked ? g_windowPolicy.docked_width : g_windowPolicy.handheld_width;
        u32 height = docked ? g_windowPolicy.docked_height : g_windowPolicy.handheld_height;
        if (!width) {
            width = g_windowPolicy.default_width;
            height = g_windowPolicy.default_height;
        }
        nw->width = width;
        nw->height = height;
    }
    mutexUnlock(&g_windowPolicy.mutex);
}

void _nwindowClearModeDimensions(NWindow* nw)
{
    if (nw != &g_defaultWin)
        return;
    mutexLock(&g_windowPolicy.mutex);
    g_windowPolicy.mode_dimensions = false;
    mutexUnlock(&g_windowPolicy.mutex);
}

void _nwindowClearFrameRates(NWindow* nw)
{
    if (nw != &g_defaultWin)
        return;
    mutexLock(&g_windowPolicy.mutex);
    g_windowPolicy.frame_rates = false;
    _nwindowApplyRefreshCapabilities();
    mutexUnlock(&g_windowPolicy.mutex);
}

// The display's nominal refresh rate, reread on VI2's refresh-rate event.
// VI2 signals that event once at registration, so the first value is read
// then. The rate is 60 Hz while the window has no layer. Call with
// g_windowPolicy.mutex held.
static u32 _nwindowRefreshRate(void)
{
    if (!g_vi2Window.display_open)
        return 60;
    if (!g_windowPolicy.refresh_event_open) {
        g_windowPolicy.refresh_event_open = R_SUCCEEDED(vi2CreateDisplayEvent(
            &g_windowPolicy.refresh_event, &g_vi2Window.display,
            Vi2DisplayEventType_RefreshRate, true));
    }
    if (g_windowPolicy.refresh_event_open &&
        R_SUCCEEDED(eventWait(&g_windowPolicy.refresh_event.event, 0))) {
        u32 hz = 0;
        if (R_SUCCEEDED(vi2QueryDisplayRefreshRate(&g_vi2Window.display, &hz, NULL)))
            g_windowPolicy.refresh_hz = hz;
    }
    return g_windowPolicy.refresh_hz ? g_windowPolicy.refresh_hz : 60;
}

// Asks for 120 Hz when a 60 Hz display cannot present the window's 120 Hz
// frame rate. A rate that divides 60 evenly (30, 20, ...) keeps the display
// at 60 Hz, which uses less power. Call with g_windowPolicy.mutex held.
static bool _nwindowWants120Hz(void)
{
    if (!g_windowPolicy.frame_rates)
        return false;
    const u32 fps = g_windowPolicy.fps_at_120hz;
    return !fps || fps > 60 || 60 % fps;
}

// Sends the window's 120 Hz and VRR requests to VI2 when they change. VI2
// takes them for each registered display, so this function sends them only
// after the window's display exists. Call with g_windowPolicy.mutex held.
static void _nwindowApplyRefreshCapabilities(void)
{
    if (!g_vi2Window.display_open)
        return;
    const bool want_120hz = _nwindowWants120Hz();
    if (want_120hz != g_windowPolicy.sent_120hz &&
        R_SUCCEEDED(vi2SetDisplay120HzCapability(&g_vi2Window.display, want_120hz)))
        g_windowPolicy.sent_120hz = want_120hz;
    const bool want_vrr = !g_windowPolicy.vrr_declined;
    if (want_vrr != g_windowPolicy.sent_vrr &&
        R_SUCCEEDED(vi2SetDisplayVrrCapability(&g_vi2Window.display, want_vrr)))
        g_windowPolicy.sent_vrr = want_vrr;
}

// Withdraws the window's 120 Hz and VRR requests and destroys its
// refresh-rate event before the window's display closes. VI2 accepts these
// requests only while the display is registered.
static Result _nwindowCloseRefreshEvent(void)
{
    Result rc = 0;
    mutexLock(&g_windowPolicy.mutex);
    if (g_windowPolicy.sent_120hz)
        vi2SetDisplay120HzCapability(&g_vi2Window.display, false);
    if (g_windowPolicy.sent_vrr)
        vi2SetDisplayVrrCapability(&g_vi2Window.display, false);
    g_windowPolicy.sent_120hz = g_windowPolicy.sent_vrr = false;
    if (g_windowPolicy.refresh_event_open) {
        rc = vi2DestroyDisplayEvent(&g_windowPolicy.refresh_event);
        if (R_SUCCEEDED(rc))
            g_windowPolicy.refresh_event_open = false;
    }
    mutexUnlock(&g_windowPolicy.mutex);
    return rc;
}

// Layer teardown: detach, clear the endpoint's selection, close the layer,
// then the endpoint, display and VI2. Stops at the first failure, leaving
// the rest open.
static Result _nwindowCloseVi2Layer(void)
{
    Result rc = _vi2WindowCloseLayer(&g_vi2Window);
    if (R_SUCCEEDED(rc) && g_vi2Window.display_open)
        rc = _nwindowCloseRefreshEvent();
    if (R_SUCCEEDED(rc))
        rc = _vi2WindowCloseDisplay(&g_vi2Window);
    return rc;
}

Result nwindowGetVi2Layer(NWindow* nw, Vi2Layer** out_layer)
{
    if (!out_layer)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    // A window of nwindowCreateForDisplay has its layer from its creation.
    Vi2Window* display_window = _nwindowGetDisplayWindow(nw);
    if (display_window) {
        *out_layer = &display_window->layer;
        return 0;
    }
    if (nw != &g_defaultWin)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    Result rc = 0;
    if (!g_vi2Window.layer_selected)
        rc = _vi2WindowOpen(&g_vi2Window, NULL);
    if (R_SUCCEEDED(rc))
        *out_layer = &g_vi2Window.layer;
    return rc;
}

Result nwindowGetVi2Display(NWindow* nw, Vi2Display** out_display)
{
    if (!out_display)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    Vi2Layer* layer;
    Result rc = nwindowGetVi2Layer(nw, &layer);
    if (R_SUCCEEDED(rc)) {
        Vi2Window* display_window = _nwindowGetDisplayWindow(nw);
        *out_display = display_window ? &display_window->display : &g_vi2Window.display;
    }
    return rc;
}

Result nwindowSetModeDimensions(NWindow* nw, u32 handheld_width, u32 handheld_height,
                                u32 docked_width, u32 docked_height)
{
    if (nw != &g_defaultWin || !handheld_width != !handheld_height ||
        !docked_width != !docked_height)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    Result rc = 0;
    mutexLock(&g_windowPolicy.mutex);
    const bool follows_default = !handheld_width || !docked_width;
    if (follows_default && !g_windowPolicy.resolution_event_open) {
        rc = appletGetDefaultDisplayResolutionChangeEvent(&g_windowPolicy.resolution_event);
        if (R_SUCCEEDED(rc)) {
            g_windowPolicy.resolution_event_open = true;
            eventClear(&g_windowPolicy.resolution_event);
            rc = appletGetDefaultDisplayResolution(&g_windowPolicy.default_width,
                                                   &g_windowPolicy.default_height);
        }
    }
    if (R_SUCCEEDED(rc)) {
        g_windowPolicy.handheld_width = handheld_width;
        g_windowPolicy.handheld_height = handheld_height;
        g_windowPolicy.docked_width = docked_width;
        g_windowPolicy.docked_height = docked_height;
        g_windowPolicy.mode_dimensions = true;
    }
    mutexUnlock(&g_windowPolicy.mutex);

    if (R_SUCCEEDED(rc))
        _nwindowApplyModeDimensions(nw);
    return rc;
}

Result nwindowSetFrameRates(NWindow* nw, u32 fps_at_120hz, u32 fps_at_60hz, bool vrr)
{
    if (nw != &g_defaultWin)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    mutexLock(&g_windowPolicy.mutex);
    g_windowPolicy.fps_at_120hz = fps_at_120hz;
    g_windowPolicy.fps_at_60hz = fps_at_60hz;
    g_windowPolicy.frame_rates = true;
    g_windowPolicy.vrr_declined = !vrr;
    _nwindowApplyRefreshCapabilities();
    mutexUnlock(&g_windowPolicy.mutex);
    return 0;
}

Result nwindowSetVrr(NWindow* nw, bool enable)
{
    if (nw != &g_defaultWin)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    mutexLock(&g_windowPolicy.mutex);
    g_windowPolicy.vrr_declined = !enable;
    _nwindowApplyRefreshCapabilities();
    mutexUnlock(&g_windowPolicy.mutex);
    return 0;
}

// VI2 presents an image for numerator/denominator seconds (a rational
// interval). Frame rates follow the current refresh rate. A swap interval
// keeps the basic interval, N/60 s, at every refresh rate. The reason: the
// window requests VRR by default, and VRR can make VI2 select 120 Hz. That
// must not double the rate of an application that did not ask for 120 Hz.
Result nwindowGetPresentInterval(NWindow* nw, u16* out_numerator, u16* out_denominator)
{
    if (!out_numerator || !out_denominator)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    // A window of nwindowCreateForDisplay paces with its swap interval.
    if (_nwindowGetDisplayWindow(nw)) {
        *out_numerator = (u16)nw->swap_interval;
        *out_denominator = 60;
        return 0;
    }
    if (nw != &g_defaultWin)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    mutexLock(&g_windowPolicy.mutex);
    _nwindowApplyRefreshCapabilities();
    const u32 refresh_hz = _nwindowRefreshRate();
    if (g_windowPolicy.frame_rates) {
        u32 fps = refresh_hz >= 120 ? g_windowPolicy.fps_at_120hz : g_windowPolicy.fps_at_60hz;
        if (!fps || fps > refresh_hz)
            fps = refresh_hz;
        *out_numerator = 1;
        *out_denominator = (u16)fps;
    } else {
        *out_numerator = (u16)nw->swap_interval;
        *out_denominator = 60;
    }
    mutexUnlock(&g_windowPolicy.mutex);
    return 0;
}

// The layer and an idle framebuffer window hold the process's GSV root. Each
// root delegates the nvservices session only once: a second
// gsvOpenNvServices on it returned 0x200f3. A GPU user that shuts down and
// opens the GPU again needs a new root, so it releases the layer here first.
Result nwindowReleaseVi2Layer(NWindow* nw)
{
    // A window of nwindowCreateForDisplay keeps its layer until nwindowClose.
    if (nw != &g_defaultWin)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    if (cpuwinDefaultHasImages())
        return MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized);
    if (!cpuwinCloseDefault())
        return MAKERESULT(Module_Libnx, LibnxError_BadGfxQueueBuffer);
    return _nwindowCloseVi2Layer();
}

// Retires a framebuffer that is still open first. If its images or the layer
// cannot be retired, the layer stays open. A producer may still present to it.
void __nx_win_exit(void)
{
    if (!cpuwinCloseDefault())
        return;
    _nwindowCloseVi2Layer();
}
#else
void __nx_win_init(void)
{
    Result rc;
    rc = viInitialize(ViServiceType_Default);
    if (R_SUCCEEDED(rc)) {
        rc = viOpenDefaultDisplay(&g_viDisplay);
        if (R_SUCCEEDED(rc)) {
            rc = viCreateLayer(&g_viDisplay, &g_viLayer);
            if (R_SUCCEEDED(rc)) {
                rc = viSetLayerScalingMode(&g_viLayer, ViScalingMode_FitToLayer);
                if (R_SUCCEEDED(rc)) {
                    rc = nwindowCreateFromLayer(&g_defaultWin, &g_viLayer);
                    if (R_SUCCEEDED(rc))
                        nwindowSetDimensions(&g_defaultWin, 1280, 720);
                }
                if (R_FAILED(rc))
                    viCloseLayer(&g_viLayer);
            }
            if (R_FAILED(rc))
                viCloseDisplay(&g_viDisplay);
        }
        if (R_FAILED(rc))
            viExit();
    }
    if (R_FAILED(rc))
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_BadGfxInit));
}

void __nx_win_exit(void)
{
    nwindowClose(&g_defaultWin);
    viCloseLayer(&g_viLayer);
    viCloseDisplay(&g_viDisplay);
    viExit();
}
#endif
