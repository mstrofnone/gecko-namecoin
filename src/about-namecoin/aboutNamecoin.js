/* -*- Mode: JavaScript; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * aboutNamecoin.js — Backing script for about:namecoin diagnostic page.
 *
 * In the production build this would use ChromeUtils.importESModule and
 * privileged XPCOM APIs to communicate with nsNamecoinResolver. For the
 * prototype we read preferences via Services.prefs and populate panels
 * with realistic mock data so the UI can be evaluated and tested.
 *
 * Integration points marked with "// REAL IMPL:" comments show where
 * actual IPC/XPCOM calls would replace mock data.
 */

"use strict";

// ---------------------------------------------------------------------------
// Gecko privileged imports (available in about: page chrome context)
// ---------------------------------------------------------------------------

// REAL IMPL: These would be the actual imports in a chrome-privileged page.
// const { XPCOMUtils } = ChromeUtils.importESModule(
//   "resource://gre/modules/XPCOMUtils.sys.mjs"
// );
// const { Services } = ChromeUtils.importESModule(
//   "resource://gre/modules/Services.sys.mjs"
// );
// const { nsNamecoinResolver } = ChromeUtils.importESModule(
//   "resource://gre/modules/NamecoinResolver.sys.mjs"
// );

// ---------------------------------------------------------------------------
// Preference helpers (mock for prototype, real Services.prefs in production)
// ---------------------------------------------------------------------------

const Prefs = (() => {
  // REAL IMPL: return Services.prefs for each call, e.g.:
  // getBool(key)  → Services.prefs.getBoolPref(key, defaultVal)
  // getString(key) → Services.prefs.getStringPref(key, defaultVal)

  // Prototype defaults — mimic what StaticPrefList.yaml defines
  const defaults = {
    "network.namecoin.enabled": true,
    "network.namecoin.electrumx_servers":
      "wss://electrumx-nmc.example.org:50004,wss://nmc.obelisk.xyz:50004,ws://127.0.0.1:50003",
    "network.namecoin.cache_ttl_seconds": 3600,
    "network.namecoin.max_alias_hops": 5,
    "network.namecoin.connection_timeout_ms": 10000,
    "network.namecoin.require_tls": true,
  };

  return {
    getBool(key) {
      return !!defaults[key];
    },
    getString(key) {
      return defaults[key] || "";
    },
    getInt(key) {
      return defaults[key] | 0;
    },
  };
})();

// ---------------------------------------------------------------------------
// Mock data generators — realistic structures matching nsNamecoinResolver
// ---------------------------------------------------------------------------

function generateMockServers() {
  const urls = Prefs.getString("network.namecoin.electrumx_servers").split(",");
  const statuses = ["connected", "connected", "disconnected"];
  const now = Date.now();

  return urls.map((url, i) => ({
    url: url.trim(),
    status: statuses[i % statuses.length],
    latencyMs: statuses[i % statuses.length] === "connected"
      ? Math.floor(45 + Math.random() * 120)
      : null,
    blockHeight: statuses[i % statuses.length] === "connected"
      ? 824150 + Math.floor(Math.random() * 10)
      : null,
    lastSeen: statuses[i % statuses.length] === "connected"
      ? new Date(now - Math.floor(Math.random() * 30000)).toISOString()
      : null,
  }));
}

function generateMockResolutions() {
  const domains = [
    { domain: "testls.bit", ip: "185.45.12.88", time: 82, cache: false },
    { domain: "wiki.bit", ip: "94.23.204.17", time: 4, cache: true },
    { domain: "nf.bit", ip: "2a01:4f8:c2c:51d2::1", time: 156, cache: false },
    { domain: "dns.bit", ip: "107.152.35.22", time: 3, cache: true },
    { domain: "nx.bit", ip: "—", time: 211, cache: false, error: "EXPIRED" },
    { domain: "market.bit", ip: "45.33.32.156", time: 67, cache: false },
    { domain: "forum.bit", ip: "172.67.182.31", time: 5, cache: true },
    { domain: "keys.bit", ip: "—", time: 189, cache: false, error: "NO_ADDRESS" },
    { domain: "bitcoin.bit", ip: "209.141.35.20", time: 93, cache: false },
    { domain: "status.bit", ip: "198.51.100.14", time: 6, cache: true },
    { domain: "tor.bit", ip: "—", time: 145, cache: false, error: "NO_ADDRESS" },
    { domain: "mail.bit", ip: "203.0.113.42", time: 71, cache: false },
    { domain: "chat.bit", ip: "10.0.0.1", time: 2, cache: true },
    { domain: "blog.bit", ip: "192.0.2.88", time: 55, cache: false },
    { domain: "api.bit", ip: "198.51.100.7", time: 4, cache: true },
    { domain: "pay.bit", ip: "45.79.112.203", time: 108, cache: false },
    { domain: "dev.bit", ip: "172.104.210.55", time: 3, cache: true },
    { domain: "radio.bit", ip: "—", time: 302, cache: false, error: "NOT_FOUND" },
    { domain: "news.bit", ip: "151.101.1.140", time: 89, cache: false },
    { domain: "shop.bit", ip: "104.21.57.128", time: 7, cache: true },
  ];

  const now = Date.now();
  return domains.map((d, i) => ({
    ...d,
    timestamp: new Date(now - i * 47000).toISOString(),
  }));
}

function generateMockDANECache() {
  return [
    {
      domain: "testls.bit",
      fingerprint: "a3:f2:7c:89:bb:12:d4:e5:90:1f:c3:22:aa:bc:de:f0:11:22:33:44",
      matchType: "SHA-256 Full Certificate",
      usage: "3 (DANE-EE)",
      expiryBlock: 860150,
    },
    {
      domain: "bitcoin.bit",
      fingerprint: "d1:e4:56:78:9a:bc:de:f0:12:34:56:78:9a:bc:de:f0:fe:dc:ba:98",
      matchType: "SHA-256 SubjectPublicKeyInfo",
      usage: "3 (DANE-EE)",
      expiryBlock: 858900,
    },
    {
      domain: "wiki.bit",
      fingerprint: "b8:c7:1a:2b:3c:4d:5e:6f:70:81:92:a3:b4:c5:d6:e7:f8:09:1a:2b",
      matchType: "SHA-512 Full Certificate",
      usage: "2 (DANE-TA)",
      expiryBlock: 862300,
    },
    {
      domain: "forum.bit",
      fingerprint: "ff:ee:dd:cc:bb:aa:99:88:77:66:55:44:33:22:11:00:ab:cd:ef:01",
      matchType: "SHA-256 SubjectPublicKeyInfo",
      usage: "3 (DANE-EE)",
      expiryBlock: 857200,
    },
  ];
}

// ---------------------------------------------------------------------------
// DOM helpers
// ---------------------------------------------------------------------------

const $ = (sel) => document.getElementById(sel);

function clearTable(tbody) {
  while (tbody.firstChild) {
    tbody.removeChild(tbody.firstChild);
  }
}

function createRow(cells, tag = "td") {
  const tr = document.createElement("tr");
  for (const cell of cells) {
    const td = document.createElement(tag);
    if (typeof cell === "string" || typeof cell === "number") {
      td.textContent = cell;
    } else if (cell instanceof HTMLElement) {
      td.appendChild(cell);
    } else if (cell && cell.html) {
      td.innerHTML = cell.html;
    }
    tr.appendChild(td);
  }
  return tr;
}

function formatTime(isoString) {
  if (!isoString) return "—";
  const d = new Date(isoString);
  return d.toLocaleTimeString(undefined, {
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit",
  });
}

function truncateFingerprint(fp) {
  if (!fp) return "—";
  // Show first 8 and last 8 hex chars
  const parts = fp.split(":");
  if (parts.length <= 8) return fp;
  return parts.slice(0, 4).join(":") + "…" + parts.slice(-4).join(":");
}

// ---------------------------------------------------------------------------
// Panel renderers
// ---------------------------------------------------------------------------

function renderStatusBanner() {
  const enabled = Prefs.getBool("network.namecoin.enabled");
  const banner = $("status-banner");
  const text = $("status-text");

  if (enabled) {
    banner.className = "status-banner enabled";
    text.textContent = "Namecoin (.bit) resolution is enabled";
  } else {
    banner.className = "status-banner disabled";
    text.textContent =
      "Namecoin resolution is disabled — enable via about:config → network.namecoin.enabled";
  }
}

function renderServers() {
  // REAL IMPL: const servers = await nsNamecoinResolver.getServerStatus();
  const servers = generateMockServers();
  const tbody = $("servers-tbody");
  clearTable(tbody);

  if (servers.length === 0) {
    tbody.appendChild(
      createRow([{ html: '<td colspan="5" class="empty-state">No servers configured</td>' }])
    );
    return;
  }

  const connected = servers.filter((s) => s.status === "connected");
  const avgLatency =
    connected.length > 0
      ? Math.round(connected.reduce((a, s) => a + s.latencyMs, 0) / connected.length)
      : 0;
  const maxHeight = Math.max(...connected.map((s) => s.blockHeight || 0), 0);

  // Update stat cards
  $("stat-height").textContent = maxHeight > 0 ? maxHeight.toLocaleString() : "—";
  $("stat-connected").textContent = `${connected.length} / ${servers.length}`;
  $("stat-latency").textContent = avgLatency > 0 ? `${avgLatency} ms` : "—";
  $("stat-cache").textContent = Prefs.getInt("network.namecoin.cache_ttl_seconds");

  // Update badge
  const badge = $("servers-badge");
  badge.textContent = `${connected.length} / ${servers.length} connected`;
  badge.className =
    "badge " +
    (connected.length === servers.length
      ? "badge-ok"
      : connected.length > 0
        ? "badge-warn"
        : "badge-err");

  for (const server of servers) {
    const statusHtml = `<span class="status-dot ${server.status}"></span>` +
      `<span class="status-text ${server.status}">${server.status}</span>`;

    tbody.appendChild(
      createRow([
        { html: `<code>${server.url}</code>` },
        { html: statusHtml },
        server.latencyMs != null ? `${server.latencyMs} ms` : "—",
        server.blockHeight != null ? server.blockHeight.toLocaleString() : "—",
        formatTime(server.lastSeen),
      ])
    );
  }
}

function renderResolutions() {
  // REAL IMPL: const log = await nsNamecoinResolver.getRecentResolutions(20);
  const resolutions = generateMockResolutions();
  const tbody = $("resolutions-tbody");
  clearTable(tbody);

  $("resolutions-badge").textContent = `${resolutions.length} lookups`;

  if (resolutions.length === 0) {
    tbody.appendChild(
      createRow([{ html: '<td colspan="5" class="empty-state">No recent lookups</td>' }])
    );
    return;
  }

  for (const r of resolutions) {
    const resultText = r.error
      ? `⚠ ${r.error}`
      : r.ip;
    const cacheHtml = r.cache
      ? '<span class="cache-tag cache-hit">HIT</span>'
      : '<span class="cache-tag cache-miss">MISS</span>';

    tbody.appendChild(
      createRow([
        { html: `<code>${r.domain}</code>` },
        { html: `<code>${resultText}</code>` },
        `${r.time} ms`,
        { html: cacheHtml },
        formatTime(r.timestamp),
      ])
    );
  }
}

function renderDANECache() {
  // REAL IMPL: const cache = await nsNamecoinResolver.getDANECache();
  const entries = generateMockDANECache();
  const tbody = $("dane-tbody");
  clearTable(tbody);

  $("dane-badge").textContent = `${entries.length} entries`;

  if (entries.length === 0) {
    tbody.appendChild(
      createRow([{ html: '<td colspan="5" class="empty-state">No DANE/TLS entries cached</td>' }])
    );
    return;
  }

  for (const e of entries) {
    tbody.appendChild(
      createRow([
        { html: `<code>${e.domain}</code>` },
        { html: `<span class="truncated mono" title="${e.fingerprint}">${truncateFingerprint(e.fingerprint)}</span>` },
        e.matchType,
        e.usage,
        e.expiryBlock.toLocaleString(),
      ])
    );
  }
}

// ---------------------------------------------------------------------------
// Name Lookup Tool
// ---------------------------------------------------------------------------

async function performLookup(name) {
  // Normalize: strip trailing dots, ensure .bit suffix
  name = name.trim().replace(/\.+$/, "");
  if (!name.endsWith(".bit")) {
    name += ".bit";
  }

  const resultContainer = $("lookup-result");
  const rawBox = $("result-raw");
  const parsedDl = $("result-parsed");

  resultContainer.classList.add("visible");
  rawBox.textContent = "Resolving…";
  rawBox.classList.remove("result-error");
  parsedDl.innerHTML = "";

  // REAL IMPL: Call into nsNamecoinResolver via XPCOM
  // try {
  //   const resolver = Cc["@mozilla.org/network/namecoin-resolver;1"]
  //     .getService(Ci.nsINamecoinResolver);
  //   const result = await resolver.resolveName(name);
  //   displayResult(result);
  // } catch (e) {
  //   displayError(e);
  // }

  // Prototype: simulate network delay and return mock data
  await new Promise((r) => setTimeout(r, 200 + Math.random() * 400));

  if (!Prefs.getBool("network.namecoin.enabled")) {
    rawBox.textContent = "Error: Namecoin resolution is disabled.\nEnable via about:config → network.namecoin.enabled";
    rawBox.classList.add("result-error");
    return;
  }

  // Mock resolution results keyed by name
  const mockResults = {
    "testls.bit": {
      value: {
        ip: "185.45.12.88",
        ip6: "2a01:4f8:c2c:51d2::1",
        tls: {
          tcp: {
            443: [
              [3, 1, 1, "a3f27c89bb12d4e5901fc322aabcdef011223344aabbccdd"]
            ]
          }
        },
        map: { www: { ip: "185.45.12.88" } },
      },
      blockHeight: 821455,
      txHash: "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592",
      expiresAtBlock: 857455,
      nameOp: "NAME_UPDATE",
    },
    "bitcoin.bit": {
      value: {
        ip: "209.141.35.20",
        translate: "bitcoin.org",
      },
      blockHeight: 340210,
      txHash: "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
      expiresAtBlock: 876210,
      nameOp: "NAME_UPDATE",
    },
  };

  const strippedName = name.replace(/\.bit$/, "");
  const result = mockResults[name];

  if (!result) {
    // Simulate NOT_FOUND for unknown names
    rawBox.textContent = `Error: NS_ERROR_NAMECOIN_NOT_FOUND\n\nThe name "d/${strippedName}" has no registration history on the Namecoin blockchain.`;
    rawBox.classList.add("result-error");
    return;
  }

  // Display raw JSON
  rawBox.textContent = JSON.stringify(result.value, null, 2);

  // Display parsed result
  const fields = [
    ["Domain", name],
    ["Namecoin Name", `d/${strippedName}`],
    ["IPv4", result.value.ip || "—"],
    ["IPv6", result.value.ip6 || "—"],
    ["Translate", result.value.translate || "—"],
    ["Name Op", result.nameOp],
    ["Block Height", result.blockHeight.toLocaleString()],
    ["TX Hash", result.txHash],
    ["Expires At Block", result.expiresAtBlock.toLocaleString()],
    ["Has TLSA", result.value.tls ? "Yes" : "No"],
  ];

  for (const [label, value] of fields) {
    if (value === "—") continue;
    const dt = document.createElement("dt");
    dt.textContent = label;
    const dd = document.createElement("dd");
    dd.textContent = value;
    parsedDl.appendChild(dt);
    parsedDl.appendChild(dd);
  }
}

// ---------------------------------------------------------------------------
// Event wiring
// ---------------------------------------------------------------------------

document.addEventListener("DOMContentLoaded", () => {
  renderStatusBanner();
  renderServers();
  renderResolutions();
  renderDANECache();

  const lookupBtn = $("lookup-btn");
  const lookupInput = $("lookup-input");

  lookupBtn.addEventListener("click", () => {
    const name = lookupInput.value.trim();
    if (name) {
      lookupBtn.disabled = true;
      performLookup(name).finally(() => {
        lookupBtn.disabled = false;
      });
    }
  });

  lookupInput.addEventListener("keydown", (e) => {
    if (e.key === "Enter") {
      lookupBtn.click();
    }
  });

  // Auto-refresh server status every 30 seconds
  // REAL IMPL: Use a timer that stops when the page is hidden
  // setInterval(renderServers, 30000);
});
