<p align="right">
  <a href="meta-pass-v1-retrospective.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# The meta-pass v1.0.0 Journey — An ELI5 Retrospective

> Status: stage retrospective (2026-09-13 → 2026-09-18; the `feat/sign` branch merged
> into `main` and shipped as v1.0.0). Audience: anyone who wants to know how this was
> built and which pits we fell into — including future us. The authoritative technical
> detail lives in the [design document](meta-pass-design.md) and the
> [signing design](meta-pass-signing-design.md); this file tells the story plainly.

## 1. What we set out to build (goals)

Imagine a toy computer (AI Passport) that can only run "one program" at a time. We
turned it into a **small box that holds several programs** — fun, safe, and hard to
break:

1. **A boot menu (the meta-pass launcher)**: power on, see a list, pick a program,
   play it;
2. **Three drawers (slots)**: up to 3 play firmwares installed at once; switch or
   delete anytime, each keeps its own data;
3. **An official sticker (signing)**: officially recognized firmware carries a
   wax-seal (signature + name); unknown firmware warns you before it boots — and
   each firmware can carry a small "boot surprise" (easter egg);
4. **Moving-day packing (backup/restore)**: snapshot everything on the box
   (firmware + data) into one zip; restore it in one click on a new or re-flashed
   device;
5. **Upgrades that lose nothing**: upgrading the menu program itself must never
   touch your plays or data;
6. **Every boot returns to the menu**: after playing, power-cycling, even crashing —
   always back to the list.

## 2. How it works (the analogy version)

### Slots = a row of drawers

Flash memory holds three drawers of different sizes (ota_0/ota_1/ota_2). Each drawer
stores the **firmware body** plus, at the very bottom, a **sticker page** (a 4 KB
metadata sector) carrying the signature and the play's name. On every boot the
launcher looks through all three drawers and draws the list on screen.

### Signatures = a seal only the official publisher has

There is a key pair: the private key belongs to the publisher; anyone may hold the
public key for comparison. When publishing, the private key "stamps" the sticker
page; the launcher on the device verifies the stamp with the public key — pattern
matches, badge and name show up. The signing tool `sign-firmware.sh` can also tuck
an "easter-egg text" onto the sticker page, with `--check-egg-text` to verify it
fits before shipping.

### The single-file firmware = one hamburger

Releases used to need three files (full image / upgrade container / factory image);
users picked the wrong one. Now we ship **one file**: bun (bootloader) + lettuce
(partition table) + patty (app) stacked, with a 44-byte "toothpick" stuck in at the
end (the MPUPV2 fingerprint):

- **Fresh market install**: stuff the whole hamburger into flash at 0x0 (the
  toothpick lands on spare ground behind the patty; the launcher ignores it and
  boots normally);
- **Data-preserving web upgrade**: the companion web page knows the toothpick,
  takes the burger apart, and replaces only bun/lettuce/patty — never touching your
  "notebook" (NVS) or the three drawers;
- A validator (`tools/validate.sh`) watches the whole chain: a shipped burger must
  be consistent inside and out.

### Backup/restore = photograph everything, keep a packing list

Each drawer yields at most three photos: the **firmware body**, **extra stored
data**, and the **sticker page**. Every photo gets a fingerprint (SHA-256) written
into a **packing list** (manifest.json), all zipped up. When moving (new device or
re-flash):

- New drawer smaller than the old one? Check each photo's length in the list — only
  proceed if it fits; refuse loudly otherwise. Never write half and shred your data;
- Fingerprint mismatch between photo and list? Refuse — guards against corruption
  in transit;
- The "notebook" (NVS: Wi-Fi settings, app notes) is photographed **automatically**
  into the same zip and written back **automatically** on restore — users never need
  to know what NVS is.

### Every boot returns to the menu = a gatekeeper acts before any game starts

This was the **most important architecture decision** of the cycle. The first design
asked play firmwares to "behave": don't press the "I want to stay resident" button
(writing VALID into otadata). But old plays were compiled under the old rule and
pressed it anyway — the menu never got a say.

Final design: put a **gatekeeper** (2nd-stage bootloader hook) at a stage that runs
**before every program**. On each power-up the gatekeeper glances at the
"reservation table" (otadata); any "stay resident" mark is **erased on the spot**,
then normal boot selection proceeds. The result:

- Whether a play misbehaves, old or new — every boot returns to the menu;
- Devices previously "locked" by a play self-heal after one upgrade + reboot;
- Play firmware needs zero changes — boot policy is now decided
  **unilaterally by meta-pass**.

## 3. Pits and lessons (the valuable part)

### Technical

**Lesson 1 — Critical policy must never rely on participants' good behavior.**
The single-session model first depended on a hook template in each play repo
"not writing VALID"; that copy stayed outdated and the policy silently failed.
Rule: system-level constraints must be enforced by the system itself at an
unbypassable layer (the bootloader).

**Lesson 2 — "It compiles" ≠ "it's actually in there".**
The bootloader hook's first build was all green, yet the binary check showed the
hook **was never linked** (a new component directory doesn't trigger cmake
reconfigure; the component was also missing its CMakeLists.txt). Only map-file and
log-string inspection caught it. Rule: before delivering, demand direct evidence
the feature is inside the artifact — not "no errors".

**Lesson 3 — When reads are slow or flaky, suspect the protocol, not gremlins.**
Browser backup first took ~3 minutes per MB with frequent errors. A line-by-line
comparison against esptool.py's battle-tested implementation found three gaps: the
MD5 digest frame was never read, per-frame ACKs were missing, and chunks were too
small. Aligning to the official stop-and-wait cadence + raising the baud rate to
921600 made it an order of magnitude faster with a collapse in failure rate.
Rule: before reinventing, read the proven implementation line by line.

**Lesson 4 — Tests must run in a bare environment, same as CI.**
One test read build artifacts from a local `build/` directory for comparison:
green locally, crashed in CI (bare checkouts have no `build/`). It now runs in dual
mode: real-artifact comparison when artifacts exist, the same logic assertions on
synthetic data when they don't. Rule: static gates must not depend on local state.

**Lesson 5 — One backslash inside a comment can break the whole CI.**
A `//` comment line ending in `\` makes the compiler swallow the next line;
`-Werror` fails the build — and the issue had been lurking for over a week. Two
more of the same kind were swept out right after. Rule: run gates often; after
fixing one instance, scan the whole repo for its siblings.

### Process

**Lesson 6 — The deployed site ≠ the repo; version-stamp everything.**
A three-day "signature won't verify" hunt ended at: the live installer page was an
old deployment. Now firmware carries a git-hash version stamp, the web page logs
its deployed git SHA on first line, and a gate guards the version placeholder —
one glance reconciles them.

**Lesson 7 — Walk the toolchain's release path end to end.**
`validate.sh` once contained, simultaneously: a filename glob mismatch, firmware
not copied into the release directory, and a truncated Chinese comment executed by
bash as a command — each step looked fine, the chain was broken. Rule: after
touching a toolchain, run the real release flow start-to-finish; that is acceptance.

**Lesson 8 — For one-shot resources, verify the landing spot before acting.**
The publishing authorization code is single-use; the first redemption burned the
code server-side while a local config-path mistake meant nothing was saved, and a
re-issue was needed. Rule: for irreversible operations, dry-run the entire command
and check every parameter first.

### Collaboration

**Lesson 9 — Align vocabulary before solving the problem.**
"Detail page" and "boot-confirmation page" were the same screen; each side thought
the other meant a different page, and the discussion spun. Rule: when two names
appear in a conversation, first confirm they aren't two things.

**Lesson 10 — Say it plainly.**
When the user explicitly asks "no metaphors, no adverb padding", lay out the causal
chain: what was done, why, and the evidence. Keep technical writing adjective-light
by default.

### Engineering habits that carried this cycle

- **Static analysis first**: grep / map files / syntax checks before asking the
  user to verify anything;
- **Device evidence over inference**: did the screen flicker, does the serial log
  contain that line — worth more than "it should work";
- **Bilingual docs in lockstep**: every maintained document exists as English +
  Simplified Chinese pairs; changing one means changing the other;
- **No AI attribution in commits**: the repo history was rewritten once to strip
  such footers; do not reintroduce them.

## 4. Where things stand and what's next

| Item | Status |
| --- | --- |
| Version | v1.0.0 (tag + GitHub Release, with SHA-256) |
| Market publishing | revision 1037 submitted, **pending community review** — not yet public |
| Gates | layout protection / upgrade safety / single-file format / host tests / QEMU boot cases all green |
| Open items | sync the hook template copy in the pass-radar repo (defense-in-depth only, non-blocking); track the market review result |

**ELI5 in one sentence**: the box now guards its own door, recognizes official
stickers, moves house without losing toys, and upgrades without losing files —
next, it goes out to meet its users.
