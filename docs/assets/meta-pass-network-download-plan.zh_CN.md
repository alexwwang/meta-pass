[English](meta-pass-network-download-plan.md) | 简体中文

# meta-pass 商店通道改造方案 v3.2 —— 服务端解包 + 数字 ID 直达（本期核心版）

> 基准：GitHub `alexwwang/meta-pass` main 分支（v1.0.0, commit `e08dc4d`）
> 服务端：metapass.chuanxilu.net（现有托管 + 本次扩展）
> 固件样本：FoloToy AI Passport 应用商店（plays #563, slug `ai-passport-9`, revision 1279-4）
> 日期：2026-09-24

---

## 0. 本期范围（v3.2 收敛）

**做什么（本期唯一交付链路）**：

```
用户在官方市场网页/App 看到好玩的玩法 → 记住玩法编号(ID)
  → 设备 STORE:输入编号 → 一次调用 /api/analyze 拿到 {应用名称, 能不能装, 可装的最小槽位}
  → 确认安装 → 槽位选择(默认最小槽位) → OTA 下载已分离固件 → 刷入所选 slot
  → 提示重启 → 启动器菜单选择子固件运行
```

**信息获取约束（v3.2 修订）**：设备**不在本地拉取商店详情**，对商店详情接口零调用；
应用名称、能不能装、可装的最小槽位三项信息**只通过 metapass.chuanxilu.net 的
`/api/analyze` 一次请求获得**，随后界面上只提供"确认安装"按钮进入下一步。
名称由服务端给出 ASCII 安全值（slug），顺带解决设备屏无 CJK 字形、MNAM 只收 ASCII 的
约束。

**不做什么（明确推后，调研结论见附录 A，接口事实已确认，后续迭代直接可用）**：
设备端市场登录、我的收藏、分类-列表-详情浏览。理由：设备上登录交互成本高，本期以核心
"OTA 下载刷 slot"功能为先；浏览入口由官方市场网站/App 承担，设备只认编号。

**架构决策（继承 v3，不变）**：
- 拆包/解包**不做进固件**，由 metapass.chuanxilu.net 服务端承担（复用其已托管、与仓库
  `install-slot/extract-app-image.js` 逐字节一致的同一份实现，Node 端 import 同一个 ES module）；
- 设备**只与 metapass.chuanxilu.net 通信**（单一 TLS 信任锚），由它代理商店；
- 固件只做：配网 → 数字输入玩法 ID → analyze（名称/能不能装/最小槽位）→ 确认 → 槽位选择
  → 下载纯 app 镜像 → OTA 写入；设备对商店详情接口零调用。

```
┌──────────────────────┐         ┌──────────────────────────┐         ┌─────────────────────────┐
│  AI Passport 设备      │  HTTPS  │  metapass.chuanxilu.net   │  HTTP   │  ai-passport.folotoy.cn  │
│  meta-pass STORE 固件 │ ◄────► │  (server.mjs 扩展)        │ ◄────► │  (商店 API + 固件分发)    │
│  输ID/详情/选槽/下载  │         │  代理 + 解包分析 + 缓存    │         │                          │
└──────────────────────┘         └──────────────────────────┘         └─────────────────────────┘
```

---

## 1. 服务端设计（metapass.chuanxilu.net 扩展）

### 1.1 现状实测（2026-09-24）

| 端点 | 状态 | 说明 |
| --- | --- | --- |
| `GET /api/play?id=<数字>` | 200 | 商店详情代理（**仅数字 ID**，slug 报 400） |
| `GET /api/firmware?path=<商店路径>` | 200 | 固件字节代理（路径白名单，实测 SHA-256 与商店一致） |
| `/extract-app-image.js` 等静态文件 | 200 | 与仓库 `install-slot/` 逐字节一致 |
| 解包分析 / 已分离固件端点 | 404 | **本次新增** |

### 1.2 端点契约（本期新增 2 个）

#### `GET /api/analyze?id=<数字>` —— 解包分析 + 槽位建议

流程：取详情 → 检查 `ok` / `firmware.available` / `format` → 下载合并镜像（走缓存）→
**校验与商店公布的 `firmware.sha256` 一致** → 用 `extract-app-image.js` 解包 →
得 app 镜像精确长度与 SHA-256 → 与槽位几何表对比 → 返回：

```json
{
  "ok": true,
  "id": 563, "revisionId": 1279,
  "name": "ai-passport-9",
  "store":  { "size": 3219456, "sha256": "0d68...c70a" },
  "extracted": { "imageLen": 1844496, "sha256": "<解包后镜像 sha256>" },
  "slots": [
    { "slot": 0, "limit": 1921024, "fit": true },
    { "slot": 1, "limit": 2093056, "fit": true },
    { "slot": 2, "limit": 2740224, "fit": true }
  ],
  "suggestedSlot": 0,
  "supported": true, "reason": "ok"
}
```

（HTTP 状态码：业务结果一律 200 + 上述 JSON——含 `supported=false` 的原因分支；
设备端对非 200 不读响应体，用 4xx 表达业务原因会把真实 reason 吞成 unavailable。）

- `name`：**应用名称（ASCII 安全，直接取自 slug）**，设备直接显示并写入 MNAM 显示名
  blob；设备因此无需访问商店详情，title.zh 的 CJK 问题在服务端消除；
- `supported=false` 时 `suggestedSlot=-1`，`reason` 取值：`not-found`（ID 不存在）/
  `unavailable`（下架）/ `format`（非 esp-merged-0x0）/ `no-factory`（分区表无 factory
  应用）/ `wrong-chip`（非 ESP32-C3）/ `custom-partitions`（依赖白名单外自定义 data
  分区）/ `too-large`（超出最大槽位）。判定规则与 `extract-app-image.js` 错误分支 +
  商店固件语义一致，服务端实现直接复用。

槽位几何表（服务端常量，与 `partitions.csv` / `meta_sign_app_limit` 同步维护；
app 上限 = 分区大小 − 0x1000，尾部 4KB metadata sector）：
slot0=0x1D5000(1,921,024B)、slot1=0x1FF000(2,093,056B)、slot2=0x29D000(2,740,224B)。
**建议槽位 = 能装下的最小槽位**；都装不下 → `too-large`。

#### `GET /api/extracted?id=<数字>` —— 返回已分离的纯 app 镜像

- 复用 analyze 的缓存结果，`application/octet-stream`，`Content-Length = imageLen`，
  响应头附 `X-Image-Len` / `X-SHA256`（与 analyze 的 `extracted.sha256` 一致）；
- 首版不支持 HTTP Range，设备侧重试 = 完整重下（≤3 次）；
- 实现建议：只缓存 `{imageLen, sha256}` + 合并镜像原始字节（LRU，默认 512MB），
  extracted 请求到来时即时解包流式吐出（CPU 换磁盘）。

#### 缓存策略

- 详情代理：商店标 `no-store` → 不缓存；
- analyze：key=`id:revisionId`，命中也需回源确认 revisionId 未变；
- extracted：由 analyze 缓存派生，不单独落盘。

#### 服务端安全约束

- `id` 数字校验防遍历/SSRF；上游固定 `https://ai-passport.folotoy.cn`；
- 合并镜像下载上限 8MB；analyze/extracted 按 IP 限速；
- extracted 必须来自"sha256 已校验的合并镜像"的解包结果，禁止未校验边下边传。

### 1.3 Cloudflare Pages 部署可行性核对（v3.2-r3 实测结论）

现状：站点托管于 Cloudflare Pages（静态资源 + Pages Functions 提供现有 `/api/*` 代理，
本地开发对应物为 `tools/install-slot/server.mjs`）。本期新增需求逐项核对：

| 需求 | Pages/Workers 支持度 | 方案 |
| --- | --- | --- |
| `/api/analyze`（回源拉 3MB + SHA-256 + 纯 JS 解包） | ✅ 全部兼容 | 解包链 `extract-app-image.js → name-blob.js` **零 Node API、纯 ESM**（已核实 import 链），Functions 直接 `import`；SHA-256 用 WebCrypto `subtle.digest`（原生实现，CPU 毫秒级）；3MB `fetch` + 流式处理内存 ~10MB，远低于 128MB 限制 |
| `/api/extracted`（~2.7MB 字节流响应） | ✅ | Workers 流式响应；R2 命中时 `R2Object.body` 直接 pipe |
| 缓存（无文件系统） | ✅ 换存储介质 | **R2**（强一致，存合并镜像 + analyze JSON，10GB 免费额度足够 512MB LRU 策略）+ **KV**（只做限速计数；KV 有 ~60s 全球最终一致性，不用于 analyze 结果这类要求强一致的读） |
| 速率限制 | ✅ | KV token bucket |
| 免费版限制 | ⚠️ 两项需关注 | ① CPU 50ms/请求：冷 analyze（下载+哈希+解包）实测压测，超了升付费版（$5/月，30s CPU）；② 每日 10 万请求：设备量级远低于此 |
| Functions 脚本体积 | ✅ | 上限 ~3MB，解包模块仅 6KB |

**结论：无需迁出 Cloudflare Pages**。实施约定：Functions 保持薄壳，纯逻辑全部放 ESM 模块
（与 `extract-app-image.js` 双端复用同一模式）；本地用 `wrangler pages dev` 对齐线上行为；
R2 配 lifecycle rule 做缓存淘汰。

### 1.4 信任链

```
商店公布 merged sha256 ──► 服务端下载 merged 并校验一致 ──► 解包 ──► extracted sha256
设备 ◄── 同一 TLS 会话拿到 {extracted sha256} + {字节流} ──► 流式累计 SHA-256 强制比对
```

哈希绑定由服务端在已验证的合并镜像上完成，设备校验接收完整性；残余风险 = chuanxilu 被
攻陷同时替换字节与哈希（TLS + 主机安全承担）。USB 通道保留为侧路。

---

## 2. 固件侧设计（meta-pass STORE 模块）

### 2.1 模块划分与文件变更

| 变更 | 说明 |
| --- | --- |
| `+ main/meta_store_net.c/h` | WiFi STA + SNTP + HTTP 客户端 + 命令队列 + fetch task + 状态快照 poll（对 UI 暴露与 `meta_net` 同构接口） |
| `+ main/meta_store_api.c/h` | chuanxilu API 客户端：有界 JSON 提取器（不引入 cJSON）、play/analyze/extracted 调用、错误映射 |
| `± main/meta_import.h` | 状态机事件改名复用（NET_READY / FETCH_META / RECEIVING / VERIFYING / DONE / ERROR） |
| `± main/main.c` | Import 页替换为 STORE 页组：P0 配网 → P1 输 ID → P2 详情 → P3 选槽 → P4 进度 → P5 完成 |
| `− main/meta_net.c/h` | 删除（配网 AP 网页逻辑并入 meta_store_net，仅保留 ssid/password 表单） |
| `main/CMakeLists.txt` | `REQUIRES` 增 `esp_http_client esp-tls nvs_flash esp_wifi`；ESP-IDF **v5.5.3** 基线，改动后重生成 `dependencies.lock` |
| `sdkconfig.defaults` | `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y`（锚 GTS Root R4，禁止硬编码服务器证书） |
| **固件不含任何解包逻辑** | 非目标 |

### 2.2 页面流（本期六页）

**P0 配网页（NETWORK）**
- 进 STORE 查 NVS 既有 STA 凭证（`esp_wifi_get_config`；存储必须默认 flash——
  `meta_net` 的 `WIFI_STORAGE_RAM` 不带入）→ 有则直接连；
- 无凭证：SoftAP + 极简网页表单收 `ssid/password`（沿用原 meta_net 的 AP 配置），
  保存后关 AP 转 STA；Captive Portal 自动弹窗（DNS 劫持）列为 fast-follow；
- 拿 IP → **SNTP 同步**（3–5s，失败仅记录）→ 进 P1。

**P1 编号输入页（ENTER ID）**
- 3–6 位数字键盘（上/下调位、OK 确认），带已输入位显示与退格（OK LONG 退格/清空）；
- 确认后**一次** `GET /api/analyze?id=`（设备对商店的唯一信息来源）→ P2。

**P2 详情页（DETAIL）**
- 完全由 analyze 响应渲染，**本地零商店详情拉取**：
  - `supported=true` → `<name>\nIMG <解包后大小>  MIN SLOT <建议槽位>` + `INSTALL` 确认项；
  - `supported=false` → `<name|id>\nNOT SUPPORTED` + 原因短句（`TOO LARGE` /
    `NEEDS DATA PART` / `UNAVAILABLE` / `NOT FOUND`…）；
- 本页唯一动作：OK = 确认安装（进 P3）；OK LONG = 返回 P1；
- analyze 请求失败 → `FETCH FAILED` + 原地重试。

**P3 槽位选择页（SLOT SELECT）**
- 三行槽位：EMPTY / 已装名字+大小；**建议槽位默认高亮**；
- 选中槽位 2 且该槽无有效镜像但分区非全 0xFF（littlefs 录音存储，设计文档 §6.3）→
  追加 `WIPES RECORDINGS` 确认文案；
- OK → P4；OK LONG → 回 P2。

**P4 下载页（DOWNLOAD）**
- `GET /api/extracted?id=` 流式接收（独立 fetch task；UI 锁定仅 OK LONG 取消）；
- `Content-Length` / `X-Image-Len` 与 analyze 的 `imageLen` 不一致 → 立即报错；
- `esp_ota_begin(part, imageLen)` → 1–2KB 分块 `esp_ota_write`，同时滚动累计 SHA-256；
- 收完与 `extracted.sha256` 强制比对 → `esp_ota_end` → `esp_image_verify` →
  `meta_slot_set_valid` + MNAM 显示名 blob（取 analyze 下发的 `name`，服务端已保证 ASCII）；
- 重试 = 完整重下（≤3 次，退避 1s/2s/4s）；取消/失败 = `esp_ota_abort` + mark invalid。

**P5 完成页（DONE）**
- `Installed: <name> -> slot N` + `OK = reboot`；OK → `esp_restart()`；
- 重启回启动器列表页，用户在菜单 BOOT 子固件（现有流程，含未签名警告页）。

### 2.3 网络层实施约定

- **线程模型**：fetch task（栈 ≥8KB）跑阻塞 TLS；UI poll 快照；按键经队列发 START/CANCEL；
  RECEIVING 期间导航锁定、仅 OK LONG 取消；
- **WiFi STA 流程**：STA netif → `esp_wifi_get_config` 读既有凭证 → 事件组等
  `IP_EVENT_GOT_IP`（10s×3）→ 中途 `STA_DISCONNECTED` 判 BROKEN 重试；仅 2.4GHz，UI 提示；
- **SNTP**：拿 IP 先同步（`MBEDTLS_HAVE_TIME_DATE` 默认开启，冷启动不校正可能
  `BADCERT_FUTURE`）；
- **HTTP**：串行 client 实例，用完 cleanup；`.max_redirect_count=3`；非 200 按 BROKEN
  重试；10s 零进展看门狗；
- **电池**：`bsp_battery_soc() < 20%`（真机标定）时详情页提示接 USB；
- **会话启停**：离开 STORE 完整 `cleanup → wifi_stop → netif_destroy`；STORE 内保持连接。

### 2.4 固件体积预估与裁剪（保持"单一最小固件"约定）

约定不变：所有新代码进 factory 分区，不拆固件、不加第二应用分区；factory 预算
0x170000 = 1,441,792B（<1.43MB，README/设计文档 §3 约束）。

**体积增量估算（v1.0.0 基线之上的净增）**：

| 组成 | 估算 | 依据 |
| --- | --- | --- |
| mbedTLS TLS 协议层 + X.509 解析 | +40~60KB | SHA-256/ECDSA/bignum 等加密原语**已随 `meta_sign` 链接**（`main/CMakeLists.txt` 已 `REQUIRES mbedtls`），新增仅为 ssl/tls client、x509_crt、ASN1 |
| 证书包 | +2KB ~ +80KB | **本期设备唯一上游是 chuanxilu（锚 GTS Root R4）**：推荐 `CONFIG_MBEDTLS_CUSTOM_CERTIFICATE_BUNDLE` 自定义包只放 GTS R4（预留 ISRG X1 供后续直连商店）≈ +2~3KB；保守备选 `DEFAULT_CMN`（+15~25KB，是否含 GTS R4 需实测）或 `DEFAULT_FULL`（+60~80KB） |
| esp_http_client + esp-tls | +15~25KB | 组件代码 + 配置 |
| lwip DHCP client + SNTP | +3~6KB | lwip 本体已在（SoftAP 用） |
| WiFi STA 模式 | +0~5KB | 与 AP 同一 WiFi lib |
| 新应用代码（meta_store_net/api + 六页 UI + JSON 提取器） | +30~50KB | 参考 meta_net.c（519 行）≈20~30KB + 页面/UI |
| **合计** | **约 +90~170KB（自定义证书包取低端，FULL bundle 取高端）** | 规划值按 **~120KB** 预留 |

**基线余量判断**：v1.0.0 实际镜像大小未在仓库记录（CHANGELOG 无 binary size 条目），
**必须以 `idf.py size` 实测复核**——预算 1.44MB 减去当前镜像即真实余量；按 factory 内容
（LVGL + 音频 codec + button + 签名验签）合理推测余量在数百 KB 级，+120KB 预计可放入，
但"预计"不是"确认"，Phase 2 立项门槛 = 先跑一次 `idf.py size` 拿到当前值再定证书包策略。

**若超支的裁剪杠杆（按性价比排序）**：
1. 自定义双根证书包（省 60~80KB，代价：上游换 CA 时需发版固件；缓解：bundle 放 2~3 张根 +
   USB 升级通道兜底）；
2. mbedTLS client-only 裁剪（禁服务端套件、按需禁 TLS1.3，省 ~20~30KB）；
3. 复用现有 UI 字体/组件，不新增字库（CJK 本来就进不了固件）；
4. 关闭不用的 lwip 特性。

---

## 3. 信任模型与校验策略

| 环节 | 校验 | 信任锚 |
| --- | --- | --- |
| 详情/分析 | HTTPS + JSON 字段白名单 | chuanxilu TLS |
| 解包 | 服务端校验 merged sha256 == 商店公布值后才解包 | 商店公布哈希 → 服务端 |
| 固件落地 | 设备流式 SHA-256 == analyze.extracted.sha256；`esp_ota_end`/`esp_image_verify` 权威校验；不匹配擦槽位 mark invalid | 同一 TLS 会话内哈希绑定 |
| 恶意固件面 | 只能装商店上架 play（服务端分析白名单规则） | 商店审核 + 服务端规则 |
| 侧路 | USB 串口安装保留（自编译/未上架唯一通道） | 物理持有 |

## 4. 风险与缓解

| 风险 | 缓解 |
| --- | --- |
| chuanxilu 服务端不可用 | 设备提示重试；USB 通道兜底；不变砖（bootloader hook 机制不变） |
| 商店 API 改版 | 服务端先行适配，设备契约稳定（analyze/extracted 抽象商店细节） |
| 商店下载 302 迁 CDN | 服务端代理处理重定向，设备契约不变 |
| 商店 play 按 3MB 设计，槽位更小 | analyze 双重拦截（store.size + 解包 imageLen vs 槽位上限），写 Flash 前拒绝 |
| WiFi 断连/超时 | 连接 3 次、下载完整重下 ≤3 次（1s/2s/4s）、10s 零进展看门狗；取消优先，abort + mark invalid |
| 设备时间未同步 | SNTP 先同步（§2.3）；失败不阻断但记录 |
| TLS 握手 OOM | RECEIVING 切静态进度页降 LVGL 占用；`idf.py size` 与堆水位实测复核 |
| 低电棕出 | soc<20% 提示接 USB |

## 5. 分阶段实施与验收

1. **Phase 1 — 服务端**：扩展 `server.mjs`（analyze、extracted），解包直接 import 托管的
   `extract-app-image.js`；Node 单测用 `test-extract.mjs` 构造用例 + play 563 实测固件做
   golden test（**imageLen 必须等于 1,844,496**）；curl 验收各 reason 分支（找一个
   wrong-chip/超尺寸样本 play）。
2. **Phase 2 — 固件网络层**：meta_store_net + meta_store_api + host stub 测试
   （`tests/esp_stubs` 增 esp_http_client/esp_tls stub，风格对齐 `test_meta_net_upload.c`）。
3. **Phase 3 — UI 六页**：P0–P5 + 按键分发 + 槽位注册表回写；设计文档 §6/§7、README 同步。
4. **Phase 4 — 模拟器端到端**：esp_emu WiFi 仿真（`wifi_rx_push`/`wifi_tx_drain`）+
   Node mock server 实现 chuanxilu 契约（桩掉证书校验或注入自签 CA），跑通
   输 ID→详情→选槽→下载→列表可见→BOOT 全流程，免真机免路由器。
5. **Phase 5 — 真机验收**：真网下载 play #563（suggestedSlot=0，imageLen=1,844,496 <
   slot0 上限 1,921,024）安装-重启-运行-回启动器；断电中断下载验证槽位 invalid 与防变砖；
   低电提示；`idf.py size` 复核 factory 1.43MB 预算；SNTP/TLS 冷启动实测。
6. **回归**：`tools/validate.sh`、模拟器用例、`test_meta_net_*` 删除或迁移。

## 6. 关键代码骨架

```c
// main/meta_store_api.h
typedef struct {
    char     name[48];          // analyze.name(ASCII,来自 slug;显示与 MNAM 均用它)
    uint32_t image_len;         // analyze.extracted.imageLen
    uint8_t  sha256[32];        // analyze.extracted.sha256
    int8_t   suggested_slot;    // analyze.suggestedSlot;-1 = 不支持
    bool     supported;
    char     reason[24];        // TOO LARGE / NEEDS DATA PART / UNAVAILABLE / NOT FOUND ...
} meta_store_analysis_t;

esp_err_t meta_store_api_fetch_analysis(uint32_t play_id, meta_store_analysis_t *out);
// 设备侧唯一信息入口:GET /api/analyze?id=(slug/available 预检在服务端完成)

esp_err_t meta_store_api_download(uint32_t play_id, int slot,
                                  const meta_store_analysis_t *meta,
                                  meta_slot_info_t slots[META_SLOT_COUNT]);
// GET /api/extracted?id= → 流式 esp_ota_write + 滚动 SHA-256
//   → 比对 meta->sha256 → meta_slot_install_finish()(esp_ota_end/esp_image_verify/MNAM)
```

```js
// server.mjs 侧(示意)
import { extractAppImage } from "./extract-app-image.js";
app.get("/api/analyze", async (req, res) => {
  const id = validateId(req.query.id);
  const play = await storeDetail(id);
  const merged = await cachedMerged(play);            // 按 sha256 LRU
  assertSha256(merged, play.firmware.sha256);         // 信任链第一步
  const appImg = extractAppImage(merged, Infinity);
  res.json(buildAnalyze(play, { imageLen: appImg.length, sha256: sha256(appImg) },
                        SLOT_GEOMETRY));
});
app.get("/api/extracted", async (req, res) => {
  const { appImg } = await analyzedCache(validateId(req.query.id));
  res.set({ "Content-Length": appImg.length, "X-SHA256": sha256(appImg) });
  res.end(appImg);
});
```

## 7. 附：实测记录（本次方案编制所用）

- 商店：`/api/plays/id/563` 与 `/api/plays/ai-passport-9` 等价；`/api/plays/563` 404；
  play 563 固件下载 SHA-256 与公布值一致；ESP32-C3 / IDF v5.5.3 / merged-0x0；
  证书链 Let's Encrypt YR1 ← ISRG Root YR（X1 交叉签名）。
- chuanxilu：`/api/play?id=`、`/api/firmware?path=` 可用；解包端点 404（本次新增）；
  证书链 GTS WE1 ← GTS Root R4（mozilla bundle 覆盖）。
- 解包 golden 值：play 563 factory app `imageLen=1,844,496B`（段表走读 + %16==15 填充 +
  1B 校验和 + 32B hash，已对尾哈希自校验）。

## 8. 修订记录

- **v3.2-r6（2026-09-25）取消权与超时配置**：① 会话超时改为"到期问用户"：到期不强制
  关网络回列表，屏上出浮层 "Session timeout. OK = continue / LONG = exit store"，冻结
  其他按键语义直到用户决策（OK=继续当前操作并续期 / OK LONG=退出，teardown 照旧停
  网络）；到期前 P0 状态行改显示 "timeout in Ns"。② **取消走确认页**（新增 P4b，
  PAGE_STORE_CANCEL）：下载中 OK LONG 进确认页，后台下载不停，三选一——CANCEL=继续
  取消（`meta_store_api_request_cancel` 块边界响应，半成品槽位作废，失败页显示
  "Cancelled."）；RETRY=取消当前并在作业失败终态自动重新入队同玩法同槽位安装
  （s_store_retry 标记，入队失败兜底落失败页）；BACK=不取消，回进度页继续等。
  install 各终态的精准文案（Cancelled. / Checksum mismatch. / Version changed. 等）
  落进作业消息上屏，esp_err_to_name 仅兜底。③ 超时可配置：新增 `main/Kconfig.projbuild`——
  `CONFIG_META_STORE_SESSION_TIMEOUT_MS`（默认 300000，范围 30s~24h）与
  `CONFIG_META_STORE_HTTP_TIMEOUT_MS`（默认 30000）；会话超时另提供运行时覆盖接口
  `meta_store_session_set_timeout_ms()`（钳制同范围，供后续设置页/NVS 持久化接入），
  两者均无 sdkconfig 时回退同值默认值（host 桩编译路径）。④ 进度实时比对确认：
  下载循环每块以 `received*100/content_len` 刷新百分比，P4 显示 "N%  X/Y KB"，
  结束时再与服务端 analyze 摘要 + HTTP 头三重比对（实现自 r4 起已具备，本次复核无改动）。
- **v3.2-r5（2026-09-25）代码评审修复（k3 review）**：对齐文档目标的全量 review，
  修复与收敛如下。① **致命**：`meta_store_net_init()` 未接收槽位注册表，网络模块内
  `s_slots` 恒为 NULL → install 必失败；init 改签名
  `meta_store_net_init(meta_slot_info_t slots[META_SLOT_COUNT])`，`app_main`/列表页
  两处调用点传入启动器静态注册表。② **契约**:`/api/analyze` 业务结果（含
  supported=false 的原因码）一律 200 + JSON——原 422 会被设备端吞成 unavailable，
  too-large/wrong-chip 等真实原因无法上屏；id 参数校验收敛为 `\d{1,7}`。③ **语义**:
  install 失败分两档——esp_ota_begin 之前（网络/TOCTOU 校验）不碰槽位注册表（闪存未动，
  不允许纯网络错误抹掉既有槽位信息）；begin 及之后（闪存可能已擦）照旧作废。④ **UI**:
  P5 完成页槽位号改用专用变量（`store_goto` 清 `s_sel` 导致原先恒显示 slot 0）；商店
  会话 5 分钟超时在 analyze/下载作业进行中自动延续（慢网络大镜像不再被误杀）。
  ⑤ **健壮性**：`meta_store_json_get_int` 溢出判定改在乘加之前（消除有符号溢出 UB）；
  `sta_online` 等待前清残留 GOT_IP/DISCONNECT 事件位（上轮失败残留不再击穿本轮等待）。
  ⑥ **可读性/去冗余**：删 `s_wifi_mode_ap` 死状态、`s_store_timer_on` 死标志、
  `esp_sntp.h` 未用 include、`loadMerged` 失败分支不可达的 detail 拷贝；修正"断连自动
  重试一次"等两处失实注释；SNTP 超时后的 ONLINE 日志不再声称"时钟已同步"；
  hex64 改查表；content-length 缺失（chunked）的日志不再误称"长度不一致"。
  验证：node 10 用例、host JSON 测试、`-fsyntax-only` 桩检查、`check_repo.py`、
  `validate.sh --static` 全绿。
- **v3.2-r4（2026-09-24）代码实现完成**：按本方案完成 main 分支实现。服务端：
  `tools/install-slot/store-analyze.js`（纯 ESM 分析/分离核心，依赖全部可注入，
  Cloudflare Pages 可直接复用）+ `server.mjs` 新增 `/api/analyze?id=N`、
  `/api/extracted?id=N`（二进制流 + `x-image-len`/`x-image-sha256` 头）；
  Node 单测 `test-store-analyze.mjs` 10 用例全过，真实 play 563 冒烟通过
  （analyze 返回 supported/suggestedSlot=0，extracted 流 SHA-256 与响应头一致）。
  固件：`meta_store_json`（有界 JSON 提取器，host 测试全过）、`meta_store_api`
  （analyze + 流式 OTA，边下边算 SHA-256 与 analyze/响应头双重比对）、
  `meta_store_net`（配网 SoftAP 表单（无配对码）/STA/SNTP/作业队列网络任务）；
  `main.c` STORE 六页（P0 配网→P1 输ID→P2 详情→P3 选槽→P4 进度→P5 重启提示）；
  `meta_net`/`meta_import` 及其测试随上传通道删除；自定义双根证书包
  （GTS Root R4 + ISRG Root X1，`main/certs/`）替换 Mozilla 全量包。
  host 检查：`test_meta_store_json.c` 全过，两 ESP-IDF 模块过 IDF 5.x 签名桩
  `-fsyntax-only`；`tools/validate.sh --static` 全绿。真机构建（`idf.py size`
  体积门槛）与设备端到端为剩余验收项（§5 P4/P5）。
  实施期新发现：play 563 分区表含 `recovery` 应用分区（固件自身回退分区）——
  custom-partitions 白名单收敛为只查数据类型分区（type=1），应用分区内容在
  目标布局惰性；analyze 契约失败分支不带 slots 数组（设备不支持页只展示原因）。
- **v3.2（2026-09-24）范围收敛**：本期只做"数字键盘输入玩法 ID → 服务端解包分析 →
  槽位选择 → OTA 下载刷 slot"核心链路；**删除登录/收藏/分类浏览**（登录交互在设备上
  成本过高，浏览入口由官方市场承担）；P0 恢复为单一步骤配网；服务端相应只保留
  analyze/extracted 两个新增端点。被删功能的接口调研结论保留在附录 A。
- **v3.2-r2（2026-09-24）信息入口收敛**：设备不再本地拉取商店详情，应用名称/
  能不能装/可装的最小槽位只经 `/api/analyze` 一次请求获得；analyze 响应增加
  `name` 字段（服务端取 slug，ASCII 安全），设备侧 slug 相关显示/MNAM 写入全部改用
  `name`；`reason` 增加 `not-found`；固件删除对 `/api/play` 的调用（服务端内部仍用）。
- **v3.2-r3（2026-09-24）可落地性核查**：① 固件体积——净增估算 +90~170KB（mbedTLS
  加密原语已随 meta_sign 链接，大头是 TLS/X509 层与证书包；推荐自定义双根证书包压到
  ~+2KB），规划预留 ~120KB，以 `idf.py size` 实测为立项门槛（§2.4）；② Cloudflare
  Pages 可行性——解包链零 Node API 纯 ESM 可直接在 Pages Functions 运行，缓存改
  R2（强一致）+ KV（限速），免费版 CPU 50ms/请求需压测、超限升 $5/月付费版（§1.3）。
- **v3.1（2026-09-24）**：收藏改官方市场登录方案（短信/密码登录 + Cookie 透传）——
  随 v3.2 整体推后。
- **v3（2026-09-24）**：架构反转——解包移出固件由 chuanxilu 服务端承担；设备单一信任
  锚；analyze 槽位建议/不支持展示。
- v2 继承且仍有效的约定：线程模型、SNTP、重试/取消矩阵、MNAM ASCII 约束、电池提示、
  模拟器 WiFi 端到端、IDF v5.5.3 基线等（§2.3、§4、§5）。

---

## 附录 A：后续迭代预留（接口事实已调研确认，未实现）

**A.1 官方市场登录与收藏**（v3.1 已完成调研）：
- 短信登录：`POST /api/auth/phone-login/verification-code/request` `{phone}` →
  `POST /api/auth/phone-login` `{phone, verification_code, locale}`；
  密码登录：`POST /api/auth/login` `{email, password}`；会话 cookie + CSRF
  （`GET /api/session` 下发 csrfToken）；登录态 `GET /api/me` → `{ok, user|null}`；
- 收藏开关 `GET/POST/DELETE /api/me/favorites/play/<id>`（GET 返回 `{saved}`；与
  "关注" `/api/me/follows/...` 是两套）；收藏列表 `GET /api/me/favorites?kind=play`（未登录 401）；
- 设备交互草案：配网网页第 2 步"市场登录"（手机浏览器填手机号/收码，设备代发验证码请求），
  Cookie 存 NVS `mpsess`；chuanxilu 增加 `/api/auth/*`、`/api/me/*` Cookie/CSRF 透传白名单；
- Phase 1 待实测项：收藏列表响应结构、收藏开关 CSRF 头名、会话过期形态。

**A.2 分类-列表-详情浏览**：
- 数据源：商店列表响应自带 `categoryCounts`（key→计数）与 `discoveryTags`
  （key/name.zh/en/sortOrder）；无独立分类端点；chuanxilu 的 `/api/plays` 代理当前
  **丢弃这两个字段**，做浏览时需补齐透传（建议附加 `asciiName` 短标签，因设备屏无 CJK 字体）；
- 已知分类 key：must-play / multi-device / child-friendly / games / productivity /
  social / learning / information / developer / media。
