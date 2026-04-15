/* -*- Mode: JavaScript; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * NamecoinChild.sys.mjs — Content-process JSWindowActor for about:namecoin.
 *
 * Exposes a message-passing interface that aboutNamecoin.js uses to request
 * privileged data from NamecoinParent (chrome process). Each method sends an
 * async message and returns the structured-clone response.
 *
 * aboutNamecoin.js obtains this actor via:
 *   const actor = window.windowGlobalChild.getActor("Namecoin");
 *
 * This is the standard Firefox JSWindowActor child pattern
 * (cf. AboutLoginsChild.sys.mjs, NetErrorChild.sys.mjs).
 */

export class NamecoinChild extends JSWindowActorChild {
  /**
   * Fetch current Namecoin-related preferences.
   * @returns {Promise<Object>} prefs object
   */
  getPrefs() {
    return this.sendQuery("Namecoin:GetPrefs");
  }

  /**
   * Fetch ElectrumX server connection status.
   * @returns {Promise<{servers?: Array, error?: string}>}
   */
  getServerStatus() {
    return this.sendQuery("Namecoin:GetServerStatus");
  }

  /**
   * Fetch the most recent name resolution log entries.
   * @param {number} count - maximum entries to return
   * @returns {Promise<{resolutions?: Array, error?: string}>}
   */
  getRecentResolutions(count = 20) {
    return this.sendQuery("Namecoin:GetRecentResolutions", { count });
  }

  /**
   * Fetch the DANE/TLSA validation cache contents.
   * @returns {Promise<{entries?: Array, error?: string}>}
   */
  getDANECache() {
    return this.sendQuery("Namecoin:GetDANECache");
  }

  /**
   * Resolve a .bit name via the Namecoin blockchain.
   * @param {string} name - domain name (e.g. "example.bit")
   * @returns {Promise<{result?: Object, error?: string}>}
   */
  resolveName(name) {
    return this.sendQuery("Namecoin:ResolveName", { name });
  }
}
