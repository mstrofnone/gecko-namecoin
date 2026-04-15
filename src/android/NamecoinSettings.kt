/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package org.mozilla.geckoview

/**
 * Namecoin (.bit domain) settings for GeckoView embedding applications.
 *
 * This is the Kotlin-side API that embedding apps (Fenix, Focus, custom
 * browsers) use to configure Namecoin resolution. It bridges to
 * GeckoRuntimeSettings, which in turn writes to the C++ preference layer
 * via NmcGeckoViewSettings.
 *
 * Usage from an embedding app:
 * ```kotlin
 * val runtime = GeckoRuntime.getDefault(context)
 * val settings = runtime.settings.namecoinSettings
 *
 * // Enable Namecoin resolution
 * settings.isEnabled = true
 *
 * // Configure ElectrumX servers
 * settings.electrumXServers = listOf(
 *     "wss://electrumx.example.com:50004",
 *     "wss://backup.example.com:50004"
 * )
 *
 * // Require HTTPS when Namecoin value has a TLS record
 * settings.requireHttps = true
 * ```
 *
 * All setters take effect immediately — they write to the underlying Gecko
 * preferences which nsNamecoinResolver reads on each resolve call.
 */
class NamecoinSettings internal constructor(
    private val runtime: GeckoRuntime
) {
    /**
     * Whether Namecoin (.bit) domain resolution is enabled.
     *
     * When enabled, hostnames ending in ".bit" are resolved via the Namecoin
     * blockchain through ElectrumX servers, bypassing standard DNS.
     *
     * Default: false
     *
     * Corresponds to preference: `network.namecoin.enabled`
     */
    var isEnabled: Boolean
        get() = runtime.settings.getNamecoinEnabled()
        set(value) {
            runtime.settings.setNamecoinEnabled(value)
        }

    /**
     * List of ElectrumX WebSocket server URLs for Namecoin queries.
     *
     * Servers are tried in order; the first responsive server is used.
     * URLs must use `wss://` (recommended) or `ws://` scheme.
     *
     * Example: `listOf("wss://electrumx.example.com:50004")`
     *
     * Default: built-in community servers (see nsNamecoinResolver.cpp)
     *
     * Corresponds to preference: `network.namecoin.electrumx_servers`
     */
    var electrumXServers: List<String>
        get() = runtime.settings.getElectrumXServers()
        set(value) {
            runtime.settings.setElectrumXServers(value)
        }

    /**
     * Whether to require HTTPS when a Namecoin name has a TLSA/TLS record.
     *
     * When true and a .bit domain's blockchain value contains a `tls` field,
     * the browser will enforce HTTPS and perform DANE-TLSA validation.
     * This prevents downgrade attacks on domains that opt into TLS pinning.
     *
     * Default: true
     *
     * Corresponds to preference: `network.namecoin.require_https`
     */
    var requireHttps: Boolean
        get() = runtime.settings.getRequireHttps()
        set(value) {
            runtime.settings.setRequireHttps(value)
        }

    companion object {
        /**
         * Get the NamecoinSettings for a GeckoRuntime.
         *
         * This is a convenience factory; the instance is lightweight and
         * delegates all state to GeckoRuntimeSettings preferences.
         */
        @JvmStatic
        fun fromRuntime(runtime: GeckoRuntime): NamecoinSettings {
            return NamecoinSettings(runtime)
        }
    }
}
