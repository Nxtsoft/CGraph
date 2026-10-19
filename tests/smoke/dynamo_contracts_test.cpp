// DynamoDB table facts (dynamo_contracts.cpp), read through the TypeScript and
// JavaScript extractors: a call that writes a table records
// `provides_contract dynamo:<name>`, one that only reads it records
// `uses_contract dynamo:<name>`, with the env variables the name defaults from
// as target_label. Driven through the extractors' public entry points, plus
// the dynamo_lookup_counts test hook.
#include "cgraph/dynamo_contracts.hpp"
#include "cgraph/javascript_extractor.hpp"
#include "cgraph/normalize.hpp"

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <set>
#include <string>
#include <string_view>

namespace {

// "<provides|uses>|<source id>|<context>|<env>" for every dynamo fact.
std::set<std::string> dynamo_facts(const cgraph::ExtractionResult& result) {
  std::set<std::string> facts;
  for (const auto& relation : result.raw_relations) {
    if ((relation.relation == "provides_contract" || relation.relation == "uses_contract") &&
        relation.context.starts_with("dynamo:")) {
      facts.insert(std::string(relation.relation == "provides_contract" ? "provides" : "uses") + "|" +
                   relation.source_id + "|" + relation.context + "|" + relation.target_label);
    }
  }
  return facts;
}

bool expect(std::string_view what, const std::set<std::string>& actual, const std::set<std::string>& expected) {
  if (actual == expected) {
    return true;
  }
  std::cerr << what << ": unexpected dynamo facts\n";
  for (const auto& fact : actual) std::cerr << "  actual:   " << fact << '\n';
  for (const auto& fact : expected) std::cerr << "  expected: " << fact << '\n';
  return false;
}

std::string id(std::string_view file, std::string_view symbol) {
  return cgraph::make_id(std::string(file) + ":" + std::string(symbol));
}

cgraph::ExtractionResult typescript(const std::string& file, std::string_view source) {
  return cgraph::extract_typescript({.source_file = file, .relative_path = file, .source = source});
}

}  // namespace

int main() {
  bool ok = true;

  // The shared-store shape: a module const defaulting an env variable, v3
  // commands. Put and Delete write (provide), Get and Query read (use). A
  // literal TableName and a template with no substitution read as written.
  {
    const std::string file = "lib/connectors/dynamo.ts";
    const auto result = typescript(file, R"ts(
import { DynamoDBClient, GetItemCommand, PutItemCommand, DeleteItemCommand, QueryCommand } from '@aws-sdk/client-dynamodb';
const TABLE_NAME = process.env.DYNAMODB_TABLE_NAME || 'turing-agents-dev';
export async function getUserConnection(userId: string) {
  return getClient().send(new GetItemCommand({ TableName: TABLE_NAME, Key: k(userId) }));
}
export async function putUserConnection(userId: string) {
  await getClient().send(new PutItemCommand({ TableName: TABLE_NAME, Item: i(userId) }));
}
export async function deleteUserConnection(userId: string) {
  await getClient().send(new DeleteItemCommand({ TableName: TABLE_NAME, Key: k(userId) }));
}
export async function listUserConnections(userId: string) {
  return getClient().send(new QueryCommand({ TableName: TABLE_NAME, KeyConditionExpression: 'PK = :pk' }));
}
export async function audit() {
  await getClient().send(new PutItemCommand({ TableName: 'audit-log', Item: {} }));
  return getClient().send(new GetItemCommand({ TableName: `audit-log`, Key: {} }));
}
)ts");
    ok &= expect("v3 commands through a module const", dynamo_facts(result),
                 {
                     "uses|" + id(file, "getUserConnection") + "|dynamo:turing-agents-dev|DYNAMODB_TABLE_NAME",
                     "provides|" + id(file, "putUserConnection") + "|dynamo:turing-agents-dev|DYNAMODB_TABLE_NAME",
                     "provides|" + id(file, "deleteUserConnection") + "|dynamo:turing-agents-dev|DYNAMODB_TABLE_NAME",
                     "uses|" + id(file, "listUserConnections") + "|dynamo:turing-agents-dev|DYNAMODB_TABLE_NAME",
                     "provides|" + id(file, "audit") + "|dynamo:audit-log|",
                     "uses|" + id(file, "audit") + "|dynamo:audit-log|",
                 });
  }

  // A command imported dynamically (`await import(...)`) inside the handler,
  // `??` and a chain of env reads, `process.env['X']`, and the same env
  // variable with a different default naming a different table.
  {
    const std::string file = "src/modules/connectors/index.ts";
    const auto result = typescript(file, R"ts(
const DYNAMODB_TABLE = process.env.DYNAMODB_TABLE_NAME ?? 'turing-agents-dev';
const MEMORY = process.env['DYNAMODB_TABLE_NAME'] || process.env.DYNAMODB_TABLE || 'wiki-agent-memory';
export async function storeTokens(client: any) {
  const { PutItemCommand } = await import('@aws-sdk/client-dynamodb');
  await client.send(new PutItemCommand({ TableName: DYNAMODB_TABLE, Item: {} }));
}
export async function remember(client: any) {
  const { UpdateItemCommand } = await import('@aws-sdk/client-dynamodb');
  await client.send(new UpdateItemCommand({ TableName: MEMORY, Key: {} }));
}
)ts");
    ok &= expect("dynamic import, ?? and env chains", dynamo_facts(result),
                 {
                     "provides|" + id(file, "storeTokens") + "|dynamo:turing-agents-dev|DYNAMODB_TABLE_NAME",
                     "provides|" + id(file, "remember") +
                         "|dynamo:wiki-agent-memory|DYNAMODB_TABLE_NAME,DYNAMODB_TABLE",
                 });
  }

  // Paginators read the table named in their second argument; a command
  // named off a namespace import counts like an imported one.
  {
    const std::string file = "src/history.ts";
    const auto result = typescript(file, R"ts(
import * as ddb from '@aws-sdk/client-dynamodb';
import { paginateQuery, paginateScan } from '@aws-sdk/lib-dynamodb';
const HISTORY = process.env.HISTORY_TABLE || 'chat-history';
export async function* pages(client: any) {
  yield* paginateQuery({ client }, { TableName: HISTORY, KeyConditionExpression: 'PK = :pk' });
}
export async function* everything(client: any) {
  yield* paginateScan({ client, pageSize: 25 }, { TableName: 'chat-archive' });
}
export async function record(client: any) {
  await client.send(new ddb.PutItemCommand({ TableName: HISTORY, Item: {} }));
}
export async function* wrongSlot(client: any) {
  yield* paginateQuery({ client, TableName: 'not-the-input' }, {});
}
)ts");
    ok &= expect("paginators and namespaced commands", dynamo_facts(result),
                 {
                     "uses|" + id(file, "pages") + "|dynamo:chat-history|HISTORY_TABLE",
                     "uses|" + id(file, "everything") + "|dynamo:chat-archive|",
                     "provides|" + id(file, "record") + "|dynamo:chat-history|HISTORY_TABLE",
                 });
  }

  // lib-dynamodb commands and DocumentClient methods (v3 `DynamoDBDocument`,
  // v2 `require('aws-sdk')`): put/update/delete provide, get/query/scan use.
  {
    const std::string file = "src/stores/sessions.ts";
    const auto result = typescript(file, R"ts(
import { DynamoDBDocumentClient, PutCommand, GetCommand, ScanCommand } from "@aws-sdk/lib-dynamodb";
const SESSIONS = "Sessions";
export async function save(doc: any) {
  await doc.send(new PutCommand({ TableName: SESSIONS, Item: {} }));
}
export async function load(doc: any) {
  return doc.send(new GetCommand({ TableName: SESSIONS, Key: {} }));
}
export async function sweep(doc: any) {
  await doc.delete({ TableName: SESSIONS, Key: {} });
  return doc.scan({ TableName: SESSIONS });
}
)ts");
    ok &= expect("lib-dynamodb commands and document methods", dynamo_facts(result),
                 {
                     "provides|" + id(file, "save") + "|dynamo:Sessions|",
                     "uses|" + id(file, "load") + "|dynamo:Sessions|",
                     "provides|" + id(file, "sweep") + "|dynamo:Sessions|",
                     "uses|" + id(file, "sweep") + "|dynamo:Sessions|",
                 });
  }
  {
    const std::string file = "src/legacy.js";
    const auto result = cgraph::extract_javascript({.source_file = file, .relative_path = file, .source = R"js(
const AWS = require('aws-sdk');
const docClient = new AWS.DynamoDB.DocumentClient();
async function getOrder(id) {
  return docClient.get({ TableName: 'orders-prod', Key: { id } }).promise();
}
async function putOrder(order) {
  return docClient.put({ TableName: 'orders-prod', Item: order }).promise();
}
)js"});
    ok &= expect("v2 DocumentClient", dynamo_facts(result),
                 {
                     "uses|" + id(file, "getOrder") + "|dynamo:orders-prod|",
                     "provides|" + id(file, "putOrder") + "|dynamo:orders-prod|",
                 });
  }

  // Not facts: a table held on `this`, a parameter, a constant shadowed by a
  // parameter or a local (plain or destructured), a non-env fallback, an env read with no default, an
  // interpolated template, a name DynamoDB refuses, a batch request, and a
  // method with no TableName.
  {
    const std::string file = "src/stores/dynamodb-store.ts";
    const auto result = typescript(file, R"ts(
import { DynamoDBDocumentClient, PutCommand, GetCommand, BatchWriteCommand } from "@aws-sdk/lib-dynamodb";
const TABLE = 'shared-table';
const NO_DEFAULT = process.env.DYNAMODB_TABLE_NAME;
export class Store {
  private tableName: string;
  async put(doc: any) {
    await doc.send(new PutCommand({ TableName: this.tableName, Item: {} }));
  }
}
export async function byParameter(doc: any, tableName: string) {
  await doc.send(new PutCommand({ TableName: tableName, Item: {} }));
}
export async function shadowedByParameter(doc: any, TABLE: string) {
  await doc.send(new PutCommand({ TableName: TABLE, Item: {} }));
}
export async function shadowedByDestructuredParameter(doc: any, { TABLE }: { TABLE: string }) {
  await doc.send(new PutCommand({ TableName: TABLE, Item: {} }));
}
export const shadowedByArrowPattern = async ([TABLE]: string[], doc: any) =>
  doc.send(new GetCommand({ TableName: TABLE, Key: {} }));
export async function shadowedByDestructuredLocal(doc: any, config: any) {
  const { TABLE } = config;
  return doc.send(new GetCommand({ TableName: TABLE, Key: {} }));
}
export async function shadowedByLocal(doc: any) {
  const TABLE = pick();
  return doc.send(new GetCommand({ TableName: TABLE, Key: {} }));
}
export async function notEnvFallback(doc: any, config: any) {
  await doc.send(new PutCommand({ TableName: config.tableName || 'wiki-agent-memory', Item: {} }));
}
export async function noDefault(doc: any, env: string) {
  await doc.send(new PutCommand({ TableName: NO_DEFAULT, Item: {} }));
  await doc.send(new PutCommand({ TableName: `turing-agents-${env}`, Item: {} }));
  await doc.send(new PutCommand({ TableName: 'ab', Item: {} }));
  await doc.send(new PutCommand({ TableName: 'has space', Item: {} }));
  await doc.send(new BatchWriteCommand({ RequestItems: { [TABLE]: [] } }));
  return cache.get('shared-table');
}
)ts");
    ok &= expect("unreadable names", dynamo_facts(result), {});
  }

  // A file that does not import the DynamoDB SDK records nothing, whatever its
  // calls look like; nor does a test source.
  {
    const std::string file = "src/other.ts";
    const auto result = typescript(file, R"ts(
import { PutItemCommand } from './fake-dynamo';
export async function write(c: any) {
  await c.send(new PutItemCommand({ TableName: 'turing-agents-dev', Item: {} }));
  return store.put({ TableName: 'turing-agents-dev' });
}
)ts");
    ok &= expect("no SDK import", dynamo_facts(result), {});
  }
  {
    const std::string file = "src/connectors/dynamo.test.ts";
    const auto result = typescript(file, R"ts(
import { PutItemCommand } from '@aws-sdk/client-dynamodb';
export async function seed(c: any) {
  await c.send(new PutItemCommand({ TableName: 'turing-agents-dev', Item: {} }));
}
)ts");
    ok &= expect("test source", dynamo_facts(result), {});
  }

  // One fact per reading symbol and table is checked against a set the file
  // scope keeps, caught up with the relations appended since: each relation is
  // read once per file, not once per call (a scan per call was quadratic).
  {
    constexpr std::size_t kFunctions = 200;
    const std::string file = "src/tables.ts";
    std::string source = "import { PutItemCommand } from '@aws-sdk/client-dynamodb';\n";
    std::set<std::string> expected;
    for (std::size_t index = 0; index < kFunctions; ++index) {
      const auto n = std::to_string(index);
      source += "export async function put" + n + "(c: any) { await c.send(new PutItemCommand({ TableName: 'tbl-" + n +
                "', Item: {} })); await c.send(new PutItemCommand({ TableName: 'tbl-" + n + "', Item: {} })) }\n";
      expected.insert("provides|" + id(file, "put" + n) + "|dynamo:tbl-" + n + "|");
    }
    const auto before = cgraph::dynamo_lookup_counts();
    const auto result = typescript(file, source);
    const auto reads = cgraph::dynamo_lookup_counts().fact_reads - before.fact_reads;
    ok &= expect("many tables", dynamo_facts(result), expected);
    // The set above would hide a duplicate: each table's second Put writes none.
    const auto relations = std::ranges::count_if(result.raw_relations, [](const cgraph::RawRelation& relation) {
      return (relation.relation == "provides_contract" || relation.relation == "uses_contract") &&
             relation.context.starts_with("dynamo:");
    });
    if (static_cast<std::size_t>(relations) != kFunctions) {
      std::cerr << "dynamo fact set: " << relations << " dynamo relations for " << kFunctions << " tables\n";
      ok = false;
    }
    if (reads > result.raw_relations.size()) {
      std::cerr << "dynamo fact set: " << reads << " relation reads for " << result.raw_relations.size()
                << " relations\n";
      ok = false;
    }
  }

  return ok ? 0 : 1;
}
