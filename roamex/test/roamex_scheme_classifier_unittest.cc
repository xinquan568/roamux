// SPDX-License-Identifier: Apache-2.0
#include "chrome/browser/autocomplete/chrome_autocomplete_scheme_classifier.h"

#include "chrome/test/base/testing_profile.h"
#include "content/public/test/browser_task_environment.h"
#include "roamex/common/roamex_url_constants.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/metrics_proto/omnibox_input_type.pb.h"

namespace roamex {
namespace {

// roam-91 (D8): typing roamex://about in the omnibox must classify as URL
// input, not a search. ChromeAutocompleteSchemeClassifier consults
// ProfileIOData::IsHandledProtocol, where patch 0028 lists "roamex" — this
// pins that the typed-input path reaches the alias rewrite at all.
TEST(RoamexSchemeClassifierTest, RoamexSchemeClassifiesAsUrlInput) {
  content::BrowserTaskEnvironment task_environment;
  TestingProfile profile;
  ChromeAutocompleteSchemeClassifier classifier(&profile);
  EXPECT_EQ(metrics::OmniboxInputType::URL,
            classifier.GetInputTypeForScheme(kRoamexScheme));
}

} // namespace
} // namespace roamex
