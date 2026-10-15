[English](HANDOFF-usb-v1-archive-compatibility.md)

# Handoff：USB 历史 v1.1 归档兼容性回归

分支：`feat/storage`。禁止在 `main` 上开发。

## 目的与范围

在当前 checkout 上验证 USB 服务页保留的历史 v1.1 固定槽 APP/NVS 归档路径。本 handoff **不要求手机内嵌页支持 v1**。USB 与手机页面的功能对齐，不代表归档版本支持范围必须相同。

当前已核验基线：`95eea1911646eefcc5ab679317f7929195ca4601`；该提交的 Static checks 和 Firmware checks 均通过。执行真机测试时必须记录实际 HEAD；若 HEAD 已前进，先重新运行 `./tools/validate.sh`。

## 安全规则

- 不移动 `cardid`，不改变分区布局，不猜测 Flash 地址，不绕过动态分配器。
- 动态 v2 设备不得执行 v1 恢复写入；页面必须在任何写入前拒绝该归档。
- v1.1 归档可能包含原始 NVS 镜像，其中可能有 Wi-Fi 凭证和设备状态。归档仅保存在本地，限制访问，绝不提交或上传。
- 未经设备所有者明确批准，且没有已验证的恢复方案时，不得在生产设备上执行会覆盖 APP 槽和 NVS 的破坏性恢复。
- ZIP 下载成功不等于备份完整；必须检查 manifest 和 payload 哈希。

## 阶段 A：主机测试与代码契约

```bash
git checkout feat/storage
git pull --ff-only origin feat/storage
git rev-parse HEAD
./tools/validate.sh
```

确认 USB 页面和测试明确保证：
1. 还原入口接受经过完整校验的 `manifest_version: 1`、kind 为 `meta-pass-slot-backup` 的归档。
2. v1 还原 handler 仅供 USB 页面使用，且仅适用于固定槽设备。
3. 动态 v2 设备必须在任何 Flash 写入前拒绝 v1 归档。
4. 写入前校验历史 NVS payload 的长度与 SHA-256。
5. 归档包含 NVS 时，旧路径从实时分区表定位设备 NVS 分区，并将历史原始 NVS payload 写回该分区。
6. `manifest_version: 2` 的旧 APP+DATA 逻辑归档必须拒绝；`meta-pass-storage-snapshot` 归档仍受门禁限制，因为安全的 USB 快照恢复尚未实现。
7. 手机内嵌页继续只接受 v2 整设备快照。不得为了“功能对齐”而修改手机页接受 v1。

## 阶段 B：确认可用硬件类型

连接设备，记录固件 SHA-256、分区表、检测到的存储模式、APP/槽位清单，以及设备属于固定槽还是动态 v2。

- 如果是动态 v2 设备，只执行负向测试：载入已知有效的历史 v1.1 归档，确认页面清楚提示仅支持固定槽；保存页面日志，并确认 APP/DATA 清单和设备状态没有变化。不得尝试其他写入路径。
- 只有具备已知固定槽设备且有明确批准的恢复方案时，才进入阶段 C。

若没有固定槽设备，将正向历史恢复测试记为 `BLOCKED_HARDWARE_UNAVAILABLE`。仅凭代码检查或动态设备上的拒绝行为，不得将正向恢复记为 PASS。

## 阶段 C：固定槽 v1.1 备份/恢复往返（仅限已批准的旧布局设备）

1. 通过 USB 页面创建新的 v1.1 备份。ZIP 仅保存在本地并记录其 SHA-256。
2. 验证 `manifest.json`：`manifest_version=1`、`kind=meta-pass-slot-backup)；所有引用的 APP/NVS payload 均存在，长度与 SHA-256 匹配。
3. 从设备实时信息记录源槽位布局和 NVS 分区几何。
4. 通过 USB UI 将归档恢复到同一台固定槽设备，不直接调用内部 API。
5. 保存 UI 结果和串口/loader 日志。对归档定义为原始字节的内容，使用只读 dump 与 payload 做逐字节比较。只有在明确批准的流程中验证 NVS 恢复；报告不得泄露 NVS 内容。
6. 正常重启，确认设备能启动、预期固定槽清单存在、必要连接可用。如果恢复 NVS 改变了 Wi-Fi 配置，按已批准的恢复方案处理。
7. 确认设备仍处于固定槽模式，分区表和 `cardid` 位置均未改变。

任何失败都必须停止后续破坏性操作。保留脱敏日志，并报告失败阶段、首个错误以及 Flash 写入是否已经开始。

## 证据与报告

在 `tools/realdevice/logs/<timestamp>-usb-v1-archive-compat/` 下生成：
- `report.md`、`report.json`、`environment.txt`；
- 实际分支/commit、固件 SHA-256、工具和浏览器版本、检测到的存储模式；
- manifest 摘要及归档/payload 哈希（不包含秘密内容）；
- 前后分区与槽位清单；
- 页面 console/日志、loader 输出、重启/启动证据和只读逐字节比较结果；
- 总体状态，取值之一：`PASS`、`FAIL`、`BLOCKED_HARDWARE_UNAVAILABLE`、`BLOCKED_RECOVERY_PLAN`。

ZIP、NVS 镜像、Wi-Fi 凭证、配对 token 和设备秘密不得进入 Git 或最终报告。只有在固定槽硬件上执行了阶段 C，才能声称历史 v1 正向恢复通过。
