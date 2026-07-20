from __future__ import annotations

import argparse
from pathlib import Path
import re

from course_paths import parse_day, parse_group


TODO_PATTERN = re.compile(r"DAY([1-5])(?:_G([1-5]))?_TODO[^\n]*")


def main() -> int:
    parser = argparse.ArgumentParser(description="List bounded student TODOs")
    parser.add_argument("--root", required=True)
    parser.add_argument("--day")
    parser.add_argument("--team")
    args = parser.parse_args()
    day_filter = parse_day(args.day) if args.day else None
    group_filter = parse_group(args.team) if args.team else None

    count = 0
    for path in sorted(Path(args.root).rglob("*.c")):
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            for match in TODO_PATTERN.finditer(line):
                day = int(match.group(1))
                group = f"G{match.group(2)}" if match.group(2) else None
                if day_filter is not None and day != day_filter:
                    continue
                if group_filter is not None and group not in (None, group_filter):
                    continue
                print(f"{path}:{number}: {match.group(0)}")
                count += 1
    print(f"TODO count: {count}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
