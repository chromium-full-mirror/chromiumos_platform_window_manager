// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_COMPOSITOR_LAYER_VISITOR_H_
#define WINDOW_MANAGER_COMPOSITOR_LAYER_VISITOR_H_

#include "window_manager/compositor/real_compositor.h"

namespace window_manager {

// LayerVisitor is used to update actors' opacities, transformation
// matrices and culling information.  It traverses through the actor tree
// before DrawVisitor on each frame.  LayerVisitor keeps information about the
// composition of the actors during the traversal, and the information is used
// to help RealCompositor and DrawCompositor perform optimizations.
class LayerVisitor : virtual public RealCompositor::ActorVisitor {
 public:
  LayerVisitor(int32 count, bool use_partial_updates)
      : count_(count),
        has_fullscreen_actor_(false),
        stage_actor_(NULL),
        visiting_top_visible_actor_(true),
        top_fullscreen_actor_(NULL),
        damaged_region_(Rect()),
        use_partial_updates_(use_partial_updates) {}
  virtual ~LayerVisitor() {}

  bool has_fullscreen_actor() const { return has_fullscreen_actor_; }
  const RealCompositor::TexturePixmapActor* top_fullscreen_actor() const {
    return top_fullscreen_actor_;
  }

  virtual void VisitActor(RealCompositor::Actor* actor);
  virtual void VisitStage(RealCompositor::StageActor* actor);
  virtual void VisitContainer(RealCompositor::ContainerActor* actor);
  virtual void VisitQuad(RealCompositor::QuadActor* actor);
  virtual void VisitImage(RealCompositor::ImageActor* actor);
  virtual void VisitTexturePixmap(RealCompositor::TexturePixmapActor* actor);

  void VisitTexturedQuadActor(RealCompositor::QuadActor* actor,
      bool is_texture_opaque);

  // Get the damaged region in screen coordinates where (0, 0) is bottom_left
  // and (w-1, h-1) is top_right.
  const Region& damaged_region() { return damaged_region_; }

 private:
  int32 count_;
  bool has_fullscreen_actor_;
  const RealCompositor::StageActor* stage_actor_;

  // This flag indicates whether the actor being visited is the topmost
  // visible actor.
  bool visiting_top_visible_actor_;

  // The screen clip region for the currently visited actor. Clip is in
  // screen coordinates where (0, 0) is bottom_left and (w-1, h-1) is
  // top_right.
  Region visiting_clip_;

  // This keeps track of the actor that is fullscreen and topmost visible
  // during the traversal. It is set to NULL if either of the criteria is
  // not satisfied.
  const RealCompositor::TexturePixmapActor* top_fullscreen_actor_;

  // This restores the dirty region union of all actors from the most
  // recent VisitStage.  It's defined in screen coordinates where (0, 0)
  // is bottom_left and (w-1, h-1) is top_right.
  Region damaged_region_;

  bool use_partial_updates_;

  DISALLOW_COPY_AND_ASSIGN(LayerVisitor);
};

}  // namespace window_manager

#endif  // WINDOW_MANAGER_COMPOSITOR_LAYER_VISITOR_H_
