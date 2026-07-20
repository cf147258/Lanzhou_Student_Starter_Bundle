from __future__ import annotations

import argparse
import fcntl
import os
from pathlib import Path
import socket
import stat
import subprocess
import sys
import tempfile
import time


HOST = "127.0.0.1"
PORT = 18883


def secure_runtime_dir() -> Path:
    base = Path(tempfile.gettempdir())
    directory = base / f"course-broker-{os.geteuid()}"
    try:
        os.mkdir(directory, 0o700)
    except FileExistsError:
        pass
    info = os.lstat(directory)
    if stat.S_ISLNK(info.st_mode) or not stat.S_ISDIR(info.st_mode):
        raise RuntimeError(f"unsafe broker runtime directory: {directory}")
    if info.st_uid != os.geteuid():
        raise RuntimeError(f"broker runtime directory has the wrong owner: {directory}")
    os.chmod(directory, 0o700)
    return directory


def open_lock(directory: Path):
    path = directory / "port-18883.lock"
    flags = os.O_RDWR | os.O_CREAT | getattr(os, "O_CLOEXEC", 0)
    flags |= getattr(os, "O_NOFOLLOW", 0)
    descriptor = os.open(path, flags, 0o600)
    try:
        info = os.fstat(descriptor)
        if not stat.S_ISREG(info.st_mode) or info.st_uid != os.geteuid():
            raise RuntimeError(f"unsafe broker lock file: {path}")
        os.fchmod(descriptor, 0o600)
        return os.fdopen(descriptor, "a+", encoding="utf-8")
    except BaseException:
        os.close(descriptor)
        raise


def wait_ready(process: subprocess.Popen[bytes], timeout_seconds: float) -> bool:
    limit = time.monotonic() + timeout_seconds
    while time.monotonic() < limit:
        if process.poll() is not None:
            return False
        try:
            with socket.create_connection((HOST, PORT), timeout=0.1):
                return True
        except OSError:
            time.sleep(0.05)
    return False


def stop(process: subprocess.Popen[bytes]) -> None:
    if process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=2)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=2)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run one command with a private loopback MQTT broker"
    )
    parser.add_argument("--config", required=True)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command
    if command and command[0] == "--":
        command = command[1:]
    if not command:
        parser.error("a command is required after --")

    config = Path(os.path.abspath(args.config))
    try:
        config_info = os.lstat(config)
    except FileNotFoundError:
        parser.error(f"broker config not found: {config}")
    if stat.S_ISLNK(config_info.st_mode) or not stat.S_ISREG(config_info.st_mode):
        parser.error(f"broker config must be a regular file, not a link: {config}")

    try:
        runtime_dir = secure_runtime_dir()
        with open_lock(runtime_dir) as lock:
            fcntl.flock(lock.fileno(), fcntl.LOCK_EX)
            with tempfile.TemporaryFile(mode="w+b", dir=runtime_dir) as log:
                broker = subprocess.Popen(
                    ["mosquitto", "-c", str(config), "-v"],
                    stdout=log,
                    stderr=subprocess.STDOUT,
                )
                try:
                    if not wait_ready(broker, 5.0):
                        log.flush()
                        log.seek(0)
                        sys.stderr.write(log.read().decode("utf-8", errors="replace"))
                        print("broker failed to bind its loopback listener", file=sys.stderr)
                        return 2
                    env = os.environ.copy()
                    env["COURSE_MQTT_HOST"] = HOST
                    env["COURSE_MQTT_PORT"] = str(PORT)
                    result = subprocess.run(command, env=env, check=False)
                    return result.returncode
                finally:
                    stop(broker)
    except (OSError, RuntimeError) as error:
        parser.error(str(error))
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
