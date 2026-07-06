// SPDX-License-Identifier: Apache-2.0
#include "roamex/common/roamex_features.h"

namespace roamex::features {

BASE_FEATURE(kTabStripPosition, "RoamexTabStripPosition", base::FEATURE_DISABLED_BY_DEFAULT);
BASE_FEATURE(kInitialUrl, "RoamexInitialUrl", base::FEATURE_DISABLED_BY_DEFAULT);
BASE_FEATURE(kEdgeImport, "RoamexEdgeImport", base::FEATURE_DISABLED_BY_DEFAULT);
BASE_FEATURE(kTabVisitNav, "RoamexTabVisitNav", base::FEATURE_DISABLED_BY_DEFAULT);
BASE_FEATURE(kBraveStyleProfiles, "RoamexBraveStyleProfiles", base::FEATURE_DISABLED_BY_DEFAULT);

}  // namespace roamex::features
