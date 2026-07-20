from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import stat


REQUIRED = (
    ".dockerignore",
    ".gitignore",
    "Makefile",
    "Dockerfile",
    "README.md",
    "VERSION",
    "container/entrypoint",
    "container/image.lock",
    "container/mosquitto.conf",
    "container/README.md",
    "docs/CONTAINER.md",
    "docs/CONTRACTS.md",
    "docs/OWNERS.md",
    "docs/WORKFLOW.md",
    "evidence/README.md",
    "evidence/templates/README.template.md",
    "evidence/templates/chapter-evidence.template.md",
    "evidence/templates/manifest.template.json",
    "evidence/templates/references.template.bib",
    "evidence/templates/code-or-patch/.gitkeep",
    "evidence/templates/figures/.gitkeep",
    "evidence/templates/raw/.gitkeep",
    "checkpoints/README.md",
    "fixtures/day1/faults.csv",
    "fixtures/day1/known_poses.csv",
    "fixtures/day1/manifest.json",
    "fixtures/day2/messages.csv",
    "fixtures/day2/topics.csv",
    "fixtures/day2/manifest.json",
    "fixtures/day3/clients.csv",
    "fixtures/day3/workloads.csv",
    "fixtures/day3/manifest.json",
    "fixtures/day4/faults.csv",
    "fixtures/day4/lock_orders.csv",
    "fixtures/day4/manifest.json",
    "fixtures/day5/controller_faults.csv",
    "fixtures/day5/estop_100.json",
    "fixtures/day5/release_fixtures.csv",
    "fixtures/day5/manifest.json",
    "include/course.h",
    "scripts/checkpoint.py",
    "scripts/course_paths.py",
    "scripts/fault_demo.py",
    "scripts/linux_preflight.py",
    "scripts/list_todos.py",
    "scripts/replay.py",
    "scripts/run_check.py",
    "scripts/run_gate.py",
    "scripts/starter_audit.py",
    "scripts/validate_selection.py",
    "scripts/with_broker.py",
    "src/support/course_support.c",
    "src/student/g1_state.c",
    "src/student/g2_transport.c",
    "src/student/g3_runtime.c",
    "src/student/g4_safety.c",
    "src/student/g5_gatekeeper.c",
    "tests/test_harness.c",
    "tests/test_harness.h",
    "tests/verify_main.c",
    "tests/smoke.c",
    "tests/day1.c",
    "tests/day2.c",
    "tests/day3.c",
    "tests/day4.c",
    "tests/day5.c",
)

TEXT_SUFFIXES = {
    "",
    ".c",
    ".h",
    ".py",
    ".md",
    ".txt",
    ".json",
    ".jsonl",
    ".csv",
    ".conf",
    ".lock",
    ".bib",
}

ALLOWED_EXTENSIONLESS_EXECUTABLES = {"container/entrypoint"}
BINARY_MAGICS = (
    b"\x7fELF",
    b"\xfe\xed\xfa\xce",
    b"\xce\xfa\xed\xfe",
    b"\xfe\xed\xfa\xcf",
    b"\xcf\xfa\xed\xfe",
)
IMAGE_ID_PATTERNS = {
    "arm64": re.compile(r"^COURSE_IMAGE_ARM64_ID=sha256:[0-9a-f]{64}$"),
    "amd64": re.compile(r"^COURSE_IMAGE_AMD64_ID=sha256:[0-9a-f]{64}$"),
}

RUST_OR_CARGO_BASENAMES = {
    "cargo.toml",
    "cargo.lock",
    "clippy.toml",
    "rustfmt.toml",
    "rust-toolchain",
    "rust-toolchain.toml",
}
RUST_OR_CARGO_DIRECTORIES = {".cargo", ".rustup", "target"}


def is_regular_file(path: Path) -> bool:
    try:
        return stat.S_ISREG(os.lstat(path).st_mode)
    except OSError:
        return False


def is_rust_or_cargo_path(relative: Path) -> bool:
    lowered = tuple(part.lower() for part in relative.parts)
    basename = lowered[-1] if lowered else ""
    return (
        relative.suffix.lower() == ".rs"
        or basename in RUST_OR_CARGO_BASENAMES
        or basename.startswith("rust-toolchain.")
        or any(part in RUST_OR_CARGO_DIRECTORIES for part in lowered)
    )


def main() -> int:
    parser = argparse.ArgumentParser(description="Audit the public student archive")
    parser.add_argument("--root", required=True)
    args = parser.parse_args()
    root = Path(args.root).resolve()
    errors: list[str] = []
    banned_token = "".join(("i", "g", "i"))

    for relative in REQUIRED:
        if not is_regular_file(root / relative):
            errors.append(f"missing required file: {relative}")

    for generated_name in ("build", "out"):
        generated = root / generated_name
        if generated.is_symlink():
            errors.append(f"generated path may not be a link: {generated_name}")
            continue
        if generated.exists():
            for path in sorted(generated.rglob("*")):
                info = os.lstat(path)
                if not stat.S_ISDIR(info.st_mode):
                    errors.append(
                        f"generated output must be removed before release: {path.relative_to(root)}"
                    )

    evidence_root = root / "evidence"
    if evidence_root.is_dir():
        for path in sorted(evidence_root.iterdir()):
            if path.name not in {"README.md", "templates"}:
                errors.append(
                    "generated evidence must be removed before release: "
                    f"{path.relative_to(root)}"
                )

    skipped = {"build", "out"}
    for path in sorted(root.rglob("*")):
        relative = path.relative_to(root)
        if any(part in skipped for part in relative.parts):
            continue
        info = os.lstat(path)
        lowered_parts = [part.lower() for part in relative.parts]
        if ".git" in lowered_parts:
            errors.append(f"repository history is not allowed: {relative}")
            continue
        if "__pycache__" in lowered_parts or path.suffix.lower() == ".pyc":
            errors.append(f"Python cache is not allowed: {relative}")
            continue
        if is_rust_or_cargo_path(relative):
            errors.append(f"Rust/Cargo material is not allowed: {relative}")
            continue
        if stat.S_ISLNK(info.st_mode):
            errors.append(f"links are not allowed in the student archive: {relative}")
            continue
        if banned_token in str(relative).lower():
            errors.append(f"banned course token in path: {relative}")
        if any(word in part for part in lowered_parts for word in ("solution", "answer", "instructor")):
            errors.append(f"non-student material in path: {relative}")
        if stat.S_ISREG(info.st_mode) and path.suffix.lower() in {
            ".o",
            ".a",
            ".so",
            ".dylib",
            ".dll",
            ".exe",
        }:
            errors.append(f"prebuilt code is not allowed: {relative}")
        if stat.S_ISREG(info.st_mode) and path.suffix == "":
            if info.st_mode & 0o111 and str(relative) not in ALLOWED_EXTENSIONLESS_EXECUTABLES:
                errors.append(f"extensionless executable is not allowed: {relative}")
            with path.open("rb") as source:
                magic = source.read(4)
            if magic in BINARY_MAGICS:
                errors.append(f"extensionless binary is not allowed: {relative}")
        if not stat.S_ISREG(info.st_mode) or path.suffix.lower() not in TEXT_SUFFIXES:
            continue
        text = path.read_text(encoding="utf-8", errors="ignore")
        if banned_token in text.lower():
            errors.append(f"banned course token in text: {relative}")

    docker_path = root / "Dockerfile"
    if is_regular_file(docker_path):
        docker_text = docker_path.read_text(encoding="utf-8")
        if "FROM ubuntu:24.04" not in docker_text:
            errors.append("Dockerfile must use the frozen Ubuntu 24.04 base")
        if "USER student" not in docker_text:
            errors.append("Dockerfile must switch to the student account")
        if any(word in docker_text.lower() for word in ("cargo", "rustc")):
            errors.append("Dockerfile contains a second compiler toolchain")

    lock_path = root / "container/image.lock"
    if is_regular_file(lock_path):
        lock_text = lock_path.read_text(encoding="utf-8")
        if "BASE_IMAGE=ubuntu:24.04@sha256:" not in lock_text:
            errors.append("image lock does not pin the Ubuntu base by hash")
        lock_lines = [line.strip() for line in lock_text.splitlines() if line.strip()]
        arm_lines = [
            line for line in lock_lines if line.startswith("COURSE_IMAGE_ARM64_ID=")
        ]
        amd_lines = [
            line for line in lock_lines if line.startswith("COURSE_IMAGE_AMD64_ID=")
        ]
        single_lines = [
            line for line in lock_lines if line.startswith("COURSE_IMAGE_ID=")
        ]
        if len(arm_lines) != 1 or IMAGE_ID_PATTERNS["arm64"].fullmatch(
            arm_lines[0]
        ) is None:
            errors.append("image lock does not contain one verified arm64 image ID")
        if len(amd_lines) != 1 or IMAGE_ID_PATTERNS["amd64"].fullmatch(
            amd_lines[0]
        ) is None:
            errors.append("image lock does not contain one verified amd64 image ID")
        if single_lines:
            errors.append("image lock may not use one image ID for two platforms")
        if lock_lines.count("PLATFORMS=linux/arm64,linux/amd64") != 1:
            errors.append("image lock platforms must be exactly linux/arm64,linux/amd64")
        if any(
            line.startswith("PLATFORMS=")
            and line != "PLATFORMS=linux/arm64,linux/amd64"
            for line in lock_lines
        ):
            errors.append("image lock contains a conflicting platform set")
        if any(
            word in lock_text.upper()
            for word in ("UNSET", "PLACEHOLDER", "SET_AFTER")
        ):
            errors.append("image lock still contains a placeholder")

    broker_path = root / "container/mosquitto.conf"
    if is_regular_file(broker_path):
        broker_text = broker_path.read_text(encoding="utf-8")
        listeners = [
            line.strip()
            for line in broker_text.splitlines()
            if line.strip().startswith("listener")
        ]
        if listeners != ["listener 18883 127.0.0.1"]:
            errors.append("broker must expose exactly one loopback listener")

    archives = list((root / "checkpoints").glob("*.tar.gz"))
    if archives:
        errors.append("student release contains a preloaded checkpoint archive")

    if errors:
        for error in errors:
            print(f"AUDIT_FAIL {error}")
        print(f"starter audit failed: {len(errors)} issue(s)")
        return 1
    print("starter audit green: C-only student archive, no preloaded checkpoint")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
