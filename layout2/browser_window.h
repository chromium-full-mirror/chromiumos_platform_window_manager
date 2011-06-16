// Copyright (c) 2011 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_LAYOUT2_BROWSER_WINDOW_H_
#define WINDOW_MANAGER_LAYOUT2_BROWSER_WINDOW_H_

#include "base/basictypes.h"
#include "base/scoped_ptr.h"
#include "window_manager/compositor/compositor.h"
#include "window_manager/focus_manager.h"
#include "window_manager/stacking_manager.h"
#include "window_manager/x11/x_types.h"

namespace window_manager {

class EventConsumerRegistrar;
class LayoutManager2;
class Rect;
class TransientWindowCollection;
class Window;
class WindowManager;

// Wraps a Window object that represents one of Chrome's browser windows.
// (Note that this may not necessarily be a browser; terminal windows are also
// tracked using this class -- more precisely, this is a big window that gets
// managed by LayoutManager2.)
class BrowserWindow {
 public:
  // Specifies whether this window is anchored to the left or right edge of the
  // screen when unmaximized.
  enum AnchorPosition {
    ANCHOR_LEFT = 0,
    ANCHOR_RIGHT,
  };

  BrowserWindow(Window* win,
                LayoutManager2* layout_manager,
                int unmaximized_width);
  ~BrowserWindow();

  // Should the passed-in window be handled by this class?
  static bool ShouldHandleWindow(const Window& win);

  WindowManager* wm() const;
  Window* win() const { return win_; }
  int unmaximized_width() const { return unmaximized_width_; }
  void set_unmaximized_width(int width) { unmaximized_width_ = width; }
  bool anchored_to_left() const {
    return unmaximized_anchoring_ == ANCHOR_LEFT;
  }
  void set_unmaximized_anchoring(AnchorPosition anchoring) {
    unmaximized_anchoring_ = anchoring;
  }
  double brightness() const { return brightness_; }

  // Move and resize the window to |bounds| over |anim_ms| milliseconds.
  void SetBounds(const Rect& bounds, int anim_ms);

  // Stack the window at the top of |layer|.
  void StackAtTopOfLayer(StackingManager::Layer layer,
                         StackingManager::ShadowPolicy shadow_policy,
                         StackingManager::Layer shadow_layer);

  // Stack the window directly beneath |other_browser|.
  void StackBelowOtherBrowserWindow(BrowserWindow* other_browser,
                                    StackingManager::ShadowPolicy shadow_policy,
                                    StackingManager::Layer shadow_layer);

  // Assign the input focus to |win_| or one of its transient windows.
  void TakeFocus(XTime timestamp);

  // Handle transient windows belonging (per Window::transient_for_xid()) to
  // this browser window being mapped, unmapped, or reconfigured.
  void HandleTransientWindowMap(Window* transient_win);
  void HandleTransientWindowUnmap(Window* transient_win);
  void HandleTransientWindowConfigureRequest(Window* transient_win,
                                             const Rect& requested_bounds);

  // Does |win_| or one of the entries in |transients_| have the input focus?
  bool IsWindowOrTransientFocused() const;

  // Specify that a previously-registered transient window should get the input
  // focus the next time that TakeFocus() is called.  NULL can be passed to make
  // the browser window itself get the focus.
  void SetPreferredTransientWindowToFocus(Window* transient_win);

  // Show or hide all of this browser's transient windows.
  void SetTransientWindowVisibility(bool shown);

  // Display an animation where the window tries to slide offscreen (to the left
  // if |move_to_left| is true or to the right otherwise) but then bounces back.
  // Used when the user tries to cycle windows while only one window is present.
  void DoNudgeAnimation(bool move_to_left);

  // Set a window's brightness.  |brightness|'s range is [0.0, 1.0].  At 1, the
  // window is displayed unmodified; at 0, it's completely black.
  void SetBrightness(double brightness, int anim_ms);

  // Show or hide this window's status area.
  void SetStatusAreaDisplayed(bool displayed);

  // Add or remove atoms from the window's _NET_WM_STATE property that are used
  // to let Chrome know about the window's current state.  Calling this is a
  // no-op if the property already contains the desired state.
  void SetFullscreenProperty(bool fullscreen);
  void SetMaximizedProperty(bool maximized);

 private:
  Window* win_;  // not owned

  scoped_ptr<EventConsumerRegistrar> event_consumer_registrar_;

  // Width of the browser window when unmaximized, in pixels.
  int unmaximized_width_;

  // Which side of the screen are we anchored to when unmaximized?
  AnchorPosition unmaximized_anchoring_;

  // Is this window currently displaying its status area?
  bool status_area_displayed_;

  // Most-recently-set brightness for the window.
  double brightness_;

  // Transient windows belonging to this window.
  scoped_ptr<TransientWindowCollection> transients_;

  // Actor displayed above the window to control its brightness.
  scoped_ptr<Compositor::ColoredBoxActor> dimming_box_;

  DISALLOW_COPY_AND_ASSIGN(BrowserWindow);
};

}  // namespace window_manager

#endif  // WINDOW_MANAGER_LAYOUT2_BROWSER_WINDOW_H_
