/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsNamecoinErrors_h__
#define nsNamecoinErrors_h__

/**
 * Namecoin-specific error codes.
 * Module 77 (NS_ERROR_MODULE_NAMECOIN) verified free in gecko-dev as of 2026.
 * These map to ErrorList.py entries.
 */

#include "nsError.h"

// Module 77 base
#define NS_ERROR_MODULE_NAMECOIN 77

#define NS_ERROR_NAMECOIN_SERVERS_UNREACHABLE \
  NS_ERROR_GENERATE_FAILURE(NS_ERROR_MODULE_NAMECOIN, 1)

#define NS_ERROR_NAMECOIN_NOT_FOUND \
  NS_ERROR_GENERATE_FAILURE(NS_ERROR_MODULE_NAMECOIN, 2)

#define NS_ERROR_NAMECOIN_EXPIRED \
  NS_ERROR_GENERATE_FAILURE(NS_ERROR_MODULE_NAMECOIN, 3)

#define NS_ERROR_NAMECOIN_NO_ADDRESS \
  NS_ERROR_GENERATE_FAILURE(NS_ERROR_MODULE_NAMECOIN, 4)

#define NS_ERROR_NAMECOIN_DANE_FAIL \
  NS_ERROR_GENERATE_FAILURE(NS_ERROR_MODULE_NAMECOIN, 5)

#endif  // nsNamecoinErrors_h__
