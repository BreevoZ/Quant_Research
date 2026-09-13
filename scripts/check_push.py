#!/usr/bin/env python3
"""Read-only pre-push checks. Diagnostics never include matched secret values.

默认检查将由 git add -A 收集的文件；--staged 检查索引中的实际内容。
两种模式都扫描现有可达 Git 历史，不修改索引、不联网。
"""

import argparse
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
MAX_BYTES = 50 * 1024 * 1024
SECRET_PATTERNS = {
    "AWS access key": rb"\b(?:AKIA|ASIA)[0-9A-Z]{16}\b",
    "GitHub token": rb"\b(?:gh[pousr]_[A-Za-z0-9]{36}|github_pat_[A-Za-z0-9_]{70,})\b",
    "private key": rb"-----BEGIN (?:[A-Z0-9]+ )?PRIVATE KEY-----",
    "Slack token": rb"\bxox[baprs]-[A-Za-z0-9-]{20,}\b",
    "API token": rb"\bsk-(?:proj-|svcacct-)?[A-Za-z0-9_-]{32,}\b",
}


def git(root: Path, *args: str, input_data: bytes | None = None) -> bytes:
    return subprocess.check_output(["git", "-C", str(root), *args], input=input_data,
                                   stderr=subprocess.PIPE)


def local_credentials(root: Path) -> list[bytes]:
    """Compare local Massive key values without importing or running the downloader."""
    values = set()
    for name in ("KEYS.txt", "data_sources/massive/KEYS.txt", "massive_down/KEYS.txt"):
        path = root / name
        if path.is_file():
            for line in path.read_text(encoding="utf-8").splitlines():
                token = line.strip()
                if re.fullmatch(r"[A-Za-z0-9_./+=-]{16,}", token) and len(set(token)) > 4:
                    values.add(token.encode())
    return sorted(values)


def content_findings(data: bytes, credentials: list[bytes], *, history: bool = False) -> list[str]:
    findings = []
    for name, pattern in SECRET_PATTERNS.items():
        match = re.search(pattern, data)
        if match:
            line = data.count(b"\n", 0, match.start()) + 1
            findings.append(f"{name} at line {line}")
    if any(value in data for value in credentials):
        findings.append("matches a local Massive credential (value redacted)")
    # Binary files may coincidentally contain marker bytes; only check text for conflicts.
    if not history and b"\0" not in data:
        match = re.search(rb"^(?:<{7}|>{7}) [^\r\n]+$|^={7}$", data, re.M)
        if match:
            line = data.count(b"\n", 0, match.start()) + 1
            findings.append(f"merge conflict marker at line {line}")
    return findings


def private_path(name: str) -> bool:
    path = Path(name)
    return (path.name == "qr.toml" or path.name == "KEYS.txt" or path.name.startswith("KEYS.txt.")
            or path.name == ".env"
            or (path.name.startswith(".env.") and path.name not in {".env.example", ".env.template"})
            or path.suffix.lower() in {".key", ".pem", ".p12", ".pfx"})


def candidate_files(root: Path, staged: bool):
    if staged:
        for record in git(root, "ls-files", "--stage", "-z").split(b"\0"):
            if not record:
                continue
            metadata, name = record.split(b"\t", 1)
            mode, oid, stage = metadata.decode().split()
            if stage != "0":
                raise ValueError("Unmerged index entries; resolve conflicts before pushing")
            if mode == "160000":
                raise ValueError("Submodules need a separate audit before pushing")
            size = int(git(root, "cat-file", "-s", oid))
            yield name.decode(), mode, size, (git(root, "cat-file", "blob", oid) if size < MAX_BYTES else b"")
    else:
        names = set(git(root, "ls-files", "-z").split(b"\0"))
        names.update(git(root, "ls-files", "--others", "--exclude-standard", "-z").split(b"\0"))
        for raw in sorted(names - {b""}):
            name = raw.decode()
            path = root / name
            if path.is_symlink():
                data = str(path.readlink()).encode()
                yield name, "120000", len(data), data
            elif path.is_file():
                size = path.stat().st_size
                yield name, "100644", size, path.read_bytes() if size < MAX_BYTES else b""
            elif path.exists():
                raise ValueError(f"Nested repository or non-file entry needs separate audit: {name}")


def history_blobs(root: Path):
    objects = git(root, "rev-list", "--objects", "--all").splitlines()
    if not objects:
        return
    ids = b"\n".join(line.split(b" ", 1)[0] for line in objects) + b"\n"
    metadata = git(root, "cat-file", "--batch-check=%(objectname) %(objecttype) %(objectsize)",
                   input_data=ids).splitlines()
    for entry, meta in zip(objects, metadata):
        oid, kind, size = meta.decode().split()
        if kind == "blob":
            name = entry.partition(b" ")[2].decode()
            yield name or oid, int(size), (git(root, "cat-file", "blob", oid) if int(size) < MAX_BYTES else b"")
        elif kind in {"commit", "tag"}:
            # Commit/tag messages are pushed too; include them in the content scan.
            yield f"{kind}:{oid}", int(size), (git(root, "cat-file", kind, oid) if int(size) < MAX_BYTES else b"")


def audit(root: Path, staged: bool = False) -> tuple[dict, list[str]]:
    credentials = local_credentials(root)
    errors, count, total, largest = [], 0, 0, 0
    for name, mode, size, data in candidate_files(root, staged):
        count += 1
        total += size
        largest = max(largest, size)
        if private_path(name):
            errors.append(f"candidate:{name}: private configuration/credential filename")
        if size >= MAX_BYTES:
            errors.append(f"candidate:{name}: exceeds repository guard of 50 MiB")
        if mode == "120000":
            target = Path(data.decode())
            resolved = (root / name).parent.joinpath(target).resolve()
            if target.is_absolute() or not resolved.is_relative_to(root.resolve()) or not resolved.exists():
                errors.append(f"candidate:{name}: nonportable or broken symlink")
        errors.extend(f"candidate:{name}: {finding}" for finding in content_findings(data, credentials))
    history_count = 0
    for name, size, data in history_blobs(root):
        history_count += 1
        if size >= MAX_BYTES:
            errors.append(f"history:{name}: exceeds repository guard of 50 MiB")
        if private_path(name):
            errors.append(f"history:{name}: private configuration/credential filename")
        errors.extend(f"history:{name}: {finding}"
                      for finding in content_findings(data, credentials, history=True))
    return {"mode": "staged" if staged else "worktree", "candidate_files": count,
            "total_bytes": total, "largest_bytes": largest, "history_objects": history_count,
            "local_credentials_compared": len(credentials), "errors": len(errors)}, errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--staged", action="store_true", help="Inspect Git index contents, not working files")
    args = parser.parse_args()
    try:
        summary, errors = audit(ROOT, args.staged)
    except (OSError, ValueError, subprocess.CalledProcessError):
        # Git/parser errors can embed file contents; avoid echoing exception messages.
        print("Pre-push audit could not complete; check Git state and local file readability.", file=sys.stderr)
        return 2
    for error in errors:
        print(error, file=sys.stderr)
    print("Pre-push checks:", ", ".join(f"{key}={value}" for key, value in summary.items()))
    print("Heuristic checks only; review staged changes too. / 启发式检查，仍需查看暂存差异。")
    return int(bool(errors))


if __name__ == "__main__":
    raise SystemExit(main())
