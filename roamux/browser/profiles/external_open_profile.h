// SPDX-License-Identifier: Apache-2.0
#ifndef ROAMUX_BROWSER_PROFILES_EXTERNAL_OPEN_PROFILE_H_
#define ROAMUX_BROWSER_PROFILES_EXTERNAL_OPEN_PROFILE_H_

#include <optional>

#include "base/files/file_path.h"
#include "base/functional/function_ref.h"

namespace base {
class CommandLine;
}
class PrefService;

namespace roamux {

// roam-213: D4 resolution for external opens (Finder files, links from other
// apps). Returns the designated profile's directory when every gate passes,
// nullopt in every other state — nullopt means "current behavior,
// bit-for-bit" (the caller keeps its RunInLastProfileSafely dispatch).
// `profile_exists` abstracts ProfileAttributesStorage so this core never
// links //chrome/browser; the glue that supplies the real globals lives in
// the patched app_controller_mac.mm seam (patch 0050).
std::optional<base::FilePath> ResolveExternalOpenProfile(
    const base::CommandLine& command_line,
    const PrefService* local_state,
    const base::FilePath& user_data_dir,
    base::FunctionRef<bool(const base::FilePath&)> profile_exists);

}  // namespace roamux

#endif  // ROAMUX_BROWSER_PROFILES_EXTERNAL_OPEN_PROFILE_H_
