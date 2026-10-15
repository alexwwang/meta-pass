#!/usr/bin/env node
// Host-side local installer for the dedicated DATA child fixture.
//
// The marketplace-driven E2E runners cannot install tests/realdevice/data-child
// because no marketplace entry serves it. This driver reuses the exact page
// modules (extractAppImage / extractDataImages / geomFromListing / createBridge
// / runInstall) so the device sees byte-identical offers to a USB-page local
// install — same endpoints, same allocator claims, same chunk protocol.
//
// Usage:
//   node tools/realdevice/local_data_child_install.mjs \
//     --ip <device-ip> --play-id <id> --name <display-name> \
//     --image <merged-full.bin> [--token <32hex>] [--logdir <dir>]
// Token falls back to META_PASS_SESSION; never pass it on the command line in
// shared environments (it lands in shell history).
import fs from "node:fs";
import path from "node:path";
import crypto from "node:crypto";
import { fileURLToPath } from "node:url";
import { extractAppImage, extractDataImages, isFullImage } from "../../install-slot/extract-app-image.js";
import { geomFromListing } from "../../install-slot/dynslot-pool.js";
import { createBridge, parseSlots, runInstall, setSha256, sha256Pure } from "../../install-slot/phone-install.js";

setSha256((u8) => crypto.createHash("sha256").update(u8).digest("hex"));

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
const LOGROOT = path.join(ROOT, "tools/realdevice/logs");

function argsOf(argv) {
  const out = {};
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (!a.startsWith("--")) continue;
    const eq = a.indexOf("=");
    if (eq > 2) out[a.slice(2, eq)] = a.slice(eq + 1);
    else out[a.slice(2)] = argv[i + 1] && !argv[i + 1].startsWith("--") ? argv[++i] : true;
  }
  return out;
}
const args = argsOf(process.argv.slice(2));
const ip = String(args.ip || "");
const playId = Number(args["play-id"] || 0);
const name = String(args.name || "");
const imagePath = path.resolve(String(args.image || ""));
const token = String(args.token || process.env.META_PASS_SESSION || "");
const logDir = path.resolve(String(args.logdir || path.join(LOGROOT, "local-data-child-install-" + new Date().toISOString().replace(/[:.]/g, "-"))));

if (!/^\d+\.\d+\.\d+\.\d+$/.test(ip) || !Number.isInteger(playId) || playId <= 0 ||
    !name || !fs.existsSync(imagePath) || !/^[a-f0-9]{32}$/i.test(token)) {
  console.error("usage: node tools/realdevice/local_data_child_install.mjs --ip <device-ip> --play-id <n> --name <name> --image <merged-full.bin> [--token <32hex>]");
  process.exit(2);
}
fs.mkdirSync(logDir, { recursive: true });

const merged = new Uint8Array(fs.readFileSync(imagePath));
if (!isFullImage(merged)) {
  console.error("image is not a merged full-flash image; run idf.py merge-bin first");
  process.exit(2);
}
const ext = extractAppImage(merged, 0x7ffff000);
const dataImages = extractDataImages(merged);
if (dataImages.length === 0) {
  console.error("image declares no child DATA partitions (expected e2edata)");
  process.exit(2);
}

const bridge = createBridge(`http://${ip}`, token);

const listingReply = await bridge.slots();
if (!listingReply.ok) throw new Error("slots read failed: HTTP " + listingReply.status + " " + listingReply.text.slice(0, 120));
const listing = parseSlots(listingReply.text);
if (!listing) throw new Error("device slots response failed shape checks");
const geom = geomFromListing(listing, ext.length);
if (!geom.proposal) {
  console.error(`no carve proposal: maxGap=${geom.maxGap} totalFree=${geom.totalFree} need=${ext.length}`);
  process.exit(3);
}

const appSha = sha256Pure(ext.data);

const offer = {
  protocol: 1,
  playId,
  revisionId: 1,
  name,
  storeSha256: appSha,          // local file: nothing upstream to disagree with
  imageLen: ext.length,
  sha256: appSha,
  suggestedSlot: geom.proposal.slot,
  slot: geom.proposal.slot,    // 交互 v2:手机直确认新槽
  slots: geom.placed.map((s) => ({ slot: s.slot, limit: s.limit, fit: s.fit })),
  data: await Promise.all(dataImages.map(async (d) => ({
    playId,
    size: d.required_size,
    label: d.label,
    subtype: d.subtype,
    initialImageSize: d.initial_image_size,
    sha256: d.initial_image_size > 0 ? sha256Pure(d.data) : undefined,
  }))),
  reason: "ok",
  carveOffset: geom.proposal.carveOffset,
  carveSize: geom.proposal.carveSize,
};

fs.writeFileSync(path.join(logDir, "offer.json"), JSON.stringify(offer, null, 2) + "\n");
console.log(`offer: play=${playId} name=${name} slot=${offer.slot} carve=${geom.proposal.carveOffset}+${geom.proposal.carveSize} data=${offer.data.map((d) => `${d.label}:${d.size}`).join(",")}`);

const t0 = Date.now();
const result = await runInstall(bridge, offer, ext.data, {
  dataImages,
  status: (s) => console.log(`[+${((Date.now() - t0) / 1000).toFixed(1)}s] status: ${JSON.stringify(s).slice(0, 160)}`),
  progress: (off, total) => process.stdout.write(`\rupload ${((off / total) * 100).toFixed(0)}%`),
  resume: (off) => console.log(`\nresume from ${off}`),
});
console.log("");
if (!result.ok) {
  console.error(`install FAILED [${result.stage}]: ${result.reason}`);
  fs.writeFileSync(path.join(logDir, "result.json"), JSON.stringify({ ok: false, ...result }, null, 2) + "\n");
  process.exit(1);
}
const after = await bridge.slots();
fs.writeFileSync(path.join(logDir, "result.json"), JSON.stringify({ ok: true, result, slotsAfter: JSON.parse(after.text) }, null, 2) + "\n");
console.log("install OK; slots after:");
console.log(after.text);
