// main/meta_store_api.h —— metapass.chuanxilu.net 设备端客户端:
//   analyze 接口(设备 P2 详情页唯一信息源)+ extracted 流式下载刷槽(P4)。
// 信任链(与 tools/install-slot/store-analyze.js 服务端同源):
//   商店公布固件 sha256 → 服务端校验合并镜像 → 解包出 factory 镜像算 extracted sha256
//   → analyze JSON 下发设备 → 设备边下载边算 SHA-256,且与 HTTP 响应头
//   x-image-sha256(同一 TLS 会话内)双重比对,任一不符即中止并作废槽位。
// 线程模型:本模块所有阻塞调用运行在 meta_store_net 的网络任务里;
// UI 只读 meta_store_api_poll() 进度快照,不直接调用本模块 API。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "meta_slots.h"

// 设备唯一信任的服务端(Cloudflare Pages;证书链 GTS Root R4 / ISRG Root X1,
// 打包进自定义 mbedTLS 证书包,见 sdkconfig.defaults)。
#define META_STORE_API_BASE "https://metapass.chuanxilu.net"

#define META_STORE_API_NAME_MAX 48

// analyze 响应的物化结构(契约见改造方案 §1:与 /api/analyze JSON 字段一一对应)。
typedef struct {
    char     name[META_STORE_API_NAME_MAX];  // MNAM(可打印 ASCII)或商店 slug
    uint32_t image_len;                      // 解包后 factory 镜像字节数
    uint8_t  sha256[32];                     // 解包后镜像 SHA-256(设备下载时流式比对)
    int8_t   suggested_slot;                 // 最小可装槽位;-1 = 不可装
    bool     supported;
    char     reason[24];                     // not-found / unavailable / format /
                                             // no-factory / wrong-chip /
                                             // custom-partitions / too-large / ok;
                                             // supported=true 时 custom-partitions
                                             // 表示警告可继续(见 detail)
    char     detail[24];                     // reason 的补充参数(警告分区名等);
                                             // 空串 = 无补充
} meta_store_analysis_t;

// analyze 响应体解析在独立编译单元 main/meta_store_analysis.{h,c}(纯逻辑,
// 与 meta_store_json 同模式,host 合同测试可直接链接同一份代码)。

// UI 轮询用的下载进度快照(纯数据)。
typedef struct {
    bool     active;          // 有下载任务进行中
    int      progress_pct;    // 0..100
    uint32_t received;        // 已收字节
    uint32_t expected;        // content-length;0 = 未知
    bool     verify_phase;    // 下载完毕、正在 esp_ota_end/镜像校验
    char     message[32];     // 英文短句,直接上屏
} meta_store_api_progress_t;

// GET /api/analyze?id=<play_id>,解析并填充 out(零初始化后填充)。
// 网络错误/非 2xx/解析失败均返回 ESP_FAIL,reason 字段给出可展示的原因码。
// 必须在 SNTP 已同步、WiFi 已连接后调用(由 meta_store_net 保证)。
esp_err_t meta_store_api_analyze(uint32_t play_id, meta_store_analysis_t *out);

// GET /api/extracted?id=<play_id>,流式写入 slot 槽位。
//   analysis  : P2 页取得的 analyze 结果;下载流会与其 image_len/sha256 比对
//               (防"P2 之后上游换了 revision"的 TOCTOU,不一致即失败要求重新 analyze)。
//   slots     : 启动器槽位注册表;仅"闪存已被改动"的失败路径会作废对应条目
//               (下载中中止/校验失败/begin 可能已擦除),纯网络与 TOCTOU 校验
//               失败发生在写 flash 之前,注册表保持原状。
// 成功返回 ESP_OK;写到一半的任何失败都会 esp_ota_abort(半成品不可启动)。
esp_err_t meta_store_api_install(uint32_t play_id, int slot,
                                 const meta_store_analysis_t *analysis,
                                 meta_slot_info_t slots[META_SLOT_COUNT]);

// 进度快照(LVGL/任意任务上下文可读;由网络任务写,字段为短字宽或短字符串,
// 允许读到短暂过渡态)。
void meta_store_api_poll(meta_store_api_progress_t *out);

// 请求取消进行中的下载(任意任务可调用;网络任务在当前块读完时响应)。
// 取消按失败处理:半成品槽位照旧作废。仅 install 执行期有效;分析阶段
// 请求会在 install 开始时清除,避免误杀下一次下载。
void meta_store_api_request_cancel(void);
