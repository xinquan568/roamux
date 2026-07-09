// SPDX-License-Identifier: Apache-2.0
#include "roamex/browser/tab_visit/settled_visit_journal_factory.h"

#include <memory>
#include <utility>

#include "chrome/browser/profiles/profile.h"
#include "roamex/browser/tab_visit/settled_visit_journal_service.h"

namespace roamex::tab_visit {

namespace {
// The per-profile journal database file, created under the profile directory.
constexpr base::FilePath::CharType kJournalDbName[] =
    FILE_PATH_LITERAL("RoamexTabVisits");
}  // namespace

// static
SettledVisitJournalService* SettledVisitJournalFactory::GetForProfile(
    Profile* profile) {
  return static_cast<SettledVisitJournalService*>(
      GetInstance()->GetServiceForBrowserContext(profile, /*create=*/true));
}

// static
SettledVisitJournalFactory* SettledVisitJournalFactory::GetInstance() {
  static base::NoDestructor<SettledVisitJournalFactory> instance;
  return instance.get();
}

SettledVisitJournalFactory::SettledVisitJournalFactory()
    : ProfileKeyedServiceFactory(
          "RoamexSettledVisitJournalService",
          ProfileSelections::Builder()
              .WithRegular(ProfileSelection::kOwnInstance)
              .Build()) {}

SettledVisitJournalFactory::~SettledVisitJournalFactory() = default;

std::unique_ptr<KeyedService>
SettledVisitJournalFactory::BuildServiceInstanceForBrowserContext(
    content::BrowserContext* context) const {
  Profile* profile = Profile::FromBrowserContext(context);
  const bool in_memory = profile->IsOffTheRecord();
  // OTR: no path (in-memory). Regular: the journal DB under the profile dir.
  base::FilePath db_path =
      in_memory ? base::FilePath() : profile->GetPath().Append(kJournalDbName);
  return std::make_unique<SettledVisitJournalService>(in_memory,
                                                      std::move(db_path));
}

}  // namespace roamex::tab_visit
