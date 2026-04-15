/* -*- Mode: JavaScript; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * NamecoinParent.sys.mjs — Chrome-process JSWindowActor for about:namecoin.
 *
 * Receives IPC messages from NamecoinChild (content process) and fulfils
 * them by calling into the nsINamecoinResolver XPCOM service. This is the
 * standard Firefox pattern for privileged data access from about: pages
 * (cf. AboutLoginsParent.sys.mjs, NetErrorParent.sys.mjs).
 */

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  Services: "resource://gre/modules/Services.sys.mjs",
});

/**
 * Lazily obtain the nsINamecoinResolver singleton.
 * Returns null if the component is not registered (e.g. build without
 * Namecoin support) or if resolution is disabled via prefs.
 */
function getResolver() {
  try {
    return Cc["@mozilla.org/network/namecoin-resolver;1"].getService(
      Ci.nsINamecoinResolver
    );
  } catch (e) {
    console.error("NamecoinParent: resolver service unavailable:", e);
    return null;
  }
}

/**
 * Read Namecoin-related preferences and return them as a plain object.
 */
function getPrefs() {
  const prefs = lazy.Services.prefs;
  return {
    enabled: prefs.getBoolPref("network.namecoin.enabled", false),
    electrumxServers: prefs.getStringPref(
      "network.namecoin.electrumx_servers",
      ""
    ),
    cacheTtlSeconds: prefs.getIntPref(
      "network.namecoin.cache_ttl_seconds",
      3600
    ),
    maxAliasHops: prefs.getIntPref("network.namecoin.max_alias_hops", 5),
    connectionTimeoutMs: prefs.getIntPref(
      "network.namecoin.connection_timeout_ms",
      10000
    ),
    requireTls: prefs.getBoolPref("network.namecoin.require_tls", true),
  };
}

/**
 * Enumerate an nsIArray of nsIPropertyBag-like XPCOM objects into
 * plain JS objects suitable for structured-clone transfer over IPC.
 *
 * If the XPCOM interface returns a native JS array (common for
 * script-implemented components), we pass it through directly.
 */
function xpcomArrayToJS(arrayOrList) {
  if (!arrayOrList) {
    return [];
  }

  // Already a plain JS array (script-based resolver implementation).
  if (Array.isArray(arrayOrList)) {
    return arrayOrList;
  }

  // nsIArray / nsISimpleEnumerator path.
  const results = [];
  try {
    const count = arrayOrList.length;
    for (let i = 0; i < count; i++) {
      const item = arrayOrList.queryElementAt(i, Ci.nsIPropertyBag2);
      results.push(propertyBagToJS(item));
    }
  } catch (e) {
    console.error("NamecoinParent: failed to enumerate XPCOM array:", e);
  }
  return results;
}

/**
 * Convert an nsIPropertyBag2 into a plain object.
 */
function propertyBagToJS(bag) {
  const obj = {};
  try {
    const enumerator = bag.enumerator;
    while (enumerator.hasMoreElements()) {
      const prop = enumerator.getNext().QueryInterface(Ci.nsIProperty);
      obj[prop.name] = prop.value;
    }
  } catch (e) {
    // If the bag doesn't support enumeration, it may already be plain JS.
    return bag;
  }
  return obj;
}

export class NamecoinParent extends JSWindowActorParent {
  async receiveMessage(msg) {
    switch (msg.name) {
      case "Namecoin:GetPrefs":
        return getPrefs();

      case "Namecoin:GetServerStatus":
        return this.#getServerStatus();

      case "Namecoin:GetRecentResolutions":
        return this.#getRecentResolutions(msg.data?.count ?? 20);

      case "Namecoin:GetDANECache":
        return this.#getDANECache();

      case "Namecoin:ResolveName":
        return this.#resolveName(msg.data?.name);

      default:
        console.warn(`NamecoinParent: unknown message "${msg.name}"`);
        return undefined;
    }
  }

  #getServerStatus() {
    const resolver = getResolver();
    if (!resolver) {
      return { error: "resolver_unavailable" };
    }

    try {
      const servers = resolver.getServerStatus();
      return { servers: xpcomArrayToJS(servers) };
    } catch (e) {
      console.error("NamecoinParent: getServerStatus failed:", e);
      return { error: e.message || String(e) };
    }
  }

  #getRecentResolutions(count) {
    const resolver = getResolver();
    if (!resolver) {
      return { error: "resolver_unavailable" };
    }

    try {
      const log = resolver.getRecentResolutions(count);
      return { resolutions: xpcomArrayToJS(log) };
    } catch (e) {
      console.error("NamecoinParent: getRecentResolutions failed:", e);
      return { error: e.message || String(e) };
    }
  }

  #getDANECache() {
    const resolver = getResolver();
    if (!resolver) {
      return { error: "resolver_unavailable" };
    }

    try {
      const entries = resolver.getDANECache();
      return { entries: xpcomArrayToJS(entries) };
    } catch (e) {
      console.error("NamecoinParent: getDANECache failed:", e);
      return { error: e.message || String(e) };
    }
  }

  async #resolveName(name) {
    if (!name) {
      return { error: "no_name_provided" };
    }

    const resolver = getResolver();
    if (!resolver) {
      return { error: "resolver_unavailable" };
    }

    try {
      const result = await resolver.resolveName(name);
      // result should be a structured-clone-safe object.  If it's an
      // XPCOM wrapper we convert it here.
      if (result && typeof result === "object" && !Array.isArray(result)) {
        return { result: result.wrappedJSObject ?? result };
      }
      return { result };
    } catch (e) {
      console.error("NamecoinParent: resolveName failed:", e);
      return {
        error: e.result ?? e.message ?? String(e),
        errorMessage: e.message ?? String(e),
      };
    }
  }
}
