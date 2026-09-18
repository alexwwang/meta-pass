<p align="right">
  <a href="AGENTS.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Repository Guidelines for AI Agents

This file is the only mandatory entry point for AI-assisted work in this repository. Read task-specific documents from the routing table below; do not load every README by default.

## Project and safety baseline

- Target: ESP32-C3, 8 MB Flash, no PSRAM, ESP-IDF 5.5.3.
- Preserve the protected Flash layout: the 3 MB application limit and `cardid`
  at `0x356000` are mandatory template contracts.
- Preserve existing user changes. Start with `git status --short --branch`; never overwrite or clean unrelated files.
- Hardware facts follow this priority: product specifications and measured results → `components/bsp/include/bsp_pins.h` → BSP headers and implementation → hardware guide → README/demo code. If a task requires a hardware detail not defined by these sources, ask the user instead of guessing.
- Reusable board logic belongs in `components/bsp`; pages, state machines, animations, and application tasks belong in `main`.
- LVGL is not thread-safe. Code outside the LVGL task must hold `bsp_lvgl_lock()` while accessing LVGL objects.
- Button callbacks must stay non-blocking. Audio, storage, networking, and other slow operations belong in worker tasks.
- A demo must stop every task, timer, callback, and event handler that can access its UI before deleting the screen.
- System-level policies (boot policy, otadata state, flash layout) must be enforced at an unbypassable layer (bootloader or validator), never delegated to child-firmware cooperation. Child-firmware hooks are defense-in-depth only.
- Before delivering a firmware or bootloader change, verify the change is physically present in the built artifact (map file symbols, log strings) — a clean build is not evidence of linkage.
- Tests and static gates must not depend on local uncommitted build state; when `build/` artifacts are absent (CI bare checkout), tests fall back to synthetic fixtures and keep the same assertions.
- Keep testable state machines, protocols, timing, and layout calculations independent from ESP-IDF/LVGL and cover them with host tests.
- Never commit credentials, device QR secrets, private keys, personal data, or unsanitized logs.
- Every maintained Markdown document uses English at its default `.md` path and Simplified Chinese in a paired `.zh_CN.md` file. Keep both versions aligned and retain reciprocal language links.

## Task-specific context routing

| Task | Read before editing |
| --- | --- |
| Any code change | `docs/development/ai-guide.md`, relevant headers and neighboring implementation |
| Environment bootstrap or missing toolchain | `docs/development/engineering/environment-setup.md` |
| BSP, pins, buses, display, audio, battery | `docs/hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.md`, `components/bsp/include/bsp_pins.h` |
| Launcher UI or menu | `main/main.c`, `components/bsp/` button and display APIs |
| Build, test, dependencies, partitions | `docs/development/engineering/build-and-test.md`, `docs/development/engineering/protected-flash-layout.md`, `sdkconfig.defaults`, `partitions.csv` |
| Debugging device-behavior or signature/installer issues | `docs/development/engineering/debugging-workflow.md`, `docs/BUGS.md`, `docs/assets/handoff-unsigned-rootcause.md` |
| CI or release | the matching file in `docs/development/ci/CI-*.md` and `.github/workflows/` |
| Project completion | full `./tools/validate.sh` gate + the acceptance checklist in `docs/assets/meta-pass-design.md` (§10) |
| Documentation | `docs/contribution/doc-conventions.md`, `docs/README.md` |
| Commit or PR | `docs/contribution/commit-and-pr.md` |

Use the root `README.md` for the project overview and `docs/README.md` for the documentation index. For the detailed AI development workflow — context setup, source-of-truth priority, application/BSP boundary, runtime invariants, material placement, and delivery format — read `docs/development/ai-guide.md`. `docs/fork-guide.md` is background on upstream fork conventions, kept for reference.

## Required validation and delivery

Run the smallest relevant check while iterating, then run the complete gate before delivery:

```bash
./tools/validate.sh --static    # repository checks + host tests
./tools/validate.sh --firmware  # ESP-IDF build + merged-image verification
./tools/validate.sh             # complete gate
```

The complete gate requires an activated ESP-IDF 5.5.3 environment. Do not describe a successful build as hardware validation. Final delivery must report these fields separately:

```text
Build: PASS / FAIL / NOT RUN
Host tests: PASS / FAIL / NOT RUN
Device tests: PASS / FAIL / NOT RUN
Unverified: remaining board, instrument, or user checks
```

Create commits and push only when the user requests them or the active workflow explicitly requires them. Record user-visible changes in `docs/CHANGELOG.md`; internal refactors, CI maintenance, typo fixes, and generated-file refreshes do not require a changelog entry.

Commit messages must NOT contain any AI/agent attribution footers — no `Co-Authored-By:`
trailer naming a tool or agent, no `Generated with ...` lines. This repo's history was
rewritten once (2026-09-17) to strip such footers; do not reintroduce them.

Community guidance is in `.github/CONTRIBUTING.md`, `.github/CODE_OF_CONDUCT.md`, `.github/SECURITY.md`, and `.github/SUPPORT.md`.
