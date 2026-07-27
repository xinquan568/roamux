// SPDX-License-Identifier: Apache-2.0
#include "roamux/test/support/roamux_browser_test.h"

#include "chrome/browser/feedback/feedback_uploader_factory_chrome.h"
#include "components/keyed_service/content/browser_context_keyed_service_factory.h"

namespace roamux::test {

void SuppressFeedbackUploaderForTesting(content::BrowserContext* context) {
  // A null TestingFactory installs a factory returning a null service
  // (keyed_service_templated_factory.cc: "If the factory is null, install a
  // factory returning a null service"). The null association persists, so a
  // later GetForBrowserContext() returns nullptr rather than constructing the
  // uploader.
  feedback::FeedbackUploaderFactoryChrome::GetInstance()->SetTestingFactory(
      context, BrowserContextKeyedServiceFactory::TestingFactory());
}

RoamuxBrowserTest::RoamuxBrowserTest() {
  DisableWebUIToolbarFeatures(webui_toolbar_disables_);
}

RoamuxBrowserTest::~RoamuxBrowserTest() = default;

void RoamuxBrowserTest::SetUpBrowserContextKeyedServices(
    content::BrowserContext* context) {
  InProcessBrowserTest::SetUpBrowserContextKeyedServices(context);
  SuppressFeedbackUploaderForTesting(context);
}

}  // namespace roamux::test
