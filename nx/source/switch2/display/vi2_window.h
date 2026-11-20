/* SPDX-License-Identifier: ISC
 * A window's VI2 layer: the display it is on, the display's endpoint and the
 * layer, attached and selected. The default window (default_window.c) and the
 * windows of nwindowCreateForDisplay each have one.
 * @author kurumuz
 */
#pragma once
#include "types.h"
#include "result.h"
#include "display/native_window.h"

typedef struct {
    bool vi2_initialized, display_open, layer_created, layer_attached, layer_selected;
    Vi2Display display;
    Vi2Endpoint endpoint;
    Vi2Layer layer;
} Vi2Window;

/// Opens what is not open yet: VI2, the display (NULL for the default display),
/// its endpoint "default" and the layer, attached and selected.
Result _vi2WindowOpen(Vi2Window* w, const char* display_name);

/// Detaches the layer, clears the endpoint's selection and closes the layer.
/// Stops at the first failure, leaving the rest open.
Result _vi2WindowCloseLayer(Vi2Window* w);

/// Closes the endpoint, the display and VI2, after \ref _vi2WindowCloseLayer.
Result _vi2WindowCloseDisplay(Vi2Window* w);

/// The VI2 state of a window that \ref nwindowCreateForDisplay made, or NULL.
Vi2Window* _nwindowGetDisplayWindow(NWindow* nw);

/// Closes the layer and display of a window that \ref nwindowCreateForDisplay
/// made and releases the window's VI2 state. A window of another kind succeeds.
Result _nwindowCloseDisplayWindow(NWindow* nw);
