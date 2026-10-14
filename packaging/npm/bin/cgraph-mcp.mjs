#!/usr/bin/env node
import { main } from "../cgraph-launch.mjs";

await main(["cgraph-mcp", ...process.argv.slice(2)]);
