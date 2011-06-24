// Copyright (c) 2011 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/layout2/browser_window.h"

#include <map>

#include "base/time.h"
#include "cros/chromeos_wm_ipc_enums.h"
#include "window_manager/atom_cache.h"
#include "window_manager/callback.h"
#include "window_manager/event_consumer_registrar.h"
#include "window_manager/event_loop.h"
#include "window_manager/layout2/layout_manager2.h"
#include "window_manager/shadow.h"
#include "window_manager/stacking_manager.h"
#include "window_manager/transient_window_collection.h"
#include "window_manager/window.h"
#include "window_manager/window_manager.h"

using base::TimeDelta;
using std::map;

namespace {

// Distance over which we move the window for the no-op window-switching
// animation.
const int kNudgeAnimPixels = 25;

// Amount of time used for the no-op window-switching animation.
const int kNudgeAnimMs = 180;

}  // anonymous namespace

namespace window_manager {

BrowserWindow::BrowserWindow(Window* win,
                             LayoutManager2* layout_manager,
                             int unmaximized_width)
    : win_(win),
      event_consumer_registrar_(
          new EventConsumerRegistrar(wm(), layout_manager)),
      unmaximized_width_(unmaximized_width),
      unmaximized_anchoring_(ANCHOR_LEFT),
      status_area_displayed_(
          !win->chrome_state_xatoms().count(ATOM_CHROME_STATE_STATUS_HIDDEN)),
      brightness_(1.0),
      transients_(
          new TransientWindowCollection(
              win,  // owner_win
              TransientWindowCollection::STACK_IN_LAYER,
              TransientWindowCollection::CENTER_OVER_OWNER,
              TransientWindowCollection::KEEP_ONSCREEN_IF_OWNER_IS_ONSCREEN,
              layout_manager)),
      dimming_box_(
          wm()->compositor()->CreateColoredBox(
              win_->client_width(), win_->client_height(),
              Compositor::Color("#000"))) {
  DCHECK(ShouldHandleWindow(*win));
  wm()->focus_manager()->UseClickToFocusForWindow(
      win, FocusManager::PASS_CLICKS_THROUGH);
  event_consumer_registrar_->RegisterForWindowEvents(win->xid());

  win_->SetVisibility(Window::VISIBILITY_SHOWN);
  win_->SetShadowType(Shadow::TYPE_RECTANGULAR);

  dimming_box_->Move(win_->client_x(), win_->client_y(), 0);
  dimming_box_->SetOpacity(0, 0);
  wm()->stage()->AddActor(dimming_box_.get());
  wm()->stacking_manager()->StackActorRelativeToOtherActor(
      dimming_box_.get(),
      win_->GetTopActor(),
      StackingManager::ABOVE_SIBLING);
}

BrowserWindow::~BrowserWindow() {
  win_->SetVisibility(Window::VISIBILITY_HIDDEN);
  transients_->CloseAllWindows();
  win_ = NULL;
}

// static
bool BrowserWindow::ShouldHandleWindow(const Window& win) {
  if (win.transient_for_xid())
    return false;

  return win.type() == chromeos::WM_IPC_WINDOW_CHROME_TOPLEVEL ||
         win.type() == chromeos::WM_IPC_WINDOW_UNKNOWN;
}

WindowManager* BrowserWindow::wm() const {
  return win_->wm();
}

void BrowserWindow::SetBounds(const Rect& bounds, int anim_ms) {
  win_->SetBounds(bounds, anim_ms);
  dimming_box_->Move(bounds.x, bounds.y, anim_ms);
  dimming_box_->SetSize(bounds.width, bounds.height);
  transients_->ConfigureAllWindowsRelativeToOwner(anim_ms);
}

void BrowserWindow::StackAtTopOfLayer(
    StackingManager::Layer layer,
    StackingManager::ShadowPolicy shadow_policy,
    StackingManager::Layer shadow_layer) {
  wm()->stacking_manager()->StackWindowAtTopOfLayer(
      win_, layer, shadow_policy, shadow_layer);
  wm()->stacking_manager()->StackActorRelativeToOtherActor(
      dimming_box_.get(),
      win_->GetTopActor(),
      StackingManager::ABOVE_SIBLING);

  transients_->set_stacking_policy(
      layer == StackingManager::LAYER_FULLSCREEN_WINDOW ?
      TransientWindowCollection::STACK_ABOVE_OWNER :
      TransientWindowCollection::STACK_IN_LAYER);
  transients_->ApplyStackingForAllWindows();
}

void BrowserWindow::StackBelowOtherBrowserWindow(
    BrowserWindow* other_browser,
    StackingManager::ShadowPolicy shadow_policy,
    StackingManager::Layer shadow_layer) {
  DCHECK(other_browser);
  // The layer that we pass here is irrelevant since we're stacking our shadow
  // directly below our actor rather than at the bottom of the layer.
  wm()->stacking_manager()->StackWindowRelativeToOtherWindow(
      win_,
      other_browser->win(),
      StackingManager::BELOW_SIBLING,
      shadow_policy,
      shadow_layer);
  wm()->stacking_manager()->StackActorRelativeToOtherActor(
      dimming_box_.get(),
      win_->GetTopActor(),
      StackingManager::ABOVE_SIBLING);
  transients_->set_stacking_policy(TransientWindowCollection::STACK_IN_LAYER);
  transients_->ApplyStackingForAllWindows();
}

void BrowserWindow::TakeFocus(XTime timestamp) {
  if (!transients_->TakeFocus(timestamp))
    wm()->FocusWindow(win_, timestamp);
}

void BrowserWindow::HandleTransientWindowMap(Window* transient_win) {
  transients_->AddWindow(transient_win);
}

void BrowserWindow::HandleTransientWindowUnmap(Window* transient_win) {
  transients_->RemoveWindow(transient_win);
}

void BrowserWindow::HandleTransientWindowConfigureRequest(
    Window* transient_win, const Rect& requested_bounds) {
  transients_->HandleConfigureRequest(transient_win, requested_bounds);
}

bool BrowserWindow::IsWindowOrTransientFocused() const {
  return win_->IsFocused() || transients_->HasFocusedWindow();
}

void BrowserWindow::SetPreferredTransientWindowToFocus(Window* transient_win) {
  transients_->SetPreferredWindowToFocus(transient_win);
}

void BrowserWindow::SetTransientWindowVisibility(bool shown) {
  if (shown)
    transients_->Show();
  else
    transients_->Hide();
}

void BrowserWindow::DoNudgeAnimation(bool move_to_left) {
  AnimationPair* animations = win_->CreateMoveCompositedAnimation();
  animations->AppendKeyframe(
      win_->composited_x() + (move_to_left ? -1 : 1) * kNudgeAnimPixels,
      win_->composited_y(),
      TimeDelta::FromMilliseconds(kNudgeAnimMs / 2));
  animations->AppendKeyframe(
      win_->composited_x(), win_->composited_y(),
      TimeDelta::FromMilliseconds(kNudgeAnimMs / 2));
  win_->SetMoveCompositedAnimation(animations);
}

void BrowserWindow::SetBrightness(double brightness, int anim_ms) {
  DCHECK_GE(brightness, 0.0);
  DCHECK_LE(brightness, 1.0);
  brightness_ = brightness;
  dimming_box_->SetOpacity(1.0 - brightness, anim_ms);
}

double BrowserWindow::GetInstantaneousBrightness() const {
  return 1.0 - dimming_box_->GetOpacity();
}

void BrowserWindow::SetStatusAreaDisplayed(bool displayed) {
  if (status_area_displayed_ == displayed)
    return;

  status_area_displayed_ = displayed;
  map<XAtom, bool> states;
  states[wm()->GetXAtom(ATOM_CHROME_STATE_STATUS_HIDDEN)] = !displayed;
  win_->ChangeChromeState(states);
}

void BrowserWindow::SetFullscreenProperty(bool fullscreen) {
  if (win_->wm_state_fullscreen() == fullscreen)
    return;

  map<XAtom, bool> states;
  states[wm()->GetXAtom(ATOM_NET_WM_STATE_FULLSCREEN)] = fullscreen;
  win_->ChangeWmState(states);
}

void BrowserWindow::SetMaximizedProperty(bool maximized) {
  if (win_->wm_state_maximized_horz() == maximized &&
      win_->wm_state_maximized_vert() == maximized)
    return;

  map<XAtom, bool> states;
  if (win_->wm_state_maximized_horz() != maximized)
    states[wm()->GetXAtom(ATOM_NET_WM_STATE_MAXIMIZED_HORZ)] = maximized;
  if (win_->wm_state_maximized_vert() != maximized)
    states[wm()->GetXAtom(ATOM_NET_WM_STATE_MAXIMIZED_VERT)] = maximized;
  win_->ChangeWmState(states);
}

}  // namespace window_manager
