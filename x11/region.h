// Copyright (c) 2011 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_X11_REGION_H_
#define WINDOW_MANAGER_X11_REGION_H_

#include <vector>

#include "window_manager/geometry.h"

typedef struct _XRegion REGION;

namespace window_manager {

class Region {
 public:
  Region();
  explicit Region(const Rect& rect);
  explicit Region(const Region& region);

  ~Region();

  const std::vector<Rect>& rects() const { return rects_; }

  Rect GetBounds() const;

  void Reset(const Region& region);
  void Reset(const Rect& rect) { Reset(Region(rect)); }

  void Unite(const Region& region);
  void Unite(const Rect& rect) { Unite(Region(rect)); }

  void Intersect(const Region& region);
  void Intersect(const Rect& rect) { Intersect(Region(rect)); }

  void Subtract(const Region& region);
  void Subtract(const Rect& rect) { Subtract(Region(rect)); }

  bool IsEmpty() const;

  bool ContainsPoint(const Point& point) const;
  bool ContainsRect(const Rect& rect) const;

  bool IsEqual(const Region& other) const;

  bool operator==(const Region& other) const { return IsEqual(other); }
  bool operator!=(const Region& other) const { return !IsEqual(other); }

 private:
  // Updates |rects_| to match |region_|.
  void UpdateRects();

  std::vector<Rect> rects_;
  REGION* region_;
};

}  // namespace window_manager

std::ostream& operator<<(std::ostream& out,
                         const window_manager::Region& region);

#endif  // WINDOW_MANAGER_X11_REGION_H_
