<p align="right">
  <a href="dynslot-install-data-wiring-design.md">English</a> · <strong>简体中文</strong>
</p>

# dynslot 安装路径数据 carve 接线（P0-5 草案）

日期：2026-10-03
分支：`feat/dynslot`
父文档：`dynslot-m5-design.md`（生命周期）、`dynslot-design.md` §4.5（回收阶梯）。

状态：**定稿（2026-10-04，后续实现补充）**——审核修订（§1.0/§4/§7）+ 仲裁裁决：①退役原先错误的
P0-5 data_copy 迁移接线 ②F4 掩码式 DIRTY ③物化方案B（结束复位 + 伪造句柄）。当前实现保留
M5 的**正确 pool 内 DATA 扩容迁移**（目标区先擦、复制成功后切换 carve、失败恢复旧记录）；它是
既有 DATA 升级能力，不属于 P0-5 新建 DATA 初始镜像上传链。按 §1.0/§2/§3/§5 实现；P1-4 已落地，P0-5
设备/手机侧已接线。

## 1. 问题

两层缺口，第一层比第二层更根本：

**F1（阻断）——安装路径从不物化新槽位。** `meta_install_model_carve_ok` /
`meta_install_geom_from_carve` 零生产调用方；`geom_refresh()` 只从 live
分区表派生 limit，不看提案；chunk 写路径按子类型查表，新设备（安全表、
0 个 ota_N 条目）首装直接在 partition lookup 失败。后果：fresh 设备首装
不可能成功，P0-5 只修数据记录则事务无所依附。

**F2——数据记录无创建者。** `meta_carve_place_data`、
`meta_carve_largest_gap` 在生产代码中无调用方（`meta_carve_reclaimable`
仅备份导入日志一处）。M5 状态机永不启动——回收阶梯第 3–5 级在真实安装
路径上不可达。证据：`grep -rn` `main/*.c` 只有定义与测试。

本草案把两层一起接线：新槽位物化（F1）+ 记录创建（F2）在同一事务，
回收阶梯接入 no-fit 决策。

### 1.0 F1 的物化机制(2026-10-04 定稿:方案B —— 结束复位)

**硬约束:IDF 分区表缓存。** `esp_partition_find_first`(
`meta_store_slot_partition` 与 `esp_ota_begin` 都走它)在首次访问时把
0x8000 表加载进 SRAM 缓存,运行期物化的新 ota_N 条目对当前启动不可见。
子固件与 bootloader 永远在**下一次启动**读表,天然一致 —— 所以问题只剩
当前启动的上传/校验怎么走。

**方案B(已裁决 2026-10-04):上传走伪造句柄,复位落在安装成功结束时。**

1. **prepare(确认步)**:`place_offer` 裁决(纯函数副本:形状校验 → 幂等
   扫描+数据补放 → 槽位先放 → 逐条数据放置);OK 且 changed →
   `commit(&next, true)` 记录+表一次提交。当前启动缓存过期无妨。
   - no-fit → tier 3:有 ARCHIVED 可收则 `arc()` 一次重试;仍 no-fit →
     **409 + JSON**(needed/largestGap/reclaimableArchived/
     reclaimablePristine[,label],§4)。
   - 有提案时 `slot_fit` 检查跳过(place_offer 是 fit 权威;新槽不在
     geom/缓存中);F4 掩码在放置前对"既有记录"快照。
2. **上传**:chunk/finalize 按 confirmed_slot 查分区,缓存未命中回退
   **carve 伪造句柄**(address/size/subtype=0x10+idx/label=ota_idx,
   静态存储防句柄指针逃逸)。OTA 写/擦与 `esp_image_verify` 只消费值
   字段,不查表。幂等重试(手机重发同一 prepare)命中 `place_offer`
   幂等分支 → 不重复物化。
3. **成功结束**:本会话物化过表(`table_changed`)→ finalize 先返回 `done`，
   **不在当前 HTTP 会话内立即复位**。原因是手机侧需要先轮询到 `done`，
   若设备在响应后立即重启，会把“finalize 已接受但 status 暂时不可达”
   误判为失败。当前实现将复位挂起(`meta_carve_flash_reboot_pending`)，
   在退出商店页时执行复位；下一启动设备列表/启动路径读取同一份已提交
   carve 表，保证运行时缓存与 bootloader 视图一致。**这不是降低持久化要求**：
   carve 记录仍然先于表物化落盘，物理断电恢复仍由 boot hook 验证。
4. **取消/拒绝/被新 offer 覆盖**:本会话**新建槽** `meta_carve_flash_remove`
   回收(槽内无用户数据:空或半截垃圾镜像);既有槽保持 INVALID 路径。
   `manifest_valid` 守卫防 boot 零态下 `carved_new_slot=0`(静态零初始化)
   误删槽 0。成功路径先把 `carved_new_slot` 置 -1,清场不得回收已装槽。

**被拒绝的备选**:prepare 后即复位(多一次手机-设备往返重发 prepare,
且删除流程实证"复位"本身只是节奏约定);全程零复位(伪造句柄 + 设备
列表 scan 改从 carve 派生——省 ~3s 换两条 IDF 旁路,不划算)。

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
  "reclaimableArchived": 4096,
  "reclaimablePristine": 8192
}
```

- `for`：`"slot"` 或 `"data"`；`"data"` 时附 `"label": "<label>"`。
- `needed`：所需字节（槽位 = `meta_carve_need(image_len)`；数据 = 记录尺寸）。
- `largestGap`：`meta_carve_largest_gap(cur)`——最大单块可分配区间。
- `reclaimableArchived`：ARCHIVED 字节数——tier 3 可**自动**回收。
- `reclaimablePristine`：PRISTINE 字节数——tier 4 需用户**显式同意**。
  拆分两字段（原单一 `reclaimable` 混合了两级授权）：UI 必须能告诉用户
  "多少无需点头可腾、多少需要你确认"，否则同意无从谈起。

满足设计 §4.5 第 5 条（"报数字"）与 §12.4（"告知需腾多少"）。

## 5. 回收阶梯接线

槽位或数据条目分配失败时：

1. **第 3 级**——`reclaimableArchived > 0` 则
   `meta_carve_flash_arc(needed - free)`（只回收 ARCHIVED，不碰
   PRISTINE），然后重试放置一次。
2. **第 4 级**——PRISTINE 内容虽可由安装镜像复现，但可能仍含在用玩法数据；
   **不自动丢弃**。报 `reclaimablePristine`，要求用户显式同意（未来的
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

## 7. 已决问题（2026-10-03 审核后拍板）

1. **no-fit 状态码**：`409 Conflict`。语义 = 请求与资源当前状态冲突
   （空间不够是状态问题，不是请求语法问题）；备份导入的 `507` 是另一
   语境（WebDAV 存储类），不强行一致。响应体 = §4 JSON。
2. **顺序**：槽位先放（app 偏移稳定、与手机槽位提案一致）。
3. **逐条明细**：只报**第一个**失败条目。失败即整体拒绝（原子），
   全列表无决策价值；UI 按首个失败提示腾空间即可。
4. **P1-4 依赖**：已落地（58caa0e：`/api/install/slots` 暴露 data 记录，
   `parseSlots` 透传，`geomFromListing` union 占用）。


## 8. 新安装的 APP 槽位生命周期（2026-10-10 决策）

**每次安装都必须从动态回收池创建新的 APP carve；禁止复用既有 APP 槽位原位覆盖。** 该规则由手机 UI 与设备 prepare API 双重执行：手机端只展示新的 carve 提案，无法读取动态槽位清单时 fail closed；设备端拒绝不带 `has_carve` 的旧式安装请求。

- 安装器只展示分配器为本次 APP 镜像计算的新槽提案。现有空槽、有效槽和无效槽都不是安装目标。
- 删除 APP 时先清除可导致启动扫描“复活”的镜像头，再提交移除 carve 记录；成功后该范围立即回到池分配器的可用空间中。后续安装可以得到相同物理 offset，但必须是新分配、新槽位身份。
- DATA 生命周期与 APP 槽位分离。默认删除 APP 时归档关联 DATA，不因 APP 槽位释放而静默擦除用户数据；显式擦除仍走独立确认路径。
- prepare/上传失败时，只回收本次新建且未成功安装的 carve；不能提前释放原有已提交槽位。
- UI 展示的槽位编号是当前 carve 排序产生的临时索引，不是持久化身份。新建/删除可能导致索引变化，业务关联必须使用持久化记录中的 play ID/label 等字段。

该策略以减少状态分支和提高池利用率为目标。旧式固定槽安装路径不再作为新安装的兼容回退；不支持动态池的设备必须升级固件，不能静默改用旧槽覆盖。
