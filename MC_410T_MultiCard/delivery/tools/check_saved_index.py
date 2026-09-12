#!/usr/bin/env python3
"""Read-only checker for FileSaver A/B data and index commit sidecars.

The checker never repairs or rewrites user files. It accepts an index sidecar,
an A-channel DAT file, and optionally a B-channel DAT file; with only a
directory it discovers ``*.index.jsonl`` sidecars and their sibling DAT files.
"""
from __future__ import annotations

import argparse
import json
import pathlib
import sys
from collections import defaultdict


def as_int(value, name):
    try:
        return int(value)
    except (TypeError, ValueError):
        raise ValueError(f"{name} is not an integer: {value!r}")


def check_index(index_path: pathlib.Path, a_path: pathlib.Path | None = None,
               b_path: pathlib.Path | None = None) -> dict:
    errors: list[str] = []
    warnings: list[str] = []
    rows_by_batch: dict[str, list[dict]] = defaultdict(list)
    commits: dict[str, dict] = {}
    seen_identity: set[tuple] = set()
    data_rows = 0

    try:
        lines = index_path.read_text(encoding="utf-8").splitlines()
    except OSError as exc:
        return {"index": str(index_path), "ok": False, "errors": [str(exc)], "warnings": []}
    for line_no, line in enumerate(lines, 1):
        if not line.strip():
            continue
        try:
            item = json.loads(line)
        except json.JSONDecodeError as exc:
            errors.append(f"line {line_no}: invalid JSON: {exc.msg}")
            continue
        kind = item.get("kind")
        batch = str(item.get("batchId", ""))
        if kind == "data":
            data_rows += 1
            if not batch:
                errors.append(f"line {line_no}: data row has no batchId")
            rows_by_batch[batch].append(item)
            try:
                identity = (str(item.get("session", "")), int(item.get("card", -1)),
                            str(item.get("wireTrigger", "")), str(item.get("expandedTrigger", "")))
                if identity in seen_identity:
                    errors.append(f"line {line_no}: duplicate trigger identity {identity}")
                seen_identity.add(identity)
                for field in ("aOffset", "aBytes", "bOffset", "bBytes", "sampleCount"):
                    as_int(item.get(field), field)
                if as_int(item["aBytes"], "aBytes") != as_int(item["bBytes"], "bBytes"):
                    errors.append(f"line {line_no}: A/B byte lengths differ")
            except (KeyError, ValueError) as exc:
                errors.append(f"line {line_no}: {exc}")
        elif kind == "commit":
            if not batch:
                errors.append(f"line {line_no}: commit has no batchId")
            elif batch in commits:
                errors.append(f"line {line_no}: duplicate commit for batch {batch}")
            commits[batch] = item
        else:
            errors.append(f"line {line_no}: unknown kind {kind!r}")

    for batch, rows in rows_by_batch.items():
        if batch not in commits:
            errors.append(f"batch {batch}: uncommitted data rows at index tail")
    for batch, commit in commits.items():
        rows = rows_by_batch.get(batch, [])
        expected = int(commit.get("rows", -1))
        if expected != len(rows):
            errors.append(f"batch {batch}: commit rows={expected}, data rows={len(rows)}")

    if a_path is None:
        a_path = index_path.with_name(index_path.name.replace(".index.jsonl", ".dat"))
    if b_path is None:
        stem = a_path.name
        b_name = stem.replace("_ChA_", "_ChB_")
        b_path = a_path.with_name(b_name)
    a_size = a_path.stat().st_size if a_path.exists() else None
    b_size = b_path.stat().st_size if b_path.exists() else None
    if a_size is None:
        errors.append(f"missing A file: {a_path}")
    if b_size is None:
        errors.append(f"missing B file: {b_path}")
    for batch, rows in rows_by_batch.items():
        if batch not in commits:
            continue
        for row in rows:
            a_offset, a_bytes = int(row["aOffset"]), int(row["aBytes"])
            b_offset, b_bytes = int(row["bOffset"]), int(row["bBytes"])
            if a_size is not None and a_offset + a_bytes > a_size:
                errors.append(f"batch {batch}: A range exceeds file")
            if b_size is not None and b_offset + b_bytes > b_size:
                errors.append(f"batch {batch}: B range exceeds file")

    expanded = []
    for rows in rows_by_batch.values():
        for row in rows:
            try:
                expanded.append((int(row.get("expandedTrigger", "")), row))
            except (TypeError, ValueError):
                pass
    expanded.sort(key=lambda pair: pair[0])
    for (previous, _), (current, _) in zip(expanded, expanded[1:]):
        if current > previous + 1:
            warnings.append(f"expanded trigger gap {previous}->{current}")

    return {
        "index": str(index_path),
        "aFile": str(a_path),
        "bFile": str(b_path),
        "rows": data_rows,
        "commits": len(commits),
        "ok": not errors,
        "errors": errors,
        "warnings": warnings,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Check FileSaver index.jsonl without modifying files")
    parser.add_argument("path", type=pathlib.Path, help="index.jsonl or directory containing sidecars")
    parser.add_argument("--a", type=pathlib.Path, help="A-channel DAT path")
    parser.add_argument("--b", type=pathlib.Path, help="B-channel DAT path")
    args = parser.parse_args()
    if args.path.is_dir():
        indexes = sorted(args.path.glob("*.index.jsonl"))
        if not indexes:
            print(json.dumps({"ok": False, "errors": [f"no index sidecars in {args.path}"]}, ensure_ascii=False))
            return 1
        results = [check_index(path) for path in indexes]
        result = {"ok": all(item["ok"] for item in results), "files": results}
    else:
        result = check_index(args.path, args.a, args.b)
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
