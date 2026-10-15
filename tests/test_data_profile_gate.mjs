#!/usr/bin/env node
import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import net from "node:net";
import { setTimeout as sleep } from "node:timers/promises";
import worker from "../install-slot/_worker.js";

const PROFILE = "play28-recordings-4m";
async function workerRequest(path, env = {}) {
  const response = await worker.fetch(new Request("https://worker.test" + path), env, { waitUntil() {} });
  return { status: response.status, body: await response.json() };
}
let result = await workerRequest("/api/analyze?id=28&dataProfile=" + PROFILE);
assert.equal(result.status, 403, "Worker must keep test profiles disabled by default");
assert.match(result.body.error, /disabled/);
result = await workerRequest("/api/analyze?id=28&dataProfile=arbitrary", { ENABLE_TEST_DATA_PROFILES: "1" });
assert.equal(result.status, 400, "Worker must reject unknown profile names");
result = await workerRequest("/api/analyze?id=563&dataProfile=" + PROFILE, { ENABLE_TEST_DATA_PROFILES: "1" });
assert.equal(result.status, 400, "Worker must reject profile/play mismatch before market lookup");
assert.match(result.body.error, /only valid for Play 28/);

async function unusedPort() {
  const server = net.createServer();
  server.listen(0, "127.0.0.1");
  await new Promise((resolve, reject) => {
    server.once("listening", resolve);
    server.once("error", reject);
  });
  const port = server.address().port;
  await new Promise((resolve, reject) => server.close((error) => error ? reject(error) : resolve()));
  return port;
}
async function startDevServer(enabled) {
  const port = await unusedPort();
  const child = spawn(process.execPath, ["tools/install-slot/server.mjs"], {
    cwd: new URL("..", import.meta.url),
    env: { ...process.env, PORT: String(port), ...(enabled ? { ENABLE_TEST_DATA_PROFILES: "1" } : { ENABLE_TEST_DATA_PROFILES: "" }) },
    stdio: ["ignore", "ignore", "pipe"],
  });
  let stderr = "";
  child.stderr.setEncoding("utf8");
  child.stderr.on("data", (chunk) => { stderr += chunk; });
  const base = "http://127.0.0.1:" + port;
  try {
    for (let attempt = 0; attempt < 80; attempt++) {
      if (child.exitCode !== null) throw new Error("dev server exited early: " + stderr);
      try {
        const response = await fetch(base + "/", { signal: AbortSignal.timeout(500) });
        if (response.status === 200) return { child, base, stderr: () => stderr };
      } catch {}
      await sleep(50);
    }
    throw new Error("dev server startup timeout: " + stderr);
  } catch (error) {
    child.kill("SIGTERM");
    throw error;
  }
}
async function stop(child) {
  if (child.exitCode !== null) return;
  child.kill("SIGTERM");
  await Promise.race([
    new Promise((resolve) => child.once("exit", resolve)),
    sleep(1500),
  ]);
}
let server = await startDevServer(false);
try {
  const response = await fetch(server.base + "/api/analyze?id=28&dataProfile=" + PROFILE);
  assert.equal(response.status, 403, "local dev server must keep test profiles disabled by default");
  assert.match((await response.json()).error, /disabled/);
} finally { await stop(server.child); }

server = await startDevServer(true);
try {
  let response = await fetch(server.base + "/api/analyze?id=563&dataProfile=" + PROFILE);
  assert.equal(response.status, 400, "local dev server must reject profile/play mismatch before upstream access");
  assert.match((await response.json()).error, /only valid for Play 28/);
  response = await fetch(server.base + "/api/analyze?id=28&dataProfile=arbitrary");
  assert.equal(response.status, 400, "local dev server must reject unknown profiles");
} finally { await stop(server.child); }

console.log("DATA profile route gate: PASS (Worker + local server; default-off, allowlist, Play 28-only before upstream)");
