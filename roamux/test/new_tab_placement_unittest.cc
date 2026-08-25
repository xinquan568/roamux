// SPDX-License-Identifier: Apache-2.0
// roam-277: the pure new-tab placement helper — the (index, group) request
// chrome::NewTab() makes of TabStripModel for each roamux.tabs.new_tab_position
// mode (TDD/P6: written RED before the implementation). The helper never sees
// group bounds, pin or split state: TabStripModel owns every clamp, so the
// "last in group" / "group last in strip" cases below deliberately produce the
// SAME request — the model, not the helper, distinguishes them.

#include "roamux/browser/tabs/new_tab_placement.h"

#include <optional>

#include "components/tab_groups/tab_group_id.h"
#include "roamux/common/new_tab_position.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace roamux::tabs {
namespace {

constexpr int kFourTabs = 4;

class NewTabPlacementTest : public testing::Test {
 protected:
  const tab_groups::TabGroupId group_ = tab_groups::TabGroupId::GenerateNew();
};

// ---- end_of_strip: always a plain append, never a group -------------------

TEST_F(NewTabPlacementTest, EndOfStripUngroupedActive) {
  const NewTabPlacement p = ComputeNewTabPlacement(
      NewTabPosition::kEndOfStrip, kFourTabs, /*active_index=*/0, std::nullopt,
      /*upstream_new_tab_adds_to_active_group=*/true);
  EXPECT_EQ(-1, p.index);
  EXPECT_EQ(std::nullopt, p.group);
}

TEST_F(NewTabPlacementTest, EndOfStripGroupedActiveMidGroup) {
  const NewTabPlacement p = ComputeNewTabPlacement(
      NewTabPosition::kEndOfStrip, kFourTabs, /*active_index=*/1, group_,
      /*upstream_new_tab_adds_to_active_group=*/true);
  EXPECT_EQ(-1, p.index);
  EXPECT_EQ(std::nullopt, p.group);
}

TEST_F(NewTabPlacementTest, EndOfStripGroupedActiveLastInGroupGroupNotLast) {
  const NewTabPlacement p = ComputeNewTabPlacement(
      NewTabPosition::kEndOfStrip, kFourTabs, /*active_index=*/2, group_,
      /*upstream_new_tab_adds_to_active_group=*/true);
  EXPECT_EQ(-1, p.index);
  EXPECT_EQ(std::nullopt, p.group);
}

TEST_F(NewTabPlacementTest, EndOfStripGroupedActiveGroupLastInStrip) {
  const NewTabPlacement p = ComputeNewTabPlacement(
      NewTabPosition::kEndOfStrip, /*tab_count=*/3, /*active_index=*/2, group_,
      /*upstream_new_tab_adds_to_active_group=*/true);
  EXPECT_EQ(-1, p.index);
  EXPECT_EQ(std::nullopt, p.group);
}

// ---- end_of_active_group: the stock patch-0067 request ---------------------

TEST_F(NewTabPlacementTest, EndOfActiveGroupUngroupedActive) {
  const NewTabPlacement p = ComputeNewTabPlacement(
      NewTabPosition::kEndOfActiveGroup, kFourTabs, /*active_index=*/0,
      std::nullopt, /*upstream_new_tab_adds_to_active_group=*/true);
  EXPECT_EQ(-1, p.index);
  EXPECT_EQ(std::nullopt, p.group);
}

TEST_F(NewTabPlacementTest, EndOfActiveGroupGroupedActiveMidGroup) {
  const NewTabPlacement p = ComputeNewTabPlacement(
      NewTabPosition::kEndOfActiveGroup, kFourTabs, /*active_index=*/1, group_,
      /*upstream_new_tab_adds_to_active_group=*/true);
  EXPECT_EQ(-1, p.index);
  EXPECT_EQ(group_, p.group);
}

TEST_F(NewTabPlacementTest,
       EndOfActiveGroupGroupedActiveLastInGroupGroupNotLast) {
  const NewTabPlacement p = ComputeNewTabPlacement(
      NewTabPosition::kEndOfActiveGroup, kFourTabs, /*active_index=*/2, group_,
      /*upstream_new_tab_adds_to_active_group=*/true);
  EXPECT_EQ(-1, p.index);
  EXPECT_EQ(group_, p.group);
}

TEST_F(NewTabPlacementTest, EndOfActiveGroupGroupedActiveGroupLastInStrip) {
  const NewTabPlacement p = ComputeNewTabPlacement(
      NewTabPosition::kEndOfActiveGroup, /*tab_count=*/3, /*active_index=*/2,
      group_, /*upstream_new_tab_adds_to_active_group=*/true);
  EXPECT_EQ(-1, p.index);
  EXPECT_EQ(group_, p.group);
}

// An explicit upstream opt-out (chrome://flags/#new-tab-adds-to-active-group
// disabled) keeps meaning strip end under this mode — exactly as upstream.
TEST_F(NewTabPlacementTest, EndOfActiveGroupHonoursUpstreamFlagOff) {
  const NewTabPlacement p = ComputeNewTabPlacement(
      NewTabPosition::kEndOfActiveGroup, kFourTabs, /*active_index=*/1, group_,
      /*upstream_new_tab_adds_to_active_group=*/false);
  EXPECT_EQ(-1, p.index);
  EXPECT_EQ(std::nullopt, p.group);
}

// ---- after_active_tab: active+1, the active tab's group passed explicitly --

TEST_F(NewTabPlacementTest, AfterActiveTabUngroupedActive) {
  const NewTabPlacement p = ComputeNewTabPlacement(
      NewTabPosition::kAfterActiveTab, kFourTabs, /*active_index=*/0,
      std::nullopt, /*upstream_new_tab_adds_to_active_group=*/true);
  EXPECT_EQ(1, p.index);
  EXPECT_EQ(std::nullopt, p.group);
}

TEST_F(NewTabPlacementTest, AfterActiveTabGroupedActiveMidGroup) {
  const NewTabPlacement p = ComputeNewTabPlacement(
      NewTabPosition::kAfterActiveTab, kFourTabs, /*active_index=*/1, group_,
      /*upstream_new_tab_adds_to_active_group=*/true);
  EXPECT_EQ(2, p.index);
  EXPECT_EQ(group_, p.group);
}

TEST_F(NewTabPlacementTest,
       AfterActiveTabGroupedActiveLastInGroupGroupNotLast) {
  const NewTabPlacement p = ComputeNewTabPlacement(
      NewTabPosition::kAfterActiveTab, kFourTabs, /*active_index=*/2, group_,
      /*upstream_new_tab_adds_to_active_group=*/true);
  EXPECT_EQ(3, p.index);
  EXPECT_EQ(group_, p.group);
}

TEST_F(NewTabPlacementTest, AfterActiveTabGroupedActiveGroupLastInStrip) {
  // Same request as the not-last case above: active+1 == count() here, which
  // TabStripModel normalises to an append inside the group.
  const NewTabPlacement p = ComputeNewTabPlacement(
      NewTabPosition::kAfterActiveTab, /*tab_count=*/3, /*active_index=*/2,
      group_, /*upstream_new_tab_adds_to_active_group=*/true);
  EXPECT_EQ(3, p.index);
  EXPECT_EQ(group_, p.group);
}

TEST_F(NewTabPlacementTest, AfterActiveTabLastUngroupedTabAppends) {
  const NewTabPlacement p = ComputeNewTabPlacement(
      NewTabPosition::kAfterActiveTab, kFourTabs, /*active_index=*/3,
      std::nullopt, /*upstream_new_tab_adds_to_active_group=*/true);
  EXPECT_EQ(4, p.index);
  EXPECT_EQ(std::nullopt, p.group);
}

// Mode 2 does not consult the upstream feature: an adjacent tab inside a
// group cannot be ungrouped, so the active group is always passed through.
TEST_F(NewTabPlacementTest, AfterActiveTabIgnoresUpstreamFlag) {
  const NewTabPlacement p = ComputeNewTabPlacement(
      NewTabPosition::kAfterActiveTab, kFourTabs, /*active_index=*/1, group_,
      /*upstream_new_tab_adds_to_active_group=*/false);
  EXPECT_EQ(2, p.index);
  EXPECT_EQ(group_, p.group);
}

// ---- totality: no active tab / out-of-range active index -------------------

TEST_F(NewTabPlacementTest, EmptyStripAppendsInEveryMode) {
  for (const NewTabPosition mode :
       {NewTabPosition::kEndOfStrip, NewTabPosition::kEndOfActiveGroup,
        NewTabPosition::kAfterActiveTab}) {
    const NewTabPlacement p = ComputeNewTabPlacement(
        mode, /*tab_count=*/0, /*active_index=*/-1, std::nullopt,
        /*upstream_new_tab_adds_to_active_group=*/true);
    EXPECT_EQ(-1, p.index) << static_cast<int>(mode);
    EXPECT_EQ(std::nullopt, p.group) << static_cast<int>(mode);
  }
}

TEST_F(NewTabPlacementTest, ActiveIndexOutOfRangeAppendsInEveryMode) {
  for (const NewTabPosition mode :
       {NewTabPosition::kEndOfStrip, NewTabPosition::kEndOfActiveGroup,
        NewTabPosition::kAfterActiveTab}) {
    const NewTabPlacement p = ComputeNewTabPlacement(
        mode, kFourTabs, /*active_index=*/kFourTabs, group_,
        /*upstream_new_tab_adds_to_active_group=*/true);
    EXPECT_EQ(-1, p.index) << static_cast<int>(mode);
    EXPECT_EQ(std::nullopt, p.group) << static_cast<int>(mode);
  }
}

}  // namespace
}  // namespace roamux::tabs
