#!/usr/bin/env python3
"""Offline layout, documentation-link, syntax and path-contract checks.

离线检查目录、文档链接、语法和路径契约；不读取行情或密钥，不修改 Git 索引。
"""

import ast
from pathlib import Path
import re
import subprocess
import sys
import tomllib
from urllib.parse import unquote, urlsplit
import os

ROOT = Path(__file__).resolve().parents[1]
SOURCE_ROOTS = ("ashare", "us_equity", "qr", "data_sources", "archive", "docs", "scripts")
PRUNE = {"build", "data", "__pycache__", ".git", "downloads"}


def source_files():
    yield ROOT / "README.md"
    yield ROOT / "third_party/README.md"
    for name in SOURCE_ROOTS:
        for directory, children, files in os.walk(ROOT / name):
            children[:] = [child for child in children if child not in PRUNE and not child.startswith(".")]
            for file in files:
                path = Path(directory) / file
                if path.suffix in {".py", ".sh", ".md"}:
                    yield path


def main() -> int:
    errors, counts = [], {"python": 0, "shell": 0, "markdown": 0, "links": 0}
    for name in ("README.md", "ashare/README.md", "us_equity/README.md", "environment.yml"):
        if not (ROOT / name).is_file():
            errors.append(f"Missing primary entry: {name}")
    for old in ("tltoflow", "datacheck", "auction", "build_tick3s", "vorder_sim", "l3_factor", "factor", "massive_down"):
        if (ROOT / old).exists():
            errors.append(f"Old top-level source directory still exists: {old}")
    for name in ("ashare/tltoflow/run.sh", "ashare/build_tick3s/run.sh", "ashare/build_tick3s/build.sh",
                 "ashare/auction/cpp/run.sh", "ashare/auction/cpp/build.sh"):
        if not (ROOT / name).is_file() or (os.name != "nt" and not os.access(ROOT / name, os.X_OK)):
            errors.append(f"Documented Shell entry is missing or not executable: {name}")
    for path in source_files():
        relative = path.relative_to(ROOT)
        text = path.read_text(encoding="utf-8-sig")
        if path.suffix == ".py":
            try:
                ast.parse(text, filename=str(relative))
                counts["python"] += 1
            except SyntaxError as exc:
                errors.append(f"Python syntax: {relative}: {exc}")
        elif path.suffix == ".sh":
            result = subprocess.run(["bash", "-n", str(path)], capture_output=True, text=True)
            counts["shell"] += 1
            if result.returncode:
                errors.append(f"Shell syntax: {relative}: {result.stderr.strip()}")
        else:
            counts["markdown"] += 1
            # Do not mistake source examples or generated image references for navigation links.
            prose = re.sub(r"```.*?```", "", text, flags=re.S)
            for target in re.findall(r"(?<!!)\[[^\]\n]+\]\(([^)\n]+)\)", prose):
                target = target.strip().split(' "', 1)[0].strip("<>")
                url = urlsplit(target)
                if url.scheme or not url.path:
                    continue
                counts["links"] += 1
                if not (path.parent / unquote(url.path)).exists():
                    errors.append(f"Broken local link: {relative} -> {target}")

    tree = ast.parse((ROOT / "qr/paths.py").read_text())
    node = next(n for n in tree.body if isinstance(n, ast.AnnAssign)
                and isinstance(n.target, ast.Name) and n.target.id == "DEFAULTS")
    defaults = ast.literal_eval(node.value)
    with (ROOT / "qr.example.toml").open("rb") as fh:
        example = tomllib.load(fh)["paths"]
    if example != defaults:
        errors.append("qr.example.toml and Python defaults differ")
    header = (ROOT / "qr/qr_paths.h").read_text().split("kDefaults = {", 1)[1].split("};", 1)[0]
    cpp_defaults = dict(re.findall(r'\{"([^"\n]+)",\s*"([^"\n]+)"\}', header))
    if cpp_defaults != defaults:
        errors.append("C++ and Python path defaults differ")
    with (ROOT / "pyproject.toml").open("rb") as fh:
        project = tomllib.load(fh)
    for package in project["tool"]["setuptools"]["packages"]:
        if not (ROOT / package / "__init__.py").is_file():
            errors.append(f"Missing installed package: {package}")
    for name in ("KEYS.txt", "data_sources/massive/KEYS.txt", "data_sources/massive/KEYS.txt.bak",
                 ".env", ".env.local", "us_equity/.env.production", "qr.toml"):
        ignored = subprocess.run(["git", "check-ignore", "-q", name], cwd=ROOT)
        if ignored.returncode:
            errors.append(f"Private configuration path is not ignored by Git: {name}")
    for error in errors:
        print(error, file=sys.stderr)
    print("Repository checks:", ", ".join(f"{key}={value}" for key, value in counts.items()),
          f"path_keys={len(defaults)}, errors={len(errors)}")
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
