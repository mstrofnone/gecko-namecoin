/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef NmcSigVerify_h__
#define NmcSigVerify_h__

/**
 * NmcSigVerify — Namecoin message signature verification
 *
 * Implements Bitcoin-compatible "signmessage" signature verification for
 * Namecoin addresses. Used to verify that the caAIAMessage.txt produced by
 * ncgencert (https://github.com/namecoin/ncgencert) was signed by the
 * private key corresponding to the Namecoin address that controls a domain.
 *
 * Signature format (65 bytes, base64-encoded):
 *   byte 0:     recovery flag (27, 28, 29, or 30; +4 for compressed key)
 *   bytes 1-32: r (big-endian)
 *   bytes 33-64: s (big-endian)
 *
 * Message hashing:
 *   prefix = "\x18Bitcoin Signed Message:\n" + varint(len(message)) + message
 *   hash   = SHA256d(prefix)  (double SHA-256)
 *
 * Address derivation from recovered public key:
 *   1. SHA-256 of compressed public key bytes
 *   2. RIPEMD-160 of step 1
 *   3. Prepend version byte (0x34 = Namecoin mainnet)
 *   4. Checksum = first 4 bytes of SHA256d(versioned payload)
 *   5. Base58Check encode (versioned payload + checksum)
 *
 * Curve: secp256k1
 *   p  = FFFFFFFF FFFFFFFF FFFFFFFF FFFFFFFF FFFFFFFF FFFFFFFF FFFFFFFE FFFFFC2F
 *   n  = FFFFFFFF FFFFFFFF FFFFFFFF FFFFFFFE BAAEDCE6 AF48A03B BFD25E8C D0364141
 *   Gx = 79BE667E F9DCBBAC 55A06295 CE870B07 029BFCDB 2DCE28D9 59F2815B 16F81798
 *   Gy = 483ADA77 26A3C465 5DA4FBFC 0E1108A8 FD17B448 A6855419 9C47D08F FB10D4B8
 *
 * Reference:
 *   bitcoin/src/util/message.cpp — SignVerifyMessage
 *   ncgencert aiaparent.go — caAIAMessage format
 */

#include "nscore.h"
#include "nsString.h"

namespace mozilla {
namespace net {

/**
 * Verify a Namecoin message signature.
 *
 * @param aAddress    Expected Namecoin address (Base58Check, e.g. "N...")
 * @param aMessage    The message that was signed (UTF-8 string)
 * @param aSigBase64  Base64-encoded 65-byte signature
 * @returns true if the signature is valid for this address+message pair
 */
bool NmcVerifyMessageSignature(const nsACString& aAddress,
                               const nsACString& aMessage,
                               const nsACString& aSigBase64);

/**
 * Recover the Namecoin address from a message signature.
 *
 * @param aMessage    The signed message
 * @param aSigBase64  Base64-encoded 65-byte signature
 * @param aAddress    Output: recovered Namecoin address
 * @returns NS_OK on success
 */
nsresult NmcRecoverSigningAddress(const nsACString& aMessage,
                                  const nsACString& aSigBase64,
                                  nsACString& aAddress);

}  // namespace net
}  // namespace mozilla

#endif  // NmcSigVerify_h__
