# Lanzhou Networked Robot-Arm Student Starter Bundle

This is the single assessed implementation path for the five-day course.
Students write C17. The **Soda Code** label in the slides is a teaching aid;
the container does not install a Rust compiler or a second implementation
toolchain.

All assessed work runs in the supplied Linux container. Native host builds,
host compatibility code, physical-arm access, and external network services
are outside the student task.

## First 10 minutes

Prerequisite: Docker on a Linux computer. No compiler or broker is required on
the host. The course team supplies one source bundle plus a ready-built image
for each supported Linux architecture. Use the supplied image; do not rebuild
it during normal class setup.

1. Enter this directory. Select the archive, tag, and lock entry for your
   computer. Set `COURSE_IMAGE_ARCHIVE` to the path where the course team placed
   the matching archive, then load it. Keep these variables in the same shell
   for the commands below:

   ```sh
   case "$(uname -m)" in
     arm64|aarch64)
       COURSE_IMAGE_ARCHIVE=/course-release/lanzhou-course-starter-1.0.0-arm64.tar
       COURSE_IMAGE_TAG=lanzhou-course-starter:1.0.0-arm64
       LOCK_KEY=COURSE_IMAGE_ARM64_ID
       ;;
     x86_64|amd64)
       COURSE_IMAGE_ARCHIVE=/course-release/lanzhou-course-starter-1.0.0-amd64.tar
       COURSE_IMAGE_TAG=lanzhou-course-starter:1.0.0-amd64
       LOCK_KEY=COURSE_IMAGE_AMD64_ID
       ;;
     *) echo "unsupported architecture: $(uname -m)" >&2; exit 2 ;;
   esac
   export COURSE_IMAGE_TAG LOCK_KEY
   docker load --input "$COURSE_IMAGE_ARCHIVE"
   ```

   If the course team uses a registry, run its supplied pull command for the
   same platform tag instead of `docker load`.

2. Verify that the loaded image has the ID locked for your platform:

   ```sh
   EXPECTED_ID="$(sed -n "s/^${LOCK_KEY}=//p" container/image.lock)"
   ACTUAL_ID="$(docker image inspect "$COURSE_IMAGE_TAG" --format '{{.Id}}')"
   test "$ACTUAL_ID" = "$EXPECTED_ID" && echo "course image ID verified"
   ```

   Stop and contact the instructor if this check does not print the success
   line. Do not submit evidence from a different image.

3. Run the untouched starter smoke gate:

   ```sh
   docker run --rm --init --network=none \
     -v "$PWD:/workspace" -w /workspace \
     "$COURSE_IMAGE_TAG" make smoke SEED=20260719
   ```

4. Open an interactive course shell:

   ```sh
   docker run --rm -it --init --network=none \
     -v "$PWD:/workspace" -w /workspace \
     "$COURSE_IMAGE_TAG" /bin/bash
   ```

5. Inside that shell, find the bounded Day 1 work:

   ```sh
   make todo DAY=1 TEAM=G1
   make check DAY=1 TEAM=G1
   ```

The second command is expected to be red on the untouched starter and must say
`TODO_NOT_IMPLEMENTED`. A compiler error, missing dependency, crash, or direct
actuator write is not an expected starter failure.

### Local image build: fallback only

Build locally only if the course image cannot be loaded or pulled and the
instructor has asked you to use this fallback:

```sh
docker build \
  --build-arg STUDENT_UID="$(id -u)" \
  --build-arg STUDENT_GID="$(id -g)" \
  -t lanzhou-course-starter:1.0.0-local .
docker image inspect lanzhou-course-starter:1.0.0-local --format '{{.Id}}'
```

Record the new ID and notify the instructor. It is not the locked course image
and must not be used for assessed evidence until the course team accepts it.

## Starter-state contract

- `make build` succeeds in the supplied container.
- `make smoke SEED=20260719` is green before students edit code.
- `make verify-day1` is initially red because bounded student functions return
  a fail-closed TODO result.
- Later verification commands stop at the first unmet earlier gate.
- TODO stubs reject work; they never approve a command or bypass the sole
  actuator writer.
- The simulator and all fault fixtures are sufficient for assessment. No robot
  hardware is required.

Run `make help` for the complete command summary.

## One class repository, five module owners

All 15 students extend the same program. Each group works on its own module and
lands small owner-approved commits into the shared integration branch during
the sprint. The public group check is an integration check at that group's
chapter boundary, not an isolated unit test.

| Group | Chapters | Owned module |
|---|---|---|
| G1 | Ch1–2 | State, twin, calibration, and quality |
| G2 | Ch3–4 | Frame, parser, transport, and freshness |
| G3 | Ch5–6 | Nonblocking I/O, `poll`, and `epoll` runtime |
| G4 | Ch7–8 | Arbitration, safety gate, writer, and audit |
| G5 | Ch9–10 | Controller containment, gatekeeper, and replay |

Iteration uses `make check DAY=N TEAM=Gx` against the latest shared branch. A
check may stay red while a named upstream interface is still red; record that
dependency and do not edit the upstream owner's module. After all five owner
commits land, the class runs `make verify-dayN`. Frozen declarations in
`include/course.h` change only after a whole-class interface review.

## Five cumulative days

| Day | Input | Bounded addition | Output and gate |
|---|---|---|---|
| 1 | `starter_repo` | Trusted state, frame integrity, parser recovery | `frame_v1`; `make verify-day1` |
| 2 | `frame_v1` | MQTT-facing timestamps, latest value, stale/duplicate rejection | `transport_v1`; `make verify-day2` |
| 3 | `transport_v1` | Nonblocking clients, `poll`, partial I/O, backpressure | `runtime_poll_v1`; `make verify-day3` |
| 4 | `runtime_poll_v1` | One writer, fail-closed safety, stable reasons, audit | `safety_v1`; `make verify-day4` |
| 5 | `safety_v1` | `epoll`, wake/timer fds, cancellation, fallback, replay | `release_v1`; `make verify-day5` |

Each `verify-dayN` is cumulative. Day 5 reruns every earlier public gate before
testing the final release behavior.

## Fixed failure demonstrations

These commands operate only on the simulator and save raw evidence:

```sh
make fail-day1 CASE=unit-plus-bitflip GROUP=G1
make demo-day2-stale GROUP=G2
make demo-day3-blocking GROUP=G3
make demo-day4-race GROUP=G4
make demo-day5-fifo GROUP=G5
```

Day 2 starts a private Mosquitto process bound only to
`127.0.0.1:18883`. Verification itself remains reproducible without an
external broker or Internet access.

## Evidence

`make check` creates the daily group pack:

```text
evidence/Gx_DN_evidence/
├── README.md
├── code-or-patch/
├── raw/
├── figures/
├── chapter-A-evidence.md
├── chapter-B-evidence.md
├── references.bib
└── manifest.json
```

The scaffold README, chapter notes, references, and manifest are created only
when missing. Public gates deliberately regenerate their canonical raw files
and `manifest.json` on each run. Copy any prior raw run you need before rerunning
the same gate. The manifest records the platform-matching image ID from
`container/image.lock` unless the course team supplies an explicit environment
value. It records `COURSE_COMMIT` when supplied, otherwise a Git commit when one
is available, and `NOT_A_GIT_CHECKOUT` for an unpacked release.

Keep the fixed seed, image ID, commit, commands, raw CSV/JSON/logs, failed runs,
and known limits. Screenshots are not raw evidence. The first line of the pack
README is the input tag; its last line is the output tag.

## Green checkpoints and recovery

This release contains **no answers and no preloaded last-green bundle**. A
checkpoint is created from the class's own merged work only after its cumulative
gate is green:

```sh
make checkpoint DAY=1 GROUP=CLASS
```

The archive is written under `checkpoints/` with file hashes. Restore only a
checkpoint named by the instructor:

```sh
make restore CHECKPOINT=checkpoints/class_day1_green_<time>.tar.gz
```

Restore first backs up the five current student modules under
`checkpoints/backups/`, then validates the checkpoint metadata, every archive
path, and every hash. It replaces only those five modules and evidence from the
day and group named in the checkpoint manifest. A `CLASS` checkpoint may
contain G1–G5 evidence for that one day; a group checkpoint may contain only
that group's evidence for that day. Frozen course support is never replaced.
Existing evidence files are not included in the source backup, so copy any
evidence you must retain before restoring.

## Trace replay

Use a CSV or JSON Lines trace containing `verdict` and `reason`. Optional
`replay_verdict` and `replay_reason` fields are compared directly.

```sh
make replay TRACE=evidence/G2_D2_evidence/trace.jsonl
```

## Troubleshooting

- **“run this target in the supplied Linux container”** — do not add host
  compatibility code; enter the Docker shell above.
- **Bind-mounted files are not writable** — contact the instructor first. Use
  the fallback local build with your Linux `id -u` and `id -g` values only when
  directed, then record its image ID.
- **`TODO_NOT_IMPLEMENTED`** — this is the intended daily red gate; use
  `make todo DAY=N TEAM=Gx` and edit only the marked student module.
- **Broker cannot bind** — leave the wrapper to stop its private process, then
  rerun the demo. Do not start a second broker in the container.
- **Day 3 or Day 5 timing is noisy** — stop unrelated container workloads and
  rerun the fixed workload; do not loosen frozen safety deadlines.
- **A prior day becomes red** — stop current work, return to the last class tag
  or an instructor-named class checkpoint, and repair the earlier contract.

Before distribution, course staff run `make starter-audit` and the clean-machine
procedure in [`docs/CONTAINER.md`](docs/CONTAINER.md).
