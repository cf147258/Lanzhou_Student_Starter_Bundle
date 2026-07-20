FROM ubuntu:24.04@sha256:4fbb8e6a8395de5a7550b33509421a2bafbc0aab6c06ba2cef9ebffbc7092d90

ARG DEBIAN_FRONTEND=noninteractive
ARG STUDENT_UID=1000
ARG STUDENT_GID=1000

LABEL org.opencontainers.image.title="Lanzhou Robot-Arm Student Starter"
LABEL org.opencontainers.image.version="1.0.0-starter"
LABEL org.opencontainers.image.description="Linux-only C17 course environment"

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        gcc \
        make \
        mosquitto \
        mosquitto-clients \
        libmosquitto-dev \
        pkg-config \
        python3 \
        socat \
        tini \
    && rm -rf /var/lib/apt/lists/*

RUN set -eu; \
    old_user="$(getent passwd "${STUDENT_UID}" | cut -d: -f1 || true)"; \
    if [ -n "${old_user}" ]; then userdel "${old_user}"; fi; \
    old_group="$(getent group "${STUDENT_GID}" | cut -d: -f1 || true)"; \
    if [ -n "${old_group}" ]; then groupdel "${old_group}"; fi; \
    groupadd --gid "${STUDENT_GID}" student; \
    useradd --uid "${STUDENT_UID}" --gid "${STUDENT_GID}" \
        --create-home --shell /bin/bash student

WORKDIR /workspace
COPY --chown=student:student . /workspace
RUN chown student:student /workspace

USER student
ENTRYPOINT ["/usr/bin/tini", "--", "/bin/sh", "/workspace/container/entrypoint"]
CMD ["make", "smoke"]
