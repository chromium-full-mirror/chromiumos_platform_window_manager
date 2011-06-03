// Copyright (c) 2011 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <map>

#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include "base/logging.h"
#include "window_manager/test_lib.h"
#include "window_manager/transient_window_collection.h"
#include "window_manager/stacking_manager.h"
#include "window_manager/window.h"
#include "window_manager/window_manager.h"

DEFINE_bool(logtostderr, false,
            "Print debugging messages to stderr (suppressed otherwise)");

using std::map;

namespace window_manager {

class TransientWindowCollectionTest : public BasicWindowManagerTest {};

TEST_F(TransientWindowCollectionTest, Stacking) {
  // Create an owner window and stack it in the fullscreen layer.
  XWindow owner_xid = CreateSimpleWindow();
  XConnection::WindowGeometry geometry;
  ASSERT_TRUE(xconn_->GetWindowGeometry(owner_xid, &geometry));
  Window owner_win(wm_.get(), owner_xid, false, geometry);
  wm_->stacking_manager()->StackWindowAtTopOfLayer(
      &owner_win,
      StackingManager::LAYER_FULLSCREEN_WINDOW,
      StackingManager::SHADOW_AT_BOTTOM_OF_LAYER);

  TestEventConsumer event_consumer;
  TransientWindowCollection transients(
      &owner_win,
      TransientWindowCollection::STACK_ABOVE_OWNER,
      TransientWindowCollection::CENTER_OVER_OWNER,
      TransientWindowCollection::KEEP_ONSCREEN_ALWAYS,
      &event_consumer);

  // Create a window to treat as transient.
  XWindow transient_xid = CreateSimpleWindow();
  ASSERT_TRUE(xconn_->GetWindowGeometry(transient_xid, &geometry));
  Window transient_win(wm_.get(), transient_xid, false, geometry);

  // After the window is added to the collection, it should be stacked above its
  // owner.
  transients.AddWindow(&transient_win);
  EXPECT_TRUE(WindowIsInLayer(&transient_win,
                              StackingManager::LAYER_FULLSCREEN_WINDOW));
  MockCompositor::StageActor* stage = compositor_->GetDefaultStage();
  EXPECT_LT(stage->GetStackingIndex(transient_win.GetBottomActor()),
            stage->GetStackingIndex(owner_win.GetTopActor()));

  // Now change the stacking policy and check that the transient is restacked.
  transients.set_stacking_policy(TransientWindowCollection::STACK_IN_LAYER);
  transients.ApplyStackingForAllWindows();
  EXPECT_TRUE(WindowIsInLayer(&transient_win,
                              StackingManager::LAYER_ACTIVE_TRANSIENT_WINDOW));

  // Add a second transient.  It should get stacked on top.
  XWindow transient_xid2 = CreateSimpleWindow();
  ASSERT_TRUE(xconn_->GetWindowGeometry(transient_xid2, &geometry));
  Window transient_win2(wm_.get(), transient_xid2, false, geometry);
  transients.AddWindow(&transient_win2);
  EXPECT_LT(stage->GetStackingIndex(transient_win2.GetBottomActor()),
            stage->GetStackingIndex(transient_win.GetTopActor()));

  // Make the second transient modal.
  map<XAtom, bool> wm_state;
  wm_state[xconn_->GetAtomOrDie("_NET_WM_STATE_MODAL")] = true;
  transient_win2.ChangeWmState(wm_state);
  ASSERT_TRUE(transient_win2.wm_state_modal());

  // Now add a third, non-modal transient and check that it gets stacked below
  // the second transient.
  XWindow transient_xid3 = CreateSimpleWindow();
  ASSERT_TRUE(xconn_->GetWindowGeometry(transient_xid3, &geometry));
  Window transient_win3(wm_.get(), transient_xid3, false, geometry);
  transients.AddWindow(&transient_win3);
  EXPECT_LT(stage->GetStackingIndex(transient_win2.GetBottomActor()),
            stage->GetStackingIndex(transient_win3.GetTopActor()));
  EXPECT_LT(stage->GetStackingIndex(transient_win3.GetBottomActor()),
            stage->GetStackingIndex(transient_win.GetTopActor()));

  // If all of the windows are modal, then a fourth, non-modal transient should
  // get stacked on the bottom.
  transient_win.ChangeWmState(wm_state);
  transient_win3.ChangeWmState(wm_state);
  XWindow transient_xid4 = CreateSimpleWindow();
  ASSERT_TRUE(xconn_->GetWindowGeometry(transient_xid4, &geometry));
  Window transient_win4(wm_.get(), transient_xid4, false, geometry);
  transients.AddWindow(&transient_win4);
  EXPECT_LT(stage->GetStackingIndex(transient_win2.GetBottomActor()),
            stage->GetStackingIndex(transient_win3.GetTopActor()));
  EXPECT_LT(stage->GetStackingIndex(transient_win3.GetBottomActor()),
            stage->GetStackingIndex(transient_win.GetTopActor()));
  EXPECT_LT(stage->GetStackingIndex(transient_win.GetBottomActor()),
            stage->GetStackingIndex(transient_win4.GetTopActor()));

  transients.RemoveWindow(&transient_win);
  transients.RemoveWindow(&transient_win2);
  transients.RemoveWindow(&transient_win3);
  transients.RemoveWindow(&transient_win4);
}

}  // namespace window_manager

int main(int argc, char** argv) {
  return window_manager::InitAndRunTests(&argc, argv, &FLAGS_logtostderr);
}
