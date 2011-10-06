// Copyright (c) 2011 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <algorithm>
#include <cstdarg>

#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include "base/command_line.h"
#include "base/logging.h"
#include "base/memory/scoped_ptr.h"
#include "window_manager/compositor/compositor.h"
#include "window_manager/compositor/gl/mock_gl_interface.h"
#include "window_manager/compositor/gl/opengl_visitor.h"
#include "window_manager/compositor/real_compositor.h"
#include "window_manager/event_loop.h"
#include "window_manager/geometry.h"
#include "window_manager/test_lib.h"
#include "window_manager/util.h"
#include "window_manager/x11/mock_x_connection.h"

DEFINE_bool(logtostderr, false,
            "Print debugging messages to stderr (suppressed otherwise)");

namespace window_manager {

class OpenGlVisitorTest : public BasicCompositingTest {};

// Check that the viewport gets resized correctly when the stage is resized.
TEST_F(OpenGlVisitorTest, ResizeViewport) {
  RealCompositor::StageActor* stage = compositor_->GetDefaultStage();
  OpenGlDrawVisitor visitor(gl_.get(), compositor_.get(), stage);

  stage->Accept(&visitor);
  EXPECT_EQ(0, gl_->viewport().x);
  EXPECT_EQ(0, gl_->viewport().y);
  EXPECT_EQ(stage->width(), gl_->viewport().width);
  EXPECT_EQ(stage->height(), gl_->viewport().height);

  int new_width = stage->width() + 20;
  int new_height = stage->height() + 10;
  stage->SetSize(new_width, new_height);

  stage->Accept(&visitor);
  EXPECT_EQ(0, gl_->viewport().x);
  EXPECT_EQ(0, gl_->viewport().y);
  EXPECT_EQ(new_width, gl_->viewport().width);
  EXPECT_EQ(new_height, gl_->viewport().height);
}

// Check that the stage background color is used.
TEST_F(OpenGlVisitorTest, StageColor) {
  RealCompositor::StageActor* stage = compositor_->GetDefaultStage();

  stage->SetStageColor(Compositor::Color(0.f, 0.f, 0.f));
  OpenGlDrawVisitor visitor(gl_.get(), compositor_.get(), stage);
  stage->Accept(&visitor);
  EXPECT_FLOAT_EQ(0.f, gl_->clear_red());
  EXPECT_FLOAT_EQ(0.f, gl_->clear_green());
  EXPECT_FLOAT_EQ(0.f, gl_->clear_blue());
  EXPECT_FLOAT_EQ(1.f, gl_->clear_alpha());

  const float new_red = 0.86f, new_green = 0.24f, new_blue = 0.51f;
  stage->SetStageColor(Compositor::Color(new_red, new_green, new_blue));
  stage->Accept(&visitor);
  EXPECT_FLOAT_EQ(new_red, gl_->clear_red());
  EXPECT_FLOAT_EQ(new_green, gl_->clear_green());
  EXPECT_FLOAT_EQ(new_blue, gl_->clear_blue());
  EXPECT_FLOAT_EQ(1.f, gl_->clear_alpha());
}

// Check that we don't refresh a TexturePixmapActor's texture more than once per
// frame.  See http://crosbug.com/20971.
TEST_F(OpenGlVisitorTest, OnlyRefreshTextureOncePerFrame) {
  RealCompositor::StageActor* stage = compositor_->GetDefaultStage();

  XWindow xid = xconn_->CreateWindow(
      xconn_->GetRootWindow(),  // parent
      Rect(0, 0, 400, 300),
      false,  // override_redirect=false
      false,  // input_only=false
      0, 0);  // event_mask, visual
  XID pixmap_id = xconn_->GetCompositingPixmapForWindow(xid);
  scoped_ptr<RealCompositor::TexturePixmapActor> actor(
      compositor_->CreateTexturePixmap());
  stage->AddActor(actor.get());
  actor->SetPixmap(pixmap_id);

  // After drawing once, we should've updated the texture from the GLX pixmap
  // once.
  OpenGlDrawVisitor visitor(gl_.get(), compositor_.get(), stage);
  stage->Accept(&visitor);
  OpenGlPixmapData* pixmap_data =
      dynamic_cast<OpenGlPixmapData*>(actor->texture_data());
  CHECK(pixmap_data);
  GLXPixmap glx_pixmap = pixmap_data->glx_pixmap_;
  EXPECT_EQ(1, gl_->get_num_bindings_for_glx_pixmap(glx_pixmap));

  // If we tell the actor to update the texture multiple times, it should defer
  // actually doing the update until we draw again.
  actor->UpdateTexture();
  actor->UpdateTexture();
  actor->UpdateTexture();
  EXPECT_EQ(1, gl_->get_num_bindings_for_glx_pixmap(glx_pixmap));
  stage->Accept(&visitor);
  EXPECT_EQ(2, gl_->get_num_bindings_for_glx_pixmap(glx_pixmap));
}

}  // end namespace window_manager

int main(int argc, char** argv) {
  return window_manager::InitAndRunTests(&argc, argv, &FLAGS_logtostderr);
}
