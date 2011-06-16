// Copyright (c) 2011 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include <string>

#include "base/file_path.h"
#include "base/file_util.h"
#include "base/logging.h"
#include "base/stringprintf.h"
#include "cros/chromeos_wm_ipc_enums.h"
#include "window_manager/layout2/browser_window.h"
#include "window_manager/layout2/layout_manager2.h"
#include "window_manager/panels/panel.h"
#include "window_manager/shadow.h"
#include "window_manager/stacking_manager.h"
#include "window_manager/test_lib.h"
#include "window_manager/window.h"

DEFINE_bool(logtostderr, false,
            "Print debugging messages to stderr (suppressed otherwise)");

// Flags from layout/layout_manager2.cc:
DECLARE_string(initial_chrome_window_mapped_file);
DECLARE_bool(multiple_windows);

using std::string;

namespace window_manager {

class LayoutManager2Test : public BasicWindowManagerTest {
 protected:
  enum Side {
    SIDE_LEFT = 0,
    SIDE_RIGHT,
  };

  // Describes the configuration of browser windows at an edge of the screen.
  enum EdgeConfiguration {
    // There is a slice of an offscreen browser window visible at the edge of
    // the screen.
    EDGE_HAS_BROWSER = 0,

    // No slice is visible (i.e. the active or secondary browser window on that
    // side of the screen is flush with the edge).
    EDGE_HAS_NO_BROWSER,
  };

  LayoutManager2* layout_manager() { return wm_->layout_manager_.get(); }

  // Computes the expected bounds of a browser window.
  // Windows are laid out such that switching to a particular window effectively
  // pans a viewport -- the windows' positions relative to each other don't
  // change.
  //
  // |screen_bounds| contains a particular viewport.  For either the active
  // (focused) or secondary browser window, the viewport is equivalent to the
  // root window's bounds.  For offscreen windows, the viewport is shifted to
  // the left or right.
  //
  // |side| represents the side of |screen_bounds| that the browser is on.
  // |edge_config| describes whether a slice of the next offscreen browser
  // window is visible at the edge of this window's side of the screen or not --
  // if so, the window is shifted slightly away from the edge.
  Rect GetExpectedBoundsForBrowser(
      const Rect& screen_bounds,
      Side side,
      EdgeConfiguration edge_config,
      int browser_width) {
    const int kGap =
        (edge_config == EDGE_HAS_BROWSER) ? LayoutManager2::kEdgeGapPixels : 0;

    switch (side) {
      case SIDE_LEFT:
        return Rect(
            screen_bounds.x + kGap, screen_bounds.y,
            browser_width, screen_bounds.height);
      case SIDE_RIGHT:
        return Rect(
            screen_bounds.right() - browser_width - kGap, screen_bounds.y,
            browser_width, screen_bounds.height);
      default:
        NOTREACHED() << "Unknown side " << side;
        return Rect();
    }
  }

  // Drag the resize handle input window to the left or the right.
  void DragResizeHandle(int dx) {
    const XWindow kResizeXid = layout_manager()->resize_handle_xid_;
    XEvent event;
    xconn_->set_pointer_grab_xid(kResizeXid);
    xconn_->InitButtonPressEvent(&event, kResizeXid, Point(0, 0), 1);
    wm_->HandleEvent(&event);
    xconn_->InitMotionNotifyEvent(&event, kResizeXid, Point(dx, 0));
    wm_->HandleEvent(&event);
    xconn_->InitButtonReleaseEvent(&event, kResizeXid, Point(dx, 0), 1);
    wm_->HandleEvent(&event);
  }
};

// Test that we set windows' horizontally- and vertically-maximized state.
TEST_F(LayoutManager2Test, LegacyMaximized) {
  AutoReset<bool> multiple_windows_flag_resetter(
      &FLAGS_multiple_windows, false);
  CreateAndInitNewWm();

  const XAtom kWmStateAtom = xconn_->GetAtomOrDie("_NET_WM_STATE");
  const XAtom kWmStateMaximizedHorzAtom =
      xconn_->GetAtomOrDie("_NET_WM_STATE_MAXIMIZED_HORZ");
  const XAtom kWmStateMaximizedVertAtom =
      xconn_->GetAtomOrDie("_NET_WM_STATE_MAXIMIZED_VERT");

  XWindow xid = CreateSimpleWindow();
  ASSERT_TRUE(wm_->wm_ipc()->SetWindowType(
      xid, chromeos::WM_IPC_WINDOW_CHROME_TOPLEVEL, NULL));
  SendInitialEventsForWindow(xid);
  EXPECT_TRUE(
      IntArrayPropertyContains(xid, kWmStateAtom, kWmStateMaximizedHorzAtom));
  EXPECT_TRUE(
      IntArrayPropertyContains(xid, kWmStateAtom, kWmStateMaximizedVertAtom));
}

// Test the window-switching logic when windows are mapped and unmapped and when
// the Alt-Tab and Alt-Shift-Tab shortcuts are used.
TEST_F(LayoutManager2Test, LegacyWindowSwitching) {
  AutoReset<bool> multiple_windows_flag_resetter(
      &FLAGS_multiple_windows, false);
  CreateAndInitNewWm();

  const Rect kRootBounds = xconn_->root_bounds();
  const Rect kLeftOffscreenBounds(
      Point(0 - kRootBounds.width, 0), kRootBounds.size());
  const Rect kRightOffscreenBounds(
      Point(kRootBounds.width, 0), kRootBounds.size());
  const Rect kFarRightOffscreenBounds(
      Point(2 * kRootBounds.width, 0), kRootBounds.size());

  // Create a window and check that it gets focused and is resized to cover the
  // entire screen.
  XWindow xid = CreateSimpleWindow();
  ASSERT_TRUE(wm_->wm_ipc()->SetWindowType(
      xid, chromeos::WM_IPC_WINDOW_CHROME_TOPLEVEL, NULL));
  SendInitialEventsForWindow(xid);
  EXPECT_EQ(xid, xconn_->focused_xid());
  EXPECT_EQ(kRootBounds, xconn_->GetWindowInfoOrDie(xid)->bounds);
  EXPECT_EQ(kRootBounds, GetCompositedWindowBounds(xid));

  // Create a second window and check that it's focused and |xid| is pushed
  // offscreen to the left.
  XWindow xid2 = CreateSimpleWindow();
  SendInitialEventsForWindow(xid2);
  EXPECT_EQ(xid2, xconn_->focused_xid());
  EXPECT_EQ(kLeftOffscreenBounds, xconn_->GetWindowInfoOrDie(xid)->bounds);
  EXPECT_EQ(kLeftOffscreenBounds, GetCompositedWindowBounds(xid));
  EXPECT_EQ(kRootBounds, xconn_->GetWindowInfoOrDie(xid2)->bounds);
  EXPECT_EQ(kRootBounds, GetCompositedWindowBounds(xid2));

  // After sending Alt-Tab, we should wrap around and switch back to |xid|.
  const KeyBindings::KeyCombo kAltTabKeyCombo(XK_Tab, KeyBindings::kAltMask);
  const XTime kEventTime = wm_->GetCurrentTimeFromServer();
  SendKey(xconn_->GetRootWindow(), kAltTabKeyCombo, kEventTime, kEventTime);
  EXPECT_EQ(xid, xconn_->focused_xid());
  EXPECT_EQ(kRootBounds, xconn_->GetWindowInfoOrDie(xid)->bounds);
  EXPECT_EQ(kRootBounds, GetCompositedWindowBounds(xid));
  EXPECT_EQ(kRightOffscreenBounds, xconn_->GetWindowInfoOrDie(xid2)->bounds);
  EXPECT_EQ(kRightOffscreenBounds, GetCompositedWindowBounds(xid2));

  // Alt-Shift-Tab should make us wrap around the left edge to |xid2|.
  const KeyBindings::KeyCombo kAltShiftTabKeyCombo(
      XK_Tab, KeyBindings::kShiftMask | KeyBindings::kAltMask);
  SendKey(xconn_->GetRootWindow(),
          kAltShiftTabKeyCombo, kEventTime, kEventTime);
  EXPECT_EQ(xconn_->focused_xid(), xid2);
  EXPECT_EQ(kLeftOffscreenBounds, xconn_->GetWindowInfoOrDie(xid)->bounds);
  EXPECT_EQ(kLeftOffscreenBounds, GetCompositedWindowBounds(xid));
  EXPECT_EQ(kRootBounds, xconn_->GetWindowInfoOrDie(xid2)->bounds);
  EXPECT_EQ(kRootBounds, GetCompositedWindowBounds(xid2));

  // Send a _NET_ACTIVE_WINDOW message asking to move back to |xid|.
  SendActiveWindowMessage(xid);
  EXPECT_EQ(xconn_->focused_xid(), xid);
  EXPECT_EQ(kRootBounds, xconn_->GetWindowInfoOrDie(xid)->bounds);
  EXPECT_EQ(kRootBounds, GetCompositedWindowBounds(xid));
  EXPECT_EQ(kRightOffscreenBounds, xconn_->GetWindowInfoOrDie(xid2)->bounds);
  EXPECT_EQ(kRightOffscreenBounds, GetCompositedWindowBounds(xid2));

  // If we open another window, it should be placed between the other two,
  // giving us the window order |xid|, |xid3|, |xid2|, with |xid3| focused.
  XWindow xid3 = CreateSimpleWindow();
  SendInitialEventsForWindow(xid3);
  EXPECT_EQ(xid3, xconn_->focused_xid());
  EXPECT_EQ(kLeftOffscreenBounds, xconn_->GetWindowInfoOrDie(xid)->bounds);
  EXPECT_EQ(kLeftOffscreenBounds, GetCompositedWindowBounds(xid));
  EXPECT_EQ(kRootBounds, xconn_->GetWindowInfoOrDie(xid3)->bounds);
  EXPECT_EQ(kRootBounds, GetCompositedWindowBounds(xid3));
  EXPECT_EQ(kRightOffscreenBounds, xconn_->GetWindowInfoOrDie(xid2)->bounds);
  EXPECT_EQ(kRightOffscreenBounds, GetCompositedWindowBounds(xid2));

  // If |xid3| is unmapped, we should switch to the window to its left, |xid|.
  XEvent event;
  xconn_->InitUnmapEvent(&event, xid3);
  wm_->HandleEvent(&event);
  EXPECT_EQ(xid, xconn_->focused_xid());
  EXPECT_EQ(kRootBounds, xconn_->GetWindowInfoOrDie(xid)->bounds);
  EXPECT_EQ(kRootBounds, GetCompositedWindowBounds(xid));
  EXPECT_EQ(kRightOffscreenBounds, xconn_->GetWindowInfoOrDie(xid2)->bounds);
  EXPECT_EQ(kRightOffscreenBounds, GetCompositedWindowBounds(xid2));

  // Now unmap |xid| and check that we switch to |xid2|.
  xconn_->InitUnmapEvent(&event, xid);
  wm_->HandleEvent(&event);
  EXPECT_EQ(xid2, xconn_->focused_xid());
  EXPECT_EQ(kRootBounds, xconn_->GetWindowInfoOrDie(xid2)->bounds);
  EXPECT_EQ(kRootBounds, GetCompositedWindowBounds(xid2));

  // Finally, unmap |xid2|.
  xconn_->InitUnmapEvent(&event, xid2);
  wm_->HandleEvent(&event);
}

TEST_F(LayoutManager2Test, LegacyTransients) {
  AutoReset<bool> multiple_windows_flag_resetter(
      &FLAGS_multiple_windows, false);
  CreateAndInitNewWm();

  const XAtom kWmStateAtom = xconn_->GetAtomOrDie("_NET_WM_STATE");
  const XAtom kWmStateModalAtom = xconn_->GetAtomOrDie("_NET_WM_STATE_MODAL");

  // Map two windows.
  XWindow xid = CreateSimpleWindow();
  SendInitialEventsForWindow(xid);
  XWindow xid2 = CreateSimpleWindow();
  SendInitialEventsForWindow(xid2);
  ASSERT_EQ(xid2, xconn_->focused_xid());

  // Map a transient window on behalf of |xid| and check that we don't switch to
  // it automatically.
  const Rect kInitialTransientBounds(0, 0, 300, 200);
  XWindow transient_xid = CreateBasicWindow(kInitialTransientBounds);
  MockXConnection::WindowInfo* transient_info =
      xconn_->GetWindowInfoOrDie(transient_xid);
  transient_info->transient_for = xid;
  SendInitialEventsForWindow(transient_xid);
  EXPECT_EQ(xid2, xconn_->focused_xid());
  EXPECT_EQ(Window::VISIBILITY_HIDDEN,
            wm_->GetWindowOrDie(transient_xid)->visibility());

  // Send a _NET_ACTIVE_WINDOW message asking the WM to focus |transient_xid|
  // and check that it focuses it and switches to |xid|.
  SendActiveWindowMessage(transient_xid);
  EXPECT_EQ(transient_xid, xconn_->focused_xid());
  const Rect kCenteredTransientBounds(
      (xconn_->root_bounds().width - kInitialTransientBounds.width) / 2,
      (xconn_->root_bounds().height - kInitialTransientBounds.height) / 2,
      kInitialTransientBounds.width, kInitialTransientBounds.height);
  EXPECT_EQ(kCenteredTransientBounds,
            xconn_->GetWindowInfoOrDie(transient_xid)->bounds);
  EXPECT_EQ(kCenteredTransientBounds, GetCompositedWindowBounds(transient_xid));
  EXPECT_EQ(Window::VISIBILITY_SHOWN,
            wm_->GetWindowOrDie(transient_xid)->visibility());
  EXPECT_EQ(xconn_->root_bounds(), xconn_->GetWindowInfoOrDie(xid)->bounds);
  EXPECT_EQ(xconn_->root_bounds(), GetCompositedWindowBounds(xid));

  // Map an initially-modal transient window for |xid2|.
  XWindow transient_xid2 = CreateSimpleWindow();
  MockXConnection::WindowInfo* transient_info2 =
      xconn_->GetWindowInfoOrDie(transient_xid2);
  transient_info2->transient_for = xid2;
  AppendAtomToProperty(transient_xid2, kWmStateAtom, kWmStateModalAtom);
  SendInitialEventsForWindow(transient_xid2);

  // We should focus |transient_xid2| and switch to |xid2|.
  EXPECT_EQ(transient_xid2, xconn_->focused_xid());
  EXPECT_EQ(xconn_->root_bounds(), xconn_->GetWindowInfoOrDie(xid2)->bounds);
  EXPECT_EQ(xconn_->root_bounds(), GetCompositedWindowBounds(xid2));
  EXPECT_EQ(Window::VISIBILITY_HIDDEN,
            wm_->GetWindowOrDie(transient_xid)->visibility());
  EXPECT_EQ(Window::VISIBILITY_SHOWN,
            wm_->GetWindowOrDie(transient_xid2)->visibility());

  // Create another (non-modal) transient window for |xid2| and check that
  // |transient_xid2| remains focused (since it's modal) and is stacked on top
  // of |transient_xid3|.  We also say that the window supports
  // WM_DELETE_WINDOW; we use this for another test later.
  XWindow transient_xid3 = CreateSimpleWindow();
  MockXConnection::WindowInfo* transient_info3 =
      xconn_->GetWindowInfoOrDie(transient_xid3);
  transient_info3->transient_for = xid2;
  AppendAtomToProperty(transient_xid3,
                       xconn_->GetAtomOrDie("WM_PROTOCOLS"),
                       xconn_->GetAtomOrDie("WM_DELETE_WINDOW"));
  SendInitialEventsForWindow(transient_xid3);
  EXPECT_EQ(transient_xid2, xconn_->focused_xid());
  EXPECT_TRUE(
      TestWindowIsAboveOtherWindow(wm_->GetWindowOrDie(transient_xid2),
                                   wm_->GetWindowOrDie(transient_xid3)));
  EXPECT_EQ(Window::VISIBILITY_HIDDEN,
            wm_->GetWindowOrDie(transient_xid)->visibility());
  EXPECT_EQ(Window::VISIBILITY_SHOWN,
            wm_->GetWindowOrDie(transient_xid2)->visibility());
  EXPECT_EQ(Window::VISIBILITY_SHOWN,
            wm_->GetWindowOrDie(transient_xid3)->visibility());

  // We should also ignore _NET_ACTIVE_WINDOW messages about |transient_xid|
  // (again, since |transient_xid2| is modal).
  SendActiveWindowMessage(transient_xid);
  EXPECT_EQ(transient_xid2, xconn_->focused_xid());
  EXPECT_EQ(xconn_->root_bounds(), xconn_->GetWindowInfoOrDie(xid2)->bounds);
  EXPECT_EQ(xconn_->root_bounds(), GetCompositedWindowBounds(xid2));

  // We should also ignore clicks on |transient_xid3|.
  xconn_->set_pointer_grab_xid(transient_xid3);
  XEvent event;
  xconn_->InitButtonPressEvent(&event, transient_xid3, Point(0, 0), 1);
  wm_->HandleEvent(&event);
  EXPECT_EQ(0, xconn_->pointer_grab_xid());
  EXPECT_EQ(transient_xid2, xconn_->focused_xid());

  // Alt-Tab shouldn't do anything, either.
  const KeyBindings::KeyCombo kAltTabKeyCombo(XK_Tab, KeyBindings::kAltMask);
  const XTime kEventTime = wm_->GetCurrentTimeFromServer();
  SendKey(xconn_->GetRootWindow(), kAltTabKeyCombo, kEventTime, kEventTime);
  EXPECT_EQ(transient_xid2, xconn_->focused_xid());

  // Send an event asking for |transient_xid2| to be made non-modal.
  xconn_->InitClientMessageEvent(
      &event, transient_xid2, kWmStateAtom, 0, kWmStateModalAtom, 0, 0, 0);
  wm_->HandleEvent(&event);
  xconn_->InitPropertyNotifyEvent(&event, transient_xid2, kWmStateAtom);
  wm_->HandleEvent(&event);
  EXPECT_EQ(transient_xid2, xconn_->focused_xid());

  // Click on |xid2| and check that it takes the focus.
  xconn_->set_pointer_grab_xid(xid2);
  xconn_->InitButtonPressEvent(&event, xid2, Point(0, 0), 1);
  wm_->HandleEvent(&event);
  EXPECT_EQ(0, xconn_->pointer_grab_xid());
  EXPECT_EQ(xid2, xconn_->focused_xid());

  // Click on |transient_xid3| again.  This time, it should be focused and
  // raised above |transient_xid2|.
  xconn_->set_pointer_grab_xid(transient_xid3);
  xconn_->InitButtonPressEvent(&event, transient_xid3, Point(0, 0), 1);
  wm_->HandleEvent(&event);
  EXPECT_EQ(0, xconn_->pointer_grab_xid());
  EXPECT_EQ(transient_xid3, xconn_->focused_xid());
  EXPECT_TRUE(
      TestWindowIsAboveOtherWindow(wm_->GetWindowOrDie(transient_xid3),
                                   wm_->GetWindowOrDie(transient_xid2)));

  // Send a message asking for |transient_xid| to be made modal.
  // We should focus it and automatically switch to |xid|.
  xconn_->InitClientMessageEvent(
      &event, transient_xid, kWmStateAtom, 1, kWmStateModalAtom, 0, 0, 0);
  wm_->HandleEvent(&event);
  xconn_->InitPropertyNotifyEvent(&event, transient_xid, kWmStateAtom);
  wm_->HandleEvent(&event);
  EXPECT_EQ(transient_xid, xconn_->focused_xid());
  EXPECT_EQ(xconn_->root_bounds(), xconn_->GetWindowInfoOrDie(xid)->bounds);
  EXPECT_EQ(xconn_->root_bounds(), GetCompositedWindowBounds(xid));
  EXPECT_EQ(Window::VISIBILITY_SHOWN,
            wm_->GetWindowOrDie(transient_xid)->visibility());
  EXPECT_EQ(Window::VISIBILITY_HIDDEN,
            wm_->GetWindowOrDie(transient_xid2)->visibility());
  EXPECT_EQ(Window::VISIBILITY_HIDDEN,
            wm_->GetWindowOrDie(transient_xid3)->visibility());

  // Unmap |transient_xid| and check that |xid| gets the focus.
  xconn_->InitUnmapEvent(&event, transient_xid);
  wm_->HandleEvent(&event);
  EXPECT_EQ(xid, xconn_->focused_xid());

  // Unmap |xid2| and check that |transient_xid3| got a delete request -- we
  // should try to close a browser window's transients when the browser window
  // is unmapped.
  ASSERT_EQ(0, GetNumDeleteWindowMessagesForWindow(transient_xid3));
  xconn_->InitUnmapEvent(&event, xid2);
  wm_->HandleEvent(&event);
  EXPECT_EQ(1, GetNumDeleteWindowMessagesForWindow(transient_xid3));

  // Map an info bubble and check that its initial position is preserved.
  const Rect kInitialBubbleBounds(20, 30, 40, 50);
  XWindow bubble_xid = CreateBasicWindow(kInitialBubbleBounds);
  ASSERT_TRUE(wm_->wm_ipc()->SetWindowType(
      bubble_xid,
      chromeos::WM_IPC_WINDOW_CHROME_INFO_BUBBLE,
      NULL));
  MockXConnection::WindowInfo* bubble_info =
      xconn_->GetWindowInfoOrDie(bubble_xid);
  bubble_info->transient_for = xid;
  SendInitialEventsForWindow(bubble_xid);
  EXPECT_EQ(bubble_xid, xconn_->focused_xid());
  EXPECT_EQ(kInitialBubbleBounds, bubble_info->bounds);
  EXPECT_EQ(kInitialBubbleBounds, GetCompositedWindowBounds(bubble_xid));

  // Ask the WM to move and resize the bubble, and check that it does.
  const Rect kNewBubbleBounds(60, 70, 80, 90);
  xconn_->InitConfigureRequestEvent(&event, bubble_xid, kNewBubbleBounds);
  wm_->HandleEvent(&event);
  EXPECT_EQ(kNewBubbleBounds, bubble_info->bounds);
  xconn_->InitConfigureNotifyEvent(&event, bubble_xid);
  wm_->HandleEvent(&event);
  EXPECT_EQ(kNewBubbleBounds, GetCompositedWindowBounds(bubble_xid));
}

TEST_F(LayoutManager2Test, LegacyFullscreen) {
  AutoReset<bool> multiple_windows_flag_resetter(
      &FLAGS_multiple_windows, false);
  CreateAndInitNewWm();

  const XAtom kWmStateAtom = xconn_->GetAtomOrDie("_NET_WM_STATE");
  const XAtom kWmStateFullscreenAtom =
      xconn_->GetAtomOrDie("_NET_WM_STATE_FULLSCREEN");

  // Create and map a window.
  XWindow xid = CreateSimpleWindow();
  SendInitialEventsForWindow(xid);
  Window* win = wm_->GetWindowOrDie(xid);
  MockXConnection::WindowInfo* info = xconn_->GetWindowInfoOrDie(xid);

  // Request that the window be made fullscreen.  Check that the fullscreen hint
  // is set on it and that it's restacked.
  XEvent event;
  xconn_->InitClientMessageEvent(
      &event, xid, kWmStateAtom, 1, kWmStateFullscreenAtom, 0, 0, 0);
  wm_->HandleEvent(&event);
  EXPECT_EQ(xid, xconn_->focused_xid());
  EXPECT_EQ(wm_->root_bounds(), info->bounds);
  EXPECT_EQ(wm_->root_bounds(), GetCompositedWindowBounds(xid));
  EXPECT_TRUE(
      IntArrayPropertyContains(xid, kWmStateAtom, kWmStateFullscreenAtom));
  EXPECT_TRUE(WindowIsInLayer(win, StackingManager::LAYER_FULLSCREEN_WINDOW));

  // Map a second window.  It should take the focus, and the first window should
  // be unfullscreened.
  XWindow xid2 = CreateSimpleWindow();
  SendInitialEventsForWindow(xid2);
  EXPECT_EQ(xid2, xconn_->focused_xid());
  EXPECT_FALSE(
      IntArrayPropertyContains(xid, kWmStateAtom, kWmStateFullscreenAtom));

  // Map a third, already-fullscreen window.
  XWindow xid3 = CreateSimpleWindow();
  AppendAtomToProperty(xid3, kWmStateAtom, kWmStateFullscreenAtom);
  SendInitialEventsForWindow(xid3);
  Window* win3 = wm_->GetWindowOrDie(xid);
  MockXConnection::WindowInfo* info3 = xconn_->GetWindowInfoOrDie(xid3);
  EXPECT_EQ(xid3, xconn_->focused_xid());
  EXPECT_EQ(wm_->root_bounds(), info3->bounds);
  EXPECT_EQ(wm_->root_bounds(), GetCompositedWindowBounds(xid3));
  EXPECT_TRUE(
      IntArrayPropertyContains(xid3, kWmStateAtom, kWmStateFullscreenAtom));
  EXPECT_TRUE(WindowIsInLayer(win3, StackingManager::LAYER_FULLSCREEN_WINDOW));

  // Now press Alt-Tab and check that we unfullscreen the third window and
  // switch back to the first window.
  const KeyBindings::KeyCombo kAltTabKeyCombo(XK_Tab, KeyBindings::kAltMask);
  const XTime kEventTime = wm_->GetCurrentTimeFromServer();
  SendKey(xconn_->GetRootWindow(), kAltTabKeyCombo, kEventTime, kEventTime);
  EXPECT_EQ(xid, xconn_->focused_xid());
  EXPECT_TRUE(
      WindowIsInLayer(win, StackingManager::LAYER_ACTIVE_BROWSER_WINDOW));
  EXPECT_FALSE(
      IntArrayPropertyContains(xid3, kWmStateAtom, kWmStateFullscreenAtom));

  // Send a message to make the first window fullscreen again...
  xconn_->InitClientMessageEvent(
      &event, xid, kWmStateAtom, 1, kWmStateFullscreenAtom, 0, 0, 0);
  wm_->HandleEvent(&event);
  EXPECT_TRUE(WindowIsInLayer(win, StackingManager::LAYER_FULLSCREEN_WINDOW));
  EXPECT_TRUE(
      IntArrayPropertyContains(xid, kWmStateAtom, kWmStateFullscreenAtom));

  // ... and then send a message to make it not fullscreen.
  xconn_->InitClientMessageEvent(
      &event, xid, kWmStateAtom, 0, kWmStateFullscreenAtom, 0, 0, 0);
  wm_->HandleEvent(&event);
  EXPECT_TRUE(
      WindowIsInLayer(win, StackingManager::LAYER_ACTIVE_BROWSER_WINDOW));
  EXPECT_FALSE(
      IntArrayPropertyContains(xid, kWmStateAtom, kWmStateFullscreenAtom));

  // Creating a panel (which will take the focus) should also unfullscreen the
  // browser.
  xconn_->InitClientMessageEvent(
      &event, xid, kWmStateAtom, 1, kWmStateFullscreenAtom, 0, 0, 0);
  wm_->HandleEvent(&event);
  Panel* panel = CreatePanel(200, 20, 400);
  ASSERT_EQ(panel->content_xid(), xconn_->focused_xid());
  EXPECT_FALSE(
      IntArrayPropertyContains(xid, kWmStateAtom, kWmStateFullscreenAtom));

  // Check that we don't crash when a fullscreen window is unmapped.
  xconn_->InitClientMessageEvent(
      &event, xid, kWmStateAtom, 1, kWmStateFullscreenAtom, 0, 0, 0);
  wm_->HandleEvent(&event);
  xconn_->InitUnmapEvent(&event, xid);
  wm_->HandleEvent(&event);
}

TEST_F(LayoutManager2Test, LegacyResizeScreen) {
  AutoReset<bool> multiple_windows_flag_resetter(
      &FLAGS_multiple_windows, false);
  CreateAndInitNewWm();

  const Rect kInitialRootBounds = xconn_->root_bounds();
  XWindow xid = CreateSimpleWindow();
  SendInitialEventsForWindow(xid);
  EXPECT_EQ(kInitialRootBounds, xconn_->GetWindowInfoOrDie(xid)->bounds);
  EXPECT_EQ(kInitialRootBounds, GetCompositedWindowBounds(xid));

  // Resize the root window and check that the browser gets resized.
  const Rect kNewRootBounds(
      0, 0, kInitialRootBounds.width * 2, kInitialRootBounds.height * 2);
  xconn_->ResizeWindow(xconn_->GetRootWindow(), kNewRootBounds.size());
  XEvent event;
  xconn_->InitConfigureNotifyEvent(&event, xconn_->GetRootWindow());
  wm_->HandleEvent(&event);
  EXPECT_EQ(kNewRootBounds, xconn_->GetWindowInfoOrDie(xid)->bounds);
  xconn_->InitConfigureNotifyEvent(&event, xid);
  wm_->HandleEvent(&event);
  EXPECT_EQ(kNewRootBounds, GetCompositedWindowBounds(xid));
}

TEST_F(LayoutManager2Test, LegacyStackShadowsAtBottomOfLayer) {
  AutoReset<bool> multiple_windows_flag_resetter(
      &FLAGS_multiple_windows, false);
  CreateAndInitNewWm();

  // Map two windows.  The second should be focused.
  XWindow xid = CreateSimpleWindow();
  SendInitialEventsForWindow(xid);
  Window* win = wm_->GetWindowOrDie(xid);
  XWindow xid2 = CreateSimpleWindow();
  SendInitialEventsForWindow(xid2);
  Window* win2 = wm_->GetWindowOrDie(xid2);
  ASSERT_EQ(xid2, xconn_->focused_xid());

  // The first window should be stacked under the second (focused) window, but
  // both windows' shadows should be under the first window.
  MockCompositor::StageActor* stage = compositor_->GetDefaultStage();
  EXPECT_LT(stage->GetStackingIndex(win2->actor()),
            stage->GetStackingIndex(win->actor()));
  EXPECT_LT(stage->GetStackingIndex(win->actor()),
            stage->GetStackingIndex(win->shadow()->group()));
  EXPECT_LT(stage->GetStackingIndex(win->actor()),
            stage->GetStackingIndex(win2->shadow()->group()));
}

// Test that the layout manager handles windows that claim to be transient
// for already-transient windows reasonably -- see http://crosbug.com/3316.
TEST_F(LayoutManager2Test, NestedTransients) {
  XWindow xid = CreateSimpleWindow();
  SendInitialEventsForWindow(xid);

  // Map a transient window.
  XWindow transient_xid = CreateSimpleWindow();
  xconn_->GetWindowInfoOrDie(transient_xid)->transient_for = xid;
  SendInitialEventsForWindow(transient_xid);
  EXPECT_EQ(transient_xid, xconn_->focused_xid());
  EXPECT_EQ(Window::VISIBILITY_SHOWN,
            wm_->GetWindowOrDie(transient_xid)->visibility());

  // Now map another window that's transient for the first transient window, and
  // check that it gets shown and takes the focus.
  XWindow transient_xid2 = CreateSimpleWindow();
  xconn_->GetWindowInfoOrDie(transient_xid2)->transient_for = transient_xid;
  SendInitialEventsForWindow(transient_xid2);
  EXPECT_EQ(transient_xid2, xconn_->focused_xid());
  EXPECT_EQ(Window::VISIBILITY_SHOWN,
            wm_->GetWindowOrDie(transient_xid2)->visibility());
}

TEST_F(LayoutManager2Test, InitialBrowserWindow) {
  ScopedTempDirectory temp_dir;
  FilePath initial_file_path =
      temp_dir.path().Append("initial_chrome_window_mapped");

  AutoReset<string> file_flag_resetter(
      &FLAGS_initial_chrome_window_mapped_file, initial_file_path.value());
  AutoReset<bool> multiple_windows_flag_resetter(
      &FLAGS_multiple_windows, false);
  CreateAndInitNewWm();

  EXPECT_FALSE(file_util::PathExists(initial_file_path));
  EXPECT_FALSE(layout_manager()->key_bindings_group_->enabled());

  // Check that the first window doesn't get animated sliding onscreen.
  XWindow xid = CreateSimpleWindow();
  SendInitialEventsForWindow(xid);
  Window* win = wm_->GetWindowOrDie(xid);
  EXPECT_FALSE(GetMockActorForWindow(win)->position_was_animated());

  // We should write the first browser window's ID to a file after it's mapped.
  EXPECT_TRUE(file_util::PathExists(initial_file_path));
  string file_contents;
  EXPECT_TRUE(file_util::ReadFileToString(initial_file_path, &file_contents));
  EXPECT_EQ(StringPrintf("%lu", xid), file_contents);

  // We should also enable our key bindings after the first window is mapped.
  EXPECT_TRUE(layout_manager()->key_bindings_group_->enabled());

  // Create a second window and check that both are animated now.
  XWindow xid2 = CreateSimpleWindow();
  SendInitialEventsForWindow(xid2);
  Window* win2 = wm_->GetWindowOrDie(xid2);
  EXPECT_TRUE(GetMockActorForWindow(win)->position_was_animated());
  EXPECT_TRUE(GetMockActorForWindow(win2)->position_was_animated());
}

TEST_F(LayoutManager2Test, CloseTransientWindowWhenOwnerIsUnmapped) {
  XWindow owner_xid = CreateSimpleWindow();
  SendInitialEventsForWindow(owner_xid);

  XWindow transient_xid = CreateSimpleWindow();
  // Say that we support the WM_DELETE_WINDOW protocol.
  AppendAtomToProperty(transient_xid,
                       xconn_->GetAtomOrDie("WM_PROTOCOLS"),
                       xconn_->GetAtomOrDie("WM_DELETE_WINDOW"));
  xconn_->GetWindowInfoOrDie(transient_xid)->transient_for = owner_xid;
  SendInitialEventsForWindow(transient_xid);

  // After we unmap the owner, the transient should receive a delete request.
  ASSERT_EQ(0, GetNumDeleteWindowMessagesForWindow(transient_xid));
  XEvent event;
  xconn_->InitUnmapEvent(&event, owner_xid);
  wm_->HandleEvent(&event);
  ASSERT_EQ(1, GetNumDeleteWindowMessagesForWindow(transient_xid));
}

TEST_F(LayoutManager2Test, MultipleWindowLayout) {
  // Create and map four browser windows.
  XWindow xid1 = CreateSimpleWindow();
  MockXConnection::WindowInfo* info1 = xconn_->GetWindowInfoOrDie(xid1);
  SendInitialEventsForWindow(xid1);
  BrowserWindow* browser1 = layout_manager()->FindBrowserWindowByXid(xid1);
  ASSERT_TRUE(browser1 != NULL);

  XWindow xid2 = CreateSimpleWindow();
  MockXConnection::WindowInfo* info2 = xconn_->GetWindowInfoOrDie(xid2);
  SendInitialEventsForWindow(xid2);
  BrowserWindow* browser2 = layout_manager()->FindBrowserWindowByXid(xid2);
  ASSERT_TRUE(browser2 != NULL);

  XWindow xid3 = CreateSimpleWindow();
  MockXConnection::WindowInfo* info3 = xconn_->GetWindowInfoOrDie(xid3);
  SendInitialEventsForWindow(xid3);
  BrowserWindow* browser3 = layout_manager()->FindBrowserWindowByXid(xid3);
  ASSERT_TRUE(browser3 != NULL);

  XWindow xid4 = CreateSimpleWindow();
  MockXConnection::WindowInfo* info4 = xconn_->GetWindowInfoOrDie(xid4);
  SendInitialEventsForWindow(xid4);
  ASSERT_EQ(xconn_->focused_xid(), xid4);
  BrowserWindow* browser4 = layout_manager()->FindBrowserWindowByXid(xid4);
  ASSERT_TRUE(browser4 != NULL);

  const int kInitialWidth = layout_manager()->GetInitialUnmaximizedWidth();
  const int kGap = LayoutManager2::kEdgeGapPixels;
  const Rect kRootBounds = xconn_->root_bounds();
  const double kSecondaryBrightness =
      LayoutManager2::kSecondaryBrowserWindowBrightness;
  const double kEdgeBrightness = LayoutManager2::kEdgeBrowserWindowBrightness;

  // Initially, the fourth window should be focused on the right, with the third
  // window visible on the left and a strip of the second window visible at the
  // left edge.
  // +-+-----+-------+
  // | |     |       |
  // |2|  3  |   4*  |
  // | |     |       |
  // +-+-----+-------+
  EXPECT_EQ(xconn_->focused_xid(), xid4);

  Rect left_bounds =
      GetExpectedBoundsForBrowser(
          kRootBounds, SIDE_LEFT, EDGE_HAS_BROWSER, kInitialWidth);
  Rect right_bounds =
      GetExpectedBoundsForBrowser(
          kRootBounds, SIDE_RIGHT, EDGE_HAS_NO_BROWSER, kInitialWidth);
  Rect left_offscreen_bounds =
      GetExpectedBoundsForBrowser(
          Rect(left_bounds.right() + kGap - kRootBounds.width, 0,
               kRootBounds.width, kRootBounds.height),
          SIDE_LEFT, EDGE_HAS_BROWSER, kInitialWidth);
  Rect far_left_offscreen_bounds =
      GetExpectedBoundsForBrowser(
          Rect(left_offscreen_bounds.right() + kGap - kRootBounds.width, 0,
               kRootBounds.width, kRootBounds.height),
          SIDE_LEFT, EDGE_HAS_NO_BROWSER, kInitialWidth);

  EXPECT_EQ(far_left_offscreen_bounds, info1->bounds);
  EXPECT_EQ(left_offscreen_bounds, info2->bounds);
  EXPECT_EQ(left_bounds, info3->bounds);
  EXPECT_EQ(right_bounds, info4->bounds);

  EXPECT_TRUE(TestWindowIsAboveOtherWindow(browser4->win(), browser3->win()));
  EXPECT_TRUE(TestWindowIsAboveOtherWindow(browser3->win(), browser2->win()));
  EXPECT_TRUE(TestWindowIsAboveOtherWindow(browser2->win(), browser1->win()));

  EXPECT_DOUBLE_EQ(kEdgeBrightness, browser1->brightness());
  EXPECT_DOUBLE_EQ(kEdgeBrightness, browser2->brightness());
  EXPECT_DOUBLE_EQ(kSecondaryBrightness, browser3->brightness());
  EXPECT_DOUBLE_EQ(1.0, browser4->brightness());

  // Switch back to the second window, which should result in it being focused
  // on the left, the third window being focused on the right, and strips of the
  // first and fourth windows visible at the left and right edges of the screen,
  // respectively.
  // +-+-------+---+-+
  // | |       |   | |
  // |1|   2*  | 3 |4|
  // | |       |   | |
  // +-+-------+---+-+
  SendActiveWindowMessage(xid2);
  EXPECT_EQ(xconn_->focused_xid(), xid2);

  left_bounds =
      GetExpectedBoundsForBrowser(
          kRootBounds, SIDE_LEFT, EDGE_HAS_BROWSER, kInitialWidth);
  right_bounds =
      GetExpectedBoundsForBrowser(
          kRootBounds, SIDE_RIGHT, EDGE_HAS_BROWSER, kInitialWidth);
  left_offscreen_bounds =
      GetExpectedBoundsForBrowser(
          Rect(left_bounds.right() + kGap - kRootBounds.width, 0,
               kRootBounds.width, kRootBounds.height),
          SIDE_LEFT, EDGE_HAS_NO_BROWSER, kInitialWidth);
  Rect right_offscreen_bounds =
      GetExpectedBoundsForBrowser(
          Rect(right_bounds.x - kGap, 0,
               kRootBounds.width, kRootBounds.height),
          SIDE_RIGHT, EDGE_HAS_NO_BROWSER, kInitialWidth);

  EXPECT_EQ(left_offscreen_bounds, info1->bounds);
  EXPECT_EQ(left_bounds, info2->bounds);
  EXPECT_EQ(right_bounds, info3->bounds);
  EXPECT_EQ(right_offscreen_bounds, info4->bounds);

  EXPECT_TRUE(TestWindowIsAboveOtherWindow(browser2->win(), browser3->win()));
  EXPECT_TRUE(TestWindowIsAboveOtherWindow(browser3->win(), browser4->win()));
  EXPECT_TRUE(TestWindowIsAboveOtherWindow(browser2->win(), browser1->win()));
  // The relative stacking of the first and fourth browsers isn't important.

  EXPECT_DOUBLE_EQ(kEdgeBrightness, browser1->brightness());
  EXPECT_DOUBLE_EQ(1.0, browser2->brightness());
  EXPECT_DOUBLE_EQ(kSecondaryBrightness, browser3->brightness());
  EXPECT_DOUBLE_EQ(kEdgeBrightness, browser4->brightness());

  // Finally, switch to the first window.
  // +-------+-----+-+
  // |       |     | |
  // |   1*  |  2  |3|
  // |       |     | |
  // +-------+-----+-+
  SendActiveWindowMessage(xid1);
  EXPECT_EQ(xconn_->focused_xid(), xid1);

  left_bounds =
      GetExpectedBoundsForBrowser(
          kRootBounds, SIDE_LEFT, EDGE_HAS_NO_BROWSER, kInitialWidth);
  right_bounds =
      GetExpectedBoundsForBrowser(
          kRootBounds, SIDE_RIGHT, EDGE_HAS_BROWSER, kInitialWidth);
  right_offscreen_bounds =
      GetExpectedBoundsForBrowser(
          Rect(right_bounds.x - kGap, 0,
               kRootBounds.width, kRootBounds.height),
          SIDE_RIGHT, EDGE_HAS_BROWSER, kInitialWidth);
  Rect far_right_offscreen_bounds =
      GetExpectedBoundsForBrowser(
          Rect(right_offscreen_bounds.x - kGap, 0,
               kRootBounds.width, kRootBounds.height),
          SIDE_RIGHT, EDGE_HAS_NO_BROWSER, kInitialWidth);

  EXPECT_EQ(left_bounds, info1->bounds);
  EXPECT_EQ(right_bounds, info2->bounds);
  EXPECT_EQ(right_offscreen_bounds, info3->bounds);
  EXPECT_EQ(far_right_offscreen_bounds, info4->bounds);

  EXPECT_TRUE(TestWindowIsAboveOtherWindow(browser1->win(), browser2->win()));
  EXPECT_TRUE(TestWindowIsAboveOtherWindow(browser2->win(), browser3->win()));
  EXPECT_TRUE(TestWindowIsAboveOtherWindow(browser3->win(), browser4->win()));

  EXPECT_DOUBLE_EQ(1.0, browser1->brightness());
  EXPECT_DOUBLE_EQ(kSecondaryBrightness, browser2->brightness());
  EXPECT_DOUBLE_EQ(kEdgeBrightness, browser3->brightness());
  EXPECT_DOUBLE_EQ(kEdgeBrightness, browser4->brightness());
}

TEST_F(LayoutManager2Test, ClickToActivateBrowser) {
  // Create and map three browser windows.
  XWindow xid1 = CreateSimpleWindow();
  MockXConnection::WindowInfo* info1 = xconn_->GetWindowInfoOrDie(xid1);
  SendInitialEventsForWindow(xid1);

  XWindow xid2 = CreateSimpleWindow();
  MockXConnection::WindowInfo* info2 = xconn_->GetWindowInfoOrDie(xid2);
  SendInitialEventsForWindow(xid2);

  XWindow xid3 = CreateSimpleWindow();
  MockXConnection::WindowInfo* info3 = xconn_->GetWindowInfoOrDie(xid3);
  SendInitialEventsForWindow(xid3);

  // The third window should be focused initially.
  const int kInitialWidth = layout_manager()->GetInitialUnmaximizedWidth();
  const Rect kRootBounds = xconn_->root_bounds();
  EXPECT_EQ(xid3, xconn_->focused_xid());
  EXPECT_EQ(GetExpectedBoundsForBrowser(
                kRootBounds, SIDE_RIGHT, EDGE_HAS_NO_BROWSER, kInitialWidth),
            info3->bounds);

  // When we click on the input window at the left edge of the screen, we should
  // activate the first browser window.
  XEvent event;
  xconn_->InitButtonPressEvent(
      &event, layout_manager()->left_edge_input_xid_, Point(0, 0), 1);
  wm_->HandleEvent(&event);
  EXPECT_EQ(xid1, xconn_->focused_xid());
  EXPECT_EQ(GetExpectedBoundsForBrowser(
                kRootBounds, SIDE_LEFT, EDGE_HAS_NO_BROWSER, kInitialWidth),
            info1->bounds);

  // Clicking on the (onscreen) second browser window should activate it.
  // The click should also be passed through.
  int num_replay_ungrabs = xconn_->num_pointer_ungrabs_with_replayed_events();
  xconn_->set_pointer_grab_xid(xid2);
  xconn_->InitButtonPressEvent(&event, xid2, Point(0, 0), 1);
  wm_->HandleEvent(&event);
  EXPECT_EQ(0, xconn_->pointer_grab_xid());
  EXPECT_EQ(num_replay_ungrabs + 1,
            xconn_->num_pointer_ungrabs_with_replayed_events());
  EXPECT_EQ(xid2, xconn_->focused_xid());
  EXPECT_EQ(GetExpectedBoundsForBrowser(
                kRootBounds, SIDE_RIGHT, EDGE_HAS_BROWSER, kInitialWidth),
            info2->bounds);
}

TEST_F(LayoutManager2Test, ResizeBrowser) {
  XWindow xid1 = CreateSimpleWindow();
  MockXConnection::WindowInfo* info1 = xconn_->GetWindowInfoOrDie(xid1);
  SendInitialEventsForWindow(xid1);

  XWindow xid2 = CreateSimpleWindow();
  MockXConnection::WindowInfo* info2 = xconn_->GetWindowInfoOrDie(xid2);
  SendInitialEventsForWindow(xid2);

  const Rect kRootBounds = xconn_->root_bounds();

  // Make the second browser window smaller by dragging the resize handle to the
  // right.
  int new_width = info2->bounds.width - 50;
  DragResizeHandle(-1 * (new_width - info2->bounds.width));
  EXPECT_EQ(GetExpectedBoundsForBrowser(
                kRootBounds, SIDE_RIGHT, EDGE_HAS_NO_BROWSER, new_width),
            info2->bounds);

  // Now drag it back to the the left.
  new_width = info2->bounds.width + 25;
  DragResizeHandle(-1 * (new_width - info2->bounds.width));
  EXPECT_EQ(GetExpectedBoundsForBrowser(
                kRootBounds, SIDE_RIGHT, EDGE_HAS_NO_BROWSER, new_width),
            info2->bounds);

  // Do the same thing with the first browser on the left side of the screen.
  SendActiveWindowMessage(xid1);
  EXPECT_EQ(xconn_->focused_xid(), xid1);
  new_width = info1->bounds.width - 60;
  DragResizeHandle(new_width - info1->bounds.width);
  EXPECT_EQ(GetExpectedBoundsForBrowser(
                kRootBounds, SIDE_LEFT, EDGE_HAS_NO_BROWSER, new_width),
            info1->bounds);

  new_width = info1->bounds.width + 30;
  DragResizeHandle(new_width - info1->bounds.width);
  EXPECT_EQ(GetExpectedBoundsForBrowser(
                kRootBounds, SIDE_LEFT, EDGE_HAS_NO_BROWSER, new_width),
            info1->bounds);

  // Try resizing via the keyboard as well.
  const KeyBindings::KeyCombo kAltCommaKeyCombo(
      XK_comma, KeyBindings::kAltMask);
  const XTime kEventTime = wm_->GetCurrentTimeFromServer();
  int old_width = info1->bounds.width;
  SendKey(xconn_->GetRootWindow(), kAltCommaKeyCombo, kEventTime, kEventTime);
  EXPECT_LT(info1->bounds.width, old_width);

  const KeyBindings::KeyCombo kAltPeriodKeyCombo(
      XK_period, KeyBindings::kAltMask);
  old_width = info1->bounds.width;
  SendKey(xconn_->GetRootWindow(), kAltPeriodKeyCombo, kEventTime, kEventTime);
  EXPECT_GT(info1->bounds.width, old_width);
}

}  // namespace window_manager

int main(int argc, char** argv) {
  return window_manager::InitAndRunTests(&argc, argv, &FLAGS_logtostderr);
}
