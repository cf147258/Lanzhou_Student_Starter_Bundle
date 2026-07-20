#ifndef COURSE_H
#define COURSE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Frozen course contract. Change only through a whole-class interface review. */
enum {
    COURSE_ARM_DOF = 3,
    COURSE_FRAME_ID_BASE = 1,
    COURSE_FRAME_SYNC_0 = 0xA5,
    COURSE_FRAME_SYNC_1 = 0x5A,
    COURSE_FRAME_VERSION = 1,
    COURSE_FRAME_TYPE_COMMAND = 1,
    COURSE_FRAME_TYPE_ESTOP = 2,
    COURSE_COMMAND_PAYLOAD_LEN = 24,
    COURSE_FRAME_V1_LEN = 56,
    COURSE_FRAME_CRC_OFFSET = 54,
    COURSE_CLIENT_BUFFER_SIZE = 4096,
    COURSE_MAX_CLIENTS = 64,
    COURSE_WORK_BUDGET = 4,
    COURSE_WRITER_QUEUE_CAPACITY = 16,
    COURSE_AUDIT_CAPACITY = 256
};

#define COURSE_FIXED_SEED UINT64_C(20260719)
#define COURSE_FIXED_STEP_NS INT64_C(10000000)
#define COURSE_MAX_CMD_AGE_NS UINT64_C(100000000)
#define COURSE_MAX_STATE_AGE_NS UINT64_C(100000000)
#define COURSE_MAX_QUEUE_AGE_NS UINT64_C(50000000)
#define COURSE_MAX_QUEUE_BYTES ((size_t)8192)
#define COURSE_ESTOP_DEADLINE_NS UINT64_C(50000000)
#define COURSE_CONTROLLER_TIMEOUT_NS UINT64_C(40000000)
#define COURSE_MIN_CONTROLLER_CONFIDENCE 0.60
#define COURSE_JOINT_MIN_RAD (-3.14159265358979323846)
#define COURSE_JOINT_MAX_RAD (3.14159265358979323846)
#define COURSE_MAX_JOINT_RATE_RAD_S 2.0
#define COURSE_MAX_SIGMA_Q_RAD 0.10

#define COURSE_TOPIC_STATE "arm/state"
#define COURSE_TOPIC_COMMAND "arm/cmd"
#define COURSE_TOPIC_ESTOP "arm/estop"

typedef enum {
    COURSE_RX_NEED_MORE = 0,
    COURSE_RX_ACCEPT,
    COURSE_RX_NACK_LENGTH,
    COURSE_RX_NACK_CRC,
    COURSE_RX_NACK_SEQUENCE,
    COURSE_RX_NACK_VERSION,
    COURSE_RX_REJECT
} RxVerdict;

typedef enum {
    COURSE_VERDICT_REJECT = 0,
    COURSE_VERDICT_APPROVE,
    COURSE_VERDICT_FALLBACK,
    COURSE_VERDICT_DISCARD
} Verdict;

typedef enum {
    COURSE_REASON_NONE = 0,
    COURSE_REASON_STUDENT_TODO,
    COURSE_REASON_BAD_FRAME,
    COURSE_REASON_NOT_NEW,
    COURSE_REASON_CLOCK_ERROR,
    COURSE_REASON_STALE_COMMAND,
    COURSE_REASON_STALE_STATE,
    COURSE_REASON_NONFINITE,
    COURSE_REASON_JOINT_RANGE,
    COURSE_REASON_RATE_LIMIT,
    COURSE_REASON_UNCERTAINTY,
    COURSE_REASON_QUEUE_FULL,
    COURSE_REASON_QUEUE_EXPIRED,
    COURSE_REASON_DEADLINE_MISS,
    COURSE_REASON_CONTROLLER_TIMEOUT,
    COURSE_REASON_CONTROLLER_INVALID,
    COURSE_REASON_LOW_CONFIDENCE,
    COURSE_REASON_CANCELLED,
    COURSE_REASON_INTERNAL
} Reason;

typedef enum {
    COURSE_CLIENT_FAST = 0,
    COURSE_CLIENT_SLOW,
    COURSE_CLIENT_FLOOD,
    COURSE_CLIENT_SAFETY
} ClientClass;

typedef enum {
    COURSE_READY_NONE = 0,
    COURSE_READY_ESTOP,
    COURSE_READY_CONTROLLER,
    COURSE_READY_TIMER,
    COURSE_READY_MOTION
} ReadyKind;

typedef enum {
    COURSE_CONTROLLER_USE = 0,
    COURSE_CONTROLLER_FALLBACK,
    COURSE_CONTROLLER_DISCARD
} ControllerAction;

typedef struct {
    uint64_t seq;
    int64_t t_mono_ns;
    uint8_t frame_id;
    double q_rad[COURSE_ARM_DOF];
    double dq_rad_s[COURSE_ARM_DOF];
    float sigma_q_rad[COURSE_ARM_DOF];
} ArmState;

typedef struct {
    uint64_t seq;
    uint64_t t_source_ns;
    uint64_t trace_id;
    double q_target_rad[COURSE_ARM_DOF];
    uint64_t generation;
    bool emergency;
} ArmCommand;

typedef struct {
    uint8_t bytes[COURSE_FRAME_V1_LEN];
    size_t used;
    bool syncing;
} FrameParser;

typedef struct {
    uint64_t last_seq;
    bool has_last;
    ArmCommand latest;
    bool has_latest;
} FreshnessGate;

typedef struct {
    uint64_t trace_id;
    uint64_t seq;
    uint64_t t_pub_ns;
    uint64_t t_rx_ns;
    uint64_t t_gate_ns;
    uint64_t t_ack_ns;
    Verdict verdict;
    Reason reason;
} TraceRow;

typedef struct {
    int fd;
    uint32_t client_id;
    ClientClass class_id;
    uint8_t input[COURSE_CLIENT_BUFFER_SIZE];
    size_t input_used;
    size_t queued_bytes;
    uint64_t oldest_enqueue_ns;
    size_t work_this_turn;
    size_t dispatched_total;
    size_t dropped_total;
} ClientState;

typedef struct {
    size_t ready_events;
    size_t fast_dispatched;
    size_t safety_dispatched;
    size_t slow_dispatched;
    size_t flood_dropped;
    size_t max_work_per_client;
    uint64_t max_queue_age_ns;
} RuntimeStats;

typedef struct {
    Verdict verdict;
    Reason reason;
} SafetyDecision;

typedef struct {
    uint64_t trace_id;
    uint64_t command_seq;
    uint64_t state_seq;
    Verdict verdict;
    Reason reason;
    uint32_t writer_id;
    uint64_t t_commit_ns;
} AuditRow;

typedef struct {
    ArmCommand queue[COURSE_WRITER_QUEUE_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    uint32_t writer_id;
    size_t write_count;
    size_t unsafe_write_count;
    AuditRow audit[COURSE_AUDIT_CAPACITY];
    size_t audit_count;
} ActuatorWriter;

typedef struct {
    size_t write_count;
    size_t rejected_write_attempts;
    uint64_t last_command_seq;
    uint64_t last_trace_id;
    uint32_t last_writer_id;
} Simulator;

typedef struct {
    ArmCommand command;
    uint64_t generation;
    uint64_t produced_ns;
    double confidence;
    bool timed_out;
} ControllerResult;

typedef struct {
    ReadyKind kind;
    uint64_t observed_ns;
    uint64_t cancel_generation;
} ReadyEvent;

/* G1 — Ch1/2: state contract and deterministic twin. */
bool state_valid(const ArmState *state, const ArmState *previous);
void twin_step(ArmState *state, const ArmCommand *command,
                   int64_t step_ns);
uint64_t state_schema_hash(void);

/* G2 — Ch3/4: FrameV1, MQTT-facing freshness, and latest value. */
uint16_t crc16_ccitt_false(const uint8_t *bytes, size_t length);
bool frame_encode(const ArmCommand *command, uint8_t *out,
                      size_t capacity, size_t *written);
RxVerdict frame_decode(const uint8_t *bytes, size_t length,
                              uint64_t last_seq, ArmCommand *out);
void frame_parser_init(FrameParser *parser);
RxVerdict frame_feed(FrameParser *parser, uint8_t byte,
                            uint64_t last_seq, ArmCommand *out);
Reason freshness_accept(FreshnessGate *gate,
                               const ArmCommand *command,
                               uint64_t now_ns);
Reason transport_on_message(FreshnessGate *gate,
                                   const uint8_t *bytes, size_t length,
                                   uint64_t now_ns, TraceRow *trace,
                                   ArmCommand *accepted);

/* G3 — Ch5/6: timestamps, nonblocking poll, and Linux epoll priority. */
bool trace_complete(const TraceRow *trace);
int set_nonblocking(int fd);
int poll_service_once(ClientState *clients, size_t client_count,
                          int timeout_ms, RuntimeStats *stats);
int epoll_wait_priority(int epoll_fd, int wake_fd, int controller_fd,
                            int timer_fd, int motion_fd, int timeout_ms,
                            uint64_t cancel_generation, ReadyEvent *event);

/* G4 — Ch7/8: pure safety decision, bounded submit, one writer, audit. */
SafetyDecision safety_gate(const ArmState *state,
                                  const ArmCommand *command,
                                  uint64_t now_ns);
void writer_init(ActuatorWriter *writer, uint32_t writer_id);
bool actuator_submit(ActuatorWriter *writer,
                         const ArmCommand *command);
bool actuator_pump(ActuatorWriter *writer, const ArmState *state,
                       uint64_t now_ns, Simulator *simulator);

/* G5 — Ch9/10: controller containment, gatekeeper, trace replay. */
ControllerAction controller_sanitize(
    const ControllerResult *result, uint64_t active_generation,
    uint64_t now_ns, const ArmState *state, ArmCommand *command_out,
    Reason *reason_out);
Verdict gatekeeper_process(FreshnessGate *freshness,
                                  ActuatorWriter *writer,
                                  const ArmState *state,
                                  const uint8_t *frame, size_t frame_length,
                                  uint64_t now_ns, TraceRow *trace);
bool trace_replay_matches(const TraceRow *recorded,
                              const TraceRow *replayed);

/* Supplied infrastructure; students do not edit these functions. */
uint64_t monotonic_ns(void);
const char *rx_verdict_name(RxVerdict verdict);
const char *verdict_name(Verdict verdict);
const char *reason_name(Reason reason);
void simulator_reset(Simulator *simulator);
bool simulator_commit_from_writer(Simulator *simulator,
                                  const ActuatorWriter *writer,
                                  const ArmCommand *command,
                                  SafetyDecision decision);

#endif
