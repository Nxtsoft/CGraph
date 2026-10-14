#!/usr/bin/env python3
"""Pin the plugin/npm launcher to a published binaries release.

    scripts/pin_launcher_release.py bin-v0.9.0

Reads the release's tarball assets and their SHA-256 digests from GitHub
(`gh release view`), rewrites integrations/launcher/release.json, and sets the
same version (the tag without its `bin-v` prefix) in the plugin manifest, the
npm package and server.json, so the four never disagree.
"""

import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
REPOSITORY = "Nxtsoft/CGraph"
PREFIX = "cgraph-"
SUFFIX = ".tar.gz"


def release_assets(tag: str) -> dict:
    out = subprocess.run(
        ["gh", "release", "view", tag, "--repo", REPOSITORY, "--json", "assets"],
        check=True, capture_output=True, text=True,
    ).stdout
    assets = {}
    for asset in json.loads(out)["assets"]:
        name = asset["name"]
        if not (name.startswith(PREFIX) and name.endswith(SUFFIX)):
            continue
        algorithm, _, digest = asset["digest"].partition(":")
        if algorithm != "sha256" or len(digest) != 64:
            raise SystemExit(f"{name}: unexpected digest {asset['digest']!r}")
        assets[name[len(PREFIX):-len(SUFFIX)]] = {"name": name, "sha256": digest}
    if not assets:
        raise SystemExit(f"{tag} has no {PREFIX}*{SUFFIX} assets")
    return dict(sorted(assets.items()))


def rewrite(path: Path, update) -> None:
    data = json.loads(path.read_text())
    update(data)
    path.write_text(json.dumps(data, indent=2) + "\n")


def main() -> None:
    if len(sys.argv) != 2 or not sys.argv[1].startswith("bin-v"):
        raise SystemExit("usage: pin_launcher_release.py bin-vX.Y.Z")
    tag = sys.argv[1]
    version = tag[len("bin-v"):]
    release = {"repository": REPOSITORY, "tag": tag, "assets": release_assets(tag)}
    (ROOT / "integrations/launcher/release.json").write_text(json.dumps(release, indent=2) + "\n")

    def set_version(data):
        data["version"] = version

    def set_server(data):
        data["version"] = version
        for package in data["packages"]:
            package["version"] = version

    rewrite(ROOT / "integrations/.claude-plugin/plugin.json", set_version)
    rewrite(ROOT / "packaging/npm/package.json", set_version)
    rewrite(ROOT / "server.json", set_server)
    print(f"pinned {tag}: {', '.join(release['assets'])}")


if __name__ == "__main__":
    main()
