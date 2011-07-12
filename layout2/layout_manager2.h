// Copyright (c) 2011 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_LAYOUT2_LAYOUT_MANAGER2_H_
#define WINDOW_MANAGER_LAYOUT2_LAYOUT_MANAGER2_H_

#include <gtest/gtest_prod.h>  // for FRIEND_TEST() macro

#include <map>
#include <set>
#include <tr1/memory>
#include <vector>

#include "base/basictypes.h"
#include "base/scoped_ptr.h"
#include "cros/chromeos_wm_ipc_enums.h"
#include "window_manager/compositor/compositor.h"
#include "window_manager/event_consumer.h"
#include "window_manager/focus_manager.h"
#include "window_manager/geometry.h"
#include "window_manager/modality_handler.h"
#include "window_manager/panels/panel_manager.h"
#include "window_manager/wm_ipc.h"
#include "window_manager/x11/x_types.h"

namespace window_manager {

class BrowserWindow;
class EventConsumerRegistrar;
class KeyBindingsActionRegistrar;
class KeyBindingsGroup;
class ResizeBox;
class Window;
class WindowManager;

class LayoutManager2 : public EventConsumer,
                       public FocusChangeListener,
                       public ModalityChangeListener,
                       public PanelManagerAreaChangeListener {
 public:
  explicit LayoutManager2(WindowManager* wm);
  virtual ~LayoutManager2();

  chromeos::WmIpcLayoutMode layout_mode() const { return layout_mode_; }
  bool maximized() const {
    return layout_mode_ == chromeos::WM_IPC_LAYOUT_MAXIMIZED;
  }
  int num_browser_windows() const { return browser_windows_.size(); }

  // EventConsumer implementation.
  virtual bool IsInputWindow(XWindow xid) {
    return xid == resize_handle_xid_ ||
           xid == left_edge_input_xid_ ||
           xid == right_edge_input_xid_;
  }
  virtual void HandleScreenResize();
  virtual void HandleLoggedInStateChange() {}
  virtual bool HandleWindowMapRequest(Window* win);
  virtual void HandleWindowMap(Window* win);
  virtual void HandleWindowUnmap(Window* win);
  virtual void HandleWindowPixmapFetch(Window* win);
  virtual void HandleWindowConfigureRequest(Window* win,
                                            const Rect& requested_bounds);
  virtual void HandleButtonPress(XWindow xid,
                                 const Point& relative_pos,
                                 const Point& absolute_pos,
                                 int button,
                                 XTime timestamp);
  virtual void HandleButtonRelease(XWindow xid,
                                   const Point& relative_pos,
                                   const Point& absolute_pos,
                                   int button,
                                   XTime timestamp);
  virtual void HandlePointerEnter(XWindow xid,
                                  const Point& relative_pos,
                                  const Point& absolute_pos,
                                  XTime timestamp) {}
  virtual void HandlePointerLeave(XWindow xid,
                                  const Point& relative_pos,
                                  const Point& absolute_pos,
                                  XTime timestamp) {}
  virtual void HandlePointerMotion(XWindow xid,
                                   const Point& relative_pos,
                                   const Point& absolute_pos,
                                   XTime timestamp);
  virtual void HandleChromeMessage(const WmIpc::Message& msg);
  virtual void HandleClientMessage(XWindow xid,
                                   XAtom message_type,
                                   const long data[5]);
  virtual void HandleWindowPropertyChange(XWindow xid, XAtom xatom) {}
  virtual void OwnDestroyedWindow(DestroyedWindow* destroyed_win,
                                  XWindow xid) {}

  // FocusChangeListener implementation.
  virtual void HandleFocusChange();

  // ModalityChangeListener implementation.
  virtual void HandleModalityChange();

  // PanelManagerAreaChangeListener implementation.
  virtual void HandlePanelManagerAreaChange();

  // Assign the input focus to the fullscreen/active browser if possible.
  // Returns false if we were unable to take the focus.
  bool TakeFocus(XTime timestamp);

  // The PanelManager is typically passed in here, but it's decoupled from the
  // c'tor so it can be set by tests.
  void SetPanelAreaNotifier(PanelManagerAreaChangeNotifier* notifier);

 private:
  friend class LayoutManager2Test;
  FRIEND_TEST(LayoutManager2Test, ResizeScreen);
  FRIEND_TEST(LayoutManager2Test, InitialBrowserWindow);
  FRIEND_TEST(LayoutManager2Test, MultipleWindowLayout);
  FRIEND_TEST(LayoutManager2Test, ClickToActivateBrowser);
  FRIEND_TEST(LayoutManager2Test, AvoidJankOnLayoutModeChange);

  // Specifies ArrangeBrowserWindows()'s behavior with regard to assigning the
  // input focus.
  enum ArrangeFocusPolicy {
    // Assign the focus to the fullscreen or active window if it doesn't have it
    // already.
    ASSIGN_FOCUS_DURING_ARRANGE = 0,

    // Leave the focus alone (e.g. we're rearranging the windows because we've
    // exited fullscreen mode in response to a panel being mapped and taking the
    // focus, and we don't want to immediately steal the focus back from the
    // panel).
    PRESERVE_FOCUS_DURING_ARRANGE,
  };

  // Specifies the browser windows that ArrangeBrowserWindows() should modify.
  enum ArrangeSet {
    // Arrange and restack all browser windows.
    ARRANGE_ALL_BROWSERS = 0,

    // Arrange only the active browser.  None of the windows are restacked.
    ARRANGE_ACTIVE_BROWSER,

    // Arrange all but the active browser.  None of the windows are restacked.
    ARRANGE_INACTIVE_BROWSERS,
  };

  // Width used for new windows (although it may be capped).
  static const int kWindowInitialWidthPixels;

  // How many pixels should we leave at the edge of the screen for the next
  // window to be visible?
  static const int kEdgeGapPixels;

  // Brightness for the secondary browser window when in non-maximized mode.
  static const double kSecondaryBrowserWindowBrightness;

  // Brightness for browser windows shown at the edges of the screen.
  static const double kEdgeBrowserWindowBrightness;

  // Get the currently-active browser window, or NULL if none is active (i.e.
  // |browser_windows_| is empty).
  BrowserWindow* GetActiveBrowserWindow() const;

  // Return the index in |browser_| of the BrowserWindow representing |win|.
  // Returns -1 if the window isn't present in the vector.
  int FindBrowserWindowIndexByWindow(const Window& win) const;

  // Get a browser window's index by X ID, or -1 if none exists with the ID.
  int FindBrowserWindowIndexByXid(XWindow xid) const;

  // Get a browser window by X ID, or NULL if none exists with the ID.
  BrowserWindow* FindBrowserWindowByXid(XWindow xid) const;

  // Get the browser window owning |win|, or NULL if no browser window owns it.
  BrowserWindow* FindBrowserWindowOwningTransientWindow(
      const Window& win) const;

  // Get |browser|'s index within |browser_windows_|.
  int GetBrowserWindowIndex(const BrowserWindow& browser) const;

  // Get the initial, minimum, and maximum widths allowed for an unmaximized
  // browser window.
  int GetInitialUnmaximizedWidth() const;
  int GetMinUnmaximizedWidth() const;
  int GetMaxUnmaximizedWidth() const;

  // Constrain a browser window's unmaximized width within the allowed range.
  int ConstrainUnmaximizedWidth(int reqested_width);

  // Get the number of milliseconds that we should take to switch from the
  // currently-active browser to the one at |new_index|.
  int GetWindowAnimationMs(int new_index) const;

  // Get the current time (typically to use when assigning the focus).  If we're
  // handling a key binding, returns the time from the event; otherwise, fetches
  // the current time from the server.
  XTime GetCurrentXTime();

  // Save the area available for layout to |workarea_| and reconfigure browser
  // windows as needed.
  void MoveAndResizeForAvailableArea();

  // Register all of our key bindings.  Called once during initialization.
  void InitKeyBindings();

  // Handle various types of windows being mapped.  Called by HandleWindowMap().
  void HandleBrowserWindowMap(Window* win);
  void HandleTransientWindowMap(Window* transient_win,
                                BrowserWindow* owning_browser);

  // Do additional setup that's needed after we see the first browser window.
  // Called by HandleBrowserWindowMap().
  void HandleFirstBrowserWindowMapped(Window* win);

  // Fill |bounds| with bounds for the windows in |browser_windows_|.
  // The indices of the first (possibly only partially-)offscreen windows to
  // the left and the right are stored in |left_offscreen_index| and
  // |right_offscreen_index|.  The |*_index| parameters may be NULL if unneeded.
  void ComputeBrowserWindowBounds(std::vector<Rect>* bounds,
                                  int* left_offscreen_index,
                                  int* right_offscreen_index);

  // Restack all browser windows.  Helper method for ArrangeBrowserWindows().
  void RestackBrowserWindows(int secondary_browser_index,
                             int left_offscreen_index,
                             int right_offscreen_index);

  // Arrange the browser windows described by |arrange_set|.
  // |timestamp_for_focus| is used to focus the active window; 0 can be passed
  // if |focus_policy| is PRESERVE_FOCUS_DURING_ARRANGE.
  void ArrangeBrowserWindows(ArrangeSet arrange_set,
                             ArrangeFocusPolicy focus_policy,
                             XTime timestamp_for_focus,
                             int anim_ms);

  // Update |active_browser_index_|, along with the new active browser window's
  // anchor position.  ArrangeBrowserWindows() must be called afterwards.
  void SetActiveBrowserWindowIndex(int browser_index);

  // Cycle forward or backward through browser windows.
  // Calls ArrangeBrowserWindows() itself.
  void CycleActiveBrowserWindow(bool forward);

  // Activate the browser window at index |browser_index|.  Effectively, just
  // here so it can be used as a callback that calls both
  // SetActiveBrowserWindow() and ArrangeBrowserWindows().
  void ActivateBrowserWindowByIndex(int browser_index);

  // Call SetLayoutMode() to toggle back and forth between the maximized and
  // overlapping modes.
  void ToggleMaximized();

  // Change the layout mode.
  void SetLayoutMode(chromeos::WmIpcLayoutMode mode, bool arrange_browsers);

  // Change the active browser window's stored width to use while unmaximized.
  // If |maximized_| is currently false, the window is also resized.
  void ChangeActiveBrowserUnmaximizedWidth(int new_width);

  // Resize the active browser window one "step".
  void ResizeActiveBrowserWindowIncrementally(bool to_right);

  // Configure |resize_handle_xid_| appropriately given the current position of
  // the active browser window.
  void ConfigureResizeHandle();

  // Configure |left_edge_input_xid_| and |right_edge_input_xid_| to cover the
  // onscreen portions of the browser windows at |left_offscreen_index| and
  // |right_offscreen_index|, or to be offscreen if those indices are -1.
  void ConfigureEdgeInputWindows(int left_offscreen_index,
                                 int right_offscreen_index);

  void HandleResizeDragStart(const Point& pos);
  void HandleResizeDragMotion(const Point& pos);
  void HandleResizeDragEnd(const Point& pos);

  // Handle |win| no longer preventing us from arranging all of the browser
  // windows (either because we've gotten a new pixmap for it after it's been
  // resized, or because it's been unmapped).  Safe to call even if |win| isn't
  // in |windows_blocking_arrange_|.
  void HandleWindowNoLongerBlockingArrange(Window* win);

  // Call |browser|'s TakeFocus() method, switching to it if it isn't already
  // focused.  This is useful if one of the browser's transient windows has
  // become modal and we want to make sure that it gets the focus.
  void FocusBrowserWindow(BrowserWindow* browser);

  // Make |browser| be fullscreen.  If a different browser window is already
  // fullscreen, it will be restored.  NULL can be supplied to exit fullscreen
  // mode.
  void SetFullscreenBrowserWindow(BrowserWindow* browser);

  // Handle EWMH ClientMessage requests sent by clients (_NET_WM_STATE,
  // _NET_ACTIVE_WINDOW, etc.).
  void HandleFullscreenRequest(Window* win, bool fullscreen);
  void HandleModalityRequest(Window* win, bool modal);
  void HandleActiveWindowRequest(Window* win, XTime timestamp);

  // Close the currently-focused "browser" window if it doesn't appear to be a
  // Chrome window.  (This provides a way to close cros-term windows.)
  void CloseFocusedNonChromeWindow();

  WindowManager* wm_;  // not owned

  // Typically the PanelManager, although it can be set by tests.
  // NULL until SetPanelAreaNotifier() is called.
  PanelManagerAreaChangeNotifier* panel_area_notifier_;  // not owned

  // Area where we lay out browser windows which we intend to be visible.
  // Note that other browser windows may extend outside of this area.
  Rect workarea_;

  scoped_ptr<EventConsumerRegistrar> event_consumer_registrar_;
  scoped_ptr<KeyBindingsActionRegistrar> key_bindings_actions_;

  // Key bindings that are always enabled (while a modal dialog isn't open).
  scoped_ptr<KeyBindingsGroup> key_bindings_group_;

  // Key bindings that are only enabled when a non-Chrome window is focused.
  scoped_ptr<KeyBindingsGroup> non_chrome_key_bindings_group_;

  std::vector<std::tr1::shared_ptr<BrowserWindow> > browser_windows_;

  // Index in |browsers_| of the active browser window.
  int active_browser_index_;

  // The current layout mode (e.g. maximized, overlapping, etc.).
  chromeos::WmIpcLayoutMode layout_mode_;

  // The currently-fullscreen browser window, or NULL if none is fullscreen.
  BrowserWindow* fullscreen_browser_;

  // Map from transient window IDs to the browsers owning them.
  std::map<XWindow, BrowserWindow*> transient_xids_to_browsers_;

  // Windows that we're currently waiting on (either for them to get a new
  // pixmap or to get unmapped) before we can arrange the browser windows.
  std::set<Window*> windows_blocking_arrange_;

  // Duration for arrange animations when ArrangeBrowserWindows() is next called
  // in response to |windows_blocking_arrange_| becoming empty, in milliseconds.
  int anim_ms_for_pending_arrange_;

  // Input window that can be dragged to resize the active browser window.
  XWindow resize_handle_xid_;

  // Left and right resize cursors used by |resize_handle_xid_|.
  XID left_cursor_;
  XID right_cursor_;

  // Input windows that listen for clicks at the left or right edges of the
  // screen so we can switch to the browser windows there.  We stack these on
  // top of the browsers so that the browsers won't display hover effects in
  // response to mouse motion events.
  XWindow left_edge_input_xid_;
  XWindow right_edge_input_xid_;

  // Is the user currently dragging |resize_handle_xid_| to resize the active
  // browser window?
  bool in_resize_drag_;

  // Pointer position and active browser window width when the resize started.
  Point resize_drag_start_pos_;
  int resize_width_at_start_;

  // Box displayed to show the current window dimensions while resizing.
  scoped_ptr<ResizeBox> resize_box_;

  // Have we seen a browser window get mapped yet?
  bool first_browser_window_mapped_;

  DISALLOW_COPY_AND_ASSIGN(LayoutManager2);
};

}  // namespace window_manager

#endif  // WINDOW_MANAGER_LAYOUT2_LAYOUT_MANAGER2_H_
