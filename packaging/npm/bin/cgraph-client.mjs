#!/usr/bin/env node
import { main } from "../cgraph-launch.mjs";

await main(["cgraph-client", ...process.argv.slice(2)]);
