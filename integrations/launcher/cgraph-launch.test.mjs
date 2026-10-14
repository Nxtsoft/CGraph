// Exercises the launcher against the real GitHub release named in release.json.
// Run: node --test integrations/launcher/
import assert from "node:assert/strict";
import { spawn, spawnSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";

import { platformKey } from "./cgraph-launch.mjs";

const here = path.dirname(fileURLToPath(import.meta.url));
const release = JSON.parse(fs.readFileSync(path.join(here, "release.json"), "utf8"));

function run(launcher, args, env) {
  return new Promise((resolve) => {
    const child = spawn(process.execPath, [launcher, ...args], {
      env: { ...process.env, ...env },
      stdio: ["ignore", "pipe", "pipe"],
    });
    let stdout = "";
    let stderr = "";
    child.stdout.on("data", (d) => (stdout += d));
    child.stderr.on("data", (d) => (stderr += d));
    child.on("exit", (code, signal) => resolve({ code, signal, stdout, stderr }));
  });
}

function tempDir(t) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "cgraph-launch-"));
  t.after(() => fs.rmSync(dir, { recursive: true, force: true }));
  return dir;
}

test("racing first launches share one verified unpack and leave no staging behind", async (t) => {
  const cache = tempDir(t);
  const launcher = path.join(here, "cgraph-launch.mjs");
  const runs = await Promise.all([
    run(launcher, ["cgraph-client", "--help"], { CGRAPH_CACHE_DIR: cache }),
    run(launcher, ["cgraph-mcp", "--help"], { CGRAPH_CACHE_DIR: cache }),
    run(launcher, ["cgraph", "--help"], { CGRAPH_CACHE_DIR: cache }),
  ]);
  for (const r of runs) {
    assert.doesNotMatch(r.stderr, /cgraph-launch:.*(mismatch|failed|cannot)/, r.stderr);
  }
  const tagDir = path.join(cache, release.tag);
  assert.deepEqual(fs.readdirSync(tagDir), [platformKey()]);
  for (const name of ["cgraph", "graphd", "cgraph-client", "cgraph-mcp"]) {
    assert.ok(fs.statSync(path.join(tagDir, platformKey(), name)).mode & 0o111, `${name} is executable`);
  }
});

test("an archive whose checksum does not match is refused and nothing is cached", async (t) => {
  const cache = tempDir(t);
  const copy = tempDir(t);
  fs.copyFileSync(path.join(here, "cgraph-launch.mjs"), path.join(copy, "cgraph-launch.mjs"));
  const tampered = structuredClone(release);
  tampered.assets[platformKey()].sha256 = "0".repeat(64);
  fs.writeFileSync(path.join(copy, "release.json"), JSON.stringify(tampered));

  const r = await run(path.join(copy, "cgraph-launch.mjs"), ["cgraph", "--help"], { CGRAPH_CACHE_DIR: cache });
  assert.equal(r.code, 1);
  assert.match(r.stderr, /checksum mismatch/);
  assert.equal(fs.existsSync(path.join(cache, release.tag)), false);
});

test("an executable outside the release set is refused before any download", async (t) => {
  const cache = tempDir(t);
  const r = await run(path.join(here, "cgraph-launch.mjs"), ["sh", "-c", "true"], { CGRAPH_CACHE_DIR: cache });
  assert.equal(r.code, 1);
  assert.match(r.stderr, /usage: cgraph-launch\.mjs/);
  assert.deepEqual(fs.readdirSync(cache), []);
});

test("an executable killed by SIGTERM makes the launcher die of SIGTERM, not exit 0", async (t) => {
  const cache = tempDir(t);
  const launcher = path.join(here, "cgraph-launch.mjs");
  // Unpack once, then serve: cgraph-mcp waits on stdin, which stays open here.
  await run(launcher, ["cgraph", "--help"], { CGRAPH_CACHE_DIR: cache });
  const child = spawn(process.execPath, [launcher, "cgraph-mcp"], {
    env: { ...process.env, CGRAPH_CACHE_DIR: cache },
    stdio: ["pipe", "ignore", "ignore"],
  });
  t.after(() => child.stdin.end());
  const mcp = path.join(cache, release.tag, platformKey(), "cgraph-mcp");
  let pid;
  for (let i = 0; i < 100 && pid === undefined; i++) {
    await new Promise((r) => setTimeout(r, 50));
    const ps = spawnSync("pgrep", ["-f", "-P", String(child.pid), mcp], { encoding: "utf8" });
    pid = ps.stdout.trim().split("\n").filter(Boolean).map(Number)[0];
  }
  assert.ok(pid, "cgraph-mcp started under the launcher");
  const exited = new Promise((resolve) => child.on("exit", (code, signal) => resolve({ code, signal })));
  process.kill(pid, "SIGTERM");
  const { code, signal } = await exited;
  assert.ok(signal === "SIGTERM" || code === 143, `launcher ended with code ${code}, signal ${signal}`);
});
