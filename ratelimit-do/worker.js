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
// asum 摘要缓存(create-only):键 = asum:<id>:<固件sha256>,sha256 即内容指纹,
// 同键值永不改变(仅 analyzedAt 刷新,而 analyzedAt 无任何消费方),故键已存在
// 即跳过 —— 去重不为配额(DO 内存写不计费,落盘按 alarm 批量),为语义卫生。

export class RateLimiter extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);
    this.counters = new Map();  // key -> { n, exp } (exp = 窗口截止 epoch ms)
    this.cache = new Map();     // asum 摘要:create-only,键存在即不覆盖
    this.dirty = false;
    // 恢复上次落盘的未过期计数与摘要缓存(驱逐/重部署后窗口与缓存延续)。
    ctx.blockConcurrencyWhile(async () => {
      const saved = await ctx.storage.get(["counters", "cache"]).catch(() => null);
      const now = Date.now();
      const counters = saved?.counters ?? saved?.get?.("counters");
      if (counters) {
        for (const [k, v] of Object.entries(counters)) {
          if (v && typeof v.n === "number" && v.exp > now) this.counters.set(k, v);
        }
      }
      const cache = saved?.cache ?? saved?.get?.("cache");
      if (cache) {
        for (const [k, v] of Object.entries(cache)) {
          if (typeof v === "string") this.cache.set(k, v);
        }
      }
    });
  }

  async fetch(request) {
    let args;
    try { args = await request.json(); } catch { /* 非 JSON */ }
    const op = String(args?.op ?? "check");

    if (op === "kvmget") {
      // 批量读:一次 subrequest 取全部键。/api/plays 富化必须走这里 —— 逐键读
      // 在 2000+ 款目录下会打爆 Free plan 每请求 50 次 subrequest 上限(2026-11-19
      // 生产定位:富化恒 0、exceededResources 告警)。上限 5000 键,响应 ~1MB 内。
      const keys = Array.isArray(args?.keys) ? args.keys.map(String) : [];
      if (keys.length === 0 || keys.length > 5000) {
        return Response.json({ error: "bad args" }, { status: 400 });
      }
      const entries = {};
      for (const k of keys) {
        if (this.cache.has(k)) entries[k] = this.cache.get(k);
      }
      return Response.json({ entries });
    }

    if (op === "kvget" || op === "kvset") {
      const key = String(args?.key ?? "");
      if (!key) return Response.json({ error: "bad args" }, { status: 400 });
      if (op === "kvget") {
        return Response.json(this.cache.has(key)
          ? { found: true, value: this.cache.get(key) }
          : { found: false });
      }
      // kvset:create-only。键存在即视为成功但不覆盖(值由固件字节决定,不变)。
      if (this.cache.has(key)) return Response.json({ written: false });
      this.cache.set(key, String(args?.value ?? ""));
      this.dirty = true;
      if ((await this.ctx.storage.getAlarm()) == null) {
        this.ctx.storage.setAlarm(Date.now() + FLUSH_INTERVAL_MS);
      }
      return Response.json({ written: true });
    }

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
      const countersOut = {};
      for (const [k, v] of this.counters) {
        if (v.exp > now) countersOut[k] = v;   // 过期键不落盘,加载侧也再校一次
      }
      await this.ctx.storage.put("counters", countersOut);
      // cache 是 create-only,只增不减;全量重写即增量落盘。两个键各一次写,
      // 合计 ≤2880 次/天,DO storage 无 KV 式日限额。
      await this.ctx.storage.put("cache", Object.fromEntries(this.cache));
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
