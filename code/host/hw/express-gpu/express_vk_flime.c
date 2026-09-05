/*
 * FLIME host-side metadata, planning, and recovery core.
 *
 * The direct-routing template and authoritative descriptor shadow live on the
 * guest side.  This file intentionally handles metadata only; callers must
 * not treat ExpressVkFlimeProgressStage as proof of Vulkan semantic validity.
 */
#include "qemu/osdep.h"
#include "hw/express-gpu/express_vk_flime.h"

#include <float.h>
#include <math.h>
#include <string.h>

typedef struct ExpressVkFlimeUnitEstimate {
    uint32_t dispatch_end;
    uint64_t template_offset;
    uint64_t encoded_bytes;
    double guest_prepare_ns;
    double host_realize_ns;
    bool valid;
} ExpressVkFlimeUnitEstimate;

typedef struct ExpressVkFlimePlanSlot {
    ExpressVkFlimePlanBoundary *boundaries;
    uint32_t boundary_count;
    uint32_t unit_count;
    uint64_t epoch;
    uint64_t apply_period;
    uint64_t predicted_completion_ns;
    bool valid;
} ExpressVkFlimePlanSlot;

typedef struct ExpressVkFlimeLedgerEntry {
    uint64_t update_id;
    uint64_t submission_id;
    uint64_t template_offset;
    ExpressVkFlimeLedgerState state;
} ExpressVkFlimeLedgerEntry;

struct ExpressVkFlimeSession {
    gint ref_count;
    GMutex lock;
    uint64_t process_id;
    uint64_t stream_id;
    ExpressVkFlimeConfig config;

    bool negotiated;
    bool legacy_fallback;
    uint16_t major;
    uint16_t minor;
    uint64_t capabilities;
    uint32_t max_units;
    uint32_t max_chunks;
    uint32_t dispatches_per_unit;
    uint32_t replan_periods;

    uint64_t *history;
    uint32_t *kmp;
    uint64_t *history_hash;
    uint64_t *history_power;
    uint32_t history_count;
    ExpressVkFlimeProgressInfo progress;

    bool period_open;
    uint64_t current_period_id;
    uint64_t last_period_id;
    uint32_t current_period_flags;
    ExpressVkFlimeUnitSample *period_units;
    bool *period_unit_host_valid;
    uint32_t period_unit_count;
    uint32_t period_unit_capacity;
    uint32_t period_unit_valid_capacity;
    ExpressVkFlimeChunkSample *period_chunks;
    bool *period_chunk_host_valid;
    uint32_t period_chunk_count;
    uint32_t period_chunk_capacity;
    uint32_t period_chunk_valid_capacity;
    bool period_from_wire;

    ExpressVkFlimeUnitEstimate *estimates;
    uint32_t estimate_count;
    uint32_t estimate_capacity;
    double guest_handoff_ns;
    double host_handoff_ns;
    bool handoff_valid;
    uint64_t successful_stable_periods;
    uint32_t initial_fast_profiles;
    bool replan_due;
    uint64_t replan_apply_period;
    uint64_t replan_generation;

    ExpressVkFlimePlanSlot plans[2];
    unsigned active_plan;
    unsigned pending_plan;
    bool pending_valid;
    uint64_t next_plan_epoch;
    uint64_t acknowledged_plan_epoch;
    uint64_t acknowledged_apply_period;
    bool teardown_requested;

    ExpressVkFlimeLedgerEntry *ledger;
    GHashTable *ledger_index;
    uint32_t ledger_count;
    uint32_t ledger_capacity;
    uint64_t checkpoint_period_id;

    struct ExpressVkFlimeSession *manager_next;
};

struct ExpressVkFlimeManager {
    GMutex lock;
    ExpressVkFlimeConfig config;
    ExpressVkFlimeSession *sessions;
};

G_STATIC_ASSERT(sizeof(ExpressVkFlimeWireHeader) ==
                EXPRESS_VK_FLIME_WIRE_HEADER_SIZE);
G_STATIC_ASSERT(sizeof(ExpressVkFlimeWireCapabilities) ==
                EXPRESS_VK_FLIME_WIRE_CAPS_SIZE);
G_STATIC_ASSERT(sizeof(ExpressVkFlimeWireProfile) ==
                EXPRESS_VK_FLIME_WIRE_PROFILE_SIZE);
G_STATIC_ASSERT(sizeof(ExpressVkFlimeWireUnit) ==
                EXPRESS_VK_FLIME_WIRE_UNIT_SIZE);
G_STATIC_ASSERT(sizeof(ExpressVkFlimeWireChunk) ==
                EXPRESS_VK_FLIME_WIRE_CHUNK_SIZE);
G_STATIC_ASSERT(sizeof(ExpressVkFlimeWireProgress) ==
                EXPRESS_VK_FLIME_WIRE_PROGRESS_SIZE);
G_STATIC_ASSERT(sizeof(ExpressVkFlimeWirePlanAck) ==
                EXPRESS_VK_FLIME_WIRE_PLAN_ACK_SIZE);
G_STATIC_ASSERT(sizeof(ExpressVkFlimeControlHeader) ==
                EXPRESS_VK_FLIME_CONTROL_HEADER_SIZE);
G_STATIC_ASSERT(sizeof(ExpressVkFlimeControlBoundary) ==
                EXPRESS_VK_FLIME_CONTROL_BOUNDARY_SIZE);
G_STATIC_ASSERT(sizeof(ExpressVkFlimeCapsReply) ==
                EXPRESS_VK_FLIME_CAPS_REPLY_SIZE);

static bool flime_size_mul(size_t a, size_t b, size_t *result)
{
    if (a != 0 && b > G_MAXSIZE / a) {
        return false;
    }
    *result = a * b;
    return true;
}

static bool flime_size_add(size_t a, size_t b, size_t *result)
{
    if (b > G_MAXSIZE - a) {
        return false;
    }
    *result = a + b;
    return true;
}

static void *flime_try_array0(size_t count, size_t element_size)
{
    size_t bytes;

    if (!flime_size_mul(count, element_size, &bytes) ||
        bytes > G_MAXSSIZE) {
        return NULL;
    }
    return g_try_malloc0(bytes);
}

static bool flime_reserve_array(void **array, uint32_t *capacity,
                                uint32_t needed, uint32_t limit,
                                size_t element_size)
{
    uint32_t new_capacity;
    size_t old_bytes;
    size_t new_bytes;
    void *new_array;

    if (needed <= *capacity) {
        return true;
    }
    if (needed > limit) {
        return false;
    }

    new_capacity = *capacity ? *capacity : 16;
    while (new_capacity < needed) {
        if (new_capacity > limit / 2) {
            new_capacity = limit;
            break;
        }
        new_capacity *= 2;
    }
    if (!flime_size_mul(*capacity, element_size, &old_bytes) ||
        !flime_size_mul(new_capacity, element_size, &new_bytes)) {
        return false;
    }
    new_array = g_try_realloc(*array, new_bytes);
    if (new_array == NULL) {
        return false;
    }
    memset((uint8_t *)new_array + old_bytes, 0, new_bytes - old_bytes);
    *array = new_array;
    *capacity = new_capacity;
    return true;
}

GQuark express_vk_flime_error_quark(void)
{
    return g_quark_from_static_string("express-vk-flime-error-quark");
}

static void flime_clear_plan(ExpressVkFlimePlanSlot *plan)
{
    g_free(plan->boundaries);
    memset(plan, 0, sizeof(*plan));
}

static void flime_clear_profile_locked(ExpressVkFlimeSession *session)
{
    session->period_open = false;
    session->current_period_flags = 0;
    session->period_unit_count = 0;
    session->period_chunk_count = 0;
    session->period_from_wire = false;
}

static void flime_clear_specialization_locked(ExpressVkFlimeSession *session,
                                              bool clear_estimates)
{
    flime_clear_profile_locked(session);
    session->history_count = 0;
    memset(&session->progress, 0, sizeof(session->progress));
    session->progress.stage = EXPRESS_VK_FLIME_STAGE_DETECT;
    session->ledger_count = 0;
    if (session->ledger_index != NULL) {
        g_hash_table_remove_all(session->ledger_index);
    }
    session->checkpoint_period_id = 0;
    flime_clear_plan(&session->plans[0]);
    flime_clear_plan(&session->plans[1]);
    session->active_plan = 0;
    session->pending_plan = 1;
    session->pending_valid = false;
    session->acknowledged_plan_epoch = 0;
    session->acknowledged_apply_period = 0;
    session->successful_stable_periods = 0;
    session->initial_fast_profiles = 0;
    session->replan_due = false;
    session->replan_apply_period = 0;
    session->replan_generation++;
    session->handoff_valid = false;
    if (clear_estimates) {
        session->estimate_count = 0;
    }
}

static void flime_reset_protocol_locked(ExpressVkFlimeSession *session,
                                        bool clear_estimates)
{
    flime_clear_specialization_locked(session, clear_estimates);
    session->current_period_id = 0;
    session->last_period_id = 0;
    session->checkpoint_period_id = 0;
    session->teardown_requested = false;
}

static void flime_enter_legacy_locked(ExpressVkFlimeSession *session)
{
    flime_reset_protocol_locked(session, true);
    session->negotiated = false;
    session->legacy_fallback = true;
    session->capabilities = 0;
    session->progress.stage = EXPRESS_VK_FLIME_STAGE_LEGACY;
}

static bool flime_fail_locked(ExpressVkFlimeSession *session, GError **error,
                              ExpressVkFlimeError code, bool fallback,
                              const char *message)
{
    if (fallback && session != NULL) {
        flime_enter_legacy_locked(session);
    }
    g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR, code, message);
    return false;
}

static bool flime_ready_locked(ExpressVkFlimeSession *session, GError **error)
{
    if (!session->negotiated || session->legacy_fallback) {
        return flime_fail_locked(session, error,
                                 EXPRESS_VK_FLIME_ERROR_NOT_NEGOTIATED,
                                 false, "FLIME session is in legacy mode");
    }
    return true;
}

static uint64_t flime_normalize_capabilities(uint64_t caps)
{
    caps &= EXPRESS_VK_FLIME_CAP_ALL;
    if (!(caps & EXPRESS_VK_FLIME_CAP_PROGRESSIVE_METADATA) ||
        !(caps & EXPRESS_VK_FLIME_CAP_RECOVERY_LEDGER)) {
        caps &= ~EXPRESS_VK_FLIME_CAP_DIRECT_ROUTING;
    }
    if (!(caps & EXPRESS_VK_FLIME_CAP_UNIT_PROFILING) ||
        !(caps & EXPRESS_VK_FLIME_CAP_CHUNK_PROFILING) ||
        !(caps & EXPRESS_VK_FLIME_CAP_CONTROL_PLAN) ||
        !(caps & EXPRESS_VK_FLIME_CAP_DIRECT_ROUTING) ||
        !(caps & EXPRESS_VK_FLIME_CAP_RECOVERY_LEDGER)) {
        caps &= ~EXPRESS_VK_FLIME_CAP_ADAPTIVE_FORWARDING;
    }
    if (!(caps & EXPRESS_VK_FLIME_CAP_ADAPTIVE_FORWARDING) ||
        !(caps & EXPRESS_VK_FLIME_CAP_RECOVERY_LEDGER) ||
        !(caps & EXPRESS_VK_FLIME_CAP_DIRECT_ROUTING)) {
        caps &= ~EXPRESS_VK_FLIME_CAP_EARLY_RELEASE;
    }
    return caps;
}

void express_vk_flime_config_init(ExpressVkFlimeConfig *config)
{
    g_return_if_fail(config != NULL);

    memset(config, 0, sizeof(*config));
    config->host_capabilities = EXPRESS_VK_FLIME_CAP_ALL;
    config->history_limit = EXPRESS_VK_FLIME_DEFAULT_HISTORY_LIMIT;
    config->max_units = EXPRESS_VK_FLIME_DEFAULT_MAX_UNITS;
    config->max_chunks = EXPRESS_VK_FLIME_DEFAULT_MAX_CHUNKS;
    config->max_ledger_entries =
        EXPRESS_VK_FLIME_DEFAULT_MAX_LEDGER_ENTRIES;
    config->dispatches_per_unit =
        EXPRESS_VK_FLIME_PAPER_DISPATCHES_PER_UNIT;
    config->replan_periods = EXPRESS_VK_FLIME_PAPER_REPLAN_PERIODS;
    config->ewma_alpha = EXPRESS_VK_FLIME_DEFAULT_EWMA_ALPHA;
}

static bool flime_config_valid(const ExpressVkFlimeConfig *config)
{
    return config != NULL &&
           config->history_limit >= 2 &&
           config->history_limit <= EXPRESS_VK_FLIME_HARD_MAX_HISTORY &&
           config->max_units != 0 &&
           config->max_units <= EXPRESS_VK_FLIME_HARD_MAX_UNITS &&
           config->max_chunks != 0 &&
           config->max_chunks <= EXPRESS_VK_FLIME_HARD_MAX_CHUNKS &&
           config->max_chunks <= config->max_units &&
           config->max_ledger_entries != 0 &&
           config->max_ledger_entries <=
               EXPRESS_VK_FLIME_HARD_MAX_LEDGER_ENTRIES &&
           config->dispatches_per_unit != 0 &&
           config->dispatches_per_unit <= UINT32_MAX / config->max_units &&
           config->replan_periods != 0 &&
           isfinite(config->ewma_alpha) &&
           config->ewma_alpha > 0.0 && config->ewma_alpha <= 1.0;
}

ExpressVkFlimeManager *express_vk_flime_manager_new(
    const ExpressVkFlimeConfig *config, GError **error)
{
    ExpressVkFlimeConfig local;
    ExpressVkFlimeManager *manager;

    if (config == NULL) {
        express_vk_flime_config_init(&local);
        config = &local;
    }
    if (!flime_config_valid(config)) {
        g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                            EXPRESS_VK_FLIME_ERROR_INVALID_ARGUMENT,
                            "invalid FLIME manager configuration");
        return NULL;
    }
    manager = g_try_new0(ExpressVkFlimeManager, 1);
    if (manager == NULL) {
        g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                            EXPRESS_VK_FLIME_ERROR_OOM,
                            "cannot allocate FLIME manager");
        return NULL;
    }
    manager->config = *config;
    manager->config.host_capabilities =
        flime_normalize_capabilities(config->host_capabilities);
    g_mutex_init(&manager->lock);
    return manager;
}

static ExpressVkFlimeSession *flime_session_new(
    const ExpressVkFlimeConfig *config, uint64_t process_id,
    uint64_t stream_id, GError **error)
{
    ExpressVkFlimeSession *session;

    session = g_try_new0(ExpressVkFlimeSession, 1);
    if (session == NULL) {
        g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                            EXPRESS_VK_FLIME_ERROR_OOM,
                            "cannot allocate FLIME session");
        return NULL;
    }
    session->history = flime_try_array0(config->history_limit,
                                        sizeof(*session->history));
    session->kmp = flime_try_array0(config->history_limit,
                                    sizeof(*session->kmp));
    session->history_hash = flime_try_array0(
        (size_t)config->history_limit + 1, sizeof(*session->history_hash));
    session->history_power = flime_try_array0(
        (size_t)config->history_limit + 1, sizeof(*session->history_power));
    if (session->history == NULL || session->kmp == NULL ||
        session->history_hash == NULL || session->history_power == NULL) {
        g_free(session->history);
        g_free(session->kmp);
        g_free(session->history_hash);
        g_free(session->history_power);
        g_free(session);
        g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                            EXPRESS_VK_FLIME_ERROR_OOM,
                            "cannot allocate FLIME detection history");
        return NULL;
    }
    session->ref_count = 1; /* manager's reference */
    session->process_id = process_id;
    session->stream_id = stream_id;
    session->config = *config;
    session->legacy_fallback = true;
    session->progress.stage = EXPRESS_VK_FLIME_STAGE_LEGACY;
    session->active_plan = 0;
    session->pending_plan = 1;
    session->ledger_index = g_hash_table_new_full(
        g_int64_hash, g_int64_equal, g_free, NULL);
    g_mutex_init(&session->lock);
    return session;
}

ExpressVkFlimeSession *express_vk_flime_session_ref(
    ExpressVkFlimeSession *session)
{
    g_return_val_if_fail(session != NULL, NULL);
    g_atomic_int_inc(&session->ref_count);
    return session;
}

void express_vk_flime_session_unref(ExpressVkFlimeSession *session)
{
    if (session == NULL || !g_atomic_int_dec_and_test(&session->ref_count)) {
        return;
    }
    flime_clear_plan(&session->plans[0]);
    flime_clear_plan(&session->plans[1]);
    g_free(session->history);
    g_free(session->kmp);
    g_free(session->history_hash);
    g_free(session->history_power);
    g_free(session->period_units);
    g_free(session->period_unit_host_valid);
    g_free(session->period_chunks);
    g_free(session->period_chunk_host_valid);
    g_free(session->estimates);
    g_free(session->ledger);
    g_hash_table_destroy(session->ledger_index);
    g_mutex_clear(&session->lock);
    g_free(session);
}

void express_vk_flime_manager_free(ExpressVkFlimeManager *manager)
{
    ExpressVkFlimeSession *session;

    if (manager == NULL) {
        return;
    }
    g_mutex_lock(&manager->lock);
    session = manager->sessions;
    manager->sessions = NULL;
    g_mutex_unlock(&manager->lock);
    while (session != NULL) {
        ExpressVkFlimeSession *next = session->manager_next;

        session->manager_next = NULL;
        express_vk_flime_session_unref(session);
        session = next;
    }
    g_mutex_clear(&manager->lock);
    g_free(manager);
}

ExpressVkFlimeSession *express_vk_flime_manager_acquire(
    ExpressVkFlimeManager *manager, uint64_t process_id, uint64_t stream_id,
    GError **error)
{
    ExpressVkFlimeSession *session;

    if (manager == NULL || stream_id == 0) {
        g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                            EXPRESS_VK_FLIME_ERROR_INVALID_ARGUMENT,
                            "FLIME manager is NULL or stream id is zero");
        return NULL;
    }
    g_mutex_lock(&manager->lock);
    for (session = manager->sessions; session != NULL;
         session = session->manager_next) {
        if (session->process_id == process_id &&
            session->stream_id == stream_id) {
            express_vk_flime_session_ref(session);
            g_mutex_unlock(&manager->lock);
            return session;
        }
    }
    session = flime_session_new(&manager->config, process_id, stream_id,
                                error);
    if (session != NULL) {
        session->manager_next = manager->sessions;
        manager->sessions = session;
        express_vk_flime_session_ref(session); /* caller's reference */
    }
    g_mutex_unlock(&manager->lock);
    return session;
}

bool express_vk_flime_manager_remove(ExpressVkFlimeManager *manager,
                                     uint64_t process_id,
                                     uint64_t stream_id)
{
    ExpressVkFlimeSession **link;
    ExpressVkFlimeSession *removed = NULL;

    if (manager == NULL) {
        return false;
    }
    g_mutex_lock(&manager->lock);
    for (link = &manager->sessions; *link != NULL;
         link = &(*link)->manager_next) {
        if ((*link)->process_id == process_id &&
            (*link)->stream_id == stream_id) {
            removed = *link;
            *link = removed->manager_next;
            removed->manager_next = NULL;
            break;
        }
    }
    g_mutex_unlock(&manager->lock);
    express_vk_flime_session_unref(removed);
    return removed != NULL;
}

uint64_t express_vk_flime_session_process_id(ExpressVkFlimeSession *session)
{
    return session != NULL ? session->process_id : 0;
}

uint64_t express_vk_flime_session_stream_id(ExpressVkFlimeSession *session)
{
    return session != NULL ? session->stream_id : 0;
}

static bool flime_u64_add(uint64_t a, uint64_t b, uint64_t *result)
{
    if (b > UINT64_MAX - a) {
        return false;
    }
    *result = a + b;
    return true;
}

static uint16_t flime_get_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t flime_get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t flime_get_le64(const uint8_t *p)
{
    return (uint64_t)flime_get_le32(p) |
           ((uint64_t)flime_get_le32(p + 4) << 32);
}

static void flime_put_le16(uint8_t *p, uint16_t value)
{
    p[0] = value;
    p[1] = value >> 8;
}

static void flime_put_le32(uint8_t *p, uint32_t value)
{
    p[0] = value;
    p[1] = value >> 8;
    p[2] = value >> 16;
    p[3] = value >> 24;
}

static void flime_put_le64(uint8_t *p, uint64_t value)
{
    flime_put_le32(p, value);
    flime_put_le32(p + 4, value >> 32);
}

static int flime_version_compare(uint16_t a_major, uint16_t a_minor,
                                 uint16_t b_major, uint16_t b_minor)
{
    if (a_major != b_major) {
        return a_major < b_major ? -1 : 1;
    }
    if (a_minor != b_minor) {
        return a_minor < b_minor ? -1 : 1;
    }
    return 0;
}

static void flime_fill_negotiated_locked(ExpressVkFlimeSession *session,
                                         ExpressVkFlimeNegotiated *result)
{
    if (result == NULL) {
        return;
    }
    memset(result, 0, sizeof(*result));
    result->major = session->major;
    result->minor = session->minor;
    result->capabilities = session->capabilities;
    result->max_units = session->max_units;
    result->max_chunks = session->max_chunks;
    result->dispatches_per_unit = session->dispatches_per_unit;
    result->replan_periods = session->replan_periods;
    result->legacy_fallback = session->legacy_fallback;
}

bool express_vk_flime_session_negotiate(ExpressVkFlimeSession *session,
                                        const ExpressVkFlimePeerCaps *peer,
                                        ExpressVkFlimeNegotiated *result,
                                        GError **error)
{
    uint64_t capabilities;

    if (session == NULL || peer == NULL || peer->max_units == 0 ||
        peer->max_chunks == 0 || peer->dispatches_per_unit == 0 ||
        peer->replan_periods == 0 ||
        flime_version_compare(peer->min_major, peer->min_minor,
                              peer->max_major, peer->max_minor) > 0) {
        g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                            EXPRESS_VK_FLIME_ERROR_INVALID_ARGUMENT,
                            "invalid FLIME capability range");
        return false;
    }

    g_mutex_lock(&session->lock);
    if (flime_version_compare(peer->min_major, peer->min_minor,
                              EXPRESS_VK_FLIME_PROTOCOL_MAJOR,
                              EXPRESS_VK_FLIME_PROTOCOL_MINOR) > 0 ||
        flime_version_compare(peer->max_major, peer->max_minor,
                              EXPRESS_VK_FLIME_PROTOCOL_MAJOR,
                              EXPRESS_VK_FLIME_PROTOCOL_MINOR) < 0) {
        flime_enter_legacy_locked(session);
        g_mutex_unlock(&session->lock);
        g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                            EXPRESS_VK_FLIME_ERROR_UNSUPPORTED_VERSION,
                            "no compatible FLIME protocol version");
        return false;
    }

    capabilities = flime_normalize_capabilities(
        peer->capabilities & session->config.host_capabilities);
    if (peer->dispatches_per_unit != session->config.dispatches_per_unit ||
        peer->replan_periods != session->config.replan_periods) {
        capabilities &= ~(EXPRESS_VK_FLIME_CAP_ADAPTIVE_FORWARDING |
                          EXPRESS_VK_FLIME_CAP_EARLY_RELEASE);
    }

    flime_reset_protocol_locked(session, true);
    session->major = EXPRESS_VK_FLIME_PROTOCOL_MAJOR;
    session->minor = EXPRESS_VK_FLIME_PROTOCOL_MINOR;
    session->capabilities = capabilities;
    session->max_units = MIN(MIN(peer->max_units, session->config.max_units),
                             EXPRESS_VK_FLIME_HARD_MAX_UNITS);
    session->max_chunks = MIN(MIN(peer->max_chunks,
                                  session->config.max_chunks),
                              EXPRESS_VK_FLIME_HARD_MAX_CHUNKS);
    session->max_chunks = MIN(session->max_chunks, session->max_units);
    session->dispatches_per_unit = session->config.dispatches_per_unit;
    session->replan_periods = session->config.replan_periods;
    session->negotiated = capabilities != 0;
    session->legacy_fallback = capabilities == 0;
    session->progress.stage = session->legacy_fallback ?
        EXPRESS_VK_FLIME_STAGE_LEGACY : EXPRESS_VK_FLIME_STAGE_DETECT;
    flime_fill_negotiated_locked(session, result);
    g_mutex_unlock(&session->lock);
    return true;
}

void express_vk_flime_session_get_negotiated(ExpressVkFlimeSession *session,
                                             ExpressVkFlimeNegotiated *result)
{
    if (result == NULL) {
        return;
    }
    memset(result, 0, sizeof(*result));
    if (session == NULL) {
        result->legacy_fallback = true;
        return;
    }
    g_mutex_lock(&session->lock);
    flime_fill_negotiated_locked(session, result);
    g_mutex_unlock(&session->lock);
}

bool express_vk_flime_session_is_legacy(ExpressVkFlimeSession *session)
{
    bool legacy;

    if (session == NULL) {
        return true;
    }
    g_mutex_lock(&session->lock);
    legacy = session->legacy_fallback;
    g_mutex_unlock(&session->lock);
    return legacy;
}

bool express_vk_flime_session_encode_caps_reply(
    ExpressVkFlimeSession *session, void *reply, size_t reply_capacity,
    size_t *reply_bytes, GError **error)
{
    uint8_t *out = reply;

    if (reply_bytes != NULL) {
        *reply_bytes = EXPRESS_VK_FLIME_CAPS_REPLY_SIZE;
    }
    if (session == NULL || reply == NULL ||
        reply_capacity < EXPRESS_VK_FLIME_CAPS_REPLY_SIZE) {
        g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                            EXPRESS_VK_FLIME_ERROR_INVALID_ARGUMENT,
                            "capability reply buffer is too small");
        return false;
    }

    memset(out, 0, EXPRESS_VK_FLIME_CAPS_REPLY_SIZE);
    g_mutex_lock(&session->lock);
    flime_put_le32(out + 0, EXPRESS_VK_FLIME_CAPS_REPLY_MAGIC);
    flime_put_le16(out + 4, EXPRESS_VK_FLIME_PROTOCOL_MAJOR);
    flime_put_le16(out + 6, EXPRESS_VK_FLIME_PROTOCOL_MINOR);
    flime_put_le16(out + 8, EXPRESS_VK_FLIME_CAPS_REPLY_SIZE);
    flime_put_le16(out + 10, session->legacy_fallback ?
                   EXPRESS_VK_FLIME_CAPS_REPLY_LEGACY :
                   EXPRESS_VK_FLIME_CAPS_REPLY_NEGOTIATED);
    flime_put_le64(out + 16, session->capabilities);
    flime_put_le32(out + 24, session->max_units);
    flime_put_le32(out + 28, session->max_chunks);
    flime_put_le32(out + 32, session->dispatches_per_unit);
    flime_put_le32(out + 36, session->replan_periods);
    g_mutex_unlock(&session->lock);
    return true;
}

static double flime_ewma(double old_value, uint64_t sample,
                         double alpha, bool old_valid)
{
    if (!old_valid) {
        return (double)sample;
    }
    return alpha * (double)sample + (1.0 - alpha) * old_value;
}

static bool flime_plan_locked(ExpressVkFlimeSession *session,
                              uint64_t apply_period, GError **error)
{
    ExpressVkFlimePlanBoundary *boundaries = NULL;
    double *guest_prefix = NULL;
    double *host_prefix = NULL;
    double *dp = NULL;
    uint32_t *predecessor = NULL;
    size_t columns;
    size_t cells;
    uint32_t n = session->estimate_count;
    uint32_t k_limit;
    uint32_t best_k = 0;
    double best = DBL_MAX;
    uint32_t i;
    uint32_t k;
    ExpressVkFlimePlanSlot *slot;

    if (!(session->capabilities &
          EXPRESS_VK_FLIME_CAP_ADAPTIVE_FORWARDING)) {
        return flime_fail_locked(session, error,
                                 EXPRESS_VK_FLIME_ERROR_STATE, false,
                                 "adaptive forwarding was not negotiated");
    }
    if (n == 0 || n > session->max_units) {
        return flime_fail_locked(session, error,
                                 EXPRESS_VK_FLIME_ERROR_STATE, false,
                                 "no bounded unit profile is available");
    }
    for (i = 0; i < n; i++) {
        if (!session->estimates[i].valid ||
            !isfinite(session->estimates[i].guest_prepare_ns) ||
            !isfinite(session->estimates[i].host_realize_ns)) {
            return flime_fail_locked(session, error,
                                     EXPRESS_VK_FLIME_ERROR_STATE, false,
                                     "unit estimate is incomplete");
        }
    }

    k_limit = session->capabilities & EXPRESS_VK_FLIME_CAP_EARLY_RELEASE ?
        MIN(session->max_chunks, n) : 1;
    columns = (size_t)n + 1;
    if (!flime_size_mul((size_t)k_limit + 1, columns, &cells)) {
        return flime_fail_locked(session, error,
                                 EXPRESS_VK_FLIME_ERROR_OVERFLOW, false,
                                 "planner matrix size overflow");
    }
    guest_prefix = flime_try_array0(columns, sizeof(*guest_prefix));
    host_prefix = flime_try_array0(columns, sizeof(*host_prefix));
    dp = flime_try_array0(cells, sizeof(*dp));
    predecessor = flime_try_array0(cells, sizeof(*predecessor));
    if (guest_prefix == NULL || host_prefix == NULL || dp == NULL ||
        predecessor == NULL) {
        g_free(guest_prefix);
        g_free(host_prefix);
        g_free(dp);
        g_free(predecessor);
        return flime_fail_locked(session, error,
                                 EXPRESS_VK_FLIME_ERROR_OOM, false,
                                 "cannot allocate planner matrices");
    }

    for (i = 0; i < cells; i++) {
        dp[i] = DBL_MAX;
        predecessor[i] = UINT32_MAX;
    }
    for (i = 1; i <= n; i++) {
        guest_prefix[i] = guest_prefix[i - 1] +
            session->estimates[i - 1].guest_prepare_ns;
        host_prefix[i] = host_prefix[i - 1] +
            session->estimates[i - 1].host_realize_ns;
    }
    dp[0] = 0.0;

    /*
     * Paper recurrence (O(K n^2)):
     * A(i,k) = P_i + k sigma_G
     * D[i,k] = min_j max(D[j,k-1], A(i,k)) + sigma_H + H_i-H_j.
     */
    for (k = 1; k <= k_limit; k++) {
        for (i = k; i <= n; i++) {
            double availability = guest_prefix[i] +
                (double)k * (session->handoff_valid ?
                             session->guest_handoff_ns : 0.0);
            uint32_t j;

            for (j = k - 1; j < i; j++) {
                double previous = dp[(size_t)(k - 1) * columns + j];
                double candidate;

                if (previous == DBL_MAX) {
                    continue;
                }
                candidate = MAX(previous, availability) +
                    (session->handoff_valid ?
                     session->host_handoff_ns : 0.0) +
                    host_prefix[i] - host_prefix[j];
                if (candidate < dp[(size_t)k * columns + i]) {
                    dp[(size_t)k * columns + i] = candidate;
                    predecessor[(size_t)k * columns + i] = j;
                }
            }
        }
        if (dp[(size_t)k * columns + n] < best) {
            best = dp[(size_t)k * columns + n];
            best_k = k;
        }
    }
    if (best_k == 0 || !isfinite(best)) {
        g_free(guest_prefix);
        g_free(host_prefix);
        g_free(dp);
        g_free(predecessor);
        return flime_fail_locked(session, error,
                                 EXPRESS_VK_FLIME_ERROR_OVERFLOW, false,
                                 "planner produced no finite schedule");
    }

    boundaries = flime_try_array0(best_k, sizeof(*boundaries));
    if (boundaries == NULL) {
        g_free(guest_prefix);
        g_free(host_prefix);
        g_free(dp);
        g_free(predecessor);
        return flime_fail_locked(session, error,
                                 EXPRESS_VK_FLIME_ERROR_OOM, false,
                                 "cannot allocate forwarding boundaries");
    }
    i = n;
    for (k = best_k; k > 0; k--) {
        uint32_t j = predecessor[(size_t)k * columns + i];
        uint64_t boundary_offset;

        if (j == UINT32_MAX ||
            !flime_u64_add(session->estimates[i - 1].template_offset,
                           session->estimates[i - 1].encoded_bytes,
                           &boundary_offset)) {
            g_free(boundaries);
            g_free(guest_prefix);
            g_free(host_prefix);
            g_free(dp);
            g_free(predecessor);
            return flime_fail_locked(session, error,
                                     EXPRESS_VK_FLIME_ERROR_OVERFLOW, false,
                                     "invalid planner predecessor or offset");
        }
        boundaries[k - 1].unit_past_end = i;
        boundaries[k - 1].flags = i == n ?
            EXPRESS_VK_FLIME_UNIT_FINAL : 0;
        boundaries[k - 1].template_offset = boundary_offset;
        i = j;
    }

    slot = &session->plans[session->pending_plan];
    flime_clear_plan(slot);
    if (session->next_plan_epoch == UINT64_MAX) {
        g_free(boundaries);
        g_free(guest_prefix);
        g_free(host_prefix);
        g_free(dp);
        g_free(predecessor);
        return flime_fail_locked(session, error,
                                 EXPRESS_VK_FLIME_ERROR_OVERFLOW, false,
                                 "plan epoch overflow");
    }
    slot->boundaries = boundaries;
    slot->boundary_count = best_k;
    slot->unit_count = n;
    slot->epoch = ++session->next_plan_epoch;
    slot->apply_period = apply_period;
    slot->predicted_completion_ns = best >= (double)UINT64_MAX ?
        UINT64_MAX : (uint64_t)ceil(best);
    slot->valid = true;
    session->pending_valid = true;

    g_free(guest_prefix);
    g_free(host_prefix);
    g_free(dp);
    g_free(predecessor);
    return true;
}

static bool flime_validate_unit_locked(ExpressVkFlimeSession *session,
                                       const ExpressVkFlimeUnitSample *sample,
                                       GError **error)
{
    uint32_t previous_dispatch = session->period_unit_count == 0 ? 0 :
        session->period_units[session->period_unit_count - 1].dispatch_end;
    uint64_t sample_end;
    uint64_t previous_end = 0;
    uint32_t dispatch_count = sample->dispatch_end - previous_dispatch;

    if (session->period_unit_count != 0 &&
        !flime_u64_add(
            session->period_units[session->period_unit_count - 1].template_offset,
            session->period_units[session->period_unit_count - 1].encoded_bytes,
            &previous_end)) {
        return flime_fail_locked(session, error,
                                 EXPRESS_VK_FLIME_ERROR_OVERFLOW, false,
                                 "previous unit boundary overflow");
    }

    if (sample->unit_index != session->period_unit_count ||
        (sample->flags & ~EXPRESS_VK_FLIME_UNIT_FINAL) != 0 ||
        sample->dispatch_end <= previous_dispatch ||
        dispatch_count > session->dispatches_per_unit ||
        (!(sample->flags & EXPRESS_VK_FLIME_UNIT_FINAL) &&
         dispatch_count != session->dispatches_per_unit) ||
        (session->period_unit_count != 0 &&
         (session->period_units[session->period_unit_count - 1].flags &
          EXPRESS_VK_FLIME_UNIT_FINAL)) ||
        sample->encoded_bytes == 0 ||
        sample->template_offset < previous_end ||
        !flime_u64_add(sample->template_offset, sample->encoded_bytes,
                       &sample_end)) {
        return flime_fail_locked(session, error,
                                 EXPRESS_VK_FLIME_ERROR_INVALID_ARGUMENT,
                                 false, "invalid or non-contiguous unit sample");
    }
    return true;
}

static bool flime_validate_chunk_locked(
    ExpressVkFlimeSession *session, const ExpressVkFlimeChunkSample *sample,
    GError **error)
{
    uint32_t expected_first = session->period_chunk_count == 0 ? 0 :
        session->period_chunks[session->period_chunk_count - 1].unit_past_end;

    if (sample->chunk_index != session->period_chunk_count ||
        sample->flags != 0 || sample->first_unit != expected_first ||
        sample->unit_past_end <= sample->first_unit ||
        sample->unit_past_end > session->period_unit_count) {
        return flime_fail_locked(session, error,
                                 EXPRESS_VK_FLIME_ERROR_INVALID_ARGUMENT,
                                 false,
                                 "invalid or non-contiguous chunk sample");
    }
    return true;
}

static void flime_install_pending_locked(ExpressVkFlimeSession *session)
{
    unsigned old_active = session->active_plan;

    flime_clear_plan(&session->plans[old_active]);
    session->active_plan = session->pending_plan;
    session->pending_plan = old_active;
    session->pending_valid = false;
}

static bool flime_has_published_plan_locked(ExpressVkFlimeSession *session)
{
    return session->pending_valid ||
           session->plans[session->active_plan].valid;
}

static bool flime_has_selected_plan_locked(ExpressVkFlimeSession *session,
                                           uint64_t period_id)
{
    ExpressVkFlimePlanSlot *pending;

    if (session->plans[session->active_plan].valid) {
        return true;
    }
    if (!session->pending_valid) {
        return false;
    }
    pending = &session->plans[session->pending_plan];
    return pending->valid &&
           session->acknowledged_plan_epoch == pending->epoch &&
           session->acknowledged_apply_period == period_id;
}

static bool flime_fine_profile_due_locked(ExpressVkFlimeSession *session)
{
    if (!(session->capabilities &
          EXPRESS_VK_FLIME_CAP_ADAPTIVE_FORWARDING)) {
        return false;
    }
    if (!flime_has_published_plan_locked(session)) {
        return true;
    }
    return session->successful_stable_periods % session->replan_periods ==
           session->replan_periods - 1;
}

static bool flime_period_begin(ExpressVkFlimeSession *session,
                               uint64_t period_id, uint32_t flags,
                               bool require_active_stage,
                               bool *installed_new_plan, GError **error)
{
    const uint32_t allowed = EXPRESS_VK_FLIME_PERIOD_SINGLE_FLUSH |
        EXPRESS_VK_FLIME_PERIOD_FINE_PROFILE |
        EXPRESS_VK_FLIME_PERIOD_STABLE_FAST |
        EXPRESS_VK_FLIME_PERIOD_FORCE_REPLAN;

    if (installed_new_plan != NULL) {
        *installed_new_plan = false;
    }
    if (session == NULL || period_id == 0 || (flags & ~allowed) != 0) {
        g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                            EXPRESS_VK_FLIME_ERROR_INVALID_ARGUMENT,
                            "invalid FLIME period begin");
        return false;
    }
    g_mutex_lock(&session->lock);
    if (require_active_stage &&
        session->progress.stage != EXPRESS_VK_FLIME_STAGE_LEARN &&
        session->progress.stage != EXPRESS_VK_FLIME_STAGE_MATCH &&
        session->progress.stage != EXPRESS_VK_FLIME_STAGE_FAST) {
        bool ret = flime_fail_locked(
            session, error, EXPRESS_VK_FLIME_ERROR_STATE, false,
            "wire period begin is only valid in Learn, Match, or Fast stage");

        g_mutex_unlock(&session->lock);
        return ret;
    }
    if (!flime_ready_locked(session, error)) {
        g_mutex_unlock(&session->lock);
        return false;
    }
    if (session->period_open || period_id <= session->last_period_id) {
        bool ret = flime_fail_locked(session, error,
                                     EXPRESS_VK_FLIME_ERROR_STATE, false,
                                     "period is already open or not monotonic");
        g_mutex_unlock(&session->lock);
        return ret;
    }
    if ((flags & EXPRESS_VK_FLIME_PERIOD_STABLE_FAST) &&
        session->progress.stage != EXPRESS_VK_FLIME_STAGE_FAST) {
        bool ret = flime_fail_locked(
            session, error, EXPRESS_VK_FLIME_ERROR_STATE, false,
            "stable-period flag is only valid in FLIME Fast stage");

        g_mutex_unlock(&session->lock);
        return ret;
    }
    if (session->progress.stage == EXPRESS_VK_FLIME_STAGE_FAST &&
        flime_has_selected_plan_locked(session, period_id) &&
        !(flags & EXPRESS_VK_FLIME_PERIOD_STABLE_FAST)) {
        bool ret = flime_fail_locked(
            session, error, EXPRESS_VK_FLIME_ERROR_STATE, false,
            "planned FLIME Fast period must be marked stable");

        g_mutex_unlock(&session->lock);
        return ret;
    }
    if (flime_fine_profile_due_locked(session) &&
        !(flags & EXPRESS_VK_FLIME_PERIOD_FINE_PROFILE)) {
        bool ret = flime_fail_locked(
            session, error, EXPRESS_VK_FLIME_ERROR_STATE, false,
            "requested FLIME fine profile is missing from period flags");

        g_mutex_unlock(&session->lock);
        return ret;
    }
    if ((session->capabilities &
         EXPRESS_VK_FLIME_CAP_ADAPTIVE_FORWARDING) &&
        !flime_has_published_plan_locked(session) &&
        !(flags & EXPRESS_VK_FLIME_PERIOD_SINGLE_FLUSH)) {
        bool ret = flime_fail_locked(
            session, error, EXPRESS_VK_FLIME_ERROR_STATE, false,
            "initial FLIME profile must use a single flush");

        g_mutex_unlock(&session->lock);
        return ret;
    }
    if (session->pending_valid &&
        session->plans[session->pending_plan].apply_period == period_id &&
        session->acknowledged_plan_epoch ==
            session->plans[session->pending_plan].epoch &&
        session->acknowledged_apply_period == period_id) {
        flime_install_pending_locked(session);
        if (installed_new_plan != NULL) {
            *installed_new_plan = true;
        }
    }
    session->period_open = true;
    session->current_period_id = period_id;
    session->last_period_id = period_id;
    session->current_period_flags = flags;
    session->period_from_wire = require_active_stage;
    session->period_unit_count = 0;
    session->period_chunk_count = 0;
    g_mutex_unlock(&session->lock);
    return true;
}

bool express_vk_flime_session_period_begin(ExpressVkFlimeSession *session,
                                           uint64_t period_id,
                                           uint32_t flags,
                                           bool *installed_new_plan,
                                           GError **error)
{
    return flime_period_begin(session, period_id, flags, false,
                              installed_new_plan, error);
}

void express_vk_flime_session_get_period(ExpressVkFlimeSession *session,
                                         ExpressVkFlimePeriodInfo *info)
{
    if (info == NULL) {
        return;
    }
    memset(info, 0, sizeof(*info));
    if (session == NULL) {
        return;
    }
    g_mutex_lock(&session->lock);
    info->open = session->period_open;
    info->current_period_id = session->current_period_id;
    info->last_period_id = session->last_period_id;
    info->flags = session->current_period_flags;
    g_mutex_unlock(&session->lock);
}

bool express_vk_flime_session_validate_open_period(
    ExpressVkFlimeSession *session, uint64_t period_id,
    uint32_t required_flags, GError **error)
{
    bool valid;

    if (session == NULL || period_id == 0) {
        g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                            EXPRESS_VK_FLIME_ERROR_INVALID_ARGUMENT,
                            "invalid FLIME period validation request");
        return false;
    }
    g_mutex_lock(&session->lock);
    valid = session->period_open &&
        session->current_period_id == period_id &&
        (session->current_period_flags & required_flags) == required_flags;
    g_mutex_unlock(&session->lock);
    if (!valid) {
        g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                            EXPRESS_VK_FLIME_ERROR_STATE,
                            "route packet is outside its declared period");
    }
    return valid;
}

bool express_vk_flime_session_validate_recovery_period(
    ExpressVkFlimeSession *session, uint64_t period_id, GError **error)
{
    bool valid;

    if (session == NULL || period_id == 0) {
        g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                            EXPRESS_VK_FLIME_ERROR_INVALID_ARGUMENT,
                            "invalid FLIME recovery period request");
        return false;
    }
    g_mutex_lock(&session->lock);
    valid = session->progress.stage == EXPRESS_VK_FLIME_STAGE_RECOVER &&
        session->current_period_id == period_id &&
        session->last_period_id == period_id;
    g_mutex_unlock(&session->lock);
    if (!valid) {
        g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                            EXPRESS_VK_FLIME_ERROR_STATE,
                            "recovery route does not match the failed period");
    }
    return valid;
}

bool express_vk_flime_session_profile_unit(
    ExpressVkFlimeSession *session,
    const ExpressVkFlimeUnitSample *sample,
    GError **error)
{
    if (session == NULL || sample == NULL) {
        g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                            EXPRESS_VK_FLIME_ERROR_INVALID_ARGUMENT,
                            "unit sample is NULL");
        return false;
    }
    g_mutex_lock(&session->lock);
    if (!flime_ready_locked(session, error) || !session->period_open ||
        !(session->capabilities & EXPRESS_VK_FLIME_CAP_UNIT_PROFILING)) {
        if (error != NULL && *error == NULL) {
            g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                                EXPRESS_VK_FLIME_ERROR_STATE,
                                "unit profiling is not active");
        }
        g_mutex_unlock(&session->lock);
        return false;
    }
    if (session->period_unit_count >= session->max_units ||
        !flime_validate_unit_locked(session, sample, error)) {
        if (session->period_unit_count >= session->max_units &&
            error != NULL && *error == NULL) {
            g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                                EXPRESS_VK_FLIME_ERROR_LIMIT,
                                "unit profile exceeds negotiated limit");
        }
        g_mutex_unlock(&session->lock);
        return false;
    }
    if (!flime_reserve_array((void **)&session->period_units,
                             &session->period_unit_capacity,
                             session->period_unit_count + 1,
                             session->max_units,
                             sizeof(*session->period_units)) ||
        !flime_reserve_array((void **)&session->period_unit_host_valid,
                             &session->period_unit_valid_capacity,
                             session->period_unit_count + 1,
                             session->max_units,
                             sizeof(*session->period_unit_host_valid))) {
        g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                            EXPRESS_VK_FLIME_ERROR_OOM,
                            "cannot grow unit profile");
        g_mutex_unlock(&session->lock);
        return false;
    }
    session->period_units[session->period_unit_count] = *sample;
    session->period_units[session->period_unit_count].host_realize_ns = 0;
    session->period_unit_host_valid[session->period_unit_count] = false;
    session->period_unit_count++;
    g_mutex_unlock(&session->lock);
    return true;
}

bool express_vk_flime_session_profile_chunk(
    ExpressVkFlimeSession *session,
    const ExpressVkFlimeChunkSample *sample,
    GError **error)
{
    if (session == NULL || sample == NULL) {
        g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                            EXPRESS_VK_FLIME_ERROR_INVALID_ARGUMENT,
                            "chunk sample is NULL");
        return false;
    }
    g_mutex_lock(&session->lock);
    if (!flime_ready_locked(session, error) || !session->period_open ||
        !(session->capabilities & EXPRESS_VK_FLIME_CAP_CHUNK_PROFILING)) {
        if (error != NULL && *error == NULL) {
            g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                                EXPRESS_VK_FLIME_ERROR_STATE,
                                "chunk profiling is not active");
        }
        g_mutex_unlock(&session->lock);
        return false;
    }
    if (session->period_chunk_count >= session->max_chunks ||
        !flime_validate_chunk_locked(session, sample, error)) {
        if (session->period_chunk_count >= session->max_chunks &&
            error != NULL && *error == NULL) {
            g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                                EXPRESS_VK_FLIME_ERROR_LIMIT,
                                "chunk profile exceeds negotiated limit");
        }
        g_mutex_unlock(&session->lock);
        return false;
    }
    if (!flime_reserve_array((void **)&session->period_chunks,
                             &session->period_chunk_capacity,
                             session->period_chunk_count + 1,
                             session->max_chunks,
                             sizeof(*session->period_chunks)) ||
        !flime_reserve_array((void **)&session->period_chunk_host_valid,
                             &session->period_chunk_valid_capacity,
                             session->period_chunk_count + 1,
                             session->max_chunks,
                             sizeof(*session->period_chunk_host_valid))) {
        g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                            EXPRESS_VK_FLIME_ERROR_OOM,
                            "cannot grow chunk profile");
        g_mutex_unlock(&session->lock);
        return false;
    }
    session->period_chunks[session->period_chunk_count] = *sample;
    session->period_chunks[session->period_chunk_count].host_handoff_ns = 0;
    session->period_chunks[session->period_chunk_count].host_realize_ns = 0;
    session->period_chunks[session->period_chunk_count].completion_ns = 0;
    session->period_chunk_host_valid[session->period_chunk_count] = false;
    session->period_chunk_count++;
    g_mutex_unlock(&session->lock);
    return true;
}

bool express_vk_flime_session_profile_host_unit(
    ExpressVkFlimeSession *session, uint32_t unit_index,
    uint64_t host_realize_ns, GError **error)
{
    if (session == NULL) {
        g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                            EXPRESS_VK_FLIME_ERROR_INVALID_ARGUMENT,
                            "FLIME session is NULL");
        return false;
    }
    g_mutex_lock(&session->lock);
    if (!session->period_open || unit_index >= session->period_unit_count ||
        session->period_unit_host_valid[unit_index]) {
        bool ret = flime_fail_locked(
            session, error, EXPRESS_VK_FLIME_ERROR_STATE, false,
            "host unit timing is duplicate or out of range");
        g_mutex_unlock(&session->lock);
        return ret;
    }
    session->period_units[unit_index].host_realize_ns = host_realize_ns;
    session->period_unit_host_valid[unit_index] = true;
    g_mutex_unlock(&session->lock);
    return true;
}

bool express_vk_flime_session_profile_host_chunk(
    ExpressVkFlimeSession *session, uint32_t chunk_index,
    uint64_t host_handoff_ns, uint64_t host_realize_ns,
    uint64_t completion_ns, GError **error)
{
    if (session == NULL) {
        g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                            EXPRESS_VK_FLIME_ERROR_INVALID_ARGUMENT,
                            "FLIME session is NULL");
        return false;
    }
    g_mutex_lock(&session->lock);
    if (!session->period_open || chunk_index >= session->period_chunk_count ||
        session->period_chunk_host_valid[chunk_index]) {
        bool ret = flime_fail_locked(
            session, error, EXPRESS_VK_FLIME_ERROR_STATE, false,
            "host chunk timing is duplicate or out of range");
        g_mutex_unlock(&session->lock);
        return ret;
    }
    session->period_chunks[chunk_index].host_handoff_ns = host_handoff_ns;
    session->period_chunks[chunk_index].host_realize_ns = host_realize_ns;
    session->period_chunks[chunk_index].completion_ns = completion_ns;
    session->period_chunk_host_valid[chunk_index] = true;
    g_mutex_unlock(&session->lock);
    return true;
}

static bool flime_update_estimates_locked(ExpressVkFlimeSession *session,
                                          bool fine_profile,
                                          GError **error)
{
    uint32_t i;

    if (fine_profile) {
        if (!flime_reserve_array((void **)&session->estimates,
                                 &session->estimate_capacity,
                                 session->period_unit_count,
                                 session->max_units,
                                 sizeof(*session->estimates))) {
            return flime_fail_locked(session, error,
                                     EXPRESS_VK_FLIME_ERROR_OOM, false,
                                     "cannot grow unit estimates");
        }
        for (i = 0; i < session->period_unit_count; i++) {
            ExpressVkFlimeUnitEstimate *estimate = &session->estimates[i];
            const ExpressVkFlimeUnitSample *sample = &session->period_units[i];
            bool same_shape = estimate->valid &&
                estimate->dispatch_end == sample->dispatch_end &&
                estimate->template_offset == sample->template_offset &&
                estimate->encoded_bytes == sample->encoded_bytes;

            estimate->guest_prepare_ns = flime_ewma(
                estimate->guest_prepare_ns, sample->guest_prepare_ns,
                session->config.ewma_alpha, same_shape);
            estimate->host_realize_ns = flime_ewma(
                estimate->host_realize_ns, sample->host_realize_ns,
                session->config.ewma_alpha, same_shape);
            estimate->dispatch_end = sample->dispatch_end;
            estimate->template_offset = sample->template_offset;
            estimate->encoded_bytes = sample->encoded_bytes;
            estimate->valid = true;
        }
        for (i = session->period_unit_count; i < session->estimate_count; i++) {
            memset(&session->estimates[i], 0, sizeof(session->estimates[i]));
        }
        session->estimate_count = session->period_unit_count;
    }

    for (i = 0; i < session->period_chunk_count; i++) {
        const ExpressVkFlimeChunkSample *sample = &session->period_chunks[i];
        session->guest_handoff_ns = flime_ewma(
            session->guest_handoff_ns, sample->guest_handoff_ns,
            session->config.ewma_alpha, session->handoff_valid);
        session->host_handoff_ns = flime_ewma(
            session->host_handoff_ns, sample->host_handoff_ns,
            session->config.ewma_alpha, session->handoff_valid);
        session->handoff_valid = true;
    }
    return true;
}

bool express_vk_flime_session_period_end(ExpressVkFlimeSession *session,
                                         uint64_t elapsed_ns,
                                         bool successful,
                                         GError **error)
{
    bool adaptive;
    bool fast_profile;
    bool fine_profile;
    bool initial_profile;
    bool profile_eligible;
    bool stable_period;
    bool should_replan;
    uint64_t apply_period;

    (void)elapsed_ns;
    if (session == NULL) {
        g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                            EXPRESS_VK_FLIME_ERROR_INVALID_ARGUMENT,
                            "FLIME session is NULL");
        return false;
    }
    g_mutex_lock(&session->lock);
    if (!flime_ready_locked(session, error) || !session->period_open) {
        if (error != NULL && *error == NULL) {
            g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                                EXPRESS_VK_FLIME_ERROR_STATE,
                                "no FLIME period is open");
        }
        g_mutex_unlock(&session->lock);
        return false;
    }
    if (!successful) {
        flime_clear_profile_locked(session);
        g_mutex_unlock(&session->lock);
        return true;
    }
    if (session->period_unit_count == 0 ||
        !(session->period_units[session->period_unit_count - 1].flags &
          EXPRESS_VK_FLIME_UNIT_FINAL) ||
        ((session->capabilities &
          EXPRESS_VK_FLIME_CAP_ADAPTIVE_FORWARDING) &&
         session->period_chunk_count == 0) ||
        (session->period_chunk_count != 0 &&
         session->period_chunks[session->period_chunk_count - 1].unit_past_end !=
         session->period_unit_count)) {
        bool ret = flime_fail_locked(session, error,
                                     EXPRESS_VK_FLIME_ERROR_INVALID_ARGUMENT,
                                     false,
                                     "profile does not cover a complete period");
        flime_clear_profile_locked(session);
        g_mutex_unlock(&session->lock);
        return ret;
    }
    fine_profile = (session->current_period_flags &
                    EXPRESS_VK_FLIME_PERIOD_FINE_PROFILE) != 0;
    stable_period = (session->current_period_flags &
                     EXPRESS_VK_FLIME_PERIOD_STABLE_FAST) != 0;
    if (fine_profile) {
        for (uint32_t i = 0; i < session->period_unit_count; i++) {
            if (!session->period_unit_host_valid[i]) {
                bool ret = flime_fail_locked(
                    session, error, EXPRESS_VK_FLIME_ERROR_STATE, false,
                    "fine profile is missing host-owned unit timing");
                flime_clear_profile_locked(session);
                g_mutex_unlock(&session->lock);
                return ret;
            }
        }
    }
    for (uint32_t i = 0; i < session->period_chunk_count; i++) {
        if (!session->period_chunk_host_valid[i]) {
            bool ret = flime_fail_locked(
                session, error, EXPRESS_VK_FLIME_ERROR_STATE, false,
                "period is missing host-owned chunk timing");
            flime_clear_profile_locked(session);
            g_mutex_unlock(&session->lock);
            return ret;
        }
    }
    if (stable_period && session->successful_stable_periods == UINT64_MAX) {
        g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                            EXPRESS_VK_FLIME_ERROR_OVERFLOW,
                            "stable profile period count overflow");
        flime_clear_profile_locked(session);
        g_mutex_unlock(&session->lock);
        return false;
    }
    adaptive = (session->capabilities &
                EXPRESS_VK_FLIME_CAP_ADAPTIVE_FORWARDING) != 0;
    fast_profile =
        session->progress.stage == EXPRESS_VK_FLIME_STAGE_FAST;
    /*
     * Learn and Match use the authoritative generic shadow, so their guest
     * preparation costs are not representative of direct-routing Fast.
     * Negotiated wire sessions therefore collect a few single-flush Fast
     * periods before publishing their first adaptive plan.  The public,
     * non-wire profiling API remains immediately plannable.
     */
    profile_eligible = !session->period_from_wire || fast_profile;
    if (session->period_from_wire && fast_profile &&
        !flime_has_published_plan_locked(session)) {
        if (session->initial_fast_profiles == UINT32_MAX) {
            g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                                EXPRESS_VK_FLIME_ERROR_OVERFLOW,
                                "initial Fast profile count overflow");
            flime_clear_profile_locked(session);
            g_mutex_unlock(&session->lock);
            return false;
        }
        session->initial_fast_profiles++;
    }
    initial_profile = adaptive &&
        !flime_has_published_plan_locked(session) &&
        (!session->period_from_wire ||
         session->initial_fast_profiles >=
             EXPRESS_VK_FLIME_PAPER_INITIAL_FAST_PROFILES);
    if (profile_eligible) {
        if (!flime_update_estimates_locked(session, fine_profile, error)) {
            flime_clear_profile_locked(session);
            g_mutex_unlock(&session->lock);
            return false;
        }
        session->replan_generation++;
    }
    if (stable_period) {
        session->successful_stable_periods++;
    }
    should_replan = adaptive && profile_eligible &&
        (initial_profile ||
         (flime_has_published_plan_locked(session) && fine_profile) ||
        (session->current_period_flags &
         EXPRESS_VK_FLIME_PERIOD_FORCE_REPLAN));
    if (should_replan) {
        if (!flime_u64_add(session->current_period_id, 1, &apply_period)) {
            g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                                EXPRESS_VK_FLIME_ERROR_OVERFLOW,
                                "next plan period overflow");
            flime_clear_profile_locked(session);
            g_mutex_unlock(&session->lock);
            return false;
        }
        session->replan_due = true;
        session->replan_apply_period = apply_period;
    }
    flime_clear_profile_locked(session);
    g_mutex_unlock(&session->lock);
    return true;
}

bool express_vk_flime_session_force_replan(ExpressVkFlimeSession *session,
                                           GError **error)
{
    uint64_t apply_period;
    bool result;

    if (session == NULL) {
        g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                            EXPRESS_VK_FLIME_ERROR_INVALID_ARGUMENT,
                            "FLIME session is NULL");
        return false;
    }
    g_mutex_lock(&session->lock);
    if (!flime_ready_locked(session, error) ||
        !flime_u64_add(session->last_period_id, 1, &apply_period)) {
        if (error != NULL && *error == NULL) {
            g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                                EXPRESS_VK_FLIME_ERROR_OVERFLOW,
                                "cannot choose next plan period");
        }
        g_mutex_unlock(&session->lock);
        return false;
    }
    session->replan_due = true;
    session->replan_apply_period = apply_period;
    session->replan_generation++;
    result = true;
    g_mutex_unlock(&session->lock);
    return result;
}

bool express_vk_flime_session_run_pending_planner(
    ExpressVkFlimeSession *session, bool *published, GError **error)
{
    ExpressVkFlimeSession snapshot = { 0 };
    ExpressVkFlimePlanSlot *computed;
    ExpressVkFlimePlanSlot *destination;
    uint64_t generation;
    uint64_t earliest_apply;
    uint64_t apply_period;
    bool result;

    if (published != NULL) {
        *published = false;
    }
    if (session == NULL) {
        g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                            EXPRESS_VK_FLIME_ERROR_INVALID_ARGUMENT,
                            "FLIME session is NULL");
        return false;
    }
    g_mutex_lock(&session->lock);
    if (!flime_ready_locked(session, error)) {
        g_mutex_unlock(&session->lock);
        return false;
    }
    if (!session->replan_due) {
        g_mutex_unlock(&session->lock);
        return true;
    }
    if (session->pending_valid) {
        g_mutex_unlock(&session->lock);
        return true;
    }
    if (!flime_u64_add(session->last_period_id, 1, &earliest_apply)) {
        bool ret = flime_fail_locked(session, error,
                                     EXPRESS_VK_FLIME_ERROR_OVERFLOW, false,
                                     "late planner apply period overflow");
        g_mutex_unlock(&session->lock);
        return ret;
    }
    apply_period = MAX(session->replan_apply_period, earliest_apply);
    generation = session->replan_generation;
    snapshot.capabilities = session->capabilities;
    snapshot.max_units = session->max_units;
    snapshot.max_chunks = session->max_chunks;
    snapshot.estimate_count = session->estimate_count;
    snapshot.estimate_capacity = session->estimate_count;
    snapshot.guest_handoff_ns = session->guest_handoff_ns;
    snapshot.host_handoff_ns = session->host_handoff_ns;
    snapshot.handoff_valid = session->handoff_valid;
    snapshot.pending_plan = 0;
    snapshot.estimates = flime_try_array0(
        snapshot.estimate_count, sizeof(*snapshot.estimates));
    if (snapshot.estimates == NULL) {
        g_mutex_unlock(&session->lock);
        g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                            EXPRESS_VK_FLIME_ERROR_OOM,
                            "cannot snapshot planner estimates");
        return false;
    }
    memcpy(snapshot.estimates, session->estimates,
           (size_t)snapshot.estimate_count * sizeof(*snapshot.estimates));
    g_mutex_unlock(&session->lock);

    /* Deliberately outside session->lock and the guest RPC critical path. */
    result = flime_plan_locked(&snapshot, apply_period, error);
    if (!result) {
        g_free(snapshot.estimates);
        flime_clear_plan(&snapshot.plans[0]);
        return false;
    }
    computed = &snapshot.plans[0];

    g_mutex_lock(&session->lock);
    if (!session->replan_due || session->pending_valid ||
        session->replan_generation != generation ||
        session->legacy_fallback || !session->negotiated) {
        g_mutex_unlock(&session->lock);
        g_free(snapshot.estimates);
        flime_clear_plan(computed);
        return true;
    }
    if (!flime_u64_add(session->last_period_id, 1, &earliest_apply) ||
        session->next_plan_epoch == UINT64_MAX) {
        g_mutex_unlock(&session->lock);
        g_free(snapshot.estimates);
        flime_clear_plan(computed);
        g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                            EXPRESS_VK_FLIME_ERROR_OVERFLOW,
                            "cannot publish planner epoch or apply period");
        return false;
    }
    computed->apply_period = MAX(computed->apply_period, earliest_apply);
    computed->epoch = ++session->next_plan_epoch;
    destination = &session->plans[session->pending_plan];
    flime_clear_plan(destination);
    *destination = *computed;
    computed->boundaries = NULL;
    computed->valid = false;
    session->pending_valid = true;
    session->replan_due = false;
    if (published != NULL) {
        *published = true;
    }
    g_mutex_unlock(&session->lock);
    g_free(snapshot.estimates);
    flime_clear_plan(computed);
    return result;
}

bool express_vk_flime_session_planner_due(ExpressVkFlimeSession *session)
{
    bool due;

    if (session == NULL) {
        return false;
    }
    g_mutex_lock(&session->lock);
    due = session->replan_due && !session->pending_valid &&
        session->negotiated && !session->legacy_fallback;
    g_mutex_unlock(&session->lock);
    return due;
}

static ExpressVkFlimePlanSlot *flime_published_plan_locked(
    ExpressVkFlimeSession *session)
{
    if (session->pending_valid) {
        return &session->plans[session->pending_plan];
    }
    return &session->plans[session->active_plan];
}

size_t express_vk_flime_session_control_size(ExpressVkFlimeSession *session)
{
    ExpressVkFlimePlanSlot *plan;
    size_t boundary_bytes = 0;
    size_t total = EXPRESS_VK_FLIME_CONTROL_HEADER_SIZE;

    if (session == NULL) {
        return 0;
    }
    g_mutex_lock(&session->lock);
    plan = flime_published_plan_locked(session);
    if (flime_size_mul(plan->valid ? plan->boundary_count : 0,
                       EXPRESS_VK_FLIME_CONTROL_BOUNDARY_SIZE,
                       &boundary_bytes) &&
        flime_size_add(total, boundary_bytes, &total)) {
        g_mutex_unlock(&session->lock);
        return total;
    }
    g_mutex_unlock(&session->lock);
    return 0;
}

bool express_vk_flime_session_encode_control(ExpressVkFlimeSession *session,
                                             void *control,
                                             size_t control_capacity,
                                             size_t *control_bytes,
                                             GError **error)
{
    ExpressVkFlimePlanSlot *plan;
    size_t needed;
    size_t boundary_bytes;
    uint8_t *out = control;
    uint16_t flags = 0;
    uint32_t i;

    if (control_bytes != NULL) {
        *control_bytes = 0;
    }
    if (session == NULL || control == NULL) {
        g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                            EXPRESS_VK_FLIME_ERROR_INVALID_ARGUMENT,
                            "control output is NULL");
        return false;
    }
    g_mutex_lock(&session->lock);
    plan = flime_published_plan_locked(session);
    if (!flime_size_mul(plan->valid ? plan->boundary_count : 0,
                        EXPRESS_VK_FLIME_CONTROL_BOUNDARY_SIZE,
                        &boundary_bytes) ||
        !flime_size_add(EXPRESS_VK_FLIME_CONTROL_HEADER_SIZE,
                        boundary_bytes, &needed) ||
        needed > UINT32_MAX) {
        g_mutex_unlock(&session->lock);
        g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                            EXPRESS_VK_FLIME_ERROR_OVERFLOW,
                            "control plan size overflow");
        return false;
    }
    if (control_bytes != NULL) {
        *control_bytes = needed;
    }
    if (control_capacity < needed) {
        g_mutex_unlock(&session->lock);
        g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                            EXPRESS_VK_FLIME_ERROR_INVALID_ARGUMENT,
                            "control output buffer is too small");
        return false;
    }

    memset(out, 0, needed);
    if (session->legacy_fallback) {
        flags |= EXPRESS_VK_FLIME_CONTROL_LEGACY_FALLBACK;
    }
    if (plan->valid) {
        flags |= EXPRESS_VK_FLIME_CONTROL_PLAN_VALID;
    }
    if (flime_fine_profile_due_locked(session)) {
        flags |= EXPRESS_VK_FLIME_CONTROL_REQUEST_FINE_PROFILE;
    }
    flime_put_le32(out + 0, EXPRESS_VK_FLIME_CONTROL_MAGIC);
    flime_put_le16(out + 4, EXPRESS_VK_FLIME_PROTOCOL_MAJOR);
    flime_put_le16(out + 6, EXPRESS_VK_FLIME_PROTOCOL_MINOR);
    flime_put_le16(out + 8, EXPRESS_VK_FLIME_CONTROL_HEADER_SIZE);
    flime_put_le16(out + 10, flags);
    flime_put_le32(out + 12, needed);
    flime_put_le64(out + 16, session->process_id);
    flime_put_le64(out + 24, session->stream_id);
    flime_put_le64(out + 32, plan->valid ? plan->epoch : 0);
    flime_put_le64(out + 40, plan->valid ? plan->apply_period : 0);
    flime_put_le32(out + 48, plan->valid ? plan->boundary_count : 0);
    flime_put_le64(out + 56, session->capabilities);
    for (i = 0; plan->valid && i < plan->boundary_count; i++) {
        size_t offset = EXPRESS_VK_FLIME_CONTROL_HEADER_SIZE +
            (size_t)i * EXPRESS_VK_FLIME_CONTROL_BOUNDARY_SIZE;

        flime_put_le32(out + offset + 0,
                       plan->boundaries[i].unit_past_end);
        flime_put_le32(out + offset + 4, plan->boundaries[i].flags);
        flime_put_le64(out + offset + 8,
                       plan->boundaries[i].template_offset);
    }
    g_mutex_unlock(&session->lock);
    return true;
}

bool express_vk_flime_session_copy_plan(
    ExpressVkFlimeSession *session, bool pending,
    ExpressVkFlimePlanInfo *info,
    ExpressVkFlimePlanBoundary *boundaries, size_t boundary_capacity,
    size_t *needed_boundaries, GError **error)
{
    ExpressVkFlimePlanSlot *plan;

    if (session == NULL || info == NULL || needed_boundaries == NULL) {
        g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                            EXPRESS_VK_FLIME_ERROR_INVALID_ARGUMENT,
                            "invalid plan snapshot output");
        return false;
    }
    g_mutex_lock(&session->lock);
    plan = pending ? &session->plans[session->pending_plan] :
        &session->plans[session->active_plan];
    memset(info, 0, sizeof(*info));
    info->epoch = plan->epoch;
    info->apply_period = plan->apply_period;
    info->predicted_completion_ns = plan->predicted_completion_ns;
    info->unit_count = plan->unit_count;
    info->chunk_count = plan->boundary_count;
    info->valid = plan->valid && (!pending || session->pending_valid);
    info->pending = pending && session->pending_valid;
    *needed_boundaries = info->valid ? plan->boundary_count : 0;
    if (*needed_boundaries > boundary_capacity ||
        (*needed_boundaries != 0 && boundaries == NULL)) {
        g_mutex_unlock(&session->lock);
        g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                            EXPRESS_VK_FLIME_ERROR_INVALID_ARGUMENT,
                            "plan boundary output is too small");
        return false;
    }
    if (*needed_boundaries != 0) {
        memcpy(boundaries, plan->boundaries,
               *needed_boundaries * sizeof(*boundaries));
    }
    g_mutex_unlock(&session->lock);
    return true;
}

bool express_vk_flime_session_is_candidate_dispatch(
    ExpressVkFlimeSession *session, uint32_t dispatch_ordinal)
{
    uint32_t dispatches;

    if (session == NULL || dispatch_ordinal == 0) {
        return false;
    }
    g_mutex_lock(&session->lock);
    dispatches = session->dispatches_per_unit != 0 ?
        session->dispatches_per_unit : session->config.dispatches_per_unit;
    g_mutex_unlock(&session->lock);
    return dispatches != 0 && dispatch_ordinal % dispatches == 0;
}

bool express_vk_flime_session_note_interval(
    ExpressVkFlimeSession *session, uint64_t structural_signature,
    bool *candidate_found, GError **error)
{
    uint32_t i;
    uint32_t period;
    bool found = false;

    if (candidate_found != NULL) {
        *candidate_found = false;
    }
    if (session == NULL) {
        g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                            EXPRESS_VK_FLIME_ERROR_INVALID_ARGUMENT,
                            "FLIME session is NULL");
        return false;
    }
    g_mutex_lock(&session->lock);
    if (!flime_ready_locked(session, error)) {
        g_mutex_unlock(&session->lock);
        return false;
    }
    if (session->history_count == session->config.history_limit) {
        memmove(session->history, session->history + 1,
                (session->history_count - 1) * sizeof(*session->history));
        session->history_count--;
    }
    session->history[session->history_count++] = structural_signature;
    session->kmp[0] = 0;
    for (i = 1; i < session->history_count; i++) {
        uint32_t prefix = session->kmp[i - 1];

        while (prefix != 0 &&
               session->history[i] != session->history[prefix]) {
            prefix = session->kmp[prefix - 1];
        }
        if (session->history[i] == session->history[prefix]) {
            prefix++;
        }
        session->kmp[i] = prefix;
    }
    period = session->history_count -
        session->kmp[session->history_count - 1];
    if (period != 0 && period <= session->history_count / 2 &&
        session->history_count % period == 0) {
        found = true;
    } else {
        const uint64_t multiplier = UINT64_C(11400714819323198485);
        uint32_t n = session->history_count;

        session->history_hash[0] = 0;
        session->history_power[0] = 1;
        for (i = 0; i < n; i++) {
            session->history_hash[i + 1] =
                session->history_hash[i] * multiplier +
                session->history[i] + UINT64_C(0x9e3779b97f4a7c15);
            session->history_power[i + 1] =
                session->history_power[i] * multiplier;
        }
        /* Find an adjacent repeated suffix in O(n); collisions only learn. */
        for (period = 1; period <= n / 2; period++) {
            uint64_t older = session->history_hash[n - period] -
                session->history_hash[n - 2 * period] *
                session->history_power[period];
            uint64_t newer = session->history_hash[n] -
                session->history_hash[n - period] *
                session->history_power[period];

            if (older == newer) {
                found = true;
                break;
            }
        }
    }
    if (found && session->progress.stage == EXPRESS_VK_FLIME_STAGE_DETECT) {
        /* A hash collision is only a candidate; validation starts in Learn. */
        session->progress.stage = EXPRESS_VK_FLIME_STAGE_LEARN;
        session->progress.candidate_period_intervals = period;
    }
    if (candidate_found != NULL) {
        *candidate_found = found;
    }
    g_mutex_unlock(&session->lock);
    return true;
}

static bool flime_begin_recovery_locked(
    ExpressVkFlimeSession *session, ExpressVkFlimeRecoveryStats *stats,
    GError **error)
{
    ExpressVkFlimeRecoveryStats local = { 0 };
    uint32_t prefix = 0;
    uint32_t i;

    while (prefix < session->ledger_count &&
           (session->ledger[prefix].state ==
                EXPRESS_VK_FLIME_LEDGER_RELEASED ||
            session->ledger[prefix].state ==
                EXPRESS_VK_FLIME_LEDGER_COMMITTED ||
            session->ledger[prefix].state ==
                EXPRESS_VK_FLIME_LEDGER_REPLAY_EMITTED)) {
        if (session->ledger[prefix].state ==
            EXPRESS_VK_FLIME_LEDGER_COMMITTED) {
            local.retained_committed++;
        } else {
            local.retained_released++;
        }
        prefix++;
    }
    for (i = prefix; i < session->ledger_count; i++) {
        ExpressVkFlimeLedgerState state = session->ledger[i].state;

        if (state == EXPRESS_VK_FLIME_LEDGER_RELEASED ||
            state == EXPRESS_VK_FLIME_LEDGER_COMMITTED ||
            state == EXPRESS_VK_FLIME_LEDGER_REPLAY_EMITTED) {
            return flime_fail_locked(
                session, error, EXPRESS_VK_FLIME_ERROR_STATE, false,
                "released ledger entry appears after unreleased suffix");
        }
        if (state != EXPRESS_VK_FLIME_LEDGER_PREPARED &&
            state != EXPRESS_VK_FLIME_LEDGER_READY &&
            state != EXPRESS_VK_FLIME_LEDGER_DISCARDED) {
            return flime_fail_locked(
                session, error, EXPRESS_VK_FLIME_ERROR_STATE, false,
                "invalid ledger state in recovery suffix");
        }
    }
    for (i = prefix; i < session->ledger_count; i++) {
        session->ledger[i].state = EXPRESS_VK_FLIME_LEDGER_DISCARDED;
        local.discarded_unreleased++;
    }
    session->progress.stage = EXPRESS_VK_FLIME_STAGE_RECOVER;
    if (stats != NULL) {
        *stats = local;
    }
    return true;
}

bool express_vk_flime_session_progress(
    ExpressVkFlimeSession *session, ExpressVkFlimeProgressEvent event,
    bool match_succeeded, bool authoritative_generic_shadow_ran,
    uint32_t template_entries, GError **error)
{
    bool valid = true;

    if (session == NULL) {
        g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                            EXPRESS_VK_FLIME_ERROR_INVALID_ARGUMENT,
                            "FLIME session is NULL");
        return false;
    }
    g_mutex_lock(&session->lock);
    if (!((event == EXPRESS_VK_FLIME_PROGRESS_RECOVERY_COMPLETE &&
           session->legacy_fallback && session->negotiated &&
           session->progress.stage == EXPRESS_VK_FLIME_STAGE_RECOVER) ||
          flime_ready_locked(session, error)) ||
        !(session->capabilities &
          EXPRESS_VK_FLIME_CAP_PROGRESSIVE_METADATA)) {
        if (error != NULL && *error == NULL) {
            g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                                EXPRESS_VK_FLIME_ERROR_STATE,
                                "progressive metadata was not negotiated");
        }
        g_mutex_unlock(&session->lock);
        return false;
    }

    switch (event) {
    case EXPRESS_VK_FLIME_PROGRESS_LEARN_COMPLETE:
        if (session->progress.stage != EXPRESS_VK_FLIME_STAGE_LEARN ||
            !authoritative_generic_shadow_ran || template_entries == 0) {
            valid = false;
            break;
        }
        session->progress.template_entries = template_entries;
        session->progress.stage = EXPRESS_VK_FLIME_STAGE_MATCH;
        break;
    case EXPRESS_VK_FLIME_PROGRESS_MATCH_COMPLETE:
        if (session->progress.stage != EXPRESS_VK_FLIME_STAGE_MATCH) {
            valid = false;
            break;
        }
        if (match_succeeded && authoritative_generic_shadow_ran &&
            template_entries == session->progress.template_entries) {
            session->progress.stage = EXPRESS_VK_FLIME_STAGE_FAST;
        } else {
            session->progress.mismatches++;
            if (!flime_begin_recovery_locked(session, NULL, error)) {
                g_mutex_unlock(&session->lock);
                return false;
            }
        }
        break;
    case EXPRESS_VK_FLIME_PROGRESS_FAST_PERIOD_COMPLETE:
        if (session->progress.stage != EXPRESS_VK_FLIME_STAGE_FAST) {
            valid = false;
            break;
        }
        if (match_succeeded && template_entries != 0 &&
            template_entries == session->progress.template_entries &&
            !session->period_open &&
            session->last_period_id > session->checkpoint_period_id) {
            for (uint32_t i = 0; i < session->ledger_count; i++) {
                if (session->ledger[i].state !=
                    EXPRESS_VK_FLIME_LEDGER_COMMITTED) {
                    match_succeeded = false;
                    break;
                }
            }
        } else {
            match_succeeded = false;
        }
        if (!match_succeeded) {
            session->progress.mismatches++;
            if (!flime_begin_recovery_locked(session, NULL, error)) {
                g_mutex_unlock(&session->lock);
                return false;
            }
        } else {
            session->ledger_count = 0;
            g_hash_table_remove_all(session->ledger_index);
            session->checkpoint_period_id = session->last_period_id;
            session->progress.completed_fast_periods++;
        }
        break;
    case EXPRESS_VK_FLIME_PROGRESS_MISMATCH:
        if (session->progress.stage != EXPRESS_VK_FLIME_STAGE_LEARN &&
            session->progress.stage != EXPRESS_VK_FLIME_STAGE_MATCH &&
            session->progress.stage != EXPRESS_VK_FLIME_STAGE_FAST) {
            valid = false;
            break;
        }
        session->progress.mismatches++;
        if (!flime_begin_recovery_locked(session, NULL, error)) {
            g_mutex_unlock(&session->lock);
            return false;
        }
        break;
    case EXPRESS_VK_FLIME_PROGRESS_RECOVERY_COMPLETE:
        if (session->progress.stage != EXPRESS_VK_FLIME_STAGE_RECOVER ||
            !authoritative_generic_shadow_ran) {
            valid = false;
            break;
        }
        for (uint32_t i = 0; i < session->ledger_count; i++) {
            if (session->ledger[i].state !=
                EXPRESS_VK_FLIME_LEDGER_COMMITTED) {
                valid = false;
                break;
            }
        }
        if (valid) {
            uint64_t completed = session->progress.completed_fast_periods;
            uint64_t mismatches = session->progress.mismatches;
            bool finish_in_legacy = session->legacy_fallback;

            flime_clear_specialization_locked(session, true);
            session->progress.stage = finish_in_legacy ?
                EXPRESS_VK_FLIME_STAGE_LEGACY :
                EXPRESS_VK_FLIME_STAGE_DETECT;
            session->progress.completed_fast_periods = completed;
            session->progress.mismatches = mismatches;
            if (finish_in_legacy) {
                session->negotiated = false;
                session->capabilities = 0;
            }
        }
        break;
    default:
        valid = false;
        break;
    }
    if (!valid) {
        bool ret = flime_fail_locked(session, error,
                                     EXPRESS_VK_FLIME_ERROR_STATE, false,
                                     "invalid progressive state transition");
        g_mutex_unlock(&session->lock);
        return ret;
    }
    g_mutex_unlock(&session->lock);
    return true;
}

void express_vk_flime_session_get_progress(ExpressVkFlimeSession *session,
                                           ExpressVkFlimeProgressInfo *info)
{
    if (info == NULL) {
        return;
    }
    memset(info, 0, sizeof(*info));
    if (session == NULL) {
        info->stage = EXPRESS_VK_FLIME_STAGE_LEGACY;
        return;
    }
    g_mutex_lock(&session->lock);
    *info = session->progress;
    g_mutex_unlock(&session->lock);
}

bool express_vk_flime_session_reset(ExpressVkFlimeSession *session,
                                    GError **error)
{
    if (session == NULL) {
        g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                            EXPRESS_VK_FLIME_ERROR_INVALID_ARGUMENT,
                            "FLIME session is NULL");
        return false;
    }
    g_mutex_lock(&session->lock);
    if (!flime_ready_locked(session, error)) {
        g_mutex_unlock(&session->lock);
        return false;
    }
    flime_reset_protocol_locked(session, true);
    session->progress.stage = EXPRESS_VK_FLIME_STAGE_DETECT;
    session->teardown_requested = false;
    g_mutex_unlock(&session->lock);
    return true;
}

bool express_vk_flime_session_take_teardown_request(
    ExpressVkFlimeSession *session)
{
    bool requested;

    if (session == NULL) {
        return false;
    }
    g_mutex_lock(&session->lock);
    requested = session->teardown_requested;
    session->teardown_requested = false;
    g_mutex_unlock(&session->lock);
    return requested;
}

static int flime_ledger_find_locked(ExpressVkFlimeSession *session,
                                    uint64_t update_id)
{
    gpointer value = g_hash_table_lookup(session->ledger_index, &update_id);

    return value == NULL ? -1 : (int)(GPOINTER_TO_UINT(value) - 1);
}

static bool flime_ledger_emitted_state(ExpressVkFlimeLedgerState state)
{
    return state == EXPRESS_VK_FLIME_LEDGER_RELEASED ||
           state == EXPRESS_VK_FLIME_LEDGER_COMMITTED ||
           state == EXPRESS_VK_FLIME_LEDGER_REPLAY_EMITTED;
}

static bool flime_ledger_prefix_emitted_locked(
    ExpressVkFlimeSession *session, uint32_t index)
{
    uint32_t i;

    for (i = 0; i < index; i++) {
        if (!flime_ledger_emitted_state(session->ledger[i].state)) {
            return false;
        }
    }
    return true;
}

bool express_vk_flime_session_ledger_prepare(
    ExpressVkFlimeSession *session, uint64_t update_id,
    uint64_t submission_id, uint64_t template_offset, bool *inserted,
    GError **error)
{
    int found;
    uint64_t *index_key;

    if (inserted != NULL) {
        *inserted = false;
    }
    if (session == NULL || update_id == 0 || submission_id == 0) {
        g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                            EXPRESS_VK_FLIME_ERROR_INVALID_ARGUMENT,
                            "invalid recovery ledger identity");
        return false;
    }
    g_mutex_lock(&session->lock);
    if (!flime_ready_locked(session, error) ||
        !(session->capabilities & EXPRESS_VK_FLIME_CAP_RECOVERY_LEDGER)) {
        if (error != NULL && *error == NULL) {
            g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                                EXPRESS_VK_FLIME_ERROR_STATE,
                                "recovery ledger was not negotiated");
        }
        g_mutex_unlock(&session->lock);
        return false;
    }
    found = flime_ledger_find_locked(session, update_id);
    if (found >= 0) {
        ExpressVkFlimeLedgerEntry *entry = &session->ledger[found];

        if (entry->submission_id != submission_id ||
            entry->template_offset != template_offset) {
            bool ret = flime_fail_locked(
                session, error, EXPRESS_VK_FLIME_ERROR_STATE, false,
                "duplicate update id has conflicting ledger metadata");
            g_mutex_unlock(&session->lock);
            return ret;
        }
        g_mutex_unlock(&session->lock);
        return true;
    }
    if (session->ledger_count >= session->config.max_ledger_entries) {
        bool ret = flime_fail_locked(session, error,
                                     EXPRESS_VK_FLIME_ERROR_LIMIT, false,
                                     "recovery ledger limit reached");
        g_mutex_unlock(&session->lock);
        return ret;
    }
    if (!flime_reserve_array((void **)&session->ledger,
                             &session->ledger_capacity,
                             session->ledger_count + 1,
                             session->config.max_ledger_entries,
                             sizeof(*session->ledger))) {
        bool ret = flime_fail_locked(session, error,
                                     EXPRESS_VK_FLIME_ERROR_OOM, false,
                                     "cannot grow recovery ledger");
        g_mutex_unlock(&session->lock);
        return ret;
    }
    index_key = g_try_new(uint64_t, 1);
    if (index_key == NULL) {
        bool ret = flime_fail_locked(session, error,
                                     EXPRESS_VK_FLIME_ERROR_OOM, false,
                                     "cannot allocate ledger index key");
        g_mutex_unlock(&session->lock);
        return ret;
    }
    *index_key = update_id;
    session->ledger[session->ledger_count++] =
        (ExpressVkFlimeLedgerEntry) {
            .update_id = update_id,
            .submission_id = submission_id,
            .template_offset = template_offset,
            .state = EXPRESS_VK_FLIME_LEDGER_PREPARED,
        };
    g_hash_table_insert(session->ledger_index, index_key,
                        GUINT_TO_POINTER(session->ledger_count));
    if (inserted != NULL) {
        *inserted = true;
    }
    g_mutex_unlock(&session->lock);
    return true;
}

bool express_vk_flime_session_ledger_mark_ready(
    ExpressVkFlimeSession *session, uint64_t update_id, GError **error)
{
    int found;

    if (session == NULL || update_id == 0) {
        g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                            EXPRESS_VK_FLIME_ERROR_INVALID_ARGUMENT,
                            "invalid recovery ledger identity");
        return false;
    }
    g_mutex_lock(&session->lock);
    found = flime_ledger_find_locked(session, update_id);
    if (found < 0) {
        bool ret = flime_fail_locked(session, error,
                                     EXPRESS_VK_FLIME_ERROR_STATE, false,
                                     "ledger update was not prepared");
        g_mutex_unlock(&session->lock);
        return ret;
    }
    switch (session->ledger[found].state) {
    case EXPRESS_VK_FLIME_LEDGER_PREPARED:
        session->ledger[found].state = EXPRESS_VK_FLIME_LEDGER_READY;
        break;
    case EXPRESS_VK_FLIME_LEDGER_READY:
    case EXPRESS_VK_FLIME_LEDGER_RELEASED:
    case EXPRESS_VK_FLIME_LEDGER_COMMITTED:
        break;
    default:
        g_mutex_unlock(&session->lock);
        g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                            EXPRESS_VK_FLIME_ERROR_STATE,
                            "ledger update cannot become ready");
        return false;
    }
    g_mutex_unlock(&session->lock);
    return true;
}

bool express_vk_flime_session_ledger_begin_release(
    ExpressVkFlimeSession *session, uint64_t update_id, bool *should_emit,
    GError **error)
{
    int found;

    if (should_emit != NULL) {
        *should_emit = false;
    }
    if (session == NULL || update_id == 0 || should_emit == NULL) {
        g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                            EXPRESS_VK_FLIME_ERROR_INVALID_ARGUMENT,
                            "invalid ledger begin-release request");
        return false;
    }
    g_mutex_lock(&session->lock);
    found = flime_ledger_find_locked(session, update_id);
    if (found < 0 ||
        !flime_ledger_prefix_emitted_locked(session, found)) {
        bool ret = flime_fail_locked(session, error,
                                     EXPRESS_VK_FLIME_ERROR_STATE, false,
                                     "ledger release is not a contiguous prefix");
        g_mutex_unlock(&session->lock);
        return ret;
    }
    switch (session->ledger[found].state) {
    case EXPRESS_VK_FLIME_LEDGER_READY:
        session->ledger[found].state =
            EXPRESS_VK_FLIME_LEDGER_RELEASE_IN_FLIGHT;
        *should_emit = true;
        break;
    case EXPRESS_VK_FLIME_LEDGER_RELEASE_IN_FLIGHT:
    case EXPRESS_VK_FLIME_LEDGER_RELEASED:
    case EXPRESS_VK_FLIME_LEDGER_COMMITTED:
    case EXPRESS_VK_FLIME_LEDGER_REPLAY_EMITTED:
        break;
    default:
        g_mutex_unlock(&session->lock);
        g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                            EXPRESS_VK_FLIME_ERROR_STATE,
                            "ledger update is not ready for release");
        return false;
    }
    g_mutex_unlock(&session->lock);
    return true;
}

bool express_vk_flime_session_ledger_complete_release(
    ExpressVkFlimeSession *session, uint64_t update_id, bool emitted,
    GError **error)
{
    int found;

    if (session == NULL || update_id == 0) {
        g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                            EXPRESS_VK_FLIME_ERROR_INVALID_ARGUMENT,
                            "invalid ledger complete-release request");
        return false;
    }
    g_mutex_lock(&session->lock);
    found = flime_ledger_find_locked(session, update_id);
    if (found < 0 || session->ledger[found].state !=
        EXPRESS_VK_FLIME_LEDGER_RELEASE_IN_FLIGHT) {
        bool already_complete = found >= 0 &&
            (session->ledger[found].state == EXPRESS_VK_FLIME_LEDGER_RELEASED ||
             session->ledger[found].state == EXPRESS_VK_FLIME_LEDGER_COMMITTED);

        if (already_complete && emitted) {
            g_mutex_unlock(&session->lock);
            return true;
        }
        g_mutex_unlock(&session->lock);
        g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                            EXPRESS_VK_FLIME_ERROR_STATE,
                            "ledger release is not in flight");
        return false;
    }
    session->ledger[found].state = emitted ?
        EXPRESS_VK_FLIME_LEDGER_RELEASED : EXPRESS_VK_FLIME_LEDGER_READY;
    g_mutex_unlock(&session->lock);
    return true;
}

bool express_vk_flime_session_ledger_begin_release_batch(
    ExpressVkFlimeSession *session, const uint64_t *update_ids,
    size_t update_count, bool *emit_mask, GError **error)
{
    uint32_t frontier = 0;
    int previous = -1;
    size_t i;

    if (session == NULL || update_ids == NULL || emit_mask == NULL ||
        update_count == 0 ||
        update_count > EXPRESS_VK_FLIME_HARD_MAX_LEDGER_ENTRIES) {
        g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                            EXPRESS_VK_FLIME_ERROR_INVALID_ARGUMENT,
                            "invalid ledger release batch");
        return false;
    }
    memset(emit_mask, 0, update_count * sizeof(*emit_mask));
    g_mutex_lock(&session->lock);
    while (frontier < session->ledger_count &&
           flime_ledger_emitted_state(session->ledger[frontier].state)) {
        frontier++;
    }
    for (i = 0; i < update_count; i++) {
        int found = flime_ledger_find_locked(session, update_ids[i]);

        if (found < 0 || found <= previous) {
            goto invalid_release_batch;
        }
        previous = found;
        if (flime_ledger_emitted_state(session->ledger[found].state)) {
            if ((uint32_t)found >= frontier) {
                goto invalid_release_batch;
            }
            continue;
        }
        if (session->ledger[found].state != EXPRESS_VK_FLIME_LEDGER_READY ||
            (uint32_t)found != frontier) {
            goto invalid_release_batch;
        }
        emit_mask[i] = true;
        frontier++;
        while (frontier < session->ledger_count &&
               flime_ledger_emitted_state(
                   session->ledger[frontier].state)) {
            frontier++;
        }
    }
    for (i = 0; i < update_count; i++) {
        if (emit_mask[i]) {
            int found = flime_ledger_find_locked(session, update_ids[i]);
            session->ledger[found].state =
                EXPRESS_VK_FLIME_LEDGER_RELEASE_IN_FLIGHT;
        }
    }
    g_mutex_unlock(&session->lock);
    return true;

invalid_release_batch:
    g_mutex_unlock(&session->lock);
    g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                        EXPRESS_VK_FLIME_ERROR_STATE,
                        "release batch is not the ordered ledger prefix");
    return false;
}

bool express_vk_flime_session_ledger_complete_release_batch(
    ExpressVkFlimeSession *session, const uint64_t *update_ids,
    const bool *emit_mask, size_t update_count, bool emitted,
    GError **error)
{
    size_t i;

    if (session == NULL || update_ids == NULL || emit_mask == NULL ||
        update_count == 0 ||
        update_count > EXPRESS_VK_FLIME_HARD_MAX_LEDGER_ENTRIES) {
        g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                            EXPRESS_VK_FLIME_ERROR_INVALID_ARGUMENT,
                            "invalid ledger release completion batch");
        return false;
    }
    g_mutex_lock(&session->lock);
    for (i = 0; i < update_count; i++) {
        int found = flime_ledger_find_locked(session, update_ids[i]);

        if (found < 0 ||
            (emit_mask[i] && session->ledger[found].state !=
             EXPRESS_VK_FLIME_LEDGER_RELEASE_IN_FLIGHT) ||
            (!emit_mask[i] &&
             !flime_ledger_emitted_state(session->ledger[found].state))) {
            g_mutex_unlock(&session->lock);
            g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                                EXPRESS_VK_FLIME_ERROR_STATE,
                                "release batch completion state mismatch");
            return false;
        }
    }
    for (i = 0; i < update_count; i++) {
        if (emit_mask[i]) {
            int found = flime_ledger_find_locked(session, update_ids[i]);
            session->ledger[found].state = emitted ?
                EXPRESS_VK_FLIME_LEDGER_RELEASED :
                EXPRESS_VK_FLIME_LEDGER_READY;
        }
    }
    g_mutex_unlock(&session->lock);
    return true;
}

bool express_vk_flime_session_ledger_release(
    ExpressVkFlimeSession *session, uint64_t update_id, bool *should_emit,
    GError **error)
{
    int found;

    if (should_emit != NULL) {
        *should_emit = false;
    }
    if (session == NULL || update_id == 0 || should_emit == NULL) {
        g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                            EXPRESS_VK_FLIME_ERROR_INVALID_ARGUMENT,
                            "invalid ledger release request");
        return false;
    }
    g_mutex_lock(&session->lock);
    found = flime_ledger_find_locked(session, update_id);
    if (found < 0 ||
        !flime_ledger_prefix_emitted_locked(session, found)) {
        bool ret = flime_fail_locked(session, error,
                                     EXPRESS_VK_FLIME_ERROR_STATE, false,
                                     "ledger release is not a contiguous prefix");
        g_mutex_unlock(&session->lock);
        return ret;
    }
    switch (session->ledger[found].state) {
    case EXPRESS_VK_FLIME_LEDGER_READY:
        session->ledger[found].state = EXPRESS_VK_FLIME_LEDGER_RELEASED;
        *should_emit = true;
        break;
    case EXPRESS_VK_FLIME_LEDGER_RELEASED:
    case EXPRESS_VK_FLIME_LEDGER_COMMITTED:
    case EXPRESS_VK_FLIME_LEDGER_REPLAY_EMITTED:
        break;
    default:
        g_mutex_unlock(&session->lock);
        g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                            EXPRESS_VK_FLIME_ERROR_STATE,
                            "ledger update is not ready for release");
        return false;
    }
    g_mutex_unlock(&session->lock);
    return true;
}

bool express_vk_flime_session_ledger_commit(
    ExpressVkFlimeSession *session, uint64_t update_id, bool *should_emit,
    GError **error)
{
    int found;

    if (should_emit != NULL) {
        *should_emit = false;
    }
    if (session == NULL || update_id == 0 || should_emit == NULL) {
        g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                            EXPRESS_VK_FLIME_ERROR_INVALID_ARGUMENT,
                            "invalid ledger commit request");
        return false;
    }
    g_mutex_lock(&session->lock);
    found = flime_ledger_find_locked(session, update_id);
    if (found < 0 ||
        !flime_ledger_prefix_emitted_locked(session, found)) {
        bool ret = flime_fail_locked(session, error,
                                     EXPRESS_VK_FLIME_ERROR_STATE, false,
                                     "ledger commit is not a contiguous prefix");
        g_mutex_unlock(&session->lock);
        return ret;
    }
    switch (session->ledger[found].state) {
    case EXPRESS_VK_FLIME_LEDGER_READY:
        session->ledger[found].state = EXPRESS_VK_FLIME_LEDGER_COMMITTED;
        *should_emit = true;
        break;
    case EXPRESS_VK_FLIME_LEDGER_RELEASED:
    case EXPRESS_VK_FLIME_LEDGER_REPLAY_EMITTED:
        session->ledger[found].state = EXPRESS_VK_FLIME_LEDGER_COMMITTED;
        break;
    case EXPRESS_VK_FLIME_LEDGER_COMMITTED:
        break;
    default:
        g_mutex_unlock(&session->lock);
        g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                            EXPRESS_VK_FLIME_ERROR_STATE,
                            "ledger update is not ready for commit");
        return false;
    }
    g_mutex_unlock(&session->lock);
    return true;
}

bool express_vk_flime_session_ledger_begin_recovery(
    ExpressVkFlimeSession *session, ExpressVkFlimeRecoveryStats *stats,
    GError **error)
{
    if (stats != NULL) {
        memset(stats, 0, sizeof(*stats));
    }
    if (session == NULL) {
        g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                            EXPRESS_VK_FLIME_ERROR_INVALID_ARGUMENT,
                            "FLIME session is NULL");
        return false;
    }
    g_mutex_lock(&session->lock);
    if (!flime_begin_recovery_locked(session, stats, error)) {
        g_mutex_unlock(&session->lock);
        return false;
    }
    g_mutex_unlock(&session->lock);
    return true;
}

bool express_vk_flime_session_ledger_replay(
    ExpressVkFlimeSession *session, uint64_t update_id,
    uint64_t submission_id, bool *should_emit, GError **error)
{
    int found;
    uint64_t *index_key;

    if (should_emit != NULL) {
        *should_emit = false;
    }
    if (session == NULL || update_id == 0 || submission_id == 0 ||
        should_emit == NULL) {
        g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                            EXPRESS_VK_FLIME_ERROR_INVALID_ARGUMENT,
                            "invalid ledger replay request");
        return false;
    }
    g_mutex_lock(&session->lock);
    found = flime_ledger_find_locked(session, update_id);
    if (found >= 0) {
        ExpressVkFlimeLedgerEntry *entry = &session->ledger[found];

        if (entry->submission_id != submission_id) {
            bool ret = flime_fail_locked(
                session, error, EXPRESS_VK_FLIME_ERROR_STATE, false,
                "replayed update has a conflicting submission id");
            g_mutex_unlock(&session->lock);
            return ret;
        }
        if (flime_ledger_emitted_state(entry->state)) {
            g_mutex_unlock(&session->lock);
            return true;
        }
        if (entry->state != EXPRESS_VK_FLIME_LEDGER_DISCARDED ||
            !flime_ledger_prefix_emitted_locked(session, found)) {
            bool ret = flime_fail_locked(
                session, error, EXPRESS_VK_FLIME_ERROR_STATE, false,
                "replay is not in generic ledger order");
            g_mutex_unlock(&session->lock);
            return ret;
        }
        entry->state = EXPRESS_VK_FLIME_LEDGER_REPLAY_EMITTED;
        *should_emit = true;
        g_mutex_unlock(&session->lock);
        return true;
    }

    if (session->ledger_count >= session->config.max_ledger_entries ||
        !flime_reserve_array((void **)&session->ledger,
                             &session->ledger_capacity,
                             session->ledger_count + 1,
                             session->config.max_ledger_entries,
                             sizeof(*session->ledger))) {
        bool ret = flime_fail_locked(
            session, error,
            session->ledger_count >= session->config.max_ledger_entries ?
            EXPRESS_VK_FLIME_ERROR_LIMIT : EXPRESS_VK_FLIME_ERROR_OOM,
            false, "cannot append replay ledger entry");
        g_mutex_unlock(&session->lock);
        return ret;
    }
    session->ledger[session->ledger_count++] =
        (ExpressVkFlimeLedgerEntry) {
            .update_id = update_id,
            .submission_id = submission_id,
            .template_offset = 0,
            .state = EXPRESS_VK_FLIME_LEDGER_REPLAY_EMITTED,
        };
    index_key = g_try_new(uint64_t, 1);
    if (index_key == NULL) {
        session->ledger_count--;
        g_mutex_unlock(&session->lock);
        g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                            EXPRESS_VK_FLIME_ERROR_OOM,
                            "cannot allocate replay index key");
        return false;
    }
    *index_key = update_id;
    g_hash_table_insert(session->ledger_index, index_key,
                        GUINT_TO_POINTER(session->ledger_count));
    *should_emit = true;
    g_mutex_unlock(&session->lock);
    return true;
}

bool express_vk_flime_session_ledger_begin_replay(
    ExpressVkFlimeSession *session, uint64_t update_id,
    uint64_t submission_id, bool *should_emit, GError **error)
{
    int found;
    uint64_t *index_key;

    if (should_emit != NULL) {
        *should_emit = false;
    }
    if (session == NULL || update_id == 0 || submission_id == 0 ||
        should_emit == NULL) {
        g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                            EXPRESS_VK_FLIME_ERROR_INVALID_ARGUMENT,
                            "invalid ledger begin-replay request");
        return false;
    }
    g_mutex_lock(&session->lock);
    found = flime_ledger_find_locked(session, update_id);
    if (found >= 0) {
        ExpressVkFlimeLedgerEntry *entry = &session->ledger[found];

        if (entry->submission_id != submission_id) {
            bool ret = flime_fail_locked(
                session, error, EXPRESS_VK_FLIME_ERROR_STATE, false,
                "replayed update has a conflicting submission id");
            g_mutex_unlock(&session->lock);
            return ret;
        }
        if (flime_ledger_emitted_state(entry->state) ||
            entry->state == EXPRESS_VK_FLIME_LEDGER_REPLAY_IN_FLIGHT) {
            g_mutex_unlock(&session->lock);
            return true;
        }
        if (entry->state != EXPRESS_VK_FLIME_LEDGER_DISCARDED ||
            !flime_ledger_prefix_emitted_locked(session, found)) {
            bool ret = flime_fail_locked(
                session, error, EXPRESS_VK_FLIME_ERROR_STATE, false,
                "replay is not in generic ledger order");
            g_mutex_unlock(&session->lock);
            return ret;
        }
        entry->state = EXPRESS_VK_FLIME_LEDGER_REPLAY_IN_FLIGHT;
        *should_emit = true;
        g_mutex_unlock(&session->lock);
        return true;
    }
    if (session->ledger_count >= session->config.max_ledger_entries ||
        !flime_reserve_array((void **)&session->ledger,
                             &session->ledger_capacity,
                             session->ledger_count + 1,
                             session->config.max_ledger_entries,
                             sizeof(*session->ledger))) {
        bool ret = flime_fail_locked(
            session, error,
            session->ledger_count >= session->config.max_ledger_entries ?
            EXPRESS_VK_FLIME_ERROR_LIMIT : EXPRESS_VK_FLIME_ERROR_OOM,
            false, "cannot append replay ledger entry");
        g_mutex_unlock(&session->lock);
        return ret;
    }
    index_key = g_try_new(uint64_t, 1);
    if (index_key == NULL) {
        g_mutex_unlock(&session->lock);
        g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                            EXPRESS_VK_FLIME_ERROR_OOM,
                            "cannot allocate replay index key");
        return false;
    }
    *index_key = update_id;
    session->ledger[session->ledger_count++] =
        (ExpressVkFlimeLedgerEntry) {
            .update_id = update_id,
            .submission_id = submission_id,
            .template_offset = 0,
            .state = EXPRESS_VK_FLIME_LEDGER_REPLAY_IN_FLIGHT,
        };
    g_hash_table_insert(session->ledger_index, index_key,
                        GUINT_TO_POINTER(session->ledger_count));
    *should_emit = true;
    g_mutex_unlock(&session->lock);
    return true;
}

bool express_vk_flime_session_ledger_complete_replay(
    ExpressVkFlimeSession *session, uint64_t update_id, bool emitted,
    GError **error)
{
    int found;

    if (session == NULL || update_id == 0) {
        g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                            EXPRESS_VK_FLIME_ERROR_INVALID_ARGUMENT,
                            "invalid ledger complete-replay request");
        return false;
    }
    g_mutex_lock(&session->lock);
    found = flime_ledger_find_locked(session, update_id);
    if (found < 0 || session->ledger[found].state !=
        EXPRESS_VK_FLIME_LEDGER_REPLAY_IN_FLIGHT) {
        bool already_complete = found >= 0 &&
            (session->ledger[found].state ==
                 EXPRESS_VK_FLIME_LEDGER_REPLAY_EMITTED ||
             session->ledger[found].state == EXPRESS_VK_FLIME_LEDGER_COMMITTED);

        if (already_complete && emitted) {
            g_mutex_unlock(&session->lock);
            return true;
        }
        g_mutex_unlock(&session->lock);
        g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                            EXPRESS_VK_FLIME_ERROR_STATE,
                            "ledger replay is not in flight");
        return false;
    }
    session->ledger[found].state = emitted ?
        EXPRESS_VK_FLIME_LEDGER_REPLAY_EMITTED :
        EXPRESS_VK_FLIME_LEDGER_DISCARDED;
    g_mutex_unlock(&session->lock);
    return true;
}

bool express_vk_flime_session_ledger_begin_replay_batch(
    ExpressVkFlimeSession *session, const uint64_t *update_ids,
    const uint64_t *submission_ids, size_t update_count,
    bool *emit_mask, GError **error)
{
    uint32_t frontier = 0;
    uint32_t missing_count = 0;
    uint32_t missing_seen = 0;
    int previous = -1;
    int *indices = NULL;
    uint64_t **new_keys = NULL;
    GHashTable *seen;
    size_t i;

    if (session == NULL || update_ids == NULL || submission_ids == NULL ||
        emit_mask == NULL || update_count == 0 ||
        update_count > EXPRESS_VK_FLIME_HARD_MAX_LEDGER_ENTRIES) {
        g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                            EXPRESS_VK_FLIME_ERROR_INVALID_ARGUMENT,
                            "invalid ledger replay batch");
        return false;
    }
    memset(emit_mask, 0, update_count * sizeof(*emit_mask));
    seen = g_hash_table_new(g_int64_hash, g_int64_equal);
    for (i = 0; i < update_count; i++) {
        if (update_ids[i] == 0 || submission_ids[i] == 0 ||
            g_hash_table_contains(seen, &update_ids[i])) {
            g_hash_table_destroy(seen);
            g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                                EXPRESS_VK_FLIME_ERROR_INVALID_ARGUMENT,
                                "duplicate or zero identity in replay batch");
            return false;
        }
        g_hash_table_insert(seen, (gpointer)&update_ids[i],
                            GINT_TO_POINTER(1));
    }
    g_hash_table_destroy(seen);
    indices = flime_try_array0(update_count, sizeof(*indices));
    if (indices == NULL) {
        g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                            EXPRESS_VK_FLIME_ERROR_OOM,
                            "cannot allocate replay validation indices");
        return false;
    }
    g_mutex_lock(&session->lock);
    for (i = 0; i < update_count; i++) {
        indices[i] = flime_ledger_find_locked(session, update_ids[i]);
        if (indices[i] < 0) {
            missing_count++;
        } else if (session->ledger[indices[i]].submission_id !=
                   submission_ids[i]) {
            goto invalid_replay_batch;
        }
    }
    if (missing_count > session->config.max_ledger_entries -
                        session->ledger_count ||
        !flime_reserve_array((void **)&session->ledger,
                             &session->ledger_capacity,
                             session->ledger_count + missing_count,
                             session->config.max_ledger_entries,
                             sizeof(*session->ledger))) {
        g_mutex_unlock(&session->lock);
        g_free(indices);
        g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                            EXPRESS_VK_FLIME_ERROR_LIMIT,
                            "replay batch exceeds ledger capacity");
        return false;
    }
    while (frontier < session->ledger_count &&
           flime_ledger_emitted_state(session->ledger[frontier].state)) {
        frontier++;
    }
    for (i = 0; i < update_count; i++) {
        int found = indices[i];

        if (found < 0) {
            if (frontier != session->ledger_count + missing_seen) {
                goto invalid_replay_batch;
            }
            emit_mask[i] = true;
            previous = session->ledger_count + missing_seen;
            missing_seen++;
            frontier++;
            continue;
        }
        if (missing_seen != 0 || found <= previous) {
            goto invalid_replay_batch;
        }
        previous = found;
        if (flime_ledger_emitted_state(session->ledger[found].state)) {
            if ((uint32_t)found >= frontier) {
                goto invalid_replay_batch;
            }
            continue;
        }
        if (session->ledger[found].state !=
                EXPRESS_VK_FLIME_LEDGER_DISCARDED ||
            (uint32_t)found != frontier) {
            goto invalid_replay_batch;
        }
        emit_mask[i] = true;
        frontier++;
        while (frontier < session->ledger_count &&
               flime_ledger_emitted_state(
                   session->ledger[frontier].state)) {
            frontier++;
        }
    }
    if (missing_count != 0) {
        new_keys = flime_try_array0(missing_count, sizeof(*new_keys));
        if (new_keys == NULL) {
            goto replay_batch_oom;
        }
        for (i = 0; i < missing_count; i++) {
            new_keys[i] = g_try_new(uint64_t, 1);
            if (new_keys[i] == NULL) {
                goto replay_batch_oom;
            }
        }
    }
    missing_seen = 0;
    for (i = 0; i < update_count; i++) {
        if (!emit_mask[i]) {
            continue;
        }
        if (indices[i] >= 0) {
            session->ledger[indices[i]].state =
                EXPRESS_VK_FLIME_LEDGER_REPLAY_IN_FLIGHT;
        } else {
            uint32_t index = session->ledger_count++;

            *new_keys[missing_seen] = update_ids[i];
            session->ledger[index] = (ExpressVkFlimeLedgerEntry) {
                .update_id = update_ids[i],
                .submission_id = submission_ids[i],
                .template_offset = 0,
                .state = EXPRESS_VK_FLIME_LEDGER_REPLAY_IN_FLIGHT,
            };
            g_hash_table_insert(session->ledger_index,
                                new_keys[missing_seen],
                                GUINT_TO_POINTER(index + 1));
            new_keys[missing_seen] = NULL;
            missing_seen++;
        }
    }
    g_mutex_unlock(&session->lock);
    g_free(new_keys);
    g_free(indices);
    return true;

replay_batch_oom:
    if (new_keys != NULL) {
        for (i = 0; i < missing_count; i++) {
            g_free(new_keys[i]);
        }
    }
    g_free(new_keys);
    g_mutex_unlock(&session->lock);
    g_free(indices);
    g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                        EXPRESS_VK_FLIME_ERROR_OOM,
                        "cannot reserve replay batch identities");
    return false;

invalid_replay_batch:
    g_mutex_unlock(&session->lock);
    g_free(indices);
    g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                        EXPRESS_VK_FLIME_ERROR_STATE,
                        "replay batch is not the ordered recovery suffix");
    return false;
}

bool express_vk_flime_session_ledger_complete_replay_batch(
    ExpressVkFlimeSession *session, const uint64_t *update_ids,
    const bool *emit_mask, size_t update_count, bool emitted,
    GError **error)
{
    size_t i;

    if (session == NULL || update_ids == NULL || emit_mask == NULL ||
        update_count == 0 ||
        update_count > EXPRESS_VK_FLIME_HARD_MAX_LEDGER_ENTRIES) {
        g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                            EXPRESS_VK_FLIME_ERROR_INVALID_ARGUMENT,
                            "invalid ledger replay completion batch");
        return false;
    }
    g_mutex_lock(&session->lock);
    for (i = 0; i < update_count; i++) {
        int found = flime_ledger_find_locked(session, update_ids[i]);

        if (found < 0 ||
            (emit_mask[i] && session->ledger[found].state !=
             EXPRESS_VK_FLIME_LEDGER_REPLAY_IN_FLIGHT) ||
            (!emit_mask[i] &&
             !flime_ledger_emitted_state(session->ledger[found].state))) {
            g_mutex_unlock(&session->lock);
            g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                                EXPRESS_VK_FLIME_ERROR_STATE,
                                "replay batch completion state mismatch");
            return false;
        }
    }
    for (i = 0; i < update_count; i++) {
        if (emit_mask[i]) {
            int found = flime_ledger_find_locked(session, update_ids[i]);
            session->ledger[found].state = emitted ?
                EXPRESS_VK_FLIME_LEDGER_REPLAY_EMITTED :
                EXPRESS_VK_FLIME_LEDGER_DISCARDED;
        }
    }
    g_mutex_unlock(&session->lock);
    return true;
}

bool express_vk_flime_session_ledger_submission_ready_to_commit(
    ExpressVkFlimeSession *session, uint64_t submission_id,
    uint32_t *entry_count, GError **error)
{
    bool ready = true;
    uint32_t count = 0;
    uint32_t i;

    if (entry_count != NULL) {
        *entry_count = 0;
    }
    if (session == NULL || submission_id == 0 || entry_count == NULL) {
        g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                            EXPRESS_VK_FLIME_ERROR_INVALID_ARGUMENT,
                            "invalid recovery submission readiness request");
        return false;
    }
    g_mutex_lock(&session->lock);
    if (session->progress.stage != EXPRESS_VK_FLIME_STAGE_RECOVER ||
        !(session->capabilities & EXPRESS_VK_FLIME_CAP_RECOVERY_LEDGER)) {
        g_mutex_unlock(&session->lock);
        g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                            EXPRESS_VK_FLIME_ERROR_STATE,
                            "submission readiness is only valid in recovery");
        return false;
    }
    for (i = 0; i < session->ledger_count; i++) {
        const ExpressVkFlimeLedgerEntry *entry = &session->ledger[i];

        if (entry->submission_id != submission_id) {
            continue;
        }
        count++;
        if (!flime_ledger_emitted_state(entry->state)) {
            ready = false;
        }
    }
    *entry_count = count;
    g_mutex_unlock(&session->lock);
    if (!ready) {
        g_set_error_literal(
            error, EXPRESS_VK_FLIME_ERROR, EXPRESS_VK_FLIME_ERROR_STATE,
            "recovery submission has un-emitted ledger entries");
    }
    return ready;
}

bool express_vk_flime_session_ledger_checkpoint(
    ExpressVkFlimeSession *session, uint64_t period_id, GError **error)
{
    uint32_t i;

    if (session == NULL || period_id == 0) {
        g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                            EXPRESS_VK_FLIME_ERROR_INVALID_ARGUMENT,
                            "invalid ledger checkpoint");
        return false;
    }
    g_mutex_lock(&session->lock);
    if (period_id <= session->checkpoint_period_id) {
        bool ret = flime_fail_locked(session, error,
                                     EXPRESS_VK_FLIME_ERROR_STATE, false,
                                     "ledger checkpoint is not monotonic");
        g_mutex_unlock(&session->lock);
        return ret;
    }
    for (i = 0; i < session->ledger_count; i++) {
        if (session->ledger[i].state != EXPRESS_VK_FLIME_LEDGER_COMMITTED) {
            bool ret = flime_fail_locked(
                session, error, EXPRESS_VK_FLIME_ERROR_STATE, false,
                "checkpoint requires every ledger entry to be committed");
            g_mutex_unlock(&session->lock);
            return ret;
        }
    }
    session->ledger_count = 0;
    g_hash_table_remove_all(session->ledger_index);
    session->checkpoint_period_id = period_id;
    g_mutex_unlock(&session->lock);
    return true;
}

bool express_vk_flime_session_ledger_filter_fallback(
    ExpressVkFlimeSession *session, const uint64_t *update_ids,
    size_t update_count, bool *emit_mask, GError **error)
{
    GHashTable *seen = NULL;
    int previous = -1;
    size_t i;

    if (session == NULL ||
        (update_count != 0 && (update_ids == NULL || emit_mask == NULL)) ||
        update_count > EXPRESS_VK_FLIME_HARD_MAX_LEDGER_ENTRIES) {
        g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                            EXPRESS_VK_FLIME_ERROR_INVALID_ARGUMENT,
                            "invalid ledger fallback filter request");
        return false;
    }
    if (update_count != 0) {
        memset(emit_mask, 0, update_count * sizeof(*emit_mask));
        seen = g_hash_table_new(g_int64_hash, g_int64_equal);
        for (i = 0; i < update_count; i++) {
            if (update_ids[i] == 0 ||
                g_hash_table_contains(seen, &update_ids[i])) {
                g_hash_table_destroy(seen);
                g_set_error_literal(
                    error, EXPRESS_VK_FLIME_ERROR,
                    EXPRESS_VK_FLIME_ERROR_INVALID_ARGUMENT,
                    "duplicate or zero identity in fallback batch");
                return false;
            }
            g_hash_table_insert(seen, (gpointer)&update_ids[i],
                                GINT_TO_POINTER(1));
        }
        g_hash_table_destroy(seen);
    }

    g_mutex_lock(&session->lock);
    for (i = 0; i < update_count; i++) {
        int found = flime_ledger_find_locked(session, update_ids[i]);
        int skipped;

        if (found < 0) {
            uint32_t remaining;

            for (remaining = (uint32_t)(previous + 1);
                 remaining < session->ledger_count; remaining++) {
                if (!flime_ledger_emitted_state(
                        session->ledger[remaining].state)) {
                    goto invalid_fallback_batch;
                }
            }
            emit_mask[i] = true;
            previous = (int)session->ledger_count;
            continue;
        }
        if (previous >= (int)session->ledger_count || found <= previous) {
            goto invalid_fallback_batch;
        }
        for (skipped = previous + 1; skipped < found; skipped++) {
            if (!flime_ledger_emitted_state(session->ledger[skipped].state)) {
                goto invalid_fallback_batch;
            }
        }
        switch (session->ledger[found].state) {
        case EXPRESS_VK_FLIME_LEDGER_RELEASED:
        case EXPRESS_VK_FLIME_LEDGER_COMMITTED:
        case EXPRESS_VK_FLIME_LEDGER_REPLAY_EMITTED:
            break;
        case EXPRESS_VK_FLIME_LEDGER_PREPARED:
        case EXPRESS_VK_FLIME_LEDGER_READY:
        case EXPRESS_VK_FLIME_LEDGER_DISCARDED:
            emit_mask[i] = true;
            break;
        case EXPRESS_VK_FLIME_LEDGER_RELEASE_IN_FLIGHT:
        case EXPRESS_VK_FLIME_LEDGER_COMMIT_IN_FLIGHT:
        case EXPRESS_VK_FLIME_LEDGER_REPLAY_IN_FLIGHT:
        case EXPRESS_VK_FLIME_LEDGER_UNKNOWN:
        default:
            goto invalid_fallback_batch;
        }
        previous = found;
    }
    for (i = (size_t)(previous + 1); i < session->ledger_count; i++) {
        if (!flime_ledger_emitted_state(session->ledger[i].state)) {
            goto invalid_fallback_batch;
        }
    }
    g_mutex_unlock(&session->lock);
    return true;

invalid_fallback_batch:
    g_mutex_unlock(&session->lock);
    g_set_error_literal(
        error, EXPRESS_VK_FLIME_ERROR, EXPRESS_VK_FLIME_ERROR_STATE,
        "fallback batch omits, reorders, or races host ledger work");
    return false;
}

ExpressVkFlimeLedgerState express_vk_flime_session_ledger_state(
    ExpressVkFlimeSession *session, uint64_t update_id)
{
    ExpressVkFlimeLedgerState state = EXPRESS_VK_FLIME_LEDGER_UNKNOWN;
    int found;

    if (session == NULL || update_id == 0) {
        return state;
    }
    g_mutex_lock(&session->lock);
    found = flime_ledger_find_locked(session, update_id);
    if (found >= 0) {
        state = session->ledger[found].state;
    }
    g_mutex_unlock(&session->lock);
    return state;
}

typedef struct FlimeDecodedHeader {
    uint16_t type;
    uint32_t flags;
    uint32_t record_count;
    uint64_t period_id;
    uint64_t plan_epoch;
} FlimeDecodedHeader;

static void flime_wire_force_legacy(ExpressVkFlimeSession *session)
{
    g_mutex_lock(&session->lock);
    if (session->ledger_count == 0) {
        flime_enter_legacy_locked(session);
    } else {
        /* Preserve the emitted prefix until generic replay is committed. */
        (void)flime_begin_recovery_locked(session, NULL, NULL);
        session->legacy_fallback = true;
        session->progress.stage = EXPRESS_VK_FLIME_STAGE_RECOVER;
    }
    g_mutex_unlock(&session->lock);
}

static bool flime_wire_error(ExpressVkFlimeSession *session,
                             ExpressVkFlimeError code,
                             const char *message, GError **error)
{
    flime_wire_force_legacy(session);
    g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR, code, message);
    return false;
}

static bool flime_decode_wire_header(ExpressVkFlimeSession *session,
                                     const uint8_t *packet,
                                     size_t packet_bytes,
                                     FlimeDecodedHeader *header,
                                     GError **error)
{
    if (packet_bytes < EXPRESS_VK_FLIME_WIRE_HEADER_SIZE ||
        flime_get_le32(packet + 0) != EXPRESS_VK_FLIME_WIRE_MAGIC ||
        flime_get_le16(packet + 10) !=
            EXPRESS_VK_FLIME_WIRE_HEADER_SIZE ||
        flime_get_le32(packet + 12) != packet_bytes ||
        flime_get_le64(packet + 56) != 0) {
        return flime_wire_error(session,
                                EXPRESS_VK_FLIME_ERROR_INVALID_PACKET,
                                "invalid FLIME wire header", error);
    }
    if (flime_get_le16(packet + 4) != EXPRESS_VK_FLIME_PROTOCOL_MAJOR ||
        flime_get_le16(packet + 6) != EXPRESS_VK_FLIME_PROTOCOL_MINOR) {
        return flime_wire_error(session,
                                EXPRESS_VK_FLIME_ERROR_UNSUPPORTED_VERSION,
                                "unsupported FLIME wire version", error);
    }
    if (flime_get_le64(packet + 24) != session->process_id ||
        flime_get_le64(packet + 32) != session->stream_id ||
        flime_get_le64(packet + 32) == 0) {
        return flime_wire_error(session,
                                EXPRESS_VK_FLIME_ERROR_INVALID_PACKET,
                                "FLIME wire session identity mismatch", error);
    }
    header->type = flime_get_le16(packet + 8);
    header->flags = flime_get_le32(packet + 16);
    header->record_count = flime_get_le32(packet + 20);
    header->period_id = flime_get_le64(packet + 40);
    header->plan_epoch = flime_get_le64(packet + 48);
    return true;
}

static bool flime_ack_plan(ExpressVkFlimeSession *session,
                           uint64_t epoch, uint64_t installed_period,
                           GError **error)
{
    ExpressVkFlimePlanSlot *plan;

    g_mutex_lock(&session->lock);
    plan = &session->plans[session->pending_plan];
    if (!session->pending_valid || epoch == 0 || epoch != plan->epoch ||
        installed_period < plan->apply_period ||
        installed_period <= session->last_period_id) {
        bool ret = flime_fail_locked(session, error,
                                     EXPRESS_VK_FLIME_ERROR_STATE, false,
                                     "plan acknowledgement does not match pending plan");
        g_mutex_unlock(&session->lock);
        return ret;
    }
    session->acknowledged_plan_epoch = epoch;
    session->acknowledged_apply_period = installed_period;
    plan->apply_period = installed_period;
    g_mutex_unlock(&session->lock);
    return true;
}

static bool flime_ingest_guest_profile(
    ExpressVkFlimeSession *session, uint64_t period_id, uint32_t period_flags,
    const ExpressVkFlimeUnitSample *units, uint32_t unit_count,
    const ExpressVkFlimeChunkSample *chunks, uint32_t chunk_count,
    GError **error)
{
    g_mutex_lock(&session->lock);
    if (!flime_ready_locked(session, error) || !session->period_open ||
        session->current_period_id != period_id ||
        session->current_period_flags != period_flags ||
        session->period_unit_count != 0 || session->period_chunk_count != 0) {
        if (error != NULL && *error == NULL) {
            g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                                EXPRESS_VK_FLIME_ERROR_STATE,
                                "guest profile does not match the open period");
        }
        g_mutex_unlock(&session->lock);
        return false;
    }
    if (!flime_reserve_array((void **)&session->period_units,
                             &session->period_unit_capacity, unit_count,
                             session->max_units,
                             sizeof(*session->period_units)) ||
        !flime_reserve_array((void **)&session->period_unit_host_valid,
                             &session->period_unit_valid_capacity, unit_count,
                             session->max_units,
                             sizeof(*session->period_unit_host_valid)) ||
        !flime_reserve_array((void **)&session->period_chunks,
                             &session->period_chunk_capacity, chunk_count,
                             session->max_chunks,
                             sizeof(*session->period_chunks)) ||
        !flime_reserve_array((void **)&session->period_chunk_host_valid,
                             &session->period_chunk_valid_capacity,
                             chunk_count, session->max_chunks,
                             sizeof(*session->period_chunk_host_valid))) {
        bool ret = flime_fail_locked(session, error,
                                     EXPRESS_VK_FLIME_ERROR_OOM, false,
                                     "cannot allocate guest profile records");
        g_mutex_unlock(&session->lock);
        return ret;
    }
    memcpy(session->period_units, units,
           (size_t)unit_count * sizeof(*units));
    memcpy(session->period_chunks, chunks,
           (size_t)chunk_count * sizeof(*chunks));
    memset(session->period_unit_host_valid, 0,
           (size_t)unit_count * sizeof(*session->period_unit_host_valid));
    memset(session->period_chunk_host_valid, 0,
           (size_t)chunk_count * sizeof(*session->period_chunk_host_valid));
    session->period_unit_count = unit_count;
    session->period_chunk_count = chunk_count;
    g_mutex_unlock(&session->lock);
    return true;
}

static bool flime_ingest_profile_wire(
    ExpressVkFlimeSession *session, const uint8_t *packet,
    size_t packet_bytes, const FlimeDecodedHeader *header, GError **error)
{
    const uint8_t *profile = packet + EXPRESS_VK_FLIME_WIRE_HEADER_SIZE;
    const uint8_t *cursor;
    ExpressVkFlimeUnitSample *units = NULL;
    ExpressVkFlimeChunkSample *chunks = NULL;
    uint32_t unit_count;
    uint32_t chunk_count;
    uint32_t period_flags;
    uint32_t max_units;
    uint32_t max_chunks;
    uint32_t dispatches_per_unit;
    uint32_t records;
    size_t unit_bytes;
    size_t chunk_bytes;
    size_t expected;
    uint64_t previous_template_end = 0;
    uint32_t previous_dispatch = 0;
    uint32_t i;
    bool result;

    if (packet_bytes < EXPRESS_VK_FLIME_WIRE_HEADER_SIZE +
                       EXPRESS_VK_FLIME_WIRE_PROFILE_SIZE) {
        return flime_wire_error(session,
                                EXPRESS_VK_FLIME_ERROR_INVALID_PACKET,
                                "truncated FLIME profile", error);
    }
    unit_count = flime_get_le32(profile + 0);
    chunk_count = flime_get_le32(profile + 4);
    period_flags = flime_get_le32(profile + 8);
    if (flime_get_le32(profile + 12) != 0 ||
        flime_get_le64(profile + 24) != 0 || unit_count == 0 ||
        unit_count > UINT32_MAX - chunk_count) {
        return flime_wire_error(session,
                                EXPRESS_VK_FLIME_ERROR_INVALID_PACKET,
                                "invalid FLIME profile header", error);
    }
    records = unit_count + chunk_count;
    if (header->record_count != records || header->flags != 0 ||
        header->period_id == 0 || header->plan_epoch != 0 ||
        !flime_size_mul(unit_count, EXPRESS_VK_FLIME_WIRE_UNIT_SIZE,
                        &unit_bytes) ||
        !flime_size_mul(chunk_count, EXPRESS_VK_FLIME_WIRE_CHUNK_SIZE,
                        &chunk_bytes) ||
        !flime_size_add(EXPRESS_VK_FLIME_WIRE_HEADER_SIZE +
                        EXPRESS_VK_FLIME_WIRE_PROFILE_SIZE,
                        unit_bytes, &expected) ||
        !flime_size_add(expected, chunk_bytes, &expected) ||
        expected != packet_bytes) {
        return flime_wire_error(session,
                                EXPRESS_VK_FLIME_ERROR_INVALID_PACKET,
                                "FLIME profile size or count mismatch", error);
    }

    g_mutex_lock(&session->lock);
    max_units = session->max_units;
    max_chunks = session->max_chunks;
    dispatches_per_unit = session->dispatches_per_unit;
    g_mutex_unlock(&session->lock);
    if (unit_count > max_units || chunk_count > max_chunks ||
        dispatches_per_unit == 0) {
        return flime_wire_error(session, EXPRESS_VK_FLIME_ERROR_LIMIT,
                                "FLIME profile exceeds negotiated limits",
                                error);
    }
    units = flime_try_array0(unit_count, sizeof(*units));
    if (chunk_count != 0) {
        chunks = flime_try_array0(chunk_count, sizeof(*chunks));
    }
    if (units == NULL || (chunk_count != 0 && chunks == NULL)) {
        g_free(units);
        g_free(chunks);
        return flime_wire_error(session, EXPRESS_VK_FLIME_ERROR_OOM,
                                "cannot decode FLIME profile", error);
    }

    cursor = profile + EXPRESS_VK_FLIME_WIRE_PROFILE_SIZE;
    for (i = 0; i < unit_count; i++,
         cursor += EXPRESS_VK_FLIME_WIRE_UNIT_SIZE) {
        uint64_t template_end;
        uint32_t dispatch_end = flime_get_le32(cursor + 8);
        uint32_t dispatch_count = dispatch_end - previous_dispatch;

        units[i].unit_index = flime_get_le32(cursor + 0);
        units[i].flags = flime_get_le32(cursor + 4);
        units[i].dispatch_end = dispatch_end;
        units[i].template_offset = flime_get_le64(cursor + 16);
        units[i].encoded_bytes = flime_get_le64(cursor + 24);
        units[i].guest_prepare_ns = flime_get_le64(cursor + 32);
        units[i].host_realize_ns = 0;
        if (units[i].unit_index != i ||
            (units[i].flags & ~EXPRESS_VK_FLIME_UNIT_FINAL) != 0 ||
            (i + 1 < unit_count && units[i].flags != 0) ||
            (i + 1 == unit_count &&
             !(units[i].flags & EXPRESS_VK_FLIME_UNIT_FINAL)) ||
            dispatch_end <= previous_dispatch ||
            dispatch_count > dispatches_per_unit ||
            (i + 1 < unit_count &&
             dispatch_count != dispatches_per_unit) ||
            flime_get_le32(cursor + 12) != 0 ||
            flime_get_le64(cursor + 40) != 0 ||
            units[i].encoded_bytes == 0 ||
            units[i].template_offset < previous_template_end ||
            !flime_u64_add(units[i].template_offset,
                           units[i].encoded_bytes, &template_end)) {
            g_free(units);
            g_free(chunks);
            return flime_wire_error(
                session, EXPRESS_VK_FLIME_ERROR_INVALID_PACKET,
                "invalid FLIME unit record", error);
        }
        previous_dispatch = dispatch_end;
        previous_template_end = template_end;
    }
    for (i = 0; i < chunk_count; i++,
         cursor += EXPRESS_VK_FLIME_WIRE_CHUNK_SIZE) {
        uint32_t expected_first = i == 0 ? 0 :
            chunks[i - 1].unit_past_end;

        chunks[i].chunk_index = flime_get_le32(cursor + 0);
        chunks[i].first_unit = flime_get_le32(cursor + 4);
        chunks[i].unit_past_end = flime_get_le32(cursor + 8);
        chunks[i].flags = flime_get_le32(cursor + 12);
        chunks[i].guest_handoff_ns = flime_get_le64(cursor + 16);
        chunks[i].host_handoff_ns = 0;
        chunks[i].host_realize_ns = 0;
        chunks[i].completion_ns = 0;
        if (chunks[i].chunk_index != i || chunks[i].flags != 0 ||
            chunks[i].first_unit != expected_first ||
            chunks[i].unit_past_end <= chunks[i].first_unit ||
            chunks[i].unit_past_end > unit_count ||
            flime_get_le64(cursor + 24) != 0 ||
            flime_get_le64(cursor + 32) != 0 ||
            flime_get_le64(cursor + 40) != 0) {
            g_free(units);
            g_free(chunks);
            return flime_wire_error(
                session, EXPRESS_VK_FLIME_ERROR_INVALID_PACKET,
                "invalid FLIME chunk record", error);
        }
    }
    if (chunk_count != 0 &&
        chunks[chunk_count - 1].unit_past_end != unit_count) {
        g_free(units);
        g_free(chunks);
        return flime_wire_error(session,
                                EXPRESS_VK_FLIME_ERROR_INVALID_PACKET,
                                "FLIME chunks do not cover all units", error);
    }

    result = flime_ingest_guest_profile(
        session, header->period_id, period_flags, units, unit_count,
        chunks, chunk_count, error);
    g_free(units);
    g_free(chunks);
    if (!result) {
        flime_wire_force_legacy(session);
    }
    return result;
}

static bool flime_wire_progress_flags_canonical(uint16_t event,
                                                uint16_t flags)
{
    const uint16_t match_succeeded =
        EXPRESS_VK_FLIME_WIRE_PROGRESS_MATCH_SUCCEEDED;
    const uint16_t generic_shadow_ran =
        EXPRESS_VK_FLIME_WIRE_PROGRESS_GENERIC_SHADOW_RAN;

    switch (event) {
    case EXPRESS_VK_FLIME_PROGRESS_LEARN_COMPLETE:
        return flags == generic_shadow_ran;
    case EXPRESS_VK_FLIME_PROGRESS_MATCH_COMPLETE:
        return flags == generic_shadow_ran ||
               flags == (generic_shadow_ran | match_succeeded);
    case EXPRESS_VK_FLIME_PROGRESS_FAST_PERIOD_COMPLETE:
        return flags == match_succeeded;
    case EXPRESS_VK_FLIME_PROGRESS_MISMATCH:
        return flags == 0;
    case EXPRESS_VK_FLIME_PROGRESS_RECOVERY_COMPLETE:
        return flags == generic_shadow_ran;
    default:
        return false;
    }
}

bool express_vk_flime_session_ingest_wire(ExpressVkFlimeSession *session,
                                          const void *packet_data,
                                          size_t packet_bytes,
                                          ExpressVkFlimeNegotiated *caps_reply,
                                          GError **error)
{
    const uint8_t *packet = packet_data;
    FlimeDecodedHeader header;
    bool result;

    if (caps_reply != NULL) {
        memset(caps_reply, 0, sizeof(*caps_reply));
    }
    if (session == NULL || packet == NULL) {
        g_set_error_literal(error, EXPRESS_VK_FLIME_ERROR,
                            EXPRESS_VK_FLIME_ERROR_INVALID_ARGUMENT,
                            "FLIME session or wire packet is NULL");
        return false;
    }
    if (!flime_decode_wire_header(session, packet, packet_bytes,
                                  &header, error)) {
        return false;
    }

    switch (header.type) {
    case EXPRESS_VK_FLIME_WIRE_CAPABILITIES: {
        const uint8_t *caps = packet + EXPRESS_VK_FLIME_WIRE_HEADER_SIZE;
        ExpressVkFlimePeerCaps peer;

        if (packet_bytes != EXPRESS_VK_FLIME_WIRE_HEADER_SIZE +
                            EXPRESS_VK_FLIME_WIRE_CAPS_SIZE ||
            header.flags != 0 || header.record_count != 1 ||
            header.period_id != 0 || header.plan_epoch != 0 ||
            flime_get_le64(caps + 32) != 0) {
            return flime_wire_error(
                session, EXPRESS_VK_FLIME_ERROR_INVALID_PACKET,
                "invalid FLIME capability packet", error);
        }
        memset(&peer, 0, sizeof(peer));
        peer.min_major = flime_get_le16(caps + 0);
        peer.min_minor = flime_get_le16(caps + 2);
        peer.max_major = flime_get_le16(caps + 4);
        peer.max_minor = flime_get_le16(caps + 6);
        peer.capabilities = flime_get_le64(caps + 8);
        peer.max_units = flime_get_le32(caps + 16);
        peer.max_chunks = flime_get_le32(caps + 20);
        peer.dispatches_per_unit = flime_get_le32(caps + 24);
        peer.replan_periods = flime_get_le32(caps + 28);
        result = express_vk_flime_session_negotiate(
            session, &peer, caps_reply, error);
        if (!result) {
            flime_wire_force_legacy(session);
        }
        return result;
    }
    case EXPRESS_VK_FLIME_WIRE_PERIOD_BEGIN: {
        const uint32_t allowed = EXPRESS_VK_FLIME_PERIOD_SINGLE_FLUSH |
            EXPRESS_VK_FLIME_PERIOD_FINE_PROFILE |
            EXPRESS_VK_FLIME_PERIOD_STABLE_FAST |
            EXPRESS_VK_FLIME_PERIOD_FORCE_REPLAN;

        if (packet_bytes != EXPRESS_VK_FLIME_WIRE_HEADER_SIZE ||
            header.record_count != 0 || header.period_id == 0 ||
            (header.flags & ~allowed) != 0) {
            return flime_wire_error(
                session, EXPRESS_VK_FLIME_ERROR_INVALID_PACKET,
                "invalid FLIME period-begin packet", error);
        }
        if (header.plan_epoch != 0 &&
            !flime_ack_plan(session, header.plan_epoch,
                            header.period_id, error)) {
            flime_wire_force_legacy(session);
            return false;
        }
        result = flime_period_begin(session, header.period_id, header.flags,
                                    true, NULL, error);
        if (!result) {
            flime_wire_force_legacy(session);
        }
        return result;
    }
    case EXPRESS_VK_FLIME_WIRE_PROFILE_PERIOD:
        return flime_ingest_profile_wire(session, packet, packet_bytes,
                                         &header, error);
    case EXPRESS_VK_FLIME_WIRE_PROGRESS_EVENT: {
        const uint8_t *progress = packet + EXPRESS_VK_FLIME_WIRE_HEADER_SIZE;
        uint16_t event;
        uint16_t flags;
        uint32_t template_entries;

        if (packet_bytes != EXPRESS_VK_FLIME_WIRE_HEADER_SIZE +
                            EXPRESS_VK_FLIME_WIRE_PROGRESS_SIZE ||
            header.flags != 0 || header.record_count != 1 ||
            header.plan_epoch != 0 ||
            flime_get_le64(progress + 16) != 0 ||
            flime_get_le64(progress + 24) != 0) {
            return flime_wire_error(
                session, EXPRESS_VK_FLIME_ERROR_INVALID_PACKET,
                "invalid FLIME progress packet", error);
        }
        event = flime_get_le16(progress + 0);
        flags = flime_get_le16(progress + 2);
        template_entries = flime_get_le32(progress + 4);
        if (!flime_wire_progress_flags_canonical(event, flags) ||
            ((event == EXPRESS_VK_FLIME_PROGRESS_LEARN_COMPLETE ||
              event == EXPRESS_VK_FLIME_PROGRESS_MATCH_COMPLETE ||
              event == EXPRESS_VK_FLIME_PROGRESS_FAST_PERIOD_COMPLETE) ?
             template_entries == 0 : template_entries != 0) ||
            (event == EXPRESS_VK_FLIME_PROGRESS_FAST_PERIOD_COMPLETE &&
             (flime_get_le64(progress + 8) == 0 ||
              flime_get_le64(progress + 8) != header.period_id))) {
            return flime_wire_error(
                session, EXPRESS_VK_FLIME_ERROR_INVALID_PACKET,
                "invalid FLIME progress event", error);
        }
        if (event == EXPRESS_VK_FLIME_PROGRESS_FAST_PERIOD_COMPLETE) {
            bool correct_boundary;

            g_mutex_lock(&session->lock);
            correct_boundary = !session->period_open &&
                session->last_period_id == header.period_id;
            g_mutex_unlock(&session->lock);
            if (!correct_boundary) {
                return flime_wire_error(
                    session, EXPRESS_VK_FLIME_ERROR_INVALID_PACKET,
                    "FAST progress is not at the learned submit boundary",
                    error);
            }
        } else if (header.period_id != 0 ||
                   flime_get_le64(progress + 8) != 0) {
            return flime_wire_error(
                session, EXPRESS_VK_FLIME_ERROR_INVALID_PACKET,
                "non-FAST progress carries a period boundary", error);
        }
        result = express_vk_flime_session_progress(
            session, event,
            !!(flags & EXPRESS_VK_FLIME_WIRE_PROGRESS_MATCH_SUCCEEDED),
            !!(flags & EXPRESS_VK_FLIME_WIRE_PROGRESS_GENERIC_SHADOW_RAN),
            template_entries, error);
        if (!result) {
            flime_wire_force_legacy(session);
        }
        return result;
    }
    case EXPRESS_VK_FLIME_WIRE_PLAN_ACK: {
        const uint8_t *ack = packet + EXPRESS_VK_FLIME_WIRE_HEADER_SIZE;
        uint64_t installed_period;

        if (packet_bytes != EXPRESS_VK_FLIME_WIRE_HEADER_SIZE +
                            EXPRESS_VK_FLIME_WIRE_PLAN_ACK_SIZE ||
            header.flags != 0 || header.record_count != 1 ||
            header.period_id != 0 || header.plan_epoch == 0 ||
            flime_get_le32(ack + 0) !=
                EXPRESS_VK_FLIME_WIRE_PLAN_INSTALLED ||
            flime_get_le32(ack + 4) != 0) {
            return flime_wire_error(
                session, EXPRESS_VK_FLIME_ERROR_INVALID_PACKET,
                "invalid FLIME plan acknowledgement", error);
        }
        installed_period = flime_get_le64(ack + 8);
        result = flime_ack_plan(session, header.plan_epoch,
                                installed_period, error);
        if (!result) {
            flime_wire_force_legacy(session);
        }
        return result;
    }
    case EXPRESS_VK_FLIME_WIRE_SESSION_RESET:
        if (packet_bytes != EXPRESS_VK_FLIME_WIRE_HEADER_SIZE ||
            header.flags != 0 || header.record_count != 0 ||
            header.period_id != 0 || header.plan_epoch != 0) {
            return flime_wire_error(session,
                                    EXPRESS_VK_FLIME_ERROR_INVALID_PACKET,
                                    "invalid FLIME reset packet", error);
        }
        return express_vk_flime_session_reset(session, error);
    case EXPRESS_VK_FLIME_WIRE_SESSION_TEARDOWN:
        if (packet_bytes != EXPRESS_VK_FLIME_WIRE_HEADER_SIZE ||
            header.flags != 0 || header.record_count != 0 ||
            header.period_id != 0 || header.plan_epoch != 0) {
            return flime_wire_error(session,
                                    EXPRESS_VK_FLIME_ERROR_INVALID_PACKET,
                                    "invalid FLIME teardown packet", error);
        }
        g_mutex_lock(&session->lock);
        flime_enter_legacy_locked(session);
        session->teardown_requested = true;
        g_mutex_unlock(&session->lock);
        return true;
    case EXPRESS_VK_FLIME_WIRE_INTERVAL_SIGNATURE:
        if (packet_bytes != EXPRESS_VK_FLIME_WIRE_HEADER_SIZE +
                            EXPRESS_VK_FLIME_WIRE_INTERVAL_SIZE ||
            header.flags != 0 || header.record_count != 1 ||
            header.period_id != 0 || header.plan_epoch != 0) {
            return flime_wire_error(
                session, EXPRESS_VK_FLIME_ERROR_INVALID_PACKET,
                "invalid FLIME interval-signature packet", error);
        }
        result = express_vk_flime_session_note_interval(
            session,
            flime_get_le64(packet + EXPRESS_VK_FLIME_WIRE_HEADER_SIZE),
            NULL, error);
        if (!result) {
            flime_wire_force_legacy(session);
        }
        return result;
    default:
        return flime_wire_error(session,
                                EXPRESS_VK_FLIME_ERROR_INVALID_PACKET,
                                "unknown FLIME wire packet type", error);
    }
}
