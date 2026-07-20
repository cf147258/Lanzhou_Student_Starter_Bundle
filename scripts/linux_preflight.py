from __future__ import annotations

import argparse
import os
from pathlib import Path
import platform
import select
import shutil
import subprocess


REQUIRED_COMMANDS = (
    "gcc",
    "make",
    "pkg-config",
    "mosquitto",
    "mosquitto_pub",
    "mosquitto_sub",
    "socat",
    "python3",
)


def fail(message: str) -> int:
    print(f"preflight error: {message}")
    return 2


def main() -> int:
    parser = argparse.ArgumentParser(description="Check the Linux course environment")
    parser.add_argument("--platform-only", action="store_true")
    args = parser.parse_args()

    if platform.system() != "Linux":
        return fail("run this target in the supplied Linux container")
    if os.geteuid() == 0:
        return fail("the course process must not run as root")
    if args.platform_only:
        print("platform green: Linux, non-root")
        return 0

    missing = [name for name in REQUIRED_COMMANDS if shutil.which(name) is None]
    if missing:
        return fail("missing commands: " + ", ".join(missing))
    package = subprocess.run(
        ["pkg-config", "--exists", "libmosquitto"], check=False
    )
    if package.returncode != 0:
        return fail("pkg-config cannot find libmosquitto")
    if not hasattr(select, "epoll"):
        return fail("Python cannot see the Linux epoll interface")

    config = Path("container/mosquitto.conf").read_text(encoding="utf-8")
    if "listener 18883 127.0.0.1" not in config:
        return fail("broker listener is not fixed to loopback")
    print("preflight green: compiler, broker, C client library, PTY tool, Python")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
