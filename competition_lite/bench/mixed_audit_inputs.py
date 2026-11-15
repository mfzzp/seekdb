#!/usr/bin/env python3
"""Rehash retained benchmark payloads after all timing runs have finished."""
import argparse
import hashlib
import json
from pathlib import Path


def audit(root):
    datasets = []
    for rows in (200, 100000, 300000):
        directory = root / f"input-{rows}"
        prepared = json.loads((directory / "prepared.json").read_text())
        files = []
        for name, expected in prepared["sha256"].items():
            path = directory / name
            digest = hashlib.sha256()
            with path.open("rb") as stream:
                for chunk in iter(lambda: stream.read(4 * 1024 * 1024), b""):
                    digest.update(chunk)
            actual = digest.hexdigest()
            if actual != expected:
                raise RuntimeError(f"Input changed: {path}")
            files.append({"path": name, "bytes": path.stat().st_size,
                          "sha256": actual, "matches_prepared": True})
        datasets.append({"rows": rows, "files": files})
    return {"root": str(root), "all_match": True,
            "scope": "Post-run payload SHA256 matches pre-run prepared metadata",
            "datasets": datasets}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = audit(args.root)
    encoded = json.dumps(result, indent=2) + "\n"
    if args.output:
        args.output.write_text(encoded, encoding="utf-8", newline="\n")
    else:
        print(encoded, end="")
