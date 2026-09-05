/*
 * Pure validator for Teleport Express clustered calls.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/teleport-express/express_cluster_preflight.h"

#define EXPRESS_CLUSTER_FUN_ID_MASK UINT64_C(0x00ffffff)
#define EXPRESS_CLUSTER_CALL_FLAGS_MASK UINT64_C(0xff000000)
#define EXPRESS_CLUSTER_DEVICE_ID_SHIFT 32
#define EXPRESS_CLUSTER_GPU_DEVICE_ID UINT32_C(1)
#define EXPRESS_CLUSTER_FUN_ID_CLUSTER UINT32_C(9999)
#define EXPRESS_CLUSTER_FUN_ID_QUEUE_SUBMIT UINT32_C(1235)
#define EXPRESS_CLUSTER_FUN_ID_QUEUE_SUBMIT2 UINT32_C(1236)
#define EXPRESS_CLUSTER_FUN_ID_REGISTER_MAPPED_MEMORY UINT32_C(1902)
#define EXPRESS_CLUSTER_FUN_ID_FLIME_CONTROL UINT32_C(1906)
#define EXPRESS_CLUSTER_FUN_ID_FLIME_ROUTE UINT32_C(1907)

static uint64_t express_cluster_get_u64(const uint8_t *bytes)
{
    uint64_t value;

    memcpy(&value, bytes, sizeof(value));
    return value;
}

static bool express_cluster_preflight_fail(
    ExpressClusterPreflightStatus value, size_t offset,
    ExpressClusterPreflightStatus *status, size_t *error_offset)
{
    if (status != NULL) {
        *status = value;
    }
    if (error_offset != NULL) {
        *error_offset = offset;
    }
    return false;
}

bool express_cluster_envelope_within_limits(size_t call_stream_bytes,
                                            size_t save_bytes)
{
    return call_stream_bytes <= EXPRESS_CLUSTER_MAX_CALL_STREAM_BYTES &&
           save_bytes <= EXPRESS_CLUSTER_MAX_SAVE_BYTES;
}

bool express_cluster_flime_envelope_within_limits(size_t call_stream_bytes,
                                                  size_t save_bytes)
{
    return call_stream_bytes <=
               EXPRESS_CLUSTER_FLIME_MAX_CALL_STREAM_BYTES &&
           save_bytes <= EXPRESS_CLUSTER_FLIME_MAX_SAVE_BYTES;
}

bool express_cluster_preflight_with_info(
    const uint8_t *call_stream, size_t call_stream_bytes, size_t save_bytes,
    ExpressClusterPreflightInfo *info,
    ExpressClusterPreflightStatus *status, size_t *error_offset)
{
    size_t cursor = 0;
    size_t call_count = 0;
    size_t first_legacy_null_offset = SIZE_MAX;
    bool saw_route = false;
    bool saw_submit = false;

    if (status != NULL) {
        *status = EXPRESS_CLUSTER_PREFLIGHT_OK;
    }
    if (error_offset != NULL) {
        *error_offset = 0;
    }
    if (info != NULL) {
        memset(info, 0, sizeof(*info));
    }
    if (call_stream == NULL && call_stream_bytes != 0) {
        return express_cluster_preflight_fail(
            EXPRESS_CLUSTER_PREFLIGHT_INVALID_ARGUMENT, 0, status,
            error_offset);
    }
    if (!express_cluster_envelope_within_limits(call_stream_bytes,
                                                save_bytes)) {
        return express_cluster_preflight_fail(
            EXPRESS_CLUSTER_PREFLIGHT_RESOURCE_LIMIT, 0, status,
            error_offset);
    }
    if ((call_stream_bytes & (sizeof(uint64_t) - 1)) != 0) {
        return express_cluster_preflight_fail(
            EXPRESS_CLUSTER_PREFLIGHT_UNALIGNED_STREAM, call_stream_bytes,
            status, error_offset);
    }

    while (cursor < call_stream_bytes) {
        uint64_t call_id;
        uint64_t parameter_count;
        uint32_t device_id;
        uint32_t function_id;
        size_t record_bytes;
        size_t i;

        call_count++;
        if (call_stream_bytes - cursor < 2 * sizeof(uint64_t)) {
            return express_cluster_preflight_fail(
                EXPRESS_CLUSTER_PREFLIGHT_TRUNCATED_RECORD, cursor, status,
                error_offset);
        }
        call_id = express_cluster_get_u64(call_stream + cursor);
        parameter_count = express_cluster_get_u64(
            call_stream + cursor + sizeof(uint64_t));
        device_id = (uint32_t)(call_id >> EXPRESS_CLUSTER_DEVICE_ID_SHIFT);
        function_id = (uint32_t)(call_id & EXPRESS_CLUSTER_FUN_ID_MASK);

        /*
         * Cluster dispatch runs directly through the GPU decoder.  It neither
         * redistributes another device's call nor provides the transport-side
         * completion path required by either synchronization flag.
         */
        if (device_id != EXPRESS_CLUSTER_GPU_DEVICE_ID) {
            return express_cluster_preflight_fail(
                EXPRESS_CLUSTER_PREFLIGHT_WRONG_DEVICE, cursor, status,
                error_offset);
        }
        if ((call_id & EXPRESS_CLUSTER_CALL_FLAGS_MASK) != 0) {
            return express_cluster_preflight_fail(
                EXPRESS_CLUSTER_PREFLIGHT_UNSUPPORTED_CALL_FLAGS, cursor,
                status, error_offset);
        }

        if (function_id == EXPRESS_CLUSTER_FUN_ID_CLUSTER) {
            return express_cluster_preflight_fail(
                EXPRESS_CLUSTER_PREFLIGHT_NESTED_CLUSTER, cursor, status,
                error_offset);
        }
        /*
         * Control installs the second parameter as a persistent shared page.
         * An inner cluster parameter points into a temporary host copy, so it
         * can never safely serve as that sink.
         */
        if (function_id ==
                EXPRESS_CLUSTER_FUN_ID_REGISTER_MAPPED_MEMORY ||
            function_id == EXPRESS_CLUSTER_FUN_ID_FLIME_CONTROL) {
            return express_cluster_preflight_fail(
                EXPRESS_CLUSTER_PREFLIGHT_PERSISTENT_CALL, cursor, status,
                error_offset);
        }
        if (parameter_count > EXPRESS_CLUSTER_MAX_PARAMETERS) {
            return express_cluster_preflight_fail(
                EXPRESS_CLUSTER_PREFLIGHT_TOO_MANY_PARAMETERS,
                cursor + sizeof(uint64_t), status, error_offset);
        }
        if (parameter_count >
            (SIZE_MAX - 2 * sizeof(uint64_t)) /
                (2 * sizeof(uint64_t))) {
            return express_cluster_preflight_fail(
                EXPRESS_CLUSTER_PREFLIGHT_TOO_MANY_PARAMETERS,
                cursor + sizeof(uint64_t), status, error_offset);
        }
        record_bytes = 2 * sizeof(uint64_t) +
            (size_t)parameter_count * 2 * sizeof(uint64_t);
        if (record_bytes > call_stream_bytes - cursor) {
            return express_cluster_preflight_fail(
                EXPRESS_CLUSTER_PREFLIGHT_TRUNCATED_RECORD, cursor, status,
                error_offset);
        }

        for (i = 0; i < (size_t)parameter_count; i++) {
            size_t pair_offset =
                cursor + 2 * sizeof(uint64_t) +
                i * 2 * sizeof(uint64_t);
            uint64_t length = express_cluster_get_u64(
                call_stream + pair_offset);
            uint64_t data_offset = express_cluster_get_u64(
                call_stream + pair_offset + sizeof(uint64_t));

            /*
             * The legacy dgles encoder preserves nominal lengths for NULL
             * pointers and may preserve a non-NULL pointer for a zero-sized
             * argument.  create_call_from_cluster() intentionally uses only
             * the offset to reconstruct pointer nullness, so both encodings
             * are valid for a legacy cluster.  The audited FLIME helper uses
             * canonical pairs; remember a mismatch and reject it only if this
             * scan later identifies the envelope as FLIME-owned.
             */
            if ((length == 0) != (data_offset == 0) &&
                first_legacy_null_offset == SIZE_MAX) {
                first_legacy_null_offset = pair_offset;
            }
            if (data_offset != 0 && length > INT_MAX) {
                return express_cluster_preflight_fail(
                    EXPRESS_CLUSTER_PREFLIGHT_PARAMETER_TOO_LARGE,
                    pair_offset, status, error_offset);
            }
            if (data_offset != 0 &&
                (data_offset > (uint64_t)save_bytes ||
                 length > (uint64_t)save_bytes - data_offset)) {
                return express_cluster_preflight_fail(
                    EXPRESS_CLUSTER_PREFLIGHT_PARAMETER_OUT_OF_BOUNDS,
                    pair_offset, status, error_offset);
            }
        }

        saw_route |= function_id == EXPRESS_CLUSTER_FUN_ID_FLIME_ROUTE;
        if (info != NULL) {
            info->has_flime_route = saw_route;
        }
        saw_submit |= function_id == EXPRESS_CLUSTER_FUN_ID_QUEUE_SUBMIT ||
                      function_id == EXPRESS_CLUSTER_FUN_ID_QUEUE_SUBMIT2;
        if (saw_route && saw_submit) {
            return express_cluster_preflight_fail(
                EXPRESS_CLUSTER_PREFLIGHT_ROUTE_SUBMIT_CONFLICT, cursor,
                status, error_offset);
        }
        cursor += record_bytes;
    }

    if (saw_route &&
        (!express_cluster_flime_envelope_within_limits(call_stream_bytes,
                                                       save_bytes) ||
         call_count > EXPRESS_CLUSTER_FLIME_MAX_CALLS)) {
        return express_cluster_preflight_fail(
            EXPRESS_CLUSTER_PREFLIGHT_RESOURCE_LIMIT, 0, status,
            error_offset);
    }
    if (saw_route && first_legacy_null_offset != SIZE_MAX) {
        return express_cluster_preflight_fail(
            EXPRESS_CLUSTER_PREFLIGHT_INVALID_NULL_PARAMETER,
            first_legacy_null_offset, status, error_offset);
    }

    return true;
}

bool express_cluster_preflight(
    const uint8_t *call_stream, size_t call_stream_bytes, size_t save_bytes,
    ExpressClusterPreflightStatus *status, size_t *error_offset)
{
    return express_cluster_preflight_with_info(
        call_stream, call_stream_bytes, save_bytes, NULL, status,
        error_offset);
}

const char *express_cluster_preflight_status_string(
    ExpressClusterPreflightStatus status)
{
    switch (status) {
    case EXPRESS_CLUSTER_PREFLIGHT_OK:
        return "ok";
    case EXPRESS_CLUSTER_PREFLIGHT_INVALID_ARGUMENT:
        return "invalid argument";
    case EXPRESS_CLUSTER_PREFLIGHT_RESOURCE_LIMIT:
        return "cluster allocation or call-count limit exceeded";
    case EXPRESS_CLUSTER_PREFLIGHT_UNALIGNED_STREAM:
        return "call stream is not uint64 aligned";
    case EXPRESS_CLUSTER_PREFLIGHT_TRUNCATED_RECORD:
        return "truncated call record or trailing bytes";
    case EXPRESS_CLUSTER_PREFLIGHT_TOO_MANY_PARAMETERS:
        return "parameter count exceeds decoder capacity";
    case EXPRESS_CLUSTER_PREFLIGHT_WRONG_DEVICE:
        return "inner call is not addressed to the GPU device";
    case EXPRESS_CLUSTER_PREFLIGHT_UNSUPPORTED_CALL_FLAGS:
        return "inner call uses unsupported middle-byte flags";
    case EXPRESS_CLUSTER_PREFLIGHT_NESTED_CLUSTER:
        return "nested cluster call";
    case EXPRESS_CLUSTER_PREFLIGHT_PERSISTENT_CALL:
        return "call retains temporary cluster storage";
    case EXPRESS_CLUSTER_PREFLIGHT_INVALID_NULL_PARAMETER:
        return "parameter offset and length disagree on nullness";
    case EXPRESS_CLUSTER_PREFLIGHT_PARAMETER_TOO_LARGE:
        return "parameter length exceeds Guest_Mem capacity";
    case EXPRESS_CLUSTER_PREFLIGHT_PARAMETER_OUT_OF_BOUNDS:
        return "parameter lies outside the save buffer";
    case EXPRESS_CLUSTER_PREFLIGHT_ROUTE_SUBMIT_CONFLICT:
        return "FLIME route and queue submit share one cluster";
    default:
        return "unknown cluster preflight error";
    }
}
