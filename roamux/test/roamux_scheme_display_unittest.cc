// SPDX-License-Identifier: Apache-2.0
#include "roamux/browser/scheme/roamux_scheme_display.h"

#include "base/test/scoped_feature_list.h"
#include "roamux/common/roamux_features.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"
#include "url/url_util.h"

namespace roamux {
namespace {

// roam-179 (TDD red-first): presentation-only branding of formatted omnibox
// text. Same scoped-registry rationale as roamux_scheme_rewrite_unittest.cc —
// a bare unit-test binary registers no schemes, so chrome:// would parse
// hostless without the fixture registration.
class RoamuxSchemeDisplayTest : public testing::Test {
protected:
  RoamuxSchemeDisplayTest() {
    url::AddStandardScheme("chrome", url::SCHEME_WITH_HOST);
    url::AddStandardScheme("chrome-untrusted", url::SCHEME_WITH_HOST);
    url::AddStandardScheme("roamux", url::SCHEME_WITH_HOST);
  }

  url::ScopedSchemeRegistryForTests scheme_registry_;
  base::test::ScopedFeatureList features_;
};

TEST_F(RoamuxSchemeDisplayTest, BrandsChromeUrlWhenEnabled) {
  features_.InitAndEnableFeature(features::kRoamuxSchemeAlias);
  EXPECT_EQ(u"roamux://version",
            MaybeBrandFormattedUrlForDisplay(GURL("chrome://version"),
                                             u"chrome://version"));
}

TEST_F(RoamuxSchemeDisplayTest, BrandsKeepsPathAndRef) {
  features_.InitAndEnableFeature(features::kRoamuxSchemeAlias);
  EXPECT_EQ(u"roamux://settings/searchEngines#one",
            MaybeBrandFormattedUrlForDisplay(
                GURL("chrome://settings/searchEngines#one"),
                u"chrome://settings/searchEngines#one"));
}

TEST_F(RoamuxSchemeDisplayTest, LeavesNonChromeSchemesUntouched) {
  features_.InitAndEnableFeature(features::kRoamuxSchemeAlias);
  EXPECT_EQ(u"https://example.com",
            MaybeBrandFormattedUrlForDisplay(GURL("https://example.com"),
                                             u"https://example.com"));
}

// chrome-untrusted:// is NOT chrome:// — never branded (explicit roam-179
// decision), and its formatted text must not be half-matched by a naive
// prefix check ("chrome://" is not a prefix of "chrome-untrusted://", but
// this pins the URL-side scheme check too).
TEST_F(RoamuxSchemeDisplayTest, LeavesChromeUntrustedUntouched) {
  features_.InitAndEnableFeature(features::kRoamuxSchemeAlias);
  EXPECT_EQ(u"chrome-untrusted://terminal",
            MaybeBrandFormattedUrlForDisplay(GURL("chrome-untrusted://terminal"),
                                             u"chrome-untrusted://terminal"));
}

// Formatted text that no longer carries the scheme (elided display forms)
// falls through unbranded — degraded-to-unbranded, never corrupted.
TEST_F(RoamuxSchemeDisplayTest, LeavesSchemeElidedTextUntouched) {
  features_.InitAndEnableFeature(features::kRoamuxSchemeAlias);
  EXPECT_EQ(u"version", MaybeBrandFormattedUrlForDisplay(
                            GURL("chrome://version"), u"version"));
}

TEST_F(RoamuxSchemeDisplayTest, FlagOffIsInert) {
  features_.InitAndDisableFeature(features::kRoamuxSchemeAlias);
  EXPECT_EQ(u"chrome://version",
            MaybeBrandFormattedUrlForDisplay(GURL("chrome://version"),
                                             u"chrome://version"));
}

} // namespace
} // namespace roamux
