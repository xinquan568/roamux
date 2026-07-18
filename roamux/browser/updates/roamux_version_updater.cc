// SPDX-License-Identifier: Apache-2.0
#include "roamux/browser/updates/roamux_version_updater.h"

#include <algorithm>
#include <utility>

#include "base/functional/bind.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"

namespace roamux::updates {

namespace {

// The issue's plain-language copy table. Sparkle's raw error text rides after
// a newline while we are in alpha (the About TS renders it as the dimmed
// secondary line; drop before stable — see the 0033 hunk comment).
constexpr char16_t kCheckFailedCopy[] =
    u"Couldn't check for updates. Check your connection and try again.";
constexpr char16_t kDownloadFailedCopy[] =
    u"Couldn't download the update. Check your connection and try again.";
constexpr char16_t kSignatureFailedCopy[] =
    u"The update couldn't be verified and wasn't installed.";
constexpr char16_t kInstallFailedCopy[] = u"Couldn't install the update.";

std::u16string ErrorCopy(UpdateErrorClass error_class) {
  switch (error_class) {
    case UpdateErrorClass::kCheckFailed:
      return kCheckFailedCopy;
    case UpdateErrorClass::kDownloadFailed:
      return kDownloadFailedCopy;
    case UpdateErrorClass::kSignatureFailed:
      return kSignatureFailedCopy;
    case UpdateErrorClass::kInstallFailed:
      return kInstallFailedCopy;
  }
}

}  // namespace

MappedStatus MapSnapshot(const UpdateSnapshot& snapshot) {
  MappedStatus mapped;
  switch (snapshot.status) {
    case UpdateStatus::kIdle:
      // Initial state only; once a check starts the row must never fall back
      // to an empty render.
      mapped.emit = false;
      return mapped;
    case UpdateStatus::kChecking:
      mapped.status = VersionUpdater::CHECKING;
      return mapped;
    case UpdateStatus::kUpToDate:
      mapped.status = VersionUpdater::UPDATED;
      return mapped;
    case UpdateStatus::kAvailable:
      // Consent gate: render-only. Download fires exclusively from the
      // explicit WebUI command (the no-unprompted-download tests pin this).
      mapped.status = VersionUpdater::NEED_PERMISSION_TO_UPDATE;
      mapped.message = base::StrCat(
          {u"Roamux ", base::UTF8ToUTF16(snapshot.version), u" is available"});
      return mapped;
    case UpdateStatus::kDownloading:
      mapped.status = VersionUpdater::UPDATING;
      mapped.progress = static_cast<int>(
          std::clamp(snapshot.progress, 0.0, 1.0) * 100.0 + 0.5);
      return mapped;
    case UpdateStatus::kReadyToInstall:
      mapped.status = VersionUpdater::NEARLY_UPDATED;
      return mapped;
    case UpdateStatus::kError: {
      // Every failure collapses to FAILED — the about-page icon map renders
      // cr:error only for FAILED; the variant rides the message.
      mapped.status = VersionUpdater::FAILED;
      const UpdateErrorClass error_class = ClassifyUpdateError(snapshot.error);
      mapped.offer_retry = error_class != UpdateErrorClass::kSignatureFailed;
      mapped.message = ErrorCopy(error_class);
      if (!snapshot.error.empty()) {
        // Alpha-only dimmed raw line (rendered after the newline by the 0033
        // About hunk; drop before a stable release).
        mapped.message = base::StrCat(
            {mapped.message, u"\n", base::UTF8ToUTF16(snapshot.error)});
      }
      return mapped;
    }
  }
}

UpdateErrorClass ClassifyUpdateError(const std::string& error_text) {
  const std::string lower = base::ToLowerASCII(error_text);
  // Signature first: security-relevant and its Sparkle text mentions
  // validation, which must not fall through to friendlier classes.
  if (lower.find("sign") != std::string::npos ||
      lower.find("validat") != std::string::npos ||
      lower.find("verif") != std::string::npos) {
    return UpdateErrorClass::kSignatureFailed;
  }
  if (lower.find("install") != std::string::npos) {
    return UpdateErrorClass::kInstallFailed;
  }
  if (lower.find("download") != std::string::npos) {
    return UpdateErrorClass::kDownloadFailed;
  }
  return UpdateErrorClass::kCheckFailed;
}

RoamuxVersionUpdater::RoamuxVersionUpdater(UpdateCommands* commands)
    : commands_(commands) {}

RoamuxVersionUpdater::~RoamuxVersionUpdater() = default;

void RoamuxVersionUpdater::CheckForUpdate(StatusCallback status_callback,
                                          PromoteCallback promote_callback) {
  status_callback_ = std::move(status_callback);
  if (promote_callback) {
    // Sparkle has no Keystone-style system promotion; hide the row.
    promote_callback.Run(VersionUpdater::PROMOTE_HIDDEN);
  }
  if (!subscription_) {
    subscription_ = commands_->SubscribeToSnapshots(base::BindRepeating(
        &RoamuxVersionUpdater::OnSnapshot, base::Unretained(this)));
  }
  commands_->CheckForUpdates();
}

#if BUILDFLAG(IS_MAC)
void RoamuxVersionUpdater::PromoteUpdater() {
  // PROMOTE_HIDDEN posture: nothing to promote under Sparkle.
}
#endif

void RoamuxVersionUpdater::OnSnapshot(const UpdateSnapshot& snapshot) {
  if (!status_callback_) {
    return;
  }
  const MappedStatus mapped = MapSnapshot(snapshot);
  if (!mapped.emit) {
    return;
  }
  status_callback_.Run(mapped.status, mapped.progress, /*rollback=*/false,
                       /*powerwash=*/false, snapshot.version,
                       /*update_size=*/0, mapped.message);
}

}  // namespace roamux::updates
