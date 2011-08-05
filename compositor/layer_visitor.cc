// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/compositor/layer_visitor.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "window_manager/compositor/real_compositor.h"
#include "window_manager/geometry.h"
#include "window_manager/util.h"

using std::ceil;
using std::max;
using std::min;
using std::vector;

namespace window_manager {

namespace {

const float kBoundingBoxEpsilon = 0.0001;

enum CullingResult {
  CULLING_WINDOW_OFFSCREEN,
  CULLING_WINDOW_ONSCREEN,
  CULLING_WINDOW_FULLSCREEN
};

struct BoundingBox {
  BoundingBox() : x_min(0.0f), x_max(0.0f), y_min(0.0f), y_max(0.0f) {}
  BoundingBox(float x0, float x1, float y0, float y1)
      : x_min(x0),
        x_max(x1),
        y_min(y0),
        y_max(y1) {}

  void merge(const BoundingBox& other) {
    if (other.x_min == other.x_max || other.y_min == other.y_max)
      return;
    if (x_min == x_max || y_min == y_max) {
      x_min = other.x_min;
      x_max = other.x_max;
      y_min = other.y_min;
      y_max = other.y_max;
    } else {
      x_min = std::min(x_min, other.x_min);
      x_max = std::max(x_max, other.x_max);
      y_min = std::min(y_min, other.y_min);
      y_max = std::max(y_max, other.y_max);
    }
  }

  void clear() {
    x_min = 0.f;
    x_max = 0.f;
    y_min = 0.f;
    y_max = 0.f;
  }

  float x_min;
  float x_max;
  float y_min;
  float y_max;
};

inline float min4(float a, float b, float c, float d) {
  return min(min(min(a, b), c), d);
}

inline float max4(float a, float b, float c, float d) {
  return max(max(max(a, b), c), d);
}

// The input region is in window coordinates where top_left is (0, 0) and
// bottom_right is (1, 1).  Output is the bounding box of the transformed window
// in GL coordinates where bottom_left is (-1, -1) and top_right is (1, 1).
BoundingBox ComputeTransformedBoundingBox(
    const RealCompositor::StageActor& stage,
    const RealCompositor::QuadActor& actor,
    const BoundingBox& region) {
  const Matrix4& transform = stage.projection() * actor.model_view();

  Vector4 v0(region.x_min, region.y_min, 0, 1);
  Vector4 v1(region.x_min, region.y_max, 0, 1);
  Vector4 v2(region.x_max, region.y_max, 0, 1);
  Vector4 v3(region.x_max, region.y_min, 0, 1);

  v0 = transform * v0;
  v1 = transform * v1;
  v2 = transform * v2;
  v3 = transform * v3;

  v0 /= v0[3];
  v1 /= v1[3];
  v2 /= v2[3];
  v3 /= v3[3];

  return BoundingBox(min4(v0[0], v1[0], v2[0], v3[0]),
                     max4(v0[0], v1[0], v2[0], v3[0]),
                     min4(v0[1], v1[1], v2[1], v3[1]),
                     max4(v0[1], v1[1], v2[1], v3[1]));
}

// The input region is defined in the actor's window coordinates.
BoundingBox MapRegionToGlCoordinates(
    const RealCompositor::StageActor& stage,
    const RealCompositor::TexturePixmapActor& actor,
    const Rect& region) {
  DCHECK(actor.width() > 0 && actor.height() > 0);
  float x_min = region.x;
  x_min /= actor.width();
  float x_max = region.x + region.width;
  x_max /= actor.width();
  float y_min = region.y;
  y_min /= actor.height();
  float y_max = region.y + region.height;
  y_max /= actor.height();

  BoundingBox box(x_min, x_max, y_min, y_max);
  return ComputeTransformedBoundingBox(stage, actor, box);
}

// The output region is defined in the screen coordinates.
BoundingBox MapGlCoordinatesToStage(const BoundingBox& box,
                                    const Size& stage_size) {
  float x_min = (box.x_min + 1.f) / 2.f * stage_size.width;
  float y_min = (box.y_min + 1.f) / 2.f * stage_size.height;
  float x_max = (box.x_max + 1.f) / 2.f * stage_size.width;
  float y_max = (box.y_max + 1.f) / 2.f * stage_size.height;
  return BoundingBox(x_min, x_max, y_min, y_max);
}

// Output is rounded such that screen aligned regions are preserved properly.
Rect MapGlCoordinatesToClipRect(const BoundingBox& box,
                                const Size& stage_size) {
  const BoundingBox stage_box = MapGlCoordinatesToStage(box, stage_size);
  Rect region;
  region.x = static_cast<int>(stage_box.x_min + .5f);
  region.y = static_cast<int>(stage_box.y_min + .5f);
  region.width = static_cast<int>(stage_box.x_max + .5f) - region.x;
  region.height = static_cast<int>(stage_box.y_max + .5f) - region.y;
  return region;
}

// Output is rounded such that all partially covered pixels are included.
Rect MapGlCoordinatesToBounds(const BoundingBox& box, const Size& stage_size) {
  const BoundingBox stage_box = MapGlCoordinatesToStage(box, stage_size);
  Rect region;
  // Important: To be properly conservative, the differences below need to
  // happen after the conversion to int.
  region.x = static_cast<int>(stage_box.x_min);
  region.y = static_cast<int>(stage_box.y_min);
  region.width = static_cast<int>(ceil(stage_box.x_max)) - region.x;
  region.height = static_cast<int>(ceil(stage_box.y_max)) - region.y;
  return region;
}

bool IsBoxOffscreen(const BoundingBox& box) {
  static const float kAlmostOne = 1.f - kBoundingBoxEpsilon;
  return box.x_max <= -kAlmostOne || box.x_min >= kAlmostOne ||
         box.y_max <= -kAlmostOne || box.y_min >= kAlmostOne;
}

bool IsBoxFullscreen(const BoundingBox& box) {
  static const float kAlmostOne = 1.f - kBoundingBoxEpsilon;
  return box.x_max >= kAlmostOne && box.x_min <= -kAlmostOne &&
         box.y_max >= kAlmostOne && box.y_min <= -kAlmostOne;
}

CullingResult PerformActorCullingTest(const BoundingBox& box) {
  if (IsBoxOffscreen(box))
    return CULLING_WINDOW_OFFSCREEN;
  if (IsBoxFullscreen(box))
    return CULLING_WINDOW_FULLSCREEN;
  return CULLING_WINDOW_ONSCREEN;
}

}  // namespace

void LayerVisitor::VisitActor(RealCompositor::Actor* actor) {
  actor->set_is_opaque(actor->opacity() > 0.999f);
}

void LayerVisitor::VisitStage(
    RealCompositor::StageActor* actor) {
  if (!actor->IsVisible())
    return;

  stage_actor_ = actor;
  top_fullscreen_actor_ = NULL;
  visiting_top_visible_actor_ = true;
  visiting_clip_.Reset(Rect(0, 0, actor->width(), actor->height()));
  has_fullscreen_actor_ = false;

  if (use_partial_updates_)
    damaged_region_.Reset(Rect());

  actor->UpdateProjection();
  VisitContainer(actor);
}

void LayerVisitor::VisitContainer(
    RealCompositor::ContainerActor* actor) {
  CHECK(actor);
  if (!actor->IsVisible())
    return;

  // No culling test for ContainerActor because the container does not bound
  // its children actors.  No need to set_z first because container doesn't
  // use z in its model view matrix.
  actor->UpdateModelView();

  RealCompositor::ActorVector children = actor->GetChildren();
  for (RealCompositor::ActorVector::const_iterator it = children.begin();
       it != children.end(); ++it) {
    if (*it)
      (*it)->Accept(this);
  }

  // The containers should be "further" than all their children.
  this->VisitActor(actor);
}

void LayerVisitor::VisitTexturedQuadActor(
    RealCompositor::QuadActor* actor, bool is_texture_opaque) {
  actor->set_culled(has_fullscreen_actor_);
  actor->set_clip(visiting_clip_);
  if (!actor->IsVisible())
    return;

  VisitActor(actor);
  actor->set_is_opaque(actor->is_opaque() && is_texture_opaque);

  // Must update model view matrix before culling test.
  actor->UpdateModelView();

  static const BoundingBox kRegion(0, 1, 0, 1);
  const BoundingBox box =
      ComputeTransformedBoundingBox(*stage_actor_, *actor, kRegion);
  const CullingResult result = PerformActorCullingTest(box);

  const Size stage_size = stage_actor_->GetBounds().size();
  const Rect bounds = MapGlCoordinatesToBounds(box, stage_size);
  Region clip(visiting_clip_);
  clip.Intersect(bounds);

  actor->set_clip(clip);
  actor->set_culled(result == CULLING_WINDOW_OFFSCREEN);
  if (actor->culled() || actor->clip().IsEmpty())
    return;

  const bool quad_is_screen_aligned =
      !actor->IsTransformed() &&
      stage_actor_->using_passthrough_projection();

  // Don't claim that scaled actors are fullscreen, even if they're covering the
  // whole screen.  We need to continue compositing in this case so the scaling
  // will be visible.
  if (actor->is_opaque() && result == CULLING_WINDOW_FULLSCREEN &&
      actor->GetXScale() == 1.0 && actor->GetYScale() == 1.0)
    has_fullscreen_actor_ = true;

  // Subtract opaque screen aligned quads from clip region to avoid
  // unnecessary drawing of occluded actors.
  if (actor->is_opaque() && quad_is_screen_aligned) {
    Rect rect = MapGlCoordinatesToClipRect(box, stage_size);
    visiting_clip_.Subtract(rect);
  }

  visiting_top_visible_actor_ = false;
}

void LayerVisitor::VisitQuad(
    RealCompositor::QuadActor* actor) {
  DCHECK(actor->texture_data() == NULL);
  VisitTexturedQuadActor(actor, true);
}

void LayerVisitor::VisitImage(
    RealCompositor::ImageActor* actor) {
  VisitTexturedQuadActor(actor, actor->IsImageOpaque());
}

void LayerVisitor::VisitTexturePixmap(
    RealCompositor::TexturePixmapActor* actor) {
  bool visiting_top_visible_actor = visiting_top_visible_actor_;
  // OpenGlPixmapData is not created until OpenGlDrawVisitor has traversed
  // through the tree, which happens after the LayerVisitor, so we cannot rely
  // on actor->texture_data()->has_alpha() because texture_data() is NULL
  // in the beginning.
  // TODO: combine VisitQuad and VisitTexturePixmap.
  VisitTexturedQuadActor(actor, actor->pixmap_is_opaque());

  if (!actor->IsVisible() || actor->width() <= 0 || actor->height() <= 0)
    return;

  if (visiting_top_visible_actor && has_fullscreen_actor_)
    top_fullscreen_actor_ = actor;

  if (use_partial_updates_) {
    const Size stage_size = stage_actor_->GetBounds().size();
    vector<Rect>::const_iterator it =
        actor->GetDamagedRegion().rects().begin();
    for (; it != actor->GetDamagedRegion().rects().end(); ++it) {
      BoundingBox region = MapRegionToGlCoordinates(*stage_actor_, *actor, *it);
      damaged_region_.Unite(MapGlCoordinatesToBounds(region, stage_size));
    }
  }
  actor->ResetDamagedRegion();
}

}  // namespace window_manager
