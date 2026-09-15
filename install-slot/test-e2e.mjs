// 端到端测试：用实际安装器解析 signed.bin → 模拟 flash → 用公钥验签
import { readFileSync } from "fs";
import { createHash } from "crypto";
import { extractAppImage } from "./extract-app-image.js";

const signed = readFileSync("/Users/alex/ai-passport/pass-radar/build/pass-radar_v0.1-2-g8fcce59-signed.bin");
console.log(`signed.bin: ${signed.length} bytes`);

// === 步骤1: 用实际安装器解析 ===
const img = extractAppImage(signed);
console.log(`\n=== 安装器解析 ===`);
console.log(`image.length: ${img.length}`);
console.log(`image.data: ${img.data.length} bytes`);
console.log(`tailSectorOffset: ${img.tailSectorOffset}`);
console.log(`tailSector: ${img.tailSector ? img.tailSector.length : "NULL"} bytes`);
if (img.tailSector) {
  console.log(`MSIG magic: ${img.tailSector.slice(0,4).toString("ascii")}`);
}

// === 步骤2: 提取签名信息 ===
const tail = img.tailSector;
if (!tail) {
  console.log("\n❌ 没有 tailSector！");
  process.exit(1);
}

const payloadLen = tail.readUInt32LE(4);
const sig = tail.subarray(8, 8 + payloadLen);
console.log(`\n=== 签名信息 ===`);
console.log(`payload_len: ${payloadLen}`);
console.log(`sig: ${sig.toString("hex")}`);

// XOR 校验
const xorOff = 8 + payloadLen;
let xorCalc = 0;
for (let i = 0; i < xorOff; i++) xorCalc ^= tail[i];
const xorStored = tail[xorOff];
console.log(`XOR calc: 0x${xorCalc.toString(16)}`);
console.log(`XOR stored: 0x${xorStored.toString(16)}`);
console.log(`XOR: ${xorCalc === xorStored ? "PASS" : "FAIL"}`);

// === 步骤3: 模拟写入 flash ===
// 安装器写: image.data → address, tailSector → address + tailSectorOffset
const totalSize = img.tailSectorOffset + 4096;
const flash = Buffer.alloc(totalSize, 0xff);
flash.set(img.data, 0);
flash.set(tail, img.tailSectorOffset);

console.log(`\n=== Flash 内容 ===`);
console.log(`flash size: ${totalSize}`);
console.log(`flash[0:${img.length}]: image data (${img.length} bytes)`);
console.log(`flash[${img.length}:${img.tailSectorOffset}]: padding (not written by installer)`);
console.log(`flash[${img.tailSectorOffset}:${totalSize}]: tail sector (${tail.length} bytes)`);

// === 步骤4: 模拟设备读 flash，计算 digest ===
// IDF esp_image_verify 返回 image_len
// 设备用 slot_sha256(part, image_len) 计算 digest
const idfImageLen = 962416; // 从 IDF 计算得出
const deviceDigest = createHash("sha256").update(flash.subarray(0, idfImageLen)).digest();
console.log(`\n=== 设备侧 ===`);
console.log(`IDF image_len: ${idfImageLen}`);
console.log(`device digest: ${deviceDigest.toString("hex")}`);

// 验证：deviceDigest 应该等于签名时的 digest
const expectedDigest = createHash("sha256").update(signed.subarray(0, idfImageLen)).digest();
console.log(`signed digest: ${expectedDigest.toString("hex")}`);
console.log(`digest match: ${deviceDigest.equals(expectedDigest) ? "PASS" : "FAIL"}`);

// === 步骤5: ECDSA 验签 ===
// 读取固件公钥
const { default: crypto } = await import("crypto");
const pubKeyDer = Buffer.from([
  0x30,0x59,0x30,0x13,0x06,0x07,0x2a,0x86,0x48,0xce,0x3d,0x02,0x01,0x06,
  0x08,0x2a,0x86,0x48,0xce,0x3d,0x03,0x01,0x07,0x03,0x42,0x00,0x04,0x29,
  0x9a,0x8d,0xd1,0x1a,0x9c,0xf3,0xbb,0x4a,0xa5,0xe5,0x11,0xa7,0x88,0xa7,
  0x44,0x57,0xce,0x42,0x9e,0xba,0xf0,0xa5,0xcb,0xd9,0x1b,0x43,0x6a,0xf5,
  0x8b,0x43,0xfb,0xb9,0xcf,0x96,0x48,0x0e,0x4b,0xb8,0x9f,0x09,0x2e,0x40,
  0x73,0x33,0xb8,0x1e,0xcd,0x52,0x68,0xe4,0x37,0x7a,0xc4,0xcc,0xb0,0x42,
  0xe5,0x37,0x80,0xab,0x65,0x1d,0x13
]);

const pubKey = crypto.createVerify("SHA256");
pubKey.init(pubKeyDer, crypto.constants.KEY_OBJECT_TYPE.EC);

// mbedTLS 语义：对 digest 直接验签（prehashed）
// Node.js crypto 没有直接 prehashed 验签，用 DER 签名 + createVerify
// 需要手动实现 ECDSA 验签

// 用 openssl 命令验证
const { execSync } = await import("child_process");
import { writeFileSync } from "fs";

writeFileSync("/tmp/e2e_sig.der", sig);
writeFileSync("/tmp/e2e_digest.bin", deviceDigest);
writeFileSync("/tmp/e2e_pub.der", pubKeyDer);

try {
  execSync('openssl pkeyutl -verify -pubin -inkey /tmp/e2e_pub.der -sigfile /tmp/e2e_sig.der -in /tmp/e2e_digest.bin -rawin', { stdio: "pipe" });
  console.log(`\n✅ ECDSA 验签: PASS`);
  console.log(`🎉 端到端测试通过！`);
} catch (e) {
  console.log(`\n❌ ECDSA 验签: FAIL`);
  console.log(`  ${e.stderr?.toString() || e.message}`);
}
