# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

## Namecoin (.bit) resolution error pages
## These strings are displayed in netError.xhtml when a .bit domain
## fails to resolve via the Namecoin blockchain.

## --------------------------------------------------------------------------
## NS_ERROR_NAMECOIN_NOT_FOUND — Name never registered
## --------------------------------------------------------------------------

neterror-namecoin-not-found-title = Namecoin Name Not Found
neterror-namecoin-not-found-short = <strong>{ $hostname }</strong> does not exist on the Namecoin blockchain.
neterror-namecoin-not-found-long =
    The Namecoin name <strong>{ $name }</strong> has no registration history on
    the blockchain. This means the name was never registered, or it may be
    misspelled.
    <ul>
      <li>Check the spelling of the address — Namecoin names are case-sensitive
          within the <em>d/</em> namespace.</li>
      <li>If you followed a link, the name may never have been claimed.</li>
      <li>Namecoin names must be registered via a NAME_NEW + NAME_FIRSTUPDATE
          transaction pair before they can resolve.</li>
    </ul>

neterror-namecoin-not-found-learn =
    <a href="https://www.namecoin.org/docs/name-owners/">What is Namecoin?</a>
    — Learn how .bit domains work on a decentralized blockchain.

## --------------------------------------------------------------------------
## NS_ERROR_NAMECOIN_EXPIRED — Name registration lapsed
## --------------------------------------------------------------------------

neterror-namecoin-expired-title = Namecoin Name Expired
neterror-namecoin-expired-short = <strong>{ $hostname }</strong> has expired and can no longer be resolved.
neterror-namecoin-expired-long =
    The Namecoin name <strong>{ $name }</strong> was last updated at block
    <strong>{ $updateHeight }</strong>, but the current block height is
    <strong>{ $currentHeight }</strong>. Names expire after 36,000 blocks
    (~250 days) without renewal.
    <ul>
      <li>The domain owner needs to broadcast a NAME_UPDATE transaction to
          renew it.</li>
      <li>Expired names may be re-registered by anyone — resolving them would
          be unsafe.</li>
      <li>If you are the owner, use your Namecoin wallet to renew the name
          immediately.</li>
    </ul>

neterror-namecoin-expired-blocks =
    Last updated: block { $updateHeight } · Current height: { $currentHeight } ·
    Expired by { $expiredBlocks } blocks

neterror-namecoin-expired-learn =
    <a href="https://www.namecoin.org/docs/name-owners/renew-a-name/">How to
    renew a Namecoin name</a>

## --------------------------------------------------------------------------
## NS_ERROR_NAMECOIN_SERVERS_UNREACHABLE — Cannot reach ElectrumX
## --------------------------------------------------------------------------

neterror-namecoin-servers-unreachable-title = Namecoin Servers Unreachable
neterror-namecoin-servers-unreachable-short = Could not connect to any ElectrumX server to resolve <strong>{ $hostname }</strong>.
neterror-namecoin-servers-unreachable-long =
    { -brand-short-name } could not reach any of the configured ElectrumX
    servers needed to look up Namecoin (.bit) domain names. The name may
    still be valid — this is a connectivity problem, not a name problem.
    <ul>
      <li>Check your internet connection.</li>
      <li>The ElectrumX servers may be temporarily down — try again in a few
          minutes.</li>
      <li>If you are behind a firewall or proxy, WebSocket connections to
          ElectrumX servers (typically port 50003 or 50004) may be blocked.</li>
      <li>You can configure alternative servers in
          <code>about:config</code> →
          <code>network.namecoin.electrumx_servers</code>.</li>
    </ul>

neterror-namecoin-servers-unreachable-retry = Try Again

neterror-namecoin-servers-unreachable-learn =
    <a href="https://www.namecoin.org/docs/electrum-nmc/">About ElectrumX
    servers for Namecoin</a>

## --------------------------------------------------------------------------
## NS_ERROR_NAMECOIN_NO_ADDRESS — Name exists, no IP
## --------------------------------------------------------------------------

neterror-namecoin-no-address-title = No Address for Namecoin Name
neterror-namecoin-no-address-short = <strong>{ $hostname }</strong> exists on the blockchain but has no IP address configured.
neterror-namecoin-no-address-long =
    The Namecoin name <strong>{ $name }</strong> was found and is not expired,
    but its value does not contain a usable IP address (<code>ip</code>,
    <code>ip6</code>), DNS delegation (<code>ns</code>), or domain
    translation (<code>translate</code>).
    <ul>
      <li>The name may be reserved as a placeholder or used only for
          metadata.</li>
      <li>Some names store only a <code>.onion</code> (Tor) address, which
          requires Tor Browser to access.</li>
      <li>The domain owner needs to add an <code>"ip"</code> or
          <code>"ip6"</code> field to the name's JSON value via a
          NAME_UPDATE transaction.</li>
    </ul>

neterror-namecoin-no-address-learn =
    <a href="https://www.namecoin.org/docs/name-owners/dns/">Namecoin DNS
    configuration guide</a>

## --------------------------------------------------------------------------
## Common / shared strings
## --------------------------------------------------------------------------

neterror-namecoin-what-is =
    <a href="https://www.namecoin.org/">What is Namecoin?</a> — Namecoin is
    a decentralized naming system based on blockchain technology. The
    <strong>.bit</strong> top-level domain is not part of the traditional DNS
    system — it is resolved directly from the Namecoin blockchain.
