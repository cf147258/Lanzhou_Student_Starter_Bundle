SHELL := /bin/sh
.DEFAULT_GOAL := help
.NOTPARALLEL:

CC ?= gcc
PYTHON ?= python3
SEED ?= 20260719
DAY ?=
TEAM ?=
GROUP ?= G1
CASE ?= unit-plus-bitflip
TRACE ?=
CHECKPOINT ?=

BUILD_DIR := build
VERIFY_BIN := $(BUILD_DIR)/course_verify
EVIDENCE_ROOT := evidence
VERSION := $(shell sed -n '1p' VERSION)

CPPFLAGS += -Iinclude -Itests -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE
CFLAGS ?= -std=c17 -O2 -g
CFLAGS += -Wall -Wextra -Wpedantic -Werror -MMD -MP
LDFLAGS += -pthread
LDLIBS += -lm $(shell pkg-config --libs libmosquitto 2>/dev/null)

SUPPORT_SOURCES := src/support/course_support.c
STUDENT_SOURCES := \
	src/student/g1_state.c \
	src/student/g2_transport.c \
	src/student/g3_runtime.c \
	src/student/g4_safety.c \
	src/student/g5_gatekeeper.c
TEST_SOURCES := \
	tests/test_harness.c \
	tests/verify_main.c \
	tests/smoke.c \
	tests/day1.c \
	tests/day2.c \
	tests/day3.c \
	tests/day4.c \
	tests/day5.c
SOURCES := $(SUPPORT_SOURCES) $(STUDENT_SOURCES) $(TEST_SOURCES)
OBJECTS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(SOURCES))
DEPS := $(OBJECTS:.o=.d)

DAYS := 1 2 3 4 5
GROUP_NUMBERS := 1 2 3 4 5

.PHONY: help require-linux build smoke todo check starter-audit \
	verify-day1 verify-day2 verify-day3 verify-day4 verify-day5 \
	gate-day1 gate-day2 gate-day3 gate-day4 gate-day5 \
	fail-day1 demo-day2-stale demo-day3-blocking demo-day4-race \
	demo-day5-fifo checkpoint restore replay clean

help:
	@echo "Lanzhou Robot-Arm Student Starter $(VERSION)"
	@echo ""
	@echo "Core targets:"
	@echo "  make build"
	@echo "  make smoke SEED=20260719"
	@echo "  make todo DAY=1 TEAM=G1"
	@echo "  make check DAY=1 TEAM=G1"
	@echo "  make test-day1-g1"
	@echo "  make verify-day1  ...  make verify-day5"
	@echo ""
	@echo "Failure demonstrations:"
	@echo "  make fail-day1 CASE=unit-plus-bitflip GROUP=G1"
	@echo "  make demo-day2-stale GROUP=G2"
	@echo "  make demo-day3-blocking GROUP=G3"
	@echo "  make demo-day4-race GROUP=G4"
	@echo "  make demo-day5-fifo GROUP=G5"
	@echo ""
	@echo "Recovery and evidence:"
	@echo "  make checkpoint DAY=1 GROUP=CLASS"
	@echo "  make restore CHECKPOINT=checkpoints/<file>.tar.gz"
	@echo "  make replay TRACE=evidence/<group>/<trace>.jsonl"
	@echo "  make starter-audit"

require-linux:
	@$(PYTHON) scripts/linux_preflight.py --platform-only

build: require-linux $(VERIFY_BIN)
	@echo "build green: $(VERIFY_BIN)"

$(VERIFY_BIN): $(OBJECTS)
	@mkdir -p $(@D)
	$(CC) $(LDFLAGS) $(OBJECTS) $(LDLIBS) -o $@

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

smoke: build
	@$(PYTHON) scripts/linux_preflight.py
	@COURSE_SEED="$(SEED)" \
	 COURSE_EVIDENCE_DIR="$(EVIDENCE_ROOT)/smoke" \
	 $(PYTHON) scripts/with_broker.py --config container/mosquitto.conf -- \
	 "$(VERIFY_BIN)" smoke ALL

todo:
	@$(PYTHON) scripts/list_todos.py --root src/student \
	 $(if $(DAY),--day "$(DAY)",) $(if $(TEAM),--team "$(TEAM)",)

check: build
	@$(PYTHON) scripts/run_check.py --binary "$(VERIFY_BIN)" \
	 --day "$(DAY)" --team "$(TEAM)" --seed "$(SEED)" \
	 --evidence-root "$(EVIDENCE_ROOT)"

define TEAM_TEST_RULE
.PHONY: test-day$(1)-g$(2)
test-day$(1)-g$(2): build
	@$(PYTHON) scripts/run_check.py --binary "$(VERIFY_BIN)" \
	 --day "$(1)" --team "G$(2)" --seed "$(SEED)" \
	 --evidence-root "$(EVIDENCE_ROOT)"
endef
$(foreach day,$(DAYS),$(foreach group,$(GROUP_NUMBERS),$(eval $(call TEAM_TEST_RULE,$(day),$(group)))))

define DAY_GATE_RULE
gate-day$(1): build
	@$(PYTHON) scripts/run_gate.py --binary "$(VERIFY_BIN)" \
	 --day "$(1)" --seed "$(SEED)" --evidence-root "$(EVIDENCE_ROOT)"
endef
$(foreach day,$(DAYS),$(eval $(call DAY_GATE_RULE,$(day))))

verify-day1: gate-day1

verify-day2: verify-day1
	@$(MAKE) --no-print-directory gate-day2 SEED="$(SEED)"

verify-day3: verify-day2
	@$(MAKE) --no-print-directory gate-day3 SEED="$(SEED)"

verify-day4: verify-day3
	@$(MAKE) --no-print-directory gate-day4 SEED="$(SEED)"

verify-day5: verify-day4
	@$(MAKE) --no-print-directory gate-day5 SEED="$(SEED)"

fail-day1: build
	@$(PYTHON) scripts/fault_demo.py --day 1 --case "$(CASE)" \
	 --group "$(GROUP)" --seed "$(SEED)" --evidence-root "$(EVIDENCE_ROOT)" \
	 --binary "$(VERIFY_BIN)"

demo-day2-stale: build
	@$(PYTHON) scripts/with_broker.py --config container/mosquitto.conf -- \
	 $(PYTHON) scripts/fault_demo.py --day 2 --case delayed-old \
	 --group "$(GROUP)" --seed "$(SEED)" --evidence-root "$(EVIDENCE_ROOT)" \
	 --binary "$(VERIFY_BIN)"

demo-day3-blocking: build
	@$(PYTHON) scripts/fault_demo.py --day 3 --case blocking-slow-client \
	 --group "$(GROUP)" --seed "$(SEED)" --evidence-root "$(EVIDENCE_ROOT)" \
	 --binary "$(VERIFY_BIN)"

demo-day4-race: build
	@$(PYTHON) scripts/fault_demo.py --day 4 --case check-write-race \
	 --group "$(GROUP)" --seed "$(SEED)" --evidence-root "$(EVIDENCE_ROOT)" \
	 --binary "$(VERIFY_BIN)"

demo-day5-fifo: build
	@$(PYTHON) scripts/fault_demo.py --day 5 --case fifo-estop \
	 --group "$(GROUP)" --seed "$(SEED)" --evidence-root "$(EVIDENCE_ROOT)" \
	 --binary "$(VERIFY_BIN)"

starter-audit:
	@$(PYTHON) scripts/starter_audit.py --root .

checkpoint: require-linux
	@$(PYTHON) scripts/validate_selection.py --day "$(DAY)" --group "$(GROUP)"
	@$(MAKE) --no-print-directory verify-day$(DAY) SEED="$(SEED)"
	@$(PYTHON) scripts/checkpoint.py create --root . --day "$(DAY)" \
	 --group "$(GROUP)" --seed "$(SEED)"

restore: require-linux
	@$(PYTHON) scripts/checkpoint.py restore --root . --archive "$(CHECKPOINT)"

replay: require-linux
	@$(PYTHON) scripts/replay.py --trace "$(TRACE)"

clean:
	@rm -rf -- "$(BUILD_DIR)"
	@echo "removed build outputs; evidence and checkpoints were preserved"

-include $(DEPS)
