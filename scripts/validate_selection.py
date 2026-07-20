from __future__ import annotations

import argparse

from course_paths import parse_day, parse_group


def main() -> int:
    parser = argparse.ArgumentParser(description="Validate a course selection")
    parser.add_argument("--day", required=True)
    parser.add_argument("--group", required=True)
    args = parser.parse_args()
    try:
        day = parse_day(args.day)
        group = parse_group(args.group, allow_class=True)
    except ValueError as error:
        parser.error(str(error))
    print(f"selection: day={day} group={group}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
