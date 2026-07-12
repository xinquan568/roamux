// SPDX-License-Identifier: Apache-2.0
#include "roamux/test/support/roamux_browser_test.h"

namespace roamux::test {

RoamuxBrowserTest::RoamuxBrowserTest() {
  DisableWebUIToolbarFeatures(webui_toolbar_disables_);
}

RoamuxBrowserTest::~RoamuxBrowserTest() = default;

}  // namespace roamux::test
