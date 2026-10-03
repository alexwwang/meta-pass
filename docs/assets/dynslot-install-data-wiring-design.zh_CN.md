<p align="right">
  <a href="dynslot-install-data-wiring-design.md">English</a> · <strong>简体中文</strong>
</p>

# dynslot 安装路径数据 carve 接线（P0-5 草案）

日期：2026-10-03
分支：`feat/dynslot`
父文档：`dynslot-m5-design.md`（生命周期）、`dynslot-design.md` §4.5（回收阶梯）。

状态：**决策已确认（2026-10-03）**——按 §2/§3/§5 的选定方案实现；
顺序为 **P1-4 → P0-5**（见 §7）。

## 1. 问题

`meta_install_model.c` 已把 manifest 的 `data[]` 解析进 session，但没有
任何代码创建数据 carve 记录：`meta_carve_place_data`、
`meta_carve_largest_gap`、`meta_carve_reclaimable` 在生产代码中无调用方。
因此 M5 状态机永远不会启动——回收阶梯第 3–5 级在真实安装路径上不可达。
证据：`grep -rn` `main/*.c` 只有定义与测试。

本草案把记录创建接入安装提交，把回收阶梯接入 no-fit 决策。

### 1.1 寻址模型（为什么 offset 对子固件透明）

整套接线所依赖的机制，源自 `dynslot-data-unification-research.md` §4
（T1/T3）：

- **子固件按分区标签寻址，不按 offset。** FatFS/SPIFFS 按标签挂载
  （`esp_vfs_fat_spiflash_mount_rw_wl(path, "recordings", …)`），裸分区
  用户调 `esp_partition_find_first(TYPE, subtype, label)`。子固件从来看不到
  offset。
- 这次查找读的是 **0x8000 的 live 分区表**——即 metapass 物化的 carved
  表——**而非**子固件镜像内嵌的那张。
- 因此流程是：子固件内嵌表声明 `{label, type, subtype, size}` → analyze 读出
  → 设备在池内分配 offset 并在该 offset 物化**同 label** 条目 → 子固件运行
  时按 label 解析到 metapass 分配的 offset。
- **所以迁移就是改一个表条目**：把字节拷到新 offset
  （`meta_carve_flash_data_copy`），更新记录的 `offset`，用同 label 重新物化
  表——子固件下次挂载即看到新位置。这就是记录按 `(play_id, label)` 而非
  offset 键控的原因。
- **写死裸地址的子固件（T3）无法重映射。** 没有插入点（VFS 是每注册者私有
  的驱动表；cache-MMU 重映射不受支持且与 bootloader 冲突），只能契约层警告。
  已知唯一违规者是 metapass 自己的 Wi-Fi 凭据备份 @0x35A000，已迁移（L6）。
- **两个边界**：默认 `nvs` 是单标签、不能按槽重定向（子固件必须用自己声明的
  标签）；v1 中 `(label, subtype)` 每 carve 唯一（M4 共享属未来项）。

对本设计的推论：`manifest.data[]` 的 `{label, size}` 就是子固件声明的需求，
分配的 offset 留在设备端，手机不需要 offset（只需要占用，见 §3）。

## 2. 决策 1（已确认）——何时、由谁创建记录

**在 confirm/commit 步骤创建，与槽位物化同一次事务。**

设备对每个 `manifest.data[]` 条目跑 `meta_carve_place_data`，作用于**已
放好新槽位**的同一个 `meta_carve_t`，追加 `PRISTINE` 记录，一次性提交。

理由：

- 与槽位一次原子提交——无半安装态、无第二个断电窗口。
- 不需要子固件配合；系统策略留在不可绕过的层（`AGENTS.md`）。
- manifest 已携带 `play_id` / `size` / `label`。

已否决：子固件首启自报（依赖不可信子固件）；上传后单独提交（两个提交、
部分状态窗口）。

## 3. 决策 2（已确认）——提案绑定与顺序

**纯设备端分配；v1 手机不带 data 提案。槽位先放。**

一次提交内的顺序：

1. `meta_carve_place(cur, carve_size, APP, &next)` 放槽位。
2. 对每个 `manifest.data[i]`：先拒保留标签
   （`meta_carve_data_label_reserved`），再
   `meta_carve_place_data(&next, size, &off)` +
   `meta_carve_data_append(&next, ...)`（`state=PRISTINE`）。
3. **任一条目放不下 → 整个 confirm 失败**（原子，无部分 carve）。
4. `meta_carve_flash_commit(&next, true)`。

对手机侧的影响：`dynslot-pool.js` 的 `carvePlace` 必须镜像**槽位∪数据**的
占用域，否则槽位提案会落进数据记录、被设备拒（L4 分歧）。这使 **P1-4 成为
前置**：`/api/install/slots` 必须先暴露数据记录的 `offset`/`size`，否则
不能安全创建数据记录。

`(label, subtype)` 跨玩法唯一性已由 `meta_carve_valid` 保证；M4 共享属未来项。

## 4. 决策 3——no-fit 拒绝的形态

拟在 prepare/confirm 失败时返回（状态码**待定**，见 §7）：

```json
{
  "reason": "no-fit",
  "for": "slot",
  "needed": 1572864,
  "largestGap": 1048576,
  "reclaimable": 4096
}
```

- `for`：`"slot"` 或 `"data"`；`"data"` 时附 `"label": "<label>"`。
- `needed`：所需字节（槽位 = `meta_carve_need(image_len)`；数据 = 记录尺寸）。
- `largestGap`：`meta_carve_largest_gap(cur)`——最大单块可分配区间。
- `reclaimable`：`meta_carve_reclaimable(cur)`——阶梯可释放的
  ARCHIVED + PRISTINE 字节数。

满足设计 §4.5 第 5 条（"报数字"）与 §12.4（"告知需腾多少"）。

## 5. 回收阶梯接线

槽位或数据条目分配失败时：

1. **第 3 级**——`reclaimable > 0` 则 `meta_carve_flash_arc(needed - free)`，
   然后重试放置一次。
2. **第 4 级**——PRISTINE 内容虽可由安装镜像复现，但可能仍含在用玩法数据；
   **不自动丢弃**。报 `reclaimable`，要求用户显式同意（未来的
   `/api/install/reclaim` PRISTINE 范围，或既有 `eraseData` 路径）。
3. **第 5 级**——仍放不下 → 按 §4 JSON 拒绝。

不做静默自动搬移 / 压缩（L1 诚实边界保持；压缩推迟到 v2）。

## 6. 测试计划

- **模型（host 可链接）**：新增 `meta_install_model_data_ok(m, cur)`，在
  `meta_carve_t` 副本上重放逐条目放置，返回裁决 + §4 数字。覆盖：放得下、
  no-fit（槽位）、no-fit（数据）、保留标签、重复标签、原子回滚。
- **Flash（host，RAM NOR）**：安装路径一次事务加槽位 + 数据；验证记录跳
  模拟重启持久化、物化表含数据条目。
- **JS（Node）**：P1-4 落地后给 `test_dynslot_pool.mjs` 加 union-occupancy
  用例；验证池内存在数据记录时手机槽位提案与设备分配器一致。
- **夹具**：含一槽位 + 一条数据记录的 carve 快照。

## 7. 待决问题

1. **no-fit 状态码**：`400` / `409` / `507`？（备份导入已用 `507`。）
2. **顺序**：已定**槽位先放**（app 偏移稳定、与手机槽位提案一致）。
3. **逐条明细**：只报第一个失败条目够用，还是 UI 需要全部失败列表？
4. **P1-4 依赖**：已确认**先做 P1-4**，再创建数据记录。
