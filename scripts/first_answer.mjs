#!/usr/bin/env node
// Times a cold install end to end: with an empty launcher cache, starts
// cgraph-mcp through the plugin launcher (which downloads, verifies and unpacks
// the pinned release), sends `initialize` and one `graph_query`, and prints the
// seconds until the answer as JSON. Exits 1 when the answer is not a graph
// answer or takes longer than --max-seconds.
//
//   node scripts/first_answer.mjs --project <repo> --query <symbol> [--max-seconds 60]

import { spawn } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const args = Object.fromEntries(
  process.argv.slice(2).reduce((pairs, value, index, all) => (index % 2 ? pairs : [...pairs, [value.replace(/^--/, ""), all[index + 1]]]), []),
);
if (!args.project || !args.query) {
  console.error("usage: first_answer.mjs --project <repo> --query <symbol> [--max-seconds 60]");
  process.exit(2);
}
const maxSeconds = Number(args["max-seconds"] ?? 60);
const launcher = path.join(path.dirname(fileURLToPath(import.meta.url)), "..", "integrations", "launcher", "cgraph-launch.mjs");
const cache = fs.mkdtempSync(path.join(os.tmpdir(), "cgraph-first-answer-"));

const started = performance.now();
const child = spawn(process.execPath, [launcher, "cgraph-mcp"], {
  env: { ...process.env, CGRAPH_CACHE_DIR: cache, CLAUDE_PROJECT_DIR: path.resolve(args.project) },
  stdio: ["pipe", "pipe", "inherit"],
});
const send = (message) => child.stdin.write(JSON.stringify({ jsonrpc: "2.0", ...message }) + "\n");
send({ id: 1, method: "initialize", params: {} });
send({ id: 2, method: "tools/call", params: { name: "graph_query", arguments: { query: args.query } } });

let buffer = "";
child.stdout.on("data", (chunk) => {
  buffer += chunk;
  for (let end = buffer.indexOf("\n"); end >= 0; end = buffer.indexOf("\n")) {
    const line = buffer.slice(0, end);
    buffer = buffer.slice(end + 1);
    const message = JSON.parse(line);
    if (message.id !== 2) continue;
    const seconds = (performance.now() - started) / 1000;
    const result = message.result ? JSON.parse(message.result.content[0].text) : null;
    const hits = result?.nodes?.length ?? 0;
    const report = { seconds: Number(seconds.toFixed(2)), max_seconds: maxSeconds, route: result?.route, hits,
                     first: result?.nodes?.[0]?.label, error: message.error?.message, platform: `${process.platform}-${process.arch}` };
    console.log(JSON.stringify(report));
    child.stdin.end();
    child.kill();
    process.exit(hits > 0 && seconds <= maxSeconds ? 0 : 1);
  }
});
child.on("exit", (code) => {
  console.error(`cgraph-mcp exited (${code}) before answering`);
  process.exit(1);
});
