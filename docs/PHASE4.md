# Phase 4: Android / GeckoView Integration

## Overview

Phase 4 adapts the Namecoin (.bit) domain resolver for the Android platform.
Since Firefox Android (Fenix) uses GeckoView — which embeds the same Gecko
engine as desktop Firefox — the core C++ implementation from Phases 1-3 works
automatically on Android. Phase 4 adds the Android-specific layers: battery-
aware connection management, GeckoView runtime settings API, and Fenix UI.

## What Works Automatically (Section 7.1)

The following Phase 1-3 components require **zero Android-specific work**:

| Component | Why It Works |
|-----------|-------------|
| DNS resolution (`nsNamecoinResolver`) | Same C++ code compiled into GeckoView's libxul.so |
| TLS/DANE validation | Same NSS integration |
| eTLD handling (`.bit` in effective_tld_names.dat) | Same eTLD data file |
| Certificate caching | Same cert cache implementation |
| `about:namecoin` diagnostic page | Same chrome:// content, renders in mobile GeckoView |
| ElectrumX WebSocket protocol | Gecko's WebSocket implementation works on Android |
| Name expiry checks | Same block-height comparison logic |
| Multi-server cross-validation | Same consensus algorithm |

## Android-Specific Work (This Phase)

### 1. Battery-Aware Connection Management

**Files:** `src/android/NmcConnectionManager.cpp` + `.h`

The desktop resolver keeps WebSocket connections alive for the full cache TTL
(up to 3600 seconds). On mobile this drains battery. `NmcConnectionManager`
wraps the connection pool with Android lifecycle awareness:

- **App backgrounded:** Starts a 5-minute idle timer. If no `.bit` lookup
  occurs before it fires, all WebSocket connections are closed.
- **App foregrounded:** Cancels the idle timer. Connections are created lazily
  on the next `.bit` resolve (no eager reconnect).
- **Network change (Wi-Fi ↔ LTE):** Drops all connections immediately. The
  pool reconnects on the new interface when the next resolve occurs.
- **Battery-aware pool mode:** When `network.namecoin.battery_aware` is true
  (default on Android), the pool's idle TTL is reduced to 30 seconds.

Listens for Gecko observer notifications:
- `geckoview:activity-state-changed` — data: `"foreground"` / `"background"`
- `network:offline-status-changed` — data: `"online"` / `"offline"`
- `network:link-status-changed` — data: `"changed"`

### 2. GeckoView Runtime Settings API

**Files:** `src/android/NmcGeckoViewSettings.cpp` + `.h`, `src/android/NamecoinSettings.kt`

XPCOM component bridging GeckoView's `GeckoRuntimeSettings` to the underlying
`network.namecoin.*` preferences. This allows any GeckoView embedding app
(not just Fenix) to configure Namecoin resolution programmatically:

```kotlin
val settings = NamecoinSettings.fromRuntime(geckoRuntime)
settings.isEnabled = true
settings.electrumXServers = listOf("wss://my-server.example.com:50004")
settings.requireHttps = true
```

### 3. Fenix Settings UI

**File:** `src/android/FenixNamecoinSettings.kt`

`NamecoinSettingsFragment` integrates into Fenix's Settings → Privacy &
Security panel:

- **Enable Namecoin (.bit) domains** — toggle switch
- **ElectrumX servers** — editable text (newline-separated URLs)
- **Require HTTPS when available** — toggle switch
- **Diagnostics** — opens `about:namecoin` in a new tab

### 4. Patches

| Patch | Purpose |
|-------|---------|
| `0013-geckoview-namecoin-api.patch` | GeckoRuntimeSettings Java API + JNI bridge |
| `0014-android-connection-lifecycle.patch` | Wire NmcConnectionManager into resolver init/shutdown |
| `0015-android-websocket-battery.patch` | Battery-aware pool mode with `#ifdef ANDROID` |

### 5. Build System

**File:** `src/android/moz.build`

Compiles the Android sources into `libxul.so` via `FINAL_LIBRARY = 'xul'`.
Conditionally included from the parent `netwerk/dns/moz.build` on Android.

## Preferences

| Preference | Type | Default (Android) | Description |
|-----------|------|-------------------|-------------|
| `network.namecoin.enabled` | bool | `false` | Master enable switch |
| `network.namecoin.electrumx_servers` | string | community servers | Comma-separated `wss://` URLs |
| `network.namecoin.battery_aware` | bool | `true` | Battery-efficient connection pool |
| `network.namecoin.require_https` | bool | `true` | Enforce HTTPS for domains with TLS records |
| `network.namecoin.cache_ttl_seconds` | int | `3600` | DNS cache TTL |
| `network.namecoin.connection_timeout_ms` | int | `10000` | WebSocket connect timeout |

## Testing

### GeckoView Test App

```bash
# Build GeckoView with Namecoin patches applied
./mach build
./mach geckoview:test

# Launch the test app and enable Namecoin
adb shell am start -n org.mozilla.geckoview_example/.GeckoViewActivity
```

In the test app's URL bar, navigate to `http://testls.bit` — should resolve
via ElectrumX and display the page.

### Automated Tests

```bash
# Run the Namecoin-specific Android instrumentation tests
./mach test mobile/android/geckoview/src/androidTest/java/.../NamecoinTest.java

# Run xpcshell tests (same as desktop — validates core resolver)
./mach xpcshell-test netwerk/test/unit/test_namecoin_resolver.js
```

### Manual Testing Checklist

1. **Basic resolution:** Navigate to `testls.bit` — page loads
2. **Background lifecycle:** Background the app, wait >5 min, foreground,
   navigate to a `.bit` domain — should reconnect and resolve
3. **Network switch:** Resolve a `.bit` domain on Wi-Fi, switch to mobile
   data, resolve again — should work after brief reconnect
4. **Settings:** Toggle Namecoin off in Fenix Settings → verify `.bit`
   domains produce an error page; toggle back on → verify resolution works
5. **about:namecoin:** Open diagnostic page on mobile — should show server
   connectivity status, test resolution, and display cache info
6. **Battery:** Use Android's battery stats to confirm no persistent
   wakelock or excessive network usage from WebSocket connections

### Real Device Testing

Test on the following network conditions:

- **Wi-Fi (home/office):** Standard connectivity, all ports open
- **Mobile data (LTE/5G):** Some carriers block non-standard WebSocket ports.
  Verify `wss://` on port 443 works. Port 50004 may be blocked.
- **Restrictive networks:** Corporate Wi-Fi, captive portals. Namecoin
  resolution should fail gracefully with a clear error page.

## Known Issues

### Port Blocking on Mobile Networks

Some mobile carriers block WebSocket connections on non-standard ports
(e.g., port 50003/50004 used by many ElectrumX servers). **Mitigation:**
- Configure `wss://` servers on port 443 (standard HTTPS port)
- The resolver falls back through the server list, so include at least
  one port-443 server
- `about:namecoin` shows connectivity test results per server

### Battery Considerations

Even with battery-aware mode, `.bit` domain resolution involves:
- WebSocket handshake (TCP + TLS + WS upgrade) — ~300ms on LTE
- 2-3 JSON-RPC round trips per resolve — ~200ms each on LTE
- Total: ~1 second per cold resolve

This is acceptable for user-initiated navigation but would be problematic
for background prefetch. The resolver intentionally does NOT prefetch
`.bit` domains and closes connections when the app is backgrounded.

### GeckoView Embedding API Stability

The `NamecoinSettings` API is new and may change. Embedding apps should
pin to a specific GeckoView version and test Namecoin settings on upgrade.

## GeckoView Embedding App API Reference

### NamecoinSettings (Kotlin)

```kotlin
class NamecoinSettings {
    var isEnabled: Boolean          // Enable/disable .bit resolution
    var electrumXServers: List<String>  // ElectrumX server URLs
    var requireHttps: Boolean       // Enforce HTTPS for DANE domains

    companion object {
        fun fromRuntime(runtime: GeckoRuntime): NamecoinSettings
    }
}
```

### GeckoRuntimeSettings.Builder (Java)

```java
new GeckoRuntimeSettings.Builder()
    .namecoinEnabled(true)
    .electrumXServers("wss://server1.example.com:50004",
                      "wss://server2.example.com:443")
    .build();
```

### GeckoRuntimeSettings (Java)

```java
settings.setNamecoinEnabled(true);
boolean enabled = settings.getNamecoinEnabled();

settings.setElectrumXServers(Arrays.asList("wss://server.example.com:443"));
List<String> servers = settings.getElectrumXServers();

settings.setRequireHttps(true);
boolean requireHttps = settings.getRequireHttps();
```
