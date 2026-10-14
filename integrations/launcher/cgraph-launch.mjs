#!/usr/bin/env node
// Runs a CGraph executable from the release named in release.json, downloading
// and verifying it on first use. Usage: cgraph-launch.mjs <executable> [args...]
// where <executable> is one of cgraph, graphd, cgraph-client, cgraph-mcp.
//
// The archive for this platform is fetched from the GitHub release, its SHA-256
// is checked against release.json, and it is unpacked into
// <cache>/<tag>/<platform>/. The cache is $CGRAPH_CACHE_DIR when set (the Claude
// Code plugin points it at ${CLAUDE_PLUGIN_DATA}), otherwise the user cache
// directory: $XDG_CACHE_HOME/cgraph or ~/.cache/cgraph on Linux,
// ~/Library/Caches/cgraph on macOS, %LOCALAPPDATA%\cgraph on Windows.
// The unpacked directory appears by one atomic rename, so a launch that races
// another one (the MCP server and the pre-edit hook) never sees half a release.
// Nothing is written to stdout: it belongs to the executable (MCP speaks on it).

import { createHash, randomBytes } from "node:crypto";
import { spawn, spawnSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const EXECUTABLES = new Set(["cgraph", "graphd", "cgraph-client", "cgraph-mcp"]);

const here = path.dirname(fileURLToPath(import.meta.url));
const release = JSON.parse(fs.readFileSync(path.join(here, "release.json"), "utf8"));

function fail(message) {
  process.stderr.write(`cgraph-launch: ${message}\n`);
  process.exit(1);
}

export function platformKey(platform = process.platform, arch = process.arch) {
  const osName = { linux: "linux", darwin: "macos", win32: "windows" }[platform];
  const cpu = { x64: "x64", arm64: "arm64" }[arch];
  if (osName === undefined || cpu === undefined) {
    return undefined;
  }
  return `${osName}-${cpu}`;
}

export function cacheRoot(env = process.env, platform = process.platform, home = os.homedir()) {
  if (env.CGRAPH_CACHE_DIR) {
    return env.CGRAPH_CACHE_DIR;
  }
  if (platform === "win32") {
    return path.join(env.LOCALAPPDATA, "cgraph");
  }
  if (platform === "darwin") {
    return path.join(home, "Library", "Caches", "cgraph");
  }
  return path.join(env.XDG_CACHE_HOME || path.join(home, ".cache"), "cgraph");
}

async function download(url) {
  const response = await fetch(url, { redirect: "follow" });
  if (!response.ok) {
    throw new Error(`GET ${url} answered ${response.status} ${response.statusText}`);
  }
  return Buffer.from(await response.arrayBuffer());
}

// Returns the directory holding the four executables, downloading it if absent.
export async function ensureRelease(root = cacheRoot(), key = platformKey()) {
  if (key === undefined) {
    throw new Error(`no CGraph build for ${process.platform}-${process.arch}`);
  }
  const asset = release.assets[key];
  if (asset === undefined) {
    const have = Object.keys(release.assets).join(", ");
    throw new Error(`release ${release.tag} has no ${key} build (it has ${have})`);
  }
  const dir = path.join(root, release.tag, key);
  if (fs.existsSync(dir)) {
    return dir;
  }

  const url = `https://github.com/${release.repository}/releases/download/${release.tag}/${asset.name}`;
  process.stderr.write(`cgraph-launch: downloading ${release.tag} ${key} (first run only)\n`);
  const bytes = await download(url);
  const digest = createHash("sha256").update(bytes).digest("hex");
  if (digest !== asset.sha256) {
    throw new Error(`${asset.name} checksum mismatch: expected ${asset.sha256}, got ${digest}`);
  }

  fs.mkdirSync(path.dirname(dir), { recursive: true });
  const nonce = `${process.pid}-${randomBytes(4).toString("hex")}`;
  const staging = `${dir}.partial-${nonce}`;
  const archive = `${staging}.tar.gz`;
  fs.mkdirSync(staging);
  try {
    fs.writeFileSync(archive, bytes);
    const tar = spawnSync("tar", ["-xzf", archive, "-C", staging], { stdio: ["ignore", "ignore", "pipe"] });
    if (tar.error) {
      throw new Error(`cannot run tar: ${tar.error.message}`);
    }
    if (tar.status !== 0) {
      throw new Error(`tar failed (${tar.status}): ${tar.stderr.toString().trim()}`);
    }
    try {
      fs.renameSync(staging, dir);
    } catch (error) {
      // Another launch finished first; its copy is the same verified release.
      if (!fs.existsSync(dir)) {
        throw error;
      }
    }
  } finally {
    fs.rmSync(archive, { force: true });
    fs.rmSync(staging, { recursive: true, force: true });
  }
  return dir;
}

export function executablePath(dir, name, platform = process.platform) {
  return path.join(dir, platform === "win32" ? `${name}.exe` : name);
}

export async function main(argv) {
  const [name, ...args] = argv;
  if (!EXECUTABLES.has(name)) {
    fail(`usage: cgraph-launch.mjs <${[...EXECUTABLES].join("|")}> [args...]`);
  }
  let dir;
  try {
    dir = await ensureRelease();
  } catch (error) {
    fail(error.message);
  }
  const child = spawn(executablePath(dir, name), args, { stdio: "inherit" });
  const forward = (signal) => child.kill(signal);
  const forwarded = ["SIGINT", "SIGTERM", "SIGHUP"];
  for (const signal of forwarded) {
    process.on(signal, forward);
  }
  child.on("error", (error) => fail(`cannot start ${name}: ${error.message}`));
  child.on("exit", (code, signal) => {
    if (signal) {
      // Die of the same signal so the caller sees what the executable saw; with
      // the forwarder still installed, Node would catch it and exit 0.
      for (const s of forwarded) {
        process.removeListener(s, forward);
      }
      process.kill(process.pid, signal);
      return;
    }
    process.exit(code);
  });
}

if (process.argv[1] && fs.realpathSync(process.argv[1]) === fileURLToPath(import.meta.url)) {
  await main(process.argv.slice(2));
}
