// SPDX-License-Identifier: Apache-2.0
#ifndef ROAMEX_BROWSER_IMPORTER_ROAMEX_SECRET_IMPORT_STAGE_H_
#define ROAMEX_BROWSER_IMPORTER_ROAMEX_SECRET_IMPORT_STAGE_H_

#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"

class ProfileWriter;

namespace net {
class CanonicalCookie;
}

namespace crypto::apple {
class KeychainV2;
}

namespace roamex {

// The browser-side secret-import stage (roam-16 / I-3.2). It reads Edge's
// encrypted Login Data (passwords) from `source_path`, decrypts them with the
// Edge Keychain-derived key, and writes them to the destination via
// ProfileWriter (which re-encrypts under Roamex). Runs entirely browser-side —
// the Keychain read + decrypt are co-located here and only plaintext reaches
// ProfileWriter; it does NOT go through the utility-process importer or its
// host/bridge lifecycle (frozen analysis / plan finding 1). Cookies are added
// in a follow-up method once ProfileWriter::AddCookies lands.
class RoamexSecretImportStage {
 public:
  // `keychain_for_testing` is nullptr in production.
  RoamexSecretImportStage(base::FilePath source_path,
                          scoped_refptr<ProfileWriter> writer,
                          crypto::apple::KeychainV2* keychain_for_testing);
  RoamexSecretImportStage(const RoamexSecretImportStage&) = delete;
  RoamexSecretImportStage& operator=(const RoamexSecretImportStage&) = delete;
  ~RoamexSecretImportStage();

  // Decrypts and imports Edge passwords. Returns the number imported; a
  // declined/unavailable Keychain yields 0 and `keychain_available()` == false
  // (reported by the caller, never a silent drop).
  size_t ImportPasswords();

  // Decrypts and imports Edge first-party cookies. Runs `done` with the count
  // imported after all async CookieManager writes complete.
  void ImportCookies(base::OnceCallback<void(size_t)> done);

  bool keychain_available() const { return keychain_available_; }

  // Testing seam: the decrypted, v24-stripped, first-party-filtered cookies the
  // stage would write (bypasses the async CookieManager round-trip).
  std::vector<net::CanonicalCookie> ParseCookiesForTesting();

 private:
  const base::FilePath source_path_;
  const scoped_refptr<ProfileWriter> writer_;
  std::vector<net::CanonicalCookie> ParseCookies();

  const raw_ptr<crypto::apple::KeychainV2> keychain_for_testing_;
  bool keychain_available_ = false;
};

}  // namespace roamex

#endif  // ROAMEX_BROWSER_IMPORTER_ROAMEX_SECRET_IMPORT_STAGE_H_
