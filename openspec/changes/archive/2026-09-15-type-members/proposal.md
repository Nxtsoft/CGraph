# Type members (CGR-9 extraction)

## Why

Structural type comparison requires a declared member list for each owner. C/C++ currently emits fields; TypeScript, Go, Rust, Python and Java do not. C/C++ forward declarations also create duplicate type nodes.

## What Changes

- Add an opt-in member handler to LanguageConfig, invoked for named class/type definitions.
- Emit extracted field nodes and defines edges with IDs `make_id(source_file + ":" + TypeName + "::" + member)`.
- Preserve declared type text and grammar-level optional/readonly flags as string properties.
- Cover TypeScript interfaces, object aliases, classes and enums; Go structs; Rust named/tuple structs, enums and trait associated types and constants; Python class-body declarations; Java classes, records and enums.
- Skip C/C++ class-like declarations without bodies before creating graph nodes.
- Enable and verify languages in order: TypeScript, Go, Rust, Python, Java. Preserve disabled-language output byte for byte.

## Boundaries

Only declared members are extracted. No inherited member expansion, alias resolution, runtime Python attribute inference or cross-language type canonicalization. TypeScript union/intersection aliases do not flatten into a single misleading member set. Rust enum payload text belongs to the variant; tuple fields use positional labels.

Declarations that are already graph nodes of their own are never re-emitted as members: TypeScript `method_signature`/`abstract_method_signature` and Rust `function_item`/`function_signature_item`/`type_item` keep their function and type nodes. A member node would reuse their id and overwrite them, erasing the `interface_method` tag that trait and interface dispatch resolution reads.

The modules teammate owns the Report operation. The `report types` view, unused/duplicate/overlap calculations and CLI/MCP integration are a follow-up after that operation lands.
