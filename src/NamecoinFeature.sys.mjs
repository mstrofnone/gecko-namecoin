/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * NamecoinFeature — three-stage rollout gate for Namecoin (.bit) resolution.
 *
 * Stage A (Nightly):  about:config only, no Settings UI.
 * Stage B (Beta):     Settings toggle visible in Privacy & Security, default-off.
 * Stage C (Release):  Toggle present, default-off; default-on only by explicit decision.
 *
 * Prefs:
 *   network.namecoin.enabled           — the actual feature toggle
 *   network.namecoin.settings_visible  — whether the Settings panel shows the toggle
 *
 * Consumers:
 *   - nsNamecoinResolver (C++) reads network.namecoin.enabled via StaticPrefs.
 *   - browser/components/preferences/privacy.js uses this module to gate the UI.
 *   - about:namecoin reads this module for diagnostics display.
 *
 * Pattern follows browser/components/urlbar/UrlbarPrefs.sys.mjs and
 * toolkit/components/search/SearchUtils.sys.mjs.
 */

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  AppConstants: "resource://gre/modules/AppConstants.sys.mjs",
});

const PREF_ENABLED = "network.namecoin.enabled";
const PREF_SETTINGS_VISIBLE = "network.namecoin.settings_visible";

export const NamecoinFeature = {
  /**
   * Stage A — Whether Namecoin resolution is enabled.
   * In Nightly, this can only be true if the user manually flipped it in
   * about:config. In Beta+, the Settings toggle also controls this.
   *
   * @returns {boolean}
   */
  isNightlyEnabled() {
    return Services.prefs.getBoolPref(PREF_ENABLED, false);
  },

  /**
   * Stage B — Whether the Namecoin Settings toggle is visible in the
   * Privacy & Security panel. Controlled by Normandy rollout or by
   * directly setting network.namecoin.settings_visible.
   *
   * @returns {boolean}
   */
  isSettingsVisible() {
    return Services.prefs.getBoolPref(PREF_SETTINGS_VISIBLE, false);
  },

  /**
   * Stage C — Whether Namecoin is enabled on a release channel.
   * Identical check to isNightlyEnabled; separated for semantic clarity
   * and to allow adding release-specific guards in the future (e.g.
   * enterprise policy overrides).
   *
   * @returns {boolean}
   */
  isReleaseEnabled() {
    return Services.prefs.getBoolPref(PREF_ENABLED, false);
  },

  /**
   * Determine the current rollout stage based on channel and pref state.
   *
   *   "nightly" — Stage A: about:config only
   *   "beta"    — Stage B: Settings toggle visible
   *   "release" — Stage C: toggle present on Release channel
   *
   * @returns {"nightly"|"beta"|"release"}
   */
  getStage() {
    if (this._isReleaseChannel()) {
      return "release";
    }
    if (this._isBetaChannel()) {
      return "beta";
    }
    // Default to nightly for nightly, aurora, and local builds.
    return "nightly";
  },

  /**
   * Whether the current build is Nightly (or a local developer build).
   *
   * @returns {boolean}
   */
  isNightlyBuild() {
    return this._isNightlyChannel();
  },

  /**
   * Whether the current build is Beta or newer (i.e. Beta or Release).
   *
   * @returns {boolean}
   */
  isBetaOrNewer() {
    return this._isBetaChannel() || this._isReleaseChannel();
  },

  /**
   * Whether the Namecoin feature is effectively active — the single
   * entry-point that resolver and UI code should use.
   *
   * The feature is active when:
   *   - network.namecoin.enabled is true, AND
   *   - On Nightly: always (about:config is sufficient)
   *   - On Beta/Release: network.namecoin.settings_visible must also be true
   *     (prevents premature activation if the pref leaks via sync/profile copy)
   *
   * @returns {boolean}
   */
  isActive() {
    if (!this.isNightlyEnabled()) {
      return false;
    }
    // On Nightly, about:config alone is enough.
    if (this._isNightlyChannel()) {
      return true;
    }
    // On Beta/Release, the settings toggle must have been enabled by
    // Normandy or admin policy for the feature to be truly active.
    return this.isSettingsVisible();
  },

  // ---------------------------------------------------------------------------
  // Internal helpers
  // ---------------------------------------------------------------------------

  /** @returns {boolean} */
  _isNightlyChannel() {
    const channel = lazy.AppConstants.MOZ_UPDATE_CHANNEL;
    return channel === "nightly" || channel === "default";
  },

  /** @returns {boolean} */
  _isBetaChannel() {
    return lazy.AppConstants.MOZ_UPDATE_CHANNEL === "beta";
  },

  /** @returns {boolean} */
  _isReleaseChannel() {
    return lazy.AppConstants.MOZ_UPDATE_CHANNEL === "release";
  },
};
