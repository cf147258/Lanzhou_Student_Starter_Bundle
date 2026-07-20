from __future__ import annotations

import argparse
from collections import deque
import csv
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import select
import shutil
import struct
import subprocess
import sys
import threading
import time
from typing import Any

from course_paths import init_evidence, parse_day, parse_group, parse_seed


FIXED_SEED = 20260719
FRAME_LENGTH = 56
CRC_OFFSET = 54
JOINT_LIMIT_RAD = math.pi
ALLOWED_CASES = {
    2: {"delayed-old"},
    3: {"blocking-slow-client"},
    4: {"check-write-race"},
    5: {"fifo-estop"},
}
GATE_ARTIFACTS = {
    1: "corruption-results.csv",
    2: "qos-freshness.csv",
    3: "blocking-vs-poll.csv",
    4: "raw/fault_verdicts.csv",
    5: "raw/estop_100.csv",
}


class DemoError(RuntimeError):
    """A reproducibility or safety precondition was not met."""


def write_json(path: Path, payload: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


def sha256_bytes(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(65536), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_csv(path: Path) -> list[dict[str, str]]:
    if not path.is_file():
        raise DemoError(f"fixture not found: {path}")
    with path.open("r", encoding="utf-8", newline="") as source:
        rows = list(csv.DictReader(source))
    if not rows:
        raise DemoError(f"fixture is empty: {path}")
    if any(value is None for row in rows for value in row.values()):
        raise DemoError(f"fixture has a malformed row: {path}")
    return rows


def read_json(path: Path) -> dict[str, Any]:
    if not path.is_file():
        raise DemoError(f"fixture not found: {path}")
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise DemoError(f"cannot read fixture {path}: {error}") from error
    if not isinstance(payload, dict):
        raise DemoError(f"fixture root must be an object: {path}")
    return payload


def fixture_seed(value: object, path: Path, seed: int) -> None:
    try:
        actual = int(value)
    except (TypeError, ValueError) as error:
        raise DemoError(f"fixture seed is invalid: {path}") from error
    if actual != seed or actual != FIXED_SEED:
        raise DemoError(
            f"fixture seed mismatch in {path}: expected {FIXED_SEED}, got {actual}"
        )


def run_behavior_gate(
    binary: Path, day: int, group: str, seed: int, target: Path
) -> dict[str, Any]:
    gate_scope = "ALL"
    gate_dir = target / "raw/behavior-gate"
    gate_dir.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env["COURSE_SEED"] = str(seed)
    env["COURSE_DAY"] = str(day)
    env["COURSE_GROUP"] = gate_scope
    env["COURSE_EVIDENCE_DIR"] = str(gate_dir)
    command = [str(binary), f"day{day}", gate_scope]
    try:
        result = subprocess.run(
            command,
            env=env,
            check=False,
            capture_output=True,
            text=True,
            timeout=30,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise DemoError(f"C behavior gate could not complete: {error}") from error

    stdout_path = gate_dir / "gate.stdout"
    stderr_path = gate_dir / "gate.stderr"
    stdout_path.write_text(result.stdout, encoding="utf-8")
    stderr_path.write_text(result.stderr, encoding="utf-8")
    artifact = gate_dir / GATE_ARTIFACTS[day]
    summary = f"SUMMARY day{day}.{gate_scope}"
    executed = (
        result.returncode in (0, 1)
        and summary in result.stdout
        and artifact.is_file()
        and artifact.stat().st_size > 0
    )
    report = {
        "artifact": str(artifact),
        "artifact_sha256": sha256_file(artifact) if artifact.is_file() else None,
        "binary": str(binary),
        "command": command,
        "executed": executed,
        "exit_code": result.returncode,
        "gate_status": "GREEN" if result.returncode == 0 else "RED",
        "requested_evidence_group": group,
        "scope": gate_scope,
        "stderr": str(stderr_path),
        "stdout": str(stdout_path),
    }
    write_json(gate_dir / "gate-run.json", report)
    if not executed:
        raise DemoError(
            "C behavior gate did not produce its summary and day evidence; "
            f"see {stdout_path} and {stderr_path}"
        )
    return report


def crc16_ccitt_false(payload: bytes) -> int:
    crc = 0xFFFF
    for byte in payload:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def encode_frame(
    sequence: int, source_ns: int, trace_id: int, targets_rad: list[float]
) -> bytes:
    if len(targets_rad) != 3:
        raise DemoError("the frozen frame requires exactly three joints")
    without_crc = struct.pack(
        "<BBBBHQQQddd",
        0xA5,
        0x5A,
        1,
        1,
        24,
        sequence,
        source_ns,
        trace_id,
        *targets_rad,
    )
    if len(without_crc) != CRC_OFFSET:
        raise DemoError("internal frame layout does not match the frozen contract")
    return without_crc + struct.pack("<H", crc16_ccitt_false(without_crc[2:]))


def frame_crc_valid(frame: bytes) -> bool:
    if len(frame) != FRAME_LENGTH:
        return False
    stored = struct.unpack("<H", frame[CRC_OFFSET:FRAME_LENGTH])[0]
    return stored == crc16_ccitt_false(frame[2:CRC_OFFSET])


def day1(
    target: Path,
    fixture_root: Path,
    case: str,
    seed: int,
    binary: Path,
    group: str,
) -> list[Path]:
    manifest_path = fixture_root / "day1/manifest.json"
    faults_path = fixture_root / "day1/faults.csv"
    poses_path = fixture_root / "day1/known_poses.csv"
    manifest = read_json(manifest_path)
    fixture_seed(manifest.get("seed"), manifest_path, seed)
    if manifest.get("simulator_only") is not True or manifest.get("arm_dof") != 3:
        raise DemoError("Day 1 manifest violates the simulator-only 3-DOF contract")

    matching = [row for row in read_csv(faults_path) if row["case"] == case]
    if len(matching) != 1:
        choices = sorted(row["case"] for row in read_csv(faults_path))
        raise DemoError(f"case {case!r} must match one Day 1 fixture; choices: {choices}")
    if case not in manifest.get("faults", []):
        raise DemoError(f"Day 1 manifest does not declare case {case!r}")
    fault = matching[0]
    fixture_seed(fault["seed"], faults_path, seed)
    poses = read_csv(poses_path)
    pose = next((row for row in poses if row["case"] == "reach_right"), poses[-1])
    if pose["frame_id"] != "1":
        raise DemoError("Day 1 pose uses an unexpected frame id")
    source_targets = [float(pose[f"q{joint}_rad"]) for joint in range(3)]
    encoded_targets = (
        [math.radians(value) for value in source_targets]
        if case == "unit-plus-bitflip"
        else source_targets
    )
    sequence = int(pose["seq"])
    source_ns = int(pose["t_mono_ns"])
    correct_frame = encode_frame(sequence, source_ns, seed, source_targets)
    frame = encode_frame(sequence, source_ns, seed, encoded_targets)

    offset = int(fault["byte_offset"])
    mask = int(fault["mask"])
    truncate_to = int(fault["truncate_to"])
    order = fault["delivery_order"]
    if not 0 <= truncate_to <= FRAME_LENGTH:
        raise DemoError("Day 1 truncation length is outside the frozen frame")
    if not 0 <= offset < FRAME_LENGTH or not 0 <= mask <= 0xFF:
        raise DemoError("Day 1 mutation offset or mask is invalid")

    before_stream = correct_frame
    after_stream = frame
    applied: list[str] = []
    observed_verdict = "ACCEPT"
    if case in {"bitflip", "unit-plus-bitflip"}:
        changed = bytearray(after_stream)
        changed[offset] ^= mask
        after_stream = bytes(changed)
        applied.append(f"xor byte {offset} with 0x{mask:02x}")
    if truncate_to < FRAME_LENGTH:
        after_stream = after_stream[:truncate_to]
        applied.append(f"truncate to {truncate_to} bytes")
    if order == "42-before-41":
        first = encode_frame(41, source_ns, seed + 41, encoded_targets)
        second = encode_frame(42, source_ns + 1, seed + 42, encoded_targets)
        before_stream = first + second
        after_stream = second + first
        applied.append("deliver sequence 42 before sequence 41")
        observed_verdict = "NACK_SEQUENCE"
    elif order != "in-order":
        raise DemoError(f"unknown Day 1 delivery order: {order}")

    if order == "in-order":
        if len(after_stream) != FRAME_LENGTH:
            observed_verdict = "NACK_LENGTH"
        elif not frame_crc_valid(after_stream):
            observed_verdict = "NACK_CRC"
    expected_verdict = fault["expected_verdict"]
    expected_writes = int(fault["expected_actuator_writes"])
    reproduced = (
        before_stream != after_stream
        and observed_verdict == expected_verdict
        and expected_writes == 0
    )

    before_path = target / "raw/day1-before.bin"
    after_path = target / "raw/day1-after.bin"
    before_path.write_bytes(before_stream)
    after_path.write_bytes(after_stream)
    result_path = target / "raw/day1-wire-mutation.json"
    result = {
        "applied_mutations": applied,
        "before_bytes": len(before_stream),
        "before_crc_valid": frame_crc_valid(correct_frame),
        "before_sha256": sha256_bytes(before_stream),
        "case": case,
        "encoded_targets_rad": encoded_targets,
        "expected_actuator_writes": expected_writes,
        "expected_verdict": expected_verdict,
        "fixture": str(faults_path),
        "fixture_mutation": {
            "byte_offset": offset,
            "delivery_order": order,
            "mask": mask,
            "truncate_to": truncate_to,
        },
        "fixture_sha256": sha256_file(faults_path),
        "observed_bytes": len(after_stream),
        "observed_crc_valid": frame_crc_valid(after_stream),
        "observed_sha256": sha256_bytes(after_stream),
        "observed_verdict": observed_verdict,
        "reproduced": reproduced,
        "seed": seed,
        "source_targets_rad": source_targets,
        "unit_fault_applied": case == "unit-plus-bitflip",
    }
    write_json(result_path, result)
    if not reproduced:
        raise DemoError("Day 1 fixture mutation did not reproduce its reject verdict")
    gate = run_behavior_gate(binary, 1, group, seed, target)
    write_json(target / "raw/demo-run.json", {"fault": result, "gate": gate})
    return [before_path, after_path, result_path, target / "raw/demo-run.json"]


def run_mqtt_roundtrip(
    host: str,
    port: int,
    topic: str,
    qos: int,
    messages: list[dict[str, object]],
) -> tuple[list[dict[str, object]], list[dict[str, object]]]:
    if host not in {"127.0.0.1", "localhost", "::1"}:
        raise DemoError("Day 2 broker must be loopback-only")
    for program in ("mosquitto_pub", "mosquitto_sub"):
        if shutil.which(program) is None:
            raise DemoError(f"required loopback MQTT client is missing: {program}")

    subscriber = subprocess.Popen(
        [
            "mosquitto_sub",
            "-h",
            host,
            "-p",
            str(port),
            "-t",
            topic,
            "-q",
            str(qos),
            "-C",
            str(len(messages)),
            "-W",
            "5",
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    publish_events: list[dict[str, object]] = []
    try:
        time.sleep(0.15)
        for message in messages:
            wire_payload = json.dumps(message, sort_keys=True, separators=(",", ":"))
            started_ns = time.monotonic_ns()
            published = subprocess.run(
                [
                    "mosquitto_pub",
                    "-h",
                    host,
                    "-p",
                    str(port),
                    "-t",
                    topic,
                    "-q",
                    str(qos),
                    "-m",
                    wire_payload,
                ],
                check=False,
                capture_output=True,
                text=True,
                timeout=5,
            )
            publish_events.append(
                {
                    "completed_ns": time.monotonic_ns(),
                    "exit_code": published.returncode,
                    "payload": message,
                    "started_ns": started_ns,
                    "stderr": published.stderr,
                }
            )
            if published.returncode != 0:
                raise DemoError(f"mosquitto_pub failed: {published.stderr.strip()}")
        try:
            output, error = subscriber.communicate(timeout=6)
        except subprocess.TimeoutExpired as timeout_error:
            subscriber.kill()
            output, error = subscriber.communicate()
            raise DemoError(f"loopback MQTT receive timed out: {error.strip()}") from timeout_error
    finally:
        if subscriber.poll() is None:
            subscriber.terminate()
            try:
                subscriber.wait(timeout=2)
            except subprocess.TimeoutExpired:
                subscriber.kill()
                subscriber.wait(timeout=2)
    if subscriber.returncode != 0:
        raise DemoError(f"mosquitto_sub failed: {error.strip()}")
    try:
        received = [json.loads(line) for line in output.splitlines() if line.strip()]
    except json.JSONDecodeError as decode_error:
        raise DemoError("loopback MQTT returned a non-JSON payload") from decode_error
    if received != messages:
        raise DemoError(f"loopback MQTT mismatch: sent {messages!r}, received {received!r}")
    return publish_events, received


def day2(
    target: Path,
    fixture_root: Path,
    case: str,
    seed: int,
    binary: Path,
    group: str,
) -> list[Path]:
    if case not in ALLOWED_CASES[2]:
        raise DemoError(f"case {case!r} is not valid for Day 2")
    manifest_path = fixture_root / "day2/manifest.json"
    messages_path = fixture_root / "day2/messages.csv"
    topics_path = fixture_root / "day2/topics.csv"
    manifest = read_json(manifest_path)
    fixture_seed(manifest.get("seed"), manifest_path, seed)
    topics = read_csv(topics_path)
    command_topic = next((row for row in topics if row["topic"] == "arm/cmd"), None)
    if command_topic is None or command_topic["qos"] != "1":
        raise DemoError("Day 2 command topic must exist with QoS 1")
    fixture_messages = read_csv(messages_path)
    first = next((row for row in fixture_messages if row["case"] == "newer"), None)
    delayed = next((row for row in fixture_messages if row["case"] == "stale_new"), None)
    if first is None or delayed is None or delayed["expected_reason"] != "STALE_COMMAND":
        raise DemoError("Day 2 delayed-old fixture pair is incomplete")
    if delayed["case"] not in manifest.get("workloads", []):
        raise DemoError("Day 2 manifest does not declare the stale workload")
    wire_messages: list[dict[str, object]] = []
    for row in (first, delayed):
        wire_messages.append(
            {
                "case": row["case"],
                "delivery_order": int(row["delivery_order"]),
                "expected_reason": row["expected_reason"],
                "now_ns": int(row["now_ns"]),
                "seed": seed,
                "seq": int(row["seq"]),
                "t_source_ns": int(row["t_source_ns"]),
            }
        )

    host = os.environ.get("COURSE_MQTT_HOST")
    port_text = os.environ.get("COURSE_MQTT_PORT")
    if host is None or port_text is None:
        raise DemoError("Day 2 demo must run through scripts/with_broker.py")
    try:
        port = int(port_text)
    except ValueError as error:
        raise DemoError("COURSE_MQTT_PORT is invalid") from error
    published, received = run_mqtt_roundtrip(
        host, port, command_topic["topic"], int(command_topic["qos"]), wire_messages
    )
    reproduced = (
        len(received) == 2
        and received[-1]["expected_reason"] == "STALE_COMMAND"
        and int(received[-1]["now_ns"]) - int(received[-1]["t_source_ns"])
        > int(manifest["max_command_age_ns"])
    )
    result_path = target / "raw/mqtt-loopback-roundtrip.json"
    result = {
        "broker": {"host": host, "port": port},
        "case": case,
        "fixture": str(messages_path),
        "fixture_sha256": sha256_file(messages_path),
        "published": published,
        "qos": int(command_topic["qos"]),
        "received": received,
        "reproduced": reproduced,
        "topic": command_topic["topic"],
    }
    write_json(result_path, result)
    trace_path = target / "raw/mqtt-received.jsonl"
    trace_path.write_text(
        "".join(json.dumps(row, sort_keys=True) + "\n" for row in received),
        encoding="utf-8",
    )
    if not reproduced:
        raise DemoError("Day 2 stale command was not reproduced over loopback MQTT")
    gate = run_behavior_gate(binary, 2, group, seed, target)
    write_json(target / "raw/demo-run.json", {"fault": result, "gate": gate})
    return [result_path, trace_path, target / "raw/demo-run.json"]


def day3(
    target: Path,
    fixture_root: Path,
    case: str,
    seed: int,
    binary: Path,
    group: str,
) -> list[Path]:
    if case not in ALLOWED_CASES[3]:
        raise DemoError(f"case {case!r} is not valid for Day 3")
    manifest_path = fixture_root / "day3/manifest.json"
    clients_path = fixture_root / "day3/clients.csv"
    workloads_path = fixture_root / "day3/workloads.csv"
    manifest = read_json(manifest_path)
    fixture_seed(manifest.get("seed"), manifest_path, seed)
    clients = read_csv(clients_path)
    workloads = read_csv(workloads_path)
    counts = sorted(int(row["client_count"]) for row in workloads)
    if counts != [1, 10, 50] or "poll" not in manifest.get("linux_api", []):
        raise DemoError("Day 3 workload or Linux poll contract is incomplete")
    by_class = {row["class"]: row for row in clients}
    if not {"slow", "fast", "safety"}.issubset(by_class):
        raise DemoError("Day 3 blocking fixture lacks slow, fast, or safety clients")
    expected_ids = {"D3_SLOW", "D3_FAST", "D3_SAFETY"}
    if not expected_ids.issubset({row["fixture_id"] for row in clients}):
        raise DemoError("Day 3 blocking case does not map to its declared fixtures")
    pause_ms = int(by_class["slow"]["pause_ms"])
    pipes: dict[str, tuple[int, int]] = {}
    poller = select.poll()
    service_ns: dict[str, int] = {}
    try:
        for name in ("slow", "fast", "safety"):
            read_fd, write_fd = os.pipe()
            pipes[name] = (read_fd, write_fd)
            poller.register(read_fd, select.POLLIN)
            os.write(write_fd, b"x")
        started_ns = time.monotonic_ns()
        ready = {fd for fd, flags in poller.poll(1000) if flags & select.POLLIN}
        if len(ready) != 3:
            raise DemoError("controlled Day 3 poll did not report all ready clients")
        for name in ("slow", "fast", "safety"):
            read_fd = pipes[name][0]
            if read_fd not in ready:
                raise DemoError(f"controlled Day 3 poll omitted {name}")
            if name == "slow":
                time.sleep(pause_ms / 1000.0)
            os.read(read_fd, 1)
            service_ns[name] = time.monotonic_ns() - started_ns
    finally:
        for read_fd, write_fd in pipes.values():
            os.close(read_fd)
            os.close(write_fd)
    threshold_ns = int(pause_ms * 1_000_000 * 0.9)
    reproduced = (
        service_ns.get("fast", 0) >= threshold_ns
        and service_ns.get("safety", 0) >= threshold_ns
    )
    result_path = target / "raw/blocking-poll-observation.json"
    result = {
        "case": case,
        "failure": "slow handler delays already-ready fast and safety clients",
        "fixture": str(clients_path),
        "fixture_sha256": sha256_file(clients_path),
        "pause_ms": pause_ms,
        "poll_ready_count": 3,
        "reproduced": reproduced,
        "service_latency_ns": service_ns,
        "workload_client_counts": counts,
    }
    write_json(result_path, result)
    if not reproduced:
        raise DemoError("Day 3 blocking failure signature was not observed")
    gate = run_behavior_gate(binary, 3, group, seed, target)
    write_json(target / "raw/demo-run.json", {"fault": result, "gate": gate})
    return [result_path, target / "raw/demo-run.json"]


def day4(
    target: Path,
    fixture_root: Path,
    case: str,
    seed: int,
    binary: Path,
    group: str,
) -> list[Path]:
    if case not in ALLOWED_CASES[4]:
        raise DemoError(f"case {case!r} is not valid for Day 4")
    manifest_path = fixture_root / "day4/manifest.json"
    faults_path = fixture_root / "day4/faults.csv"
    locks_path = fixture_root / "day4/lock_orders.csv"
    manifest = read_json(manifest_path)
    fixture_seed(manifest.get("seed"), manifest_path, seed)
    faults = read_csv(faults_path)
    race = next((row for row in faults if row["category"] == "check_write_race"), None)
    range_fault = next((row for row in faults if row["category"] == "joint_range"), None)
    locks = read_csv(locks_path)
    if race is None or range_fault is None or not any(
        row["acquire_order"] == "queue>state>wire" for row in locks
    ):
        raise DemoError("Day 4 race or lock-order fixture is incomplete")
    unsafe_value = float(range_fault["input"].split("=", 1)[1])
    shared = {"joint": 0.01}
    checked = threading.Event()
    mutated = threading.Event()
    observation: dict[str, object] = {}

    def unsafe_check_then_write() -> None:
        checked_value = float(shared["joint"])
        observation["check_passed"] = -JOINT_LIMIT_RAD <= checked_value <= JOINT_LIMIT_RAD
        observation["checked_value_rad"] = checked_value
        checked.set()
        if not mutated.wait(timeout=2):
            return
        written_value = float(shared["joint"])
        observation["written_value_rad"] = written_value
        observation["unsafe_write"] = bool(
            observation["check_passed"]
            and not (-JOINT_LIMIT_RAD <= written_value <= JOINT_LIMIT_RAD)
        )

    def mutate_source() -> None:
        if not checked.wait(timeout=2):
            return
        shared["joint"] = unsafe_value
        mutated.set()

    checker = threading.Thread(target=unsafe_check_then_write, name="unsafe-writer")
    mutator = threading.Thread(target=mutate_source, name="source-mutator")
    checker.start()
    mutator.start()
    checker.join(timeout=3)
    mutator.join(timeout=3)
    reproduced = (
        not checker.is_alive()
        and not mutator.is_alive()
        and observation.get("unsafe_write") is True
    )
    result_path = target / "raw/check-write-race-observation.json"
    result = {
        "case": case,
        "fixture": str(faults_path),
        "fixture_id": race["fixture_id"],
        "fixture_input": race["input"],
        "fixture_sha256": sha256_file(faults_path),
        "observation": observation,
        "required_lock_order": "queue>state>wire",
        "reproduced": reproduced,
        "seed": seed,
    }
    write_json(result_path, result)
    if not reproduced:
        raise DemoError("Day 4 check-write race did not produce its unsafe signature")
    gate = run_behavior_gate(binary, 4, group, seed, target)
    write_json(target / "raw/demo-run.json", {"fault": result, "gate": gate})
    return [result_path, target / "raw/demo-run.json"]


def day5(
    target: Path,
    fixture_root: Path,
    case: str,
    seed: int,
    binary: Path,
    group: str,
) -> list[Path]:
    if case not in ALLOWED_CASES[5]:
        raise DemoError(f"case {case!r} is not valid for Day 5")
    manifest_path = fixture_root / "day5/manifest.json"
    estop_path = fixture_root / "day5/estop_100.json"
    manifest = read_json(manifest_path)
    fixture = read_json(estop_path)
    fixture_seed(fixture.get("seed"), estop_path, seed)
    if (
        "epoll" not in manifest.get("linux_api", [])
        or fixture.get("fixture_id") != "E_STOP_100"
        or fixture.get("repetitions") != 100
    ):
        raise DemoError("Day 5 epoll or 100-run e-stop fixture is incomplete")
    planner_hz = int(fixture["planner_hz"])
    deadline_ns = int(fixture["deadline_ns"])
    ordinary = fixture.get("ordinary_sources")
    if planner_hz <= 0 or not isinstance(ordinary, list) or not ordinary:
        raise DemoError("Day 5 FIFO fixture has invalid scheduling parameters")
    queue = deque([*(str(source) for source in ordinary), "estop"])
    period_s = 1.0 / planner_hz
    started_ns = time.monotonic_ns()
    processed: list[dict[str, object]] = []
    ordinary_ahead = 0
    while queue:
        item = queue.popleft()
        if item == "estop":
            observed_ns = time.monotonic_ns()
            break
        time.sleep(period_s)
        ordinary_ahead += 1
        processed.append(
            {"completed_ns": time.monotonic_ns(), "source": item}
        )
    else:
        raise DemoError("Day 5 FIFO fixture did not contain e-stop")
    latency_ns = observed_ns - started_ns
    reproduced = latency_ns > deadline_ns and ordinary_ahead == len(ordinary)
    result_path = target / "raw/fifo-estop-observation.json"
    result = {
        "case": case,
        "deadline_ns": deadline_ns,
        "fixture": str(estop_path),
        "fixture_repetitions_for_behavior_gate": int(fixture["repetitions"]),
        "fixture_sha256": sha256_file(estop_path),
        "latency_ns": latency_ns,
        "ordinary_ahead": ordinary_ahead,
        "processed": processed,
        "reproduced": reproduced,
        "seed": seed,
    }
    write_json(result_path, result)
    if not reproduced:
        raise DemoError("Day 5 FIFO e-stop deadline miss was not observed")
    gate = run_behavior_gate(binary, 5, group, seed, target)
    write_json(target / "raw/demo-run.json", {"fault": result, "gate": gate})
    return [result_path, target / "raw/demo-run.json"]


WRITERS = {1: day1, 2: day2, 3: day3, 4: day4, 5: day5}


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run one fixture-driven, simulator-only failure demonstration"
    )
    parser.add_argument("--day", required=True)
    parser.add_argument("--case", required=True)
    parser.add_argument("--group", required=True)
    parser.add_argument("--seed", required=True)
    parser.add_argument("--evidence-root", required=True)
    parser.add_argument("--binary", required=True)
    parser.add_argument(
        "--fixtures",
        default=str(Path(__file__).resolve().parents[1] / "fixtures"),
    )
    args = parser.parse_args()
    try:
        day = parse_day(args.day)
        group = parse_group(args.group)
        seed = parse_seed(args.seed)
    except ValueError as error:
        parser.error(str(error))
    binary = Path(args.binary).resolve()
    if not binary.is_file() or not os.access(binary, os.X_OK):
        parser.error(f"verification binary is not executable: {binary}")
    if platform.system() != "Linux":
        parser.error("failure demonstrations require the provided Linux container")
    target = init_evidence(Path(args.evidence_root), group, day, seed)
    try:
        paths = WRITERS[day](
            target, Path(args.fixtures).resolve(), args.case, seed, binary, group
        )
    except DemoError as error:
        print(f"demo failed safely: {error}", file=sys.stderr)
        return 1
    print(f"fault reproduced and evidence saved for {group} day {day}")
    for path in paths:
        print(path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
