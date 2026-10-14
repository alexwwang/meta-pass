<p align="right">
  <a href="storage-snapshot-implementation-status.md">English</a> · <strong>简体中文</strong>
</p>

# 存储快照实现状态（2026-10-14）

分支：`feat/storage`。

## 已提交的代码基础

- `install-slot/storage-snapshot.js`：v2 快照 manifest、分区几何/保护范围/extent 校验、ZIP 生成与解析、SHA-256 校验，以及非变更式恢复计划生成。
- `main/meta_store_install.c`：新增带同源与 session token 检查的只读分区清单和分区相对偏移读取 API；禁止读取当前运行的宿主分区和 `cardid`，不接受绝对 Flash 地址作为读写目标。
- `install-slot/phone-install.js`：手机空间管理 UI 已切换到整设备快照备份，包含敏感数据警告；快照归档恢复目前只做完整性与目标布局预检，不会写入 Flash。
- `docs/storage-snapshot-backup-restore-design.md` 与本文件：补齐中英文设计/状态文档。
- 自动化测试已增加到 `tests/test_storage_snapshot.mjs`、`tests/test_phone_install.mjs` 和 `tests/test_storage_snapshot_device_contract.py`，并登记到 `tools/validate.sh`。

## 尚未完成

- USB 页面尚未切换到新快照归档；USB v1.1 历史备份/还原路径未修改。USB 页面必须先可靠取得设备当前运行的宿主固件分区，不能仅从分区表或猜测 OTA 状态推断。
- 手机快照恢复尚未执行任何写操作。设备端目前只有受保护的只读 API；必须先实现可验证、边界受限的事务恢复协议，并明确写入失败/设备断连时的回滚方案，才可开放恢复。
- 旧 APP+DATA v2 归档不能按新快照格式处理；USB 端现有 v2 APP+DATA 流程尚待迁移/明确兼容策略。
- 最新 GitHub Actions Static checks 在文档语言链接检查处失败；已修正文档链接并补齐英文状态文档，后续 CI 仍需重新确认。
- 尚未完成当前分支的最终静态测试、ESP-IDF 固件构建、USB/手机浏览器自动化回归。真机测试未执行；完成代码与 CI 检查后再交给本地 agent 执行 passport-sim 和真机回归。

## 安全约束

快照恢复必须先校验完整归档、所有 payload 的长度与 SHA-256、目标设备实时分区布局和保护范围，再允许任何写入。恢复计划必须只覆盖明确允许的分区；不能暴露任意地址写接口。旧 APP+DATA v2 归档不能被当成 `meta-pass-storage-snapshot`；USB v1.1 历史归档继续由旧路径处理。
