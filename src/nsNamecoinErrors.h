/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsNamecoinErrors_h__
#define nsNamecoinErrors_h__

/**
 * Namecoin-specific error codes for .bit domain resolution.
 *
 * These codes allow the Gecko error page system (netError.xhtml) to display
 * informative, Namecoin-aware messages instead of the generic "Server not
 * found" page when .bit resolution fails.
 *
 * Module number: 77 (MODULE_NAMECOIN)
 *
 * Error code layout (see xpcom/base/ErrorList.py, nsError.h):
 *   NS_ERROR_GENERATE_FAILURE(module, code)
 *     = 0x80000000 | (module << 16) | code
 *
 * These are registered in netwerk/base/nsNetErrorCodes.h for the network
 * layer to propagate them through nsHostResolver → docshell → netError.xhtml.
 */

#include "nsError.h"

// Module 77 — Namecoin (.bit) resolution
#define NS_ERROR_MODULE_NAMECOIN 77

/**
 * NS_ERROR_NAMECOIN_NOT_FOUND
 *
 * The requested d/ name has no transaction history on the Namecoin blockchain.
 * This means the name was never registered, or the ElectrumX index has no
 * record of it.
 *
 * User-facing: "Name not found on the Namecoin blockchain"
 */
#define NS_ERROR_NAMECOIN_NOT_FOUND \
  NS_ERROR_GENERATE_FAILURE(NS_ERROR_MODULE_NAMECOIN, 1)

/**
 * NS_ERROR_NAMECOIN_EXPIRED
 *
 * The name exists on the blockchain but its most recent NAME_UPDATE is older
 * than 36,000 blocks (~250 days). Expired names MUST NOT be resolved because
 * they are eligible for re-registration by a different owner.
 *
 * User-facing: "This Namecoin name has expired"
 * Includes: last update height and current block height.
 */
#define NS_ERROR_NAMECOIN_EXPIRED \
  NS_ERROR_GENERATE_FAILURE(NS_ERROR_MODULE_NAMECOIN, 2)

/**
 * NS_ERROR_NAMECOIN_SERVERS_UNREACHABLE
 *
 * All configured ElectrumX servers failed to respond within the timeout.
 * This is a connectivity issue — the name may or may not exist.
 *
 * User-facing: "ElectrumX servers unreachable"
 * Includes: retry suggestion, server configuration hint.
 */
#define NS_ERROR_NAMECOIN_SERVERS_UNREACHABLE \
  NS_ERROR_GENERATE_FAILURE(NS_ERROR_MODULE_NAMECOIN, 3)

/**
 * NS_ERROR_NAMECOIN_NO_ADDRESS
 *
 * The name was found and is not expired, but its value JSON contains no
 * usable IP address (no "ip", "ip6", "translate", or "ns" fields).
 * Common for names that only hold metadata, TOR addresses, or are
 * placeholders.
 *
 * User-facing: "Name exists but has no IP address"
 */
#define NS_ERROR_NAMECOIN_NO_ADDRESS \
  NS_ERROR_GENERATE_FAILURE(NS_ERROR_MODULE_NAMECOIN, 4)

#endif  // nsNamecoinErrors_h__
