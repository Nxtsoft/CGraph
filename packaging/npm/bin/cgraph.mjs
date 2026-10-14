#!/usr/bin/env node
import { main } from "../cgraph-launch.mjs";

await main(["cgraph", ...process.argv.slice(2)]);
