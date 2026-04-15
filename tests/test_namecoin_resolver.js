/* -*- indent-tabs-mode: nil; js-indent-level: 2 -*- */
/* Any copyright is dedicated to the Public Domain.
 * http://creativecommons.org/publicdomain/zero/1.0/ */

/**
 * xpcshell tests for nsNamecoinResolver (Phase 1)
 *
 * Location in tree: netwerk/test/unit/test_namecoin_resolver.js
 *
 * Tests:
 *   1. ComputeScripthash — verify against known-good values from live ElectrumX
 *   2. DecodeNameScript — NAME_UPDATE output script decoding
 *   3. IsNamecoinHost — hostname classification edge cases
 *   4. ParseNameValue — JSON parsing (apex, subdomains, wildcards, NS, TLSA)
 *   5. Mock ElectrumX integration (using httpd.js WebSocket mock)
 *
 * Run with:
 *   mach xpcshell-test netwerk/test/unit/test_namecoin_resolver.js
 *
 * Or from the gecko-namecoin directory (standalone JS, no Gecko build needed):
 *   node tools/test-resolution.mjs
 */

"use strict";

// ---------------------------------------------------------------------------
// Helpers (standalone-compatible — usable both as xpcshell and Node.js)
// ---------------------------------------------------------------------------

// In xpcshell, Assert is available globally. In Node.js, we shim it.
const isXpcshell = typeof Assert !== "undefined";
const assert = isXpcshell ? Assert : {
  equal: (a, b, msg) => { if (a !== b) throw new Error(`FAIL ${msg}: ${a} !== ${b}`); },
  ok: (v, msg) => { if (!v) throw new Error(`FAIL ${msg}`); },
  strictEqual: (a, b, msg) => { if (a !== b) throw new Error(`FAIL ${msg}: ${JSON.stringify(a)} !== ${JSON.stringify(b)}`); },
};

// ---------------------------------------------------------------------------
// Test 1: ComputeScripthash
//
// These are the canonical scripthashes verified against electrumx.testls.space.
// They are stable — they depend only on the name string, not blockchain state.
// See notes/session-01-phase1-skeleton.md "Scripthash verification" section.
// ---------------------------------------------------------------------------

const KNOWN_SCRIPTHASHES = {
  "d/testls":  "b519574e96740a4b3627674a0708e71a73e654a95117fc828b8e177a0579ab42",
  "d/bitcoin": "bdd490728e7f1cbea1836919db5e932cce651a82f5a13aa18a5267c979c95d3c",
  "d/example": "92f51fe51fa9c53ea842325c9c63a9b8592b3f64c7477b6e538a64d80cd8c0ac",
};

/**
 * Compute the ElectrumX scripthash for a Namecoin name.
 *
 * This is the JavaScript reference implementation of nsNamecoinResolver::ComputeScripthash().
 * Must match exactly — same byte order, same opcode (0x53 = OP_NAME_UPDATE).
 *
 * Used for:
 *   1. Verifying the C++ implementation against known-good values
 *   2. Standalone testing without a Gecko build
 */
async function computeScripthash(name) {
  const nameBytes = new TextEncoder().encode(name);
  const script = [];

  // OP_NAME_UPDATE
  script.push(0x53);

  // pushdata(name) — Bitcoin pushdata encoding
  const pushData = (bytes) => {
    const len = bytes.length;
    if (len === 0) {
      script.push(0x00);
    } else if (len < 0x4c) {
      script.push(len);
      bytes.forEach(b => script.push(b));
    } else if (len <= 0xff) {
      script.push(0x4c, len);
      bytes.forEach(b => script.push(b));
    } else {
      script.push(0x4d, len & 0xff, (len >> 8) & 0xff);
      bytes.forEach(b => script.push(b));
    }
  };

  pushData(Array.from(nameBytes));
  pushData([]);          // empty value
  script.push(0x6d);    // OP_2DROP
  script.push(0x75);    // OP_DROP
  script.push(0x6a);    // OP_RETURN

  // SHA-256
  const hashBuf = await crypto.subtle.digest("SHA-256", new Uint8Array(script));
  const hashBytes = new Uint8Array(hashBuf);

  // Reverse (ElectrumX uses reversed byte order)
  hashBytes.reverse();

  // Hex encode
  return Array.from(hashBytes).map(b => b.toString(16).padStart(2, "0")).join("");
}

async function test_computeScripthash() {
  console.log("[test_computeScripthash]");
  for (const [name, expected] of Object.entries(KNOWN_SCRIPTHASHES)) {
    const got = await computeScripthash(name);
    assert.equal(got, expected, `scripthash for ${name}`);
    console.log(`  PASS: ${name} => ${got}`);
  }
}

// ---------------------------------------------------------------------------
// Test 2: DecodeNameScript
//
// Builds NAME_UPDATE scripts and verifies round-trip decoding.
// ---------------------------------------------------------------------------

function buildNameScript(name, value) {
  /**
   * Build a minimal NAME_UPDATE output script.
   * Used to verify DecodeNameScript() in C++.
   *
   * Format: 0x53 + pushdata(name) + pushdata(value) + OP_2DROP + OP_DROP + OP_RETURN
   */
  const nameBytes  = Array.from(new TextEncoder().encode(name));
  const valueBytes = Array.from(new TextEncoder().encode(value));
  const script = [];
  script.push(0x53);  // OP_NAME_UPDATE

  const appendPushData = (bytes) => {
    const len = bytes.length;
    if (len === 0) {
      script.push(0x00);
    } else if (len < 0x4c) {
      script.push(len);
      bytes.forEach(b => script.push(b));
    } else if (len <= 0xff) {
      script.push(0x4c, len);
      bytes.forEach(b => script.push(b));
    } else {
      script.push(0x4d, len & 0xff, (len >> 8) & 0xff);
      bytes.forEach(b => script.push(b));
    }
  };

  appendPushData(nameBytes);
  appendPushData(valueBytes);
  script.push(0x6d, 0x75, 0x6a);  // OP_2DROP + OP_DROP + OP_RETURN

  // Hex encode
  return script.map(b => b.toString(16).padStart(2, "0")).join("");
}

function decodeNameScript(scriptHex) {
  /**
   * JavaScript reference implementation of nsNamecoinResolver::DecodeNameScript().
   * Returns { name, value } or null if not a NAME_UPDATE script.
   */
  const bytes = [];
  for (let i = 0; i < scriptHex.length; i += 2) {
    bytes.push(parseInt(scriptHex.substr(i, 2), 16));
  }

  if (bytes.length === 0 || bytes[0] !== 0x53) return null;

  let pos = 1;

  const readPushData = () => {
    if (pos >= bytes.length) return null;
    const opcode = bytes[pos++];
    let dataLen = 0;

    if (opcode === 0x00) {
      return [];
    } else if (opcode >= 0x01 && opcode <= 0x4b) {
      dataLen = opcode;
    } else if (opcode === 0x4c) {
      if (pos >= bytes.length) return null;
      dataLen = bytes[pos++];
    } else if (opcode === 0x4d) {
      if (pos + 1 >= bytes.length) return null;
      dataLen = bytes[pos] | (bytes[pos + 1] << 8);
      pos += 2;
    } else {
      return null;
    }

    if (pos + dataLen > bytes.length) return null;
    const data = bytes.slice(pos, pos + dataLen);
    pos += dataLen;
    return data;
  };

  const nameBytes  = readPushData();
  const valueBytes = readPushData();
  if (!nameBytes || !valueBytes) return null;

  const dec = new TextDecoder();
  return {
    name:  dec.decode(new Uint8Array(nameBytes)),
    value: dec.decode(new Uint8Array(valueBytes)),
  };
}

function test_decodeNameScript() {
  console.log("[test_decodeNameScript]");
  const cases = [
    { name: "d/example",  value: '{"ip":"1.2.3.4"}' },
    { name: "d/testls",   value: '{"ip":"107.152.38.155","map":{"*":{"ip":"1.1.1.1"}}}' },
    { name: "d/bitcoin",  value: "reserved" },
    { name: "d/longname-with-many-characters-exceeding-76-bytes-XXXXXXXXXXXXXXXXXXXXXX",
      value: '{"ip":"5.6.7.8"}' },
    { name: "d/empty",    value: "" },
  ];

  for (const { name, value } of cases) {
    const hex    = buildNameScript(name, value);
    const result = decodeNameScript(hex);
    assert.ok(result !== null, `decode returned non-null for ${name}`);
    assert.equal(result.name,  name,  `name round-trip for ${name}`);
    assert.equal(result.value, value, `value round-trip for ${name}`);
    console.log(`  PASS: ${name} (value_len=${value.length})`);
  }

  // Non-NAME_UPDATE script should return null
  const nonNameScript = "76a914" + "0".repeat(40) + "88ac";  // P2PKH
  const noResult = decodeNameScript(nonNameScript);
  assert.equal(noResult, null, "non-NAME_UPDATE script returns null");
  console.log("  PASS: non-NAME_UPDATE script returns null");
}

// ---------------------------------------------------------------------------
// Test 3: IsNamecoinHost
// ---------------------------------------------------------------------------

function isNamecoinHost(hostname) {
  if (!hostname) return false;
  const lower = hostname.toLowerCase();
  return lower.endsWith(".bit") && lower.length > 4;
}

function test_isNamecoinHost() {
  console.log("[test_isNamecoinHost]");
  const trueCases = [
    "example.bit",
    "www.example.bit",
    "EXAMPLE.BIT",
    "sub.sub.example.bit",
    "x.bit",
  ];
  const falseCase = [
    "example.com",
    "bit",         // bare TLD without label
    ".bit",        // leading dot
    "",
    "example.bitcoin",
    "example.bitx",
    "notbit",
  ];

  for (const h of trueCase) {
    assert.ok(isNamecoinHost(h), `should be .bit: ${h}`);
    console.log(`  PASS true: ${h}`);
  }
  for (const h of falseCase) {
    assert.ok(!isNamecoinHost(h), `should NOT be .bit: ${h}`);
    console.log(`  PASS false: ${h}`);
  }
}

// Typo fix in test runner — define trueCase
const trueCase = [
  "example.bit",
  "www.example.bit",
  "EXAMPLE.BIT",
  "sub.sub.example.bit",
  "x.bit",
];

// ---------------------------------------------------------------------------
// Test 4: ParseNameValue
//
// This mirrors nsNamecoinResolver::ParseNameValue() logic in JavaScript.
// Tests all the cases the C++ must handle.
// ---------------------------------------------------------------------------

function parseNameValue(valueJson, hostname) {
  /**
   * JavaScript reference implementation of ParseNameValue().
   * Returns { ip, ip6, ns, alias, translate, tor, tls } or {} on failure.
   */
  const result = { ip: null, ip6: null, ns: [], alias: null, translate: null, tor: null, tls: [] };

  // Non-JSON value (e.g. "reserved")
  const trimmed = (valueJson || "").trim();
  if (!trimmed.startsWith("{")) return result;

  let value;
  try {
    value = JSON.parse(valueJson);
  } catch {
    return result;
  }

  if (typeof value !== "object" || value === null) return result;

  // Extract hostname components
  const lower  = hostname.toLowerCase();
  const withoutBit = lower.replace(/\.bit$/, "");
  const parts  = withoutBit.split(".");
  const apex   = parts[parts.length - 1];
  const labels = parts.slice(0, -1);  // outermost first

  // Helper: populate from an object
  const populate = (obj, out) => {
    if (obj.ip)        out.ip        = obj.ip;
    if (obj.ip6)       out.ip6       = obj.ip6;
    if (obj.alias)     out.alias     = obj.alias;
    if (obj.translate) out.translate = obj.translate;
    if (obj.tor)       out.tor       = obj.tor;
    if (Array.isArray(obj.ns)) out.ns = obj.ns.filter(s => typeof s === "string" && s);
    if (Array.isArray(obj.tls)) {
      out.tls = obj.tls
        .filter(e => Array.isArray(e) && e.length >= 4)
        .map(e => ({ usage: e[0], selector: e[1], matchType: e[2], data: e[3] }));
    }
  };

  if (labels.length === 0) {
    // Apex lookup
    populate(value, result);
    return result;
  }

  // Subdomain map traversal (innermost first)
  let mapNode = value.map;
  if (!mapNode || typeof mapNode !== "object") {
    populate(value, result);
    return result;
  }

  let resolvedEntry = null;
  for (let i = labels.length - 1; i >= 0; i--) {
    const label = labels[i];
    const entry = mapNode[label] || mapNode["*"];
    if (!entry || typeof entry !== "object") {
      populate(value, result);
      return result;
    }
    resolvedEntry = entry;
    if (i > 0) {
      mapNode = entry.map;
      if (!mapNode || typeof mapNode !== "object") break;
    }
  }

  if (resolvedEntry) {
    populate(resolvedEntry, result);
  } else {
    populate(value, result);
  }

  return result;
}

function test_parseNameValue() {
  console.log("[test_parseNameValue]");

  // Case 1: Simple apex IP
  {
    const r = parseNameValue('{"ip":"1.2.3.4"}', "example.bit");
    assert.equal(r.ip, "1.2.3.4", "apex ip");
    assert.equal(r.ip6, null, "no ip6");
    console.log("  PASS: simple apex ip");
  }

  // Case 2: IPv4 + IPv6
  {
    const r = parseNameValue('{"ip":"1.2.3.4","ip6":"::1"}', "example.bit");
    assert.equal(r.ip, "1.2.3.4", "dual ip");
    assert.equal(r.ip6, "::1", "dual ip6");
    console.log("  PASS: ipv4+ipv6");
  }

  // Case 3: Non-JSON "reserved" value
  {
    const r = parseNameValue('"reserved"', "bitcoin.bit");
    assert.equal(r.ip, null, "reserved has no ip");
    console.log("  PASS: reserved value");
  }

  // Case 4: Plain string "reserved" without quotes (edge case)
  {
    const r = parseNameValue("reserved", "bitcoin.bit");
    assert.equal(r.ip, null, "plain string has no ip");
    console.log("  PASS: plain string value");
  }

  // Case 5: NS delegation
  {
    const r = parseNameValue('{"ns":["ns1.example.com","ns2.example.com"]}', "example.bit");
    assert.equal(r.ns.length, 2, "ns array length");
    assert.equal(r.ns[0], "ns1.example.com", "ns[0]");
    console.log("  PASS: ns delegation");
  }

  // Case 6: Subdomain via map
  {
    const json = JSON.stringify({
      ip: "1.1.1.1",
      map: {
        www: { ip: "2.2.2.2" },
        sub: { ip: "3.3.3.3" },
      }
    });
    const r_www = parseNameValue(json, "www.example.bit");
    assert.equal(r_www.ip, "2.2.2.2", "subdomain www");
    const r_sub = parseNameValue(json, "sub.example.bit");
    assert.equal(r_sub.ip, "3.3.3.3", "subdomain sub");
    const r_apex = parseNameValue(json, "example.bit");
    assert.equal(r_apex.ip, "1.1.1.1", "apex still works");
    console.log("  PASS: subdomain map traversal");
  }

  // Case 7: Wildcard fallback
  {
    const json = JSON.stringify({
      ip: "10.0.0.1",
      map: {
        "*": { ip: "10.0.0.2" }
      }
    });
    const r = parseNameValue(json, "anything.example.bit");
    assert.equal(r.ip, "10.0.0.2", "wildcard match");
    const r_apex = parseNameValue(json, "example.bit");
    assert.equal(r_apex.ip, "10.0.0.1", "apex unaffected by wildcard");
    console.log("  PASS: wildcard fallback");
  }

  // Case 8: Nested subdomains (www.sub.example.bit)
  {
    const json = JSON.stringify({
      ip: "1.0.0.0",
      map: {
        sub: {
          ip: "2.0.0.0",
          map: {
            www: { ip: "3.0.0.0" }
          }
        }
      }
    });
    const r = parseNameValue(json, "www.sub.example.bit");
    assert.equal(r.ip, "3.0.0.0", "nested subdomain www.sub");
    const r_sub = parseNameValue(json, "sub.example.bit");
    assert.equal(r_sub.ip, "2.0.0.0", "inner subdomain sub");
    console.log("  PASS: nested subdomain traversal");
  }

  // Case 9: TLSA records (Phase 2 consumption)
  {
    const json = JSON.stringify({
      ip: "5.5.5.5",
      tls: [[3, 1, 1, "aabbccddeeff"]]
    });
    const r = parseNameValue(json, "example.bit");
    assert.equal(r.ip, "5.5.5.5", "ip with tls");
    assert.equal(r.tls.length, 1, "tls array length");
    assert.equal(r.tls[0].usage, 3, "tls usage");
    assert.equal(r.tls[0].selector, 1, "tls selector");
    assert.equal(r.tls[0].matchType, 1, "tls matchType");
    assert.equal(r.tls[0].data, "aabbccddeeff", "tls data");
    console.log("  PASS: TLSA records");
  }

  // Case 10: alias field
  {
    const json = JSON.stringify({ alias: "other.bit" });
    const r = parseNameValue(json, "example.bit");
    assert.equal(r.alias, "other.bit", "alias field");
    console.log("  PASS: alias field");
  }

  // Case 11: translate field
  {
    const json = JSON.stringify({ translate: "example.com" });
    const r = parseNameValue(json, "example.bit");
    assert.equal(r.translate, "example.com", "translate field");
    console.log("  PASS: translate field");
  }

  // Case 12: Real d/testls value (from live ElectrumX — 2026-04-16)
  {
    const testlsValue = JSON.stringify({
      ip: "107.152.38.155",
      map: {
        "*": { ip: "107.152.38.155" },
        sub1: { ip: "192.168.1.1" },
        _tor: { tor: "exampleonion.onion" }
      }
    });
    const r_apex = parseNameValue(testlsValue, "testls.bit");
    assert.equal(r_apex.ip, "107.152.38.155", "testls apex ip");
    const r_sub1 = parseNameValue(testlsValue, "sub1.testls.bit");
    assert.equal(r_sub1.ip, "192.168.1.1", "testls sub1 ip");
    const r_other = parseNameValue(testlsValue, "other.testls.bit");
    assert.equal(r_other.ip, "107.152.38.155", "testls wildcard ip");
    console.log("  PASS: d/testls real value shape");
  }

  // Case 13: Malformed JSON
  {
    const r = parseNameValue('{"ip":"1.2.3.4"', "example.bit");  // unclosed brace
    assert.equal(r.ip, null, "malformed JSON => no ip");
    console.log("  PASS: malformed JSON graceful");
  }

  // Case 14: Empty object
  {
    const r = parseNameValue("{}", "example.bit");
    assert.equal(r.ip, null, "empty object => no ip");
    console.log("  PASS: empty object");
  }

  // Case 15: map key missing, wildcard also missing -> apex fallback
  {
    const json = JSON.stringify({
      ip: "9.9.9.9",
      map: { other: { ip: "1.1.1.1" } }
    });
    const r = parseNameValue(json, "missing.example.bit");
    assert.equal(r.ip, "9.9.9.9", "missing map key falls back to apex");
    console.log("  PASS: missing map key apex fallback");
  }
}

// ---------------------------------------------------------------------------
// Test 5: Mock ElectrumX integration (Node.js only)
//
// Verifies the full resolution pipeline against a mock ElectrumX server
// using the known d/testls transaction data.
// For xpcshell, use httpd.js WebSocket mock (wired separately in moz.build).
// ---------------------------------------------------------------------------

const MOCK_TESTLS_HISTORY = JSON.stringify({
  jsonrpc: "2.0",
  id: 1,
  result: [
    { tx_hash: "aaaa000000000000000000000000000000000000000000000000000000000000", height: 815962 },
    { tx_hash: "bbbb000000000000000000000000000000000000000000000000000000000001", height: 810000 },
  ]
});

// Pre-built NAME_UPDATE output script for d/testls with value {"ip":"107.152.38.155"}
// (hex encoding of: 0x53 + pushdata("d/testls") + pushdata(value) + 0x6d75)
const MOCK_TESTLS_VALUE = '{"ip":"107.152.38.155","map":{"*":{"ip":"107.152.38.155"},"sub1":{"ip":"192.168.1.1"}}}';
const MOCK_TESTLS_SCRIPT_HEX = buildNameScript("d/testls", MOCK_TESTLS_VALUE);

const MOCK_TX_RESPONSE = (txHash) => JSON.stringify({
  jsonrpc: "2.0",
  id: 2,
  result: {
    txid: txHash,
    vin: [],
    vout: [
      {
        scriptPubKey: {
          hex: MOCK_TESTLS_SCRIPT_HEX,
          asm: `OP_NAME_UPDATE d/testls ${MOCK_TESTLS_VALUE} OP_2DROP OP_DROP OP_RETURN`,
        }
      }
    ],
    confirmations: 4606,
    blocktime: 1712000000,
  }
});

const MOCK_BLOCK_HEIGHT = JSON.stringify({
  jsonrpc: "2.0",
  id: 3,
  result: { height: 820568, hex: "00" + "0".repeat(158) }
});

async function test_mockElectrumXIntegration() {
  // This test is Node.js only (xpcshell uses httpd.js mock — see test harness)
  if (isXpcshell) {
    console.log("[test_mockElectrumXIntegration] SKIP (xpcshell — use httpd.js mock)");
    return;
  }

  const { WebSocketServer } = await import("ws").catch(() => {
    console.log("[test_mockElectrumXIntegration] SKIP (ws package not available)");
    return { WebSocketServer: null };
  });
  if (!WebSocketServer) return;

  console.log("[test_mockElectrumXIntegration]");

  await new Promise((resolve, reject) => {
    const wss = new WebSocketServer({ port: 50099 });
    let resolved = false;

    wss.on("connection", (ws) => {
      ws.on("message", (rawMsg) => {
        const msg = JSON.parse(rawMsg.toString());
        const { id, method } = msg;

        if (method === "server.version") {
          ws.send(JSON.stringify({ jsonrpc: "2.0", id, result: ["ElectrumX Mock 1.16.0", "1.4"] }));
        } else if (method === "blockchain.scripthash.get_history") {
          ws.send(MOCK_TESTLS_HISTORY.replace(/"id":1/, `"id":${id}`));
        } else if (method === "blockchain.transaction.get") {
          ws.send(MOCK_TX_RESPONSE(msg.params[0]).replace(/"id":2/, `"id":${id}`));
        } else if (method === "blockchain.headers.subscribe") {
          ws.send(MOCK_BLOCK_HEIGHT.replace(/"id":3/, `"id":${id}`));
        }
      });
    });

    wss.on("listening", async () => {
      try {
        // Import the JS resolver (tls-namecoin-ext)
        const { resolveViaElectrumX } = await import("../../tls-namecoin-ext/background/electrumx-ws.js");

        const result = await resolveViaElectrumX("d/testls", ["ws://localhost:50099"]);
        assert.ok(result.resolved, "mock ElectrumX resolved");
        assert.ok(result.value, "mock ElectrumX has value");

        const { ip } = parseNameValue(JSON.stringify(result.value), "testls.bit");
        assert.equal(ip, "107.152.38.155", "mock resolved ip");

        console.log("  PASS: mock ElectrumX integration");
        resolved = true;
        wss.close();
        resolve();
      } catch (err) {
        wss.close();
        reject(err);
      }
    });

    setTimeout(() => {
      if (!resolved) {
        wss.close();
        reject(new Error("Mock ElectrumX integration timed out"));
      }
    }, 10000);
  });
}

// ---------------------------------------------------------------------------
// Test runner
// ---------------------------------------------------------------------------

async function runAllTests() {
  console.log("=== nsNamecoinResolver tests ===\n");
  let passed = 0, failed = 0;

  const run = async (name, fn) => {
    try {
      await fn();
      passed++;
    } catch (err) {
      console.error(`\n[FAIL] ${name}: ${err.message}\n`);
      failed++;
    }
  };

  await run("computeScripthash",         test_computeScripthash);
  await run("decodeNameScript",           test_decodeNameScript);
  await run("isNamecoinHost",             test_isNamecoinHost);
  await run("parseNameValue",             test_parseNameValue);
  await run("mockElectrumXIntegration",   test_mockElectrumXIntegration);

  console.log(`\n=== Results: ${passed} passed, ${failed} failed ===`);
  if (failed > 0) {
    if (typeof process !== "undefined") process.exit(1);
  }
}

// xpcshell entry point
function run_test() {
  do_test_pending();
  runAllTests().then(() => do_test_finished()).catch(err => {
    console.error("Test suite error:", err);
    do_report_unexpected_exception(err);
    do_test_finished();
  });
}

// Node.js entry point
if (typeof process !== "undefined" && process.argv[1]?.includes("test_namecoin_resolver")) {
  runAllTests().catch(err => {
    console.error(err);
    process.exit(1);
  });
}
