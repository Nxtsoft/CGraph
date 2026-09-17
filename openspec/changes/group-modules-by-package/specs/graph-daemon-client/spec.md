## ADDED Requirements

### Requirement: The modules view groups by workspace package when the project declares one
The `report` op's `modules` view SHALL read the project root's workspace manifest and use the packages it declares as module names. The manifests SHALL be a `package.json` with a `workspaces` array or a `workspaces.packages` array, a `pnpm-workspace.yaml` with a `packages:` list (block or inline), a `Cargo.toml` with `[workspace]` `members` and `exclude` arrays that may span lines, and a `go.work` with `use` directives bare or in a `use ( … )` block; the first that names members SHALL be used. Member globs SHALL support a literal segment, `*` for exactly one segment, `**` for any number, and a leading `!` to exclude, expanded against directories under the root, skipping the directories the project scanners skip and bounded in depth and count. A matched directory SHALL be a package only when it declares its own `package.json`, `Cargo.toml` or `go.mod`, taking its name from that manifest's `name` (Cargo `[package] name`, go.mod `module`) or, when absent, the directory name; a package nested inside another SHALL own its own files. A file under no package SHALL keep its directory-depth module, so no file is dropped. The response SHALL carry `group_by` (`packages` or `depth`), the `manifest` that supplied the packages and the `packages` count, and the rendered caption SHALL name the manifest and package count in place of the depth. The request SHALL accept `group_by`: `auto` (default: packages when declared, else depth), `packages`, or `depth`, and SHALL reject any other value with an error naming the three.

#### Scenario: An npm workspace names the modules
- **GIVEN** a root `package.json` with `"workspaces": ["apps/*", "packages/*"]`, `apps/web/package.json` named `@acme/web`, `packages/ui/package.json` named `ui-kit`, and a file under `tools/`
- **WHEN** `report` runs with `view: "modules"`
- **THEN** `group_by` is `packages`, `manifest` is `package.json`, `packages` is 2, the modules include `@acme/web` and `ui-kit`, a call between their files is an edge between those modules, and the `tools` file's module is `tools`

#### Scenario: Each manifest format is read
- **GIVEN** a pnpm workspace listing `apps/*`, `libs/**` and `!libs/private`; a Cargo workspace whose `members` span lines with an `exclude`; and a `go.work` with a bare `use` and a `use ( … )` block
- **THEN** each yields its declared packages, named by their own manifests, with the excluded ones left out and `manifest` naming the file that declared them

#### Scenario: A glob hit that declares nothing is not a package
- **GIVEN** `packages/*` matching a directory with no `package.json`
- **THEN** it is not a package, and its files keep their directory-depth module

#### Scenario: A project without a workspace is unchanged
- **GIVEN** a root whose `package.json` has no `workspaces`, or a malformed manifest, or globs matching nothing
- **THEN** `group_by` is `depth`, `packages` is 0, and module names are the directory components as before

#### Scenario: group_by overrides
- **WHEN** `group_by` is `depth` on a repository that declares a workspace
- **THEN** the modules are directory components and `packages` is 0
- **AND** an unknown `group_by` is an error naming `auto|packages|depth`
