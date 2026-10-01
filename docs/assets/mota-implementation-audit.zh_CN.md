[English](mota-implementation-audit.md) | 简体中文

# feat/mota 实现审计（r10.22 / r10.22b）

对 `feat/mota` 分支上的局域网手机辅助安装实现，对照已确认的设计文档
`docs/assets/lan-pair-install-design.md` 进行审计。范围：提交
`0c004a9`（设计稿）→ `aaf1b3a`（设备侧 cutover）→ `f6c8bcc`（手机 web 模块）→
`859c4af`（清理）。方法：两路独立 reviewer（固件端；手机/Worker 端）、对
`https://metapass.chuanxilu.net` 的生产实测探针、host 测试套件
（`./tools/validate.sh --static`），以及在并发结论依赖 ESP-IDF 语义处直接核对本地
v5.5.3 源码。

修复状态图例：✅ 本轮已修 · ⬜ 未修（注明原因）。

## 总体结论

协议/状态机层与设计高度一致，若干细节优于设计（见"优于设计"节）。但**尚不可合并**：
手机侧两个独立缺陷各自完全阻断用户流程；设备侧 UI 取消路径会在写入中途释放仍在使用的
OTA handle。以下全部发现均有证据支撑，无推测成分。

## 阻塞性问题

### B1. boot() 上传合并镜像而非剥离后的 app 镜像 — ✅

- 位置：`install-slot/phone-install.js:534`（自动挂载 UI 路径）。
- 问题：`install()` 调 `runInstall(bridge, pre.offer, pre.merged, …)`，而
  `runInstall` 强制 `upload.length === offer.imageLen`（剥离后 factory app 长度，
  `phone-install.js:386`）。`pre.merged` 是完整合并镜像（bootloader + 分区表 + app），
  守卫必触发 `upload buffer missing/length mismatch`。stub bridge 下已复现。
- 测试为何全绿：所有测试都直接给 `runInstall` 传 `pre.ext.data`；没有任何用例走
  `boot()`/`install()` —— 恰恰是真实用户唯一入口。从设备 QR 页发起的每次安装都会在
  用户物理确认槽位之后失败。
- 修复：改传 `pre.ext`（runInstall 接受 `{data}` 形态），一行；补 boot 路径回归测试。

### B2. getPlayDetail 调 `/api/plays/id/<id>`，Worker 从未路由该路径 — ✅

- 位置：`install-slot/phone-install.js:206`。
- 问题：Worker 只路由精确的 `/api/plays`（`_worker.js:211）与 `/api/play?id=`
  （`_worker.js:213-216`）；`/api/plays/id/<id>` 落到 `env.ASSETS.fetch` → Pages 404。
  生产实测：`GET /api/plays/id/563` → 404，`GET /api/play?id=563` → 200。
- 后果：`preflight()` 的 `market` 步骤在生产必败，流程永远到不了下载/校验/上传。
- 修复：手机改调 `/api/play?id=<id>`（Worker 已代理到上游详情端点）；host 测试 mock
  对齐 Worker 真实路由表。

### B3. Worker `/api/plays` 代理丢弃整个 query string — ✅

- 位置：`install-slot/_worker.js:211`；`tools/install-slot/server.mjs:172` 同款 bug。
- 问题：`proxy("/api/plays")` 拼 `BACKEND + upstreamPath` 不带 `url.search`。
  生产实测：`/api/plays?q=<无意义词>` 与无 query 返回完全相同的 673 条未过滤列表；
  同一 `q` 直打上游返回 0 条。设计 §4.3 第 1 条要求转发受支持参数（尤其 `q`）；
  §11 验收"中英文关键词搜索返回官方市场结果"不成立。`worker_contract.mjs` 抓不到
  —— 它只 grep 源码里的端点字符串，不执行路由。
- 修复：Worker 改 `proxy("/api/plays" + url.search)`；`server.mjs` 同步。

### B4. UI 取消与在途 chunk 写入竞争：`esp_ota_abort` 释放仍在使用的 OTA handle — ✅

- 位置：`main/meta_store_install.c:552-565`（`meta_install_cancel`），由按键/UI 任务调用
  （`main/main.c:1153-1155`）；竞争对象是同模块 httpd 任务上的
  `meta_install_chunk_write`（`meta_store_install.c:400-440`）。
- 问题：取消路径无同步地执行 `offer_and_upload_clear()` → `esp_ota_abort()` +
  `mbedtls_sha256_free()`，而并发方正在同一 handle/context 上执行
  `esp_ota_write()` / `sha256_update()`。已核对本地 ESP-IDF v5.5.3：
  `esp_ota_abort`（`esp_ota_ops.c:438-448`）无锁执行 `LIST_REMOVE` + `free()` →
  在途 write 解引用已释放的 `ota_ops_entry_t`（heap use-after-free）。手机侧
  `/api/install/cancel` 安全（与 chunk handler 同 httpd 任务）；
  `meta_install_net_stop()` 基本安全（`httpd_stop()` 先排干在途 handler）。危险路径是
  上传中设备端 OK 长按取消。
- 修复：用会话互斥锁串行化 UI 任务入口（`cancel`/`token_stop` 及其清理的会话状态）与
  httpd 侧上传路径。

### B5. 取消落在 `esp_ota_begin` 擦除窗口：槽位被毁但未标 INVALID — ✅

- 位置：`main/meta_store_install.c:407-424`。
- 问题：`flash_touched` 在 `esp_ota_begin` 返回后才置位，但带非零长度的 `begin` 在
  内部同步擦除 `ALIGN_UP(image_len, erase_size)`（IDF v5.5.3
  `esp_ota_ops.c:189-197`）—— MB 级槽位是数秒窗口。窗口内到来的取消（或离店）看到
  `flash_touched==false`/`ota_open==false`，既不 abort 也不标 INVALID；`begin` 返回后
  handler 继续写并把状态从 `cancelled` 翻回 `uploading`（`:436-437`）。净效果：旧固件
  被擦/半覆写，注册表到下次开机扫描前仍显示 VALID，UI 在明确取消后显示复活的
  上传。设计 §6.5/§8 强制要求 INVALID 标记。
- 修复：`esp_ota_begin` 调用前先置 `flash_touched=true`；并入 B4 互斥锁，使取消与
  begin/write 完全不能交错。

## 非阻塞问题

### M1. 设备永远到不了 `done` 时 runInstall 仍报成功 — ✅

- 位置：`install-slot/phone-install.js:437-445`。
- 问题：finalize 返回 200 后，完成轮询限 30 s；超时时 `pollUntil` 伪造
  `state:"timeout"`，状态读异常被吞成 `null`，两条路都落到 `{ok:true, slot}`
  —— 接受了 finalize 但随后卡死（或 status 端点失联）的设备被报成安装成功。
- 修复：`timeout`/不可读一律返回
  `{ok:false, stage:"finalize", reason:"device did not reach done"}`。

### M2. 会话恰在 `imageLen` 处续传被拒，而非跳到 finalize — ✅

- 位置：`install-slot/phone-install.js:378-383`。
- 问题：设备上报 `offset === offer.imageLen`（上次上传已完成但 finalize 未发出
  —— 正是 §6.5"从设备上报 offset 续传"场景）时，runInstall 报
  `device offset … already beyond image length`。用户不在设备上取消就无法完成
  这种中断的安装。
- 修复：接受 `offset === imageLen` 直接进 finalize；仅 `offset > imageLen` 拒绝。

### M3. Worker `/api/firmware` 整包缓冲而非流式转发 — ✅

- 位置：`install-slot/_worker.js:100-119`（`proxy()` 里 `await upstream.arrayBuffer()`）。
- 问题：每个数 MB 合并镜像在 isolate 内存里完整物化后才向手机发出第一个字节；
  增加 TTFB，并发下载有 isolate 内存上限风险。设计 §4.3 第 4 条明确要求流式。
  本地 dev server 已正确流式（`server.mjs:94-124` 带背压管道），Worker 是落后者。
- 修复：`return new Response(upstream.body, …)`，转发 content-type 与 content-length。

### M4. 版本错位窗口：no-store 入口模块 import 了缓存 4h 的未版本化模块 — ✅

- 位置：`install-slot/_worker.js:508-515`。
- 问题：`/phone-install.js` 是 `cache-control: no-store`，但它的静态 import
  `./extract-app-image.js`、`./store-analyze.js`、`./name-blob.js` 落到
  `env.ASSETS.fetch`，线上以 `public, max-age=14400` 伺服（三个文件均 curl 验证）。
  每次部署后，手机可能加载到新的 phone-install.js 却 import 最旧 4 小时的依赖
  —— 设备协议握手覆盖不到这种跨模块错位。设计 §4.3 第 5 条仅允许对内容寻址或
  版本化资产做 immutable 缓存。
- 修复：三个 import 模块与 phone-install.js 走同一条 no-store 路径。

### M5. 设备 `meta_install_finalize` 忽略 MNAM 显示名写入失败 — ✅

- 位置：`main/meta_store_install.c:523-540`。
- 问题：MNAM 尾 sector 写失败只打日志；函数继续走并返回 `ESP_OK`，而注册表已先把
  槽位标成 VALID —— 槽位通过校验但没有显示名。设计要求显示名 blob 写入必须成功。
- 修复：MNAM 写失败 → finalize 失败（经既有 `flash_touched` 路径置槽位 INVALID）。

### M6. 上传无停滞超时：手机消失后 store 会话被永久吊住 — ✅

- 位置：`main/main.c:673-675`（`store_tick`）。
- 问题：`state=="uploading"` 是闩锁状态，每 250ms 一拍都给 store 死线续期；
  没有任何东西把续期挂在真实 chunk 到达上。上传中手机消失（浏览器关闭、WiFi
  漫游）后不再有 chunk，死线永不触发，设备无限持有 WiFi + install httpd +
  打开的 OTA 会话。设计 §6.5 规定"中断后会话保持到超时或显式取消"——停滞上传的
  超时半边未实现。
- 修复：按上传活动（chunk 到达 / session 打开）续期，不按闩锁状态；加停滞阈值
  （如 30 s 无 chunk），之后走正常 store 死线路径。

### M7. 迟到 prepare 可擦除已完成的物理槽位确认 — ✅

- 位置：`main/meta_store_install.c:773-809`。
- 问题：`h_install_prepare` 只在入口检查一次 `confirmed || session_opened`，随后
  阻塞在 `req_body()` 最多 ~10 s，然后无条件清场装新 offer。若用户在这窗口内完成
  物理确认，prepare 完成时会静默抹掉确认：UI 永远停在上传页，手机的 `session`
  调用 409 "not confirmed" 失败。死锁到会话超时或手动退出。
- 修复：body 读完后（在 B4 互斥锁内）复查 `confirmed || session_opened`，冲突回 409。

### M8. 详情页缺 `updatedAt`，违反设计元数据契约 — ✅

- 位置：`install-slot/phone-install.js:525-531`（`showDetail`）。
- 问题：`normalizePlay` 保留了 `updatedAt`（`:184`），但详情渲染只显示
  名称/大小/revisionId/sha 前缀。设计 §5/§6.2 与 §11 要求详情页显示更新时间。
- 修复：一行 UI 补全。

### M9. 注释/配置中残留对已删 WAN 模块的引用 — ✅

- 位置：`main/meta_store.h:20-24`（消费者写成 `meta_store_api`，实际消费者是
  `meta_store_install.c`）；`sdkconfig.defaults:27-29`（用已删组件
  `esp_http_client`/`esp-tls` 的诊断日志为 INFO 级辩护）与
  `sdkconfig.defaults:109` 附近的节标题"商店下载通道(TLS + SNTP)"（下面已是
  LAN 调优配置）。
- 修复：仅改注释，无行为变化。

## 优于设计（保持原样）

- 配对码具体化：6 位 / 5 分钟 TTL / 5 次尝试上限，token 生命周期绑定 store 会话；
  `meta_install_token_stop` 在 flash 动过时任废槽位。
- chunk 上传拒绝 chunked-transfer 请求体（`content_len==0`），不让 httpd 隐式缓冲。
- 状态机严格有序：prepare→confirm→session→chunk→finalize 无乱序转移；
  `flash_touched` 防跨重启续传。
- resume 语义正确：设备 offset 是唯一事实源；半块写后从设备上报 offset 续传；
  原地重试封顶 3 次。
- 手机 preflight 三道门（size / store sha / analyze-vs-本地剥离）完整实现；
  纯 JS SHA-256 与 node:crypto 对拍 9 组向量。
- `meta_store_json_get_array_*` 助手接受灵活的局部槽位数组。

## 本轮关闭的测试套件盲区

- `tests/test_phone_install.mjs` 的 metapass mock 实现的是 `/api/plays/id/<id>`
  （镜像手机模块的期望）而非 Worker 真实路由表 —— B1/B2 类 bug 在它面前全绿。mock
  现在镜像 Worker 真实路由，并断言 `q` 转发。
- `tests/worker_contract.mjs` 是源码文本匹配；新增可执行式门：`/api/plays` query
  转发、`/api/firmware` 流式（proxy 内无 `arrayBuffer`）、手机模块全部 import 的
  no-store、手机 `getPlayDetail` 路径必命中一条 Worker 路由。
- 新增回归测试：boot 路径 payload 形态（merged vs extracted）、恰在 `imageLen`
  处续传、finalize 完成轮询超时 → 失败。

## 部署说明（非代码修复）

线上 `/phone-install.js` 返回 404 是因为 `feat/mota` 尚未合并部署（Worker 路由与
ASSETS 包都从 `main` 发布）。合并 + `workers.yml` 部署后 B1-B4/M3/M4 方可在生产
生效。此前的线上探针已记录修复前状态。

## 验证

- `./tools/validate.sh --static` —— 修复后 PASS（含新增回归用例）。
- 固件 rebuild 未在审计 shell 重跑（本地 ESP-IDF 工具链缺 xtensa 组件）；固件改动
  限于 `meta_store_install.c` / `main.c`，由 host model 测试 + 桩语法检查覆盖。
  打 tag 前需重建。
