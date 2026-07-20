from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess

from course_paths import (
    init_evidence,
    parse_day,
    parse_group,
    parse_seed,
    prepare_course_metadata,
)


def main() -> int:
    parser = argparse.ArgumentParser(description="Run one group/day public gate")
    parser.add_argument("--binary", required=True)
    parser.add_argument("--day", required=True)
    parser.add_argument("--team", required=True)
    parser.add_argument("--seed", required=True)
    parser.add_argument("--evidence-root", required=True)
    args = parser.parse_args()
    try:
        day = parse_day(args.day)
        group = parse_group(args.team)
        seed = parse_seed(args.seed)
    except ValueError as error:
        parser.error(str(error))

    binary = Path(args.binary)
    if not binary.is_file():
        parser.error(f"verification binary not found: {binary}")
    root = Path(args.evidence_root)
    try:
        image_id, commit = prepare_course_metadata()
        target = init_evidence(root, group, day, seed)
    except ValueError as error:
        parser.error(str(error))
    env = os.environ.copy()
    env["COURSE_CONTAINER_IMAGE"] = image_id
    env["COURSE_COMMIT"] = commit
    env["COURSE_SEED"] = str(seed)
    env["COURSE_EVIDENCE_DIR"] = str(target)
    env["COURSE_GROUP"] = group
    env["COURSE_DAY"] = str(day)
    result = subprocess.run(
        [str(binary), f"day{day}", group], env=env, check=False
    )
    return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
