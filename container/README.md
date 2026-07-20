# Container contract

The released environment uses Ubuntu 24.04 and runs as the unprivileged
`student` account. The only broker listener is `127.0.0.1:18883` inside the
container. No host-native build path or host compatibility layer is maintained.

Before distribution, the course team builds and releases one `linux/arm64`
image and one `linux/amd64` image. Both immutable IDs are recorded in
`image.lock`, and both pass the clean-machine checks in
[`docs/CONTAINER.md`](../docs/CONTAINER.md). Students load or pull the image for
their platform and verify its inspected ID against the matching lock entry.
The fixed tags are `lanzhou-course-starter:1.0.0-arm64` and
`lanzhou-course-starter:1.0.0-amd64`.

Student-side rebuilding is only an instructor-approved fallback when the
released image cannot be obtained. A locally built image is not a substitute
for a locked course image until the course team accepts and records it.

`image.lock` is deliberately excluded from the Docker build context. This
prevents the recorded image ID from changing the image whose ID it records.
