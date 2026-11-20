/**
 * @file native_window.h
 * @brief Native window (NWindow) wrapper object, used for presenting images to the display (or other sinks).
 * @author fincs
 * @author kurumuz
 * @copyright libnx Authors
 */
#pragma once
#include "../kernel/mutex.h"
#include "../kernel/event.h"
#include "../services/vi.h"
#include "../nvidia/graphic_buffer.h"
#include "types.h"
#include "binder.h"
#include "buffer_producer.h"

/// Native window structure.
typedef struct NWindow {
    u32 magic;
    Binder bq;
    Event event;
    Mutex mutex;
    u64 slots_configured;
    u64 slots_requested;
    s32 cur_slot;
    u32 width;
    u32 height;
    u32 format;
    u32 usage;
    BqRect crop;
    u32 scaling_mode;
    u32 transform;
    u32 sticky_transform;
    u32 default_width;
    u32 default_height;
    u32 swap_interval;
    bool is_connected;
    bool producer_controlled_by_app;
    bool consumer_running_behind;
} NWindow;

///@name Basic functions
///@{

/// Checks whether a pointer refers to a valid \ref NWindow object.
bool nwindowIsValid(NWindow* nw);

/**
 * @brief Retrieves the default \ref NWindow object.
 * @return Pointer to the default \ref NWindow object.
 * @note When this function is used/referenced, libnx will initialize VI services
 *       and create a \ref NWindow object from a \ref ViLayer created on the default \ref ViDisplay;
 *       all of this happening automatically during application startup (i.e. before main is called).
 *       If creating the default \ref NWindow fails, libnx will throw a LibnxError_BadGfxInit fatal error.
 *       Likewise, after main returns (or exit is called) libnx will clean up all resources used by it.
 * @note [Switch 2] The default window has no binder buffer queue, because VI2 has none.
 *       It holds its dimensions, crop and format, and \ref framebufferCreate can use it.
 *       Its display layer opens on the first \ref framebufferCreate. Calls that use the
 *       queue (dequeue, queue, configure, release) fail.
 */
NWindow* nwindowGetDefault(void);

#if defined(__SWITCH2__)
#include "../../switch2/services/vi2.h"

/**
 * @brief Creates a window whose VI2 layer is on a named display.
 * @param[out] nw Output \ref NWindow structure.
 * @param[in] display_name Name of the display, for example "player1": the display of
 *            the first guest of a GameShare session with separate views.
 * @note The window is like the default window: it has no buffer queue, and a producer
 *       presents its own images to the window's layer (\ref nwindowGetVi2Layer). A
 *       Vulkan swapchain can be created on it. \ref framebufferCreate cannot use it.
 * @note Its size is 1280x720 until \ref nwindowSetDimensions, and it paces with its swap
 *       interval. \ref nwindowClose closes its layer and display; stop presenting to it first.
 * @note A process can have four such windows.
 */
Result nwindowCreateForDisplay(NWindow* nw, const char* display_name);

/**
 * @brief Gets the VI2 layer that presents a window's images, opening it on first use.
 * @param[in] nw Window: the default window (\ref nwindowGetDefault) or a window of
 *            \ref nwindowCreateForDisplay. Other windows have no layer.
 * @param[out] out_layer The window's layer, attached to the window's display and selected
 *             on its endpoint "default". The window owns it. The default window closes
 *             it when the application exits.
 * @note One producer presents to the layer at a time: the window's framebuffer or a
 *       Vulkan swapchain. Before another producer presents, the previous one unregisters
 *       the layer's image (a present without the image flag) and waits for its releases.
 */
Result nwindowGetVi2Layer(NWindow* nw, Vi2Layer** out_layer);

/**
 * @brief Gets the VI2 display that a window's layer is attached to, opening the layer on first use.
 * @param[in] nw Window: the default window (\ref nwindowGetDefault) or a window of
 *            \ref nwindowCreateForDisplay.
 * @param[out] out_display The window's display, registered in the process's VI2
 *             session. Display events and queries (\ref vi2CreateDisplayEvent) need it.
 * @note Destroy display events created on it before \ref nwindowReleaseVi2Layer closes it.
 */
Result nwindowGetVi2Display(NWindow* nw, Vi2Display** out_display);

/**
 * @brief Sets a window's size separately for handheld and docked mode.
 * @param[in] nw Window. Only the default window (\ref nwindowGetDefault).
 * @param[in] handheld_width,handheld_height Size in handheld mode.
 * @param[in] docked_width,docked_height Size in docked mode.
 * @note A 0x0 size uses the console's default resolution for that mode:
 *       1920x1080 handheld, and the TV output setting docked (3840x2160 at 4K).
 *       \ref nwindowGetDimensions then returns the size for the current mode. It
 *       changes when \ref appletMainLoop handles the operation-mode message and,
 *       for 0x0, when AM signals a default-resolution change. A Vulkan swapchain
 *       on the window returns VK_SUBOPTIMAL_KHR once its size no longer matches.
 *       \ref nwindowSetDimensions sets one fixed size again.
 */
Result nwindowSetModeDimensions(NWindow* nw, u32 handheld_width, u32 handheld_height,
                                u32 docked_width, u32 docked_height);

/**
 * @brief Sets a window's frame rate for each display refresh rate.
 * @param[in] nw Window. Only the default window (\ref nwindowGetDefault).
 * @param[in] fps_at_120hz Frames per second while the display runs at 120 Hz.
 * @param[in] fps_at_60hz Frames per second while it runs at 60 Hz.
 * @param[in] vrr Use variable refresh rate where it is available, as
 *            \ref nwindowSetVrr. Pass true unless the application must not run with VRR.
 * @note A rate of 0, or a rate above the refresh rate, presents every refresh.
 *       Rates that divide the refresh rate (40 or 30 at 120 Hz, 30 or 20 at
 *       60 Hz) pace evenly. With VRR, other rates (45 at 120 Hz) can also pace
 *       evenly.
 * @note The window asks VI2 for 120 Hz when a 60 Hz display cannot present
 *       fps_at_120hz. This applies to 0, to rates above 60, and to rates that
 *       do not divide 60, such as 40. The display changes to 120 Hz where the
 *       system and the display allow it (the handheld screen).
 * @note Examples: (40, 30, true) gives 40 fps at 120 Hz and 30 fps otherwise.
 *       (0, 0, true) presents every refresh at the highest available rate.
 *       \ref nwindowSetSwapInterval presents every N/60 s again and withdraws
 *       the 120 Hz request.
 */
Result nwindowSetFrameRates(NWindow* nw, u32 fps_at_120hz, u32 fps_at_60hz, bool vrr);

/**
 * @brief Allows or declines variable refresh rate (VRR) for a window.
 * @param[in] nw Window. Only the default window (\ref nwindowGetDefault).
 * @note VRR is allowed by default. The window declares it to VI2. VI2 enables
 *       it where the system permits it and the display supports it (the
 *       handheld screen, not a 60 Hz TV). \ref nwindowSetFrameRates also sets
 *       it. Use this call when the application paces with a swap interval.
 */
Result nwindowSetVrr(NWindow* nw, bool enable);

/**
 * @brief Gets the interval to present a window's images with.
 * @param[in] nw Window: the default window (\ref nwindowGetDefault) or a window of
 *            \ref nwindowCreateForDisplay, which always uses its swap interval.
 * @param[out] out_numerator,out_denominator The interval in seconds, as in \ref Vi2PresentOptions.
 * @note With frame rates (\ref nwindowSetFrameRates), the interval is the rate
 *       for the display's current refresh rate. That refresh rate follows
 *       VI2's refresh-rate event after the window's layer opens, and is 60 Hz
 *       before. Without frame rates, the interval is the swap interval / 60 s,
 *       at every refresh rate.
 */
Result nwindowGetPresentInterval(NWindow* nw, u16* out_numerator, u16* out_denominator);

/**
 * @brief Closes the default window's VI2 layer once no producer presents to it.
 * @param[in] nw Window. Only the default window (\ref nwindowGetDefault): a window of
 *            \ref nwindowCreateForDisplay keeps its layer until \ref nwindowClose.
 * @return LibnxError_AlreadyInitialized while a framebuffer exists on the window.
 * @note The layer holds the process's GSV root. That root delegates the GPU's
 *       nvservices session only once. To open the GPU again after a shutdown
 *       (for example, a Vulkan driver's shutdown), release the layer first. The
 *       root then closes, and the next open gets a new root. The next
 *       \ref nwindowGetVi2Layer opens a new layer.
 */
Result nwindowReleaseVi2Layer(NWindow* nw);
#endif

/**
 * @brief Creates a \ref NWindow.
 * @param[out] nw Output \ref NWindow structure.
 * @param[in] binder_session Service object for the Android IGraphicBufferProducer binder session.
 * @param[in] binder_id Android IGraphicBufferProducer binder session ID.
 * @param[in] producer_controlled_by_app Specifies whether the producer is controlled by the application.
 */
Result nwindowCreate(NWindow* nw, Service* binder_session, s32 binder_id, bool producer_controlled_by_app);

/**
 * @brief Creates a \ref NWindow operating on a \ref ViLayer.
 * @param[out] nw Output \ref NWindow structure.
 * @param[in] layer Pointer to \ref ViLayer structure (such as the one returned by \ref viCreateLayer).
 */
Result nwindowCreateFromLayer(NWindow* nw, const ViLayer* layer);

/// Closes a \ref NWindow, freeing all resources associated with it.
void nwindowClose(NWindow* nw);

///@}

///@name Window configuration
///@{

/**
 * @brief Retrieves the dimensions of a \ref NWindow.
 * @param[in] nw Pointer to \ref NWindow structure.
 * @param[out] out_width Output variable containing the width of the \ref NWindow.
 * @param[out] out_height Output variable containing the height of the \ref NWindow.
 * @note After creation, a \ref NWindow reports a default size (usually 1280x720).
 *       This size can be overriden by calling \ref nwindowSetDimensions.
 */
Result nwindowGetDimensions(NWindow* nw, u32* out_width, u32* out_height);

/**
 * @brief Sets the dimensions of a \ref NWindow.
 * @param[in] nw Pointer to \ref NWindow structure.
 * @param[in] width Desired width of the \ref NWindow.
 * @param[in] height Desired width of the \ref NWindow.
 * @note This function cannot be called when there are buffers registered with the \ref NWindow.
 */
Result nwindowSetDimensions(NWindow* nw, u32 width, u32 height);

/**
 * @brief Configures the crop applied to images presented through a \ref NWindow.
 * @param[in] nw Pointer to \ref NWindow structure.
 * @param[in] left X coordinate of the left margin of the crop bounding box.
 * @param[in] top Y coordinate of the top margin of the crop bounding box.
 * @param[in] right X coordinate of the right margin of the crop bounding box.
 * @param[in] bottom Y coordinate of the bottom margin of the crop bounding box.
 * @note Passing zero to all parameters disables the crop functionality. This is also the default.
 * @note The bounding box defined by the parameters will be adjusted to fit within the dimensions of the \ref NWindow.
 * @note \p left must be less or equal than \p right.
 * @note \p top must be less or equal than \p bottom.
 */
Result nwindowSetCrop(NWindow* nw, s32 left, s32 top, s32 right, s32 bottom);

/**
 * @brief Configures the transformation applied to images presented through a \ref NWindow.
 * @param[in] nw Pointer to \ref NWindow structure.
 * @param[in] transform Android transformation mode (see NATIVE_WINDOW_TRANSFORM_* enum)
 * @note The default transformation is 0 (i.e. no transformation applied)
 */
Result nwindowSetTransform(NWindow* nw, u32 transform);

/**
 * @brief Configures the swap interval of a \ref NWindow.
 * @param[in] nw Pointer to \ref NWindow structure.
 * @param[in] swap_interval Value specifying the number of display refreshes (VBlanks) that must occur between presenting images.
 * @note The default swap interval is 1.
 * @note If the \ref NWindow has three or more buffers configured (with \ref nwindowConfigureBuffer), it is possible to pass 0
 *       to disable the swap interval feature and present images as fast as allowed by the compositor. Otherwise, the system
 *       enforces a minimum of 1 as the swap interval.
 */
Result nwindowSetSwapInterval(NWindow* nw, u32 swap_interval);

/// Checks whether the consumer of a \ref NWindow is running behind.
static inline bool nwindowIsConsumerRunningBehind(NWindow* nw)
{
    return nw->consumer_running_behind;
}

///@}

///@name Buffer configuration and presentation
///@{

/**
 * @brief Registers a \ref NvGraphicBuffer with a \ref NWindow.
 * @param[in] nw Pointer to \ref NWindow structure.
 * @param[in] slot ID of the slot to configure (starting from 0).
 * @param[in] buf Pointer to \ref NvGraphicBuffer structure.
 * @note When a buffer is registered, it is added to the internal queue of buffers used for presenting.
 * @note All buffers registered with a \ref NWindow must have the same dimensions, format and usage.
 *       If \ref nwindowSetDimensions has not been previously called, the \ref NWindow will automatically
 *       adopt the dimensions of the first buffer registered with it. Otherwise, said buffer will need
 *       to match the dimensions that were previously configured.
 */
Result nwindowConfigureBuffer(NWindow* nw, s32 slot, NvGraphicBuffer* buf);

/**
 * @brief Dequeues a buffer from a \ref NWindow.
 * @param[in] nw Pointer to \ref NWindow structure.
 * @param[out] out_slot Output variable containing the ID of the slot that has been dequeued.
 * @param[out] out_fence Output variable containing a \ref NvMultiFence that will be signalled by
 *             the compositor when the buffer is ready to be written to. Pass NULL to wait instead
 *             on this fence before this function returns.
 * @note For \p out_fence=NULL to work, \ref nvFenceInit must have been previously called.
 */
Result nwindowDequeueBuffer(NWindow* nw, s32* out_slot, NvMultiFence* out_fence);

/**
 * @brief Cancels a buffer previously dequeued with \ref nwindowDequeueBuffer.
 * @param[in] nw Pointer to \ref NWindow structure.
 * @param[in] slot ID of the slot to cancel. This must match the output of the previous \ref nwindowDequeueBuffer call.
 * @param[in] fence Pointer to the \ref NvMultiFence that will be waited on by the compositor before cancelling the buffer.
 *                  Pass NULL if there is no such fence.
 */
Result nwindowCancelBuffer(NWindow* nw, s32 slot, const NvMultiFence* fence);

/**
 * @brief Queues a buffer previously dequeued with \ref nwindowDequeueBuffer, making it ready for presentation.
 * @param[in] nw Pointer to \ref NWindow structure.
 * @param[in] slot ID of the slot to queue. This must match the output of the previous \ref nwindowDequeueBuffer call.
 * @param[in] fence Pointer to the \ref NvMultiFence that will be waited on by the compositor before queuing/presenting the buffer.
 *                  Pass NULL if there is no such fence.
 */
Result nwindowQueueBuffer(NWindow* nw, s32 slot, const NvMultiFence* fence);

/// Releases all buffers registered with a \ref NWindow.
Result nwindowReleaseBuffers(NWindow* nw);

///@}
