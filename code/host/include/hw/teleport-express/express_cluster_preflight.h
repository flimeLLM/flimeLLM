/*
 * Bounds-only validation for the Teleport Express clustered-call envelope.
 *
 * The existing cluster wire format is host-native uint64_t fields:
 *   call_id, parameter_count, then { length, save-buffer offset } pairs.
 * This helper deliberately performs no dispatch and never dereferences the
 * save buffer, so the entire batch can be rejected before any inner call runs.
 */
#ifndef HW_TELEPORT_EXPRESS_CLUSTER_PREFLIGHT_H
#define HW_TELEPORT_EXPRESS_CLUSTER_PREFLIGHT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EXPRESS_CLUSTER_MAX_PARAMETERS 32u
/*
 * Legacy dgles batches predate FLIME and may contain far more than 64 calls.
 * Keep a bounded compatibility envelope for the host allocations, while the
 * tighter FLIME-owned limits below are applied only after a route call is
 * identified by the side-effect-free scan.
 */
#define EXPRESS_CLUSTER_MAX_CALL_STREAM_BYTES (1024u * 1024u)
#define EXPRESS_CLUSTER_MAX_SAVE_BYTES (1024u * 1024u)

/* Match the audited FLIME guest helper's batch-flush ceilings. */
#define EXPRESS_CLUSTER_FLIME_MAX_CALL_STREAM_BYTES (64u * 1024u)
#define EXPRESS_CLUSTER_FLIME_MAX_SAVE_BYTES (1024u * 1024u)
#define EXPRESS_CLUSTER_FLIME_MAX_CALLS 64u

typedef enum ExpressClusterPreflightStatus {
    EXPRESS_CLUSTER_PREFLIGHT_OK = 0,
    EXPRESS_CLUSTER_PREFLIGHT_INVALID_ARGUMENT,
    EXPRESS_CLUSTER_PREFLIGHT_RESOURCE_LIMIT,
    EXPRESS_CLUSTER_PREFLIGHT_UNALIGNED_STREAM,
    EXPRESS_CLUSTER_PREFLIGHT_TRUNCATED_RECORD,
    EXPRESS_CLUSTER_PREFLIGHT_TOO_MANY_PARAMETERS,
    EXPRESS_CLUSTER_PREFLIGHT_WRONG_DEVICE,
    EXPRESS_CLUSTER_PREFLIGHT_UNSUPPORTED_CALL_FLAGS,
    EXPRESS_CLUSTER_PREFLIGHT_NESTED_CLUSTER,
    EXPRESS_CLUSTER_PREFLIGHT_PERSISTENT_CALL,
    EXPRESS_CLUSTER_PREFLIGHT_INVALID_NULL_PARAMETER,
    EXPRESS_CLUSTER_PREFLIGHT_PARAMETER_TOO_LARGE,
    EXPRESS_CLUSTER_PREFLIGHT_PARAMETER_OUT_OF_BOUNDS,
    EXPRESS_CLUSTER_PREFLIGHT_ROUTE_SUBMIT_CONFLICT,
} ExpressClusterPreflightStatus;

typedef struct ExpressClusterPreflightInfo {
    bool has_flime_route;
} ExpressClusterPreflightInfo;

bool express_cluster_preflight(
    const uint8_t *call_stream, size_t call_stream_bytes, size_t save_bytes,
    ExpressClusterPreflightStatus *status, size_t *error_offset);

bool express_cluster_preflight_with_info(
    const uint8_t *call_stream, size_t call_stream_bytes, size_t save_bytes,
    ExpressClusterPreflightInfo *info,
    ExpressClusterPreflightStatus *status, size_t *error_offset);

bool express_cluster_envelope_within_limits(size_t call_stream_bytes,
                                            size_t save_bytes);

bool express_cluster_flime_envelope_within_limits(size_t call_stream_bytes,
                                                  size_t save_bytes);

const char *express_cluster_preflight_status_string(
    ExpressClusterPreflightStatus status);

#endif /* HW_TELEPORT_EXPRESS_CLUSTER_PREFLIGHT_H */
