#pragma once

// JWT claims as contracts (contracts.hpp: `claim:<name>`), typed only.
//
// A claim name alone proves nothing: `session_id`, `roles` and `email` are
// ordinary field and map-key names in most services. So a claim fact comes
// only from code that is provably about a JWT:
//
//   provides_contract  a claim a token builder writes:
//     Kotlin/Java  `.claim("x", v)` on a chain rooted at jjwt's
//                  `Jwts.builder()` or Nimbus's `JWTClaimsSet.Builder()`, and
//                  a bare `claim("x", v)` inside an `apply { }` / `run { }`
//                  lambda on that chain (its receiver is the builder); the
//                  registered-claim setters (`.subject(..)`, `.issuer(..)`,
//                  `.expiration(..)`, ...) write `sub`, `iss`, `exp`, ...
//     TS/JS        the keys of the payload object of `jwt.sign({..})`
//                  (`jsonwebtoken`) and `new SignJWT({..})` (`jose`), plus
//                  `SignJWT`'s registered-claim setters
//     Python       the string keys of the dict passed to `jwt.encode({..})`
//                  where `jwt` is PyJWT's or python-jose's module
//   uses_contract      a claim a token reader reads:
//     Go           the `json:"x"` tags of a struct that embeds golang-jwt's
//                  RegisteredClaims/StandardClaims, is the claims argument of
//                  `jwt.ParseWithClaims`, or is the `json.Unmarshal` target in
//                  a same-file function that splits a token on "." and
//                  base64-decodes a segment (a hand-written payload decode)
//     TS           the properties of an interface or object type alias a
//                  decoder returns: `jwtDecode<T>` (`jwt-decode`),
//                  `decodeJwt<T>` / `jwtVerify<T>` (`jose`), `jwt.verify(..)
//                  as T` (`jsonwebtoken`), or a same-file function whose
//                  declared return type names T and whose body calls one of
//                  those or decodes the payload by hand (`.split('.')`,
//                  `JSON.parse`, base64); and, in any file, a property read
//                  `v.x` where `v` holds the result of calling such a
//                  hand-written decoder and `x` is a property of its type
//                  (resolve_claim_reads, through the file's imports)
//     Kotlin       `.jsonObject["x"]` directly on `Json.parseToJsonElement(..)`
//                  in a function that also splits on '.' and calls a
//                  `Base64...decode(..)`: all three in one function
//
// The fact's source is the field node for a typed field, else the innermost
// function (the file at module level).

#include "cgraph/graph_builder.hpp"
#include "cgraph/language_config.hpp"

#include <tree_sitter/api.h>

#include <span>
#include <string_view>
#include <vector>

namespace cgraph {

// Appends the claim facts of one parsed file. `language` is the
// LanguageConfig name (`kotlin`, `java`, `go`, `typescript`, `tsx`,
// `javascript`, `python`); other languages emit nothing. `fragment` is the
// file's finished symbol fragment, which names the source nodes.
// Besides provides_contract / uses_contract this emits two intermediate facts
// for reads through a decoder another file defines:
//   "claim_decoder"  source_id = a hand-written decoder function, context =
//                    one property of the claims type it returns
//   "claim_read"     source_id = the reading function, target_label = the
//                    function called for the value (`decodeToken`), context =
//                    the property read
void extract_claim_contracts(TSNode root, std::string_view language, const ExtractionContext& context,
                             const Fragment& fragment, std::vector<RawRelation>& out);

// True for the intermediate relations above, which resolve_raw_relations skips.
[[nodiscard]] bool is_claim_read_relation(std::string_view relation);

// The uses_contract facts the claim_read facts amount to: a read whose callee
// resolves (imports first, then the reading file's own declarations) to a
// claim_decoder function that returns a type with that property.
[[nodiscard]] std::vector<RawRelation> resolve_claim_reads(std::span<const RawRelation> raw_relations,
                                                           const RelationScopes& scopes);

}  // namespace cgraph
