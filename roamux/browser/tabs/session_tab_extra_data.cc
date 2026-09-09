// SPDX-License-Identifier: Apache-2.0
#include "roamux/browser/tabs/session_tab_extra_data.h"

#include "components/sessions/core/session_command.h"
#include "components/sessions/core/session_service_commands.h"
#include "content/public/browser/web_contents.h"
#include "roamux/browser/tabs/tab_initial_url_helper.h"
#include "roamux/browser/tabs/tab_uid_tab_helper.h"

namespace roamux::tabs {

std::vector<std::unique_ptr<sessions::SessionCommand>>
BuildTabExtraDataCommands(content::WebContents* contents, SessionID tab_id) {
  std::vector<std::unique_ptr<sessions::SessionCommand>> commands;
  if (!contents || !tab_id.is_valid()) {
    return commands;
  }

  // FromWebContents() looks the helper up; it never creates one. A serializer
  // must not allocate helpers or mint identities as a side effect of being
  // asked what to write.
  if (TabInitialUrlHelper* helper =
          TabInitialUrlHelper::FromWebContents(contents)) {
    if (helper->has_initial_url()) {
      commands.push_back(sessions::CreateAddTabExtraDataCommand(
          tab_id, TabInitialUrlHelper::kExtraDataKey,
          TabInitialUrlHelper::EncodeExtraData(helper->initial_url(),
                                               helper->is_user_locked())));
    }
  }

  if (TabUidTabHelper* helper = TabUidTabHelper::FromWebContents(contents)) {
    if (!helper->uid().empty()) {
      commands.push_back(sessions::CreateAddTabExtraDataCommand(
          tab_id, TabUidTabHelper::kExtraDataKey, helper->uid()));
    }
  }

  return commands;
}

}  // namespace roamux::tabs
