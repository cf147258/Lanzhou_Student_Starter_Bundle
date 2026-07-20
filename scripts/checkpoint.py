from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import stat
import tarfile
import tempfile
import time

from course_paths import parse_day, parse_group, parse_seed


MANIFEST_NAME = "checkpoint-manifest.json"
MAX_MEMBER_BYTES = 128 * 1024 * 1024
STUDENT_MODULES = {
    "src/student/g1_state.c",
    "src/student/g2_transport.c",
    "src/student/g3_runtime.c",
    "src/student/g4_safety.c",
    "src/student/g5_gatekeeper.c",
}
STUDENT_GROUPS = {"G1", "G2", "G3", "G4", "G5"}


def absolute(path: Path) -> Path:
    return Path(os.path.abspath(os.fspath(path)))


def inside(base: Path, candidate: Path) -> bool:
    try:
        return os.path.commonpath((str(base), str(candidate))) == str(base)
    except ValueError:
        return False


def checked_root(path: Path) -> Path:
    root = absolute(path)
    info = os.lstat(root)
    if stat.S_ISLNK(info.st_mode) or not stat.S_ISDIR(info.st_mode):
        raise RuntimeError("bundle root must be a real directory, not a link")
    return root


def check_path(root: Path, target: Path, must_exist: bool = False) -> None:
    target = absolute(target)
    if not inside(root, target):
        raise RuntimeError(f"target escapes the bundle root: {target}")
    relative = target.relative_to(root)
    current = root
    missing = False
    parts = relative.parts
    for index, part in enumerate(parts):
        current = current / part
        try:
            info = os.lstat(current)
        except FileNotFoundError:
            missing = True
            continue
        if stat.S_ISLNK(info.st_mode):
            raise RuntimeError(f"linked path is not allowed: {current}")
        is_leaf = index == len(parts) - 1
        if not is_leaf and not stat.S_ISDIR(info.st_mode):
            raise RuntimeError(f"non-directory path component: {current}")
        if is_leaf and not stat.S_ISREG(info.st_mode) and not stat.S_ISDIR(info.st_mode):
            raise RuntimeError(f"special target is not allowed: {current}")
    if must_exist and missing:
        raise RuntimeError(f"required path does not exist: {target}")


def ensure_directory(root: Path, directory: Path) -> None:
    directory = absolute(directory)
    if not inside(root, directory):
        raise RuntimeError(f"directory escapes the bundle root: {directory}")
    current = root
    for part in directory.relative_to(root).parts:
        current = current / part
        try:
            info = os.lstat(current)
        except FileNotFoundError:
            try:
                os.mkdir(current, 0o775)
            except FileExistsError:
                pass
            info = os.lstat(current)
        if stat.S_ISLNK(info.st_mode) or not stat.S_ISDIR(info.st_mode):
            raise RuntimeError(f"unsafe directory component: {current}")


def file_hash(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while True:
            block = source.read(1024 * 1024)
            if not block:
                break
            digest.update(block)
    return digest.hexdigest()


def unique_archive_path(directory: Path, prefix: str) -> Path:
    for counter in range(1000):
        stamp = time.time_ns()
        candidate = directory / f"{prefix}_{stamp}_{os.getpid()}_{counter}.tar.gz"
        try:
            os.lstat(candidate)
        except FileNotFoundError:
            return candidate
    raise RuntimeError("could not allocate a unique checkpoint name")


def collect(root: Path, day: int, group: str) -> list[Path]:
    candidates = sorted((root / "src/student").glob("*.c"))
    if group == "CLASS":
        candidates.extend(sorted((root / "evidence").glob(f"G*_D{day}_evidence/**/*")))
    else:
        candidates.extend(
            sorted((root / "evidence" / f"{group}_D{day}_evidence").glob("**/*"))
        )
    files: list[Path] = []
    for path in candidates:
        info = os.lstat(path)
        if stat.S_ISLNK(info.st_mode):
            raise RuntimeError(f"checkpoint source may not be a link: {path}")
        if stat.S_ISREG(info.st_mode):
            check_path(root, path, must_exist=True)
            files.append(path)
    return files


def create(root: Path, day: int, group: str, seed: int) -> int:
    files = collect(root, day, group)
    student_paths = {
        path.relative_to(root).as_posix()
        for path in files
        if path.relative_to(root).as_posix().startswith("src/student/")
    }
    if student_paths != STUDENT_MODULES:
        raise RuntimeError("checkpoint requires exactly the five student source modules")
    output_dir = root / "checkpoints"
    ensure_directory(root, output_dir)
    output = unique_archive_path(output_dir, f"{group.lower()}_day{day}_green")
    records = []
    for path in files:
        relative = path.relative_to(root).as_posix()
        records.append({"path": relative, "sha256": file_hash(path)})
    manifest = {
        "format": 1,
        "course_version": (root / "VERSION").read_text(encoding="utf-8").strip(),
        "day": day,
        "group": group,
        "seed": seed,
        "created_utc": datetime.now(timezone.utc).isoformat(),
        "files": records,
    }
    encoded = (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode("utf-8")
    with tarfile.open(output, "x:gz") as archive:
        for path in files:
            check_path(root, path, must_exist=True)
            archive.add(path, arcname=path.relative_to(root).as_posix(), recursive=False)
        info = tarfile.TarInfo(MANIFEST_NAME)
        info.size = len(encoded)
        info.mode = 0o644
        info.mtime = 0
        archive.addfile(info, io.BytesIO(encoded))
    print(f"checkpoint created: {output}")
    print("This archive contains class work that already passed the selected gate.")
    return 0


def safe_member_shape(name: str) -> bool:
    path = PurePosixPath(name)
    if not name or "\\" in name or path.is_absolute() or ".." in path.parts:
        return False
    if name == MANIFEST_NAME:
        return True
    if name in STUDENT_MODULES:
        return True
    return len(path.parts) >= 3 and path.parts[0] == "evidence"


def member_matches_manifest(name: str, day: int, group: str) -> bool:
    if name in STUDENT_MODULES:
        return True
    path = PurePosixPath(name)
    if len(path.parts) < 3 or path.parts[0] != "evidence":
        return False
    if group == "CLASS":
        allowed_packs = {
            f"{student_group}_D{day}_evidence"
            for student_group in STUDENT_GROUPS
        }
    else:
        allowed_packs = {f"{group}_D{day}_evidence"}
    return path.parts[1] in allowed_packs


def validate_manifest(manifest: object, root: Path) -> dict[str, str]:
    if (
        not isinstance(manifest, dict)
        or type(manifest.get("format")) is not int
        or manifest.get("format") != 1
    ):
        raise RuntimeError("unsupported checkpoint manifest")

    course_version_path = root / "VERSION"
    check_path(root, course_version_path, must_exist=True)
    course_version = course_version_path.read_text(encoding="utf-8").strip()
    if not course_version or manifest.get("course_version") != course_version:
        raise RuntimeError("checkpoint course version does not match this bundle")

    day = manifest.get("day")
    group = manifest.get("group")
    seed = manifest.get("seed")
    if type(day) is not int or day not in {1, 2, 3, 4, 5}:
        raise RuntimeError("checkpoint manifest has an invalid day")
    if not isinstance(group, str) or group not in STUDENT_GROUPS | {"CLASS"}:
        raise RuntimeError("checkpoint manifest has an invalid group")
    if type(seed) is not int or seed < 0:
        raise RuntimeError("checkpoint manifest has an invalid seed")

    created_utc = manifest.get("created_utc")
    try:
        created = datetime.fromisoformat(created_utc)
    except (TypeError, ValueError) as error:
        raise RuntimeError("checkpoint manifest has an invalid creation time") from error
    offset = created.utcoffset()
    if offset is None or offset.total_seconds() != 0:
        raise RuntimeError("checkpoint creation time must be UTC")

    entries = manifest.get("files")
    if not isinstance(entries, list):
        raise RuntimeError("checkpoint manifest has no file list")
    expected: dict[str, str] = {}
    for entry in entries:
        if not isinstance(entry, dict):
            raise RuntimeError("checkpoint manifest has an invalid file entry")
        name = entry.get("path")
        checksum = entry.get("sha256")
        if (
            not isinstance(name, str)
            or not safe_member_shape(name)
            or name == MANIFEST_NAME
            or not member_matches_manifest(name, day, group)
        ):
            raise RuntimeError("checkpoint manifest has an unsafe file path")
        if name in expected:
            raise RuntimeError(f"duplicate manifest path: {name}")
        if (
            not isinstance(checksum, str)
            or len(checksum) != 64
            or any(character not in "0123456789abcdefABCDEF" for character in checksum)
        ):
            raise RuntimeError(f"invalid hash for {name}")
        expected[name] = checksum.lower()
    source_paths = {
        name for name in expected if name.startswith("src/student/")
    }
    if source_paths != STUDENT_MODULES:
        raise RuntimeError("checkpoint must contain exactly the five student modules")
    return expected


def backup_sources(root: Path) -> Path:
    backup_dir = root / "checkpoints/backups"
    ensure_directory(root, backup_dir)
    output = unique_archive_path(backup_dir, "before_restore")
    with tarfile.open(output, "x:gz") as archive:
        for relative in sorted(STUDENT_MODULES):
            path = root / relative
            check_path(root, path, must_exist=True)
            info = os.lstat(path)
            if not stat.S_ISREG(info.st_mode):
                raise RuntimeError(f"student module is not a regular file: {path}")
            archive.add(path, arcname=relative, recursive=False)
    return output


def stage_member(
    root: Path, archive: tarfile.TarFile, member: tarfile.TarInfo, checksum: str
) -> tuple[Path, Path]:
    target = absolute(root / PurePosixPath(member.name))
    ensure_directory(root, target.parent)
    check_path(root, target)
    if target.exists() and not stat.S_ISREG(os.lstat(target).st_mode):
        raise RuntimeError(f"restore target is not a regular file: {target}")
    stream = archive.extractfile(member)
    if stream is None:
        raise RuntimeError(f"cannot read {member.name}")
    fd, temp_name = tempfile.mkstemp(prefix=".course-restore-", dir=target.parent)
    temp_path = Path(temp_name)
    digest = hashlib.sha256()
    try:
        with os.fdopen(fd, "wb") as output:
            while True:
                block = stream.read(1024 * 1024)
                if not block:
                    break
                digest.update(block)
                output.write(block)
            output.flush()
            os.fsync(output.fileno())
        if digest.hexdigest() != checksum:
            raise RuntimeError(f"hash mismatch for {member.name}")
        os.chmod(temp_path, 0o644)
        return temp_path, target
    except BaseException:
        try:
            temp_path.unlink()
        except FileNotFoundError:
            pass
        raise


def atomic_replace(root: Path, temp_path: Path, target: Path) -> None:
    check_path(root, target)
    temp_info = os.lstat(temp_path)
    if stat.S_ISLNK(temp_info.st_mode) or not stat.S_ISREG(temp_info.st_mode):
        raise RuntimeError(f"unsafe staged file: {temp_path}")
    if temp_path.parent != target.parent:
        raise RuntimeError("staged file is not in the target directory")
    os.replace(temp_path, target)
    directory_fd = os.open(target.parent, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
    try:
        os.fsync(directory_fd)
    finally:
        os.close(directory_fd)


def restore(root: Path, archive_path: Path) -> int:
    raw_archive = archive_path if archive_path.is_absolute() else root / archive_path
    archive_path = absolute(raw_archive)
    checkpoint_root = root / "checkpoints"
    check_path(root, checkpoint_root, must_exist=True)
    if not inside(checkpoint_root, archive_path):
        raise RuntimeError("checkpoint must be inside the checkpoints directory")
    check_path(root, archive_path, must_exist=True)
    if not stat.S_ISREG(os.lstat(archive_path).st_mode):
        raise RuntimeError("checkpoint must be a regular file")

    staged: list[tuple[Path, Path]] = []
    try:
        with tarfile.open(archive_path, "r:gz") as archive:
            members = archive.getmembers()
            names = [member.name for member in members]
            if not members or len(names) != len(set(names)):
                raise RuntimeError("checkpoint is empty or contains duplicate paths")
            if any(not safe_member_shape(member.name) for member in members):
                raise RuntimeError("checkpoint contains an unsafe path")
            if any(not member.isreg() or member.size > MAX_MEMBER_BYTES for member in members):
                raise RuntimeError("checkpoint may contain only bounded regular files")
            manifest_members = [member for member in members if member.name == MANIFEST_NAME]
            if len(manifest_members) != 1:
                raise RuntimeError("checkpoint must contain exactly one manifest")
            manifest_stream = archive.extractfile(manifest_members[0])
            if manifest_stream is None:
                raise RuntimeError("checkpoint manifest cannot be read")
            expected = validate_manifest(json.load(manifest_stream), root)
            data_members = [member for member in members if member.name != MANIFEST_NAME]
            if {member.name for member in data_members} != set(expected):
                raise RuntimeError("archive members do not match the checkpoint manifest")
            for member in data_members:
                staged.append(stage_member(root, archive, member, expected[member.name]))

        backup = backup_sources(root)
        for temp_path, target in staged:
            atomic_replace(root, temp_path, target)
        staged.clear()
    finally:
        for temp_path, _ in staged:
            try:
                temp_path.unlink()
            except FileNotFoundError:
                pass
    print(f"restore complete; pre-restore backup: {backup}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Create or restore a green class checkpoint")
    subparsers = parser.add_subparsers(dest="action", required=True)
    create_parser = subparsers.add_parser("create")
    create_parser.add_argument("--root", required=True)
    create_parser.add_argument("--day", required=True)
    create_parser.add_argument("--group", required=True)
    create_parser.add_argument("--seed", required=True)
    restore_parser = subparsers.add_parser("restore")
    restore_parser.add_argument("--root", required=True)
    restore_parser.add_argument("--archive", required=True)
    args = parser.parse_args()
    try:
        root = checked_root(Path(args.root))
        if args.action == "create":
            return create(
                root,
                parse_day(args.day),
                parse_group(args.group, allow_class=True),
                parse_seed(args.seed),
            )
        return restore(root, Path(args.archive))
    except (KeyError, OSError, RuntimeError, tarfile.TarError, ValueError) as error:
        parser.error(str(error))
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
