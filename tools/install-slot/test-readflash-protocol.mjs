// tools/install-slot/test-readflash-protocol.mjs — readFlash 协议修复的单测(Node)。
// 运行:node tools/install-slot/test-readflash-protocol.mjs
//
// 覆盖两个修复面:
//   1. vendor/md5.js —— RFC 1321 标准向量 + 与 Node crypto MD5 交叉验证
//      (含非 64 倍数长度,防补位/长度编码错误);
//   2. vendor/esptool-js.js readFlash —— 对齐 esptool.py 官方协议:
//      a) 每个数据帧 ACK 一次(累计字节数,不再是按窗口);
//      b) 数据帧收完后读取 stub 无条件追加的 16B MD5 digest 帧并校验
//         (stub_commands.c: MD5Final + SLIP_send,无条件);
//      c) digest 篡改 → 报错(而不是静默吞下坏数据);
//      d) digest 缺失 → 报错(协议错位宁可失败也不带病运行)。
//
// 测试不依赖真机:Transport 用内存 mock,按 stub 的真实行为(4KB 数据帧 + 结尾
// digest 帧)回放;ESPLoader 只实例化到能用 readFlash 的最小状态。

import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { readFileSync, writeFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import { tmpdir } from "node:os";

const here = dirname(fileURLToPath(import.meta.url));
const vendor = join(here, "..", "..", "install-slot", "vendor");

// ---- 被测模块 ----
const md5 = (await import("file://" + join(vendor, "md5.js"))).default;
const esptoolSrc = readFileSync(join(vendor, "esptool-js.js"), "utf8");

// 用临时文件加载 vendored ESPLoader:把两个相对 import 重写为 file:// 绝对路径,
// 写入 os.tmpdir 后动态 import(兼容 Node 与 Bun——Bun 拒绝超长 data: URL)。
const rewritten = esptoolSrc
  .replaceAll('from"./pako.js"', `from"file://${join(vendor, "pako.js").split("\\").join("/")}"`)
  .replaceAll('from"./atob-lite.js"', `from"file://${join(vendor, "atob-lite.js").split("\\").join("/")}"`);
const tmpLoader = join(tmpdir(), `esptool-js-test-${process.pid}.mjs`);
writeFileSync(tmpLoader, rewritten);
const { ESPLoader } = await import(
  "file://" + tmpLoader.split("\\").join("/")
);

// ---------- 1) MD5 正确性 ----------
console.log("1. vendor/md5.js 正确性");
{
  // RFC 1321 标准向量
  const vectors = [
    ["", "d41d8cd98f00b204e9800998ecf8427e"],
    ["a", "0cc175b9c0f1b6a831c399e269772661"],
    ["abc", "900150983cd24fb0d6963f7d28e17f72"],
    ["message digest", "f96b697d7cb7938d525a2f31aaf161d0"],
    ["abcdefghijklmnopqrstuvwxyz", "c3fcd3d76192e4007dfb496cca67e13b"],
    [
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789",
      "d174ab98d277d9f5a5611c2c9f419d9f",
    ],
    [
      "12345678901234567890123456789012345678901234567890123456789012345678901234567890",
      "57edf4a22be3c955ac49da2e2107b67a",
    ],
  ];
  for (const [msg, want] of vectors) {
    const got = Buffer.from(md5(Buffer.from(msg))).toString("hex");
    assert.equal(got, want, `RFC1321 向量失败: ${JSON.stringify(msg)}`);
  }
  // 与 Node crypto 交叉验证(含非 64 倍数、跨块长度)
  for (const len of [1, 55, 56, 63, 64, 65, 127, 128, 1000, 65536, 4096 * 16 + 3]) {
    const data = Buffer.alloc(len);
    for (let i = 0; i < len; i++) data[i] = (i * 7 + len) & 0xff;
    assert.equal(
      Buffer.from(md5(data)).toString("hex"),
      createHash("md5").update(data).digest("hex"),
      `crypto 对拍失败: len=${len}`,
    );
  }
  console.log("   PASS: RFC 1321 向量 7/7 + crypto 交叉验证 11/11");
}

// ---------- 2) readFlash 协议行为(mock Transport) ----------
// 模拟 stub 真实行为:收到 0xD2 命令后按在途窗口突发 4KB SLIP 数据帧,
// 每收到一个 ACK(4 字节累计)续发,数据发完且全部确认后追加 16B MD5 digest 帧
// (stub_commands.c: MD5Final + SLIP_send,无条件)。read() 与真实 Transport 一致:
// 按完整 SLIP 帧解码后 yield Uint8Array。
function makeStubTransport(data, { tamperDigest = false, omitDigest = false, ackLog } = {}) {
  const SLIP_END = 0xc0, SLIP_ESC = 0xdb, SLIP_ESC_END = 0xdc, SLIP_ESC_ESC = 0xdd;
  const slip = (bytes) => {
    const out = [SLIP_END];
    for (const b of bytes) {
      if (b === SLIP_END) out.push(SLIP_ESC, SLIP_ESC_END);
      else if (b === SLIP_ESC) out.push(SLIP_ESC, SLIP_ESC_ESC);
      else out.push(b);
    }
    out.push(SLIP_END);
    return out;
  };
  let sent = 0;          // stub 已发出字节数(num_sent)
  let acked = 0;         // 主机已确认字节数(num_acked)
  let phase = "wait_cmd"; // wait_cmd -> data -> digest/done
  const digest = md5(data);
  if (tamperDigest) digest[0] ^= 0xff;
  const pending = [];    // 线上待读原始字节(SLIP 帧流)

  // stub 主循环:在途未确认 < max_in_flight(64 帧 × 4KB)就继续发;
  // 全部发出且全部确认 → 发 digest 帧(omitDigest 则不发)。phase=done 后
  // pending 中剩余帧(如 digest)仍可被读走,读完即终止。
  const streamFrames = () => {
    while (sent < data.length && sent - acked < 64 * 4096) {
      const n = Math.min(4096, data.length - sent);
      pending.push(...slip(data.subarray(sent, sent + n)));
      sent += n;
    }
    if (sent >= data.length && acked >= data.length && phase === "data") {
      if (!omitDigest) pending.push(...slip(digest));
      phase = "done";
    }
  };

  // 读端:从 pending 提取一个完整 SLIP 帧并解码(模拟 Transport.read 语义)
  let frame = [], started = false, esc = false;
  const tryFrame = () => {
    while (pending.length > 0) {
      const c = pending.shift();
      if (!started) {
        if (c === SLIP_END) { started = true; frame = []; }
        continue;
      }
      if (c === SLIP_END) {
        const out = Uint8Array.from(frame);
        frame = []; started = false;
        return out;
      }
      if (esc) {
        frame.push(c === SLIP_ESC_END ? SLIP_END : c === SLIP_ESC_ESC ? SLIP_ESC : c);
        esc = false;
      } else if (c === SLIP_ESC) esc = true;
      else frame.push(c);
    }
    return null;
  };

  return {
    pending,
    async *read() {
      for (;;) {
        const f = tryFrame();
        if (f) { yield f; continue; }
        if (phase === "done") return; // 无帧可读且 stub 已结束 → undefined(模拟超时空读)
        await new Promise((r) => setTimeout(r, 1));
      }
    },
    async write(bytes) {
      if (bytes.length === 4) {
        // ACK:累计字节数,LE32(stub 侧 SLIP_recv 裸读 4 字节)
        acked = bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | (bytes[3] << 24);
        ackLog?.push(acked);
        if (phase === "data") streamFrames();
      } else if (bytes.length > 8 && bytes[1] === 0xd2) {
        // ESP_READ_FLASH 命令 → 首波突发
        phase = "data";
        streamFrames();
      }
    },
    async connect() {},
    async disconnect() {},
  };
}

// 构造一个只跑 readFlash 所需路径的 ESPLoader
function makeLoader(transport) {
  const l = Object.create(ESPLoader.prototype);
  l.transport = transport;
  l.FLASH_READ_TIMEOUT = 100000;
  l.ESP_READ_FLASH = 0xd2;
  // checkCommand 最小实现:真实组包并发到 transport(stub mock 靠 0xD2 触发数据阶段),
  // 响应帧直接视为成功(本测试聚焦 readFlash 的数据/ACK/digest 协议,不含命令响应解析)。
  l.checkCommand = async (_desc, op, data) => {
    const pkt = new Uint8Array(8 + data.length);
    pkt[1] = op;
    pkt.set(data, 8);
    await transport.write(pkt);
    return 0;
  };
  l._intToByteArray = function (n) {
    return Uint8Array.from([n & 0xff, (n >>> 8) & 0xff, (n >>> 16) & 0xff, (n >>> 24) & 0xff]);
  };
  l._appendArray = function (a, b) {
    const out = new Uint8Array(a.length + b.length);
    out.set(a, 0);
    out.set(b, a.length);
    return out;
  };
  return l;
}

console.log("2. readFlash 协议行为(esptool.py 对齐)");
{
  const data = Buffer.alloc(0x4000); // 16KB = 4 个 4KB 数据帧
  for (let i = 0; i < data.length; i++) data[i] = i & 0xff;

  // a+b) 正常路径:数据正确 + 每帧 ACK + digest 被读走
  {
    const ackLog = [];
    const t = makeStubTransport(data, { ackLog });
    const l = makeLoader(t);
    const got = await l.readFlash(0, data.length);
    assert.equal(Buffer.compare(Buffer.from(got), data), 0, "读回数据不一致");
    // 4 帧 → 4 次 ACK,且为累计值(256 段递增到 0x4000)
    assert.ok(ackLog.length >= 4, `ACK 次数不足:${ackLog.length}`);
    assert.equal(ackLog.at(-1), data.length, "最终 ACK 未覆盖全部数据");
    // digest 帧被读走:阶段推进到 idle 后不再有 pending
    assert.equal(t.pending.length, 0, "digest 帧残留在传输缓冲(协议错位未修)");
    console.log("   PASS: 数据一致,逐帧 ACK(累计),digest 帧已读走不残留");
  }

  // c) digest 篡改 → 报错
  {
    const t = makeStubTransport(data, { tamperDigest: true });
    const l = makeLoader(t);
    await assert.rejects(() => l.readFlash(0, data.length), /MD5 digest mismatch/);
    console.log("   PASS: digest 篡改被拒(坏数据不再静默通过)");
  }

  // d) digest 缺失 → 报错
  {
    const t = makeStubTransport(data, { omitDigest: true });
    const l = makeLoader(t);
    await assert.rejects(() => l.readFlash(0, data.length), /Expected 16-byte MD5 digest frame/);
    console.log("   PASS: digest 缺失被拒(宁可失败,不残留下一条命令的毒帧)");
  }
}

console.log("\nAll readFlash protocol tests passed.");
