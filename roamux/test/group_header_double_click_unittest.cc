// SPDX-License-Identifier: Apache-2.0
// roam-322: the pure core of "double-click a group header to collapse/expand
// all groups" — gesture state machine, target rule, and the bulk pass with its
// revalidation, stop and reentrancy contracts.

#include "roamux/browser/tabs/group_header_double_click.h"

#include <optional>
#include <vector>

#include "base/time/time.h"
#include "components/tab_groups/tab_group_id.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace roamux::group_header_double_click {
namespace {

using tab_groups::TabGroupId;

constexpr base::TimeDelta kMaxAge = base::Milliseconds(500);

struct Window {
  std::vector<GroupState> groups;

  Snapshot Snap() const { return groups; }
  GroupState* Find(const TabGroupId& id) {
    for (auto& g : groups) {
      if (g.id == id) {
        return &g;
      }
    }
    return nullptr;
  }
};

GroupState G(const TabGroupId& id, bool collapsed, bool active = false) {
  return GroupState{id, collapsed, active};
}

class GroupHeaderDoubleClickTest : public testing::Test {
 protected:
  const TabGroupId a_ = TabGroupId::GenerateNew();
  const TabGroupId b_ = TabGroupId::GenerateNew();
  const TabGroupId c_ = TabGroupId::GenerateNew();
  const base::TimeTicks t0_ = base::TimeTicks() + base::Seconds(100);
};

// --- EqualExceptClickedFlipped ---------------------------------------------

TEST_F(GroupHeaderDoubleClickTest, EqualWhenOnlyTheClickedGroupFlipped) {
  Snapshot before = {G(a_, false), G(b_, false)};
  Snapshot now = {G(b_, false), G(a_, true, /*active=*/true)};
  EXPECT_TRUE(EqualExceptClickedFlipped(before, now, a_));
}

TEST_F(GroupHeaderDoubleClickTest, NotEqualWhenAnotherGroupChanged) {
  EXPECT_FALSE(EqualExceptClickedFlipped({G(a_, false), G(b_, false)},
                                         {G(a_, true), G(b_, true)}, a_));
}

TEST_F(GroupHeaderDoubleClickTest, NotEqualWhenTheClickedGroupDidNotFlip) {
  EXPECT_FALSE(EqualExceptClickedFlipped({G(a_, false)}, {G(a_, false)}, a_));
}

TEST_F(GroupHeaderDoubleClickTest, NotEqualWhenAGroupWasAddedOrRemoved) {
  EXPECT_FALSE(EqualExceptClickedFlipped({G(a_, false)},
                                         {G(a_, true), G(b_, false)}, a_));
  EXPECT_FALSE(EqualExceptClickedFlipped({G(a_, false), G(b_, false)},
                                         {G(a_, true)}, a_));
}

// --- PlanDoubleClickAll ----------------------------------------------------

TEST_F(GroupHeaderDoubleClickTest, AllExpandedBeforeCollapsesAll) {
  // The first click already collapsed A; before the gesture all were expanded.
  Plan plan = PlanDoubleClickAll({G(a_, true), G(b_, false), G(c_, false)}, a_,
                                 /*clicked_pre_collapsed=*/false);
  EXPECT_TRUE(plan.target_collapsed);
  EXPECT_EQ(plan.to_toggle, (std::vector<TabGroupId>{b_, c_}));
}

TEST_F(GroupHeaderDoubleClickTest, OneCollapsedBeforeExpandsAll) {
  // B was collapsed before the gesture; the first click collapsed A.
  Plan plan = PlanDoubleClickAll({G(a_, true), G(b_, true), G(c_, false)}, a_,
                                 /*clicked_pre_collapsed=*/false);
  EXPECT_FALSE(plan.target_collapsed);
  EXPECT_EQ(plan.to_toggle, (std::vector<TabGroupId>{a_, b_}));
}

TEST_F(GroupHeaderDoubleClickTest, ClickedPreStateOverridesItsCurrentState) {
  // A was collapsed before the gesture (the first click expanded it): not all
  // expanded -> expand all; A is already expanded now, so only C toggles.
  Plan plan = PlanDoubleClickAll({G(a_, false), G(b_, false), G(c_, true)}, a_,
                                 /*clicked_pre_collapsed=*/true);
  EXPECT_FALSE(plan.target_collapsed);
  EXPECT_EQ(plan.to_toggle, (std::vector<TabGroupId>{c_}));
}

TEST_F(GroupHeaderDoubleClickTest, OneGroupWindowMatchesASingleClick) {
  // Expanded before: a single click collapses it; the double-click ends
  // collapsed too, with nothing further to toggle.
  Plan collapse = PlanDoubleClickAll({G(a_, true)}, a_, false);
  EXPECT_TRUE(collapse.target_collapsed);
  EXPECT_TRUE(collapse.to_toggle.empty());
  // Collapsed before: a single click expands it; so does the double-click.
  Plan expand = PlanDoubleClickAll({G(a_, false)}, a_, true);
  EXPECT_FALSE(expand.target_collapsed);
  EXPECT_TRUE(expand.to_toggle.empty());
}

TEST_F(GroupHeaderDoubleClickTest, CollapsePassTogglesTheActiveGroupLast) {
  Plan plan = PlanDoubleClickAll(
      {G(a_, true), G(b_, false, /*active=*/true), G(c_, false)}, a_, false);
  EXPECT_TRUE(plan.target_collapsed);
  EXPECT_EQ(plan.to_toggle, (std::vector<TabGroupId>{c_, b_}));
}

// --- RunDoubleClickAll -----------------------------------------------------

TEST_F(GroupHeaderDoubleClickTest, RunTogglesPlannedGroupsThroughTheCallback) {
  Window w{{G(a_, true), G(b_, false), G(c_, false)}};
  std::vector<TabGroupId> toggled;
  PassResult r = RunDoubleClickAll(
      a_, false, [&]() -> std::optional<Snapshot> { return w.Snap(); },
      [&](const TabGroupId& id) {
        toggled.push_back(id);
        w.Find(id)->collapsed = !w.Find(id)->collapsed;
        return true;
      });
  EXPECT_EQ(r, PassResult::kCompleted);
  EXPECT_EQ(toggled, (std::vector<TabGroupId>{b_, c_}));
  for (const auto& g : w.groups) {
    EXPECT_TRUE(g.collapsed);
  }
}

TEST_F(GroupHeaderDoubleClickTest, RunRevalidatesBeforeEachToggle) {
  // Toggling B removes C and flips nothing else: C must be skipped safely.
  Window w{{G(a_, true), G(b_, false), G(c_, false)}};
  std::vector<TabGroupId> toggled;
  PassResult r = RunDoubleClickAll(
      a_, false, [&]() -> std::optional<Snapshot> { return w.Snap(); },
      [&](const TabGroupId& id) {
        toggled.push_back(id);
        w.Find(id)->collapsed = true;
        if (id == b_) {
          std::erase_if(w.groups,
                        [&](const GroupState& g) { return g.id == c_; });
        }
        return true;
      });
  EXPECT_EQ(r, PassResult::kCompleted);
  EXPECT_EQ(toggled, (std::vector<TabGroupId>{b_}));
}

TEST_F(GroupHeaderDoubleClickTest, RunSkipsAGroupThatAlreadyMatchesTheTarget) {
  // An observer collapses C while B toggles: C is not toggled back open.
  Window w{{G(a_, true), G(b_, false), G(c_, false)}};
  std::vector<TabGroupId> toggled;
  RunDoubleClickAll(
      a_, false, [&]() -> std::optional<Snapshot> { return w.Snap(); },
      [&](const TabGroupId& id) {
        toggled.push_back(id);
        w.Find(id)->collapsed = true;
        w.Find(c_)->collapsed = true;
        return true;
      });
  EXPECT_EQ(toggled, (std::vector<TabGroupId>{b_}));
}

TEST_F(GroupHeaderDoubleClickTest, RunStopsWhenTheOwnerIsGone) {
  Window w{{G(a_, true), G(b_, false), G(c_, false)}};
  bool owner_alive = true;
  int toggles = 0;
  PassResult r = RunDoubleClickAll(
      a_, false,
      [&]() -> std::optional<Snapshot> {
        if (!owner_alive) {
          return std::nullopt;
        }
        return w.Snap();
      },
      [&](const TabGroupId& id) {
        ++toggles;
        owner_alive = false;  // the toggle destroyed the strip
        return true;
      });
  EXPECT_EQ(r, PassResult::kStopped);
  EXPECT_EQ(toggles, 1);
}

TEST_F(GroupHeaderDoubleClickTest, RunStopsWhenAToggleReportsFailure) {
  Window w{{G(a_, true), G(b_, false), G(c_, false)}};
  int toggles = 0;
  PassResult r = RunDoubleClickAll(
      a_, false, [&]() -> std::optional<Snapshot> { return w.Snap(); },
      [&](const TabGroupId&) {
        ++toggles;
        return false;
      });
  EXPECT_EQ(r, PassResult::kStopped);
  EXPECT_EQ(toggles, 1);
}

TEST_F(GroupHeaderDoubleClickTest, RunIgnoresANestedCall) {
  Window w{{G(a_, true), G(b_, false), G(c_, false)}};
  std::optional<PassResult> nested;
  RunDoubleClickAll(
      a_, false, [&]() -> std::optional<Snapshot> { return w.Snap(); },
      [&](const TabGroupId& id) {
        if (!nested) {
          nested = RunDoubleClickAll(
              a_, false, [&]() -> std::optional<Snapshot> { return w.Snap(); },
              [&](const TabGroupId&) { return true; });
        }
        w.Find(id)->collapsed = true;
        return true;
      });
  EXPECT_EQ(nested, PassResult::kIgnoredNested);
}

// --- GestureState ------------------------------------------------------------

TEST_F(GroupHeaderDoubleClickTest, SingleToggleThenFlaggedPressLatches) {
  GestureState s;
  s.RecordSingleToggle(a_, Snapshot{G(a_, false), G(b_, false)}, t0_);
  s.OnLeftPress(true, t0_ + base::Milliseconds(200), kMaxAge,
                Snapshot{G(a_, true), G(b_, false)});
  EXPECT_TRUE(s.is_latched_for_testing());
  EXPECT_EQ(
      s.ConsumeOnDoubleClickRelease(a_, Snapshot{G(a_, true), G(b_, false)}),
      std::optional<bool>(false));
  EXPECT_TRUE(s.is_idle_for_testing());
}

TEST_F(GroupHeaderDoubleClickTest, HeldSecondPressStillRunsIfNothingChanged) {
  // No age check at the release: a second press held for seconds latches at
  // the press and the release still yields the bulk verdict.
  GestureState s;
  s.RecordSingleToggle(a_, Snapshot{G(a_, false)}, t0_);
  s.OnLeftPress(true, t0_ + base::Milliseconds(300), kMaxAge,
                Snapshot{G(a_, true)});
  EXPECT_EQ(s.ConsumeOnDoubleClickRelease(a_, Snapshot{G(a_, true)}),
            std::optional<bool>(false));
}

TEST_F(GroupHeaderDoubleClickTest, UnflaggedPressResetsToIdle) {
  GestureState s;
  s.RecordSingleToggle(a_, Snapshot{G(a_, false)}, t0_);
  s.OnLeftPress(false, t0_ + base::Milliseconds(100), kMaxAge,
                Snapshot{G(a_, true)});
  EXPECT_TRUE(s.is_idle_for_testing());
  EXPECT_EQ(s.ConsumeOnDoubleClickRelease(a_, Snapshot{G(a_, true)}),
            std::nullopt);
}

TEST_F(GroupHeaderDoubleClickTest, StaleRecordDoesNotLatch) {
  GestureState s;
  s.RecordSingleToggle(a_, Snapshot{G(a_, false)}, t0_);
  s.OnLeftPress(true, t0_ + base::Milliseconds(900), kMaxAge,
                Snapshot{G(a_, true)});
  EXPECT_TRUE(s.is_idle_for_testing());
}

TEST_F(GroupHeaderDoubleClickTest,
       InterveningChangeBeforeThePressDoesNotLatch) {
  GestureState s;
  s.RecordSingleToggle(a_, Snapshot{G(a_, false), G(b_, false)}, t0_);
  s.OnLeftPress(true, t0_ + base::Milliseconds(100), kMaxAge,
                Snapshot{G(a_, true), G(b_, true)});  // B changed elsewhere
  EXPECT_TRUE(s.is_idle_for_testing());
}

TEST_F(GroupHeaderDoubleClickTest, ChangeDuringAHeldPressFallsBackToSingle) {
  GestureState s;
  s.RecordSingleToggle(a_, Snapshot{G(a_, false), G(b_, false)}, t0_);
  s.OnLeftPress(true, t0_ + base::Milliseconds(100), kMaxAge,
                Snapshot{G(a_, true), G(b_, false)});
  ASSERT_TRUE(s.is_latched_for_testing());
  EXPECT_EQ(s.ConsumeOnDoubleClickRelease(
                a_, Snapshot{G(a_, true), G(b_, true)}),  // changed meanwhile
            std::nullopt);
  EXPECT_TRUE(s.is_idle_for_testing());
}

TEST_F(GroupHeaderDoubleClickTest, ResetAndInvalidOwnerReturnToIdle) {
  GestureState s;
  s.RecordSingleToggle(a_, Snapshot{G(a_, false)}, t0_);
  s.Reset();
  EXPECT_TRUE(s.is_idle_for_testing());

  s.RecordSingleToggle(a_, Snapshot{G(a_, false)}, t0_);
  s.OnLeftPress(true, t0_ + base::Milliseconds(100), kMaxAge, std::nullopt);
  EXPECT_TRUE(s.is_idle_for_testing());
}

TEST_F(GroupHeaderDoubleClickTest, UnmatchedFlaggedReleaseIsASingleToggle) {
  // First click suppressed (nothing recorded): the flagged release is single.
  GestureState s;
  s.OnLeftPress(true, t0_, kMaxAge, Snapshot{G(a_, false)});
  EXPECT_EQ(s.ConsumeOnDoubleClickRelease(a_, Snapshot{G(a_, false)}),
            std::nullopt);
}

TEST_F(GroupHeaderDoubleClickTest, ADifferentGroupDoesNotLatch) {
  GestureState s;
  s.RecordSingleToggle(a_, Snapshot{G(a_, false), G(b_, false)}, t0_);
  s.OnLeftPress(true, t0_ + base::Milliseconds(100), kMaxAge,
                Snapshot{G(a_, true), G(b_, false)});
  EXPECT_EQ(
      s.ConsumeOnDoubleClickRelease(b_, Snapshot{G(a_, true), G(b_, false)}),
      std::nullopt);
}

TEST_F(GroupHeaderDoubleClickTest, NoRecordLeavesAnInvalidSnapshotIdle) {
  GestureState s;
  s.RecordSingleToggle(a_, std::nullopt, t0_);
  EXPECT_TRUE(s.is_idle_for_testing());
}

// --- SnapshotFromModel (with a fake TabStripModel shape) ------------------

struct FakeVisual {
  bool collapsed = false;
  bool is_collapsed() const { return collapsed; }
};
struct FakeGroup {
  FakeVisual visual;
  const FakeVisual* visual_data() const { return &visual; }
};
struct FakeTab {
  std::optional<TabGroupId> group;
  std::optional<TabGroupId> GetGroup() const { return group; }
};
struct FakeGroupModel {
  std::vector<TabGroupId> ids;
  std::vector<FakeGroup> groups;
  std::vector<TabGroupId> ListTabGroups() const { return ids; }
  const FakeGroup* GetTabGroup(const TabGroupId& id) const {
    for (size_t i = 0; i < ids.size(); ++i) {
      if (ids[i] == id) {
        return &groups[i];
      }
    }
    return nullptr;
  }
};
struct FakeModel {
  bool closing = false;
  FakeGroupModel gm;
  std::optional<FakeTab> active;
  bool closing_all() const { return closing; }
  const FakeGroupModel* group_model() const { return &gm; }
  const FakeTab* GetActiveTab() const { return active ? &*active : nullptr; }
};

TEST_F(GroupHeaderDoubleClickTest, SnapshotFromModelReadsStatesAndActiveGroup) {
  FakeModel m;
  m.gm.ids = {a_, b_};
  m.gm.groups = {FakeGroup{FakeVisual{true}}, FakeGroup{FakeVisual{false}}};
  m.active = FakeTab{b_};
  std::optional<Snapshot> s = SnapshotFromModel(&m);
  ASSERT_TRUE(s);
  ASSERT_EQ(s->size(), 2u);
  EXPECT_TRUE((*s)[0].collapsed);
  EXPECT_FALSE((*s)[0].contains_active_tab);
  EXPECT_FALSE((*s)[1].collapsed);
  EXPECT_TRUE((*s)[1].contains_active_tab);
}

TEST_F(GroupHeaderDoubleClickTest, SnapshotFromModelIsNulloptWhileClosing) {
  FakeModel m;
  m.closing = true;
  EXPECT_FALSE(SnapshotFromModel(&m));
  EXPECT_FALSE(SnapshotFromModel(static_cast<const FakeModel*>(nullptr)));
}

}  // namespace
}  // namespace roamux::group_header_double_click
