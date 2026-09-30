[English](lan-pair-install-design.md) | 简体中文

# 局域网手机辅助安装 — 设计(r10.22 提案)

> 状态:**设计稿,未实现**
> 日期:2026-09-30
> 起因:C3 广域路径实测均值 5.7kB/s、30 秒僵尸停顿(v50 日志);同一 WiFi 到国内
> 市场源站却是多 MB/s。瓶颈在广域跳,不在 radio。

---

## 0. 目标与非目标

**目标**:手机扫码(后期:手输 IP)打开预加载页面,本地渲染市场内容,用户选定
玩法后**在手机上**下载固件,必要时本地分离,再经**局域网**推给设备 —— 完全绕开
设备的慢广域路径。

**非目标**:

- 不替代设备端商店 UI(保留;这是新增的快速车道)。
- 不新增任何后端服务。现有 Cloudflare Worker 继续提供目录与字节,只加静态资源
  和一个设备端端点。
- 不动信任链。设备仍是最终权威(`esp_ota_end` + 对照 analyze 的 SHA-256)。

## 1. 为什么浏览器安全模型决定了这个拓扑(已实测的约束)

| # | 约束 | 实测事实 | 推论 |
|---|---|---|---|
| C1 | **Mixed content** | `https://` 页面被禁止 fetch `http://192.168.x.x`;C3 做设备端 TLS 不现实 | 落地页**必须从 http 源打开** —— 只有设备能自托管(`http://<设备IP>/install`) |
| C2 | **CORS:我们的 Worker** | `/api/plays`、`/api/play`、analyze、extracted 均回 `access-control-allow-origin: *` | 手机 JS 可跨源读目录与字节 ✓ |
| C3 | **CORS:市场源站** | folotoy 只回 `access-control-allow-credentials: true`,无 allow-origin | 手机 JS **读不了**官方站字节;目录/字节必须走我们的 Worker 代理(现成) |
| C4 | **https → http 脚本** | 从 `http://` 页面加载 `https://metapass…/loader.js` 合法 | "从 metapass 下载一个 js"的需求成立:设备只放一个极小 HTML,真逻辑从 Worker 加载 |

## 2. 架构

```
设备屏:  "http://192.168.x.x/install"  (后期二维码)
   │
手机: 设备自托管落地页(http 源 —— C1)
   ├─ <script src="https://metapass.chuanxilu.net/install-loader.js">   (C4)
   ├─ 目录经 Worker 代理(CORS *,C3) → 本地渲染
   ├─ 选定玩法 → /api/analyze → { extracted.sha256, imageLen, slots, dl 票据 }
   ├─ 手机 WiFi/蜂窝 拉 2.66MB extracted(实测 269-678kB/s)
   │    (解包模块现成且极小 —— 10.7KB 纯 ESM —— 留作"直拉官方 2.73MB 容器"
   │     时的手机端分离路径)
   └─ POST http://<设备IP>/ota/install (同源;sha256 + slot + 配对码)
        └─ esp_http_server 流式写 esp_ota_write → esp_ota_end → boot policy
```

最后一跳纯局域网:2.66MB 秒级完成。镜像字节完全不经过慢的广域路径;Worker
的角色收缩为元数据 + 备份字节源(设备端 OTA 车道原样保留作回退)。

## 3. 组件

### 3.1 设备侧(`main/`)

- `install_page` — 静态 HTML(gzip 后 ~1-2KB),由现有 `esp_http_server`
  (配网页在用,已在构建里)在 `/install` 服务。
- `/ota/install` POST 端点:
  - 头:`X-Meta-Slot`、`X-Meta-Len`、`X-Meta-Sha256`、`X-Meta-Pair`
  - 流式:chunked 读取 → 逐块 `esp_ota_write`(与下载循环同模式;不整包缓冲)
  - 头检查在 `esp_ota_begin` **之前**(拒绝的请求不碰 flash 擦除):
    配对码不匹配 → 403;槽位不适/未知 → 400;len 超槽 → 413
  - 流结束后:流式 SHA-256 对照 `X-Meta-Sha256`,然后 `esp_ota_end`,
    再经现有槽位/boot-policy 代码设启动槽
- 配对码:6 位数字,安装页激活期间显示在设备屏;`/ota/install` 不回带匹配码
  一律 403。缓解"局域网任意访客可写 flash"。

### 3.2 Worker(`install-slot/`)

- `/install-loader.js` — 手机端整个应用(目录渲染、analyze、下载、进度、推送)。
  静态资源,后端零改动。
- CORS 已开(C2)。票据/analyze 一切照旧。

### 3.3 手机页面流程

1. 探测设备源 `GET /hello`(不通则明确报"手机不在同一 WiFi" —— 唯一真实 UX 失败点)
2. 目录(Worker)→ 本地渲染 → 用户选玩法
3. analyze(Worker)→ 本地做槽位适配检查,不适槽置灰
4. 下载 extracted(Worker;`Range` 续传照常适用)
5. 推送设备带进度;设备校验后重启进槽

## 4. 安全模型

- **flash 写授权**:配对码强制(否则 403)。配对窗口只在设备显示期间存在。
- **字节完整性**:不变且设备权威。手机喂不了任意字节:SHA-256 必须匹配
  analyze,`esp_ota_end` 复验镜像,boot policy 复查槽位签名链。
- **无新密钥**:手机上没有任何 secret;配对码瞬态且仅局域网内有效。

## 5. 风险 / 待定问题

| 风险 | 缓解 / 状态 |
|---|---|
| 手机在蜂窝上打开二维码 | 页面先探测设备,显式报错 + 重试提示(r1) |
| iOS Safari + 局域网 http | 从 http 源页面访问合法;落地页**永久**保持 http 源(千万别"升级"成 https —— 那会永久毁掉 C1) |
| 2.7MB POST 的 httpd 内存 | chunked 流式,与下载循环同模式;bring-up 时实测 |
| 配网页与安装端点并发 | 同一 server 实例不同 URI;预期无冲突,验证 |
| LVGL QR 组件未编入 | v1 屏显 IP;二维码后补 |

## 6. 实施顺序

1. 设备:`/hello` + `/ota/install` + 配对码(门禁逻辑配 host 测试)
2. 设备:`/install` 落地页
3. Worker:`install-loader.js`(目录/analyze/下载/推送)
4. 门禁:头/错误码合同测试,局域网真机 E2E
5. 文档:BUGS/CHANGELOG 随代码落地

## 7. 成功标准

- 基准网络下 2.66MB 玩法从"扫码到重启进新固件"**1 分钟内**
  (对照广域路径 ~8 分钟)。
- 信任链零改动;设备端安装车道原样保留作回退。
