/* -*- Mode: JavaScript; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * aboutNamecoin.js — Backing script for about:namecoin diagnostic page.
 *
 * Communicates with the nsINamecoinResolver XPCOM service through
 * the NamecoinChild/NamecoinParent JSWindowActor pair. The child actor
 * (content process) forwards requests over IPC to the parent actor
 * (chrome process), which has direct access to the resolver service.
 *
 * This follows the same pattern as about:logins, about:certificate, and
 * other privileged about: pages in Firefox.
 */

"use strict";

// ---------------------------------------------------------------------------
// JSWindowActor access
// ---------------------------------------------------------------------------

/**
 * Obtain the NamecoinChild actor for this window. All privileged operations
 * go through this actor → NamecoinParent → nsINamecoinResolver.
 *
 * @returns {NamecoinChild|null}
 */
function getActor() {
  try {
    return window.windowGlobalChild.getActor("Namecoin");
  } catch (e) {
    console.error("about:namecoin: failed to get NamecoinChild actor:", e);
    return null;
  }
}

// ---------------------------------------------------------------------------
// DOM helpers
// ---------------------------------------------------------------------------

const $ = sel => document.getElementById(sel);

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
  if (!isoString) {
    return "\u2014";
  }
  const d = new Date(isoString);
  return d.toLocaleTimeString(undefined, {
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit",
  });
}

function truncateFingerprint(fp) {
  if (!fp) {
    return "\u2014";
  }
  const parts = fp.split(":");
  if (parts.length <= 8) {
    return fp;
  }
  return parts.slice(0, 4).join(":") + "\u2026" + parts.slice(-4).join(":");
}

/**
 * Show an "unavailable" placeholder in a table body and badge.
 */
function showUnavailable(tbodyId, badgeId, colspan, message) {
  const tbody = $(tbodyId);
  clearTable(tbody);
  const tr = document.createElement("tr");
  const td = document.createElement("td");
  td.setAttribute("colspan", colspan);
  td.className = "empty-state";
  td.textContent = message || "Resolver service unavailable";
  tr.appendChild(td);
  tbody.appendChild(tr);

  if (badgeId) {
    const badge = $(badgeId);
    badge.textContent = "unavailable";
    badge.className = "badge badge-err";
  }
}

// ---------------------------------------------------------------------------
// Panel renderers
// ---------------------------------------------------------------------------

async function renderStatusBanner() {
  const actor = getActor();
  let enabled = false;

  if (actor) {
    try {
      const prefs = await actor.getPrefs();
      enabled = prefs.enabled;
    } catch (e) {
      console.error("about:namecoin: failed to read prefs:", e);
    }
  }

  const banner = $("status-banner");
  const text = $("status-text");

  if (enabled) {
    banner.className = "status-banner enabled";
    text.textContent = "Namecoin (.bit) resolution is enabled";
  } else {
    banner.className = "status-banner disabled";
    text.textContent =
      "Namecoin resolution is disabled \u2014 enable via " +
      "about:config \u2192 network.namecoin.enabled";
  }
}

async function renderServers() {
  const actor = getActor();
  if (!actor) {
    showUnavailable("servers-tbody", "servers-badge", 5);
    return;
  }

  let response;
  try {
    response = await actor.getServerStatus();
  } catch (e) {
    console.error("about:namecoin: getServerStatus IPC failed:", e);
    showUnavailable("servers-tbody", "servers-badge", 5, String(e));
    return;
  }

  if (response.error) {
    showUnavailable(
      "servers-tbody",
      "servers-badge",
      5,
      `Error: ${response.error}`
    );
    return;
  }

  const servers = response.servers || [];
  const tbody = $("servers-tbody");
  clearTable(tbody);

  if (servers.length === 0) {
    tbody.appendChild(
      createRow([
        { html: '<td colspan="5" class="empty-state">No servers configured</td>' },
      ])
    );
    return;
  }

  const connected = servers.filter(s => s.status === "connected");
  const avgLatency =
    connected.length > 0
      ? Math.round(
          connected.reduce((a, s) => a + (s.latencyMs || 0), 0) /
            connected.length
        )
      : 0;
  const maxHeight = Math.max(
    ...connected.map(s => s.blockHeight || 0),
    0
  );

  // Read cache TTL from prefs via actor.
  let cacheTtl = "\u2014";
  try {
    const prefs = await actor.getPrefs();
    cacheTtl = prefs.cacheTtlSeconds;
  } catch (_) {
    // Ignore — non-critical.
  }

  // Update stat cards.
  $("stat-height").textContent =
    maxHeight > 0 ? maxHeight.toLocaleString() : "\u2014";
  $("stat-connected").textContent = `${connected.length} / ${servers.length}`;
  $("stat-latency").textContent =
    avgLatency > 0 ? `${avgLatency} ms` : "\u2014";
  $("stat-cache").textContent = cacheTtl;

  // Update badge.
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
    const statusHtml =
      `<span class="status-dot ${server.status}"></span>` +
      `<span class="status-text ${server.status}">${server.status}</span>`;

    tbody.appendChild(
      createRow([
        { html: `<code>${server.url}</code>` },
        { html: statusHtml },
        server.latencyMs != null ? `${server.latencyMs} ms` : "\u2014",
        server.blockHeight != null
          ? server.blockHeight.toLocaleString()
          : "\u2014",
        formatTime(server.lastSeen),
      ])
    );
  }
}

async function renderResolutions() {
  const actor = getActor();
  if (!actor) {
    showUnavailable("resolutions-tbody", "resolutions-badge", 5);
    return;
  }

  let response;
  try {
    response = await actor.getRecentResolutions(20);
  } catch (e) {
    console.error("about:namecoin: getRecentResolutions IPC failed:", e);
    showUnavailable("resolutions-tbody", "resolutions-badge", 5, String(e));
    return;
  }

  if (response.error) {
    showUnavailable(
      "resolutions-tbody",
      "resolutions-badge",
      5,
      `Error: ${response.error}`
    );
    return;
  }

  const resolutions = response.resolutions || [];
  const tbody = $("resolutions-tbody");
  clearTable(tbody);

  $("resolutions-badge").textContent = `${resolutions.length} lookups`;

  if (resolutions.length === 0) {
    tbody.appendChild(
      createRow([
        {
          html: '<td colspan="5" class="empty-state">No recent lookups</td>',
        },
      ])
    );
    return;
  }

  for (const r of resolutions) {
    const resultText = r.error ? `\u26A0 ${r.error}` : r.ip;
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

async function renderDANECache() {
  const actor = getActor();
  if (!actor) {
    showUnavailable("dane-tbody", "dane-badge", 5);
    return;
  }

  let response;
  try {
    response = await actor.getDANECache();
  } catch (e) {
    console.error("about:namecoin: getDANECache IPC failed:", e);
    showUnavailable("dane-tbody", "dane-badge", 5, String(e));
    return;
  }

  if (response.error) {
    showUnavailable(
      "dane-tbody",
      "dane-badge",
      5,
      `Error: ${response.error}`
    );
    return;
  }

  const entries = response.entries || [];
  const tbody = $("dane-tbody");
  clearTable(tbody);

  $("dane-badge").textContent = `${entries.length} entries`;

  if (entries.length === 0) {
    tbody.appendChild(
      createRow([
        {
          html: '<td colspan="5" class="empty-state">No DANE/TLS entries cached</td>',
        },
      ])
    );
    return;
  }

  for (const e of entries) {
    tbody.appendChild(
      createRow([
        { html: `<code>${e.domain}</code>` },
        {
          html:
            `<span class="truncated mono" title="${e.fingerprint}">` +
            `${truncateFingerprint(e.fingerprint)}</span>`,
        },
        e.matchType,
        e.usage,
        typeof e.expiryBlock === "number"
          ? e.expiryBlock.toLocaleString()
          : String(e.expiryBlock),
      ])
    );
  }
}

// ---------------------------------------------------------------------------
// Name Lookup Tool
// ---------------------------------------------------------------------------

async function performLookup(name) {
  // Normalize: strip trailing dots, ensure .bit suffix.
  name = name.trim().replace(/\.+$/, "");
  if (!name.endsWith(".bit")) {
    name += ".bit";
  }

  const resultContainer = $("lookup-result");
  const rawBox = $("result-raw");
  const parsedDl = $("result-parsed");

  resultContainer.classList.add("visible");
  rawBox.textContent = "Resolving\u2026";
  rawBox.classList.remove("result-error");
  parsedDl.innerHTML = "";

  const actor = getActor();
  if (!actor) {
    rawBox.textContent =
      "Error: NamecoinChild actor unavailable.\n" +
      "The resolver service may not be registered in this build.";
    rawBox.classList.add("result-error");
    return;
  }

  let response;
  try {
    response = await actor.resolveName(name);
  } catch (e) {
    rawBox.textContent = `Error: IPC failure\n\n${e}`;
    rawBox.classList.add("result-error");
    return;
  }

  if (response.error) {
    const strippedName = name.replace(/\.bit$/, "");
    rawBox.textContent =
      `Error: ${response.error}\n\n` +
      (response.errorMessage || `Resolution of "d/${strippedName}" failed.`);
    rawBox.classList.add("result-error");
    return;
  }

  const result = response.result;
  if (!result) {
    rawBox.textContent = "Error: Empty result from resolver.";
    rawBox.classList.add("result-error");
    return;
  }

  // Display raw JSON value.
  const value = result.value ?? result;
  rawBox.textContent = JSON.stringify(value, null, 2);

  // Display parsed result fields.
  const strippedName = name.replace(/\.bit$/, "");
  const fields = [
    ["Domain", name],
    ["Namecoin Name", `d/${strippedName}`],
    ["IPv4", value.ip || "\u2014"],
    ["IPv6", value.ip6 || "\u2014"],
    ["Translate", value.translate || "\u2014"],
    ["Name Op", result.nameOp || "\u2014"],
    ["Block Height", result.blockHeight
      ? result.blockHeight.toLocaleString()
      : "\u2014"],
    ["TX Hash", result.txHash || "\u2014"],
    ["Expires At Block", result.expiresAtBlock
      ? result.expiresAtBlock.toLocaleString()
      : "\u2014"],
    ["Has TLSA", value.tls ? "Yes" : "No"],
  ];

  for (const [label, val] of fields) {
    if (val === "\u2014") {
      continue;
    }
    const dt = document.createElement("dt");
    dt.textContent = label;
    const dd = document.createElement("dd");
    dd.textContent = val;
    parsedDl.appendChild(dt);
    parsedDl.appendChild(dd);
  }
}

// ---------------------------------------------------------------------------
// Auto-refresh timer (pauses when page hidden)
// ---------------------------------------------------------------------------

let refreshTimer = null;
const REFRESH_INTERVAL_MS = 30000;

function startAutoRefresh() {
  stopAutoRefresh();
  refreshTimer = setInterval(() => {
    renderServers();
  }, REFRESH_INTERVAL_MS);
}

function stopAutoRefresh() {
  if (refreshTimer !== null) {
    clearInterval(refreshTimer);
    refreshTimer = null;
  }
}

// ---------------------------------------------------------------------------
// Event wiring
// ---------------------------------------------------------------------------

document.addEventListener("DOMContentLoaded", () => {
  // Initial data load — all async, errors handled per-panel.
  renderStatusBanner();
  renderServers();
  renderResolutions();
  renderDANECache();

  // Name lookup tool.
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

  lookupInput.addEventListener("keydown", e => {
    if (e.key === "Enter") {
      lookupBtn.click();
    }
  });

  // Auto-refresh server status; pause when tab hidden.
  startAutoRefresh();

  document.addEventListener("visibilitychange", () => {
    if (document.hidden) {
      stopAutoRefresh();
    } else {
      renderServers();
      startAutoRefresh();
    }
  });
});
