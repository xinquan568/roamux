// SPDX-License-Identifier: Apache-2.0
#ifndef ROAMEX_COMMON_ROAMEX_CRYPTO_PASSKEY_H_
#define ROAMEX_COMMON_ROAMEX_CRYPTO_PASSKEY_H_

#include "crypto/subtle_passkey.h"

namespace roamex {

// Mints a crypto::SubtlePassKey for Roamex's own use of low-level crypto KDFs
// (the Edge import decryptor derives the Edge key via crypto::kdf). Mirrors the
// password_manager::MakeCryptoPassKeyForPasswordHash precedent; friended in
// crypto/subtle_passkey.h by patch 0014.
crypto::SubtlePassKey MakeCryptoPassKey();

}  // namespace roamex

#endif  // ROAMEX_COMMON_ROAMEX_CRYPTO_PASSKEY_H_
