from __future__ import annotations

import json
import os
import platform
from pathlib import Path
import subprocess
from typing import Any


DAY_TAGS = {
    1: ("starter_repo", "frame_v1"),
    2: ("frame_v1", "transport_v1"),
    3: ("transport_v1", "runtime_poll_v1"),
    4: ("runtime_poll_v1", "safety_v1"),
    5: ("safety_v1", "release_v1"),
}

BUNDLE_ROOT = Path(__file__).resolve().parent.parent
IMAGE_KEYS = {
    "arm64": "COURSE_IMAGE_ARM64_ID",
    "aarch64": "COURSE_IMAGE_ARM64_ID",
    "amd64": "COURSE_IMAGE_AMD64_ID",
    "x86_64": "COURSE_IMAGE_AMD64_ID",
}
EMPTY_METADATA = {"", "UNSET", "PLACEHOLDER", "SET_AFTER_BUILD", "NOT_RECORDED"}


def parse_day(value: str | int) -> int:
    try:
        day = int(value)
    except (TypeError, ValueError) as error:
        raise ValueError("DAY must be one of 1, 2, 3, 4, or 5") from error
    if day not in DAY_TAGS:
        raise ValueError("DAY must be one of 1, 2, 3, 4, or 5")
    return day


def parse_group(value: str, allow_class: bool = False) -> str:
    group = str(value).strip().upper()
    if group in {"1", "2", "3", "4", "5"}:
        group = f"G{group}"
    allowed = {"G1", "G2", "G3", "G4", "G5"}
    if allow_class:
        allowed.add("CLASS")
    if group not in allowed:
        suffix = " or CLASS" if allow_class else ""
        raise ValueError(f"GROUP/TEAM must be G1 through G5{suffix}")
    return group


def parse_seed(value: str | int) -> int:
    try:
        seed = int(value)
    except (TypeError, ValueError) as error:
        raise ValueError("SEED must be a non-negative integer") from error
    if seed < 0:
        raise ValueError("SEED must be a non-negative integer")
    return seed


def evidence_dir(root: Path, group: str, day: int) -> Path:
    return root / f"{group}_D{day}_evidence"


def write_if_missing(path: Path, text: str) -> None:
    if path.exists():
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def useful_environment_value(name: str) -> str | None:
    value = os.environ.get(name, "").strip()
    if value.upper() in EMPTY_METADATA:
        return None
    return value


def read_image_lock(bundle_root: Path = BUNDLE_ROOT) -> dict[str, str]:
    path = bundle_root / "container/image.lock"
    values: dict[str, str] = {}
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise ValueError(f"cannot read course image lock: {path}") from error
    for number, raw_line in enumerate(lines, start=1):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            raise ValueError(f"bad course image lock line {number}")
        key, value = line.split("=", 1)
        key = key.strip()
        value = value.strip()
        if not key or key in values:
            raise ValueError(f"duplicate or empty course image lock key on line {number}")
        values[key] = value
    return values


def valid_image_id(value: str) -> bool:
    payload = value.removeprefix("sha256:")
    return (
        value.startswith("sha256:")
        and len(payload) == 64
        and all(character in "0123456789abcdef" for character in payload)
    )


def locked_container_image(bundle_root: Path = BUNDLE_ROOT) -> str:
    values = read_image_lock(bundle_root)
    machine = platform.machine().strip().lower()
    key = IMAGE_KEYS.get(machine)
    if key is None:
        raise ValueError(
            f"unsupported host architecture {machine!r}; expected arm64/aarch64 or amd64/x86_64"
        )
    if values.get("PLATFORMS") != "linux/arm64,linux/amd64":
        raise ValueError("course image lock has the wrong platform set")
    image_id = values.get(key, "")
    if not valid_image_id(image_id):
        raise ValueError(f"course image lock has no valid {machine} image ID")
    return image_id


def resolve_container_image(bundle_root: Path = BUNDLE_ROOT) -> str:
    return useful_environment_value("COURSE_CONTAINER_IMAGE") or locked_container_image(
        bundle_root
    )


def resolve_course_commit(bundle_root: Path = BUNDLE_ROOT) -> str:
    supplied = useful_environment_value("COURSE_COMMIT")
    if supplied is not None:
        return supplied
    try:
        result = subprocess.run(
            ["git", "-C", str(bundle_root), "rev-parse", "--verify", "HEAD"],
            check=False,
            capture_output=True,
            text=True,
            timeout=3,
        )
    except (FileNotFoundError, OSError, subprocess.TimeoutExpired):
        return "NOT_A_GIT_CHECKOUT"
    commit = result.stdout.strip()
    if result.returncode == 0 and commit:
        return commit
    return "NOT_A_GIT_CHECKOUT"


def prepare_course_metadata(bundle_root: Path = BUNDLE_ROOT) -> tuple[str, str]:
    image_id = resolve_container_image(bundle_root)
    commit = resolve_course_commit(bundle_root)
    os.environ["COURSE_CONTAINER_IMAGE"] = image_id
    os.environ["COURSE_COMMIT"] = commit
    return image_id, commit


def init_evidence(root: Path, group: str, day: int, seed: int) -> Path:
    image_id, commit = prepare_course_metadata()
    target = evidence_dir(root, group, day)
    input_tag, output_tag = DAY_TAGS[day]
    target.mkdir(parents=True, exist_ok=True)
    for folder in ("code-or-patch", "raw", "figures"):
        (target / folder).mkdir(exist_ok=True)

    readme = (
        f"input_tag: {input_tag}\n\n"
        f"# {group} Day {day} Evidence\n\n"
        "## Goal\n\nDescribe the bounded change and the frozen interface.\n\n"
        "## Commands and result\n\nRecord every command and its pass/fail result.\n\n"
        "## Known issue and owner\n\nState one current limit and its owner.\n\n"
        f"output_tag: {output_tag}\n"
    )
    write_if_missing(target / "README.md", readme)
    chapter = (
        f"# {group} Day {day} Chapter Evidence\n\n"
        "## Claim or method bullets\n\n- TODO\n- TODO\n\n"
        "## Planned figure or table caption\n\nTODO\n"
    )
    write_if_missing(target / "chapter-A-evidence.md", chapter)
    write_if_missing(target / "chapter-B-evidence.md", chapter)
    write_if_missing(
        target / "references.bib",
        "% Add only sources read and used during this session.\n",
    )
    manifest: dict[str, Any] = {
        "group": group,
        "day": day,
        "seed": seed,
        "container_image": image_id,
        "commit": commit,
        "input_tag": input_tag,
        "output_tag": output_tag,
    }
    write_if_missing(
        target / "manifest.json",
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
    )
    return target
