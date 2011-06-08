// Copyright (c) 2011 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include "base/logging.h"
#include "window_manager/test_lib.h"
#include "window_manager/x11/region.h"

DEFINE_bool(logtostderr, false,
            "Print debugging messages to stderr (suppressed otherwise)");

namespace window_manager {

class RegionTest : public ::testing::Test { };

TEST_F(RegionTest, Empty) {
  Region region;

  EXPECT_TRUE(region.IsEmpty());
  EXPECT_EQ(Rect(), region.GetBounds());
  EXPECT_EQ(0, region.rects().size());
}

TEST_F(RegionTest, Simple) {
  Rect rect(0, 0, 5, 5);
  Region region(rect);

  EXPECT_FALSE(region.IsEmpty());
  EXPECT_EQ(rect, region.GetBounds());
  EXPECT_EQ(Region(rect), region);
  EXPECT_EQ(1, region.rects().size());
  EXPECT_EQ(rect, *region.rects().begin());
  EXPECT_TRUE(region.ContainsRect(rect));
  EXPECT_FALSE(region.ContainsPoint(Point(5, 0)));
  EXPECT_FALSE(region.ContainsPoint(Point(0, 5)));
  EXPECT_TRUE(region.ContainsPoint(Point(2, 0)));
  EXPECT_TRUE(region.ContainsPoint(Point(2, 4)));
}

TEST_F(RegionTest, Subtract) {
  Rect rect(0, 0, 5, 5);
  Region region(rect);

  region.Subtract(Rect(2, 0, 1, 5));
  EXPECT_FALSE(region.IsEmpty());
  EXPECT_EQ(rect, region.GetBounds());
  EXPECT_NE(Region(rect), region);
  EXPECT_EQ(2, region.rects().size());
  EXPECT_FALSE(region.ContainsRect(rect));
  EXPECT_TRUE(region.ContainsRect(Rect(0, 0, 2, 4)));
  EXPECT_TRUE(region.ContainsRect(Rect(3, 0, 2, 4)));
  EXPECT_FALSE(region.ContainsPoint(Point(2, 0)));
  EXPECT_FALSE(region.ContainsPoint(Point(2, 4)));
  EXPECT_TRUE(region.ContainsPoint(Point(0, 0)));
  EXPECT_TRUE(region.ContainsPoint(Point(4, 4)));
}

TEST_F(RegionTest, Unite) {
  Rect rect(0, 0, 5, 5);
  Region region(rect);

  region.Subtract(Rect(2, 0, 1, 5));
  region.Unite(rect);
  EXPECT_FALSE(region.IsEmpty());
  EXPECT_EQ(rect, region.GetBounds());
  EXPECT_EQ(Region(rect), region);
  EXPECT_EQ(1, region.rects().size());
  EXPECT_EQ(rect, *region.rects().begin());
  EXPECT_TRUE(region.ContainsRect(rect));
  EXPECT_FALSE(region.ContainsPoint(Point(5, 0)));
  EXPECT_FALSE(region.ContainsPoint(Point(0, 5)));
  EXPECT_TRUE(region.ContainsPoint(Point(2, 0)));
  EXPECT_TRUE(region.ContainsPoint(Point(2, 4)));
}

TEST_F(RegionTest, Intersect) {
  Rect rect(0, 0, 5, 5);
  Region region(rect);

  region.Subtract(Rect(2, 0, 1, 5));
  region.Intersect(Rect(1, 1, 3, 3));
  EXPECT_FALSE(region.IsEmpty());
  EXPECT_EQ(Rect(1, 1, 3, 3), region.GetBounds());
  EXPECT_NE(Region(rect), region);
  EXPECT_EQ(2, region.rects().size());
  EXPECT_FALSE(region.ContainsRect(rect));
  EXPECT_TRUE(region.ContainsRect(Rect(1, 1, 1, 3)));
  EXPECT_TRUE(region.ContainsRect(Rect(3, 1, 1, 3)));
  EXPECT_FALSE(region.ContainsPoint(Point(0, 0)));
  EXPECT_FALSE(region.ContainsPoint(Point(4, 4)));
  EXPECT_FALSE(region.ContainsPoint(Point(2, 2)));
  EXPECT_FALSE(region.ContainsPoint(Point(4, 2)));
}

}  // namespace window_manager

int main(int argc, char** argv) {
  return window_manager::InitAndRunTests(&argc, argv, &FLAGS_logtostderr);
}
