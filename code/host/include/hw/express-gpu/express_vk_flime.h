/*
 * FLIME host-side metadata, planning, and recovery core.
 *
 * Copyright (c) 2026
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This module deliberately does not implement the guest descriptor shadow.
 * It consumes results produced by an authoritative generic shadow/template
 * matcher and maintains only the host-visible metadata needed for capability
 * negotiation, profiling, forwarding plans, and replay de-duplication.
 */
#ifndef HW_EXPRESS_GPU_EXPRESS_VK_FLIME_H
#define HW_EXPRESS_GPU_EXPRESS_VK_FLIME_H

#include <glib.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct ExpressVkFlimeManager ExpressVkFlimeManager;
typedef struct ExpressVkFlimeSession ExpressVkFlimeSession;

/* "FLIM" when stored as little-endian bytes. */
#define EXPRESS_VK_FLIME_WIRE_MAGIC UINT32_C(0x4d494c46)
#define EXPRESS_VK_FLIME_CONTROL_MAGIC UINT32_C(0x434c4646) /* "FFLC" */
#define EXPRESS_VK_FLIME_CAPS_REPLY_MAGIC UINT32_C(0x52434646) /* "FFCR" */

#define EXPRESS_VK_FLIME_PROTOCOL_MAJOR 1
#define EXPRESS_VK_FLIME_PROTOCOL_MINOR 1

#define EXPRESS_VK_FLIME_WIRE_HEADER_SIZE 64
#define EXPRESS_VK_FLIME_WIRE_CAPS_SIZE 40
#define EXPRESS_VK_FLIME_WIRE_PROFILE_SIZE 32
#define EXPRESS_VK_FLIME_WIRE_UNIT_SIZE 48
#define EXPRESS_VK_FLIME_WIRE_CHUNK_SIZE 48
#define EXPRESS_VK_FLIME_WIRE_PROGRESS_SIZE 32
#define EXPRESS_VK_FLIME_WIRE_PLAN_ACK_SIZE 16
#define EXPRESS_VK_FLIME_WIRE_INTERVAL_SIZE 8
#define EXPRESS_VK_FLIME_CONTROL_HEADER_SIZE 64
#define EXPRESS_VK_FLIME_CONTROL_BOUNDARY_SIZE 16
#define EXPRESS_VK_FLIME_CAPS_REPLY_SIZE 48

/* Values stated by the paper. */
#define EXPRESS_VK_FLIME_PAPER_DISPATCHES_PER_UNIT 10
#define EXPRESS_VK_FLIME_PAPER_REPLAN_PERIODS 32
#define EXPRESS_VK_FLIME_PAPER_INITIAL_FAST_PROFILES 3

/*
 * Engineering defaults: the paper does not prescribe these values.  They are
 * intentionally configurable through ExpressVkFlimeConfig.
 */
#define EXPRESS_VK_FLIME_DEFAULT_EWMA_ALPHA 0.20
#define EXPRESS_VK_FLIME_DEFAULT_HISTORY_LIMIT 256
#define EXPRESS_VK_FLIME_DEFAULT_MAX_UNITS 256
#define EXPRESS_VK_FLIME_DEFAULT_MAX_CHUNKS 8
#define EXPRESS_VK_FLIME_DEFAULT_MAX_LEDGER_ENTRIES 65536

/* Protocol/allocation ceilings, independent of negotiated engineering defaults. */
#define EXPRESS_VK_FLIME_HARD_MAX_UNITS 4096
#define EXPRESS_VK_FLIME_HARD_MAX_CHUNKS 64
#define EXPRESS_VK_FLIME_HARD_MAX_HISTORY 65536
#define EXPRESS_VK_FLIME_HARD_MAX_LEDGER_ENTRIES (UINT32_C(1) << 20)

typedef enum ExpressVkFlimeError {
    EXPRESS_VK_FLIME_ERROR_INVALID_ARGUMENT,
    EXPRESS_VK_FLIME_ERROR_INVALID_PACKET,
    EXPRESS_VK_FLIME_ERROR_UNSUPPORTED_VERSION,
    EXPRESS_VK_FLIME_ERROR_LIMIT,
    EXPRESS_VK_FLIME_ERROR_OOM,
    EXPRESS_VK_FLIME_ERROR_STATE,
    EXPRESS_VK_FLIME_ERROR_OVERFLOW,
    EXPRESS_VK_FLIME_ERROR_NOT_NEGOTIATED,
} ExpressVkFlimeError;

#define EXPRESS_VK_FLIME_ERROR express_vk_flime_error_quark()
GQuark express_vk_flime_error_quark(void);

typedef enum ExpressVkFlimeCapability {
    EXPRESS_VK_FLIME_CAP_PROGRESSIVE_METADATA = UINT64_C(1) << 0,
    EXPRESS_VK_FLIME_CAP_DIRECT_ROUTING = UINT64_C(1) << 1,
    EXPRESS_VK_FLIME_CAP_RECOVERY_LEDGER = UINT64_C(1) << 2,
    EXPRESS_VK_FLIME_CAP_ADAPTIVE_FORWARDING = UINT64_C(1) << 3,
    EXPRESS_VK_FLIME_CAP_UNIT_PROFILING = UINT64_C(1) << 4,
    EXPRESS_VK_FLIME_CAP_CHUNK_PROFILING = UINT64_C(1) << 5,
    EXPRESS_VK_FLIME_CAP_EARLY_RELEASE = UINT64_C(1) << 6,
    EXPRESS_VK_FLIME_CAP_CONTROL_PLAN = UINT64_C(1) << 7,
} ExpressVkFlimeCapability;

#define EXPRESS_VK_FLIME_CAP_ALL                                           \
    (EXPRESS_VK_FLIME_CAP_PROGRESSIVE_METADATA |                           \
     EXPRESS_VK_FLIME_CAP_DIRECT_ROUTING |                                 \
     EXPRESS_VK_FLIME_CAP_RECOVERY_LEDGER |                                \
     EXPRESS_VK_FLIME_CAP_ADAPTIVE_FORWARDING |                            \
     EXPRESS_VK_FLIME_CAP_UNIT_PROFILING |                                 \
     EXPRESS_VK_FLIME_CAP_CHUNK_PROFILING |                                \
     EXPRESS_VK_FLIME_CAP_EARLY_RELEASE |                                  \
     EXPRESS_VK_FLIME_CAP_CONTROL_PLAN)

typedef enum ExpressVkFlimeWireType {
    EXPRESS_VK_FLIME_WIRE_CAPABILITIES = 1,
    EXPRESS_VK_FLIME_WIRE_PERIOD_BEGIN = 2,
    EXPRESS_VK_FLIME_WIRE_PROFILE_PERIOD = 3,
    EXPRESS_VK_FLIME_WIRE_PROGRESS_EVENT = 4,
    EXPRESS_VK_FLIME_WIRE_PLAN_ACK = 5,
    EXPRESS_VK_FLIME_WIRE_SESSION_RESET = 6,
    EXPRESS_VK_FLIME_WIRE_SESSION_TEARDOWN = 7,
    EXPRESS_VK_FLIME_WIRE_INTERVAL_SIGNATURE = 8,
} ExpressVkFlimeWireType;

/*
 * Fixed, versioned little-endian layouts.  Never cast untrusted bytes to one
 * of these structs; express_vk_flime_session_ingest_wire() uses checked,
 * unaligned-safe decoding.  The declarations document the ABI for a guest.
 */
typedef struct ExpressVkFlimeWireHeader {
    uint32_t magic_le;
    uint16_t major_le;
    uint16_t minor_le;
    uint16_t type_le;
    uint16_t header_bytes_le;
    uint32_t packet_bytes_le;
    uint32_t flags_le;
    uint32_t record_count_le;
    uint64_t process_id_le;
    uint64_t stream_id_le;
    uint64_t period_id_le;
    uint64_t plan_epoch_le;
    uint64_t reserved0_le;
} ExpressVkFlimeWireHeader;

typedef struct ExpressVkFlimeWireCapabilities {
    uint16_t min_major_le;
    uint16_t min_minor_le;
    uint16_t max_major_le;
    uint16_t max_minor_le;
    uint64_t capabilities_le;
    uint32_t max_units_le;
    uint32_t max_chunks_le;
    uint32_t dispatches_per_unit_le;
    uint32_t replan_periods_le;
    uint64_t reserved0_le;
} ExpressVkFlimeWireCapabilities;

typedef struct ExpressVkFlimeWireProfile {
    uint32_t unit_count_le;
    uint32_t chunk_count_le;
    uint32_t period_flags_le;
    uint32_t reserved0_le;
    uint64_t period_elapsed_ns_le;
    uint64_t reserved1_le;
} ExpressVkFlimeWireProfile;

typedef struct ExpressVkFlimeWireUnit {
    uint32_t unit_index_le;
    uint32_t flags_le;
    uint32_t dispatch_end_le;
    uint32_t reserved0_le;
    uint64_t template_offset_le;
    uint64_t encoded_bytes_le;
    uint64_t guest_prepare_ns_le;
    /* Guest must write zero; host realization time is injected locally. */
    uint64_t host_realize_ns_le;
} ExpressVkFlimeWireUnit;

typedef struct ExpressVkFlimeWireChunk {
    uint32_t chunk_index_le;
    uint32_t first_unit_le;
    uint32_t unit_past_end_le;
    uint32_t flags_le;
    uint64_t guest_handoff_ns_le;
    /* Guest must write zero; both values are owned by host-side timing. */
    uint64_t host_handoff_ns_le;
    uint64_t host_realize_ns_le;
    uint64_t completion_ns_le;
} ExpressVkFlimeWireChunk;

typedef enum ExpressVkFlimeWireProgressFlag {
    EXPRESS_VK_FLIME_WIRE_PROGRESS_MATCH_SUCCEEDED = 1u << 0,
    EXPRESS_VK_FLIME_WIRE_PROGRESS_GENERIC_SHADOW_RAN = 1u << 1,
} ExpressVkFlimeWireProgressFlag;

typedef struct ExpressVkFlimeWireProgress {
    uint16_t event_le;
    uint16_t flags_le;
    uint32_t template_entries_le;
    uint64_t completed_period_id_le;
    uint64_t reserved0_le;
    uint64_t reserved1_le;
} ExpressVkFlimeWireProgress;

typedef enum ExpressVkFlimeWirePlanAckFlag {
    EXPRESS_VK_FLIME_WIRE_PLAN_INSTALLED = 1u << 0,
} ExpressVkFlimeWirePlanAckFlag;

typedef struct ExpressVkFlimeWirePlanAck {
    uint32_t flags_le;
    uint32_t reserved0_le;
    uint64_t installed_period_le;
} ExpressVkFlimeWirePlanAck;

typedef struct ExpressVkFlimeControlHeader {
    uint32_t magic_le;
    uint16_t major_le;
    uint16_t minor_le;
    uint16_t header_bytes_le;
    uint16_t flags_le;
    uint32_t control_bytes_le;
    uint64_t process_id_le;
    uint64_t stream_id_le;
    uint64_t plan_epoch_le;
    uint64_t apply_period_le;
    uint32_t boundary_count_le;
    uint32_t reserved0_le;
    uint64_t capabilities_le;
} ExpressVkFlimeControlHeader;

typedef struct ExpressVkFlimeControlBoundary {
    uint32_t unit_past_end_le;
    uint32_t flags_le;
    uint64_t template_offset_le;
} ExpressVkFlimeControlBoundary;

typedef enum ExpressVkFlimeCapsReplyStatus {
    EXPRESS_VK_FLIME_CAPS_REPLY_NEGOTIATED = 0,
    EXPRESS_VK_FLIME_CAPS_REPLY_LEGACY = 1,
} ExpressVkFlimeCapsReplyStatus;

/* Fixed-size little-endian reply to a capability query. */
typedef struct ExpressVkFlimeCapsReply {
    uint32_t magic_le;
    uint16_t major_le;
    uint16_t minor_le;
    uint16_t reply_bytes_le;
    uint16_t status_le;
    uint32_t reserved0_le;
    uint64_t capabilities_le;
    uint32_t max_units_le;
    uint32_t max_chunks_le;
    uint32_t dispatches_per_unit_le;
    uint32_t replan_periods_le;
    uint64_t reserved1_le;
} ExpressVkFlimeCapsReply;

typedef enum ExpressVkFlimeControlFlag {
    EXPRESS_VK_FLIME_CONTROL_LEGACY_FALLBACK = 1u << 0,
    EXPRESS_VK_FLIME_CONTROL_PLAN_VALID = 1u << 1,
    EXPRESS_VK_FLIME_CONTROL_REQUEST_FINE_PROFILE = 1u << 2,
} ExpressVkFlimeControlFlag;

typedef struct ExpressVkFlimeConfig {
    uint64_t host_capabilities;
    uint32_t history_limit;
    uint32_t max_units;
    uint32_t max_chunks;
    uint32_t max_ledger_entries;
    uint32_t dispatches_per_unit;
    uint32_t replan_periods;
    double ewma_alpha;
} ExpressVkFlimeConfig;

typedef struct ExpressVkFlimePeerCaps {
    uint16_t min_major;
    uint16_t min_minor;
    uint16_t max_major;
    uint16_t max_minor;
    uint64_t capabilities;
    uint32_t max_units;
    uint32_t max_chunks;
    uint32_t dispatches_per_unit;
    uint32_t replan_periods;
} ExpressVkFlimePeerCaps;

typedef struct ExpressVkFlimeNegotiated {
    uint16_t major;
    uint16_t minor;
    uint64_t capabilities;
    uint32_t max_units;
    uint32_t max_chunks;
    uint32_t dispatches_per_unit;
    uint32_t replan_periods;
    bool legacy_fallback;
} ExpressVkFlimeNegotiated;

typedef enum ExpressVkFlimePeriodFlag {
    EXPRESS_VK_FLIME_PERIOD_SINGLE_FLUSH = 1u << 0,
    EXPRESS_VK_FLIME_PERIOD_FINE_PROFILE = 1u << 1,
    EXPRESS_VK_FLIME_PERIOD_STABLE_FAST = 1u << 2,
    EXPRESS_VK_FLIME_PERIOD_FORCE_REPLAN = 1u << 3,
} ExpressVkFlimePeriodFlag;

typedef enum ExpressVkFlimeUnitFlag {
    /* The final (possibly shorter than ten-dispatch) planning unit. */
    EXPRESS_VK_FLIME_UNIT_FINAL = 1u << 0,
} ExpressVkFlimeUnitFlag;

typedef struct ExpressVkFlimeUnitSample {
    uint32_t unit_index;
    uint32_t flags;
    uint32_t dispatch_end;
    uint64_t template_offset;
    uint64_t encoded_bytes;
    uint64_t guest_prepare_ns;
    uint64_t host_realize_ns;
} ExpressVkFlimeUnitSample;

typedef struct ExpressVkFlimeChunkSample {
    uint32_t chunk_index;
    uint32_t first_unit;
    uint32_t unit_past_end;
    uint32_t flags;
    uint64_t guest_handoff_ns;
    uint64_t host_handoff_ns;
    uint64_t host_realize_ns;
    uint64_t completion_ns;
} ExpressVkFlimeChunkSample;

typedef struct ExpressVkFlimePlanBoundary {
    uint32_t unit_past_end;
    uint32_t flags;
    uint64_t template_offset;
} ExpressVkFlimePlanBoundary;

typedef struct ExpressVkFlimePlanInfo {
    uint64_t epoch;
    uint64_t apply_period;
    uint64_t predicted_completion_ns;
    uint32_t unit_count;
    uint32_t chunk_count;
    bool valid;
    bool pending;
} ExpressVkFlimePlanInfo;

typedef struct ExpressVkFlimePeriodInfo {
    bool open;
    uint64_t current_period_id;
    uint64_t last_period_id;
    uint32_t flags;
} ExpressVkFlimePeriodInfo;

/* A metadata mirror only; it is not an authoritative guest shadow state. */
typedef enum ExpressVkFlimeProgressStage {
    EXPRESS_VK_FLIME_STAGE_DETECT,
    EXPRESS_VK_FLIME_STAGE_LEARN,
    EXPRESS_VK_FLIME_STAGE_MATCH,
    EXPRESS_VK_FLIME_STAGE_FAST,
    EXPRESS_VK_FLIME_STAGE_RECOVER,
    EXPRESS_VK_FLIME_STAGE_LEGACY,
} ExpressVkFlimeProgressStage;

typedef struct ExpressVkFlimeProgressInfo {
    ExpressVkFlimeProgressStage stage;
    uint32_t candidate_period_intervals;
    uint32_t template_entries;
    uint64_t completed_fast_periods;
    uint64_t mismatches;
} ExpressVkFlimeProgressInfo;

typedef enum ExpressVkFlimeProgressEvent {
    EXPRESS_VK_FLIME_PROGRESS_LEARN_COMPLETE,
    EXPRESS_VK_FLIME_PROGRESS_MATCH_COMPLETE,
    EXPRESS_VK_FLIME_PROGRESS_FAST_PERIOD_COMPLETE,
    EXPRESS_VK_FLIME_PROGRESS_MISMATCH,
    EXPRESS_VK_FLIME_PROGRESS_RECOVERY_COMPLETE,
} ExpressVkFlimeProgressEvent;

typedef enum ExpressVkFlimeLedgerState {
    EXPRESS_VK_FLIME_LEDGER_UNKNOWN,
    EXPRESS_VK_FLIME_LEDGER_PREPARED,
    EXPRESS_VK_FLIME_LEDGER_READY,
    EXPRESS_VK_FLIME_LEDGER_RELEASE_IN_FLIGHT,
    EXPRESS_VK_FLIME_LEDGER_COMMIT_IN_FLIGHT,
    EXPRESS_VK_FLIME_LEDGER_RELEASED,
    EXPRESS_VK_FLIME_LEDGER_COMMITTED,
    EXPRESS_VK_FLIME_LEDGER_DISCARDED,
    EXPRESS_VK_FLIME_LEDGER_REPLAY_IN_FLIGHT,
    EXPRESS_VK_FLIME_LEDGER_REPLAY_EMITTED,
} ExpressVkFlimeLedgerState;

typedef struct ExpressVkFlimeRecoveryStats {
    uint32_t discarded_unreleased;
    uint32_t retained_released;
    uint32_t retained_committed;
} ExpressVkFlimeRecoveryStats;

void express_vk_flime_config_init(ExpressVkFlimeConfig *config);

ExpressVkFlimeManager *express_vk_flime_manager_new(
    const ExpressVkFlimeConfig *config, GError **error);
void express_vk_flime_manager_free(ExpressVkFlimeManager *manager);

ExpressVkFlimeSession *express_vk_flime_manager_acquire(
    ExpressVkFlimeManager *manager, uint64_t process_id, uint64_t stream_id,
    GError **error);
bool express_vk_flime_manager_remove(ExpressVkFlimeManager *manager,
                                     uint64_t process_id,
                                     uint64_t stream_id);

ExpressVkFlimeSession *express_vk_flime_session_ref(
    ExpressVkFlimeSession *session);
void express_vk_flime_session_unref(ExpressVkFlimeSession *session);

uint64_t express_vk_flime_session_process_id(ExpressVkFlimeSession *session);
uint64_t express_vk_flime_session_stream_id(ExpressVkFlimeSession *session);

bool express_vk_flime_session_negotiate(ExpressVkFlimeSession *session,
                                        const ExpressVkFlimePeerCaps *peer,
                                        ExpressVkFlimeNegotiated *result,
                                        GError **error);
void express_vk_flime_session_get_negotiated(ExpressVkFlimeSession *session,
                                             ExpressVkFlimeNegotiated *result);
bool express_vk_flime_session_is_legacy(ExpressVkFlimeSession *session);

bool express_vk_flime_session_encode_caps_reply(
    ExpressVkFlimeSession *session, void *reply, size_t reply_capacity,
    size_t *reply_bytes, GError **error);

/*
 * Strict decoder for the fixed little-endian wire ABI above.  A guest
 * PROFILE_PERIOD installs guest-owned samples but intentionally leaves the
 * period open; the host merges local timings with profile_host_*() and then
 * calls period_end().
 */
bool express_vk_flime_session_ingest_wire(ExpressVkFlimeSession *session,
                                          const void *packet,
                                          size_t packet_bytes,
                                          ExpressVkFlimeNegotiated *caps_reply,
                                          GError **error);

/* Serializes the pending plan when present, otherwise the installed plan. */
size_t express_vk_flime_session_control_size(ExpressVkFlimeSession *session);
bool express_vk_flime_session_encode_control(ExpressVkFlimeSession *session,
                                             void *control,
                                             size_t control_capacity,
                                             size_t *control_bytes,
                                             GError **error);

bool express_vk_flime_session_period_begin(ExpressVkFlimeSession *session,
                                           uint64_t period_id,
                                           uint32_t flags,
                                           bool *installed_new_plan,
                                           GError **error);
void express_vk_flime_session_get_period(ExpressVkFlimeSession *session,
                                         ExpressVkFlimePeriodInfo *info);
bool express_vk_flime_session_validate_open_period(
    ExpressVkFlimeSession *session, uint64_t period_id,
    uint32_t required_flags, GError **error);
bool express_vk_flime_session_validate_recovery_period(
    ExpressVkFlimeSession *session, uint64_t period_id, GError **error);
bool express_vk_flime_session_profile_unit(
    ExpressVkFlimeSession *session,
    const ExpressVkFlimeUnitSample *sample,
    GError **error);
bool express_vk_flime_session_profile_chunk(
    ExpressVkFlimeSession *session,
    const ExpressVkFlimeChunkSample *sample,
    GError **error);
/* Merge host-owned timestamps after a guest PROFILE_PERIOD wire packet. */
bool express_vk_flime_session_profile_host_unit(
    ExpressVkFlimeSession *session, uint32_t unit_index,
    uint64_t host_realize_ns, GError **error);
bool express_vk_flime_session_profile_host_chunk(
    ExpressVkFlimeSession *session, uint32_t chunk_index,
    uint64_t host_handoff_ns, uint64_t host_realize_ns,
    uint64_t completion_ns, GError **error);
bool express_vk_flime_session_period_end(ExpressVkFlimeSession *session,
                                         uint64_t elapsed_ns,
                                         bool successful,
                                         GError **error);
bool express_vk_flime_session_force_replan(ExpressVkFlimeSession *session,
                                           GError **error);
/* Run only from a host worker; this performs the O(K n^2) DP. */
bool express_vk_flime_session_run_pending_planner(
    ExpressVkFlimeSession *session, bool *published, GError **error);
bool express_vk_flime_session_planner_due(ExpressVkFlimeSession *session);
bool express_vk_flime_session_reset(ExpressVkFlimeSession *session,
                                    GError **error);
bool express_vk_flime_session_take_teardown_request(
    ExpressVkFlimeSession *session);

/*
 * Copies an immutable snapshot while holding the session lock.  On a small
 * boundary buffer, false is returned and *needed_boundaries receives the
 * required count; this caller sizing error does not force legacy mode.
 */
bool express_vk_flime_session_copy_plan(
    ExpressVkFlimeSession *session, bool pending,
    ExpressVkFlimePlanInfo *info,
    ExpressVkFlimePlanBoundary *boundaries, size_t boundary_capacity,
    size_t *needed_boundaries, GError **error);

bool express_vk_flime_session_is_candidate_dispatch(
    ExpressVkFlimeSession *session, uint32_t dispatch_ordinal);

bool express_vk_flime_session_note_interval(
    ExpressVkFlimeSession *session, uint64_t structural_signature,
    bool *candidate_found, GError **error);
bool express_vk_flime_session_progress(
    ExpressVkFlimeSession *session, ExpressVkFlimeProgressEvent event,
    bool match_succeeded, bool authoritative_generic_shadow_ran,
    uint32_t template_entries, GError **error);
void express_vk_flime_session_get_progress(ExpressVkFlimeSession *session,
                                           ExpressVkFlimeProgressInfo *info);

/*
 * Recovery ledger state transitions.  should_emit is true exactly once for
 * an update that the caller must emit; repeated release/commit/replay calls
 * are safely suppressed.
 */
bool express_vk_flime_session_ledger_prepare(
    ExpressVkFlimeSession *session, uint64_t update_id,
    uint64_t submission_id, uint64_t template_offset, bool *inserted,
    GError **error);
bool express_vk_flime_session_ledger_mark_ready(
    ExpressVkFlimeSession *session, uint64_t update_id, GError **error);
bool express_vk_flime_session_ledger_release(
    ExpressVkFlimeSession *session, uint64_t update_id, bool *should_emit,
    GError **error);
bool express_vk_flime_session_ledger_begin_release(
    ExpressVkFlimeSession *session, uint64_t update_id, bool *should_emit,
    GError **error);
bool express_vk_flime_session_ledger_complete_release(
    ExpressVkFlimeSession *session, uint64_t update_id, bool emitted,
    GError **error);
bool express_vk_flime_session_ledger_begin_release_batch(
    ExpressVkFlimeSession *session, const uint64_t *update_ids,
    size_t update_count, bool *emit_mask, GError **error);
bool express_vk_flime_session_ledger_complete_release_batch(
    ExpressVkFlimeSession *session, const uint64_t *update_ids,
    const bool *emit_mask, size_t update_count, bool emitted,
    GError **error);
bool express_vk_flime_session_ledger_commit(
    ExpressVkFlimeSession *session, uint64_t update_id, bool *should_emit,
    GError **error);
bool express_vk_flime_session_ledger_begin_recovery(
    ExpressVkFlimeSession *session, ExpressVkFlimeRecoveryStats *stats,
    GError **error);
bool express_vk_flime_session_ledger_replay(
    ExpressVkFlimeSession *session, uint64_t update_id,
    uint64_t submission_id, bool *should_emit, GError **error);
bool express_vk_flime_session_ledger_begin_replay(
    ExpressVkFlimeSession *session, uint64_t update_id,
    uint64_t submission_id, bool *should_emit, GError **error);
bool express_vk_flime_session_ledger_complete_replay(
    ExpressVkFlimeSession *session, uint64_t update_id, bool emitted,
    GError **error);
bool express_vk_flime_session_ledger_begin_replay_batch(
    ExpressVkFlimeSession *session, const uint64_t *update_ids,
    const uint64_t *submission_ids, size_t update_count,
    bool *emit_mask, GError **error);
bool express_vk_flime_session_ledger_complete_replay_batch(
    ExpressVkFlimeSession *session, const uint64_t *update_ids,
    const bool *emit_mask, size_t update_count, bool emitted,
    GError **error);
/*
 * Validate a synchronous fallback packet against the complete host-known
 * ledger suffix and derive its exactly-once emit mask atomically.  An already
 * emitted prefix may be omitted, but no prepared/discarded suffix entry may
 * be skipped or reordered; previously unknown ids are accepted only after
 * the known suffix.
 */
bool express_vk_flime_session_ledger_filter_fallback(
    ExpressVkFlimeSession *session, const uint64_t *update_ids,
    size_t update_count, bool *emit_mask, GError **error);
bool express_vk_flime_session_ledger_checkpoint(
    ExpressVkFlimeSession *session, uint64_t period_id, GError **error);
/*
 * Read-only pre-submit recovery gate.  entry_count includes every host-known
 * ledger entry for submission_id, including an omitted discarded suffix.
 */
bool express_vk_flime_session_ledger_submission_ready_to_commit(
    ExpressVkFlimeSession *session, uint64_t submission_id,
    uint32_t *entry_count, GError **error);
ExpressVkFlimeLedgerState express_vk_flime_session_ledger_state(
    ExpressVkFlimeSession *session, uint64_t update_id);

#endif /* HW_EXPRESS_GPU_EXPRESS_VK_FLIME_H */
