<p align="right">
  <a href="storage-snapshot-implementation-status.md">English</a> · <strong>简体中文</strong>
</p>

# 存储快照实现状态（2026-10-14）

分支：`feat/storage`。

## 当前分支已核验的事实

- 本轮新增代码提交：`ea48b9ca`（宿主恢复协调器）、`488017ce`（fail-closed 契约测试）、`5e4a530b`（CI 登记测试）、`c419d117`（禁止向设备传送绝对分区几何）、`eeece85a`（协调器协议测试）。当前 HEAD 的 CI 尚在运行，不能沿用旧提交的通过状态。
- 该提交的 GitHub Actions **Static checks** 与 **Firmware checks** 均已通过。
- `install-slot/storage-snapshot.js` 提供 v2 快照 manifest、分区几何/保护范围/extent 校验、ZIP 编解码、SHA-256 校验、擦除态稀疏表示，以及不修改设备的恢复计划生成。新增 `install-slot/storage-snapshot-restore.js` 定义宿主侧恢复协调协议：先完整校验归档与目标布局，再要求设备事务 API 分阶段暂存、验证、提交并报告写后状态；设备 API 缺失时立即 fail-closed。
- 设备端提供鉴权、同源的只读分区清单和分区相对偏移读取 API；禁止读取当前运行的宿主分区和 `cardid`，调用方不能提交绝对 Flash 地址作为读取目标。
- 手机内嵌页支持 v2 整设备快照备份，并显示敏感数据警告。恢复目前严格限制为预检：验证归档和目标布局，不写 Flash。
- USB 页面保留历史 v1.1 固定槽归档路径。NVS 恢复写入有明确的契约测试；不能用 v2 快照解析替换这条旧版兼容路径。此次还修复了不完整槽位备份仍可能被当成完整 v1 归档的问题：manifest 现在记录 `complete=false` 和缺失槽位，恢复入口在写入前拒绝不完整归档。
- 较早版本的真机报告只作为历史证据，不能证明当前 HEAD 的 v2 快照行为。本轮没有修改 USB v1.1 兼容分支，因此不把 v1.1 备份/恢复列为本轮必测项；只有后续代码差异实际触及该分支时，才追加针对性回归。
- 本轮自动化测试聚焦 v2 快照：归档 discriminator、payload 完整性、目标布局和只读恢复预检。USB v1.1 既有分支不变，不纳入本轮专项回归；手机页面只接受 v2 整设备快照 discriminator。
- 既有 USB v1.1 兼容 handoff 保留为历史维护资料，但不是本轮真机测试任务；本轮真机测试只针对已经实现的 v2 快照备份/预检，并明确标记尚未实现的写入式恢复。

## 尚未完成 / 当前阻塞

- **USB v2 快照备份仍 fail-closed。** USB 串口/bootloader 通路还不能可信地获得当前运行宿主分区的设备报告。不得根据静态分区表、硬编码地址或猜测 OTA 状态推断。
- **两端仍未开放 v2 快照写入式恢复。** 宿主侧协调器与 mock 协议测试已补齐，但固件尚未实现 `beginSnapshotRestore`、`stageSnapshotRestoreChunk`、`verifyStagedSnapshotRestore`、`commitSnapshotRestore`、`getSnapshotRestoreStatus` 对应的持久化事务 API。因此协调器不会被页面启用，实际恢复仍停留在预检；不得增加任意地址写入/擦除接口。
- USB 历史 v1.1 归档是独立的既有兼容路径，本轮未改动；不因此触发 v1.1 专项回归。不要把 v1 归档支持扩展到手机页面；手机页面仍然只支持 v2。
- 旧 APP+DATA 逻辑 v2 归档必须作为已退役格式明确拒绝，不能被当成 `meta-pass-storage-snapshot`。
- 真机测试不属于 GitHub CI。已有报告覆盖 USB 分段备份以及旧逻辑归档的删除/恢复场景；它不能证明 USB 已支持新整设备 v2 快照备份或恢复。

## 安全约束

开放任何快照恢复写入前，必须在修改设备前验证完整归档、每个 payload 的长度与 SHA-256、目标设备实时分区布局和保护范围。设备端必须独立强制执行分区身份、边界、sector 对齐、保护范围和事务状态。恢复计划只能覆盖明确允许的分区，绝不可暴露任意 Flash 地址。不得移动 `cardid`，也不得绕过动态分配器。
