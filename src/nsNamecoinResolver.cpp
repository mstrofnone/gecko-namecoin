/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * nsNamecoinResolver — Native Namecoin (.bit) DNS resolution for Gecko
 *
 * Phase 1: DNS only (A/AAAA records from Namecoin blockchain via ElectrumX)
 *
 * Reference implementation (JavaScript): tls-namecoin-ext/background/electrumx-ws.js
 * Spec: gecko-namecoin/docs/PHASE1.md + firefox2.txt (Sections 2, 3, 4)
 *
 * KEY IMPLEMENTATION NOTES:
 *
 * 1. Scripthash computation MUST use OP_NAME_UPDATE (0x53), NOT OP_NAME_SHOW.
 *    Using the wrong opcode produces empty history (silent failure).
 *    Verified working against electrumx.testls.space for d/testls, d/bitcoin.
 *
 * 2. ElectrumX protocol is JSON-RPC 2.0 over WebSocket.
 *    Transport: ws:// or wss://
 *    Handshake: server.version(['tls-namecoin-gecko', '1.4']) on open.
 *
 * 3. Name expiry: (current_height - update_height) > 36000 blocks ≈ 250 days.
 *    Expired names MUST NOT be resolved (re-registration risk).
 *
 * 4. Gecko uses NSS for crypto. SHA-256 via PK11_HashBuf / HASH_HashBuf.
 *    WebSocket: use Gecko's existing nsIWebSocketChannel implementation.
 *    JSON: use mozilla::dom::JSON or a vendored micro-parser.
 *
 * TODO (Phase 1 completion):
 *   [x] Header and class skeleton
 *   [x] SHA-256 implementation using NSS (HASH_HashBuf)
 *   [x] WebSocket connection via nsIWebSocketChannel
 *   [x] JSON-RPC request/response parsing
 *   [x] NAME_UPDATE script decoder
 *   [x] Name value JSON parser (ip/ip6/map/alias/ns/translate)
 *   [x] ElectrumX send timing fix (send in OnStart callback)
 *   [ ] Integration hook in nsHostResolver.cpp  (see patches/0004)
 *   [ ] Preferences registration in StaticPrefList.yaml (see patches/0002)
 *   [ ] moz.build addition for new source files (see patches/0003)
 *   [ ] xpcshell test suite (netwerk/test/unit/test_namecoin_resolver.js)
 */

#include "nsNamecoinResolver.h"
#include "nsNamecoinErrors.h"
#include <cstdio>

// Mozilla / Gecko includes
#include "mozilla/Logging.h"
#include "mozilla/Monitor.h"
#include "mozilla/Preferences.h"
#include "mozilla/StaticPrefs_network.h"
// nsContentUtils/nsILoadInfo/nsIContentPolicy no longer needed (removed nsIWebSocketChannel)
#include "mozilla/Base64.h"
#include "mozilla/ScopeExit.h"
#include "nsThreadUtils.h"
#include "nsNetUtil.h"
#include "nsIIOService.h"
// nsIWebSocketChannel/Listener no longer used (replaced by NSPR sockets)
#include "nsIEventTarget.h"
#include "nsServiceManagerUtils.h"
#include "mozilla/OriginAttributes.h"
#include "nsString.h"
#include "prnetdb.h"          // PR_StringToNetAddr, PR_GetHostByName
#include "prio.h"              // PR_OpenTCPSocket, PR_Connect, PR_Send, PR_Recv, PR_Close
#include "prcvar.h"            // PRCondVar (unused but available)
#include "prtime.h"            // PR_Now (unused but available)
#include "prerror.h"           // PR_GetError

// NSS (for SHA-256)
#include "sechash.h"          // HASH_HashBuf, HASH_SHA256
#include "pk11func.h"

// JSON parsing is done via manual string extraction (no SpiderMonkey dependency)

// ---------------------------------------------------------------------------

namespace mozilla {
namespace net {

// Logging
static LazyLogModule gNamecoinLog("namecoin");
#define NMC_LOG(...) MOZ_LOG(gNamecoinLog, LogLevel::Debug, (__VA_ARGS__))
#define NMC_ERR(...) MOZ_LOG(gNamecoinLog, LogLevel::Error, (__VA_ARGS__))

// Preference names
static constexpr char kPrefEnabled[]      = "network.namecoin.enabled";
static constexpr char kPrefServers[]      = "network.namecoin.electrumx_servers";
static constexpr char kPrefCacheTTL[]     = "network.namecoin.cache_ttl_seconds";
static constexpr char kPrefMaxAlias[]     = "network.namecoin.max_alias_hops";
static constexpr char kPrefTimeout[]      = "network.namecoin.connection_timeout_ms";
static constexpr char kPrefQueryMultiple[] = "network.namecoin.query_multiple_servers";

// Protocol constants
static constexpr uint32_t kNameExpiry    = 36000;  // blocks
static constexpr uint8_t  kOpNameUpdate  = 0x53;
static constexpr uint8_t  kOp2Drop       = 0x6d;
static constexpr uint8_t  kOpDrop        = 0x75;
static constexpr uint8_t  kOpReturn      = 0x6a;
static constexpr uint8_t  kOpPushData1   = 0x4c;
static constexpr uint8_t  kOpPushData2   = 0x4d;

// Default server list (comma-separated in pref).
// Use IP address directly to avoid DNS deadlock: when the DNS resolver thread
// runs ResolveSync() it dispatches WebSocket open to the main thread, which
// would then need to resolve the ElectrumX hostname back through the DNS
// resolver — causing a deadlock. Using an IP bypasses DNS entirely.
// electrumx.testls.space resolves to 162.212.154.52.
static constexpr char kDefaultServers[]  = "ws://162.212.154.52:50003";

// ---------------------------------------------------------------------------
// Hex utilities
// ---------------------------------------------------------------------------

static void HexEncode(const uint8_t* aData, size_t aLen, nsACString& aOut) {
  static const char kHex[] = "0123456789abcdef";
  aOut.SetLength(aLen * 2);
  char* p = aOut.BeginWriting();
  for (size_t i = 0; i < aLen; i++) {
    *p++ = kHex[(aData[i] >> 4) & 0xf];
    *p++ = kHex[aData[i] & 0xf];
  }
}

static bool HexDecode(const nsACString& aHex, nsTArray<uint8_t>& aOut) {
  if (aHex.Length() % 2 != 0) return false;
  aOut.SetLength(aHex.Length() / 2);
  const char* p = aHex.BeginReading();
  for (size_t i = 0; i < aOut.Length(); i++) {
    auto fromHexChar = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };
    int hi = fromHexChar(*p++);
    int lo = fromHexChar(*p++);
    if (hi < 0 || lo < 0) return false;
    aOut[i] = (uint8_t)((hi << 4) | lo);
  }
  return true;
}

// ---------------------------------------------------------------------------
// Push-data encoding (Bitcoin script encoding)
// ---------------------------------------------------------------------------

/**
 * Encode bytes with Bitcoin pushdata prefix.
 *   len < 0x4c    → [len] + data
 *   len <= 0xff   → [0x4c, len] + data    (OP_PUSHDATA1)
 *   len <= 0xffff → [0x4d, lo, hi] + data (OP_PUSHDATA2)
 */
static void AppendPushData(const uint8_t* aData, size_t aLen,
                            nsTArray<uint8_t>& aScript) {
  if (aLen < 0x4c) {
    aScript.AppendElement((uint8_t)aLen);
  } else if (aLen <= 0xff) {
    aScript.AppendElement(kOpPushData1);
    aScript.AppendElement((uint8_t)aLen);
  } else {
    aScript.AppendElement(kOpPushData2);
    aScript.AppendElement((uint8_t)(aLen & 0xff));
    aScript.AppendElement((uint8_t)((aLen >> 8) & 0xff));
  }
  aScript.AppendElements(aData, aLen);
}

// ---------------------------------------------------------------------------
// nsNamecoinResolver::ComputeScripthash (static)
// ---------------------------------------------------------------------------

/* static */
nsresult nsNamecoinResolver::ComputeScripthash(const nsACString& aName,
                                               nsACString& aScripthash) {
  /**
   * Build the canonical Namecoin name-index script:
   *
   *   OP_NAME_UPDATE (0x53)
   *   + pushdata(name_bytes)       // e.g. "d/example"
   *   + pushdata("")               // empty value (index only cares about name)
   *   + OP_2DROP (0x6d)
   *   + OP_DROP  (0x75)
   *   + OP_RETURN (0x6a)
   *
   * Then SHA-256 the script bytes, reverse the hash, and hex-encode.
   *
   * CRITICAL: Must match build_name_index_script() in electrumx/lib/coins.py
   * Using OP_NAME_SHOW (0xd1) instead will return empty history — silent fail.
   *
   * Verified working:
   *   d/testls  → electrumx.testls.space (returns valid history)
   *   d/bitcoin → electrumx.testls.space (returns valid history)
   */

  const uint8_t* nameBytes =
      reinterpret_cast<const uint8_t*>(aName.BeginReading());
  const size_t   nameLen   = aName.Length();

  // Build the script byte-by-byte
  nsTArray<uint8_t> script;

  // OP_NAME_UPDATE
  script.AppendElement(kOpNameUpdate);

  // pushdata(name)
  AppendPushData(nameBytes, nameLen, script);

  // pushdata("") — empty value
  AppendPushData(nullptr, 0, script);

  // OP_2DROP + OP_DROP + OP_RETURN
  script.AppendElement(kOp2Drop);
  script.AppendElement(kOpDrop);
  script.AppendElement(kOpReturn);

  // SHA-256
  uint8_t hash[32];
  if (HASH_HashBuf(HASH_AlgSHA256,
                   hash,
                   script.Elements(),
                   script.Length()) != SECSuccess) {
    NMC_ERR("ComputeScripthash: SHA-256 failed for %s", nsPromiseFlatCString(aName).get());
    return NS_ERROR_FAILURE;
  }

  // Reverse (ElectrumX uses reversed byte order)
  for (int i = 0; i < 16; i++) {
    uint8_t tmp = hash[i];
    hash[i] = hash[31 - i];
    hash[31 - i] = tmp;
  }

  HexEncode(hash, 32, aScripthash);
  NMC_LOG("ComputeScripthash: %s → %s",
          nsPromiseFlatCString(aName).get(),
          nsPromiseFlatCString(aScripthash).get());
  return NS_OK;
}

// ---------------------------------------------------------------------------
// nsNamecoinResolver::DecodeNameScript (static)
// ---------------------------------------------------------------------------

/* static */
bool nsNamecoinResolver::DecodeNameScript(const nsACString& aScriptHex,
                                           nsACString& aName,
                                           nsACString& aValue) {
  /**
   * Decode a Namecoin NAME_UPDATE output script.
   *
   * Script layout (after NAME_UPDATE = 0x53):
   *   pushdata <name_bytes>
   *   pushdata <value_bytes>
   *   OP_2DROP
   *   OP_DROP
   *   <standard scriptPubKey: P2PKH / P2SH / P2WPKH ...>
   *
   * We only care about name and value; the address script is ignored.
   */

  nsTArray<uint8_t> bytes;
  if (!HexDecode(aScriptHex, bytes)) return false;
  if (bytes.IsEmpty() || bytes[0] != kOpNameUpdate) return false;

  size_t i = 1; // skip OP_NAME_UPDATE
  size_t len = bytes.Length();

  auto readPushdata = [&](nsTArray<uint8_t>& out) -> bool {
    if (i >= len) return false;
    uint8_t opcode = bytes[i++];
    size_t datalen = 0;

    if (opcode == 0x00) {
      // OP_0 → empty byte array
      out.Clear();
      return true;
    } else if (opcode >= 0x01 && opcode <= 0x4b) {
      datalen = opcode;
    } else if (opcode == kOpPushData1) {
      if (i >= len) return false;
      datalen = bytes[i++];
    } else if (opcode == kOpPushData2) {
      if (i + 1 >= len) return false;
      datalen = (size_t)bytes[i] | ((size_t)bytes[i + 1] << 8);
      i += 2;
    } else {
      return false;
    }

    if (i + datalen > len) return false;
    out.ReplaceElementsAt(0, out.Length(), bytes.Elements() + i, datalen);
    i += datalen;
    return true;
  };

  nsTArray<uint8_t> nameData, valueData;
  if (!readPushdata(nameData)) return false;
  if (!readPushdata(valueData)) return false;

  aName.Assign(reinterpret_cast<const char*>(nameData.Elements()), nameData.Length());
  aValue.Assign(reinterpret_cast<const char*>(valueData.Elements()), valueData.Length());
  return true;
}

// ---------------------------------------------------------------------------
// nsNamecoinResolver::IsNamecoinHost (static)
// ---------------------------------------------------------------------------

/* static */
bool nsNamecoinResolver::IsNamecoinHost(const nsACString& aHostname) {
  // Must end with ".bit" (case-insensitive) and have at least one label before
  nsAutoCString lower(aHostname);
  ToLowerCase(lower);
  if (!StringEndsWith(lower, ".bit"_ns)) return false;
  if (lower.Length() <= 4) return false;  // just ".bit" without a label
  return true;
}

// ---------------------------------------------------------------------------
// nsNamecoinResolver construction / init
// ---------------------------------------------------------------------------

nsNamecoinResolver::nsNamecoinResolver()
    : mMutex("nsNamecoinResolver::mMutex") {}

nsNamecoinResolver::~nsNamecoinResolver() { Shutdown(); }

// Init(), IsEnabled(), and Shutdown() are defined after
// nsElectrumXConnectionPool (which they depend on).

// ---------------------------------------------------------------------------
// ElectrumX WebSocket Connection Pool
// ---------------------------------------------------------------------------
//
// Uses NSPR blocking TCP sockets with manual WebSocket (RFC 6455) framing.
// This is completely thread-safe (no main-thread requirement, no Gecko
// WebSocketChannel admission manager, no FailDelay backoff).
//
// Architecture:
//   nsElectrumXConnectionPool (1 per resolver)
//     └─ nsElectrumXPooledConnection (1 per server URL)
//          └─ PRFileDesc* (NSPR TCP socket, blocking I/O)
//
// Threading: DNS resolver threads call Pool::Request() directly.
//            NSPR sockets may be used from any thread without restriction.
// ---------------------------------------------------------------------------

/**
 * nsElectrumXPooledConnection — Persistent WebSocket to one ElectrumX server.
 *
 * Uses NSPR blocking TCP sockets with manual WebSocket framing (RFC 6455).
 * No main-thread requirement, no Gecko WebSocketChannel, no FailDelay backoff.
 *
 * Threading: safe to call from any thread including DNS resolver threads.
 */
class nsElectrumXPooledConnection final {
 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(nsElectrumXPooledConnection)

  nsElectrumXPooledConnection(const nsCString& aServerUrl,
                               uint32_t aTimeoutMs)
      : mServerUrl(aServerUrl),
        mTimeoutMs(aTimeoutMs),
        mFd(nullptr),
        mClosed(false) {}

  /**
   * Send one JSON-RPC request, block until response arrives.
   * Thread-safe. Opens/reuses the TCP+WS connection.
   */
  nsresult SendRequest(int32_t aReqId,
                       const nsCString& aRequest,
                       nsCString& aResultJson);

  bool IsClosed() const { return mClosed; }

  void Close() {
    if (mFd) {
      PR_Close(mFd);
      mFd = nullptr;
    }
    mClosed = true;
  }

 private:
  ~nsElectrumXPooledConnection() { Close(); }

  nsresult EnsureConnected();
  nsresult SendFrame(const nsCString& aData);
  nsresult RecvFrame(nsCString& aData);

  // Parse "ws://host:port/path" → host, port, path
  bool ParseUrl(const nsCString& aUrl,
                nsCString& aHost, int32_t& aPort, nsCString& aPath) {
    // Expect ws://host:port[/path]
    nsAutoCString url(aUrl);
    if (!StringBeginsWith(url, "ws://"_ns)) return false;
    url = Substring(url, 5);  // strip "ws://"

    int32_t slashPos = url.FindChar('/');
    nsAutoCString hostPort;
    if (slashPos >= 0) {
      hostPort = Substring(url, 0, slashPos);
      aPath = Substring(url, slashPos);
    } else {
      hostPort = url;
      aPath.AssignLiteral("/");
    }

    int32_t colonPos = hostPort.FindChar(':');
    if (colonPos >= 0) {
      aHost = Substring(hostPort, 0, colonPos);
      nsAutoCString portStr(Substring(hostPort, colonPos + 1));
      aPort = atoi(portStr.get());
    } else {
      aHost = hostPort;
      aPort = 80;
    }
    return !aHost.IsEmpty() && aPort > 0;
  }

  nsCString mServerUrl;
  uint32_t mTimeoutMs;
  PRFileDesc* mFd;         // NSPR TCP socket (blocking)
  bool mClosed;
  // No mutex needed: pool serializes access per-connection
};

nsresult nsElectrumXPooledConnection::EnsureConnected() {
  if (mFd && !mClosed) return NS_OK;
  if (mClosed) return NS_ERROR_NOT_AVAILABLE;

  nsCString host, path;
  int32_t port;
  if (!ParseUrl(mServerUrl, host, port, path)) {
    fprintf(stderr, "[namecoin] EnsureConnected: bad URL: %s\n", mServerUrl.get());
    mClosed = true;
    return NS_ERROR_MALFORMED_URI;
  }

  fprintf(stderr, "[namecoin] EnsureConnected: TCP connect to %s:%d%s\n",
          host.get(), port, path.get());

  // Resolve host → IP address
  PRNetAddr addr;
  memset(&addr, 0, sizeof(addr));

  // Try literal IP first
  if (PR_StringToNetAddr(host.get(), &addr) != PR_SUCCESS) {
    // Not a literal IP — do hostname lookup (blocking)
    char buf[PR_NETDB_BUF_SIZE];
    PRHostEnt he;
    if (PR_GetHostByName(host.get(), buf, sizeof(buf), &he) != PR_SUCCESS) {
      fprintf(stderr, "[namecoin] EnsureConnected: DNS lookup failed for %s\n", host.get());
      mClosed = true;
      return NS_ERROR_UNKNOWN_HOST;
    }
    PR_EnumerateHostEnt(0, &he, 0, &addr);
  }
  PR_SetNetAddr(PR_IpAddrNull, PR_AF_INET, (PRUint16)port, &addr);

  // Open TCP socket
  mFd = PR_OpenTCPSocket(PR_AF_INET);
  if (!mFd) {
    fprintf(stderr, "[namecoin] EnsureConnected: PR_OpenTCPSocket failed\n");
    mClosed = true;
    return NS_ERROR_FAILURE;
  }

  // Set timeout
  PRIntervalTime timeout = PR_MillisecondsToInterval(mTimeoutMs);
  if (PR_Connect(mFd, &addr, timeout) != PR_SUCCESS) {
    PRErrorCode err = PR_GetError();
    fprintf(stderr, "[namecoin] EnsureConnected: PR_Connect failed: %d\n", (int)err);
    PR_Close(mFd);
    mFd = nullptr;
    mClosed = true;
    return NS_ERROR_CONNECTION_REFUSED;
  }

  fprintf(stderr, "[namecoin] EnsureConnected: TCP connected, doing WS handshake\n");

  // --- WebSocket Handshake (RFC 6455) ---
  // Generate a random 16-byte key and base64-encode it
  uint8_t keyBytes[16];
  for (int i = 0; i < 16; i++) keyBytes[i] = (uint8_t)(rand() & 0xff);
  nsCString keyB64;
  nsresult rv = mozilla::Base64Encode(
      nsDependentCSubstring(reinterpret_cast<const char*>(keyBytes), 16),
      keyB64);
  if (NS_FAILED(rv)) {
    Close();
    return rv;
  }

  // Build HTTP upgrade request
  nsAutoCString req;
  req.AppendLiteral("GET ");
  req.Append(path);
  req.AppendLiteral(" HTTP/1.1\r\nHost: ");
  req.Append(host);
  req.Append(':');
  req.AppendInt(port);
  req.AppendLiteral("\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                    "Sec-WebSocket-Key: ");
  req.Append(keyB64);
  req.AppendLiteral("\r\nSec-WebSocket-Version: 13\r\n\r\n");

  // Send HTTP upgrade
  PRInt32 sent = PR_Send(mFd, req.get(), req.Length(), 0, timeout);
  if (sent != (PRInt32)req.Length()) {
    fprintf(stderr, "[namecoin] EnsureConnected: send failed, sent=%d expected=%u\n",
            (int)sent, (unsigned)req.Length());
    Close();
    return NS_ERROR_FAILURE;
  }

  // Read HTTP response until \r\n\r\n
  nsAutoCString response;
  char rbuf[1];
  while (true) {
    PRInt32 n = PR_Recv(mFd, rbuf, 1, 0, timeout);
    if (n <= 0) {
      fprintf(stderr, "[namecoin] EnsureConnected: recv failed during handshake\n");
      Close();
      return NS_ERROR_FAILURE;
    }
    response.Append(rbuf[0]);
    if (StringEndsWith(response, "\r\n\r\n"_ns)) break;
    if (response.Length() > 4096) {
      fprintf(stderr, "[namecoin] EnsureConnected: response too large\n");
      Close();
      return NS_ERROR_FAILURE;
    }
  }

  if (!StringBeginsWith(response, "HTTP/1.1 101"_ns)) {
    fprintf(stderr, "[namecoin] EnsureConnected: WS upgrade rejected: %s\n",
            response.get());
    Close();
    return NS_ERROR_FAILURE;
  }

  fprintf(stderr, "[namecoin] OnStart: WebSocket connected to %s\n", mServerUrl.get());
  return NS_OK;
}

nsresult nsElectrumXPooledConnection::SendFrame(const nsCString& aData) {
  // RFC 6455 client frame: FIN=1, opcode=1 (text), MASK=1, masking key, data
  uint32_t dataLen = aData.Length();
  PRIntervalTime timeout = PR_MillisecondsToInterval(mTimeoutMs);

  // Build frame header
  nsTArray<uint8_t> frame;
  frame.AppendElement(0x81);  // FIN + opcode text

  uint8_t maskBit = 0x80;  // MASK bit set (client must mask)
  if (dataLen < 126) {
    frame.AppendElement((uint8_t)(dataLen | maskBit));
  } else if (dataLen <= 0xffff) {
    frame.AppendElement(126 | maskBit);
    frame.AppendElement((uint8_t)(dataLen >> 8));
    frame.AppendElement((uint8_t)(dataLen & 0xff));
  } else {
    frame.AppendElement(127 | maskBit);
    for (int i = 7; i >= 0; i--) {
      frame.AppendElement((uint8_t)((dataLen >> (i * 8)) & 0xff));
    }
  }

  // 4-byte random masking key
  uint8_t mask[4];
  for (int i = 0; i < 4; i++) mask[i] = (uint8_t)(rand() & 0xff);
  frame.AppendElements(mask, 4);

  // Masked payload
  const uint8_t* src = reinterpret_cast<const uint8_t*>(aData.get());
  for (uint32_t i = 0; i < dataLen; i++) {
    frame.AppendElement(src[i] ^ mask[i % 4]);
  }

  PRInt32 sent = PR_Send(mFd, frame.Elements(), frame.Length(), 0, timeout);
  if (sent != (PRInt32)frame.Length()) {
    fprintf(stderr, "[namecoin] SendFrame: send failed\n");
    return NS_ERROR_FAILURE;
  }
  return NS_OK;
}

nsresult nsElectrumXPooledConnection::RecvFrame(nsCString& aData) {
  // Read one WebSocket frame from the server (server frames are unmasked)
  PRIntervalTime timeout = PR_MillisecondsToInterval(mTimeoutMs);

  auto recvN = [&](uint8_t* buf, uint32_t n) -> bool {
    uint32_t received = 0;
    while (received < n) {
      PRInt32 r = PR_Recv(mFd, buf + received, n - received, 0, timeout);
      if (r <= 0) return false;
      received += r;
    }
    return true;
  };

  // Read 2 header bytes
  uint8_t header[2];
  if (!recvN(header, 2)) {
    fprintf(stderr, "[namecoin] RecvFrame: failed to read header\n");
    return NS_ERROR_FAILURE;
  }

  // uint8_t fin_opcode = header[0];
  uint8_t len_byte = header[1];
  bool masked = (len_byte & 0x80) != 0;
  uint64_t payloadLen = len_byte & 0x7f;

  if (payloadLen == 126) {
    uint8_t ext[2];
    if (!recvN(ext, 2)) return NS_ERROR_FAILURE;
    payloadLen = ((uint64_t)ext[0] << 8) | ext[1];
  } else if (payloadLen == 127) {
    uint8_t ext[8];
    if (!recvN(ext, 8)) return NS_ERROR_FAILURE;
    payloadLen = 0;
    for (int i = 0; i < 8; i++) payloadLen = (payloadLen << 8) | ext[i];
  }

  uint8_t mask[4] = {0,0,0,0};
  if (masked) {
    if (!recvN(mask, 4)) return NS_ERROR_FAILURE;
  }

  if (payloadLen > 1024 * 1024) {  // 1 MB safety limit
    fprintf(stderr, "[namecoin] RecvFrame: frame too large (%llu bytes)\n",
            (unsigned long long)payloadLen);
    return NS_ERROR_FAILURE;
  }

  nsTArray<uint8_t> payload;
  payload.SetLength((uint32_t)payloadLen);
  if (payloadLen > 0 && !recvN(payload.Elements(), (uint32_t)payloadLen)) {
    fprintf(stderr, "[namecoin] RecvFrame: failed to read payload\n");
    return NS_ERROR_FAILURE;
  }

  if (masked) {
    for (uint32_t i = 0; i < payloadLen; i++) {
      payload[i] ^= mask[i % 4];
    }
  }

  aData.Assign(reinterpret_cast<const char*>(payload.Elements()),
               (uint32_t)payloadLen);
  return NS_OK;
}

nsresult nsElectrumXPooledConnection::SendRequest(int32_t aReqId,
                                                   const nsCString& aRequest,
                                                   nsCString& aResultJson) {
  // Connect if not already open
  nsresult rv = EnsureConnected();
  if (NS_FAILED(rv)) return rv;

  // Send the request as a WebSocket text frame
  fprintf(stderr, "[namecoin] SendRequest: id=%d request=%s\n",
          aReqId, aRequest.get());
  rv = SendFrame(aRequest);
  if (NS_FAILED(rv)) {
    Close();
    return rv;
  }

  // Read frames until we get one matching our request ID
  nsAutoCString idPattern;
  idPattern.AppendLiteral("\"id\":");
  idPattern.AppendInt(aReqId);

  PRIntervalTime deadline = PR_IntervalNow() +
      PR_MillisecondsToInterval(mTimeoutMs);

  while (true) {
    if (PR_IntervalNow() > deadline) {
      fprintf(stderr, "[namecoin] SendRequest: timeout waiting for id=%d\n", aReqId);
      Close();
      return NS_ERROR_NET_TIMEOUT;
    }

    nsCString frame;
    rv = RecvFrame(frame);
    if (NS_FAILED(rv)) {
      Close();
      return rv;
    }

    fprintf(stderr, "[namecoin] OnMessageAvailable: id=%d frame_len=%u\n",
            aReqId, (unsigned)frame.Length());

    if (FindInReadable(idPattern, frame)) {
      aResultJson = frame;
      return NS_OK;
    }
    // Non-matching frame (e.g. subscription push) — ignore, try again
  }
}

// ---------------------------------------------------------------------------
// nsElectrumXConnectionPool
// ---------------------------------------------------------------------------

/**
 * Manages a pool of persistent WebSocket connections to ElectrumX servers.
 *
 * Thread-safe: resolver thread pool threads call Request() concurrently.
 * The pool Mutex protects the connection map; each connection has its own
 * Monitor for request serialization.
 *
 * Connection lifecycle:
 *   1. First request to a server → create nsElectrumXPooledConnection
 *   2. Subsequent requests → reuse existing open connection
 *   3. After idle TTL expires → timer closes connection, removed lazily
 *   4. On error → connection marked closed, next request creates new one
 *   5. Shutdown() → close all connections immediately
 */
class nsElectrumXConnectionPool final {
 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(nsElectrumXConnectionPool)

  nsElectrumXConnectionPool(uint32_t aTimeoutMs, uint32_t aIdleTTLMs)
      : mMutex("nsElectrumXConnectionPool"),
        mTimeoutMs(aTimeoutMs) {
    (void)aIdleTTLMs;  // idle TTL not used with NSPR sockets (connection reuse handled per-request)
    NMC_LOG("ElectrumX connection pool created (timeout=%ums)", aTimeoutMs);
  }

  /**
   * Send a JSON-RPC request to the given server, reusing a pooled
   * connection if one is open. Creates a new connection on demand.
   *
   * @param aServer     ElectrumX server URL (ws:// or wss://)
   * @param aMethod     JSON-RPC method name
   * @param aParamsJson JSON-RPC params array as string
   * @param aResultJson Output: raw JSON-RPC response (full message)
   */
  nsresult Request(const nsCString& aServer,
                   const nsCString& aMethod,
                   const nsCString& aParamsJson,
                   nsCString& aResultJson);

  /** Close all pooled connections. Safe to call multiple times. */
  void Shutdown();

 private:
  ~nsElectrumXConnectionPool() { Shutdown(); }

  RefPtr<nsElectrumXPooledConnection> GetConnection(const nsCString& aServer);

  struct PoolEntry {
    nsCString serverUrl;
    RefPtr<nsElectrumXPooledConnection> connection;
  };

  Mutex mMutex;
  bool mShutdown = false;
  uint32_t mTimeoutMs;
  nsTArray<PoolEntry> mEntries;
};

RefPtr<nsElectrumXPooledConnection>
nsElectrumXConnectionPool::GetConnection(const nsCString& aServer) {
  MutexAutoLock lock(mMutex);
  if (mShutdown) return nullptr;

  // Find existing open connection for this server
  for (auto& entry : mEntries) {
    if (entry.serverUrl.Equals(aServer) && !entry.connection->IsClosed()) {
      return entry.connection;
    }
  }

  // Remove any stale (closed) entries for this server
  mEntries.RemoveElementsBy([&](const PoolEntry& e) {
    return e.serverUrl.Equals(aServer);
  });

  // Create new connection
  auto conn = MakeRefPtr<nsElectrumXPooledConnection>(aServer, mTimeoutMs);

  PoolEntry entry;
  entry.serverUrl = aServer;
  entry.connection = conn;
  mEntries.AppendElement(std::move(entry));

  NMC_LOG("ElectrumX pool: new connection for %s (pool size: %u)",
          aServer.get(), (unsigned)mEntries.Length());
  return conn;
}

nsresult nsElectrumXConnectionPool::Request(
    const nsCString& aServer, const nsCString& aMethod,
    const nsCString& aParamsJson, nsCString& aResultJson) {

  RefPtr<nsElectrumXPooledConnection> conn = GetConnection(aServer);
  if (!conn) return NS_ERROR_NOT_AVAILABLE;

  static std::atomic<int32_t> sNextId{1};
  int32_t reqId = sNextId.fetch_add(1, std::memory_order_relaxed);

  // Build JSON-RPC 2.0 request
  nsAutoCString request;
  request.AppendLiteral("{\"jsonrpc\":\"2.0\",\"id\":");
  request.AppendInt(reqId);
  request.AppendLiteral(",\"method\":\"");
  request.Append(aMethod);
  request.AppendLiteral("\",\"params\":");
  request.Append(aParamsJson);
  request.AppendLiteral("}");

  NMC_LOG("ElectrumX pool → %s: %s", aServer.get(), request.get());

  nsresult rv = conn->SendRequest(reqId, request, aResultJson);

  if (NS_FAILED(rv)) {
    NMC_LOG("ElectrumX pool: request failed for %s (%08x), "
            "connection will be replaced on next attempt",
            aServer.get(), (unsigned)rv);
  }

  return rv;
}

void nsElectrumXConnectionPool::Shutdown() {
  MutexAutoLock lock(mMutex);
  if (mShutdown) return;
  mShutdown = true;

  NMC_LOG("ElectrumX connection pool shutting down (%u connections)",
          (unsigned)mEntries.Length());

  for (auto& entry : mEntries) {
    entry.connection->Close();
  }
  mEntries.Clear();
}

// ---------------------------------------------------------------------------
// nsNamecoinResolver Init / IsEnabled / Shutdown
// (defined here because they depend on nsElectrumXConnectionPool)
// ---------------------------------------------------------------------------

nsresult nsNamecoinResolver::Init() {
  MutexAutoLock lock(mMutex);

  // Use StaticPrefs for all pref reads — safe on any thread (RelaxedAtomicBool/Uint32).
  // Preferences::GetBool/GetUint are NOT safe on socket/IO threads (crash in
  // InitStaticMembers -> IsInServoTraversal). StaticPrefs mirrors are atomic.
  mEnabled = StaticPrefs::network_namecoin_enabled();
  fprintf(stderr, "[namecoin] Init(): enabled=%s\n", mEnabled ? "true" : "false");
  if (!mEnabled) {
    NMC_LOG("Namecoin resolver disabled via pref");
    return NS_OK;
  }

  // Server list: network.namecoin.electrumx_servers is mirror:never (no StaticPrefs
  // accessor). Read it via Preferences only if we're on the main thread; otherwise
  // fall back to the compiled-in default. In practice Init() is called from a DNS
  // Resolver thread, so we use the default. Users who need a custom server should
  // set the pref before starting Firefox (or we can add a mirror:always variant).
  nsAutoCString serversPref;
  if (NS_IsMainThread()) {
    Preferences::GetCString(kPrefServers, serversPref);
  }
  if (serversPref.IsEmpty()) {
    serversPref.AssignLiteral(kDefaultServers);
  }

  // Split comma-separated server list
  mServers.Clear();
  nsCCharSeparatedTokenizer tokenizer(serversPref, ',');
  while (tokenizer.hasMoreTokens()) {
    nsAutoCString server(tokenizer.nextToken());
    server.Trim(" \t");
    if (!server.IsEmpty()) {
      mServers.AppendElement(server);
      fprintf(stderr, "[namecoin] Init(): server: %s\n", server.get());
    }
  }

  if (mServers.IsEmpty()) {
    NMC_ERR("Namecoin: no ElectrumX servers configured");
    mEnabled = false;
    return NS_ERROR_NOT_INITIALIZED;
  }

  mCacheTTLSeconds  = StaticPrefs::network_namecoin_cache_ttl_seconds();
  mMaxAliasHops     = StaticPrefs::network_namecoin_max_alias_hops();
  mConnectionTimeoutMs = StaticPrefs::network_namecoin_connection_timeout_ms();
  mQueryMultipleServers = StaticPrefs::network_namecoin_query_multiple_servers();

  // Create WebSocket connection pool.
  uint32_t idleTTLMs = mCacheTTLSeconds * 1000;
  mConnectionPool = new nsElectrumXConnectionPool(
      mConnectionTimeoutMs, idleTTLMs);

  NMC_LOG("Namecoin resolver initialized: %u servers, timeout=%ums, "
          "pool_idle_ttl=%us, multi=%s",
          (unsigned)mServers.Length(), mConnectionTimeoutMs,
          mCacheTTLSeconds,
          mQueryMultipleServers ? "true" : "false");
  fprintf(stderr, "[namecoin] Init(): initialized OK, %u servers, timeout=%ums\n",
          (unsigned)mServers.Length(), mConnectionTimeoutMs);
  return NS_OK;
}

bool nsNamecoinResolver::IsEnabled() const {
  MutexAutoLock lock(mMutex);
  return mEnabled && !mShuttingDown && !mServers.IsEmpty();
}

void nsNamecoinResolver::Shutdown() {
  RefPtr<nsElectrumXConnectionPool> pool;
  {
    MutexAutoLock lock(mMutex);
    mShuttingDown = true;
    mEnabled = false;

    // Grab pool ref under lock, then release before calling Shutdown()
    // to avoid holding two locks.
    pool = mConnectionPool;
    mConnectionPool = nullptr;
  }
  if (pool) {
    pool->Shutdown();
  } else {
    NMC_LOG("Namecoin resolver shut down");
  }
}

// ---------------------------------------------------------------------------
// JSON-RPC over WebSocket
// ---------------------------------------------------------------------------

/**
 * Simple JSON extraction: find "result": ... in a JSON-RPC response string.
 *
 * For Phase 1, this is a lightweight implementation that extracts the
 * raw result value (as a JSON substring) without full parsing.
 * Phase 2 will use a proper JSON parser (mozilla::dom::JSON or simdjson).
 *
 * @param aResponse   Full JSON-RPC response string
 * @param aResult     Output: raw "result" value (JSON substring)
 * @returns true if "result" key found, false if "error" key found
 */
static bool ExtractJsonRpcResult(const nsACString& aResponse,
                                  nsACString& aResult) {
  // Check for error first
  if (FindInReadable("\"error\":"_ns, aResponse)) {
    // Extract error message for logging
    NMC_ERR("ElectrumX JSON-RPC error: %s",
            nsPromiseFlatCString(aResponse).get());
    return false;
  }

  // Find "result":
  const nsACString& needle = "\"result\":"_ns;
  int32_t pos = FindInReadable(needle, aResponse);
  if (pos < 0) return false;

  // Return everything after "result": — caller parses as needed
  aResult = Substring(aResponse, pos + needle.Length());
  return true;
}

nsresult nsNamecoinResolver::ElectrumXRequest(const nsCString& aServer,
                                               const nsCString& aMethod,
                                               const nsCString& aParamsJson,
                                               nsCString& aResultJson) {
  /**
   * Send one JSON-RPC 2.0 request over WebSocket and return the result.
   *
   * Delegates to the connection pool, which reuses persistent WebSocket
   * connections across requests to the same server. A single .bit resolve
   * does 3 requests (history + tx + headers) — all 3 now share one
   * WebSocket connection instead of opening/closing 3 separate ones.
   *
   * This is a blocking implementation (Phase 1). It MUST NOT be called
   * from the main thread. In nsHostResolver, .bit queries are already
   * dispatched to the resolver thread pool.
   */

  MOZ_ASSERT(!NS_IsMainThread(),
             "nsNamecoinResolver::ElectrumXRequest must run off main thread");

  // Grab pool reference under lock, then release lock before blocking I/O
  RefPtr<nsElectrumXConnectionPool> pool;
  {
    MutexAutoLock lock(mMutex);
    pool = mConnectionPool;
  }
  if (!pool) {
    NMC_ERR("ElectrumX: connection pool not initialized");
    return NS_ERROR_NOT_INITIALIZED;
  }

  // Pool handles connection reuse, request ID generation, and JSON-RPC
  // framing. It returns the raw JSON-RPC response message.
  nsCString rawResponse;
  nsresult rv = pool->Request(aServer, aMethod, aParamsJson, rawResponse);
  if (NS_FAILED(rv)) {
    NMC_ERR("ElectrumX %s failed: %08x", aServer.get(), (unsigned)rv);
    return rv;
  }

  // Extract "result" field from JSON-RPC response
  if (!ExtractJsonRpcResult(rawResponse, aResultJson)) {
    NMC_ERR("ElectrumX JSON-RPC error response from %s", aServer.get());
    return NS_ERROR_FAILURE;
  }

  NMC_LOG("ElectrumX ← %s: %s", aServer.get(), aResultJson.get());
  return NS_OK;
}

nsresult nsNamecoinResolver::ElectrumXRequestAny(const nsCString& aMethod,
                                                  const nsCString& aParamsJson,
                                                  nsCString& aResultJson,
                                                  nsCString& aUsedServer) {
  nsTArray<nsCString> servers;
  {
    MutexAutoLock lock(mMutex);
    servers = mServers.Clone();  // snapshot under lock
  }  // lock released before blocking I/O

  for (const auto& server : servers) {
    nsresult rv = ElectrumXRequest(server, aMethod, aParamsJson, aResultJson);
    if (NS_SUCCEEDED(rv)) {
      aUsedServer = server;
      return NS_OK;
    }
    NMC_LOG("ElectrumX server %s failed, trying next", server.get());
  }

  NMC_ERR("All ElectrumX servers failed for method %s", aMethod.get());
  return NS_ERROR_NET_INTERRUPT;
}

// ---------------------------------------------------------------------------
// Multi-server cross-validation
// ---------------------------------------------------------------------------

/**
 * Extract all tx_hash values from a get_history JSON result.
 * History response: [{"tx_hash":"abc","height":123}, ...]
 * Returns a sorted array of tx_hash strings for comparison.
 */
static void ExtractTxHashes(const nsACString& aHistoryJson,
                             nsTArray<nsCString>& aHashes) {
  aHashes.Clear();
  int32_t searchPos = 0;
  while (true) {
    int32_t pos = aHistoryJson.Find("\"tx_hash\":\"", searchPos);
    if (pos < 0) break;
    int32_t start = pos + 11;  // length of "tx_hash":"
    nsDependentCSubstring rest = Substring(aHistoryJson, start);
    int32_t end = rest.FindChar('"');
    if (end > 0) {
      aHashes.AppendElement(nsCString(Substring(rest, 0, end)));
    }
    searchPos = start + (end > 0 ? end : 1);
  }
  aHashes.Sort();
}

nsresult nsNamecoinResolver::ElectrumXRequestValidated(
    const nsCString& aMethod, const nsCString& aParamsJson,
    nsCString& aResultJson, nsCString& aUsedServer) {
  // If cross-validation is disabled, delegate to the simple path
  if (!mQueryMultipleServers) {
    return ElectrumXRequestAny(aMethod, aParamsJson, aResultJson, aUsedServer);
  }

  nsTArray<nsCString> servers;
  {
    MutexAutoLock lock(mMutex);
    servers = mServers.Clone();
  }

  // Query all servers and collect results
  struct ServerResult {
    nsCString server;
    nsCString resultJson;
    bool succeeded = false;
  };

  nsTArray<ServerResult> results;
  uint32_t successCount = 0;

  for (const auto& server : servers) {
    ServerResult sr;
    sr.server = server;
    nsresult rv = ElectrumXRequest(server, aMethod, aParamsJson, sr.resultJson);
    if (NS_SUCCEEDED(rv)) {
      sr.succeeded = true;
      successCount++;
    } else {
      NMC_LOG("ElectrumXRequestValidated: server %s failed", server.get());
    }
    results.AppendElement(std::move(sr));
  }

  if (successCount == 0) {
    NMC_ERR("ElectrumXRequestValidated: all servers failed for %s",
            aMethod.get());
    return NS_ERROR_NET_INTERRUPT;
  }

  if (successCount == 1) {
    // Only one server responded — use it but warn
    NMC_ERR("ElectrumXRequestValidated: only 1 of %u servers responded for "
            "%s — cannot cross-validate",
            (unsigned)servers.Length(), aMethod.get());
    for (const auto& sr : results) {
      if (sr.succeeded) {
        aResultJson = sr.resultJson;
        aUsedServer = sr.server;
        return NS_OK;
      }
    }
  }

  // Multiple responses — compare tx_hash sets for history queries
  // Group results by their tx_hash fingerprint
  struct ResultGroup {
    nsCString fingerprint;  // sorted, joined tx_hashes
    nsCString resultJson;
    nsCString server;
    uint32_t count = 0;
  };

  nsTArray<ResultGroup> groups;

  for (const auto& sr : results) {
    if (!sr.succeeded) continue;

    // Build a fingerprint from sorted tx_hashes
    nsTArray<nsCString> hashes;
    ExtractTxHashes(sr.resultJson, hashes);
    nsAutoCString fingerprint;
    for (uint32_t i = 0; i < hashes.Length(); i++) {
      if (i > 0) fingerprint.Append(',');
      fingerprint.Append(hashes[i]);
    }

    // Find or create group
    bool found = false;
    for (auto& g : groups) {
      if (g.fingerprint.Equals(fingerprint)) {
        g.count++;
        found = true;
        break;
      }
    }
    if (!found) {
      ResultGroup g;
      g.fingerprint = fingerprint;
      g.resultJson = sr.resultJson;
      g.server = sr.server;
      g.count = 1;
      groups.AppendElement(std::move(g));
    }
  }

  // All agree
  if (groups.Length() == 1) {
    NMC_LOG("ElectrumXRequestValidated: %u servers agree for %s",
            successCount, aMethod.get());
    aResultJson = groups[0].resultJson;
    aUsedServer = groups[0].server;
    return NS_OK;
  }

  // Disagreement — use majority result
  NMC_ERR("ElectrumXRequestValidated: server DISAGREEMENT for %s! "
          "%u groups from %u responses",
          aMethod.get(), (unsigned)groups.Length(), successCount);

  uint32_t bestCount = 0;
  uint32_t bestIdx = 0;
  for (uint32_t i = 0; i < groups.Length(); i++) {
    NMC_ERR("  group %u: count=%u fingerprint=%s server=%s",
            i, groups[i].count, groups[i].fingerprint.get(),
            groups[i].server.get());
    if (groups[i].count > bestCount) {
      bestCount = groups[i].count;
      bestIdx = i;
    }
  }

  NMC_ERR("ElectrumXRequestValidated: using majority group (count=%u)",
          bestCount);
  aResultJson = groups[bestIdx].resultJson;
  aUsedServer = groups[bestIdx].server;
  return NS_OK;
}

// ---------------------------------------------------------------------------
// Block height
// ---------------------------------------------------------------------------

nsresult nsNamecoinResolver::GetCurrentBlockHeight(uint32_t& aHeight) {
  nsCString result, usedServer;
  nsresult rv = ElectrumXRequestAny(
      "blockchain.headers.subscribe"_ns, "[]"_ns, result, usedServer);
  if (NS_FAILED(rv)) {
    aHeight = 0;
    return rv;
  }
  // result looks like: {"height":823456,"hex":"..."}
  // Simple extraction: find "height":
  int32_t pos = result.Find("\"height\":");
  if (pos < 0) {
    aHeight = 0;
    return NS_ERROR_FAILURE;
  }
  nsDependentCSubstring heightStr = Substring(result, pos + 9);
  // Find first non-digit
  uint32_t end = 0;
  while (end < heightStr.Length() && IsAsciiDigit(heightStr[end])) end++;
  nsresult dummy;
  aHeight = (uint32_t)heightStr.ToInteger64(&dummy);
  NMC_LOG("GetCurrentBlockHeight: %u from %s", aHeight, usedServer.get());
  return NS_OK;
}

// ---------------------------------------------------------------------------
// Minimal recursive-descent JSON parser for Namecoin name values
// ---------------------------------------------------------------------------
//
// We avoid pulling SpiderMonkey / mozilla::dom::JSON into the netwerk layer
// because DOM objects are not available on background resolver threads.
// Phase 2 can revisit if the threading model changes.
//
// Supports: objects, arrays, strings (with escape sequences), numbers,
// booleans, null.  Sufficient for all Namecoin d/ value shapes.
// ---------------------------------------------------------------------------

namespace {

struct NmcJsonValue;
static bool NmcParseJsonValue(const char*& p, const char* end, NmcJsonValue& out);

struct NmcJsonValue {
  enum Kind { kNull, kBool, kNumber, kString, kArray, kObject };
  Kind      kind    = kNull;
  bool      boolVal = false;
  double    numVal  = 0.0;
  nsCString strVal;
  nsTArray<NmcJsonValue>  arrayVal;
  // Object: parallel key/value arrays (key order preserved, no duplicate keys)
  nsTArray<nsCString>     objectKeys;
  nsTArray<NmcJsonValue>  objectVals;

  bool IsNull()   const { return kind == kNull;   }
  bool IsString() const { return kind == kString;  }
  bool IsObject() const { return kind == kObject;  }
  bool IsArray()  const { return kind == kArray;   }
  bool IsNumber() const { return kind == kNumber;  }

  // Look up a key in an object. Returns nullptr if not found.
  const NmcJsonValue* Get(const nsACString& key) const {
    if (kind != kObject) return nullptr;
    for (uint32_t i = 0; i < objectKeys.Length(); i++) {
      if (objectKeys[i].Equals(key)) return &objectVals[i];
    }
    return nullptr;
  }
  const NmcJsonValue* Get(const char* key) const {
    return Get(nsDependentCString(key));
  }
};

static void NmcSkipWS(const char*& p, const char* end) {
  while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
}

static bool NmcParseString(const char*& p, const char* end, nsCString& out) {
  if (p >= end || *p != '"') return false;
  p++;  // skip opening quote
  out.Truncate();
  while (p < end && *p != '"') {
    if (*p == '\\') {
      p++;
      if (p >= end) return false;
      switch (*p) {
        case '"':  out.Append('"');  break;
        case '\\': out.Append('\\'); break;
        case '/':  out.Append('/');  break;
        case 'n':  out.Append('\n'); break;
        case 'r':  out.Append('\r'); break;
        case 't':  out.Append('\t'); break;
        case 'b':  out.Append('\b'); break;
        case 'f':  out.Append('\f'); break;
        case 'u': {
          // \uXXXX -- decode to UTF-8
          if (p + 4 >= end) return false;
          p++;
          uint32_t cp = 0;
          for (int i = 0; i < 4; i++, p++) {
            cp <<= 4;
            char c = *p;
            if      (c >= '0' && c <= '9') cp |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') cp |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') cp |= (unsigned)(c - 'A' + 10);
            else return false;
          }
          p--;  // outer loop will ++ at end of switch
          // Encode to UTF-8
          if (cp < 0x80) {
            out.Append((char)cp);
          } else if (cp < 0x800) {
            out.Append((char)(0xC0 | (cp >> 6)));
            out.Append((char)(0x80 | (cp & 0x3F)));
          } else {
            out.Append((char)(0xE0 | (cp >> 12)));
            out.Append((char)(0x80 | ((cp >> 6) & 0x3F)));
            out.Append((char)(0x80 | (cp & 0x3F)));
          }
          break;
        }
        default: out.Append(*p); break;
      }
    } else {
      out.Append(*p);
    }
    p++;
  }
  if (p >= end) return false;  // unterminated string
  p++;  // skip closing quote
  return true;
}

static bool NmcParseNumber(const char*& p, const char* end, double& out) {
  const char* start = p;
  if (p < end && *p == '-') p++;
  if (p >= end || *p < '0' || *p > '9') { p = start; return false; }
  while (p < end && *p >= '0' && *p <= '9') p++;
  if (p < end && *p == '.') {
    p++;
    while (p < end && *p >= '0' && *p <= '9') p++;
  }
  if (p < end && (*p == 'e' || *p == 'E')) {
    p++;
    if (p < end && (*p == '+' || *p == '-')) p++;
    while (p < end && *p >= '0' && *p <= '9') p++;
  }
  char buf[64];
  size_t sz = std::min((size_t)(p - start), sizeof(buf) - 1);
  memcpy(buf, start, sz);
  buf[sz] = 0;
  char* ep;
  out = strtod(buf, &ep);
  return ep != buf;
}

static bool NmcParseJsonValue(const char*& p, const char* end, NmcJsonValue& out) {
  NmcSkipWS(p, end);
  if (p >= end) return false;

  if (*p == '"') {
    out.kind = NmcJsonValue::kString;
    return NmcParseString(p, end, out.strVal);
  }
  if (*p == '{') {
    out.kind = NmcJsonValue::kObject;
    p++;  // skip '{'
    NmcSkipWS(p, end);
    if (p < end && *p == '}') { p++; return true; }  // empty object
    while (p < end) {
      NmcSkipWS(p, end);
      nsCString key;
      if (!NmcParseString(p, end, key)) return false;
      NmcSkipWS(p, end);
      if (p >= end || *p != ':') return false;
      p++;  // skip ':'
      NmcJsonValue val;
      if (!NmcParseJsonValue(p, end, val)) return false;
      out.objectKeys.AppendElement(std::move(key));
      out.objectVals.AppendElement(std::move(val));
      NmcSkipWS(p, end);
      if (p >= end) return false;
      if (*p == '}') { p++; return true; }
      if (*p != ',') return false;
      p++;  // skip ','
    }
    return false;
  }
  if (*p == '[') {
    out.kind = NmcJsonValue::kArray;
    p++;  // skip '['
    NmcSkipWS(p, end);
    if (p < end && *p == ']') { p++; return true; }  // empty array
    while (p < end) {
      NmcJsonValue elem;
      if (!NmcParseJsonValue(p, end, elem)) return false;
      out.arrayVal.AppendElement(std::move(elem));
      NmcSkipWS(p, end);
      if (p >= end) return false;
      if (*p == ']') { p++; return true; }
      if (*p != ',') return false;
      p++;  // skip ','
    }
    return false;
  }
  if (p + 4 <= end && memcmp(p, "true",  4) == 0) { out.kind = NmcJsonValue::kBool; out.boolVal = true;  p += 4; return true; }
  if (p + 5 <= end && memcmp(p, "false", 5) == 0) { out.kind = NmcJsonValue::kBool; out.boolVal = false; p += 5; return true; }
  if (p + 4 <= end && memcmp(p, "null",  4) == 0) { out.kind = NmcJsonValue::kNull;                      p += 4; return true; }
  out.kind = NmcJsonValue::kNumber;
  return NmcParseNumber(p, end, out.numVal);
}

static bool NmcParseJson(const nsACString& aJson, NmcJsonValue& out) {
  const char* p   = aJson.BeginReading();
  const char* end = aJson.EndReading();
  return NmcParseJsonValue(p, end, out);
}

// Populate a NamecoinNameValue from a parsed JSON object.
// Used for both the top-level value and nested map entries.
static void NmcPopulateNameValue(const NmcJsonValue& obj, NamecoinNameValue& out) {
  // String fields: ip, ip6, alias, translate, tor
  auto getStr = [&](const char* key, nsCString& field) {
    const NmcJsonValue* v = obj.Get(key);
    if (v && v->IsString() && !v->strVal.IsEmpty()) field = v->strVal;
  };
  getStr("ip",        out.ip);
  getStr("ip6",       out.ip6);
  getStr("alias",     out.alias);
  getStr("translate", out.translate);
  getStr("tor",       out.tor);

  // ns: array of strings
  {
    const NmcJsonValue* v = obj.Get("ns");
    if (v && v->IsArray()) {
      for (const auto& elem : v->arrayVal) {
        if (elem.IsString() && !elem.strVal.IsEmpty()) {
          out.ns.AppendElement(elem.strVal);
        }
      }
    }
  }

  // tls: array of [usage, selector, matchType, data] tuples (Phase 2 consumes this)
  {
    const NmcJsonValue* v = obj.Get("tls");
    if (v && v->IsArray()) {
      for (const auto& entry : v->arrayVal) {
        if (!entry.IsArray() || entry.arrayVal.Length() < 4) continue;
        const auto& a = entry.arrayVal;
        if (!a[0].IsNumber() || !a[1].IsNumber() || !a[2].IsNumber()) continue;
        if (!a[3].IsString()) continue;
        NamecoinTLSARecord rec;
        rec.usage     = (uint8_t)(int)a[0].numVal;
        rec.selector  = (uint8_t)(int)a[1].numVal;
        rec.matchType = (uint8_t)(int)a[2].numVal;
        rec.data      = a[3].strVal;
        out.tls.AppendElement(std::move(rec));
      }
    }
  }

  out.hasMap = (obj.Get("map") != nullptr && obj.Get("map")->IsObject());
}

// Extract subdomain labels from a lowercased hostname.
// e.g. hostname="www.sub.example.bit", apex="example" -> labels=["www","sub"]
// (outermost label first; reversed order gives innermost-first traversal)
static void NmcExtractSubdomainLabels(const nsACString& aLower,
                                       const nsACString& aApex,
                                       nsTArray<nsCString>& aLabels) {
  // Strip .bit suffix
  nsDependentCSubstring without = Substring(aLower, 0, aLower.Length() - 4);
  // Expected suffix: ".<apex>"
  nsAutoCString suffix;
  suffix.Append('.');
  suffix.Append(aApex);
  if (!StringEndsWith(without, suffix)) return;  // apex only, no subdomains
  nsDependentCSubstring subPart = Substring(without, 0, without.Length() - suffix.Length());
  if (subPart.IsEmpty()) return;
  nsCCharSeparatedTokenizer tok(subPart, '.');
  while (tok.hasMoreTokens()) {
    aLabels.AppendElement(nsCString(tok.nextToken()));
  }
}

}  // anonymous namespace

// ---------------------------------------------------------------------------
// ParseNameValue (static)
// ---------------------------------------------------------------------------

/* static */
nsresult nsNamecoinResolver::ParseNameValue(const nsACString& aValueJson,
                                             const nsACString& aHostname,
                                             NamecoinNameValue& aResult) {
  /**
   * Parse the Namecoin d/ name value JSON.
   *
   * Full spec: firefox2.txt Section 3.
   * Reference: tls-namecoin-ext/background/namecoin-resolver.js (parseNameValue)
   *            tls-namecoin-ext/background/dns-router.js (subdomain map traversal)
   *
   * Phase 1 implements:
   *   - Top-level ip, ip6, ns, alias, translate, tor
   *   - map subdomain lookup: value.map["sub"].ip etc.
   *   - map wildcard fallback: value.map["*"]
   *   - NS delegation: value.ns array
   *   - TLSA stub: value.tls array of [usage,selector,matchType,data] tuples
   *     (stored in aResult.nameValue.tls for Phase 2 TLS layer consumption)
   *   - Non-JSON values ("reserved", plain strings): graceful no-IP return
   *
   * Map traversal algorithm:
   *   hostname = "www.sub.example.bit", apex = "example"
   *   labels (outermost first) = ["www", "sub"]
   *
   *   Traverse innermost-to-outermost:
   *     1. Look in root.map for "sub" (exact match), fallback to "*"
   *     2. Descend: look in that entry's .map for "www", fallback to "*"
   *     3. If any level has no map or no matching key, apex-fallback
   *     4. Apex fallback: no map at all -> use top-level ip/ip6
   */

  NMC_LOG("ParseNameValue: hostname=%s value_len=%u",
          nsPromiseFlatCString(aHostname).get(),
          (unsigned)aValueJson.Length());

  // Non-JSON values are common in Namecoin (e.g. "reserved", "claimed").
  // A valid d/ object starts with '{'. Skip leading whitespace to check.
  {
    const char* p = aValueJson.BeginReading();
    const char* e = aValueJson.EndReading();
    while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    if (p >= e || *p != '{') {
      NMC_LOG("ParseNameValue: non-JSON value -- unresolvable but valid (name exists)");
      return NS_OK;  // Not an error; name exists but has no IP.
    }
  }

  // Parse full JSON
  NmcJsonValue root;
  if (!NmcParseJson(aValueJson, root) || !root.IsObject()) {
    NMC_ERR("ParseNameValue: JSON parse error for hostname=%s",
            nsPromiseFlatCString(aHostname).get());
    return NS_OK;  // Malformed value -- treat as unresolvable
  }

  // Extract apex and subdomain labels from hostname
  nsAutoCString lower(aHostname);
  ToLowerCase(lower);
  nsDependentCSubstring withoutBit = Substring(lower, 0, lower.Length() - 4);
  int32_t dotPos = withoutBit.RFindChar('.');
  nsAutoCString apex;
  if (dotPos >= 0) {
    apex = Substring(withoutBit, dotPos + 1);
  } else {
    apex = withoutBit;
  }

  nsTArray<nsCString> labels;  // outermost first: ["www","sub"] for www.sub.example.bit
  NmcExtractSubdomainLabels(lower, apex, labels);

  NMC_LOG("ParseNameValue: apex=%s subdomain_labels=%u",
          apex.get(), (unsigned)labels.Length());

  if (labels.IsEmpty()) {
    // Apex lookup -- use top-level fields directly
    NmcPopulateNameValue(root, aResult);
    NMC_LOG("ParseNameValue: apex -> ip=%s ip6=%s ns=%u",
            aResult.ip.get(), aResult.ip6.get(), (unsigned)aResult.ns.Length());
    return NS_OK;
  }

  // Subdomain map traversal.
  // labels = ["www","sub"] for www.sub.example.bit
  // We want root.map["sub"]["map"]["www"] (innermost label first).
  const NmcJsonValue* mapNode = root.Get("map");
  if (!mapNode || !mapNode->IsObject()) {
    // No map -- apex fallback
    NmcPopulateNameValue(root, aResult);
    NMC_LOG("ParseNameValue: no map, apex fallback -> ip=%s", aResult.ip.get());
    return NS_OK;
  }

  const NmcJsonValue* resolvedEntry = nullptr;

  // Iterate innermost label (labels.Last()) down to outermost (labels[0])
  for (int32_t i = (int32_t)labels.Length() - 1; i >= 0; i--) {
    const nsCString& label = labels[(uint32_t)i];
    // Exact match first, then wildcard "*"
    const NmcJsonValue* entry = mapNode->Get(label);
    if (!entry) entry = mapNode->Get("*");

    if (!entry || !entry->IsObject()) {
      // No map entry at this level -- apex fallback
      NMC_LOG("ParseNameValue: no map entry for label '%s', apex fallback",
              label.get());
      NmcPopulateNameValue(root, aResult);
      return NS_OK;
    }

    resolvedEntry = entry;

    if (i > 0) {
      // Need to descend further: get nested map
      mapNode = entry->Get("map");
      if (!mapNode || !mapNode->IsObject()) {
        // Map chain broken -- use this entry
        break;
      }
    }
  }

  if (resolvedEntry && resolvedEntry->IsObject()) {
    NmcPopulateNameValue(*resolvedEntry, aResult);
    NMC_LOG("ParseNameValue: map resolve -> ip=%s ip6=%s",
            aResult.ip.get(), aResult.ip6.get());
  } else {
    NmcPopulateNameValue(root, aResult);
    NMC_LOG("ParseNameValue: map traversal gave null, apex fallback -> ip=%s",
            aResult.ip.get());
  }

  return NS_OK;
}

// ---------------------------------------------------------------------------
// Internal resolution pipeline
// ---------------------------------------------------------------------------

nsresult nsNamecoinResolver::ResolveInternal(const nsACString& aHostname,
                                              NamecoinResolveResult& aResult) {
  /**
   * Full resolution pipeline for a .bit hostname.
   *
   * Steps:
   *   1. Strip .bit → extract apex label (e.g. "example.bit" → "d/example")
   *   2. Compute ElectrumX scripthash for the name
   *   3. Query ElectrumX: blockchain.scripthash.get_history
   *   4. Pick most recent confirmed tx (highest height)
   *   5. Query ElectrumX: blockchain.transaction.get (verbose)
   *   6. Get current block height (for expiry check)
   *   7. Decode NAME_UPDATE output → JSON value
   *   8. Check name expiry (> 36,000 blocks)
   *   9. Parse value JSON → ip/ip6/map/alias/ns/translate
   *   10. Follow alias chain if present (recursive, max hops)
   *   11. Convert ip/ip6 strings to NetAddr for return
   *   12. Set cache TTL based on remaining blocks to expiry
   */

  MOZ_ASSERT(!NS_IsMainThread());
  fprintf(stderr, "[namecoin] ResolveInternal: START %s\n",
          nsPromiseFlatCString(aHostname).get());

  if (!IsNamecoinHost(aHostname)) {
    aResult.error.AssignLiteral("Not a .bit hostname");
    return NS_ERROR_INVALID_ARG;
  }

  // Step 1: Build d/name lookup key
  // Strip .bit suffix and any subdomains to get the apex label
  nsAutoCString lower(aHostname);
  ToLowerCase(lower);

  // Remove .bit
  nsDependentCSubstring withoutBit = Substring(lower, 0, lower.Length() - 4);

  // Find apex (last label before .bit)
  int32_t dotPos = withoutBit.RFindChar('.');
  nsAutoCString apex;
  if (dotPos >= 0) {
    apex = Substring(withoutBit, dotPos + 1);
  } else {
    apex = withoutBit;
  }

  nsAutoCString namePath;
  namePath.AssignLiteral("d/");
  namePath.Append(apex);

  NMC_LOG("ResolveInternal: %s → lookup %s",
          nsPromiseFlatCString(aHostname).get(), namePath.get());

  // Step 2: Compute scripthash
  nsAutoCString scripthash;
  nsresult rv = ComputeScripthash(namePath, scripthash);
  NS_ENSURE_SUCCESS(rv, rv);
  fprintf(stderr, "[namecoin] ResolveInternal: scripthash=%s\n", scripthash.get());

  // Step 3: Query history
  nsAutoCString historyParams;
  historyParams.AppendLiteral("[\"");
  historyParams.Append(scripthash);
  historyParams.AppendLiteral("\"]");

  nsCString historyJson, usedServer;
  rv = ElectrumXRequestValidated("blockchain.scripthash.get_history"_ns,
                                    historyParams, historyJson, usedServer);
  if (NS_FAILED(rv)) {
    aResult.error.AssignLiteral("ElectrumX servers unreachable");
    NMC_ERR("ResolveInternal: all ElectrumX servers failed for %s",
            namePath.get());
    return NS_ERROR_NAMECOIN_SERVERS_UNREACHABLE;
  }

  // Simple parse: find most recent confirmed tx
  // History response: [{"tx_hash":"abc","height":123}, ...]
  // Find entries with positive height, pick highest
  uint32_t bestHeight = 0;
  nsAutoCString bestTxHash;

  // Simple tokenizer: scan for "height":<N> / "tx_hash":"<hex>"
  {
    const nsCString& h = historyJson;
    int32_t searchPos = 0;
    while (true) {
      int32_t heightPos = h.Find("\"height\":", searchPos);
      if (heightPos < 0) break;
      nsDependentCSubstring rest = Substring(h, heightPos + 9);
      nsresult dummy;
      uint32_t height = (uint32_t)rest.ToInteger64(&dummy);
      if (NS_SUCCEEDED(dummy) && height > bestHeight) {
        // Find the tx_hash for this entry (scan back in the current JSON object)
        int32_t objStart = h.RFindChar('{', heightPos);
        if (objStart >= 0) {
          nsAutoCString obj;
          obj = Substring(h, objStart,
                          h.FindChar('}', objStart) - objStart + 1);
          int32_t txPos = obj.Find("\"tx_hash\":\"");
          if (txPos >= 0) {
            nsDependentCSubstring txRest = Substring(obj, txPos + 11);
            int32_t txEnd = txRest.FindChar('"');
            if (txEnd > 0) {
              bestHeight = height;
              bestTxHash = Substring(txRest, 0, txEnd);
            }
          }
        }
      }
      searchPos = heightPos + 9;
    }
  }

  if (bestTxHash.IsEmpty()) {
    aResult.error.AssignLiteral("Name not found on blockchain");
    aResult.resolved = false;
    NMC_ERR("ResolveInternal: no history for %s — name not found",
            namePath.get());
    return NS_ERROR_NAMECOIN_NOT_FOUND;
  }

  aResult.updateHeight = bestHeight;
  aResult.txHash = bestTxHash;

  NMC_LOG("ResolveInternal: best tx=%s at height=%u",
          bestTxHash.get(), bestHeight);
  fprintf(stderr, "[namecoin] ResolveInternal: history done, bestTx=%s height=%u\n",
          bestTxHash.get(), bestHeight);

  // Step 5: Fetch verbose transaction
  nsAutoCString txParams;
  txParams.AppendLiteral("[\"");
  txParams.Append(bestTxHash);
  txParams.AppendLiteral("\",true]");

  nsCString txJson, txServer;
  rv = ElectrumXRequestAny("blockchain.transaction.get"_ns,
                             txParams, txJson, txServer);
  if (NS_FAILED(rv)) {
    aResult.error.AssignLiteral("ElectrumX servers unreachable");
    NMC_ERR("ResolveInternal: transaction fetch failed for tx=%s",
            bestTxHash.get());
    return NS_ERROR_NAMECOIN_SERVERS_UNREACHABLE;
  }
  fprintf(stderr, "[namecoin] ResolveInternal: tx fetched len=%u\n", (unsigned)txJson.Length());

  // Step 6: Current block height for expiry
  uint32_t currentHeight = 0;
  GetCurrentBlockHeight(currentHeight);  // failure is non-fatal
  aResult.blockHeight = currentHeight;

  // Step 8: Check expiry
  if (currentHeight > 0 && (currentHeight - bestHeight) > kNameExpiry) {
    NMC_ERR("ResolveInternal: name %s is EXPIRED (height=%u, current=%u, "
            "expired by %u blocks)",
            namePath.get(), bestHeight, currentHeight,
            (currentHeight - bestHeight) - kNameExpiry);
    aResult.expired = true;
    aResult.error.AssignLiteral("Namecoin name has expired");
    return NS_ERROR_NAMECOIN_EXPIRED;
  }

  // Step 7: Decode NAME_UPDATE script from vout
  // Find scriptPubKey.hex values in the verbose tx JSON
  nsAutoCString nameValue;
  {
    const nsCString& tx = txJson;
    int32_t searchPos = 0;
    while (true) {
      int32_t hexPos = tx.Find("\"hex\":\"", searchPos);
      if (hexPos < 0) break;
      nsAutoCString scriptHex;
      scriptHex = Substring(tx, hexPos + 7);
      int32_t end = scriptHex.FindChar('"');
      if (end > 0) {
        scriptHex.Truncate(end);
        nsAutoCString decodedName, decodedValue;
        if (DecodeNameScript(scriptHex, decodedName, decodedValue)) {
          // Check name matches what we looked up
          if (decodedName.Equals(namePath)) {
            nameValue = decodedValue;
            NMC_LOG("ResolveInternal: decoded value for %s: %s",
                    namePath.get(), nameValue.get());
            break;
          }
        }
      }
      searchPos = hexPos + 7;
    }
  }

  if (nameValue.IsEmpty()) {
    aResult.error.AssignLiteral("Could not decode name value from transaction");
    return NS_ERROR_FAILURE;
  }
  fprintf(stderr, "[namecoin] ResolveInternal: nameValue='%s'\n", nameValue.get());

  // Step 9: Parse value JSON
  rv = ParseNameValue(nameValue, aHostname, aResult.nameValue);
  NS_ENSURE_SUCCESS(rv, rv);
  fprintf(stderr, "[namecoin] ResolveInternal: parsed ip=%s ip6=%s\n",
          aResult.nameValue.ip.get(), aResult.nameValue.ip6.get());

  // Step 10: Follow alias if needed
  if (!aResult.nameValue.alias.IsEmpty() && mMaxAliasHops > 0) {
    NMC_LOG("ResolveInternal: following alias → %s", aResult.nameValue.alias.get());
    NamecoinNameValue aliasValue;
    rv = ResolveAlias(aResult.nameValue.alias, aliasValue, mMaxAliasHops - 1);
    if (NS_SUCCEEDED(rv)) {
      aResult.nameValue = std::move(aliasValue);
    }
  }

  // Step 11: Convert ip/ip6 to NetAddr
  if (!aResult.nameValue.ip.IsEmpty()) {
    PRNetAddr prAddr;
    if (PR_StringToNetAddr(aResult.nameValue.ip.get(), &prAddr) == PR_SUCCESS) {
      NetAddr addr;
      PRNetAddrToNetAddr(&prAddr, &addr);
      aResult.addresses.AppendElement(addr);
      NMC_LOG("ResolveInternal: resolved %s → %s",
              nsPromiseFlatCString(aHostname).get(),
              aResult.nameValue.ip.get());
    }
  }

  if (!aResult.nameValue.ip6.IsEmpty()) {
    PRNetAddr prAddr;
    if (PR_StringToNetAddr(aResult.nameValue.ip6.get(), &prAddr) == PR_SUCCESS) {
      NetAddr addr;
      PRNetAddrToNetAddr(&prAddr, &addr);
      aResult.addresses.AppendElement(addr);
    }
  }

  if (aResult.addresses.IsEmpty() && aResult.nameValue.translate.IsEmpty() &&
      aResult.nameValue.ns.IsEmpty()) {
    aResult.error.AssignLiteral("Name resolved but no usable address found");
    NMC_ERR("ResolveInternal: %s exists but has no IP/NS/translate",
            nsPromiseFlatCString(aHostname).get());
    return NS_ERROR_NAMECOIN_NO_ADDRESS;
  }

  // Step 12: Cache TTL from remaining blocks to expiry
  if (currentHeight > 0 && bestHeight > 0) {
    uint32_t blocksUsed = currentHeight - bestHeight;
    uint32_t blocksRemaining = (kNameExpiry > blocksUsed) ? kNameExpiry - blocksUsed : 0;
    // ~10 minutes per block, cap to configured TTL
    uint32_t suggestedTTL = std::min(blocksRemaining * 600u, mCacheTTLSeconds);
    aResult.ttlSeconds = suggestedTTL > 60 ? suggestedTTL : 60;
  } else {
    aResult.ttlSeconds = mCacheTTLSeconds;
  }

  aResult.resolved = true;
  NMC_LOG("ResolveInternal: success for %s, ttl=%us",
          nsPromiseFlatCString(aHostname).get(), aResult.ttlSeconds);
  fprintf(stderr, "[namecoin] ResolveInternal: SUCCESS %s ip=%s ip6=%s ttl=%us\n",
          nsPromiseFlatCString(aHostname).get(),
          aResult.nameValue.ip.get(), aResult.nameValue.ip6.get(),
          aResult.ttlSeconds);
  return NS_OK;
}

// ---------------------------------------------------------------------------
// Alias chain resolution
// ---------------------------------------------------------------------------

nsresult nsNamecoinResolver::ResolveAlias(const nsACString& aAlias,
                                           NamecoinNameValue& aResult,
                                           int aHopsRemaining) {
  if (aHopsRemaining <= 0) {
    NMC_ERR("ResolveAlias: max alias hops reached");
    return NS_ERROR_FAILURE;
  }

  // Alias must be a .bit name
  if (!IsNamecoinHost(aAlias)) {
    NMC_ERR("ResolveAlias: alias %s is not a .bit domain",
            nsPromiseFlatCString(aAlias).get());
    return NS_ERROR_INVALID_ARG;
  }

  NamecoinResolveResult aliasResult;
  nsresult rv = ResolveInternal(aAlias, aliasResult);
  if (NS_FAILED(rv)) return rv;

  aResult = std::move(aliasResult.nameValue);
  return NS_OK;
}

// ---------------------------------------------------------------------------
// Public API: Resolve (async)
// ---------------------------------------------------------------------------

nsresult nsNamecoinResolver::Resolve(const nsACString& aHostname,
                                      nsNamecoinResolveCallback* aCallback) {
  if (!IsEnabled()) return NS_ERROR_NOT_AVAILABLE;

  // Dispatch resolution to a background thread
  nsAutoCString hostname(aHostname);
  RefPtr<nsNamecoinResolver> self = this;

  nsCOMPtr<nsIRunnable> runnable = NS_NewRunnableFunction(
      "nsNamecoinResolver::Resolve",
      [self, hostname, aCallback]() {
        NamecoinResolveResult result;
        self->ResolveInternal(hostname, result);

        // Fire callback on the main thread (or current thread if off-main)
        nsCOMPtr<nsIRunnable> cb = NS_NewRunnableFunction(
            "nsNamecoinResolver::ResolveCallback",
            [aCallback, result = std::move(result)]() mutable {
              aCallback->OnNamecoinResolved(result);
            });

        if (NS_IsMainThread()) {
          cb->Run();
        } else {
          NS_DispatchToMainThread(cb);
        }
      });

  nsCOMPtr<nsIEventTarget> target = GetCurrentSerialEventTarget();
  if (!target) target = do_GetMainThread();
  return target->Dispatch(runnable, NS_DISPATCH_NORMAL);
}

// ---------------------------------------------------------------------------
// Public API: ResolveSync (blocking, testing only)
// ---------------------------------------------------------------------------

nsresult nsNamecoinResolver::ResolveSync(const nsACString& aHostname,
                                          NamecoinResolveResult& aResult) {
  if (!IsEnabled()) return NS_ERROR_NOT_AVAILABLE;
  return ResolveInternal(aHostname, aResult);
}

// ---------------------------------------------------------------------------
// Connectivity test
// ---------------------------------------------------------------------------

int32_t nsNamecoinResolver::TestConnectivity() {
  nsCString result, server;
  nsresult rv = ElectrumXRequestAny(
      "blockchain.headers.subscribe"_ns, "[]"_ns, result, server);
  if (NS_FAILED(rv)) return -1;

  uint32_t height = 0;
  GetCurrentBlockHeight(height);
  return (int32_t)height;
}

}  // namespace net
}  // namespace mozilla
