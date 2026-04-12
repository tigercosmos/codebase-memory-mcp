"""Downloads the codebase-memory-mcp binary on first run, then exec's it."""

import os
import sys
import platform
import stat
import shutil
import tempfile
import urllib.request
import urllib.error
from pathlib import Path

REPO = "DeusData/codebase-memory-mcp"


def _version() -> str:
    try:
        from importlib.metadata import version
        return version("codebase-memory-mcp")
    except Exception:
        return "0.6.0"


def _os_name() -> str:
    p = sys.platform
    if p == "linux":
        return "linux"
    if p == "darwin":
        return "darwin"
    if p == "win32":
        return "windows"
    sys.exit(f"codebase-memory-mcp: unsupported platform: {p}")


def _arch() -> str:
    m = platform.machine().lower()
    if m in ("arm64", "aarch64"):
        return "arm64"
    if m in ("x86_64", "amd64"):
        return "amd64"
    sys.exit(f"codebase-memory-mcp: unsupported architecture: {m}")


def _cache_dir() -> Path:
    if sys.platform == "win32":
        base = Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData" / "Local"))
    elif sys.platform == "darwin":
        base = Path.home() / "Library" / "Caches"
    else:
        base = Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache"))
    return base / "codebase-memory-mcp"


def _bin_path(version: str) -> Path:
    name = "codebase-memory-mcp.exe" if sys.platform == "win32" else "codebase-memory-mcp"
    return _cache_dir() / version / name


def _download(version: str) -> Path:
    os_name = _os_name()
    arch = _arch()
    ext = "zip" if os_name == "windows" else "tar.gz"
    url = (
        f"https://github.com/{REPO}/releases/download/v{version}"
        f"/codebase-memory-mcp-{os_name}-{arch}.{ext}"
    )

    dest = _bin_path(version)
    dest.parent.mkdir(parents=True, exist_ok=True)

    print(
        f"codebase-memory-mcp: downloading v{version} for {os_name}/{arch}...",
        file=sys.stderr,
    )

    with tempfile.TemporaryDirectory() as tmp:
        tmp_archive = os.path.join(tmp, f"cbm.{ext}")
        try:
            urllib.request.urlretrieve(url, tmp_archive)
        except urllib.error.HTTPError as e:
            sys.exit(
                f"codebase-memory-mcp: download failed ({e})\n"
                f"URL: {url}\n"
                f"See https://github.com/{REPO}/releases for available versions."
            )

        if ext == "tar.gz":
            import tarfile
            with tarfile.open(tmp_archive) as tf:
                tf.extractall(tmp)
        else:
            import zipfile
            with zipfile.ZipFile(tmp_archive) as zf:
                zf.extractall(tmp)

        bin_name = "codebase-memory-mcp.exe" if os_name == "windows" else "codebase-memory-mcp"
        extracted = os.path.join(tmp, bin_name)
        if not os.path.exists(extracted):
            sys.exit(f"codebase-memory-mcp: binary not found after extraction")

        shutil.copy2(extracted, dest)
        current = dest.stat().st_mode
        dest.chmod(current | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)

    return dest


def main() -> None:
    version = _version()
    bin_path = _bin_path(version)

    if not bin_path.exists():
        bin_path = _download(version)

    args = [str(bin_path)] + sys.argv[1:]

    if sys.platform != "win32":
        os.execv(str(bin_path), args)
    else:
        import subprocess
        result = subprocess.run(args)
        sys.exit(result.returncode)
