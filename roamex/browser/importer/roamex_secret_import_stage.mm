// SPDX-License-Identifier: Apache-2.0
#include "roamex/browser/importer/roamex_secret_import_stage.h"

#include <string>
#include <vector>

#include "base/containers/span.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/importer/profile_writer.h"
#include "components/password_manager/core/browser/password_form.h"
#include "crypto/hash.h"
#include "net/cookies/canonical_cookie.h"
#include "net/cookies/cookie_constants.h"
#include "net/cookies/cookie_partition_key.h"
#include "roamex/browser/importer/edge_secret_decryptor.h"
#include "sql/database.h"
#include "sql/statement.h"
#include "url/gurl.h"

namespace roamex {

namespace {
constexpr sql::Database::Tag kTag{"RoamexEdgeImporter"};
}  // namespace

RoamexSecretImportStage::RoamexSecretImportStage(
    base::FilePath source_path,
    scoped_refptr<ProfileWriter> writer,
    crypto::apple::KeychainV2* keychain_for_testing)
    : source_path_(std::move(source_path)),
      writer_(std::move(writer)),
      keychain_for_testing_(keychain_for_testing) {}

RoamexSecretImportStage::~RoamexSecretImportStage() = default;

size_t RoamexSecretImportStage::ImportPasswords() {
  EdgeSecretDecryptor decryptor(keychain_for_testing_);
  keychain_available_ = decryptor.status() == EdgeSecretDecryptor::Status::kOk;
  if (!keychain_available_) {
    return 0;  // Declined/unavailable — reported via keychain_available().
  }

  const base::FilePath source =
      source_path_.Append(FILE_PATH_LITERAL("Login Data"));
  if (!base::PathExists(source)) {
    return 0;
  }
  // Copy-to-temp + read-only (roam-15 discipline): never touch the live DB.
  base::ScopedTempDir temp_dir;
  if (!temp_dir.CreateUniqueTempDir()) {
    return 0;
  }
  const base::FilePath copy =
      temp_dir.GetPath().Append(FILE_PATH_LITERAL("Login Data"));
  if (!base::CopyFile(source, copy)) {
    return 0;
  }
  sql::Database db(kTag);
  if (!db.Open(copy)) {
    return 0;
  }
  sql::Statement s(db.GetUniqueStatement(
      "SELECT origin_url, username_value, password_value, signon_realm "
      "FROM logins"));
  if (!s.is_valid()) {
    return 0;
  }
  size_t imported = 0;
  while (s.Step()) {
    std::string blob = s.ColumnString(2);
    std::optional<std::string> password =
        decryptor.DecryptV10(base::as_byte_span(blob));
    if (!password) {
      continue;  // Unparseable row — skip, don't abort the batch.
    }
    password_manager::PasswordForm form;
    form.url = GURL(s.ColumnStringView(0));
    form.username_value = s.ColumnString16(1);
    form.password_value = base::UTF8ToUTF16(*password);
    form.signon_realm = s.ColumnString(3);
    if (!form.url.is_valid() || form.signon_realm.empty()) {
      continue;
    }
    writer_->AddPasswordForm(form);  // Re-encrypts under Roamex on store.
    ++imported;
  }
  return imported;
}

std::vector<net::CanonicalCookie> RoamexSecretImportStage::ParseCookies() {
  EdgeSecretDecryptor decryptor(keychain_for_testing_);
  keychain_available_ = decryptor.status() == EdgeSecretDecryptor::Status::kOk;
  std::vector<net::CanonicalCookie> cookies;
  if (!keychain_available_) {
    return cookies;
  }
  const base::FilePath source =
      source_path_.Append(FILE_PATH_LITERAL("Cookies"));
  base::ScopedTempDir temp_dir;
  if (base::PathExists(source) && temp_dir.CreateUniqueTempDir()) {
    const base::FilePath copy =
        temp_dir.GetPath().Append(FILE_PATH_LITERAL("Cookies"));
    sql::Database db(kTag);
    if (base::CopyFile(source, copy) && db.Open(copy)) {
      sql::Statement s(db.GetUniqueStatement(
          "SELECT host_key, top_frame_site_key, name, encrypted_value, path, "
          "creation_utc, expires_utc, last_access_utc, last_update_utc, "
          "is_secure, is_httponly, samesite, priority, source_scheme, "
          "source_port, source_type, has_cross_site_ancestor FROM cookies"));
      if (s.is_valid()) {
        while (s.Step()) {
          const std::string host_key = s.ColumnString(0);
          const std::string top_frame_site_key = s.ColumnString(1);
          // First-party / unpartitioned only: partitioned cookies carry a
          // non-empty top_frame_site_key (has_cross_site_ancestor alone is set
          // even for unpartitioned cookies, so it is NOT the filter).
          if (!top_frame_site_key.empty()) {
            continue;
          }
          std::string blob = s.ColumnString(3);
          std::optional<std::string> plain =
              decryptor.DecryptV10(base::as_byte_span(blob));
          if (!plain) {
            continue;
          }
          // v24 payload = SHA256(host_key) || value; verify + strip the prefix.
          const std::array<uint8_t, 32> expected =
              crypto::hash::Sha256(host_key);
          if (plain->size() < expected.size() ||
              base::as_byte_span(*plain).first(expected.size()) !=
                  base::span(expected)) {
            continue;  // Prefix mismatch → drop (never import a corrupt value).
          }
          const std::string value = plain->substr(expected.size());
          std::unique_ptr<net::CanonicalCookie> cookie =
              net::CanonicalCookie::FromStorage(
                  s.ColumnString(2), value, host_key, s.ColumnString(4),
                  base::Time::FromDeltaSinceWindowsEpoch(
                      base::Microseconds(s.ColumnInt64(5))),
                  base::Time::FromDeltaSinceWindowsEpoch(
                      base::Microseconds(s.ColumnInt64(6))),
                  base::Time::FromDeltaSinceWindowsEpoch(
                      base::Microseconds(s.ColumnInt64(7))),
                  base::Time::FromDeltaSinceWindowsEpoch(
                      base::Microseconds(s.ColumnInt64(8))),
                  s.ColumnBool(9), s.ColumnBool(10),
                  static_cast<net::CookieSameSite>(s.ColumnInt(11)),
                  static_cast<net::CookiePriority>(s.ColumnInt(12)),
                  /*partition_key=*/std::nullopt,
                  static_cast<net::CookieSourceScheme>(s.ColumnInt(13)),
                  s.ColumnInt(14),
                  static_cast<net::CookieSourceType>(s.ColumnInt(15)),
                  net::CanonicalCookieFromStorageCallSite::kCookieManager);
          if (cookie) {
            cookies.push_back(std::move(*cookie));
          }
        }
      }
    }
  }
  return cookies;
}

void RoamexSecretImportStage::ImportCookies(
    base::OnceCallback<void(size_t)> done) {
  std::vector<net::CanonicalCookie> cookies = ParseCookies();
  const size_t count = cookies.size();
  writer_->AddCookies(cookies, base::BindOnce(std::move(done), count));
}

std::vector<net::CanonicalCookie>
RoamexSecretImportStage::ParseCookiesForTesting() {
  return ParseCookies();
}

}  // namespace roamex
