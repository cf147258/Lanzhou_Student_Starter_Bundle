from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path


def row_key(row: dict[str, object]) -> tuple[str, str]:
    trace = str(row.get("trace_id", ""))
    seq = str(row.get("seq", row.get("command_seq", "")))
    return trace, seq


def verdict_pair(row: dict[str, object]) -> tuple[str, str]:
    verdict = str(row.get("verdict", ""))
    reason = str(row.get("reason", ""))
    replay_verdict = str(row.get("replay_verdict", verdict))
    replay_reason = str(row.get("replay_reason", reason))
    if not verdict or not reason:
        raise ValueError("each row needs verdict and reason")
    if (verdict, reason) != (replay_verdict, replay_reason):
        raise ValueError(
            f"replay mismatch for trace={row_key(row)}: "
            f"{verdict}/{reason} != {replay_verdict}/{replay_reason}"
        )
    return verdict, reason


def load(path: Path) -> list[dict[str, object]]:
    if path.suffix == ".jsonl":
        rows = []
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            if line.strip():
                value = json.loads(line)
                if not isinstance(value, dict):
                    raise ValueError(f"line {number} is not an object")
                rows.append(value)
        return rows
    if path.suffix == ".csv":
        with path.open("r", encoding="utf-8", newline="") as source:
            return list(csv.DictReader(source))
    raise ValueError("TRACE must be a .jsonl or .csv file")


def main() -> int:
    parser = argparse.ArgumentParser(description="Check recorded and replayed verdicts")
    parser.add_argument("--trace", required=True)
    args = parser.parse_args()
    path = Path(args.trace)
    if not path.is_file():
        parser.error(f"trace not found: {path}")
    try:
        rows = load(path)
        if not rows:
            raise ValueError("trace has no rows")
        stable: dict[tuple[str, str], tuple[str, str]] = {}
        for row in rows:
            pair = verdict_pair(row)
            key = row_key(row)
            if key in stable and stable[key] != pair:
                raise ValueError(f"unstable verdict for trace={key}")
            stable[key] = pair
    except (json.JSONDecodeError, OSError, ValueError) as error:
        parser.error(str(error))
    print(f"replay green: {len(rows)} row(s), {len(stable)} stable trace key(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
