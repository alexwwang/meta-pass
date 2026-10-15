// main/meta_store_install.h —— LAN 手机辅助安装的设备侧通道(feat/mota v1)。
//
// 职责(设计文档 docs/assets/lan-pair-install-design.md):
//   §4.1 设备自托管 boot 页面 + 同源 DeviceBridge;
//   §6.5 本地 install HTTP 面(prepare/session/chunk/finalize/status/cancel/pair);
//   §8   一次性 token、配对码兜底(短 TTL/尝试上限/一次性)、物理确认门控。
// manifest 形状/边界、槽位几何复核、chunk 偏移判定全部在 meta_install_model
// (纯逻辑,host 测试同一份);本模块只做 HTTP/OTA/token 副作用与上屏快照。
//
// 线程模型:URI handler 运行在本地 httpd 任务;UI(LVGL/按键任务)只读快照,
// 经 meta_install_confirm_slot()/meta_install_offer_reject()/meta_install_cancel()
// 提交物理动作。与 meta_store_net 的 httpd 纪律一致,UI 不直接碰阻塞网络调用。
//
// v1 信任模型(文档 §3/§12):设备不向 metapass 重查 analyze,只校验 manifest
// 形状/边界、上传长度/sha256、ESP 镜像结构;剩余保护 = 一次性 LAN token、
// 物理槽位确认、image_len/sha256/slot 强绑定、esp_ota_end/esp_image_verify、
// 失败后槽位 INVALID。签名 install manifest 是后续加固项。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "meta_slots.h"
#include "meta_install_model.h"

// ---- 本地 HTTP 面(文档 §6.5;所有本地 API 的自定义头) ----
#define META_INSTALL_SESSION_HDR "X-Meta-Session"   // token hex;全部端点必带
#define META_INSTALL_OFFSET_HDR  "X-Meta-Offset"    // chunk 写偏移(十进制)
#define META_INSTALL_DATA_INDEX_HDR "X-Meta-Data-Index" // Child DATA manifest index

// QR token:128 bit 随机(文档 §8 下限),hex 形态上屏/入 URL fragment。
#define META_INSTALL_TOKEN_BYTES    16
#define META_INSTALL_TOKEN_HEX_LEN  (META_INSTALL_TOKEN_BYTES * 2)

// 配对码:6 位十进制、短 TTL、连续错误上限、一次性(文档 §8 文本兜底)。
#define META_INSTALL_PAIR_DIGITS     6
#define META_INSTALL_PAIR_TTL_MS     300000u  // 5 分钟内未用即作废
#define META_INSTALL_PAIR_MAX_TRIES  5        // 连续错误达到即作废(重新进店才再生成)

// 上传停滞阈值(审计 M6):uploading 态下超过此时长无 chunk 活动,store 死线
// 不再续期 —— 手机消失后由正常超时浮层询问用户,而不是无限吊住 WiFi/OTA。
#define META_INSTALL_UPLOAD_STALL_MS 30000u

// 会话状态快照(UI 轮询与 /api/install/status 同源)。
typedef struct {
    bool     active;         // 店内 token 会话存活(进店到离店)
    bool     offer_ready;    // offer 已 prepare,等待设备物理确认
    bool     confirmed;      // 已物理确认槽位(手机方可开 session)
    bool     session_opened; // 手机已开上传 session
    int8_t   slot;           // 已确认槽位(-1 = 无)
    uint32_t offset;         // 设备已写偏移(事实,断点续传基准)
    uint32_t expected;       // 已确认 image_len(0 = 无)
    char     name[META_NAME_LEN + 1];  // offer/完成展示名(done 态保留供 P5)
    const char *state;       // pairing/offer/confirmed/uploading/done/failed/cancelled
    const char *message;     // 最近一次提示/失败短句(空串 = 无)
    int64_t  upload_idle_ms; // state=="uploading" 时起算的无活动毫秒(停滞判定,审计 M6;
                             // 其他状态为 0;负值 = 时钟异常按 0 处理)
} meta_install_session_status_t;

// ---- 服务生命周期 ----
// init:登记槽位注册表(成功安装回写它),幂等;生命周期覆盖整个启动器。
// start:起本地 httpd(boot 页 + install API,STA 模式端口 80);幂等。
// stop:httpd 停、token/会话全部作废;可重复调用(离店时由 UI teardown 调)。
esp_err_t meta_install_net_init(meta_slot_info_t slots[META_SLOT_COUNT]);
esp_err_t meta_install_net_start(void);
void      meta_install_net_stop(void);

// ---- LAN 地址 / QR 信息 ----
// 服务已起且 STA 拿到 IP 才为 true;IP 以 esp_netif 实时查询为准。
bool     meta_install_lan_ready(void);
uint32_t meta_install_lan_ip(void);   // 网络字节序 IPv4(0 = 未知)
// token hex / 配对码在各自有效时非 NULL;url_buf 总是被写入(IP 未知时为 0.0.0.0 占位)。
void meta_install_qr_info(const char **token_hex_out, const char **pair_code_out,
                          char url_buf[256]);

// ---- token 门控 ----
// token_start:生成(或从 NVS 复用上次未离店的,重启续连)本店 token,配对码
// 一律重发;无 offer 在途时把状态归位 pairing。stop:全部作废并清 NVS。
esp_err_t meta_install_token_start(void);
void      meta_install_token_stop(void);
bool      meta_install_token_from_hex(const char *hex, size_t hex_len);
// 中断续连(用户决策①):上次 install 会话未正常离店(token_start 后未
// token_stop,如上传中途断电)→ true。复位后 app_main 据此自动恢复 STA +
// install 服务,手机用持久化 token 重发 prepare 免重扫 QR。NVS 标志,
// 读取须在 meta_store_net_init()(内部 nvs_flash_init)之后。
bool      meta_install_resume_pending(void);

// ---- offer 确认状态机(UI 入口,文档 §6.4) ----
// 拷贝当前待确认 offer(无则 false)。快照复制:UI 渲染期间允许手机重新
// prepare;最终一致性由 confirm/session 的实时校验兜底。
bool meta_install_offer_copy(meta_install_manifest_t *out);
// 物理确认:slot 必须本地图形可装;成功后 confirmed=1,手机方可开 session(§8)。
esp_err_t meta_install_confirm_slot(int8_t slot);
// 设备侧拒绝 offer(用户 BACK):清 offer、回 pairing,手机可重新 prepare。
esp_err_t meta_install_offer_reject(void);

// ---- 快照与手机侧动作(本地 HTTP handler 调用;UI 不直接调用) ----
void meta_install_session_poll(meta_install_session_status_t *out);
esp_err_t meta_install_session_open(const meta_install_session_req_t *req);
// chunk 判定(顺序/幂等重复/拒绝);duplicate=true 表示整段已写过、应跳过写。
esp_err_t meta_install_chunk_accept(uint32_t offset, uint32_t length, bool *duplicate);
// 顺序写入一块(内部推进 offset;首次写入懒做 OTA begin 与 SHA 起算)。
esp_err_t meta_install_chunk_write(const void *data, uint32_t length);
/* Child DATA initial-image upload. Index/offset are validated against the
 * already-confirmed manifest; existing DATA records are preserved and are
 * reported as already complete by session setup. */
esp_err_t meta_install_data_write(uint8_t data_index, uint32_t offset,
                                   const void *data, uint32_t length,
                                   bool *duplicate);
esp_err_t meta_install_finalize(void);
esp_err_t meta_install_cancel(void);

// 默认选中槽位(转发 model;suggestedSlot 本地 fit 才用)。
int8_t meta_install_default_slot_from_manifest(const meta_install_manifest_t *m);

// 本地上限快照(dynslot:由规范 carve 派生,无提案预填;UI 选槽页用它
// 判 fit —— 表 == carve 不变量,与 prepare/confirm 同一几何事实源)。
void meta_install_local_geom(meta_install_geom_t *out);
