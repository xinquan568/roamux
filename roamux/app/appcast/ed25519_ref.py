# SPDX-License-Identifier: Apache-2.0
"""Pure-Python Ed25519 (public-domain ed25519.cr.yp.to reference impl) — sign
AND verify (roam-34). Slow (~1 s of curve arithmetic per verify; SHA-512 over
the message is hashlib) and dependency-free.

Used by (1) the release job's staging validation (verify_appcast.py, roam-286):
the DOWNLOADED appcast/artifact are verified with the committed SUPublicEDKey
ONLY — no keychain, no private key, no external tool; and (2) the hermetic tests
and the fixture generator (regenerate_fixture.py's parity self-check, which
keeps this verifier byte-equivalent to Sparkle's own sign_update). In-app update
verification is Sparkle's; the BoringSSL roamux/app/appcast_verifier is the
separate C++ pipeline/test helper. Signing here is test-only: the release job
signs with Sparkle's sign_update and the production key never appears in the
repo.

verify() enforces RFC 8032 §5.1.7 decoding: S < l, canonical y (< q), and no
x = 0 with the sign bit set (roam-286). Small-order rejection is deliberately
not added — policy beyond the RFC, unnecessary for verifying our own canonical
signatures under a fixed trusted key."""

import hashlib

b = 256
q = 2 ** 255 - 19
l = 2 ** 252 + 27742317777372353535851937790883648493


def H(m):
    return hashlib.sha512(m).digest()


def inv(x):
    return pow(x, q - 2, q)


d = -121665 * inv(121666) % q
I = pow(2, (q - 1) // 4, q)


def xrecover(y):
    xx = (y * y - 1) * inv(d * y * y + 1)
    x = pow(xx, (q + 3) // 8, q)
    if (x * x - xx) % q != 0:
        x = (x * I) % q
    if x % 2 != 0:
        x = q - x
    return x


By = 4 * inv(5) % q
Bx = xrecover(By)
B = [Bx % q, By % q]


def edwards(P, Q):
    x1, y1, x2, y2 = P[0], P[1], Q[0], Q[1]
    x3 = (x1 * y2 + x2 * y1) * inv(1 + d * x1 * x2 * y1 * y2)
    y3 = (y1 * y2 + x1 * x2) * inv(1 - d * x1 * x2 * y1 * y2)
    return [x3 % q, y3 % q]


def scalarmult(P, e):
    if e == 0:
        return [0, 1]
    Q = scalarmult(P, e // 2)
    Q = edwards(Q, Q)
    if e & 1:
        Q = edwards(Q, P)
    return Q


def encodeint(y):
    return y.to_bytes(b // 8, "little")


def encodepoint(P):
    return (P[1] | ((P[0] & 1) << (b - 1))).to_bytes(b // 8, "little")


def bit(h, i):
    return (h[i // 8] >> (i % 8)) & 1


def publickey(sk):
    h = H(sk)
    a = 2 ** (b - 2) + sum(2 ** i * bit(h, i) for i in range(3, b - 2))
    return encodepoint(scalarmult(B, a))


def Hint(m):
    h = H(m)
    return sum(2 ** i * bit(h, i) for i in range(2 * b))


def signature(m, sk, pk):
    h = H(sk)
    a = 2 ** (b - 2) + sum(2 ** i * bit(h, i) for i in range(3, b - 2))
    r = Hint(h[b // 8:b // 4] + m)
    R = scalarmult(B, r)
    S = (r + Hint(encodepoint(R) + pk + m) * a) % l
    return encodepoint(R) + encodeint(S)


def isoncurve(P):
    x, y = P
    return (-x * x + y * y - 1 - d * x * x * y * y) % q == 0


def decodeint(s):
    return int.from_bytes(s, "little")


def decodepoint(s):
    y = int.from_bytes(s, "little") & ((1 << (b - 1)) - 1)
    if y >= q:
        raise ValueError("non-canonical point encoding (y >= q)")  # RFC 8032 §5.1.3 step 1
    x = xrecover(y)
    sign = bit(s, b - 1)
    if x == 0 and sign:
        raise ValueError("non-canonical point encoding (x = 0 with sign bit set)")  # §5.1.3 step 4
    if x & 1 != sign:
        x = q - x
    P = [x, y]
    if not isoncurve(P):
        raise ValueError("decoding point that is not on curve")
    return P


def verify(m, s, pk):
    """True iff signature `s` (64 bytes) over `m` verifies with `pk` (32)."""
    if len(s) != b // 4 or len(pk) != b // 8:
        return False
    try:
        R = decodepoint(s[:b // 8])
        A = decodepoint(pk)
    except ValueError:
        return False
    S = decodeint(s[b // 8:b // 4])
    if S >= l:
        return False  # RFC 8032 §5.1.7 step 1: reject non-canonical scalars (malleable S + l)
    h = Hint(encodepoint(R) + pk + m)
    return scalarmult(B, S) == edwards(R, scalarmult(A, h))
