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
  if (failures == 0) {
    std::cout << "claim_contracts_test: ok\n";
  }
  return failures == 0 ? 0 : 1;
}
