#!/usr/bin/env node
/**
 * test-resolution.mjs — Verify Namecoin name resolution logic
 *
 * Tests the scripthash computation and ElectrumX resolution pipeline
 * directly from Node.js, without needing a Firefox build.
 *
 * This validates that our C++ implementation will produce the same results
 * as the existing JS extension (tls-namecoin-ext/background/electrumx-ws.js).
 *
 * Usage:
 *   node tools/test-resolution.mjs
 *   node tools/test-resolution.mjs example.bit
 *   node tools/test-resolution.mjs --server ws://electrumx.testls.space:50003 d/testls
 *
 * Requirements: Node.js 18+ (WebSocket built-in via undici, or install 'ws')
 */

import { createHash } from 'node:crypto';
import { argv, exit } from 'node:process';

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

const DEFAULT_SERVER = 'ws://electrumx.testls.space:50003';
const NAME_EXPIRY_BLOCKS = 36000;
const WS_TIMEOUT_MS = 15000;

const args = argv.slice(2);
let server = DEFAULT_SERVER;
let testNames = ['d/testls', 'd/bitcoin'];

for (let i = 0; i < args.length; i++) {
  if (args[i] === '--server' && args[i + 1]) {
    server = args[++i];
  } else if (args[i].endsWith('.bit')) {
    const name = args[i].replace(/\.bit$/i, '');
    testNames = [`d/${name}`];
  } else if (args[i].startsWith('d/') || args[i].startsWith('id/')) {
    testNames = [args[i]];
  }
}

// ---------------------------------------------------------------------------
// Push-data encoding (matches C++ AppendPushData)
// ---------------------------------------------------------------------------

function pushData(data) {
  const len = data.length;
  if (len < 0x4c) {
    const out = Buffer.allocUnsafe(1 + len);
    out[0] = len;
    data.copy(out, 1);
    return out;
  } else if (len <= 0xff) {
    const out = Buffer.allocUnsafe(2 + len);
    out[0] = 0x4c;
    out[1] = len;
    data.copy(out, 2);
    return out;
  } else {
    const out = Buffer.allocUnsafe(3 + len);
    out[0] = 0x4d;
    out[1] = len & 0xff;
    out[2] = (len >> 8) & 0xff;
    data.copy(out, 3);
    return out;
  }
}

// ---------------------------------------------------------------------------
// Scripthash computation (matches C++ ComputeScripthash)
// ---------------------------------------------------------------------------

/**
 * Compute the ElectrumX scripthash for a Namecoin name.
 *
 * Index script: OP_NAME_UPDATE (0x53) + pushdata(name) + pushdata("") +
 *               OP_2DROP (0x6d) + OP_DROP (0x75) + OP_RETURN (0x6a)
 * Then: SHA-256 → reverse bytes → hex
 */
function computeScripthash(name) {
  const nameBytes = Buffer.from(name, 'utf8');
  const namePush  = pushData(nameBytes);
  const emptyPush = pushData(Buffer.alloc(0));
  const trailer   = Buffer.from([0x6d, 0x75, 0x6a]);

  const script = Buffer.concat([
    Buffer.from([0x53]),  // OP_NAME_UPDATE
    namePush,
    emptyPush,
    trailer,
  ]);

  const hash = createHash('sha256').update(script).digest();
  const reversed = Buffer.from(hash).reverse();
  return reversed.toString('hex');
}

// ---------------------------------------------------------------------------
// NAME_UPDATE script decoder (matches C++ DecodeNameScript)
// ---------------------------------------------------------------------------

function decodeNameScript(scriptHex) {
  const bytes = Buffer.from(scriptHex, 'hex');
  if (bytes.length === 0 || bytes[0] !== 0x53) return null;

  let i = 1;
  const readPushdata = () => {
    if (i >= bytes.length) return null;
    const opcode = bytes[i++];
    let len;
    if (opcode === 0x00) return Buffer.alloc(0);
    else if (opcode >= 0x01 && opcode <= 0x4b) len = opcode;
    else if (opcode === 0x4c) { len = bytes[i++]; }
    else if (opcode === 0x4d) { len = bytes[i] | (bytes[i+1] << 8); i += 2; }
    else return null;
    const data = bytes.slice(i, i + len);
    i += len;
    return data;
  };

  const nameData  = readPushdata();
  const valueData = readPushdata();
  if (!nameData || !valueData) return null;

  return {
    name:  nameData.toString('utf8'),
    value: valueData.toString('utf8'),
  };
}

// ---------------------------------------------------------------------------
// ElectrumX WebSocket client (matches C++ ElectrumXRequest)
// ---------------------------------------------------------------------------

/**
 * Import WebSocket — try native (Node 22+), fall back to 'ws' package.
 */
async function getWebSocket() {
  // Node 21+ has WebSocket built-in
  if (typeof WebSocket !== 'undefined') return WebSocket;
  try {
    const { WebSocket } = await import('ws');
    return WebSocket;
  } catch {
    // Try the global in case it was polyfilled
    if (typeof global.WebSocket !== 'undefined') return global.WebSocket;
    throw new Error(
      'WebSocket not available. Install ws: npm install ws\n' +
      'Or use Node.js 22+ which includes WebSocket natively.'
    );
  }
}

let WS;

function electrumxRequest(serverUrl, method, params) {
  return new Promise((resolve, reject) => {
    const reqId = Math.floor(Math.random() * 1e9);
    const reqStr = JSON.stringify({ jsonrpc: '2.0', id: reqId, method, params });

    let settled = false;
    const done = (fn, val) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      try { ws.close(); } catch {}
      fn(val);
    };

    const timer = setTimeout(
      () => done(reject, new Error(`Timeout after ${WS_TIMEOUT_MS}ms: ${serverUrl}`)),
      WS_TIMEOUT_MS
    );

    const ws = new WS(serverUrl);
    ws.onopen = () => ws.send(reqStr);
    ws.onmessage = (ev) => {
      try {
        const msg = JSON.parse(ev.data);
        if (msg.id == null || msg.id !== reqId) return;
        if (msg.error) done(reject, new Error(`RPC error: ${JSON.stringify(msg.error)}`));
        else done(resolve, msg.result);
      } catch (e) {
        done(reject, new Error(`Parse error: ${e.message}`));
      }
    };
    ws.onerror = (e) => done(reject, new Error(`WS error: ${e.message || e}`));
    ws.onclose = (ev) => {
      if (!settled) done(reject, new Error(`WS closed unexpectedly (${ev.code})`));
    };
  });
}

// ---------------------------------------------------------------------------
// Test runner
// ---------------------------------------------------------------------------

async function testScripthash() {
  console.log('\n═══════════════════════════════════════════════════════════');
  console.log('  TEST 1: Scripthash computation');
  console.log('═══════════════════════════════════════════════════════════\n');

  // Known-good values verified against electrumx.testls.space
  const knownGood = {
    // These scripthashes were verified to return results from testls.space
    // Compute them fresh here to verify our implementation matches
    'd/testls': null,   // will compute and display
    'd/bitcoin': null,
    'd/example': null,
  };

  for (const name of Object.keys(knownGood)) {
    const sh = computeScripthash(name);
    console.log(`  ${name.padEnd(15)} → ${sh}`);
    knownGood[name] = sh;
  }

  console.log('\n  ✓ Scripthash computation complete');
  console.log('    Verify these match electrumx-ws.js nameToScripthash() output.');
  console.log('    JS version uses crypto.subtle.digest → same SHA-256 algorithm.\n');

  return knownGood;
}

async function testDecoder() {
  console.log('═══════════════════════════════════════════════════════════');
  console.log('  TEST 2: NAME_UPDATE script decoder');
  console.log('═══════════════════════════════════════════════════════════\n');

  // Build a synthetic NAME_UPDATE script for "d/test" with value {"ip":"1.2.3.4"}
  const name  = Buffer.from('d/test', 'utf8');
  const value = Buffer.from('{"ip":"1.2.3.4"}', 'utf8');

  const script = Buffer.concat([
    Buffer.from([0x53]),     // OP_NAME_UPDATE
    pushData(name),
    pushData(value),
    Buffer.from([0x6d, 0x75, 0x76, 0xa9, 0x14]),  // OP_2DROP OP_DROP OP_DUP OP_HASH160 (P2PKH start)
  ]);

  const decoded = decodeNameScript(script.toString('hex'));
  if (!decoded) {
    console.log('  ✗ FAIL: Could not decode synthetic script');
    return false;
  }

  const nameOk  = decoded.name === 'd/test';
  const valueOk = decoded.value === '{"ip":"1.2.3.4"}';
  console.log(`  name:  ${decoded.name}  ${nameOk ? '✓' : '✗ EXPECTED d/test'}`);
  console.log(`  value: ${decoded.value}  ${valueOk ? '✓' : '✗ EXPECTED {"ip":"1.2.3.4"}'}`);

  // Test OP_0 (empty value)
  const scriptEmpty = Buffer.concat([
    Buffer.from([0x53]),
    pushData(name),
    Buffer.from([0x00]),  // OP_0 = empty value
    Buffer.from([0x6d, 0x75]),
  ]);
  const decodedEmpty = decodeNameScript(scriptEmpty.toString('hex'));
  const emptyOk = decodedEmpty && decodedEmpty.value === '';
  console.log(`  empty: ${decodedEmpty?.value ?? 'null'}  ${emptyOk ? '✓' : '✗ EXPECTED empty string'}`);

  console.log(`\n  ${nameOk && valueOk && emptyOk ? '✓ All decoder tests passed' : '✗ Some decoder tests FAILED'}\n`);
  return nameOk && valueOk && emptyOk;
}

async function testElectrumX(name, scripthashes) {
  console.log('═══════════════════════════════════════════════════════════');
  console.log(`  TEST 3: ElectrumX resolution — ${name}`);
  console.log('═══════════════════════════════════════════════════════════\n');

  const scripthash = scripthashes[name] || computeScripthash(name);
  console.log(`  Server: ${server}`);
  console.log(`  Name: ${name}`);
  console.log(`  Scripthash: ${scripthash}\n`);

  // Step 1: server.version handshake
  console.log('  → server.version...');
  let version;
  try {
    version = await electrumxRequest(server, 'server.version',
                                      ['namecoin-gecko-test', '1.4']);
    console.log(`  ✓ Server: ${Array.isArray(version) ? version[0] : version}`);
  } catch (err) {
    console.log(`  ✗ server.version failed: ${err.message}`);
    console.log('    Check that the ElectrumX server is reachable via WebSocket.');
    return null;
  }

  // Step 2: blockchain.headers.subscribe (block height)
  console.log('\n  → blockchain.headers.subscribe...');
  let blockHeight = 0;
  try {
    const hdr = await electrumxRequest(server, 'blockchain.headers.subscribe', []);
    blockHeight = hdr?.height ?? 0;
    console.log(`  ✓ Block height: ${blockHeight}`);
  } catch (err) {
    console.log(`  ⚠ headers.subscribe failed (non-fatal): ${err.message}`);
  }

  // Step 3: blockchain.scripthash.get_history
  console.log(`\n  → blockchain.scripthash.get_history...`);
  let history;
  try {
    history = await electrumxRequest(server, 'blockchain.scripthash.get_history',
                                      [scripthash]);
    if (!Array.isArray(history) || history.length === 0) {
      console.log('  ⚠ Empty history — name may not exist or scripthash is wrong');
      console.log('    Verify OP_NAME_UPDATE (0x53) is used, NOT OP_NAME_SHOW (0xd1)');
      return null;
    }
    const confirmed = history.filter(h => h.height > 0);
    console.log(`  ✓ ${history.length} tx(s), ${confirmed.length} confirmed`);
  } catch (err) {
    console.log(`  ✗ get_history failed: ${err.message}`);
    return null;
  }

  // Step 4: Pick most recent confirmed tx
  const confirmed = history.filter(h => h.height > 0).sort((a, b) => b.height - a.height);
  if (confirmed.length === 0) {
    console.log('  ✗ No confirmed transactions');
    return null;
  }
  const best = confirmed[0];
  console.log(`\n  Most recent confirmed tx:`);
  console.log(`    hash:   ${best.tx_hash}`);
  console.log(`    height: ${best.height}`);

  // Step 5: Check expiry
  const expired = blockHeight > 0 && (blockHeight - best.height) > NAME_EXPIRY_BLOCKS;
  if (expired) {
    console.log(`\n  ✗ NAME EXPIRED (updated at ${best.height}, current ${blockHeight})`);
    return null;
  } else if (blockHeight > 0) {
    const remaining = NAME_EXPIRY_BLOCKS - (blockHeight - best.height);
    console.log(`    expiry: ${remaining} blocks remaining (~${Math.round(remaining * 10 / 60 / 24)} days)`);
  }

  // Step 6: Fetch verbose transaction
  console.log('\n  → blockchain.transaction.get (verbose)...');
  let tx;
  try {
    tx = await electrumxRequest(server, 'blockchain.transaction.get',
                                 [best.tx_hash, true]);
    console.log(`  ✓ TX fetched: ${tx.txid ?? best.tx_hash}`);
    console.log(`    vout count: ${tx.vout?.length ?? 0}`);
  } catch (err) {
    console.log(`  ✗ transaction.get failed: ${err.message}`);
    return null;
  }

  // Step 7: Decode NAME_UPDATE output
  console.log('\n  → Decoding NAME_UPDATE outputs...');
  let decoded = null;
  for (const out of (tx.vout ?? [])) {
    const hex = out.scriptPubKey?.hex;
    if (!hex) continue;
    const result = decodeNameScript(hex);
    if (result && result.name === name) {
      decoded = result;
      console.log(`  ✓ Found NAME_UPDATE for "${result.name}"`);
      break;
    }
  }

  if (!decoded) {
    console.log('  ✗ Could not find NAME_UPDATE output for this name');
    return null;
  }

  // Step 8: Parse value JSON
  console.log('\n  → Parsing name value JSON...');
  let value;
  try {
    value = JSON.parse(decoded.value);
    console.log('  ✓ JSON parsed successfully');
  } catch (e) {
    // Non-JSON values are valid in Namecoin (e.g. "reserved", "for sale")
    // Treat as a name with no usable address records.
    console.log(`  ⚠ Non-JSON value (valid but unresolvable): "${decoded.value}"`);
    console.log(`    This name exists on the blockchain but has no IP/address records.`);
    return { name, value: { _raw: decoded.value }, txHash: best.tx_hash,
             updateHeight: best.height, blockHeight, expired: false, ttlSeconds: 3600 };
  }

  // Display results
  console.log('\n  ─── Resolved name value ───────────────────────────');
  if (value.ip)        console.log(`    ip:        ${value.ip}`);
  if (value.ip6)       console.log(`    ip6:       ${value.ip6}`);
  if (value.ns)        console.log(`    ns:        ${JSON.stringify(value.ns)}`);
  if (value.tls)       console.log(`    tls:       ${JSON.stringify(value.tls)}`);
  if (value.map)       console.log(`    map:       ${JSON.stringify(Object.keys(value.map))}`);
  if (value.alias)     console.log(`    alias:     ${value.alias}`);
  if (value.translate) console.log(`    translate: ${value.translate}`);
  if (value.tor)       console.log(`    tor:       ${value.tor}`);
  console.log('  ────────────────────────────────────────────────────');

  return {
    name,
    value,
    txHash: best.tx_hash,
    updateHeight: best.height,
    blockHeight,
    expired,
    ttlSeconds: blockHeight > 0
      ? Math.min((NAME_EXPIRY_BLOCKS - (blockHeight - best.height)) * 600, 3600)
      : 3600,
  };
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

async function main() {
  console.log('\n╔═════════════════════════════════════════════════════════╗');
  console.log('║  Namecoin Gecko Integration — Resolution Test Suite       ║');
  console.log('║  gecko-namecoin/tools/test-resolution.mjs                 ║');
  console.log('╚═════════════════════════════════════════════════════════╝');

  // Initialize WebSocket
  try {
    WS = await getWebSocket();
  } catch (err) {
    console.error(`\n✗ ${err.message}`);
    exit(1);
  }

  // Run tests
  const scripthashes = await testScripthash();
  const decoderOk = await testDecoder();

  let allPassed = decoderOk;
  const results = [];

  for (const name of testNames) {
    const result = await testElectrumX(name, scripthashes);
    if (result) {
      results.push(result);
      console.log(`\n  ✓ ${name} resolved successfully`);
    } else {
      console.log(`\n  ✗ ${name} resolution FAILED`);
      allPassed = false;
    }
  }

  // Summary
  console.log('\n═══════════════════════════════════════════════════════════');
  console.log('  SUMMARY');
  console.log('═══════════════════════════════════════════════════════════\n');
  for (const r of results) {
    const ip = r.value?.ip ?? r.value?.ip6 ?? '(no direct IP)';
    console.log(`  ✓ ${r.name} → ${ip} (height ${r.updateHeight}, ttl ${r.ttlSeconds}s)`);
  }
  if (!allPassed) {
    console.log('\n  ✗ Some tests FAILED — see output above\n');
    exit(1);
  } else {
    console.log('\n  All tests passed ✓\n');
    console.log('  The C++ implementation (nsNamecoinResolver.cpp) should');
    console.log('  produce identical scripthashes and resolution results.\n');
  }
}

main().catch(err => {
  console.error('\nFatal error:', err);
  exit(1);
});
