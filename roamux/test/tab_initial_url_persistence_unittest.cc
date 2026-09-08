// SPDX-License-Identifier: Apache-2.0
// roam-14 (I-2.5): the extra-data encode/parse round-trip, including the
// lock bit and malformed-input rejection. (TDD: RED before the impl.)

#include "roamux/browser/tabs/tab_initial_url_helper.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace roamux::tabs {
namespace {

TEST(TabInitialUrlPersistenceTest, EncodeDecodeRoundTripsUnlocked) {
  const GURL url("https://example.test/path?q=1");
  const std::string encoded =
      TabInitialUrlHelper::EncodeExtraData(url, /*locked=*/false);
  GURL out;
  bool locked = true;
  ASSERT_TRUE(TabInitialUrlHelper::DecodeExtraData(encoded, &out, &locked));
  EXPECT_EQ(url, out);
  EXPECT_FALSE(locked);
}

TEST(TabInitialUrlPersistenceTest, EncodeDecodeRoundTripsLocked) {
  const GURL url("https://example.test/");
  const std::string encoded =
      TabInitialUrlHelper::EncodeExtraData(url, /*locked=*/true);
  GURL out;
  bool locked = false;
  ASSERT_TRUE(TabInitialUrlHelper::DecodeExtraData(encoded, &out, &locked));
  EXPECT_EQ(url, out);
  EXPECT_TRUE(locked);
}

TEST(TabInitialUrlPersistenceTest, DecodeRejectsMalformed) {
  GURL out;
  bool locked = false;
  // Too short, bad prefix, and an invalid URL all reject without mutating.
  EXPECT_FALSE(TabInitialUrlHelper::DecodeExtraData("", &out, &locked));
  EXPECT_FALSE(TabInitialUrlHelper::DecodeExtraData("x", &out, &locked));
  EXPECT_FALSE(
      TabInitialUrlHelper::DecodeExtraData("2https://x.test/", &out, &locked));
  EXPECT_FALSE(
      TabInitialUrlHelper::DecodeExtraData("1not a url", &out, &locked));
  EXPECT_TRUE(out.is_empty());
}

// roam-289 (grill M18): persisted values are DATA a hand-edited session file
// can carry. A decoded initial URL must be an http(s) navigation target or
// exactly about:blank — anything else (a javascript: payload would execute in
// the tab's current document on every replay) restores as "uncaptured".
TEST(TabInitialUrlPersistenceTest, DecodeRejectsDisallowedSchemes) {
  const char* const kRejected[] = {
      "javascript:alert(1)",
      "data:text/html,x",
      "file:///etc/hosts",
      "chrome://settings/",
      "ftp://x.test/",
      "blob:https://x.test/1",
      "filesystem:https://x.test/t/f",
      "view-source:https://x.test/",
      "chrome-extension://abc/p",
      "about:srcdoc",
      "about:blank#frag",
  };
  for (const char* spec : kRejected) {
    SCOPED_TRACE(spec);
    GURL out;
    bool locked = false;
    EXPECT_FALSE(TabInitialUrlHelper::DecodeExtraData(std::string("1") + spec,
                                                      &out, &locked));
    EXPECT_TRUE(out.is_empty()) << "a rejected value must not mutate `url`";
  }
}

TEST(TabInitialUrlPersistenceTest, DecodeAcceptsAllowedSchemes) {
  const char* const kAllowed[] = {"http://x.test/", "https://x.test/p?q=1",
                                  "about:blank"};
  for (const char* spec : kAllowed) {
    SCOPED_TRACE(spec);
    GURL out;
    bool locked = false;
    ASSERT_TRUE(TabInitialUrlHelper::DecodeExtraData(
        TabInitialUrlHelper::EncodeExtraData(GURL(spec), /*locked=*/true),
        &out, &locked));
    EXPECT_EQ(GURL(spec), out);
    EXPECT_TRUE(locked);
  }
}

}  // namespace
}  // namespace roamux::tabs
