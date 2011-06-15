// Copyright (c) 2011 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/layout2/layout_manager2.h"

#include <gflags/gflags.h>
extern "C" {
#include <X11/cursorfont.h>
}

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>

#include "base/logging.h"
#include "base/string_util.h"
#include "cros/chromeos_wm_ipc_enums.h"
#include "window_manager/atom_cache.h"
#include "window_manager/callback.h"
#include "window_manager/event_consumer_registrar.h"
#include "window_manager/focus_manager.h"
#include "window_manager/geometry.h"
#include "window_manager/key_bindings.h"
#include "window_manager/layout2/browser_window.h"
#include "window_manager/resize_box.h"
#include "window_manager/stacking_manager.h"
#include "window_manager/util.h"
#include "window_manager/window.h"
#include "window_manager/window_manager.h"
#include "window_manager/x11/x_connection.h"

DEFINE_string(initial_chrome_window_mapped_file,
              "", "When we first see a toplevel Chrome window get mapped, "
              "we write its ID as an ASCII decimal number to this file.  "
              "Tests can watch for the file to know when the user is fully "
              "logged in.  Leave empty to disable.");

DEFINE_bool(multiple_windows, true,
            "Support displaying multiple windows onscreen at once");

DEFINE_bool(show_window_edges, true,
            "Show a bit of the next and previous windows at the edges of the "
            "screen");

DEFINE_string(xterm_command, "xterm", "Command to launch a terminal");

using std::make_pair;
using std::map;
using std::max;
using std::min;
using std::string;
using std::tr1::shared_ptr;
using std::vector;
using window_manager::util::RunCommandInBackground;
using window_manager::util::ToggleBool;
using window_manager::util::XidStr;

namespace window_manager {

namespace {

// Duration of browser window movement animations.
const int kWindowAnimMs = 150;

// Maximum amount of time we'll take to cycle through windows.
const int kMaxWindowCycleAnimMs = 400;

// Width used for new windows (although it may be capped).
const int kWindowInitialWidthPixels = 1024;

// Number of steps between the minimum and maximum browser window size for the
// incremental resize keyboard shortcuts.
const int kNumIncrementalResizeSteps = 2;

// We'll try to change the window's width by at least this fraction of the
// incremental resize amount.
const double kIncrementalResizeMinChangePercent = 0.25;

// How many pixels should we reserve for the status area?  This sets the maximum
// size that a browser window can be in its non-maximized state.
const int kStatusWidthPixels = 200;

// How many pixels should we leave at the edge of the screen for the next window
// to be visible?
const int kEdgeGapPixels = 32;

// Size of the invisible resize handle that can be dragged to resize the active
// browser window in non-maximized mode.
const int kResizeHandleWidthPixels = 12;

// Opacity of the box that we display while the user is resizing a window.
const double kResizeBoxOpacity = 0.4;

// Key binding action names.
static const char kActivateBrowserByIndexActionFormat[] = "activate-browser-%d";
static const char kCycleBackwardAction[] = "cycle-browser-backward";
static const char kCycleForwardAction[] = "cycle-browser-forward";
static const char kLaunchTerminalAction[] = "launch-terminal";
static const char kResizeLeftAction[] = "resize-browser-left";
static const char kResizeRightAction[] = "resize-browser-right";
static const char kToggleMaximizedAction[] = "toggle-browser-maximized";
#ifndef NDEBUG
static const char kToggleShowWindowEdgesAction[] = "toggle-show-window-edges";
#endif

}  // anonymous namespace

LayoutManager2::LayoutManager2(WindowManager* wm)
    : wm_(wm),
      event_consumer_registrar_(new EventConsumerRegistrar(wm, this)),
      key_bindings_actions_(new KeyBindingsActionRegistrar(wm->key_bindings())),
      key_bindings_group_(new KeyBindingsGroup(wm->key_bindings())),
      active_browser_index_(-1),
      maximized_(true),
      fullscreen_browser_(NULL),
      window_to_activate_after_pixmap_fetch_(NULL),
      window_for_arrange_after_pixmap_fetch_(NULL),
      resize_handle_xid_(wm->CreateInputWindow(Rect(-1, -1, 1, 1), 0)),
      left_cursor_(wm_->xconn()->CreateShapedCursor(XC_left_side)),
      right_cursor_(wm_->xconn()->CreateShapedCursor(XC_right_side)),
      left_edge_input_xid_(
          wm_->CreateInputWindow(Rect(-1, -1, 1, 1), ButtonPressMask)),
      right_edge_input_xid_(
          wm_->CreateInputWindow(Rect(-1, -1, 1, 1), ButtonPressMask)),
      in_resize_drag_(false),
      resize_width_at_start_(0),
      last_screen_size_(wm_->root_size()),
      first_browser_window_mapped_(false) {
  event_consumer_registrar_->RegisterForChromeMessages(
      chromeos::WM_IPC_MESSAGE_WM_CYCLE_WINDOWS);
  wm_->focus_manager()->RegisterFocusChangeListener(this);
  wm_->modality_handler()->RegisterModalityChangeListener(this);
  InitKeyBindings();

  wm_->stacking_manager()->StackXidAtTopOfLayer(
      resize_handle_xid_,
      StackingManager::LAYER_ACTIVE_BROWSER_WINDOW);
  event_consumer_registrar_->RegisterForWindowEvents(resize_handle_xid_);
  int event_mask = ButtonPressMask | ButtonReleaseMask | PointerMotionMask;
  wm->xconn()->AddButtonGrabOnWindow(resize_handle_xid_, 1, event_mask, false);
  wm->SetNamePropertiesForXid(resize_handle_xid_,
                              "input window for browser resize");

  wm_->stacking_manager()->StackXidAtTopOfLayer(
      left_edge_input_xid_,
      StackingManager::LAYER_ACTIVE_BROWSER_WINDOW);
  event_consumer_registrar_->RegisterForWindowEvents(left_edge_input_xid_);
  wm->SetNamePropertiesForXid(left_edge_input_xid_, "left edge input window");

  wm_->stacking_manager()->StackXidAtTopOfLayer(
      right_edge_input_xid_,
      StackingManager::LAYER_ACTIVE_BROWSER_WINDOW);
  event_consumer_registrar_->RegisterForWindowEvents(right_edge_input_xid_);
  wm->SetNamePropertiesForXid(right_edge_input_xid_, "right edge input window");
}

LayoutManager2::~LayoutManager2() {
  browser_windows_.clear();
  wm_->focus_manager()->UnregisterFocusChangeListener(this);
  wm_->modality_handler()->UnregisterModalityChangeListener(this);
  wm_->xconn()->DestroyWindow(resize_handle_xid_);
  wm_->xconn()->FreeCursor(left_cursor_);
  wm_->xconn()->FreeCursor(right_cursor_);
  wm_->xconn()->DestroyWindow(left_edge_input_xid_);
  wm_->xconn()->DestroyWindow(right_edge_input_xid_);
}

void LayoutManager2::HandleScreenResize() {
  // Try to preserve windows' old fractions of the screen's width while ensuring
  // that they stay within the acceptable limits.
  Size new_size = wm_->root_size();
  for (int i = 0; i < num_browser_windows(); ++i) {
    BrowserWindow* browser = browser_windows_[i].get();
    const double ratio =
        static_cast<double>(browser->unmaximized_width()) /
        last_screen_size_.width;
    browser->set_unmaximized_width(
        ConstrainUnmaximizedWidth(round(ratio * new_size.width)));
  }
  // TODO(derat): Recenter transient windows?
  ArrangeBrowserWindows(GetCurrentXTime(), PRESERVE_FOCUS_DURING_ARRANGE, 0);
  last_screen_size_ = new_size;
}

bool LayoutManager2::HandleWindowMapRequest(Window* win) {
  if (win->transient_for_xid()) {
    BrowserWindow* owning_browser =
        FindBrowserWindowByXid(win->transient_for_xid());
    if (!owning_browser) {
      // Handle windows that are transient for other transient windows --
      // see http://crosbug.com/3316.
      Window* owning_win = wm_->GetWindow(win->transient_for_xid());
      if (owning_win)
        owning_browser = FindBrowserWindowOwningTransientWindow(*owning_win);
    }
    return (owning_browser != NULL);
  }

  if (BrowserWindow::ShouldHandleWindow(*win)) {
    win->SetVisibility(Window::VISIBILITY_HIDDEN);
    win->Move(Point(wm_->width(), 0), 0);

    const bool maximize =
        !FLAGS_multiple_windows || (maximized_ && num_browser_windows() != 1);
    const int width =
        maximize || win->wm_state_fullscreen() ?
        wm_->width() :
        GetInitialUnmaximizedWidth();
    win->Resize(Size(width, wm_->height()), GRAVITY_NORTHWEST);
    return true;
  }

  return false;
}

void LayoutManager2::HandleWindowMap(Window* win) {
  if (win->override_redirect())
      return;

  if (win->transient_for_xid()) {
    BrowserWindow* owning_browser =
        FindBrowserWindowByXid(win->transient_for_xid());
    if (!owning_browser) {
      Window* owning_win = wm_->GetWindow(win->transient_for_xid());
      if (owning_win)
        owning_browser = FindBrowserWindowOwningTransientWindow(*owning_win);
    }
    if (owning_browser)
      HandleTransientWindowMap(win, owning_browser);
  } else if (BrowserWindow::ShouldHandleWindow(*win)) {
    HandleBrowserWindowMap(win);
  }
}

void LayoutManager2::HandleWindowUnmap(Window* win) {
  if (win->override_redirect())
    return;

  BrowserWindow* owning_browser = FindBrowserWindowOwningTransientWindow(*win);
  if (owning_browser) {
    const bool transient_had_focus = win->IsFocused();
    owning_browser->HandleTransientWindowUnmap(win);
    transient_xids_to_browsers_.erase(win->xid());
    if (transient_had_focus)
      owning_browser->TakeFocus(GetCurrentXTime());
    return;
  }

  int browser_index = FindBrowserWindowIndexByWindow(*win);
  if (browser_index < 0)
    return;

  const bool had_focus = (win == wm_->focus_manager()->focused_win());
  if (window_to_activate_after_pixmap_fetch_ == win)
    window_to_activate_after_pixmap_fetch_ = NULL;
  if (window_for_arrange_after_pixmap_fetch_ == win)
    window_for_arrange_after_pixmap_fetch_ = NULL;
  if (fullscreen_browser_ && fullscreen_browser_->win() == win)
    SetFullscreenBrowserWindow(NULL);

  browser_windows_.erase(browser_windows_.begin() + browser_index);

  if (num_browser_windows() <= 1)
    maximized_ = true;

  if (browser_windows_.empty()) {
    active_browser_index_ = -1;
    return;
  }

  int new_index = active_browser_index_;
  if (new_index >= num_browser_windows())
    new_index = num_browser_windows() - 1;
  else if (browser_index <= new_index && new_index > 0)
    new_index--;

  SetActiveBrowserWindowIndex(new_index);
  ArrangeBrowserWindows(
      GetCurrentXTime(),
      had_focus ? ASSIGN_FOCUS_DURING_ARRANGE : PRESERVE_FOCUS_DURING_ARRANGE,
      kWindowAnimMs);
}

void LayoutManager2::HandleWindowPixmapFetch(Window* win) {
  bool arranged = false;

  if (window_to_activate_after_pixmap_fetch_ == win) {
    window_to_activate_after_pixmap_fetch_ = NULL;
    const int index = FindBrowserWindowIndexByWindow(*win);
    DCHECK_GE(index, 0);
    SetActiveBrowserWindowIndex(index);
    ArrangeBrowserWindows(
        GetCurrentXTime(), ASSIGN_FOCUS_DURING_ARRANGE, kWindowAnimMs);
    arranged = true;
  }

  if (window_for_arrange_after_pixmap_fetch_ == win) {
    window_for_arrange_after_pixmap_fetch_ = NULL;
    if (!arranged) {
      ArrangeBrowserWindows(GetCurrentXTime(), ASSIGN_FOCUS_DURING_ARRANGE, 0);
      arranged = true;
    }
  }
}

void LayoutManager2::HandleWindowConfigureRequest(
    Window* win, const Rect& requested_bounds) {
  // Ignore requests to resize browser windows, but send them fake
  // ConfigureNotify events to let them know that we saw the requests.
  BrowserWindow* browser = FindBrowserWindowByXid(win->xid());
  if (browser) {
    win->SendSyntheticConfigureNotify();
    return;
  }

  BrowserWindow* owning_browser = FindBrowserWindowOwningTransientWindow(*win);
  if (owning_browser) {
    owning_browser->HandleTransientWindowConfigureRequest(
        win, requested_bounds);
    return;
  }
}

void LayoutManager2::HandleButtonPress(XWindow xid,
                                       const Point& relative_pos,
                                       const Point& absolute_pos,
                                       int button,
                                       XTime timestamp) {
  if (xid == resize_handle_xid_ && button == 1) {
    HandleResizeDragStart(absolute_pos);
    return;
  }

  if (xid == left_edge_input_xid_ || xid == right_edge_input_xid_) {
    vector<Rect> bounds;
    int left_offscreen_index = -1, right_offscreen_index = -1;
    ComputeBrowserWindowBounds(
        &bounds, &left_offscreen_index, &right_offscreen_index);
    if (xid == left_edge_input_xid_ && left_offscreen_index != -1)
      SetActiveBrowserWindowIndex(left_offscreen_index);
    else if (xid == right_edge_input_xid_ && right_offscreen_index != -1)
      SetActiveBrowserWindowIndex(right_offscreen_index);
    else
      LOG(WARNING) << "Unhandled button press in input window " << xid;
    ArrangeBrowserWindows(
        timestamp, ASSIGN_FOCUS_DURING_ARRANGE, kWindowAnimMs);
    return;
  }

  Window* win = wm_->GetWindow(xid);
  if (!win)
    return;

  int browser_index = -1;
  if (win->transient_for_xid()) {
    // If we got a click in a transient window, then give it the focus.
    BrowserWindow* owning_browser =
        FindBrowserWindowOwningTransientWindow(*win);
    if (!owning_browser)
      return;
    owning_browser->SetPreferredTransientWindowToFocus(win);
    browser_index = GetBrowserWindowIndex(*owning_browser);
  } else {
    // Otherwise, tell the browser window to steal the focus from its transient
    // window.
    browser_index = FindBrowserWindowIndexByWindow(*win);
    if (browser_index == -1)
      return;
    BrowserWindow* browser = browser_windows_[browser_index].get();
    browser->SetPreferredTransientWindowToFocus(NULL);
  }

  // If the click happened in the active browser, then we're probably stealing
  // the focus from a panel or something.
  DCHECK_NE(browser_index, -1);
  if (browser_index == active_browser_index_) {
    TakeFocus(timestamp);
  } else {
    SetActiveBrowserWindowIndex(browser_index);
    ArrangeBrowserWindows(
        timestamp, ASSIGN_FOCUS_DURING_ARRANGE, kWindowAnimMs);
  }
}

void LayoutManager2::HandleButtonRelease(XWindow xid,
                                         const Point& relative_pos,
                                         const Point& absolute_pos,
                                         int button,
                                         XTime timestamp) {
  if (xid == resize_handle_xid_) {
    if (button == 1)
      HandleResizeDragEnd(absolute_pos);
    return;
  }
}

void LayoutManager2::HandlePointerMotion(XWindow xid,
                                         const Point& relative_pos,
                                         const Point& absolute_pos,
                                         XTime timestamp) {
  if (xid == resize_handle_xid_) {
    HandleResizeDragMotion(absolute_pos);
    return;
  }
}

void LayoutManager2::HandleChromeMessage(const WmIpc::Message& msg) {
  if (msg.type() == chromeos::WM_IPC_MESSAGE_WM_CYCLE_WINDOWS)
    CycleActiveBrowserWindow(msg.param(0) != 0);
}

void LayoutManager2::HandleClientMessage(XWindow xid,
                                         XAtom message_type,
                                         const long data[5]) {
  Window* win = wm_->GetWindow(xid);
  if (!win)
    return;

  if (message_type == wm_->GetXAtom(ATOM_NET_WM_STATE)) {
    map<XAtom, bool> states;
    win->ParseWmStateMessage(data, &states);

    map<XAtom, bool>::const_iterator it =
        states.find(wm_->GetXAtom(ATOM_NET_WM_STATE_FULLSCREEN));
    if (it != states.end())
      HandleFullscreenRequest(win, it->second);

    it = states.find(wm_->GetXAtom(ATOM_NET_WM_STATE_MODAL));
    if (it != states.end())
      HandleModalityRequest(win, it->second);

  } else if (message_type == wm_->GetXAtom(ATOM_NET_ACTIVE_WINDOW)) {
    HandleActiveWindowRequest(win, data[1]);
  }
}

void LayoutManager2::HandleFocusChange() {
  if (fullscreen_browser_ &&
      !fullscreen_browser_->IsWindowOrTransientFocused()) {
    SetFullscreenBrowserWindow(NULL);
    ArrangeBrowserWindows(GetCurrentXTime(), PRESERVE_FOCUS_DURING_ARRANGE, 0);
  }
}

void LayoutManager2::HandleModalityChange() {
  if (wm_->IsModalWindowFocused())
    key_bindings_group_->Disable();
  else
    key_bindings_group_->Enable();
}

bool LayoutManager2::TakeFocus(XTime timestamp) {
  if (browser_windows_.empty())
    return false;

  if (fullscreen_browser_)
    fullscreen_browser_->TakeFocus(timestamp);
  else
    GetActiveBrowserWindow()->TakeFocus(timestamp);
  return true;
}

BrowserWindow* LayoutManager2::GetActiveBrowserWindow() const {
  if (browser_windows_.empty())
    return NULL;

  DCHECK_GE(active_browser_index_, 0);
  DCHECK_LT(active_browser_index_, num_browser_windows());
  return browser_windows_[active_browser_index_].get();
}

int LayoutManager2::FindBrowserWindowIndexByWindow(const Window& win) const {
  for (int i = 0; i < num_browser_windows(); ++i) {
    if (browser_windows_[i]->win() == &win)
      return i;
  }
  return -1;
}

int LayoutManager2::FindBrowserWindowIndexByXid(XWindow xid) const {
  const Window* win = wm_->GetWindow(xid);
  return win ? FindBrowserWindowIndexByWindow(*win) : -1;
}

BrowserWindow* LayoutManager2::FindBrowserWindowByXid(XWindow xid) const {
  const int index = FindBrowserWindowIndexByXid(xid);
  return index >= 0 ? browser_windows_[index].get() : NULL;
}

BrowserWindow* LayoutManager2::FindBrowserWindowOwningTransientWindow(
    const Window& win) const {
  map<XWindow, BrowserWindow*>::const_iterator it =
      transient_xids_to_browsers_.find(win.xid());
  return (it != transient_xids_to_browsers_.end()) ? it->second : NULL;
}

int LayoutManager2::GetBrowserWindowIndex(const BrowserWindow& browser) const {
  return FindBrowserWindowIndexByWindow(*(browser.win()));
}

int LayoutManager2::GetInitialUnmaximizedWidth() const {
  return max(min(kWindowInitialWidthPixels, GetMaxUnmaximizedWidth()),
             GetMinUnmaximizedWidth());
}

int LayoutManager2::GetMinUnmaximizedWidth() const {
  // TODO(derat): Change this based on whether there's a window to the edge or
  // not?
  return 0.5 * (wm_->width() + 1);
}

int LayoutManager2::GetMaxUnmaximizedWidth() const {
  return wm_->width() - kStatusWidthPixels -
      (FLAGS_show_window_edges ? 2 * kEdgeGapPixels : 0);
}

int LayoutManager2::ConstrainUnmaximizedWidth(int requested_width) {
  return max(min(requested_width, GetMaxUnmaximizedWidth()),
             GetMinUnmaximizedWidth());
}

int LayoutManager2::GetWindowAnimationMs(int new_index) const {
  const int num_windows_to_traverse =
      max(new_index, active_browser_index_) -
      min(new_index, active_browser_index_);
  return maximized_ ?
         min(num_windows_to_traverse * kWindowAnimMs, kMaxWindowCycleAnimMs) :
         kWindowAnimMs;
}

XTime LayoutManager2::GetCurrentXTime() {
  XTime current_xtime = wm_->key_bindings()->current_event_time();
  if (!current_xtime)
    current_xtime = wm_->GetCurrentTimeFromServer();
  return current_xtime;
}

void LayoutManager2::InitKeyBindings() {
  // Disable the key bindings until we see the first browser window.
  key_bindings_group_->Disable();

  key_bindings_actions_->AddAction(
      kCycleForwardAction,
      NewPermanentCallback(
          this, &LayoutManager2::CycleActiveBrowserWindow, true),
      NULL, NULL);
  key_bindings_group_->AddBinding(
      KeyBindings::KeyCombo(XK_Tab, KeyBindings::kAltMask),
      kCycleForwardAction);
  key_bindings_group_->AddBinding(
      KeyBindings::KeyCombo(XK_F5, 0),
      kCycleForwardAction);

  key_bindings_actions_->AddAction(
      kCycleBackwardAction,
      NewPermanentCallback(
          this, &LayoutManager2::CycleActiveBrowserWindow, false),
      NULL, NULL);
  key_bindings_group_->AddBinding(
      KeyBindings::KeyCombo(
          XK_Tab, KeyBindings::kAltMask | KeyBindings::kShiftMask),
      kCycleBackwardAction);
  key_bindings_group_->AddBinding(
      KeyBindings::KeyCombo(XK_F5, KeyBindings::kShiftMask),
      kCycleBackwardAction);

  for (int i = 0; i < 8; ++i) {
    const string action_name =
        StringPrintf(kActivateBrowserByIndexActionFormat, i);
    key_bindings_actions_->AddAction(
        action_name,
        NewPermanentCallback(
            this, &LayoutManager2::ActivateBrowserWindowByIndex, i),
        NULL, NULL);
    key_bindings_group_->AddBinding(
        KeyBindings::KeyCombo(XK_1 + i, KeyBindings::kAltMask),
        action_name);
  }

  if (FLAGS_multiple_windows) {
    key_bindings_actions_->AddAction(
        kToggleMaximizedAction,
        NewPermanentCallback(this, &LayoutManager2::ToggleMaximized),
        NULL, NULL);
    key_bindings_group_->AddBinding(
        KeyBindings::KeyCombo(XK_Return, KeyBindings::kAltMask),
        kToggleMaximizedAction);

    key_bindings_actions_->AddAction(
        kResizeLeftAction,
        NewPermanentCallback(
            this,
            &LayoutManager2::ResizeActiveBrowserWindowIncrementally, false),
        NULL, NULL);
    key_bindings_group_->AddBinding(
        KeyBindings::KeyCombo(XK_comma, KeyBindings::kAltMask),
        kResizeLeftAction);

    key_bindings_actions_->AddAction(
        kResizeRightAction,
        NewPermanentCallback(
            this,
            &LayoutManager2::ResizeActiveBrowserWindowIncrementally, true),
        NULL, NULL);
    key_bindings_group_->AddBinding(
        KeyBindings::KeyCombo(XK_period, KeyBindings::kAltMask),
        kResizeRightAction);
  }

  key_bindings_actions_->AddAction(
      kLaunchTerminalAction,
      NewPermanentCallback(&RunCommandInBackground, FLAGS_xterm_command),
      NULL, NULL);
  key_bindings_group_->AddBinding(
      KeyBindings::KeyCombo(
          XK_t, KeyBindings::kControlMask | KeyBindings::kAltMask),
      kLaunchTerminalAction);

#ifndef NDEBUG
  const int kCtrlAltShiftMask = KeyBindings::kControlMask |
                                KeyBindings::kAltMask |
                                KeyBindings::kShiftMask;

  key_bindings_actions_->AddAction(
      kToggleShowWindowEdgesAction,
      NewPermanentCallback(&ToggleBool, &FLAGS_show_window_edges),
      NULL, NULL);
  key_bindings_group_->AddBinding(
      KeyBindings::KeyCombo(XK_e, kCtrlAltShiftMask),
      kToggleShowWindowEdgesAction);
#endif
}

void LayoutManager2::HandleBrowserWindowMap(Window* win) {
  DCHECK(FindBrowserWindowIndexByWindow(*win) < 0)
      << "Window " << win->xid_str() << " is already being tracked";

  if (FLAGS_multiple_windows && maximized_ && num_browser_windows() == 1)
    maximized_ = false;

  shared_ptr<BrowserWindow> browser(
      new BrowserWindow(win, this, GetInitialUnmaximizedWidth()));

  int index = 0;
  if (num_browser_windows()) {
    index = active_browser_index_ + 1;
    browser_windows_.insert(browser_windows_.begin() + index, browser);
  } else {
    browser_windows_.push_back(browser);
  }

  if (win->wm_state_fullscreen())
    SetFullscreenBrowserWindow(browser.get());
  else if (fullscreen_browser_)
    SetFullscreenBrowserWindow(NULL);

  // If the window is ready to be composited, activate it now.
  // Otherwise, wait until we've fetched its pixmap.
  if (win->has_initial_pixmap() || active_browser_index_ < 0) {
    const int anim_ms = num_browser_windows() > 1 ? kWindowAnimMs : 0;
    SetActiveBrowserWindowIndex(index);
    ArrangeBrowserWindows(
        GetCurrentXTime(), ASSIGN_FOCUS_DURING_ARRANGE, anim_ms);
  } else {
    window_to_activate_after_pixmap_fetch_ = win;
  }

  if (!first_browser_window_mapped_) {
    HandleFirstBrowserWindowMapped(win);
    first_browser_window_mapped_ = true;
  }
}

void LayoutManager2::HandleTransientWindowMap(Window* transient_win,
                                              BrowserWindow* owning_browser) {
  DCHECK(transient_win);
  DCHECK(owning_browser);
  DCHECK(transient_xids_to_browsers_.count(transient_win->xid()) == 0)
      << "Transient window " << transient_win->xid_str() << " is already "
      << "registered";

  transient_xids_to_browsers_.insert(
      make_pair(transient_win->xid(), owning_browser));
  owning_browser->HandleTransientWindowMap(transient_win);

  // If the window is modal or if its owner is the active window, make sure that
  // it gets the focus.
  if (transient_win->wm_state_modal() ||
      owning_browser == GetActiveBrowserWindow())
    FocusBrowserWindow(owning_browser);
}

void LayoutManager2::HandleFirstBrowserWindowMapped(Window* win) {
  DCHECK(win);

  key_bindings_group_->Enable();

  if (!FLAGS_initial_chrome_window_mapped_file.empty()) {
    DLOG(INFO) << "Writing initial Chrome window's ID to file "
               << FLAGS_initial_chrome_window_mapped_file;
    FILE* file = fopen(FLAGS_initial_chrome_window_mapped_file.c_str(), "w+");
    if (!file) {
      PLOG(ERROR) << "Unable to open file "
                  << FLAGS_initial_chrome_window_mapped_file;
    } else {
      fprintf(file, "%lu", win->xid());
      fclose(file);
    }
  }
}

void LayoutManager2::ComputeBrowserWindowBounds(vector<Rect>* bounds,
                                                int* left_offscreen_index,
                                                int* right_offscreen_index) {
  DCHECK(bounds);
  bounds->clear();
  if (left_offscreen_index)
    *left_offscreen_index = -1;
  if (right_offscreen_index)
    *right_offscreen_index = -1;

  if (browser_windows_.empty())
    return;

  bounds->resize(num_browser_windows(), Rect());
  BrowserWindow* active_browser = GetActiveBrowserWindow();
  DCHECK(active_browser);

  if (maximized_) {
    // The maximized case is simple: align the active browser window with the
    // left edge of the screen and move one screen width away from it for each
    // other window.
    (*bounds)[active_browser_index_].reset(0, 0, wm_->width(), wm_->height());
    for (int i = active_browser_index_ - 1; i >= 0; --i)
      (*bounds)[i].reset(
          (*bounds)[i + 1].x - wm_->width(), 0, wm_->width(), wm_->height());
    for (int i = active_browser_index_ + 1; i < num_browser_windows(); ++i)
      (*bounds)[i].reset(
          (*bounds)[i - 1].x + wm_->width(), 0, wm_->width(), wm_->height());

  } else {
    // Find the index of the browser windows displayed on the left and right
    // portions of the screen.
    const int left_index =
        active_browser_index_ - (active_browser->anchored_to_left() ? 0 : 1);
    const int right_index =
        active_browser_index_ + (active_browser->anchored_to_left() ? 1 : 0);
    DCHECK_GE(left_index, 0);
    DCHECK_LE(right_index, num_browser_windows() - 1);

    // Width of the gaps to leave at the edge of the screen.
    const int gap_width = FLAGS_show_window_edges ? kEdgeGapPixels : 0;

    if (left_offscreen_index && left_index > 0)
      *left_offscreen_index = left_index - 1;
    if (right_offscreen_index && right_index < num_browser_windows() - 1)
      *right_offscreen_index = right_index + 1;

    BrowserWindow* left_browser = browser_windows_[left_index].get();
    BrowserWindow* right_browser = browser_windows_[right_index].get();

    // Assign positions to the left and right windows, leaving small gaps on the
    // sides if there are more windows in either direction.
    (*bounds)[left_index].reset(
        left_index >= 1 ? gap_width : 0, 0,
        left_browser->unmaximized_width(), wm_->height());
    (*bounds)[right_index].reset(
        wm_->width() -
        right_browser->unmaximized_width() -
        (right_index < num_browser_windows() - 1 ? gap_width : 0), 0,
        right_browser->unmaximized_width(), wm_->height());

    // Next, walk to the left, putting each window one screen width from the
    // right edge of the window to its right.
    int prev_right_edge =
        (*bounds)[left_index].x + left_browser->unmaximized_width();
    for (int i = left_index - 1; i >= 0; --i) {
      BrowserWindow* browser = browser_windows_[i].get();
      (*bounds)[i].reset(
          prev_right_edge - wm_->width(), 0,
          browser->unmaximized_width(), wm_->height());
      prev_right_edge = (*bounds)[i].x + browser->unmaximized_width();
    }

    // Now take the same approach while walking to the right.
    int prev_left_edge = (*bounds)[right_index].x;
    for (int i = right_index + 1; i < num_browser_windows(); ++i) {
      BrowserWindow* browser = browser_windows_[i].get();
      (*bounds)[i].reset(
          prev_left_edge + wm_->width() - browser->unmaximized_width(), 0,
          browser->unmaximized_width(), wm_->height());
      prev_left_edge = (*bounds)[i].x;
    }
  }
}

void LayoutManager2::RestackBrowserWindows(
    int secondary_browser_index,
    int left_offscreen_index,
    int right_offscreen_index) {
  if (browser_windows_.empty())
    return;

  if (fullscreen_browser_)
    fullscreen_browser_->StackAtTopOfLayer(
        StackingManager::LAYER_FULLSCREEN_WINDOW,
        StackingManager::SHADOW_DIRECTLY_BELOW_ACTOR,
        StackingManager::LAYER_FULLSCREEN_WINDOW);

  StackingManager::ShadowPolicy shadow_policy =
      maximized_ ?
      StackingManager::SHADOW_AT_BOTTOM_OF_SHADOW_LAYER :
      StackingManager::SHADOW_DIRECTLY_BELOW_ACTOR;

  BrowserWindow* active_browser = GetActiveBrowserWindow();
  if (active_browser != fullscreen_browser_)
    active_browser->StackAtTopOfLayer(
        StackingManager::LAYER_ACTIVE_BROWSER_WINDOW,
        shadow_policy,
        StackingManager::LAYER_INACTIVE_BROWSER_WINDOW);

  BrowserWindow* secondary_browser =
      secondary_browser_index != -1 ?
      browser_windows_[secondary_browser_index].get() :
      NULL;
  if (secondary_browser && secondary_browser != fullscreen_browser_)
    secondary_browser->StackAtTopOfLayer(
        StackingManager::LAYER_INACTIVE_BROWSER_WINDOW,
        shadow_policy,
        StackingManager::LAYER_INACTIVE_BROWSER_WINDOW);

  BrowserWindow* prev_browser = secondary_browser;
  if (left_offscreen_index != -1) {
    for (int i = left_offscreen_index; i >= 0; --i) {
      BrowserWindow* browser = browser_windows_[i].get();
      if (browser == fullscreen_browser_)
        continue;
      if (prev_browser)
        browser->StackBelowOtherBrowserWindow(
            prev_browser,
            shadow_policy,
            StackingManager::LAYER_INACTIVE_BROWSER_WINDOW);
      else
        browser->StackAtTopOfLayer(
            StackingManager::LAYER_INACTIVE_BROWSER_WINDOW,
            shadow_policy,
            StackingManager::LAYER_INACTIVE_BROWSER_WINDOW);
      prev_browser = browser;
    }
  }

  prev_browser = secondary_browser;
  if (right_offscreen_index != -1) {
    for (int i = right_offscreen_index; i < num_browser_windows(); ++i) {
      BrowserWindow* browser = browser_windows_[i].get();
      if (browser == fullscreen_browser_)
        continue;
      if (prev_browser)
        browser->StackBelowOtherBrowserWindow(
            prev_browser,
            shadow_policy,
            StackingManager::LAYER_INACTIVE_BROWSER_WINDOW);
      else
        browser->StackAtTopOfLayer(
            StackingManager::LAYER_INACTIVE_BROWSER_WINDOW,
            shadow_policy,
            StackingManager::LAYER_INACTIVE_BROWSER_WINDOW);
      prev_browser = browser;
    }
  }
}

void LayoutManager2::ArrangeBrowserWindows(
    XTime timestamp, ArrangeFocusPolicy focus_policy, int anim_ms) {
  if (browser_windows_.empty())
    return;

  vector<Rect> bounds;
  int left_offscreen_index = -1, right_offscreen_index = -1;
  ComputeBrowserWindowBounds(&bounds,
                             &left_offscreen_index,
                             &right_offscreen_index);

  // Make sure that we're compositing before we start moving windows around;
  // otherwise, if we're not currently compositing, making the active browser
  // slide offscreen will look janky -- when we move its X window offscreen,
  // it'll instantaneously disappear.
  scoped_ptr<WindowManager::ScopedCompositingRequest> comp_request(
      wm_->CreateScopedCompositingRequest());

  if (fullscreen_browser_) {
    fullscreen_browser_->SetStatusAreaDisplayed(true);
    fullscreen_browser_->SetBounds(wm_->root_bounds(), 0);
    fullscreen_browser_->SetDimmed(false, 0);
    fullscreen_browser_->SetTransientWindowVisibility(true);
    if (focus_policy == ASSIGN_FOCUS_DURING_ARRANGE)
      fullscreen_browser_->TakeFocus(timestamp);
  }

  BrowserWindow* active_browser = GetActiveBrowserWindow();
  DCHECK(active_browser);
  if (focus_policy == ASSIGN_FOCUS_DURING_ARRANGE && !fullscreen_browser_)
    active_browser->TakeFocus(timestamp);

  int secondary_browser_index = -1;
  if (!maximized_ && num_browser_windows() > 1) {
    secondary_browser_index =
        active_browser_index_ + (active_browser->anchored_to_left() ? 1 : -1);
    DCHECK_GE(secondary_browser_index, 0);
    DCHECK_LT(secondary_browser_index, num_browser_windows());
  }

  RestackBrowserWindows(secondary_browser_index,
                        left_offscreen_index,
                        right_offscreen_index);

  for (int i = 0; i < num_browser_windows(); ++i) {
    BrowserWindow* browser = browser_windows_[i].get();

    if (browser == fullscreen_browser_)
      continue;

    browser->SetTransientWindowVisibility(
        browser == active_browser && !fullscreen_browser_);

    // Is the window either partially or entirely offscreen?
    const bool offscreen =
        (maximized_ && i != active_browser_index_) ||
        (left_offscreen_index >= 0 && i <= left_offscreen_index) ||
        (right_offscreen_index >= 0 && i >= right_offscreen_index);

    // Is the window onscreen and extending to the right edge (disregarding any
    // sliver of a window that we may be showing due to
    // FLAGS_show_window_edges)?
    const bool on_right =
        (maximized_ && i == active_browser_index_) ||
        (i == active_browser_index_ && !browser->anchored_to_left()) ||
        (i != active_browser_index_ && !offscreen &&
         active_browser->anchored_to_left());
    browser->SetStatusAreaDisplayed(on_right);

    browser->SetMaximizedProperty(maximized_);
    browser->SetBounds(bounds[i], anim_ms);

    const bool should_dim = offscreen && !maximized_;
    browser->SetDimmed(should_dim, should_dim ? anim_ms : 0);
  }

  ConfigureResizeHandle();
  ConfigureEdgeInputWindows(left_offscreen_index, right_offscreen_index);

  // If there's a pending request to arrange the windows after a particular
  // window gets updated, there's no need to honor it now.
  window_for_arrange_after_pixmap_fetch_ = NULL;
}

void LayoutManager2::SetActiveBrowserWindowIndex(int browser_index) {
  DCHECK_GE(browser_index, 0);
  DCHECK_LT(browser_index, num_browser_windows());

  const int old_index = active_browser_index_;
  active_browser_index_ = browser_index;
  DLOG(INFO) << "Activating browser window at index " << browser_index
             << " (old was " << old_index << ")";

  BrowserWindow::AnchorPosition anchoring = BrowserWindow::ANCHOR_LEFT;
  if (browser_index == 0)
    anchoring = BrowserWindow::ANCHOR_LEFT;
  else if (browser_index == num_browser_windows() - 1)
    anchoring = BrowserWindow::ANCHOR_RIGHT;
  else if (browser_index >= old_index)
    anchoring = BrowserWindow::ANCHOR_RIGHT;

  BrowserWindow* active_browser = GetActiveBrowserWindow();
  active_browser->set_unmaximized_anchoring(anchoring);
}

void LayoutManager2::CycleActiveBrowserWindow(bool forward) {
  if (browser_windows_.empty())
    return;

  if (wm_->key_bindings()->current_event_time()) {
    const KeyBindings::KeyCombo& combo =
        wm_->key_bindings()->current_key_combo();
    if (forward) {
      if (combo.keysym == XK_Tab)
        wm_->ReportUserAction("Accel_NextWindow_Tab");
      else if (combo.keysym == XK_F5)
        wm_->ReportUserAction("Accel_NextWindow_F5");
    } else {
      if (combo.keysym == XK_Tab)
        wm_->ReportUserAction("Accel_PrevWindow_Tab");
      else if (combo.keysym == XK_F5)
        wm_->ReportUserAction("Accel_PrevWindow_F5");
    }
  }

  if (num_browser_windows() == 1) {
    BrowserWindow* active_browser = GetActiveBrowserWindow();
    DCHECK(active_browser);
    active_browser->TakeFocus(GetCurrentXTime());
    active_browser->DoNudgeAnimation(forward);
    return;
  }

  if (fullscreen_browser_)
    SetFullscreenBrowserWindow(NULL);

  const int new_index =
      (active_browser_index_ + num_browser_windows() + (forward ? 1 : -1)) %
      num_browser_windows();
  const int anim_ms = GetWindowAnimationMs(new_index);
  SetActiveBrowserWindowIndex(new_index);
  ArrangeBrowserWindows(
      GetCurrentXTime(), ASSIGN_FOCUS_DURING_ARRANGE, anim_ms);
}

void LayoutManager2::ActivateBrowserWindowByIndex(int browser_index) {
  if (browser_index < 0 || browser_index >= num_browser_windows())
    return;
  if (browser_index == active_browser_index_)
    return;

  const int anim_ms = GetWindowAnimationMs(browser_index);
  SetActiveBrowserWindowIndex(browser_index);
  ArrangeBrowserWindows(
      GetCurrentXTime(), ASSIGN_FOCUS_DURING_ARRANGE, anim_ms);
}

void LayoutManager2::ToggleMaximized() {
  if (num_browser_windows() < 2)
    return;

  if (fullscreen_browser_)
    SetFullscreenBrowserWindow(NULL);

  maximized_ = !maximized_;

  BrowserWindow* active_browser = GetActiveBrowserWindow();
  Window* active_win = active_browser->win();

  if (maximized_)
    active_browser->SetStatusAreaDisplayed(true);

  // Resize the active window first.
  vector<Rect> bounds;
  ComputeBrowserWindowBounds(&bounds, NULL, NULL);
  scoped_ptr<WindowManager::ScopedCompositingRequest> comp_request(
      !maximized_ ? wm_->CreateScopedCompositingRequest() : NULL);
  active_browser->SetMaximizedProperty(maximized_);
  active_browser->SetBounds(bounds[active_browser_index_], 0);

  if (!maximized_ || active_win->client_has_redrawn_after_last_resize()) {
    // Resize and arrange the other windows if we're unmaximizing or if the
    // active window has already been repainted after the resize (being
    // repainted already is basically impossible here; it will only be true if
    // the window doesn't support _NET_WM_SYNC_REQUEST).
    ArrangeBrowserWindows(GetCurrentXTime(), ASSIGN_FOCUS_DURING_ARRANGE, 0);
  } else {
    // Otherwise, wait to update the other windows until after the active window
    // has been repainted.  If we updated them immediately, there would be an
    // ugly moment where the active window isn't yet repainted at its maximized
    // size but the other windows are moved away, leaving a black area onscreen.
    window_for_arrange_after_pixmap_fetch_ = active_browser->win();
  }
}

void LayoutManager2::ChangeActiveBrowserUnmaximizedWidth(int new_width) {
  BrowserWindow* active_browser = GetActiveBrowserWindow();
  DCHECK(active_browser);
  if (active_browser->unmaximized_width() == new_width)
    return;

  active_browser->set_unmaximized_width(new_width);
  if (maximized_)
    return;

  vector<Rect> bounds;
  ComputeBrowserWindowBounds(&bounds, NULL, NULL);
  active_browser->SetBounds(bounds[active_browser_index_], 0);
  ConfigureResizeHandle();
}

void LayoutManager2::ResizeActiveBrowserWindowIncrementally(bool to_right) {
  if (browser_windows_.empty())
    return;

  BrowserWindow* active_browser = GetActiveBrowserWindow();

  const bool should_widen =
      (to_right && active_browser->anchored_to_left()) ||
      (!to_right && !active_browser->anchored_to_left());
  const int cur_width = active_browser->unmaximized_width();
  const int max_width = GetMaxUnmaximizedWidth();
  const int min_width = GetMinUnmaximizedWidth();

  if (maximized_) {
    if (!should_widen) {
      ChangeActiveBrowserUnmaximizedWidth(max_width);
      ToggleMaximized();
    }
    return;
  }

  if (should_widen && cur_width >= max_width) {
    ToggleMaximized();
    return;
  }

  const double increment =
      static_cast<double>(max_width - min_width) / kNumIncrementalResizeSteps;

  int new_width = 0;
  if (should_widen) {
    const int min_new_size =
        cur_width + kIncrementalResizeMinChangePercent * increment;
    for (int i = 0; i <= kNumIncrementalResizeSteps; ++i) {
      new_width = round(min_width + i * increment);
      if (new_width >= min_new_size)
        break;
    }
  } else {
    const int max_new_size =
        cur_width - kIncrementalResizeMinChangePercent * increment;
    for (int i = 0; i <= kNumIncrementalResizeSteps; ++i) {
      new_width = round(max_width - i * increment);
      if (new_width <= max_new_size)
        break;
    }
  }

  ChangeActiveBrowserUnmaximizedWidth(ConstrainUnmaximizedWidth(new_width));
}

void LayoutManager2::ConfigureResizeHandle() {
  if (maximized_ || fullscreen_browser_ || browser_windows_.empty()) {
    wm_->xconn()->ConfigureWindowOffscreen(resize_handle_xid_);
  } else {
    BrowserWindow* browser = GetActiveBrowserWindow();
    int x = browser->win()->client_x() +
            (browser->anchored_to_left() ? browser->unmaximized_width() : 0);
    if (!browser->anchored_to_left())
      x -= kResizeHandleWidthPixels;
    Rect bounds(x, 0, kResizeHandleWidthPixels, wm_->height());
    wm_->ConfigureInputWindow(resize_handle_xid_, bounds);

    XID cursor = browser->anchored_to_left() ? right_cursor_ : left_cursor_;
    wm_->xconn()->SetWindowCursor(resize_handle_xid_, cursor);
  }
}

void LayoutManager2::ConfigureEdgeInputWindows(int left_offscreen_index,
                                               int right_offscreen_index) {
  if (left_offscreen_index != -1) {
    wm_->ConfigureInputWindow(
        left_edge_input_xid_, Rect(0, 0, kEdgeGapPixels, wm_->height()));
  } else {
    wm_->xconn()->ConfigureWindowOffscreen(left_edge_input_xid_);
  }

  if (right_offscreen_index != -1) {
    wm_->ConfigureInputWindow(
        right_edge_input_xid_,
        Rect(wm_->width() - kEdgeGapPixels, 0, kEdgeGapPixels, wm_->height()));
  } else {
    wm_->xconn()->ConfigureWindowOffscreen(right_edge_input_xid_);
  }
}

void LayoutManager2::HandleResizeDragStart(const Point& absolute_pos) {
  if (browser_windows_.empty() || maximized_)
    return;

  BrowserWindow* active_browser = GetActiveBrowserWindow();
  in_resize_drag_ = true;
  resize_drag_start_pos_ = absolute_pos;
  resize_width_at_start_ = active_browser->unmaximized_width();

  resize_box_.reset(new ResizeBox(wm_->compositor()));
  Rect bounds(active_browser->win()->client_x(), 0,
              active_browser->unmaximized_width(), wm_->height());
  resize_box_->SetBounds(bounds, 0);
  resize_box_->actor()->SetOpacity(kResizeBoxOpacity, 0);
  resize_box_->actor()->Show();
  wm_->stage()->AddActor(resize_box_->actor());
}

void LayoutManager2::HandleResizeDragMotion(const Point& absolute_pos) {
  if (browser_windows_.empty() || !in_resize_drag_)
    return;

  const int dx = absolute_pos.x - resize_drag_start_pos_.x;
  BrowserWindow* active_browser = GetActiveBrowserWindow();
  int width =
      resize_width_at_start_ +
      (active_browser->anchored_to_left() ? 1 : -1) * dx;
  width = ConstrainUnmaximizedWidth(width);

  Rect bounds(active_browser->win()->client_x(), 0, width, wm_->height());
  if (!active_browser->anchored_to_left())
    bounds.x -= (width - resize_width_at_start_);
  resize_box_->SetBounds(bounds, 0);
}

void LayoutManager2::HandleResizeDragEnd(const Point& absolute_pos) {
  if (browser_windows_.empty() || !in_resize_drag_)
    return;

  in_resize_drag_ = false;
  resize_box_.reset();

  const int dx = absolute_pos.x - resize_drag_start_pos_.x;
  BrowserWindow* active_browser = GetActiveBrowserWindow();
  int new_width =
      active_browser->unmaximized_width() +
      (active_browser->anchored_to_left() ? 1 : -1) * dx;
  ChangeActiveBrowserUnmaximizedWidth(ConstrainUnmaximizedWidth(new_width));
}

void LayoutManager2::FocusBrowserWindow(BrowserWindow* browser) {
  if (fullscreen_browser_ && browser != fullscreen_browser_) {
    SetFullscreenBrowserWindow(NULL);
    ArrangeBrowserWindows(GetCurrentXTime(), ASSIGN_FOCUS_DURING_ARRANGE, 0);
  }

  if (browser == GetActiveBrowserWindow()) {
    browser->TakeFocus(GetCurrentXTime());
  } else {
    SetActiveBrowserWindowIndex(GetBrowserWindowIndex(*browser));
    ArrangeBrowserWindows(
        GetCurrentXTime(), ASSIGN_FOCUS_DURING_ARRANGE, kWindowAnimMs);
  }
}

void LayoutManager2::SetFullscreenBrowserWindow(BrowserWindow* browser) {
  if (browser == fullscreen_browser_)
    return;

  if (fullscreen_browser_) {
    fullscreen_browser_->SetFullscreenProperty(false);
    fullscreen_browser_ = NULL;
  }

  if (browser) {
    browser->SetFullscreenProperty(true);
    browser->SetBounds(wm_->root_bounds(), 0);
    fullscreen_browser_ = browser;
  }
}

void LayoutManager2::HandleFullscreenRequest(Window* win, bool fullscreen) {
  BrowserWindow* browser = FindBrowserWindowByXid(win->xid());
  if (!browser)
    return;

  DLOG(INFO) << "Got _NET_WM_STATE request to make " << win->xid_str()
             << (fullscreen ? " fullscreen" : " not fullscreen");


  if (!fullscreen && fullscreen_browser_ == browser) {
    SetFullscreenBrowserWindow(NULL);
  } else if (fullscreen) {
    scoped_ptr<WindowManager::ScopedCompositingRequest> comp_request(
        wm_->CreateScopedCompositingRequest());
    SetFullscreenBrowserWindow(browser);
  }

  if (fullscreen && !win->client_has_redrawn_after_last_resize())
    window_for_arrange_after_pixmap_fetch_ = win;
  else
    ArrangeBrowserWindows(GetCurrentXTime(), ASSIGN_FOCUS_DURING_ARRANGE, 0);
}

void LayoutManager2::HandleModalityRequest(Window* win, bool modal) {
  BrowserWindow* owning_browser = FindBrowserWindowOwningTransientWindow(*win);
  if (!owning_browser)
    return;

  DLOG(INFO) << "Got _NET_WM_STATE request to make " << win->xid_str()
             << (modal ? " modal" : " not modal");
  map<XAtom, bool> new_state;
  new_state[wm_->GetXAtom(ATOM_NET_WM_STATE_MODAL)] = modal;
  win->ChangeWmState(new_state);

  if (modal)
    FocusBrowserWindow(owning_browser);
}

void LayoutManager2::HandleActiveWindowRequest(Window* win, XTime timestamp) {
  int index = FindBrowserWindowIndexByWindow(*win);
  if (index == -1) {
    BrowserWindow* browser = FindBrowserWindowOwningTransientWindow(*win);
    if (!browser)
      return;
    browser->SetPreferredTransientWindowToFocus(win);
    index = GetBrowserWindowIndex(*browser);
  }

  DLOG(INFO) << "Got _NET_ACTIVE_WINDOW request to focus " << win->xid_str();

  // Ignore the request if a modal window is already focused.
  if (wm_->IsModalWindowFocused())
    return;

  if (fullscreen_browser_ &&
      browser_windows_[index].get() != fullscreen_browser_)
    SetFullscreenBrowserWindow(NULL);

  if (index == active_browser_index_) {
    FocusBrowserWindow(browser_windows_[index].get());
  } else {
    SetActiveBrowserWindowIndex(index);
    ArrangeBrowserWindows(
        timestamp, ASSIGN_FOCUS_DURING_ARRANGE, kWindowAnimMs);
  }
}

}  // namespace window_manager
