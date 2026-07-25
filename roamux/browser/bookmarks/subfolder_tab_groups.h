// SPDX-License-Identifier: Apache-2.0
#ifndef ROAMUX_BROWSER_BOOKMARKS_SUBFOLDER_TAB_GROUPS_H_
#define ROAMUX_BROWSER_BOOKMARKS_SUBFOLDER_TAB_GROUPS_H_

#include <string>
#include <vector>

#include "url/gurl.h"

class Browser;
class Profile;

namespace bookmarks {
class BookmarkNode;
}

namespace roamux {

// roam-208: one planned tab group — a qualifying subfolder's exact title and
// its first-level links in bookmark order. Snapshotted: no BookmarkNode is
// retained across the (asynchronous) confirmation prompt.
struct SubfolderGroupPlan {
  SubfolderGroupPlan();
  SubfolderGroupPlan(std::u16string title, std::vector<GURL> urls);
  SubfolderGroupPlan(const SubfolderGroupPlan &);
  SubfolderGroupPlan(SubfolderGroupPlan &&);
  SubfolderGroupPlan &operator=(const SubfolderGroupPlan &);
  SubfolderGroupPlan &operator=(SubfolderGroupPlan &&);
  ~SubfolderGroupPlan();

  std::u16string title;
  std::vector<GURL> urls;
};

// The single qualifying-subfolder enumeration: one plan per immediate child
// folder of `folder` holding at least one first-level link, in bookmark
// order. Feeds the label's N, the row's visibility, and execution alike.
std::vector<SubfolderGroupPlan>
BuildSubfolderGroupPlans(const bookmarks::BookmarkNode &folder);

// Opens one new browser window in `source`'s profile and materializes each
// plan as a named tab group (exact titles; colors assigned by the tab strip).
// Shows the aggregate confirmation first when the total link count reaches
// the bookmark-open prompt threshold.
void OpenSubfolderGroupsInNewWindow(Browser *source,
                                    std::vector<SubfolderGroupPlan> plans);

// roam-220: the shared menu-surface seam — ALL gates in one place, used by
// both the bookmarks-bar controller row and the side-panel WebUI row. Returns
// the qualifying-subfolder count (the label's N / row visibility), or 0 when
// any gate fails: feature flag off, null/non-folder/permanent anchor, OTR
// profile, or Incognito availability forced.
int GetQualifyingSubfolderCountForMenus(Profile *profile,
                                        const bookmarks::BookmarkNode *folder);

// roam-220: the renderer-hardened execute seam for the side-panel mojo path.
// Re-runs every gate and freshly rebuilds plans immediately before opening —
// the WebUI message is untrusted input, so a stale or non-qualifying request
// is a safe no-op (returns false). Opens via `source` on success.
bool ValidateAndOpenSubfolderGroups(Browser *source,
                                    const bookmarks::BookmarkNode *folder);

// Test seam for the aggregate prompt: when set, replaces the dialog and
// returns the canned answer. `message` is the exact dialog text production
// would show. Returns the previous callback.
using BulkOpenPromptCallback = bool (*)(size_t total_urls,
                                        const std::u16string &message);
BulkOpenPromptCallback
SetBulkOpenPromptCallbackForTesting(BulkOpenPromptCallback callback);

} // namespace roamux

#endif // ROAMUX_BROWSER_BOOKMARKS_SUBFOLDER_TAB_GROUPS_H_
