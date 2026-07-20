# Linux container and clean-machine check

## Runtime policy

- Base system: Ubuntu 24.04.
- Process user: `student`, never root.
- Assessed compiler: GCC in C17 mode.
- Build: GNU Make and `pkg-config`.
- Local communication: POSIX threads/sockets, Mosquitto and its C client
  library, plus `socat` for paired pseudo terminals.
- Day 5 uses Linux `epoll`, `eventfd`, and `timerfd` directly.
- Python standard-library scripts create fixtures, evidence, and public gate
  reports. Students do not submit Python implementations.
- No Rust compiler, package manager, ROS, model runtime, or external service is
  installed.

Mosquitto has exactly one listener: `127.0.0.1:18883` inside the container.
The normal run command uses `--network=none`; loopback still works while outside
network access is disabled.

## Release freeze

1. Build one `linux/arm64` image and one `linux/amd64` image from the same
   allowlisted student-only directory.
2. Record both immutable image IDs in `container/image.lock`, with
   `PLATFORMS=linux/arm64,linux/amd64`.
3. Export or publish both images and save their checksums beside the source
   archive. Give students the matching archive or an exact pull command. This
   team-built image is the normal course path. The release tags are
   `lanzhou-course-starter:1.0.0-arm64` and
   `lanzhou-course-starter:1.0.0-amd64`.
4. On each platform, load or pull the released image, inspect its ID, and
   compare it with the matching `COURSE_IMAGE_ARM64_ID` or
   `COURSE_IMAGE_AMD64_ID` entry before any gate runs.
5. Never place staff patches in the Docker build context. Deleting a file in a
   later image layer does not remove it from earlier layers.

Students do not build the image during normal setup. A local student build is a
fallback only when distribution fails and the instructor approves it. Its ID
must be recorded and must not stand in for either locked release ID without a
new course-team acceptance run.

## Clean-machine acceptance

On separate `arm64` and `amd64` Linux computers with Docker but no local
compiler or broker:

1. Verify the source and image checksums, unpack the untouched source bundle,
   and enter its root directory. Load or pull the platform-matching course
   image. Use `lanzhou-course-starter:1.0.0-arm64` on `arm64`/`aarch64` and
   `lanzhou-course-starter:1.0.0-amd64` on `x86_64`/`amd64`; set
   `COURSE_IMAGE_TAG` to that tag in the same shell. Do not rebuild it for this
   acceptance pass.
2. Confirm `docker image inspect` reports the matching ID in
   `container/image.lock`.
3. Run every acceptance command with the source directory bind-mounted. The
   bind is required because `image.lock` is deliberately outside the image
   build context. For the first command, use:

   ```sh
   docker run --rm --init --network=none \
     -v "$PWD:/workspace" -w /workspace \
     "$COURSE_IMAGE_TAG" make starter-audit
   ```

   This runs as the image's built-in non-root user.
4. Confirm that bound-source `make starter-audit` command is green.
5. Confirm `make smoke SEED=20260719` is green on the untouched source.
6. Confirm `make verify-day1` is red only with explicit TODO results.
7. In a private staff job outside the student archive, apply the QA patches and
   run `make verify-day1` through `make verify-day5` in order.
8. Run the fixed-seed suite twice and compare the normalized trace hashes.
9. Confirm `make verify-day5` finishes inside the eight-minute class budget.
10. Inspect the final archive: no staff patch, prebuilt answer, green checkpoint,
   second compiler toolchain, build output, or repository history may remain.

The private QA patches prove that the public tests are achievable; they are not
a second student package and must never enter the release image or archive.
