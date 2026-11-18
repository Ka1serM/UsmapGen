#!/usr/bin/env python3
import argparse
import hashlib
import json
import os
import platform
import shutil
import subprocess
import sys
import urllib.request
import zipfile
from pathlib import Path

DOWNLOADER_VERSION = "3.4.0"
DOWNLOADERS = {
    ("linux", "x64"): "a999dec66b4850fc961bd50366696d23c2d0fad7b18790e6a5647b2f19097a53",
    ("linux", "arm64"): "d9fb612ccebc1db8eeea3b4045d2221ec70431381393ce908fb72f01d4f9c812",
    ("windows", "x64"): "41c9e9f0df54b3ad02e67a11726756e5c73283bd7c2e1b04acfa5ae4c2ed3767",
    ("windows", "arm64"): "1449ba47775e9974036e615bedf00d72bef747cc5b93a78048ed4e0b2c63b2b3",
    ("macos", "x64"): "3214b689564d73e9342a8a4aef693de6ad3d293801b0f300a4466f60ec75befb",
    ("macos", "arm64"): "60e80c7c496f3f9a079cd3c62036b35d088c27bc0149baf38f009eb57a52f6a5",
}
EXECUTABLES = Path(__file__).resolve().parent / "Executables.json"


def digest(path: Path, algorithm: str) -> str:
    hasher = hashlib.new(algorithm)
    with path.open("rb") as file:
        while chunk := file.read(1 << 20):
            hasher.update(chunk)
    return hasher.hexdigest()


def host() -> tuple[str, str]:
    system = {"linux": "linux", "win32": "windows", "darwin": "macos"}.get(sys.platform)
    machine = {"x86_64": "x64", "amd64": "x64", "arm64": "arm64", "aarch64": "arm64"}.get(platform.machine().lower())
    return system, machine


def downloader_path(cache: Path) -> Path:
    name = "DepotDownloader.exe" if host()[0] == "windows" else "DepotDownloader"
    return cache / f"DepotDownloader-{DOWNLOADER_VERSION}" / name


def fetch_downloader(cache: Path) -> int:
    path = downloader_path(cache)
    if path.is_file():
        return 0
    system, machine = host()
    expected = DOWNLOADERS.get((system, machine))
    if expected is None:
        print(f"DepotDownloader has no release for {sys.platform} {platform.machine()}.", file=sys.stderr)
        return 1
    url = f"https://github.com/SteamRE/DepotDownloader/releases/download/DepotDownloader_{DOWNLOADER_VERSION}/DepotDownloader-{system}-{machine}.zip"
    archive = path.parent.with_suffix(".zip")
    archive.parent.mkdir(parents=True, exist_ok=True)
    urllib.request.urlretrieve(url, archive)
    if digest(archive, "sha256") != expected:
        archive.unlink()
        print(f"{url} does not have the pinned SHA-256.", file=sys.stderr)
        return 1
    with zipfile.ZipFile(archive) as release:
        release.extractall(path.parent)
    archive.unlink()
    path.chmod(0o755)
    return 0


def is_valid(path: Path, entry: dict) -> bool:
    return path.is_file() and path.stat().st_size == entry["size"] and digest(path, "sha1") == entry["sha1"]


def fetch_executable(cache: Path, app: int) -> int:
    entry = next((e for e in json.loads(EXECUTABLES.read_text()) if e["app"] == app), None)
    if entry is None:
        print(f"{EXECUTABLES} has no app {app}.", file=sys.stderr)
        return 1
    target = cache / str(app) / Path(entry["file"]).name
    if is_valid(target, entry):
        return 0
    work = cache / f"{app}.download"
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    file_list = work / "files.txt"
    file_list.write_text(entry["file"] + "\n")
    #! Without -username DepotDownloader logs in anonymously, which has every free dedicated server.
    command = [str(downloader_path(cache)), "-app", str(app), "-depot", str(entry["depot"]), "-manifest", entry["manifest"],
               "-filelist", str(file_list), "-dir", str(work / "depot"), "-validate"]
    result = subprocess.run(command, stdin=subprocess.DEVNULL, capture_output=True, text=True, env=os.environ | {"TMPDIR": str(work)})
    downloaded = work / "depot" / entry["file"]
    if result.returncode != 0 or not is_valid(downloaded, entry):
        print(result.stdout + result.stderr, file=sys.stderr)
        print(f"App {app} depot {entry['depot']} manifest {entry['manifest']} did not give {entry['file']} with the pinned size and SHA-1.", file=sys.stderr)
        return 1
    target.parent.mkdir(parents=True, exist_ok=True)
    downloaded.replace(target)
    shutil.rmtree(work)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Downloads the pinned dedicated server executables of the tests from Steam.")
    parser.add_argument("cache", type=Path)
    parser.add_argument("app", type=int, nargs="?", help="the app of one executable; without it, the script gets DepotDownloader")
    args = parser.parse_args()
    if args.app is None:
        return fetch_downloader(args.cache)
    return fetch_executable(args.cache, args.app)


if __name__ == "__main__":
    sys.exit(main())
