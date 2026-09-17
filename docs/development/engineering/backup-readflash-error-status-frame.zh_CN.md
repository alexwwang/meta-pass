# 备份功能持续失败的根因排查 —— 第六轮:stub error/status 帧未被消费

<p align="right">
  <strong>简体中文</strong> · <a href="backup-readflash-error-status-frame.md">English</a>
</p>

> **⚠️ 后验修正(2026-09-17 傍晚)**:本文的"主根因"结论**不成立**。重新核对
> stub_flasher.c 源码:`SLIP_send_frame_delimiter()` 只有一对 —— 8B 响应头与 2B
> error/status 字节在**同一个 SLIP 帧内**,`checkCommand` 一次读完,不存在残帧。
> 真机证据同样否证:探针 4KB 读取 OK(若存在 +2 残帧,严格校验必报
> `Read more than expected: 4098 > 4096`,实际从未出现),且当日多次完整备份成功。
> 本文价值仅在于 §4 chunkT0 作用域问题(真)与 §3.4 "mock 与真机协议不一致导致
> 单测假绿"(方法论教训,真)。保留存档,结论以正文顶部修正为准。
> 真正的最终根因链见 `debugging-workflow.md`(digest 帧消费 + 波特率限速 + 停等窗口)。
>
> 以下为原文(未修订)。

> 状态:~~根因已定位并经 mock 复现/修复验证~~。时间:2026-09-17。涉及 commit:`01f4faa`
> (64KB→32KB 分块)、`e7a78b3`(transport 修复)、`a8afbdd`(digest 帧消费 + 尺寸校验)、
> 工作树 probe4(chunkT0 作用域修复,未提交)。
> 排查方法遵循 `debugging-workflow.md` 的既有约定:先读**服务端(stub)源码**与
> **参考实现(esptool.py)** 对协议,再改失败处理;证据优先,猜测靠边。

---

## 1. 现象

```
[10:27:59] 开始备份槽位…
[10:27:59] 读取槽位 0(1.84 MB @ 0x180000)… 每个槽位可能需要一分钟。
[10:28:02] 槽位 0:重试 5 次后仍失败 —— 已跳过,其余槽位继续。
[10:28:02] 读取槽位 1(2.00 MB @ 0x360000)… 每个槽位可能需要一分钟。
[10:28:05] 槽位 1:重试 5 次后仍失败 —— 已跳过,其余槽位继续。
[10:28:05] 备份失败:所选槽位全部读取失败,未产出备份。
```

两个特征:

- 每个槽位恰好 ~3 秒失败,且报 "重试 5 次后仍失败" —— 但日志里没有一条
  "读取 0x… 出错(…)—— 重试(n/5)…" 重试明细行。
- 勾选 Debug 模式后没有任何 `[debug]` 协议级日志输出。

## 2. 结论(先行)

| # | 问题 | 性质 | 状态 |
|---|---|---|---|
| 1 | vendored `readFlash` 没有消费 stub 在响应头之后**无条件发送的 2 字节 error/status 帧**,该帧被当成第一个"数据帧"拼入结果 → 每块多出 2 字节 → 尺寸校验 `Read more than expected: 32770 > 32768` 必炸 | **主根因(每个分块必败)** | 未修(本报告给出补丁) |
| 2 | 已提交版 `readFlashChunked` 的 `const chunkT0` 声明在 `for` 循环内、却在循环外被 Debug 成功日志引用 → **开 Debug 必抛 ReferenceError**,整槽读取被中断并误报为"重试 5 次后仍失败" | **次根因(解释 Debug 无输出)** | 工作树 probe4 已修,未提交 |
| 3 | `01f4faa` 的 "32KB 分块消灭 0xC0 ACK" 理论**机制错误**:ACK 经 `transport.write()` 的 `slipWriter` 封帧,payload 里的 0xC0/0xDB 已被转义,线上永远不会出现裸 0xC0。分块缩小是徒劳的 | **理论修正** | 无需回退,但非修复 |

三个根因叠加,解释了全部观察:块读取本身必败(根因 1);即使读成功,开 Debug 也会崩
(根因 2);此前 ~130KB 周期失败与 3 秒/槽位失败都不是 0xC0 ACK 造成的(根因 3)。

## 3. 主根因:error/status 帧未消费(证据链)

### 3.1 stub 侧的真实协议(服务端源码,esptool v4.6.1 flasher_stub)

`flasher_stub/stub_flasher.c` 的命令循环对**每个命令**回复两帧:

```c
    /* Send command response header */
    esp_command_response_t resp = { .resp = 1, .op_ret = command->op, .len_ret = 2, .value = 0 };
    SLIP_send_frame_delimiter();
    SLIP_send_frame_data_buf(&resp, sizeof(esp_command_response_t));   // 帧1:8 字节响应头
    ...
    SLIP_send_frame_data(error);      // 帧2:error/status(2 字节,ESP_OK=0x00)
    SLIP_send_frame_data(status);
    SLIP_send_frame_delimiter();

    if (error == ESP_OK) {
      switch (command->op) {
      case ESP_READ_FLASH:
        handle_flash_read(data_words[0], data_words[1], data_words[2], data_words[3]);
        break;
```

`handle_flash_read`(stub_commands.c)之后才流式发送数据块(每块 4KB,逐块等 ACK,
数据发完且全部确认后追加 16 字节 MD5 digest 帧)。

**即 READ_FLASH 的线上帧序是:响应头(8B)→ error/status(2B)→ 数据块 ×N → digest(16B)。**

### 3.2 浏览器侧的消费(vendored esptool-js.js readFlash)

```js
const r = await this.checkCommand("read flash", this.ESP_READ_FLASH, i);
if (r != 0) throw new _("Failed to read memory: " + r);
let a = new Uint8Array(0);
for (; a.length < e;) {
  const { value: n } = await this.transport.read(this.FLASH_READ_TIMEOUT).next();
  if (n instanceof Uint8Array) n.length > 0 && (a = appendArray(a, n), ...);   // ← error/status 帧(2B)被拼进 a
  else throw ...
}
if (a.length !== e) throw new _("Read more than expected: " + a.length + " > " + e);   // ← a = 2 + e,必炸
```

`checkCommand → command → readPacket` 只读响应头(8B)就返回,2 字节 error/status 帧
留在流里;数据循环把它当成第一个数据帧拼进 `a`。于是 32KB 分块的 `a.length = 2 + 32768
= 32770 ≠ 32768` → **`Read more than expected: 32770 > 32768` 每个分块必抛**。

上游 esptool-js@0.5.6 的 `readFlash` 没有这个尺寸校验,所以 +2 偏移只是悄悄带上
(数据仍可用,调用方多收到 2 字节);`a8afbdd` 为"宁可失败也不带病运行"加的唯一严格
尺寸校验,恰好把协议里本来就存在的 error/status 帧变成了致命错误。esptool.py 的
`command()` 会在命令阶段就消费掉 error/status,数据流是干净的 —— 浏览器侧漏了这一步。

### 3.3 复现与修复验证(mock,非真机)

用**真实 vendored readFlash + 忠实还原 classic stub 行为**的内存 mock 复现
(含 8B 响应头、2B error/status 帧、逐块 ACK 的 4KB 数据、16B digest 帧):

```
未修复:readFlash FAILED after 25ms: Read more than expected: 32770 > 32768   ← 复现
已修复:readFlash(0x180000, 0x8000) -> 32768 bytes, content OK                 ← 修复
        readFlash(0x188000, 0x8000) -> 32768 bytes, content OK                 ← 连续 3 块
        readFlash(0x190000, 0x8000) -> 32768 bytes, content OK
        stub 收到的 ACK:0x1000,0x2000,…,0x8000(每块 8 次,干净累计)             ← 协议对齐
```

### 3.4 为什么单测没抓住

`tools/install-slot/test-readflash-protocol.mjs` 的 mock stub **没有发 2 字节
error/status 帧** —— 它的 `checkCommand` 直接返回 0,数据循环从"第一个数据帧"开始。
而真实 stub 会在响应头之后无条件插这 2 字节。mock 与真机协议不一致,单测自然全绿。
(测试注释写 "stub 侧 SLIP_recv 裸读 4 字节",对 ACK 的假设是对的,但漏了 error/status
帧这一环节。)

## 4. 次根因:chunkT0 作用域(解释 Debug 无输出)

已提交的 `01f4faa` 中:

```js
for (let attempt = 1; attempt <= BACKUP_RETRIES && !chunk; attempt++) {
  const chunkT0 = Date.now();        // ← const 声明在 for 块内
  ...
}
...
if (debugMode()) log(`[debug] … ${Date.now() - chunkT0}ms`, "warn");   // ← 循环外引用 → ReferenceError
```

`const` 是块级作用域,循环外引用即抛 `ReferenceError: chunkT0 is not defined`。
触发条件恰好是 Debug 模式 + 某块读取成功 —— 与用户"勾选 debug 模式后无 debug 输出"
完全吻合:成功日志在打印前就抛异常,整槽读取中断,落到外层 catch 变成
"槽位 X:重试 5 次后仍失败"(该文案对所有 readFlashChunked 错误一视同仁,不含真实
错误信息)。工作树 probe4 已把 `chunkT0` 提升到 `while` 级作用域修复(未提交)。

## 5. 时序吻合:为什么每槽 ~3 秒

- C3 原生 USB-CDC 的吞吐不受 115200 波特率约束,32KB 分块的数据传输在毫秒级完成
  (不是注释里按 115200 推算的 2.9 秒)。
- 每槽 5 次尝试:每次 readFlash 秒败(尺寸校验),加上 4 次 resync(ACK 补发 + 排空 +
  sync,约 0.3–0.6s/次)≈ 2.5–4s —— 与日志的 3 秒间隔吻合。
- 重试明细行("读取 0x… 出错…")按代码路径**确实会输出**(attempt 1–4 各一条 warn),
  用户粘贴的日志应是筛选过的摘要。

## 6. 修复建议

### 6.1 主修复:消费 error/status 帧(vendored esptool-js.js readFlash)

数据循环跳过第一个 2 字节帧(仅当是 error/status;数据块恒为 4096 字节,不会误伤):

```js
let a = new Uint8Array(0);
let first = true;
for (; a.length < e;) {
  const { value: n } = await this.transport.read(this.FLASH_READ_TIMEOUT).next();
  if (n instanceof Uint8Array) {
    if (n.length > 0) {
      if (first && n.length === 2) { first = false; continue; }   // 跳过 stub 的 error/status 帧
      first = false;
      a = this._appendArray(a, n);
      await this.transport.write(this._intToByteArray(a.length));
      // onPacket...
    }
  } else {
    throw new _("Failed to read memory: " + n);
  }
}
```

(已在验证脚本中实测通过:连续 3 × 32KB 分块、ACK 累计 0x1000→0x8000、digest 校验全过。
补丁已具备,可随确认一起落 vendor。)

### 6.2 同步修复:单测 mock 补上 error/status 帧

`test-readflash-protocol.mjs` 的 stub mock 在响应头之后追加 `send(2 字节 0x00 0x00)`
帧,并断言数据循环跳过它 —— 让单测覆盖真实协议,防回归。

### 6.3 提示信息修正(install-slot.html)

槽级失败文案 `msg_backup_read_failed` 应带上真实错误(`{ err }`),避免把
ReferenceError/尺寸校验这类非重试错误误报为"重试 5 次后仍失败"。

### 6.4 保留 32KB 分块(无害)

虽然 0xC0 理论不成立,32KB 分块本身没有副作用(ACK 0x1000–0x8000 的 payload 确实
不含 0xC0/0xDB,即使将来改回裸 ACK 写也是安全的),无需回退。

## 7. 遗留风险

- 本验证基于 classic stub 的源码级还原 mock(esptool v4.6.1 的 flasher_stub,即
  vendored esptool-js@0.5.6 内嵌 stub 的构建来源)。真机上应在连接后先用页面的
  `btn-probe`(工作树新增)复现:修复前它会打出 `Read more than expected: 4098 > 4096`,
  修复后打出 `[probe] 4KB @0x180000 OK`。
- digest 帧存在性假设(本次 3 秒时序反推 stub 确实发了 digest 帧)需真机最终确认。
- resync 的 ACK 补发在 stub 空闲时会被当成一条垃圾命令吞掉(再经排空+sync 恢复)——
  该路径按设计工作,但未被本次 mock 覆盖。