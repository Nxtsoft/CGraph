#include "cgraph/claim_contracts.hpp"

#include "cgraph/configured_extractors.hpp"
#include "cgraph/contracts.hpp"
#include "cgraph/graph_builder.hpp"
#include "cgraph/javascript_extractor.hpp"
#include "cgraph/python_extractor.hpp"

#include <algorithm>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

int fail(std::string_view message) {
  std::cerr << "claim_contracts_test: " << message << '\n';
  return 1;
}

// Extracts every (path, source) with the real extractors, merges, resolves
// imports, then resolves contracts: the order run_one_shot uses.
cgraph::GraphSnapshot build(const std::vector<std::pair<std::string, std::string>>& files) {
  std::vector<cgraph::Fragment> fragments;
  std::vector<cgraph::RawRelation> relations;
  for (const auto& [path, source] : files) {
    const cgraph::ExtractionContext context{.source_file = path, .relative_path = path, .source = source};
    const auto result = path.ends_with(".kt")     ? *cgraph::extract_configured_language(cgraph::DetectedLanguage::Kotlin, context)
                        : path.ends_with(".java") ? *cgraph::extract_configured_language(cgraph::DetectedLanguage::Java, context)
                        : path.ends_with(".go")   ? *cgraph::extract_configured_language(cgraph::DetectedLanguage::Go, context)
                        : path.ends_with(".py")   ? cgraph::extract_python(context)
                        : path.ends_with(".js")   ? cgraph::extract_javascript(context)
                                                  : cgraph::extract_typescript(context);
    fragments.push_back(result.fragment);
    relations.insert(relations.end(), result.raw_relations.begin(), result.raw_relations.end());
  }
  auto graph = cgraph::merge_fragments(fragments);
  cgraph::resolve_imports(graph);
  cgraph::resolve_raw_relations(graph, relations);
  cgraph::resolve_contracts(graph, relations);
  return graph;
}

// Claims a node writes (`provided`: handled_by from the claim to it) or reads
// (CONSUMES from it to the claim), by claim name.
std::set<std::string> claims(const cgraph::GraphSnapshot& graph, std::string_view node_id, bool provided) {
  std::set<std::string> names;
  for (const auto& edge : graph.edges) {
    if (provided && edge.relation == "handled_by" && edge.target == node_id && edge.source.starts_with("claim:")) {
      names.insert(edge.source.substr(6));
    } else if (!provided && edge.relation == "CONSUMES" && edge.source == node_id && edge.target.starts_with("claim:")) {
      names.insert(edge.target.substr(6));
    }
  }
  return names;
}

// Every claim node in the graph.
std::set<std::string> all_claims(const cgraph::GraphSnapshot& graph) {
  std::set<std::string> names;
  for (const auto& node : graph.nodes) {
    if (node.kind == "claim") {
      names.insert(node.label);
    }
  }
  return names;
}

std::string show(const std::set<std::string>& names) {
  std::string out = "{";
  for (const auto& name : names) {
    out += " " + name;
  }
  return out + " }";
}

using Names = std::set<std::string>;

// jjwt's builder: `.claim("x", ..)` on the chain, a bare `claim("x", ..)`
// inside its `apply { }` (through a nested `let`), and the registered setters.
// A `claim(key, value)` with a computed key, `header().add("kid", ..)` and a
// `.claim("x", ..)` on anything but a JWT builder are not claims.
int test_kotlin_jjwt_builder() {
  const auto graph = build({{"idp/JwtTokenService.kt", R"(package idp
class JwtTokenService {
    fun generateAccessToken(user: User, sessionId: String?): String {
        return Jwts.builder()
            .subject(user.username)
            .expiration(expiration)
            .claim("roles", roles)
            .claim("tenant_id", user.tenantId)
            .apply {
                sessionId?.let { claim("session_id", it) }
                bindingClaims.forEach { (key, value) -> claim(key, value) }
                kid?.let { header().add("kid", it) }
            }
            .compact()
    }
    fun audit(builder: AuditBuilder) {
        builder.claim("not_a_jwt_claim", 1)
        claim("bare_outside_a_builder", 2)
    }
}
)"}});
  const auto generated = claims(graph, "idp_jwttokenservice_kt_generateaccesstoken", true);
  if (generated != Names{"exp", "roles", "session_id", "sub", "tenant_id"}) {
    return fail("jjwt builder claims: " + show(generated));
  }
  if (all_claims(graph) != generated) {
    return fail("only builder claims are claims: " + show(all_claims(graph)));
  }
  return 0;
}

// Nimbus in Java: `new JWTClaimsSet.Builder().claim(..).issuer(..)`, and
// jjwt's `Jwts.builder()`; a `.claim` on another builder is nothing.
int test_java_builders() {
  const auto graph = build({{"svc/Tokens.java", R"(package svc;
class Tokens {
  JWTClaimsSet nimbus(String s) { return new JWTClaimsSet.Builder().claim("session_id", s).issuer("idp").build(); }
  String jjwt() { return Jwts.builder().subject("u").claim("roles", r).compact(); }
  Object other() { return new Other.Builder().claim("other_claim", 1).build(); }
}
)"}});
  if (claims(graph, "svc_tokens_java_nimbus", true) != Names{"iss", "session_id"} ||
      claims(graph, "svc_tokens_java_jjwt", true) != Names{"roles", "sub"} ||
      all_claims(graph).contains("other_claim")) {
    return fail("java builders: " + show(all_claims(graph)));
  }
  return 0;
}

// Go: the json tags of a struct decoded from the payload segment
// (`parts[1]`) by hand in a file that base64-decodes; a header struct decoded
// from `parts[0]` and a struct that is merely JSON are not claims.
int test_go_payload_decode() {
  const auto graph = build({{"cli/jwt.go", R"(package jwt

import (
	"encoding/base64"
	"encoding/json"
	"strings"
)

type Claims struct {
	Subject   string `json:"sub"`
	TenantID  string `json:"tenant_id"`
	SessionID string `json:"session_id,omitempty"`
	All map[string]any `json:"-"`
}

type Header struct {
	Algorithm string `json:"alg"`
}

type Token struct {
	Header Header
	Claims Claims
}

type UserInfo struct {
	Roles []string `json:"roles"`
}

func Decode(token string) (*Token, error) {
	parts := strings.Split(token, ".")
	headerBytes, err := decodeSegment(parts[0])
	claimBytes, err := decodeSegment(parts[1])
	var t Token
	if err := json.Unmarshal(headerBytes, &t.Header); err != nil {
		return nil, err
	}
	if err := json.Unmarshal(claimBytes, &t.Claims); err != nil {
		return nil, err
	}
	var info UserInfo
	json.Unmarshal([]byte(token), &info)
	return &t, err
}

func decodeSegment(seg string) ([]byte, error) {
	return base64.RawURLEncoding.DecodeString(seg)
}
)"},
                            {"cli/api.go", R"(package api

type UserInfo struct {
	Roles []string `json:"roles"`
	SessionID string `json:"session_id"`
}
)"}});
  if (claims(graph, "cli_jwt_go_claims_sessionid", false) != Names{"session_id"} ||
      claims(graph, "cli_jwt_go_claims_tenantid", false) != Names{"tenant_id"} ||
      claims(graph, "cli_jwt_go_claims_subject", false) != Names{"sub"}) {
    return fail("go payload struct fields are claims: " + show(all_claims(graph)));
  }
  if (all_claims(graph) != Names{"session_id", "sub", "tenant_id"} ||
      !claims(graph, "cli_api_go_userinfo_sessionid", false).empty()) {
    return fail("header and plain JSON structs are not claims: " + show(all_claims(graph)));
  }
  return 0;
}

// golang-jwt: a struct embedding RegisteredClaims, and one passed to
// ParseWithClaims, under the package's import alias.
int test_go_golang_jwt() {
  const auto graph = build({{"svc/auth.go", R"(package auth

import gjwt "github.com/golang-jwt/jwt/v5"

type AccessClaims struct {
	Roles []string `json:"roles"`
	gjwt.RegisteredClaims
}

type RefreshClaims struct {
	FamilyID string `json:"family_id"`
}

type Plain struct {
	Email string `json:"email"`
}

func parse(token string) {
	gjwt.ParseWithClaims(token, &RefreshClaims{}, nil)
}
)"}});
  if (all_claims(graph) != Names{"family_id", "roles"}) {
    return fail("golang-jwt claims types: " + show(all_claims(graph)));
  }
  return 0;
}

// TypeScript: a same-file decoder (split, payload segment, JSON.parse,
// base64) whose return type is DecodedToken makes its properties claims, and
// another file reading `.roles` off its result (through one alias) consumes
// `roles`. An untyped decoder, a header decoder, a property the type lacks,
// and an interface no decoder returns yield nothing.
int test_typescript_decoder_and_reads() {
  const auto graph = build({{"web/lib/sessionCookies.ts", R"(
export interface DecodedToken {
  sub: string;
  roles: string[];
  'session_id': string;
}
export interface TokenHeader {
  alg: string;
}
export function decodeToken(token: string): DecodedToken | null {
  const parts = token.split('.');
  const payload = parts[1];
  const decoded = JSON.parse(Buffer.from(payload, 'base64').toString('utf-8'));
  return decoded as DecodedToken;
}
export function decodeHeader(token: string): TokenHeader | null {
  const parts = token.split('.');
  return JSON.parse(Buffer.from(parts[0], 'base64').toString('utf-8')) as TokenHeader;
}
)"},
                            {"web/proxy.ts", R"(
import { decodeToken } from './lib/sessionCookies';
export interface UserInfo { roles: string[]; email: string }
function hasRole(roles: string[]) { return roles.length > 0; }
export async function proxy(request: Request, token: string, fresh: string) {
  const decoded = decodeToken(token);
  let effectiveDecoded = decoded;
  const freshDecoded = fresh ? decodeToken(fresh) : null;
  if (freshDecoded) { effectiveDecoded = freshDecoded; }
  if (!effectiveDecoded || !hasRole(effectiveDecoded.roles)) { return null; }
  return effectiveDecoded.notAClaim;
}
)"},
                            {"web/api/route.ts", R"(
function decodeJwt(token: string): any {
  const payload = token.split('.')[1];
  return JSON.parse(atob(payload));
}
export function GET(accessToken: string) {
  const jwtPayload = decodeJwt(accessToken);
  return jwtPayload?.roles;
}
)"}});
  if (claims(graph, "web_proxy_ts_proxy", false) != Names{"roles"}) {
    return fail("a read through an imported decoder: " + show(claims(graph, "web_proxy_ts_proxy", false)));
  }
  if (claims(graph, "web_lib_sessioncookies_ts_decodedtoken_roles", false) != Names{"roles"} ||
      claims(graph, "web_lib_sessioncookies_ts_decodedtoken_session_id", false) != Names{"session_id"}) {
    return fail("the decoded type's properties are claims: " + show(all_claims(graph)));
  }
  if (all_claims(graph) != Names{"roles", "session_id", "sub"} ||
      !claims(graph, "web_api_route_ts_get", false).empty()) {
    return fail("untyped, header and undecoded types are not claims: " + show(all_claims(graph)));
  }
  return 0;
}

// Library decoders and signers: jwtDecode<T> types its payload as T,
// jsonwebtoken's sign and jose's SignJWT write their payload keys, SignJWT's
// setters write registered claims. A same-named `sign` from elsewhere is not.
int test_typescript_libraries() {
  const auto graph = build({{"svc/mfa.ts", R"(
import { jwtDecode } from 'jwt-decode';
interface JwtPayload { user_id: string; tenant_id: string }
export function userId(token: string) {
  return jwtDecode<JwtPayload>(token).user_id;
}
)"},
                            {"svc/issue.ts", R"(
import jwt from 'jsonwebtoken';
import { SignJWT } from 'jose';
import { sign } from './crypto';
export function issue(roles: string[], key: string) {
  const legacy = jwt.sign({ roles, 'org_id': 1 }, key);
  const signed = new SignJWT({ scope: 'read' }).setSubject('u').setIssuedAt().sign(key);
  sign({ not_a_claim: 1 });
  return [legacy, signed];
}
)"}});
  if (claims(graph, "svc_mfa_ts_jwtpayload_user_id", false) != Names{"user_id"} ||
      claims(graph, "svc_issue_ts_issue", true) != Names{"iat", "org_id", "roles", "scope", "sub"} ||
      all_claims(graph).contains("not_a_claim")) {
    return fail("library decoders and signers: " + show(all_claims(graph)));
  }
  return 0;
}

// Kotlin hand decode: `.jsonObject["x"]` on the parsed payload in a function
// that splits on '.', takes segment 1 and base64-decodes; the same index on a
// JSON body that is no token is nothing.
int test_kotlin_payload_reads() {
  const auto graph = build({{"app/AccessTokenClaims.kt", R"(package app
fun sessionIdClaim(accessToken: String): String? {
    val payload = accessToken.split('.').getOrNull(1) ?: return null
    return runCatching {
        val decoded = Base64.UrlSafe.withPadding(Base64.PaddingOption.ABSENT_OPTIONAL).decode(payload)
        Json.parseToJsonElement(decoded.decodeToString())
            .jsonObject["session_id"]
            ?.jsonPrimitive
            ?.content
    }.getOrNull()
}
fun bodyRoles(body: String): String? {
    return Json.parseToJsonElement(body).jsonObject["roles"]?.jsonPrimitive?.content
}
)"}});
  if (claims(graph, "app_accesstokenclaims_kt_sessionidclaim", false) != Names{"session_id"} ||
      all_claims(graph) != Names{"session_id"}) {
    return fail("kotlin payload reads: " + show(all_claims(graph)));
  }
  return 0;
}

// PyJWT: the string keys of the payload dict; an f-string key and a dict
// passed to anything else are not claims.
int test_python_encode() {
  const auto graph = build({{"svc/tokens.py", R"(import jwt
import json

def issue(user, key):
    token = jwt.encode({"sub": user.id, "roles": user.roles, f"x{user.id}": 1}, key)
    json.dumps({"not_a_claim": 1})
    return token
)"}});
  if (claims(graph, "svc_tokens_py_issue", true) != Names{"roles", "sub"} || all_claims(graph) != Names{"roles", "sub"}) {
    return fail("pyjwt payload keys: " + show(all_claims(graph)));
  }
  return 0;
}

// The issuer's and a reader's graphs mint the same id for an application
// claim, which bridges; a registered claim's id does not bridge.
int test_cross_repo_ids() {
  const auto issuer = build({{"idp/T.kt", R"(fun t() = Jwts.builder().subject("u").claim("session_id", s).compact()
)"}});
  const auto reader = build({{"cli/jwt.go", R"(package jwt
import gjwt "github.com/golang-jwt/jwt/v5"
type Claims struct {
	SessionID string `json:"session_id"`
	Subject string `json:"sub"`
	gjwt.RegisteredClaims
}
)"}});
  const auto has = [](const cgraph::GraphSnapshot& graph, std::string_view id) {
    return std::ranges::any_of(graph.nodes, [&](const cgraph::Node& node) { return node.id == id; });
  };
  if (!has(issuer, "claim:session_id") || !has(reader, "claim:session_id") ||
      !cgraph::is_bridged_contract("claim:session_id") || !has(issuer, "claim:sub") || !has(reader, "claim:sub") ||
      cgraph::is_bridged_contract("claim:sub")) {
    return fail("claim ids across repos");
  }
  return 0;
}

// Review round 1, item 1: a function that calls a JWT library somewhere but
// returns something else is no decoder. `getCurrentUser` verifies a token and
// returns a database user; `parse` returns `Result<Claims, AuthError>`. Neither
// type's properties are claims, and an importer's `user.email` is no read.
int test_typescript_library_in_body_is_no_decoder() {
  const auto graph = build({{"api/auth.ts", R"(
import { jwtVerify } from 'jose';
import { jwtDecode } from 'jwt-decode';
export interface AuthUser { email: string; orgId: string }
export interface Claims { roles: string[] }
export interface AuthError { code: string }
type Result<T, E> = { ok: T } | { err: E };
export async function getCurrentUser(token: string): Promise<AuthUser | null> {
  const { payload } = await jwtVerify(token, key);
  const user = await db.users.find(payload.sub);
  return user;
}
export function parse(token: string): Result<Claims, AuthError> {
  const decoded = jwtDecode(token);
  return wrap(decoded);
}
)"},
                            {"api/route.ts", R"(
import { getCurrentUser } from './auth';
export async function GET(token: string) {
  const user = await getCurrentUser(token);
  return user.email;
}
)"}});
  if (!all_claims(graph).empty()) {
    return fail("a library call in the body made a claims type: " + show(all_claims(graph)));
  }
  return 0;
}

// Item 3: the payload segment must flow through the base64 decode into the
// returned JSON.parse. A signed cookie `value.sig` keeps its payload in [0];
// a function that has every signal but on unrelated values is no decoder.
int test_hand_decoders_follow_the_data_path() {
  const auto graph = build({{"web/cookie.ts", R"(
export interface Session { user_id: string; roles: string[] }
export function readCookie(cookie: string): Session {
  const [value] = cookie.split('.');
  return JSON.parse(atob(value));
}
export function readSigned(cookie: string): Session | null {
  const parts = cookie.split('.');
  return JSON.parse(Buffer.from(parts[0], 'base64').toString('utf-8')) as Session;
}
export function scattered(token: string, body: string): Session {
  const parts = token.split('.');
  const segment = parts[1];
  const unrelated = Buffer.from(body, 'base64').toString();
  return JSON.parse(body) as Session;
}
)"},
                            {"app/Scattered.kt", R"(package app
fun scattered(token: String, body: String): String? {
    val segment = token.split('.').getOrNull(1)
    val bytes = Base64.UrlSafe.decode(body)
    return Json.parseToJsonElement(body).jsonObject["roles"]?.jsonPrimitive?.content
}
)"}});
  if (!all_claims(graph).empty()) {
    return fail("signals off the payload's data path made claims: " + show(all_claims(graph)));
  }
  // The same shapes on the payload path are decoders.
  const auto good = build({{"web/token.ts", R"(
export interface Payload { tenant_id: string }
export function viaDestructuring(token: string): Payload {
  const [, payload] = token.split('.');
  return JSON.parse(atob(payload));
}
)"}});
  if (all_claims(good) != Names{"tenant_id"}) {
    return fail("an array-destructured payload segment: " + show(all_claims(good)));
  }
  return 0;
}

// Item 4: every parameter and declaration is a binding, so an inner `payload`
// shadows the outer decoded one.
int test_reads_respect_shadowing() {
  const auto graph = build({{"web/lib/token.ts", R"(
export interface DecodedToken { roles: string[]; tenant_id: string }
export function decodeToken(token: string): DecodedToken | null {
  const parts = token.split('.');
  return JSON.parse(Buffer.from(parts[1], 'base64').toString('utf-8')) as DecodedToken;
}
)"},
                            {"web/page.ts", R"(
import { decodeToken } from './lib/token';
export function page(token: string, rows: Row[]) {
  const payload = decodeToken(token);
  const tenants = rows.map((payload) => payload.tenant_id);
  return payload.roles;
}
)"}});
  if (claims(graph, "web_page_ts_page", false) != Names{"roles"}) {
    return fail("a shadowing parameter read as the decoded payload: " + show(claims(graph, "web_page_ts_page", false)));
  }
  return 0;
}

// Item 5: a value from an npm package's function is never a candidate read.
int test_package_callees_emit_no_reads() {
  const std::string source = R"(
import { useRouter } from 'next/router';
import { decodeToken } from './token';
export function view() {
  const router = useRouter();
  const token = decodeToken(router.query.t);
  return [router.pathname, token.roles];
}
)";
  const cgraph::ExtractionContext context{.source_file = "web/view.ts", .relative_path = "web/view.ts", .source = source};
  const auto result = cgraph::extract_typescript(context);
  std::set<std::string> callees;
  for (const auto& relation : result.raw_relations) {
    if (relation.relation == "claim_read") {
      callees.insert(relation.target_label);
    }
  }
  if (callees != Names{"decodeToken"}) {
    return fail("claim reads through a package callee: " + show(callees));
  }
  return 0;
}

// Item 6: one positive and one negative per library form.
int test_library_forms() {
  const auto graph = build({{"a/verify.ts", R"(
import jwt from 'jsonwebtoken';
interface VerifiedPayload { org_id: string }
interface Serialized { not_verified: string }
export function check(t: string) {
  const ok = jwt.verify(t, key) as VerifiedPayload;
  const no = jwt.sign(body, key) as Serialized;
  return [ok, no];
}
)"},
                            {"a/jose.ts", R"(
import { jwtVerify, decodeJwt } from 'jose';
import { fetchJson } from './http';
interface JosePayload { workspace_id: string }
interface Decoded { device_id: string }
interface Fetched { not_jose: string }
export async function run(t: string) {
  const { payload } = await jwtVerify<JosePayload>(t, key);
  const d = decodeJwt<Decoded>(t);
  const f = await fetchJson<Fetched>('/x');
  return [payload, d, f];
}
)"},
                            {"a/local.ts", R"(
import { decodeJwt } from './jwt-helpers';
interface LocalOnly { not_library: string }
export function run(t: string) { return decodeJwt<LocalOnly>(t); }
)"},
                            {"a/require.js", R"(
const jwt = require('jsonwebtoken');
const other = require('./signer');
function issue(k) {
  other.sign({ not_jsonwebtoken: 1 }, k);
  return jwt.sign({ app_role: 'admin' }, k);
}
)"},
                            {"p/jose_tokens.py", R"(from jose import jwt
from mylib import jwt as notjwt

def issue(key):
    notjwt.encode({"not_jose": 1}, key)
    return jwt.encode({"device_id": "d"}, key)
)"},
                            {"g/std.go", R"(package g

import (
	jwtgo "github.com/dgrijalva/jwt-go"
	f3 "github.com/form3tech-oss/jwt-go"
	other "example.com/claims"
)

type LegacyClaims struct {
	Plan string `json:"plan"`
	jwtgo.StandardClaims
}

type Form3Claims struct {
	Region string `json:"region"`
	*f3.StandardClaims
}

type NotJwt struct {
	Shelf string `json:"shelf"`
	other.RegisteredClaims
}
)"},
                            {"g/parse.go", R"(package g

import "github.com/golang-jwt/jwt/v5"

type PointerClaims struct {
	Team string `json:"team"`
	*jwt.RegisteredClaims
}

type ParsedClaims struct {
	Grade string `json:"grade"`
}

type Unparsed struct {
	Unused string `json:"unused"`
}

func parse(tok string) {
	claims := &ParsedClaims{}
	jwt.ParseWithClaims(tok, claims, nil)
	other := &Unparsed{}
	use(other)
}
)"},
                            {"k/Nimbus.kt", R"(package k
fun nimbus(s: String) = JWTClaimsSet.Builder().claim("kotlin_nimbus", s).issuer("idp").build()
fun notNimbus() = Other.Builder().claim("not_nimbus", 1).build()
)"}});
  const Names expected{"app_role", "device_id", "grade", "iss",  "kotlin_nimbus", "org_id",
                       "plan",     "region",    "team",  "workspace_id"};
  if (all_claims(graph) != expected) {
    return fail("library forms: " + show(all_claims(graph)));
  }
  return 0;
}

// Review round 2, item 1: a variable holds what was last stored in it. A
// payload overwritten by a database row is no longer the payload, so `User`
// is no claims type; a `null` placeholder later filled with the payload is.
int test_reassignment_clears_the_payload() {
  const auto graph = build({{"api/load.ts", R"(
export interface User { email_address: string; plan: string }
export interface Late { tenant_id: string }
export function load(token: string): User {
  const parts = token.split('.');
  let data = JSON.parse(Buffer.from(parts[1], 'base64').toString());
  data = db.users.get(data.sub);
  return data;
}
export function late(token: string): Late {
  const parts = token.split('.');
  let data = null;
  data = JSON.parse(Buffer.from(parts[1], 'base64').toString());
  return data;
}
)"},
                            {"app/Load.kt", R"(package app
fun overwritten(token: String, body: String): String? {
    val segment = token.split('.').getOrNull(1) ?: return null
    var obj = Json.parseToJsonElement(Base64.UrlSafe.decode(segment).decodeToString())
    obj = Json.parseToJsonElement(body)
    return obj.jsonObject["roles"]?.jsonPrimitive?.content
}
fun kept(token: String): String? {
    val segment = token.split('.').getOrNull(1) ?: return null
    var obj = Json.parseToJsonElement(Base64.UrlSafe.decode(segment).decodeToString())
    return obj.jsonObject["session_id"]?.jsonPrimitive?.content
}
)"}});
  if (all_claims(graph) != Names{"session_id", "tenant_id"}) {
    return fail("an overwritten payload stayed the payload: " + show(all_claims(graph)));
  }
  return 0;
}

// Item 2: jose's `jwtVerify` and jsonwebtoken's `complete: true` return a
// wrapper; only its `.payload` (or a destructured `payload`) is the payload.
int test_library_wrappers_are_not_the_payload() {
  const auto graph = build({{"api/wrap.ts", R"(
import { jwtVerify } from 'jose';
import jwt from 'jsonwebtoken';
interface Wrapped { protectedHeader: string; payload: string }
interface Inner { org_slug: string }
export async function whole(t: string): Promise<Wrapped> {
  const r = await jwtVerify(t, key);
  return r;
}
export async function inner(t: string): Promise<Inner> {
  const r = await jwtVerify(t, key);
  return r.payload as Inner;
}
export function complete(t: string) {
  return jwt.decode(t, { complete: true }) as Wrapped;
}
)"}});
  if (all_claims(graph) != Names{"org_slug"}) {
    return fail("a library wrapper was taken for the payload: " + show(all_claims(graph)));
  }
  return 0;
}

// Item 3: `for (const payload of rows)` and `catch (payload)` shadow an outer
// decoded `payload` inside their blocks only.
int test_loop_and_catch_bindings_shadow() {
  const auto graph = build({{"web/lib/token.ts", R"(
export interface DecodedToken { roles: string[]; tenant_id: string; user_id: string }
export function decodeToken(token: string): DecodedToken | null {
  const parts = token.split('.');
  return JSON.parse(Buffer.from(parts[1], 'base64').toString('utf-8')) as DecodedToken;
}
)"},
                            {"web/rows.ts", R"(
import { decodeToken } from './lib/token';
export function rows(token: string, list: Row[]) {
  const payload = decodeToken(token);
  for (const payload of list) { use(payload.tenant_id); }
  try { run(); } catch (payload) { use(payload.user_id); }
  return payload.roles;
}
)"}});
  if (claims(graph, "web_rows_ts_rows", false) != Names{"roles"}) {
    return fail("a loop or catch binding read as the decoded payload: " + show(claims(graph, "web_rows_ts_rows", false)));
  }
  return 0;
}

}  // namespace

int main() {
  int failures = 0;
  failures += test_kotlin_jjwt_builder();
  failures += test_java_builders();
  failures += test_go_payload_decode();
  failures += test_go_golang_jwt();
  failures += test_typescript_decoder_and_reads();
  failures += test_typescript_libraries();
  failures += test_kotlin_payload_reads();
  failures += test_python_encode();
  failures += test_cross_repo_ids();
  failures += test_typescript_library_in_body_is_no_decoder();
  failures += test_hand_decoders_follow_the_data_path();
  failures += test_reads_respect_shadowing();
  failures += test_package_callees_emit_no_reads();
  failures += test_library_forms();
  failures += test_reassignment_clears_the_payload();
  failures += test_library_wrappers_are_not_the_payload();
  failures += test_loop_and_catch_bindings_shadow();
  if (failures == 0) {
    std::cout << "claim_contracts_test: ok\n";
  }
  return failures == 0 ? 0 : 1;
}
