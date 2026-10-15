#!/usr/bin/env node
// End-to-end MCP smoke: starts an MCP stdio server, initializes it, and asks
// graph_query about tests/fixtures/one_shot until the answer names `helper`
// (the server auto-spawns graphd, which builds the graph), then shuts the
// daemon down. Exits 0 on a correct answer, 1 otherwise.
//
//   node scripts/mcp_smoke.mjs <project root> <command> [args...]
//
// CI runs it on windows-latest through the plugin launcher against the packaged
// cgraph-windows-x64.tar.gz: launcher -> cgraph-mcp.exe -> graphd.exe over AF_UNIX.

import { spawn } from "node:child_process";
import readline from "node:readline";

const [root, command, ...args] = process.argv.slice(2);
if (!root || !command) {
  process.stderr.write("usage: mcp_smoke.mjs <project root> <command> [args...]\n");
  process.exit(2);
}

const server = spawn(command, [...args, "--root", root], { stdio: ["pipe", "pipe", "inherit"] });
const pending = new Map();
let nextId = 1;
readline.createInterface({ input: server.stdout }).on("line", (line) => {
  let message;
  try {
    message = JSON.parse(line);
  } catch {
    process.stderr.write(`mcp_smoke: not JSON on stdout: ${JSON.stringify(line)}\n`);
    return;
  }
  pending.get(message.id)?.(message);
  pending.delete(message.id);
});
server.on("exit", (code) => {
  for (const resolve of pending.values()) resolve({ error: { message: `server exited ${code}` } });
  pending.clear();
});

function call(method, params) {
  const id = nextId++;
  return new Promise((resolve) => {
    pending.set(id, resolve);
    server.stdin.write(`${JSON.stringify({ jsonrpc: "2.0", id, method, params })}\n`);
  });
}

const text = (reply) => reply.result?.content?.map((part) => part.text ?? "").join("\n") ?? JSON.stringify(reply);

async function main() {
  const init = await call("initialize", {
    protocolVersion: "2025-06-18",
    capabilities: {},
    clientInfo: { name: "mcp_smoke", version: "1" },
  });
  if (!init.result?.serverInfo) throw new Error(`initialize failed: ${JSON.stringify(init)}`);
  process.stderr.write(`mcp_smoke: initialized ${JSON.stringify(init.result.serverInfo)}\n`);

  const deadline = Date.now() + 120_000;
  let answer = "";
  while (Date.now() < deadline) {
    answer = text(await call("tools/call", { name: "graph_query", arguments: { query: "helper" } }));
    if (answer.includes("helper") && answer.includes("main.py")) break;
    await new Promise((resolve) => setTimeout(resolve, 1000));
  }
  process.stderr.write(`mcp_smoke: graph_query answered:\n${answer.slice(0, 2000)}\n`);
  if (!(answer.includes("helper") && answer.includes("main.py"))) throw new Error("graph_query never named helper in main.py");

  const shutdown = await call("tools/call", { name: "graph_shutdown", arguments: {} });
  process.stderr.write(`mcp_smoke: graph_shutdown answered: ${text(shutdown).slice(0, 200)}\n`);
}

main()
  .then(() => {
    server.stdin.end();
    process.exitCode = 0;
  })
  .catch((error) => {
    process.stderr.write(`mcp_smoke: FAIL: ${error.message}\n`);
    server.kill();
    process.exitCode = 1;
  });
