/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * NmcSigVerify — Namecoin/Bitcoin message signature verification
 *
 * Implements secp256k1 ECDSA public-key recovery + Namecoin address derivation.
 * Used to verify ncgencert caAIAMessage.txt signatures.
 *
 * Architecture note: NSS (Firefox's crypto library) does not support secp256k1.
 * We implement the minimal required operations inline:
 *   - 256-bit big integer arithmetic (field Fp and group order Fn)
 *   - secp256k1 elliptic curve point arithmetic
 *   - ECDSA public key recovery from (r, s, hash, recovery_bit)
 *   - SHA-256 (via NSS HASH_HashBuf), RIPEMD-160 (via NSS)
 *   - Base58Check encoding for Namecoin address formatting
 */

#include "NmcSigVerify.h"
#include "nsNamecoinErrors.h"

#include "mozilla/Logging.h"
#include "mozilla/Base64.h"
#include "nsString.h"
#include "nsTArray.h"

// NSS crypto
#include "sechash.h"   // HASH_HashBuf, HASH_AlgSHA256
#include "pk11func.h"

#include <stdint.h>
#include <string.h>

namespace mozilla {
namespace net {

static LazyLogModule gNmcSigLog("namecoin");
#define SIG_LOG(...) MOZ_LOG(gNmcSigLog, LogLevel::Debug, (__VA_ARGS__))
#define NMC_SIG_ERR(...) MOZ_LOG(gNmcSigLog, LogLevel::Error, (__VA_ARGS__))

// ==========================================================================
// 256-bit big integer (little-endian limbs, 4x uint64_t)
// All arithmetic is modular (mod p or mod n as appropriate).
// ==========================================================================

struct U256 {
  uint64_t limbs[4];  // limbs[0] = least significant

  U256() { memset(limbs, 0, sizeof(limbs)); }

  static U256 fromBE(const uint8_t* bytes) {
    U256 r;
    for (int i = 0; i < 4; i++) {
      uint64_t v = 0;
      for (int j = 0; j < 8; j++) {
        v = (v << 8) | bytes[i * 8 + j];
      }
      r.limbs[3 - i] = v;
    }
    return r;
  }

  void toBE(uint8_t* bytes) const {
    for (int i = 0; i < 4; i++) {
      uint64_t v = limbs[3 - i];
      for (int j = 7; j >= 0; j--) {
        bytes[i * 8 + j] = (uint8_t)(v & 0xff);
        v >>= 8;
      }
    }
  }

  bool isZero() const {
    return (limbs[0] | limbs[1] | limbs[2] | limbs[3]) == 0;
  }

  bool operator==(const U256& o) const {
    return limbs[0] == o.limbs[0] && limbs[1] == o.limbs[1] &&
           limbs[2] == o.limbs[2] && limbs[3] == o.limbs[3];
  }

  bool operator<(const U256& o) const {
    for (int i = 3; i >= 0; i--) {
      if (limbs[i] != o.limbs[i]) return limbs[i] < o.limbs[i];
    }
    return false;
  }

  bool operator>=(const U256& o) const { return !(*this < o); }
};

// secp256k1 parameters
// p = FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEFFFFFC2F
static const uint8_t kSecp256k1_P_BE[32] = {
  0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
  0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
  0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
  0xff,0xff,0xff,0xfe,0xff,0xff,0xfc,0x2f
};

// n = FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141
static const uint8_t kSecp256k1_N_BE[32] = {
  0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
  0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xfe,
  0xba,0xae,0xdc,0xe6,0xaf,0x48,0xa0,0x3b,
  0xbf,0xd2,0x5e,0x8c,0xd0,0x36,0x41,0x41
};

// Gx = 79BE667EF9DCBBAC55A06295CE870B07029BFCDB2DCE28D959F2815B16F81798
static const uint8_t kSecp256k1_Gx_BE[32] = {
  0x79,0xbe,0x66,0x7e,0xf9,0xdc,0xbb,0xac,
  0x55,0xa0,0x62,0x95,0xce,0x87,0x0b,0x07,
  0x02,0x9b,0xfc,0xdb,0x2d,0xce,0x28,0xd9,
  0x59,0xf2,0x81,0x5b,0x16,0xf8,0x17,0x98
};

// Gy = 483ADA7726A3C4655DA4FBFC0E1108A8FD17B448A68554199C47D08FFB10D4B8
static const uint8_t kSecp256k1_Gy_BE[32] = {
  0x48,0x3a,0xda,0x77,0x26,0xa3,0xc4,0x65,
  0x5d,0xa4,0xfb,0xfc,0x0e,0x11,0x08,0xa8,
  0xfd,0x17,0xb4,0x48,0xa6,0x85,0x54,0x19,
  0x9c,0x47,0xd0,0x8f,0xfb,0x10,0xd4,0xb8
};

// ---------------------------------------------------------------------------
// Modular arithmetic helpers (using __uint128_t for 64-bit overflow)
// ---------------------------------------------------------------------------

static U256 u256_add(const U256& a, const U256& b, const U256& mod) {
  // a + b mod mod (assumes a, b < mod)
  uint64_t carry = 0;
  U256 r;
  for (int i = 0; i < 4; i++) {
    __uint128_t s = (__uint128_t)a.limbs[i] + b.limbs[i] + carry;
    r.limbs[i] = (uint64_t)s;
    carry = (uint64_t)(s >> 64);
  }
  // reduce if r >= mod
  if (carry || r >= mod) {
    uint64_t borrow = 0;
    for (int i = 0; i < 4; i++) {
      __uint128_t d = (__uint128_t)r.limbs[i] - mod.limbs[i] - borrow;
      r.limbs[i] = (uint64_t)d;
      borrow = (d >> 64) ? 1 : 0;
    }
  }
  return r;
}

static U256 u256_sub(const U256& a, const U256& b, const U256& mod) {
  // a - b mod mod (assumes a, b < mod)
  uint64_t borrow = 0;
  U256 r;
  for (int i = 0; i < 4; i++) {
    __uint128_t d = (__uint128_t)a.limbs[i] - b.limbs[i] - borrow;
    r.limbs[i] = (uint64_t)d;
    borrow = (d >> 64) ? 1 : 0;
  }
  if (borrow) {
    // add mod back
    uint64_t carry = 0;
    for (int i = 0; i < 4; i++) {
      __uint128_t s = (__uint128_t)r.limbs[i] + mod.limbs[i] + carry;
      r.limbs[i] = (uint64_t)s;
      carry = (uint64_t)(s >> 64);
    }
  }
  return r;
}

static U256 u256_mul_mod(const U256& a, const U256& b, const U256& mod) {
  // Compute a * b mod mod using schoolbook multiplication + Barrett reduction.
  // For secp256k1, both p and n are close to 2^256, so we need 512-bit product.
  // Simple but correct approach: double-and-add with modular reduction.
  U256 result;
  U256 addend = a;
  // reduce addend mod mod first
  if (addend >= mod) {
    uint64_t borrow = 0;
    for (int i = 0; i < 4; i++) {
      __uint128_t d = (__uint128_t)addend.limbs[i] - mod.limbs[i] - borrow;
      addend.limbs[i] = (uint64_t)d;
      borrow = (d >> 64) ? 1 : 0;
    }
  }

  U256 bCopy = b;
  for (int bit = 0; bit < 256; bit++) {
    int limb = bit / 64;
    int shift = bit % 64;
    if ((bCopy.limbs[limb] >> shift) & 1) {
      result = u256_add(result, addend, mod);
    }
    addend = u256_add(addend, addend, mod);
  }
  return result;
}

static U256 u256_negate(const U256& a, const U256& mod) {
  if (a.isZero()) return a;
  return u256_sub(mod, a, mod);
}

// Modular inverse via Fermat's little theorem: a^(mod-2) mod mod
// (works when mod is prime)
static U256 u256_modinv(const U256& a, const U256& mod) {
  // Compute exp = mod - 2
  uint64_t borrow = 0;
  U256 exp;
  uint64_t two[4] = {2, 0, 0, 0};
  for (int i = 0; i < 4; i++) {
    __uint128_t d = (__uint128_t)mod.limbs[i] - two[i] - borrow;
    exp.limbs[i] = (uint64_t)d;
    borrow = (d >> 64) ? 1 : 0;
  }
  // Square-and-multiply
  U256 result;
  result.limbs[0] = 1;  // result = 1
  U256 base = a;
  for (int bit = 0; bit < 256; bit++) {
    int limb = bit / 64;
    int shift = bit % 64;
    if ((exp.limbs[limb] >> shift) & 1) {
      result = u256_mul_mod(result, base, mod);
    }
    base = u256_mul_mod(base, base, mod);
  }
  return result;
}

// ==========================================================================
// Elliptic curve point (Jacobian projective coordinates)
// Point at infinity: X=0, Y=1, Z=0
// ==========================================================================

struct JPoint {
  U256 X, Y, Z;
  bool isInfinity;

  JPoint() : isInfinity(true) {
    Y.limbs[0] = 1;  // Y=1 for point at infinity convention
  }

  JPoint(const U256& x, const U256& y) : isInfinity(false) {
    X = x;
    Y = y;
    Z.limbs[0] = 1;  // Z=1 for affine point
  }
};

static U256 gP = U256::fromBE(kSecp256k1_P_BE);

static JPoint jpoint_double(const JPoint& P) {
  if (P.isInfinity) return P;

  const U256& p = gP;

  // Using formulas from https://hyperelliptic.org/EFD/g1p/auto-shortw-jacobian-0.html
  // doubling (a=0 for secp256k1): dbl-2009-l
  // A = X1^2, B = Y1^2, C = B^2
  U256 A = u256_mul_mod(P.X, P.X, p);
  U256 B = u256_mul_mod(P.Y, P.Y, p);
  U256 C = u256_mul_mod(B, B, p);

  // D = 2*((X1+B)^2 - A - C)
  U256 xpb = u256_add(P.X, B, p);
  U256 xpb2 = u256_mul_mod(xpb, xpb, p);
  U256 D = u256_sub(xpb2, A, p);
  D = u256_sub(D, C, p);
  D = u256_add(D, D, p);

  // E = 3*A (secp256k1 has a=0, so this is just 3*X1^2)
  U256 E = u256_add(A, u256_add(A, A, p), p);

  // F = E^2
  U256 F = u256_mul_mod(E, E, p);

  // X3 = F - 2*D
  U256 X3 = u256_sub(F, u256_add(D, D, p), p);

  // Y3 = E*(D-X3) - 8*C
  U256 dminx3 = u256_sub(D, X3, p);
  U256 Y3 = u256_mul_mod(E, dminx3, p);
  U256 eightC = u256_mul_mod(C, U256::fromBE((const uint8_t[]){
    0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,8}), p);
  Y3 = u256_sub(Y3, eightC, p);

  // Z3 = 2*Y1*Z1
  U256 Z3 = u256_mul_mod(P.Y, P.Z, p);
  Z3 = u256_add(Z3, Z3, p);

  JPoint R;
  R.isInfinity = false;
  R.X = X3;
  R.Y = Y3;
  R.Z = Z3;
  return R;
}

static JPoint jpoint_add(const JPoint& P, const JPoint& Q) {
  if (P.isInfinity) return Q;
  if (Q.isInfinity) return P;

  const U256& p = gP;

  // add-2007-bl
  U256 Z1Z1 = u256_mul_mod(P.Z, P.Z, p);
  U256 Z2Z2 = u256_mul_mod(Q.Z, Q.Z, p);
  U256 U1   = u256_mul_mod(P.X, Z2Z2, p);
  U256 U2   = u256_mul_mod(Q.X, Z1Z1, p);
  U256 S1   = u256_mul_mod(u256_mul_mod(P.Y, Q.Z, p), Z2Z2, p);
  U256 S2   = u256_mul_mod(u256_mul_mod(Q.Y, P.Z, p), Z1Z1, p);
  U256 H    = u256_sub(U2, U1, p);
  U256 R2   = u256_sub(S2, S1, p);  // = R in the formula

  if (H.isZero()) {
    if (R2.isZero()) {
      return jpoint_double(P);  // P == Q
    }
    JPoint inf;
    return inf;  // P == -Q
  }

  U256 HH   = u256_mul_mod(H, H, p);
  U256 HHH  = u256_mul_mod(H, HH, p);
  U256 V    = u256_mul_mod(U1, HH, p);

  // X3 = R^2 - HHH - 2*V
  U256 X3 = u256_sub(u256_mul_mod(R2, R2, p), HHH, p);
  X3 = u256_sub(X3, u256_add(V, V, p), p);

  // Y3 = R*(V-X3) - S1*HHH
  U256 Y3 = u256_mul_mod(R2, u256_sub(V, X3, p), p);
  Y3 = u256_sub(Y3, u256_mul_mod(S1, HHH, p), p);

  // Z3 = H*Z1*Z2
  U256 Z3 = u256_mul_mod(u256_mul_mod(H, P.Z, p), Q.Z, p);

  JPoint R;
  R.isInfinity = false;
  R.X = X3;
  R.Y = Y3;
  R.Z = Z3;
  return R;
}

// Convert Jacobian to affine
static bool jpoint_to_affine(const JPoint& P, U256& outX, U256& outY) {
  if (P.isInfinity) return false;

  const U256& p = gP;
  U256 Zinv  = u256_modinv(P.Z, p);
  U256 Zinv2 = u256_mul_mod(Zinv, Zinv, p);
  U256 Zinv3 = u256_mul_mod(Zinv2, Zinv, p);
  outX = u256_mul_mod(P.X, Zinv2, p);
  outY = u256_mul_mod(P.Y, Zinv3, p);
  return true;
}

// Scalar multiplication: k * P
static JPoint point_mul(const U256& k, const JPoint& P) {
  JPoint R;  // infinity
  JPoint addend = P;
  for (int bit = 0; bit < 256; bit++) {
    int limb = bit / 64;
    int shift = bit % 64;
    if ((k.limbs[limb] >> shift) & 1) {
      R = jpoint_add(R, addend);
    }
    addend = jpoint_double(addend);
  }
  return R;
}

// ==========================================================================
// SHA-256d (double SHA-256) via NSS
// ==========================================================================

static bool sha256d(const uint8_t* data, size_t len, uint8_t out[32]) {
  uint8_t tmp[32];
  if (HASH_HashBuf(HASH_AlgSHA256, tmp, data, (unsigned)len) != SECSuccess)
    return false;
  if (HASH_HashBuf(HASH_AlgSHA256, out, tmp, 32) != SECSuccess)
    return false;
  return true;
}

// ==========================================================================
// Bitcoin message hash
// ==========================================================================

static bool bitcoin_message_hash(const nsACString& aMessage, uint8_t out[32]) {
  // prefix = "\x18Bitcoin Signed Message:\n" + varint(len) + message
  static const char kMagic[] = "\x18" "Bitcoin Signed Message:\n";
  static const size_t kMagicLen = sizeof(kMagic) - 1;

  size_t msgLen = aMessage.Length();
  nsTArray<uint8_t> buf;

  // Append magic prefix
  buf.AppendElements(reinterpret_cast<const uint8_t*>(kMagic), kMagicLen);

  // Append varint for message length (Bitcoin varint encoding)
  if (msgLen < 0xfd) {
    buf.AppendElement((uint8_t)msgLen);
  } else if (msgLen <= 0xffff) {
    buf.AppendElement(0xfd);
    buf.AppendElement((uint8_t)(msgLen & 0xff));
    buf.AppendElement((uint8_t)((msgLen >> 8) & 0xff));
  } else {
    // Extremely long message — not expected for caAIAMessage
    buf.AppendElement(0xfe);
    for (int i = 0; i < 4; i++) buf.AppendElement((uint8_t)((msgLen >> (i * 8)) & 0xff));
  }

  // Append message bytes
  buf.AppendElements(reinterpret_cast<const uint8_t*>(aMessage.BeginReading()),
                     msgLen);

  return sha256d(buf.Elements(), buf.Length(), out);
}

// ==========================================================================
// RIPEMD-160 (standalone implementation, ~RFC 1320 style)
// NSS does not expose RIPEMD-160 via HASH_HashBuf.
// ==========================================================================

static uint32_t rmd_rotl(uint32_t x, int n) {
  return (x << n) | (x >> (32 - n));
}

static uint32_t rmd_f(int j, uint32_t x, uint32_t y, uint32_t z) {
  if (j < 16)  return x ^ y ^ z;
  if (j < 32)  return (x & y) | (~x & z);
  if (j < 48)  return (x | ~y) ^ z;
  if (j < 64)  return (x & z) | (y & ~z);
  return x ^ (y | ~z);
}

static const uint32_t kRmdK[5]  = {0x00000000,0x5A827999,0x6ED9EBA1,0x8F1BBCDC,0xA953FD4E};
static const uint32_t kRmdKP[5] = {0x50A28BE6,0x5C4DD124,0x6D703EF3,0x7A6D76E9,0x00000000};

static const uint8_t kRmdR[80] = {
  0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,
  7,4,13,1,10,6,15,3,12,0,9,5,2,14,11,8,
  3,10,14,4,9,15,8,1,2,7,0,6,13,11,5,12,
  1,9,11,10,0,8,12,4,13,3,7,15,14,5,6,2,
  4,0,5,9,7,12,2,10,14,1,3,8,11,6,15,13
};
static const uint8_t kRmdRP[80] = {
  5,14,7,0,9,2,11,4,13,6,15,8,1,10,3,12,
  6,11,3,7,0,13,5,10,14,15,8,12,4,9,1,2,
  15,5,1,3,7,14,6,9,11,8,12,2,10,0,4,13,
  8,6,4,1,3,11,15,0,5,12,2,13,9,7,10,14,
  12,15,10,4,1,5,8,7,6,2,13,14,0,3,9,11
};
static const uint8_t kRmdS[80] = {
  11,14,15,12,5,8,7,9,11,13,14,15,6,7,9,8,
  7,6,8,13,11,9,7,15,7,12,15,9,11,7,13,12,
  11,13,6,7,14,9,13,15,14,8,13,6,5,12,7,5,
  11,12,14,15,14,15,9,8,9,14,5,6,8,6,5,12,
  9,15,5,11,6,8,13,12,5,12,13,14,11,8,5,6
};
static const uint8_t kRmdSP[80] = {
  8,9,9,11,13,15,15,5,7,7,8,11,14,14,12,6,
  9,13,15,7,12,8,9,11,7,7,12,7,6,15,13,11,
  9,7,15,11,8,6,6,14,12,13,5,14,13,13,7,5,
  15,5,8,11,14,14,6,14,6,9,12,9,12,5,15,8,
  8,5,12,9,12,5,14,6,8,13,6,5,15,13,11,11
};

static void ripemd160(const uint8_t* data, size_t len, uint8_t out[20]) {
  // Initialize hash state
  uint32_t h0=0x67452301, h1=0xEFCDAB89, h2=0x98BADCFE, h3=0x10325476, h4=0xC3D2E1F0;

  // Pad message
  nsTArray<uint8_t> padded;
  padded.AppendElements(data, len);
  padded.AppendElement(0x80);
  while ((padded.Length() % 64) != 56) padded.AppendElement(0);
  uint64_t bitlen = (uint64_t)len * 8;
  for (int i = 0; i < 8; i++) padded.AppendElement((uint8_t)(bitlen >> (i*8)));

  // Process each 64-byte block
  for (size_t blk = 0; blk < padded.Length(); blk += 64) {
    const uint8_t* block = padded.Elements() + blk;
    uint32_t X[16];
    for (int i = 0; i < 16; i++) {
      X[i] = (uint32_t)block[i*4] | ((uint32_t)block[i*4+1]<<8) |
              ((uint32_t)block[i*4+2]<<16) | ((uint32_t)block[i*4+3]<<24);
    }

    uint32_t al=h0,bl=h1,cl=h2,dl=h3,el=h4;
    uint32_t ar=h0,br=h1,cr=h2,dr=h3,er=h4;

    for (int j = 0; j < 80; j++) {
      int jj = j/16;
      uint32_t T = rmd_rotl(al + rmd_f(j,bl,cl,dl) + X[kRmdR[j]] + kRmdK[jj], kRmdS[j]) + el;
      al=el; el=dl; dl=rmd_rotl(cl,10); cl=bl; bl=T;

      T = rmd_rotl(ar + rmd_f(79-j,br,cr,dr) + X[kRmdRP[j]] + kRmdKP[jj], kRmdSP[j]) + er;
      ar=er; er=dr; dr=rmd_rotl(cr,10); cr=br; br=T;
    }

    uint32_t T2 = h1 + cl + dr;
    h1 = h2 + dl + er; h2 = h3 + el + ar;
    h3 = h4 + al + br; h4 = h0 + bl + cr; h0 = T2;
  }

  // Output (little-endian)
  for (int i = 0; i < 4; i++) {
    out[0*4+i] = (h0>>(i*8))&0xff; out[1*4+i] = (h1>>(i*8))&0xff;
    out[2*4+i] = (h2>>(i*8))&0xff; out[3*4+i] = (h3>>(i*8))&0xff;
    out[4*4+i] = (h4>>(i*8))&0xff;
  }
}

// ==========================================================================
// Base58Check encoding for Namecoin address
// ==========================================================================

static const char kBase58Alphabet[] =
  "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

static void base58_encode(const uint8_t* data, size_t len, nsACString& out) {
  // Count leading zeros
  size_t zeros = 0;
  while (zeros < len && data[zeros] == 0) zeros++;

  // Convert to base-58
  nsTArray<uint8_t> digits;
  for (size_t i = 0; i < len; i++) {
    int carry = data[i];
    for (size_t j = 0; j < digits.Length(); j++) {
      carry += 256 * (int)digits[j];
      digits[j] = (uint8_t)(carry % 58);
      carry /= 58;
    }
    while (carry > 0) {
      digits.AppendElement((uint8_t)(carry % 58));
      carry /= 58;
    }
  }

  // Leading '1's for each leading zero byte
  out.Truncate();
  for (size_t i = 0; i < zeros; i++) out.Append('1');
  for (int i = (int)digits.Length() - 1; i >= 0; i--) {
    out.Append(kBase58Alphabet[digits[i]]);
  }
}

// Derive Namecoin address from compressed public key bytes (33 bytes)
static void pubkey_to_namecoin_address(const uint8_t pubkey[33], nsACString& addr) {
  // Step 1: SHA-256 of pubkey
  uint8_t sha[32];
  HASH_HashBuf(HASH_AlgSHA256, sha, pubkey, 33);

  // Step 2: RIPEMD-160 of SHA-256
  uint8_t rmd[20];
  ripemd160(sha, 32, rmd);

  // Step 3: Prepend Namecoin version byte (0x34)
  uint8_t versioned[21];
  versioned[0] = 0x34;  // Namecoin mainnet P2PKH
  memcpy(versioned + 1, rmd, 20);

  // Step 4: Checksum = first 4 bytes of SHA256d(versioned)
  uint8_t checksum_full[32];
  sha256d(versioned, 21, checksum_full);

  // Step 5: Payload = versioned + checksum
  uint8_t payload[25];
  memcpy(payload, versioned, 21);
  memcpy(payload + 21, checksum_full, 4);

  // Step 6: Base58 encode
  base58_encode(payload, 25, addr);
}

// ==========================================================================
// ECDSA public key recovery from (r, s, recid, hash)
// Returns the recovered public key as a 33-byte compressed point.
// ==========================================================================

// Compute the secp256k1 point corresponding to x (with given parity)
static bool lift_x(const U256& x, bool odd, JPoint& out) {
  const U256& p = gP;

  // y^2 = x^3 + 7 (mod p)  [secp256k1: a=0, b=7]
  U256 x2 = u256_mul_mod(x, x, p);
  U256 x3 = u256_mul_mod(x2, x, p);
  // b = 7
  uint8_t seven_be[32] = {};
  seven_be[31] = 7;
  U256 b = U256::fromBE(seven_be);
  U256 rhs = u256_add(x3, b, p);  // x^3 + 7

  // Compute y = sqrt(rhs) mod p
  // For secp256k1: p ≡ 3 (mod 4), so sqrt(a) = a^((p+1)/4) mod p
  // (p+1)/4 = 3FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFBFFFFF0C
  static const uint8_t kSqrtExp[32] = {
    0x3f,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
    0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
    0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
    0xff,0xff,0xff,0xff,0xbf,0xff,0xff,0x0c
  };
  U256 exp = U256::fromBE(kSqrtExp);

  // Compute y = rhs^exp mod p (square-and-multiply)
  U256 y;
  y.limbs[0] = 1;
  U256 base2 = rhs;
  for (int bit = 0; bit < 256; bit++) {
    int limb = bit / 64;
    int shift = bit % 64;
    if ((exp.limbs[limb] >> shift) & 1) {
      y = u256_mul_mod(y, base2, p);
    }
    base2 = u256_mul_mod(base2, base2, p);
  }

  // Verify y^2 == rhs
  U256 y2 = u256_mul_mod(y, y, p);
  if (!(y2 == rhs)) {
    return false;  // x is not on the curve
  }

  // Adjust parity
  bool yIsOdd = (y.limbs[0] & 1) != 0;
  if (yIsOdd != odd) {
    y = u256_sub(p, y, p);  // negate y
  }

  out.isInfinity = false;
  out.X = x;
  out.Y = y;
  out.Z.limbs[0] = 1;
  return true;
}

static bool ecdsa_recover(const uint8_t hash[32], const uint8_t r_be[32],
                           const uint8_t s_be[32], int recid,
                           uint8_t pubkey_out[33]) {
  U256 n = U256::fromBE(kSecp256k1_N_BE);
  U256 p = U256::fromBE(kSecp256k1_P_BE);

  U256 r = U256::fromBE(r_be);
  U256 s = U256::fromBE(s_be);
  U256 e = U256::fromBE(hash);

  if (r.isZero() || s.isZero()) return false;
  if (r >= n || s >= n) return false;

  // x = r + (recid/2) * n
  U256 x = r;
  if (recid & 2) {
    // x might be r + n, but we only support recid 0/1 for non-overflow
    // (recid >= 2 is very rare and requires x > p − n)
    uint64_t carry = 0;
    for (int i = 0; i < 4; i++) {
      __uint128_t s2 = (__uint128_t)r.limbs[i] + n.limbs[i] + carry;
      x.limbs[i] = (uint64_t)s2;
      carry = (uint64_t)(s2 >> 64);
    }
    if (x >= p) return false;
  }

  bool yOdd = (recid & 1) != 0;
  JPoint R;
  if (!lift_x(x, yOdd, R)) return false;

  // Compute public key: Q = r^(-1) * (s*R - e*G)
  U256 rInv = u256_modinv(r, n);

  JPoint G(U256::fromBE(kSecp256k1_Gx_BE), U256::fromBE(kSecp256k1_Gy_BE));

  // sR = s * R
  JPoint sR = point_mul(s, R);

  // eNeg = -e mod n (to compute -e*G)
  U256 eNeg = u256_negate(e, n);

  // eNegG = (-e) * G
  JPoint eNegG = point_mul(eNeg, G);

  // sR + eNegG = s*R - e*G
  JPoint sum = jpoint_add(sR, eNegG);

  // Q = rInv * (sR - eG)
  JPoint Q = point_mul(rInv, sum);

  U256 Qx, Qy;
  if (!jpoint_to_affine(Q, Qx, Qy)) return false;

  // Compress: 0x02 if Y is even, 0x03 if odd
  pubkey_out[0] = (Qy.limbs[0] & 1) ? 0x03 : 0x02;
  Qx.toBE(pubkey_out + 1);
  return true;
}

// ==========================================================================
// Public API
// ==========================================================================

nsresult NmcRecoverSigningAddress(const nsACString& aMessage,
                                  const nsACString& aSigBase64,
                                  nsACString& aAddress) {
  // Decode base64 signature
  nsAutoCString sigDecoded;
  nsresult rv = Base64Decode(aSigBase64, sigDecoded);
  if (NS_FAILED(rv) || sigDecoded.Length() != 65) {
    NMC_SIG_ERR("NmcRecoverSigningAddress: sig decode failed (len=%u)",
            (unsigned)sigDecoded.Length());
    return NS_ERROR_FAILURE;
  }

  const uint8_t* sigBytes = reinterpret_cast<const uint8_t*>(sigDecoded.BeginReading());
  uint8_t recoveryByte = sigBytes[0];

  // Recovery byte: 27-30 = uncompressed, 31-34 = compressed
  bool compressed = (recoveryByte >= 31);
  int recid = (recoveryByte - (compressed ? 31 : 27));
  if (recid < 0 || recid > 3) {
    NMC_SIG_ERR("NmcRecoverSigningAddress: invalid recovery byte %u", recoveryByte);
    return NS_ERROR_FAILURE;
  }

  const uint8_t* r_be = sigBytes + 1;
  const uint8_t* s_be = sigBytes + 33;

  // Compute message hash
  uint8_t hash[32];
  if (!bitcoin_message_hash(aMessage, hash)) {
    NMC_SIG_ERR("NmcRecoverSigningAddress: hash computation failed");
    return NS_ERROR_FAILURE;
  }

  // Recover public key
  uint8_t pubkey[33];
  if (!ecdsa_recover(hash, r_be, s_be, recid, pubkey)) {
    NMC_SIG_ERR("NmcRecoverSigningAddress: ECDSA recovery failed (recid=%d)", recid);
    return NS_ERROR_FAILURE;
  }

  // Derive address (always use compressed form for modern Namecoin)
  pubkey_to_namecoin_address(pubkey, aAddress);
  SIG_LOG("NmcRecoverSigningAddress: recovered address = %s",
          nsPromiseFlatCString(aAddress).get());
  return NS_OK;
}

bool NmcVerifyMessageSignature(const nsACString& aAddress,
                               const nsACString& aMessage,
                               const nsACString& aSigBase64) {
  nsAutoCString recovered;
  nsresult rv = NmcRecoverSigningAddress(aMessage, aSigBase64, recovered);
  if (NS_FAILED(rv)) {
    NMC_SIG_ERR("NmcVerifyMessageSignature: recovery failed");
    return false;
  }
  bool match = recovered.Equals(aAddress);
  if (match) {
    SIG_LOG("NmcVerifyMessageSignature: address match: %s",
            nsPromiseFlatCString(aAddress).get());
  } else {
    SIG_LOG("NmcVerifyMessageSignature: address mismatch: expected=%s recovered=%s",
            nsPromiseFlatCString(aAddress).get(),
            recovered.get());
  }
  return match;
}

}  // namespace net
}  // namespace mozilla
