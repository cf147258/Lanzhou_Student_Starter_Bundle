# Frozen implementation contracts

The complete declarations and numeric limits live in `include/course.h`. This
page explains ownership; it does not replace that header.

## State and command

The shared state carries sequence, monotonic time, frame ID, three joint
positions, three joint rates, and uncertainty. The command carries source time,
trace ID, generation, three joint targets, and an emergency flag. Units are
radians, radians per second, and nanoseconds. Generation is local controller
metadata and is not serialized by Frame V1.

## Frame and transport

Frame V1 has a fixed byte length, byte order, sync, version, type, payload,
sequence, source time, trace ID, and CRC. Type 1 is ordinary command traffic;
type 2 is emergency-stop traffic, so the decoder restores the command's
emergency flag while preserving its source time. Code serializes fields
explicitly; it never copies an in-memory structure onto the wire. MQTT QoS does
not replace sequence or freshness checks. The latest-value store changes only
after frame, order, clock, and age checks pass.

## Runtime

Each client owns partial input state, queued byte count, oldest enqueue time,
and a per-turn work count. No readiness callback blocks. `poll` and `epoll`
service bounded work before returning to readiness. Emergency readiness has
priority over ordinary motion work.

## Safety and actuation

All producers submit to one bounded writer queue. Only the writer pump may call
the simulator commit operation, which requires a matching writer identity,
APPROVE decision, command, and append-only audit row. Lower-layer tests count
accepted outputs without calling that commit operation. The pure safety
decision rejects stale state, non-finite values, joint/rate violations, and
excess uncertainty. Every decision has a stable verdict and reason and is
appended to the audit before an approved write.

## Controller containment and replay

Controller output is untrusted. Timeout, late generation, invalid values, and
low confidence lead to fallback or discard; no controller bypasses freshness,
safety, writer ownership, or audit. Replay compares the final verdict and reason
for the same trace and command sequence.

## Determinism boundary

Correctness fixtures use seed `20260719`, a fixed-step simulator, explicit
fault parameters, and normalized trace rows. Environment time, process IDs, and
pseudo-terminal path numbers are excluded from byte-for-byte comparisons.
Day 3 and Day 5 additionally record real `CLOCK_MONOTONIC` latency; maximum and
deadline misses are reported separately from deterministic verdict checks.
