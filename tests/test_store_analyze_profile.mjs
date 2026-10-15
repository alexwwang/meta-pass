#!/usr/bin/env node
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { createStoreAnalyzer } from "../install-slot/store-analyze.js";
import { PLAY28_RECORDINGS_4M_PROFILE, PLAY28_RECORDINGS_4M_SIZE } from "../install-slot/data-size-profile.js";

const sha256 = (b) => createHash("sha256").update(b).digest("hex");
function appImage() {
  const firstLen = 1500000, secondLen = 64;
  let total = 24 + 16 + 8 + firstLen + 8 + secondLen;
  while (total % 16 !== 15) total++;
  total += 1 + 32;
  const b = new Uint8Array(total).fill(0xab);
  b[0] = 0xe9; b[1] = 2; b[12] = 5; b[13] = 0; b[23] = 1;
  const dv = new DataView(b.buffer);
  dv.setUint32(24 + 16, 0x3fc80000, true); dv.setUint32(24 + 20, firstLen, true);
  dv.setUint32(24 + 16 + 8 + firstLen, 0x42000020, true);
  dv.setUint32(24 + 16 + 8 + firstLen + 4, secondLen, true);
  return b;
}
function entry(label, type, subtype, offset, size) {
  const b = new Uint8Array(32); b.set([0xaa,0x50,type,subtype],0);
  const dv = new DataView(b.buffer); dv.setUint32(4,offset,true); dv.setUint32(8,size,true);
  for(let i=0;i<label.length;i++) b[12+i]=label.charCodeAt(i);
  return b;
}
const app = appImage();
const originalDataSize = 6592 * 1024;
const merged = new Uint8Array(0x180000 + originalDataSize).fill(0xff);
merged[0] = 0xe9;
const parts = [
  entry("nvs",1,2,0x9000,0x6000),
  entry("phy_init",1,1,0xf000,0x1000),
  entry("factory",0,0,0x10000,0x170000),
  entry("recordings",1,0x81,0x180000,originalDataSize),
];
parts.forEach((p,i)=>merged.set(p,0x8000+i*32));
merged.set(app,0x10000);
const digest=sha256(merged);
const play={play:{id:28,revisionId:1,slug:"recorder",firmware:{available:true,size:merged.length,sha256:digest,url:"/api/download/recorder"}}};
const fetchImpl=async (url)=>{
  const u=new URL(url);
  if(u.pathname==="/api/plays/id/28") return new Response(JSON.stringify(play));
  if(u.pathname==="/api/download/recorder") return new Response(merged);
  return new Response("not found",{status:404});
};
const analyzer=createStoreAnalyzer({fetchImpl,backend:"https://example.test",sha256:async b=>sha256(b)});
const normal=await analyzer.analyze(28);
assert.equal(normal.supported,false,"declared 6.4 MiB DATA must not fit by default");
assert.equal(normal.reason,"too-large");
const profiled=await analyzer.analyze(28,PLAY28_RECORDINGS_4M_PROFILE);
assert.equal(profiled.supported,true,"explicit test profile should fit in the pool");
assert.equal(profiled.data.length,1);
assert.equal(profiled.data[0].label,"recordings");
assert.equal(profiled.data[0].subtype,0x81);
assert.equal(profiled.data[0].requiredSize,PLAY28_RECORDINGS_4M_SIZE);
assert.equal(profiled.data[0].initialImageSize,0);
assert.equal(profiled.storage.requiredSize,Math.ceil(app.length/4096)*4096+PLAY28_RECORDINGS_4M_SIZE);
console.log("Play 28 analyzer integration: PASS (default reject; explicit blank-FAT profile accepted)");
