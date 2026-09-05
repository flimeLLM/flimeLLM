/*
 * FLIME express-protocol bridge.
 *
 * Metadata/control policy lives in express_vk_flime.c.  This file owns the
 * transport trust boundary, guest-handle translation, progressive descriptor
 * forwarding, submit gating, and process-lifetime plumbing.
 */
#include "qemu/osdep.h"

#include "hw/express-gpu/express_vk_flime_bridge.h"
#include "hw/express-gpu/express_vk_handle_mapping.h"
#include "hw/virtio/virtio.h"
#include "exec/cpu-common.h"
#include "qemu/atomic.h"
#include "sysemu/dma.h"

#include <stdarg.h>

G_STATIC_ASSERT(sizeof(ExpressVkFlimeRouteHeader) ==
                EXPRESS_VK_FLIME_ROUTE_HEADER_SIZE);
G_STATIC_ASSERT(sizeof(ExpressVkFlimeRouteRecord) ==
                EXPRESS_VK_FLIME_ROUTE_RECORD_SIZE);
G_STATIC_ASSERT(sizeof(ExpressVkFlimeRouteBuffer) ==
                EXPRESS_VK_FLIME_ROUTE_BUFFER_SIZE);
G_STATIC_ASSERT(sizeof(ExpressVkFlimeRouteImage) ==
                EXPRESS_VK_FLIME_ROUTE_IMAGE_SIZE);
G_STATIC_ASSERT(sizeof(ExpressVkFlimeRouteTexel) ==
                EXPRESS_VK_FLIME_ROUTE_TEXEL_SIZE);
G_STATIC_ASSERT(sizeof(ExpressVkFlimeRouteReply) ==
                 EXPRESS_VK_FLIME_ROUTE_REPLY_SIZE);
G_STATIC_ASSERT(sizeof(ExpressVkFlimeControlPageHeader) ==
                 EXPRESS_VK_FLIME_CONTROL_PAGE_HEADER_SIZE);

typedef struct ExpressVkFlimeBridgeSession ExpressVkFlimeBridgeSession;
typedef struct ExpressVkFlimePendingSubmission ExpressVkFlimePendingSubmission;

/*
 * Bound hostile stream fan-out even before the core's per-session allocation
 * limits apply.  The per-process cap also prevents one guest process from
 * consuming the global table.
 */
#define EXPRESS_VK_FLIME_MAX_BRIDGE_SESSIONS 256u
#define EXPRESS_VK_FLIME_MAX_PROCESS_SESSIONS 32u

typedef struct ExpressVkFlimeRouteHostHeader {
    uint16_t flags;
    uint32_t record_count;
    uint32_t submission_record_count;
    uint32_t chunk_index;
    uint64_t process_id;
    uint64_t stream_id;
    uint64_t period_id;
    uint64_t plan_epoch;
    uint64_t submission_id;
    uint64_t guest_device;
    uint64_t guest_queue;
    uint32_t first_unit;
    uint32_t unit_past_end;
    uint64_t template_offset;
} ExpressVkFlimeRouteHostHeader;

typedef struct ExpressVkFlimeRecordView {
    const uint8_t *payload;
    uint64_t update_id;
    uint64_t template_offset;
    uint64_t guest_dst_set;
    uint32_t dst_binding;
    uint32_t dst_array_element;
    uint32_t descriptor_count;
    VkDescriptorType descriptor_type;
    uint16_t payload_kind;
    uint16_t flags;
    uint32_t payload_bytes;
} ExpressVkFlimeRecordView;

typedef struct ExpressVkFlimeDecodedRoute {
    ExpressVkFlimeRouteHostHeader header;
    VkDevice device;
    VkQueue queue;
    VkWriteDescriptorSet *writes;
    void **payloads;
    uint64_t *update_ids;
    uint64_t *template_offsets;
} ExpressVkFlimeDecodedRoute;

typedef struct ExpressVkFlimeHostChunkTiming {
    uint64_t handoff_ns;
    uint64_t realize_ns;
    bool handoff_valid;
    bool realize_valid;
} ExpressVkFlimeHostChunkTiming;

struct ExpressVkFlimeBridgeSession {
    gint ref_count;
    bool removed;
    uint64_t process_id;
    uint64_t stream_id;
    ExpressVkFlimeSession *session;
    bool queue_bound;
    uint64_t guest_device;
    uint64_t guest_queue;
    VkDevice device;
    VkQueue queue;
    uint64_t last_submission_id;
    uint64_t active_period_id;
    uint32_t active_period_flags;
    uint64_t period_submission_id;
    uint32_t period_submission_record_count;
    bool period_open;
    bool period_submission_executed;
    bool period_submission_committed;
    bool profiled_occurrence_ready;
    uint32_t period_expected_chunk_count;
    uint32_t period_expected_unit_count;
    uint32_t period_chunk_first_unit[EXPRESS_VK_FLIME_HARD_MAX_CHUNKS];
    uint32_t period_chunk_unit_past_end[EXPRESS_VK_FLIME_HARD_MAX_CHUNKS];
    uint64_t period_chunk_template_offset[EXPRESS_VK_FLIME_HARD_MAX_CHUNKS];
    bool period_chunk_seen[EXPRESS_VK_FLIME_HARD_MAX_CHUNKS];
    uint64_t timing_period_id;
    VirtIODevice *control_vdev;
    uint64_t control_sink_address;
    size_t control_sink_capacity;
    uint64_t control_sequence;
    bool planner_queued;
    uint32_t pending_count;
    uint32_t host_work_inflight;
    bool recovery_deferred;
    uint64_t planner_generation;
    ExpressVkFlimeHostChunkTiming
        chunk_timing[EXPRESS_VK_FLIME_HARD_MAX_CHUNKS];
    ExpressVkFlimeBridgeSession *next;
};

struct ExpressVkFlimePendingSubmission {
    gint ref_count;
    bool listed;
    bool complete;
    bool recovery;
    bool release_failed;
    bool recovery_started;
    bool tail_applied;
    bool submit_inflight;
    uint32_t release_inflight;
    uint32_t next_chunk_index;
    uint32_t expected_chunk_count;
    uint32_t received_record_count;
    uint32_t submission_record_count;
    uint32_t update_count;
    uint32_t tail_count;
    uint32_t tail_release_begun;
    uint64_t process_id;
    uint64_t stream_id;
    uint64_t period_id;
    uint64_t plan_epoch;
    uint64_t submission_id;
    uint64_t guest_device;
    uint64_t guest_queue;
    VkDevice device;
    VkQueue queue;
    ExpressVkFlimeBridgeSession *bridge_state;
    ExpressVkFlimeSession *session;
    uint64_t *update_ids;
    uint64_t *tail_update_ids;
    bool *tail_emit_mask;
    VkWriteDescriptorSet *tail_writes;
    void **tail_payloads;
    ExpressVkFlimePendingSubmission *next;
};

struct ExpressVkFlimeReleaseBatch {
    ExpressVkFlimePendingSubmission *owner;
    VkDevice device;
    uint32_t write_count;
    uint32_t chunk_index;
    uint64_t period_id;
    uint64_t submission_id;
    bool recovery;
    bool fallback_flush;
    VkWriteDescriptorSet *writes;
    void **payloads;
    uint64_t *update_ids;
    bool *emit_mask;
};

struct ExpressVkFlimeSubmitBatch {
    ExpressVkFlimePendingSubmission *owner;
};

static GMutex flime_bridge_lock;
static ExpressVkFlimeManager *flime_bridge_manager;
static ExpressVkFlimeBridgeSession *flime_bridge_sessions;
static ExpressVkFlimePendingSubmission *flime_bridge_pending;
static GAsyncQueue *flime_planner_queue;
static GThread *flime_planner_thread;
static bool flime_planner_stopping;
static uint8_t flime_planner_stop_token;

static gpointer flime_planner_worker(gpointer opaque);
static void flime_cancel_session_pending_locked(
    ExpressVkFlimeBridgeSession *state, bool start_recovery);
static bool flime_session_has_inflight_locked(
    ExpressVkFlimeBridgeSession *state);
static void flime_begin_or_defer_session_recovery_locked(
    ExpressVkFlimeBridgeSession *state);

static uint16_t flime_get_u16(const uint8_t *p)
{
    uint16_t value;

    memcpy(&value, p, sizeof(value));
    return GUINT16_FROM_LE(value);
}

static uint32_t flime_get_u32(const uint8_t *p)
{
    uint32_t value;

    memcpy(&value, p, sizeof(value));
    return GUINT32_FROM_LE(value);
}

static uint64_t flime_get_u64(const uint8_t *p)
{
    uint64_t value;

    memcpy(&value, p, sizeof(value));
    return GUINT64_FROM_LE(value);
}

static void flime_put_u16(uint8_t *p, uint16_t value)
{
    value = GUINT16_TO_LE(value);
    memcpy(p, &value, sizeof(value));
}

static void flime_put_u32(uint8_t *p, uint32_t value)
{
    value = GUINT32_TO_LE(value);
    memcpy(p, &value, sizeof(value));
}

static void flime_put_u64(uint8_t *p, uint64_t value)
{
    value = GUINT64_TO_LE(value);
    memcpy(p, &value, sizeof(value));
}

static bool flime_size_mul(size_t a, size_t b, size_t *result)
{
    if (a != 0 && b > SIZE_MAX / a) {
        return false;
    }
    *result = a * b;
    return true;
}

static bool flime_size_add(size_t a, size_t b, size_t *result)
{
    if (b > SIZE_MAX - a) {
        return false;
    }
    *result = a + b;
    return true;
}

static bool flime_bridge_error(GError **error, ExpressVkFlimeError code,
                               const char *format, ...)
{
    va_list ap;
    char *message;

    if (error == NULL || *error != NULL) {
        return false;
    }
    va_start(ap, format);
    message = g_strdup_vprintf(format, ap);
    va_end(ap);
    g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR, code, message);
    g_free(message);
    return false;
}

static void flime_route_reply_init(ExpressVkFlimeRouteReply *reply,
                                   ExpressVkFlimeRouteStatus status,
                                   uint32_t flags,
                                   const ExpressVkFlimeRouteHostHeader *header)
{
    if (reply == NULL) {
        return;
    }
    memset(reply, 0, sizeof(*reply));
    reply->magic_le = GUINT32_TO_LE(EXPRESS_VK_FLIME_ROUTE_REPLY_MAGIC);
    reply->major_le = GUINT16_TO_LE(EXPRESS_VK_FLIME_PROTOCOL_MAJOR);
    reply->minor_le = GUINT16_TO_LE(EXPRESS_VK_FLIME_PROTOCOL_MINOR);
    reply->header_bytes_le = GUINT16_TO_LE(sizeof(*reply));
    reply->status_le = GUINT16_TO_LE(status);
    reply->reply_bytes_le = GUINT32_TO_LE(sizeof(*reply));
    reply->flags_le = GUINT32_TO_LE(flags);
    if (header != NULL) {
        reply->process_id_le = GUINT64_TO_LE(header->process_id);
        reply->stream_id_le = GUINT64_TO_LE(header->stream_id);
        reply->submission_id_le = GUINT64_TO_LE(header->submission_id);
    }
}

static void flime_route_reply_counts(ExpressVkFlimeRouteReply *reply,
                                     uint32_t accepted, uint32_t queued,
                                     uint32_t extra_flags)
{
    uint32_t flags;

    if (reply == NULL) {
        return;
    }
    flags = GUINT32_FROM_LE(reply->flags_le) | extra_flags;
    reply->flags_le = GUINT32_TO_LE(flags);
    reply->accepted_records_le = GUINT32_TO_LE(accepted);
    reply->queued_writes_le = GUINT32_TO_LE(queued);
}

static void flime_encode_legacy_control(uint64_t process_id,
                                        uint64_t stream_id,
                                        void *control_out,
                                        size_t control_capacity,
                                        size_t *control_bytes)
{
    uint8_t *out = control_out;

    if (control_bytes != NULL) {
        *control_bytes = 0;
    }
    if (out == NULL || control_capacity < EXPRESS_VK_FLIME_CONTROL_HEADER_SIZE) {
        return;
    }
    memset(out, 0, EXPRESS_VK_FLIME_CONTROL_HEADER_SIZE);
    flime_put_u32(out + 0, EXPRESS_VK_FLIME_CONTROL_MAGIC);
    flime_put_u16(out + 4, EXPRESS_VK_FLIME_PROTOCOL_MAJOR);
    flime_put_u16(out + 6, EXPRESS_VK_FLIME_PROTOCOL_MINOR);
    flime_put_u16(out + 8, EXPRESS_VK_FLIME_CONTROL_HEADER_SIZE);
    flime_put_u16(out + 10, EXPRESS_VK_FLIME_CONTROL_LEGACY_FALLBACK);
    flime_put_u32(out + 12, EXPRESS_VK_FLIME_CONTROL_HEADER_SIZE);
    flime_put_u64(out + 16, process_id);
    flime_put_u64(out + 24, stream_id);
    if (control_bytes != NULL) {
        *control_bytes = EXPRESS_VK_FLIME_CONTROL_HEADER_SIZE;
    }
}

static ExpressVkFlimeManager *flime_bridge_manager_locked(GError **error)
{
    ExpressVkFlimeConfig config;

    if (flime_bridge_manager != NULL) {
        return flime_bridge_manager;
    }
    if (flime_planner_stopping) {
        flime_bridge_error(error, EXPRESS_VK_FLIME_ERROR_STATE,
                           "FLIME bridge is shutting down");
        return NULL;
    }
    if (flime_planner_queue == NULL) {
        flime_planner_queue = g_async_queue_new();
        flime_planner_thread = g_thread_new("flime-planner",
                                             flime_planner_worker, NULL);
    }
    express_vk_flime_config_init(&config);
    flime_bridge_manager = express_vk_flime_manager_new(&config, error);
    return flime_bridge_manager;
}

static ExpressVkFlimeBridgeSession *flime_find_session_locked(
    uint64_t process_id, uint64_t stream_id)
{
    ExpressVkFlimeBridgeSession *state;

    for (state = flime_bridge_sessions; state != NULL; state = state->next) {
        if (state->process_id == process_id && state->stream_id == stream_id) {
            return state;
        }
    }
    return NULL;
}

static ExpressVkFlimeBridgeSession *flime_session_state_ref(
    ExpressVkFlimeBridgeSession *state)
{
    g_atomic_int_inc(&state->ref_count);
    return state;
}

static void flime_session_state_unref(ExpressVkFlimeBridgeSession *state)
{
    if (state != NULL && g_atomic_int_dec_and_test(&state->ref_count)) {
        if (state->control_vdev != NULL) {
            object_unref(OBJECT(state->control_vdev));
        }
        express_vk_flime_session_unref(state->session);
        g_free(state);
    }
}

static ExpressVkFlimeBridgeSession *flime_get_session(uint64_t process_id,
                                                       uint64_t stream_id,
                                                       bool create,
                                                       GError **error)
{
    ExpressVkFlimeBridgeSession *state;
    ExpressVkFlimeManager *manager;
    unsigned int global_count = 0;
    unsigned int process_count = 0;

    g_mutex_lock(&flime_bridge_lock);
    state = flime_find_session_locked(process_id, stream_id);
    if (state != NULL) {
        flime_session_state_ref(state);
        g_mutex_unlock(&flime_bridge_lock);
        return state;
    }
    if (!create) {
        g_mutex_unlock(&flime_bridge_lock);
        return NULL;
    }
    for (state = flime_bridge_sessions; state != NULL; state = state->next) {
        global_count++;
        if (state->process_id == process_id) {
            process_count++;
        }
    }
    if (global_count >= EXPRESS_VK_FLIME_MAX_BRIDGE_SESSIONS ||
        process_count >= EXPRESS_VK_FLIME_MAX_PROCESS_SESSIONS) {
        g_mutex_unlock(&flime_bridge_lock);
        flime_bridge_error(error, EXPRESS_VK_FLIME_ERROR_LIMIT,
                           "FLIME bridge session limit exceeded");
        return NULL;
    }
    manager = flime_bridge_manager_locked(error);
    if (manager == NULL) {
        g_mutex_unlock(&flime_bridge_lock);
        return NULL;
    }
    state = g_try_new0(ExpressVkFlimeBridgeSession, 1);
    if (state == NULL) {
        g_mutex_unlock(&flime_bridge_lock);
        flime_bridge_error(error, EXPRESS_VK_FLIME_ERROR_OOM,
                           "out of memory creating FLIME bridge session");
        return NULL;
    }
    state->session = express_vk_flime_manager_acquire(manager, process_id,
                                                       stream_id, error);
    if (state->session == NULL) {
        g_free(state);
        g_mutex_unlock(&flime_bridge_lock);
        return NULL;
    }
    state->process_id = process_id;
    state->stream_id = stream_id;
    state->ref_count = 1; /* session-list reference */
    state->next = flime_bridge_sessions;
    flime_bridge_sessions = state;
    flime_session_state_ref(state); /* caller reference */
    g_mutex_unlock(&flime_bridge_lock);
    return state;
}

static void flime_put_session(ExpressVkFlimeBridgeSession *state)
{
    flime_session_state_unref(state);
}

static void flime_control_sink_clear(ExpressVkFlimeBridgeSession *state)
{
    VirtIODevice *vdev;

    if (state == NULL) {
        return;
    }
    vdev = state->control_vdev;
    state->control_vdev = NULL;
    state->control_sink_address = 0;
    state->control_sink_capacity = 0;
    state->control_sequence = 0;
    if (vdev != NULL) {
        object_unref(OBJECT(vdev));
    }
}

/*
 * Remove the session from both bridge and core-manager namespaces.  The
 * return value means that the caller owns one former list reference to drop
 * after releasing flime_bridge_lock.  Independent caller/planner/pending
 * references keep the removed shell alive until their work observes removed.
 */
static bool flime_remove_session_locked(ExpressVkFlimeBridgeSession *state)
{
    ExpressVkFlimeBridgeSession **link;
    bool unlinked = false;

    if (state == NULL || state->removed) {
        return false;
    }
    state->removed = true;
    state->planner_generation++;
    flime_control_sink_clear(state);
    for (link = &flime_bridge_sessions; *link != NULL;
         link = &(*link)->next) {
        if (*link == state) {
            *link = state->next;
            state->next = NULL;
            unlinked = true;
            break;
        }
    }
    if (flime_bridge_manager != NULL) {
        express_vk_flime_manager_remove(flime_bridge_manager,
                                        state->process_id,
                                        state->stream_id);
    }
    return unlinked;
}

static void flime_control_delivery_failed_locked(
    ExpressVkFlimeBridgeSession *state)
{
    flime_control_sink_clear(state);
    flime_cancel_session_pending_locked(state, true);
    /*
     * This helper intentionally does nothing for an idle Detect/Legacy
     * session, while learned or period-owning state enters/defer Recover.
     */
    flime_begin_or_defer_session_recovery_locked(state);
}

static bool flime_control_sink_map(VirtIODevice *vdev, uint64_t address,
                                   size_t capacity, size_t bytes,
                                   uint8_t **page_out,
                                   dma_addr_t *mapped_bytes_out)
{
    dma_addr_t mapped_bytes;
    uint8_t *page;

    if (page_out != NULL) {
        *page_out = NULL;
    }
    if (mapped_bytes_out != NULL) {
        *mapped_bytes_out = 0;
    }
    if (vdev == NULL || vdev->dma_as == NULL || bytes == 0 ||
        bytes > capacity || address > UINT64_MAX - bytes ||
        ((address + 24) & (sizeof(uint64_t) - 1)) != 0) {
        return false;
    }

    mapped_bytes = bytes;
    page = dma_memory_map(vdev->dma_as, (dma_addr_t)address, &mapped_bytes,
                          DMA_DIRECTION_FROM_DEVICE,
                          MEMTXATTRS_UNSPECIFIED);
    if (page == NULL || mapped_bytes != bytes ||
        ((uintptr_t)(page + 24) & (sizeof(uint64_t) - 1)) != 0 ||
        qemu_ram_addr_from_host(page) == RAM_ADDR_INVALID) {
        if (page != NULL) {
            dma_memory_unmap(vdev->dma_as, page, mapped_bytes,
                             DMA_DIRECTION_FROM_DEVICE, 0);
        }
        return false;
    }
    *page_out = page;
    *mapped_bytes_out = mapped_bytes;
    return true;
}

static bool flime_control_sink_validate(
    const ExpressVkFlimeControlSink *sink, size_t required_bytes)
{
    uint8_t *page;
    dma_addr_t mapped_bytes;

    if (sink == NULL || sink->capacity < required_bytes ||
        sink->guest_address > UINT64_MAX - sink->capacity ||
        !flime_control_sink_map(sink->vdev, sink->guest_address,
                                sink->capacity, required_bytes,
                                &page, &mapped_bytes)) {
        return false;
    }
    dma_memory_unmap(sink->vdev->dma_as, page, mapped_bytes,
                     DMA_DIRECTION_FROM_DEVICE, 0);
    return true;
}

static void flime_control_sink_replace_locked(
    ExpressVkFlimeBridgeSession *state,
    const ExpressVkFlimeControlSink *sink)
{
    VirtIODevice *new_vdev;

    g_assert(state != NULL);
    g_assert(sink != NULL);
    g_assert(sink->vdev != NULL);

    new_vdev = sink->vdev;
    object_ref(OBJECT(new_vdev));
    flime_control_sink_clear(state);
    state->control_vdev = new_vdev;
    state->control_sink_address = sink->guest_address;
    state->control_sink_capacity = sink->capacity;
}

static bool flime_publish_payload_locked(ExpressVkFlimeBridgeSession *state,
                                         uint16_t payload_kind,
                                         const void *payload,
                                         size_t payload_bytes)
{
    uint8_t header[EXPRESS_VK_FLIME_CONTROL_PAGE_HEADER_SIZE] = { 0 };
    uint64_t odd_sequence;
    uint64_t even_sequence;
    uint64_t sequence_le;
    uint8_t *page;
    aligned_uint64_t *sequence;
    size_t page_bytes;
    dma_addr_t mapped_bytes;

    if (state == NULL || state->removed || state->control_vdev == NULL ||
        payload == NULL || payload_bytes == 0 || payload_bytes > UINT32_MAX ||
        !flime_size_add(EXPRESS_VK_FLIME_CONTROL_PAGE_HEADER_SIZE,
                        payload_bytes, &page_bytes) ||
        page_bytes > state->control_sink_capacity || page_bytes > UINT32_MAX ||
        !flime_control_sink_map(state->control_vdev,
                                state->control_sink_address,
                                state->control_sink_capacity, page_bytes,
                                &page, &mapped_bytes)) {
        return false;
    }
    sequence = (aligned_uint64_t *)(page + 24);

    odd_sequence = state->control_sequence >= UINT64_MAX - 2 ?
        1 : (state->control_sequence | UINT64_C(1));
    if (odd_sequence == state->control_sequence) {
        odd_sequence += 2;
    }
    even_sequence = odd_sequence + 1;

    flime_put_u32(header + 0, EXPRESS_VK_FLIME_CONTROL_PAGE_MAGIC);
    flime_put_u16(header + 4, EXPRESS_VK_FLIME_PROTOCOL_MAJOR);
    flime_put_u16(header + 6, EXPRESS_VK_FLIME_PROTOCOL_MINOR);
    flime_put_u16(header + 8, EXPRESS_VK_FLIME_CONTROL_PAGE_HEADER_SIZE);
    flime_put_u16(header + 10, payload_kind);
    flime_put_u32(header + 12, page_bytes);
    flime_put_u32(header + 16, payload_bytes);

    sequence_le = GUINT64_TO_LE(odd_sequence);
    qatomic_set_u64(sequence, sequence_le);
    smp_wmb();
    memcpy(page, header, 24);
    memcpy(page + EXPRESS_VK_FLIME_CONTROL_PAGE_HEADER_SIZE, payload,
           payload_bytes);
    smp_wmb();
    sequence_le = GUINT64_TO_LE(even_sequence);
    qatomic_set_u64(sequence, sequence_le);
    state->control_sequence = even_sequence;
    dma_memory_unmap(state->control_vdev->dma_as, page, mapped_bytes,
                     DMA_DIRECTION_FROM_DEVICE, page_bytes);
    return true;
}

static void flime_schedule_planner_locked(ExpressVkFlimeBridgeSession *state)
{
    if (state == NULL || state->removed || flime_planner_stopping ||
        flime_planner_queue == NULL) {
        return;
    }
    state->planner_generation++;
    if (!state->planner_queued) {
        state->planner_queued = true;
        g_async_queue_push(flime_planner_queue,
                           flime_session_state_ref(state));
    }
}

static gpointer flime_planner_worker(gpointer opaque)
{
    (void)opaque;
    for (;;) {
        gpointer item = g_async_queue_pop(flime_planner_queue);
        ExpressVkFlimeBridgeSession *state;

        if (item == &flime_planner_stop_token) {
            break;
        }
        state = item;
        for (;;) {
            GError *local_error = NULL;
            uint64_t generation;
            bool published = false;
            bool removed;
            bool ok;

            g_mutex_lock(&flime_bridge_lock);
            generation = state->planner_generation;
            removed = state->removed;
            g_mutex_unlock(&flime_bridge_lock);
            if (removed) {
                break;
            }

            ok = express_vk_flime_session_run_pending_planner(
                state->session, &published, &local_error);
            g_mutex_lock(&flime_bridge_lock);
            if (state->removed) {
                state->planner_queued = false;
                g_mutex_unlock(&flime_bridge_lock);
                g_clear_error(&local_error);
                break;
            }
            if (generation != state->planner_generation) {
                g_mutex_unlock(&flime_bridge_lock);
                g_clear_error(&local_error);
                continue;
            }
            if (!ok) {
                flime_control_delivery_failed_locked(state);
            } else if (published) {
                size_t capacity =
                    express_vk_flime_session_control_size(state->session);
                uint8_t *control =
                    capacity != 0 ? g_try_malloc(capacity) : NULL;
                size_t control_bytes = 0;
                bool delivered = false;

                /*
                 * Keep generation validation, core encoding and DMA publish
                 * in one bridge-lock critical section.  Every other bridge
                 * path already takes bridge->core locks in that order.
                 */
                if (control != NULL &&
                    express_vk_flime_session_encode_control(
                        state->session, control, capacity, &control_bytes,
                        &local_error)) {
                    delivered = flime_publish_payload_locked(
                        state, EXPRESS_VK_FLIME_CONTROL_PAYLOAD_PLAN,
                        control, control_bytes);
                }
                if (!delivered) {
                    /*
                     * Planner, allocation, encoding, and DMA failures are all
                     * equivalent at the guest boundary: the new plan was not
                     * delivered.  Drop the sink and fail closed rather than
                     * silently leaving a pending or stale plan installed.
                     */
                    flime_control_delivery_failed_locked(state);
                }
                g_free(control);
            }
            g_clear_error(&local_error);
            state->planner_queued = false;
            g_mutex_unlock(&flime_bridge_lock);
            break;
        }
        flime_session_state_unref(state); /* planner-queue reference */
    }
    return NULL;
}

static void flime_decoded_route_clear(ExpressVkFlimeDecodedRoute *decoded)
{
    uint32_t i;

    if (decoded == NULL) {
        return;
    }
    if (decoded->payloads != NULL) {
        for (i = 0; i < decoded->header.record_count; i++) {
            g_free(decoded->payloads[i]);
        }
    }
    g_free(decoded->writes);
    g_free(decoded->payloads);
    g_free(decoded->update_ids);
    g_free(decoded->template_offsets);
    memset(decoded, 0, sizeof(*decoded));
}

static bool flime_descriptor_payload(VkDescriptorType type,
                                     uint16_t *kind, size_t *stride)
{
    switch (type) {
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
        *kind = EXPRESS_VK_FLIME_ROUTE_PAYLOAD_BUFFER;
        *stride = EXPRESS_VK_FLIME_ROUTE_BUFFER_SIZE;
        return true;
    case VK_DESCRIPTOR_TYPE_SAMPLER:
    case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
    case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
    case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
    case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
        *kind = EXPRESS_VK_FLIME_ROUTE_PAYLOAD_IMAGE;
        *stride = EXPRESS_VK_FLIME_ROUTE_IMAGE_SIZE;
        return true;
    case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
    case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
        *kind = EXPRESS_VK_FLIME_ROUTE_PAYLOAD_TEXEL;
        *stride = EXPRESS_VK_FLIME_ROUTE_TEXEL_SIZE;
        return true;
    default:
        /* Inline-uniform, acceleration-structure, mutable, and future pNext
         * descriptor payloads must travel through the generic decoder. */
        return false;
    }
}

static uint64_t flime_map_optional(ExpressVkObjectType type, uint64_t guest,
                                   bool *ok)
{
    uint64_t host;

    if (guest == 0) {
        return 0;
    }
    host = lookup_mapping(type, guest);
    if (host == 0) {
        *ok = false;
    }
    return host;
}

static bool flime_decode_route(uint64_t transport_process_id,
                               const void *packet, size_t packet_bytes,
                               ExpressVkFlimeDecodedRoute *decoded,
                               ExpressVkFlimeRouteStatus *status,
                               GError **error)
{
    const uint8_t *bytes = packet;
    ExpressVkFlimeRecordView *views = NULL;
    size_t records_bytes;
    size_t cursor;
    uint64_t total_elements = 0;
    uint32_t i;
    bool ok = false;

    memset(decoded, 0, sizeof(*decoded));
    *status = EXPRESS_VK_FLIME_ROUTE_INVALID;
    if (bytes == NULL || packet_bytes < EXPRESS_VK_FLIME_ROUTE_HEADER_SIZE ||
        packet_bytes > EXPRESS_VK_FLIME_ROUTE_MAX_PACKET_BYTES) {
        return flime_bridge_error(error, EXPRESS_VK_FLIME_ERROR_INVALID_PACKET,
                                  "FLIME route packet length %zu is invalid",
                                  packet_bytes);
    }

    decoded->header.flags = flime_get_u16(bytes + 10);
    decoded->header.record_count = flime_get_u32(bytes + 16);
    decoded->header.submission_record_count = flime_get_u32(bytes + 20);
    decoded->header.chunk_index = flime_get_u32(bytes + 28);
    decoded->header.process_id = flime_get_u64(bytes + 32);
    decoded->header.stream_id = flime_get_u64(bytes + 40);
    decoded->header.period_id = flime_get_u64(bytes + 48);
    decoded->header.plan_epoch = flime_get_u64(bytes + 56);
    decoded->header.submission_id = flime_get_u64(bytes + 64);
    decoded->header.guest_device = flime_get_u64(bytes + 72);
    decoded->header.guest_queue = flime_get_u64(bytes + 80);
    decoded->header.first_unit = flime_get_u32(bytes + 88);
    decoded->header.unit_past_end = flime_get_u32(bytes + 92);
    decoded->header.template_offset = flime_get_u64(bytes + 96);

    if (flime_get_u32(bytes + 0) != EXPRESS_VK_FLIME_ROUTE_MAGIC ||
        flime_get_u16(bytes + 4) != EXPRESS_VK_FLIME_PROTOCOL_MAJOR ||
        flime_get_u16(bytes + 6) > EXPRESS_VK_FLIME_PROTOCOL_MINOR ||
        flime_get_u16(bytes + 8) != EXPRESS_VK_FLIME_ROUTE_HEADER_SIZE ||
        flime_get_u32(bytes + 12) != packet_bytes) {
        return flime_bridge_error(error, EXPRESS_VK_FLIME_ERROR_INVALID_PACKET,
                                  "FLIME route header magic/version/length mismatch");
    }
    if (decoded->header.process_id != transport_process_id ||
        decoded->header.stream_id == 0 ||
        decoded->header.period_id == 0 ||
        decoded->header.submission_id == 0 ||
        decoded->header.guest_device == 0 ||
        (decoded->header.guest_queue == 0 &&
         !(decoded->header.flags &
           EXPRESS_VK_FLIME_ROUTE_FALLBACK_FLUSH))) {
        return flime_bridge_error(error, EXPRESS_VK_FLIME_ERROR_INVALID_PACKET,
                                  "FLIME route identity is missing or does not match transport process");
    }
    if (decoded->header.flags &
        ~(EXPRESS_VK_FLIME_ROUTE_SUBMISSION_BEGIN |
          EXPRESS_VK_FLIME_ROUTE_SUBMISSION_FINAL |
          EXPRESS_VK_FLIME_ROUTE_RECOVERY_REPLAY |
          EXPRESS_VK_FLIME_ROUTE_PROFILE_BOOTSTRAP |
          EXPRESS_VK_FLIME_ROUTE_FALLBACK_FLUSH)) {
        return flime_bridge_error(error, EXPRESS_VK_FLIME_ERROR_INVALID_PACKET,
                                  "FLIME route flags contain reserved bits");
    }
    if ((decoded->header.flags & EXPRESS_VK_FLIME_ROUTE_SUBMISSION_BEGIN) &&
        decoded->header.chunk_index != 0) {
        return flime_bridge_error(error, EXPRESS_VK_FLIME_ERROR_INVALID_PACKET,
                                  "FLIME BEGIN must have chunk index zero");
    }
    if (decoded->header.flags & EXPRESS_VK_FLIME_ROUTE_FALLBACK_FLUSH) {
        if (decoded->header.flags !=
                (EXPRESS_VK_FLIME_ROUTE_SUBMISSION_BEGIN |
                 EXPRESS_VK_FLIME_ROUTE_SUBMISSION_FINAL |
                 EXPRESS_VK_FLIME_ROUTE_FALLBACK_FLUSH) ||
            decoded->header.plan_epoch != 0 ||
            decoded->header.chunk_index != 0 ||
            decoded->header.first_unit != 0 ||
            decoded->header.unit_past_end != 1 ||
            decoded->header.record_count !=
                decoded->header.submission_record_count) {
            return flime_bridge_error(
                error, EXPRESS_VK_FLIME_ERROR_INVALID_PACKET,
                "invalid FLIME synchronous fallback-flush route");
        }
    } else if (decoded->header.flags &
               EXPRESS_VK_FLIME_ROUTE_RECOVERY_REPLAY) {
        if (decoded->header.flags !=
                (EXPRESS_VK_FLIME_ROUTE_SUBMISSION_BEGIN |
                 EXPRESS_VK_FLIME_ROUTE_SUBMISSION_FINAL |
                 EXPRESS_VK_FLIME_ROUTE_RECOVERY_REPLAY) ||
            decoded->header.plan_epoch != 0 ||
            decoded->header.chunk_index != 0 ||
            decoded->header.first_unit != 0) {
            return flime_bridge_error(
                error, EXPRESS_VK_FLIME_ERROR_INVALID_PACKET,
                "invalid FLIME plan-independent recovery route");
        }
    } else if (decoded->header.flags & EXPRESS_VK_FLIME_ROUTE_SINGLE_FLUSH) {
        if (decoded->header.flags !=
                (EXPRESS_VK_FLIME_ROUTE_SUBMISSION_BEGIN |
                 EXPRESS_VK_FLIME_ROUTE_SUBMISSION_FINAL |
                 EXPRESS_VK_FLIME_ROUTE_SINGLE_FLUSH) ||
            decoded->header.plan_epoch != 0 ||
            decoded->header.chunk_index != 0 ||
            decoded->header.first_unit != 0) {
            return flime_bridge_error(
                error, EXPRESS_VK_FLIME_ERROR_INVALID_PACKET,
                "invalid FLIME plan-independent single-flush route");
        }
    } else if (decoded->header.plan_epoch == 0) {
        return flime_bridge_error(error,
                                  EXPRESS_VK_FLIME_ERROR_INVALID_PACKET,
                                  "planned FLIME route has zero plan epoch");
    }
    if (decoded->header.record_count > EXPRESS_VK_FLIME_ROUTE_MAX_RECORDS ||
        decoded->header.submission_record_count > EXPRESS_VK_FLIME_ROUTE_MAX_RECORDS ||
        decoded->header.record_count > decoded->header.submission_record_count) {
        *status = EXPRESS_VK_FLIME_ROUTE_RESOURCE_LIMIT;
        return flime_bridge_error(error, EXPRESS_VK_FLIME_ERROR_LIMIT,
                                  "FLIME route record limit exceeded");
    }
    records_bytes = packet_bytes - EXPRESS_VK_FLIME_ROUTE_HEADER_SIZE;
    if (flime_get_u32(bytes + 24) != records_bytes) {
        return flime_bridge_error(error, EXPRESS_VK_FLIME_ERROR_INVALID_PACKET,
                                  "FLIME route records length mismatch");
    }
    if (decoded->header.first_unit >= decoded->header.unit_past_end) {
        return flime_bridge_error(error, EXPRESS_VK_FLIME_ERROR_INVALID_PACKET,
                                  "FLIME route planned unit interval is empty");
    }

    if (decoded->header.record_count != 0) {
        views = g_try_new0(ExpressVkFlimeRecordView,
                           decoded->header.record_count);
        if (views == NULL) {
            *status = EXPRESS_VK_FLIME_ROUTE_RESOURCE_LIMIT;
            return flime_bridge_error(error, EXPRESS_VK_FLIME_ERROR_OOM,
                                      "out of memory validating FLIME route records");
        }
    }

    /* Phase one: validate every boundary and size before mapping any handle. */
    cursor = EXPRESS_VK_FLIME_ROUTE_HEADER_SIZE;
    for (i = 0; i < decoded->header.record_count; i++) {
        ExpressVkFlimeRecordView *view = &views[i];
        const uint8_t *record;
        uint16_t expected_kind;
        size_t payload_stride;
        size_t expected_payload;
        size_t expected_record;
        uint32_t j;

        if (cursor > packet_bytes ||
            packet_bytes - cursor < EXPRESS_VK_FLIME_ROUTE_RECORD_SIZE) {
            flime_bridge_error(error, EXPRESS_VK_FLIME_ERROR_INVALID_PACKET,
                               "truncated FLIME route record %u", i);
            goto out;
        }
        record = bytes + cursor;
        view->update_id = flime_get_u64(record + 0);
        view->template_offset = flime_get_u64(record + 8);
        view->guest_dst_set = flime_get_u64(record + 16);
        view->dst_binding = flime_get_u32(record + 24);
        view->dst_array_element = flime_get_u32(record + 28);
        view->descriptor_count = flime_get_u32(record + 32);
        view->descriptor_type = (VkDescriptorType)flime_get_u32(record + 36);
        view->payload_kind = flime_get_u16(record + 40);
        view->flags = flime_get_u16(record + 42);
        view->payload_bytes = flime_get_u32(record + 48);

        if (view->update_id == 0 || view->guest_dst_set == 0 ||
            view->descriptor_count == 0 ||
            view->descriptor_count > UINT32_MAX - view->dst_array_element ||
            (view->flags & ~EXPRESS_VK_FLIME_ROUTE_RECORD_DERIVED) ||
            flime_get_u32(record + 52) != 0 || flime_get_u64(record + 56) != 0) {
            flime_bridge_error(error, EXPRESS_VK_FLIME_ERROR_INVALID_PACKET,
                               "invalid FLIME route record %u", i);
            goto out;
        }
        for (j = 0; j < i; j++) {
            if (views[j].update_id == view->update_id) {
                flime_bridge_error(error, EXPRESS_VK_FLIME_ERROR_INVALID_PACKET,
                                   "duplicate update id inside FLIME route packet");
                goto out;
            }
        }
        if (view->template_offset >= decoded->header.template_offset ||
            (i != 0 && view->template_offset <=
                         views[i - 1].template_offset)) {
            flime_bridge_error(
                error, EXPRESS_VK_FLIME_ERROR_INVALID_PACKET,
                "FLIME route record template offsets are not strictly ordered inside the chunk");
            goto out;
        }
        if (!flime_descriptor_payload(view->descriptor_type,
                                      &expected_kind, &payload_stride)) {
            *status = EXPRESS_VK_FLIME_ROUTE_UNSUPPORTED;
            flime_bridge_error(error, EXPRESS_VK_FLIME_ERROR_INVALID_PACKET,
                               "descriptor type %u requires generic fallback",
                               (uint32_t)view->descriptor_type);
            goto out;
        }
        if (view->payload_kind != expected_kind ||
            !flime_size_mul(view->descriptor_count, payload_stride,
                            &expected_payload) ||
            expected_payload != view->payload_bytes ||
            !flime_size_add(EXPRESS_VK_FLIME_ROUTE_RECORD_SIZE,
                            expected_payload, &expected_record) ||
            flime_get_u32(record + 44) != expected_record ||
            expected_record > packet_bytes - cursor) {
            flime_bridge_error(error, EXPRESS_VK_FLIME_ERROR_INVALID_PACKET,
                               "FLIME route record %u payload size mismatch", i);
            goto out;
        }
        total_elements += view->descriptor_count;
        if (total_elements > EXPRESS_VK_FLIME_ROUTE_MAX_ELEMENTS) {
            *status = EXPRESS_VK_FLIME_ROUTE_RESOURCE_LIMIT;
            flime_bridge_error(error, EXPRESS_VK_FLIME_ERROR_LIMIT,
                               "FLIME route descriptor element limit exceeded");
            goto out;
        }
        view->payload = record + EXPRESS_VK_FLIME_ROUTE_RECORD_SIZE;
        if (view->payload_kind == EXPRESS_VK_FLIME_ROUTE_PAYLOAD_IMAGE) {
            for (j = 0; j < view->descriptor_count; j++) {
                if (flime_get_u32(view->payload +
                                  j * EXPRESS_VK_FLIME_ROUTE_IMAGE_SIZE + 20) != 0) {
                    flime_bridge_error(error, EXPRESS_VK_FLIME_ERROR_INVALID_PACKET,
                                       "FLIME image payload reserved field is nonzero");
                    goto out;
                }
            }
        }
        cursor += expected_record;
    }
    if (cursor != packet_bytes) {
        flime_bridge_error(error, EXPRESS_VK_FLIME_ERROR_INVALID_PACKET,
                           "FLIME route packet has trailing bytes");
        goto out;
    }

    /* Phase two: allocate native payloads and translate every guest handle. */
    decoded->device = (VkDevice)(uintptr_t)lookup_mapping(
        EXPRESS_VK_OBJECT_TYPE_DEVICE, decoded->header.guest_device);
    if (decoded->header.guest_queue != 0) {
        decoded->queue = (VkQueue)(uintptr_t)lookup_mapping(
            EXPRESS_VK_OBJECT_TYPE_QUEUE, decoded->header.guest_queue);
    }
    if (decoded->device == VK_NULL_HANDLE ||
        (!(decoded->header.flags &
           EXPRESS_VK_FLIME_ROUTE_FALLBACK_FLUSH) &&
         decoded->queue == VK_NULL_HANDLE)) {
        flime_bridge_error(error, EXPRESS_VK_FLIME_ERROR_INVALID_PACKET,
                           "FLIME route device or queue handle is unmapped");
        goto out;
    }
    if (decoded->header.record_count != 0) {
        decoded->writes = g_try_new0(VkWriteDescriptorSet,
                                      decoded->header.record_count);
        decoded->payloads = g_try_new0(void *, decoded->header.record_count);
        decoded->update_ids = g_try_new0(uint64_t,
                                         decoded->header.record_count);
        decoded->template_offsets = g_try_new0(uint64_t,
                                                decoded->header.record_count);
        if (decoded->writes == NULL || decoded->payloads == NULL ||
            decoded->update_ids == NULL || decoded->template_offsets == NULL) {
            *status = EXPRESS_VK_FLIME_ROUTE_RESOURCE_LIMIT;
            flime_bridge_error(error, EXPRESS_VK_FLIME_ERROR_OOM,
                               "out of memory decoding FLIME route packet");
            goto out;
        }
    }
    for (i = 0; i < decoded->header.record_count; i++) {
        ExpressVkFlimeRecordView *view = &views[i];
        VkWriteDescriptorSet *write = &decoded->writes[i];
        bool handles_ok = true;
        uint32_t j;

        write->sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write->pNext = NULL;
        write->dstSet = (VkDescriptorSet)(uintptr_t)lookup_mapping(
            EXPRESS_VK_OBJECT_TYPE_DESCRIPTOR_SET, view->guest_dst_set);
        write->dstBinding = view->dst_binding;
        write->dstArrayElement = view->dst_array_element;
        write->descriptorCount = view->descriptor_count;
        write->descriptorType = view->descriptor_type;
        if (write->dstSet == VK_NULL_HANDLE) {
            handles_ok = false;
        }

        if (view->payload_kind == EXPRESS_VK_FLIME_ROUTE_PAYLOAD_BUFFER) {
            VkDescriptorBufferInfo *infos = g_try_new0(
                VkDescriptorBufferInfo, view->descriptor_count);
            decoded->payloads[i] = infos;
            if (infos == NULL) {
                handles_ok = false;
            }
            for (j = 0; infos != NULL && j < view->descriptor_count; j++) {
                const uint8_t *src = view->payload +
                    j * EXPRESS_VK_FLIME_ROUTE_BUFFER_SIZE;
                uint64_t guest_buffer = flime_get_u64(src + 0);
                infos[j].buffer = (VkBuffer)(uintptr_t)flime_map_optional(
                    EXPRESS_VK_OBJECT_TYPE_BUFFER, guest_buffer, &handles_ok);
                infos[j].offset = (VkDeviceSize)flime_get_u64(src + 8);
                infos[j].range = (VkDeviceSize)flime_get_u64(src + 16);
            }
            write->pBufferInfo = infos;
        } else if (view->payload_kind == EXPRESS_VK_FLIME_ROUTE_PAYLOAD_IMAGE) {
            VkDescriptorImageInfo *infos = g_try_new0(
                VkDescriptorImageInfo, view->descriptor_count);
            decoded->payloads[i] = infos;
            if (infos == NULL) {
                handles_ok = false;
            }
            for (j = 0; infos != NULL && j < view->descriptor_count; j++) {
                const uint8_t *src = view->payload +
                    j * EXPRESS_VK_FLIME_ROUTE_IMAGE_SIZE;
                infos[j].sampler = (VkSampler)(uintptr_t)flime_map_optional(
                    EXPRESS_VK_OBJECT_TYPE_SAMPLER,
                    flime_get_u64(src + 0), &handles_ok);
                infos[j].imageView = (VkImageView)(uintptr_t)flime_map_optional(
                    EXPRESS_VK_OBJECT_TYPE_IMAGE_VIEW,
                    flime_get_u64(src + 8), &handles_ok);
                infos[j].imageLayout = (VkImageLayout)flime_get_u32(src + 16);
            }
            write->pImageInfo = infos;
        } else {
            VkBufferView *views_out = g_try_new0(VkBufferView,
                                                  view->descriptor_count);
            decoded->payloads[i] = views_out;
            if (views_out == NULL) {
                handles_ok = false;
            }
            for (j = 0; views_out != NULL && j < view->descriptor_count; j++) {
                const uint8_t *src = view->payload +
                    j * EXPRESS_VK_FLIME_ROUTE_TEXEL_SIZE;
                views_out[j] = (VkBufferView)(uintptr_t)flime_map_optional(
                    EXPRESS_VK_OBJECT_TYPE_BUFFER_VIEW,
                    flime_get_u64(src), &handles_ok);
            }
            write->pTexelBufferView = views_out;
        }
        if (!handles_ok) {
            flime_bridge_error(error, EXPRESS_VK_FLIME_ERROR_INVALID_PACKET,
                               "FLIME route record %u contains an unmapped handle", i);
            goto out;
        }
        decoded->update_ids[i] = view->update_id;
        decoded->template_offsets[i] = view->template_offset;
    }
    ok = true;

out:
    g_free(views);
    if (!ok) {
        flime_decoded_route_clear(decoded);
    }
    return ok;
}

static ExpressVkFlimePendingSubmission *flime_pending_ref(
    ExpressVkFlimePendingSubmission *pending)
{
    g_atomic_int_inc(&pending->ref_count);
    return pending;
}

static void flime_pending_unref(ExpressVkFlimePendingSubmission *pending)
{
    uint32_t i;

    if (pending == NULL ||
        !g_atomic_int_dec_and_test(&pending->ref_count)) {
        return;
    }
    for (i = 0; i < pending->tail_count; i++) {
        g_free(pending->tail_payloads[i]);
    }
    g_free(pending->tail_payloads);
    g_free(pending->tail_writes);
    g_free(pending->tail_update_ids);
    g_free(pending->tail_emit_mask);
    g_free(pending->update_ids);
    express_vk_flime_session_unref(pending->session);
    flime_session_state_unref(pending->bridge_state);
    g_free(pending);
}

static void flime_pending_host_work_begin_locked(
    ExpressVkFlimePendingSubmission *pending)
{
    g_assert(pending != NULL);
    g_assert(pending->bridge_state != NULL);
    g_assert(pending->bridge_state->host_work_inflight != UINT32_MAX);
    pending->bridge_state->host_work_inflight++;
}

static bool flime_session_begin_recovery_locked(
    ExpressVkFlimeBridgeSession *state, ExpressVkFlimeRecoveryStats *stats,
    GError **error)
{
    bool ok;

    g_assert(state != NULL);
    ok = express_vk_flime_session_ledger_begin_recovery(
        state->session, stats, error);
    if (ok) {
        state->planner_generation++;
    }
    return ok;
}

static void flime_begin_or_defer_session_recovery_locked(
    ExpressVkFlimeBridgeSession *state)
{
    ExpressVkFlimeProgressInfo progress;
    ExpressVkFlimeRecoveryStats stats;

    if (state == NULL || state->removed) {
        return;
    }
    express_vk_flime_session_get_progress(state->session, &progress);
    if (progress.stage == EXPRESS_VK_FLIME_STAGE_RECOVER) {
        state->recovery_deferred = false;
        return;
    }
    if (progress.stage != EXPRESS_VK_FLIME_STAGE_LEARN &&
        progress.stage != EXPRESS_VK_FLIME_STAGE_MATCH &&
        progress.stage != EXPRESS_VK_FLIME_STAGE_FAST &&
        !state->period_open && state->pending_count == 0) {
        return;
    }
    if (state->host_work_inflight != 0) {
        if (!state->recovery_deferred) {
            state->planner_generation++;
        }
        state->recovery_deferred = true;
        return;
    }
    state->recovery_deferred =
        !flime_session_begin_recovery_locked(state, &stats, NULL);
}

static void flime_pending_host_work_end_locked(
    ExpressVkFlimePendingSubmission *pending)
{
    ExpressVkFlimeBridgeSession *state;

    g_assert(pending != NULL);
    g_assert(pending->bridge_state != NULL);
    g_assert(pending->bridge_state->host_work_inflight != 0);
    state = pending->bridge_state;
    state->host_work_inflight--;
    if (state->host_work_inflight == 0 && state->recovery_deferred) {
        flime_begin_or_defer_session_recovery_locked(state);
    }
}

static ExpressVkFlimePendingSubmission *flime_find_pending_locked(
    uint64_t process_id, uint64_t stream_id, uint64_t submission_id)
{
    ExpressVkFlimePendingSubmission *pending;

    for (pending = flime_bridge_pending; pending != NULL;
         pending = pending->next) {
        if (pending->process_id == process_id &&
            pending->stream_id == stream_id &&
            pending->submission_id == submission_id) {
            return pending;
        }
    }
    return NULL;
}

static ExpressVkFlimePendingSubmission *flime_find_queue_pending_locked(
    uint64_t process_id, uint64_t guest_queue)
{
    ExpressVkFlimePendingSubmission *pending;

    for (pending = flime_bridge_pending; pending != NULL;
         pending = pending->next) {
        if (pending->process_id == process_id &&
            pending->guest_queue == guest_queue) {
            return pending;
        }
    }
    return NULL;
}

static void flime_append_pending_locked(
    ExpressVkFlimePendingSubmission *pending)
{
    ExpressVkFlimePendingSubmission **link = &flime_bridge_pending;

    while (*link != NULL) {
        link = &(*link)->next;
    }
    g_assert(pending->bridge_state != NULL);
    g_assert(pending->bridge_state->pending_count != UINT32_MAX);
    pending->bridge_state->pending_count++;
    pending->listed = true;
    pending->next = NULL;
    *link = pending;
}

static void flime_unlink_pending_locked(
    ExpressVkFlimePendingSubmission *pending)
{
    ExpressVkFlimePendingSubmission **link;

    if (pending == NULL || !pending->listed) {
        return;
    }
    for (link = &flime_bridge_pending; *link != NULL;
         link = &(*link)->next) {
        if (*link == pending) {
            *link = pending->next;
            pending->next = NULL;
            pending->listed = false;
            g_assert(pending->bridge_state != NULL);
            g_assert(pending->bridge_state->pending_count != 0);
            pending->bridge_state->pending_count--;
            return;
        }
    }
    if (pending->bridge_state != NULL &&
        pending->bridge_state->pending_count != 0) {
        pending->bridge_state->pending_count--;
    }
    pending->listed = false;
}

static void flime_pending_begin_recovery_locked(
    ExpressVkFlimePendingSubmission *pending)
{
    ExpressVkFlimeRecoveryStats stats;

    if (pending == NULL || pending->recovery_started ||
        pending->release_inflight != 0 || pending->submit_inflight) {
        return;
    }
    if (flime_session_begin_recovery_locked(
            pending->bridge_state, &stats, NULL)) {
        pending->recovery_started = true;
    }
}

/*
 * A bad later chunk must not leave an earlier staged prefix permanently
 * blocking the queue.  When the fixed header is trustworthy, cancel only its
 * exact logical submission; otherwise fail closed for this transport process.
 */
static void flime_cancel_route_locked(uint64_t transport_process_id,
                                      const void *packet,
                                      size_t packet_bytes)
{
    const uint8_t *bytes = packet;
    ExpressVkFlimePendingSubmission *pending;
    ExpressVkFlimePendingSubmission *next;
    ExpressVkFlimeBridgeSession *state = NULL;
    uint64_t stream_id = 0;
    uint64_t period_id = 0;
    uint64_t submission_id = 0;
    uint32_t submission_record_count = 0;
    uint16_t flags = 0;
    bool trusted_identity = false;

    if (bytes != NULL && packet_bytes >= EXPRESS_VK_FLIME_ROUTE_HEADER_SIZE &&
        packet_bytes <= EXPRESS_VK_FLIME_ROUTE_MAX_PACKET_BYTES &&
        flime_get_u32(bytes + 0) == EXPRESS_VK_FLIME_ROUTE_MAGIC &&
        flime_get_u16(bytes + 4) == EXPRESS_VK_FLIME_PROTOCOL_MAJOR &&
        flime_get_u16(bytes + 6) <= EXPRESS_VK_FLIME_PROTOCOL_MINOR &&
        flime_get_u16(bytes + 8) == EXPRESS_VK_FLIME_ROUTE_HEADER_SIZE &&
        flime_get_u32(bytes + 12) == packet_bytes &&
        flime_get_u64(bytes + 32) == transport_process_id) {
        flags = flime_get_u16(bytes + 10);
        stream_id = flime_get_u64(bytes + 40);
        period_id = flime_get_u64(bytes + 48);
        submission_id = flime_get_u64(bytes + 64);
        submission_record_count = flime_get_u32(bytes + 20);
        trusted_identity = stream_id != 0 && period_id != 0 &&
            submission_id != 0 &&
            submission_record_count <= EXPRESS_VK_FLIME_ROUTE_MAX_RECORDS;
    }
    if (trusted_identity) {
        state = flime_find_session_locked(transport_process_id, stream_id);
        if (state == NULL || state->removed || !state->period_open ||
            state->active_period_id != period_id) {
            state = NULL;
        } else if (state->period_submission_id == 0 &&
                   (flags & EXPRESS_VK_FLIME_ROUTE_SUBMISSION_BEGIN) &&
                   !(flags & EXPRESS_VK_FLIME_ROUTE_RECOVERY_REPLAY) &&
                   submission_id > state->last_submission_id) {
            /*
             * Pin the occurrence before recovery even when phase-two route
             * allocation or handle translation failed before pending creation.
             */
            state->period_submission_id = submission_id;
            state->period_submission_record_count =
                submission_record_count;
            state->period_submission_committed = false;
            state->last_submission_id = submission_id;
        }
        if (state != NULL &&
            state->period_submission_id != submission_id) {
            state = NULL;
        }
    }

    for (pending = flime_bridge_pending; pending != NULL; pending = next) {
        bool matches;

        next = pending->next;
        matches = pending->process_id == transport_process_id &&
            (!trusted_identity ||
             (pending->stream_id == stream_id &&
              pending->submission_id == submission_id));
        if (!matches) {
            continue;
        }
        pending->release_failed = true;
        flime_unlink_pending_locked(pending);
        flime_pending_begin_recovery_locked(pending);
        flime_pending_unref(pending); /* former pending-list reference */
    }
    if (state != NULL) {
        flime_begin_or_defer_session_recovery_locked(state);
    } else {
        ExpressVkFlimeBridgeSession *candidate;

        /*
         * With no trustworthy active identity, fail closed for every
         * specialized stream owned by this transport process.
         */
        for (candidate = flime_bridge_sessions; candidate != NULL;
             candidate = candidate->next) {
            if (candidate->process_id == transport_process_id) {
                flime_begin_or_defer_session_recovery_locked(candidate);
            }
        }
    }
}

static void flime_cancel_route(uint64_t transport_process_id,
                               const void *packet, size_t packet_bytes)
{
    g_mutex_lock(&flime_bridge_lock);
    flime_cancel_route_locked(transport_process_id, packet, packet_bytes);
    g_mutex_unlock(&flime_bridge_lock);
}

static void flime_cancel_control_process(uint64_t transport_process_id)
{
    ExpressVkFlimeBridgeSession *state;

    g_mutex_lock(&flime_bridge_lock);
    for (state = flime_bridge_sessions; state != NULL; state = state->next) {
        if (!state->removed && state->process_id == transport_process_id) {
            flime_cancel_session_pending_locked(state, true);
            flime_begin_or_defer_session_recovery_locked(state);
        }
    }
    g_mutex_unlock(&flime_bridge_lock);
}

static ExpressVkFlimePendingSubmission *flime_pending_new(
    ExpressVkFlimeBridgeSession *state,
    const ExpressVkFlimeRouteHostHeader *header,
    uint32_t expected_chunk_count, GError **error)
{
    ExpressVkFlimePendingSubmission *pending;

    pending = g_try_new0(ExpressVkFlimePendingSubmission, 1);
    if (pending == NULL) {
        flime_bridge_error(error, EXPRESS_VK_FLIME_ERROR_OOM,
                           "out of memory creating FLIME pending submission");
        return NULL;
    }
    if (header->submission_record_count != 0) {
        pending->update_ids = g_try_new0(uint64_t,
                                         header->submission_record_count);
        pending->tail_writes = g_try_new0(VkWriteDescriptorSet,
                                           header->submission_record_count);
        pending->tail_payloads = g_try_new0(void *,
                                            header->submission_record_count);
        pending->tail_update_ids = g_try_new0(uint64_t,
                                              header->submission_record_count);
        pending->tail_emit_mask = g_try_new0(bool,
                                             header->submission_record_count);
        if (pending->update_ids == NULL || pending->tail_writes == NULL ||
            pending->tail_payloads == NULL ||
            pending->tail_update_ids == NULL ||
            pending->tail_emit_mask == NULL) {
            g_free(pending->update_ids);
            g_free(pending->tail_writes);
            g_free(pending->tail_payloads);
            g_free(pending->tail_update_ids);
            g_free(pending->tail_emit_mask);
            g_free(pending);
            flime_bridge_error(error, EXPRESS_VK_FLIME_ERROR_OOM,
                               "out of memory reserving FLIME submission records");
            return NULL;
        }
    }
    pending->ref_count = 1; /* pending-list reference */
    pending->process_id = header->process_id;
    pending->stream_id = header->stream_id;
    pending->period_id = header->period_id;
    pending->plan_epoch = header->plan_epoch;
    pending->submission_id = header->submission_id;
    pending->guest_device = header->guest_device;
    pending->guest_queue = header->guest_queue;
    pending->device = state->device;
    pending->queue = state->queue;
    pending->bridge_state = flime_session_state_ref(state);
    pending->session = express_vk_flime_session_ref(state->session);
    pending->submission_record_count = header->submission_record_count;
    pending->expected_chunk_count = expected_chunk_count;
    pending->recovery = (header->flags &
                         EXPRESS_VK_FLIME_ROUTE_RECOVERY_REPLAY) != 0;
    return pending;
}

static bool flime_validate_route_plan_locked(
    ExpressVkFlimeBridgeSession *state,
    const ExpressVkFlimeDecodedRoute *decoded,
    uint32_t *chunk_count, ExpressVkFlimeRouteStatus *status,
    GError **error)
{
    const ExpressVkFlimeRouteHostHeader *header = &decoded->header;
    ExpressVkFlimeNegotiated negotiated;
    ExpressVkFlimeProgressInfo progress;
    ExpressVkFlimePlanInfo info;
    ExpressVkFlimePlanBoundary *boundaries = NULL;
    size_t needed = 0;
    uint32_t expected_first;
    uint32_t i;
    uint64_t template_start;
    bool recovery = (header->flags &
                     EXPRESS_VK_FLIME_ROUTE_RECOVERY_REPLAY) != 0;
    bool single_flush = (header->flags &
                         EXPRESS_VK_FLIME_ROUTE_SINGLE_FLUSH) != 0;
    bool ok = false;

    express_vk_flime_session_get_negotiated(state->session, &negotiated);
    if ((!recovery && negotiated.legacy_fallback) ||
        negotiated.major == 0 ||
        (negotiated.capabilities &
         (EXPRESS_VK_FLIME_CAP_DIRECT_ROUTING |
          EXPRESS_VK_FLIME_CAP_RECOVERY_LEDGER)) !=
         (EXPRESS_VK_FLIME_CAP_DIRECT_ROUTING |
          EXPRESS_VK_FLIME_CAP_RECOVERY_LEDGER)) {
        *status = EXPRESS_VK_FLIME_ROUTE_NOT_NEGOTIATED;
        return flime_bridge_error(error,
                                  EXPRESS_VK_FLIME_ERROR_NOT_NEGOTIATED,
                                  "FLIME direct routing was not negotiated");
    }
    if (header->record_count != 0 &&
        !(header->flags & EXPRESS_VK_FLIME_ROUTE_SUBMISSION_FINAL) &&
        !(negotiated.capabilities & EXPRESS_VK_FLIME_CAP_EARLY_RELEASE)) {
        *status = EXPRESS_VK_FLIME_ROUTE_NOT_NEGOTIATED;
        return flime_bridge_error(
            error, EXPRESS_VK_FLIME_ERROR_NOT_NEGOTIATED,
            "FLIME non-final descriptor release was not negotiated");
    }
    express_vk_flime_session_get_progress(state->session, &progress);
    if (recovery || single_flush) {
        bool valid_stage = recovery ?
            progress.stage == EXPRESS_VK_FLIME_STAGE_RECOVER :
            (progress.stage == EXPRESS_VK_FLIME_STAGE_LEARN ||
             progress.stage == EXPRESS_VK_FLIME_STAGE_MATCH ||
             progress.stage == EXPRESS_VK_FLIME_STAGE_FAST);

        if (!valid_stage) {
            *status = EXPRESS_VK_FLIME_ROUTE_BAD_STATE;
            return flime_bridge_error(
                error, EXPRESS_VK_FLIME_ERROR_STATE,
                "FLIME plan-independent route is invalid in this progress stage");
        }
        if (header->unit_past_end > negotiated.max_units ||
            header->template_offset == 0) {
            *status = EXPRESS_VK_FLIME_ROUTE_PLAN_MISMATCH;
            return flime_bridge_error(
                error, EXPRESS_VK_FLIME_ERROR_STATE,
                "FLIME plan-independent route exceeds negotiated unit/template bounds");
        }
        for (i = 0; i < header->record_count; i++) {
            if (decoded->template_offsets[i] >= header->template_offset) {
                *status = EXPRESS_VK_FLIME_ROUTE_PLAN_MISMATCH;
                return flime_bridge_error(
                    error, EXPRESS_VK_FLIME_ERROR_STATE,
                    "FLIME plan-independent record lies outside its template interval");
            }
        }
        *chunk_count = 1;
        return true;
    }
    if (progress.stage != EXPRESS_VK_FLIME_STAGE_FAST) {
        *status = EXPRESS_VK_FLIME_ROUTE_BAD_STATE;
        return flime_bridge_error(error, EXPRESS_VK_FLIME_ERROR_STATE,
                                  "FLIME route is not valid in progress stage %u",
                                  (uint32_t)progress.stage);
    }
    if (negotiated.max_chunks == 0) {
        *status = EXPRESS_VK_FLIME_ROUTE_PLAN_MISMATCH;
        return flime_bridge_error(error, EXPRESS_VK_FLIME_ERROR_STATE,
                                  "FLIME has no negotiated plan chunks");
    }
    boundaries = g_try_new0(ExpressVkFlimePlanBoundary,
                             negotiated.max_chunks);
    if (boundaries == NULL) {
        *status = EXPRESS_VK_FLIME_ROUTE_RESOURCE_LIMIT;
        return flime_bridge_error(error, EXPRESS_VK_FLIME_ERROR_OOM,
                                  "out of memory copying FLIME plan");
    }
    if (!express_vk_flime_session_copy_plan(state->session, false, &info,
                                             boundaries,
                                             negotiated.max_chunks,
                                             &needed, error)) {
        *status = EXPRESS_VK_FLIME_ROUTE_PLAN_MISMATCH;
        goto out;
    }
    if (!info.valid || info.pending || info.chunk_count == 0 ||
        info.chunk_count > negotiated.max_chunks || needed != info.chunk_count ||
        info.epoch != header->plan_epoch ||
        header->period_id < info.apply_period ||
        header->chunk_index >= info.chunk_count) {
        *status = EXPRESS_VK_FLIME_ROUTE_PLAN_MISMATCH;
        flime_bridge_error(error, EXPRESS_VK_FLIME_ERROR_STATE,
                           "FLIME route does not target the active plan");
        goto out;
    }
    expected_first = header->chunk_index == 0 ? 0 :
        boundaries[header->chunk_index - 1].unit_past_end;
    if (header->first_unit != expected_first ||
        header->unit_past_end !=
            boundaries[header->chunk_index].unit_past_end ||
        header->template_offset !=
            boundaries[header->chunk_index].template_offset ||
        ((header->flags & EXPRESS_VK_FLIME_ROUTE_SUBMISSION_BEGIN) != 0) !=
            (header->chunk_index == 0) ||
        ((header->flags & EXPRESS_VK_FLIME_ROUTE_SUBMISSION_FINAL) != 0) !=
            (header->chunk_index + 1 == info.chunk_count)) {
        *status = EXPRESS_VK_FLIME_ROUTE_PLAN_MISMATCH;
        flime_bridge_error(error, EXPRESS_VK_FLIME_ERROR_STATE,
                           "FLIME route chunk boundary or FINAL marker mismatches plan");
        goto out;
    }
    template_start = header->chunk_index == 0 ? 0 :
        boundaries[header->chunk_index - 1].template_offset;
    for (i = 0; i < header->record_count; i++) {
        if (decoded->template_offsets[i] < template_start ||
            decoded->template_offsets[i] >=
                boundaries[header->chunk_index].template_offset) {
            *status = EXPRESS_VK_FLIME_ROUTE_PLAN_MISMATCH;
            flime_bridge_error(
                error, EXPRESS_VK_FLIME_ERROR_STATE,
                "FLIME route record lies outside its active plan chunk");
            goto out;
        }
    }
    *chunk_count = info.chunk_count;
    ok = true;

out:
    g_free(boundaries);
    return ok;
}

typedef struct ExpressVkFlimeControlIdentity {
    uint16_t type;
    uint32_t flags;
    uint64_t process_id;
    uint64_t stream_id;
    uint64_t period_id;
} ExpressVkFlimeControlIdentity;

static bool flime_decode_control_identity(uint64_t transport_process_id,
                                          const void *packet,
                                          size_t packet_bytes,
                                          ExpressVkFlimeControlIdentity *id,
                                          GError **error)
{
    const uint8_t *bytes = packet;

    memset(id, 0, sizeof(*id));
    if (bytes == NULL || packet_bytes < EXPRESS_VK_FLIME_WIRE_HEADER_SIZE ||
        packet_bytes > EXPRESS_VK_FLIME_ROUTE_MAX_PACKET_BYTES ||
        flime_get_u32(bytes + 0) != EXPRESS_VK_FLIME_WIRE_MAGIC ||
        flime_get_u16(bytes + 4) != EXPRESS_VK_FLIME_PROTOCOL_MAJOR ||
        flime_get_u16(bytes + 6) != EXPRESS_VK_FLIME_PROTOCOL_MINOR ||
        flime_get_u16(bytes + 10) != EXPRESS_VK_FLIME_WIRE_HEADER_SIZE ||
        flime_get_u32(bytes + 12) != packet_bytes ||
        flime_get_u64(bytes + 56) != 0) {
        return flime_bridge_error(error,
                                  EXPRESS_VK_FLIME_ERROR_INVALID_PACKET,
                                  "invalid FLIME control wire header");
    }
    id->type = flime_get_u16(bytes + 8);
    id->flags = flime_get_u32(bytes + 16);
    id->process_id = flime_get_u64(bytes + 24);
    id->stream_id = flime_get_u64(bytes + 32);
    id->period_id = flime_get_u64(bytes + 40);
    if (id->process_id != transport_process_id || id->stream_id == 0) {
        return flime_bridge_error(
            error, EXPRESS_VK_FLIME_ERROR_INVALID_PACKET,
            "FLIME control identity does not match transport process");
    }
    return true;
}

static bool flime_session_has_inflight_locked(
    ExpressVkFlimeBridgeSession *state)
{
    /*
     * Pending submissions may be detached by cancellation or teardown while
     * their release/submit owner references are still alive.  Keep the
     * control-plane exclusion invariant on the ref-counted session state,
     * independent of pending-list membership.
     */
    return state->host_work_inflight != 0;
}

static void flime_cancel_session_pending_locked(
    ExpressVkFlimeBridgeSession *state, bool start_recovery)
{
    ExpressVkFlimePendingSubmission *pending;
    ExpressVkFlimePendingSubmission *next;

    for (pending = flime_bridge_pending; pending != NULL; pending = next) {
        next = pending->next;
        if (pending->process_id != state->process_id ||
            pending->stream_id != state->stream_id) {
            continue;
        }
        pending->release_failed = true;
        flime_unlink_pending_locked(pending);
        if (start_recovery) {
            flime_pending_begin_recovery_locked(pending);
        }
        flime_pending_unref(pending); /* former pending-list reference */
    }
}

static void flime_reset_bridge_session_locked(
    ExpressVkFlimeBridgeSession *state)
{
    state->queue_bound = false;
    state->guest_device = 0;
    state->guest_queue = 0;
    state->device = VK_NULL_HANDLE;
    state->queue = VK_NULL_HANDLE;
    state->last_submission_id = 0;
    state->active_period_id = 0;
    state->active_period_flags = 0;
    state->period_submission_id = 0;
    state->period_submission_record_count = 0;
    state->period_open = false;
    state->period_submission_executed = false;
    state->period_submission_committed = false;
    state->profiled_occurrence_ready = false;
    state->recovery_deferred = false;
    state->period_expected_chunk_count = 0;
    state->period_expected_unit_count = 0;
    memset(state->period_chunk_first_unit, 0,
           sizeof(state->period_chunk_first_unit));
    memset(state->period_chunk_unit_past_end, 0,
           sizeof(state->period_chunk_unit_past_end));
    memset(state->period_chunk_template_offset, 0,
           sizeof(state->period_chunk_template_offset));
    memset(state->period_chunk_seen, 0, sizeof(state->period_chunk_seen));
    state->timing_period_id = 0;
    memset(state->chunk_timing, 0, sizeof(state->chunk_timing));
    state->planner_generation++;
}

static bool flime_merge_host_profile_locked(
    ExpressVkFlimeBridgeSession *state, const uint8_t *packet,
    size_t packet_bytes, uint64_t period_id, GError **error)
{
    const uint8_t *profile = packet + EXPRESS_VK_FLIME_WIRE_HEADER_SIZE;
    const uint8_t *units = profile + EXPRESS_VK_FLIME_WIRE_PROFILE_SIZE;
    const uint8_t *chunks;
    uint32_t unit_count = flime_get_u32(profile + 0);
    uint32_t chunk_count = flime_get_u32(profile + 4);
    bool fine_profile =
        (flime_get_u32(profile + 8) &
         EXPRESS_VK_FLIME_PERIOD_FINE_PROFILE) != 0;
    uint64_t elapsed_ns = flime_get_u64(profile + 16);
    size_t unit_bytes;
    uint32_t chunk_index;

    if (!flime_size_mul(unit_count, EXPRESS_VK_FLIME_WIRE_UNIT_SIZE,
                        &unit_bytes) ||
        EXPRESS_VK_FLIME_WIRE_HEADER_SIZE +
            EXPRESS_VK_FLIME_WIRE_PROFILE_SIZE + unit_bytes > packet_bytes) {
        return flime_bridge_error(error,
                                  EXPRESS_VK_FLIME_ERROR_INVALID_PACKET,
                                  "invalid FLIME profile unit table");
    }
    chunks = units + unit_bytes;
    if (chunk_count == 0) {
        express_vk_flime_session_period_end(state->session, elapsed_ns,
                                             false, NULL);
        return flime_bridge_error(error,
                                  EXPRESS_VK_FLIME_ERROR_INVALID_PACKET,
                                  "FLIME profile must contain at least one chunk");
    }
    if (chunk_count != state->period_expected_chunk_count ||
        unit_count != state->period_expected_unit_count) {
        express_vk_flime_session_period_end(state->session, elapsed_ns,
                                             false, NULL);
        return flime_bridge_error(
            error, EXPRESS_VK_FLIME_ERROR_STATE,
            "FLIME profile geometry does not match the submitted occurrence");
    }
    for (chunk_index = 0; chunk_index < chunk_count; chunk_index++) {
        const uint8_t *chunk =
            chunks + chunk_index * EXPRESS_VK_FLIME_WIRE_CHUNK_SIZE;
        uint32_t unit_past_end = flime_get_u32(chunk + 8);
        const uint8_t *last_unit;
        uint64_t last_offset;
        uint64_t last_bytes;
        uint64_t template_boundary;

        if (unit_past_end == 0 || unit_past_end > unit_count) {
            express_vk_flime_session_period_end(state->session, elapsed_ns,
                                                 false, NULL);
            return flime_bridge_error(
                error, EXPRESS_VK_FLIME_ERROR_STATE,
                "FLIME profile chunk has no valid terminal unit");
        }
        last_unit = units +
            (unit_past_end - 1) * EXPRESS_VK_FLIME_WIRE_UNIT_SIZE;
        last_offset = flime_get_u64(last_unit + 16);
        last_bytes = flime_get_u64(last_unit + 24);
        if (last_bytes > UINT64_MAX - last_offset) {
            express_vk_flime_session_period_end(state->session, elapsed_ns,
                                                 false, NULL);
            return flime_bridge_error(error,
                                      EXPRESS_VK_FLIME_ERROR_OVERFLOW,
                                      "FLIME profile template boundary overflow");
        }
        template_boundary = last_offset + last_bytes;

        if (!state->period_chunk_seen[chunk_index] ||
            flime_get_u32(chunk + 4) !=
                state->period_chunk_first_unit[chunk_index] ||
            unit_past_end !=
                state->period_chunk_unit_past_end[chunk_index] ||
            template_boundary !=
                state->period_chunk_template_offset[chunk_index]) {
            express_vk_flime_session_period_end(state->session, elapsed_ns,
                                                 false, NULL);
            return flime_bridge_error(
                error, EXPRESS_VK_FLIME_ERROR_STATE,
                "FLIME profile chunk geometry does not match routed chunks");
        }
    }
    if (state->timing_period_id != period_id) {
        express_vk_flime_session_period_end(state->session, elapsed_ns,
                                             false, NULL);
        return flime_bridge_error(
            error, EXPRESS_VK_FLIME_ERROR_STATE,
            "FLIME profile has no host-local route timing for its period");
    }

    for (chunk_index = 0; chunk_index < chunk_count; chunk_index++) {
        const ExpressVkFlimeHostChunkTiming *timing =
            &state->chunk_timing[chunk_index];
        uint32_t first_unit = flime_get_u32(
            chunks + chunk_index * EXPRESS_VK_FLIME_WIRE_CHUNK_SIZE + 4);
        uint32_t unit_past_end = flime_get_u32(
            chunks + chunk_index * EXPRESS_VK_FLIME_WIRE_CHUNK_SIZE + 8);
        long double total_weight = 0.0L;
        uint64_t distributed = 0;
        uint64_t completion_ns;
        uint32_t unit_index;

        if (chunk_index >= EXPRESS_VK_FLIME_HARD_MAX_CHUNKS ||
            !timing->handoff_valid || !timing->realize_valid) {
            express_vk_flime_session_period_end(state->session, elapsed_ns,
                                                 false, NULL);
            return flime_bridge_error(
                error, EXPRESS_VK_FLIME_ERROR_STATE,
                "FLIME profile is missing a host-owned chunk timing");
        }
        if (timing->realize_ns > UINT64_MAX - timing->handoff_ns) {
            express_vk_flime_session_period_end(state->session, elapsed_ns,
                                                 false, NULL);
            return flime_bridge_error(error,
                                      EXPRESS_VK_FLIME_ERROR_OVERFLOW,
                                      "FLIME host timing overflow");
        }
        completion_ns = timing->handoff_ns + timing->realize_ns;
        if (fine_profile) {
            for (unit_index = first_unit; unit_index < unit_past_end;
                 unit_index++) {
                total_weight += (long double)flime_get_u64(
                    units + unit_index * EXPRESS_VK_FLIME_WIRE_UNIT_SIZE + 24);
            }
            for (unit_index = first_unit; unit_index < unit_past_end;
                 unit_index++) {
                uint64_t share;

                if (unit_index + 1 == unit_past_end) {
                    share = timing->realize_ns - distributed;
                } else if (total_weight > 0.0L) {
                    long double weight = (long double)flime_get_u64(
                        units + unit_index *
                            EXPRESS_VK_FLIME_WIRE_UNIT_SIZE + 24);
                    long double value = (long double)timing->realize_ns *
                        weight / total_weight;

                    share = value >=
                        (long double)(timing->realize_ns - distributed) ?
                        timing->realize_ns - distributed : (uint64_t)value;
                } else {
                    share = (timing->realize_ns - distributed) /
                        (unit_past_end - unit_index);
                }
                distributed += share;
                if (!express_vk_flime_session_profile_host_unit(
                        state->session, unit_index, share, error)) {
                    express_vk_flime_session_period_end(
                        state->session, elapsed_ns, false, NULL);
                    return false;
                }
            }
        }
        if (!express_vk_flime_session_profile_host_chunk(
                state->session, chunk_index, timing->handoff_ns,
                timing->realize_ns, completion_ns, error)) {
            express_vk_flime_session_period_end(state->session, elapsed_ns,
                                                 false, NULL);
            return false;
        }
    }
    if (!express_vk_flime_session_period_end(state->session, elapsed_ns,
                                              true, error)) {
        return false;
    }
    state->period_open = false;
    state->profiled_occurrence_ready = true;
    state->timing_period_id = 0;
    memset(state->chunk_timing, 0, sizeof(state->chunk_timing));
    return true;
}

bool express_vk_flime_bridge_control(uint64_t transport_process_id,
                                     const void *packet,
                                     size_t packet_bytes,
                                     const ExpressVkFlimeControlSink *
                                         new_control_sink,
                                     void *control_out,
                                     size_t control_capacity,
                                     size_t *control_bytes,
                                     GError **error)
{
    ExpressVkFlimeControlIdentity id;
    ExpressVkFlimeBridgeSession *state;
    ExpressVkFlimeNegotiated negotiated;
    bool mismatch = false;
    bool recovery_complete = false;
    bool completion_event = false;
    bool destructive;
    bool teardown;
    bool initial_caps = false;
    bool drop_list_ref = false;
    bool ok;
    uint16_t payload_kind;
    uint16_t progress_event = 0;
    const size_t required_sink = EXPRESS_VK_FLIME_CONTROL_PAGE_HEADER_SIZE +
        EXPRESS_VK_FLIME_CONTROL_HEADER_SIZE +
        EXPRESS_VK_FLIME_HARD_MAX_CHUNKS *
            EXPRESS_VK_FLIME_CONTROL_BOUNDARY_SIZE;

    if (control_bytes != NULL) {
        *control_bytes = 0;
    }
    if (control_out == NULL || control_bytes == NULL ||
        (new_control_sink != NULL &&
         !flime_control_sink_validate(new_control_sink, required_sink))) {
        flime_cancel_control_process(transport_process_id);
        return flime_bridge_error(error,
                                  EXPRESS_VK_FLIME_ERROR_INVALID_ARGUMENT,
                                  "FLIME control page must be aligned direct RAM DMA");
    }
    if (!flime_decode_control_identity(transport_process_id, packet,
                                       packet_bytes, &id, error)) {
        flime_cancel_control_process(transport_process_id);
        return false;
    }
    state = flime_get_session(id.process_id, id.stream_id,
                              id.type == EXPRESS_VK_FLIME_WIRE_CAPABILITIES,
                              error);
    if (state == NULL) {
        flime_encode_legacy_control(id.process_id, id.stream_id, control_out,
                                    control_capacity, control_bytes);
        return false;
    }
    if (id.type == EXPRESS_VK_FLIME_WIRE_PROGRESS_EVENT &&
        packet_bytes >= EXPRESS_VK_FLIME_WIRE_HEADER_SIZE +
                        EXPRESS_VK_FLIME_WIRE_PROGRESS_SIZE) {
        progress_event = flime_get_u16(
            (const uint8_t *)packet + EXPRESS_VK_FLIME_WIRE_HEADER_SIZE);

        mismatch = progress_event == EXPRESS_VK_FLIME_PROGRESS_MISMATCH;
        recovery_complete =
            progress_event == EXPRESS_VK_FLIME_PROGRESS_RECOVERY_COMPLETE;
        completion_event =
            progress_event == EXPRESS_VK_FLIME_PROGRESS_LEARN_COMPLETE ||
            progress_event == EXPRESS_VK_FLIME_PROGRESS_MATCH_COMPLETE ||
            progress_event == EXPRESS_VK_FLIME_PROGRESS_FAST_PERIOD_COMPLETE;
    }
    destructive = mismatch || id.type == EXPRESS_VK_FLIME_WIRE_SESSION_RESET ||
        id.type == EXPRESS_VK_FLIME_WIRE_SESSION_TEARDOWN;

    g_mutex_lock(&flime_bridge_lock);
    if (state->removed || flime_session_has_inflight_locked(state)) {
        g_mutex_unlock(&flime_bridge_lock);
        flime_put_session(state);
        return flime_bridge_error(
            error, EXPRESS_VK_FLIME_ERROR_STATE,
            "FLIME control packet conflicts with host work in flight");
    }
    {
        ExpressVkFlimeNegotiated current;
        ExpressVkFlimeProgressInfo progress;

        express_vk_flime_session_get_negotiated(state->session, &current);
        express_vk_flime_session_get_progress(state->session, &progress);
        /*
         * Creation and this lock acquisition are separate so a concurrent
         * caller may execute the first CAPS transaction on a wrapper created
         * by another thread.  Rollback authority follows the serialized core
         * state observed here, not which caller allocated the wrapper.
         */
        initial_caps =
            id.type == EXPRESS_VK_FLIME_WIRE_CAPABILITIES &&
            current.major == 0;
        if (id.type == EXPRESS_VK_FLIME_WIRE_CAPABILITIES &&
            (state->queue_bound || state->period_open ||
             (current.major != 0 &&
              progress.stage != EXPRESS_VK_FLIME_STAGE_LEGACY))) {
            g_mutex_unlock(&flime_bridge_lock);
            flime_put_session(state);
            return flime_bridge_error(
                error, EXPRESS_VK_FLIME_ERROR_STATE,
                "FLIME capabilities cannot renegotiate an active binding");
        }
        if (id.type == EXPRESS_VK_FLIME_WIRE_PERIOD_BEGIN &&
            (state->period_open || state->profiled_occurrence_ready)) {
            g_mutex_unlock(&flime_bridge_lock);
            flime_put_session(state);
            return flime_bridge_error(
                error, EXPRESS_VK_FLIME_ERROR_STATE,
                "FLIME period begin crosses an unconsumed occurrence");
        }
        if (id.type == EXPRESS_VK_FLIME_WIRE_SESSION_RESET &&
            progress.stage == EXPRESS_VK_FLIME_STAGE_RECOVER) {
            g_mutex_unlock(&flime_bridge_lock);
            flime_put_session(state);
            return flime_bridge_error(
                error, EXPRESS_VK_FLIME_ERROR_STATE,
                "FLIME recovery must complete before session reset");
        }
    }
    if (state->pending_count != 0 && !mismatch &&
        id.type != EXPRESS_VK_FLIME_WIRE_SESSION_TEARDOWN) {
        g_mutex_unlock(&flime_bridge_lock);
        flime_put_session(state);
        return flime_bridge_error(
            error, EXPRESS_VK_FLIME_ERROR_STATE,
            "FLIME control packet crosses an incomplete routed submission");
    }
    if (recovery_complete &&
        (state->active_period_id == 0 ||
         state->period_submission_id == 0 ||
         !state->period_submission_executed ||
         !state->period_submission_committed)) {
        g_mutex_unlock(&flime_bridge_lock);
        flime_put_session(state);
        return flime_bridge_error(
            error, EXPRESS_VK_FLIME_ERROR_STATE,
            "FLIME recovery completion has no committed real submission");
    }
    if (completion_event &&
        (state->period_open || !state->profiled_occurrence_ready ||
         state->active_period_id == 0 ||
         state->period_submission_id == 0 ||
         !state->period_submission_committed ||
         (progress_event == EXPRESS_VK_FLIME_PROGRESS_FAST_PERIOD_COMPLETE &&
          id.period_id != state->active_period_id))) {
        g_mutex_unlock(&flime_bridge_lock);
        flime_put_session(state);
        return flime_bridge_error(
            error, EXPRESS_VK_FLIME_ERROR_STATE,
            "FLIME progress has no successful profiled occurrence to consume");
    }
    if (id.type == EXPRESS_VK_FLIME_WIRE_PROFILE_PERIOD &&
        (!state->period_open ||
         state->active_period_id != id.period_id ||
         state->period_submission_id == 0 ||
         !state->period_submission_committed)) {
        g_mutex_unlock(&flime_bridge_lock);
        flime_put_session(state);
        return flime_bridge_error(
            error, EXPRESS_VK_FLIME_ERROR_STATE,
            "FLIME profile arrived before its period submission committed");
    }
    if (destructive) {
        /* Core performs the matching ledger transition atomically below. */
        flime_cancel_session_pending_locked(state, false);
    }
    ok = express_vk_flime_session_ingest_wire(state->session, packet,
                                               packet_bytes, &negotiated,
                                               error);
    if (ok && id.type == EXPRESS_VK_FLIME_WIRE_PROFILE_PERIOD) {
        ok = flime_merge_host_profile_locked(
            state, packet, packet_bytes, id.period_id, error);
    }
    if (ok) {
        ExpressVkFlimeProgressInfo post_progress;

        express_vk_flime_session_get_progress(state->session, &post_progress);
        if (post_progress.stage == EXPRESS_VK_FLIME_STAGE_RECOVER ||
            recovery_complete) {
            state->planner_generation++;
        }
    }
    if (ok && id.type == EXPRESS_VK_FLIME_WIRE_PERIOD_BEGIN) {
        state->active_period_id = id.period_id;
        state->active_period_flags = id.flags;
        state->period_submission_id = 0;
        state->period_submission_record_count = 0;
        state->period_open = true;
        state->period_submission_executed = false;
        state->period_submission_committed = false;
        state->profiled_occurrence_ready = false;
        state->recovery_deferred = false;
        state->period_expected_chunk_count = 0;
        state->period_expected_unit_count = 0;
        memset(state->period_chunk_first_unit, 0,
               sizeof(state->period_chunk_first_unit));
        memset(state->period_chunk_unit_past_end, 0,
               sizeof(state->period_chunk_unit_past_end));
        memset(state->period_chunk_template_offset, 0,
               sizeof(state->period_chunk_template_offset));
        memset(state->period_chunk_seen, 0,
               sizeof(state->period_chunk_seen));
        state->timing_period_id = 0;
        memset(state->chunk_timing, 0, sizeof(state->chunk_timing));
    }
    if (ok && id.type == EXPRESS_VK_FLIME_WIRE_SESSION_RESET) {
        flime_reset_bridge_session_locked(state);
    }
    if (ok && recovery_complete) {
        /*
         * Core has verified that every recovery-ledger entry committed and
         * closed its period.  Mirror only period-local bridge state; queue
         * identity and the monotonic submission watermark remain bound.
         */
        state->active_period_id = 0;
        state->active_period_flags = 0;
        state->period_submission_id = 0;
        state->period_submission_record_count = 0;
        state->period_open = false;
        state->period_submission_executed = false;
        state->period_submission_committed = false;
        state->profiled_occurrence_ready = false;
        state->recovery_deferred = false;
        state->period_expected_chunk_count = 0;
        state->period_expected_unit_count = 0;
        memset(state->period_chunk_first_unit, 0,
               sizeof(state->period_chunk_first_unit));
        memset(state->period_chunk_unit_past_end, 0,
               sizeof(state->period_chunk_unit_past_end));
        memset(state->period_chunk_template_offset, 0,
               sizeof(state->period_chunk_template_offset));
        memset(state->period_chunk_seen, 0,
               sizeof(state->period_chunk_seen));
        state->timing_period_id = 0;
        memset(state->chunk_timing, 0, sizeof(state->chunk_timing));
    }
    if (ok && completion_event) {
        /* One completed profile occurrence advances at most one stage. */
        state->profiled_occurrence_ready = false;
    }
    if (ok && mismatch) {
        state->profiled_occurrence_ready = false;
    }
    if (!ok) {
        ExpressVkFlimeProgressInfo progress;

        express_vk_flime_session_get_progress(state->session, &progress);
        if (progress.stage == EXPRESS_VK_FLIME_STAGE_LEGACY) {
            /*
             * Core enters Legacy on a wire error only when its ledger is
             * empty, so dropping the binding cannot bypass released work.
             */
            flime_cancel_session_pending_locked(state, false);
            flime_reset_bridge_session_locked(state);
            flime_control_sink_clear(state);
        } else {
            ExpressVkFlimeRecoveryStats stats;

            /*
             * Preserve queue, period and submission identity while the core
             * retains an emitted prefix.  The recovery route must use that
             * same identity before any submit can pass the FINAL gate.
             */
            flime_cancel_session_pending_locked(state, true);
            if (!flime_session_has_inflight_locked(state)) {
                flime_session_begin_recovery_locked(state, &stats, NULL);
            }
        }
        if (initial_caps) {
            /*
             * A failed first CAPS transaction must not consume a bridge/core
             * session slot or make an identical retry look like renegotiation.
             */
            drop_list_ref = flime_remove_session_locked(state);
        }
        g_mutex_unlock(&flime_bridge_lock);
        if (drop_list_ref) {
            flime_session_state_unref(state);
        }
        flime_put_session(state);
        return false;
    }

    if (id.type == EXPRESS_VK_FLIME_WIRE_CAPABILITIES) {
        payload_kind = EXPRESS_VK_FLIME_CONTROL_PAYLOAD_CAPS;
        ok = express_vk_flime_session_encode_caps_reply(
            state->session, control_out, control_capacity, control_bytes,
            error);
    } else {
        payload_kind = EXPRESS_VK_FLIME_CONTROL_PAYLOAD_PLAN;
        ok = express_vk_flime_session_encode_control(
            state->session, control_out, control_capacity, control_bytes,
            error);
    }
    if (!ok) {
        if (initial_caps) {
            drop_list_ref = flime_remove_session_locked(state);
        } else {
            flime_control_delivery_failed_locked(state);
        }
        g_mutex_unlock(&flime_bridge_lock);
        if (drop_list_ref) {
            flime_session_state_unref(state);
        }
        flime_put_session(state);
        return false;
    }
    if (new_control_sink != NULL) {
        flime_control_sink_replace_locked(state, new_control_sink);
    }
    if (!flime_publish_payload_locked(state, payload_kind, control_out,
                                      *control_bytes)) {
        if (initial_caps) {
            /*
             * Roll back the newly negotiated session atomically from both
             * namespaces.  The caller reference below and any independent
             * queued reference keep the removed shell alive safely.
             */
            drop_list_ref = flime_remove_session_locked(state);
        } else {
            flime_control_delivery_failed_locked(state);
        }
        g_mutex_unlock(&flime_bridge_lock);
        if (drop_list_ref) {
            flime_session_state_unref(state);
        }
        flime_put_session(state);
        return flime_bridge_error(
            error, EXPRESS_VK_FLIME_ERROR_STATE,
            "FLIME control-page DMA publication failed");
    }

    if (id.type == EXPRESS_VK_FLIME_WIRE_SESSION_RESET ||
        id.type == EXPRESS_VK_FLIME_WIRE_SESSION_TEARDOWN) {
        flime_control_sink_clear(state);
    }

    teardown = express_vk_flime_session_take_teardown_request(state->session);
    if (teardown) {
        drop_list_ref = flime_remove_session_locked(state);
    } else if (express_vk_flime_session_planner_due(state->session)) {
        flime_schedule_planner_locked(state);
    }
    g_mutex_unlock(&flime_bridge_lock);

    if (drop_list_ref) {
        flime_session_state_unref(state);
    }
    flime_put_session(state);
    return true;
}

bool express_vk_flime_bridge_route(uint64_t transport_process_id,
                                   const void *packet,
                                   size_t packet_bytes,
                                   uint64_t host_receive_ns,
                                   ExpressVkFlimeRouteReply *reply,
                                   ExpressVkFlimeReleaseBatch **release_batch,
                                   GError **error)
{
    ExpressVkFlimeDecodedRoute decoded;
    ExpressVkFlimeBridgeSession *state = NULL;
    ExpressVkFlimePendingSubmission *pending = NULL;
    ExpressVkFlimeReleaseBatch *release = NULL;
    ExpressVkFlimeRouteStatus status = EXPRESS_VK_FLIME_ROUTE_INVALID;
    bool *emit = NULL;
    bool *begun = NULL;
    uint64_t *replay_submission_ids = NULL;
    bool reservation_begun = false;
    bool created = false;
    bool duplicate = false;
    bool failed = false;
    bool drop_session_list_ref = false;
    uint32_t expected_chunks = 0;
    uint32_t emitted = 0;
    uint32_t i;
    bool recovery_route;
    bool fallback_route;

    if (release_batch != NULL) {
        *release_batch = NULL;
    }
    flime_route_reply_init(reply, EXPRESS_VK_FLIME_ROUTE_INVALID,
                           EXPRESS_VK_FLIME_ROUTE_REPLY_FALLBACK_REQUIRED,
                           NULL);
    if (!flime_decode_route(transport_process_id, packet, packet_bytes,
                            &decoded, &status, error)) {
        flime_cancel_route(transport_process_id, packet, packet_bytes);
        flime_route_reply_init(reply, status,
                               EXPRESS_VK_FLIME_ROUTE_REPLY_FALLBACK_REQUIRED |
                               EXPRESS_VK_FLIME_ROUTE_REPLY_RECOVERY_REQUIRED,
                               NULL);
        return false;
    }
    recovery_route = (decoded.header.flags &
                      EXPRESS_VK_FLIME_ROUTE_RECOVERY_REPLAY) != 0;
    fallback_route = (decoded.header.flags &
                      EXPRESS_VK_FLIME_ROUTE_FALLBACK_FLUSH) != 0;
    flime_route_reply_init(reply, EXPRESS_VK_FLIME_ROUTE_INVALID,
                           EXPRESS_VK_FLIME_ROUTE_REPLY_FALLBACK_REQUIRED,
                           &decoded.header);
    state = flime_get_session(decoded.header.process_id,
                               decoded.header.stream_id, false, error);
    if (state == NULL) {
        status = EXPRESS_VK_FLIME_ROUTE_NOT_NEGOTIATED;
        flime_route_reply_init(reply, status,
                               EXPRESS_VK_FLIME_ROUTE_REPLY_FALLBACK_REQUIRED |
                               EXPRESS_VK_FLIME_ROUTE_REPLY_RECOVERY_REQUIRED,
                               &decoded.header);
        flime_decoded_route_clear(&decoded);
        return false;
    }
    if (decoded.header.record_count != 0) {
        emit = g_try_new0(bool, decoded.header.record_count);
        begun = g_try_new0(bool, decoded.header.record_count);
        replay_submission_ids = g_try_new0(uint64_t,
                                           decoded.header.record_count);
        if (emit == NULL || begun == NULL || replay_submission_ids == NULL) {
            status = EXPRESS_VK_FLIME_ROUTE_RESOURCE_LIMIT;
            flime_bridge_error(error, EXPRESS_VK_FLIME_ERROR_OOM,
                               "out of memory staging FLIME ledger decisions");
            failed = true;
            goto out;
        }
    }

    g_mutex_lock(&flime_bridge_lock);
    if (state->removed) {
        status = EXPRESS_VK_FLIME_ROUTE_BAD_STATE;
        flime_bridge_error(error, EXPRESS_VK_FLIME_ERROR_STATE,
                           "FLIME process session is being destroyed");
        failed = true;
        goto unlock;
    }
    if (fallback_route) {
        ExpressVkFlimeNegotiated negotiated;
        ExpressVkFlimeProgressInfo progress;

        express_vk_flime_session_get_negotiated(state->session, &negotiated);
        express_vk_flime_session_get_progress(state->session, &progress);
        if (release_batch == NULL ||
            negotiated.major == 0 || negotiated.legacy_fallback ||
            (negotiated.capabilities &
             (EXPRESS_VK_FLIME_CAP_DIRECT_ROUTING |
              EXPRESS_VK_FLIME_CAP_RECOVERY_LEDGER)) !=
                (EXPRESS_VK_FLIME_CAP_DIRECT_ROUTING |
                 EXPRESS_VK_FLIME_CAP_RECOVERY_LEDGER) ||
            (progress.stage != EXPRESS_VK_FLIME_STAGE_LEARN &&
             progress.stage != EXPRESS_VK_FLIME_STAGE_MATCH &&
             progress.stage != EXPRESS_VK_FLIME_STAGE_FAST &&
             progress.stage != EXPRESS_VK_FLIME_STAGE_RECOVER) ||
            state->host_work_inflight != 0 ||
            (state->queue_bound &&
             (state->guest_device != decoded.header.guest_device ||
              state->device != decoded.device))) {
            status = EXPRESS_VK_FLIME_ROUTE_BAD_STATE;
            flime_bridge_error(
                error, EXPRESS_VK_FLIME_ERROR_STATE,
                "FLIME fallback flush crossed an invalid or active session");
            failed = true;
            goto unlock;
        }

        if (!express_vk_flime_session_ledger_filter_fallback(
                state->session, decoded.update_ids,
                decoded.header.record_count, emit, error)) {
            status = EXPRESS_VK_FLIME_ROUTE_BAD_STATE;
            failed = true;
            goto unlock;
        }
        for (i = 0; i < decoded.header.record_count; i++) {
            emitted += emit[i] ? 1 : 0;
        }

        if (emitted != 0) {
            uint32_t out_index = 0;

            release = g_try_new0(ExpressVkFlimeReleaseBatch, 1);
            if (release == NULL) {
                status = EXPRESS_VK_FLIME_ROUTE_RESOURCE_LIMIT;
                flime_bridge_error(
                    error, EXPRESS_VK_FLIME_ERROR_OOM,
                    "out of memory reserving FLIME fallback release");
                failed = true;
                goto unlock;
            }
            for (i = 0; i < decoded.header.record_count; i++) {
                if (!emit[i]) {
                    g_free(decoded.payloads[i]);
                    decoded.payloads[i] = NULL;
                    continue;
                }
                decoded.writes[out_index] = decoded.writes[i];
                decoded.payloads[out_index] = decoded.payloads[i];
                if (out_index != i) {
                    decoded.payloads[i] = NULL;
                }
                out_index++;
            }
            release->device = decoded.device;
            release->write_count = emitted;
            release->period_id = decoded.header.period_id;
            release->submission_id = decoded.header.submission_id;
            release->fallback_flush = true;
            release->writes = decoded.writes;
            release->payloads = decoded.payloads;
            decoded.writes = NULL;
            decoded.payloads = NULL;
        }

        /*
         * The caller holds the global descriptor/submit transaction lock.
         * Removing the session here therefore makes every later submit wait
         * until the returned release batch has been realized.
         */
        flime_cancel_session_pending_locked(state, false);
        drop_session_list_ref = flime_remove_session_locked(state);
        status = EXPRESS_VK_FLIME_ROUTE_ACCEPTED;
        flime_route_reply_init(
            reply, status,
            EXPRESS_VK_FLIME_ROUTE_REPLY_FALLBACK_DRAINED,
            &decoded.header);
        flime_route_reply_counts(reply, decoded.header.record_count,
                                 emitted, 0);
        if (release != NULL) {
            *release_batch = release;
            release = NULL;
        }
        g_mutex_unlock(&flime_bridge_lock);
        if (drop_session_list_ref) {
            flime_session_state_unref(state);
            drop_session_list_ref = false;
        }
        goto out;
    }
    if (!state->period_open ||
        state->active_period_id != decoded.header.period_id) {
        status = EXPRESS_VK_FLIME_ROUTE_BAD_STATE;
        flime_bridge_error(error, EXPRESS_VK_FLIME_ERROR_STATE,
                           "FLIME route does not match the current open period");
        failed = true;
        goto unlock;
    }
    if (recovery_route && state->period_submission_executed) {
        status = EXPRESS_VK_FLIME_ROUTE_BAD_STATE;
        flime_bridge_error(
            error, EXPRESS_VK_FLIME_ERROR_STATE,
            "executed FLIME submission cannot be resubmitted in recovery");
        failed = true;
        goto unlock;
    }
    if (!recovery_route &&
        (!!(decoded.header.flags & EXPRESS_VK_FLIME_ROUTE_SINGLE_FLUSH) !=
         !!(state->active_period_flags &
            EXPRESS_VK_FLIME_PERIOD_SINGLE_FLUSH))) {
        status = EXPRESS_VK_FLIME_ROUTE_BAD_STATE;
        flime_bridge_error(
            error, EXPRESS_VK_FLIME_ERROR_STATE,
            "FLIME route shape disagrees with its period SINGLE_FLUSH flag");
        failed = true;
        goto unlock;
    }
    if (!(recovery_route ?
          express_vk_flime_session_validate_recovery_period(
              state->session, decoded.header.period_id, error) :
          express_vk_flime_session_validate_open_period(
              state->session, decoded.header.period_id,
              (decoded.header.flags & EXPRESS_VK_FLIME_ROUTE_SINGLE_FLUSH) ?
                  EXPRESS_VK_FLIME_PERIOD_SINGLE_FLUSH : 0,
              error))) {
        status = EXPRESS_VK_FLIME_ROUTE_BAD_STATE;
        failed = true;
        goto unlock;
    }
    if (!flime_validate_route_plan_locked(state, &decoded,
                                           &expected_chunks, &status, error)) {
        failed = true;
        goto unlock;
    }
    if (decoded.header.record_count != 0 &&
        (!(decoded.header.flags & EXPRESS_VK_FLIME_ROUTE_SUBMISSION_FINAL) ||
         (decoded.header.flags & EXPRESS_VK_FLIME_ROUTE_RECOVERY_REPLAY))) {
        release = g_try_new0(ExpressVkFlimeReleaseBatch, 1);
        if (release != NULL) {
            release->emit_mask = g_try_new0(bool,
                                             decoded.header.record_count);
        }
        if (release == NULL || release->emit_mask == NULL) {
            status = EXPRESS_VK_FLIME_ROUTE_RESOURCE_LIMIT;
            flime_bridge_error(error, EXPRESS_VK_FLIME_ERROR_OOM,
                               "out of memory reserving FLIME release batch");
            failed = true;
            goto unlock;
        }
    }
    {
        ExpressVkFlimeBridgeSession *other_state;

        for (other_state = flime_bridge_sessions; other_state != NULL;
             other_state = other_state->next) {
            if (other_state != state && !other_state->removed &&
                other_state->queue_bound &&
                other_state->process_id == decoded.header.process_id &&
                other_state->guest_queue == decoded.header.guest_queue) {
                status = EXPRESS_VK_FLIME_ROUTE_BAD_STATE;
                flime_bridge_error(
                    error, EXPRESS_VK_FLIME_ERROR_STATE,
                    "multiple FLIME streams attempted to bind the same queue");
                failed = true;
                goto unlock;
            }
        }
    }
    if (!state->queue_bound) {
        state->queue_bound = true;
        state->guest_device = decoded.header.guest_device;
        state->guest_queue = decoded.header.guest_queue;
        state->device = decoded.device;
        state->queue = decoded.queue;
    } else if (state->guest_device != decoded.header.guest_device ||
               state->device != decoded.device ||
               ((state->guest_queue != decoded.header.guest_queue ||
                 state->queue != decoded.queue) &&
                (!(decoded.header.flags &
                   EXPRESS_VK_FLIME_ROUTE_SUBMISSION_BEGIN) ||
                 state->pending_count != 0))) {
        status = EXPRESS_VK_FLIME_ROUTE_BAD_STATE;
        flime_bridge_error(error, EXPRESS_VK_FLIME_ERROR_STATE,
                           "FLIME stream attempted to change its bound queue");
        failed = true;
        goto unlock;
    }
    if (state->queue_bound &&
        (state->guest_queue != decoded.header.guest_queue ||
         state->queue != decoded.queue)) {
        state->guest_queue = decoded.header.guest_queue;
        state->queue = decoded.queue;
    }

    pending = flime_find_pending_locked(decoded.header.process_id,
                                         decoded.header.stream_id,
                                         decoded.header.submission_id);
    if (decoded.header.flags & EXPRESS_VK_FLIME_ROUTE_SUBMISSION_BEGIN) {
        ExpressVkFlimePendingSubmission *other;
        bool recovery = (decoded.header.flags &
                         EXPRESS_VK_FLIME_ROUTE_RECOVERY_REPLAY) != 0;

        if (pending != NULL ||
            (!recovery &&
             (state->period_submission_id != 0 ||
              decoded.header.submission_id <= state->last_submission_id)) ||
            (recovery &&
             ((state->period_submission_id == 0 &&
               decoded.header.submission_id <= state->last_submission_id) ||
              (state->period_submission_id != 0 &&
               (decoded.header.submission_id !=
                    state->period_submission_id ||
                decoded.header.submission_record_count !=
                    state->period_submission_record_count))))) {
            status = EXPRESS_VK_FLIME_ROUTE_BAD_STATE;
            flime_bridge_error(error, EXPRESS_VK_FLIME_ERROR_STATE,
                               "duplicate, cross-period, or non-monotonic FLIME submission BEGIN");
            failed = true;
            goto unlock;
        }
        for (other = flime_bridge_pending; other != NULL;
             other = other->next) {
            if (other->process_id == decoded.header.process_id &&
                other->guest_queue == decoded.header.guest_queue) {
                status = EXPRESS_VK_FLIME_ROUTE_BAD_STATE;
                flime_bridge_error(error, EXPRESS_VK_FLIME_ERROR_STATE,
                                   "a prior FLIME submission on this queue is not committed");
                failed = true;
                goto unlock;
            }
        }
        if (!recovery) {
            state->period_submission_id = decoded.header.submission_id;
            state->period_submission_record_count =
                decoded.header.submission_record_count;
            state->period_submission_committed = false;
            state->last_submission_id = decoded.header.submission_id;
        } else if (state->period_submission_id == 0) {
            /*
             * A mismatch may enter Recover before the first normal FLRD.  In
             * that ledger-empty case the recovery BEGIN establishes the sole
             * occurrence identity; an already pinned/released occurrence can
             * never change either id or declared record count.
             */
            state->period_submission_id = decoded.header.submission_id;
            state->period_submission_record_count =
                decoded.header.submission_record_count;
            state->period_submission_committed = false;
            state->last_submission_id = decoded.header.submission_id;
        }
        state->period_expected_chunk_count = expected_chunks;
        state->period_expected_unit_count = 0;
        memset(state->period_chunk_first_unit, 0,
               sizeof(state->period_chunk_first_unit));
        memset(state->period_chunk_unit_past_end, 0,
               sizeof(state->period_chunk_unit_past_end));
        memset(state->period_chunk_template_offset, 0,
               sizeof(state->period_chunk_template_offset));
        memset(state->period_chunk_seen, 0,
               sizeof(state->period_chunk_seen));
        state->timing_period_id = 0;
        memset(state->chunk_timing, 0, sizeof(state->chunk_timing));
        pending = flime_pending_new(state, &decoded.header,
                                     expected_chunks, error);
        if (pending == NULL) {
            status = EXPRESS_VK_FLIME_ROUTE_RESOURCE_LIMIT;
            failed = true;
            goto unlock;
        }
        flime_append_pending_locked(pending);
        created = true;
        if (pending->recovery) {
            ExpressVkFlimeRecoveryStats recovery_stats;

            if (!flime_session_begin_recovery_locked(
                    state, &recovery_stats, error)) {
                status = EXPRESS_VK_FLIME_ROUTE_BAD_STATE;
                failed = true;
                goto unlock;
            }
            pending->recovery_started = true;
        }
    } else if (pending == NULL) {
        status = EXPRESS_VK_FLIME_ROUTE_BAD_STATE;
        flime_bridge_error(error, EXPRESS_VK_FLIME_ERROR_STATE,
                           "FLIME route chunk has no matching BEGIN");
        failed = true;
        goto unlock;
    }

    if (pending->complete || pending->release_failed ||
        pending->release_inflight != 0 ||
        pending->period_id != decoded.header.period_id ||
        pending->plan_epoch != decoded.header.plan_epoch ||
        pending->guest_device != decoded.header.guest_device ||
        pending->guest_queue != decoded.header.guest_queue ||
        pending->submission_record_count !=
            decoded.header.submission_record_count ||
        pending->expected_chunk_count != expected_chunks ||
        pending->next_chunk_index != decoded.header.chunk_index ||
        pending->recovery !=
            ((decoded.header.flags &
              EXPRESS_VK_FLIME_ROUTE_RECOVERY_REPLAY) != 0) ||
        decoded.header.record_count >
            pending->submission_record_count -
                pending->received_record_count) {
        status = EXPRESS_VK_FLIME_ROUTE_BAD_STATE;
        flime_bridge_error(error, EXPRESS_VK_FLIME_ERROR_STATE,
                           "FLIME route chunk sequence/state mismatch");
        failed = true;
        goto unlock;
    }
    if ((decoded.header.flags & EXPRESS_VK_FLIME_ROUTE_SUBMISSION_FINAL) &&
        pending->received_record_count + decoded.header.record_count !=
            pending->submission_record_count) {
        status = EXPRESS_VK_FLIME_ROUTE_BAD_STATE;
        flime_bridge_error(error, EXPRESS_VK_FLIME_ERROR_STATE,
                           "FLIME FINAL does not consume the declared record sequence");
        failed = true;
        goto unlock;
    }

    if (pending->recovery && decoded.header.record_count != 0) {
        for (i = 0; i < decoded.header.record_count; i++) {
            replay_submission_ids[i] = decoded.header.submission_id;
        }
        if (!express_vk_flime_session_ledger_begin_replay_batch(
                state->session, decoded.update_ids, replay_submission_ids,
                decoded.header.record_count, emit, error)) {
            status = EXPRESS_VK_FLIME_ROUTE_BAD_STATE;
            failed = true;
            goto unlock;
        }
        memcpy(begun, emit, decoded.header.record_count * sizeof(*begun));
        reservation_begun = true;
        for (i = 0; i < decoded.header.record_count; i++) {
            duplicate = duplicate || !emit[i];
        }
    } else {
        for (i = 0; i < decoded.header.record_count; i++) {
            bool inserted = false;

            if (!express_vk_flime_session_ledger_prepare(
                    state->session, decoded.update_ids[i],
                    decoded.header.submission_id,
                    decoded.template_offsets[i], &inserted, error) ||
                (inserted && !express_vk_flime_session_ledger_mark_ready(
                    state->session, decoded.update_ids[i], error))) {
                status = EXPRESS_VK_FLIME_ROUTE_BAD_STATE;
                failed = true;
                goto unlock;
            }
            if (!inserted) {
                status = EXPRESS_VK_FLIME_ROUTE_BAD_STATE;
                flime_bridge_error(error, EXPRESS_VK_FLIME_ERROR_STATE,
                                   "FLIME update id was reused across chunks");
                failed = true;
                goto unlock;
            }
            emit[i] = true;
        }
    }
    if (!pending->recovery &&
        !(decoded.header.flags & EXPRESS_VK_FLIME_ROUTE_SUBMISSION_FINAL) &&
        decoded.header.record_count != 0) {
        if (!express_vk_flime_session_ledger_begin_release_batch(
                state->session, decoded.update_ids,
                decoded.header.record_count, begun, error)) {
            status = EXPRESS_VK_FLIME_ROUTE_BAD_STATE;
            failed = true;
            goto unlock;
        }
        reservation_begun = true;
        for (i = 0; i < decoded.header.record_count; i++) {
            if (!begun[i]) {
                status = EXPRESS_VK_FLIME_ROUTE_BAD_STATE;
                flime_bridge_error(error, EXPRESS_VK_FLIME_ERROR_STATE,
                                   "FLIME ledger refused an ordered release");
                failed = true;
                goto unlock;
            }
        }
    }

    for (i = 0; i < decoded.header.record_count; i++) {
        if (!emit[i] && !pending->recovery) {
            continue;
        }
        if (pending->update_count >= pending->submission_record_count) {
            status = EXPRESS_VK_FLIME_ROUTE_BAD_STATE;
            flime_bridge_error(error, EXPRESS_VK_FLIME_ERROR_OVERFLOW,
                               "FLIME update ledger count overflow");
            failed = true;
            goto unlock;
        }
        pending->update_ids[pending->update_count++] = decoded.update_ids[i];
        if (emit[i]) {
            emitted++;
        }
    }

    if ((decoded.header.flags & EXPRESS_VK_FLIME_ROUTE_SUBMISSION_FINAL) &&
        !pending->recovery) {
        for (i = 0; i < decoded.header.record_count; i++) {
            if (!emit[i]) {
                continue;
            }
            pending->tail_writes[pending->tail_count] = decoded.writes[i];
            pending->tail_payloads[pending->tail_count] = decoded.payloads[i];
            pending->tail_update_ids[pending->tail_count] =
                decoded.update_ids[i];
            decoded.payloads[i] = NULL;
            pending->tail_count++;
        }
    } else if (emitted != 0) {
        uint32_t out_index = 0;

        g_assert(release != NULL);
        for (i = 0; i < decoded.header.record_count; i++) {
            if (!emit[i]) {
                g_free(decoded.payloads[i]);
                decoded.payloads[i] = NULL;
            }
        }
        for (i = 0; i < decoded.header.record_count; i++) {
            if (!emit[i]) {
                continue;
            }
            decoded.writes[out_index] = decoded.writes[i];
            decoded.payloads[out_index] = decoded.payloads[i];
            decoded.update_ids[out_index] = decoded.update_ids[i];
            release->emit_mask[out_index] = true;
            if (out_index != i) {
                decoded.payloads[i] = NULL;
            }
            out_index++;
        }
        release->owner = flime_pending_ref(pending);
        release->device = pending->device;
        release->write_count = emitted;
        release->chunk_index = decoded.header.chunk_index;
        release->period_id = decoded.header.period_id;
        release->submission_id = decoded.header.submission_id;
        release->recovery = pending->recovery;
        release->writes = decoded.writes;
        release->payloads = decoded.payloads;
        release->update_ids = decoded.update_ids;
        decoded.writes = NULL;
        decoded.payloads = NULL;
        decoded.update_ids = NULL;
        g_assert(pending->release_inflight != UINT32_MAX);
        pending->release_inflight++;
        flime_pending_host_work_begin_locked(pending);
    }
    if (decoded.payloads != NULL) {
        for (i = 0; i < decoded.header.record_count; i++) {
            if (!emit[i]) {
                g_free(decoded.payloads[i]);
                decoded.payloads[i] = NULL;
            }
        }
    }
    pending->received_record_count += decoded.header.record_count;
    pending->next_chunk_index++;
    if (decoded.header.flags & EXPRESS_VK_FLIME_ROUTE_SUBMISSION_FINAL) {
        pending->complete = true;
    }
    if (decoded.header.chunk_index < EXPRESS_VK_FLIME_HARD_MAX_CHUNKS) {
        uint64_t now_ns = (uint64_t)g_get_monotonic_time() * 1000;

        state->period_chunk_first_unit[decoded.header.chunk_index] =
            decoded.header.first_unit;
        state->period_chunk_unit_past_end[decoded.header.chunk_index] =
            decoded.header.unit_past_end;
        state->period_chunk_template_offset[decoded.header.chunk_index] =
            decoded.header.template_offset;
        state->period_chunk_seen[decoded.header.chunk_index] = true;
        if (decoded.header.flags & EXPRESS_VK_FLIME_ROUTE_SUBMISSION_FINAL) {
            state->period_expected_unit_count =
                decoded.header.unit_past_end;
        }
        if (state->timing_period_id != decoded.header.period_id) {
            state->timing_period_id = decoded.header.period_id;
            memset(state->chunk_timing, 0, sizeof(state->chunk_timing));
        }
        state->chunk_timing[decoded.header.chunk_index].handoff_ns =
            host_receive_ns != 0 && now_ns >= host_receive_ns ?
            now_ns - host_receive_ns : 0;
        state->chunk_timing[decoded.header.chunk_index].handoff_valid = true;
        if (emitted == 0) {
            state->chunk_timing[decoded.header.chunk_index].realize_ns = 0;
            state->chunk_timing[decoded.header.chunk_index].realize_valid = true;
        }
    }

unlock:
    if (failed && pending != NULL) {
        if (reservation_begun && decoded.header.record_count != 0) {
            if (pending->recovery) {
                express_vk_flime_session_ledger_complete_replay_batch(
                    pending->session, decoded.update_ids, begun,
                    decoded.header.record_count, false, NULL);
            } else {
                express_vk_flime_session_ledger_complete_release_batch(
                    pending->session, decoded.update_ids, begun,
                    decoded.header.record_count, false, NULL);
            }
        }
        pending->release_failed = true;
        flime_unlink_pending_locked(pending);
        flime_pending_begin_recovery_locked(pending);
    }
    g_mutex_unlock(&flime_bridge_lock);

    if (failed) {
        if (created && pending != NULL) {
            flime_pending_unref(pending);
        } else if (pending != NULL && !pending->listed) {
            flime_pending_unref(pending);
        }
        if (release != NULL) {
            g_free(release->emit_mask);
            g_free(release);
        }
        release = NULL;
        goto out;
    }

    status = EXPRESS_VK_FLIME_ROUTE_ACCEPTED;
    if (release != NULL && emitted == 0) {
        g_free(release->emit_mask);
        g_free(release);
        release = NULL;
    }
    flime_route_reply_init(reply, status,
                           EXPRESS_VK_FLIME_ROUTE_REPLY_DEFERRED_TO_SUBMIT |
                           (pending->recovery ?
                            EXPRESS_VK_FLIME_ROUTE_REPLY_RECOVERY : 0),
                           &decoded.header);
    flime_route_reply_counts(
        reply, decoded.header.record_count, emitted,
        duplicate ? EXPRESS_VK_FLIME_ROUTE_REPLY_DUPLICATE_SUPPRESSED : 0);
    if (release_batch != NULL) {
        *release_batch = release;
        release = NULL;
    }

out:
    if (failed) {
        flime_cancel_route(transport_process_id, packet, packet_bytes);
        flime_route_reply_init(reply, status,
                               EXPRESS_VK_FLIME_ROUTE_REPLY_FALLBACK_REQUIRED |
                               EXPRESS_VK_FLIME_ROUTE_REPLY_RECOVERY_REQUIRED,
                               &decoded.header);
    }
    if (release != NULL) {
        express_vk_flime_bridge_complete_release(release, false, 0);
    }
    g_free(emit);
    g_free(begun);
    g_free(replay_submission_ids);
    flime_put_session(state);
    flime_decoded_route_clear(&decoded);
    return !failed && status == EXPRESS_VK_FLIME_ROUTE_ACCEPTED;
}

VkDevice express_vk_flime_bridge_release_device(
    const ExpressVkFlimeReleaseBatch *batch)
{
    return batch != NULL ? batch->device : VK_NULL_HANDLE;
}

uint32_t express_vk_flime_bridge_release_write_count(
    const ExpressVkFlimeReleaseBatch *batch)
{
    return batch != NULL ? batch->write_count : 0;
}

const VkWriteDescriptorSet *express_vk_flime_bridge_release_writes(
    const ExpressVkFlimeReleaseBatch *batch)
{
    return batch != NULL ? batch->writes : NULL;
}

void express_vk_flime_bridge_complete_release(
    ExpressVkFlimeReleaseBatch *batch, bool applied,
    uint64_t host_realize_ns)
{
    ExpressVkFlimePendingSubmission *pending;
    ExpressVkFlimeBridgeSession *state;
    bool drop_list_ref = false;
    bool ledger_ok = true;
    uint32_t i;

    if (batch == NULL) {
        return;
    }
    if (batch->fallback_flush) {
        for (i = 0; i < batch->write_count; i++) {
            g_free(batch->payloads[i]);
        }
        g_free(batch->payloads);
        g_free(batch->writes);
        g_free(batch);
        return;
    }
    pending = batch->owner;
    g_mutex_lock(&flime_bridge_lock);
    if (batch->write_count != 0) {
        if (batch->recovery) {
            ledger_ok = express_vk_flime_session_ledger_complete_replay_batch(
                pending->session, batch->update_ids, batch->emit_mask,
                batch->write_count, applied, NULL);
        } else {
            ledger_ok = express_vk_flime_session_ledger_complete_release_batch(
                pending->session, batch->update_ids, batch->emit_mask,
                batch->write_count, applied, NULL);
        }
    }
    if (pending->release_inflight != 0) {
        pending->release_inflight--;
        flime_pending_host_work_end_locked(pending);
    }
    if (!applied || !ledger_ok || pending->release_failed) {
        pending->release_failed = true;
        if (pending->listed) {
            flime_unlink_pending_locked(pending);
            drop_list_ref = true;
        }
        flime_pending_begin_recovery_locked(pending);
    } else {
        /*
         * The pending owner pins its original session shell.  Never relookup
         * by wire identity here: reset may already have admitted a new session
         * with the same process/stream ids.
         */
        state = pending->bridge_state;
        if (state != NULL && !state->removed &&
            state->timing_period_id == batch->period_id &&
            batch->chunk_index < EXPRESS_VK_FLIME_HARD_MAX_CHUNKS) {
            state->chunk_timing[batch->chunk_index].realize_ns =
                host_realize_ns;
            state->chunk_timing[batch->chunk_index].realize_valid = true;
        }
    }
    g_mutex_unlock(&flime_bridge_lock);

    for (i = 0; i < batch->write_count; i++) {
        g_free(batch->payloads[i]);
    }
    g_free(batch->payloads);
    g_free(batch->writes);
    g_free(batch->update_ids);
    g_free(batch->emit_mask);
    if (drop_list_ref) {
        flime_pending_unref(pending);
    }
    flime_pending_unref(pending); /* release-batch reference */
    g_free(batch);
}

ExpressVkFlimeSubmitGate express_vk_flime_bridge_prepare_submit(
    uint64_t transport_process_id, uint64_t guest_queue, VkQueue host_queue,
    VkDevice host_device, ExpressVkFlimeSubmitBatch **batch_out)
{
    ExpressVkFlimePendingSubmission *pending;
    ExpressVkFlimeSubmitBatch *batch = NULL;
    uint32_t i;

    if (batch_out != NULL) {
        *batch_out = NULL;
    }
    if (transport_process_id == 0 || guest_queue == 0 ||
        host_queue == VK_NULL_HANDLE || batch_out == NULL) {
        return EXPRESS_VK_FLIME_SUBMIT_ERROR;
    }

    g_mutex_lock(&flime_bridge_lock);
    pending = flime_find_queue_pending_locked(transport_process_id,
                                               guest_queue);
    if (pending == NULL) {
        ExpressVkFlimeBridgeSession *bound;

        for (bound = flime_bridge_sessions; bound != NULL;
             bound = bound->next) {
            ExpressVkFlimeProgressInfo progress;

            if (bound->removed || !bound->queue_bound ||
                bound->process_id != transport_process_id ||
                bound->guest_queue != guest_queue) {
                continue;
            }
            if (bound->queue != host_queue ||
                bound->device == VK_NULL_HANDLE ||
                bound->device != host_device) {
                ExpressVkFlimeRecoveryStats stats;

                flime_session_begin_recovery_locked(bound, &stats, NULL);
                g_mutex_unlock(&flime_bridge_lock);
                return EXPRESS_VK_FLIME_SUBMIT_ERROR;
            }
            if (bound->profiled_occurrence_ready) {
                g_mutex_unlock(&flime_bridge_lock);
                return EXPRESS_VK_FLIME_SUBMIT_BLOCKED;
            }
            express_vk_flime_session_get_progress(bound->session, &progress);
            if (progress.stage == EXPRESS_VK_FLIME_STAGE_RECOVER) {
                g_mutex_unlock(&flime_bridge_lock);
                return EXPRESS_VK_FLIME_SUBMIT_BLOCKED;
            }
            if (progress.stage == EXPRESS_VK_FLIME_STAGE_LEGACY ||
                progress.stage == EXPRESS_VK_FLIME_STAGE_DETECT) {
                break;
            }
            /*
             * Once a learned stream is bound to this queue, a measured period
             * owns exactly one routed submission occurrence.  Falling through
             * to legacy here would permit a submit before SINGLE_FLUSH, or a
             * second real submit after the routed occurrence was committed.
             */
            if (bound->period_open &&
                (bound->period_submission_committed ||
                 bound->period_submission_id != 0)) {
                ExpressVkFlimeRecoveryStats stats;

                if (bound->host_work_inflight != 0) {
                    g_mutex_unlock(&flime_bridge_lock);
                    return EXPRESS_VK_FLIME_SUBMIT_BLOCKED;
                }
                flime_session_begin_recovery_locked(bound, &stats, NULL);
                g_mutex_unlock(&flime_bridge_lock);
                return EXPRESS_VK_FLIME_SUBMIT_ERROR;
            }
            if (bound->period_open &&
                (progress.stage == EXPRESS_VK_FLIME_STAGE_LEARN ||
                 progress.stage == EXPRESS_VK_FLIME_STAGE_MATCH ||
                 progress.stage == EXPRESS_VK_FLIME_STAGE_RECOVER)) {
                g_mutex_unlock(&flime_bridge_lock);
                return EXPRESS_VK_FLIME_SUBMIT_BLOCKED;
            }
            if (!bound->period_open &&
                (progress.stage == EXPRESS_VK_FLIME_STAGE_LEARN ||
                 progress.stage == EXPRESS_VK_FLIME_STAGE_MATCH)) {
                break;
            }
            if (progress.stage == EXPRESS_VK_FLIME_STAGE_FAST) {
                ExpressVkFlimeRecoveryStats stats;

                flime_session_begin_recovery_locked(bound, &stats, NULL);
                g_mutex_unlock(&flime_bridge_lock);
                return EXPRESS_VK_FLIME_SUBMIT_ERROR;
            }
            g_mutex_unlock(&flime_bridge_lock);
            return EXPRESS_VK_FLIME_SUBMIT_BLOCKED;
        }
        /*
         * The first learned occurrence has not bound its queue until FLRD
         * arrives.  Conservatively block any submit from the same transport
         * process while such a period is open, otherwise QueueSubmit could
         * race ahead and become an unaccounted legacy submission.
         */
        for (bound = flime_bridge_sessions; bound != NULL;
             bound = bound->next) {
            ExpressVkFlimeProgressInfo progress;

            if (bound->removed || bound->queue_bound ||
                bound->process_id != transport_process_id ||
                !bound->period_open) {
                continue;
            }
            express_vk_flime_session_get_progress(bound->session, &progress);
            if (progress.stage == EXPRESS_VK_FLIME_STAGE_LEARN ||
                progress.stage == EXPRESS_VK_FLIME_STAGE_MATCH ||
                progress.stage == EXPRESS_VK_FLIME_STAGE_RECOVER) {
                g_mutex_unlock(&flime_bridge_lock);
                return EXPRESS_VK_FLIME_SUBMIT_BLOCKED;
            }
            if (progress.stage == EXPRESS_VK_FLIME_STAGE_FAST) {
                ExpressVkFlimeRecoveryStats stats;

                flime_session_begin_recovery_locked(bound, &stats, NULL);
                g_mutex_unlock(&flime_bridge_lock);
                return EXPRESS_VK_FLIME_SUBMIT_ERROR;
            }
        }
        g_mutex_unlock(&flime_bridge_lock);
        return EXPRESS_VK_FLIME_SUBMIT_LEGACY;
    }
    if (pending->queue != host_queue || pending->device != host_device) {
        pending->release_failed = true;
        flime_unlink_pending_locked(pending);
        flime_pending_begin_recovery_locked(pending);
        g_mutex_unlock(&flime_bridge_lock);
        flime_pending_unref(pending);
        return EXPRESS_VK_FLIME_SUBMIT_ERROR;
    }
    if (!pending->complete || pending->release_inflight != 0) {
        g_mutex_unlock(&flime_bridge_lock);
        return EXPRESS_VK_FLIME_SUBMIT_BLOCKED;
    }
    if (pending->submit_inflight) {
        g_mutex_unlock(&flime_bridge_lock);
        return EXPRESS_VK_FLIME_SUBMIT_BLOCKED;
    }
    if (pending->release_failed) {
        flime_unlink_pending_locked(pending);
        flime_pending_begin_recovery_locked(pending);
        g_mutex_unlock(&flime_bridge_lock);
        flime_pending_unref(pending);
        return EXPRESS_VK_FLIME_SUBMIT_ERROR;
    }
    if (pending->recovery) {
        uint32_t ledger_entry_count = 0;

        if (!express_vk_flime_session_ledger_submission_ready_to_commit(
                pending->session, pending->submission_id,
                &ledger_entry_count, NULL) ||
            ledger_entry_count != pending->submission_record_count) {
            /*
             * An omitted DISCARDED suffix or substituted record sequence must
             * never reach the real QueueSubmit.  Drop this replay attempt but
             * leave the session in Recover so the exact occurrence can retry.
             */
            pending->release_failed = true;
            flime_unlink_pending_locked(pending);
            g_mutex_unlock(&flime_bridge_lock);
            flime_pending_unref(pending);
            return EXPRESS_VK_FLIME_SUBMIT_ERROR;
        }
    }

    batch = g_try_new0(ExpressVkFlimeSubmitBatch, 1);
    if (batch == NULL) {
        g_mutex_unlock(&flime_bridge_lock);
        return EXPRESS_VK_FLIME_SUBMIT_ERROR;
    }
    if (pending->tail_count != 0 &&
        !express_vk_flime_session_ledger_begin_release_batch(
            pending->session, pending->tail_update_ids, pending->tail_count,
            pending->tail_emit_mask, NULL)) {
        pending->release_failed = true;
        flime_unlink_pending_locked(pending);
        flime_pending_begin_recovery_locked(pending);
        g_mutex_unlock(&flime_bridge_lock);
        flime_pending_unref(pending);
        g_free(batch);
        return EXPRESS_VK_FLIME_SUBMIT_ERROR;
    }
    for (i = 0; i < pending->tail_count; i++) {
        if (!pending->tail_emit_mask[i]) {
            express_vk_flime_session_ledger_complete_release_batch(
                pending->session, pending->tail_update_ids,
                pending->tail_emit_mask, pending->tail_count, false, NULL);
            pending->release_failed = true;
            flime_unlink_pending_locked(pending);
            flime_pending_begin_recovery_locked(pending);
            g_mutex_unlock(&flime_bridge_lock);
            flime_pending_unref(pending);
            g_free(batch);
            return EXPRESS_VK_FLIME_SUBMIT_ERROR;
        }
    }
    pending->tail_release_begun = pending->tail_count;
    pending->submit_inflight = true;
    flime_pending_host_work_begin_locked(pending);
    batch->owner = flime_pending_ref(pending);
    *batch_out = batch;
    g_mutex_unlock(&flime_bridge_lock);
    return EXPRESS_VK_FLIME_SUBMIT_READY;
}

VkDevice express_vk_flime_bridge_batch_device(
    const ExpressVkFlimeSubmitBatch *batch)
{
    return batch != NULL && batch->owner != NULL ?
        batch->owner->device : VK_NULL_HANDLE;
}

uint32_t express_vk_flime_bridge_batch_write_count(
    const ExpressVkFlimeSubmitBatch *batch)
{
    return batch != NULL && batch->owner != NULL ?
        batch->owner->tail_count : 0;
}

const VkWriteDescriptorSet *express_vk_flime_bridge_batch_writes(
    const ExpressVkFlimeSubmitBatch *batch)
{
    return batch != NULL && batch->owner != NULL ?
        batch->owner->tail_writes : NULL;
}

bool express_vk_flime_bridge_submit_updates_applied(
    ExpressVkFlimeSubmitBatch *batch)
{
    ExpressVkFlimePendingSubmission *pending;
    bool ok = true;

    if (batch == NULL || batch->owner == NULL) {
        return false;
    }
    pending = batch->owner;
    g_mutex_lock(&flime_bridge_lock);
    if (pending->tail_applied) {
        g_mutex_unlock(&flime_bridge_lock);
        return true;
    }
    if (pending->tail_release_begun != 0) {
        ok = express_vk_flime_session_ledger_complete_release_batch(
            pending->session, pending->tail_update_ids,
            pending->tail_emit_mask, pending->tail_release_begun, true, NULL);
    }
    pending->tail_release_begun = 0;
    pending->tail_applied = ok;
    if (!ok) {
        pending->release_failed = true;
        flime_pending_begin_recovery_locked(pending);
    }
    g_mutex_unlock(&flime_bridge_lock);
    return ok;
}

void express_vk_flime_bridge_complete_submit(ExpressVkFlimeSubmitBatch *batch,
                                              VkResult result,
                                              uint64_t host_realize_ns)
{
    ExpressVkFlimePendingSubmission *pending;
    ExpressVkFlimeBridgeSession *state;
    bool ledger_ok = true;
    bool submission_committed;
    bool drop_list_ref = false;
    uint32_t i;

    if (batch == NULL) {
        return;
    }
    pending = batch->owner;
    g_mutex_lock(&flime_bridge_lock);
    if (!pending->tail_applied && pending->tail_release_begun != 0) {
        express_vk_flime_session_ledger_complete_release_batch(
            pending->session, pending->tail_update_ids,
            pending->tail_emit_mask, pending->tail_release_begun, false, NULL);
        pending->tail_release_begun = 0;
    }
    if (result == VK_SUCCESS &&
        (pending->tail_count == 0 || pending->tail_applied)) {
        for (i = 0; i < pending->update_count; i++) {
            bool should_emit = false;

            if (!express_vk_flime_session_ledger_commit(
                    pending->session, pending->update_ids[i],
                    &should_emit, NULL) || should_emit) {
                ledger_ok = false;
                break;
            }
        }
    } else if (result == VK_SUCCESS) {
        ledger_ok = false;
    }
    submission_committed = result == VK_SUCCESS && ledger_ok &&
        (pending->tail_count == 0 || pending->tail_applied);
    if (!submission_committed) {
        pending->release_failed = true;
        flime_pending_begin_recovery_locked(pending);
    }
    if (pending->submit_inflight) {
        pending->submit_inflight = false;
        flime_pending_host_work_end_locked(pending);
    }
    if (pending->release_failed) {
        flime_pending_begin_recovery_locked(pending);
    }
    if (pending->listed) {
        flime_unlink_pending_locked(pending);
        drop_list_ref = true;
    }
    /*
     * Completion belongs only to the exact ref-counted session that created
     * pending.  An id lookup could corrupt a replacement session after reset.
     */
    state = pending->bridge_state;
    if (result == VK_SUCCESS && state != NULL && !state->removed &&
        state->active_period_id == pending->period_id &&
        state->period_submission_id == pending->submission_id) {
        /*
         * This token follows the irreversible GPU side effect, not ledger
         * bookkeeping.  A later invariant failure may require shadow repair,
         * but it can never authorize this logical submission to execute twice.
         */
        state->period_submission_executed = true;
    }
    if (submission_committed && state != NULL && !state->removed &&
        state->period_open &&
        state->active_period_id == pending->period_id &&
        state->period_submission_id == pending->submission_id &&
        state->period_expected_chunk_count ==
            pending->expected_chunk_count &&
        state->period_expected_unit_count != 0) {
        state->period_submission_committed = true;
    }
    if (state != NULL && !state->removed &&
        state->timing_period_id == pending->period_id &&
        pending->expected_chunk_count != 0 &&
        pending->expected_chunk_count <=
            EXPRESS_VK_FLIME_HARD_MAX_CHUNKS) {
        uint32_t final_chunk = pending->expected_chunk_count - 1;

        state->chunk_timing[final_chunk].realize_ns = host_realize_ns;
        state->chunk_timing[final_chunk].realize_valid = true;
    }
    g_mutex_unlock(&flime_bridge_lock);

    if (drop_list_ref) {
        flime_pending_unref(pending);
    }
    flime_pending_unref(pending); /* submit-batch reference */
    g_free(batch);
}

void express_vk_flime_bridge_cleanup_process(uint64_t process_id)
{
    ExpressVkFlimePendingSubmission *pending;
    ExpressVkFlimePendingSubmission *next_pending;
    ExpressVkFlimeBridgeSession **link;

    g_mutex_lock(&flime_bridge_lock);
    for (pending = flime_bridge_pending; pending != NULL;
         pending = next_pending) {
        next_pending = pending->next;
        if (pending->process_id != process_id) {
            continue;
        }
        pending->release_failed = true;
        flime_unlink_pending_locked(pending);
        flime_pending_begin_recovery_locked(pending);
        flime_pending_unref(pending); /* former pending-list reference */
    }

    link = &flime_bridge_sessions;
    while (*link != NULL) {
        ExpressVkFlimeBridgeSession *state = *link;

        if (state->process_id != process_id) {
            link = &state->next;
            continue;
        }
        *link = state->next;
        state->next = NULL;
        state->removed = true;
        flime_control_sink_clear(state);
        if (flime_bridge_manager != NULL) {
            express_vk_flime_manager_remove(flime_bridge_manager,
                                            state->process_id,
                                            state->stream_id);
        }
        flime_session_state_unref(state); /* former session-list reference */
    }
    g_mutex_unlock(&flime_bridge_lock);
}

void express_vk_flime_bridge_cleanup_device(uint64_t process_id,
                                             uint64_t guest_device)
{
    ExpressVkFlimePendingSubmission *pending;
    ExpressVkFlimePendingSubmission *next_pending;
    ExpressVkFlimeBridgeSession **link;

    if (guest_device == 0) {
        return;
    }

    /*
     * Normal guest teardown removes the stream synchronously before the
     * vkDestroyDevice RPC. This path is the crash/transport-failure fallback:
     * remove only streams that have actually bound the destroyed device.
     * Unbound CAPS-only streams cannot have planner work or released updates.
     */
    g_mutex_lock(&flime_bridge_lock);
    for (pending = flime_bridge_pending; pending != NULL;
         pending = next_pending) {
        next_pending = pending->next;
        if (pending->process_id != process_id ||
            pending->guest_device != guest_device) {
            continue;
        }
        pending->release_failed = true;
        flime_unlink_pending_locked(pending);
        flime_pending_begin_recovery_locked(pending);
        flime_pending_unref(pending); /* former pending-list reference */
    }

    link = &flime_bridge_sessions;
    while (*link != NULL) {
        ExpressVkFlimeBridgeSession *state = *link;

        if (state->process_id != process_id || !state->queue_bound ||
            state->guest_device != guest_device) {
            link = &state->next;
            continue;
        }
        *link = state->next;
        state->next = NULL;
        state->removed = true;
        state->planner_generation++;
        flime_control_sink_clear(state);
        if (flime_bridge_manager != NULL) {
            express_vk_flime_manager_remove(flime_bridge_manager,
                                            state->process_id,
                                            state->stream_id);
        }
        flime_session_state_unref(state); /* former session-list reference */
    }
    g_mutex_unlock(&flime_bridge_lock);
}

void express_vk_flime_bridge_reset_transport(void)
{
    ExpressVkFlimePendingSubmission *pending;
    ExpressVkFlimePendingSubmission *next_pending;
    ExpressVkFlimeBridgeSession *state;
    ExpressVkFlimeBridgeSession *next_state;

    /*
     * Teleport Express owns one global transport namespace.  A virtio reset
     * lets the guest immediately recycle every descriptor address, so remove
     * all logical streams before the virtio core clears its vring state.
     * Planner and Vulkan callbacks retain ref-counted, removed session shells
     * and therefore cannot publish to the former DMA pages.
     */
    g_mutex_lock(&flime_bridge_lock);
    for (pending = flime_bridge_pending; pending != NULL;
         pending = next_pending) {
        next_pending = pending->next;
        pending->release_failed = true;
        flime_unlink_pending_locked(pending);
        flime_pending_begin_recovery_locked(pending);
        flime_pending_unref(pending); /* former pending-list reference */
    }

    state = flime_bridge_sessions;
    flime_bridge_sessions = NULL;
    while (state != NULL) {
        next_state = state->next;
        state->next = NULL;
        state->removed = true;
        state->planner_generation++;
        flime_control_sink_clear(state);
        if (flime_bridge_manager != NULL) {
            express_vk_flime_manager_remove(flime_bridge_manager,
                                            state->process_id,
                                            state->stream_id);
        }
        flime_session_state_unref(state); /* former session-list reference */
        state = next_state;
    }
    g_mutex_unlock(&flime_bridge_lock);
}

void express_vk_flime_bridge_shutdown(void)
{
    ExpressVkFlimeBridgeSession *states;
    ExpressVkFlimePendingSubmission *pending;
    ExpressVkFlimeManager *manager;
    GThread *thread;
    GAsyncQueue *queue;

    g_mutex_lock(&flime_bridge_lock);
    if (flime_planner_stopping) {
        g_mutex_unlock(&flime_bridge_lock);
        return;
    }
    flime_planner_stopping = true;

    states = flime_bridge_sessions;
    flime_bridge_sessions = NULL;
    for (ExpressVkFlimeBridgeSession *state = states; state != NULL;
         state = state->next) {
        state->removed = true;
        flime_control_sink_clear(state);
    }
    pending = flime_bridge_pending;
    flime_bridge_pending = NULL;
    for (ExpressVkFlimePendingSubmission *item = pending; item != NULL;
         item = item->next) {
        item->listed = false;
        if (item->bridge_state != NULL &&
            item->bridge_state->pending_count != 0) {
            item->bridge_state->pending_count--;
        }
        item->release_failed = true;
        flime_pending_begin_recovery_locked(item);
    }
    manager = flime_bridge_manager;
    flime_bridge_manager = NULL;
    thread = flime_planner_thread;
    queue = flime_planner_queue;
    if (thread != NULL && queue != NULL) {
        g_async_queue_push(queue, &flime_planner_stop_token);
    }
    g_mutex_unlock(&flime_bridge_lock);

    if (thread != NULL) {
        g_thread_join(thread);
    }
    while (pending != NULL) {
        ExpressVkFlimePendingSubmission *next = pending->next;

        pending->next = NULL;
        flime_pending_unref(pending); /* former pending-list reference */
        pending = next;
    }
    while (states != NULL) {
        ExpressVkFlimeBridgeSession *next = states->next;

        states->next = NULL;
        flime_session_state_unref(states); /* former session-list reference */
        states = next;
    }
    express_vk_flime_manager_free(manager);
    if (queue != NULL) {
        g_async_queue_unref(queue);
    }

    g_mutex_lock(&flime_bridge_lock);
    flime_planner_thread = NULL;
    flime_planner_queue = NULL;
    flime_planner_stopping = false;
    g_mutex_unlock(&flime_bridge_lock);
}
