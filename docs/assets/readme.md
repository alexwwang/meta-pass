[简体中文](readme.zh_CN.md) | English

# docs/assets/

This directory holds supplementary documents and assets for fork projects (upstream `main`
keeps only a `.gitkeep` placeholder here).

## Purpose

Content that does not fit the root `README.md` or does not belong on the product landing
page: architecture design, decision records, protocol conventions, and similar material.

## Layout

- `meta-pass-design.md` / `meta-pass-design.zh_CN.md` — the design document for the
  meta-pass firmware launcher (single source of truth).
- `meta-pass-v1-retrospective.md` / `meta-pass-v1-retrospective.zh_CN.md` — ELI5-style
  retrospective of the v1.0.0 cycle: goals, approach, and lessons learned.
- `ota-r10.17-r10.19-audit.md` / `ota-r10.17-r10.19-audit.zh_CN.md` — code audit of the
  OTA Range/R2 changes, including evidence boundaries, findings, and follow-up order.
- `lan-pair-install-design.md` / `lan-pair-install-design.zh_CN.md` — approved
  direction for LAN phone-assisted installs: a minimal device-hosted boot page proxies
  local operations, metapass supplies the market/extract/write modules, and `feat/mota`
  removes the legacy numeric-ID download flow.
- `play563-appstore-download-reverse.md` / `play563-appstore-download-reverse.zh_CN.md` —
  reverse analysis of the hosted AppStore play 563 download path, with host/device evidence
  and actionable meta-pass improvements.
- `mota-implementation-audit.md` / `mota-implementation-audit.zh_CN.md` — audit of the
  `feat/mota` LAN-install implementation against the design doc: five blocking and nine
  non-blocking findings (all fixed in-round), test-suite blind spots closed, and the
  production deployment note.

## For AI agents

When entering the repository, first read the upstream `docs/README.md` for the baseline,
then read this directory for the fork project's (meta-pass) design context.
Read `meta-pass-design.md` before touching any meta-pass code.

## Boundary

Content in this directory lives only on the fork / feature branches and must never be
synced back to upstream `main`.
