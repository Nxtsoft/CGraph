"""Check out every system in systems.json at its pinned commit and lay each service out as
its own git repository: <work>/repos/<system>/<service>/. Every tool indexes those copies,
so a monorepo service and a service with its own upstream repo look the same to it.

    python3 materialize.py <work>
"""
import json
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent


def git(*args, cwd=None):
    subprocess.run(["git", *args], cwd=cwd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def fetch(cache: Path, url: str, commit: str) -> Path:
    """A checkout of url at commit under cache (shallow fetch of that one commit)."""
    dest = cache / (url.rstrip("/").split("github.com/")[-1].replace("/", "__") + "@" + commit[:12])
    if not (dest / ".git").is_dir():
        if dest.exists():
            shutil.rmtree(dest)
        git("init", "--quiet", str(dest))
        git("remote", "add", "origin", url, cwd=dest)
        git("fetch", "--quiet", "--depth", "1", "origin", commit, cwd=dest)
        git("-c", "advice.detachedHead=false", "checkout", "--quiet", "--force", commit, cwd=dest)
    head = subprocess.run(["git", "rev-parse", "HEAD"], cwd=dest, check=True, capture_output=True, text=True).stdout.strip()
    if head != commit:
        sys.exit(f"{url}: checked out {head}, expected {commit}")
    return dest


def make_repo(src: Path, dest: Path):
    """Copy the tracked files of src (a directory inside a checkout) into a fresh git repo."""
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    top = Path(subprocess.run(["git", "rev-parse", "--show-toplevel"], cwd=src, check=True, capture_output=True,
                              text=True).stdout.strip())
    rel = src.relative_to(top)
    files = subprocess.run(["git", "ls-files", "-z", "--", str(rel) if str(rel) != "." else "."], cwd=top, check=True,
                           capture_output=True, text=True).stdout.split("\0")
    for f in filter(None, files):
        target = dest / Path(f).relative_to(rel)
        target.parent.mkdir(parents=True, exist_ok=True)
        if (top / f).is_symlink() or not (top / f).is_file():
            continue
        shutil.copy2(top / f, target)
    git("init", "--quiet", cwd=dest)
    git("add", "-A", cwd=dest)
    git("-c", "user.name=bench", "-c", "user.email=bench@localhost", "commit", "--quiet", "-m", "snapshot", cwd=dest)


def main():
    work = Path(sys.argv[1]).resolve()
    cache = work / "cache"
    cache.mkdir(parents=True, exist_ok=True)
    systems = json.loads((HERE / "systems.json").read_text())["systems"]
    for system in systems:
        for service, where in system["services"].items():
            if isinstance(where, dict):
                src = fetch(cache, where["repo"], where["commit"])
            else:
                src = fetch(cache, system["repo"], system["commit"]) / where
            make_repo(src, work / "repos" / system["name"] / service)
        print(f"{system['name']}: {len(system['services'])} service repos", file=sys.stderr)


if __name__ == "__main__":
    main()
