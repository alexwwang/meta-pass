# DATA 字节级备份与恢复设计

<p align="right">
  <a href="data-byte-backup-restore-design.md">English</a> · <strong>简体中文</strong>
</p>

日期：2026-10-11  
分支：`feat/storage`

## 目标与当前状态

统一 USB 服务页与手机内嵌页的 DATA 生命周期语义：安装时创建 DATA、备份实际字节、恢复实际字节、卸载 APP 时删除关联 DATA。

**当前状态：尚未完成端到端备份/恢复。** USB ZIP 当前只备份 APP 槽位镜像和可选 NVS，不包含 DATA 字节；手机空间管理页也明确提示这一限制。新增的手机端 DATA 只读 API 只是后续字节级导出的基础能力，不代表已经有完整归档、恢复或回滚。

## 不可破坏的约束

- 不移动 `cardid`、`store`，不改变固定分区布局。
- 所有 DATA 地址必须来自已验证的 carve 记录；客户端不能提交任意 Flash 地址。
- DATA 所有权键为 `(play_id, label)`，不能用槽位数组下标表示所有权。
- 卸载顺序保持“先提交 APP + DATA 元数据删除，再擦除关联字节”；若设备端卸载恢复状态不确定，拒绝并发写入。
- 不允许把仅含元数据的清单称为数据备份；不得在字节未读回验证时提示备份/恢复成功。
- 不支持对存有用户数据的设备进行自动化破坏性测试。

## 归档格式建议：MPTB v2 ZIP

沿用现有 ZIP 容器与 manifest，但提升 manifest 版本；v1 仍只表示 APP/NVS 备份，不能假定它含 DATA。

```json
{
  "manifest_version": 2,
  "kind": "meta-pass-slot-backup",
  "created_utc": "ISO-8601",
  "slots": [
    {
      "play_id": 123,
      "source_slot": 1,
      "name": "example",
      "files": [{"name": "slot-1.bin", "bytes": 12345, "sha256": "...", "type": "app"}]
    }
  ],
  "data": [
    {
      "play_id": 123,
      "label": "cfg_save",
      "type": 1,
      "subtype": 64,
      "state": 1,
      "bytes": 65536,
      "sha256": "...",
      "file": "data/123/cfg_save.bin"
    }
  ]
}
```

Manifest 不保存可直接用于写入的源 Flash 绝对地址。归档文件名须安全规范化；标签只作为逻辑身份，不可直接作为未转义路径。

## 备份流程

1. 读取设备槽位和 DATA reservations，检查 carve 几何与安装器状态。
2. 对每个 DATA reservation 按 `(play_id, label)` 读取真实字节，使用小块流式读取，单块上限 4 KiB。
3. 客户端对完整内容计算 SHA-256，并将实际字节与摘要写入 ZIP；manifest 中保存长度、摘要、标签、类型/子类型及状态。
4. 再次读取 reservation 清单，确认读取期间身份、offset、size 没变化；若变化，废弃整份归档。
5. 只有 APP 文件和所有关联 DATA 都完整、长度和 SHA-256 校验通过时，才标记为“完整备份”。部分成功必须明确标记，不得误称完整备份。

## 恢复流程

1. 在设备写入前完整解析归档，验证版本、所有文件存在性、长度、SHA-256、重复身份、文件路径、分区大小粒度和记录上限。
2. 要求每组 DATA 都能关联到归档中的同一 `play_id` APP；如果目标设备上已存在该 `play_id`，拒绝覆盖或要求显式卸载/替换策略，不允许静默覆盖正在使用的数据。
3. 使用共享分配器为 DATA 选择新地址，不复用归档中的旧地址；检查总容量、连续空间、记录数量和分区表容量。
4. 先写 DATA 到尚未登记的暂存范围，再逐块读回并计算 SHA-256；任何不匹配都停止并尽力擦除暂存范围。
5. 写入 APP 镜像并完成 APP 验证后，以单一 carve/table 提交发布 APP 与 DATA 所有权。元数据提交前失败可擦除暂存区；提交已开始但结果不确定时保留暂存字节并进入恢复检查，不能盲目擦除。
6. 提交后重新读取槽表和 DATA reservations，验证所有权、几何、大小与非重叠；再执行一次字节校验。仅此后报告成功。

## 手机 API 合约

已新增只读端点：

- `GET /api/install/data/read`
- 必须带有效 `X-Meta-Session`，并通过设备既有 Origin 校验。
- 请求头：`X-Meta-Play-Id`、`X-Meta-Data-Label`、`X-Meta-Offset`（相对该 DATA 分区的偏移）、`X-Meta-Length`。
- 单次读取长度为 1–4096 字节；服务器按 `(play_id, label)` 找到 carve 记录后再校验范围。请求中没有任意 Flash 地址参数。
- 成功返回 `application/octet-stream`；失败返回非 2xx。
- 手机 `DeviceBridge.dataRead(playId, label, offset, length)` 返回二进制字节数组及 HTTP 状态。

仍需实现：

- 手机端分块归档 UI、完整长度/摘要校验和文件导出。
- 恢复端点及其授权、容量预检、暂存写入、读回校验与原子元数据提交。
- USB 页面在 ZIP v2 中导出和恢复真实 DATA 字节，并与手机格式共用 manifest 解析/校验模块。
- 对设备端 API、USB ZIP 和手机 UI 增加可重复的模拟设备测试及专用真机测试。

## 验收矩阵

| 场景 | 必须结果 |
|---|---|
| DATA 读取范围超出 reservation | 拒绝；不返回相邻 Flash 字节 |
| 缺 token / 错 Origin / 错 play_id / 错 label | 拒绝 |
| 长度为 0 或大于 4 KiB | 拒绝 |
| 归档缺文件、长度不符、SHA-256 不符 | 恢复前拒绝，不写 Flash |
| DATA 总容量足够但连续空间不足 | 预检拒绝，不写 Flash |
| DATA 记录数超上限 | 预检拒绝，不写 Flash |
| 暂存写入或读回失败 | 不提交所有权；可确认范围内尽力清理暂存字节 |
| 元数据提交结果不确定 | 保留暂存数据，报告状态不确定并要求恢复检查 |
| 恢复后重启 | DATA 字节摘要一致，APP 仍能启动 |
| 卸载 APP | 关联 DATA 记录先删除，随后擦除；其他 APP 的 DATA 保留 |
| v1 归档 | 明确显示“不包含 DATA”，不得静默当作完整备份 |

## 本次实现边界

当前代码已实现手机端受 token/Origin 保护、按 DATA 所有权键控、4 KiB 有界、相对偏移范围校验的只读字节 API，以及 `DeviceBridge.dataRead` 二进制桥接。完整备份/恢复 UI 和写入事务仍未实现；CI 通过也不等于真机读回或重启持久性验证通过。
