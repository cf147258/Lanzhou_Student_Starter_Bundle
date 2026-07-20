from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys

from course_paths import parse_day, parse_seed


def main() -> int:
    parser = argparse.ArgumentParser(description="Run all five group checks for one day")
    parser.add_argument("--binary", required=True)
    parser.add_argument("--day", required=True)
    parser.add_argument("--seed", required=True)
    parser.add_argument("--evidence-root", required=True)
    args = parser.parse_args()
    try:
        day = parse_day(args.day)
        seed = parse_seed(args.seed)
    except ValueError as error:
        parser.error(str(error))

    runner = Path(__file__).with_name("run_check.py")
    failures: list[str] = []
    for number in range(1, 6):
        group = f"G{number}"
        result = subprocess.run(
            [
                sys.executable,
                str(runner),
                "--binary",
                args.binary,
                "--day",
                str(day),
                "--team",
                group,
                "--seed",
                str(seed),
                "--evidence-root",
                args.evidence_root,
            ],
            check=False,
        )
        if result.returncode != 0:
            failures.append(group)
    if failures:
        print(f"day {day} class gate red: {', '.join(failures)}")
        return 1
    print(f"day {day} class gate green")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
