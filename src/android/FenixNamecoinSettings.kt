/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package org.mozilla.fenix.settings.namecoin

import android.os.Bundle
import android.widget.Toast
import androidx.preference.EditTextPreference
import androidx.preference.Preference
import androidx.preference.PreferenceFragmentCompat
import androidx.preference.SwitchPreferenceCompat
import org.mozilla.fenix.R
import org.mozilla.fenix.ext.requireComponents
import org.mozilla.fenix.ext.showToolbar
import org.mozilla.geckoview.NamecoinSettings

/**
 * Fenix Settings → Privacy & Security → Namecoin (.bit Domains)
 *
 * Provides user-facing toggles and configuration for Namecoin domain resolution.
 * This fragment integrates into Fenix's existing preference hierarchy under
 * the Privacy & Security section.
 *
 * Preferences:
 *   - Enable Namecoin (.bit) domains       → network.namecoin.enabled
 *   - ElectrumX servers                    → network.namecoin.electrumx_servers
 *   - Require HTTPS when available         → network.namecoin.require_https
 *   - Diagnostics (about:namecoin)         → opens in a new tab
 *
 * All preferences are persisted through GeckoRuntimeSettings → Gecko prefs.
 * Changes take effect immediately without requiring a browser restart.
 */
class NamecoinSettingsFragment : PreferenceFragmentCompat() {

    private lateinit var namecoinSettings: NamecoinSettings

    override fun onCreatePreferences(savedInstanceState: Bundle?, rootKey: String?) {
        setPreferencesFromResource(R.xml.namecoin_preferences, rootKey)

        val runtime = requireComponents.core.engine.geckoRuntime
        namecoinSettings = NamecoinSettings.fromRuntime(runtime)

        setupEnabledToggle()
        setupServerConfiguration()
        setupRequireHttpsToggle()
        setupDiagnosticsLink()
    }

    override fun onResume() {
        super.onResume()
        showToolbar(getString(R.string.preferences_namecoin_title))
        refreshPreferenceStates()
    }

    // ---- Enable/Disable toggle ----

    private fun setupEnabledToggle() {
        val pref = findPreference<SwitchPreferenceCompat>(
            PREF_KEY_NAMECOIN_ENABLED
        ) ?: return

        pref.isChecked = namecoinSettings.isEnabled

        pref.setOnPreferenceChangeListener { _, newValue ->
            val enabled = newValue as Boolean
            namecoinSettings.isEnabled = enabled
            refreshDependentPreferences(enabled)
            true
        }
    }

    // ---- Server configuration ----

    private fun setupServerConfiguration() {
        val pref = findPreference<EditTextPreference>(
            PREF_KEY_ELECTRUMX_SERVERS
        ) ?: return

        // Display current servers as summary
        val servers = namecoinSettings.electrumXServers
        pref.summary = if (servers.isNotEmpty()) {
            servers.joinToString("\n")
        } else {
            getString(R.string.preferences_namecoin_servers_default)
        }

        // Text is stored as newline-separated URLs for easier editing
        pref.text = servers.joinToString("\n")

        pref.setOnPreferenceChangeListener { _, newValue ->
            val text = newValue as String
            val serverList = text.lines()
                .map { it.trim() }
                .filter { it.isNotEmpty() }
                .filter { it.startsWith("ws://") || it.startsWith("wss://") }

            if (serverList.isEmpty() && text.isNotBlank()) {
                Toast.makeText(
                    requireContext(),
                    R.string.preferences_namecoin_servers_invalid,
                    Toast.LENGTH_SHORT
                ).show()
                return@setOnPreferenceChangeListener false
            }

            namecoinSettings.electrumXServers = serverList
            pref.summary = if (serverList.isNotEmpty()) {
                serverList.joinToString("\n")
            } else {
                getString(R.string.preferences_namecoin_servers_default)
            }
            true
        }
    }

    // ---- Require HTTPS toggle ----

    private fun setupRequireHttpsToggle() {
        val pref = findPreference<SwitchPreferenceCompat>(
            PREF_KEY_REQUIRE_HTTPS
        ) ?: return

        pref.isChecked = namecoinSettings.requireHttps

        pref.setOnPreferenceChangeListener { _, newValue ->
            namecoinSettings.requireHttps = newValue as Boolean
            true
        }
    }

    // ---- Diagnostics link ----

    private fun setupDiagnosticsLink() {
        val pref = findPreference<Preference>(
            PREF_KEY_DIAGNOSTICS
        ) ?: return

        pref.setOnPreferenceClickListener {
            // Open about:namecoin in a new tab
            requireComponents.useCases.tabsUseCases.addTab(
                url = ABOUT_NAMECOIN_URL,
                selectTab = true,
                private = false
            )
            // Navigate back to the browser
            requireActivity().onBackPressedDispatcher.onBackPressed()
            true
        }
    }

    // ---- Helpers ----

    private fun refreshPreferenceStates() {
        val enabled = namecoinSettings.isEnabled
        findPreference<SwitchPreferenceCompat>(PREF_KEY_NAMECOIN_ENABLED)
            ?.isChecked = enabled
        findPreference<SwitchPreferenceCompat>(PREF_KEY_REQUIRE_HTTPS)
            ?.isChecked = namecoinSettings.requireHttps

        refreshDependentPreferences(enabled)
    }

    /**
     * Enable/disable preferences that depend on Namecoin being enabled.
     */
    private fun refreshDependentPreferences(enabled: Boolean) {
        findPreference<EditTextPreference>(PREF_KEY_ELECTRUMX_SERVERS)
            ?.isEnabled = enabled
        findPreference<SwitchPreferenceCompat>(PREF_KEY_REQUIRE_HTTPS)
            ?.isEnabled = enabled
        findPreference<Preference>(PREF_KEY_DIAGNOSTICS)
            ?.isEnabled = enabled
    }

    companion object {
        // Preference keys — must match res/xml/namecoin_preferences.xml
        private const val PREF_KEY_NAMECOIN_ENABLED = "pref_key_namecoin_enabled"
        private const val PREF_KEY_ELECTRUMX_SERVERS = "pref_key_electrumx_servers"
        private const val PREF_KEY_REQUIRE_HTTPS = "pref_key_namecoin_require_https"
        private const val PREF_KEY_DIAGNOSTICS = "pref_key_namecoin_diagnostics"

        private const val ABOUT_NAMECOIN_URL = "about:namecoin"
    }
}

/*
 * ============================================================================
 * Required Fenix resource files (not created here — for reference):
 * ============================================================================
 *
 * 1. res/xml/namecoin_preferences.xml:
 *
 *    <?xml version="1.0" encoding="utf-8"?>
 *    <PreferenceScreen xmlns:android="http://schemas.android.com/apk/res/android"
 *        xmlns:app="http://schemas.android.com/apk/res-auto">
 *
 *        <SwitchPreferenceCompat
 *            android:key="pref_key_namecoin_enabled"
 *            android:title="@string/preferences_namecoin_enable_title"
 *            android:summary="@string/preferences_namecoin_enable_summary"
 *            android:defaultValue="false" />
 *
 *        <EditTextPreference
 *            android:key="pref_key_electrumx_servers"
 *            android:title="@string/preferences_namecoin_servers_title"
 *            android:summary="@string/preferences_namecoin_servers_default"
 *            android:dependency="pref_key_namecoin_enabled" />
 *
 *        <SwitchPreferenceCompat
 *            android:key="pref_key_namecoin_require_https"
 *            android:title="@string/preferences_namecoin_require_https_title"
 *            android:summary="@string/preferences_namecoin_require_https_summary"
 *            android:defaultValue="true"
 *            android:dependency="pref_key_namecoin_enabled" />
 *
 *        <Preference
 *            android:key="pref_key_namecoin_diagnostics"
 *            android:title="@string/preferences_namecoin_diagnostics_title"
 *            android:summary="@string/preferences_namecoin_diagnostics_summary"
 *            android:dependency="pref_key_namecoin_enabled" />
 *
 *    </PreferenceScreen>
 *
 * 2. res/values/strings.xml additions:
 *
 *    <string name="preferences_namecoin_title">Namecoin (.bit Domains)</string>
 *    <string name="preferences_namecoin_enable_title">Enable Namecoin (.bit) domains</string>
 *    <string name="preferences_namecoin_enable_summary">Resolve .bit domains via the Namecoin blockchain</string>
 *    <string name="preferences_namecoin_servers_title">ElectrumX servers</string>
 *    <string name="preferences_namecoin_servers_default">Using default servers</string>
 *    <string name="preferences_namecoin_servers_invalid">Enter valid ws:// or wss:// URLs, one per line</string>
 *    <string name="preferences_namecoin_require_https_title">Require HTTPS when available</string>
 *    <string name="preferences_namecoin_require_https_summary">Enforce HTTPS for .bit domains with TLS records</string>
 *    <string name="preferences_namecoin_diagnostics_title">Namecoin diagnostics</string>
 *    <string name="preferences_namecoin_diagnostics_summary">Open about:namecoin to test connectivity and resolution</string>
 *
 * 3. Navigation graph addition (nav_graph.xml):
 *
 *    <fragment
 *        android:id="@+id/namecoinSettingsFragment"
 *        android:name="org.mozilla.fenix.settings.namecoin.NamecoinSettingsFragment"
 *        android:label="@string/preferences_namecoin_title" />
 *
 * 4. Privacy & Security settings fragment — add preference linking to above:
 *
 *    <Preference
 *        android:key="pref_key_namecoin_settings"
 *        android:title="@string/preferences_namecoin_title"
 *        app:fragment="org.mozilla.fenix.settings.namecoin.NamecoinSettingsFragment" />
 */
