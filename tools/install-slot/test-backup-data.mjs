#!/usr/bin/env node
// tools/install-slot/test-backup-data.mjs —— M5 数据备份清单格式与导入载荷契约。
// 运行:node tools/install-slot/test-backup-data.mjs(已接入 tools/validate.sh --static)
//
// 钉死两层契约:
//   1. MPTB 清单二进制:serializeBackup → parseBackup 往返字节级一致
//      (与设备端 meta_backup.c 同布局:头 48B + 记录 32B)。
//   2. 导入载荷 JSON:buildImportPayload 的键名与设备 h_backup_import
//      的手写解析器逐字对应("data" 数组;元素 offset/size/state/label)。
//      2026-10-04 曾在此断链(手机发 "records",设备只认 "data")——
//      没有 UI 接线时两端各自"正确",联调才爆;本测试把断链钉成不可能。
import assert from "node:assert/strict";
import { fileURLToPath } from "node:url";
import path from "node:path";

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..", "..");
const bk = await import(path.join(ROOT, "install-slot", "backup-data.js"));

// ── 1. MPTB 往返:头 + 两条记录字节级一致 ─────────────────────────────────
{
  const records = [
    { play_id: 42, offset: 0x1A0000, size: 0x2000, state: 2, type: 1, subtype: 1, label: "rec" },
    { play_id: 42, offset: 0x2B0000, size: 0x1000, state: 2, type: 1, subtype: 1, label: "cfg_save" },
  ];
  const bytes = bk.serializeBackup(42, "1.2.3-dirty", records);
  assert.ok(bytes instanceof Uint8Array);
  assert.equal(bytes.length, 48 + 2 * 32);
  const parsed = bk.parseBackup(bytes);
  assert.equal(parsed.ok, true);
  assert.equal(parsed.play_id, 42);
  assert.equal(parsed.firmware_version, "1.2.3-dirty");
  assert.equal(parsed.records.length, 2);
  assert.deepEqual(
    parsed.records.map((r) => [r.play_id, r.offset, r.size, r.state, r.label]),
    [[42, 0x1A0000, 0x2000, 2, "rec"], [42, 0x2B0000, 0x1000, 2, "cfg_save"]],
  );
  console.log("PASS 1: MPTB serialize/parse roundtrip byte-exact");
}

// ── 2. 坏输入拒绝:魔数/版本/truncated/坏记录 ──────────────────────────────
{
  const good = bk.serializeBackup(7, "v1", [
    { play_id: 7, offset: 0x1000, size: 0x1000, state: 2, type: 1, subtype: 1, label: "a" },
  ]);
  const badMagic = good.slice(); badMagic[0] ^= 0xFF;
  assert.equal(bk.parseBackup(badMagic).ok, false);

  const badVer = good.slice(); badVer[4] = 99;
  assert.equal(bk.parseBackup(badVer).ok, false);

  assert.equal(bk.parseBackup(good.subarray(0, 48 + 16)).ok, false, "truncated record");

  const zeroPid = bk.serializeBackup(0, "v1", []);
  assert.equal(bk.parseBackup(zeroPid).ok, false, "play_id 0 rejected");

  const tooMany = Array.from({ length: bk.BACKUP_DATA_MAX + 1 }, (_, i) => ({
    play_id: 7, offset: 0x1000 + i * 0x1000, size: 0x1000, state: 2, type: 1, subtype: 1, label: "x",
  }));
  assert.equal(bk.serializeBackup(7, "v1", tooMany), null, "> MAX records rejected");
  console.log("PASS 2: malformed backups rejected (magic/version/truncation/pid/count)");
}

// ── 3. 导入载荷契约:键名与设备解析器逐字对应 ───────────────────────────────
{
  const records = [
    { play_id: 9, offset: 0x1A0000, size: 0x2000, state: 2, label: "rec" },
    { play_id: 9, offset: 0x2B0000, size: 0x1000, state: 2, label: "cfg" },
  ];
  const payload = JSON.parse(bk.buildImportPayload({
    play_id: 9, firmware_version: "1.2.3", records,
  }));
  // 设备 h_backup_import 手写解析器认的键(顺序无关,但名必须逐字一致):
  assert.equal(typeof payload.play_id, "number");
  assert.equal(typeof payload.firmware_version, "string");
  assert.ok(Array.isArray(payload.data), 'key must be "data" (device parser rejects "records")');
  assert.equal(payload.data.length, 2);
  for (const rec of payload.data) {
    assert.ok(Number.isInteger(rec.offset), "device parser reads \"offset\"");
    assert.ok(Number.isInteger(rec.size), "device parser reads \"size\"");
    assert.ok(Number.isInteger(rec.state), "device parser reads \"state\"");
    assert.equal(typeof rec.label, "string", "device parser reads \"label\"");
  }
  // 载荷可被设备 strstr 解析器走通(字段序:offset/size/state/label 都在):
  const body = JSON.stringify(payload);
  for (const key of ['"play_id"', '"firmware_version"', '"data"', '"offset"', '"size"', '"state"', '"label"']) {
    assert.ok(body.includes(key), `payload missing ${key}`);
  }
  console.log("PASS 3: import payload keys match device parser contract (data[], offset/size/state/label)");
}

console.log("ALL backup-data TESTS PASSED");
