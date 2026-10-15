#!/usr/bin/env node
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { prepareImage } from "../install-slot/phone-install.js";
import { extractAppImage } from "../install-slot/extract-app-image.js";
import { PLAY28_RECORDINGS_4M_PROFILE, PLAY28_RECORDINGS_4M_SIZE } from "../install-slot/data-size-profile.js";

const sha = (b) => createHash("sha256").update(b).digest("hex");
function appImage() {
  const firstLen=1500000, secondLen=64;
  let total=24+16+8+firstLen+8+secondLen;
  while(total%16!==15) total++;
  total+=1+32;
  const b=new Uint8Array(total).fill(0xab);
  b[0]=0xe9; b[1]=2; b[12]=5; b[13]=0; b[23]=1;
  const dv=new DataView(b.buffer);
  dv.setUint32(40,0x3fc80000,true); dv.setUint32(44,firstLen,true);
  dv.setUint32(48+firstLen,0x42000020,true); dv.setUint32(52+firstLen,secondLen,true);
  return b;
}
function part(label,type,subtype,offset,size) {
  const b=new Uint8Array(32); b.set([0xaa,0x50,type,subtype]);
  const dv=new DataView(b.buffer); dv.setUint32(4,offset,true); dv.setUint32(8,size,true);
  for(let i=0;i<label.length;i++) b[12+i]=label.charCodeAt(i);
  return b;
}
const app=appImage();
const dataSize=6592*1024;
const merged=new Uint8Array(0x180000+dataSize).fill(0xff);
merged[0]=0xe9;
[
  part("nvs",1,2,0x9000,0x6000),
  part("phy_init",1,1,0xf000,0x1000),
  part("factory",0,0,0x10000,0x170000),
  part("recordings",1,0x81,0x180000,dataSize),
].forEach((p,i)=>merged.set(p,0x8000+i*32));
merged.set(app,0x10000);
const ext=extractAppImage(merged,0x29e000-0x1000);
const play={id:28,size:merged.length,sha256:sha(merged),downloadUrl:"/api/download/recorder",
  name:"Recorder",enName:"Recorder",slug:"recorder",revisionId:1};
const meta={play,analyze:{id:28,revisionId:1,name:"recorder",reason:"ok",
  extracted:{imageLen:ext.length,sha256:sha(ext.data)}}};
const originalFetch=globalThis.fetch;
try {
  globalThis.fetch=async()=>new Response(merged);
  const out=await prepareImage(meta,-1,{dataSizeProfile:PLAY28_RECORDINGS_4M_PROFILE});
  assert.equal(out.ok,true,JSON.stringify(out));
  assert.equal(out.offer.playId,28);
  assert.equal(out.offer.data.length,1);
  assert.equal(out.offer.data[0].label,"recordings");
  assert.equal(out.offer.data[0].subtype,0x81);
  assert.equal(out.offer.data[0].size,PLAY28_RECORDINGS_4M_SIZE);
  assert.equal(out.offer.data[0].initialImageSize,0);
  assert.equal(out.offer.data[0].sha256,undefined,"blank DATA must not be copied or hashed as initial payload");
  console.log("Phone install offer integration: PASS (4 MiB DATA allocation; zero initial DATA bytes)");
} finally {
  globalThis.fetch=originalFetch;
}
