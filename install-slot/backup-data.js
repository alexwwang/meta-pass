// install-slot/backup-data.js —— M5 数据级备份/导入（设计 §12）
//
// 与 slot-backup.js 的区别：
//   - slot-backup: 整槽镜像（含 firmware + tail + extra）
//   - backup-data: 仅归档数据记录（ARCHIVED state），带 firmware_version 校验
//
// 备份格式（二进制，设计 §12.1）：
//   [magic:4][version:4][play_id:4][firmware_version:32][data_count:4][data records...]
//
// 导入流程（设计 §12.2）：
//   1. 读取备份头，验证 magic/version
//   2. 严格匹配 firmware_version
//   3. 检查 pool 空间
//   4. 写入数据记录

export const BACKUP_MAGIC = 0x4254504D; // 'MPTB' little-endian
export const BACKUP_VERSION = 1;
export const BACKUP_VERSION_MAX = 32;
export const BACKUP_DATA_MAX = 8;

// 单条数据记录（与设备端 meta_backup_data_t 对齐）
export function serializeDataRecord(rec) {
  const { play_id, offset, size, state, type, subtype, label } = rec;
  const labelBytes = new TextEncoder().encode(label.padEnd(16, '\0').slice(0, 16));
  const buf = new ArrayBuffer(4 + 4 + 4 + 1 + 1 + 1 + 1 + 16);  // 32B(此前少算 reserved 1B,从未运行故未爆)
  const view = new DataView(buf);
  view.setUint32(0, play_id, true);   // little-endian
  view.setUint32(4, offset, true);
  view.setUint32(8, size, true);
  view.setUint8(12, state);
  view.setUint8(13, type);
  view.setUint8(14, subtype);
  view.setUint8(15, 0); // reserved
  for (let i = 0; i < 16; i++) view.setUint8(16 + i, labelBytes[i] ?? 0);
  return new Uint8Array(buf);
}

export function deserializeDataRecord(bytes, offset = 0) {
  const view = new DataView(bytes.buffer, bytes.byteOffset + offset, 32);
  return {
    play_id: view.getUint32(0, true),
    offset: view.getUint32(4, true),
    size: view.getUint32(8, true),
    state: view.getUint8(12),
    type: view.getUint8(13),
    subtype: view.getUint8(14),
    label: new TextDecoder().decode(bytes.slice(offset + 16, offset + 32)).replace(/\0+$/, ''),
  };
}

// 序列化备份头
export function serializeHeader(play_id, firmware_version, data_count) {
  const versionBytes = new TextEncoder().encode(firmware_version.padEnd(32, '\0').slice(0, 32));
  const buf = new ArrayBuffer(4 + 4 + 4 + 32 + 4);
  const view = new DataView(buf);
  view.setUint32(0, BACKUP_MAGIC, true);
  view.setUint32(4, BACKUP_VERSION, true);
  view.setUint32(8, play_id, true);
  for (let i = 0; i < 32; i++) view.setUint8(12 + i, versionBytes[i] ?? 0);
  view.setUint32(44, data_count, true);
  return new Uint8Array(buf);
}

// 解析备份头
export function parseHeader(bytes) {
  if (bytes.length < 48) return null;
  const view = new DataView(bytes.buffer, bytes.byteOffset, 48);
  const magic = view.getUint32(0, true);
  if (magic !== BACKUP_MAGIC) return null;
  const version = view.getUint32(4, true);
  if (version !== BACKUP_VERSION) return null;
  const play_id = view.getUint32(8, true);
  if (play_id === 0) return null;
  const firmware_version = new TextDecoder().decode(bytes.slice(12, 44)).replace(/\0+$/, '');
  if (firmware_version.length === 0) return null;
  const data_count = view.getUint32(44, true);
  if (data_count > BACKUP_DATA_MAX) return null;
  return { play_id, firmware_version, data_count };
}

// 解析完整备份（头 + 数据记录）
export function parseBackup(bytes) {
  const header = parseHeader(bytes);
  if (!header) return { ok: false, reason: 'invalid magic or version' };
  
  const records = [];
  let offset = 48;
  for (let i = 0; i < header.data_count; i++) {
    if (offset + 32 > bytes.length) {
      return { ok: false, reason: 'truncated data record' };
    }
    const rec = deserializeDataRecord(bytes, offset);
    if (!rec || rec.play_id === 0) {
      return { ok: false, reason: 'invalid data record' };
    }
    records.push(rec);
    offset += 32;
  }
  
  return { ok: true, play_id: header.play_id, firmware_version: header.firmware_version, records };
}

// 序列化完整备份
export function serializeBackup(play_id, firmware_version, records) {
  if (records.length > BACKUP_DATA_MAX) {
    return null;
  }
  const header = serializeHeader(play_id, firmware_version, records.length);
  const parts = [header];
  for (const rec of records) {
    parts.push(serializeDataRecord(rec));
  }
  return new Uint8Array(parts.flatMap((p) => Array.from(p)));
}

// 导出：从手机获取设备上的归档数据并生成备份
export async function exportBackup(bridge, play_id) {
  // 1. 获取当前固件版本
  const status = await bridge.status();
  if (!status || !status.firmware_version) {
    return { ok: false, reason: 'cannot read firmware version' };
  }
  
  // 2. 获取 carve 状态（包含数据记录）
  const slots = await bridge.slots();
  if (!slots || !slots.data || slots.data.length === 0) {
    return { ok: false, reason: 'no archived data found' };
  }
  
  // 3. 过滤出指定 play_id 的归档数据
  const archived = slots.data.filter(d => 
    d.play_id === play_id && d.state === 2 // META_DATA_ARCHIVED = 2
  );
  
  if (archived.length === 0) {
    return { ok: false, reason: 'no archived data for this play' };
  }
  
  // 4. 序列化备份(type/subtype 设备侧恒为 1/1,清单模型不依赖,补默认值)
  const backup = serializeBackup(play_id, status.firmware_version,
    archived.map((d) => ({ type: 1, subtype: 1, ...d })));
  if (!backup) {
    return { ok: false, reason: 'backup serialization failed' };
  }
  
  // 5. 下载为文件
  const blob = new Blob([backup], { type: 'application/octet-stream' });
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = `metapass-backup-${play_id}-${Date.now()}.bin`;
  document.body.appendChild(a);
  a.click();
  document.body.removeChild(a);
  URL.revokeObjectURL(url);
  
  return { ok: true, count: archived.length };
}

// 由 parseBackup 结果构造导入载荷。键名与设备 h_backup_import 的解析器
// 逐字对应("data" 数组,元素 offset/size/state/label)——改任何一侧都必须
// 同步另一侧;host 测试钉死该契约。
export function buildImportPayload(parsed) {
  return JSON.stringify({
    play_id: parsed.play_id,
    firmware_version: parsed.firmware_version,
    data: parsed.records.map((r) => ({
      offset: r.offset,
      size: r.size,
      state: r.state,
      label: r.label,
    })),
  });
}

// 导入：上传备份文件到设备
export async function importBackup(bridge, file, firmwareVersion) {
  // 1. 读取文件
  const bytes = new Uint8Array(await file.arrayBuffer());
  
  // 2. 解析备份
  const parsed = parseBackup(bytes);
  if (!parsed.ok) {
    return { ok: false, reason: `backup parse failed: ${parsed.reason}` };
  }
  
  // 3. 严格版本匹配
  if (parsed.firmware_version !== firmwareVersion) {
    return {
      ok: false,
      reason: 'version mismatch',
      details: {
        backup: parsed.firmware_version,
        device: firmwareVersion,
      },
    };
  }
  
  // 4. 调用设备导入 API(载荷键名与设备解析器逐字对应)
  const payload = buildImportPayload(parsed);
  
  const result = await fetch(`${bridge.origin}/api/backup/import`, {
    method: 'POST',
    headers: {
      'Content-Type': 'application/json',
      'X-Meta-Token': bridge.token(),
    },
    body: payload,
  });
  
  if (!result.ok) {
    const text = await result.text();
    return { ok: false, reason: `import failed: ${text}` };
  }
  
  const response = await result.json();
  return { ok: true, ...response };
}
