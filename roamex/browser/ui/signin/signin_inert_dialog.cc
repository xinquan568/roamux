// SPDX-License-Identifier: Apache-2.0
#include "roamex/browser/ui/signin/signin_inert_dialog.h"

#include <memory>
#include <utility>

#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_finder.h"
#include "chrome/browser/ui/browser_window.h"
#include "components/constrained_window/constrained_window_views.h"
#include "roamex/browser/signin/roamex_signin_build_state.h"
#include "roamex/browser/signin/signin_inert.h"
#include "ui/base/models/dialog_model.h"

namespace roamex::signin_ui {

bool MaybeInterceptInertSignin(Profile* profile) {
  if (!roamex::signin::ShouldInterceptInertSignin()) {
    return false;
  }
  roamex::signin::RecordInertExplanationShown();

  if (!roamex::signin::IsInertExplanationDialogSuppressedForTesting()) {
    Browser* browser = nullptr;
    if (profile) {
      browser = chrome::FindBrowserWithProfile(profile);
    }
    if (!browser) {
      BrowserWindowInterface* last_active = chrome::FindLastActive();
      browser =
          last_active ? last_active->GetBrowserForMigrationOnly() : nullptr;
    }
    if (browser && browser->window()) {
      ui::DialogModel::Builder builder;
      builder.SetTitle(u"Google sign-in isn't available in this build")
          .AddParagraph(
              ui::DialogModelLabel(roamex::signin::GetInertSigninExplanation(
                  roamex::signin::GetBuildState())))
          .AddOkButton(base::DoNothing());
      constrained_window::ShowBrowserModal(
          builder.Build(), browser->window()->GetNativeWindow());
    }
  }
  return true;
}

}  // namespace roamex::signin_ui
