# Firefox Native Namecoin Integration — Session 6 Handoff Notes
# =============================================================
# Written: 2026-04-16 08:22 AEST
# Purpose: Operational learnings for the next session to pick up from.
# Previous: notes/session-03-phase2-4-complete.md (Phase 2-4 implementation), notes/session-02-phase1-complete.md (Phase 1 completion)
# Repo: https://github.com/mstrofnone/gecko-namecoin
# Note: notes/session-04-orchestration.md captures *workflow/orchestration* learnings, not implementation
#       changes. The canonical implementation handoff is notes/session-03-phase2-4-complete.md.


================================================================================
WHAT WAS DONE THIS SESSION
================================================================================

This session was orchestration only — Phases 2, 3, and 4 were already
implemented (by subagents launched in this session). The work here was:

  [x] Spawned 3 parallel Opus subagents (phase2-dane-tls, phase3-ui, phase4-android)
  [x] Recovered Phase 3 subagent work (it timed out at commit step, files intact)
  [x] Built clean 4-phase git history and pushed to mstrofnone/gecko-namecoin
  [x] Updated README.md to reflect all 4 phases (via subagent, pushed)
  [x] Wrote notes/session-03-phase2-4-complete.md session handoff notes (via subagent)

For implementation details of what each phase produced, see notes/session-03-phase2-4-complete.md.


================================================================================
WORKSPACE / REPO LAYOUT — IMPORTANT CONTEXT
================================================================================

The browse workspace (/Users/m/.openclaw/workspace/browse) IS the gecko-namecoin
git repo root. Files like README.md, src/, patches/, docs/, tests/, tools/ are
all at the workspace root — NOT inside a gecko-namecoin/ subdirectory.

HOWEVER: subagents run with the workspace root as cwd, so when they commit they
may write files to a gecko-namecoin/ SUBDIRECTORY instead of the root. This
happened this session — all three phase subagents wrote to gecko-namecoin/src/,
gecko-namecoin/patches/ etc. instead of src/, patches/.

The fix: extract their files using `git show main:<gecko-namecoin/path>` and
re-commit them at the correct root-level paths on a clean branch. See the
build process below.

CLEAN PUSH PROCESS USED THIS SESSION:
  1. git checkout -b clean-history origin/main
  2. Extract files from subagent commits:
       git diff origin/main main --name-only | grep "^gecko-namecoin/" | while IFS= read -r f; do
         dest="${f#gecko-namecoin/}"
         mkdir -p "$(dirname "$dest")"
         git show "main:$f" > "$dest"
       done
  3. Stage and commit in logical phase groups (Phase 2, then 3, then 4)
  4. git push origin clean-history:main
  5. git checkout main && git branch -d clean-history

SUBAGENT TASK WORDING FIX:
When spawning subagents for this repo, tell them explicitly:
  "Write all files to the WORKSPACE ROOT (not a gecko-namecoin/ subdirectory).
   The repo root IS /Users/m/.openclaw/workspace/browse — use that as your cwd
   for all file writes and git commands."

This prevents the path confusion that required the extraction step above.


================================================================================
SUBAGENT WORKFLOW LEARNINGS
================================================================================

1. PARALLEL PHASES: Phases 2, 3, and 4 had no dependencies on each other and
   ran successfully in parallel. Future parallel work is safe for any phases
   that touch different files.

2. PHASE 3 TIMEOUT: The phase3-ui subagent timed out (10 minute limit) just
   as it was running `git commit`. The files were written and staged correctly.
   Recovery: the files showed up as untracked/staged in the workspace even
   after the agent timed out. They were committed in the clean-history step.
   Lesson: for large UI phases, give more timeout headroom (>10m).

3. SUBAGENT MODEL: All three implementation agents used opus (claude-opus-4-6).
   This is important — lower models produced noticeably thinner code in earlier
   sessions. Keep using opus for C++ and Kotlin generation.

4. STREAMTO ERROR: `streamTo: "parent"` is not supported for runtime=subagent
   (only ACP). Remove that param when spawning subagents.

5. COMMIT MESSAGES: Subagents write good commit messages when given the
   message format in the task. Include the full format in the task spec.


================================================================================
CURRENT REPO STATE (as of this session)
================================================================================

Remote: https://github.com/mstrofnone/gecko-namecoin
Branch: main (4 clean commits)

  8ba165a  Phase 1 — native .bit DNS resolution for Firefox
  3c4edb4  Phase 3: UI integration — about:namecoin, address bar, cert viewer, settings
  91ca666  Phase 2: DANE-TLSA validation, name constraints, HTTPS upgrade
  756e86e  Phase 4: Android/GeckoView — battery-aware connections, Fenix settings, GeckoView API

Local branch: main (same as remote after clean push)
No uncommitted changes in the gecko-namecoin files.

NOTE: The local workspace has untracked files (AGENTS.md, SOUL.md, HEARTBEAT.md,
etc.) that are OpenClaw workspace files — NOT part of the gecko-namecoin project.
These should never be committed to the gecko-namecoin repo. A .gitignore covering
these would be a good future hygiene step.


================================================================================
WHAT'S LEFT TO DO (from notes/session-03-phase2-4-complete.md, reproduced for convenience)
================================================================================

IMMEDIATE / HIGH PRIORITY:

  [ ] Build and test in real Firefox Nightly
      - Clone mozilla-central (hg or git, ~10GB)
      - Apply patches: tools/build-patch-series.sh <mozilla-central-path>
      - Copy src/ files to netwerk/dns/ (resolver) and security/manager/ssl/ (DANE)
      - ./mach build
      - In about:config: network.namecoin.enabled = true
      - Navigate to testls.bit — should resolve to 107.152.38.155
      - Test DANE cert validation (testls.bit has TLSA records)
      - Verify error pages show Namecoin-specific messages

  [ ] File Bugzilla meta-bug
      - Use docs/BUGZILLA.md content (copy-paste ready)
      - File meta-bug first, get bug number, then Phase 1 sub-bug
      - CC: necko-reviewers, jeremyrand@airmail.cc

  [ ] Phabricator submission
      - moz-phab submit on the patch series
      - Key review points: nsHostResolver hook (thread safety), eTLD patch
        (maintenance burden), error code module 77 (verify no conflict)

MEDIUM PRIORITY:

  [ ] Wire about:namecoin JS to real XPCOM resolver
      - Currently uses mock data (Services.prefs + hardcoded status)
      - Need a privileged JS module (JSM or ESM) + IPC message to resolver

  [ ] Verify error code module 77 (nsNamecoinErrors.h)
      - Check security/manager/ssl/nsNetErrorCodes.h in mozilla-central
      - Module 77 may conflict; may need to change to an unused number

  [ ] Add .gitignore to repo root
      - Exclude OpenClaw workspace files: AGENTS.md, SOUL.md, HEARTBEAT.md,
        IDENTITY.md, USER.md, TOOLS.md, BOOTSTRAP.md, .openclaw/, firefox*.txt,
        extension*.txt, browser.txt, electrumx.txt, namecoin.txt, howto-*.txt,
        .DS_Store, TLS Namecoin Resolver/, tls-namecoin-ext/

ANDROID:

  [ ] Build GeckoView AAR with patches applied
  [ ] Test NmcConnectionManager on physical Android device
      - Verify Wi-Fi ↔ LTE reconnection
      - Verify 5-minute background idle timer closes connections
  [ ] Submit FenixNamecoinSettings as PR to mozilla-mobile/firefox-android

PHASE 5 — ENABLEMENT (not started):

  [ ] about:config only (Nightly) → evaluate stability
  [ ] Settings toggle (Beta) → broader testing
  [ ] Default-off with discovery UI (Release)
  [ ] Default-on after sufficient adoption/confidence


================================================================================
ARCHITECTURE STATE SUMMARY
================================================================================

The full implementation is in 4 layers, all in src/:

  Layer 1 — DNS (nsNamecoinResolver.h/cpp)
    ElectrumX WS transport, JSON parser, scripthash, pooling, cross-validation,
    name expiry, alias/translate/NS delegation, DNS cache integration.
    Entry point: nsHostResolver calls IsNamecoinHost() then Resolve().

  Layer 2 — TLS (NmcDaneValidator.h/cpp)
    DANE-TLSA validation engine. Called from AuthCertificateCallback in NSS
    when CA validation fails for .bit. Cache in NmcDaneCache. Name constraints
    block public CAs via pre-check before DANE validation.

  Layer 3 — UI (src/about-namecoin/, src/namecoin-ui.ftl)
    about:namecoin diagnostic page (HTML/JS), address bar CSS, cert viewer
    patch, settings UI patch. Currently JS uses mock data pending XPCOM wiring.

  Layer 4 — Android (src/android/)
    NmcConnectionManager (battery/lifecycle), NmcGeckoViewSettings (XPCOM bridge),
    NamecoinSettings.kt (embedding API), FenixNamecoinSettings.kt (settings UI).
    Wired via #ifdef ANDROID in nsNamecoinResolver.

Patches 0001-0015 are integration templates — they show where/how to apply the
above into mozilla-central, but the actual Gecko source isn't present here.
build-patch-series.sh automates applying them.


================================================================================
END — NEXT SESSION: START WITH notes/session-03-phase2-4-complete.md FOR IMPLEMENTATION DETAIL,
      USE THIS FILE FOR WORKFLOW/ORCHESTRATION CONTEXT
================================================================================
