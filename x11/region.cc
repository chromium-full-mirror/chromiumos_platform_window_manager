// Copyright (c) 2011 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/x11/region.h"

#include <ostream>

#include <X11/Xutil.h>
#include <X11/Xregion.h>

namespace window_manager {

Region::Region() : region_(XCreateRegion()) { }

Region::Region(const Rect& rect) : region_(XCreateRegion()) {
  XRectangle x_rect;
  x_rect.x = rect.x;
  x_rect.y = rect.y;
  x_rect.width = rect.width;
  x_rect.height = rect.height;
  XUnionRectWithRegion(&x_rect, region_, region_);
  UpdateRects();
}

Region::Region(const Region& region) : region_(XCreateRegion()) {
  Unite(region);
}

Region::~Region () {
  XDestroyRegion(region_);
}

Rect Region::GetBounds() const {
  XRectangle x_rect;
  XClipBox(region_, &x_rect);
  return Rect(x_rect.x, x_rect.y, x_rect.width, x_rect.height);
}

void Region::Reset(const Region& region) {
  Region empty;
  XSubtractRegion(region.region_, empty.region_, region_);
  UpdateRects();
}

void Region::Unite(const Region& region) {
  XUnionRegion(region_, region.region_, region_);
  UpdateRects();
}

void Region::Intersect(const Region& region) {
  XIntersectRegion(region_, region.region_, region_);
  UpdateRects();
}

void Region::Subtract(const Region& region) {
  XSubtractRegion(region_, region.region_, region_);
  UpdateRects();
}

bool Region::IsEmpty() const {
  return XEmptyRegion(region_);
}

bool Region::ContainsPoint(const Point& point) const {
  return XPointInRegion(region_, point.x, point.y) == True;
}

bool Region::ContainsRect(const Rect& rect) const {
  int result = XRectInRegion(region_, rect.x, rect.y, rect.width, rect.height);
  return result == RectangleIn;
}

bool Region::IsEqual(const Region& other) const {
  return XEqualRegion(region_, other.region_);
}

void Region::UpdateRects() {
  rects_.reserve(region_->numRects);
  rects_.resize(region_->numRects);

  for (int i = 0; i < region_->numRects; i++)
    rects_[i] = Rect(region_->rects[i].x1,
                     region_->rects[i].y1,
                     region_->rects[i].x2 - region_->rects[i].x1,
                     region_->rects[i].y2 - region_->rects[i].y1);
}

};

std::ostream& operator<<(std::ostream& out,
                         const window_manager::Region& region) {
  std::vector<window_manager::Rect>::const_iterator it = region.rects().begin();
  if (it != region.rects().end())
    out << *it++;
  while (it != region.rects().end())
    out << " " << *it++;
  return out;
}
