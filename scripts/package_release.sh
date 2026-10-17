#!/usr/bin/env bash
# Packages one platform's release asset: the four binaries from a CMake build
# tree into cgraph-<platform>.tar.gz in the current directory. Used by
# release-binaries.yml (upload) and ci.yml (Windows asset check).
#   scripts/package_release.sh BUILD_DIR PLATFORM
set -euo pipefail
build="$1/src"
platform="$2"
exe=""
case "$platform" in windows-*) exe=".exe" ;; esac
dist=dist
mkdir -p "$dist"
cp "$build/cli/cgraph$exe" \
   "$build/daemon/graphd$exe" \
   "$build/client/cgraph-client$exe" \
   "$build/mcp/cgraph-mcp$exe" "$dist/"
tar -czf "cgraph-$platform.tar.gz" -C "$dist" .
tar -tzf "cgraph-$platform.tar.gz"
