<p align="right">
  <a href="mobile-page-data-e2e-market-firmware.md">English</a> · <strong>简体中文</strong>
</p>

# 手机内嵌页 DATA E2E：真实市场固件选择

状态：测试对象已选定；显式 opt-in 的 DATA 缩容 profile 已接入分析与安装 offer；真实市场镜像和真机验收仍未完成。

## 决策

**正式的真实业务 DATA 验收目标选择录音笔（Play 28），DATA 标签为 `recordings`，分区类型为 FAT（subtype `0x81`）。** 自制 `e2edata` 子固件仅保留为协议、启动器控制通道和分配器回归测试 fixture，不得把它的通过结果记为市场玩法 DATA 生命周期通过。

## 为什么选 Play 28
横向比较：参见[语音与电子书固件 DATA 存储机制对比](mobile-page-data-e2e-market-comparison.zh_CN.md)，其中包含公开 FoloToy AI Passport / DeepSeek 语音源码与开源电子书参考实现的分析，并明确标记尚未完成二进制下载校验的边界。


依据 `docs/assets/dynslot-data-unification-research.zh_CN.md`（2026-10-02，对市场玩法二进制做字节级分析）：

- 应用镜像约 1,536 KiB，`recordings` 分区约 6,592 KiB，出厂分区数据为全 `0xFF`。
- 固件通过 `esp_vfs_fat_spiflash_mount_rw_wl` 按 `recordings` 标签挂载 FAT 分区，属于 T1 标签寻址，正是动态 carve 要支持的生产访问方式。
- 录音是运行时生成的用户数据；玩法 UI 支持导出 WAV 和删除录音。因此可以验证真实的“写入 → 导出/读回 → 重启后仍存在 → 删除后消失”生命周期，而不是只验证某个测试记录结构。
- 研究中抽样的多数玩法是约 8 MiB 的整盘独立镜像，不适合直接作为动态子固件。Play 563 虽然可安装，但其 `store` 是 NVS（MVP 中属于全局共享资源），不适合作为首选的每玩法隔离用户数据验收对象。

## 容量门槛：不能跳过

Play 28 的完整声明为 app 约 1.5 MiB + `recordings` 约 6.4 MiB，超出当前动态池对单个玩法可提供的容量。研究提出将空白的 `recordings` carve 缩至约 4 MiB；标准 IDF FAT 挂载会从实际分区条目推导大小，因此这是合理候选，默认安装链路仍按固件声明的 DATA size 计算需求；本分支已加入显式 opt-in 测试配置，但尚不能据此宣称真实设备兼容。

实现状态：已加入 `?mp_test_data_profile=play28-recordings-4m` 测试配置（仅 Play ID 28），服务端 analyze 与手机安装 offer 共用相同 profile；必须同时显式设置服务端环境变量 `ENABLE_TEST_DATA_PROFILES=1`，默认环境禁用。后续仍需用真实市场镜像证明分区表确实为空白且运行时挂载大小为 4 MiB。

该配置的安全边界：

1. 只允许在 `ENABLE_TEST_DATA_PROFILES=1` 且显式测试 query 参数下，对 Play ID 28，将 `recordings` 的 carve size 从声明值覆写为 4 MiB；生产默认行为不变。
2. 不改写原始固件二进制，也不把源镜像中的越界 DATA 字节复制到缩小后的 carve。
3. analyze/prepare/finalize 和设备端 store 记录必须一致报告最终大小；启动后由真实固件按 `recordings` 标签查找到的分区大小必须是 4 MiB。
4. profile 只接受唯一的 `recordings` FAT（subtype `0x81`）分区、声明大小至少 6 MiB、`initial_image_size=0` 的镜像；任何不匹配都 fail closed。
5. 如果无法以受控配置做到以上一致性，必须判定容量门槛未通过，不能通过删减检查或伪造 DATA 预留来绕过。

## 正式验收场景

1. 从市场安装 Play 28，确认 analyze 结果中有 `recordings`、FAT subtype `0x81`，并核对缩容后的分区大小和 carve 地址。
2. 在玩法 UI 中生成一段短录音，导出 WAV；保存文件大小和 SHA-256 作为第一次数据证据。
3. 通过真实设备生命周期返回启动器并重新启动 Play 28；再次导出相同录音，要求文件内容 SHA-256 一致。不能以槽位清单或安装 HTTP 200 代替文件证据。
4. 在 UI 删除该录音，重新列举/导出应确认该条目消失。
5. 删除玩法后，按当前产品策略验证 DATA 记录是 ARCHIVED 还是擦除；不能把“槽位不存在”直接等同于物理数据已擦除。
6. 报告记录固件来源/版本、play ID、原始 DATA 声明、实际 carve size/offset、WAV 两次 SHA-256、重启方式、删除策略和所有未覆盖项。

## 自动化边界与后续实施

当前通用手机页面 runner 能自动化安装器 UI 和核对只读槽位/DATA 元数据；它的 `e2edata` 串口协议只适用于专用测试 fixture，不能直接控制 Play 28 的录音业务。真实玩法测试应通过该玩法现有 UI/导出接口完成文件级证据采集，不得向市场固件注入虚构的 `WRITE/READ` 命令。

要完成无人值守验收，还需要取得/固定 Play 28 的确切市场固件版本，并确认其录音 UI 的 DOM/接口以及真机重启/返回启动器的可用控制路径。上述信息未在当前仓库中固定，因此在补齐前，Play 28 的真实业务 E2E 状态必须为 **BLOCKED / NOT RUN**；专用 fixture 通过不改变此状态。

## 为什么保留专用 fixture

`tests/realdevice/data-child/` 仍有价值：它用于独立验证动态分区查找、串口控制协议、DATA carve 隔离、设备侧校验和以及 CI 中的固件构建。它是底层基础设施测试，不是市场业务固件的替代品。