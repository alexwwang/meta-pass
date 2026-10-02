<p align="right">
  <a href="dynslot-m5-design.md">English</a> · <strong>简体中文</strong>
</p>

# M5 — 解耦数据 carve 生命周期（详细设计）

日期：2026-10-03
分支：`feat/dynslot`
父文档：`dynslot-data-unification-research.md` §4 M5、
        `dynslot-design.md` §4.5 回收阶梯第 3–5 级。

## 1. 范围与现状

M5 让玩法的 flash 用户数据（录音、存档、档案）**在升级时保留**（同 slug
重装）与**在卸载时默认归档**（不立刻擦除，等池压力回收）。

### 1.1 已实现（schema + 底层工具）

- `meta_carve.h` 数据记录（v2，32 字节）：
  ```c
  typedef struct {
      uint32_t play_id;   // >0；生命周期键控（0 = v1 孤儿，跳过）
      uint32_t offset;    // 64 KB 对齐
      uint32_t size;      // 4 KB 粒度，≥ META_CARVE_MIN_DATA (4 KB)
      uint8_t  state;     // meta_data_state_t
      uint8_t  subtype;   // 子固件声明的分区子类型（0x81 fat / 0x82 spiffs / …）；
                          // 0 = DATA_OTA 被 meta_carve_valid 拒
      uint8_t  type;      // 必须 1 (data)
      char     label[META_DATA_LABEL_MAX + 1];  // ≤ 16 B，含 NUL
  } meta_carve_data_t;
  ```
- 状态枚举：`PRISTINE=0`、`DIRTY=1`、`ARCHIVED=2`。
- `meta_carve_place_data(cur, size, &offset)` — first-fit 分配；非法尺寸
  或空间不足 → false。
- `meta_carve_find_data(cur, play_id, label)` — 按键查找；未找到 → -1。
- `meta_carve_data_append/remove` — 原始 CRUD（通过
  `meta_carve_place_data` + 不变量守卫调用）。
- `meta_carve_reclaimable(c)` = 所有 PRISTINE + ARCHIVED 记录的字节和
  （DIRTY 是“在用”，不计入可回收）。
- Store 布局：`data[8]` 行在每 4 KB 扇区的 0x37A0–0x3FB0，CRC 覆盖
  `[0, 4032)`——已处理这些字段；解码拒 `type≠1`、`subtype=0`、
  `play_id=0`、`state>ARCHIVED`（`meta_carve_store.h` §25）。

### 1.2 仍缺失

Schema 和分配器已接线，但**没有任何生命周期 API 或调用方**：

| 缺口 | 证据 |
|---|---|
| 无 `meta_carve_flash_set_dirty(play_id)` | `meta_carve_flash.h` — 未声明 |
| 无 `meta_carve_flash_archive_data(play_id)` | `meta_carve_flash.h` — 未声明 |
| 无新槽升级数据拷贝路径 | `meta_store_install.c:472-503` — 仅处理槽位，无数据 |
| 无启动时 DIRTY 标记钩子 | `main.c:1306-1320` — 只 sync_states，无数据 |
| 无卸载归档步骤 | `meta_carve_flash_remove(slot)` 第 167 行 — 数据成孤儿 |
| 无池压力 ARC 步骤 | `meta_install_model_remove_ok` — 只调 `meta_carve_flash_remove` |

### 1.3 本文档范围

- 状态机及守卫。
- 三个触点及其精确调用点、前置条件、失败模式。
- 新增公共 API（全在 `meta_carve_flash.h`）。
- 升级拷贝语义（何时拷贝、何时拒绝）。
- 池压力 ARC（回收阶梯第 3 级）。
- 边界案例：v1 孤儿数据、`play_id=0`、PRISTINE 签名绑定、断电 mid-copy、
  多玩法标签共享（M4 泛化规则）。

## 2. 状态机

```text
          install            launch(ok 长按)        uninstall
PRISTINE ─────────────────► DIRTY ──────────────────► ARCHIVED
   │                              ▲                        │
   │  升级拷贝                │  擦除/重置数据           │  ARC 池压力回收
   │  PRISTINE→PRISTINE       │  或 DIRTY→DIRTY         │  或用户显式擦除
   │                              │                        │
   └─── 升级：无数据记录 ──────┘
       → 新鲜 PRISTINE
```

转换及守卫：

| 转换 | 守卫 | 动作 | 失败模式 |
|---|---|---|---|
| **安装 → PRISTINE** | `meta_carve_place_data` 分配；内容由 app 写入或手机重发还原 | 记录 `state=PRISTINE` | 池满 → `no-fit`，UI 拒绝 |
| **启动 → DIRTY** | 槽位 VALID，启动器收到 OK | 翻 `state=DIRTY`；不擦除 | —（尽力而为；失败静默） |
| **升级 → 拷贝 PRISTINE 或新鲜 PRISTINE** | 见 §4 | 池内拷贝字节，或降级到 fresh PRISTINE 下次安装 | 缩小（新 < 旧）→ 拒绝；扩容 → 池内拷贝 + 重新物化表 |
| **卸载 → ARCHIVED** | `meta_carve_flash_archive_slot_and_data` | 翻 `state=ARCHIVED`；字节留在 flash | — |
| **池压力 → 擦 ARCHIVED** | `meta_carve_flash_unarchive_oldest`（见 §5） | 擦除字节；移除记录 | 无——尽力循环 |
| **显式擦除** | UI 选择 | 擦除字节；移除记录 | 无 |

`DIRTY` 不复原——跨重启持久化。只有卸载或 ARC 把它转到 `ARCHIVED`。

## 3. play_id 派生

- **手机端**（analyze）：`play.revisionId`——同一玩法跨版本稳定。在
  提案 manifest 里透出：`manifest.play_id`。
- **设备端**（prepare/离线重装）：按 `manifest.slug` 查 catalog 缓存
  → 解码出 play_id（手机端预填进 manifest JSON，设备端对字段无状态依赖）。
  字段缺失时 `play_id=0`（v1 兼容路径；§6.3）。
- **不变量**：`play_id=0` 记录视作 v1 孤儿——不做任何生命周期操作。它们
  保留在 store 里，但永远不被 DIRTY、ARCHIVED 或 ARC 触碰。（迁移种子
  从 v1 carve 播种时给这些记录设 `play_id=0`。）

## 4. 触点 1：安装 / 升级

### 4.1 安装路径（`meta_store_install.c:472 slot_materialize_locked`）

当前形状（dynslot 迁移后）：

```c
// 第 487-503 行：新槽分支
if (!s_session.manifest_valid || !s_session.manifest.has_carve) return ...;
meta_carve_t next;
if (meta_carve_place(cur, s_session.manifest.carve_size,
                     META_CARVE_KIND_APP, &next) != slot ||
    !meta_carve_valid(&next)) { ... }
if (meta_carve_flash_commit(&next, true) != ESP_OK) { ... }
```

在 `meta_carve_flash_commit` 前插入：

```c
// 升级数据迁移（M5，安装/升级触点）：
// 对 manifest 声明的每条数据记录，按 (play_id, label) 在现有 carve 中
// 查找。找到且 state ∈ {PRISTINE, DIRTY}：
//   - new_size >= old_size：池内拷贝旧字节到新偏移，更新 next 中的记录
//     并保留 state（拷贝成功即视为 PRISTINE）。
//   - new_size < old_size：拒绝（用户需先手动归档，或移除后不带数据重装）。
// 未找到：新鲜 PRISTINE——无需迁移。
// V1 记录（play_id=0）：跳过迁移；下次安装写数据时重新 PRISTINE。
for (int i = 0; i < s_session.manifest.data_count; i++) {
    const meta_manifest_data_t *req = &s_session.manifest.data[i];
    int j = meta_carve_find_data(cur, req->play_id, req->label);
    if (j < 0) continue;  // 首次安装——无动作
    const meta_carve_data_t *old = &cur->data[j];
    if (req->size < old->size) {
        // 缩小——拒绝，无副作用。
        ESP_LOGW(TAG, "data shrink rejected: play_id=%u label=%s %u->%u",
                 (unsigned)req->play_id, req->label,
                 (unsigned)old->size, (unsigned)req->size);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (req->size == old->size && old->offset == req->offset) {
        // 无变化——跳过拷贝。
        memcpy(&next.data[next.data_count++], old, sizeof(*old));
        continue;
    }
    // 池内拷贝（尽力）：
    esp_err_t cp = meta_carve_flash_data_copy(old->offset, old->size,
                                              req->offset);
    if (cp != ESP_OK) {
        // 拷贝失败（地址非法或 mid-flight 断电恢复）——中止升级，保留旧
        // 记录完整；下次安装尝试会再次走迁移路径。
        ESP_LOGE(TAG, "data copy failed: %s", esp_err_to_name(cp));
        return ESP_ERR_NOT_SUPPORTED;
    }
    meta_carve_data_t d = *old;
    d.offset = req->offset;
    d.state = META_DATA_PRISTINE;  // 拷贝成功——视为新鲜
    memcpy(&next.data[next.data_count++], &d, sizeof(d));
}
// manifest 未匹配的旧记录原样保留（可能对应新版不再声明的 label——留在
// DIRTY/ARCHIVED 直到 ARC 或显式擦除）。
```

**失败模式：**
- 缩小 → 立即拒绝，无任何副作用。
- 拷贝写失败 → commit 前中止，旧记录完好。
- manifest 无 data 段（老式安装）→ 循环空操作；新鲜记录下次安装时 PRISTINE。

### 4.2 Manifest 形状

`meta_install_model.h:40-50` —— 新增：

```c
#define META_MANIFEST_DATA_MAX 8
typedef struct {
    uint32_t play_id;
    uint32_t size;
    char     label[META_DATA_LABEL_MAX + 1];
} meta_manifest_data_t;

// 已有 meta_install_manifest_t 增加：
uint8_t  data_count;                           // §7 L (0..8)
meta_manifest_data_t data[META_MANIFEST_DATA_MAX];
```

手机侧：`dynslot-pool.js` analyze 已产出 `child.partitions[]`（嵌入表的
原始分区列表）。加一个 helper `buildManifestData(play)` 把每个声明的子
固件数据分区映射到 `{play_id, size, label}`（用 `play.revisionId` 作
play_id）。

## 5. 触点 2：启动（DIRTY 标记）

`main.c:1306-1320` —— `meta_store_mark_factory_valid` 与
`meta_carve_flash_sync_states` 之后，扫描即将启动的槽位。当
`meta_install_session_open` 对 VALID 槽位调用时：

**决策**：在 `finalize_locked` 成功后标记（单次调用，无双重触发风险）。
理由：DIRTY 是 ARC 优化，不是正确性闸门；标记在启动时容易误标尚未填充
数据的 PRISTINE 记录。

在 `meta_store_install.c:finalize_locked` 成功路径（`esp_ota_end` 之后，
约第 690 行后）插入：

```c
// M5：安装成功——将本玩法的数据记录翻为 DIRTY（即将运行，ARC 不得回收其内容）。
if (s_session.manifest.play_id != 0) {
    meta_carve_flash_set_dirty(s_session.manifest.play_id);
}
```

`meta_carve_flash_set_dirty` 扫 store 的 `data[]`，匹配 `play_id` 的记
录翻 `PRISTINE→DIRTY`，重算 CRC，重提交（A/B 轮转）。幂等。

## 6. 触点 3：卸载 / 移除（归档）

当前 `meta_carve_flash_remove(int slot)`（第 167 行）只删槽位记录——数
据记录成孤儿（play_id ≠ 0 仍在 store 但不可达）。替换为：

```c
// M5：移除槽位 AND 归档匹配的数据记录。
esp_err_t meta_carve_flash_archive_slot_and_data(int slot)
{
    const meta_carve_t *cur = meta_carve_flash_carve();
    if (!cur || slot < 0 || slot >= (int)cur->count)
        return ESP_ERR_INVALID_ARG;
    if (cur->slot[slot].kind != META_CARVE_KIND_APP)
        return ESP_ERR_NOT_SUPPORTED;

    uint32_t pid = cur->slot[slot].play_id;

    meta_carve_t next = *cur;
    // 优先归档最旧记录（符合 §4.5 回收阶梯第 3 级顺序）。
    for (int8_t j = (int8_t)next.data_count - 1; j >= 0; j--) {
        if (next.data[j].play_id != pid) continue;
        if (next.data[j].state == META_DATA_DIRTY ||
            next.data[j].state == META_DATA_PRISTINE) {
            next.data[j].state = META_DATA_ARCHIVED;
            // 不擦除字节——延迟到 ARC 时间。
        }
    }
    // 再删槽位。
    if (!meta_carve_remove(&next, (uint8_t)slot))
        return ESP_ERR_INVALID_ARG;

    return meta_carve_flash_commit(&next, true);
}
```

从 `meta_install_model_remove_ok`（或当前 remove 路径）调用，替代
`meta_carve_flash_remove`。

**归档策略**：默认总是 ARCHIVE（用户数据保留）。显式擦除（UI 选择）调
`meta_carve_flash_erase_data(pid, label)`——擦字节 + 移除记录。这是另一
条独立路径。

## 7. 池压力 ARC（回收阶梯第 3 级）

`meta_carve_place` 失败且 `meta_carve_free(c) < needed` 时，调 ARC：

```c
// 从 ARCHIVED 记录中回收至多 target 字节，最旧优先。
// 足够 → true；耗尽 → false。
bool meta_carve_flash_arc(uint32_t target)
{
    const meta_carve_t *cur = meta_carve_flash_carve();
    uint32_t reclaimed = 0;
    for (uint8_t i = 0; i < cur->data_count && reclaimed < target; i++) {
        if (cur->data[i].state != META_DATA_ARCHIVED) continue;
        if (cur->data[i].play_id == 0) continue;
        esp_err_t e = meta_carve_flash_erase_range(
            cur->data[i].offset, cur->data[i].size);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "ARC erase failed: %s", esp_err_to_name(e));
            break;
        }
        reclaimed += cur->data[i].size;
        meta_carve_remove_data(cur, (uint8_t)i);
        i--;
    }
    if (reclaimed >= target) {
        return meta_carve_flash_commit(cur, false) == ESP_OK;
    }
    return false;
}
```

**调用点**：`meta_install_model_slot_fit` 或
`meta_install_model_default_slot`——在 no-fit 决策前加一次 ARC 重试。

**UI 集成**：ARC 耗尽仍 no-fit，报 `no-fit; largest_gap=X;
archive=Y bytes available`，UI 提示"请先归档 X 的录音"。

## 8. 待新增公共 API

全在 `meta_carve_flash.h`：

```c
// M5 — DIRTY 标记（幂等；已是 DIRTY 则空操作）。
// 在 finalize 成功后调用。
esp_err_t meta_carve_flash_set_dirty(uint32_t play_id);

// M5 — 归档某槽位的全部数据记录（卸载时）。
// 不擦除字节；只翻 state + 重新提交。
esp_err_t meta_carve_flash_archive_slot_and_data(int slot);

// M5 — 显式擦除特定数据记录（用户主动选择）。
esp_err_t meta_carve_flash_erase_data(uint32_t play_id, const char *label);

// M5 — 池压力 ARC；从 ARCHIVED 记录回收至多 target 字节。
// 足够 → true；耗尽 → false。
bool meta_carve_flash_arc(uint32_t target);

// 内部：擦除池内范围（ARC 与显式擦除共用）。
esp_err_t meta_carve_flash_erase_range(uint32_t offset, uint32_t size);

// 内部：池内字节拷贝（升级迁移共用）。
esp_err_t meta_carve_flash_data_copy(uint32_t src, uint32_t size,
                                     uint32_t dst);
```

## 9. 测试计划

新主机测试 `tests/test_meta_carve_lifecycle.c`（RAM NOR 模型）：

| 用例 | 覆盖 |
|---|---|
| `test_set_dirty_pristine` | PRISTINE→DIRTY finalize 后，幂等 |
| `test_set_dirty_already_dirty` | 已是 DIRTY：空操作 |
| `test_archive_slot_and_data` | 槽位移除归档匹配数据，保留其他 |
| `test_upgrade_copy_same_size` | 原地拷贝，state 保持 PRISTINE |
| `test_upgrade_copy_grow` | 扩容拷贝，旧偏移下次 ARC 回收 |
| `test_upgrade_shrink_reject` | 缩小 → `ESP_ERR_NOT_SUPPORTED`，无副作用 |
| `test_arc_archived_records` | 按序回收，释放足够空间 |
| `test_arc_exhausted` | 全部 ARCHIVED 耗尽 → false |
| `test_arc_preserves_dirty` | DIRTY 记录不受 ARC 影响 |
| `test_v1_orphan_data_ignored` | play_id=0 记录全跳过 |

黄金夹具：复用 `tests/fixtures/carve_migration_table.bin` 加扩展 store
快照（混合状态 PRISTINE/DIRTY/ARCHIVED 共 4 条数据记录）。

## 10. 边界案例

- **V1 孤儿数据**（`play_id=0`）：跳过所有生命周期操作。永驻 store（或
  等 ARC——当前实现也跳过）。未来：加"孤儿清理"UI 选项。
- **多玩法标签共享（M4）**：两个玩法声明相同 `(label, type)` 映射到同
  一物理分区，各自有独立 `play_id` 数据记录。对其中一个 ARC 会擦掉共
  享字节——**破坏另一个玩法的数据**。守卫：ARC 时检查同 `(label, type)`
  是否还有活跃（DIRTY/PRISTINE）记录，若有则拒绝并告知 UI。实现：
  扩展 `meta_carve_data_label_reserved` 接受 manifest 声明的"共享"标
  签，并在 ARC 路径加守卫。
- **断电 mid-copy（升级）**：字节可能半拷贝。下次启动 `meta_carve_flash_ensure`
  校验 store CRC；撕裂则旧 A/B 胜出。旧偏移仍有内容，新偏移半份。下
  次启动数据记录仍是 PRISTINE，下次安装重跑拷贝。可接受。
- **断电 mid-state-flip（ARC、归档、set_dirty）**：A/B 轮转保护 store
  （与 §4.7 "store 提交中途" 同崩溃故事）。Flash 擦除（ARC 字节）是
  扇区粒度；撕裂擦除后扇区处于 erased 状态，是安全基线（字节读 0xFF，
  下次 FS 格式化自动恢复或干净失败）。
- **PRISTINE 签名绑定**（`meta_sign_sha256`）：PRISTINE 数据分区可能携
  带预填内容（如 GameBoy `roms` 出厂自带 ROM 包）。DIRTY 翻转后 sha256
  失效。设计决策：**数据记录不存 sha256**。PRISTINE 内容信任来源是池
  内 carve 偏移处的字节；验证仅在安装时（手机重发内容填充数据分区）；
  之后状态转换即是充分信任信号。

## 11. 待决项（实现前确认）

1. **DIRTY 标记时机**：`finalize_locked` 成功后 vs. `app_main` 启动扫描
   已选槽位。finalize 路径更干净（单次调用，无双重触发）；启动扫描对
   finalize 与首次启动之间的断电更鲁棒。**建议 finalize 路径**——DIRTY
   是 ARC 优化，不是正确性闸门；漏标只是浪费少量归档空间，不丢数据。
2. **卸载默认归档 vs. 显式擦除**：默认 ARCHIVE 更安全（用户数据保留）但
   累积池债。显式擦除（UI 选择）作为 opt-out。**建议默认 ARCHIVE + UI
   确认**。
3. **M4 共享标签 ARC 守卫**：在两个活跃记录共享同一 label 时，ARC 必
   须等另一方也 ARCHIVED 才可执行。**建议 yes**——否则 ARC 静默破坏其
   他玩法数据。

## 12. 导入失败处理（格式不兼容）

当用户尝试导入旧版备份到新玩法版本时，可能因数据格式不兼容导致失败。
必须**准确告知用户原因**，而非静默丢弃数据。

### 12.1 格式版本验证

每个数据记录新增 `format_version` 字段（可选，默认 1）：
```c
typedef struct {
    uint32_t play_id;
    uint32_t offset;
    uint32_t size;
    uint8_t  state;
    uint8_t  subtype;
    uint8_t  type;
    uint8_t  format_version;   // 新增：数据格式版本（默认 1）
    char     label[META_DATA_LABEL_MAX + 1];
} meta_carve_data_t;
```

玩法 manifest 中声明期望的 `data_format_version`。

### 12.2 导入失败场景与用户提示

| 失败场景 | 错误码 | 用户提示（示例） |
|---|---|---|
| `format_version` 不匹配 | `ERR_DATA_FORMAT_MISMATCH` | "备份数据格式不兼容。当前玩法需要格式 v2，但备份为 v1。请先在旧版玩法设备上恢复此备份，再重新导出。" |
| `label` 不存在于新版 | `ERR_DATA_LABEL_MISSING` | "无法恢复数据：备份中的 'recordings' 分区已在新版中移除或改名。请检查玩法更新说明。" |
| 文件大小超过新分区限制 | `ERR_DATA_SIZE_EXCEED` | "备份数据大小 (X KB) 超过新版分区限制 (Y KB)。请先删除部分数据后再导入。" |
| 校验和失败 | `ERR_DATA_CORRUPT` | "备份数据已损坏，无法恢复。请重新导出或联系玩法作者。" |

### 12.3 降级恢复路径

当格式不兼容时，提供**明确的降级恢复指引**：
1. 提示用户下载旧版玩法（保留原数据格式）
2. 在旧版设备上恢复备份
3. 重新导出数据（新版格式）
4. 升级到最新版

**关键原则**：不自动尝试格式转换（元数据层不懂业务格式），而是明确告知用户"这是玩法层面的问题，需玩法作者提供迁移工具"。

### 12.4 UI 交互流程

```
用户点击"导入备份"
  → 解析备份文件头（检查 format_version、play_id、labels）
  → 对比目标玩法的 manifest 声明
  → 若不兼容：
      · 显示具体错误原因（哪一项不匹配）
      · 提供"查看详情"按钮（展开技术细节）
      · 提供"降级恢复指引"链接（跳转帮助页）
  → 若兼容：正常导入流程
```

**禁止行为**：
- 静默跳过不兼容的数据（用户无感知丢失）
- 模糊的错误提示（如"导入失败"无原因）
- 自动尝试格式转换（可能导致数据损坏）
