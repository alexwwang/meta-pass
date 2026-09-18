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

## For AI agents

When entering the repository, first read the upstream `docs/README.md` for the baseline,
then read this directory for the fork project's (meta-pass) design context.
Read `meta-pass-design.md` before touching any meta-pass code.

## Boundary

Content in this directory lives only on the fork / feature branches and must never be
synced back to upstream `main`.
