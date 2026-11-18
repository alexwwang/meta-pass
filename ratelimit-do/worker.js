// meta-pass-ratelimit-do —— 速率计数 Durable Object(独立 Worker 上传)。
//
// 为什么单独存在:Cloudflare Pages 不支持 migrations,Pages 项目的
// Durable Object class 必须由另一个 Worker 上传后,经 Pages 项目设置
// 绑定(durable_object_bindings.script_name 指向本 Worker)。本 Worker
// 不直接对外服务(default fetch 恒 404),唯一职责是承载 RateLimiter class。
//
// 部署:bunx wrangler deploy(在 ratelimit-do/ 目录)。
// 绑定:Pages 项目设置 production durable_object_bindings 增
//       { name = "RATE_LIMITER", class_name = "RateLimiter",
//         script_name = "meta-pass-ratelimit-do" }(REST PATCH,同
//       RATE_KV/CAPKEY_KV 的挂载方式,见 meta-pass/wrangler.toml 注释)。
//
// 计数语义(与迁入前的 KV 实现等价):
//   - 滑动窗口:首次命中起 windowSec 秒,过期即重置(KV 版是固定分钟桶 +
//     TTL 120s/172800s,语义差异对限速目的无影响);
//   - 计数只存内存,alarm 每 60s 把未过期计数落 storage(DO 被驱逐/
//     本 Worker 重部署后窗口仍延续,最多丢未落盘的最后一分钟);
//   - alarm 顺带清理过期键。写入量 ≤1440 次/天,与请求量解耦 ——
//     这正是从 KV 迁出的原因(Free plan KV write 限额 1000/天,
//     生产基线 ~530/天已达 53%,见 docs/CHANGELOG 2026-11-18)。

import { DurableObject } from "cloudflare:workers";

const FLUSH_INTERVAL_MS = 60_000;
// 内存键数硬上限:异常流量下防止 Map 无界增长(正常量级 ~数百键)。
const MAX_KEYS = 10_000;

export class RateLimiter extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);
    this.counters = new Map();  // key -> { n, exp } (exp = 窗口截止 epoch ms)
    this.dirty = false;
    // 恢复上次落盘的未过期计数(驱逐/重部署后限速窗口延续)。
    ctx.blockConcurrencyWhile(async () => {
      const saved = await ctx.storage.get("counters").catch(() => null);
      if (!saved) return;
      const now = Date.now();
      for (const [k, v] of Object.entries(saved)) {
        if (v && typeof v.n === "number" && v.exp > now) this.counters.set(k, v);
      }
    });
  }

  async fetch(request) {
    let args;
    try { args = await request.json(); } catch { /* 非 JSON */ }
    const key = String(args?.key ?? "");
    const limit = Number(args?.limit);
    const windowSec = Number(args?.windowSec);
    if (!key || !Number.isFinite(limit) || limit <= 0 ||
        !Number.isFinite(windowSec) || windowSec <= 0) {
      return Response.json({ error: "bad args" }, { status: 400 });
    }

    const now = Date.now();
    let e = this.counters.get(key);
    if (!e || e.exp <= now) {
      e = { n: 0, exp: now + windowSec * 1000 };
      this.counters.set(key, e);
      if (this.counters.size > MAX_KEYS) {
        // 逐出最先过期的键;无过期键时逐出最早插入的(防御性,正常到不了)。
        let oldestK = null, oldestExp = Infinity;
        for (const [k, v] of this.counters) {
          if (v.exp < oldestExp) { oldestExp = v.exp; oldestK = k; }
        }
        if (oldestK !== null && (oldestExp <= now || this.counters.size > MAX_KEYS)) {
          this.counters.delete(oldestK);
        }
      }
    }
    e.n += 1;
    this.dirty = true;

    if ((await this.ctx.storage.getAlarm()) == null) {
      this.ctx.storage.setAlarm(now + FLUSH_INTERVAL_MS);
    }
    return Response.json({ allowed: e.n <= limit, count: e.n });
  }

  async alarm() {
    const now = Date.now();
    if (this.dirty) {
      const out = {};
      for (const [k, v] of this.counters) {
        if (v.exp > now) out[k] = v;   // 过期键不落盘,加载侧也再校一次
      }
      await this.ctx.storage.put("counters", out);
      this.dirty = false;
    }
    for (const [k, v] of this.counters) {
      if (v.exp <= now) this.counters.delete(k);
    }
    this.ctx.storage.setAlarm(now + FLUSH_INTERVAL_MS);
  }
}

export default {
  async fetch() {
    return new Response("meta-pass-ratelimit-do: not a public service",
      { status: 404 });
  },
};
