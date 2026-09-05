/**
 * @file egl_trans.c
 * @brief
 * @version 0.1
 * @date 2020-11-25
 *
 * @copyright Copyright (c) 2020
 *
 */

#include "hw/express-gpu/vk_trans.h"
#include "hw/express-gpu/express_gpu.h"

#include "hw/express-mem/express_sync.h"
#include "hw/express-gpu/express_vk_decode_from_stream.h"
#include "hw/express-gpu/express_vk_handle_mapping.h"
#include "hw/express-gpu/express_vk_flime_bridge.h"
#include "hw/express-gpu/vk_helper.h"
#include "hw/express-gpu/vulkan_surface.h"
#include <stdlib.h>
#include <time.h>

#ifdef __WIN32__

#include <vulkan/vulkan_win32.h>

PFN_vkGetMemoryWin32HandleKHR pfn_vkGetMemoryWin32HandleKHR = NULL;

static bool g_is_intel_gpu = false;

void init_interop_once(VkDevice device) {
    static bool initialized = false;
    if (initialized) return;

    pfn_vkGetMemoryWin32HandleKHR = (PFN_vkGetMemoryWin32HandleKHR)
        vkGetDeviceProcAddr(device, "vkGetMemoryWin32HandleKHR");

    if (pfn_vkGetMemoryWin32HandleKHR) {
        LOGD("[Interop] vkGetMemoryWin32HandleKHR loaded successfully");
    } else {
        LOGW("[Interop] vkGetMemoryWin32HandleKHR not available, fallback to CPU copy");
    }

    initialized = true;
}
#endif

#ifndef FUNID_vkExpressRegisterMappedMemoryANDROID
#define FUNID_vkExpressRegisterMappedMemoryANDROID 1902
#endif

#ifndef FUNID_vkExpressUnregisterMappedMemoryANDROID
#define FUNID_vkExpressUnregisterMappedMemoryANDROID 1903
#endif

#ifndef FUNID_vkExpressWaitFenceAndInvalidateANDROID
#define FUNID_vkExpressWaitFenceAndInvalidateANDROID 1904
#endif

#define EXPRESS_VK_WAIT_COMMIT_DOWNLOAD_MAX_BYTES (4ULL * 1024ULL * 1024ULL)
/*
 * Optional baseline range prefetch after fence completion. Guest-side
 * mapped-memory invalidation owns correctness, so this stays disabled by
 * default to avoid duplicate host-to-guest copies.
 */
#define EXPRESS_VK_ENABLE_FENCE_OUTPUT_HINT_COMMIT 0

typedef struct ExpressVkRegisteredMemory {
    uint64_t guest_memory;
    uint64_t host_memory;
    uint64_t size;
    Guest_Mem *guest_mem;
    struct ExpressVkRegisteredMemory *next;
} ExpressVkRegisteredMemory;

typedef struct ExpressVkSubmitRangeHintWire {
    uint64_t device;
    uint64_t memory;
    uint64_t offset;
    uint64_t size;
} ExpressVkSubmitRangeHintWire;

typedef struct ExpressVkSubmitRangeHint {
    ExpressVkRegisteredMemory *reg;
    VkDevice device;
    uint64_t guest_memory;
    uint64_t host_memory;
    void *host_ptr;
    uint64_t offset;
    uint64_t size;
} ExpressVkSubmitRangeHint;

typedef struct ExpressVkSubmitHints {
    bool present;
    uint32_t flush_count;
    uint32_t output_count;
    ExpressVkSubmitRangeHint *flush_ranges;
    ExpressVkSubmitRangeHint *output_ranges;
} ExpressVkSubmitHints;

typedef struct ExpressVkFenceOutputHints {
    VkFence fence;
    uint64_t guest_fence;
    uint32_t range_count;
    ExpressVkSubmitRangeHint *ranges;
    struct ExpressVkFenceOutputHints *next;
} ExpressVkFenceOutputHints;

static ExpressVkRegisteredMemory *g_express_vk_registered_memories = NULL;
static ExpressVkFenceOutputHints *g_express_vk_fence_output_hints = NULL;

typedef struct ExpressVkDescriptorSetMapping {
    uint64_t guest_set;
    VkDescriptorSet host_set;
    VkDescriptorPool host_pool;
    struct ExpressVkDescriptorSetMapping *next;
} ExpressVkDescriptorSetMapping;

static ExpressVkDescriptorSetMapping *g_express_vk_descriptor_sets = NULL;
static GMutex g_express_vk_descriptor_sets_lock;
static gsize g_express_vk_descriptor_sets_lock_inited = 0;

static void express_vk_descriptor_set_cache_init(void)
{
    if (g_once_init_enter(&g_express_vk_descriptor_sets_lock_inited)) {
        g_mutex_init(&g_express_vk_descriptor_sets_lock);
        g_once_init_leave(&g_express_vk_descriptor_sets_lock_inited, 1);
    }
}

static void express_vk_remember_descriptor_set(uint64_t guest_set,
                                               VkDescriptorSet host_set,
                                               VkDescriptorPool host_pool)
{
    if (guest_set == 0) {
        return;
    }

    express_vk_descriptor_set_cache_init();
    g_mutex_lock(&g_express_vk_descriptor_sets_lock);
    for (ExpressVkDescriptorSetMapping *entry = g_express_vk_descriptor_sets;
         entry;
        entry = entry->next) {
        if (entry->guest_set == guest_set) {
            entry->host_set = host_set;
            entry->host_pool = host_pool;
            g_mutex_unlock(&g_express_vk_descriptor_sets_lock);
            return;
        }
    }

    ExpressVkDescriptorSetMapping *entry = g_new0(ExpressVkDescriptorSetMapping, 1);
    entry->guest_set = guest_set;
    entry->host_set = host_set;
    entry->host_pool = host_pool;
    entry->next = g_express_vk_descriptor_sets;
    g_express_vk_descriptor_sets = entry;
    g_mutex_unlock(&g_express_vk_descriptor_sets_lock);
}

static VkDescriptorSet express_vk_lookup_descriptor_set_cached(uint64_t guest_set, bool *found)
{
    if (found) {
        *found = false;
    }
    if (guest_set == 0) {
        if (found) {
            *found = true;
        }
        return VK_NULL_HANDLE;
    }

    express_vk_descriptor_set_cache_init();
    g_mutex_lock(&g_express_vk_descriptor_sets_lock);
    for (ExpressVkDescriptorSetMapping *entry = g_express_vk_descriptor_sets;
         entry;
         entry = entry->next) {
        if (entry->guest_set == guest_set) {
            VkDescriptorSet host_set = entry->host_set;
            if (found) {
                *found = true;
            }
            g_mutex_unlock(&g_express_vk_descriptor_sets_lock);
            return host_set;
        }
    }
    g_mutex_unlock(&g_express_vk_descriptor_sets_lock);
    return VK_NULL_HANDLE;
}

static VkDescriptorSet express_vk_lookup_descriptor_set(uint64_t guest_set)
{
    bool found = false;
    VkDescriptorSet host_set =
        express_vk_lookup_descriptor_set_cached(guest_set, &found);
    if (found) {
        return host_set;
    }

    express_vk_descriptor_set_cache_init();
    g_mutex_lock(&g_express_vk_descriptor_sets_lock);
    for (ExpressVkDescriptorSetMapping *entry = g_express_vk_descriptor_sets;
         entry;
         entry = entry->next) {
        if (entry->guest_set == guest_set) {
            host_set = entry->host_set;
            g_mutex_unlock(&g_express_vk_descriptor_sets_lock);
            return host_set;
        }
    }

    ExpressVkDescriptorSetMapping *entry = g_new0(ExpressVkDescriptorSetMapping, 1);
    entry->guest_set = guest_set;
    entry->host_set = VK_NULL_HANDLE;
    entry->host_pool = VK_NULL_HANDLE;
    entry->next = g_express_vk_descriptor_sets;
    g_express_vk_descriptor_sets = entry;
    g_mutex_unlock(&g_express_vk_descriptor_sets_lock);

    LOGE("Host: descriptor set mapping missing once, guest=0x%llx",
         (unsigned long long)guest_set);
    return VK_NULL_HANDLE;
}

static void express_vk_forget_descriptor_set(uint64_t guest_set)
{
    if (guest_set == 0) {
        return;
    }

    express_vk_descriptor_set_cache_init();
    g_mutex_lock(&g_express_vk_descriptor_sets_lock);
    ExpressVkDescriptorSetMapping **link = &g_express_vk_descriptor_sets;
    while (*link) {
        ExpressVkDescriptorSetMapping *entry = *link;
        if (entry->guest_set == guest_set) {
            *link = entry->next;
            g_free(entry);
            break;
        }
        link = &entry->next;
    }
    g_mutex_unlock(&g_express_vk_descriptor_sets_lock);
}

static void express_vk_forget_descriptor_sets_for_pool(VkDescriptorPool host_pool)
{
    if (host_pool == VK_NULL_HANDLE) {
        return;
    }

    express_vk_descriptor_set_cache_init();
    g_mutex_lock(&g_express_vk_descriptor_sets_lock);
    ExpressVkDescriptorSetMapping **link = &g_express_vk_descriptor_sets;
    while (*link) {
        ExpressVkDescriptorSetMapping *entry = *link;
        if (entry->host_pool == host_pool) {
            if (entry->guest_set != 0) {
                remove_mapping(EXPRESS_VK_OBJECT_TYPE_DESCRIPTOR_SET,
                               entry->guest_set);
            }
            *link = entry->next;
            g_free(entry);
            continue;
        }
        link = &entry->next;
    }
    g_mutex_unlock(&g_express_vk_descriptor_sets_lock);
}

#define EXPRESS_VK_SUBMIT_HINTS_MAGIC 0x48564b45u
#define EXPRESS_VK_SUBMIT_HINTS_VERSION 1u

#define EXPRESS_VK_TRANSFER_UPLOAD_THRESHOLD (4ULL * 1024ULL * 1024ULL)
#define EXPRESS_VK_TRANSFER_DOWNLOAD_THRESHOLD (16ULL * 1024ULL * 1024ULL)
#define EXPRESS_VK_TRANSFER_WORKERS 2
#define EXPRESS_VK_ENABLE_SYNC_DIAG_LOG 0
#define EXPRESS_VK_ENABLE_SYNC_SUMMARY_LOG 0
#define EXPRESS_VK_ENABLE_HOST_HOT_SUMMARY_LOG 0
#define EXPRESS_VK_ENABLE_DESCRIPTOR_TRACE 0

// Correctness guard: async host uploads must be complete before any real GPU
// submit can consume mapped data. Guest hints keep the common path narrow, and
// this host-side fallback catches missing or stale hints.
#define EXPRESS_VK_STRICT_UPLOAD_WAIT_BEFORE_SUBMIT 1

typedef enum ExpressVkTransferDirection {
    EXPRESS_VK_TRANSFER_UPLOAD = 0,
    EXPRESS_VK_TRANSFER_DOWNLOAD = 1,
    EXPRESS_VK_TRANSFER_ANY = 2,
} ExpressVkTransferDirection;

typedef struct ExpressVkTransferTask {
    uint64_t seq;
    ExpressVkTransferDirection direction;
    ExpressVkRegisteredMemory *reg;
    VkDevice device;
    uint64_t host_memory;
    void *host_ptr;
    uint64_t offset;
    uint64_t size;
    int done;
    int failed;
    gint64 enqueue_us;
    gint64 start_us;
    gint64 end_us;
    GCond done_cond;
    struct ExpressVkTransferTask *next;
} ExpressVkTransferTask;

static GThreadPool *g_express_vk_transfer_pool = NULL;
static GMutex g_express_vk_transfer_lock;
static volatile gsize g_express_vk_transfer_once = 0;
static uint64_t g_express_vk_transfer_seq = 0;
static ExpressVkTransferTask *g_express_vk_transfer_tasks = NULL;
static GMutex g_express_vk_stats_lock;
typedef struct ExpressVkQueueInfo {
    VkDevice device;
    VkQueue queue;
    struct ExpressVkQueueInfo *next;
} ExpressVkQueueInfo;

static GMutex g_express_vk_staging_lock;
GMutex g_express_vk_transaction_lock;
static ExpressVkQueueInfo *g_express_vk_queues = NULL;

typedef struct ExpressVkSyncStats {
    uint64_t events;
    uint64_t hints_parsed;
    uint64_t hint_flush_ranges;
    uint64_t hint_output_ranges;
    uint64_t upload_enqueued;
    uint64_t download_enqueued;
    uint64_t upload_completed;
    uint64_t download_completed;
    uint64_t wait_tasks;
    uint64_t wait_bytes;
    uint64_t submit_wait_all_uploads;
    uint64_t submit_wait_range_calls;
    uint64_t submit_wait_range_tasks;
    uint64_t wait_committed_ranges;
    uint64_t wait_committed_bytes;
    uint64_t wait_committed_prefetched;
    uint64_t invalidate_prefetched;
    uint64_t invalidate_synced;
    uint64_t invalidate_queued;
    uint64_t invalidate_prefetched_bytes;
    uint64_t invalidate_synced_bytes;
    uint64_t invalidate_queued_bytes;
    uint64_t flush_synced;
    uint64_t flush_queued;
} ExpressVkSyncStats;

static ExpressVkSyncStats g_express_vk_stats;
static const uint64_t kExpressVkSummaryLogEvery = 4096;

static void express_vk_stats_reset(const char *reason)
{
    if (!EXPRESS_VK_ENABLE_SYNC_SUMMARY_LOG) {
        (void)reason;
        return;
    }
    g_mutex_lock(&g_express_vk_stats_lock);
    memset(&g_express_vk_stats, 0, sizeof(g_express_vk_stats));
    g_mutex_unlock(&g_express_vk_stats_lock);
#if EXPRESS_VK_ENABLE_SYNC_SUMMARY_LOG
    LOGD("[ExpressVkSummary] reset reason=%s", reason ? reason : "unknown");
#else
    (void)reason;
#endif
}

static void express_vk_stats_maybe_log_locked(const char *reason)
{
    if (!EXPRESS_VK_ENABLE_SYNC_SUMMARY_LOG) {
        (void)reason;
        return;
    }
    if ((g_express_vk_stats.events % kExpressVkSummaryLogEvery) != 0) {
        return;
    }

    LOGD("[ExpressVkSummary] reason=%s events=%llu hints=%llu flush_hint_ranges=%llu output_hint_ranges=%llu "
         "enq_up=%llu enq_down=%llu done_up=%llu done_down=%llu wait_tasks=%llu wait_bytes=%llu "
         "submit_wait_all=%llu submit_range_calls=%llu submit_range_tasks=%llu "
         "wait_commit_ranges=%llu wait_commit_bytes=%llu wait_commit_prefetched=%llu "
         "invalidate_prefetched=%llu invalidate_synced=%llu invalidate_queued=%llu "
         "invalidate_prefetched_bytes=%llu invalidate_synced_bytes=%llu invalidate_queued_bytes=%llu "
         "flush_synced=%llu flush_queued=%llu",
         reason ? reason : "periodic",
         (unsigned long long)g_express_vk_stats.events,
         (unsigned long long)g_express_vk_stats.hints_parsed,
         (unsigned long long)g_express_vk_stats.hint_flush_ranges,
         (unsigned long long)g_express_vk_stats.hint_output_ranges,
         (unsigned long long)g_express_vk_stats.upload_enqueued,
         (unsigned long long)g_express_vk_stats.download_enqueued,
         (unsigned long long)g_express_vk_stats.upload_completed,
         (unsigned long long)g_express_vk_stats.download_completed,
         (unsigned long long)g_express_vk_stats.wait_tasks,
         (unsigned long long)g_express_vk_stats.wait_bytes,
         (unsigned long long)g_express_vk_stats.submit_wait_all_uploads,
         (unsigned long long)g_express_vk_stats.submit_wait_range_calls,
         (unsigned long long)g_express_vk_stats.submit_wait_range_tasks,
         (unsigned long long)g_express_vk_stats.wait_committed_ranges,
         (unsigned long long)g_express_vk_stats.wait_committed_bytes,
         (unsigned long long)g_express_vk_stats.wait_committed_prefetched,
         (unsigned long long)g_express_vk_stats.invalidate_prefetched,
         (unsigned long long)g_express_vk_stats.invalidate_synced,
         (unsigned long long)g_express_vk_stats.invalidate_queued,
         (unsigned long long)g_express_vk_stats.invalidate_prefetched_bytes,
         (unsigned long long)g_express_vk_stats.invalidate_synced_bytes,
         (unsigned long long)g_express_vk_stats.invalidate_queued_bytes,
         (unsigned long long)g_express_vk_stats.flush_synced,
         (unsigned long long)g_express_vk_stats.flush_queued);
}

static void express_vk_stats_note(const char *reason)
{
    if (!EXPRESS_VK_ENABLE_SYNC_SUMMARY_LOG) {
        (void)reason;
        return;
    }
    g_mutex_lock(&g_express_vk_stats_lock);
    g_express_vk_stats.events++;
    express_vk_stats_maybe_log_locked(reason);
    g_mutex_unlock(&g_express_vk_stats_lock);
}

static void express_vk_stats_add_u64(uint64_t *field, uint64_t value)
{
    if (!EXPRESS_VK_ENABLE_SYNC_SUMMARY_LOG) {
        (void)field;
        (void)value;
        return;
    }
    g_mutex_lock(&g_express_vk_stats_lock);
    *field += value;
    g_express_vk_stats.events++;
    express_vk_stats_maybe_log_locked(NULL);
    g_mutex_unlock(&g_express_vk_stats_lock);
}

static void express_vk_stats_add_invalidate(bool prefetched, bool queued, uint64_t bytes)
{
    if (!EXPRESS_VK_ENABLE_SYNC_SUMMARY_LOG) {
        (void)prefetched;
        (void)queued;
        (void)bytes;
        return;
    }
    g_mutex_lock(&g_express_vk_stats_lock);
    if (prefetched) {
        g_express_vk_stats.invalidate_prefetched++;
        g_express_vk_stats.invalidate_prefetched_bytes += bytes;
    } else if (queued) {
        g_express_vk_stats.invalidate_queued++;
        g_express_vk_stats.invalidate_queued_bytes += bytes;
    } else {
        g_express_vk_stats.invalidate_synced++;
        g_express_vk_stats.invalidate_synced_bytes += bytes;
    }
    g_express_vk_stats.events++;
    express_vk_stats_maybe_log_locked("invalidate");
    g_mutex_unlock(&g_express_vk_stats_lock);
}

static const char *express_vk_transfer_direction_name(ExpressVkTransferDirection direction)
{
    return direction == EXPRESS_VK_TRANSFER_UPLOAD ? "upload" : "download";
}

static double express_vk_elapsed_ms(gint64 start_us, gint64 end_us)
{
    if (end_us <= start_us) {
        return 0.0;
    }
    return (double)(end_us - start_us) / 1000.0;
}

static void express_vk_remember_queue(VkDevice device,
                                      VkQueue queue)
{
    if (device == VK_NULL_HANDLE || queue == VK_NULL_HANDLE) {
        return;
    }

    g_mutex_lock(&g_express_vk_staging_lock);
    for (ExpressVkQueueInfo *it = g_express_vk_queues; it != NULL; it = it->next) {
        if (it->queue == queue) {
            it->device = device;
            g_mutex_unlock(&g_express_vk_staging_lock);
            return;
        }
    }
    ExpressVkQueueInfo *entry = g_new0(ExpressVkQueueInfo, 1);
    entry->device = device;
    entry->queue = queue;
    entry->next = g_express_vk_queues;
    g_express_vk_queues = entry;
    g_mutex_unlock(&g_express_vk_staging_lock);
}

static bool express_vk_lookup_queue_info(VkQueue queue, ExpressVkQueueInfo *out_info)
{
    if (queue == VK_NULL_HANDLE || out_info == NULL) {
        return false;
    }

    bool found = false;
    g_mutex_lock(&g_express_vk_staging_lock);
    for (ExpressVkQueueInfo *it = g_express_vk_queues; it != NULL; it = it->next) {
        if (it->queue == queue) {
            *out_info = *it;
            found = true;
            break;
        }
    }
    g_mutex_unlock(&g_express_vk_staging_lock);
    return found;
}

static ExpressVkRegisteredMemory *express_vk_find_registered_memory(
    uint64_t guest_memory,
    uint64_t host_memory);
static uint64_t express_vk_registered_range_size(ExpressVkRegisteredMemory *reg,
                                                 uint64_t offset,
                                                 uint64_t requested);
static size_t express_vk_descriptor_template_item_size(VkDescriptorType descriptor_type)
{
    switch (descriptor_type) {
    case VK_DESCRIPTOR_TYPE_SAMPLER:
    case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
    case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
    case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
    case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
        return sizeof(VkDescriptorImageInfo);
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
        return sizeof(VkDescriptorBufferInfo);
    case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
    case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
        return sizeof(VkBufferView);
    case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
        return sizeof(VkAccelerationStructureKHR);
    default:
        return 0;
    }
}

static bool express_vk_descriptor_template_item_in_bounds(
    const VkDescriptorUpdateTemplateEntry *entry,
    uint32_t index,
    size_t item_size,
    size_t data_size,
    size_t *out_offset)
{
    if (out_offset) {
        *out_offset = 0;
    }
    if (entry == NULL || item_size == 0 || index >= entry->descriptorCount) {
        return false;
    }
    size_t stride = entry->stride != 0 ? entry->stride : item_size;
    if (index != 0 && stride > (SIZE_MAX - entry->offset) / index) {
        return false;
    }
    size_t offset = entry->offset + (size_t)index * stride;
    if (offset > SIZE_MAX - item_size || offset + item_size > data_size) {
        return false;
    }
    if (out_offset) {
        *out_offset = offset;
    }
    return true;
}

typedef struct ExpressVkHostHotPathStats {
    uint64_t create_pool_calls;
    uint64_t alloc_set_calls;
    uint64_t update_set_calls;
    uint64_t update_template_calls;
    uint64_t bind_set_calls;
    uint64_t queue_submit_calls;
    uint64_t create_pool_us;
    uint64_t alloc_set_us;
    uint64_t update_set_us;
    uint64_t update_template_us;
    uint64_t bind_set_us;
    uint64_t queue_submit_prep_us;
    uint64_t queue_submit_gpu_us;
    uint64_t queue_submit_total_us;
} ExpressVkHostHotPathStats;

static GMutex g_express_vk_host_hot_stats_lock;
static ExpressVkHostHotPathStats g_express_vk_host_hot_stats;
static const uint64_t kExpressVkHostHotStatsLogEvery = 512;

static uint64_t express_vk_host_avg_us(uint64_t total, uint64_t count)
{
    return count ? total / count : 0;
}

static void express_vk_host_hot_stats_maybe_log_locked(const char *reason)
{
    if (!EXPRESS_VK_ENABLE_HOST_HOT_SUMMARY_LOG) {
        (void)reason;
        return;
    }
    const ExpressVkHostHotPathStats *s = &g_express_vk_host_hot_stats;
    uint64_t activity = s->create_pool_calls + s->alloc_set_calls +
                        s->update_set_calls + s->update_template_calls +
                        s->bind_set_calls + s->queue_submit_calls;
    if (activity == 0) {
        return;
    }
    if (reason == NULL &&
        (kExpressVkHostHotStatsLogEvery == 0 ||
         activity % kExpressVkHostHotStatsLogEvery != 0)) {
        return;
    }

    LOGD("[HOST_HOT_SUMMARY] reason=%s create_pool=%llu alloc_set=%llu update_set=%llu "
         "update_template=%llu bind_set=%llu queue_submit=%llu "
         "time_us create=%llu alloc=%llu update=%llu template=%llu bind=%llu "
         "qs_prep=%llu qs_gpu=%llu qs_total=%llu "
         "avg_us create=%llu alloc=%llu update=%llu template=%llu bind=%llu "
         "qs_prep=%llu qs_gpu=%llu qs_total=%llu",
         reason ? reason : "periodic",
         (unsigned long long)s->create_pool_calls,
         (unsigned long long)s->alloc_set_calls,
         (unsigned long long)s->update_set_calls,
         (unsigned long long)s->update_template_calls,
         (unsigned long long)s->bind_set_calls,
         (unsigned long long)s->queue_submit_calls,
         (unsigned long long)s->create_pool_us,
         (unsigned long long)s->alloc_set_us,
         (unsigned long long)s->update_set_us,
         (unsigned long long)s->update_template_us,
         (unsigned long long)s->bind_set_us,
         (unsigned long long)s->queue_submit_prep_us,
         (unsigned long long)s->queue_submit_gpu_us,
         (unsigned long long)s->queue_submit_total_us,
         (unsigned long long)express_vk_host_avg_us(s->create_pool_us, s->create_pool_calls),
         (unsigned long long)express_vk_host_avg_us(s->alloc_set_us, s->alloc_set_calls),
         (unsigned long long)express_vk_host_avg_us(s->update_set_us, s->update_set_calls),
         (unsigned long long)express_vk_host_avg_us(s->update_template_us, s->update_template_calls),
         (unsigned long long)express_vk_host_avg_us(s->bind_set_us, s->bind_set_calls),
         (unsigned long long)express_vk_host_avg_us(s->queue_submit_prep_us, s->queue_submit_calls),
         (unsigned long long)express_vk_host_avg_us(s->queue_submit_gpu_us, s->queue_submit_calls),
         (unsigned long long)express_vk_host_avg_us(s->queue_submit_total_us, s->queue_submit_calls));
}

static void express_vk_host_note_descriptor_timing(int kind, uint64_t elapsed_us)
{
    g_mutex_lock(&g_express_vk_host_hot_stats_lock);
    ExpressVkHostHotPathStats *s = &g_express_vk_host_hot_stats;
    switch (kind) {
    case 0: s->create_pool_calls++; s->create_pool_us += elapsed_us; break;
    case 1: s->alloc_set_calls++; s->alloc_set_us += elapsed_us; break;
    case 2: s->update_set_calls++; s->update_set_us += elapsed_us; break;
    case 3: s->update_template_calls++; s->update_template_us += elapsed_us; break;
    case 4: s->bind_set_calls++; s->bind_set_us += elapsed_us; break;
    default: break;
    }
    express_vk_host_hot_stats_maybe_log_locked(NULL);
    g_mutex_unlock(&g_express_vk_host_hot_stats_lock);
}

/*
 * Routed writes must pass through the same host mirrors as ordinary
 * vkUpdateDescriptorSets before the driver sees them.  The returned interval
 * is host-owned realization time used by FLIME profiling.
 */
static uint64_t express_vk_flime_realize_descriptor_writes(
    VkDevice device, uint32_t write_count,
    const VkWriteDescriptorSet *writes)
{
    uint64_t start_ns = (uint64_t)g_get_monotonic_time() * 1000;

    if (write_count == 0) {
        return 0;
    }
#if EXPRESS_VK_ENABLE_DESCRIPTOR_TRACE
    LOGD("[HOST_DESC][FLIME] realizing %u routed descriptor writes",
         write_count);
#endif
    vkUpdateDescriptorSets(device, write_count, writes, 0, NULL);

    uint64_t elapsed_ns = (uint64_t)g_get_monotonic_time() * 1000 - start_ns;
    express_vk_host_note_descriptor_timing(2, elapsed_ns / 1000);
    return elapsed_ns;
}

static void express_vk_host_note_queue_submit_timing(uint64_t prep_us,
                                                     uint64_t gpu_us,
                                                     uint64_t total_us)
{
    g_mutex_lock(&g_express_vk_host_hot_stats_lock);
    ExpressVkHostHotPathStats *s = &g_express_vk_host_hot_stats;
    s->queue_submit_calls++;
    s->queue_submit_prep_us += prep_us;
    s->queue_submit_gpu_us += gpu_us;
    s->queue_submit_total_us += total_us;
    express_vk_host_hot_stats_maybe_log_locked(NULL);
    g_mutex_unlock(&g_express_vk_host_hot_stats_lock);
}

typedef struct ExpressVkHostWaitInvalidateFusedStats {
    uint64_t calls;
    uint64_t ranges;
    uint64_t bytes;
    uint64_t wait_us;
    uint64_t copy_us;
    uint64_t total_us;
    uint64_t failed;
} ExpressVkHostWaitInvalidateFusedStats;

static GMutex g_express_vk_host_wait_invalidate_fused_lock;
static ExpressVkHostWaitInvalidateFusedStats g_express_vk_host_wait_invalidate_fused_stats;
static const uint64_t kExpressVkHostWaitInvalidateFusedLogEvery = 256;

static void express_vk_host_wait_invalidate_fused_note(uint64_t ranges,
                                                       uint64_t bytes,
                                                       uint64_t wait_us,
                                                       uint64_t copy_us,
                                                       uint64_t total_us,
                                                       int failed)
{
    g_mutex_lock(&g_express_vk_host_wait_invalidate_fused_lock);
    ExpressVkHostWaitInvalidateFusedStats *s =
        &g_express_vk_host_wait_invalidate_fused_stats;
    s->calls++;
    s->ranges += ranges;
    s->bytes += bytes;
    s->wait_us += wait_us;
    s->copy_us += copy_us;
    s->total_us += total_us;
    if (failed) {
        s->failed++;
    }
    if (EXPRESS_VK_ENABLE_SYNC_DIAG_LOG &&
        s->calls != 0 &&
        (s->calls % kExpressVkHostWaitInvalidateFusedLogEvery) == 0) {
        LOGD("[HOST_WAIT_INVALIDATE_FUSED] calls=%llu ranges=%llu bytes_mb=%llu wait_us=%llu copy_us=%llu total_us=%llu failed=%llu",
             (unsigned long long)s->calls,
             (unsigned long long)s->ranges,
             (unsigned long long)(s->bytes / (1024ULL * 1024ULL)),
             (unsigned long long)s->wait_us,
             (unsigned long long)s->copy_us,
             (unsigned long long)s->total_us,
             (unsigned long long)s->failed);
    }
    g_mutex_unlock(&g_express_vk_host_wait_invalidate_fused_lock);
}

/* Conservative full-range coherency path; FLIME forwarding uses its planner. */
static uint64_t express_vk_copy_host_to_guest(ExpressVkRegisteredMemory *reg,
                                              void *host_ptr,
                                              uint64_t offset,
                                              uint64_t size)
{
    if (reg == NULL || reg->guest_mem == NULL || host_ptr == NULL) return 0;
    size = express_vk_registered_range_size(reg, offset, size);
    if (size == 0) return 0;
    write_to_guest_mem(reg->guest_mem, (char *)host_ptr + offset, offset, size);
    return size;
}

static void express_vk_transfer_worker(gpointer data, gpointer user_data)
{
    (void)user_data;
    ExpressVkTransferTask *task = (ExpressVkTransferTask *)data;
    int failed = 0;

    if (task == NULL) {
        LOGE("[ExpressVkTransfer] worker received null task");
        return;
    }

    task->start_us = g_get_real_time();
    if (task->reg == NULL || task->reg->guest_mem == NULL ||
        task->host_ptr == NULL || task->size == 0) {
        failed = 1;
        goto EXIT;
    }

    if (task->direction == EXPRESS_VK_TRANSFER_UPLOAD) {
        read_from_guest_mem(
            task->reg->guest_mem,
            (char *)task->host_ptr + task->offset,
            task->offset,
            task->size);

        if (task->device != VK_NULL_HANDLE && task->host_memory != 0) {
            VkMappedMemoryRange range;
            memset(&range, 0, sizeof(range));
            range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
            range.memory = (VkDeviceMemory)(uintptr_t)task->host_memory;
            range.offset = task->offset;
            range.size = task->size;

            VkResult flush_result = vkFlushMappedMemoryRanges(task->device, 1, &range);
            if (flush_result != VK_SUCCESS) {
                LOGE("[ExpressVkTransfer] worker upload flush failed seq=%llu result=%d",
                     (unsigned long long)task->seq,
                     flush_result);
                failed = 1;
            }
        }
    } else {
        if (task->device != VK_NULL_HANDLE && task->host_memory != 0) {
            VkMappedMemoryRange range;
            memset(&range, 0, sizeof(range));
            range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
            range.memory = (VkDeviceMemory)(uintptr_t)task->host_memory;
            range.offset = task->offset;
            range.size = task->size;

            VkResult invalidate_result = vkInvalidateMappedMemoryRanges(task->device, 1, &range);
            if (invalidate_result != VK_SUCCESS) {
                LOGE("[ExpressVkTransfer] worker download invalidate failed seq=%llu result=%d",
                     (unsigned long long)task->seq,
                     invalidate_result);
                failed = 1;
            }
        }

        write_to_guest_mem(
            task->reg->guest_mem,
            (char *)task->host_ptr + task->offset,
            task->offset,
            task->size);
    }

EXIT:
    task->end_us = g_get_real_time();

    LOGD("[ExpressVkTransfer] done seq=%llu dir=%s bytes=%llu failed=%d worker_ms=%.3f queued_ms=%.3f",
         (unsigned long long)task->seq,
         express_vk_transfer_direction_name(task->direction),
         (unsigned long long)task->size,
         failed,
         express_vk_elapsed_ms(task->start_us, task->end_us),
         express_vk_elapsed_ms(task->enqueue_us, task->end_us));
    if (task->direction == EXPRESS_VK_TRANSFER_UPLOAD) {
        express_vk_stats_add_u64(&g_express_vk_stats.upload_completed, 1);
    } else {
        express_vk_stats_add_u64(&g_express_vk_stats.download_completed, 1);
    }

    g_mutex_lock(&g_express_vk_transfer_lock);
    task->failed = failed;
    task->done = 1;
    g_cond_broadcast(&task->done_cond);
    g_mutex_unlock(&g_express_vk_transfer_lock);
}

static void express_vk_transfer_init_once(void)
{
    if (g_once_init_enter(&g_express_vk_transfer_once)) {
        g_mutex_init(&g_express_vk_transfer_lock);
        g_express_vk_transfer_pool =
            g_thread_pool_new(express_vk_transfer_worker,
                              NULL,
                              EXPRESS_VK_TRANSFER_WORKERS,
                              false,
                              NULL);

        if (g_express_vk_transfer_pool == NULL) {
            LOGE("[ExpressVkTransfer] failed to create transfer pool");
        } else {
            LOGD("[ExpressVkTransfer] initialized workers=%d upload_threshold=%llu download_threshold=%llu",
                 EXPRESS_VK_TRANSFER_WORKERS,
                 (unsigned long long)EXPRESS_VK_TRANSFER_UPLOAD_THRESHOLD,
                 (unsigned long long)EXPRESS_VK_TRANSFER_DOWNLOAD_THRESHOLD);
        }
        g_once_init_leave(&g_express_vk_transfer_once, 1);
    }
}

static void express_vk_transfer_list_remove_locked(ExpressVkTransferTask *task)
{
    ExpressVkTransferTask **prev = &g_express_vk_transfer_tasks;
    ExpressVkTransferTask *it = g_express_vk_transfer_tasks;
    while (it != NULL) {
        if (it == task) {
            *prev = it->next;
            task->next = NULL;
            return;
        }
        prev = &it->next;
        it = it->next;
    }
}

static bool express_vk_transfer_matches(ExpressVkTransferTask *task,
                                        ExpressVkRegisteredMemory *reg,
                                        ExpressVkTransferDirection direction)
{
    if (task == NULL) {
        return false;
    }
    if (reg != NULL && task->reg != reg) {
        return false;
    }
    if (direction != EXPRESS_VK_TRANSFER_ANY && task->direction != direction) {
        return false;
    }
    return true;
}

static bool express_vk_ranges_overlap(uint64_t a_offset, uint64_t a_size,
                                      uint64_t b_offset, uint64_t b_size)
{
    if (a_size == 0 || b_size == 0) {
        return false;
    }
    uint64_t a_end = a_offset + a_size;
    uint64_t b_end = b_offset + b_size;
    if (a_end < a_offset) {
        a_end = UINT64_MAX;
    }
    if (b_end < b_offset) {
        b_end = UINT64_MAX;
    }
    return a_offset < b_end && b_offset < a_end;
}

static bool express_vk_range_covers(uint64_t outer_offset, uint64_t outer_size,
                                    uint64_t inner_offset, uint64_t inner_size)
{
    if (outer_size == 0 || inner_size == 0 || outer_offset > inner_offset) {
        return false;
    }
    uint64_t outer_end = outer_offset + outer_size;
    uint64_t inner_end = inner_offset + inner_size;
    if (outer_end < outer_offset) {
        outer_end = UINT64_MAX;
    }
    if (inner_end < inner_offset) {
        inner_end = UINT64_MAX;
    }
    return outer_end >= inner_end;
}

static uint32_t express_vk_wait_transfers_filtered(ExpressVkRegisteredMemory *reg,
                                                   ExpressVkTransferDirection direction,
                                                   bool use_range,
                                                   uint64_t offset,
                                                   uint64_t size,
                                                   bool require_cover,
                                                   const char *reason)
{
    express_vk_transfer_init_once();

    uint32_t waited_tasks = 0;
    uint64_t waited_bytes = 0;
    gint64 wait_begin_us = g_get_real_time();

    g_mutex_lock(&g_express_vk_transfer_lock);
    for (;;) {
        ExpressVkTransferTask *task = NULL;
        for (ExpressVkTransferTask *it = g_express_vk_transfer_tasks; it != NULL; it = it->next) {
            if (!express_vk_transfer_matches(it, reg, direction)) {
                continue;
            }
            if (use_range) {
                if (require_cover) {
                    if (!express_vk_range_covers(it->offset, it->size, offset, size)) {
                        continue;
                    }
                } else if (!express_vk_ranges_overlap(it->offset, it->size, offset, size)) {
                    continue;
                }
            }
            task = it;
            break;
        }

        if (task == NULL) {
            break;
        }

        gint64 one_wait_begin_us = g_get_real_time();
        while (!task->done) {
            g_cond_wait(&task->done_cond, &g_express_vk_transfer_lock);
        }

        express_vk_transfer_list_remove_locked(task);
        waited_tasks++;
        waited_bytes += task->size;

        LOGD("[ExpressVkTransfer] wait_one reason=%s seq=%llu dir=%s bytes=%llu failed=%d wait_ms=%.3f",
             reason ? reason : "unknown",
             (unsigned long long)task->seq,
             express_vk_transfer_direction_name(task->direction),
             (unsigned long long)task->size,
             task->failed,
             express_vk_elapsed_ms(one_wait_begin_us, g_get_real_time()));

        g_cond_clear(&task->done_cond);
        g_free(task);
    }
    g_mutex_unlock(&g_express_vk_transfer_lock);

    if (waited_tasks != 0) {
        LOGD("[ExpressVkTransfer] wait_done reason=%s tasks=%u bytes=%llu total_ms=%.3f",
             reason ? reason : "unknown",
             waited_tasks,
             (unsigned long long)waited_bytes,
             express_vk_elapsed_ms(wait_begin_us, g_get_real_time()));
        express_vk_stats_add_u64(&g_express_vk_stats.wait_tasks, waited_tasks);
        express_vk_stats_add_u64(&g_express_vk_stats.wait_bytes, waited_bytes);
    }
    return waited_tasks;
}

static uint32_t express_vk_wait_transfers(ExpressVkRegisteredMemory *reg,
                                          ExpressVkTransferDirection direction,
                                          const char *reason)
{
    return express_vk_wait_transfers_filtered(
        reg, direction, false, 0, 0, false, reason);
}

static uint32_t express_vk_wait_transfers_range(ExpressVkRegisteredMemory *reg,
                                                ExpressVkTransferDirection direction,
                                                uint64_t offset,
                                                uint64_t size,
                                                const char *reason)
{
    return express_vk_wait_transfers_filtered(
        reg, direction, true, offset, size, false, reason);
}

static uint32_t express_vk_wait_covering_transfer(ExpressVkRegisteredMemory *reg,
                                                  ExpressVkTransferDirection direction,
                                                  uint64_t offset,
                                                  uint64_t size,
                                                  const char *reason)
{
    return express_vk_wait_transfers_filtered(
        reg, direction, true, offset, size, true, reason);
}

static bool express_vk_submit_transfer(ExpressVkTransferDirection direction,
                                       ExpressVkRegisteredMemory *reg,
                                       VkDevice device,
                                       uint64_t host_memory,
                                       void *host_ptr,
                                       uint64_t offset,
                                       uint64_t size)
{
    express_vk_transfer_init_once();

    if (g_express_vk_transfer_pool == NULL || reg == NULL || reg->guest_mem == NULL ||
        host_ptr == NULL || size == 0) {
        return false;
    }

    ExpressVkTransferTask *task = g_malloc0(sizeof(*task));
    task->direction = direction;
    task->reg = reg;
    task->device = device;
    task->host_memory = host_memory;
    task->host_ptr = host_ptr;
    task->offset = offset;
    task->size = size;
    task->enqueue_us = g_get_real_time();
    g_cond_init(&task->done_cond);

    g_mutex_lock(&g_express_vk_transfer_lock);
    task->seq = ++g_express_vk_transfer_seq;
    task->next = g_express_vk_transfer_tasks;
    g_express_vk_transfer_tasks = task;
    g_mutex_unlock(&g_express_vk_transfer_lock);

    GError *error = NULL;
    if (!g_thread_pool_push(g_express_vk_transfer_pool, task, &error)) {
        g_mutex_lock(&g_express_vk_transfer_lock);
        express_vk_transfer_list_remove_locked(task);
        g_mutex_unlock(&g_express_vk_transfer_lock);

        LOGE("[ExpressVkTransfer] enqueue failed dir=%s bytes=%llu error=%s",
             express_vk_transfer_direction_name(direction),
             (unsigned long long)size,
             error ? error->message : "unknown");
        if (error) {
            g_error_free(error);
        }
        g_cond_clear(&task->done_cond);
        g_free(task);
        return false;
    }

    LOGD("[ExpressVkTransfer] enqueue seq=%llu dir=%s bytes=%llu offset=%llu guest=0x%llx host=0x%llx",
         (unsigned long long)task->seq,
         express_vk_transfer_direction_name(direction),
         (unsigned long long)size,
         (unsigned long long)offset,
         (unsigned long long)reg->guest_memory,
         (unsigned long long)host_memory);
    if (direction == EXPRESS_VK_TRANSFER_UPLOAD) {
        express_vk_stats_add_u64(&g_express_vk_stats.upload_enqueued, 1);
    } else {
        express_vk_stats_add_u64(&g_express_vk_stats.download_enqueued, 1);
    }
    return true;
}

static ExpressVkRegisteredMemory *express_vk_find_registered_memory(uint64_t guest_memory, uint64_t host_memory)
{
    for (ExpressVkRegisteredMemory *it = g_express_vk_registered_memories; it != NULL; it = it->next) {
        if ((guest_memory != 0 && it->guest_memory == guest_memory) ||
            (host_memory != 0 && it->host_memory == host_memory)) {
            return it;
        }
        if (host_memory != 0 && it->host_memory == 0 && it->guest_memory != 0) {
            uint64_t resolved_host_memory =
                lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE_MEMORY, it->guest_memory);
            if (resolved_host_memory == host_memory) {
                it->host_memory = resolved_host_memory;
                return it;
            }
        }
    }
    return NULL;
}

static uint64_t express_vk_registered_range_size(ExpressVkRegisteredMemory *reg, uint64_t offset, uint64_t requested)
{
    if (reg == NULL || offset >= reg->size) {
        return 0;
    }
    uint64_t available = reg->size - offset;
    if (requested == VK_WHOLE_SIZE || requested > available) {
        return available;
    }
    return requested;
}

static ExpressVkSubmitRangeHint express_vk_resolve_submit_range_hint(
    const ExpressVkSubmitRangeHintWire *wire,
    VkDevice device)
{
    ExpressVkSubmitRangeHint hint;
    memset(&hint, 0, sizeof(hint));
    if (wire == NULL || wire->memory == 0 || wire->size == 0) {
        return hint;
    }

    if (wire->device != 0) {
        hint.device = (VkDevice)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, wire->device);
    }
    if (hint.device == VK_NULL_HANDLE) {
        hint.device = device;
    }
    hint.guest_memory = wire->memory;
    hint.offset = wire->offset;
    hint.size = wire->size;
    hint.reg = express_vk_find_registered_memory(wire->memory, 0);
    if (hint.reg != NULL) {
        hint.host_memory = hint.reg->host_memory;
    }
    if (hint.host_memory == 0) {
        hint.host_memory = lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE_MEMORY, wire->memory);
        if (hint.reg == NULL && hint.host_memory != 0) {
            hint.reg = express_vk_find_registered_memory(0, hint.host_memory);
        }
    }
    if (hint.reg != NULL && hint.host_memory == 0) {
        hint.host_memory = hint.reg->host_memory;
    }
    hint.host_ptr = hint.host_memory ? get_memory_map(hint.host_memory) : NULL;
    if (hint.reg != NULL) {
        hint.size = express_vk_registered_range_size(hint.reg, hint.offset, hint.size);
    }
    return hint;
}

static void express_vk_free_submit_hints(ExpressVkSubmitHints *hints)
{
    if (hints == NULL) {
        return;
    }
    if (hints->flush_ranges) {
        free(hints->flush_ranges);
    }
    if (hints->output_ranges) {
        free(hints->output_ranges);
    }
    memset(hints, 0, sizeof(*hints));
}

typedef struct ExpressVkDecodedPNextBase {
    VkStructureType sType;
    const void *pNext;
} ExpressVkDecodedPNextBase;

/*
 * decode_from_stream_VkSubmitInfo() owns every allocation below.  Keep its
 * ownership explicit here: generated decoders intentionally rebuild pNext and
 * pointer arrays rather than retaining guest addresses.
 */
static void express_vk_free_decoded_submit_pnext(const void *pnext)
{
    while (pnext != NULL) {
        const ExpressVkDecodedPNextBase *base =
            (const ExpressVkDecodedPNextBase *)pnext;
        const void *next = base->pNext;

        switch (base->sType) {
#ifdef VK_VERSION_1_1
        case VK_STRUCTURE_TYPE_DEVICE_GROUP_SUBMIT_INFO: {
            const VkDeviceGroupSubmitInfo *info =
                (const VkDeviceGroupSubmitInfo *)base;
            free((void *)info->pWaitSemaphoreDeviceIndices);
            free((void *)info->pCommandBufferDeviceMasks);
            free((void *)info->pSignalSemaphoreDeviceIndices);
            break;
        }
        case VK_STRUCTURE_TYPE_PROTECTED_SUBMIT_INFO:
            break;
#endif
#ifdef VK_VERSION_1_2
        case VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO: {
            const VkTimelineSemaphoreSubmitInfo *info =
                (const VkTimelineSemaphoreSubmitInfo *)base;
            free((void *)info->pWaitSemaphoreValues);
            free((void *)info->pSignalSemaphoreValues);
            break;
        }
#endif
        default:
            /*
             * This generated protocol advertises only the three VkSubmitInfo
             * extensions above.  Validation rejects any other sType before
             * Vulkan sees it; the base allocation is still safe to release.
             */
            break;
        }

        free((void *)base);
        pnext = next;
    }
}

static bool express_vk_validate_decoded_submit(const VkSubmitInfo *submit)
{
    const ExpressVkDecodedPNextBase *base;

    if (submit == NULL ||
        submit->sType != VK_STRUCTURE_TYPE_SUBMIT_INFO ||
        (submit->waitSemaphoreCount != 0 &&
         (submit->pWaitSemaphores == NULL ||
          submit->pWaitDstStageMask == NULL)) ||
        (submit->commandBufferCount != 0 &&
         submit->pCommandBuffers == NULL) ||
        (submit->signalSemaphoreCount != 0 &&
         submit->pSignalSemaphores == NULL)) {
        return false;
    }

    for (uint32_t i = 0; i < submit->waitSemaphoreCount; ++i) {
        if (submit->pWaitSemaphores[i] == VK_NULL_HANDLE) {
            return false;
        }
    }
    for (uint32_t i = 0; i < submit->commandBufferCount; ++i) {
        if (submit->pCommandBuffers[i] == VK_NULL_HANDLE) {
            return false;
        }
    }
    for (uint32_t i = 0; i < submit->signalSemaphoreCount; ++i) {
        if (submit->pSignalSemaphores[i] == VK_NULL_HANDLE) {
            return false;
        }
    }

    base = (const ExpressVkDecodedPNextBase *)submit->pNext;
    while (base != NULL) {
        switch (base->sType) {
#ifdef VK_VERSION_1_1
        case VK_STRUCTURE_TYPE_DEVICE_GROUP_SUBMIT_INFO: {
            const VkDeviceGroupSubmitInfo *info =
                (const VkDeviceGroupSubmitInfo *)base;
            if (info->waitSemaphoreCount != submit->waitSemaphoreCount ||
                info->commandBufferCount != submit->commandBufferCount ||
                info->signalSemaphoreCount != submit->signalSemaphoreCount ||
                (info->waitSemaphoreCount != 0 &&
                 info->pWaitSemaphoreDeviceIndices == NULL) ||
                (info->commandBufferCount != 0 &&
                 info->pCommandBufferDeviceMasks == NULL) ||
                (info->signalSemaphoreCount != 0 &&
                 info->pSignalSemaphoreDeviceIndices == NULL)) {
                return false;
            }
            break;
        }
        case VK_STRUCTURE_TYPE_PROTECTED_SUBMIT_INFO:
            break;
#endif
#ifdef VK_VERSION_1_2
        case VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO: {
            const VkTimelineSemaphoreSubmitInfo *info =
                (const VkTimelineSemaphoreSubmitInfo *)base;
            if (info->waitSemaphoreValueCount != submit->waitSemaphoreCount ||
                info->signalSemaphoreValueCount != submit->signalSemaphoreCount ||
                (info->waitSemaphoreValueCount != 0 &&
                 info->pWaitSemaphoreValues == NULL) ||
                (info->signalSemaphoreValueCount != 0 &&
                 info->pSignalSemaphoreValues == NULL)) {
                return false;
            }
            break;
        }
#endif
        default:
            return false;
        }
        base = (const ExpressVkDecodedPNextBase *)base->pNext;
    }

    return true;
}

static void express_vk_free_decoded_submit_infos(VkSubmitInfo *submits,
                                                  uint32_t count)
{
    if (submits == NULL) {
        return;
    }
    for (uint32_t i = 0; i < count; ++i) {
        free((void *)submits[i].pWaitSemaphores);
        free((void *)submits[i].pWaitDstStageMask);
        free((void *)submits[i].pCommandBuffers);
        free((void *)submits[i].pSignalSemaphores);
        express_vk_free_decoded_submit_pnext(submits[i].pNext);
    }
    g_free(submits);
}

static bool express_vk_submit2_take(uint8_t **cursor, const uint8_t *end,
                                    void *value, size_t value_size)
{
    if (cursor == NULL || *cursor == NULL || end == NULL ||
        (value == NULL && value_size != 0) || *cursor > end ||
        value_size > (size_t)(end - *cursor)) {
        return false;
    }
    memcpy(value, *cursor, value_size);
    *cursor += value_size;
    return true;
}

static bool express_vk_decode_semaphore_submit_infos2(
    uint8_t **cursor, const uint8_t *end, uint32_t count,
    const VkSemaphoreSubmitInfo **infos_out)
{
    const size_t wire_size = sizeof(VkStructureType) + sizeof(uint32_t) +
        sizeof(uint64_t) + sizeof(uint64_t) +
        sizeof(VkPipelineStageFlags2) + sizeof(uint32_t);
    VkSemaphoreSubmitInfo *infos = NULL;

    if (infos_out == NULL) {
        return false;
    }
    *infos_out = NULL;
    if (cursor == NULL || *cursor == NULL || end == NULL || *cursor > end) {
        return false;
    }
    if (count == 0) {
        return true;
    }
    if (*cursor > end || count > (size_t)(end - *cursor) / wire_size) {
        return false;
    }
    infos = g_try_new0(VkSemaphoreSubmitInfo, count);
    if (infos == NULL) {
        return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t pnext_size;
        uint64_t guest_semaphore;

        if (!express_vk_submit2_take(cursor, end, &infos[i].sType,
                                    sizeof(infos[i].sType)) ||
            !express_vk_submit2_take(cursor, end, &pnext_size,
                                    sizeof(pnext_size)) ||
            infos[i].sType != VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO ||
            pnext_size != 0 ||
            !express_vk_submit2_take(cursor, end, &guest_semaphore,
                                    sizeof(guest_semaphore)) ||
            !express_vk_submit2_take(cursor, end, &infos[i].value,
                                    sizeof(infos[i].value)) ||
            !express_vk_submit2_take(cursor, end, &infos[i].stageMask,
                                    sizeof(infos[i].stageMask)) ||
            !express_vk_submit2_take(cursor, end, &infos[i].deviceIndex,
                                    sizeof(infos[i].deviceIndex))) {
            g_free(infos);
            return false;
        }
        infos[i].pNext = NULL;
        infos[i].semaphore = (VkSemaphore)(uintptr_t)lookup_mapping(
            EXPRESS_VK_OBJECT_TYPE_SEMAPHORE, guest_semaphore);
        if (guest_semaphore == 0 || infos[i].semaphore == VK_NULL_HANDLE) {
            g_free(infos);
            return false;
        }
    }
    *infos_out = infos;
    return true;
}

static bool express_vk_decode_command_buffer_submit_infos2(
    uint8_t **cursor, const uint8_t *end, uint32_t count,
    const VkCommandBufferSubmitInfo **infos_out)
{
    const size_t wire_size = sizeof(VkStructureType) + sizeof(uint32_t) +
        sizeof(uint64_t) + sizeof(uint32_t);
    VkCommandBufferSubmitInfo *infos = NULL;

    if (infos_out == NULL) {
        return false;
    }
    *infos_out = NULL;
    if (cursor == NULL || *cursor == NULL || end == NULL || *cursor > end) {
        return false;
    }
    if (count == 0) {
        return true;
    }
    if (*cursor > end || count > (size_t)(end - *cursor) / wire_size) {
        return false;
    }
    infos = g_try_new0(VkCommandBufferSubmitInfo, count);
    if (infos == NULL) {
        return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t pnext_size;
        uint64_t guest_command_buffer;

        if (!express_vk_submit2_take(cursor, end, &infos[i].sType,
                                    sizeof(infos[i].sType)) ||
            !express_vk_submit2_take(cursor, end, &pnext_size,
                                    sizeof(pnext_size)) ||
            infos[i].sType != VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO ||
            pnext_size != 0 ||
            !express_vk_submit2_take(cursor, end, &guest_command_buffer,
                                    sizeof(guest_command_buffer)) ||
            !express_vk_submit2_take(cursor, end, &infos[i].deviceMask,
                                    sizeof(infos[i].deviceMask))) {
            g_free(infos);
            return false;
        }
        infos[i].pNext = NULL;
        infos[i].commandBuffer = (VkCommandBuffer)(uintptr_t)lookup_mapping(
            EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_command_buffer);
        if (guest_command_buffer == 0 ||
            infos[i].commandBuffer == VK_NULL_HANDLE) {
            g_free(infos);
            return false;
        }
    }
    *infos_out = infos;
    return true;
}

static void express_vk_free_decoded_submit_infos2(VkSubmitInfo2 *submits,
                                                   uint32_t count)
{
    if (submits == NULL) {
        return;
    }
    for (uint32_t i = 0; i < count; ++i) {
        g_free((void *)submits[i].pWaitSemaphoreInfos);
        g_free((void *)submits[i].pCommandBufferInfos);
        g_free((void *)submits[i].pSignalSemaphoreInfos);
    }
    g_free(submits);
}

/*
 * The generated VkSubmitInfo2 decoder assumes a trusted stream: it advances
 * without an end pointer and allocates from guest-provided counts.  Queue
 * submission is a trust boundary, so decode the canonical no-pNext form here.
 * Unknown top-level or nested extension chains are rejected before Vulkan sees
 * them; this also keeps ownership of every decoded allocation explicit.
 */
static bool express_vk_decode_submit_infos2(uint8_t **cursor,
                                            const uint8_t *end,
                                            uint32_t count,
                                            VkSubmitInfo2 **submits_out)
{
    const size_t fixed_wire_size =
        sizeof(VkStructureType) + sizeof(uint32_t) +
        sizeof(VkSubmitFlags) + 3 * sizeof(uint32_t);
    VkSubmitInfo2 *submits = NULL;

    if (submits_out == NULL) {
        return false;
    }
    *submits_out = NULL;
    if (cursor == NULL || *cursor == NULL || end == NULL || *cursor > end) {
        return false;
    }
    if (count == 0) {
        return true;
    }
    if (count > (size_t)(end - *cursor) / fixed_wire_size) {
        return false;
    }

    submits = g_try_new0(VkSubmitInfo2, count);
    if (submits == NULL) {
        return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t pnext_size;

        if (!express_vk_submit2_take(cursor, end, &submits[i].sType,
                                    sizeof(submits[i].sType)) ||
            !express_vk_submit2_take(cursor, end, &pnext_size,
                                    sizeof(pnext_size)) ||
            submits[i].sType != VK_STRUCTURE_TYPE_SUBMIT_INFO_2 ||
            pnext_size != 0 ||
            !express_vk_submit2_take(cursor, end, &submits[i].flags,
                                    sizeof(submits[i].flags)) ||
            !express_vk_submit2_take(
                cursor, end, &submits[i].waitSemaphoreInfoCount,
                sizeof(submits[i].waitSemaphoreInfoCount)) ||
            !express_vk_decode_semaphore_submit_infos2(
                cursor, end, submits[i].waitSemaphoreInfoCount,
                &submits[i].pWaitSemaphoreInfos) ||
            !express_vk_submit2_take(
                cursor, end, &submits[i].commandBufferInfoCount,
                sizeof(submits[i].commandBufferInfoCount)) ||
            !express_vk_decode_command_buffer_submit_infos2(
                cursor, end, submits[i].commandBufferInfoCount,
                &submits[i].pCommandBufferInfos) ||
            !express_vk_submit2_take(
                cursor, end, &submits[i].signalSemaphoreInfoCount,
                sizeof(submits[i].signalSemaphoreInfoCount)) ||
            !express_vk_decode_semaphore_submit_infos2(
                cursor, end, submits[i].signalSemaphoreInfoCount,
                &submits[i].pSignalSemaphoreInfos)) {
            express_vk_free_decoded_submit_infos2(submits, count);
            return false;
        }
        submits[i].pNext = NULL;
    }

    *submits_out = submits;
    return true;
}

static bool express_vk_parse_submit_hints(char *cursor,
                                          char *end,
                                          VkDevice device,
                                          ExpressVkSubmitHints *hints)
{
    if (hints == NULL) {
        return false;
    }
    memset(hints, 0, sizeof(*hints));
    if (cursor == NULL || end == NULL || end - cursor < (ptrdiff_t)(sizeof(uint32_t) * 4)) {
        return false;
    }

    uint32_t magic = *(uint32_t *)cursor;
    cursor += sizeof(uint32_t);
    uint32_t version = *(uint32_t *)cursor;
    cursor += sizeof(uint32_t);
    uint32_t flush_count = *(uint32_t *)cursor;
    cursor += sizeof(uint32_t);
    uint32_t output_count = *(uint32_t *)cursor;
    cursor += sizeof(uint32_t);

    if (magic != EXPRESS_VK_SUBMIT_HINTS_MAGIC || version != EXPRESS_VK_SUBMIT_HINTS_VERSION) {
        return false;
    }

    uint64_t total_count = (uint64_t)flush_count + (uint64_t)output_count;
    uint64_t need_bytes = total_count * sizeof(ExpressVkSubmitRangeHintWire);
    if (total_count > UINT32_MAX || (uint64_t)(end - cursor) < need_bytes) {
        LOGE("[ExpressVkHints] malformed submit hints flush=%u output=%u bytes_left=%lld",
             flush_count,
             output_count,
             (long long)(end - cursor));
        return false;
    }

    hints->present = true;
    hints->flush_count = flush_count;
    hints->output_count = output_count;
    if (flush_count != 0) {
        hints->flush_ranges = (ExpressVkSubmitRangeHint *)calloc(flush_count, sizeof(ExpressVkSubmitRangeHint));
    }
    if (output_count != 0) {
        hints->output_ranges = (ExpressVkSubmitRangeHint *)calloc(output_count, sizeof(ExpressVkSubmitRangeHint));
    }
    if ((flush_count != 0 && hints->flush_ranges == NULL) ||
        (output_count != 0 && hints->output_ranges == NULL)) {
        express_vk_free_submit_hints(hints);
        return false;
    }

    for (uint32_t i = 0; i < flush_count; ++i) {
        ExpressVkSubmitRangeHintWire wire;
        memcpy(&wire, cursor, sizeof(wire));
        cursor += sizeof(wire);
        hints->flush_ranges[i] = express_vk_resolve_submit_range_hint(&wire, device);
    }
    for (uint32_t i = 0; i < output_count; ++i) {
        ExpressVkSubmitRangeHintWire wire;
        memcpy(&wire, cursor, sizeof(wire));
        cursor += sizeof(wire);
        hints->output_ranges[i] = express_vk_resolve_submit_range_hint(&wire, device);
    }

    if (flush_count != 0 || output_count != 0) {
        uint64_t flush_bytes = 0;
        uint32_t flush_resolved = 0;
        for (uint32_t i = 0; i < flush_count; ++i) {
            if (hints->flush_ranges[i].reg != NULL &&
                hints->flush_ranges[i].size != 0) {
                flush_resolved++;
                flush_bytes += hints->flush_ranges[i].size;
            }
        }
        uint64_t output_bytes = 0;
        uint32_t output_resolved = 0;
        for (uint32_t i = 0; i < output_count; ++i) {
            if (hints->output_ranges[i].reg != NULL &&
                hints->output_ranges[i].size != 0) {
                output_resolved++;
                output_bytes += hints->output_ranges[i].size;
            }
        }
        if (EXPRESS_VK_ENABLE_SYNC_DIAG_LOG) {
            LOGD("[ExpressVkHints] parsed submit hints flush=%u/%u flush_bytes=%llu output=%u/%u output_bytes=%llu",
                 flush_resolved,
                 flush_count,
                 (unsigned long long)flush_bytes,
                 output_resolved,
                 output_count,
                 (unsigned long long)output_bytes);
        }
        g_mutex_lock(&g_express_vk_stats_lock);
        g_express_vk_stats.hints_parsed++;
        g_express_vk_stats.hint_flush_ranges += flush_count;
        g_express_vk_stats.hint_output_ranges += output_count;
        g_express_vk_stats.events++;
        express_vk_stats_maybe_log_locked("hints");
        g_mutex_unlock(&g_express_vk_stats_lock);
    }
    return true;
}

static void express_vk_remove_fence_output_hints(VkFence fence)
{
    if (fence == VK_NULL_HANDLE) {
        return;
    }
    ExpressVkFenceOutputHints **prev = &g_express_vk_fence_output_hints;
    ExpressVkFenceOutputHints *it = g_express_vk_fence_output_hints;
    while (it != NULL) {
        if (it->fence == fence) {
            *prev = it->next;
            if (it->ranges) {
                free(it->ranges);
            }
            free(it);
            return;
        }
        prev = &it->next;
        it = it->next;
    }
}

static void express_vk_store_fence_output_hints(VkFence fence,
                                                uint64_t guest_fence,
                                                const ExpressVkSubmitHints *hints)
{
    if (fence == VK_NULL_HANDLE || hints == NULL || hints->output_count == 0 ||
        hints->output_ranges == NULL) {
        return;
    }

    express_vk_remove_fence_output_hints(fence);

    uint32_t usable_count = 0;
    for (uint32_t i = 0; i < hints->output_count; ++i) {
        const ExpressVkSubmitRangeHint *hint = &hints->output_ranges[i];
        if (hint->reg != NULL && hint->host_ptr != NULL && hint->size != 0 &&
            hint->size <= EXPRESS_VK_WAIT_COMMIT_DOWNLOAD_MAX_BYTES) {
            usable_count++;
        }
    }
    if (usable_count == 0) {
        return;
    }

    ExpressVkFenceOutputHints *entry =
        (ExpressVkFenceOutputHints *)calloc(1, sizeof(*entry));
    if (entry == NULL) {
        return;
    }
    entry->ranges = (ExpressVkSubmitRangeHint *)calloc(usable_count, sizeof(ExpressVkSubmitRangeHint));
    if (entry->ranges == NULL) {
        free(entry);
        return;
    }

    entry->fence = fence;
    entry->guest_fence = guest_fence;
    entry->range_count = usable_count;
    uint32_t out = 0;
    for (uint32_t i = 0; i < hints->output_count; ++i) {
        const ExpressVkSubmitRangeHint *hint = &hints->output_ranges[i];
        if (hint->reg != NULL && hint->host_ptr != NULL && hint->size != 0 &&
            hint->size <= EXPRESS_VK_WAIT_COMMIT_DOWNLOAD_MAX_BYTES) {
            entry->ranges[out++] = *hint;
        }
    }

    entry->next = g_express_vk_fence_output_hints;
    g_express_vk_fence_output_hints = entry;
    if (EXPRESS_VK_ENABLE_SYNC_DIAG_LOG) {
        LOGD("[ExpressVkOutputHints] stored fence output hints fence=%p guest=0x%llx ranges=%u",
             (void *)fence,
             (unsigned long long)guest_fence,
             usable_count);
    }
}

static void express_vk_wait_uploads_for_submit_hints(const ExpressVkSubmitHints *hints)
{
    if (hints == NULL || hints->flush_count == 0 || hints->flush_ranges == NULL) {
        if (hints == NULL || !hints->present) {
            uint32_t waited_all = express_vk_wait_transfers(
                NULL, EXPRESS_VK_TRANSFER_UPLOAD, "queue_submit_no_hints");
            if (EXPRESS_VK_ENABLE_SYNC_DIAG_LOG) {
                LOGD("[ExpressVkTransfer] submit upload wait policy=no_hints waited=%u",
                     waited_all);
            }
            express_vk_stats_add_u64(&g_express_vk_stats.submit_wait_all_uploads, 1);
        } else {
            if (EXPRESS_VK_ENABLE_SYNC_DIAG_LOG) {
                LOGD("[ExpressVkTransfer] submit upload wait policy=range hints=0 waited=0");
            }
#if EXPRESS_VK_STRICT_UPLOAD_WAIT_BEFORE_SUBMIT
            uint32_t waited_all = express_vk_wait_transfers(
                NULL, EXPRESS_VK_TRANSFER_UPLOAD, "queue_submit_strict_no_ranges");
            if (EXPRESS_VK_ENABLE_SYNC_DIAG_LOG) {
                LOGD("[ExpressVkTransfer] submit upload wait policy=strict_no_ranges waited=%u",
                     waited_all);
            }
            if (waited_all != 0) {
                express_vk_stats_add_u64(&g_express_vk_stats.submit_wait_all_uploads, 1);
            }
#endif
        }
        return;
    }

    uint32_t waited_ranges = 0;
    uint32_t valid_hints = 0;
    uint64_t hint_bytes = 0;
    for (uint32_t i = 0; i < hints->flush_count; ++i) {
        const ExpressVkSubmitRangeHint *hint = &hints->flush_ranges[i];
        if (hint->reg == NULL || hint->size == 0) {
            continue;
        }
        valid_hints++;
        hint_bytes += hint->size;
        waited_ranges += express_vk_wait_transfers_range(
            hint->reg,
            EXPRESS_VK_TRANSFER_UPLOAD,
            hint->offset,
            hint->size,
            "queue_submit_range");
    }

    if (EXPRESS_VK_ENABLE_SYNC_DIAG_LOG) {
        LOGD("[ExpressVkTransfer] submit upload wait policy=range hints=%u valid=%u hint_bytes=%llu waited=%u",
             hints->flush_count,
             valid_hints,
             (unsigned long long)hint_bytes,
             waited_ranges);
    }
#if EXPRESS_VK_STRICT_UPLOAD_WAIT_BEFORE_SUBMIT
    uint32_t waited_fallback = express_vk_wait_transfers(
        NULL, EXPRESS_VK_TRANSFER_UPLOAD, "queue_submit_strict_upload_fallback");
    if (EXPRESS_VK_ENABLE_SYNC_DIAG_LOG) {
        LOGD("[ExpressVkTransfer] submit upload wait policy=strict_fallback waited=%u",
             waited_fallback);
    }
    if (waited_fallback != 0) {
        express_vk_stats_add_u64(&g_express_vk_stats.submit_wait_all_uploads, 1);
    }
#endif
    g_mutex_lock(&g_express_vk_stats_lock);
    g_express_vk_stats.submit_wait_range_calls++;
    g_express_vk_stats.submit_wait_range_tasks += waited_ranges;
    g_express_vk_stats.events++;
    express_vk_stats_maybe_log_locked("submit_range_wait");
    g_mutex_unlock(&g_express_vk_stats_lock);
}

static uint32_t express_vk_commit_fence_outputs(VkFence fence, const char *reason)
{
    if (fence == VK_NULL_HANDLE) {
        return 0;
    }

    ExpressVkFenceOutputHints *entry = g_express_vk_fence_output_hints;
    while (entry != NULL && entry->fence != fence) {
        entry = entry->next;
    }
    if (entry == NULL) {
        return 0;
    }

    uint32_t committed = 0;
    uint32_t prefetched = 0;
    uint64_t committed_bytes = 0;
    gint64 begin_us = g_get_real_time();

    for (uint32_t i = 0; i < entry->range_count; ++i) {
        ExpressVkSubmitRangeHint *hint = &entry->ranges[i];
        if (hint->reg == NULL || hint->reg->guest_mem == NULL ||
            hint->host_ptr == NULL || hint->size == 0) {
            continue;
        }
        if (hint->size > EXPRESS_VK_WAIT_COMMIT_DOWNLOAD_MAX_BYTES) {
            continue;
        }

        express_vk_wait_transfers_range(
            hint->reg,
            EXPRESS_VK_TRANSFER_UPLOAD,
            hint->offset,
            hint->size,
            "wait_commit_before_download");

        bool used_prefetch =
            express_vk_wait_covering_transfer(
                hint->reg,
                EXPRESS_VK_TRANSFER_DOWNLOAD,
                hint->offset,
                hint->size,
                "wait_commit_prefetch") != 0;

        if (!used_prefetch) {
            if (hint->device == VK_NULL_HANDLE || hint->host_memory == 0) {
                continue;
            }
            VkMappedMemoryRange range;
            memset(&range, 0, sizeof(range));
            range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
            range.memory = (VkDeviceMemory)(uintptr_t)hint->host_memory;
            range.offset = hint->offset;
            range.size = hint->size;

            VkResult invalidate_result =
                vkInvalidateMappedMemoryRanges(hint->device, 1, &range);
            if (invalidate_result != VK_SUCCESS) {
                LOGE("[ExpressVkCommit] invalidate failed fence=%p range=%u result=%d",
                     (void *)fence,
                     i,
                     invalidate_result);
                continue;
            }

            write_to_guest_mem(
                hint->reg->guest_mem,
                (char *)hint->host_ptr + hint->offset,
                hint->offset,
                hint->size);
        }

        committed++;
        if (used_prefetch) {
            prefetched++;
        }
        committed_bytes += hint->size;
    }

    LOGD("[ExpressVkCommit] fence=%p reason=%s ranges=%u committed=%u prefetched=%u bytes=%llu ms=%.3f",
         (void *)fence,
         reason ? reason : "unknown",
         entry->range_count,
         committed,
         prefetched,
         (unsigned long long)committed_bytes,
         express_vk_elapsed_ms(begin_us, g_get_real_time()));

    if (committed != 0) {
        g_mutex_lock(&g_express_vk_stats_lock);
        g_express_vk_stats.wait_committed_ranges += committed;
        g_express_vk_stats.wait_committed_bytes += committed_bytes;
        g_express_vk_stats.wait_committed_prefetched += prefetched;
        g_express_vk_stats.events++;
        express_vk_stats_maybe_log_locked("wait_commit");
        g_mutex_unlock(&g_express_vk_stats_lock);
    } else {
        express_vk_stats_note("wait_commit_empty");
    }

    express_vk_remove_fence_output_hints(fence);
    return committed;
}

static void express_vk_register_memory(uint64_t guest_memory, uint64_t host_memory, uint64_t size, Guest_Mem *guest_mem)
{
    if (guest_memory == 0 || size == 0 || guest_mem == NULL) {
        if (guest_mem) free_copied_guest_mem(guest_mem);
        return;
    }

    ExpressVkRegisteredMemory *old = express_vk_find_registered_memory(guest_memory, host_memory);
    if (old != NULL) {
        express_vk_wait_transfers(old, EXPRESS_VK_TRANSFER_ANY, "register_update");
        if (old->guest_mem) free_copied_guest_mem(old->guest_mem);
        old->guest_memory = guest_memory;
        old->host_memory = host_memory;
        old->size = size;
        old->guest_mem = guest_mem;
        LOGD("[ExpressVkMem] updated guest=0x%llx host=0x%llx size=%llu scatter=%d all_len=%d",
             (unsigned long long)guest_memory,
             (unsigned long long)host_memory,
             (unsigned long long)size,
             guest_mem->num,
             guest_mem->all_len);
        return;
    }

    ExpressVkRegisteredMemory *entry = g_malloc0(sizeof(*entry));
    entry->guest_memory = guest_memory;
    entry->host_memory = host_memory;
    entry->size = size;
    entry->guest_mem = guest_mem;
    entry->next = g_express_vk_registered_memories;
    g_express_vk_registered_memories = entry;

    LOGD("[ExpressVkMem] registered guest=0x%llx host=0x%llx size=%llu scatter=%d all_len=%d",
         (unsigned long long)guest_memory,
         (unsigned long long)host_memory,
         (unsigned long long)size,
         guest_mem->num,
         guest_mem->all_len);
}

static void express_vk_unregister_memory(uint64_t guest_memory, uint64_t host_memory)
{
    ExpressVkRegisteredMemory **prev = &g_express_vk_registered_memories;
    ExpressVkRegisteredMemory *it = g_express_vk_registered_memories;
    while (it != NULL) {
        if ((guest_memory != 0 && it->guest_memory == guest_memory) ||
            (host_memory != 0 && it->host_memory == host_memory)) {
            express_vk_wait_transfers(it, EXPRESS_VK_TRANSFER_ANY, "unregister");
            *prev = it->next;
            LOGD("[ExpressVkMem] unregistered guest=0x%llx host=0x%llx size=%llu",
                 (unsigned long long)it->guest_memory,
                 (unsigned long long)it->host_memory,
                 (unsigned long long)it->size);
            if (it->guest_mem) free_copied_guest_mem(it->guest_mem);
            g_free(it);
            return;
        }
        prev = &it->next;
        it = it->next;
    }
}

static bool should_hide_device_extension_from_guest(const char* name)
{
    if (!name) {
        return false;
    }

    return strcmp(name, "VK_EXT_robustness2") == 0 ||
           strcmp(name, "VK_KHR_robustness2") == 0;
}

VkResult destroy_vulkan_object_other(
    uint64_t guest_dev,
    uint64_t guest_obj,
    VkObjectType obj_type1,
    VkObjectType obj_type2,
    void (*destroy_func)(void*, void*, const VkAllocationCallbacks*),
    const VkAllocationCallbacks* pAllocator) {

    void* realDev = (void*)(uintptr_t)lookup_mapping(obj_type1, guest_dev);
    void* realObj = (void*)(uintptr_t)lookup_mapping(obj_type2, guest_obj);
    if (realDev == (void*)(uintptr_t)UINT64_MAX || realObj == (void*)(uintptr_t)UINT64_MAX) {
        LOGE("Failed to destroy Vulkan object");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    destroy_func(realDev, realObj, pAllocator);
    if (!remove_mapping(obj_type2, guest_obj)) {
        LOGE("Failed to remove mapping for Vulkan object %llu", (unsigned long long)guest_obj);
    } else {
        LOGD("Successfully removed mapping for Vulkan object %llu", (unsigned long long)guest_obj);
    }
    return VK_SUCCESS;
}
VkResult destroy_vulkan_object_essential(
    uint64_t guest_dev,
    VkObjectType obj_type,
    void (*destroy_func)(void*, const VkAllocationCallbacks*),
    const VkAllocationCallbacks* pAllocator) {

    void* realDev = (void*)(uintptr_t)lookup_mapping(obj_type, guest_dev);
    if (realDev == NULL || realDev == (void*)(uintptr_t)UINT64_MAX) {
        LOGE("Failed to destroy Vulkan object, invalid mapping for guest_dev %llu",
             (unsigned long long)guest_dev);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    destroy_func(realDev, pAllocator);
    if (!remove_mapping(obj_type, guest_dev)) {
        LOGE("Failed to remove mapping for Vulkan object %llu", (unsigned long long)guest_dev);
    } else {
        LOGD("Successfully removed mapping for Vulkan object %llu", (unsigned long long)guest_dev);
    }
    return VK_SUCCESS;
}

/*
 * More than one Vulkan runtime may be alive in the guest process (for
 * example ncnn and MNN).  A new VkInstance must not invalidate mappings that
 * belong to an older, still-live instance.  Reserve the instance slot before
 * host creation so concurrent creates cannot both reset the global table.
 */
static GMutex g_express_vk_instance_lifecycle_lock;
static uint64_t g_express_vk_live_instance_count = 0;

static void express_vk_instance_begin_create(void)
{
    g_mutex_lock(&g_express_vk_instance_lifecycle_lock);
    if (g_express_vk_live_instance_count == 0) {
        clear_all_mappings();
        LOGI("Host: cleared stale Vulkan mappings before first live instance");
    } else {
        LOGI("Host: preserving Vulkan mappings for %llu live instance(s)",
             (unsigned long long)g_express_vk_live_instance_count);
    }
    g_express_vk_live_instance_count++;
    g_mutex_unlock(&g_express_vk_instance_lifecycle_lock);
}

static void express_vk_instance_finish_create(VkResult result)
{
    if (result == VK_SUCCESS) {
        return;
    }
    g_mutex_lock(&g_express_vk_instance_lifecycle_lock);
    if (g_express_vk_live_instance_count != 0) {
        g_express_vk_live_instance_count--;
    }
    g_mutex_unlock(&g_express_vk_instance_lifecycle_lock);
}

static void express_vk_instance_note_destroy(void)
{
    g_mutex_lock(&g_express_vk_instance_lifecycle_lock);
    if (g_express_vk_live_instance_count != 0) {
        g_express_vk_live_instance_count--;
    }
    LOGI("Host: Vulkan instance destroyed, live instances=%llu",
         (unsigned long long)g_express_vk_live_instance_count);
    g_mutex_unlock(&g_express_vk_instance_lifecycle_lock);
}

VkResult destroy_vulkan_object_device(
    uint64_t guest_dev,
    uint64_t guest_obj,
    VkObjectType obj_type,
    void (*destroy_func)(VkDevice, void*, const VkAllocationCallbacks*),
    const VkAllocationCallbacks* pAllocator) {

    if (guest_obj == 0) {
        LOGD("Skip destroying null Vulkan object type %d", obj_type);
        return VK_SUCCESS;
    }

    VkDevice realDev = (VkDevice)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_dev);
    void* realObj = (void*)(uintptr_t)lookup_mapping(obj_type, guest_obj);
    if (!realDev || !realObj) {
        LOGE("Failed to destroy Vulkan object type %d: guest_dev=%llu guest_obj=%llu",
             obj_type,
             (unsigned long long)guest_dev,
             (unsigned long long)guest_obj);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    destroy_func(realDev, realObj, pAllocator);
    if (!remove_mapping(obj_type, guest_obj)) {
        LOGE("Failed to remove mapping for Vulkan object %llu", (unsigned long long)guest_obj);
    }
    return VK_SUCCESS;
}

void checkHostVisible(VkPhysicalDevice physicalDevice, uint32_t memoryTypeIndex) {
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProps);

    if (memoryTypeIndex >= memProps.memoryTypeCount) {
        LOGD("Invalid memoryTypeIndex %u (max %u)",
               memoryTypeIndex, memProps.memoryTypeCount - 1);
        return;
    }

    VkMemoryPropertyFlags flags =
        memProps.memoryTypes[memoryTypeIndex].propertyFlags;

    LOGD("MemoryType %u flags: 0x%08x",
           memoryTypeIndex, flags);

    if (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
        LOGD("  -> HOST_VISIBLE is PRESENT");
    } else {
        LOGD("  -> HOST_VISIBLE is NOT present");
    }

    if (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) {
        LOGD("  -> HOST_COHERENT is PRESENT");
    } else {
        LOGD("  -> HOST_COHERENT is NOT present");
    }
}

static int copy_from_call_para_fast(Call_Para para, void* dst, size_t len)
{
    if (!dst || len == 0)
    {
        return 0;
    }

    if (!para.data || para.data_len < len)
    {
        return 0;
    }

    // Fast path: single scatter block, direct guest pointer available.
    int null_flag = 0;
    void* src = get_direct_ptr((Guest_Mem*)para.data, &null_flag);
    if (src)
    {
        memcpy(dst, src, len);
        return 1;
    }

    // Fallback path: non-direct/scatter data. Read directly into destination.
    // This keeps fallback at a single copy as well.
    if (null_flag == 0)
    {
        read_from_guest_mem((Guest_Mem*)para.data, dst, 0, len);
        return 1;
    }

    // Explicit guest NULL pointer case.
    return 0;
}

void transitionImageLayoutForSampling(VkDevice device, VkImage image, VkImageLayout format) {

    VkCommandPool commandPool = getOrCreateCommandPool(device);

    LOGD("going to update image layout for sampling image %llx", (unsigned long long)image);

    VkCommandBufferAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = commandPool;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer commandBuffer;
    vkAllocateCommandBuffers(device, &allocInfo, &commandBuffer);

    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    vkBeginCommandBuffer(commandBuffer, &beginInfo);


    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = format;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    VkPipelineStageFlags sourceStage;
    VkPipelineStageFlags destinationStage;


    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;

    vkCmdPipelineBarrier(
        commandBuffer,
        sourceStage, destinationStage,
        0,
        0, NULL,
        0, NULL,
        1, &barrier
    );

    vkEndCommandBuffer(commandBuffer);


    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;

    VkQueue graphicsQueue = getGraphicsQueue(device);
    vkQueueSubmit(graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(graphicsQueue);

    vkFreeCommandBuffers(device, commandPool, 1, &commandBuffer);

    LOGD("Host: Image layout transition completed");
}

static VkExtensionProperties* g_cached_instance_extensions = NULL;
static uint32_t g_cached_instance_extension_count = 0;
static int g_instance_extensions_cached = 0;

static VkDeviceSize g_cached_non_coherent_atom_size = 0;
static VkDeviceSize g_cached_min_storage_buffer_offset_alignment = 0;
static uint64_t g_cached_props_guest_pd = 0;
static uint64_t g_cached_props_host_pd = 0;

static void log_ncnn_align_classified(const char* api_name,
                                      VkDeviceSize c,
                                      uint64_t guest_dev,
                                      VkDevice host_dev,
                                      uint64_t guest_obj,
                                      uint64_t host_obj)
{
    VkDeviceSize a = g_cached_non_coherent_atom_size;
    VkDeviceSize b = g_cached_min_storage_buffer_offset_alignment;
    int a0 = (a == 0);
    int b0 = (b == 0);
    int c0 = (c == 0);

    if (!a0 && !b0 && !c0) {
        LOGD("NCNN_ALIGN_OK a=%llu b=%llu c=%llu api=%s guest_dev=%llu host_dev=%p guest_obj=%llu host_obj=%llu",
            (unsigned long long)a,
            (unsigned long long)b,
            (unsigned long long)c,
            api_name,
            (unsigned long long)guest_dev,
            (void*)host_dev,
            (unsigned long long)guest_obj,
            (unsigned long long)host_obj);
    } else {
        LOGD("NCNN_ALIGN_BAD a=%llu b=%llu c=%llu api=%s zero_flags=<%d,%d,%d> guest_dev=%llu host_dev=%p guest_obj=%llu host_obj=%llu",
            (unsigned long long)a,
            (unsigned long long)b,
            (unsigned long long)c,
            api_name,
            a0, b0, c0,
            (unsigned long long)guest_dev,
            (void*)host_dev,
            (unsigned long long)guest_obj,
            (unsigned long long)host_obj);
    }
}


static int is_extension_supported(const char* ext_name) {
    for (uint32_t i = 0; i < g_cached_instance_extension_count; i++) {
        if (strcmp(g_cached_instance_extensions[i].extensionName, ext_name) == 0) {
            return 1;
        }
    }
    return 0;
}


static VkResult ensure_instance_extensions_cached(void) {
    if (g_instance_extensions_cached) {
        return VK_SUCCESS;
    }

    VkResult result = vkEnumerateInstanceExtensionProperties(NULL, &g_cached_instance_extension_count, NULL);
    if (result != VK_SUCCESS) {
        LOGE("Failed to query instance extension count: %d", result);
        return result;
    }

    g_cached_instance_extensions = (VkExtensionProperties*)malloc(g_cached_instance_extension_count * sizeof(VkExtensionProperties));
    if (!g_cached_instance_extensions) {
        LOGE("Failed to allocate memory for extension cache");
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    result = vkEnumerateInstanceExtensionProperties(NULL, &g_cached_instance_extension_count, g_cached_instance_extensions);
    if (result != VK_SUCCESS) {
        LOGE("Failed to enumerate instance extensions: %d", result);
        free(g_cached_instance_extensions);
        g_cached_instance_extensions = NULL;
        g_cached_instance_extension_count = 0;
        return result;
    }

    g_instance_extensions_cached = 1;
    LOGD("Cached %u instance extensions", g_cached_instance_extension_count);
    return VK_SUCCESS;
}

static void express_vk_log_pnext_chain(const char *label, const void *pNext)
{
    const VkBaseInStructure *base = (const VkBaseInStructure *)pNext;
    uint32_t depth = 0;

    if (base == NULL) {
        LOGD("%s pNext=NULL", label ? label : "VkStruct");
        return;
    }

    while (base != NULL && depth < 32) {
        LOGD("%s pNext[%u] sType=%d next=%p",
             label ? label : "VkStruct",
             depth,
             base->sType,
             base->pNext);
        base = base->pNext;
        depth++;
    }
    if (base != NULL) {
        LOGW("%s pNext chain truncated at depth=%u",
             label ? label : "VkStruct",
             depth);
    }
}

static void express_vk_log_device_create_info(const VkDeviceCreateInfo *create_info)
{
    if (create_info == NULL) {
        LOGE("Host: vkCreateDevice create_info=NULL");
        return;
    }

    LOGD("Host: vkCreateDevice ci sType=%d flags=0x%x pNext=%p "
         "queueCreateInfoCount=%u enabledLayerCount=%u enabledExtensionCount=%u "
         "pEnabledFeatures=%p",
         create_info->sType,
         create_info->flags,
         create_info->pNext,
         create_info->queueCreateInfoCount,
         create_info->enabledLayerCount,
         create_info->enabledExtensionCount,
         create_info->pEnabledFeatures);
    express_vk_log_pnext_chain("Host: vkCreateDevice ci", create_info->pNext);

    if (create_info->pEnabledFeatures != NULL) {
        const VkPhysicalDeviceFeatures *f = create_info->pEnabledFeatures;
        LOGD("Host: vkCreateDevice features robustBufferAccess=%u fullDrawIndexUint32=%u "
             "shaderInt16=%u shaderInt64=%u shaderFloat64=%u shaderStorageImageWriteWithoutFormat=%u",
             f->robustBufferAccess,
             f->fullDrawIndexUint32,
             f->shaderInt16,
             f->shaderInt64,
             f->shaderFloat64,
             f->shaderStorageImageWriteWithoutFormat);
    }

    if (create_info->pQueueCreateInfos == NULL && create_info->queueCreateInfoCount != 0) {
        LOGE("Host: vkCreateDevice queueCreateInfoCount=%u but pQueueCreateInfos=NULL",
             create_info->queueCreateInfoCount);
        return;
    }

    uint32_t log_queue_count = create_info->queueCreateInfoCount;
    if (log_queue_count > 16) {
        log_queue_count = 16;
    }
    for (uint32_t i = 0; i < log_queue_count; ++i) {
        const VkDeviceQueueCreateInfo *q = &create_info->pQueueCreateInfos[i];
        float first_priority = 0.0f;
        if (q->pQueuePriorities != NULL && q->queueCount != 0) {
            first_priority = q->pQueuePriorities[0];
        }
        LOGD("Host: vkCreateDevice queue[%u] sType=%d flags=0x%x family=%u "
             "queueCount=%u pNext=%p priorities=%p firstPriority=%.3f",
             i,
             q->sType,
             q->flags,
             q->queueFamilyIndex,
             q->queueCount,
             q->pNext,
             q->pQueuePriorities,
             first_priority);
        express_vk_log_pnext_chain("Host: vkCreateDevice queue", q->pNext);
    }
    if (create_info->queueCreateInfoCount > log_queue_count) {
        LOGW("Host: vkCreateDevice queue logging truncated total=%u logged=%u",
             create_info->queueCreateInfoCount,
             log_queue_count);
    }
}

static int express_vk_physical_device_preference_score(const VkPhysicalDeviceProperties *props)
{
    int score = 0;

    if (props == NULL) {
        return score;
    }

    if (props->vendorID == 0x10de) {
        score += 10000;
    } else if (props->vendorID == 0x1002 || props->vendorID == 0x1022) {
        score += 6000;
    } else if (props->vendorID == 0x8086) {
        score -= 2000;
    }

    switch (props->deviceType) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
        score += 5000;
        break;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
        score += 1000;
        break;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
        score += 500;
        break;
    case VK_PHYSICAL_DEVICE_TYPE_CPU:
        score -= 3000;
        break;
    default:
        break;
    }

    return score;
}

static void express_vk_sort_physical_devices_for_guest(VkPhysicalDevice *devices,
                                                       uint32_t count)
{
    if (devices == NULL || count < 2) {
        return;
    }

    for (uint32_t i = 0; i < count; ++i) {
        VkPhysicalDeviceProperties props;
        memset(&props, 0, sizeof(props));
        vkGetPhysicalDeviceProperties(devices[i], &props);
        LOGD("Host: physical device before sort index=%u handle=%p name=%s vendor=0x%x device=0x%x type=%d score=%d",
             i,
             (void *)devices[i],
             props.deviceName,
             props.vendorID,
             props.deviceID,
             props.deviceType,
             express_vk_physical_device_preference_score(&props));
    }

    for (uint32_t i = 0; i + 1 < count; ++i) {
        uint32_t best = i;
        VkPhysicalDeviceProperties best_props;
        memset(&best_props, 0, sizeof(best_props));
        vkGetPhysicalDeviceProperties(devices[best], &best_props);
        int best_score = express_vk_physical_device_preference_score(&best_props);

        for (uint32_t j = i + 1; j < count; ++j) {
            VkPhysicalDeviceProperties props;
            memset(&props, 0, sizeof(props));
            vkGetPhysicalDeviceProperties(devices[j], &props);
            int score = express_vk_physical_device_preference_score(&props);
            if (score > best_score) {
                best = j;
                best_score = score;
                best_props = props;
            }
        }

        if (best != i) {
            VkPhysicalDevice tmp = devices[i];
            devices[i] = devices[best];
            devices[best] = tmp;
        }
    }

    for (uint32_t i = 0; i < count; ++i) {
        VkPhysicalDeviceProperties props;
        memset(&props, 0, sizeof(props));
        vkGetPhysicalDeviceProperties(devices[i], &props);
        LOGD("Host: sorted physical device index=%u name=%s vendor=0x%x device=0x%x type=%d score=%d",
             i,
             props.deviceName,
             props.vendorID,
             props.deviceID,
             props.deviceType,
             express_vk_physical_device_preference_score(&props));
        LOGD("Host: physical device after sort index=%u handle=%p name=%s vendor=0x%x device=0x%x type=%d score=%d",
             i,
             (void *)devices[i],
             props.deviceName,
             props.vendorID,
             props.deviceID,
             props.deviceType,
             express_vk_physical_device_preference_score(&props));
    }
}

void vk_decode_invoke(Render_Thread_Context *context, Teleport_Express_Call *call)

{
    Render_Thread_Context *egl_context = (Render_Thread_Context *)context;

    if (unlikely(egl_context == NULL))
    {
        call->callback(call, 0);
        return;
    }

    Call_Para all_para[MAX_PARA_NUM];

    unsigned char ret_local_buf[1024 * 4];

    unsigned char *no_ptr_buf = NULL;

    uint64_t fun_id = GET_FUN_ID(call->id);
    LOGD("get vk call with id %lld", fun_id);

    switch (fun_id)
    {

    case FUNID_vkExpressFlimeControlANDROID:
    {
        GError *flime_error = NULL;
        size_t control_bytes = 0;
        uint8_t control[EXPRESS_VK_FLIME_CONTROL_PAGE_MAX_SIZE];
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        uint8_t *packet = NULL;
        ExpressVkFlimeControlSink control_sink = { 0 };
        Teleport_Express_Queue_Elem *sink_elem = NULL;
        bool bridge_called = false;
        bool ok = false;

        g_mutex_lock(&g_express_vk_transaction_lock);
        if (para_num == 2 &&
            all_para[0].data_len >= EXPRESS_VK_FLIME_WIRE_HEADER_SIZE &&
            all_para[0].data_len <= EXPRESS_VK_FLIME_ROUTE_MAX_PACKET_BYTES &&
            all_para[1].data_len >= EXPRESS_VK_FLIME_CONTROL_PAGE_MAX_SIZE &&
            teleport_express_guest_mem_layout_exact(
                all_para[0].data, all_para[0].data_len) &&
            teleport_express_guest_mem_layout_exact(
                all_para[1].data, all_para[1].data_len) &&
            all_para[1].data->num == 1 &&
            call->elem_header != NULL &&
            call->elem_header->next != NULL) {
            sink_elem = call->elem_header->next->next;
        }
        if (sink_elem != NULL && sink_elem->next == NULL &&
            sink_elem->para == all_para[1].data &&
            sink_elem->len == all_para[1].data_len &&
            sink_elem->elem.in_num == 1 &&
            sink_elem->elem.out_num == 0 &&
            sink_elem->elem.in_addr != NULL &&
            sink_elem->elem.in_sg != NULL &&
            sink_elem->elem.in_sg[0].iov_base != NULL &&
            sink_elem->elem.in_sg[0].iov_len == all_para[1].data_len &&
            sink_elem->elem.in_addr[0] <= UINT64_MAX - 24 &&
            ((sink_elem->elem.in_addr[0] + 24) &
             (sizeof(uint64_t) - 1)) == 0) {
            packet = g_try_malloc(all_para[0].data_len);
            if (packet != NULL &&
                teleport_express_guest_mem_read_checked(
                    all_para[0].data, all_para[0].data_len, packet,
                    all_para[0].data_len)) {
                control_sink.vdev = call->vdev;
                control_sink.guest_address = sink_elem->elem.in_addr[0];
                control_sink.capacity = all_para[1].data_len;
                bridge_called = true;
                ok = express_vk_flime_bridge_control(
                    call->process_id, packet, all_para[0].data_len,
                    &control_sink, control, sizeof(control), &control_bytes,
                    &flime_error);
                if (ok &&
                    control_bytes <=
                        all_para[1].data_len -
                            EXPRESS_VK_FLIME_CONTROL_PAGE_HEADER_SIZE) {
                    sink_elem->written_len =
                        EXPRESS_VK_FLIME_CONTROL_PAGE_HEADER_SIZE +
                        control_bytes;
                }
            }
        }
        if (!bridge_called) {
            /*
             * A malformed outer envelope must still invalidate any staged
             * prefix for this transport process.  Passing an empty packet is
             * the bridge's fail-closed process-abort path.
             */
            ok = express_vk_flime_bridge_control(
                call->process_id, NULL, 0, NULL, control, sizeof(control),
                &control_bytes, &flime_error);
        }
        g_mutex_unlock(&g_express_vk_transaction_lock);
        if (!ok) {
            LOGE("Host: FLIME control rejected: %s",
                 flime_error != NULL ? flime_error->message :
                  "invalid parameter envelope");
        }
        g_clear_error(&flime_error);
        g_free(packet);
    }
    break;

    case FUNID_vkExpressFlimeRoutedDescriptorUpdatesANDROID:
    {
        ExpressVkFlimeRouteReply reply;
        ExpressVkFlimeReleaseBatch *release = NULL;
        GError *flime_error = NULL;
        uint64_t receive_ns = (uint64_t)g_get_monotonic_time() * 1000;
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        uint8_t *packet = NULL;
        bool bridge_called = false;
        bool reply_writable = para_num == 2 &&
            all_para[1].data_len >= sizeof(reply) &&
            teleport_express_guest_mem_layout_exact(
                all_para[1].data, all_para[1].data_len);
        bool ok = false;

        memset(&reply, 0, sizeof(reply));
        /*
         * Route admission, fallback-session removal, native descriptor
         * realization, and release completion are one transaction.  This
         * keeps the global lock order identical to QueueSubmit:
         * transaction -> bridge -> core session.
         */
        g_mutex_lock(&g_express_vk_transaction_lock);
        if (para_num == 2 &&
            all_para[0].data_len >= EXPRESS_VK_FLIME_ROUTE_HEADER_SIZE &&
            all_para[0].data_len <= EXPRESS_VK_FLIME_ROUTE_MAX_PACKET_BYTES &&
            reply_writable &&
            teleport_express_guest_mem_layout_exact(
                all_para[0].data, all_para[0].data_len)) {
            packet = g_try_malloc(all_para[0].data_len);
            if (packet != NULL &&
                teleport_express_guest_mem_read_checked(
                    all_para[0].data, all_para[0].data_len, packet,
                    all_para[0].data_len)) {
                bridge_called = true;
                ok = express_vk_flime_bridge_route(
                    call->process_id, packet, all_para[0].data_len,
                    receive_ns, &reply, &release, &flime_error);
            }
        }
        if (!bridge_called) {
            /*
             * Do not let an undersized, oversized, unmappable, or reply-less
             * envelope strand a pending routed submission.
             */
            ok = express_vk_flime_bridge_route(
                call->process_id, NULL, 0, receive_ns, &reply, &release,
                &flime_error);
        }
        if (release != NULL) {
            uint64_t realize_ns;

            realize_ns = express_vk_flime_realize_descriptor_writes(
                express_vk_flime_bridge_release_device(release),
                express_vk_flime_bridge_release_write_count(release),
                express_vk_flime_bridge_release_writes(release));
            express_vk_flime_bridge_complete_release(release, true,
                                                      realize_ns);
        }
        if (reply.magic_le ==
                GUINT32_TO_LE(EXPRESS_VK_FLIME_ROUTE_REPLY_MAGIC)) {
            uint64_t reply_ns =
                (uint64_t)g_get_monotonic_time() * 1000;

            reply.host_service_ns_le = GUINT64_TO_LE(
                reply_ns >= receive_ns ? reply_ns - receive_ns : 0);
        }
        g_mutex_unlock(&g_express_vk_transaction_lock);
        if (reply_writable) {
            if (!teleport_express_guest_mem_write_checked(
                    all_para[1].data, all_para[1].data_len, &reply,
                    sizeof(reply))) {
                LOGE("Host: FLIME route reply copy was incomplete");
            } else if (call->elem_tail != NULL) {
                call->elem_tail->written_len = sizeof(reply);
            }
        }
        if (!ok) {
            LOGE("Host: FLIME routed descriptor packet rejected: %s",
                 flime_error != NULL ? flime_error->message :
                 "invalid parameter envelope");
        }
        g_clear_error(&flime_error);
        g_free(packet);
    }
    break;

    case FUNID_vkCreateInstance:
    {
        LOGD("get call FUNID_vkCreateInstance!");

#ifdef __APPLE__
        vulkan_surface_set_initialized(false);
#endif

        VkInstanceCreateInfo create_info = { 0 };
        VkAllocationCallbacks ignored_guest_allocator = { 0 };
        VkInstanceCreateInfo *pCreateInfo = &create_info;
        const VkAllocationCallbacks* pAllocator = NULL;
        VkInstance pInstance = VK_NULL_HANDLE;
        VkResult result = VK_ERROR_INITIALIZATION_FAILED;

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        LOGD("get vk param number %d instance is %lld %lld", para_num, pInstance, &pInstance);
        if (para_num != 2 ||
            !teleport_express_guest_mem_layout_exact(
                all_para[0].data, all_para[0].data_len) ||
            all_para[1].data_len < sizeof(result) ||
            !teleport_express_guest_mem_layout_exact(
                all_para[1].data, all_para[1].data_len)) {
            LOGE("Host: malformed vkCreateInstance parameter envelope");
            if (para_num > 1) {
                teleport_express_guest_mem_write_checked(
                    all_para[1].data, all_para[1].data_len,
                    &result, sizeof(result));
            }
            break;
        }

        int need_free = 0;
        char *stream_ptr;
        stream_ptr = call_para_to_ptr(all_para[0], &need_free);
        if (stream_ptr == NULL) {
            LOGE("Host: vkCreateInstance input stream is NULL");
            teleport_express_guest_mem_write_checked(
                all_para[1].data, all_para[1].data_len,
                &result, sizeof(result));
            break;
        }
        char *stream_base = stream_ptr;
        uint8_t ** stream_ptr_ptr = (uint8_t **)&stream_ptr;

        decode_from_stream_VkInstanceCreateInfo(
            VK_STRUCTURE_TYPE_MAX_ENUM, pCreateInfo, stream_ptr_ptr);

        LOGD("got vkCreateinfo with %lld layers %d extensions %d",
             (long long)pCreateInfo->sType,
             pCreateInfo->enabledLayerCount,
             pCreateInfo->enabledExtensionCount);
        LOGD("application name is %s",
             pCreateInfo->pApplicationInfo != NULL &&
             pCreateInfo->pApplicationInfo->pApplicationName != NULL ?
                 pCreateInfo->pApplicationInfo->pApplicationName : "(none)");
        LOGD("application create info is %p %d",
             (void *)pCreateInfo->pApplicationInfo,
             pCreateInfo->pApplicationInfo != NULL ?
                 (int)pCreateInfo->pApplicationInfo->sType : -1);

#ifdef __APPLE__
        ((VkInstanceCreateInfo*)pCreateInfo)->flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
#endif
        uint64_t guest_allocator = 0;
        uintptr_t stream_base_addr = (uintptr_t)stream_base;
        uintptr_t stream_cursor_addr = (uintptr_t)*stream_ptr_ptr;
        size_t stream_consumed;
        if (stream_cursor_addr < stream_base_addr ||
            stream_cursor_addr - stream_base_addr > all_para[0].data_len) {
            LOGE("Host: malformed vkCreateInstance decoded stream position");
            teleport_express_guest_mem_write_checked(
                all_para[1].data, all_para[1].data_len,
                &result, sizeof(result));
            if (need_free) {
                g_free(stream_base);
            }
            break;
        }
        stream_consumed = stream_cursor_addr - stream_base_addr;
        if (all_para[0].data_len - stream_consumed <
            sizeof(guest_allocator) + sizeof(uint64_t)) {
            LOGE("Host: truncated vkCreateInstance stream tail");
            teleport_express_guest_mem_write_checked(
                all_para[1].data, all_para[1].data_len,
                &result, sizeof(result));
            if (need_free) {
                g_free(stream_base);
            }
            break;
        }
        memcpy(&guest_allocator, *stream_ptr_ptr, sizeof(guest_allocator));
        *stream_ptr_ptr += sizeof(guest_allocator);

        if(guest_allocator) {
            uint64_t guest_user_data = 0;
            size_t allocator_wire_size =
                sizeof(uint64_t) + 5 * sizeof(uint64_t);

            memcpy(&guest_user_data, *stream_ptr_ptr,
                   sizeof(guest_user_data));
            if (guest_user_data != 0) {
                allocator_wire_size++;
            }
            stream_cursor_addr = (uintptr_t)*stream_ptr_ptr;
            stream_consumed = stream_cursor_addr - stream_base_addr;
            if (allocator_wire_size >
                all_para[0].data_len - stream_consumed ||
                sizeof(uint64_t) >
                all_para[0].data_len - stream_consumed -
                    allocator_wire_size) {
                LOGE("Host: truncated vkCreateInstance allocator stream");
                teleport_express_guest_mem_write_checked(
                    all_para[1].data, all_para[1].data_len,
                    &result, sizeof(result));
                if (need_free) {
                    g_free(stream_base);
                }
                break;
            }
            /*
             * Consume legacy wire data for compatibility, but never give
             * guest function addresses to the host Vulkan loader.
             */
            decode_from_stream_VkAllocationCallbacks(
                VK_STRUCTURE_TYPE_MAX_ENUM, &ignored_guest_allocator,
                stream_ptr_ptr);
            g_free(ignored_guest_allocator.pUserData);
            ignored_guest_allocator.pUserData = NULL;
        }

        uint64_t guest_instance = 0;
        memcpy(&guest_instance, *stream_ptr_ptr, sizeof(guest_instance));
        *stream_ptr_ptr += sizeof(uint64_t);

        express_vk_instance_begin_create();


        VkResult cache_result = ensure_instance_extensions_cached();
        if (cache_result != VK_SUCCESS) {
            LOGE("Failed to cache instance extensions, proceeding without filtering");
        }


        uint32_t origExtCount = pCreateInfo->enabledExtensionCount;
        const char* const* origExts = pCreateInfo->ppEnabledExtensionNames;

        const char** filteredExts = NULL;
        uint32_t filteredExtCount = 0;

        if (g_instance_extensions_cached && origExtCount > 0) {
            filteredExts = (const char**)malloc(sizeof(char*) * origExtCount);
            for (uint32_t i = 0; i < origExtCount; i++) {
                if (is_extension_supported(origExts[i])) {
                    filteredExts[filteredExtCount++] = origExts[i];
                    LOGD("Extension accepted: %s", origExts[i]);
                } else {
                    LOGD("Extension filtered out (not supported by host): %s", origExts[i]);
                }
            }
        } else {

            filteredExtCount = origExtCount;
            filteredExts = (const char**)origExts;
        }


        uint32_t glfwExtCount = 0;
        const char** glfwExts = glfwGetRequiredInstanceExtensions(&glfwExtCount);


#ifdef __APPLE__
        uint32_t totalExtCount = filteredExtCount + glfwExtCount + 1;
#else
        uint32_t totalExtCount = filteredExtCount + glfwExtCount;
#endif
        const char** mergedExts = malloc(sizeof(char*) * totalExtCount);

        for (uint32_t i = 0; i < filteredExtCount; i++) {
            mergedExts[i] = filteredExts[i];
        }
        for (uint32_t i = 0; i < glfwExtCount; i++) {
            mergedExts[filteredExtCount + i] = glfwExts[i];
            LOGD("glfw ext %d %s", i, glfwExts[i]);
        }
#ifdef __APPLE__
        mergedExts[totalExtCount-1] = "VK_KHR_portability_enumeration";
#endif
        for (uint32_t i = 0; i < totalExtCount; i++) {
            LOGD("Final enabled extension %d: %s", i, mergedExts[i]);
        }

        ((VkInstanceCreateInfo*)pCreateInfo)->enabledExtensionCount   = totalExtCount;
        ((VkInstanceCreateInfo*)pCreateInfo)->ppEnabledExtensionNames = mergedExts;

        result = vkCreateInstance(pCreateInfo, pAllocator, &pInstance);
        express_vk_instance_finish_create(result);


        free(mergedExts);
        if (g_instance_extensions_cached && filteredExts != origExts) {
            free(filteredExts);
        }

        if (result == VK_SUCCESS) {
            LOGD("got result %d instance %lld %lld size %d guest %lld", result, pInstance, &pInstance, sizeof(VkInstance), guest_instance);
            insert_mapping(EXPRESS_VK_OBJECT_TYPE_INSTANCE, guest_instance, (uint64_t)(uintptr_t)pInstance);
            LOGD("map result is %lld", lookup_mapping(EXPRESS_VK_OBJECT_TYPE_INSTANCE, guest_instance));
        } else {
            LOGE("vkCreateInstance failed with %d", result);
        }
        write_to_guest_mem(all_para[1].data, &result, 0, sizeof(VkResult));
        if (need_free) {
            g_free(stream_base);
        }
        LOGD("create instance done with result %d", result);

    }
    break;

    case FUNID_vkCreateAndroidSurfaceKHR: {
        LOGD("Host: vkCreateAndroidSurfaceKHR request");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        LOGD("get vk param number %d instance is", para_num);

        char *stream_ptr;

        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t* ptr = (uint8_t*)stream;

        uint64_t guest_inst = *(uint64_t*)(ptr);
        ptr += sizeof(uint64_t);

        uint64_t guest_window_ptr = *(uint64_t*)(ptr);
        ptr += sizeof(uint64_t);

        // uint64_t guest_hostSurf_addr = *(uint64_t*)(ptr);
        // ptr += sizeof(uint64_t);
        VkSurfaceKHR guestSurface = VK_NULL_HANDLE;
        if (!copy_from_call_para_fast(all_para[1], &guestSurface, sizeof(VkSurfaceKHR))) {
            LOGE("Host: vkCreateAndroidSurfaceKHR failed to read guest surface handle");
            break;
        }

        //if (need_free) free(stream);

        VkInstance hostInst = (VkInstance)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_INSTANCE, guest_inst);
        LOGD("Host: mapped guestInst %llu ? hostInst %p",
            (unsigned long long)guest_inst, (void*)hostInst);

        GLFWwindow* win = (GLFWwindow*)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_NATIVE_WINDOW, guest_window_ptr);
        if (!win) {
            glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
            win = glfwCreateWindow(1, 1, "Guest Window", NULL, NULL);
            insert_mapping(EXPRESS_VK_OBJECT_TYPE_NATIVE_WINDOW,
                        guest_window_ptr,
                        (uint64_t)(uintptr_t)win);
            LOGD("Host: created GLFW window %p for guest window %llu",
                win, (unsigned long long)guest_window_ptr);
        }

        VkSurfaceKHR hostSurface = VK_NULL_HANDLE;
        VkResult res = glfwCreateWindowSurface(hostInst, win, NULL, &hostSurface);
        if (res != VK_SUCCESS) {
            LOGE("Host: vkCreateAndroidSurfaceKHR failed %d", res);
            return;
        }

        LOGD("Host: created hostSurface %lld %d", (long long)hostSurface, res);

        insert_mapping(EXPRESS_VK_OBJECT_TYPE_SURFACE, (uint64_t)guestSurface, (uint64_t)(uintptr_t)hostSurface);
    }
    break;

    case FUNID_vkCreateSurfaceOHOS: {
        LOGD("Host: FUNID_vkCreateSurfaceOHOS request");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        LOGD("get vk param number %d instance is", para_num);

        char *stream_ptr;

        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t* ptr = (uint8_t*)stream;

        uint64_t guest_inst = *(uint64_t*)(ptr);
        ptr += sizeof(uint64_t);

        uint64_t guest_window_ptr = *(uint64_t*)(ptr);
        ptr += sizeof(uint64_t);

        // uint64_t guest_hostSurf_addr = *(uint64_t*)(ptr);
        // ptr += sizeof(uint64_t);
        VkSurfaceKHR guestSurface = VK_NULL_HANDLE;
        if (!copy_from_call_para_fast(all_para[1], &guestSurface, sizeof(VkSurfaceKHR))) {
            LOGE("Host: vkCreateSurfaceOHOS failed to read guest surface handle");
            break;
        }

        //if (need_free) free(stream);
THREAD_CONTROL_BEGIN
        VkInstance hostInst = (VkInstance)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_INSTANCE, guest_inst);
        LOGD("Host: mapped guestInst %llu ? hostInst %p",
            (unsigned long long)guest_inst, (void*)hostInst);

        GLFWwindow* win = (GLFWwindow*)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_NATIVE_WINDOW, guest_window_ptr);
        VkSurfaceKHR hostSurface = VK_NULL_HANDLE;
        VkResult res;

        if (!win) {
            glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
            glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);

            #ifdef __APPLE__
                // macOS commonly uses a 2x Retina scale.
                glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, GLFW_TRUE);
            #endif

            win = glfwCreateWindow(720, 1280, "Guest Window", NULL, NULL);
            insert_mapping(EXPRESS_VK_OBJECT_TYPE_NATIVE_WINDOW,
                        guest_window_ptr,
                        (uint64_t)(uintptr_t)win);
            LOGD("Host: created GLFW window %p for guest window %llu",
                win, (unsigned long long)guest_window_ptr);
        }

        res = glfwCreateWindowSurface(hostInst, win, NULL, &hostSurface);

        if (res != VK_SUCCESS) {
            LOGE("Host: vkCreateSurfaceOHOS failed %d", res);
            return;
        }

        LOGD("Host: created hostSurface %lld %d", (long long)hostSurface, res);

        insert_mapping(EXPRESS_VK_OBJECT_TYPE_SURFACE, (uint64_t)guestSurface, (uint64_t)(uintptr_t)hostSurface);
THREAD_CONTROL_END
    }
    break;

    case FUNID_vkCreateSwapchainKHR: {
        LOGD("Host: vkCreateSwapchainKHR request %lld", (long long)vkCreateSwapchainKHR);

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);

        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t* ptr = (uint8_t*)stream;

        uint64_t guest_device        = *(uint64_t*)ptr; ptr += sizeof(uint64_t);
        uint64_t guest_surface       = *(uint64_t*)ptr; ptr += sizeof(uint64_t);
        uint32_t minImageCount       = *(uint32_t*)ptr; ptr += sizeof(uint32_t);
        uint32_t imageFormat         = *(uint32_t*)ptr; ptr += sizeof(uint32_t);
        uint32_t width               = *(uint32_t*)ptr; ptr += sizeof(uint32_t);
        uint32_t height              = *(uint32_t*)ptr; ptr += sizeof(uint32_t);
        uint32_t presentMode         = *(uint32_t*)ptr; ptr += sizeof(uint32_t);

        VkSwapchainKHR guestSwapchain = VK_NULL_HANDLE;
        if (!copy_from_call_para_fast(all_para[1], &guestSwapchain, sizeof(VkSwapchainKHR))) {
            LOGE("Host: vkCreateSwapchainKHR failed to read guest swapchain handle");
            break;
        }

        LOGD("Host: vkCreateSwapchainKHR guest_device %llu guest_surface %llu minImageCount %d imageFormat %d width %d height %d presentMode %d",
            (unsigned long long)guest_device,
            (unsigned long long)guest_surface,
            minImageCount, imageFormat, width, height, presentMode);

        //if (need_free) free(stream);
        VkDevice hostDevice  = (VkDevice)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_device);
        VkSurfaceKHR hostSurface = (VkSurfaceKHR)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_SURFACE, guest_surface);

        vulkan_surface_create_swapchain(hostDevice, hostSurface, guestSwapchain, minImageCount, imageFormat, width, height, presentMode);
    }
    break;

    case FUNID_vkGetSwapchainImagesKHR: {
        LOGD("Host: vkGetSwapchainImagesKHR");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char*     stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t*  ptr    = (uint8_t*)stream;
        uint64_t guest_device    = *(uint64_t*)ptr; ptr += sizeof(uint64_t);
        uint64_t guest_swapchain = *(uint64_t*)ptr; ptr += sizeof(uint64_t);
        uint32_t count           = *(uint32_t*)ptr; ptr += sizeof(uint32_t);
        VkDevice       realDev       = (VkDevice)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_device);
        VkSwapchainKHR realSwapchain = (VkSwapchainKHR)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_SWAPCHAIN_KHR, guest_swapchain);
        LOGD("Host: vkGetSwapchainImagesKHR guest_device %llu guest_swapchain %llu count %d real swapchain %lld",
            (unsigned long long)guest_device,
            (unsigned long long)guest_swapchain,
            count, (long long)realSwapchain);

        uint64_t* guestImages = NULL;
        uint64_t* guestBuffers = NULL;
        int* buffer_width = NULL;
        int* buffer_height = NULL;
        if (count > 0) {
            guestImages = malloc(sizeof(uint64_t) * count);
            guestBuffers = malloc(sizeof(uint64_t) * count);
            buffer_width = malloc(sizeof(int) * count);
            buffer_height = malloc(sizeof(int) * count);
            if (!guestImages || !guestBuffers || !buffer_width || !buffer_height) {
                LOGE("Host: vkGetSwapchainImagesKHR OOM while preparing metadata arrays");
                if (guestImages) free(guestImages);
                if (guestBuffers) free(guestBuffers);
                if (buffer_width) free(buffer_width);
                if (buffer_height) free(buffer_height);
                break;
            }

            if (!copy_from_call_para_fast(all_para[1], guestImages, sizeof(uint64_t) * count) ||
                !copy_from_call_para_fast(all_para[2], guestBuffers, sizeof(uint64_t) * count) ||
                !copy_from_call_para_fast(all_para[3], buffer_width, sizeof(int) * count) ||
                !copy_from_call_para_fast(all_para[4], buffer_height, sizeof(int) * count)) {
                LOGE("Host: vkGetSwapchainImagesKHR failed to read guest metadata arrays");
                free(guestImages);
                free(guestBuffers);
                free(buffer_width);
                free(buffer_height);
                break;
            }

            LOGD("Host: vkGetSwapchainImagesKHR guestImages %lld guestBuffers %lld", (long long)guestImages[0], (long long)guestBuffers[0]);
        }

        VkImage* images = malloc(sizeof(VkImage) * count);
        VkResult res = vkGetSwapchainImagesKHR(realDev, realSwapchain, &count, images);
        if (res != VK_SUCCESS) {
            LOGE("vkGetSwapchainImagesKHR failed: %d", res);
        } else {
            for (uint32_t i = 0; i < count; i++) {
                LOGD("count is %d, i is %d, guestImages[i] is %lld, guestBuffers[i] is %lld, images[i] is %lld",
                    count, i, guestImages[i], guestBuffers[i], (uint64_t)(uintptr_t)images[i]);

                insert_mapping(
                    EXPRESS_VK_OBJECT_TYPE_IMAGE,
                    guestImages[i],
                    (uint64_t)(uintptr_t)images[i]);
                LOGD("Host: vkGetSwapchainImagesKHR guest %llu mapped to host %lld",guestImages[i], (uint64_t)(uintptr_t)images[i]);

                Hardware_Buffer *gbuffer = get_gbuffer_from_global_map(guestBuffers[i]);
                if (gbuffer == NULL) {
                    gbuffer = create_gbuffer_from_vulkan(
                        buffer_width[i],
                        buffer_height[i],
                        guestBuffers[i],
                        images[i],
                        NULL,
                        realDev,
                        NULL,
                        0
                    );
                    if (gbuffer != NULL) {
                        add_gbuffer_to_global(gbuffer);
                    }
                }
            }
        }

        // vulkan_surface_register_swapchain_images(realDev, realSwapchain, guestImages, count);
        //if (need_free) free(stream);
        if (guestImages) free(guestImages);
        if (guestBuffers) free(guestBuffers);
        if (buffer_width) free(buffer_width);
        if (buffer_height) free(buffer_height);
    }
    break;

    case FUNID_vkEnumeratePhysicalDevices:
    {
        LOGD("Host: vkEnumeratePhysicalDevices request");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        LOGD("Host: vkEnumeratePhysicalDevices param count %d", para_num);
        if (para_num < 3) {
            LOGE("Host: vkEnumeratePhysicalDevices invalid param count %d", para_num);
            break;
        }

        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;
        uint64_t guest_inst = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);

        uint32_t count = 0;
        if (!copy_from_call_para_fast(all_para[1], &count, sizeof(uint32_t))) {
            LOGE("Host: vkEnumeratePhysicalDevices failed to read count");
            break;
        }

        VkInstance instance = (VkInstance)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_INSTANCE, guest_inst);
        LOGD("Host: vkEnumeratePhysicalDevices instance guest=%llu host=%p",
             (unsigned long long)guest_inst,
             (void *)instance);

        LOGD("Host: vkEnumeratePhysicalDevices input count %u", count);

        bool caller_provided_device_array =
            para_num >= 4 &&
            all_para[2].data != NULL &&
            all_para[2].data_len >= sizeof(uint64_t);
        bool enumerate_devices = caller_provided_device_array && count > 0;
        int result_para_index = para_num - 1;
        if (para_num >= 4 && !caller_provided_device_array) {
            LOGD("Host: vkEnumeratePhysicalDevices treating null device array as count query");
        }
        uint32_t requested_count = count;
        VkResult result = VK_SUCCESS;

        if (!enumerate_devices) {
            result = vkEnumeratePhysicalDevices(instance, &count, NULL);
            LOGD("Host: vkEnumeratePhysicalDevices count query result=%d count=%u",
                 result,
                 count);
            write_to_guest_mem(all_para[1].data, &count, 0, sizeof(uint32_t));
        } else {
            VkPhysicalDevice* devices = (VkPhysicalDevice*)malloc(requested_count * sizeof(VkPhysicalDevice));
            if (!devices) {
                result = VK_ERROR_OUT_OF_HOST_MEMORY;
            } else {
                uint64_t* guest_devs = (uint64_t*)malloc(requested_count * sizeof(uint64_t));
                if (!guest_devs) {
                    free(devices);
                    result = VK_ERROR_OUT_OF_HOST_MEMORY;
                } else {
                    if (!copy_from_call_para_fast(all_para[2], guest_devs, requested_count * sizeof(uint64_t))) {
                        free(devices);
                        free(guest_devs);
                        result = VK_ERROR_OUT_OF_HOST_MEMORY;
                        write_to_guest_mem(all_para[result_para_index].data, &result, 0, sizeof(VkResult));
                        break;
                    }

                    count = requested_count;
                    result = vkEnumeratePhysicalDevices(instance, &count, devices);
                    write_to_guest_mem(all_para[1].data, &count, 0, sizeof(uint32_t));
                    if (result == VK_SUCCESS || result == VK_INCOMPLETE) {
                        uint32_t mapped_count = count;
                        if (mapped_count > requested_count) {
                            LOGW("Host: vkEnumeratePhysicalDevices returned count %u > requested %u",
                                 mapped_count,
                                 requested_count);
                            mapped_count = requested_count;
                        }
                        if (mapped_count != 0) {
                            express_vk_sort_physical_devices_for_guest(devices, mapped_count);
                        }
                        for (uint32_t i = 0; i < mapped_count; ++i) {
                            uint64_t host_dev  = (uint64_t)(uintptr_t)devices[i];
                            VkPhysicalDeviceProperties props;
                            memset(&props, 0, sizeof(props));
                            vkGetPhysicalDeviceProperties(devices[i], &props);
                            insert_mapping(EXPRESS_VK_OBJECT_TYPE_PHYSICAL_DEVICE,
                                           guest_devs[i], host_dev);
                            LOGD("Host: vkEnumeratePhysicalDevices device guest and host %d %llx %lld name=%s vendor=0x%x type=%d",
                                 i,
                                 guest_devs[i],
                                 host_dev,
                                 props.deviceName,
                                 props.vendorID,
                                 props.deviceType);
                            LOGD("Host: physical device mapping index=%u guest=0x%llx host=%p name=%s vendor=0x%x device=0x%x type=%d",
                                 i,
                                 (unsigned long long)guest_devs[i],
                                 (void *)devices[i],
                                 props.deviceName,
                                 props.vendorID,
                                 props.deviceID,
                                 props.deviceType);
                        }
                    } else {
                        LOGE("Host: vkEnumeratePhysicalDevices failed result=%d", result);
                    }
                    free(guest_devs);
                }
                free(devices);
            }
        }
        write_to_guest_mem(all_para[result_para_index].data, &result, 0, sizeof(VkResult));

    }
    break;

case FUNID_vkCreateDevice: {
        LOGD("Host: vkCreateDevice request");
        express_vk_stats_reset("vkCreateDevice");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        LOGD("Host: vkCreateDevice para count = %d", para_num);

        int need_free = 0;
        char*     stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr    = (uint8_t**)&stream;

        VkDeviceCreateInfo* pCreateInfo = malloc(sizeof(VkDeviceCreateInfo));
        if (pCreateInfo == NULL) {
            VkResult result = VK_ERROR_OUT_OF_HOST_MEMORY;
            write_to_guest_mem(all_para[1].data, &result, 0, sizeof(VkResult));
            break;
        }
        decode_from_stream_VkDeviceCreateInfo(
            VK_STRUCTURE_TYPE_MAX_ENUM,
            pCreateInfo,
            ptr);
        LOGD("Host: vkCreateDevice decoded create info ptr=%p sType=%d pNext=%p queueCount=%u origExtCount=%u pEnabledFeatures=%p",
             (void *)pCreateInfo,
             pCreateInfo->sType,
             pCreateInfo->pNext,
             pCreateInfo->queueCreateInfoCount,
             pCreateInfo->enabledExtensionCount,
             pCreateInfo->pEnabledFeatures);

        VkAllocationCallbacks guestAllocStruct;
        const VkAllocationCallbacks* pAllocator = NULL;
        uint64_t guest_alloc_ptr = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        if (guest_alloc_ptr) {
            decode_from_stream_VkAllocationCallbacks(
                VK_STRUCTURE_TYPE_MAX_ENUM,
                &guestAllocStruct,
                ptr);
            pAllocator = &guestAllocStruct;
        }

        uint64_t guest_phys = *(uint64_t*)(*ptr);  *ptr += sizeof(uint64_t);
        uint64_t guest_dev  = *(uint64_t*)(*ptr);  *ptr += sizeof(uint64_t);
        LOGD("Host: vkCreateDevice guest handles guest_phys=0x%llx guest_dev=0x%llx guest_alloc=0x%llx",
             (unsigned long long)guest_phys,
             (unsigned long long)guest_dev,
             (unsigned long long)guest_alloc_ptr);

        // filter extensions
        uint32_t availCount = 0;
        VkPhysicalDevice physicalDevice = (VkPhysicalDevice)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_PHYSICAL_DEVICE, guest_phys);
        LOGD("Host: vkCreateDevice enumerate device extensions begin physicalDevice=%p",
             (void *)physicalDevice);
        VkResult ext_query_result =
            vkEnumerateDeviceExtensionProperties(physicalDevice, NULL, &availCount, NULL);
        LOGD("Host: vkCreateDevice extension count query result=%d count=%u",
             ext_query_result,
             availCount);
        if (ext_query_result != VK_SUCCESS) {
            VkResult result = ext_query_result;
            write_to_guest_mem(all_para[1].data, &result, 0, sizeof(VkResult));
            free(pCreateInfo);
            break;
        }
        VkExtensionProperties* availProps = malloc(sizeof(VkExtensionProperties) * availCount);
        if (availProps == NULL && availCount != 0) {
            VkResult result = VK_ERROR_OUT_OF_HOST_MEMORY;
            write_to_guest_mem(all_para[1].data, &result, 0, sizeof(VkResult));
            free(pCreateInfo);
            break;
        }
        VkResult ext_enum_result =
            vkEnumerateDeviceExtensionProperties(physicalDevice, NULL, &availCount, availProps);
        LOGD("Host: vkCreateDevice extension enum result=%d count=%u",
             ext_enum_result,
             availCount);
        if (ext_enum_result != VK_SUCCESS) {
            VkResult result = ext_enum_result;
            write_to_guest_mem(all_para[1].data, &result, 0, sizeof(VkResult));
            free(availProps);
            free(pCreateInfo);
            break;
        }
        for(int i = 0; i < availCount; i++) {
            LOGD("Host: Available device extension %d: %s", i, availProps[i].extensionName);
        }

        uint32_t origCount = pCreateInfo->enabledExtensionCount;
        const char* const* origExts = pCreateInfo->ppEnabledExtensionNames;
        const char** newExts = malloc(sizeof(char*) * (origCount + 4));
        if (newExts == NULL) {
            VkResult result = VK_ERROR_OUT_OF_HOST_MEMORY;
            write_to_guest_mem(all_para[1].data, &result, 0, sizeof(VkResult));
            free(availProps);
            free(pCreateInfo);
            break;
        }
        uint32_t newCount = 0;

        for (uint32_t i = 0; i < origCount; i++) {
            const char* ext = origExts[i];
            LOGD("Host: requested device extension[%u]: %s",
                 i,
                 ext ? ext : "(null)");
            if (ext != NULL && has_device_extension(availProps, availCount, ext)) {
                newExts[newCount++] = ext;
                LOGD("Host: accepted device extension[%u]: %s", i, ext);
            } else {
                LOGD("Host: filtered device extension[%u]: %s", i, ext ? ext : "(null)");
            }
        }
        if (has_device_extension(availProps, availCount,
                                 VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
            bool found = false;
            for (uint32_t i = 0; i < newCount; i++) {
                if (strcmp(newExts[i], VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                newExts[newCount++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
            }
        }

        #ifdef __APPLE__
            const char* required_interop_exts[] = {
                "VK_KHR_external_memory",
                "VK_EXT_metal_objects",
                "VK_EXT_external_memory_metal"
            };
            int num_exts = 3;
        #else
            const char* required_interop_exts[] = {
                "VK_KHR_external_memory",
                "VK_KHR_external_memory_win32"
            };
            int num_exts = 2;
        #endif

        for (int i = 0; i < num_exts; i++) {
            const char* ext = required_interop_exts[i];
            if (has_device_extension(availProps, availCount, ext)) {

                bool already_added = false;
                for (uint32_t j = 0; j < newCount; j++) {
                    if (strcmp(newExts[j], ext) == 0) {
                        already_added = true;
                        break;
                    }
                }
                if (!already_added) {
                    newExts[newCount++] = ext;
                    LOGD("Host: Added interop extension: %s", ext);
                }
            } else {
                LOGW("Host: Interop extension not available: %s", ext);
            }
        }

        pCreateInfo->enabledExtensionCount   = newCount;
        pCreateInfo->ppEnabledExtensionNames = newExts;

        LOGD("Host: vkCreateDevice final enabled extensions count: %u", newCount);
        for (uint32_t i = 0; i < newCount; i++) {
            LOGD("Host: final device extension[%u]: %s", i, newExts[i]);
        }

        free(availProps);


        VkPhysicalDevice realPD = (VkPhysicalDevice)(uintptr_t)
            lookup_mapping(
                EXPRESS_VK_OBJECT_TYPE_PHYSICAL_DEVICE,
                guest_phys);

        express_vk_log_device_create_info(pCreateInfo);

        VkDevice realDevice = VK_NULL_HANDLE;
        VkResult result = VK_ERROR_INITIALIZATION_FAILED;
        if ((uint64_t)(uintptr_t)realPD == UINT64_MAX ||
            realPD == VK_NULL_HANDLE) {
            LOGE("Host: vkCreateDevice invalid physical device mapping guest=%llu host=%p",
                 (unsigned long long)guest_phys,
                 (void *)realPD);
        } else {
            VkPhysicalDeviceProperties pd_props;
            memset(&pd_props, 0, sizeof(pd_props));
            vkGetPhysicalDeviceProperties(realPD, &pd_props);
            LOGD("Host: vkCreateDevice target physicalDevice=%p name=%s vendor=0x%x device=0x%x "
                 "api=0x%x driver=0x%x type=%d",
                 (void *)realPD,
                 pd_props.deviceName,
                 pd_props.vendorID,
                 pd_props.deviceID,
                 pd_props.apiVersion,
                 pd_props.driverVersion,
                 pd_props.deviceType);

            LOGD("Host: vkCreateDevice CALL_BEGIN realPD=%p guest_phys=%llu guest_dev=%llu extensions=%u pNext=%p",
                 (void *)realPD,
                 (unsigned long long)guest_phys,
                 (unsigned long long)guest_dev,
                 pCreateInfo->enabledExtensionCount,
                 pCreateInfo->pNext);
            result = vkCreateDevice(
                realPD,
                pCreateInfo,
                pAllocator,
                &realDevice);
            LOGD("Host: vkCreateDevice CALL_END result=%d realDevice=%p",
                 result,
                 (void *)realDevice);
        }

        if (result == VK_SUCCESS) {
            insert_mapping(
                EXPRESS_VK_OBJECT_TYPE_DEVICE,
                guest_dev,
                (uint64_t)(uintptr_t)realDevice);
            set_device_pd((uint64_t)(uintptr_t)realDevice, realPD);

            VkQueue graphicsQueue;
            vkGetDeviceQueue(realDevice, 0, 0, &graphicsQueue);
            set_device_graphics_queue((uint64_t)(uintptr_t)realDevice, (uint64_t)(uintptr_t)graphicsQueue);
            express_vk_remember_queue(realDevice, graphicsQueue);

            LOGI("Host: mapped Vulkan device guest=0x%llx host=%p",
                (unsigned long long)guest_dev,
                (void*)realDevice);
        } else {
            LOGE("vkCreateDevice failed: %d", result);
        }

        write_to_guest_mem(
            all_para[1].data,
            &result,
            0,
            sizeof(VkResult));

        // //if (need_free) free(stream);
#ifdef __WIN32__
        if (result == VK_SUCCESS && realDevice != VK_NULL_HANDLE) {
            init_interop_once(realDevice);
        }
#endif
        free(pCreateInfo);
        free(newExts);
    }
    break;

    case FUNID_vkGetDeviceQueue: {
        LOGD("Host: vkGetDeviceQueue");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        LOGD("Host vkGetDeviceQueue para_num=%d", para_num);

        int need_free = 0;
        char*      stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t**  ptr    = (uint8_t**)&stream;

        uint64_t guest_dev_handle  = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        uint32_t queueFamilyIndex  = *(uint32_t*)(*ptr);
        *ptr += sizeof(uint32_t);

        uint32_t queueIndex        = *(uint32_t*)(*ptr);
        *ptr += sizeof(uint32_t);

        uint64_t guest_queue_handle = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        VkDevice realDevice = (VkDevice)(uintptr_t)
            lookup_mapping(
                EXPRESS_VK_OBJECT_TYPE_DEVICE,
                guest_dev_handle);

        VkQueue realQueue;
        vkGetDeviceQueue(
            realDevice,
            queueFamilyIndex,
            queueIndex,
            &realQueue);

        insert_mapping(
            EXPRESS_VK_OBJECT_TYPE_QUEUE,
            guest_queue_handle,
            (uint64_t)(uintptr_t)realQueue);
        express_vk_remember_queue(realDevice, realQueue);


        // set_device_graphics_queue((uint64_t)(uintptr_t)realDevice, (uint64_t)(uintptr_t)realQueue);

        LOGD("guest queue %llu mapped to host %p",
            (unsigned long long)guest_queue_handle,
            (void*)realQueue);

        //if (need_free) free(stream);
    }
    break;

    case FUNID_vkCreateRenderPass: {
        LOGD("Host: vkCreateRenderPass");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        LOGD("vkCreateRenderPass para count = %d", para_num);

        int need_free = 0;
        char*     stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr    = (uint8_t**)&stream;

        VkRenderPassCreateInfo* pInfo = malloc(sizeof(VkRenderPassCreateInfo));
        decode_from_stream_VkRenderPassCreateInfo(
            VK_STRUCTURE_TYPE_MAX_ENUM,
            pInfo,
            ptr);

        uint64_t guest_alloc_ptr = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        VkAllocationCallbacks allocStruct;
        const VkAllocationCallbacks* pAllocator = NULL;
        if (guest_alloc_ptr) {
            decode_from_stream_VkAllocationCallbacks(
                VK_STRUCTURE_TYPE_MAX_ENUM,
                &allocStruct,
                ptr);
            pAllocator = &allocStruct;
        }

        uint64_t guest_dev = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        uint64_t guest_rp  = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        VkDevice realDev = (VkDevice)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_dev);

        VkRenderPass realRp;
        VkResult result = vkCreateRenderPass(
            realDev,
            pInfo,
            pAllocator,
            &realRp);

        if (result == VK_SUCCESS) {
            insert_mapping(
                EXPRESS_VK_OBJECT_TYPE_RENDER_PASS,
                guest_rp,
                (uint64_t)(uintptr_t)realRp);
            LOGD("Mapped RenderPass guest %llu -> host %p",
                (unsigned long long)guest_rp,
                (void*)realRp);
        } else {
            LOGE("vkCreateRenderPass failed: %d", result);
        }

        //if (need_free) free(stream);
        free(pInfo);
    }
    break;

    case FUNID_vkCreateImageView: {
        LOGD("Host: vkCreateImageView");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        LOGD("vkCreateImageView para count = %d", para_num);

        int need_free = 0;
        char*     stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr    = (uint8_t**)&stream;

        VkImageViewCreateInfo* pInfo = malloc(sizeof(VkImageViewCreateInfo));
        decode_from_stream_VkImageViewCreateInfo(
            VK_STRUCTURE_TYPE_MAX_ENUM,
            pInfo,
            ptr);

        LOGD("info image is %d %lld %d %d %d", pInfo->sType, (long long)pInfo->image, pInfo->viewType, pInfo->format, pInfo->components.r);

        uint64_t guest_alloc_ptr = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        VkAllocationCallbacks allocStruct;
        const VkAllocationCallbacks* pAllocator = NULL;
        if (guest_alloc_ptr) {
            decode_from_stream_VkAllocationCallbacks(
                VK_STRUCTURE_TYPE_MAX_ENUM,
                &allocStruct,
                ptr);
            pAllocator = &allocStruct;
        }

        uint64_t guest_dev  = *(uint64_t*)(*ptr);  *ptr += sizeof(uint64_t);
        uint64_t guest_iv   = *(uint64_t*)(*ptr);  *ptr += sizeof(uint64_t);

        VkDevice realDev = (VkDevice)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_dev);

        LOGD("Host: vkCreateImageView realDev %p", (void*)realDev);

        VkImageView realIv;
        VkResult result = vkCreateImageView(
            realDev,
            pInfo,
            pAllocator,
            &realIv);

        if (result == VK_SUCCESS) {
            insert_mapping(
                EXPRESS_VK_OBJECT_TYPE_IMAGE_VIEW,
                guest_iv,
                (uint64_t)(uintptr_t)realIv);
            set_imageview_to_image((uint64_t)(uintptr_t)realIv, (uint64_t)(uintptr_t)pInfo->image);
            LOGD("Mapped ImageView guest %llu -> host %p",
                (unsigned long long)guest_iv,
                (void*)realIv);
        } else {
            LOGE("vkCreateImageView failed: %d", result);
        }

        //if (need_free) free(stream);
        free(pInfo);
    }
    break;

    case FUNID_vkCreateFramebuffer: {
        LOGD("Host: vkCreateFramebuffer");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        LOGD("vkCreateFramebuffer para count = %d", para_num);

        int need_free = 0;
        char*     stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr    = (uint8_t**)&stream;

        VkFramebufferCreateInfo* pInfo = malloc(sizeof(VkFramebufferCreateInfo));
        decode_from_stream_VkFramebufferCreateInfo(
            VK_STRUCTURE_TYPE_MAX_ENUM,
            pInfo,
            ptr);

        uint64_t guest_alloc_ptr = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        VkAllocationCallbacks allocStruct;
        const VkAllocationCallbacks* pAllocator = NULL;
        if (guest_alloc_ptr) {
            decode_from_stream_VkAllocationCallbacks(
                VK_STRUCTURE_TYPE_MAX_ENUM,
                &allocStruct,
                ptr);
            pAllocator = &allocStruct;
        }

        uint64_t guest_dev = *(uint64_t*)(*ptr);  *ptr += sizeof(uint64_t);
        uint64_t guest_fb  = *(uint64_t*)(*ptr);  *ptr += sizeof(uint64_t);

        VkDevice realDev = (VkDevice)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_dev);

        VkFramebuffer realFb;
        VkResult result = vkCreateFramebuffer(
            realDev,
            pInfo,
            pAllocator,
            &realFb);

        if (result != VK_SUCCESS) {
            LOGE("vkCreateFramebuffer failed: %d", result);
        } else {
            insert_mapping(
                EXPRESS_VK_OBJECT_TYPE_FRAMEBUFFER,
                guest_fb,
                (uint64_t)(uintptr_t)realFb);
            LOGD("Mapped Framebuffer guest %llu -> host %p",
                (unsigned long long)guest_fb,
                (void*)realFb);
        }

        if (need_free)
            free(stream);
        free(pInfo);
    }
    break;

    case FUNID_vkCreateBuffer: {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);

        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        VkBufferCreateInfo* pInfo = malloc(sizeof(VkBufferCreateInfo));
        decode_from_stream_VkBufferCreateInfo(VK_STRUCTURE_TYPE_MAX_ENUM, pInfo, ptr);

        uint64_t guest_alloc_ptr = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        VkAllocationCallbacks allocStruct;
        const VkAllocationCallbacks* pAllocator = NULL;
        if (guest_alloc_ptr) {
            decode_from_stream_VkAllocationCallbacks(VK_STRUCTURE_TYPE_MAX_ENUM, &allocStruct, ptr);
            pAllocator = &allocStruct;
        }

        uint64_t guest_dev = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        uint64_t guest_buf = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        VkResult result = VK_ERROR_INITIALIZATION_FAILED;
        VkDevice realDev = (VkDevice)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_dev);
        if (realDev == VK_NULL_HANDLE) {
            LOGE("Host: vkCreateBuffer device mapping miss guest_dev=0x%llx guest_buffer=0x%llx",
                 (unsigned long long)guest_dev,
                 (unsigned long long)guest_buf);
        } else {
            VkBuffer realBuf = VK_NULL_HANDLE;
            result = vkCreateBuffer(realDev, pInfo, pAllocator, &realBuf);
            LOGD("pinfo is %d", (pInfo->usage & VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT) != 0);

            if (result == VK_SUCCESS) {
                insert_mapping(EXPRESS_VK_OBJECT_TYPE_BUFFER, guest_buf, (uint64_t)(uintptr_t)realBuf);
                LOGD("Mapped Buffer guest %llu -> host %p", (unsigned long long)guest_buf, (void*)realBuf);
            } else {
                LOGE("vkCreateBuffer failed: %d", result);
            }
        }

        if (para_num > 1 && all_para[1].data != NULL &&
            all_para[1].data_len >= sizeof(VkResult)) {
            write_to_guest_mem(all_para[1].data, &result, 0, sizeof(VkResult));
        }
        //if (need_free) free(stream);
        free(pInfo);
    }
    break;

    case FUNID_vkGetBufferMemoryRequirements: {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);

        int need_free = 0;
        char*     stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr    = (uint8_t**)&stream;

        uint64_t guest_dev = *(uint64_t*)(*ptr);  *ptr += sizeof(uint64_t);
        uint64_t guest_buf = *(uint64_t*)(*ptr);  *ptr += sizeof(uint64_t);

        VkMemoryRequirements req;
        VkDevice realDev = (VkDevice)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_dev);
        VkBuffer realBuf = (VkBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_BUFFER, guest_buf);
        LOGD("Host: vkGetBufferMemoryRequirements realDev %p realBuf %p", (void*)realDev, (void*)realBuf);

        if (realDev == VK_NULL_HANDLE || realBuf == VK_NULL_HANDLE) {
            memset(&req, 0, sizeof(req));
            LOGE("Host: vkGetBufferMemoryRequirements mapping miss guest_dev=0x%llx guest_buffer=0x%llx host_dev=%p host_buffer=%p",
                 (unsigned long long)guest_dev,
                 (unsigned long long)guest_buf,
                 (void*)realDev,
                 (void*)realBuf);
            write_to_guest_mem(all_para[1].data, &req, 0, sizeof(VkMemoryRequirements));
            break;
        }

        vkGetBufferMemoryRequirements(realDev, realBuf, &req);
        LOGD("Host: vkGetBufferMemoryRequirements size=%llu alignment=%llu memoryTypeBits=0x%x",
            (unsigned long long)req.size,
            (unsigned long long)req.alignment,
            req.memoryTypeBits);
        log_ncnn_align_classified("vkGetBufferMemoryRequirements",
            req.alignment,
            guest_dev,
            realDev,
            guest_buf,
            (uint64_t)(uintptr_t)realBuf);
        if (req.alignment == 0) {
            LOGE("SEARCH_ME_ALIGNMENT_ZERO: vkGetBufferMemoryRequirements alignment==0, size=%llu memoryTypeBits=0x%x",
                (unsigned long long)req.size,
                req.memoryTypeBits);
        }

        LOGD("size is %d", sizeof(VkMemoryRequirements));

        write_to_guest_mem(
            all_para[1].data,
            &req,
            0,
            sizeof(VkMemoryRequirements));

        //if (need_free) free(stream);
    }
    break;

    case FUNID_vkAllocateMemory: {
        LOGD("Host: vkAllocateMemory");
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        LOGD("Host: vkAllocateMemory para_num=%d", para_num);

        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;
        LOGD("Host: vkAllocateMemory stream=%p need_free=%d", (void*)stream, need_free);

        VkMemoryAllocateInfo* pInfo = malloc(sizeof(VkMemoryAllocateInfo));
        decode_from_stream_VkMemoryAllocateInfo(VK_STRUCTURE_TYPE_MAX_ENUM, pInfo, ptr);
        LOGD("Host: vkAllocateMemory decoded pInfo=%p sType=%d pNext=%p allocationSize=%llu memoryTypeIndex=%u",
            (void*)pInfo,
            pInfo ? pInfo->sType : -1,
            pInfo ? pInfo->pNext : NULL,
            (unsigned long long)(pInfo ? pInfo->allocationSize : 0),
            pInfo ? pInfo->memoryTypeIndex : 0);
        if (pInfo && pInfo->sType != VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO) {
            LOGE("Host: vkAllocateMemory unexpected VkMemoryAllocateInfo.sType=%d (expected=%d)",
                pInfo->sType,
                VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
        }

        uint64_t guest_alloc_ptr = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        VkAllocationCallbacks allocStruct;
        const VkAllocationCallbacks* pAllocator = NULL;
        LOGD("Host: vkAllocateMemory guest_alloc_ptr=0x%llx", (unsigned long long)guest_alloc_ptr);
        if (guest_alloc_ptr) {
            decode_from_stream_VkAllocationCallbacks(VK_STRUCTURE_TYPE_MAX_ENUM, &allocStruct, ptr);
            pAllocator = &allocStruct;
            LOGD("Host: vkAllocateMemory allocator pUserData=%p pfnAllocation=%p pfnReallocation=%p pfnFree=%p pfnInternalAllocation=%p pfnInternalFree=%p",
                allocStruct.pUserData,
                (void*)allocStruct.pfnAllocation,
                (void*)allocStruct.pfnReallocation,
                (void*)allocStruct.pfnFree,
                (void*)allocStruct.pfnInternalAllocation,
                (void*)allocStruct.pfnInternalFree);
        }

        uint64_t gbuffer_id = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint32_t buffer_width = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);
        uint32_t buffer_height = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);
        uint64_t buffer_handle = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);

        uint64_t guest_dev  = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint64_t guest_mem  = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        LOGD("Host: vkAllocateMemory guest handles guest_dev=0x%llx guest_mem=0x%llx gbuffer_id=0x%llx buffer=%ux%u buffer_handle=0x%llx",
            (unsigned long long)guest_dev,
            (unsigned long long)guest_mem,
            (unsigned long long)gbuffer_id,
            buffer_width,
            buffer_height,
            (unsigned long long)buffer_handle);

        VkDevice realDev = (VkDevice)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_dev);
        LOGD("Host: vkAllocateMemory mapped device guest=0x%llx -> host=%p",
            (unsigned long long)guest_dev,
            (void*)realDev);
        if (realDev == VK_NULL_HANDLE) {
            LOGE("Host: vkAllocateMemory DEVICE_MAPPING_MISS guest_dev=0x%llx, host device is NULL (this will trigger VUID-vkAllocateMemory-device-parameter)",
                (unsigned long long)guest_dev);
        }

        /*
         * Preserve the guest-selected memory type. Device-local allocations
         * may not be host visible; forcing host-visible memory changes
         * allocation semantics and performance. Callers that need CPU
         * mapping use staging resources.
         */
        VkPhysicalDevice hostPD = get_device_pd((uint64_t)(uintptr_t)realDev);
        LOGD("Host: vkAllocateMemory device->physicalDevice hostPD=%p", (void*)hostPD);
        if (hostPD != VK_NULL_HANDLE) {
            VkPhysicalDeviceMemoryProperties memProps;
            vkGetPhysicalDeviceMemoryProperties(hostPD, &memProps);
            LOGD("Host: vkAllocateMemory memProps memoryTypeCount=%u memoryHeapCount=%u",
                memProps.memoryTypeCount,
                memProps.memoryHeapCount);

            uint32_t reqType = pInfo->memoryTypeIndex;
            if (reqType >= memProps.memoryTypeCount) {
                LOGE("Host: vkAllocateMemory invalid memoryTypeIndex=%u, memoryTypeCount=%u",
                    reqType,
                    memProps.memoryTypeCount);
            } else {
                VkMemoryPropertyFlags flags =
                    memProps.memoryTypes[reqType].propertyFlags;
                LOGD("Requested memoryTypeIndex=%u flags=0x%x",
                    reqType, flags);

                for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
                    LOGD("  memType[%u] flags=0x%x heapIndex=%u",
                        i,
                        memProps.memoryTypes[i].propertyFlags,
                        memProps.memoryTypes[i].heapIndex);
                }

                if (!(flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
                    // Keep device-local allocations unchanged; mapped callers use staging.

                    /*
                    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
                        if (memProps.memoryTypes[i].propertyFlags &
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
                            LOGD("Override memoryTypeIndex %u -> %u (HOST_VISIBLE)",
                                reqType, i);
                            pInfo->memoryTypeIndex = i;
                            break;
                        }
                    }
                    */
                }
            }
        } else {
            LOGE("Host: vkAllocateMemory missing hostPD for host device=%p", (void*)realDev);
        }

        VkDeviceMemory realMem;

        if (gbuffer_id != 0) {
            Hardware_Buffer* gbuffer = get_gbuffer_from_global_map(gbuffer_id);
            if (gbuffer == NULL) {
                gbuffer = create_gbuffer_from_vulkan(
                    buffer_width,
                    buffer_height,
                    gbuffer_id,
                    NULL,
                    NULL,
                    realDev,
                    NULL,
                    buffer_handle
                );
                if (gbuffer != NULL) {
                    add_gbuffer_to_global(gbuffer);
                }
            }

            VkExportMemoryAllocateInfo exportInfo = {};
            exportInfo.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
            exportInfo.pNext = pInfo->pNext;

            #ifdef __APPLE__
                    exportInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLTEXTURE_BIT_EXT;
            #else
                    exportInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
            #endif

            pInfo->pNext = &exportInfo;
            LOGD("Host: vkAllocateMemory(shared) call args realDev=%p pInfo=%p allocSize=%llu memTypeIndex=%u pInfo->pNext=%p exportInfo.handleTypes=0x%x pAllocator=%p",
                (void*)realDev,
                (void*)pInfo,
                (unsigned long long)pInfo->allocationSize,
                pInfo->memoryTypeIndex,
                pInfo->pNext,
                exportInfo.handleTypes,
                (void*)pAllocator);

            VkResult result = vkAllocateMemory(realDev, pInfo, pAllocator, &realMem);

            if (result == VK_SUCCESS) {
                insert_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE_MEMORY, guest_mem, (uint64_t)(uintptr_t)realMem);
                insert_gbuffer_memory_mapping(gbuffer_id, (uint64_t)(uintptr_t)realMem);
                LOGD("Mapped shared DeviceMemory guest %llu -> host %p, gbuffer_id=%llx",
                    (unsigned long long)guest_mem, (void*)realMem, (unsigned long long)gbuffer_id);
            } else {
                LOGE("vkAllocateMemory(shared) failed: %d (realDev=%p allocSize=%llu memTypeIndex=%u)",
                    result,
                    (void*)realDev,
                    (unsigned long long)pInfo->allocationSize,
                    pInfo->memoryTypeIndex);
            }
        } else {
            LOGD("Host: vkAllocateMemory(normal) call args realDev=%p pInfo=%p allocSize=%llu memTypeIndex=%u pInfo->pNext=%p pAllocator=%p",
                (void*)realDev,
                (void*)pInfo,
                (unsigned long long)pInfo->allocationSize,
                pInfo->memoryTypeIndex,
                pInfo->pNext,
                (void*)pAllocator);
            VkResult result = vkAllocateMemory(realDev, pInfo, pAllocator, &realMem);

            if (result == VK_SUCCESS) {
                insert_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE_MEMORY, guest_mem, (uint64_t)(uintptr_t)realMem);
                LOGD("Mapped DeviceMemory guest %llu -> host %p", (unsigned long long)guest_mem, (void*)realMem);
            } else {
                LOGE("vkAllocateMemory(normal) failed: %d (realDev=%p allocSize=%llu memTypeIndex=%u)",
                    result,
                    (void*)realDev,
                    (unsigned long long)pInfo->allocationSize,
                    pInfo->memoryTypeIndex);
            }
        }

        //if (need_free) free(stream);
        free(pInfo);
    }
    break;

    case FUNID_vkExpressRegisterMappedMemoryANDROID:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        if (para_num < 2) {
            LOGE("[ExpressVkMem] register missing params para_num=%d", para_num);
            break;
        }

        int need_free = 0;
        char *stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t **ptr = (uint8_t **)&stream;

        uint64_t guest_device = *(uint64_t *)(*ptr); *ptr += sizeof(uint64_t);
        uint64_t guest_memory = *(uint64_t *)(*ptr); *ptr += sizeof(uint64_t);
        uint64_t size = *(uint64_t *)(*ptr); *ptr += sizeof(uint64_t);
        (void)guest_device;

        uint64_t host_memory = lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE_MEMORY, guest_memory);
        Guest_Mem *guest_mem = copy_guest_mem_from_call(call, 2);
        express_vk_register_memory(guest_memory, host_memory, size, guest_mem);
    }
    break;

    case FUNID_vkExpressUnregisterMappedMemoryANDROID:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        if (para_num < 1) {
            LOGE("[ExpressVkMem] unregister missing params para_num=%d", para_num);
            break;
        }

        int need_free = 0;
        char *stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t **ptr = (uint8_t **)&stream;

        uint64_t guest_device = *(uint64_t *)(*ptr); *ptr += sizeof(uint64_t);
        uint64_t guest_memory = *(uint64_t *)(*ptr); *ptr += sizeof(uint64_t);
        (void)guest_device;

        uint64_t host_memory = lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE_MEMORY, guest_memory);
        express_vk_unregister_memory(guest_memory, host_memory);
    }
    break;

    case FUNID_vkMapMemory: {
                struct timespec t0_sync, t1_sync;
        clock_gettime(CLOCK_MONOTONIC, &t0_sync);

        if (EXPRESS_VK_ENABLE_SYNC_DIAG_LOG) {
            LOGD("Host: vkMapMemory");
        }
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);

        int need_free = 0;
        char*     stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr    = (uint8_t**)&stream;

        uint64_t guest_dev  = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint64_t guest_mem  = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        VkDeviceSize offset = *(VkDeviceSize*)(*ptr); *ptr += sizeof(VkDeviceSize);
        VkDeviceSize size   = *(VkDeviceSize*)(*ptr); *ptr += sizeof(VkDeviceSize);
        VkMemoryMapFlags flags = *(VkMemoryMapFlags*)(*ptr); *ptr += sizeof(VkMemoryMapFlags);
        if (EXPRESS_VK_ENABLE_SYNC_DIAG_LOG) {
            LOGD("size of flags is %d", sizeof(VkMemoryMapFlags));
        }

        // void** guest_ppData;
        // read_from_guest_mem(all_para[5].data, &guest_ppData, 0, sizeof(void*));

        VkDevice realDev = (VkDevice)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_dev);
        VkDeviceMemory realMem = (VkDeviceMemory)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE_MEMORY, guest_mem);

        void* mappedPtr = NULL;
        // struct timespec t0_sync, t1_sync;
        // clock_gettime(CLOCK_MONOTONIC, &t0_sync);
        VkResult result = vkMapMemory(realDev, realMem, offset, size, flags, &mappedPtr);
        // clock_gettime(CLOCK_MONOTONIC, &t1_sync);
        // LOGD("[SYNC_TIME] vkMapMemory real vkMapMemory call took %.3f ms", (t1_sync.tv_sec - t0_sync.tv_sec) * 1000.0 + (t1_sync.tv_nsec - t0_sync.tv_nsec) / 1000000.0);
        if (EXPRESS_VK_ENABLE_SYNC_DIAG_LOG) {
            LOGD("real dev %p real mem %p offset %d size %d flags %d",
                (void*)realDev, (void*)realMem, offset, size, flags);
        }
        if (result != VK_SUCCESS) {
            LOGE("vkMapMemory failed: %d", result);
        } else {
            // write_to_guest_mem(all_para[5].data, &mappedPtr, 0, sizeof(void*));
            set_memory_map((uint64_t)realMem, mappedPtr);
            if (EXPRESS_VK_ENABLE_SYNC_DIAG_LOG) {
                LOGD("Mapped memory guest %llu -> host %p",
                    (unsigned long long)guest_mem,
                    mappedPtr);
            }

            // if (para_num > 1 && all_para[1].data_len > 0 && mappedPtr) {
            //     struct timespec t0_sync, t1_sync;
            //     clock_gettime(CLOCK_MONOTONIC, &t0_sync);

            //     write_to_guest_mem(
            //         all_para[1].data,
            //         mappedPtr,
            //         0,
            //         all_para[1].data_len);

            //     clock_gettime(CLOCK_MONOTONIC, &t1_sync);
            //     double sync_ms = (t1_sync.tv_sec - t0_sync.tv_sec) * 1000.0 + (t1_sync.tv_nsec - t0_sync.tv_nsec) / 1000000.0;
            //     LOGD("[SYNC_TIME] vkMapMemory synced %zu bytes to guest in %.3f ms", (size_t)all_para[1].data_len, sync_ms);
            // }
        }
        clock_gettime(CLOCK_MONOTONIC, &t1_sync);
        if (EXPRESS_VK_ENABLE_SYNC_DIAG_LOG) {
            LOGD("[SYNC_TIME] vkMapMemory! real vkMapMemory call took %.3f ms", (t1_sync.tv_sec - t0_sync.tv_sec) * 1000.0 + (t1_sync.tv_nsec - t0_sync.tv_nsec) / 1000000.0);
        }
        //if (need_free) free(stream);
    }
    break;

    case FUNID_vkUnmapMemory: {
        if (EXPRESS_VK_ENABLE_SYNC_DIAG_LOG) {
            LOGD("Host: vkUnmapMemory");
        }
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);

        int need_free = 0;
        char*     stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr    = (uint8_t**)&stream;

        uint64_t guest_dev = *(uint64_t*)(*ptr);  *ptr += sizeof(uint64_t);
        uint64_t guest_mem = *(uint64_t*)(*ptr);  *ptr += sizeof(uint64_t);

        VkDevice realDev = (VkDevice)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_dev);
        VkDeviceMemory realMem = (VkDeviceMemory)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE_MEMORY, guest_mem);

        void* hostPtr = get_memory_map((uint64_t)realMem);
        ExpressVkRegisteredMemory *reg = express_vk_find_registered_memory(guest_mem, (uint64_t)(uintptr_t)realMem);
        if (reg && reg->guest_mem) {
            if (EXPRESS_VK_ENABLE_SYNC_DIAG_LOG) {
                LOGD("[ExpressVkMem] vkUnmapMemory skip registered full sync size=%llu guest=0x%llx host=0x%llx",
                     (unsigned long long)reg->size,
                     (unsigned long long)guest_mem,
                     (unsigned long long)(uintptr_t)realMem);
            }
        } else if (hostPtr && para_num > 1) {

            // all_para[1].data_len supplies mem->length.
            struct timespec t0_sync, t1_sync;
            clock_gettime(CLOCK_MONOTONIC, &t0_sync);

            read_from_guest_mem(
                all_para[1].data,
                hostPtr,
                0,
                all_para[1].data_len);

            clock_gettime(CLOCK_MONOTONIC, &t1_sync);
            double sync_ms = (t1_sync.tv_sec - t0_sync.tv_sec) * 1000.0 + (t1_sync.tv_nsec - t0_sync.tv_nsec) / 1000000.0;
            if (EXPRESS_VK_ENABLE_SYNC_DIAG_LOG) {
                LOGD("[SYNC_TIME] vkUnmapMemory synced %zu bytes to host in %.3f ms", (size_t)all_para[1].data_len, sync_ms);
            }
        } else {
            LOGE("Host: no mapping found for guest_mem %llu", guest_mem);
        }

        vkUnmapMemory(realDev, realMem);

        //if (need_free) free(stream);
    }
    break;

    case FUNID_vkBindBufferMemory: {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);

        int need_free = 0;
        char*     stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr    = (uint8_t**)&stream;

        uint64_t guest_dev        = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint64_t guest_buffer     = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint64_t guest_memory     = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        VkDeviceSize   memoryOffset = *(VkDeviceSize*)(*ptr); *ptr += sizeof(VkDeviceSize);

        VkDevice realDev = (VkDevice)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_dev);
        VkBuffer realBuffer = (VkBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_BUFFER, guest_buffer);
        VkDeviceMemory realMemory = (VkDeviceMemory)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE_MEMORY, guest_memory);

        if (realDev == VK_NULL_HANDLE ||
            realBuffer == VK_NULL_HANDLE ||
            realMemory == VK_NULL_HANDLE) {
            LOGE("Host: vkBindBufferMemory mapping miss guest_dev=0x%llx guest_buffer=0x%llx guest_memory=0x%llx host_dev=%p host_buffer=%p host_memory=%p",
                 (unsigned long long)guest_dev,
                 (unsigned long long)guest_buffer,
                 (unsigned long long)guest_memory,
                 (void*)realDev,
                 (void*)realBuffer,
                 (void*)realMemory);
            break;
        }

        VkResult result = vkBindBufferMemory(
            realDev,
            realBuffer,
            realMemory,
            memoryOffset);

        if (result != VK_SUCCESS) {
            LOGE("vkBindBufferMemory failed: %d", result);
        } else {
            LOGD("vkBindBufferMemory Bound buffer %llu to memory %llu",
                (unsigned long long)guest_buffer,
                (unsigned long long)guest_memory);
        }

        //if (need_free) free(stream);
    }
    break;

    case FUNID_vkCreateShaderModule: {
        LOGD("Host: vkCreateShaderModule");
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);

        int need_free = 0;
        char*     stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr    = (uint8_t**)&stream;

        VkShaderModuleCreateInfo* pInfo = malloc(sizeof(VkShaderModuleCreateInfo));
        decode_from_stream_VkShaderModuleCreateInfo(
            VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            pInfo,
            ptr);

        uint64_t guest_alloc_ptr = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        VkAllocationCallbacks allocStruct;
        const VkAllocationCallbacks* pAllocator = NULL;
        if (guest_alloc_ptr) {
            decode_from_stream_VkAllocationCallbacks(
                VK_STRUCTURE_TYPE_MAX_ENUM,
                &allocStruct,
                ptr);
            pAllocator = &allocStruct;
        }

        uint64_t guest_dev     = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint64_t guest_module  = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);

        VkDevice realDev = (VkDevice)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_dev);

        VkShaderModule realModule;
        VkResult result = vkCreateShaderModule(
            realDev,
            pInfo,
            pAllocator,
            &realModule);

        if (result == VK_SUCCESS) {
            insert_mapping(
                EXPRESS_VK_OBJECT_TYPE_SHADER_MODULE,
                guest_module,
                (uint64_t)(uintptr_t)realModule);
            LOGD("Mapped ShaderModule guest %llu -> host %p",
                (unsigned long long)guest_module,
                (void*)realModule);
        } else {
            LOGE("vkCreateShaderModule failed: %d", result);
        }
        //if (need_free) free(stream);
        free(pInfo);
    }
    break;

    case FUNID_vkCreatePipelineLayout: {
        LOGD("Host: vkCreatePipelineLayout");
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);

        int need_free = 0;
        char*     stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr    = (uint8_t**)&stream;

        VkPipelineLayoutCreateInfo* pInfo = malloc(sizeof(VkPipelineLayoutCreateInfo));
        decode_from_stream_VkPipelineLayoutCreateInfo(
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            pInfo,
            ptr);

        uint64_t guest_alloc_ptr = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        VkAllocationCallbacks allocStruct;
        const VkAllocationCallbacks* pAllocator = NULL;
        if (guest_alloc_ptr) {
            decode_from_stream_VkAllocationCallbacks(
                VK_STRUCTURE_TYPE_MAX_ENUM,
                &allocStruct,
                ptr);
            pAllocator = &allocStruct;
        }

        uint64_t guest_dev     = *(uint64_t*)(*ptr);  *ptr += sizeof(uint64_t);
        uint64_t guest_layout  = *(uint64_t*)(*ptr);  *ptr += sizeof(uint64_t);

        VkDevice realDev = (VkDevice)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_dev);

        VkPipelineLayout realLayout;
        VkResult result = vkCreatePipelineLayout(
            realDev,
            pInfo,
            pAllocator,
            &realLayout);

        if (result == VK_SUCCESS) {
            insert_mapping(
                EXPRESS_VK_OBJECT_TYPE_PIPELINE_LAYOUT,
                guest_layout,
                (uint64_t)(uintptr_t)realLayout);
            LOGD("Mapped PipelineLayout guest %llu -> host %p",
                (unsigned long long)guest_layout,
                (void*)realLayout);
        } else {
            LOGE("vkCreatePipelineLayout failed: %d", result);
        }

        //if (need_free) free(stream);
        free(pInfo);
    }
    break;

    case FUNID_vkCreatePipelineCache: {
        LOGD("Host: vkCreatePipelineCache");
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);

        int need_free = 0;
        char*     stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr    = (uint8_t**)&stream;

        VkPipelineCacheCreateInfo* pInfo = malloc(sizeof(VkPipelineCacheCreateInfo));
        decode_from_stream_VkPipelineCacheCreateInfo(
            VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
            pInfo,
            ptr);

        uint64_t guest_alloc_ptr = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        VkAllocationCallbacks allocStruct;
        const VkAllocationCallbacks* pAllocator = NULL;
        if (guest_alloc_ptr) {
            decode_from_stream_VkAllocationCallbacks(
                VK_STRUCTURE_TYPE_MAX_ENUM,
                &allocStruct,
                ptr);
            pAllocator = &allocStruct;
        }

        uint64_t guest_dev    = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint64_t guest_cache  = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);

        VkDevice realDev = (VkDevice)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_dev);

        VkPipelineCache realCache;
        VkResult result = vkCreatePipelineCache(
            realDev,
            pInfo,
            pAllocator,
            &realCache);

        if (result == VK_SUCCESS) {
            insert_mapping(
                EXPRESS_VK_OBJECT_TYPE_PIPELINE_CACHE,
                guest_cache,
                (uint64_t)(uintptr_t)realCache);
            LOGD("Mapped PipelineCache guest %llu -> host %p",
                (unsigned long long)guest_cache,
                (void*)realCache);
        } else {
            LOGE("vkCreatePipelineCache failed: %d", result);
        }

        //if (need_free) free(stream);
        free(pInfo);
    }
    break;

    case FUNID_vkCreateGraphicsPipelines: {
        LOGD("Host: vkCreateGraphicsPipelines request");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);

        int need_free = 0;
        char*     stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr    = (uint8_t**)&stream;

        uint64_t guest_dev     = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint64_t guest_pipelineCache   = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint32_t createInfoCount     = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);
        // createInfoCount     = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);
        LOGD("Host: vkCreateGraphicsPipelines createInfoCount = %u guest dev %lld cache %lld", createInfoCount, (long long)guest_dev, (long long)guest_pipelineCache);

        // 2) Decode each VkGraphicsPipelineCreateInfo from the buffer
        VkGraphicsPipelineCreateInfo* infos =
            malloc(sizeof(VkGraphicsPipelineCreateInfo) * createInfoCount);
        for (uint32_t i = 0; i < createInfoCount; i++) {
            memset(&infos[i], 0, sizeof(VkGraphicsPipelineCreateInfo));
            decode_from_stream_VkGraphicsPipelineCreateInfo(
                VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                &infos[i],
                ptr);
        }
        LOGD("Decoded %u VkGraphicsPipelineCreateInfo structures", createInfoCount);

        // 3) Decode allocator pointer and callbacks at end of buffer
        uint64_t guest_alloc_ptr = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        VkAllocationCallbacks allocStruct, *pAllocator = NULL;
        LOGD("Guest allocator pointer: %llu", (unsigned long long)guest_alloc_ptr);
        if (guest_alloc_ptr) {
            decode_from_stream_VkAllocationCallbacks(
                VK_STRUCTURE_TYPE_MAX_ENUM,
                &allocStruct,
                ptr);
            pAllocator = &allocStruct;
        }

        // //if (need_free) free(stream);

        // 4) Map guest handles to host
        VkDevice       realDev    = (VkDevice)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_dev);
        VkPipelineCache realCache = guest_pipelineCache ?
            (VkPipelineCache)(uintptr_t)lookup_mapping(
                EXPRESS_VK_OBJECT_TYPE_PIPELINE_CACHE,
                guest_pipelineCache) :
            VK_NULL_HANDLE;

        // 5) Call the real Vulkan function
        VkPipeline* hostPipelines = malloc(sizeof(VkPipeline) * createInfoCount);
        VkResult result = vkCreateGraphicsPipelines(
            realDev,
            realCache,
            createInfoCount,
            infos,
            pAllocator,
            hostPipelines);

        // 6) On error, log; on success, insert mappings
        if (result != VK_SUCCESS) {
            LOGE("vkCreateGraphicsPipelines failed: %d", result);
        } else {
            for (uint32_t i = 0; i < createInfoCount; i++) {
                uint64_t guest_pipe = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
                insert_mapping(
                    EXPRESS_VK_OBJECT_TYPE_PIPELINE,
                    guest_pipe,
                    (uint64_t)(uintptr_t)hostPipelines[i]);
                LOGD("Mapped GraphicsPipeline guest %llu -> host %p",
                    (unsigned long long)guest_pipe,
                    (void*)hostPipelines[i]);
            }
            LOGD("Mapped %u VkPipelines", createInfoCount);
        }

        free(infos);
        free(hostPipelines);
    }
    break;

    case FUNID_vkCreateCommandPool: {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        VkCommandPoolCreateInfo* pInfo = malloc(sizeof(VkCommandPoolCreateInfo));
        decode_from_stream_VkCommandPoolCreateInfo(VK_STRUCTURE_TYPE_MAX_ENUM, pInfo, ptr);

        uint64_t guest_alloc_ptr = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        VkAllocationCallbacks allocStruct;
        const VkAllocationCallbacks* pAllocator = NULL;
        if (guest_alloc_ptr) {
            decode_from_stream_VkAllocationCallbacks(VK_STRUCTURE_TYPE_MAX_ENUM, &allocStruct, ptr);
            pAllocator = &allocStruct;
        }

        uint64_t guest_dev = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        uint64_t guest_pool = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        VkDevice realDev = (VkDevice)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_dev);
        VkCommandPool realPool;

        VkResult result = vkCreateCommandPool(realDev, pInfo, pAllocator, &realPool);
        if (para_num > 1) {
            write_to_guest_mem(all_para[1].data, &result, 0,
                               sizeof(result));
        }

        if (result == VK_SUCCESS) {
            insert_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_POOL, guest_pool, (uint64_t)(uintptr_t)realPool);
            LOGD("Mapped CommandPool guest %llu -> host %p", (unsigned long long)guest_pool, (void*)realPool);
        } else {
            LOGE("vkCreateCommandPool failed: %d", result);
        }

        //if (need_free) free(stream);
        free(pInfo);
    }
    break;

    case FUNID_vkAllocateCommandBuffers: {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        VkCommandBufferAllocateInfo* pInfo = malloc(sizeof(VkCommandBufferAllocateInfo));
        decode_from_stream_VkCommandBufferAllocateInfo(VK_STRUCTURE_TYPE_MAX_ENUM, pInfo, ptr);

        uint64_t guest_dev = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        VkDevice realDev = (VkDevice)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_dev);

        VkCommandBuffer* realCmdBufs = malloc(pInfo->commandBufferCount * sizeof(VkCommandBuffer));
        LOGD("pinfo values commandBufferCount %d commandPool %p level %d",
            pInfo->commandBufferCount, (void*)pInfo->commandPool, pInfo->level);
        VkResult result = vkAllocateCommandBuffers(realDev, pInfo, realCmdBufs);
        if (para_num > 1) {
            write_to_guest_mem(all_para[1].data, &result, 0,
                               sizeof(result));
        }

        if (result == VK_SUCCESS) {
            for (uint32_t i = 0; i < pInfo->commandBufferCount; ++i) {
                uint64_t guest_cmd_buf = *(uint64_t*)(*ptr);
                *ptr += sizeof(uint64_t);

                insert_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd_buf,
                            (uint64_t)(uintptr_t)realCmdBufs[i]);
                LOGD("Mapped CommandBuffer %d guest %llu -> host %p", i, (unsigned long long)guest_cmd_buf, (void*)realCmdBufs[i]);
            }
        } else {
            LOGE("vkAllocateCommandBuffers failed: %d", result);
        }

        free(realCmdBufs);
        //if (need_free) free(stream);
        free(pInfo);
    }
    break;

    // Host-side vkBeginCommandBuffer.
    case FUNID_vkBeginCommandBuffer: {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        VkCommandBufferBeginInfo* pInfo = malloc(sizeof(VkCommandBufferBeginInfo));
        decode_from_stream_VkCommandBufferBeginInfo(VK_STRUCTURE_TYPE_MAX_ENUM, pInfo, ptr);

        uint64_t guest_cmd_buf = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        VkCommandBuffer realCmdBuf = (VkCommandBuffer)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd_buf);

        VkResult result = vkBeginCommandBuffer(realCmdBuf, pInfo);
        if (para_num > 1) {
            write_to_guest_mem(all_para[1].data, &result, 0,
                               sizeof(result));
        }

        if (result == VK_SUCCESS) {
            LOGD("BeginCommandBuffer success guest %llu -> host %p", (unsigned long long)guest_cmd_buf, (void*)realCmdBuf);
        } else {
            LOGE("vkBeginCommandBuffer failed: %d", result);
        }

        //if (need_free) free(stream);
        free(pInfo);
    }
    break;

    case FUNID_vkCmdPipelineBarrier: {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd_buf = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        VkPipelineStageFlags srcStageMask = *(uint32_t*)(*ptr);
        *ptr += sizeof(uint32_t);
        VkPipelineStageFlags dstStageMask = *(uint32_t*)(*ptr);
        *ptr += sizeof(uint32_t);
        VkDependencyFlags dependencyFlags = *(uint32_t*)(*ptr);
        *ptr += sizeof(uint32_t);

        uint32_t memoryBarrierCount = *(uint32_t*)(*ptr);
        *ptr += sizeof(uint32_t);
        uint32_t bufferMemoryBarrierCount = *(uint32_t*)(*ptr);
        *ptr += sizeof(uint32_t);
        uint32_t imageMemoryBarrierCount = *(uint32_t*)(*ptr);
        *ptr += sizeof(uint32_t);

        VkMemoryBarrier* memBarriers = NULL;
        if (memoryBarrierCount > 0) {
            memBarriers = malloc(memoryBarrierCount * sizeof(VkMemoryBarrier));
            for (uint32_t i = 0; i < memoryBarrierCount; ++i) {
                decode_from_stream_VkMemoryBarrier(VK_STRUCTURE_TYPE_MAX_ENUM, &memBarriers[i], ptr);
            }
        }

        VkBufferMemoryBarrier* bufBarriers = NULL;
        if (bufferMemoryBarrierCount > 0) {
            bufBarriers = malloc(bufferMemoryBarrierCount * sizeof(VkBufferMemoryBarrier));
            for (uint32_t i = 0; i < bufferMemoryBarrierCount; ++i) {
                decode_from_stream_VkBufferMemoryBarrier(VK_STRUCTURE_TYPE_MAX_ENUM, &bufBarriers[i], ptr);
                // bufBarriers[i].buffer = (VkBuffer)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_BUFFER, (uint64_t)(uintptr_t)bufBarriers[i].buffer);
            }
        }

        VkImageMemoryBarrier* imgBarriers = NULL;
        if (imageMemoryBarrierCount > 0) {
            imgBarriers = malloc(imageMemoryBarrierCount * sizeof(VkImageMemoryBarrier));
            for (uint32_t i = 0; i < imageMemoryBarrierCount; ++i) {
                decode_from_stream_VkImageMemoryBarrier(VK_STRUCTURE_TYPE_MAX_ENUM, &imgBarriers[i], ptr);
                // imgBarriers[i].image = (VkImage)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_IMAGE, (uint64_t)(uintptr_t)imgBarriers[i].image);
            }
        }

        VkCommandBuffer realCmdBuf = (VkCommandBuffer)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd_buf);

        vkCmdPipelineBarrier(realCmdBuf, srcStageMask, dstStageMask, dependencyFlags,
                            memoryBarrierCount, memBarriers,
                            bufferMemoryBarrierCount, bufBarriers,
                            imageMemoryBarrierCount, imgBarriers);
        LOGD("vkCmdPipelineBarrier called with srcStageMask %u, dstStageMask %u, dependencyFlags %u",
            srcStageMask, dstStageMask, dependencyFlags);

        LOGD("CmdPipelineBarrier guest %llu -> host %p", (unsigned long long)guest_cmd_buf, (void*)realCmdBuf);

        if (memBarriers) free(memBarriers);
        if (bufBarriers) free(bufBarriers);
        if (imgBarriers) free(imgBarriers);
        //if (need_free) free(stream);
    }
    break;

    case FUNID_vkCmdBeginRenderPass: {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd_buf = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        VkRenderPassBeginInfo* pInfo = malloc(sizeof(VkRenderPassBeginInfo));
        decode_from_stream_VkRenderPassBeginInfo(VK_STRUCTURE_TYPE_MAX_ENUM, pInfo, ptr);

        VkSubpassContents contents = *(uint32_t*)(*ptr);
        *ptr += sizeof(uint32_t);
        VkCommandBuffer realCmdBuf = (VkCommandBuffer)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd_buf);
        vkCmdBeginRenderPass(realCmdBuf, pInfo, contents);

        LOGD("CmdBeginRenderPass guest %llu -> host %p", (unsigned long long)guest_cmd_buf, (void*)realCmdBuf);

        //if (need_free) free(stream);
        free(pInfo);
    }
    break;

    case FUNID_vkCmdBindPipeline: {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd_buf = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        VkPipelineBindPoint bindPoint = *(uint32_t*)(*ptr);
        *ptr += sizeof(uint32_t);

        uint64_t guest_pipeline = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        VkCommandBuffer realCmdBuf = (VkCommandBuffer)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd_buf);
        VkPipeline realPipeline = (VkPipeline)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_PIPELINE, guest_pipeline);

        vkCmdBindPipeline(realCmdBuf, bindPoint, realPipeline);

        LOGD("CmdBindPipeline guest %llu -> host %p pipeline %p", (unsigned long long)guest_cmd_buf, (void*)realCmdBuf, (void*)realPipeline);

        //if (need_free) free(stream);
    }
    break;

    case FUNID_vkCmdBindVertexBuffers: {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);

        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd_buf = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        uint32_t firstBinding = *(uint32_t*)(*ptr);
        *ptr += sizeof(uint32_t);
        uint32_t bindingCount = *(uint32_t*)(*ptr);
        *ptr += sizeof(uint32_t);

        VkBuffer* realBuffers = malloc(bindingCount * sizeof(VkBuffer));
        for (uint32_t i = 0; i < bindingCount; ++i) {
            uint64_t guest_buf = *(uint64_t*)(*ptr);
            *ptr += sizeof(uint64_t);
            realBuffers[i] = (VkBuffer)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_BUFFER, guest_buf);
        }

        VkDeviceSize* offsets = malloc(bindingCount * sizeof(VkDeviceSize));
        for (uint32_t i = 0; i < bindingCount; ++i) {
            offsets[i] = *(VkDeviceSize*)(*ptr);
            *ptr += sizeof(VkDeviceSize);
        }

        VkCommandBuffer realCmdBuf =
            (VkCommandBuffer)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd_buf);

        vkCmdBindVertexBuffers(realCmdBuf, firstBinding, bindingCount, realBuffers, offsets);

        LOGD("CmdBindVertexBuffers guest %llu -> host %p count %d",
            (unsigned long long)guest_cmd_buf, (void*)realCmdBuf, bindingCount);

        free(realBuffers);
        free(offsets);
        // if (need_free) free(stream);
    }
    break;


    case FUNID_vkCmdDraw: {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint32_t vertexCount = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);
        uint32_t instanceCount = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);
        uint32_t firstVertex = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);
        uint32_t firstInstance = *(uint32_t*)(*ptr);

        VkCommandBuffer realCmd = (VkCommandBuffer)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd);

        vkCmdDraw(realCmd, vertexCount, instanceCount, firstVertex, firstInstance);

        LOGD("CmdDraw executed cmd=%p vertices=%d", (void*)realCmd, vertexCount);

        //if (need_free) free(stream);

    }
    break;

    case FUNID_vkCmdEndRenderPass: {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd = *(uint64_t*)(*ptr);

        VkCommandBuffer realCmd = (VkCommandBuffer)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd);

        vkCmdEndRenderPass(realCmd);

        LOGD("CmdEndRenderPass executed cmd=%p", (void*)realCmd);

        //if (need_free) free(stream);
    }
    break;

    case FUNID_vkEndCommandBuffer: {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd = *(uint64_t*)(*ptr);

        VkCommandBuffer realCmd = (VkCommandBuffer)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd);

        VkResult result = vkEndCommandBuffer(realCmd);
        if (para_num > 1) {
            write_to_guest_mem(all_para[1].data, &result, 0,
                               sizeof(result));
        }

        LOGD("EndCommandBuffer executed cmd=%p result=%d", (void*)realCmd, result);

        //if (need_free) free(stream);
    }
    break;

    case FUNID_vkCreateFence: {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        VkFenceCreateInfo* pInfo = malloc(sizeof(VkFenceCreateInfo));
        decode_from_stream_VkFenceCreateInfo(VK_STRUCTURE_TYPE_MAX_ENUM, pInfo, ptr);

        uint64_t guest_alloc_ptr = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        VkAllocationCallbacks allocStruct;
        const VkAllocationCallbacks* pAllocator = NULL;
        if (guest_alloc_ptr) {
            decode_from_stream_VkAllocationCallbacks(VK_STRUCTURE_TYPE_MAX_ENUM, &allocStruct, ptr);
            pAllocator = &allocStruct;
        }

        uint64_t guest_dev = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        uint64_t guest_fence = *(uint64_t*)(*ptr);

        VkDevice realDev = (VkDevice)(uintptr_t)lookup_mapping(
            EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_dev);
        VkFence realFence = VK_NULL_HANDLE;
        VkResult result = VK_ERROR_INITIALIZATION_FAILED;
        if (realDev == VK_NULL_HANDLE) {
            LOGE("Host: vkCreateFence rejected missing device mapping "
                 "guest_device=0x%llx guest_fence=0x%llx para_num=%d",
                 (unsigned long long)guest_dev,
                 (unsigned long long)guest_fence,
                 para_num);
        } else {
            result = vkCreateFence(realDev, pInfo, pAllocator, &realFence);
        }
        if (result == VK_SUCCESS) {
            insert_mapping(EXPRESS_VK_OBJECT_TYPE_FENCE, guest_fence, (uint64_t)(uintptr_t)realFence);
            LOGD("Mapped Fence guest=%llu host=%p", (unsigned long long)guest_fence, (void*)realFence);
        } else {
            LOGD("vkCreateFence failed: %d", result);
        }

        //if (need_free) free(stream);
        free(pInfo);
    }
    break;

    case FUNID_vkCreateSemaphore: {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        VkSemaphoreCreateInfo* pInfo = malloc(sizeof(VkSemaphoreCreateInfo));
        decode_from_stream_VkSemaphoreCreateInfo(VK_STRUCTURE_TYPE_MAX_ENUM, pInfo, ptr);

        uint64_t guest_alloc_ptr = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        VkAllocationCallbacks allocStruct;
        const VkAllocationCallbacks* pAllocator = NULL;
        if (guest_alloc_ptr) {
            decode_from_stream_VkAllocationCallbacks(VK_STRUCTURE_TYPE_MAX_ENUM, &allocStruct, ptr);
            pAllocator = &allocStruct;
        }

        uint64_t guest_dev = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        uint64_t guest_semaphore = *(uint64_t*)(*ptr);

        VkDevice realDev = (VkDevice)(uintptr_t)lookup_mapping(
            EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_dev);
        VkSemaphore realSemaphore = VK_NULL_HANDLE;
        VkResult result = VK_ERROR_INITIALIZATION_FAILED;
        if (realDev == VK_NULL_HANDLE) {
            LOGE("Host: vkCreateSemaphore rejected missing device mapping "
                 "guest_device=0x%llx guest_semaphore=0x%llx para_num=%d",
                 (unsigned long long)guest_dev,
                 (unsigned long long)guest_semaphore,
                 para_num);
        } else {
            result = vkCreateSemaphore(
                realDev, pInfo, pAllocator, &realSemaphore);
        }
        if (result == VK_SUCCESS) {
            insert_mapping(EXPRESS_VK_OBJECT_TYPE_SEMAPHORE, guest_semaphore, (uint64_t)(uintptr_t)realSemaphore);
            LOGD("Mapped Semaphore guest=%llu host=%p", (unsigned long long)guest_semaphore, (void*)realSemaphore);
        } else {
            LOGD("vkCreateSemaphore failed: %d", result);
        }

        //if (need_free) free(stream);
        free(pInfo);
        break;
    }

    case FUNID_vkAcquireNextImageKHR:{
        LOGD("Host: vkAcquireNextImageKHR");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_device = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint64_t guest_swapchain = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint64_t timeout = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint64_t guest_semaphore = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint64_t guest_fence = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint64_t guest_buffer = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);


        VkDevice device = (VkDevice)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_device);
        VkSwapchainKHR swapchain = (VkSwapchainKHR)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_SWAPCHAIN_KHR, guest_swapchain);
        VkSemaphore semaphore = guest_semaphore ? (VkSemaphore)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_SEMAPHORE, guest_semaphore) : VK_NULL_HANDLE;
        VkFence fence = guest_fence ? (VkFence)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_FENCE, guest_fence) : VK_NULL_HANDLE;

        uint32_t imageIndex;
        VkResult result = vkAcquireNextImageKHR(device, swapchain, timeout, semaphore, fence, &imageIndex);


        LOGD("Host: vkAcquireNextImageKHR result=%d imageIndex=%d", result, imageIndex);
    }
    break;

    case FUNID_vkResetFences: {
        LOGD("Host: vkResetFences");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_device = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint32_t fenceCount = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);

        VkDevice device = (VkDevice)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_device);

        VkFence* fences = NULL;
        if (fenceCount > 0) {
            fences = (VkFence*)malloc(fenceCount * sizeof(VkFence));
            uint64_t* guest_fences = (uint64_t*)(*ptr);
            for (uint32_t i = 0; i < fenceCount; ++i) {
                fences[i] = (VkFence)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_FENCE, guest_fences[i]);
                LOGD("get Mapped Fence guest %llu -> host %p", (unsigned long long)guest_fences[i], (void*)fences[i]);
            }
        }

        VkResult result = vkResetFences(device, fenceCount, fences);

        if (result == VK_SUCCESS && fences != NULL) {
            for (uint32_t i = 0; i < fenceCount; ++i) {
                express_vk_remove_fence_output_hints(fences[i]);
            }
        }

        if (fences) free(fences);

        LOGD("Host: vkResetFences result=%d fenceCount=%d", result, fenceCount);
    }
    break;

    case FUNID_vkQueueSubmit: {
        struct timespec t_case0, t_case1;
        clock_gettime(CLOCK_MONOTONIC, &t_case0);

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        char* stream_base = stream;
        char* stream_end = stream_base + all_para[0].data_len;
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_queue = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint32_t submitCount = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);
        uint64_t guest_fence = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);

        VkQueue queue = (VkQueue)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_QUEUE, guest_queue);
        VkFence fence = guest_fence ? (VkFence)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_FENCE, guest_fence) : VK_NULL_HANDLE;

        VkSubmitInfo *pSubmits = NULL;
        char *data_ptr = (char *)(*ptr);
        int invalid_submit =
            (guest_queue != 0 && queue == VK_NULL_HANDLE) ||
            (guest_fence != 0 && fence == VK_NULL_HANDLE);
        if (submitCount > 0) {
            pSubmits = g_try_new0(VkSubmitInfo, submitCount);
            if (pSubmits == NULL) {
                invalid_submit = 1;
            } else {
                for (uint32_t i = 0; i < submitCount; ++i) {
                    decode_from_stream_VkSubmitInfo(
                        VK_STRUCTURE_TYPE_MAX_ENUM, &pSubmits[i], ptr);
                    if (!express_vk_validate_decoded_submit(&pSubmits[i])) {
                        invalid_submit = 1;
                        LOGE("Host: vkQueueSubmit rejected malformed or unmapped submit=%u",
                             i);
                    }
                }
                data_ptr = (char *)(*ptr);
                if (data_ptr < stream_base || data_ptr > stream_end) {
                    invalid_submit = 1;
                    data_ptr = stream_end;
                }
            }
        }

        ExpressVkSubmitHints submit_hints;
        memset(&submit_hints, 0, sizeof(submit_hints));
        express_vk_parse_submit_hints(data_ptr, stream_end, VK_NULL_HANDLE, &submit_hints);
        express_vk_wait_uploads_for_submit_hints(&submit_hints);

        ExpressVkFlimeSubmitBatch *flime_batch = NULL;
        ExpressVkQueueInfo flime_queue_info;
        VkDevice flime_submit_device = VK_NULL_HANDLE;
        if (express_vk_lookup_queue_info(queue, &flime_queue_info)) {
            flime_submit_device = flime_queue_info.device;
        }
        ExpressVkFlimeSubmitGate flime_gate =
            EXPRESS_VK_FLIME_SUBMIT_LEGACY;

        struct timespec t0_qs, t1_qs;
        clock_gettime(CLOCK_MONOTONIC, &t0_qs);
        VkResult result = VK_ERROR_INITIALIZATION_FAILED;
        uint64_t flime_realize_ns = 0;
        g_mutex_lock(&g_express_vk_transaction_lock);
        flime_gate = express_vk_flime_bridge_prepare_submit(
            call->process_id, guest_queue, queue, flime_submit_device,
            &flime_batch);
        if (flime_gate == EXPRESS_VK_FLIME_SUBMIT_BLOCKED ||
            flime_gate == EXPRESS_VK_FLIME_SUBMIT_ERROR) {
            invalid_submit = 1;
        }
        if (!invalid_submit) {
            if (flime_gate == EXPRESS_VK_FLIME_SUBMIT_READY &&
                express_vk_flime_bridge_batch_write_count(flime_batch) != 0) {
                flime_realize_ns = express_vk_flime_realize_descriptor_writes(
                    express_vk_flime_bridge_batch_device(flime_batch),
                    express_vk_flime_bridge_batch_write_count(flime_batch),
                    express_vk_flime_bridge_batch_writes(flime_batch));
                if (!express_vk_flime_bridge_submit_updates_applied(
                        flime_batch)) {
                    invalid_submit = 1;
                }
            }
        }
        if (!invalid_submit) {
            result = vkQueueSubmit(queue, submitCount, pSubmits, fence);
        } else {
            LOGE("Host: vkQueueSubmit skipped real call because command mappings or the FLIME FINAL gate were invalid");
        }
        if (flime_batch != NULL) {
            express_vk_flime_bridge_complete_submit(flime_batch, result,
                                                     flime_realize_ns);
        }
        g_mutex_unlock(&g_express_vk_transaction_lock);
        if (para_num > 1) {
            write_to_guest_mem(all_para[1].data, &result, 0,
                               sizeof(result));
        }
        clock_gettime(CLOCK_MONOTONIC, &t1_qs);
        double qs_cost_ms = (t1_qs.tv_sec - t0_qs.tv_sec) * 1000.0 + (t1_qs.tv_nsec - t0_qs.tv_nsec) / 1000000.0;

        double prep_ms = (t0_qs.tv_sec - t_case0.tv_sec) * 1000.0 + (t0_qs.tv_nsec - t_case0.tv_nsec) / 1000000.0;
        if (EXPRESS_VK_ENABLE_FENCE_OUTPUT_HINT_COMMIT &&
            result == VK_SUCCESS && fence != VK_NULL_HANDLE) {
            express_vk_store_fence_output_hints(fence, guest_fence, &submit_hints);
        }

        express_vk_free_decoded_submit_infos(pSubmits, submitCount);

        clock_gettime(CLOCK_MONOTONIC, &t_case1);
        double total_ms = (t_case1.tv_sec - t_case0.tv_sec) * 1000.0 + (t_case1.tv_nsec - t_case0.tv_nsec) / 1000000.0;
        express_vk_host_note_queue_submit_timing(
            (uint64_t)(prep_ms * 1000.0),
            (uint64_t)(qs_cost_ms * 1000.0),
            (uint64_t)(total_ms * 1000.0));
        express_vk_free_submit_hints(&submit_hints);
    }
    break;

    case FUNID_vkWaitForFences:
    {
        if (EXPRESS_VK_ENABLE_SYNC_DIAG_LOG) {
            LOGD("Host: vkWaitForFences");
        }
        struct timespec t_case0, t_case1;
        clock_gettime(CLOCK_MONOTONIC, &t_case0);

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_device = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint32_t fenceCount = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);
        VkBool32 waitAll = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);
        uint64_t timeout = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);

        VkDevice device = (VkDevice)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_device);

        VkFence* fences = NULL;
        if (fenceCount > 0) {
            fences = (VkFence*)malloc(fenceCount * sizeof(VkFence));
            uint64_t* guest_fences = (uint64_t*)(*ptr);
            for (uint32_t i = 0; i < fenceCount; ++i) {
                fences[i] = (VkFence)(uintptr_t)
                    lookup_mapping(EXPRESS_VK_OBJECT_TYPE_FENCE, guest_fences[i]);
            }
        }
        VkFence first_fence_for_log =
            (fences != NULL && fenceCount > 0) ? fences[0] : VK_NULL_HANDLE;

        struct timespec t0_wf, t1_wf;
        clock_gettime(CLOCK_MONOTONIC, &t0_wf);
        VkResult result =
            vkWaitForFences(device, fenceCount, fences, waitAll, timeout);
        clock_gettime(CLOCK_MONOTONIC, &t1_wf);
        double wf_cost_ms =
            (t1_wf.tv_sec - t0_wf.tv_sec) * 1000.0 +
            (t1_wf.tv_nsec - t0_wf.tv_nsec) / 1000000.0;
        if (EXPRESS_VK_ENABLE_SYNC_DIAG_LOG) {
            LOGD("[GPU_TIME] vkWaitForFences cost: %.3f ms", wf_cost_ms);
        }

        if (EXPRESS_VK_ENABLE_FENCE_OUTPUT_HINT_COMMIT &&
            result == VK_SUCCESS && fences != NULL) {
            if (waitAll || fenceCount == 1) {
                for (uint32_t i = 0; i < fenceCount; ++i) {
                    express_vk_commit_fence_outputs(fences[i], "wait_fences");
                }
            } else if (EXPRESS_VK_ENABLE_SYNC_DIAG_LOG) {
                LOGD("[ExpressVkCommit] skip wait_fences commit for waitAny fenceCount=%u",
                     fenceCount);
            }
        }

        double prep_ms =
            (t0_wf.tv_sec - t_case0.tv_sec) * 1000.0 +
            (t0_wf.tv_nsec - t_case0.tv_nsec) / 1000000.0;

        if (para_num > 1) {
            write_to_guest_mem(all_para[1].data, &result, 0, sizeof(VkResult));
        }

        if (fences) {
            free(fences);
        }

        clock_gettime(CLOCK_MONOTONIC, &t_case1);
        double total_ms =
            (t_case1.tv_sec - t_case0.tv_sec) * 1000.0 +
            (t_case1.tv_nsec - t_case0.tv_nsec) / 1000000.0;
        if (EXPRESS_VK_ENABLE_SYNC_DIAG_LOG) {
            LOGD("[HOST_OVERHEAD] vkWaitForFences prep=%.3f ms wait=%.3f ms total=%.3f ms fenceCount=%u",
                 prep_ms, wf_cost_ms, total_ms, fenceCount);
            LOGD("Host: vkWaitForFences result=%d fenceCount=%d waitAll=%d fence %llx",
                 result, fenceCount, waitAll,
                 (unsigned long long)(uintptr_t)first_fence_for_log);
        }
        break;
    }

    case FUNID_vkExpressWaitFenceAndInvalidateANDROID:
    {
        if (EXPRESS_VK_ENABLE_SYNC_DIAG_LOG) {
            LOGD("Host: vkExpressWaitFenceAndInvalidateANDROID");
        }
        struct timespec t_case0, t_wait0, t_wait1, t_copy0, t_copy1, t_case1;
        clock_gettime(CLOCK_MONOTONIC, &t_case0);

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_device = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint32_t fenceCount = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);
        VkBool32 waitAll = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);
        uint64_t timeout = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint32_t rangeCount = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);

        uint64_t* guest_fences = (uint64_t*)(*ptr);
        *ptr += fenceCount * sizeof(uint64_t);

        typedef struct ExpressWaitFenceInvalidateRangeWire {
            uint64_t memory;
            uint64_t offset;
            uint64_t size;
        } ExpressWaitFenceInvalidateRangeWire;
        ExpressWaitFenceInvalidateRangeWire* wire_ranges =
            (ExpressWaitFenceInvalidateRangeWire*)(*ptr);
        *ptr += rangeCount * sizeof(ExpressWaitFenceInvalidateRangeWire);

        VkDevice device = (VkDevice)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_device);

        VkFence* fences = NULL;
        if (fenceCount > 0) {
            fences = (VkFence*)calloc(fenceCount, sizeof(VkFence));
            for (uint32_t i = 0; i < fenceCount; ++i) {
                fences[i] = (VkFence)(uintptr_t)
                    lookup_mapping(EXPRESS_VK_OBJECT_TYPE_FENCE, guest_fences[i]);
            }
        }

        clock_gettime(CLOCK_MONOTONIC, &t_wait0);
        VkResult result = VK_ERROR_INITIALIZATION_FAILED;
        if (device != VK_NULL_HANDLE && (fenceCount == 0 || fences != NULL)) {
            result = vkWaitForFences(device, fenceCount, fences, waitAll, timeout);
        }
        clock_gettime(CLOCK_MONOTONIC, &t_wait1);


        uint64_t bytes = 0;
        uint32_t committed = 0;
        int failed = 0;
        clock_gettime(CLOCK_MONOTONIC, &t_copy0);
        if (result == VK_SUCCESS) {
            for (uint32_t i = 0; i < rangeCount; ++i) {
                uint64_t guest_memory = wire_ranges[i].memory;
                uint64_t host_memory =
                    lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE_MEMORY, guest_memory);
                ExpressVkRegisteredMemory* reg =
                    express_vk_find_registered_memory(guest_memory, host_memory);
                if (reg == NULL || reg->guest_mem == NULL || reg->host_memory == 0) {
                    LOGE("[HOST_WAIT_INVALIDATE_FUSED] missing registered memory guest=0x%llx host=0x%llx",
                         (unsigned long long)guest_memory,
                         (unsigned long long)host_memory);
                    failed = 1;
                    result = VK_ERROR_DEVICE_LOST;
                    break;
                }

                uint64_t size = express_vk_registered_range_size(
                    reg, wire_ranges[i].offset, wire_ranges[i].size);
                if (size == 0) {
                    continue;
                }

                void* host_ptr = get_memory_map(reg->host_memory);
                if (host_ptr == NULL) {
                    LOGE("[HOST_WAIT_INVALIDATE_FUSED] host mapped ptr missing guest=0x%llx host=0x%llx",
                         (unsigned long long)guest_memory,
                         (unsigned long long)reg->host_memory);
                    failed = 1;
                    result = VK_ERROR_MEMORY_MAP_FAILED;
                    break;
                }

                VkMappedMemoryRange range;
                memset(&range, 0, sizeof(range));
                range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
                range.memory = (VkDeviceMemory)(uintptr_t)reg->host_memory;
                range.offset = wire_ranges[i].offset;
                range.size = size;

                VkResult invalidate_result =
                    vkInvalidateMappedMemoryRanges(device, 1, &range);
                if (invalidate_result != VK_SUCCESS) {
                    LOGE("[HOST_WAIT_INVALIDATE_FUSED] invalidate failed guest=0x%llx offset=%llu size=%llu result=%d",
                         (unsigned long long)guest_memory,
                         (unsigned long long)wire_ranges[i].offset,
                         (unsigned long long)size,
                         invalidate_result);
                    failed = 1;
                    result = invalidate_result;
                    break;
                }
                uint64_t copied_bytes = express_vk_copy_host_to_guest(
                    reg, host_ptr, wire_ranges[i].offset, size);

                bytes += copied_bytes;
                committed++;
            }

            if (EXPRESS_VK_ENABLE_FENCE_OUTPUT_HINT_COMMIT &&
                !failed && fences != NULL && (waitAll || fenceCount == 1)) {
                for (uint32_t i = 0; i < fenceCount; ++i) {
                    express_vk_remove_fence_output_hints(fences[i]);
                }
            }
        }
        clock_gettime(CLOCK_MONOTONIC, &t_copy1);

        if (fences) {
            free(fences);
        }

        uint64_t timing_us[2];
        timing_us[0] = (uint64_t)((t_wait1.tv_sec - t_wait0.tv_sec) * 1000000ULL +
                                  (t_wait1.tv_nsec - t_wait0.tv_nsec) / 1000ULL);
        timing_us[1] = (uint64_t)((t_copy1.tv_sec - t_copy0.tv_sec) * 1000000ULL +
                                  (t_copy1.tv_nsec - t_copy0.tv_nsec) / 1000ULL);
        clock_gettime(CLOCK_MONOTONIC, &t_case1);
        uint64_t total_us = (uint64_t)((t_case1.tv_sec - t_case0.tv_sec) * 1000000ULL +
                                       (t_case1.tv_nsec - t_case0.tv_nsec) / 1000ULL);

        if (para_num > 1) {
            write_to_guest_mem(all_para[1].data, &result, 0, sizeof(VkResult));
        }
        if (para_num > 2) {
            write_to_guest_mem(all_para[2].data, timing_us, 0, sizeof(timing_us));
        }

        express_vk_host_wait_invalidate_fused_note(committed,
                                                   bytes,
                                                   timing_us[0],
                                                   timing_us[1],
                                                   total_us,
                                                   failed || result != VK_SUCCESS);
        if (EXPRESS_VK_ENABLE_SYNC_DIAG_LOG) {
            LOGD("[HOST_WAIT_INVALIDATE_FUSED] one_call ranges=%u committed=%u bytes=%llu wait_us=%llu copy_us=%llu total_us=%llu result=%d",
                 rangeCount,
                 committed,
                 (unsigned long long)bytes,
                 (unsigned long long)timing_us[0],
                 (unsigned long long)timing_us[1],
                 (unsigned long long)total_us,
                 result);
        }
        break;
    }

    case FUNID_vkQueuePresentKHR:
    {
        LOGD("Host: vkQueuePresentKHR request");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_queue = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        VkQueue queue = (VkQueue)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_QUEUE, guest_queue);

        VkPresentInfoKHR presentInfo;
        decode_from_stream_VkPresentInfoKHR(VK_STRUCTURE_TYPE_MAX_ENUM, &presentInfo, ptr);

        uint64_t* buffer_ids = NULL;
        if (presentInfo.swapchainCount > 0) {
            buffer_ids = (uint64_t*)malloc(sizeof(uint64_t) * presentInfo.swapchainCount);
            if (!buffer_ids ||
                !copy_from_call_para_fast(all_para[1], buffer_ids,
                                          sizeof(uint64_t) * presentInfo.swapchainCount)) {
                LOGE("Host: vkQueuePresentKHR failed to read buffer ids");
                if (buffer_ids) free(buffer_ids);
                break;
            }
        }

        LOGD("Host: vkQueuePresentKHR queue=%lld swapchainCount=%d firstBuffer=%llx",
            (uint64_t)(uintptr_t)queue, presentInfo.swapchainCount,
            (presentInfo.swapchainCount > 0) ? buffer_ids[0] : 0ULL);


        vulkan_surface_present_images(queue, &presentInfo, buffer_ids);
        vkQueuePresentKHR(queue, &presentInfo);
        if (buffer_ids) free(buffer_ids);
        //if (need_free) free(stream);
        break;
    }

    case FUNID_vkGetImageMemoryRequirements: {
        LOGD("Host: vkGetImageMemoryRequirements request");
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        LOGD("get vkGetImageMemoryRequirements param count %d", para_num);

        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;
        uint64_t guest_device = *(uint64_t*)(*ptr);  *ptr += sizeof(uint64_t);
        uint64_t guest_image  = *(uint64_t*)(*ptr);  *ptr += sizeof(uint64_t);

        void* guest_mem_req_ptr = all_para[1].data;

        VkDevice device = (VkDevice)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_device);
        VkImage image   = (VkImage)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_IMAGE, guest_image);

        VkMemoryRequirements memReq;
        vkGetImageMemoryRequirements(device, image, &memReq);

        write_to_guest_mem(guest_mem_req_ptr, &memReq, 0, sizeof(memReq));
        LOGD("Host: vkGetImageMemoryRequirements done with value size %d alignment %d",
             memReq.size, memReq.alignment);
        break;
    }

    case FUNID_vkGetImageMemoryRequirements2:
    {
        LOGD("Host: vkGetImageMemoryRequirements2 request");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);

        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        VkImageMemoryRequirementsInfo2 info;
        VkMemoryRequirements2 memReqs;

        decode_from_stream_VkImageMemoryRequirementsInfo2(VK_STRUCTURE_TYPE_MAX_ENUM, &info, ptr);
        decode_from_stream_VkMemoryRequirements2(VK_STRUCTURE_TYPE_MAX_ENUM, &memReqs, ptr);

        uint64_t guest_device = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);

        VkDevice device = (VkDevice)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_device);

        vkGetImageMemoryRequirements2(device, &info, &memReqs);

        write_to_guest_mem(all_para[1].data, &memReqs, 0, sizeof(VkMemoryRequirements2));
    }
    break;

    case FUNID_vkGetPhysicalDeviceMemoryProperties: {
        LOGD("Host: vkGetPhysicalDeviceMemoryProperties request");
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        LOGD("get vkGetPhysicalDeviceMemoryProperties param count %d", para_num);

        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint64_t guest_pd = *(uint64_t*)stream;

        void* guest_props_ptr = all_para[1].data;

        VkPhysicalDevice pd = (VkPhysicalDevice)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_PHYSICAL_DEVICE, guest_pd);

        VkPhysicalDeviceMemoryProperties props;
        vkGetPhysicalDeviceMemoryProperties(pd, &props);

        write_to_guest_mem(guest_props_ptr, &props, 0, sizeof(props));
        LOGD("Host: vkGetPhysicalDeviceMemoryProperties done memoryTypeCount=%u memoryHeapCount=%u",
             props.memoryTypeCount, props.memoryHeapCount);
        if (props.memoryTypeCount == 0 || props.memoryHeapCount == 0) {
            LOGE("SEARCH_ME_PD_MEMPROPS_ZERO_COUNT: memoryTypeCount=%u memoryHeapCount=%u",
                props.memoryTypeCount, props.memoryHeapCount);
        }
        for (uint32_t i = 0; i < props.memoryHeapCount; i++) {
            LOGD("  memoryHeap[%u] size=%llu flags=0x%x",
                i,
                (unsigned long long)props.memoryHeaps[i].size,
                props.memoryHeaps[i].flags);
        }
        for(int i=0; i<props.memoryTypeCount; i++) {
            LOGD("  memoryType[%d] propertyFlags 0x%x heapIndex %d",
                 i, props.memoryTypes[i].propertyFlags, props.memoryTypes[i].heapIndex);
        }
        break;
    }

    case FUNID_vkGetPhysicalDeviceMemoryProperties2: {
        LOGD("get call GetGetPhysicalDeviceMemoryProperties2! sizeof VkPhysicalDeviceMemoryProperties2: %lu",
            sizeof(VkPhysicalDeviceMemoryProperties2));
        LOGD("sizeof memory heap: %lu",
            sizeof(VkMemoryHeap));
        LOGD("sizeof memory type: %lu",
            sizeof(VkMemoryType));

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        LOGD("get vk param number %d", para_num);

        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_physicalDevice = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        VkPhysicalDevice real_physicalDevice = (VkPhysicalDevice)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_PHYSICAL_DEVICE, guest_physicalDevice);

        LOGD("physicalDevice = %p, properties = %p",
            guest_physicalDevice, all_para[1].data);

        VkPhysicalDeviceMemoryProperties2 props;
        props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
        props.pNext = NULL;
        vkGetPhysicalDeviceMemoryProperties2(real_physicalDevice, &props);

        write_to_guest_mem(all_para[1].data, &props, 0, sizeof(VkPhysicalDeviceMemoryProperties2));

        LOGD("vkGetPhysicalDeviceMemoryProperties2 physicalDevice = %p, memoryTypeCount = %d",
            guest_physicalDevice,
            props.memoryProperties.memoryTypeCount);

        //if (need_free) free(stream);
    }
    break;

    case FUNID_vkGetPhysicalDeviceProperties: {
        LOGD("get call GetPhysicalDeviceProperties");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        LOGD("get vk param number %d", para_num);

        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_physicalDevice = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);

        VkPhysicalDevice real_physicalDevice = (VkPhysicalDevice)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_PHYSICAL_DEVICE, guest_physicalDevice);

        LOGD("physical_device = %p, host_physical_device = %p, properties_ptr = %p",
            guest_physicalDevice,
            (void*)real_physicalDevice,
            all_para[1].data);
        LOGD("PD_HANDLE_CHECK vkGetPhysicalDeviceProperties guest=%p host=%p",
            guest_physicalDevice,
            (void*)real_physicalDevice);

        VkPhysicalDeviceProperties pProps;

        vkGetPhysicalDeviceProperties(real_physicalDevice, &pProps);
        LOGD("physical_device = %p, properties = %d %d %x",
            guest_physicalDevice,
            pProps.apiVersion, pProps.driverVersion, pProps.vendorID);
        LOGD("limits.bufferImageGranularity = %llu",
            (unsigned long long)pProps.limits.bufferImageGranularity);
        LOGD("limits.nonCoherentAtomSize = %llu",
            (unsigned long long)pProps.limits.nonCoherentAtomSize);
        LOGD("limits.minStorageBufferOffsetAlignment = %llu",
            (unsigned long long)pProps.limits.minStorageBufferOffsetAlignment);
        LOGD("limits.minTexelBufferOffsetAlignment = %llu",
            (unsigned long long)pProps.limits.minTexelBufferOffsetAlignment);
        g_cached_non_coherent_atom_size = pProps.limits.nonCoherentAtomSize;
        g_cached_min_storage_buffer_offset_alignment =
            pProps.limits.minStorageBufferOffsetAlignment;
        g_cached_props_guest_pd = guest_physicalDevice;
        g_cached_props_host_pd = (uint64_t)(uintptr_t)real_physicalDevice;
        LOGD("NCNN_LIMITS_CACHE api=vkGetPhysicalDeviceProperties guest_pd=%llu host_pd=%p a=%llu b=%llu",
            (unsigned long long)g_cached_props_guest_pd,
            (void*)(uintptr_t)g_cached_props_host_pd,
            (unsigned long long)g_cached_non_coherent_atom_size,
            (unsigned long long)g_cached_min_storage_buffer_offset_alignment);
        g_is_intel_gpu = (pProps.vendorID == 0x8086);

        write_to_guest_mem(all_para[1].data, &pProps, 0, sizeof(VkPhysicalDeviceProperties));

        //if (need_free) free(stream);
    }
    break;

    case FUNID_vkGetPhysicalDeviceProperties2: {
        LOGD("get call GetPhysicalDeviceProperties2");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        LOGD("get vk param number %d", para_num);

        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_physicalDevice = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);

        VkPhysicalDevice real_physicalDevice = (VkPhysicalDevice)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_PHYSICAL_DEVICE, guest_physicalDevice);
        LOGD("PD_HANDLE_CHECK vkGetPhysicalDeviceProperties2 guest=%p host=%p",
            guest_physicalDevice,
            (void*)real_physicalDevice);

        VkPhysicalDeviceProperties2 pProps = {0};
        decode_from_stream_VkPhysicalDeviceProperties2(
            VK_STRUCTURE_TYPE_MAX_ENUM,
            &pProps,
            ptr);
        if (pProps.sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2) {
            LOGW("vkGetPhysicalDeviceProperties2 unexpected input sType=%d, force to VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2",
                pProps.sType);
            pProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        }
        LOGD("Host: vkGetPhysicalDeviceProperties2 input pNext is %s",
            pProps.pNext ? "non-null" : "null");

        vkGetPhysicalDeviceProperties2(real_physicalDevice, &pProps);

        g_cached_non_coherent_atom_size = pProps.properties.limits.nonCoherentAtomSize;
        g_cached_min_storage_buffer_offset_alignment =
            pProps.properties.limits.minStorageBufferOffsetAlignment;
        g_cached_props_guest_pd = guest_physicalDevice;
        g_cached_props_host_pd = (uint64_t)(uintptr_t)real_physicalDevice;
        LOGD("NCNN_LIMITS_CACHE api=vkGetPhysicalDeviceProperties2 guest_pd=%llu host_pd=%p a=%llu b=%llu",
            (unsigned long long)g_cached_props_guest_pd,
            (void*)(uintptr_t)g_cached_props_host_pd,
            (unsigned long long)g_cached_non_coherent_atom_size,
            (unsigned long long)g_cached_min_storage_buffer_offset_alignment);

        write_to_guest_mem(all_para[1].data, &pProps, 0, sizeof(VkPhysicalDeviceProperties2));
        LOGD("physical_device = %p, properties = %d",
            guest_physicalDevice,
            pProps.properties.apiVersion);
        LOGD("Host: vkGetPhysicalDeviceProperties2 output pNext is %s",
            pProps.pNext ? "non-null" : "null");

        //if (need_free) free(stream);
    }
    break;

    case FUNID_vkGetPhysicalDeviceQueueFamilyProperties: {
        LOGD("Host: vkGetPhysicalDeviceQueueFamilyProperties request");
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        LOGD("get vkGetPhysicalDeviceQueueFamilyProperties param count %d", para_num);

        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint64_t guest_pd = *(uint64_t*)stream;

        uint32_t count = 0;
        if (!copy_from_call_para_fast(all_para[1], &count, sizeof(count))) {
            LOGE("Host: vkGetPhysicalDeviceQueueFamilyProperties failed to read count");
            break;
        }
        void* guest_props_ptr = all_para[2].data;
        uint64_t guest_props_len = all_para[2].data_len;


        VkPhysicalDevice pd = (VkPhysicalDevice)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_PHYSICAL_DEVICE, guest_pd);

        VkQueueFamilyProperties* props = NULL;
        VkResult result;

        uint64_t required_props_bytes =
            (uint64_t)sizeof(VkQueueFamilyProperties) * (uint64_t)count;
        bool can_write_props = guest_props_ptr != NULL &&
                               guest_props_len >= required_props_bytes &&
                               required_props_bytes != 0;

        if (count == 0 || !can_write_props) {
            if (count != 0) {
                LOGD("Host: vkGetPhysicalDeviceQueueFamilyProperties count-only/null-output "
                     "input_count=%u guest_props=%p guest_len=%llu required=%llu",
                     count,
                     guest_props_ptr,
                     (unsigned long long)guest_props_len,
                     (unsigned long long)required_props_bytes);
            }
            vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, NULL);
            write_to_guest_mem(all_para[1].data, &count, 0, sizeof(count));
            result = VK_SUCCESS;
        } else {
            props = malloc(sizeof(VkQueueFamilyProperties) * count);
            if (!props) {
                result = VK_ERROR_OUT_OF_HOST_MEMORY;
            } else {
                vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, props);
                            const VkQueueFlags allowed =
                VK_QUEUE_GRAPHICS_BIT |
                VK_QUEUE_COMPUTE_BIT |
                VK_QUEUE_TRANSFER_BIT |
                VK_QUEUE_SPARSE_BINDING_BIT |
                VK_QUEUE_PROTECTED_BIT;

                for (uint32_t i = 0; i < count; ++i) {
                    props[i].queueFlags &= allowed;
                }
                write_to_guest_mem(guest_props_ptr, props, 0,
                                   sizeof(VkQueueFamilyProperties) * count);
                free(props);
                result = VK_SUCCESS;
            }
        }

        LOGD("Host: vkGetPhysicalDeviceQueueFamilyProperties done, count=%d", count);
        break;
    }

    case FUNID_vkGetPhysicalDeviceQueueFamilyProperties2: {
        LOGD("Host: vkGetPhysicalDeviceQueueFamilyProperties2 request");
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);

        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint64_t guest_pd = *(uint64_t*)stream;

        uint32_t count = 0;
        if (!copy_from_call_para_fast(all_para[1], &count, sizeof(count))) {
            LOGE("Host: vkGetPhysicalDeviceQueueFamilyProperties2 failed to read count");
            break;
        }
        void* guest_props_ptr = all_para[2].data;
        uint64_t guest_props_len = all_para[2].data_len;


        VkPhysicalDevice pd = (VkPhysicalDevice)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_PHYSICAL_DEVICE, guest_pd);

        VkQueueFamilyProperties2* props = NULL;
        VkResult result;

        uint64_t required_props_bytes =
            (uint64_t)sizeof(VkQueueFamilyProperties2) * (uint64_t)count;
        bool can_write_props = guest_props_ptr != NULL &&
                               guest_props_len >= required_props_bytes &&
                               required_props_bytes != 0;

        if (count == 0 || !can_write_props) {
            if (count != 0) {
                LOGD("Host: vkGetPhysicalDeviceQueueFamilyProperties2 count-only/null-output "
                     "input_count=%u guest_props=%p guest_len=%llu required=%llu",
                     count,
                     guest_props_ptr,
                     (unsigned long long)guest_props_len,
                     (unsigned long long)required_props_bytes);
            }
            vkGetPhysicalDeviceQueueFamilyProperties2(pd, &count, NULL);
            write_to_guest_mem(all_para[1].data, &count, 0, sizeof(count));
            result = VK_SUCCESS;
        } else {
            props = calloc(count, sizeof(VkQueueFamilyProperties2));
            if (!props) {
                result = VK_ERROR_OUT_OF_HOST_MEMORY;
            } else {
            for(uint32_t i = 0; i < count; ++i) {
                props[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
                props[i].pNext = NULL;
            }
                vkGetPhysicalDeviceQueueFamilyProperties2(pd, &count, props);
                const VkQueueFlags allowed =
                VK_QUEUE_GRAPHICS_BIT |
                VK_QUEUE_COMPUTE_BIT |
                VK_QUEUE_TRANSFER_BIT |
                VK_QUEUE_SPARSE_BINDING_BIT |
                VK_QUEUE_PROTECTED_BIT;

                for (uint32_t i = 0; i < count; ++i) {
                    props[i].queueFamilyProperties.queueFlags &= allowed;
                }
                write_to_guest_mem(guest_props_ptr, props, 0,
                                   sizeof(VkQueueFamilyProperties2) * count);
                free(props);
                result = VK_SUCCESS;
            }
        }

        LOGD("Host: vkGetPhysicalDeviceQueueFamilyProperties2 done, count=%d", count);
        break;
    }

    case FUNID_vkGetImageSubresourceLayout: {
        LOGD("Host: vkGetImageSubresourceLayout request");
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        LOGD("get vkGetImageSubresourceLayout param count %d", para_num);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_device = *(uint64_t*)(*ptr);  *ptr += sizeof(uint64_t);
        uint64_t guest_image  = *(uint64_t*)(*ptr);  *ptr += sizeof(uint64_t);

        VkImageSubresource subres;
        if (!copy_from_call_para_fast(all_para[1], &subres, sizeof(subres))) {
            LOGE("Host: vkGetImageSubresourceLayout failed to read subresource");
            break;
        }

        void* guest_layout_ptr = all_para[2].data;

        VkDevice device = (VkDevice)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_device);
        VkImage image   = (VkImage)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_IMAGE, guest_image);

        VkSubresourceLayout layout;
        vkGetImageSubresourceLayout(device, image, &subres, &layout);

        write_to_guest_mem(guest_layout_ptr, &layout, 0, sizeof(layout));
        LOGD("Host: vkGetImageSubresourceLayout done with offset %lld size %lld",
             (long long)layout.offset, (long long)layout.size);
        break;
    }

    case FUNID_vkCreateImage: {
        LOGD("Host: vkCreateImage request");
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        LOGD("get vkCreateImage para count %d", para_num);

        int need_free = 0;
        char* buf = call_para_to_ptr(all_para[0], &need_free);
        uint8_t* ptr = (uint8_t*)buf;

        VkImageCreateInfo createInfo;
        decode_from_stream_VkImageCreateInfo(
            VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            &createInfo,
            (uint8_t**)&ptr);
        uint64_t guest_alloc_ptr = *(uint64_t*)ptr;
        ptr += sizeof(uint64_t);

        uint64_t guest_device = *(uint64_t*)ptr;  ptr += sizeof(uint64_t);
        uint64_t guest_image  = *(uint64_t*)ptr;  ptr += sizeof(uint64_t);
        LOGD("Decoded createInfo + guest_alloc=0x%llx, device=0x%llx, image=0x%llx layout %d",
            guest_alloc_ptr, guest_device, guest_image, createInfo.initialLayout);

        VkDevice device = (VkDevice)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_device);

        VkImage hostImage;
        VkResult result = vkCreateImage(device, &createInfo, NULL, &hostImage);
        LOGD("vkCreateImage %d, hostImage=0x%llx", result, (uint64_t)(uintptr_t)hostImage);

        if (result == VK_SUCCESS) {
            insert_mapping(EXPRESS_VK_OBJECT_TYPE_IMAGE,
                        guest_image,
                        (uint64_t)(uintptr_t)hostImage);
            LOGD("Mapped guest_image 0x%llx ? hostImage 0x%llx",
                guest_image, (uint64_t)(uintptr_t)hostImage);
        } else {
            LOGE("vkCreateImage failed with error %d", result);
        }

        write_to_guest_mem(all_para[1].data, &result, 0, sizeof(result));

        if (need_free) g_free(buf);
        break;
    }

    case FUNID_vkCreateSampler: {
        LOGD("Host: vkCreateSampler request");
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        LOGD("get vkCreateSampler para count %d", para_num);

        int need_free = 0;
        char* buf = call_para_to_ptr(all_para[0], &need_free);
        uint8_t* ptr = (uint8_t*)buf;

        VkSamplerCreateInfo samplerInfo;
        decode_from_stream_VkSamplerCreateInfo(
            VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            &samplerInfo,
            (uint8_t**)&ptr);
        uint64_t guest_alloc_ptr = *(uint64_t*)ptr;
        ptr += sizeof(uint64_t);

        uint64_t guest_device  = *(uint64_t*)ptr; ptr += sizeof(uint64_t);
        uint64_t guest_sampler = *(uint64_t*)ptr; ptr += sizeof(uint64_t);
        LOGD("Decoded samplerInfo + guest_alloc=0x%llx, device=0x%llx, sampler=0x%llx",
            guest_alloc_ptr, guest_device, guest_sampler);

        VkDevice device = (VkDevice)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_device);

        VkSampler hostSampler;
        VkResult result = vkCreateSampler(device, &samplerInfo, NULL, &hostSampler);
        LOGD("vkCreateSampler ? %d, hostSampler=0x%llx", result, (uint64_t)(uintptr_t)hostSampler);

        if (result == VK_SUCCESS) {
            insert_mapping(EXPRESS_VK_OBJECT_TYPE_SAMPLER,
                        guest_sampler,
                        (uint64_t)(uintptr_t)hostSampler);
            LOGD("Mapped guest_sampler 0x%llx ? hostSampler 0x%llx",
                guest_sampler, (uint64_t)(uintptr_t)hostSampler);
        } else {
            LOGE("vkCreateSampler failed with error %d", result);
        }

        write_to_guest_mem(all_para[1].data, &result, 0, sizeof(result));

        if (need_free) g_free(buf);
        break;
    }

    case FUNID_vkCreateDescriptorSetLayout: {
        LOGD("Host: vkCreateDescriptorSetLayout request");
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        LOGD("get vkCreateDescriptorSetLayout para count %d", para_num);

        int need_free = 0;
        char* buf = call_para_to_ptr(all_para[0], &need_free);
        uint8_t* ptr = (uint8_t*)buf;

        VkDescriptorSetLayoutCreateInfo layoutInfo;
        decode_from_stream_VkDescriptorSetLayoutCreateInfo(
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            &layoutInfo,
            (uint8_t**)&ptr);
        uint64_t guest_alloc_ptr = *(uint64_t*)ptr;
        ptr += sizeof(uint64_t);

        uint64_t guest_device    = *(uint64_t*)ptr; ptr += sizeof(uint64_t);
        uint64_t guest_layout    = *(uint64_t*)ptr; ptr += sizeof(uint64_t);
        LOGD("Decoded layoutInfo + guest_alloc=0x%llx, device=0x%llx, layout=0x%llx",
            guest_alloc_ptr, guest_device, guest_layout);

        VkDevice device = (VkDevice)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_device);

        VkDescriptorSetLayout hostLayout;
        VkResult result = vkCreateDescriptorSetLayout(
            device, &layoutInfo, NULL, &hostLayout);
        LOGD("vkCreateDescriptorSetLayout ? %d, hostLayout=0x%llx",
            result, (uint64_t)(uintptr_t)hostLayout);

        if (result == VK_SUCCESS) {
            insert_mapping(EXPRESS_VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT,
                        guest_layout,
                        (uint64_t)(uintptr_t)hostLayout);
            LOGD("Mapped guest_layout 0x%llx ? hostLayout 0x%llx",
                guest_layout, (uint64_t)(uintptr_t)hostLayout);
        } else {
            LOGE("vkCreateDescriptorSetLayout failed with error %d", result);
        }

        if (para_num > 1) {
            write_to_guest_mem(all_para[1].data, &result, 0,
                               sizeof(result));
        }

        if (need_free) g_free(buf);
        break;
    }

    case FUNID_vkBindImageMemory: {
        LOGD("Host: vkBindImageMemory request");
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        LOGD("get vkBindImageMemory para count %d", para_num);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_device = *(uint64_t*)(*ptr);  *ptr += sizeof(uint64_t);
        uint64_t guest_image  = *(uint64_t*)(*ptr);  *ptr += sizeof(uint64_t);
        uint64_t guest_mem    = *(uint64_t*)(*ptr);  *ptr += sizeof(uint64_t);
        uint64_t offset       = *(uint64_t*)(*ptr);  *ptr += sizeof(uint64_t);
        LOGD("guest_device=0x%llx, guest_image=0x%llx, guest_mem=0x%llx, offset=%llu",
             guest_device, guest_image, guest_mem, offset);

        VkDevice device = (VkDevice)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_device);
        VkImage  image  = (VkImage)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_IMAGE, guest_image);
        VkDeviceMemory mem = (VkDeviceMemory)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE_MEMORY, guest_mem);

        VkResult result = vkBindImageMemory(device, image, mem, offset);
        LOGD("vkBindImageMemory returned %d", result);

        break;
    }

    case FUNID_vkFreeDescriptorSets: {
        LOGD("Host: vkFreeDescriptorSets request");
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        LOGD("get vkFreeDescriptorSets para count %d", para_num);

        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;
        uint64_t guest_device = *(uint64_t*)(*ptr);  *ptr += sizeof(uint64_t);
        uint64_t guest_pool   = *(uint64_t*)(*ptr);  *ptr += sizeof(uint64_t);
        uint32_t count        = *(uint32_t*)(*ptr);  *ptr += sizeof(uint32_t);
        g_mutex_lock(&g_express_vk_transaction_lock);
        LOGD("guest_device=0x%llx, guest_pool=0x%llx, count=%d",
             guest_device, guest_pool, count);

        VkDevice device = (VkDevice)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_device);
        LOGD("current guest_pool is 0x%llx, host device is 0x%llx",
             guest_pool, (uint64_t)(uintptr_t)device);
        VkDescriptorPool pool = (VkDescriptorPool)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DESCRIPTOR_POOL, guest_pool);

        VkDescriptorSet* hostSets = NULL;
        uint64_t* guest_sets = NULL;
        VkResult result = VK_ERROR_OUT_OF_HOST_MEMORY;
        if (count > 0) {
            hostSets = g_try_new(VkDescriptorSet, count);
            guest_sets = g_try_new(uint64_t, count);
            if (hostSets == NULL || guest_sets == NULL) {
                g_mutex_unlock(&g_express_vk_transaction_lock);
                goto VK_FREE_DESCRIPTOR_SETS_DONE;
            }
            for (uint32_t i = 0; i < count; ++i) {
                guest_sets[i] = *(uint64_t*)(*ptr);
                *ptr += sizeof(uint64_t);
                hostSets[i] = express_vk_lookup_descriptor_set(guest_sets[i]);
                LOGD("  mapped guest_set[%u]=0x%llx to host 0x%llx",
                     i, guest_sets[i], (uint64_t)(uintptr_t)hostSets[i]);
            }
        }

        result = vkFreeDescriptorSets(device, pool, count, hostSets);
        if (result == VK_SUCCESS) {
            for (uint32_t i = 0; i < count; ++i) {
                if (guest_sets[i] != 0) {
                    remove_mapping(EXPRESS_VK_OBJECT_TYPE_DESCRIPTOR_SET,
                                   guest_sets[i]);
                    express_vk_forget_descriptor_set(guest_sets[i]);
                }
            }
        }
        g_mutex_unlock(&g_express_vk_transaction_lock);

VK_FREE_DESCRIPTOR_SETS_DONE:
        if (para_num > 1) {
            write_to_guest_mem(all_para[1].data, &result, 0,
                               sizeof(result));
        }
        LOGD("vkFreeDescriptorSets returned %d", result);
        g_free(guest_sets);
        g_free(hostSets);

        break;
    }

    case FUNID_vkCreateDescriptorPool:
    {
        gint64 hot_start_us = g_get_monotonic_time();

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);

        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** stream_ptr = (uint8_t**)&stream;

        VkDescriptorPoolCreateInfo* pCreateInfo = (VkDescriptorPoolCreateInfo*)malloc(sizeof(VkDescriptorPoolCreateInfo));
        decode_from_stream_VkDescriptorPoolCreateInfo(
            VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            pCreateInfo,
            stream_ptr);

        VkAllocationCallbacks* guest_allocator = (VkAllocationCallbacks*)*(uint64_t*)(*stream_ptr);
        *stream_ptr += sizeof(uint64_t);

        const VkAllocationCallbacks* pAllocator = NULL;
        if (guest_allocator) {
            VkAllocationCallbacks* temp_allocator = (VkAllocationCallbacks*)malloc(sizeof(VkAllocationCallbacks));
            decode_from_stream_VkAllocationCallbacks(
                VK_STRUCTURE_TYPE_MAX_ENUM,
                temp_allocator,
                stream_ptr);

            free(temp_allocator);
        }

        uint64_t guest_device = *(uint64_t*)(*stream_ptr);
        *stream_ptr += sizeof(uint64_t);

        uint64_t guest_descriptor_pool = *(uint64_t*)(*stream_ptr);
        *stream_ptr += sizeof(uint64_t);

        VkDevice device = (VkDevice)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_device);

        VkDescriptorPool descriptor_pool;
        VkResult result = vkCreateDescriptorPool(device, pCreateInfo, pAllocator, &descriptor_pool);
        if (para_num > 1) {
            write_to_guest_mem(all_para[1].data, &result, 0,
                               sizeof(result));
        }

        if (result == VK_SUCCESS) {
            insert_mapping(EXPRESS_VK_OBJECT_TYPE_DESCRIPTOR_POOL,
                        guest_descriptor_pool,
                        (uint64_t)(uintptr_t)descriptor_pool);
        } else {
            LOGE("Host: vkCreateDescriptorPool failed with result=%d", result);
        }

        free(pCreateInfo);
        if (need_free) {
            free(stream);
        }
        express_vk_host_note_descriptor_timing(
            0, (uint64_t)(g_get_monotonic_time() - hot_start_us));

        break;
    }

    case FUNID_vkFlushMappedMemoryRanges:
    {
        LOGD("Host: vkFlushMappedMemoryRanges request");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);

        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_device = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        VkDevice device = (VkDevice)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_device);

        uint32_t memoryRangeCount = *(uint32_t*)(*ptr);
        *ptr += sizeof(uint32_t);

        VkMappedMemoryRange* pMemoryRanges = NULL;
        if (memoryRangeCount > 0) {
            pMemoryRanges = (VkMappedMemoryRange*)malloc(
                memoryRangeCount * sizeof(VkMappedMemoryRange));

            for (uint32_t i = 0; i < memoryRangeCount; ++i) {
                decode_from_stream_VkMappedMemoryRange(VK_STRUCTURE_TYPE_MAX_ENUM,
                                                    &pMemoryRanges[i], ptr);
            }
        }
        int data_para_index = 1;
        for (uint32_t i = 0; i < memoryRangeCount; ++i) {
            uint64_t guest_memory = (uint64_t)(uintptr_t)pMemoryRanges[i].memory;
            ExpressVkRegisteredMemory *reg = express_vk_find_registered_memory(guest_memory, 0);
            if (reg == NULL) {
                reg = express_vk_find_registered_memory(0, guest_memory);
            }

            uint64_t host_memory = reg ? reg->host_memory : 0;
            if (host_memory == 0) {
                host_memory = lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE_MEMORY, guest_memory);
                if (host_memory != 0) {
                    pMemoryRanges[i].memory = (VkDeviceMemory)(uintptr_t)host_memory;
                } else {
                    host_memory = (uint64_t)(uintptr_t)pMemoryRanges[i].memory;
                }
            } else {
                pMemoryRanges[i].memory = (VkDeviceMemory)(uintptr_t)host_memory;
            }
            void* hostPtr = get_memory_map(host_memory);
            if (i < 2) {
                LOGD("[SYNC_DBG] flush i=%u guest_mem=%p host_map=%p offset=%llu size=%llu para_len=%zu",
                    i,
                    (void*)pMemoryRanges[i].memory,
                    hostPtr,
                    (unsigned long long)pMemoryRanges[i].offset,
                    (unsigned long long)pMemoryRanges[i].size,
                    (data_para_index < para_num ? (size_t)all_para[data_para_index].data_len : (size_t)0));
            }
            if (hostPtr && reg && reg->guest_mem) {
                struct timespec t0_sync, t1_sync;
                clock_gettime(CLOCK_MONOTONIC, &t0_sync);

                uint64_t sync_len = express_vk_registered_range_size(
                    reg,
                    (uint64_t)pMemoryRanges[i].offset,
                    (uint64_t)pMemoryRanges[i].size);
                bool async_started = false;
                if (sync_len >= EXPRESS_VK_TRANSFER_UPLOAD_THRESHOLD) {
                    async_started = express_vk_submit_transfer(
                        EXPRESS_VK_TRANSFER_UPLOAD,
                        reg,
                        device,
                        host_memory,
                        hostPtr,
                        (uint64_t)pMemoryRanges[i].offset,
                        sync_len);
                }

                if (sync_len != 0 && !async_started) {
                    read_from_guest_mem(
                        reg->guest_mem,
                        (char*)hostPtr + pMemoryRanges[i].offset,
                        pMemoryRanges[i].offset,
                        sync_len);
                }

                clock_gettime(CLOCK_MONOTONIC, &t1_sync);
                double sync_ms = (t1_sync.tv_sec - t0_sync.tv_sec) * 1000.0 + (t1_sync.tv_nsec - t0_sync.tv_nsec) / 1000000.0;
                LOGD("[ExpressVkMem] vkFlushMappedMemoryRanges %s registered %llu bytes to host in %.3f ms",
                    async_started ? "queued" : "synced",
                    (unsigned long long)sync_len,
                    sync_ms);
                express_vk_stats_add_u64(async_started ?
                                         &g_express_vk_stats.flush_queued :
                                         &g_express_vk_stats.flush_synced,
                                         1);
            } else if (hostPtr && data_para_index < para_num) {
                // all_para[1].data supplies addPtr(mem->map_data, mem->length).
                // all_para[1].data_len supplies mem->length.
                struct timespec t0_sync, t1_sync;
                clock_gettime(CLOCK_MONOTONIC, &t0_sync);

                read_from_guest_mem(
                    all_para[data_para_index].data,
                    (char*)hostPtr + pMemoryRanges[i].offset,
                    0,
                    all_para[data_para_index].data_len);

                clock_gettime(CLOCK_MONOTONIC, &t1_sync);
                double sync_ms = (t1_sync.tv_sec - t0_sync.tv_sec) * 1000.0 + (t1_sync.tv_nsec - t0_sync.tv_nsec) / 1000000.0;
                LOGD("[SYNC_TIME] vkFlushMappedMemoryRanges synced %zu bytes to host in %.3f ms", (size_t)all_para[data_para_index].data_len, sync_ms);
                data_para_index++;
            } else {
                LOGE("Host: no mapping found for guest_mem %llu", pMemoryRanges[i].memory);
            }
        }

        VkResult result = vkFlushMappedMemoryRanges(device, memoryRangeCount, pMemoryRanges);

        if (result != VK_SUCCESS) {
            LOGE("Host: vkFlushMappedMemoryRanges failed with result %d", result);
        }

        if (pMemoryRanges) free(pMemoryRanges);
        //if (need_free) free(stream);
    }
    break;

    case FUNID_vkFreeCommandBuffers:
    {
        LOGD("Host: vkFreeCommandBuffers request");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);

        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_device = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        uint64_t guest_command_pool = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        uint32_t commandBufferCount = *(uint32_t*)(*ptr);
        *ptr += sizeof(uint32_t);

        VkDevice device = (VkDevice)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_device);

        VkCommandPool commandPool = (VkCommandPool)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_POOL, guest_command_pool);

        VkCommandBuffer* pCommandBuffers = malloc(commandBufferCount * sizeof(VkCommandBuffer));
        if (!pCommandBuffers) {
            break;
        }

        for (uint32_t i = 0; i < commandBufferCount; ++i) {
            uint64_t guest_cmd_buffer = *(uint64_t*)(*ptr);
            *ptr += sizeof(uint64_t);

            pCommandBuffers[i] = (VkCommandBuffer)(uintptr_t)
                lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd_buffer);

            remove_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd_buffer);
        }

        vkFreeCommandBuffers(device, commandPool, commandBufferCount, pCommandBuffers);

        free(pCommandBuffers);
    }
    break;

    case FUNID_vkResetCommandBuffer: {
        LOGD("Host: vkResetCommandBuffer");
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd_buf = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        VkCommandBufferResetFlags flags = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);
        VkCommandBuffer cmd_buf = (VkCommandBuffer)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd_buf);
        VkResult result = vkResetCommandBuffer(cmd_buf, flags);
        if (para_num > 1) {
            write_to_guest_mem(all_para[1].data, &result, 0,
                               sizeof(result));
        }
        LOGD("Host: vkResetCommandBuffer result=%d", result);
        //if (need_free) free(stream);
    }
    break;

    case FUNID_vkResetCommandPool: {
        LOGD("Host: vkResetCommandPool");
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_device = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint64_t guest_pool = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        VkCommandPoolResetFlags flags = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);
        VkDevice device = (VkDevice)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_device);
        VkCommandPool pool = (VkCommandPool)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_POOL, guest_pool);
        VkResult result = vkResetCommandPool(device, pool, flags);
        if (para_num > 1) {
            write_to_guest_mem(all_para[1].data, &result, 0,
                               sizeof(result));
        }
        LOGD("Host: vkResetCommandPool result=%d", result);
        //if (need_free) free(stream);
    }
    break;

    case FUNID_vkResetDescriptorPool: {
        LOGD("Host: vkResetDescriptorPool");
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_device = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint64_t guest_pool = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        VkDescriptorPoolResetFlags flags = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);
        g_mutex_lock(&g_express_vk_transaction_lock);
        VkDevice device = (VkDevice)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_device);
        VkDescriptorPool pool = (VkDescriptorPool)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DESCRIPTOR_POOL, guest_pool);
        VkResult result = vkResetDescriptorPool(device, pool, flags);
        if (result == VK_SUCCESS) {
            express_vk_forget_descriptor_sets_for_pool(pool);
        }
        g_mutex_unlock(&g_express_vk_transaction_lock);
        if (para_num > 1) {
            write_to_guest_mem(all_para[1].data, &result, 0,
                               sizeof(result));
        }
        LOGD("Host: vkResetDescriptorPool result=%d", result);
        //if (need_free) free(stream);
    }
    break;

    case FUNID_vkResetEvent: {
        LOGD("Host: vkResetEvent");
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_device = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint64_t guest_event = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        VkDevice device = (VkDevice)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_device);
        VkEvent event = (VkEvent)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_EVENT, guest_event);
        VkResult result = vkResetEvent(device, event);
        LOGD("Host: vkResetEvent result=%d", result);
        //if (need_free) free(stream);
    }
    break;

    case FUNID_vkResetQueryPool: {
        LOGD("Host: vkResetQueryPool");
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_device = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint64_t guest_pool = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint32_t firstQuery = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);
        uint32_t queryCount = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);
        VkDevice device = (VkDevice)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_device);
        VkQueryPool pool = (VkQueryPool)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_QUERY_POOL, guest_pool);
        vkResetQueryPool(device, pool, firstQuery, queryCount);
        LOGD("Host: vkResetQueryPool device=%p pool=%p firstQuery=%u queryCount=%u", (void*)device, (void*)pool, firstQuery, queryCount);
        //if (need_free) free(stream);
    }
    break;

    case FUNID_vkAllocateDescriptorSets:
    {
        gint64 hot_start_us = g_get_monotonic_time();

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);

        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_device = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        VkDevice device = (VkDevice)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_device);

        VkDescriptorSetAllocateInfo allocate_info;
        decode_from_stream_VkDescriptorSetAllocateInfo(VK_STRUCTURE_TYPE_MAX_ENUM, &allocate_info, ptr);

        const size_t descriptor_sets_size =
            (size_t)allocate_info.descriptorSetCount * sizeof(VkDescriptorSet);
        const size_t guest_handles_size =
            (size_t)allocate_info.descriptorSetCount * sizeof(uint64_t);
        VkDescriptorSet *host_descriptor_sets = NULL;
        uint64_t *guest_descriptor_sets = NULL;
        VkResult result = VK_ERROR_OUT_OF_HOST_MEMORY;

        if (allocate_info.descriptorSetCount != 0 &&
            descriptor_sets_size / sizeof(VkDescriptorSet) ==
                allocate_info.descriptorSetCount &&
            guest_handles_size / sizeof(uint64_t) ==
                allocate_info.descriptorSetCount) {
            host_descriptor_sets = g_try_new0(
                VkDescriptorSet, allocate_info.descriptorSetCount);
            guest_descriptor_sets = g_try_new(
                uint64_t, allocate_info.descriptorSetCount);
        }

        /*
         * Guest wrapper handles are part of the request, not Vulkan output.
         * Fetch them before allocating host sets so a malformed/short RPC can
         * never strand live VkDescriptorSet objects on the host.
         */
        if (!host_descriptor_sets || !guest_descriptor_sets ||
            !copy_from_call_para_fast(all_para[1], guest_descriptor_sets,
                                      guest_handles_size)) {
            LOGE("Host: vkAllocateDescriptorSets could not materialize handle arrays");
            result = VK_ERROR_DEVICE_LOST;
        } else {
            result = vkAllocateDescriptorSets(
                device, &allocate_info, host_descriptor_sets);
        }

        if (result == VK_SUCCESS) {

            for (uint32_t i = 0; i < allocate_info.descriptorSetCount; ++i) {
                uint64_t host_desc_set = (uint64_t)(uintptr_t)host_descriptor_sets[i];
                insert_mapping(EXPRESS_VK_OBJECT_TYPE_DESCRIPTOR_SET,
                            guest_descriptor_sets[i], host_desc_set);
                express_vk_remember_descriptor_set(guest_descriptor_sets[i],
                                                   host_descriptor_sets[i],
                                                   allocate_info.descriptorPool);
            }
        } else {
            LOGE("Host: vkAllocateDescriptorSets failed with result %d", result);
        }

        if (para_num > 2) {
            write_to_guest_mem(all_para[2].data, &result, 0,
                               sizeof(result));
        }

        g_free(guest_descriptor_sets);
        g_free(host_descriptor_sets);

        //if (need_free) free(stream);
        express_vk_host_note_descriptor_timing(
            1, (uint64_t)(g_get_monotonic_time() - hot_start_us));
    }
    break;

    case FUNID_vkUpdateDescriptorSets:
    {
        gint64 hot_start_us = g_get_monotonic_time();

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);

        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_device = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        VkDevice device = (VkDevice)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_device);

        uint32_t descriptorWriteCount = *(uint32_t*)(*ptr);
        *ptr += sizeof(uint32_t);
        uint32_t descriptorCopyCount = *(uint32_t*)(*ptr);
        *ptr += sizeof(uint32_t);

        VkWriteDescriptorSet* pDescriptorWrites = NULL;
        if (descriptorWriteCount > 0) {
            pDescriptorWrites = (VkWriteDescriptorSet*)malloc(
                descriptorWriteCount * sizeof(VkWriteDescriptorSet));

            for (uint32_t i = 0; i < descriptorWriteCount; ++i) {
                decode_from_stream_VkWriteDescriptorSet(VK_STRUCTURE_TYPE_MAX_ENUM,
                                                    &pDescriptorWrites[i], ptr);
                // LOGD("Host: vkUpdateDescriptorSets copy %d: %llx",
                //     i, (long long)pDescriptorWrites[i].pImageInfo[0].imageView);
                // if (pDescriptorWrites[i].descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
                    // pDescriptorWrites[i].descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) {


                //     for (uint32_t j = 0; j < pDescriptorWrites[i].descriptorCount; ++j) {
                //         VkImageView imageView = pDescriptorWrites[i].pImageInfo[j].imageView;




                //         if (hostImage != VK_NULL_HANDLE) {
                //             LOGD("Host: Found image for layout transition, imageView=%llx",
                //                 (long long)imageView);


                //             // transitionImageLayoutForSampling(device, hostImage, pDescriptorWrites[i].pImageInfo[j].imageLayout);

                //             VkDescriptorImageInfo imageInfo = pDescriptorWrites[i].pImageInfo[j];
                //         }
                //     }
                // }
            }
        }

        VkCopyDescriptorSet* pDescriptorCopies = NULL;
        if (descriptorCopyCount > 0) {
            pDescriptorCopies = (VkCopyDescriptorSet*)malloc(
                descriptorCopyCount * sizeof(VkCopyDescriptorSet));

            for (uint32_t i = 0; i < descriptorCopyCount; ++i) {
                decode_from_stream_VkCopyDescriptorSet(VK_STRUCTURE_TYPE_MAX_ENUM,
                                                    &pDescriptorCopies[i], ptr);
                // LOGD("Host: vkUpdateDescriptorSets copy %d: %llx",
                //     i, (long long)pDescriptorCopies[i].pImageInfo.imageView);
            }
        }


#if EXPRESS_VK_ENABLE_DESCRIPTOR_TRACE
        uint32_t desc_logged = 0;
        for (uint32_t i = 0; i < descriptorWriteCount && desc_logged < 16; ++i) {
            VkWriteDescriptorSet *write = &pDescriptorWrites[i];
            if (write == NULL || write->pBufferInfo == NULL) {
                continue;
            }
            if (write->descriptorType != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER &&
                write->descriptorType != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER &&
                write->descriptorType != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC &&
                write->descriptorType != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) {
                continue;
            }
            bool set_found = false;
            VkDescriptorSet cached_set =
                express_vk_lookup_descriptor_set_cached(
                    (uint64_t)(uintptr_t)write->dstSet, &set_found);
            for (uint32_t j = 0; j < write->descriptorCount && desc_logged < 16; ++j) {
                const VkDescriptorBufferInfo *info = &write->pBufferInfo[j];
                uint64_t range = (uint64_t)info->range;
                if (range != VK_WHOLE_SIZE && range < EXPRESS_VK_DESC_TRACE_MIN_BYTES) {
                    continue;
                }
                uint64_t mapped_buffer = info->buffer;
                LOGD("[HOST_DESC] update raw_set=%p cached_found=%d cached_set=%p binding=%u elem=%u type=%u raw_buffer=%p mapped_buffer=%p offset=%llu range=%llu",
                     (void*)write->dstSet,
                     set_found,
                     (void*)cached_set,
                     write->dstBinding,
                     write->dstArrayElement + j,
                     write->descriptorType,
                     (void*)info->buffer,
                     (void*)(uintptr_t)mapped_buffer,
                     (unsigned long long)info->offset,
                     (unsigned long long)range);
                desc_logged++;
            }
        }
#endif

        vkUpdateDescriptorSets(device, descriptorWriteCount, pDescriptorWrites,
                            descriptorCopyCount, pDescriptorCopies);

        if (pDescriptorWrites) free(pDescriptorWrites);
        if (pDescriptorCopies) free(pDescriptorCopies);
        //if (need_free) free(stream);
        express_vk_host_note_descriptor_timing(
            2, (uint64_t)(g_get_monotonic_time() - hot_start_us));
    }
    break;

    case FUNID_vkCmdBindDescriptorSets:
    {
        gint64 hot_start_us = g_get_monotonic_time();

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        VkPipelineBindPoint bindPoint = *(VkPipelineBindPoint*)(*ptr); *ptr += sizeof(uint32_t);
        uint64_t guest_layout = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint32_t firstSet = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);
        uint32_t setCount = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);
        uint32_t dynamicOffsetCount = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);

        VkCommandBuffer commandBuffer = (VkCommandBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd);
        VkPipelineLayout layout = (VkPipelineLayout)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_PIPELINE_LAYOUT, guest_layout);

        VkDescriptorSet* descriptorSets = NULL;
        uint32_t* dynamicOffsets = NULL;

        if (setCount > 0 && para_num > 1) {
            uint64_t* guest_sets = (uint64_t*)malloc(setCount * sizeof(uint64_t));
            if (!guest_sets ||
                !copy_from_call_para_fast(all_para[1], guest_sets, setCount * sizeof(uint64_t))) {
                LOGE("Host: vkCmdBindDescriptorSets failed to get guest descriptor sets");
                if (guest_sets) free(guest_sets);
                break;
            }

            descriptorSets = (VkDescriptorSet*)malloc(setCount * sizeof(VkDescriptorSet));
            for (uint32_t i = 0; i < setCount; ++i) {
                descriptorSets[i] = express_vk_lookup_descriptor_set(guest_sets[i]);
#if EXPRESS_VK_ENABLE_DESCRIPTOR_TRACE
                LOGD("[HOST_DESC] bind cmd_guest=0x%llx cmd_host=%p set_index=%u guest_set=0x%llx host_set=%p firstSet=%u bindPoint=%u dyn_count=%u",
                     (unsigned long long)guest_cmd,
                     (void*)commandBuffer,
                     i,
                     (unsigned long long)guest_sets[i],
                     (void*)descriptorSets[i],
                     firstSet,
                     (uint32_t)bindPoint,
                     dynamicOffsetCount);
#endif
            }
            free(guest_sets);

            if (para_num > 2) {
                // Read dynamic offset count from guest memory structure
                if (dynamicOffsetCount > 0) {
                    dynamicOffsets = (uint32_t*)malloc(dynamicOffsetCount * sizeof(uint32_t));
                    if (!dynamicOffsets ||
                        !copy_from_call_para_fast(all_para[2], dynamicOffsets,
                                                  dynamicOffsetCount * sizeof(uint32_t))) {
                        LOGE("Host: vkCmdBindDescriptorSets failed to get dynamic offsets");
                        if (dynamicOffsets) free(dynamicOffsets);
                        dynamicOffsets = NULL;
                    }
                }
            }
        } else if(dynamicOffsetCount > 0) {
            // If no descriptor sets, but dynamic offsets are provided
            dynamicOffsets = (uint32_t*)malloc(dynamicOffsetCount * sizeof(uint32_t));
            if (!dynamicOffsets ||
                !copy_from_call_para_fast(all_para[1], dynamicOffsets,
                                          dynamicOffsetCount * sizeof(uint32_t))) {
                LOGE("Host: vkCmdBindDescriptorSets failed to get dynamic offsets(no sets)");
                if (dynamicOffsets) free(dynamicOffsets);
                dynamicOffsets = NULL;
            }
        }
#if EXPRESS_VK_ENABLE_DESCRIPTOR_TRACE
        if (dynamicOffsetCount > 0 && dynamicOffsets != NULL) {
            uint32_t log_count = dynamicOffsetCount < 8 ? dynamicOffsetCount : 8;
            for (uint32_t i = 0; i < log_count; ++i) {
                LOGD("[HOST_DESC] bind_dynamic cmd_guest=0x%llx index=%u value=%u",
                     (unsigned long long)guest_cmd,
                     i,
                     dynamicOffsets[i]);
            }
        }
#endif
        vkCmdBindDescriptorSets(commandBuffer, bindPoint, layout, firstSet, setCount,
                            descriptorSets, dynamicOffsetCount, dynamicOffsets);

        if (descriptorSets) free(descriptorSets);
        if (dynamicOffsets) free(dynamicOffsets);
        express_vk_host_note_descriptor_timing(
            4, (uint64_t)(g_get_monotonic_time() - hot_start_us));
    }
    break;

    case FUNID_vkCmdCopyImage:
    {
        LOGD("Host: vkCmdCopyImage request");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint64_t guest_src = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        VkImageLayout srcLayout = *(VkImageLayout*)(*ptr); *ptr += sizeof(uint32_t);
        uint64_t guest_dst = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        VkImageLayout dstLayout = *(VkImageLayout*)(*ptr); *ptr += sizeof(uint32_t);
        uint32_t regionCount = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);

        VkCommandBuffer commandBuffer = (VkCommandBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd);
        VkImage srcImage = (VkImage)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_IMAGE, guest_src);
        VkImage dstImage = (VkImage)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_IMAGE, guest_dst);

        VkImageCopy* regions = NULL;
        if (regionCount > 0 && para_num > 1) {
            regions = (VkImageCopy*)malloc(regionCount * sizeof(VkImageCopy));
            if (!regions ||
                !copy_from_call_para_fast(all_para[1], regions, regionCount * sizeof(VkImageCopy))) {
                LOGE("Host: vkCmdCopyImage failed to read image copy regions");
                if (regions) free(regions);
                break;
            }
        }

        vkCmdCopyImage(commandBuffer, srcImage, srcLayout, dstImage, dstLayout, regionCount, regions);

        if (regions) free(regions);
    }
    break;
    case FUNID_vkFreeMemory:
    {
        LOGD("Host: vkFreeMemory request");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_device = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint64_t guest_memory = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);

        VkDevice device = (VkDevice)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_device);
        VkDeviceMemory memory = (VkDeviceMemory)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE_MEMORY, guest_memory);

        ExpressVkRegisteredMemory *reg = express_vk_find_registered_memory(guest_memory, (uint64_t)(uintptr_t)memory);
        if (reg) {
            express_vk_wait_transfers(reg, EXPRESS_VK_TRANSFER_ANY, "free_memory");
        }

        vkFreeMemory(device, memory, NULL);
        uint64_t gbuffer_id = lookup_memory_gbuffer_mapping((uint64_t)(uintptr_t)memory);
        if (gbuffer_id != 0) {
            remove_gbuffer_memory_mapping(gbuffer_id);
        }
        express_vk_unregister_memory(guest_memory, (uint64_t)(uintptr_t)memory);

        remove_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE_MEMORY, guest_memory);

    }
    break;

    case FUNID_vkGetPhysicalDeviceFormatProperties:
    {
        LOGD("Host: vkGetPhysicalDeviceFormatProperties request");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_device = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        VkFormat format = *(VkFormat*)(*ptr); *ptr += sizeof(uint32_t);

        VkPhysicalDevice physicalDevice = (VkPhysicalDevice)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_PHYSICAL_DEVICE, guest_device);

        VkFormatProperties properties;



        VkFormat supported_format = VK_FORMAT_D32_SFLOAT;
        if(g_is_intel_gpu) {
            supported_format = VK_FORMAT_D24_UNORM_S8_UINT;
        }
        if (format == supported_format ||
            format == VK_FORMAT_R8G8B8A8_UNORM ||
            format == VK_FORMAT_B8G8R8A8_UNORM ||
            format == VK_FORMAT_R8G8B8A8_SRGB ||
            format == VK_FORMAT_B8G8R8A8_SRGB) {

            vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &properties);


            if (format == VK_FORMAT_D32_SFLOAT) {
                const VkFormatFeatureFlags allowed =
                    VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                    VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
                    VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                    VK_FORMAT_FEATURE_TRANSFER_DST_BIT;

                properties.linearTilingFeatures  &= allowed;
                properties.optimalTilingFeatures &= allowed;
                properties.bufferFeatures        &= allowed;
            }


        LOGD("Host: format %d is supported with features: 0x%08X", format, properties.optimalTilingFeatures);
    } else {

        properties.linearTilingFeatures  = 0;
        properties.optimalTilingFeatures = 0;
        properties.bufferFeatures        = 0;

        LOGD("Host: format %d is not supported", format);
    }


        write_to_guest_mem(all_para[1].data, &properties, 0, sizeof(VkFormatProperties));
    }
    break;

    case FUNID_vkGetPhysicalDeviceFormatProperties2: {
        LOGD("get call GetPhysicalDeviceFormatProperties2");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        LOGD("get vk param number %d", para_num);

        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_physicalDevice = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        VkPhysicalDevice real_physicalDevice = (VkPhysicalDevice)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_PHYSICAL_DEVICE, guest_physicalDevice);
        VkFormat format = (VkFormat)(*ptr); *ptr += sizeof(uint32_t);

        LOGD("host: physicalDevice = %p, format = %u, pFormatProperties = %p",
            guest_physicalDevice, format, all_para[1].data);

        VkFormatProperties2 props;
        vkGetPhysicalDeviceFormatProperties2(real_physicalDevice, format, &props);
        VkFormatFeatureFlags allowed =
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
            VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
            VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT |
            VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT |
            VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT |
            VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_ATOMIC_BIT |
            VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT |
            VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
            VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT |
            VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
            VK_FORMAT_FEATURE_BLIT_SRC_BIT |
            VK_FORMAT_FEATURE_BLIT_DST_BIT |
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
            VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
            VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
            VK_FORMAT_FEATURE_DISJOINT_BIT |
            VK_FORMAT_FEATURE_MIDPOINT_CHROMA_SAMPLES_BIT |
            VK_FORMAT_FEATURE_COSITED_CHROMA_SAMPLES_BIT |
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_LINEAR_FILTER_BIT |
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_SEPARATE_RECONSTRUCTION_FILTER_BIT |
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_CHROMA_RECONSTRUCTION_EXPLICIT_BIT |
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_CHROMA_RECONSTRUCTION_EXPLICIT_FORCEABLE_BIT;

        VkFormatProperties* p = &props.formatProperties;
        p->linearTilingFeatures &= allowed;
        p->optimalTilingFeatures &= allowed;
        p->bufferFeatures &= allowed;


        write_to_guest_mem(all_para[1].data, &props, 0, sizeof(VkFormatProperties2));

        LOGD("get PhysicalDeviceFormatProperties2: "
              "linearTilingFeatures = %x, optimalTilingFeatures = %x, bufferFeatures = %x",
              p->linearTilingFeatures, p->optimalTilingFeatures, p->bufferFeatures);

        //if (need_free) free(stream);
    }
    break;

    case FUNID_vkGetPhysicalDeviceImageFormatProperties: {
        LOGD("get call vkGetPhysicalDeviceImageFormatProperties");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        LOGD("get vk param number %d", para_num);

        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t* ptr = (uint8_t*)stream;


        uint64_t guest_physicalDevice = *(uint64_t*)ptr;
        ptr += sizeof(uint64_t);

        VkPhysicalDevice real_physicalDevice = (VkPhysicalDevice)(uintptr_t)lookup_mapping(
            EXPRESS_VK_OBJECT_TYPE_PHYSICAL_DEVICE, guest_physicalDevice);

        VkFormat format = *(VkFormat*)ptr;
        ptr += sizeof(uint32_t);

        VkImageType type = *(VkImageType*)ptr;
        ptr += sizeof(uint32_t);

        VkImageTiling tiling = *(VkImageTiling*)ptr;
        ptr += sizeof(uint32_t);

        VkImageUsageFlags usage = *(VkImageUsageFlags*)ptr;
        ptr += sizeof(uint32_t);

        VkImageCreateFlags flags = *(VkImageCreateFlags*)ptr;
        ptr += sizeof(uint32_t);

        VkImageFormatProperties pProps;
        VkResult result = vkGetPhysicalDeviceImageFormatProperties(
            real_physicalDevice, format, type, tiling, usage, flags, &pProps);

        if (result == VK_SUCCESS) {
            write_to_guest_mem(all_para[1].data, &pProps, 0, sizeof(VkImageFormatProperties));
            LOGD("Succeeded to get image format properties: maxExtent(%u, %u, %u), maxMipLevels=%u, maxArrayLayers=%u",
                pProps.maxExtent.width, pProps.maxExtent.height, pProps.maxExtent.depth,
                pProps.maxMipLevels, pProps.maxArrayLayers);
        } else {
            LOGW("Failed to get image format properties: %d", result);
        }

        //if (need_free) free(stream);

    }
    break;

    case FUNID_vkGetPhysicalDeviceImageFormatProperties2: {
        LOGD("get call vkGetPhysicalDeviceImageFormatProperties2");

        const VkPhysicalDeviceImageFormatInfo2* formatInfo = malloc(sizeof(VkPhysicalDeviceImageFormatInfo2));
        if (!formatInfo) {
            LOGE("Failed to allocate memory for VkPhysicalDeviceImageFormatInfo2");
        }
        else{
            int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
            LOGD("get vk param number %d", para_num);

            int need_free = 0;
            char* stream = call_para_to_ptr(all_para[0], &need_free);
            uint8_t** ptr = (uint8_t**)&stream;

            uint64_t guest_physicalDevice = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
            VkPhysicalDevice real_physicalDevice = (VkPhysicalDevice)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_PHYSICAL_DEVICE, guest_physicalDevice);
            decode_from_stream_VkPhysicalDeviceImageFormatInfo2(VK_STRUCTURE_TYPE_MAX_ENUM, formatInfo, ptr);

            LOGD("physicalDevice=%p, pImageFormatProperties=%p",guest_physicalDevice, all_para[1].data);

            VkImageFormatProperties2 pProps;
            VkResult result = vkGetPhysicalDeviceImageFormatProperties2(real_physicalDevice, formatInfo, &pProps);

            if (result == VK_SUCCESS) {
                write_to_guest_mem(all_para[1].data, &pProps, 0, sizeof(VkImageFormatProperties2));
                LOGD("Succeeded to get image format properties: %d %d %d %d", result,
                    pProps.imageFormatProperties.maxExtent.width,
                    pProps.imageFormatProperties.maxExtent.height,
                    pProps.imageFormatProperties.maxExtent.depth);
            } else {
                LOGW("Failed to get image format properties: %d", result);
            }

            //if (need_free) free(stream);
            free(formatInfo);
        }

    }
    break;

    case FUNID_vkInvalidateMappedMemoryRanges:
    {
        LOGD("Host: vkInvalidateMappedMemoryRanges request");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_device = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint32_t rangeCount = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);

        VkDevice device = (VkDevice)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_device);

        VkMappedMemoryRange* ranges = NULL;
        if (rangeCount > 0 && para_num > 1) {
            VkMappedMemoryRange* guest_ranges =
                (VkMappedMemoryRange*)malloc(rangeCount * sizeof(VkMappedMemoryRange));
            if (!guest_ranges ||
                !copy_from_call_para_fast(all_para[1], guest_ranges,
                                          rangeCount * sizeof(VkMappedMemoryRange))) {
                LOGE("Host: vkInvalidateMappedMemoryRanges failed to read range array");
                if (guest_ranges) free(guest_ranges);
                break;
            }

            ranges = (VkMappedMemoryRange*)malloc(rangeCount * sizeof(VkMappedMemoryRange));
            if (!ranges) {
                free(guest_ranges);
                LOGE("Host: vkInvalidateMappedMemoryRanges OOM for host ranges");
                break;
            }
            for (uint32_t i = 0; i < rangeCount; ++i) {
                ranges[i] = guest_ranges[i];
                uint64_t guest_memory = (uint64_t)(uintptr_t)guest_ranges[i].memory;
                ranges[i].memory = (VkDeviceMemory)(uintptr_t)
                    lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE_MEMORY, guest_memory);
            }
            free(guest_ranges);
        }

        VkResult result = vkInvalidateMappedMemoryRanges(device, rangeCount, ranges);

        int result_index = para_num - 1;
        if (result == VK_SUCCESS) {
            int data_para_index = 2;
            bool queued_download = false;
            for (uint32_t i = 0; i < rangeCount; ++i) {
                uint64_t host_memory = (uint64_t)(uintptr_t)ranges[i].memory;
                void* hostPtr = get_memory_map(host_memory);
                ExpressVkRegisteredMemory *reg = express_vk_find_registered_memory(0, host_memory);
                if (i < 2) {
                    LOGD("[SYNC_DBG] invalidate i=%u host_mem=0x%llx host_map=%p offset=%llu size=%llu registered=%d para_index=%d",
                        i,
                        (unsigned long long)host_memory,
                        hostPtr,
                        (unsigned long long)ranges[i].offset,
                        (unsigned long long)ranges[i].size,
                        reg != NULL,
                        data_para_index);
                }
                if (!hostPtr) {
                    continue;
                }

                struct timespec t0_sync, t1_sync;
                clock_gettime(CLOCK_MONOTONIC, &t0_sync);

                if (reg && reg->guest_mem) {
                    uint64_t sync_len = express_vk_registered_range_size(
                        reg,
                        (uint64_t)ranges[i].offset,
                        (uint64_t)ranges[i].size);
                    bool async_started = false;
                    bool prefetched = false;
                    if (sync_len != 0) {
                        prefetched =
                            express_vk_wait_covering_transfer(
                                reg,
                                EXPRESS_VK_TRANSFER_DOWNLOAD,
                                (uint64_t)ranges[i].offset,
                                sync_len,
                                "invalidate_prefetch") != 0;
                    }

                    if (!prefetched && sync_len >= EXPRESS_VK_TRANSFER_DOWNLOAD_THRESHOLD) {
                        express_vk_wait_transfers(reg, EXPRESS_VK_TRANSFER_UPLOAD, "invalidate_before_download");
                        async_started = express_vk_submit_transfer(
                            EXPRESS_VK_TRANSFER_DOWNLOAD,
                            reg,
                            device,
                            host_memory,
                            hostPtr,
                            (uint64_t)ranges[i].offset,
                            sync_len);
                        queued_download = queued_download || async_started;
                    }

                    if (sync_len != 0 && !prefetched && !async_started) {
                        write_to_guest_mem(
                            reg->guest_mem,
                            (char*)hostPtr + ranges[i].offset,
                            ranges[i].offset,
                            sync_len);
                    }

                    clock_gettime(CLOCK_MONOTONIC, &t1_sync);
                    double sync_ms = (t1_sync.tv_sec - t0_sync.tv_sec) * 1000.0 + (t1_sync.tv_nsec - t0_sync.tv_nsec) / 1000000.0;
                    LOGD("[ExpressVkMem] vkInvalidateMappedMemoryRanges %s registered %llu bytes to guest in %.3f ms (i=%u)",
                        prefetched ? "prefetched" : (async_started ? "queued" : "synced"),
                        (unsigned long long)sync_len,
                        sync_ms,
                        i);
                    express_vk_stats_add_invalidate(prefetched, async_started, sync_len);
                    continue;
                }

                if (data_para_index >= result_index || all_para[data_para_index].data_len == 0) {
                    continue;
                }

                write_to_guest_mem(
                    all_para[data_para_index].data,
                    (char*)hostPtr + ranges[i].offset,
                    0,
                    all_para[data_para_index].data_len);

                clock_gettime(CLOCK_MONOTONIC, &t1_sync);
                double sync_ms = (t1_sync.tv_sec - t0_sync.tv_sec) * 1000.0 + (t1_sync.tv_nsec - t0_sync.tv_nsec) / 1000000.0;
                LOGD("[SYNC_TIME] vkInvalidateMappedMemoryRanges synced %zu bytes to guest in %.3f ms (i=%u)",
                    (size_t)all_para[data_para_index].data_len,
                    sync_ms,
                    i);
                data_para_index++;
            }

            if (queued_download) {
                express_vk_wait_transfers(NULL, EXPRESS_VK_TRANSFER_DOWNLOAD, "invalidate_return");
            }
        }

        if (result_index >= 0) {
            write_to_guest_mem(all_para[result_index].data, &result, 0, sizeof(VkResult));
        } else {
            LOGE("Host: invalidate result slot missing para_num=%d rangeCount=%u", para_num, rangeCount);
        }

        if (ranges) free(ranges);

    }
    break;

    case FUNID_vkBindBufferMemory2:
    {
        LOGD("Host: vkBindBufferMemory2 request");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint32_t bindInfoCount = *(uint32_t*)(*ptr);
        *ptr += sizeof(uint32_t);

        VkBindBufferMemoryInfo* pBindInfos = malloc(bindInfoCount * sizeof(VkBindBufferMemoryInfo));
        if (bindInfoCount > 0 && pBindInfos == NULL) {
            LOGE("Host: vkBindBufferMemory2 OOM bindInfoCount=%u",
                 bindInfoCount);
            break;
        }
        for (uint32_t i = 0; i < bindInfoCount; ++i) {
            decode_from_stream_VkBindBufferMemoryInfo(VK_STRUCTURE_TYPE_MAX_ENUM, &pBindInfos[i], ptr);
            if (pBindInfos[i].buffer == VK_NULL_HANDLE ||
                pBindInfos[i].memory == VK_NULL_HANDLE) {
                LOGE("Host: vkBindBufferMemory2 decoded null index=%u buffer=%p memory=%p",
                     i,
                     (void*)pBindInfos[i].buffer,
                     (void*)pBindInfos[i].memory);
            }
        }

        uint64_t guest_device = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        VkDevice device = (VkDevice)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_device);

        bool invalid_mapping = (device == VK_NULL_HANDLE);
        if (invalid_mapping) {
            LOGE("Host: vkBindBufferMemory2 device mapping miss guest_device=0x%llx",
                 (unsigned long long)guest_device);
        }
        for (uint32_t i = 0; i < bindInfoCount; ++i) {
            if (pBindInfos[i].buffer == VK_NULL_HANDLE ||
                pBindInfos[i].memory == VK_NULL_HANDLE) {
                invalid_mapping = true;
            }
        }

        VkResult result = VK_ERROR_INITIALIZATION_FAILED;
        if (!invalid_mapping) {
            result = vkBindBufferMemory2(device, bindInfoCount, pBindInfos);
        }
        if (result != VK_SUCCESS) {
            LOGE("vkBindBufferMemory2 failed: %d", result);
        }
        free(pBindInfos);
    }
    break;

    case FUNID_vkCmdBeginQuery:
    {
        LOGD("Host: vkCmdBeginQuery request");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint64_t guest_pool = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint32_t query = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);
        uint32_t flags = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);

        VkCommandBuffer commandBuffer = (VkCommandBuffer)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd);
        VkQueryPool queryPool = (VkQueryPool)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_QUERY_POOL, guest_pool);

        vkCmdBeginQuery(commandBuffer, queryPool, query, flags);
    }
    break;

    case FUNID_vkCmdCopyQueryPoolResults:
    {
        LOGD("Host: vkCmdCopyQueryPoolResults request");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint64_t guest_pool = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint32_t firstQuery = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);
        uint32_t queryCount = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);
        uint64_t guest_buffer = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        VkDeviceSize dstOffset = *(VkDeviceSize*)(*ptr); *ptr += sizeof(VkDeviceSize);
        VkDeviceSize stride = *(VkDeviceSize*)(*ptr); *ptr += sizeof(VkDeviceSize);
        uint32_t flags = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);

        VkCommandBuffer commandBuffer = (VkCommandBuffer)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd);
        VkQueryPool queryPool = (VkQueryPool)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_QUERY_POOL, guest_pool);
        VkBuffer dstBuffer = (VkBuffer)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_BUFFER, guest_buffer);

        vkCmdCopyQueryPoolResults(commandBuffer, queryPool, firstQuery, queryCount, dstBuffer, dstOffset, stride, flags);
    }
    break;

    case FUNID_vkCmdDispatchIndirect:
    {
        LOGD("Host: vkCmdDispatchIndirect request");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint64_t guest_buffer = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        VkDeviceSize offset = *(VkDeviceSize*)(*ptr); *ptr += sizeof(VkDeviceSize);

        VkCommandBuffer commandBuffer = (VkCommandBuffer)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd);
        VkBuffer buffer = (VkBuffer)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_BUFFER, guest_buffer);
        LOGD("Host: vkCmdDispatchIndirect commandBuffer=%p buffer=%p offset=%llu",
            (void*)commandBuffer, (void*)buffer, offset);

        vkCmdDispatchIndirect(commandBuffer, buffer, offset);
    }
    break;

    case FUNID_vkCmdDrawIndexedIndirect:
    {
        LOGD("Host: vkCmdDrawIndexedIndirect request");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint64_t guest_buffer = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        VkDeviceSize offset = *(VkDeviceSize*)(*ptr); *ptr += sizeof(VkDeviceSize);
        uint32_t drawCount = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);
        uint32_t stride = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);

        VkCommandBuffer commandBuffer = (VkCommandBuffer)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd);
        VkBuffer buffer = (VkBuffer)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_BUFFER, guest_buffer);

        vkCmdDrawIndexedIndirect(commandBuffer, buffer, offset, drawCount, stride);
    }
    break;

    case FUNID_vkCmdDrawIndirect:
    {
        LOGD("Host: vkCmdDrawIndirect request");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint64_t guest_buffer = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        VkDeviceSize offset = *(VkDeviceSize*)(*ptr); *ptr += sizeof(VkDeviceSize);
        uint32_t drawCount = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);
        uint32_t stride = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);

        VkCommandBuffer commandBuffer = (VkCommandBuffer)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd);
        VkBuffer buffer = (VkBuffer)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_BUFFER, guest_buffer);

        vkCmdDrawIndirect(commandBuffer, buffer, offset, drawCount, stride);
    }
    break;

    case FUNID_vkCmdEndQuery:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint64_t guest_pool = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint32_t query = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);

        VkCommandBuffer commandBuffer = (VkCommandBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd);
        VkQueryPool queryPool = (VkQueryPool)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_QUERY_POOL, guest_pool);
        LOGD("Host: vkCmdEndQuery commandBuffer=%p queryPool=%p query=%u",
            (void*)commandBuffer, (void*)queryPool, query);

        vkCmdEndQuery(commandBuffer, queryPool, query);
    }
    break;

    case FUNID_vkCmdFillBuffer:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint64_t guest_buffer = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        VkDeviceSize dstOffset = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        VkDeviceSize size = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint32_t data = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);

        VkCommandBuffer commandBuffer = (VkCommandBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd);
        VkBuffer dstBuffer = (VkBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_BUFFER, guest_buffer);

        vkCmdFillBuffer(commandBuffer, dstBuffer, dstOffset, size, data);
        LOGD("Host: vkCmdFillBuffer commandBuffer=%p dstBuffer=%p dstOffset=%llu size=%llu data=%u",
            (void*)commandBuffer, (void*)dstBuffer, dstOffset, size, data);
    }
    break;

    case FUNID_vkCmdResetEvent:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint64_t guest_event = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        VkPipelineStageFlags stageMask = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);

        VkCommandBuffer commandBuffer = (VkCommandBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd);
        VkEvent event = (VkEvent)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_EVENT, guest_event);

        vkCmdResetEvent(commandBuffer, event, stageMask);
        LOGD("Host: vkCmdResetEvent commandBuffer=%p event=%p stageMask=%u",
            (void*)commandBuffer, (void*)event, stageMask);
    }
    break;

    case FUNID_vkCmdSetEvent:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint64_t guest_event = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        VkPipelineStageFlags stageMask = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);

        VkCommandBuffer commandBuffer = (VkCommandBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd);
        VkEvent event = (VkEvent)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_EVENT, guest_event);

        vkCmdSetEvent(commandBuffer, event, stageMask);
        LOGD("Host: vkCmdSetEvent commandBuffer=%p event=%p stageMask=%u",
            (void*)commandBuffer, (void*)event, stageMask);
    }
    break;

    case FUNID_vkCmdUpdateBuffer:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint64_t guest_buffer = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        VkDeviceSize dstOffset = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        VkDeviceSize dataSize = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);

        VkCommandBuffer commandBuffer = (VkCommandBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd);
        VkBuffer dstBuffer = (VkBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_BUFFER, guest_buffer);

        void* pData = NULL;
        if (dataSize > 0) {
            pData = malloc((size_t)dataSize);
            if (!pData ||
                !copy_from_call_para_fast(all_para[1], pData, (size_t)dataSize)) {
                LOGE("Host: vkCmdUpdateBuffer failed to get source data");
                if (pData) free(pData);
                break;
            }
        }

        vkCmdUpdateBuffer(commandBuffer, dstBuffer, dstOffset, dataSize, pData);

        if (pData) free(pData);
        LOGD("Host: vkCmdUpdateBuffer commandBuffer=%p dstBuffer=%p dstOffset=%llu dataSize=%llu",
            (void*)commandBuffer, (void*)dstBuffer, dstOffset, dataSize);
    }
    break;

    case FUNID_vkCmdWaitEvents:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint32_t eventCount = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);

        VkEvent* pEvents = NULL;
        if (eventCount > 0) {
            pEvents = (VkEvent*)malloc(eventCount * sizeof(VkEvent));
            for (uint32_t i = 0; i < eventCount; ++i) {
                uint64_t guest_event = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
                pEvents[i] = (VkEvent)(uintptr_t)
                    lookup_mapping(EXPRESS_VK_OBJECT_TYPE_EVENT, guest_event);
            }
        }

        VkPipelineStageFlags srcStageMask = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);
        VkPipelineStageFlags dstStageMask = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);
        uint32_t memoryBarrierCount = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);
        uint32_t bufferMemoryBarrierCount = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);
        uint32_t imageMemoryBarrierCount = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);

        VkMemoryBarrier* pMemoryBarriers = NULL;
        if (memoryBarrierCount > 0) {
            pMemoryBarriers = (VkMemoryBarrier*)malloc(memoryBarrierCount * sizeof(VkMemoryBarrier));
            for (uint32_t i = 0; i < memoryBarrierCount; ++i) {
                decode_from_stream_VkMemoryBarrier(VK_STRUCTURE_TYPE_MAX_ENUM, &pMemoryBarriers[i], ptr);
            }
        }

        VkBufferMemoryBarrier* pBufferMemoryBarriers = NULL;
        if (bufferMemoryBarrierCount > 0) {
            pBufferMemoryBarriers = (VkBufferMemoryBarrier*)malloc(bufferMemoryBarrierCount * sizeof(VkBufferMemoryBarrier));
            for (uint32_t i = 0; i < bufferMemoryBarrierCount; ++i) {
                decode_from_stream_VkBufferMemoryBarrier(VK_STRUCTURE_TYPE_MAX_ENUM, &pBufferMemoryBarriers[i], ptr);
            }
        }

        VkImageMemoryBarrier* pImageMemoryBarriers = NULL;
        if (imageMemoryBarrierCount > 0) {
            pImageMemoryBarriers = (VkImageMemoryBarrier*)malloc(imageMemoryBarrierCount * sizeof(VkImageMemoryBarrier));
            for (uint32_t i = 0; i < imageMemoryBarrierCount; ++i) {
                decode_from_stream_VkImageMemoryBarrier(VK_STRUCTURE_TYPE_MAX_ENUM, &pImageMemoryBarriers[i], ptr);
            }
        }

        VkCommandBuffer commandBuffer = (VkCommandBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd);

        vkCmdWaitEvents(commandBuffer, eventCount, pEvents, srcStageMask, dstStageMask,
                    memoryBarrierCount, pMemoryBarriers,
                    bufferMemoryBarrierCount, pBufferMemoryBarriers,
                    imageMemoryBarrierCount, pImageMemoryBarriers);
        LOGD("Host: vkCmdWaitEvents commandBuffer=%p eventCount=%u srcStageMask=%u dstStageMask=%u",
            (void*)commandBuffer, eventCount, srcStageMask, dstStageMask);

        if (pEvents) free(pEvents);
        if (pMemoryBarriers) free(pMemoryBarriers);
        if (pBufferMemoryBarriers) free(pBufferMemoryBarriers);
        if (pImageMemoryBarriers) free(pImageMemoryBarriers);
    }
    break;

    case FUNID_vkCmdWriteTimestamp:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        VkPipelineStageFlagBits pipelineStage = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);
        uint64_t guest_pool = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint32_t query = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);

        VkCommandBuffer commandBuffer = (VkCommandBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd);
        VkQueryPool queryPool = (VkQueryPool)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_QUERY_POOL, guest_pool);

        vkCmdWriteTimestamp(commandBuffer, pipelineStage, queryPool, query);
        LOGD("Host: vkCmdWriteTimestamp commandBuffer=%p pipelineStage=%u queryPool=%p query=%u",
            (void*)commandBuffer, pipelineStage, (void*)queryPool, query);
    }
    break;

    case FUNID_vkCmdBlitImage:
    {
        LOGD("Host: vkCmdBlitImage request");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);

        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmdBuf = *(uint64_t*)(*ptr); *ptr += 8;
        uint64_t guest_srcImg = *(uint64_t*)(*ptr); *ptr += 8;
        VkImageLayout srcLayout = *(VkImageLayout*)(*ptr); *ptr += 4;
        uint64_t guest_dstImg = *(uint64_t*)(*ptr); *ptr += 8;
        VkImageLayout dstLayout = *(VkImageLayout*)(*ptr); *ptr += 4;
        uint32_t regionCount = *(uint32_t*)(*ptr); *ptr += 4;
        VkFilter filter = *(VkFilter*)(*ptr); *ptr += 4;

        VkCommandBuffer cmdBuf = (VkCommandBuffer)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmdBuf);
        VkImage srcImg = (VkImage)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_IMAGE, guest_srcImg);
        VkImage dstImg = (VkImage)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_IMAGE, guest_dstImg);

        VkImageBlit* regions = NULL;
        if (regionCount > 0) {
            regions = (VkImageBlit*)(*ptr);
        }

        vkCmdBlitImage(cmdBuf, srcImg, srcLayout, dstImg, dstLayout, regionCount, regions, filter);
    }
    break;

    case FUNID_vkCmdCopyBufferToImage:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_commandBuffer = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint64_t guest_srcBuffer = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint64_t guest_dstImage = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        VkImageLayout dstImageLayout = *(VkImageLayout*)(*ptr); *ptr += sizeof(uint32_t);
        uint32_t regionCount = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);

        VkCommandBuffer commandBuffer = (VkCommandBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_commandBuffer);
        VkBuffer srcBuffer = (VkBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_BUFFER, guest_srcBuffer);
        VkImage dstImage = (VkImage)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_IMAGE, guest_dstImage);

        VkBufferImageCopy* pRegions = NULL;
        if (regionCount > 0) {
            pRegions = (VkBufferImageCopy*)malloc(regionCount * sizeof(VkBufferImageCopy));
            for (uint32_t i = 0; i < regionCount; ++i) {
                decode_from_stream_VkBufferImageCopy(VK_STRUCTURE_TYPE_MAX_ENUM, &pRegions[i], ptr);
            }
        }

        vkCmdCopyBufferToImage(commandBuffer, srcBuffer, dstImage, dstImageLayout, regionCount, pRegions);

        if (pRegions) free(pRegions);
        LOGD("Host: vkCmdCopyBufferToImage completed");
    }
    break;

    case FUNID_vkCmdPushConstants:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_commandBuffer = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint64_t guest_layout = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        VkShaderStageFlags stageFlags = *(VkShaderStageFlags*)(*ptr); *ptr += sizeof(uint32_t);
        uint32_t offset = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);
        uint32_t size = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);

        VkCommandBuffer commandBuffer = (VkCommandBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_commandBuffer);
        VkPipelineLayout layout = (VkPipelineLayout)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_PIPELINE_LAYOUT, guest_layout);

        void* pValues = malloc(size);
        memcpy(pValues, *ptr, size);
        *ptr += size;

        vkCmdPushConstants(commandBuffer, layout, stageFlags, offset, size, pValues);

        free(pValues);
        LOGD("Host: vkCmdPushConstants completed");
    }
    break;

    case FUNID_vkCreateBufferView:
    {
        LOGD("Host: vkCreateBufferView request");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_device = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        VkDevice device = (VkDevice)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_device);

        VkBufferViewCreateInfo createInfo;
        decode_from_stream_VkBufferViewCreateInfo(VK_STRUCTURE_TYPE_MAX_ENUM, &createInfo, ptr);

        VkAllocationCallbacks* guest_allocator = (VkAllocationCallbacks*)*(uint64_t*)(*ptr);
        *ptr += 8;
        VkAllocationCallbacks allocator_local;
        const VkAllocationCallbacks* pAllocator = NULL;
        if (guest_allocator) {
            decode_from_stream_VkAllocationCallbacks(VK_STRUCTURE_TYPE_MAX_ENUM, &allocator_local, ptr);
            pAllocator = &allocator_local;
        }

        uint64_t guest_view = *(uint64_t*)(*ptr);

        VkBufferView bufferView;
        VkResult result = vkCreateBufferView(device, &createInfo, pAllocator, &bufferView);

        if (result == VK_SUCCESS) {
            insert_mapping(EXPRESS_VK_OBJECT_TYPE_BUFFER_VIEW, guest_view, (uint64_t)(uintptr_t)bufferView);
            LOGD("Host: vkCreateBufferView success, guest=%lld host=%lld", guest_view, (uint64_t)(uintptr_t)bufferView);
        } else {
            LOGE("Host: vkCreateBufferView failed with error %d", result);
        }
    }
    break;

    case FUNID_vkCreateEvent:
    {
        LOGD("Host: vkCreateEvent request");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_device = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        VkDevice device = (VkDevice)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_device);

        VkEventCreateInfo createInfo;
        decode_from_stream_VkEventCreateInfo(VK_STRUCTURE_TYPE_MAX_ENUM, &createInfo, ptr);

        VkAllocationCallbacks* guest_allocator = (VkAllocationCallbacks*)*(uint64_t*)(*ptr);
        *ptr += 8;
        VkAllocationCallbacks allocator_local;
        const VkAllocationCallbacks* pAllocator = NULL;
        if (guest_allocator) {
            decode_from_stream_VkAllocationCallbacks(VK_STRUCTURE_TYPE_MAX_ENUM, &allocator_local, ptr);
            pAllocator = &allocator_local;
        }

        uint64_t guest_event = *(uint64_t*)(*ptr);

        VkEvent event;
        VkResult result = vkCreateEvent(device, &createInfo, pAllocator, &event);

        if (result == VK_SUCCESS) {
            insert_mapping(EXPRESS_VK_OBJECT_TYPE_EVENT, guest_event, (uint64_t)(uintptr_t)event);
            LOGD("Host: vkCreateEvent success, guest=%lld host=%lld", guest_event, (uint64_t)(uintptr_t)event);
        } else {
            LOGE("Host: vkCreateEvent failed with error %d", result);
        }
    }
    break;

    case FUNID_vkGetEventStatus:
    {
        LOGD("Host: vkGetEventStatus request");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_device = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint64_t guest_event = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);

        VkDevice device = (VkDevice)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_device);
        VkEvent event = (VkEvent)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_EVENT, guest_event);

        VkResult result = vkGetEventStatus(device, event);

        write_to_guest_mem(all_para[1].data, &result, 0, sizeof(VkResult));
        LOGD("Host: vkGetEventStatus result=%d", result);
    }
    break;

    case FUNID_vkGetFenceStatus:
    {
        LOGD("Host: vkGetFenceStatus request");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_device = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint64_t guest_fence = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);

        VkDevice device = (VkDevice)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_device);
        VkFence fence = (VkFence)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_FENCE, guest_fence);

        VkResult result = vkGetFenceStatus(device, fence);

        write_to_guest_mem(all_para[1].data, &result, 0, sizeof(VkResult));
        LOGD("Host: vkGetFenceStatus result=%d", result);
    }
    break;

    case FUNID_vkGetPipelineCacheData:
    {
        LOGD("Host: vkGetPipelineCacheData request");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_device = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint64_t guest_cache = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);

        VkDevice device = (VkDevice)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_device);
        VkPipelineCache pipelineCache = guest_cache ?
            (VkPipelineCache)(uintptr_t)lookup_mapping(
                EXPRESS_VK_OBJECT_TYPE_PIPELINE_CACHE,
                guest_cache) :
            VK_NULL_HANDLE;

        size_t dataSize;
        if (!copy_from_call_para_fast(all_para[1], &dataSize, sizeof(size_t))) {
            LOGE("Host: vkGetPipelineCacheData failed to read dataSize");
            break;
        }

        if(dataSize == 0) {
            VkResult result = vkGetPipelineCacheData(device, pipelineCache, &dataSize, NULL);
            if(result == VK_SUCCESS) {
                write_to_guest_mem(all_para[1].data, &dataSize, 0, sizeof(size_t));
                LOGD("Host: vkGetPipelineCacheData dataSize %d", (int)dataSize);
            } else {
                LOGE("Host: vkGetPipelineCacheData failed with error %d", result);
            }
        } else {
            void* pData = NULL;
            pData = malloc(dataSize);
            VkResult result = vkGetPipelineCacheData(device, pipelineCache, &dataSize, pData);

            if (result == VK_SUCCESS) {
                LOGD("Host: vkGetPipelineCacheData success, dataSize=%zu", dataSize);
                write_to_guest_mem(all_para[2].data, pData, 0, dataSize);
                free(pData);
            } else {
                LOGE("Host: vkGetPipelineCacheData failed with error %d", result);
            }
        }
    }
    break;

    case FUNID_vkGetDeviceMemoryCommitment:
    {
        LOGD("Host: vkGetDeviceMemoryCommitment request");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_device = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint64_t guest_memory = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);

        VkDevice device = (VkDevice)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_device);
        VkDeviceMemory memory = (VkDeviceMemory)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE_MEMORY, guest_memory);

        VkDeviceSize committedSize;
        vkGetDeviceMemoryCommitment(device, memory, &committedSize);

        write_to_guest_mem(all_para[1].data, &committedSize, 0, sizeof(VkDeviceSize));
        LOGD("Host: vkGetDeviceMemoryCommitment committedSize=%llu", committedSize);
    }
    break;

    case FUNID_vkEnumerateInstanceExtensionProperties:
    {
        LOGD("Host: vkEnumerateInstanceExtensionProperties request");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint32_t has_layer = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);

        const char* layer_name = NULL;
        if (has_layer) {
            layer_name = (const char*)(*ptr);
            *ptr += strlen(layer_name) + 1;
        }

        uint32_t count = 0;
        if (!copy_from_call_para_fast(all_para[1], &count, sizeof(uint32_t))) {
            LOGE("Host: vkEnumerateInstanceExtensionProperties failed to read count");
            break;
        }

        VkResult result;


        if (!layer_name) {
            result = ensure_instance_extensions_cached();
            if (result != VK_SUCCESS) {
                break;
            }

            if (count == 0) {
                write_to_guest_mem(all_para[1].data, &g_cached_instance_extension_count, 0, sizeof(uint32_t));
                LOGD("Host: Returning cached extension count=%u", g_cached_instance_extension_count);
            } else {
                uint32_t copy_count = (count < g_cached_instance_extension_count) ? count : g_cached_instance_extension_count;
                // write_to_guest_mem(all_para[1].data, &copy_count, 0, sizeof(uint32_t));
                write_to_guest_mem(all_para[2].data, g_cached_instance_extensions, 0, copy_count * sizeof(VkExtensionProperties));
                result = (copy_count < g_cached_instance_extension_count) ? VK_INCOMPLETE : VK_SUCCESS;
                LOGD("Host: Returning %u cached extensions", copy_count);
            }
        } else {

            if (count == 0) {
                result = vkEnumerateInstanceExtensionProperties(layer_name, &count, NULL);
                if(result != VK_SUCCESS) {
                    LOGE("vkEnumerateInstanceExtensionProperties failed with error %d", result);
                } else {
                    LOGD("vkEnumerateInstanceExtensionProperties count=%u", count);
                    write_to_guest_mem(all_para[1].data, &count, 0, sizeof(uint32_t));
                }
            } else {
                VkExtensionProperties* properties = (VkExtensionProperties*)malloc(count * sizeof(VkExtensionProperties));
                if (!properties) {
                    result = VK_ERROR_OUT_OF_HOST_MEMORY;
                } else {
                    result = vkEnumerateInstanceExtensionProperties(layer_name, &count, properties);
                    if (result == VK_SUCCESS || result == VK_INCOMPLETE) {
                        write_to_guest_mem(all_para[1].data, &count, 0, sizeof(uint32_t));
                        write_to_guest_mem(all_para[2].data, properties, 0, count * sizeof(VkExtensionProperties));
                    } else {
                        LOGE("vkEnumerateInstanceExtensionProperties failed with error %d", result);
                    }
                    free(properties);
                }
            }
        }

        LOGD("Host: vkEnumerateInstanceExtensionProperties result=%d", result);
    }
    break;

    case FUNID_vkEnumerateDeviceExtensionProperties:
    {
        LOGD("Host: vkEnumerateDeviceExtensionProperties request");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_dev = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint32_t has_layer = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);

        const char* layer_name = NULL;
        if (has_layer) {
            layer_name = (const char*)(*ptr);
            *ptr += strlen(layer_name) + 1;
        }

        VkPhysicalDevice device = (VkPhysicalDevice)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_PHYSICAL_DEVICE, guest_dev);

        uint32_t count = 0;
        if (!copy_from_call_para_fast(all_para[1], &count, sizeof(uint32_t))) {
            LOGE("Host: vkEnumerateDeviceExtensionProperties failed to read count");
            break;
        }

        VkResult result;
        uint32_t raw_count = 0;
        result = vkEnumerateDeviceExtensionProperties(device, layer_name, &raw_count, NULL);
        if (result != VK_SUCCESS) {
            LOGE("vkEnumerateDeviceExtensionProperties(count) failed with error %d", result);
            break;
        }

        VkExtensionProperties* raw_props = NULL;
        if (raw_count > 0) {
            raw_props = (VkExtensionProperties*)malloc(raw_count * sizeof(VkExtensionProperties));
            if (!raw_props) {
                result = VK_ERROR_OUT_OF_HOST_MEMORY;
                LOGE("vkEnumerateDeviceExtensionProperties malloc failed");
                break;
            }

            uint32_t tmp_count = raw_count;
            result = vkEnumerateDeviceExtensionProperties(device, layer_name, &tmp_count, raw_props);
            if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
                LOGE("vkEnumerateDeviceExtensionProperties(list) failed with error %d", result);
                free(raw_props);
                break;
            }
            raw_count = tmp_count;
        }

        VkExtensionProperties* filtered_props = NULL;
        uint32_t filtered_count = 0;
        if (raw_count > 0) {
            filtered_props = (VkExtensionProperties*)malloc(raw_count * sizeof(VkExtensionProperties));
            if (!filtered_props) {
                free(raw_props);
                result = VK_ERROR_OUT_OF_HOST_MEMORY;
                LOGE("vkEnumerateDeviceExtensionProperties filtered malloc failed");
                break;
            }

            const char* log_phase = (count == 0) ? "count" : "list";
            for (uint32_t i = 0; i < raw_count; ++i) {
                if (should_hide_device_extension_from_guest(raw_props[i].extensionName)) {
                    LOGD("hide device extension in enumerate(%s): %s",
                        log_phase,
                        raw_props[i].extensionName);
                    continue;
                }
                filtered_props[filtered_count++] = raw_props[i];
            }
        }

        if (count == 0) {
            write_to_guest_mem(all_para[1].data, &filtered_count, 0, sizeof(uint32_t));
            result = VK_SUCCESS;
        } else {
            uint32_t requested_count = count;
            uint32_t returned_count = (requested_count < filtered_count) ? requested_count : filtered_count;

            if (returned_count > 0) {
                write_to_guest_mem(all_para[2].data,
                                   filtered_props,
                                   0,
                                   returned_count * sizeof(VkExtensionProperties));
            }
            write_to_guest_mem(all_para[1].data, &returned_count, 0, sizeof(uint32_t));
            result = (requested_count < filtered_count) ? VK_INCOMPLETE : VK_SUCCESS;
        }

        LOGD("filtered count=%u raw=%u", filtered_count, raw_count);
        LOGD("Host: vkEnumerateDeviceExtensionProperties result=%d count=%u", result, filtered_count);

        if (raw_props) {
            free(raw_props);
        }
        if (filtered_props) {
            free(filtered_props);
        }
    }
    break;

    case FUNID_vkEnumerateInstanceLayerProperties:
    {
        LOGD("Host: vkEnumerateInstanceLayerProperties request");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);

        uint32_t count = 0;
        if (!copy_from_call_para_fast(all_para[0], &count, sizeof(uint32_t))) {
            LOGE("Host: vkEnumerateInstanceLayerProperties failed to read count");
            break;
        }

        VkResult result;
        if (count == 0) {
            result = vkEnumerateInstanceLayerProperties(&count, NULL);
            if(result != VK_SUCCESS) {
                LOGE("vkEnumerateInstanceLayerProperties failed with error %d", result);
            } else {
                write_to_guest_mem(all_para[0].data, &count, 0, sizeof(uint32_t));
                LOGD("vkEnumerateInstanceLayerProperties count=%u", count);
            }
        } else {
            VkLayerProperties* properties = (VkLayerProperties*)malloc(count * sizeof(VkLayerProperties));
            if (!properties) {
                result = VK_ERROR_OUT_OF_HOST_MEMORY;
            } else {
                result = vkEnumerateInstanceLayerProperties(&count, properties);
                if (result == VK_SUCCESS) {
                    write_to_guest_mem(all_para[1].data, properties, 0, count * sizeof(VkLayerProperties));
                    LOGD("vkEnumerateInstanceLayerProperties count=%u", count);
                } else {
                    LOGE("vkEnumerateInstanceLayerProperties failed with error %d", result);
                }
                free(properties);
            }
        }
    }
    break;

    case FUNID_vkEnumerateDeviceLayerProperties:
    {
        LOGD("Host: vkEnumerateDeviceLayerProperties request");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);

        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_dev = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);

        VkPhysicalDevice device = (VkPhysicalDevice)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_PHYSICAL_DEVICE, guest_dev);

        uint32_t count = 0;
        if (!copy_from_call_para_fast(all_para[1], &count, sizeof(uint32_t))) {
            LOGE("Host: vkEnumerateDeviceLayerProperties failed to read count");
            break;
        }

        VkResult result;
        if (count == 0) {
            result = vkEnumerateDeviceLayerProperties(device, &count, NULL);
            if (result != VK_SUCCESS) {
                LOGE("vkEnumerateDeviceLayerProperties failed with error %d", result);
            } else {
                write_to_guest_mem(all_para[1].data, &count, 0, sizeof(uint32_t));
                LOGD("vkEnumerateDeviceLayerProperties count=%u", count);
            }
        } else {
            VkLayerProperties* properties = (VkLayerProperties*)malloc(count * sizeof(VkLayerProperties));
            if (!properties) {
                result = VK_ERROR_OUT_OF_HOST_MEMORY;
            } else {
                result = vkEnumerateDeviceLayerProperties(device, &count, properties);
                if (result == VK_SUCCESS) {
                    write_to_guest_mem(all_para[2].data, properties, 0, count * sizeof(VkLayerProperties));
                    LOGD("vkEnumerateDeviceLayerProperties count=%u", count);
                } else {
                    LOGE("vkEnumerateDeviceLayerProperties failed with error %d", result);
                }
                free(properties);
            }
        }
    }
    break;

    case FUNID_vkEnumerateInstanceVersion:
    {
        LOGD("Host: vkEnumerateInstanceVersion request");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);

        uint32_t version;
        VkResult result = vkEnumerateInstanceVersion(&version);
        if (result != VK_SUCCESS) {
            LOGE("vkEnumerateInstanceVersion failed with error %d", result);
        } else {
            write_to_guest_mem(all_para[0].data, &version, 0, sizeof(uint32_t));
            LOGD("vkEnumerateInstanceVersion success, version=%u", version);
        }
    }
    break;

    case FUNID_vkQueueBindSparse:
    {
        LOGD("Host: vkQueueBindSparse request");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        // Decode queue
        uint64_t guest_queue = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        VkQueue queue = (VkQueue)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_QUEUE, guest_queue);

        // Decode bindInfoCount
        uint32_t bindInfoCount = *(uint32_t*)(*ptr);
        *ptr += sizeof(uint32_t);

        // Decode bind infos
        VkBindSparseInfo* pBindInfo = NULL;
        if (bindInfoCount > 0) {
            pBindInfo = (VkBindSparseInfo*)malloc(bindInfoCount * sizeof(VkBindSparseInfo));
            for (uint32_t i = 0; i < bindInfoCount; ++i) {
                decode_from_stream_VkBindSparseInfo(VK_STRUCTURE_TYPE_MAX_ENUM, &pBindInfo[i], ptr);
            }
        }

        // Decode fence
        uint64_t guest_fence = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        VkFence fence = VK_NULL_HANDLE;
        if (guest_fence != 0) {
            fence = (VkFence)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_FENCE, guest_fence);
        }

        g_mutex_lock(&g_express_vk_transaction_lock);
        VkResult result = vkQueueBindSparse(queue, bindInfoCount, pBindInfo, fence);
        g_mutex_unlock(&g_express_vk_transaction_lock);
        if (result != VK_SUCCESS) {
            LOGE("Host: vkQueueBindSparse failed with error %d", result);
        } else {
            LOGD("Host: vkQueueBindSparse success, queue=%p bindInfoCount=%u fence=%p",
                (void*)queue, bindInfoCount, (void*)fence);
        }

        if (pBindInfo) free(pBindInfo);
        //if (need_free) free(stream);
    }
    break;

    case FUNID_vkQueueWaitIdle:
    {
        LOGD("Host: vkQueueWaitIdle request");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_queue = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        VkQueue queue = (VkQueue)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_QUEUE, guest_queue);

        struct timespec t0_qwide, t1_qwide;
        clock_gettime(CLOCK_MONOTONIC, &t0_qwide);
        VkResult result = vkQueueWaitIdle(queue);
        clock_gettime(CLOCK_MONOTONIC, &t1_qwide);
        double qwide_ms = (t1_qwide.tv_sec - t0_qwide.tv_sec) * 1000.0 + (t1_qwide.tv_nsec - t0_qwide.tv_nsec) / 1000000.0;
        LOGD("[GPU_TIME] vkQueueWaitIdle cost: %.3f ms", qwide_ms);
        write_to_guest_mem(all_para[1].data, &result, 0, sizeof(VkResult));
        if (result != VK_SUCCESS) {
            LOGE("Host: vkQueueWaitIdle failed with error %d", result);
        } else {
            LOGD("Host: vkQueueWaitIdle success, queue=%p", (void*)queue);
        }

        //if (need_free) free(stream);
    }
    break;

    case FUNID_vkTrimCommandPool:
    {
        LOGD("Host: vkTrimCommandPool request");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_device = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        uint64_t guest_pool = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        uint32_t flags = *(uint32_t*)(*ptr);
        *ptr += sizeof(uint32_t);

        VkDevice device = (VkDevice)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_device);
        VkCommandPool commandPool = (VkCommandPool)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_POOL, guest_pool);

        vkTrimCommandPool(device, commandPool, flags);

        //if (need_free) free(stream);
    }
    break;

    case FUNID_vkGetPhysicalDeviceFeatures:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);

        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;
        uint64_t guest_device = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);

        VkPhysicalDevice device = (VkPhysicalDevice)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_PHYSICAL_DEVICE, guest_device);

        VkPhysicalDeviceFeatures features;
        vkGetPhysicalDeviceFeatures(device, &features);

        write_to_guest_mem(all_para[1].data, &features, 0, sizeof(VkPhysicalDeviceFeatures));
        LOGD("Host: vkGetPhysicalDeviceFeatures device=%p", (void*)device);
    }
    break;

    case FUNID_vkGetQueryPoolResults:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);

        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;
        uint64_t guest_device = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint64_t guest_queryPool = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint32_t firstQuery = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);
        uint32_t queryCount = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);
        size_t dataSize = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        VkDeviceSize stride = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        VkQueryResultFlags flags = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);

        VkDevice device = (VkDevice)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_device);
        VkQueryPool queryPool = (VkQueryPool)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_QUERY_POOL, guest_queryPool);

        void* data = malloc(dataSize);
        VkResult result = vkGetQueryPoolResults(device, queryPool, firstQuery, queryCount,
                                            dataSize, data, stride, flags);
        write_to_guest_mem(all_para[2].data, &result, 0, sizeof(VkResult));

        if (result != VK_SUCCESS) {
            LOGE("Host: vkGetQueryPoolResults failed with error %d", result);
        } else {
            write_to_guest_mem(all_para[1].data, data, 0, dataSize);
            LOGD("Host: vkGetQueryPoolResults success, firstQuery=%u queryCount=%u dataSize=%zu stride=%zu flags=%u",
                firstQuery, queryCount, dataSize, stride, flags);
        }
        free(data);
    }
    break;

    case FUNID_vkGetBufferMemoryRequirements2:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);

        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;
        uint64_t guest_device = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);

        VkBufferMemoryRequirementsInfo2 info;
        decode_from_stream_VkBufferMemoryRequirementsInfo2(VK_STRUCTURE_TYPE_MAX_ENUM, &info, ptr);
        VkBuffer host_buffer = info.buffer;

        VkDevice device = (VkDevice)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_device);

        VkMemoryRequirements2 requirements = {};
        requirements.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
        if (device == VK_NULL_HANDLE || host_buffer == VK_NULL_HANDLE) {
            LOGE("Host: vkGetBufferMemoryRequirements2 skip invalid mapping guest_dev=0x%llx host_dev=%p host_buffer=%p",
                 (unsigned long long)guest_device,
                 (void*)device,
                 (void*)host_buffer);
            write_to_guest_mem(all_para[1].data, &requirements, 0, sizeof(VkMemoryRequirements2));
            break;
        }

        LOGD("Host: before vkGetBufferMemoryRequirements2 device=%p host_buffer=%p size=%llu alignment=%llu memoryTypeBits=0x%x",
            (void*)device,
            (void*)host_buffer,
            (unsigned long long)requirements.memoryRequirements.size,
            (unsigned long long)requirements.memoryRequirements.alignment,
            requirements.memoryRequirements.memoryTypeBits);
        vkGetBufferMemoryRequirements2(device, &info, &requirements);

        write_to_guest_mem(all_para[1].data, &requirements, 0, sizeof(VkMemoryRequirements2));
        LOGD("Host: vkGetBufferMemoryRequirements2 device=%p buffer=%p size=%llu alignment=%llu memoryTypeBits=0x%x",
            (void*)device,
            (void*)info.buffer,
            (unsigned long long)requirements.memoryRequirements.size,
            (unsigned long long)requirements.memoryRequirements.alignment,
            requirements.memoryRequirements.memoryTypeBits);
        log_ncnn_align_classified("vkGetBufferMemoryRequirements2",
            requirements.memoryRequirements.alignment,
            guest_device,
            device,
            0,
            (uint64_t)(uintptr_t)info.buffer);
        if (requirements.memoryRequirements.alignment == 0) {
            LOGE("SEARCH_ME_ALIGNMENT_ZERO: vkGetBufferMemoryRequirements2 alignment==0, size=%llu memoryTypeBits=0x%x",
                (unsigned long long)requirements.memoryRequirements.size,
                requirements.memoryRequirements.memoryTypeBits);
        }
    }
    break;

    case FUNID_vkGetDeviceQueue2:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);

        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_device;
        memcpy(&guest_device, *ptr, 8); *ptr += 8;

        VkDeviceQueueInfo2 queueInfo;
        decode_from_stream_VkDeviceQueueInfo2(VK_STRUCTURE_TYPE_MAX_ENUM, &queueInfo, ptr);

        uint64_t guest_queue;
        memcpy(&guest_queue, *ptr, 8); *ptr += 8;

        VkDevice device = (VkDevice)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_device);

        VkQueue queue;
        vkGetDeviceQueue2(device, &queueInfo, &queue);

        insert_mapping(EXPRESS_VK_OBJECT_TYPE_QUEUE, guest_queue, (uint64_t)(uintptr_t)queue);
        express_vk_remember_queue(device, queue);

        LOGD("GetDeviceQueue2 queueFamilyIndex=%u queueIndex=%u",
            queueInfo.queueFamilyIndex, queueInfo.queueIndex);
    }
    break;

    case FUNID_vkMergePipelineCaches:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);

        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;
        uint64_t guest_device = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint64_t guest_dstCache = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint32_t srcCacheCount = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);

        VkDevice device = (VkDevice)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_device);
        VkPipelineCache dstCache = guest_dstCache ?
            (VkPipelineCache)(uintptr_t)lookup_mapping(
                EXPRESS_VK_OBJECT_TYPE_PIPELINE_CACHE,
                guest_dstCache) :
            VK_NULL_HANDLE;

        VkPipelineCache* srcCaches = (VkPipelineCache*)malloc(srcCacheCount * sizeof(VkPipelineCache));
        uint64_t* guest_srcCaches = (uint64_t*)malloc(srcCacheCount * sizeof(uint64_t));
        if (!srcCaches || !guest_srcCaches ||
            !copy_from_call_para_fast(all_para[1], guest_srcCaches, srcCacheCount * sizeof(uint64_t))) {
            free(srcCaches);
            if (guest_srcCaches) free(guest_srcCaches);
            LOGE("Host: vkMergePipelineCaches failed to get source caches");
            break;
        }

        for (uint32_t i = 0; i < srcCacheCount; ++i) {
            srcCaches[i] = guest_srcCaches[i] ?
                (VkPipelineCache)(uintptr_t)lookup_mapping(
                    EXPRESS_VK_OBJECT_TYPE_PIPELINE_CACHE,
                    guest_srcCaches[i]) :
                VK_NULL_HANDLE;
        }

        VkResult result = vkMergePipelineCaches(device, dstCache, srcCacheCount, srcCaches);
        if (result != VK_SUCCESS) {
            LOGE("Host: vkMergePipelineCaches failed with error %d", result);
        } else {
            LOGD("Host: vkMergePipelineCaches success, dstCache=%p srcCacheCount=%u",
                (void*)dstCache, srcCacheCount);
        }

        free(srcCaches);
        free(guest_srcCaches);
    }
    break;

    case FUNID_vkCreateQueryPool:
    {
        LOGD("get call FUNID_vkCreateQueryPool!");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        LOGD("get vk param number %d", para_num);

        int need_free = 0;
        char* stream_ptr = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** stream_ptr_ptr = (uint8_t**)&stream_ptr;

        uint64_t guest_device = *(uint64_t*)(*stream_ptr_ptr);
        *stream_ptr_ptr += sizeof(uint64_t);

        VkQueryPoolCreateInfo* pCreateInfo = (VkQueryPoolCreateInfo*)malloc(sizeof(VkQueryPoolCreateInfo));
        decode_from_stream_VkQueryPoolCreateInfo(VK_STRUCTURE_TYPE_MAX_ENUM, pCreateInfo, stream_ptr_ptr);

        VkAllocationCallbacks* guest_allocator = (VkAllocationCallbacks*)(*(uint64_t*)(*stream_ptr_ptr));
        *stream_ptr_ptr += 8;

        const VkAllocationCallbacks* pAllocator = NULL;
        if (guest_allocator) {
            pAllocator = (VkAllocationCallbacks*)malloc(sizeof(VkAllocationCallbacks));
            decode_from_stream_VkAllocationCallbacks(VK_STRUCTURE_TYPE_MAX_ENUM, (VkAllocationCallbacks*)pAllocator, stream_ptr_ptr);
        }

        uint64_t guest_querypool = *(uint64_t*)(*stream_ptr_ptr);
        *stream_ptr_ptr += sizeof(uint64_t);

        VkDevice device = (VkDevice)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_device);
        VkQueryPool queryPool;

        VkResult result = vkCreateQueryPool(device, pCreateInfo, pAllocator, &queryPool);

        if (result == VK_SUCCESS) {
            LOGD("got result %d map querypool %llx guest %llx", result, (uint64_t)(uintptr_t)queryPool, guest_querypool);
            insert_mapping(EXPRESS_VK_OBJECT_TYPE_QUERY_POOL, guest_querypool, (uint64_t)(uintptr_t)queryPool);
        } else {
            LOGE("Host: vkCreateQueryPool failed with error %d", result);
        }

        if (pCreateInfo) free(pCreateInfo);
        if (pAllocator) free((void*)pAllocator);
        if (need_free) g_free(stream_ptr);
    }
    break;

    case FUNID_vkBindImageMemory2:
    {
        LOGD("Host: vkBindImageMemory2 request");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);

        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_device = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        uint32_t bindInfoCount = *(uint32_t*)(*ptr);
        *ptr += sizeof(uint32_t);

        VkDevice device = (VkDevice)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_device);

        VkBindImageMemoryInfo* pBindInfos = malloc(bindInfoCount * sizeof(VkBindImageMemoryInfo));

        for (uint32_t i = 0; i < bindInfoCount; ++i) {
            decode_from_stream_VkBindImageMemoryInfo(VK_STRUCTURE_TYPE_MAX_ENUM,
                                                    &pBindInfos[i], ptr);
        }

        VkResult result = vkBindImageMemory2(device, bindInfoCount, pBindInfos);
        if (result != VK_SUCCESS) {
            LOGE("Host: vkBindImageMemory2 failed with error %d", result);
        } else {
            LOGD("Host: vkBindImageMemory2 success, bindInfoCount=%u", bindInfoCount);
        }

        free(pBindInfos);
    }
    break;

    case FUNID_vkDestroyBuffer: {
        LOGD("Host: vkDestroyBuffer request");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        LOGD("Host vkDestroyBuffer para count = %d", para_num);

        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        if (!stream) {
            LOGE("Host: vkDestroyBuffer failed, stream is NULL");
            break;
        }
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_dev = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        uint64_t guest_buf = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        uint64_t guest_alloc_ptr = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        VkAllocationCallbacks allocStruct;
        const VkAllocationCallbacks* pAllocator = NULL;
        if (guest_alloc_ptr) {
            decode_from_stream_VkAllocationCallbacks(VK_STRUCTURE_TYPE_MAX_ENUM, &allocStruct, ptr);
            pAllocator = &allocStruct;
        }

        g_mutex_lock(&g_express_vk_transaction_lock);
        VkResult result = destroy_vulkan_object_device(
            guest_dev,
            guest_buf,
            EXPRESS_VK_OBJECT_TYPE_BUFFER,
            (void (*)(VkDevice, void*, const VkAllocationCallbacks*))vkDestroyBuffer,
            pAllocator
        );
        g_mutex_unlock(&g_express_vk_transaction_lock);

        if (result != VK_SUCCESS) {
            LOGE("Host: vkDestroyBuffer failed for guest buffer %llu", (unsigned long long)guest_buf);
        } else {
            LOGD("Host: vkDestroyBuffer completed for guest buffer %llu", (unsigned long long)guest_buf);
        }

        //if (need_free) free(stream);
    }
    break;

    case FUNID_vkDestroyBufferView: {
        LOGD("Host: vkDestroyBufferView request");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        LOGD("Host vkDestroyBufferView para count = %d", para_num);

        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        if (!stream) {
            LOGE("Host: vkDestroyBufferView failed, stream is NULL");
            break;
        }
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_dev = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        uint64_t guest_buf_view = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        uint64_t guest_alloc_ptr = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        VkAllocationCallbacks allocStruct;
        const VkAllocationCallbacks* pAllocator = NULL;
        if (guest_alloc_ptr) {
            decode_from_stream_VkAllocationCallbacks(VK_STRUCTURE_TYPE_MAX_ENUM, &allocStruct, ptr);
            pAllocator = &allocStruct;
        }

        g_mutex_lock(&g_express_vk_transaction_lock);
        VkResult result = destroy_vulkan_object_device(
            guest_dev,
            guest_buf_view,
            EXPRESS_VK_OBJECT_TYPE_BUFFER_VIEW,
            (void (*)(VkDevice, void*, const VkAllocationCallbacks*))vkDestroyBufferView,
            pAllocator
        );
        g_mutex_unlock(&g_express_vk_transaction_lock);

        if (result != VK_SUCCESS) {
            LOGE("Host: vkDestroyBufferView failed for guest buffer view %llu", (unsigned long long)guest_buf_view);
        } else {
            LOGD("Host: vkDestroyBufferView completed for guest buffer view %llu", (unsigned long long)guest_buf_view);
        }

        //if (need_free) free(stream);
     }
     break;

    case FUNID_vkDestroyCommandPool: {
        LOGD("Host: vkDestroyCommandPool request");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        LOGD("Host vkDestroyCommandPool para count = %d", para_num);

        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        if (!stream) {
            LOGE("Host: vkDestroyCommandPool failed, stream is NULL");
            break;
        }
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_dev = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        uint64_t guest_cmd_pool = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        uint64_t guest_alloc_ptr = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        VkAllocationCallbacks allocStruct;
        const VkAllocationCallbacks* pAllocator = NULL;
        if (guest_alloc_ptr) {
            decode_from_stream_VkAllocationCallbacks(VK_STRUCTURE_TYPE_MAX_ENUM, &allocStruct, ptr);
            pAllocator = &allocStruct;
        }

        VkResult result = destroy_vulkan_object_device(
            guest_dev,
            guest_cmd_pool,
            EXPRESS_VK_OBJECT_TYPE_COMMAND_POOL,
            (void (*)(VkDevice, void*, const VkAllocationCallbacks*))vkDestroyCommandPool,
            pAllocator
        );

        if (result != VK_SUCCESS) {
            LOGE("Host: vkDestroyCommandPool failed for guest command pool %llu", (unsigned long long)guest_cmd_pool);
        } else {
            LOGD("Host: vkDestroyCommandPool completed for guest command pool %llu", (unsigned long long)guest_cmd_pool);
        }

        //if (need_free) free(stream);
     }
    break;

    case FUNID_vkDestroyDescriptorPool: {
        LOGD("Host: vkDestroyDescriptorPool request");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        LOGD("Host vkDestroyDescriptorPool para count = %d", para_num);

        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        if (!stream) {
            LOGE("Host: vkDestroyDescriptorPool failed, stream is NULL");
            break;
        }
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_dev = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        uint64_t guest_desc_pool = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        uint64_t guest_alloc_ptr = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        VkAllocationCallbacks allocStruct;
        const VkAllocationCallbacks* pAllocator = NULL;
        if (guest_alloc_ptr) {
            decode_from_stream_VkAllocationCallbacks(VK_STRUCTURE_TYPE_MAX_ENUM, &allocStruct, ptr);
            pAllocator = &allocStruct;
        }

        g_mutex_lock(&g_express_vk_transaction_lock);
        VkDescriptorPool host_desc_pool = (VkDescriptorPool)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DESCRIPTOR_POOL, guest_desc_pool);

        VkResult result = destroy_vulkan_object_device(
            guest_dev,
            guest_desc_pool,
            EXPRESS_VK_OBJECT_TYPE_DESCRIPTOR_POOL,
            (void (*)(VkDevice, void*, const VkAllocationCallbacks*))vkDestroyDescriptorPool,
            pAllocator
        );

        if (result != VK_SUCCESS) {
            LOGE("Host: vkDestroyDescriptorPool failed for guest descriptor pool %llu", (unsigned long long)guest_desc_pool);
        } else {
            express_vk_forget_descriptor_sets_for_pool(host_desc_pool);
            LOGD("Host: vkDestroyDescriptorPool completed for guest descriptor pool %llu", (unsigned long long)guest_desc_pool);
        }

        g_mutex_unlock(&g_express_vk_transaction_lock);
        //if (need_free) free(stream);
    }
    break;

    case FUNID_vkDestroyDescriptorSetLayout: {
        LOGD("Host: vkDestroyDescriptorSetLayout request");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        LOGD("Host vkDestroyDescriptorSetLayout para count = %d", para_num);

        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        if (!stream) {
            LOGE("Host: vkDestroyDescriptorSetLayout failed, stream is NULL");
            break;
        }
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_dev = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        uint64_t guest_desc_set_layout = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        uint64_t guest_alloc_ptr = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        VkAllocationCallbacks allocStruct;
        const VkAllocationCallbacks* pAllocator = NULL;
        if (guest_alloc_ptr) {
            decode_from_stream_VkAllocationCallbacks(VK_STRUCTURE_TYPE_MAX_ENUM, &allocStruct, ptr);
            pAllocator = &allocStruct;
        }

        g_mutex_lock(&g_express_vk_transaction_lock);

        VkResult result = destroy_vulkan_object_device(
            guest_dev,
            guest_desc_set_layout,
            EXPRESS_VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT,
            (void (*)(VkDevice, void*, const VkAllocationCallbacks*))vkDestroyDescriptorSetLayout,
            pAllocator
        );

        if (result != VK_SUCCESS) {
            LOGE("Host: vkDestroyDescriptorSetLayout failed for guest descriptor set layout %llu", (unsigned long long)guest_desc_set_layout);
        } else {
            LOGD("Host: vkDestroyDescriptorSetLayout completed for guest descriptor set layout %llu", (unsigned long long)guest_desc_set_layout);
        }

        g_mutex_unlock(&g_express_vk_transaction_lock);
        //if (need_free) free(stream);
    }
    break;

    case FUNID_vkDestroyDescriptorUpdateTemplate: {
        LOGD("Host: vkDestroyDescriptorUpdateTemplate request");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        LOGD("Host vkDestroyDescriptorUpdateTemplate para count = %d", para_num);

        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        if (!stream) {
            LOGE("Host: vkDestroyDescriptorUpdateTemplate failed, stream is NULL");
            break;
        }
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_dev = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        uint64_t guest_desc_update_template = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        uint64_t guest_alloc_ptr = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        VkAllocationCallbacks allocStruct;
        const VkAllocationCallbacks* pAllocator = NULL;
        if (guest_alloc_ptr) {
            decode_from_stream_VkAllocationCallbacks(VK_STRUCTURE_TYPE_MAX_ENUM, &allocStruct, ptr);
            pAllocator = &allocStruct;
        }

        g_mutex_lock(&g_express_vk_transaction_lock);
        VkResult result = destroy_vulkan_object_device(
            guest_dev,
            guest_desc_update_template,
            EXPRESS_VK_OBJECT_TYPE_DESCRIPTOR_UPDATE_TEMPLATE,
            (void (*)(VkDevice, void*, const VkAllocationCallbacks*))vkDestroyDescriptorUpdateTemplate,
            pAllocator
        );
        g_mutex_unlock(&g_express_vk_transaction_lock);

        if (result != VK_SUCCESS) {
            LOGE("Host: vkDestroyDescriptorUpdateTemplate failed for guest descriptor update template %llu", (unsigned long long)guest_desc_update_template);
        } else {
            LOGD("Host: vkDestroyDescriptorUpdateTemplate completed for guest descriptor update template %llu", (unsigned long long)guest_desc_update_template);
        }

        //if (need_free) free(stream);
    }
    break;

    //note: device
    case FUNID_vkDestroyDevice: {
        LOGD("Host: vkDestroyDevice request");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        LOGD("Host vkDestroyDevice para count = %d", para_num);

        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        if (!stream) {
            LOGE("Host: vkDestroyDevice failed, stream is NULL");
            break;
        }
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_dev = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        uint64_t guest_alloc_ptr = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        VkAllocationCallbacks allocStruct;
        const VkAllocationCallbacks* pAllocator = NULL;
        if (guest_alloc_ptr) {
            decode_from_stream_VkAllocationCallbacks(VK_STRUCTURE_TYPE_MAX_ENUM, &allocStruct, ptr);
            pAllocator = &allocStruct;
        }

        /*
         * The guest normally sends a synchronous FLIME teardown before this
         * asynchronous destroy. Retire only this device's bound stream as a
         * fallback so another VkDevice owned by the process keeps its pending
         * descriptor shadow intact.
         */
        g_mutex_lock(&g_express_vk_transaction_lock);
        express_vk_flime_bridge_cleanup_device(call->process_id, guest_dev);
        VkResult result = destroy_vulkan_object_essential(
            guest_dev,
            EXPRESS_VK_OBJECT_TYPE_DEVICE,
            (void (*)(void*, const VkAllocationCallbacks*))vkDestroyDevice,
            pAllocator
        );
        g_mutex_unlock(&g_express_vk_transaction_lock);

        if (result != VK_SUCCESS) {
            LOGE("Host: vkDestroyDevice failed for guest device %llu", (unsigned long long)guest_dev);
        } else {
            LOGD("Host: vkDestroyDevice completed for guest device %llu", (unsigned long long)guest_dev);
        }

        //if (need_free) free(stream);
    }
    break;

    case FUNID_vkDestroyEvent: {
        LOGD("Host: vkDestroyEvent request");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        LOGD("Host vkDestroyEvent para count = %d", para_num);

        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        if (!stream) {
            LOGE("Host: vkDestroyEvent failed, stream is NULL");
            break;
        }
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_dev = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        uint64_t guest_event = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        uint64_t guest_alloc_ptr = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        VkAllocationCallbacks allocStruct;
        const VkAllocationCallbacks* pAllocator = NULL;
        if (guest_alloc_ptr) {
            decode_from_stream_VkAllocationCallbacks(VK_STRUCTURE_TYPE_MAX_ENUM, &allocStruct, ptr);
            pAllocator = &allocStruct;
        }

        VkResult result = destroy_vulkan_object_device(
            guest_dev,
            guest_event,
            EXPRESS_VK_OBJECT_TYPE_EVENT,
            (void (*)(VkDevice, void*, const VkAllocationCallbacks*))vkDestroyEvent,
            pAllocator
        );

        if (result != VK_SUCCESS) {
            LOGE("Host: vkDestroyEvent failed for guest event %llu", (unsigned long long)guest_event);
        } else {
            LOGD("Host: vkDestroyEvent completed for guest event %llu", (unsigned long long)guest_event);
        }

        //if (need_free) free(stream);
    }
    break;

    case FUNID_vkDestroyFence: {
        LOGD("Host: vkDestroyFence request");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        LOGD("Host vkDestroyFence para count = %d", para_num);

        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        if (!stream) {
            LOGE("Host: vkDestroyFence failed, stream is NULL");
            break;
        }
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_dev = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        uint64_t guest_fence = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        uint64_t guest_alloc_ptr = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        VkAllocationCallbacks allocStruct;
        const VkAllocationCallbacks* pAllocator = NULL;
        if (guest_alloc_ptr) {
            decode_from_stream_VkAllocationCallbacks(VK_STRUCTURE_TYPE_MAX_ENUM, &allocStruct, ptr);
            pAllocator = &allocStruct;
        }

        VkResult result = destroy_vulkan_object_device(
            guest_dev,
            guest_fence,
            EXPRESS_VK_OBJECT_TYPE_FENCE,
            (void (*)(VkDevice, void*, const VkAllocationCallbacks*))vkDestroyFence,
            pAllocator
        );

        if (result != VK_SUCCESS) {
            LOGE("Host: vkDestroyFence failed for guest fence %llu", (unsigned long long)guest_fence);
        } else {
            LOGD("Host: vkDestroyFence completed for guest fence %llu", (unsigned long long)guest_fence);
        }

        //if (need_free) free(stream);
    }
    break;

    case FUNID_vkDestroyFramebuffer: {
        LOGD("Host: vkDestroyFramebuffer request");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        LOGD("Host vkDestroyFramebuffer para count = %d", para_num);

        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        if (!stream) {
            LOGE("Host: vkDestroyFramebuffer failed, stream is NULL");
            break;
        }
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_dev = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        uint64_t guest_framebuffer = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        uint64_t guest_alloc_ptr = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        VkAllocationCallbacks allocStruct;
        const VkAllocationCallbacks* pAllocator = NULL;
        if (guest_alloc_ptr) {
            decode_from_stream_VkAllocationCallbacks(VK_STRUCTURE_TYPE_MAX_ENUM, &allocStruct, ptr);
            pAllocator = &allocStruct;
        }

        if (guest_framebuffer == 0) {
            LOGD("Host: vkDestroyFramebuffer ignored null framebuffer");
            break;
        }

        VkResult result = destroy_vulkan_object_device(
            guest_dev,
            guest_framebuffer,
            EXPRESS_VK_OBJECT_TYPE_FRAMEBUFFER,
            (void (*)(VkDevice, void*, const VkAllocationCallbacks*))vkDestroyFramebuffer,
            pAllocator
        );

        if (result != VK_SUCCESS) {
            LOGE("Host: vkDestroyFramebuffer failed for guest framebuffer %llu", (unsigned long long)guest_framebuffer);
        } else {
            LOGD("Host: vkDestroyFramebuffer completed for guest framebuffer %llu", (unsigned long long)guest_framebuffer);
        }

        //if (need_free) free(stream);
    }
    break;

    case FUNID_vkDestroyImage: {
        LOGD("Host: vkDestroyImage request");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        LOGD("Host vkDestroyImage para count = %d", para_num);

        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        if (!stream) {
            LOGE("Host: vkDestroyImage failed, stream is NULL");
            break;
        }
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_dev = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        uint64_t guest_image = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        uint64_t guest_alloc_ptr = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        VkAllocationCallbacks allocStruct;
        const VkAllocationCallbacks* pAllocator = NULL;
        if (guest_alloc_ptr) {
            decode_from_stream_VkAllocationCallbacks(VK_STRUCTURE_TYPE_MAX_ENUM, &allocStruct, ptr);
            pAllocator = &allocStruct;
        }

        VkResult result = destroy_vulkan_object_device(
            guest_dev,
            guest_image,
            EXPRESS_VK_OBJECT_TYPE_IMAGE,
            (void (*)(VkDevice, void*, const VkAllocationCallbacks*))vkDestroyImage,
            pAllocator
        );

        if (result != VK_SUCCESS) {
            LOGE("Host: vkDestroyImage failed for guest image %llu", (unsigned long long)guest_image);
        } else {
            LOGD("Host: vkDestroyImage completed for guest image %llu", (unsigned long long)guest_image);
        }

        //if (need_free) free(stream);
    }
    break;

    case FUNID_vkDestroyImageView: {
        LOGD("Host: vkDestroyImageView request");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        LOGD("Host vkDestroyImageView para count = %d", para_num);

        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        if (!stream) {
            LOGE("Host: vkDestroyImageView failed, stream is NULL");
            break;
        }
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_dev = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        uint64_t guest_image_view = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        uint64_t guest_alloc_ptr = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        VkAllocationCallbacks allocStruct;
        const VkAllocationCallbacks* pAllocator = NULL;
        if (guest_alloc_ptr) {
            decode_from_stream_VkAllocationCallbacks(VK_STRUCTURE_TYPE_MAX_ENUM, &allocStruct, ptr);
            pAllocator = &allocStruct;
        }

        g_mutex_lock(&g_express_vk_transaction_lock);
        VkResult result = destroy_vulkan_object_device(
            guest_dev,
            guest_image_view,
            EXPRESS_VK_OBJECT_TYPE_IMAGE_VIEW,
            (void (*)(VkDevice, void*, const VkAllocationCallbacks*))vkDestroyImageView,
            pAllocator
        );
        g_mutex_unlock(&g_express_vk_transaction_lock);

        if (result != VK_SUCCESS) {
            LOGE("Host: vkDestroyImageView failed for guest image view %llu", (unsigned long long)guest_image_view);
        } else {
            LOGD("Host: vkDestroyImageView completed for guest image view %llu", (unsigned long long)guest_image_view);
        }

        //if (need_free) free(stream);
    }
    break;

    //note: instance
    case FUNID_vkDestroyInstance: {
        LOGD("Host: vkDestroyInstance request");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        LOGD("Host vkDestroyInstance para count = %d", para_num);
        if (para_num != 1 ||
            all_para[0].data_len < 2 * sizeof(uint64_t) ||
            !teleport_express_guest_mem_layout_exact(
                all_para[0].data, all_para[0].data_len)) {
            LOGE("Host: malformed vkDestroyInstance parameter envelope");
            break;
        }

        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        if (!stream) {
            LOGE("Host: vkDestroyInstance failed, stream is NULL");
            break;
        }
        char *stream_base = stream;
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_instance = 0;
        memcpy(&guest_instance, *ptr, sizeof(guest_instance));
        *ptr += sizeof(uint64_t);

        uint64_t guest_alloc_ptr = 0;
        memcpy(&guest_alloc_ptr, *ptr, sizeof(guest_alloc_ptr));
        *ptr += sizeof(uint64_t);
        VkAllocationCallbacks ignored_guest_allocator = { 0 };
        if (guest_alloc_ptr) {
            size_t consumed = (size_t)((uintptr_t)*ptr -
                                       (uintptr_t)stream_base);
            size_t remaining = all_para[0].data_len - consumed;
            uint64_t guest_user_data = 0;
            size_t allocator_wire_size =
                sizeof(uint64_t) + 5 * sizeof(uint64_t);

            if (remaining < allocator_wire_size) {
                LOGE("Host: truncated vkDestroyInstance allocator stream");
                if (need_free) {
                    g_free(stream_base);
                }
                break;
            }
            memcpy(&guest_user_data, *ptr, sizeof(guest_user_data));
            if (guest_user_data != 0) {
                allocator_wire_size++;
            }
            if (remaining < allocator_wire_size) {
                LOGE("Host: truncated vkDestroyInstance allocator payload");
                if (need_free) {
                    g_free(stream_base);
                }
                break;
            }
            /*
             * Consume the legacy encoding, but match vkCreateInstance by
             * keeping guest callback addresses out of the host Vulkan loader.
             */
            decode_from_stream_VkAllocationCallbacks(
                VK_STRUCTURE_TYPE_MAX_ENUM, &ignored_guest_allocator, ptr);
            g_free(ignored_guest_allocator.pUserData);
            ignored_guest_allocator.pUserData = NULL;
        }

        VkResult result = destroy_vulkan_object_essential(
            guest_instance,
            EXPRESS_VK_OBJECT_TYPE_INSTANCE,
            (void (*)(void*, const VkAllocationCallbacks*))vkDestroyInstance,
            NULL
        );

        if (result != VK_SUCCESS) {
            LOGE("Host: vkDestroyInstance failed for guest instance %llu", (unsigned long long)guest_instance);
        } else {
            express_vk_instance_note_destroy();
            LOGD("Host: vkDestroyInstance completed for guest instance %llu", (unsigned long long)guest_instance);
        }

        if (need_free) {
            g_free(stream_base);
        }
    }
    break;

    case FUNID_vkDestroyPipeline: {
        LOGD("Host: vkDestroyPipeline request");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        LOGD("Host vkDestroyPipeline para count = %d", para_num);

        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        if (!stream) {
            LOGE("Host: vkDestroyPipeline failed, stream is NULL");
            break;
        }
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_dev = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        uint64_t guest_pipeline = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        uint64_t guest_alloc_ptr = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        VkAllocationCallbacks allocStruct;
        const VkAllocationCallbacks* pAllocator = NULL;
        if (guest_alloc_ptr) {
            decode_from_stream_VkAllocationCallbacks(VK_STRUCTURE_TYPE_MAX_ENUM, &allocStruct, ptr);
            pAllocator = &allocStruct;
        }


        VkResult result = destroy_vulkan_object_device(
            guest_dev,
            guest_pipeline,
            EXPRESS_VK_OBJECT_TYPE_PIPELINE,
            (void (*)(VkDevice, void*, const VkAllocationCallbacks*))vkDestroyPipeline,
            pAllocator
        );

        if (result != VK_SUCCESS) {
            LOGE("Host: vkDestroyPipeline failed for guest pipeline %llu", (unsigned long long)guest_pipeline);
        } else {
            LOGD("Host: vkDestroyPipeline completed for guest pipeline %llu", (unsigned long long)guest_pipeline);
        }

        //if (need_free) free(stream);
    }
    break;

    case FUNID_vkDestroyPipelineCache: {
        LOGD("Host: vkDestroyPipelineCache request");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        LOGD("Host vkDestroyPipelineCache para count = %d", para_num);

        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        if (!stream) {
            LOGE("Host: vkDestroyPipelineCache failed, stream is NULL");
            break;
        }
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_dev = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        uint64_t guest_pipeline_cache = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        uint64_t guest_alloc_ptr = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        VkAllocationCallbacks allocStruct;
        const VkAllocationCallbacks* pAllocator = NULL;
        if (guest_alloc_ptr) {
            decode_from_stream_VkAllocationCallbacks(VK_STRUCTURE_TYPE_MAX_ENUM, &allocStruct, ptr);
            pAllocator = &allocStruct;
        }

        VkResult result = destroy_vulkan_object_device(
            guest_dev,
            guest_pipeline_cache,
            EXPRESS_VK_OBJECT_TYPE_PIPELINE_CACHE,
            (void (*)(VkDevice, void*, const VkAllocationCallbacks*))vkDestroyPipelineCache,
            pAllocator
        );

        if (result != VK_SUCCESS) {
            LOGE("Host: vkDestroyPipelineCache failed for guest pipeline cache %llu", (unsigned long long)guest_pipeline_cache);
        } else {
            LOGD("Host: vkDestroyPipelineCache completed for guest pipeline cache %llu", (unsigned long long)guest_pipeline_cache);
        }

        //if (need_free) free(stream);
    }
    break;

    case FUNID_vkDestroyPipelineLayout: {
        LOGD("Host: vkDestroyPipelineLayout request");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        LOGD("Host vkDestroyPipelineLayout para count = %d", para_num);

        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        if (!stream) {
            LOGE("Host: vkDestroyPipelineLayout failed, stream is NULL");
            break;
        }
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_dev = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        uint64_t guest_pipeline_layout = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        uint64_t guest_alloc_ptr = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        VkAllocationCallbacks allocStruct;
        const VkAllocationCallbacks* pAllocator = NULL;
        if (guest_alloc_ptr) {
            decode_from_stream_VkAllocationCallbacks(VK_STRUCTURE_TYPE_MAX_ENUM, &allocStruct, ptr);
            pAllocator = &allocStruct;
        }


        VkResult result = destroy_vulkan_object_device(
            guest_dev,
            guest_pipeline_layout,
            EXPRESS_VK_OBJECT_TYPE_PIPELINE_LAYOUT,
            (void (*)(VkDevice, void*, const VkAllocationCallbacks*))vkDestroyPipelineLayout,
            pAllocator
        );

        if (result != VK_SUCCESS) {
            LOGE("Host: vkDestroyPipelineLayout failed for guest pipeline layout %llu", (unsigned long long)guest_pipeline_layout);
        } else {
            LOGD("Host: vkDestroyPipelineLayout completed for guest pipeline layout %llu", (unsigned long long)guest_pipeline_layout);
        }

        //if (need_free) free(stream);
    }
    break;

    case FUNID_vkDestroyPrivateDataSlot: {
        LOGD("Host: vkDestroyPrivateDataSlot request");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        LOGD("Host vkDestroyPrivateDataSlot para count = %d", para_num);

        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        if (!stream) {
            LOGE("Host: vkDestroyPrivateDataSlot failed, stream is NULL");
            break;
        }
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_dev = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        uint64_t guest_private_data_slot = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        uint64_t guest_alloc_ptr = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        VkAllocationCallbacks allocStruct;
        const VkAllocationCallbacks* pAllocator = NULL;
        if (guest_alloc_ptr) {
            decode_from_stream_VkAllocationCallbacks(VK_STRUCTURE_TYPE_MAX_ENUM, &allocStruct, ptr);
            pAllocator = &allocStruct;
        }

        VkResult result = destroy_vulkan_object_device(
            guest_dev,
            guest_private_data_slot,
            EXPRESS_VK_OBJECT_TYPE_PRIVATE_DATA_SLOT,
            (void (*)(VkDevice, void*, const VkAllocationCallbacks*))vkDestroyPrivateDataSlot,
            pAllocator
        );

        if (result != VK_SUCCESS) {
            LOGE("Host: vkDestroyPrivateDataSlot failed for guest private data slot %llu", (unsigned long long)guest_private_data_slot);
        } else {
            LOGD("Host: vkDestroyPrivateDataSlot completed for guest private data slot %llu", (unsigned long long)guest_private_data_slot);
        }

        //if (need_free) free(stream);
    }
    break;

    case FUNID_vkDestroyQueryPool: {
        LOGD("Host: vkDestroyQueryPool request");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        LOGD("Host vkDestroyQueryPool para count = %d", para_num);

        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        if (!stream) {
            LOGE("Host: vkDestroyQueryPool failed, stream is NULL");
            break;
        }
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_dev = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        uint64_t guest_query_pool = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        uint64_t guest_alloc_ptr = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        VkAllocationCallbacks allocStruct;
        const VkAllocationCallbacks* pAllocator = NULL;
        if (guest_alloc_ptr) {
            decode_from_stream_VkAllocationCallbacks(VK_STRUCTURE_TYPE_MAX_ENUM, &allocStruct, ptr);
            pAllocator = &allocStruct;
        }

        VkResult result = destroy_vulkan_object_device(
            guest_dev,
            guest_query_pool,
            EXPRESS_VK_OBJECT_TYPE_QUERY_POOL,
            (void (*)(VkDevice, void*, const VkAllocationCallbacks*))vkDestroyQueryPool,
            pAllocator
        );

        if (result != VK_SUCCESS) {
            LOGE("Host: vkDestroyQueryPool failed for guest query pool %llu", (unsigned long long)guest_query_pool);
        } else {
            LOGD("Host: vkDestroyQueryPool completed for guest query pool %llu", (unsigned long long)guest_query_pool);
        }

        //if (need_free) free(stream);
    }
    break;

    case FUNID_vkDestroyRenderPass: {
        LOGD("Host: vkDestroyRenderPass request");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        LOGD("Host vkDestroyRenderPass para count = %d", para_num);

        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        if (!stream) {
            LOGE("Host: vkDestroyRenderPass failed, stream is NULL");
            break;
        }
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_dev = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        uint64_t guest_render_pass = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        uint64_t guest_alloc_ptr = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        VkAllocationCallbacks allocStruct;
        const VkAllocationCallbacks* pAllocator = NULL;
        if (guest_alloc_ptr) {
            decode_from_stream_VkAllocationCallbacks(VK_STRUCTURE_TYPE_MAX_ENUM, &allocStruct, ptr);
            pAllocator = &allocStruct;
        }

        VkResult result = destroy_vulkan_object_device(
            guest_dev,
            guest_render_pass,
            EXPRESS_VK_OBJECT_TYPE_RENDER_PASS,
            (void (*)(VkDevice, void*, const VkAllocationCallbacks*))vkDestroyRenderPass,
            pAllocator
        );

        if (result != VK_SUCCESS) {
            LOGE("Host: vkDestroyRenderPass failed for guest render pass %llu", (unsigned long long)guest_render_pass);
        } else {
            LOGD("Host: vkDestroyRenderPass completed for guest render pass %llu", (unsigned long long)guest_render_pass);
        }

        //if (need_free) free(stream);
    }
    break;

    case FUNID_vkDestroySampler: {
        LOGD("Host: vkDestroySampler request");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        LOGD("Host vkDestroySampler para count = %d", para_num);

        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        if (!stream) {
            LOGE("Host: vkDestroySampler failed, stream is NULL");
            break;
        }
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_dev = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        uint64_t guest_sampler = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        uint64_t guest_alloc_ptr = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        VkAllocationCallbacks allocStruct;
        const VkAllocationCallbacks* pAllocator = NULL;
        if (guest_alloc_ptr) {
            decode_from_stream_VkAllocationCallbacks(VK_STRUCTURE_TYPE_MAX_ENUM, &allocStruct, ptr);
            pAllocator = &allocStruct;
        }

        g_mutex_lock(&g_express_vk_transaction_lock);
        VkResult result = destroy_vulkan_object_device(
            guest_dev,
            guest_sampler,
            EXPRESS_VK_OBJECT_TYPE_SAMPLER,
            (void (*)(VkDevice, void*, const VkAllocationCallbacks*))vkDestroySampler,
            pAllocator
        );
        g_mutex_unlock(&g_express_vk_transaction_lock);

        if (result != VK_SUCCESS) {
            LOGE("Host: vkDestroySampler failed for guest sampler %llu", (unsigned long long)guest_sampler);
        } else {
            LOGD("Host: vkDestroySampler completed for guest sampler %llu", (unsigned long long)guest_sampler);
        }

        //if (need_free) free(stream);
    }
    break;

    case FUNID_vkDestroySamplerYcbcrConversion: {
        LOGD("Host: vkDestroySamplerYcbcrConversion request");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        LOGD("Host vkDestroySamplerYcbcrConversion para count = %d", para_num);

        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        if (!stream) {
            LOGE("Host: vkDestroySamplerYcbcrConversion failed, stream is NULL");
            break;
        }
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_dev = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        uint64_t guest_ycbcr_conversion = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        uint64_t guest_alloc_ptr = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        VkAllocationCallbacks allocStruct;
        const VkAllocationCallbacks* pAllocator = NULL;
        if (guest_alloc_ptr) {
            decode_from_stream_VkAllocationCallbacks(VK_STRUCTURE_TYPE_MAX_ENUM, &allocStruct, ptr);
            pAllocator = &allocStruct;
        }

        VkResult result = destroy_vulkan_object_device(
            guest_dev,
            guest_ycbcr_conversion,
            EXPRESS_VK_OBJECT_TYPE_SAMPLER_YCBCR_CONVERSION,
            (void (*)(VkDevice, void*, const VkAllocationCallbacks*))vkDestroySamplerYcbcrConversion,
            pAllocator
        );

        if (result != VK_SUCCESS) {
            LOGE("Host: vkDestroySamplerYcbcrConversion failed for guest sampler YCbCr conversion %llu", (unsigned long long)guest_ycbcr_conversion);
        } else {
            LOGD("Host: vkDestroySamplerYcbcrConversion completed for guest sampler YCbCr conversion %llu", (unsigned long long)guest_ycbcr_conversion);
        }

        //if (need_free) free(stream);
    }
    break;

    case FUNID_vkDestroySemaphore: {
        LOGD("Host: vkDestroySemaphore request");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        LOGD("Host vkDestroySemaphore para count = %d", para_num);

        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        if (!stream) {
            LOGE("Host: vkDestroySemaphore failed, stream is NULL");
            break;
        }
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_dev = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        uint64_t guest_semaphore = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        uint64_t guest_alloc_ptr = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        VkAllocationCallbacks allocStruct;
        const VkAllocationCallbacks* pAllocator = NULL;
        if (guest_alloc_ptr) {
            decode_from_stream_VkAllocationCallbacks(VK_STRUCTURE_TYPE_MAX_ENUM, &allocStruct, ptr);
            pAllocator = &allocStruct;
        }

        VkResult result = destroy_vulkan_object_device(
            guest_dev,
            guest_semaphore,
            EXPRESS_VK_OBJECT_TYPE_SEMAPHORE,
            (void (*)(VkDevice, void*, const VkAllocationCallbacks*))vkDestroySemaphore,
            pAllocator
        );

        if (result != VK_SUCCESS) {
            LOGE("Host: vkDestroySemaphore failed for guest semaphore %llu", (unsigned long long)guest_semaphore);
        } else {
            LOGD("Host: vkDestroySemaphore completed for guest semaphore %llu", (unsigned long long)guest_semaphore);
        }

        //if (need_free) free(stream);
    }
    break;

    case FUNID_vkDestroyShaderModule: {
        LOGD("Host: vkDestroyShaderModule request");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        LOGD("Host vkDestroyShaderModule para count = %d", para_num);

        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        if (!stream) {
            LOGE("Host: vkDestroyShaderModule failed, stream is NULL");
            break;
        }
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_dev = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        uint64_t guest_shader_module = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        uint64_t guest_alloc_ptr = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        VkAllocationCallbacks allocStruct;
        const VkAllocationCallbacks* pAllocator = NULL;
        if (guest_alloc_ptr) {
            decode_from_stream_VkAllocationCallbacks(VK_STRUCTURE_TYPE_MAX_ENUM, &allocStruct, ptr);
            pAllocator = &allocStruct;
        }


        VkResult result = destroy_vulkan_object_device(
            guest_dev,
            guest_shader_module,
            EXPRESS_VK_OBJECT_TYPE_SHADER_MODULE,
            (void (*)(VkDevice, void*, const VkAllocationCallbacks*))vkDestroyShaderModule,
            pAllocator
        );

        if (result != VK_SUCCESS) {
            LOGE("Host: vkDestroyShaderModule failed for guest shader module %llu", (unsigned long long)guest_shader_module);
        } else {
            LOGD("Host: vkDestroyShaderModule completed for guest shader module %llu", (unsigned long long)guest_shader_module);
        }

        //if (need_free) free(stream);
    }
    break;

    //note: surface binded to instance
    case FUNID_vkDestroySurfaceKHR: {
        LOGD("Host: vkDestroySurfaceKHR request");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        LOGD("Host vkDestroySurfaceKHR para count = %d", para_num);

        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        if (!stream) {
            LOGE("Host: vkDestroySurfaceKHR failed, stream is NULL");
            break;
        }
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_instance = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        uint64_t guest_surface = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        uint64_t guest_alloc_ptr = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        VkAllocationCallbacks allocStruct;
        const VkAllocationCallbacks* pAllocator = NULL;
        if (guest_alloc_ptr) {
            decode_from_stream_VkAllocationCallbacks(VK_STRUCTURE_TYPE_MAX_ENUM, &allocStruct, ptr);
            pAllocator = &allocStruct;
        }

        VkResult result = destroy_vulkan_object_other(
            guest_instance,
            guest_surface,
            EXPRESS_VK_OBJECT_TYPE_INSTANCE,
            EXPRESS_VK_OBJECT_TYPE_SURFACE_KHR,
            (void (*)(void*, void*, const VkAllocationCallbacks*))vkDestroySurfaceKHR,
            pAllocator
        );

        if (result != VK_SUCCESS) {
            LOGE("Host: vkDestroySurfaceKHR failed for guest surface %llu", (unsigned long long)guest_surface);
        } else {
            LOGD("Host: vkDestroySurfaceKHR completed for guest surface %llu", (unsigned long long)guest_surface);
        }

        //if (need_free) free(stream);
    }
    break;

    case FUNID_vkDestroySwapchainKHR: {
        LOGD("Host: vkDestroySwapchainKHR request");

        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        LOGD("Host vkDestroySwapchainKHR para count = %d", para_num);

        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        if (!stream) {
            LOGE("Host: vkDestroySwapchainKHR failed, stream is NULL");
            break;
        }
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_dev = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        uint64_t guest_swapchain = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        uint64_t guest_alloc_ptr = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        VkAllocationCallbacks allocStruct;
        const VkAllocationCallbacks* pAllocator = NULL;
        if (guest_alloc_ptr) {
            decode_from_stream_VkAllocationCallbacks(VK_STRUCTURE_TYPE_MAX_ENUM, &allocStruct, ptr);
            pAllocator = &allocStruct;
        }

        VkResult result = destroy_vulkan_object_device(
            guest_dev,
            guest_swapchain,
            EXPRESS_VK_OBJECT_TYPE_SWAPCHAIN_KHR,
            (void (*)(VkDevice, void*, const VkAllocationCallbacks*))vkDestroySwapchainKHR,
            pAllocator
        );

        if (result != VK_SUCCESS) {
            LOGE("Host: vkDestroySwapchainKHR failed for guest swapchain %llu", (unsigned long long)guest_swapchain);
        } else {
            LOGD("Host: vkDestroySwapchainKHR completed for guest swapchain %llu", (unsigned long long)guest_swapchain);
        }

        //if (need_free) free(stream);
    }
    break;


    case FUNID_vkCmdBeginRendering:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        VkCommandBuffer commandBuffer = (VkCommandBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd);

        VkRenderingInfo renderingInfo = {};
        decode_from_stream_VkRenderingInfo(VK_STRUCTURE_TYPE_MAX_ENUM, &renderingInfo, ptr);

        vkCmdBeginRendering(commandBuffer, &renderingInfo);
        LOGD("Host: vkCmdBeginRendering called for command buffer %llu", (unsigned long long)guest_cmd);
    }
    break;

    case FUNID_vkCmdBeginRenderPass2:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        VkCommandBuffer commandBuffer = (VkCommandBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd);

        VkRenderPassBeginInfo renderPassBegin = {};
        VkSubpassBeginInfo subpassBegin = {};

        decode_from_stream_VkRenderPassBeginInfo(VK_STRUCTURE_TYPE_MAX_ENUM, &renderPassBegin, ptr);
        decode_from_stream_VkSubpassBeginInfo(VK_STRUCTURE_TYPE_MAX_ENUM, &subpassBegin, ptr);

        vkCmdBeginRenderPass2(commandBuffer, &renderPassBegin, &subpassBegin);
        LOGD("Host: vkCmdBeginRenderPass2 called for command buffer %llu", (unsigned long long)guest_cmd);
    }
    break;

    case FUNID_vkCmdBindIndexBuffer:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint64_t guest_buffer = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        VkDeviceSize offset = *(VkDeviceSize*)(*ptr); *ptr += sizeof(VkDeviceSize);
        VkIndexType indexType = *(VkIndexType*)(*ptr); *ptr += sizeof(VkIndexType);

        VkCommandBuffer commandBuffer = (VkCommandBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd);
        VkBuffer buffer = (VkBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_BUFFER, guest_buffer);

        vkCmdBindIndexBuffer(commandBuffer, buffer, offset, indexType);
        LOGD("Host: vkCmdBindIndexBuffer called for command buffer %llu with buffer %llu",
             (unsigned long long)guest_cmd, (unsigned long long)guest_buffer);
    }
    break;

    case FUNID_vkCmdBindVertexBuffers2:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd_buffer = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint32_t firstBinding = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);
        uint32_t bindingCount = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);
        uint32_t has_sizes = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);
        uint32_t has_strides = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);

        VkCommandBuffer commandBuffer = (VkCommandBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd_buffer);

        VkBuffer* buffers = (VkBuffer*)malloc(bindingCount * sizeof(VkBuffer));
        VkDeviceSize* offsets = (VkDeviceSize*)malloc(bindingCount * sizeof(VkDeviceSize));
        VkDeviceSize* sizes = has_sizes ? (VkDeviceSize*)malloc(bindingCount * sizeof(VkDeviceSize)) : NULL;
        VkDeviceSize* strides = has_strides ? (VkDeviceSize*)malloc(bindingCount * sizeof(VkDeviceSize)) : NULL;

        for (uint32_t i = 0; i < bindingCount; ++i) {
            uint64_t guest_buffer = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
            buffers[i] = (VkBuffer)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_BUFFER, guest_buffer);
            offsets[i] = *(VkDeviceSize*)(*ptr); *ptr += sizeof(VkDeviceSize);
        }

        if (has_sizes) {
            for (uint32_t i = 0; i < bindingCount; ++i) {
                sizes[i] = *(VkDeviceSize*)(*ptr); *ptr += sizeof(VkDeviceSize);
            }
        }

        if (has_strides) {
            for (uint32_t i = 0; i < bindingCount; ++i) {
                strides[i] = *(VkDeviceSize*)(*ptr); *ptr += sizeof(VkDeviceSize);
            }
        }

        vkCmdBindVertexBuffers2(commandBuffer, firstBinding, bindingCount, buffers, offsets, sizes, strides);

        LOGD("Host: vkCmdBindVertexBuffers2 called for command buffer %llu with %u bindings",
             (unsigned long long)guest_cmd_buffer, bindingCount);

        free(buffers);
        free(offsets);
        if (sizes) free(sizes);
        if (strides) free(strides);
    }
    break;

    case FUNID_vkCmdBlitImage2:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        VkCommandBuffer commandBuffer = (VkCommandBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd);

        VkBlitImageInfo2 blitImageInfo = {};
        decode_from_stream_VkBlitImageInfo2(VK_STRUCTURE_TYPE_MAX_ENUM, &blitImageInfo, ptr);

        vkCmdBlitImage2(commandBuffer, &blitImageInfo);

        LOGD("Host: vkCmdBlitImage2 called for command buffer %llu", (unsigned long long)guest_cmd);
    }
    break;

    case FUNID_vkCmdClearAttachments:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint32_t attachmentCount = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);
        uint32_t rectCount = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);

        VkCommandBuffer commandBuffer = (VkCommandBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd);

        VkClearAttachment* pAttachments = NULL;
        VkClearRect* pRects = NULL;

        if (attachmentCount > 0) {
            pAttachments = malloc(attachmentCount * sizeof(VkClearAttachment));
            memcpy(pAttachments, *ptr, attachmentCount * sizeof(VkClearAttachment));
            *ptr += attachmentCount * sizeof(VkClearAttachment);
        }

        if (rectCount > 0) {
            pRects = malloc(rectCount * sizeof(VkClearRect));
            memcpy(pRects, *ptr, rectCount * sizeof(VkClearRect));
            *ptr += rectCount * sizeof(VkClearRect);
        }

        vkCmdClearAttachments(commandBuffer, attachmentCount, pAttachments, rectCount, pRects);

        if (pAttachments) free(pAttachments);
        if (pRects) free(pRects);
        LOGD("Host: vkCmdClearAttachments called for command buffer %llu with %u attachments and %u rects",
             (unsigned long long)guest_cmd, attachmentCount, rectCount);
    }
    break;

    case FUNID_vkCmdClearColorImage:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint64_t guest_image = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint32_t layout = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);

        VkClearColorValue clearColor;
        decode_from_stream_VkClearColorValue(VK_STRUCTURE_TYPE_MAX_ENUM, &clearColor, ptr);

        uint32_t rangeCount = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);

        VkImageSubresourceRange* ranges = malloc(rangeCount * sizeof(VkImageSubresourceRange));
        for (uint32_t i = 0; i < rangeCount; ++i) {
            decode_from_stream_VkImageSubresourceRange(VK_STRUCTURE_TYPE_MAX_ENUM, &ranges[i], ptr);
        }

        VkCommandBuffer cmdBuffer = (VkCommandBuffer)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd);
        VkImage image = (VkImage)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_IMAGE, guest_image);

        vkCmdClearColorImage(cmdBuffer, image, (VkImageLayout)layout, &clearColor, rangeCount, ranges);
        LOGD("Host: vkCmdClearColorImage called for command buffer %llu with image %llu",
             (unsigned long long)guest_cmd, (unsigned long long)guest_image);

        free(ranges);
    }
    break;

    case FUNID_vkCmdClearDepthStencilImage:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint64_t guest_image = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint32_t layout = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);

        VkClearDepthStencilValue depthStencil;
        decode_from_stream_VkClearDepthStencilValue(VK_STRUCTURE_TYPE_MAX_ENUM, &depthStencil, ptr);

        uint32_t rangeCount = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);

        VkImageSubresourceRange* ranges = malloc(rangeCount * sizeof(VkImageSubresourceRange));
        for (uint32_t i = 0; i < rangeCount; ++i) {
            decode_from_stream_VkImageSubresourceRange(VK_STRUCTURE_TYPE_MAX_ENUM, &ranges[i], ptr);
        }

        VkCommandBuffer cmdBuffer = (VkCommandBuffer)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd);
        VkImage image = (VkImage)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_IMAGE, guest_image);

        vkCmdClearDepthStencilImage(cmdBuffer, image, (VkImageLayout)layout, &depthStencil, rangeCount, ranges);
        LOGD("Host: vkCmdClearDepthStencilImage called for command buffer %llu with image %llu",
             (unsigned long long)guest_cmd, (unsigned long long)guest_image);


        free(ranges);
    }
    break;

    case FUNID_vkCmdCopyBuffer2:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);

        VkCopyBufferInfo2 copyInfo = {};
        decode_from_stream_VkCopyBufferInfo2(VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2, &copyInfo, ptr);

        VkCommandBuffer cmdBuffer = (VkCommandBuffer)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd);

        vkCmdCopyBuffer2(cmdBuffer, &copyInfo);
        LOGD("Host: vkCmdCopyBuffer2 called for command buffer %llu", (unsigned long long)guest_cmd);
    }
    break;

    case FUNID_vkCmdCopyBufferToImage2:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);

        VkCopyBufferToImageInfo2 copyInfo;
        decode_from_stream_VkCopyBufferToImageInfo2(VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2, &copyInfo, ptr);

        VkCommandBuffer cmdBuffer = (VkCommandBuffer)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd);

        vkCmdCopyBufferToImage2(cmdBuffer, &copyInfo);
        LOGD("Host: vkCmdCopyBufferToImage2 called for command buffer %llu", (unsigned long long)guest_cmd);
    }
    break;

    case FUNID_vkCmdCopyImage2:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);

        VkCopyImageInfo2 copyInfo;
        decode_from_stream_VkCopyImageInfo2(VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2, &copyInfo, ptr);

        VkCommandBuffer cmdBuffer = (VkCommandBuffer)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd);

        vkCmdCopyImage2(cmdBuffer, &copyInfo);
        LOGD("Host: vkCmdCopyImage2 called for command buffer %llu", (unsigned long long)guest_cmd);
    }
    break;

    case FUNID_vkCmdCopyImageToBuffer2:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);

        VkCopyImageToBufferInfo2 copyInfo;
        decode_from_stream_VkCopyImageToBufferInfo2(VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2, &copyInfo, ptr);

        VkCommandBuffer cmdBuffer = (VkCommandBuffer)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd);

        vkCmdCopyImageToBuffer2(cmdBuffer, &copyInfo);
        LOGD("Host: vkCmdCopyImageToBuffer2 called for command buffer %llu", (unsigned long long)guest_cmd);
    }
    break;

    case FUNID_vkCmdDispatch:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd_buffer = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint32_t groupCountX = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);
        uint32_t groupCountY = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);
        uint32_t groupCountZ = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);

        VkCommandBuffer commandBuffer = (VkCommandBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd_buffer);

        vkCmdDispatch(commandBuffer, groupCountX, groupCountY, groupCountZ);

        LOGD("Host: vkCmdDispatch called for command buffer %llu with group counts (%u, %u, %u)",
             (unsigned long long)guest_cmd_buffer, groupCountX, groupCountY, groupCountZ);
    }
    break;

    case FUNID_vkCmdDispatchBase:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd_buffer = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint32_t baseGroupX = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);
        uint32_t baseGroupY = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);
        uint32_t baseGroupZ = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);
        uint32_t groupCountX = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);
        uint32_t groupCountY = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);
        uint32_t groupCountZ = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);

        VkCommandBuffer commandBuffer = (VkCommandBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd_buffer);

        vkCmdDispatchBase(commandBuffer, baseGroupX, baseGroupY, baseGroupZ,
                        groupCountX, groupCountY, groupCountZ);
        LOGD("in FUNID_vkCmdDispatchBase finish");
    }
    break;

    case FUNID_vkCmdDrawIndexed:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd_buffer = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint32_t indexCount = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);
        uint32_t instanceCount = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);
        uint32_t firstIndex = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);
        int32_t vertexOffset = *(int32_t*)(*ptr); *ptr += sizeof(uint32_t);
        uint32_t firstInstance = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);

        VkCommandBuffer commandBuffer = (VkCommandBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd_buffer);

        vkCmdDrawIndexed(commandBuffer, indexCount, instanceCount,
                        firstIndex, vertexOffset, firstInstance);
        LOGD("Host: vkCmdDrawIndexed called");
    }
    break;

    case FUNID_vkCmdDrawIndexedIndirectCount:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd_buffer = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint64_t guest_buffer = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        VkDeviceSize offset = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint64_t guest_count_buffer = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        VkDeviceSize countBufferOffset = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint32_t maxDrawCount = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);
        uint32_t stride = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);

        VkCommandBuffer commandBuffer = (VkCommandBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd_buffer);
        VkBuffer buffer = (VkBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_BUFFER, guest_buffer);
        VkBuffer countBuffer = (VkBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_BUFFER, guest_count_buffer);

        vkCmdDrawIndexedIndirectCount(commandBuffer, buffer, offset,
                                    countBuffer, countBufferOffset,
                                    maxDrawCount, stride);
        LOGD("Host: vkCmdDrawIndexedIndirectCount called for command buffer %llu",
             (unsigned long long)guest_cmd_buffer);
    }
    break;

    case FUNID_vkCmdDrawIndirectCount:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd_buffer = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint64_t guest_buffer = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        VkDeviceSize offset = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint64_t guest_count_buffer = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        VkDeviceSize countBufferOffset = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint32_t maxDrawCount = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);
        uint32_t stride = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);

        VkCommandBuffer commandBuffer = (VkCommandBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd_buffer);
        VkBuffer buffer = (VkBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_BUFFER, guest_buffer);
        VkBuffer countBuffer = (VkBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_BUFFER, guest_count_buffer);

        vkCmdDrawIndirectCount(commandBuffer, buffer, offset,
                            countBuffer, countBufferOffset,
                            maxDrawCount, stride);
        LOGD("Host: vkCmdDrawIndirectCount called for command buffer %llu",
             (unsigned long long)guest_cmd_buffer);
    }
    break;

    case FUNID_vkCmdEndRendering:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd_buffer = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);

        VkCommandBuffer commandBuffer = (VkCommandBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd_buffer);

        vkCmdEndRendering(commandBuffer);
        LOGD("Host: vkCmdEndRendering called for command buffer %llu", (unsigned long long)guest_cmd_buffer);
    }
    break;

    case FUNID_vkCmdEndRenderPass2:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd_buffer = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);

        VkCommandBuffer commandBuffer = (VkCommandBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd_buffer);

        VkSubpassEndInfo* guest_subpass_end = (VkSubpassEndInfo*)(*ptr);
        *ptr += 8;

        VkSubpassEndInfo* pSubpassEndInfo = NULL;
        if (guest_subpass_end) {
            pSubpassEndInfo = (VkSubpassEndInfo*)malloc(sizeof(VkSubpassEndInfo));
            decode_from_stream_VkSubpassEndInfo(VK_STRUCTURE_TYPE_MAX_ENUM,
                                            pSubpassEndInfo, ptr);
        }

        vkCmdEndRenderPass2(commandBuffer, pSubpassEndInfo);

        if (pSubpassEndInfo) free(pSubpassEndInfo);
        LOGD("Host: vkCmdEndRenderPass2 called for command buffer %llu", (unsigned long long)guest_cmd_buffer);
    }
    break;

    case FUNID_vkCmdExecuteCommands:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd_buffer = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint32_t commandBufferCount = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);

        VkCommandBuffer commandBuffer = (VkCommandBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd_buffer);

        VkCommandBuffer* pCommandBuffers = NULL;
        if (commandBufferCount > 0) {
            pCommandBuffers = (VkCommandBuffer*)malloc(commandBufferCount * sizeof(VkCommandBuffer));
            for (uint32_t i = 0; i < commandBufferCount; ++i) {
                uint64_t guest_secondary_cmd = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
                pCommandBuffers[i] = (VkCommandBuffer)(uintptr_t)
                    lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_secondary_cmd);
            }
        }

        vkCmdExecuteCommands(commandBuffer, commandBufferCount, pCommandBuffers);

        if (pCommandBuffers) free(pCommandBuffers);
        LOGD("Host: vkCmdExecuteCommands called for command buffer %llu with %u secondary command buffers",
             (unsigned long long)guest_cmd_buffer, commandBufferCount);
    }
    break;

    case FUNID_vkCmdNextSubpass:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint32_t contents = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);

        VkCommandBuffer commandBuffer = (VkCommandBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd);

        LOGD("Host: vkCmdNextSubpass called with contents %u", contents);

        vkCmdNextSubpass(commandBuffer, (VkSubpassContents)contents);
        LOGD("Host: vkCmdNextSubpass called for command buffer %llu with contents %u",
             (unsigned long long)guest_cmd, contents);
    }
    break;

    case FUNID_vkCmdNextSubpass2:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        // uint64_t guest_cmd = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);

        uint64_t begin_ptr = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        VkSubpassBeginInfo* pSubpassBeginInfo = NULL;
        if (begin_ptr) {
            pSubpassBeginInfo = malloc(sizeof(VkSubpassBeginInfo));
            decode_from_stream_VkSubpassBeginInfo(VK_STRUCTURE_TYPE_MAX_ENUM,
                                                pSubpassBeginInfo, ptr);
        }

        uint64_t end_ptr = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        VkSubpassEndInfo* pSubpassEndInfo = NULL;
        if (end_ptr) {
            pSubpassEndInfo = malloc(sizeof(VkSubpassEndInfo));
            decode_from_stream_VkSubpassEndInfo(VK_STRUCTURE_TYPE_MAX_ENUM,
                                                pSubpassEndInfo, ptr);
        }

        uint64_t guest_cmd = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);

        VkCommandBuffer commandBuffer = (VkCommandBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd);

        vkCmdNextSubpass2(commandBuffer, pSubpassBeginInfo, pSubpassEndInfo);

        if (pSubpassBeginInfo) free(pSubpassBeginInfo);
        if (pSubpassEndInfo) free(pSubpassEndInfo);
        LOGD("Host: vkCmdNextSubpass2 called for command buffer %llu", (unsigned long long)commandBuffer);
    }
    break;

    case FUNID_vkCmdPipelineBarrier2:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t dep_ptr = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        VkDependencyInfo* pDependencyInfo = NULL;
        if (dep_ptr) {
            pDependencyInfo = malloc(sizeof(VkDependencyInfo));
            LOGD("Host: vkCmdPipelineBarrier2 dep_ptr %llu", dep_ptr);
            decode_from_stream_VkDependencyInfo(VK_STRUCTURE_TYPE_MAX_ENUM,
                                                pDependencyInfo, ptr);
        }
        uint64_t guest_cmd = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);

        VkCommandBuffer commandBuffer = (VkCommandBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd);

        vkCmdPipelineBarrier2(commandBuffer, pDependencyInfo);

        if (pDependencyInfo) free(pDependencyInfo);
        LOGD("Host: vkCmdPipelineBarrier2 called for command buffer %llu", (unsigned long long)guest_cmd);
    }
    break;

    case FUNID_vkCmdResetEvent2:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint64_t guest_event = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        VkPipelineStageFlags2 stageMask = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);

        VkCommandBuffer commandBuffer = (VkCommandBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd);
        VkEvent event = (VkEvent)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_EVENT, guest_event);

        vkCmdResetEvent2(commandBuffer, event, stageMask);
        LOGD("Host: vkCmdResetEvent2 called for command buffer %llu with event %llu",
             (unsigned long long)guest_cmd, (unsigned long long)guest_event);
    }
    break;

    case FUNID_vkCmdResolveImage:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint64_t guest_src = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint32_t src_layout = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);
        uint64_t guest_dst = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint32_t dst_layout = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);
        uint32_t regionCount = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);

        VkCommandBuffer commandBuffer = (VkCommandBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd);
        VkImage srcImage = (VkImage)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_IMAGE, guest_src);
        VkImage dstImage = (VkImage)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_IMAGE, guest_dst);

        VkImageResolve* pRegions = NULL;
        if (regionCount > 0) {
            pRegions = (VkImageResolve*)stream;
        }

        vkCmdResolveImage(commandBuffer, srcImage, (VkImageLayout)src_layout,
                        dstImage, (VkImageLayout)dst_layout, regionCount, pRegions);
        LOGD("Host: vkCmdResolveImage called for command buffer %llu with source image %llu and destination image %llu",
             (unsigned long long)guest_cmd, (unsigned long long)guest_src, (unsigned long)guest_dst);
    }
    break;

    case FUNID_vkCmdResolveImage2:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);

        uint64_t resolve_ptr = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        VkResolveImageInfo2* pResolveImageInfo = NULL;
        if (resolve_ptr) {
            pResolveImageInfo = malloc(sizeof(VkResolveImageInfo2));
            decode_from_stream_VkResolveImageInfo2(VK_STRUCTURE_TYPE_MAX_ENUM,
                                                pResolveImageInfo, ptr);
        }

        VkCommandBuffer commandBuffer = (VkCommandBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd);

        vkCmdResolveImage2(commandBuffer, pResolveImageInfo);

        if (pResolveImageInfo) free(pResolveImageInfo);
        LOGD("Host: vkCmdResolveImage2 called for command buffer %llu", (unsigned long long)guest_cmd);
    }
    break;

    case FUNID_vkCmdSetBlendConstants:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);

        float* blendConstants = (float*)stream;

        VkCommandBuffer commandBuffer = (VkCommandBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd);

        vkCmdSetBlendConstants(commandBuffer, blendConstants);
        LOGD("Host: vkCmdSetBlendConstants called for command buffer %llu", (unsigned long long)guest_cmd);
    }
    break;

    case FUNID_vkCmdSetDepthBounds:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);

        float minDepthBounds, maxDepthBounds;
        memcpy(&minDepthBounds, *ptr, 4); *ptr += 4;
        memcpy(&maxDepthBounds, *ptr, 4); *ptr += 4;

        VkCommandBuffer commandBuffer = (VkCommandBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd);

        vkCmdSetDepthBounds(commandBuffer, minDepthBounds, maxDepthBounds);
        LOGD("Host: vkCmdSetDepthBounds called");
    }
    break;

    case FUNID_vkCmdSetCullMode:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd_buffer = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        VkCullModeFlags cullMode = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);

        VkCommandBuffer commandBuffer = (VkCommandBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd_buffer);

        vkCmdSetCullMode(commandBuffer, cullMode);
        LOGD("Host: vkCmdSetCullMode called for command buffer %llu with cull mode %u",
             (unsigned long long)guest_cmd_buffer, cullMode);
    }
    break;

    case FUNID_vkCmdSetDepthBiasEnable:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd_buffer = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        VkBool32 depthBiasEnable = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);

        VkCommandBuffer commandBuffer = (VkCommandBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd_buffer);

        vkCmdSetDepthBiasEnable(commandBuffer, depthBiasEnable);
        LOGD("Host: vkCmdSetDepthBiasEnable called for command buffer %llu with depth bias enable %u",
             (unsigned long long)guest_cmd_buffer, depthBiasEnable);
    }
    break;

    case FUNID_vkCmdSetDepthBoundsTestEnable:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd_buffer = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        VkBool32 depthBoundsTestEnable = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);

        VkCommandBuffer commandBuffer = (VkCommandBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd_buffer);

        vkCmdSetDepthBoundsTestEnable(commandBuffer, depthBoundsTestEnable);
    }
    break;

    case FUNID_vkCmdSetDepthTestEnable:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd_buffer = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        VkBool32 depthTestEnable = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);

        VkCommandBuffer commandBuffer = (VkCommandBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd_buffer);

        vkCmdSetDepthTestEnable(commandBuffer, depthTestEnable);
    }
    break;

    case FUNID_vkCmdSetDepthWriteEnable:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd_buffer = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        VkBool32 depthWriteEnable = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);

        VkCommandBuffer commandBuffer = (VkCommandBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd_buffer);

        vkCmdSetDepthWriteEnable(commandBuffer, depthWriteEnable);
    }
    break;

    case FUNID_vkCmdSetDeviceMask:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd_buffer = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint32_t deviceMask = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);

        VkCommandBuffer commandBuffer = (VkCommandBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd_buffer);

        vkCmdSetDeviceMask(commandBuffer, deviceMask);
    }
    break;

    case FUNID_vkCmdSetEvent2:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd_buffer = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint64_t guest_event = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint32_t has_dependency_info = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);

        VkCommandBuffer commandBuffer = (VkCommandBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd_buffer);
        VkEvent event = (VkEvent)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_EVENT, guest_event);

        VkDependencyInfo dependencyInfo;
        const VkDependencyInfo* pDependencyInfo = NULL;

        if (has_dependency_info) {
            decode_from_stream_VkDependencyInfo(VK_STRUCTURE_TYPE_DEPENDENCY_INFO, &dependencyInfo, ptr);
            pDependencyInfo = &dependencyInfo;
        }

        vkCmdSetEvent2(commandBuffer, event, pDependencyInfo);
        LOGD("Host: vkCmdSetEvent2 called for command buffer %llu with event %llu",
             (unsigned long long)guest_cmd_buffer, (unsigned long long)guest_event);
    }
    break;

    case FUNID_vkCmdSetFrontFace:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd_buffer = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        VkFrontFace frontFace = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);

        VkCommandBuffer commandBuffer = (VkCommandBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd_buffer);

        vkCmdSetFrontFace(commandBuffer, frontFace);
    }
    break;

    case FUNID_vkCmdSetLineWidth:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        float lineWidth = *(float*)(*ptr); *ptr += sizeof(uint32_t);

        VkCommandBuffer commandBuffer = (VkCommandBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd);

        vkCmdSetLineWidth(commandBuffer, lineWidth);
    }
    break;

    case FUNID_vkCmdSetPrimitiveRestartEnable:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        VkBool32 primitiveRestartEnable = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);

        VkCommandBuffer commandBuffer = (VkCommandBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd);

        vkCmdSetPrimitiveRestartEnable(commandBuffer, primitiveRestartEnable);
    }
    break;

    case FUNID_vkCmdSetPrimitiveTopology:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        VkPrimitiveTopology primitiveTopology = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);

        VkCommandBuffer commandBuffer = (VkCommandBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd);

        vkCmdSetPrimitiveTopology(commandBuffer, primitiveTopology);
    }
    break;

    case FUNID_vkCmdSetRasterizerDiscardEnable:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        VkBool32 rasterizerDiscardEnable = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);

        VkCommandBuffer commandBuffer = (VkCommandBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd);

        vkCmdSetRasterizerDiscardEnable(commandBuffer, rasterizerDiscardEnable);
    }
    break;

    case FUNID_vkCmdSetScissor:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint32_t firstScissor = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);
        uint32_t scissorCount = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);

        VkRect2D* pScissors = NULL;
        if (scissorCount > 0) {
            pScissors = (VkRect2D*)malloc(scissorCount * sizeof(VkRect2D));
            memcpy(pScissors, *ptr, scissorCount * sizeof(VkRect2D));
        }
        *ptr += scissorCount * sizeof(VkRect2D);

        uint64_t guest_cmd = *(uint64_t*)(*ptr);

        VkCommandBuffer commandBuffer = (VkCommandBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd);

        vkCmdSetScissor(commandBuffer, firstScissor, scissorCount, pScissors);

        if (pScissors) free(pScissors);
    }
    break;

    case FUNID_vkCmdSetScissorWithCount:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint32_t scissorCount = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);

        VkRect2D* pScissors = NULL;
        if (scissorCount > 0) {
            pScissors = (VkRect2D*)malloc(scissorCount * sizeof(VkRect2D));
            memcpy(pScissors, *ptr, scissorCount * sizeof(VkRect2D));
        }
        *ptr += scissorCount * sizeof(VkRect2D);

        uint64_t guest_cmd = *(uint64_t*)(*ptr);

        VkCommandBuffer commandBuffer = (VkCommandBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd);

        vkCmdSetScissorWithCount(commandBuffer, scissorCount, pScissors);

        if (pScissors) free(pScissors);
    }
    break;

    case FUNID_vkCmdSetStencilCompareMask:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        VkStencilFaceFlags faceMask = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);
        uint32_t compareMask = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);

        VkCommandBuffer commandBuffer = (VkCommandBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd);

        vkCmdSetStencilCompareMask(commandBuffer, faceMask, compareMask);
    }
    break;

    case FUNID_vkCmdSetStencilOp:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        VkStencilFaceFlags faceMask = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);
        VkStencilOp failOp = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);
        VkStencilOp passOp = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);
        VkStencilOp depthFailOp = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);
        VkCompareOp compareOp = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);

        VkCommandBuffer commandBuffer = (VkCommandBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd);

        vkCmdSetStencilOp(commandBuffer, faceMask, failOp, passOp, depthFailOp, compareOp);
    }
    break;

    case FUNID_vkCmdSetStencilReference:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        VkStencilFaceFlags face_mask = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);
        uint32_t reference = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);

        VkCommandBuffer commandBuffer = (VkCommandBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd);

        vkCmdSetStencilReference(commandBuffer, face_mask, reference);
        LOGD("Host: CmdSetStencilReference");
    }
    break;

    case FUNID_vkCmdSetStencilTestEnable:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        VkBool32 stencil_test_enable = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);

        VkCommandBuffer commandBuffer = (VkCommandBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd);

        vkCmdSetStencilTestEnable(commandBuffer, stencil_test_enable);
        LOGD("Host: CmdSetStencilTestEnable");
    }
    break;

    case FUNID_vkCmdSetStencilWriteMask:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        VkStencilFaceFlags face_mask = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);
        uint32_t write_mask = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);

        VkCommandBuffer commandBuffer = (VkCommandBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd);

        vkCmdSetStencilWriteMask(commandBuffer, face_mask, write_mask);
        LOGD("Host: CmdSetStencilWriteMask");
    }
    break;

    case FUNID_vkCmdSetViewport:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint32_t first_viewport = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);
        uint32_t viewport_count = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);

        VkCommandBuffer commandBuffer = (VkCommandBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd);

        VkViewport* viewports = (VkViewport*)malloc(viewport_count * sizeof(VkViewport));
        if (!viewports ||
            !copy_from_call_para_fast(all_para[1], viewports,
                                      viewport_count * sizeof(VkViewport))) {
            LOGE("Host: CmdSetViewport failed to get viewport array");
            if (viewports) free(viewports);
            break;
        }

        vkCmdSetViewport(commandBuffer, first_viewport, viewport_count, viewports);

        free(viewports);
        LOGD("Host: CmdSetViewport");
    }
    break;

    case FUNID_vkCmdSetViewportWithCount:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint32_t viewport_count = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);

        VkCommandBuffer commandBuffer = (VkCommandBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd);

        VkViewport* viewports = (VkViewport*)malloc(viewport_count * sizeof(VkViewport));
        if (!viewports ||
            !copy_from_call_para_fast(all_para[1], viewports,
                                      viewport_count * sizeof(VkViewport))) {
            LOGE("Host: CmdSetViewportWithCount failed to get viewport array");
            if (viewports) free(viewports);
            break;
        }

        vkCmdSetViewportWithCount(commandBuffer, viewport_count, viewports);

        free(viewports);
        LOGD("Host: CmdSetViewportWithCount");
    }
    break;

    case FUNID_vkCmdWaitEvents2:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint32_t event_count = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);

        VkCommandBuffer commandBuffer = (VkCommandBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd);

        uint64_t* guest_events = (uint64_t*)malloc(event_count * sizeof(uint64_t));
        if (!guest_events ||
            !copy_from_call_para_fast(all_para[1], guest_events,
                                      event_count * sizeof(uint64_t))) {
            LOGE("Host: CmdWaitEvents2 failed to get event handles");
            if (guest_events) free(guest_events);
            break;
        }

        VkEvent* events = (VkEvent*)malloc(event_count * sizeof(VkEvent));
        for (uint32_t i = 0; i < event_count; ++i) {
            events[i] = (VkEvent)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_EVENT, guest_events[i]);
        }

        VkDependencyInfo* dependency_infos = (VkDependencyInfo*)malloc(event_count * sizeof(VkDependencyInfo));
        for (uint32_t i = 0; i < event_count; ++i) {
            decode_from_stream_VkDependencyInfo(VK_STRUCTURE_TYPE_MAX_ENUM, &dependency_infos[i], ptr);
        }

        vkCmdWaitEvents2(commandBuffer, event_count, events, dependency_infos);

        free(guest_events);
        free(events);
        free(dependency_infos);
        LOGD("Host: CmdWaitEvents2");
    }
    break;

    case FUNID_vkCmdWriteTimestamp2:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        VkPipelineStageFlags2 stage = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint64_t guest_query_pool = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint32_t query = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);

        VkCommandBuffer commandBuffer = (VkCommandBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd);
        VkQueryPool queryPool = (VkQueryPool)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_QUERY_POOL, guest_query_pool);

        vkCmdWriteTimestamp2(commandBuffer, stage, queryPool, query);
        LOGD("Host: CmdWriteTimestamp2");
    }
    break;

    case FUNID_vkCreateComputePipelines:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_device, guest_cache, guest_allocator;
        uint32_t createInfoCount;
        uint8_t has_allocator;

        memcpy(&guest_device, *ptr, sizeof(uint64_t)); *ptr += sizeof(uint64_t);
        memcpy(&guest_cache, *ptr, sizeof(uint64_t)); *ptr += sizeof(uint64_t);
        memcpy(&createInfoCount, *ptr, sizeof(uint32_t)); *ptr += sizeof(uint32_t);

        VkDevice device = (VkDevice)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_device);
        VkPipelineCache pipelineCache = guest_cache ? (VkPipelineCache)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_PIPELINE_CACHE, guest_cache) : VK_NULL_HANDLE;

        VkComputePipelineCreateInfo* pCreateInfos = (VkComputePipelineCreateInfo*)malloc(sizeof(VkComputePipelineCreateInfo) * createInfoCount);

        for (uint32_t i = 0; i < createInfoCount; ++i) {
            decode_from_stream_VkComputePipelineCreateInfo(VK_STRUCTURE_TYPE_MAX_ENUM, &pCreateInfos[i], ptr);
        }

        memcpy(&guest_allocator, *ptr, sizeof(uint64_t)); *ptr += sizeof(uint64_t);
        memcpy(&has_allocator, *ptr, sizeof(uint8_t)); *ptr += sizeof(uint8_t);

        VkAllocationCallbacks* pAllocator = NULL;
        VkAllocationCallbacks allocator_copy;
        if (has_allocator) {
            decode_from_stream_VkAllocationCallbacks(VK_STRUCTURE_TYPE_MAX_ENUM, &allocator_copy, ptr);
            pAllocator = &allocator_copy;
        }

        VkPipeline* pPipelines = (VkPipeline*)malloc(sizeof(VkPipeline) * createInfoCount);

        VkResult result = vkCreateComputePipelines(device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);

        if (result == VK_SUCCESS) {
            uint64_t* guest_pipelines = (uint64_t*)malloc(createInfoCount * sizeof(uint64_t));
            if (!guest_pipelines ||
                !copy_from_call_para_fast(all_para[1], guest_pipelines,
                                          createInfoCount * sizeof(uint64_t))) {
                free(pCreateInfos);
                free(pPipelines);
                LOGE("Host: vkCreateComputePipelines failed to get guest pipelines");
                if (guest_pipelines) free(guest_pipelines);
                break;
            }

            for (uint32_t i = 0; i < createInfoCount; ++i) {
                insert_mapping(EXPRESS_VK_OBJECT_TYPE_PIPELINE, guest_pipelines[i], (uint64_t)(uintptr_t)pPipelines[i]);
            }
            free(guest_pipelines);
        } else {
            for (uint32_t i = 0; i < createInfoCount; ++i) {
                pPipelines[i] = VK_NULL_HANDLE;
            }
            LOGE("vkCreateComputePipelines failed with error %d", result);
        }

        free(pCreateInfos);
        free(pPipelines);
        LOGD("vkCreateComputePipelines result %d", result);
    }
    break;

    case FUNID_vkCreateDescriptorUpdateTemplate:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_device, guest_allocator;
        uint8_t has_allocator;

        memcpy(&guest_device, *ptr, sizeof(uint64_t)); *ptr += sizeof(uint64_t);

        VkDevice device = (VkDevice)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_device);

        VkDescriptorUpdateTemplateCreateInfo createInfo;
        decode_from_stream_VkDescriptorUpdateTemplateCreateInfo(VK_STRUCTURE_TYPE_MAX_ENUM, &createInfo, ptr);

        memcpy(&guest_allocator, *ptr, sizeof(uint64_t)); *ptr += sizeof(uint64_t);
        memcpy(&has_allocator, *ptr, sizeof(uint8_t)); *ptr += sizeof(uint8_t);

        VkAllocationCallbacks* pAllocator = NULL;
        VkAllocationCallbacks allocator_copy;
        if (has_allocator) {
            decode_from_stream_VkAllocationCallbacks(VK_STRUCTURE_TYPE_MAX_ENUM, &allocator_copy, ptr);
            pAllocator = &allocator_copy;
        }

        VkDescriptorUpdateTemplate template;
        VkResult result = vkCreateDescriptorUpdateTemplate(device, &createInfo, pAllocator, &template);
        if (para_num > 2) {
            write_to_guest_mem(all_para[2].data, &result, 0,
                               sizeof(result));
        }

        if (result == VK_SUCCESS) {
            uint64_t guest_template;
            if (!copy_from_call_para_fast(all_para[1], &guest_template, sizeof(uint64_t))) {
                LOGE("Host: vkCreateDescriptorUpdateTemplate failed to read guest handle");
                break;
            }
            insert_mapping(EXPRESS_VK_OBJECT_TYPE_DESCRIPTOR_UPDATE_TEMPLATE, guest_template, (uint64_t)(uintptr_t)template);
        } else {
            template = VK_NULL_HANDLE;
            LOGE("vkCreateDescriptorUpdateTemplate failed with error %d", result);
        }
        LOGD("vkCreateDescriptorUpdateTemplate result %d", result);
    }
    break;

    case FUNID_vkCreatePrivateDataSlot:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_device, guest_allocator;
        uint8_t has_allocator;

        memcpy(&guest_device, *ptr, sizeof(uint64_t)); *ptr += sizeof(uint64_t);

        VkDevice device = (VkDevice)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_device);

        VkPrivateDataSlotCreateInfo createInfo;
        decode_from_stream_VkPrivateDataSlotCreateInfo(VK_STRUCTURE_TYPE_MAX_ENUM, &createInfo, ptr);

        memcpy(&guest_allocator, *ptr, sizeof(uint64_t)); *ptr += sizeof(uint64_t);
        memcpy(&has_allocator, *ptr, sizeof(uint8_t)); *ptr += sizeof(uint8_t);

        VkAllocationCallbacks* pAllocator = NULL;
        VkAllocationCallbacks allocator_copy;
        if (has_allocator) {
            decode_from_stream_VkAllocationCallbacks(VK_STRUCTURE_TYPE_MAX_ENUM, &allocator_copy, ptr);
            pAllocator = &allocator_copy;
        }

        VkPrivateDataSlot slot;
        VkResult result = vkCreatePrivateDataSlot(device, &createInfo, pAllocator, &slot);

        if (result == VK_SUCCESS) {
            uint64_t guest_slot;
            if (!copy_from_call_para_fast(all_para[1], &guest_slot, sizeof(uint64_t))) {
                LOGE("Host: vkCreatePrivateDataSlot failed to read guest handle");
                break;
            }
            insert_mapping(EXPRESS_VK_OBJECT_TYPE_PRIVATE_DATA_SLOT, guest_slot, (uint64_t)(uintptr_t)slot);
        } else {
            slot = VK_NULL_HANDLE;
            LOGE("vkCreatePrivateDataSlot failed with error %d", result);
        }

        LOGD("vkCreatePrivateDataSlot result %d", result);
    }
    break;

    case FUNID_vkCreateRenderPass2:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_device, guest_allocator;
        uint8_t has_allocator;

        memcpy(&guest_device, *ptr, sizeof(uint64_t)); *ptr += sizeof(uint64_t);

        VkDevice device = (VkDevice)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_device);

        VkRenderPassCreateInfo2 createInfo;
        decode_from_stream_VkRenderPassCreateInfo2(VK_STRUCTURE_TYPE_MAX_ENUM, &createInfo, ptr);

        memcpy(&guest_allocator, *ptr, sizeof(uint64_t)); *ptr += sizeof(uint64_t);
        memcpy(&has_allocator, *ptr, sizeof(uint8_t)); *ptr += sizeof(uint8_t);

        VkAllocationCallbacks* pAllocator = NULL;
        VkAllocationCallbacks allocator_copy;
        if (has_allocator) {
            decode_from_stream_VkAllocationCallbacks(VK_STRUCTURE_TYPE_MAX_ENUM, &allocator_copy, ptr);
            pAllocator = &allocator_copy;
        }

        VkRenderPass renderPass;
        VkResult result = vkCreateRenderPass2(device, &createInfo, pAllocator, &renderPass);

        if (result == VK_SUCCESS) {
            uint64_t guest_renderpass;
            if (!copy_from_call_para_fast(all_para[1], &guest_renderpass, sizeof(uint64_t))) {
                LOGE("Host: vkCreateRenderPass2 failed to read guest handle");
                break;
            }
            insert_mapping(EXPRESS_VK_OBJECT_TYPE_RENDER_PASS, guest_renderpass, (uint64_t)(uintptr_t)renderPass);
        } else {
            renderPass = VK_NULL_HANDLE;
            LOGE("vkCreateRenderPass2 failed with error %d", result);
        }
        LOGD("vkCreateRenderPass2 result %d", result);
    }
    break;

    case FUNID_vkCreateSamplerYcbcrConversion:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_device, guest_allocator;
        uint8_t has_allocator;

        memcpy(&guest_device, *ptr, sizeof(uint64_t)); *ptr += sizeof(uint64_t);

        VkDevice device = (VkDevice)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_device);

        VkSamplerYcbcrConversionCreateInfo createInfo;
        decode_from_stream_VkSamplerYcbcrConversionCreateInfo(VK_STRUCTURE_TYPE_MAX_ENUM, &createInfo, ptr);

        memcpy(&guest_allocator, *ptr, sizeof(uint64_t)); *ptr += sizeof(uint64_t);
        memcpy(&has_allocator, *ptr, sizeof(uint8_t)); *ptr += sizeof(uint8_t);

        VkAllocationCallbacks* pAllocator = NULL;
        VkAllocationCallbacks allocator_copy;
        if (has_allocator) {
            decode_from_stream_VkAllocationCallbacks(VK_STRUCTURE_TYPE_MAX_ENUM, &allocator_copy, ptr);
            pAllocator = &allocator_copy;
        }

        VkSamplerYcbcrConversion conversion;
        VkResult result = vkCreateSamplerYcbcrConversion(device, &createInfo, pAllocator, &conversion);

        if (result == VK_SUCCESS) {
            uint64_t guest_conversion;
            if (!copy_from_call_para_fast(all_para[1], &guest_conversion, sizeof(uint64_t))) {
                LOGE("Host: vkCreateSamplerYcbcrConversion failed to read guest handle");
                break;
            }
            insert_mapping(EXPRESS_VK_OBJECT_TYPE_SAMPLER_YCBCR_CONVERSION, guest_conversion, (uint64_t)(uintptr_t)conversion);
        } else {
            conversion = VK_NULL_HANDLE;
            LOGE("vkCreateSamplerYcbcrConversion failed with error %d", result);
        }

        LOGD("vkCreateSamplerYcbcrConversion result %d", result);
    }
    break;

    case FUNID_vkDeviceWaitIdle:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);

        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_device = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        VkDevice device = (VkDevice)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_device);

        struct timespec t0_dwide, t1_dwide;
        clock_gettime(CLOCK_MONOTONIC, &t0_dwide);
        VkResult result = vkDeviceWaitIdle(device);
        clock_gettime(CLOCK_MONOTONIC, &t1_dwide);
        double dwide_ms = (t1_dwide.tv_sec - t0_dwide.tv_sec) * 1000.0 + (t1_dwide.tv_nsec - t0_dwide.tv_nsec) / 1000000.0;
        LOGD("[GPU_TIME] vkDeviceWaitIdle cost: %.3f ms", dwide_ms);
        if (result != VK_SUCCESS) {
            LOGE("vkDeviceWaitIdle failed with error %d", result);
        } else {
            LOGD("vkDeviceWaitIdle completed successfully");
        }
        LOGD("Host: vkDeviceWaitIdle result %d", result);
    }
    break;

    case FUNID_vkCmdSetDepthBias:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);

        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd_buffer = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        float depthBiasConstantFactor = *(float*)(*ptr); *ptr += sizeof(float);
        float depthBiasClamp = *(float*)(*ptr); *ptr += sizeof(float);
        float depthBiasSlopeFactor = *(float*)(*ptr); *ptr += sizeof(float);

        VkCommandBuffer commandBuffer = (VkCommandBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd_buffer);

        vkCmdSetDepthBias(commandBuffer, depthBiasConstantFactor, depthBiasClamp, depthBiasSlopeFactor);
        LOGD("Host: vkCmdSetDepthBias executed");
    }
    break;

    case FUNID_vkCmdCopyImageToBuffer:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);

        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd_buffer = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint64_t guest_src_image = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        VkImageLayout srcImageLayout = *(VkImageLayout*)(*ptr); *ptr += sizeof(VkImageLayout);
        uint64_t guest_dst_buffer = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint32_t regionCount = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);

        VkCommandBuffer commandBuffer = (VkCommandBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd_buffer);
        VkImage srcImage = (VkImage)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_IMAGE, guest_src_image);
        VkBuffer dstBuffer = (VkBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_BUFFER, guest_dst_buffer);

        VkBufferImageCopy* pRegions = (VkBufferImageCopy*)malloc(sizeof(VkBufferImageCopy) * regionCount);
        memcpy(pRegions, *ptr, sizeof(VkBufferImageCopy) * regionCount);

        vkCmdCopyImageToBuffer(commandBuffer, srcImage, srcImageLayout, dstBuffer, regionCount, pRegions);

        free(pRegions);
        LOGD("Host: vkCmdCopyImageToBuffer executed with %d regions", regionCount);
    }
    break;

    case FUNID_vkCmdCopyBuffer:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);

        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd_buffer = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint64_t guest_src_buffer = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint64_t guest_dst_buffer = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint32_t regionCount = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);

        VkCommandBuffer commandBuffer = (VkCommandBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd_buffer);
        VkBuffer srcBuffer = (VkBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_BUFFER, guest_src_buffer);
        VkBuffer dstBuffer = (VkBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_BUFFER, guest_dst_buffer);

        VkBufferCopy* pRegions = (VkBufferCopy*)malloc(sizeof(VkBufferCopy) * regionCount);
        memcpy(pRegions, *ptr, sizeof(VkBufferCopy) * regionCount);

        vkCmdCopyBuffer(commandBuffer, srcBuffer, dstBuffer, regionCount, pRegions);

        free(pRegions);
        LOGD("Host: vkCmdCopyBuffer executed with %d regions", regionCount);
    }
    break;

    case FUNID_vkCmdSetDepthCompareOp:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd = *(uint64_t*)(*ptr); *ptr += sizeof(uint64_t);
        uint32_t compare_op = *(uint32_t*)(*ptr); *ptr += sizeof(uint32_t);

        VkCommandBuffer commandBuffer = (VkCommandBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd);

        vkCmdSetDepthCompareOp(commandBuffer, (VkCompareOp)compare_op);

        LOGD("Host: CmdSetDepthCompareOp complete");
    }
    break;

    case FUNID_vkGetBufferDeviceAddress:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_device = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        VkBufferDeviceAddressInfo info;
        decode_from_stream_VkBufferDeviceAddressInfo(VK_STRUCTURE_TYPE_MAX_ENUM, &info, ptr);

        VkDevice device = (VkDevice)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_device);

        VkDeviceAddress result = vkGetBufferDeviceAddress(device, &info);
        write_to_guest_mem(all_para[1].data, &result, 0, sizeof(VkDeviceAddress));

        LOGD("GetBufferDeviceAddress completed");
    }
    break;

    case FUNID_vkGetBufferOpaqueCaptureAddress:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_device = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        VkBufferDeviceAddressInfo info;
        decode_from_stream_VkBufferDeviceAddressInfo(VK_STRUCTURE_TYPE_MAX_ENUM, &info, ptr);

        VkDevice device = (VkDevice)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_device);

        uint64_t result = vkGetBufferOpaqueCaptureAddress(device, &info);
        write_to_guest_mem(all_para[1].data, &result, 0, sizeof(uint64_t));

        LOGD("GetBufferOpaqueCaptureAddress completed");
    }
    break;

    case FUNID_vkGetDeviceMemoryOpaqueCaptureAddress:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_device = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        VkDeviceMemoryOpaqueCaptureAddressInfo info;
        decode_from_stream_VkDeviceMemoryOpaqueCaptureAddressInfo(VK_STRUCTURE_TYPE_MAX_ENUM, &info, ptr);

        VkDevice device = (VkDevice)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_device);

        uint64_t result = vkGetDeviceMemoryOpaqueCaptureAddress(device, &info);
        write_to_guest_mem(all_para[1].data, &result, 0, sizeof(uint64_t));

        LOGD("GetDeviceMemoryOpaqueCaptureAddress completed");
    }
    break;

    case FUNID_vkGetDescriptorSetLayoutSupport:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_device = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        VkDescriptorSetLayoutCreateInfo createInfo;
        decode_from_stream_VkDescriptorSetLayoutCreateInfo(VK_STRUCTURE_TYPE_MAX_ENUM, &createInfo, ptr);

        VkDescriptorSetLayoutSupport support;
        decode_from_stream_VkDescriptorSetLayoutSupport(VK_STRUCTURE_TYPE_MAX_ENUM, &support, ptr);

        VkDevice device = (VkDevice)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_device);

        vkGetDescriptorSetLayoutSupport(device, &createInfo, &support);
        write_to_guest_mem(all_para[1].data, &support, 0, sizeof(VkDescriptorSetLayoutSupport));

        LOGD("GetDescriptorSetLayoutSupport completed");
    }
    break;

    case FUNID_vkGetDeviceBufferMemoryRequirements:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_device = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        VkDeviceBufferMemoryRequirements info;
        decode_from_stream_VkDeviceBufferMemoryRequirements(VK_STRUCTURE_TYPE_MAX_ENUM, &info, ptr);

        VkMemoryRequirements2 memReq;
        decode_from_stream_VkMemoryRequirements2(VK_STRUCTURE_TYPE_MAX_ENUM, &memReq, ptr);

        VkDevice device = (VkDevice)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_device);

        vkGetDeviceBufferMemoryRequirements(device, &info, &memReq);
        write_to_guest_mem(all_para[1].data, &memReq, 0, sizeof(VkMemoryRequirements2));

        LOGD("GetDeviceBufferMemoryRequirements completed");
    }
    break;

    case FUNID_vkGetDeviceImageMemoryRequirements:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_device = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        VkDeviceImageMemoryRequirements info;
        decode_from_stream_VkDeviceImageMemoryRequirements(VK_STRUCTURE_TYPE_MAX_ENUM, &info, ptr);

        VkMemoryRequirements2 memReq;
        decode_from_stream_VkMemoryRequirements2(VK_STRUCTURE_TYPE_MAX_ENUM, &memReq, ptr);

        VkDevice device = (VkDevice)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_device);

        vkGetDeviceImageMemoryRequirements(device, &info, &memReq);
        write_to_guest_mem(all_para[1].data, &memReq, 0, sizeof(VkMemoryRequirements2));

        LOGD("GetDeviceImageMemoryRequirements completed");
    }
    break;

    case FUNID_vkGetDeviceImageSparseMemoryRequirements:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_device = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        VkDeviceImageMemoryRequirements info;
        decode_from_stream_VkDeviceImageMemoryRequirements(VK_STRUCTURE_TYPE_MAX_ENUM, &info, ptr);

        uint32_t count = 0;
        if (!copy_from_call_para_fast(all_para[1], &count, sizeof(uint32_t))) {
            LOGE("Host: GetDeviceImageSparseMemoryRequirements failed to read count");
            break;
        }

        VkDevice device = (VkDevice)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_device);

        if (count == 0) {
            vkGetDeviceImageSparseMemoryRequirements(device, &info, &count, NULL);
            write_to_guest_mem(all_para[1].data, &count, 0, sizeof(uint32_t));
        } else {
            VkSparseImageMemoryRequirements2* reqs = malloc(count * sizeof(VkSparseImageMemoryRequirements2));
            vkGetDeviceImageSparseMemoryRequirements(device, &info, &count, reqs);
            // write_to_guest_mem(all_para[1].data, &count, 0, sizeof(uint32_t));
            write_to_guest_mem(all_para[2].data, reqs, 0, count * sizeof(VkSparseImageMemoryRequirements2));
            free(reqs);
        }

        LOGD("GetDeviceImageSparseMemoryRequirements completed");
    }
    break;

    case FUNID_vkGetDeviceGroupPeerMemoryFeatures:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_device = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        uint32_t heapIndex = *(uint32_t*)(*ptr);
        *ptr += sizeof(uint32_t);
        uint32_t localDeviceIndex = *(uint32_t*)(*ptr);
        *ptr += sizeof(uint32_t);
        uint32_t remoteDeviceIndex = *(uint32_t*)(*ptr);
        *ptr += sizeof(uint32_t);

        VkDevice device = (VkDevice)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_device);

        VkPeerMemoryFeatureFlags flags;
        vkGetDeviceGroupPeerMemoryFeatures(device, heapIndex, localDeviceIndex, remoteDeviceIndex, &flags);
        write_to_guest_mem(all_para[1].data, &flags, 0, sizeof(VkPeerMemoryFeatureFlags));

        LOGD("GetDeviceGroupPeerMemoryFeatures completed");
    }
    break;

    case FUNID_vkEnumeratePhysicalDeviceGroups:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_instance = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        VkInstance instance = (VkInstance)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_INSTANCE, guest_instance);

        uint32_t count = 0;
        if (!copy_from_call_para_fast(all_para[1], &count, sizeof(uint32_t))) {
            LOGE("Host: EnumeratePhysicalDeviceGroups failed to read count");
            break;
        }

        VkResult result;
        if (count == 0) {
            result = vkEnumeratePhysicalDeviceGroups(instance, &count, NULL);
            write_to_guest_mem(all_para[1].data, &count, 0, sizeof(uint32_t));
        } else {
            VkPhysicalDeviceGroupProperties* properties = (VkPhysicalDeviceGroupProperties*)malloc(count * sizeof(VkPhysicalDeviceGroupProperties));
            if (!properties) {
                result = VK_ERROR_OUT_OF_HOST_MEMORY;
            } else {
                result = vkEnumeratePhysicalDeviceGroups(instance, &count, properties);
                write_to_guest_mem(all_para[2].data, properties, 0, count * sizeof(VkPhysicalDeviceGroupProperties));
                free(properties);
            }
        }

        write_to_guest_mem(all_para[3].data, &result, 0, sizeof(VkResult));
        LOGD("EnumeratePhysicalDeviceGroups completed");
    }
    break;

    case FUNID_vkGetPhysicalDeviceExternalBufferProperties:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_device = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        VkPhysicalDevice physicalDevice = (VkPhysicalDevice)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_PHYSICAL_DEVICE, guest_device);

        VkPhysicalDeviceExternalBufferInfo* pExternalBufferInfo = (VkPhysicalDeviceExternalBufferInfo*)malloc(sizeof(VkPhysicalDeviceExternalBufferInfo));
        decode_from_stream_VkPhysicalDeviceExternalBufferInfo(VK_STRUCTURE_TYPE_MAX_ENUM, pExternalBufferInfo, ptr);

        VkExternalBufferProperties properties;
        vkGetPhysicalDeviceExternalBufferProperties(physicalDevice, pExternalBufferInfo, &properties);

        write_to_guest_mem(all_para[1].data, &properties, 0, sizeof(VkExternalBufferProperties));
        free(pExternalBufferInfo);
        LOGD("GetPhysicalDeviceExternalBufferProperties completed");
    }
    break;

    case FUNID_vkGetPhysicalDeviceExternalFenceProperties:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_device = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        VkPhysicalDevice physicalDevice = (VkPhysicalDevice)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_PHYSICAL_DEVICE, guest_device);

        VkPhysicalDeviceExternalFenceInfo* pExternalFenceInfo = (VkPhysicalDeviceExternalFenceInfo*)malloc(sizeof(VkPhysicalDeviceExternalFenceInfo));
        decode_from_stream_VkPhysicalDeviceExternalFenceInfo(VK_STRUCTURE_TYPE_MAX_ENUM, pExternalFenceInfo, ptr);

        VkExternalFenceProperties properties;
        vkGetPhysicalDeviceExternalFenceProperties(physicalDevice, pExternalFenceInfo, &properties);

        write_to_guest_mem(all_para[1].data, &properties, 0, sizeof(VkExternalFenceProperties));
        free(pExternalFenceInfo);
        LOGD("GetPhysicalDeviceExternalFenceProperties completed");
    }
    break;

    case FUNID_vkGetPhysicalDeviceExternalSemaphoreProperties:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_device = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        VkPhysicalDevice physicalDevice = (VkPhysicalDevice)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_PHYSICAL_DEVICE, guest_device);

        VkPhysicalDeviceExternalSemaphoreInfo* pExternalSemaphoreInfo = (VkPhysicalDeviceExternalSemaphoreInfo*)malloc(sizeof(VkPhysicalDeviceExternalSemaphoreInfo));
        decode_from_stream_VkPhysicalDeviceExternalSemaphoreInfo(VK_STRUCTURE_TYPE_MAX_ENUM, pExternalSemaphoreInfo, ptr);

        VkExternalSemaphoreProperties properties;
        vkGetPhysicalDeviceExternalSemaphoreProperties(physicalDevice, pExternalSemaphoreInfo, &properties);

        write_to_guest_mem(all_para[1].data, &properties, 0, sizeof(VkExternalSemaphoreProperties));
        free(pExternalSemaphoreInfo);
        LOGD("GetPhysicalDeviceExternalSemaphoreProperties completed");
    }
    break;

    case FUNID_vkGetImageSparseMemoryRequirements:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_device = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        uint64_t guest_image = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        VkDevice device = (VkDevice)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_device);
        VkImage image = (VkImage)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_IMAGE, guest_image);

        uint32_t count = 0;
        if (!copy_from_call_para_fast(all_para[1], &count, sizeof(uint32_t))) {
            LOGE("Host: GetImageSparseMemoryRequirements failed to read count");
            break;
        }

        if (count == 0) {
            vkGetImageSparseMemoryRequirements(device, image, &count, NULL);
            write_to_guest_mem(all_para[1].data, &count, 0, sizeof(uint32_t));
        } else {
            VkSparseImageMemoryRequirements* requirements = (VkSparseImageMemoryRequirements*)malloc(count * sizeof(VkSparseImageMemoryRequirements));
            if (requirements) {
                vkGetImageSparseMemoryRequirements(device, image, &count, requirements);
                write_to_guest_mem(all_para[2].data, requirements, 0, count * sizeof(VkSparseImageMemoryRequirements));
                free(requirements);
            }
        }
        LOGD("GetImageSparseMemoryRequirements completed");
    }
    break;

    case FUNID_vkGetImageSparseMemoryRequirements2:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_device = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        VkDevice device = (VkDevice)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_device);

        VkImageSparseMemoryRequirementsInfo2* pInfo = (VkImageSparseMemoryRequirementsInfo2*)malloc(sizeof(VkImageSparseMemoryRequirementsInfo2));
        decode_from_stream_VkImageSparseMemoryRequirementsInfo2(VK_STRUCTURE_TYPE_MAX_ENUM, pInfo, ptr);

        uint32_t count = 0;
        if (!copy_from_call_para_fast(all_para[1], &count, sizeof(uint32_t))) {
            LOGE("Host: GetImageSparseMemoryRequirements2 failed to read count");
            break;
        }

        if (count == 0) {
            vkGetImageSparseMemoryRequirements2(device, pInfo, &count, NULL);
            LOGD("GetImageSparseMemoryRequirements2 count=%u", count);
            write_to_guest_mem(all_para[1].data, &count, 0, sizeof(uint32_t));
        } else {
            VkSparseImageMemoryRequirements2* requirements = (VkSparseImageMemoryRequirements2*)malloc(count * sizeof(VkSparseImageMemoryRequirements2));
            if (requirements) {
                vkGetImageSparseMemoryRequirements2(device, pInfo, &count, requirements);
                LOGD("GetImageSparseMemoryRequirements2 fetched %u requirements", count);
                write_to_guest_mem(all_para[2].data, requirements, 0, count * sizeof(VkSparseImageMemoryRequirements2));
                free(requirements);
            }
        }
        free(pInfo);
        LOGD("GetImageSparseMemoryRequirements2 completed");
    }
    break;

    case FUNID_vkGetPhysicalDeviceFeatures2:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_device = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        VkPhysicalDevice physicalDevice = (VkPhysicalDevice)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_PHYSICAL_DEVICE, guest_device);

        VkPhysicalDeviceFeatures2 features;
        vkGetPhysicalDeviceFeatures2(physicalDevice, &features);

        write_to_guest_mem(all_para[1].data, &features, 0, sizeof(VkPhysicalDeviceFeatures2));
        LOGD("GetPhysicalDeviceFeatures2 completed");
    }
    break;

    case FUNID_vkGetPhysicalDeviceSparseImageFormatProperties:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_device = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        VkFormat format = *(VkFormat*)(*ptr); *ptr += sizeof(VkFormat);
        VkImageType type = *(VkImageType*)(*ptr); *ptr += sizeof(VkImageType);
        VkSampleCountFlagBits samples = *(VkSampleCountFlagBits*)(*ptr); *ptr += sizeof(VkSampleCountFlagBits);
        VkImageUsageFlags usage = *(VkImageUsageFlags*)(*ptr); *ptr += sizeof(VkImageUsageFlags);
        VkImageTiling tiling = *(VkImageTiling*)(*ptr); *ptr += sizeof(VkImageTiling);

        VkPhysicalDevice physicalDevice = (VkPhysicalDevice)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_PHYSICAL_DEVICE, guest_device);

        uint32_t count = 0;
        if (!copy_from_call_para_fast(all_para[1], &count, sizeof(uint32_t))) {
            LOGE("Host: GetPhysicalDeviceSparseImageFormatProperties failed to read count");
            break;
        }

        if (count == 0) {
            vkGetPhysicalDeviceSparseImageFormatProperties(physicalDevice, format, type, samples, usage, tiling, &count, NULL);
            write_to_guest_mem(all_para[1].data, &count, 0, sizeof(uint32_t));
        } else {
            VkSparseImageFormatProperties* properties = (VkSparseImageFormatProperties*)malloc(count * sizeof(VkSparseImageFormatProperties));
            if (properties) {
                vkGetPhysicalDeviceSparseImageFormatProperties(physicalDevice, format, type, samples, usage, tiling, &count, properties);
                write_to_guest_mem(all_para[2].data, properties, 0, count * sizeof(VkSparseImageFormatProperties));
                free(properties);
            }
        }
        LOGD("GetPhysicalDeviceSparseImageFormatProperties completed");
    }
    break;

    case FUNID_vkGetPhysicalDeviceSparseImageFormatProperties2:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_device = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        VkPhysicalDevice physicalDevice = (VkPhysicalDevice)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_PHYSICAL_DEVICE, guest_device);

        VkPhysicalDeviceSparseImageFormatInfo2* pFormatInfo = (VkPhysicalDeviceSparseImageFormatInfo2*)malloc(sizeof(VkPhysicalDeviceSparseImageFormatInfo2));
        decode_from_stream_VkPhysicalDeviceSparseImageFormatInfo2(VK_STRUCTURE_TYPE_MAX_ENUM, pFormatInfo, ptr);

        uint32_t count = 0;
        if (!copy_from_call_para_fast(all_para[1], &count, sizeof(uint32_t))) {
            LOGE("Host: GetPhysicalDeviceSparseImageFormatProperties2 failed to read count");
            break;
        }

        if (count == 0) {
            vkGetPhysicalDeviceSparseImageFormatProperties2(physicalDevice, pFormatInfo, &count, NULL);
            write_to_guest_mem(all_para[1].data, &count, 0, sizeof(uint32_t));
        } else {
            VkSparseImageFormatProperties2* properties = (VkSparseImageFormatProperties2*)malloc(count * sizeof(VkSparseImageFormatProperties2));
            for(int i = 0; i < count; i++) {
                properties[i].sType = VK_STRUCTURE_TYPE_SPARSE_IMAGE_FORMAT_PROPERTIES_2;
                properties[i].pNext = NULL;
            }
            if (properties) {
                vkGetPhysicalDeviceSparseImageFormatProperties2(physicalDevice, pFormatInfo, &count, properties);
                write_to_guest_mem(all_para[2].data, properties, 0, count * sizeof(VkSparseImageFormatProperties2));
                free(properties);
            }
        }
        free(pFormatInfo);
        LOGD("GetPhysicalDeviceSparseImageFormatProperties2 completed");
    }
    break;

    case FUNID_vkGetPhysicalDeviceToolProperties:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_physical_device = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        VkPhysicalDevice physicalDevice = (VkPhysicalDevice)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_PHYSICAL_DEVICE, guest_physical_device);

        uint32_t count = 0;
        if (!copy_from_call_para_fast(all_para[1], &count, sizeof(uint32_t))) {
            LOGE("Host: GetPhysicalDeviceToolProperties failed to read count");
            break;
        }

        VkResult result;
        if (count == 0) {
            result = vkGetPhysicalDeviceToolProperties(physicalDevice, &count, NULL);
            write_to_guest_mem(all_para[1].data, &count, 0, sizeof(uint32_t));
        } else {
            VkPhysicalDeviceToolProperties* properties =
                (VkPhysicalDeviceToolProperties*)malloc(count * sizeof(VkPhysicalDeviceToolProperties));
            result = vkGetPhysicalDeviceToolProperties(physicalDevice, &count, properties);
            write_to_guest_mem(all_para[2].data, properties, 0, count * sizeof(VkPhysicalDeviceToolProperties));
            free(properties);
        }

        write_to_guest_mem(all_para[3].data, &result, 0, sizeof(VkResult));
        LOGD("GetPhysicalDeviceToolProperties result: %d", result);
    }
    break;

    case FUNID_vkGetRenderAreaGranularity:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_device = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        uint64_t guest_render_pass = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        VkDevice device = (VkDevice)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_device);
        VkRenderPass renderPass = (VkRenderPass)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_RENDER_PASS, guest_render_pass);

        VkExtent2D granularity;
        vkGetRenderAreaGranularity(device, renderPass, &granularity);

        write_to_guest_mem(all_para[1].data, &granularity, 0, sizeof(VkExtent2D));
        LOGD("GetRenderAreaGranularity complete");
    }
    break;

    case FUNID_vkGetSemaphoreCounterValue:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_device = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        uint64_t guest_semaphore = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        VkDevice device = (VkDevice)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_device);
        VkSemaphore semaphore = (VkSemaphore)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_SEMAPHORE, guest_semaphore);

        uint64_t value;
        VkResult result = vkGetSemaphoreCounterValue(device, semaphore, &value);

        write_to_guest_mem(all_para[1].data, &value, 0, sizeof(uint64_t));
        write_to_guest_mem(all_para[2].data, &result, 0, sizeof(VkResult));
        LOGD("GetSemaphoreCounterValue result: %d", result);
    }
    break;

    case FUNID_vkQueueSubmit2:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        uint8_t *stream = NULL;
        uint8_t *stream_base = NULL;
        uint8_t *stream_end = NULL;
        uint8_t *cursor = NULL;
        uint64_t guest_queue = 0;
        uint64_t guest_fence = 0;
        uint32_t submitCount = 0;
        VkQueue queue = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkSubmitInfo2 *pSubmits = NULL;
        char *data_ptr = NULL;
        int invalid_submit = 0;

        if (para_num < 1) {
            invalid_submit = 1;
            LOGE("Host: vkQueueSubmit2 rejected call without an input stream");
        } else if (all_para[0].data_len > (size_t)G_MAXSSIZE) {
            invalid_submit = 1;
            LOGE("Host: vkQueueSubmit2 rejected an address-sized input overflow");
        } else {
            stream = call_para_to_ptr(all_para[0], &need_free);
            stream_base = stream;
            if (stream == NULL) {
                invalid_submit = 1;
                LOGE("Host: vkQueueSubmit2 rejected an unavailable input stream");
            } else {
                stream_end = stream + all_para[0].data_len;
                cursor = stream;
                if (!express_vk_submit2_take(
                        &cursor, stream_end, &guest_queue,
                        sizeof(guest_queue)) ||
                    !express_vk_submit2_take(
                        &cursor, stream_end, &guest_fence,
                        sizeof(guest_fence)) ||
                    !express_vk_submit2_take(
                        &cursor, stream_end, &submitCount,
                        sizeof(submitCount))) {
                    invalid_submit = 1;
                    LOGE("Host: vkQueueSubmit2 rejected a truncated header");
                }
            }
        }

        if (!invalid_submit) {
            queue = (VkQueue)(uintptr_t)lookup_mapping(
                EXPRESS_VK_OBJECT_TYPE_QUEUE, guest_queue);
            fence = guest_fence ? (VkFence)(uintptr_t)lookup_mapping(
                EXPRESS_VK_OBJECT_TYPE_FENCE, guest_fence) : VK_NULL_HANDLE;
            if (guest_queue == 0 || queue == VK_NULL_HANDLE ||
                (guest_fence != 0 && fence == VK_NULL_HANDLE)) {
                invalid_submit = 1;
                LOGE("Host: vkQueueSubmit2 rejected an unmapped queue or fence");
            }
        }

        if (!invalid_submit) {
            if (!express_vk_decode_submit_infos2(
                    &cursor, stream_end, submitCount, &pSubmits)) {
                invalid_submit = 1;
                LOGE("Host: vkQueueSubmit2 rejected malformed, extended, "
                     "unmapped, or oversized submit data");
            } else {
                data_ptr = (char *)cursor;
            }
        }

        ExpressVkSubmitHints submit_hints;
        memset(&submit_hints, 0, sizeof(submit_hints));
        if (!invalid_submit) {
            express_vk_parse_submit_hints(
                data_ptr, (char *)stream_end, VK_NULL_HANDLE, &submit_hints);
        }
        if (!invalid_submit) {
            express_vk_wait_uploads_for_submit_hints(&submit_hints);
        }

        ExpressVkFlimeSubmitBatch *flime_batch = NULL;
        ExpressVkQueueInfo flime_queue_info;
        VkDevice flime_submit_device = VK_NULL_HANDLE;
        uint64_t flime_realize_ns = 0;
        if (queue != VK_NULL_HANDLE &&
            express_vk_lookup_queue_info(queue, &flime_queue_info)) {
            flime_submit_device = flime_queue_info.device;
        }
        ExpressVkFlimeSubmitGate flime_gate =
            EXPRESS_VK_FLIME_SUBMIT_LEGACY;

        g_mutex_lock(&g_express_vk_transaction_lock);
        flime_gate = express_vk_flime_bridge_prepare_submit(
            call->process_id, guest_queue, queue, flime_submit_device,
            &flime_batch);
        if (flime_gate == EXPRESS_VK_FLIME_SUBMIT_BLOCKED ||
            flime_gate == EXPRESS_VK_FLIME_SUBMIT_ERROR ||
            (flime_gate == EXPRESS_VK_FLIME_SUBMIT_READY &&
             flime_batch == NULL)) {
            invalid_submit = 1;
        }
        VkResult result = VK_ERROR_INITIALIZATION_FAILED;
        if (!invalid_submit &&
            flime_gate == EXPRESS_VK_FLIME_SUBMIT_READY &&
            express_vk_flime_bridge_batch_write_count(flime_batch) != 0) {
            flime_realize_ns = express_vk_flime_realize_descriptor_writes(
                express_vk_flime_bridge_batch_device(flime_batch),
                express_vk_flime_bridge_batch_write_count(flime_batch),
                express_vk_flime_bridge_batch_writes(flime_batch));
            if (!express_vk_flime_bridge_submit_updates_applied(flime_batch)) {
                invalid_submit = 1;
            }
        }
        if (!invalid_submit) {
            result = vkQueueSubmit2(queue, submitCount, pSubmits, fence);
        } else {
            LOGE("Host: vkQueueSubmit2 skipped real call because its decoded "
                 "handles or the FLIME FINAL gate were invalid");
        }
        if (flime_batch != NULL) {
            express_vk_flime_bridge_complete_submit(flime_batch, result,
                                                     flime_realize_ns);
        }
        g_mutex_unlock(&g_express_vk_transaction_lock);
        if (para_num > 1) {
            write_to_guest_mem(all_para[1].data, &result, 0,
                               sizeof(result));
        }
        if(result == VK_SUCCESS) {
            if (EXPRESS_VK_ENABLE_SYNC_DIAG_LOG) {
                LOGD("QueueSubmit2 completed successfully");
            }
            if (EXPRESS_VK_ENABLE_FENCE_OUTPUT_HINT_COMMIT &&
                fence != VK_NULL_HANDLE) {
                express_vk_store_fence_output_hints(fence, guest_fence, &submit_hints);
            }
        } else {
            LOGE("QueueSubmit2 failed with error: %d", result);
        }

        express_vk_free_decoded_submit_infos2(pSubmits, submitCount);
        express_vk_free_submit_hints(&submit_hints);
        if (need_free && stream_base != NULL) {
            g_free(stream_base);
        }
        if (EXPRESS_VK_ENABLE_SYNC_DIAG_LOG) {
            LOGD("QueueSubmit2 result: %d", result);
        }
    }
    break;

    case FUNID_vkSetEvent:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_device = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        uint64_t guest_event = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        VkDevice device = (VkDevice)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_device);
        VkEvent event = (VkEvent)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_EVENT, guest_event);

        vkSetEvent(device, event);
        LOGD("SetEvent complete");
    }
    break;

    case FUNID_vkSignalSemaphore:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_device = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        VkDevice device = (VkDevice)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_device);

        VkSemaphoreSignalInfo signalInfo;
        decode_from_stream_VkSemaphoreSignalInfo(VK_STRUCTURE_TYPE_MAX_ENUM,
                                                &signalInfo, ptr);

        VkResult result = vkSignalSemaphore(device, &signalInfo);
        if(result == VK_SUCCESS) {
            LOGD("SignalSemaphore completed successfully");
        } else {
            LOGE("SignalSemaphore failed with error: %d", result);
        }
        LOGD("SignalSemaphore result: %d", result);
    }
    break;

    case FUNID_vkUpdateDescriptorSetWithTemplate:
    {
        gint64 hot_start_us = g_get_monotonic_time();
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);

        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_device;
        memcpy(&guest_device, *ptr, sizeof(uint64_t));
        *ptr += sizeof(uint64_t);

        uint64_t guest_descriptor_set;
        memcpy(&guest_descriptor_set, *ptr, sizeof(uint64_t));
        *ptr += sizeof(uint64_t);

        uint64_t guest_template;
        memcpy(&guest_template, *ptr, sizeof(uint64_t));
        *ptr += sizeof(uint64_t);

        size_t data_size;
        memcpy(&data_size, *ptr, sizeof(size_t));
        *ptr += sizeof(size_t);

        uint32_t entry_count;
        memcpy(&entry_count, *ptr, sizeof(uint32_t));
        *ptr += sizeof(uint32_t);
        VkDescriptorUpdateTemplateEntry* entries =
            (VkDescriptorUpdateTemplateEntry*)malloc(entry_count * sizeof(VkDescriptorUpdateTemplateEntry));
        if (entry_count > 0 && entries == NULL) {
            LOGE("Host: vkUpdateDescriptorSetWithTemplate failed to allocate %u entries",
                 entry_count);
            break;
        }
        for (uint32_t i = 0; i < entry_count; ++i) {
            memcpy(&entries[i], *ptr, sizeof(VkDescriptorUpdateTemplateEntry));
            *ptr += sizeof(VkDescriptorUpdateTemplateEntry);
        }

        VkDevice device = (VkDevice)(uintptr_t)lookup_mapping(
            EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_device);
        VkDescriptorSet descriptorSet =
            express_vk_lookup_descriptor_set(guest_descriptor_set);
        VkDescriptorUpdateTemplate descriptorUpdateTemplate =
            (VkDescriptorUpdateTemplate)(uintptr_t)lookup_mapping(
                EXPRESS_VK_OBJECT_TYPE_DESCRIPTOR_UPDATE_TEMPLATE, guest_template);

        void* pData_copy = malloc(data_size);
        if (data_size > 0 && pData_copy == NULL) {
            LOGE("Host: vkUpdateDescriptorSetWithTemplate failed to allocate data_size=%llu",
                 (unsigned long long)data_size);
            free(entries);
            break;
        }
        if (data_size > 0) {
            memcpy(pData_copy, *ptr, data_size);
        }

        for (uint32_t i = 0; i < entry_count; ++i) {
            VkDescriptorUpdateTemplateEntry* entry = &entries[i];
            size_t item_size =
                express_vk_descriptor_template_item_size(entry->descriptorType);

            for (uint32_t j = 0; j < entry->descriptorCount; ++j) {
                size_t desc_offset = 0;
                if (!express_vk_descriptor_template_item_in_bounds(
                        entry, j, item_size, data_size, &desc_offset)) {
                    LOGE("Host: vkUpdateDescriptorSetWithTemplate entry OOB binding=%u elem=%u type=%u count=%u index=%u offset=%llu stride=%llu item=%llu data_size=%llu",
                         entry->dstBinding,
                         entry->dstArrayElement + j,
                         entry->descriptorType,
                         entry->descriptorCount,
                         j,
                         (unsigned long long)entry->offset,
                         (unsigned long long)entry->stride,
                         (unsigned long long)item_size,
                         (unsigned long long)data_size);
                    continue;
                }

                void* desc_ptr = (char*)pData_copy + desc_offset;

                switch (entry->descriptorType) {
                    case VK_DESCRIPTOR_TYPE_SAMPLER:
                    case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
                    case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
                    case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
                    case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT: {
                        VkDescriptorImageInfo* img_info = (VkDescriptorImageInfo*)desc_ptr;
                        if (img_info->sampler) {
                            uint64_t guest_sampler = (uint64_t)(uintptr_t)img_info->sampler;
                            img_info->sampler = (VkSampler)(uintptr_t)lookup_mapping(
                                EXPRESS_VK_OBJECT_TYPE_SAMPLER, guest_sampler);
                        }
                        if (img_info->imageView) {
                            uint64_t guest_image_view = (uint64_t)(uintptr_t)img_info->imageView;
                            img_info->imageView = (VkImageView)(uintptr_t)lookup_mapping(
                                EXPRESS_VK_OBJECT_TYPE_IMAGE_VIEW, guest_image_view);
                        }
                        break;
                    }
                    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
                    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
                    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
                    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: {
                        VkDescriptorBufferInfo* buf_info = (VkDescriptorBufferInfo*)desc_ptr;
                        if (buf_info->buffer) {
                            uint64_t guest_buffer = (uint64_t)(uintptr_t)buf_info->buffer;
                            uint64_t mapped_buffer =
                                lookup_mapping(EXPRESS_VK_OBJECT_TYPE_BUFFER, guest_buffer);
#if EXPRESS_VK_ENABLE_DESCRIPTOR_TRACE
                            if (buf_info->range == VK_WHOLE_SIZE ||
                                (uint64_t)buf_info->range >= EXPRESS_VK_DESC_TRACE_MIN_BYTES) {
                                bool set_found = false;
                                VkDescriptorSet cached_set =
                                    express_vk_lookup_descriptor_set_cached(
                                        guest_descriptor_set, &set_found);
                                LOGD("[HOST_DESC] update_template guest_set=0x%llx cached_found=%d cached_set=%p binding=%u elem=%u type=%u guest_buffer=0x%llx mapped_buffer=%p offset=%llu range=%llu",
                                     (unsigned long long)guest_descriptor_set,
                                     set_found,
                                     (void*)cached_set,
                                     entry->dstBinding,
                                     entry->dstArrayElement + j,
                                     entry->descriptorType,
                                     (unsigned long long)guest_buffer,
                                     (void*)(uintptr_t)mapped_buffer,
                                     (unsigned long long)buf_info->offset,
                                     (unsigned long long)buf_info->range);
                            }
#endif
                            buf_info->buffer = (VkBuffer)(uintptr_t)mapped_buffer;
                        }
                        break;
                    }
                    case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
                    case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: {
                        VkBufferView* buffer_view = (VkBufferView*)desc_ptr;
                        if (*buffer_view) {
                            uint64_t guest_buffer_view = (uint64_t)(uintptr_t)(*buffer_view);
                            *buffer_view = (VkBufferView)(uintptr_t)lookup_mapping(
                                EXPRESS_VK_OBJECT_TYPE_BUFFER_VIEW, guest_buffer_view);
                        }
                        break;
                    }
                    case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR: {
                        VkAccelerationStructureKHR* accel = (VkAccelerationStructureKHR*)desc_ptr;
                        if (*accel) {
                            uint64_t guest_accel = (uint64_t)(uintptr_t)(*accel);
                            *accel = (VkAccelerationStructureKHR)(uintptr_t)lookup_mapping(
                                EXPRESS_VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR, guest_accel);
                        }
                        break;
                    }
                    default:
                        break;
                }
            }
        }


        vkUpdateDescriptorSetWithTemplate(device, descriptorSet,
                                        descriptorUpdateTemplate, pData_copy);

        free(entries);
        free(pData_copy);
        express_vk_host_note_descriptor_timing(
            3, (uint64_t)(g_get_monotonic_time() - hot_start_us));
    }
    break;

    case FUNID_vkWaitSemaphores:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_device = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        uint64_t timeout = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        VkDevice device = (VkDevice)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_device);

        VkSemaphoreWaitInfo waitInfo;
        decode_from_stream_VkSemaphoreWaitInfo(VK_STRUCTURE_TYPE_MAX_ENUM,
                                            &waitInfo, ptr);

        VkResult result = vkWaitSemaphores(device, &waitInfo, timeout);
        if(result == VK_SUCCESS) {
            LOGD("WaitSemaphores completed successfully");
        } else {
            LOGE("WaitSemaphores failed with error: %d", result);
        }
        LOGD("WaitSemaphores result: %d", result);
    }
    break;

    case FUNID_vkGetPrivateData:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_device = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        uint32_t objectType = *(uint32_t*)(*ptr);
        *ptr += sizeof(uint32_t);
        uint64_t objectHandle = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        uint64_t guest_private_data_slot = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        uint64_t real_handle = lookup_mapping(objectType, objectHandle);
        LOGD("going to call vkGetPrivateData");

        VkDevice device = (VkDevice)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_device);
        VkPrivateDataSlot privateDataSlot = (VkPrivateDataSlot)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_PRIVATE_DATA_SLOT, guest_private_data_slot);

        uint64_t data;
        vkGetPrivateData(device, (VkObjectType)objectType, real_handle, privateDataSlot, &data);

        write_to_guest_mem(all_para[1].data, &data, 0, sizeof(uint64_t));
        LOGD("GetPrivateData complete");
    }
    break;

    case FUNID_vkSetPrivateData:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_device = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        uint32_t objectType = *(uint32_t*)(*ptr);
        *ptr += sizeof(uint32_t);
        uint64_t objectHandle = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        uint64_t guest_private_data_slot = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);
        uint64_t data = *(uint64_t*)(*ptr);
        *ptr += sizeof(uint64_t);

        VkDevice device = (VkDevice)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_device);
        VkPrivateDataSlot privateDataSlot = (VkPrivateDataSlot)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_PRIVATE_DATA_SLOT, guest_private_data_slot);
        uint64_t real_handle = lookup_mapping(objectType, objectHandle);
        LOGD("going to call vkSetPrivateData");
        LOGD("info verbose: device=%p, objectType=%u, objectHandle=%lu, privateDataSlot=%p, data=%lu",
             device, objectType, real_handle, privateDataSlot, data);

        VkResult result = vkSetPrivateData(device, (VkObjectType)objectType, real_handle, privateDataSlot, data);
        if(result == VK_SUCCESS) {
            LOGD("SetPrivateData completed successfully");
        } else {
            LOGE("SetPrivateData failed with error: %d", result);
        }
        LOGD("SetPrivateData result: %d", result);
    }
    break;

    case FUNID_vkGetPhysicalDeviceSurfaceCapabilitiesKHR:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_phys_dev;
        memcpy(&guest_phys_dev, *ptr, 8); *ptr += 8;
        uint64_t guest_surface;
        memcpy(&guest_surface, *ptr, 8); *ptr += 8;

        VkPhysicalDevice physicalDevice = (VkPhysicalDevice)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_PHYSICAL_DEVICE, guest_phys_dev);
        VkSurfaceKHR surface = (VkSurfaceKHR)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_SURFACE_KHR, guest_surface);

        VkSurfaceCapabilitiesKHR capabilities;
        VkResult result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, &capabilities);

        write_to_guest_mem(all_para[1].data, &capabilities, 0, sizeof(VkSurfaceCapabilitiesKHR));
        LOGD("GetPhysicalDeviceSurfaceCapabilitiesKHR result %d", result);
    }
    break;

    case FUNID_vkGetPhysicalDeviceSurfaceCapabilities2KHR:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_phys_dev;
        memcpy(&guest_phys_dev, *ptr, 8); *ptr += 8;

        VkPhysicalDeviceSurfaceInfo2KHR surfaceInfo;
        decode_from_stream_VkPhysicalDeviceSurfaceInfo2KHR(VK_STRUCTURE_TYPE_MAX_ENUM, &surfaceInfo, ptr);

        VkPhysicalDevice physicalDevice = (VkPhysicalDevice)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_PHYSICAL_DEVICE, guest_phys_dev);

        VkSurfaceCapabilities2KHR capabilities = {VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR};
        VkResult result = vkGetPhysicalDeviceSurfaceCapabilities2KHR(physicalDevice, &surfaceInfo, &capabilities);

        write_to_guest_mem(all_para[1].data, &capabilities, 0, sizeof(VkSurfaceCapabilities2KHR));
        LOGD("GetPhysicalDeviceSurfaceCapabilities2KHR result %d", result);
    }
    break;

    case FUNID_vkGetPhysicalDeviceSurfaceFormatsKHR:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_phys_dev;
        memcpy(&guest_phys_dev, *ptr, 8); *ptr += 8;
        uint64_t guest_surface;
        memcpy(&guest_surface, *ptr, 8); *ptr += 8;

        VkPhysicalDevice physicalDevice = (VkPhysicalDevice)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_PHYSICAL_DEVICE, guest_phys_dev);
        VkSurfaceKHR surface = (VkSurfaceKHR)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_SURFACE_KHR, guest_surface);

        uint32_t count = 0;
        if (!copy_from_call_para_fast(all_para[1], &count, sizeof(uint32_t))) {
            LOGE("Host: GetPhysicalDeviceSurfaceFormatsKHR failed to read count");
            break;
        }

        VkResult result;
        if (count == 0) {
            result = vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &count, NULL);
            write_to_guest_mem(all_para[1].data, &count, 0, sizeof(uint32_t));
        } else {
            VkSurfaceFormatKHR* formats = (VkSurfaceFormatKHR*)malloc(count * sizeof(VkSurfaceFormatKHR));
            result = vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &count, formats);
            write_to_guest_mem(all_para[2].data, formats, 0, count * sizeof(VkSurfaceFormatKHR));
            write_to_guest_mem(all_para[1].data, &count, 0, sizeof(uint32_t));
            free(formats);
        }

        LOGD("GetPhysicalDeviceSurfaceFormatsKHR count %d result %d", count, result);
    }
    break;

    case FUNID_vkGetPhysicalDeviceSurfaceFormats2KHR:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_phys_dev;
        memcpy(&guest_phys_dev, *ptr, 8); *ptr += 8;

        VkPhysicalDeviceSurfaceInfo2KHR surfaceInfo;
        decode_from_stream_VkPhysicalDeviceSurfaceInfo2KHR(VK_STRUCTURE_TYPE_MAX_ENUM, &surfaceInfo, ptr);

        uint32_t count;
        memcpy(&count, *ptr, 4); *ptr += 4;

        VkPhysicalDevice physicalDevice = (VkPhysicalDevice)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_PHYSICAL_DEVICE, guest_phys_dev);

        VkResult result;
        if (count == 0) {
            result = vkGetPhysicalDeviceSurfaceFormats2KHR(physicalDevice, &surfaceInfo, &count, NULL);
            write_to_guest_mem(all_para[1].data, &count, 0, sizeof(uint32_t));
        } else {
            VkSurfaceFormat2KHR* formats = (VkSurfaceFormat2KHR*)malloc(count * sizeof(VkSurfaceFormat2KHR));
            for (uint32_t i = 0; i < count; i++) {
                formats[i].sType = VK_STRUCTURE_TYPE_SURFACE_FORMAT_2_KHR;
                formats[i].pNext = NULL;
            }
            result = vkGetPhysicalDeviceSurfaceFormats2KHR(physicalDevice, &surfaceInfo, &count, formats);
            write_to_guest_mem(all_para[2].data, formats, 0, count * sizeof(VkSurfaceFormat2KHR));
            write_to_guest_mem(all_para[1].data, &count, 0, sizeof(uint32_t));
            free(formats);
        }

        write_to_guest_mem(all_para[para_num - 1].data, &result, 0, sizeof(VkResult));
        LOGD("GetPhysicalDeviceSurfaceFormats2KHR count %d result %d", count, result);
    }
    break;

    case FUNID_vkGetPhysicalDeviceSurfacePresentModesKHR:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_phys_dev;
        memcpy(&guest_phys_dev, *ptr, 8); *ptr += 8;
        uint64_t guest_surface;
        memcpy(&guest_surface, *ptr, 8); *ptr += 8;

        VkPhysicalDevice physicalDevice = (VkPhysicalDevice)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_PHYSICAL_DEVICE, guest_phys_dev);
        VkSurfaceKHR surface = (VkSurfaceKHR)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_SURFACE_KHR, guest_surface);

        uint32_t count = 0;
        if (!copy_from_call_para_fast(all_para[1], &count, sizeof(uint32_t))) {
            LOGE("Host: GetPhysicalDeviceSurfacePresentModesKHR failed to read count");
            break;
        }

        VkResult result;
        if (count == 0) {
            result = vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, &count, NULL);
            write_to_guest_mem(all_para[1].data, &count, 0, sizeof(uint32_t));
        } else {
            VkPresentModeKHR* modes = (VkPresentModeKHR*)malloc(count * sizeof(VkPresentModeKHR));
            result = vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, &count, modes);
            write_to_guest_mem(all_para[2].data, modes, 0, count * sizeof(VkPresentModeKHR));
            write_to_guest_mem(all_para[1].data, &count, 0, sizeof(uint32_t));
            free(modes);
        }

        LOGD("GetPhysicalDeviceSurfacePresentModesKHR count %d result %d", count, result);
    }
    break;

    case FUNID_vkGetPhysicalDeviceSurfaceSupportKHR:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_phys_dev;
        memcpy(&guest_phys_dev, *ptr, 8); *ptr += 8;
        uint32_t queueFamilyIndex;
        memcpy(&queueFamilyIndex, *ptr, 4); *ptr += 4;
        uint64_t guest_surface;
        memcpy(&guest_surface, *ptr, 8); *ptr += 8;

        VkPhysicalDevice physicalDevice = (VkPhysicalDevice)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_PHYSICAL_DEVICE, guest_phys_dev);
        VkSurfaceKHR surface = (VkSurfaceKHR)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_SURFACE_KHR, guest_surface);

        VkBool32 supported;
        VkResult result = vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice, queueFamilyIndex, surface, &supported);

        write_to_guest_mem(all_para[1].data, &supported, 0, sizeof(VkBool32));
        LOGD("GetPhysicalDeviceSurfaceSupportKHR queue %d supported %d result %d", queueFamilyIndex, supported, result);
    }
    break;


    case FUNID_vkGetPhysicalDeviceDisplayPropertiesKHR:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_phys_dev;
        memcpy(&guest_phys_dev, *ptr, 8); *ptr += 8;

        VkPhysicalDevice physicalDevice = (VkPhysicalDevice)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_PHYSICAL_DEVICE, guest_phys_dev);

        uint32_t count = 0;
        if (!copy_from_call_para_fast(all_para[1], &count, sizeof(uint32_t))) {
            LOGE("Host: GetPhysicalDeviceDisplayPropertiesKHR failed to read count");
            break;
        }

        VkResult result;
        if (count == 0) {
            result = vkGetPhysicalDeviceDisplayPropertiesKHR(physicalDevice, &count, NULL);
            write_to_guest_mem(all_para[1].data, &count, 0, sizeof(uint32_t));
        } else {
            uint64_t* guest_displays = (uint64_t*)malloc(count * sizeof(uint64_t));
            if (!guest_displays ||
                !copy_from_call_para_fast(all_para[2], guest_displays, count * sizeof(uint64_t))) {
                LOGE("Host: GetPhysicalDeviceDisplayPropertiesKHR failed to get guest display handles");
                if (guest_displays) free(guest_displays);
                break;
            }

            VkDisplayPropertiesKHR* props = (VkDisplayPropertiesKHR*)malloc(count * sizeof(VkDisplayPropertiesKHR));
            result = vkGetPhysicalDeviceDisplayPropertiesKHR(physicalDevice, &count, props);

            for (uint32_t i = 0; i < count; ++i) {
                uint64_t host_display = (uint64_t)(uintptr_t)props[i].display;
                insert_mapping(EXPRESS_VK_OBJECT_TYPE_DISPLAY_KHR, guest_displays[i], host_display);
            }

            write_to_guest_mem(all_para[3].data, props, 0, count * sizeof(VkDisplayPropertiesKHR));
            write_to_guest_mem(all_para[1].data, &count, 0, sizeof(uint32_t));

            free(props);
            free(guest_displays);
        }

        LOGD("GetPhysicalDeviceDisplayPropertiesKHR count %d", count);
    }
    break;

    case FUNID_vkGetPhysicalDeviceDisplayProperties2KHR:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_phys_dev;
        memcpy(&guest_phys_dev, *ptr, 8); *ptr += 8;

        VkPhysicalDevice physicalDevice = (VkPhysicalDevice)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_PHYSICAL_DEVICE, guest_phys_dev);

        uint32_t count = 0;
        if (!copy_from_call_para_fast(all_para[1], &count, sizeof(uint32_t))) {
            LOGE("Host: GetPhysicalDeviceDisplayProperties2KHR failed to read count");
            break;
        }

        VkResult result;
        if (count == 0) {
            result = vkGetPhysicalDeviceDisplayProperties2KHR(physicalDevice, &count, NULL);
            write_to_guest_mem(all_para[1].data, &count, 0, sizeof(uint32_t));
        } else {
            uint64_t* guest_displays = (uint64_t*)malloc(count * sizeof(uint64_t));
            if (!guest_displays ||
                !copy_from_call_para_fast(all_para[2], guest_displays, count * sizeof(uint64_t))) {
                LOGE("Host: GetPhysicalDeviceDisplayProperties2KHR failed to get guest display handles");
                if (guest_displays) free(guest_displays);
                break;
            }

            VkDisplayProperties2KHR* props = (VkDisplayProperties2KHR*)malloc(count * sizeof(VkDisplayProperties2KHR));
            for (uint32_t i = 0; i < count; ++i) {
                props[i].sType = VK_STRUCTURE_TYPE_DISPLAY_PROPERTIES_2_KHR;
                props[i].pNext = NULL;
            }
            result = vkGetPhysicalDeviceDisplayProperties2KHR(physicalDevice, &count, props);

            for (uint32_t i = 0; i < count; ++i) {
                uint64_t host_display = (uint64_t)(uintptr_t)props[i].displayProperties.display;
                insert_mapping(EXPRESS_VK_OBJECT_TYPE_DISPLAY_KHR, guest_displays[i], host_display);
            }

            write_to_guest_mem(all_para[3].data, props, 0, count * sizeof(VkDisplayProperties2KHR));
            write_to_guest_mem(all_para[1].data, &count, 0, sizeof(uint32_t));

            free(props);
            free(guest_displays);
        }

        LOGD("GetPhysicalDeviceDisplayProperties2KHR count %d", count);
    }
    break;

    case FUNID_vkGetPhysicalDeviceDisplayPlanePropertiesKHR:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_phys_dev;
        memcpy(&guest_phys_dev, *ptr, 8); *ptr += 8;

        VkPhysicalDevice physicalDevice = (VkPhysicalDevice)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_PHYSICAL_DEVICE, guest_phys_dev);

        uint32_t count = 0;
        if (!copy_from_call_para_fast(all_para[1], &count, sizeof(uint32_t))) {
            LOGE("Host: GetPhysicalDeviceDisplayPlanePropertiesKHR failed to read count");
            break;
        }

        VkResult result;
        if (count == 0) {
            result = vkGetPhysicalDeviceDisplayPlanePropertiesKHR(physicalDevice, &count, NULL);
            write_to_guest_mem(all_para[1].data, &count, 0, sizeof(uint32_t));
        } else {
            uint64_t* guest_displays = (uint64_t*)malloc(count * sizeof(uint64_t));
            if (!guest_displays ||
                !copy_from_call_para_fast(all_para[2], guest_displays, count * sizeof(uint64_t))) {
                LOGE("Host: GetPhysicalDeviceDisplayPlanePropertiesKHR failed to get guest display handles");
                if (guest_displays) free(guest_displays);
                break;
            }

            VkDisplayPlanePropertiesKHR* props = (VkDisplayPlanePropertiesKHR*)malloc(count * sizeof(VkDisplayPlanePropertiesKHR));
            result = vkGetPhysicalDeviceDisplayPlanePropertiesKHR(physicalDevice, &count, props);

            for (uint32_t i = 0; i < count; ++i) {
                if (props[i].currentDisplay != VK_NULL_HANDLE) {
                    uint64_t host_display = (uint64_t)(uintptr_t)props[i].currentDisplay;
                    insert_mapping(EXPRESS_VK_OBJECT_TYPE_DISPLAY_KHR, guest_displays[i], host_display);
                }
            }

            write_to_guest_mem(all_para[3].data, props, 0, count * sizeof(VkDisplayPlanePropertiesKHR));
            write_to_guest_mem(all_para[1].data, &count, 0, sizeof(uint32_t));

            free(props);
            free(guest_displays);
        }

        LOGD("GetPhysicalDeviceDisplayPlanePropertiesKHR count %d", count);
    }
    break;

    case FUNID_vkGetPhysicalDeviceDisplayPlaneProperties2KHR:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_phys_dev;
        memcpy(&guest_phys_dev, *ptr, 8); *ptr += 8;

        VkPhysicalDevice physicalDevice = (VkPhysicalDevice)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_PHYSICAL_DEVICE, guest_phys_dev);

        uint32_t count = 0;
        if (!copy_from_call_para_fast(all_para[1], &count, sizeof(uint32_t))) {
            LOGE("Host: GetPhysicalDeviceDisplayPlaneProperties2KHR failed to read count");
            break;
        }

        VkResult result;
        if (count == 0) {
            result = vkGetPhysicalDeviceDisplayPlaneProperties2KHR(physicalDevice, &count, NULL);
            write_to_guest_mem(all_para[1].data, &count, 0, sizeof(uint32_t));
        } else {
            uint64_t* guest_displays = (uint64_t*)malloc(count * sizeof(uint64_t));
            if (!guest_displays ||
                !copy_from_call_para_fast(all_para[2], guest_displays, count * sizeof(uint64_t))) {
                LOGE("Host: GetPhysicalDeviceDisplayPlaneProperties2KHR failed to get guest display handles");
                if (guest_displays) free(guest_displays);
                break;
            }

            VkDisplayPlaneProperties2KHR* props = (VkDisplayPlaneProperties2KHR*)malloc(count * sizeof(VkDisplayPlaneProperties2KHR));
            for (uint32_t i = 0; i < count; ++i) {
                props[i].sType = VK_STRUCTURE_TYPE_DISPLAY_PLANE_PROPERTIES_2_KHR;
                props[i].pNext = NULL;
            }
            result = vkGetPhysicalDeviceDisplayPlaneProperties2KHR(physicalDevice, &count, props);

            for (uint32_t i = 0; i < count; ++i) {
                if (props[i].displayPlaneProperties.currentDisplay != VK_NULL_HANDLE) {
                    uint64_t host_display = (uint64_t)(uintptr_t)props[i].displayPlaneProperties.currentDisplay;
                    insert_mapping(EXPRESS_VK_OBJECT_TYPE_DISPLAY_KHR, guest_displays[i], host_display);
                }
            }

            write_to_guest_mem(all_para[3].data, props, 0, count * sizeof(VkDisplayPlaneProperties2KHR));
            write_to_guest_mem(all_para[1].data, &count, 0, sizeof(uint32_t));

            free(props);
            free(guest_displays);
        }

        LOGD("GetPhysicalDeviceDisplayPlaneProperties2KHR count %d", count);
    }
    break;

    case FUNID_vkGetDisplayModePropertiesKHR:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_phys_dev;
        memcpy(&guest_phys_dev, *ptr, 8); *ptr += 8;
        uint64_t guest_display;
        memcpy(&guest_display, *ptr, 8); *ptr += 8;

        VkPhysicalDevice physicalDevice = (VkPhysicalDevice)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_PHYSICAL_DEVICE, guest_phys_dev);
        VkDisplayKHR display = (VkDisplayKHR)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DISPLAY_KHR, guest_display);

        uint32_t count = 0;
        if (!copy_from_call_para_fast(all_para[1], &count, sizeof(uint32_t))) {
            LOGE("Host: GetDisplayModePropertiesKHR failed to read count");
            break;
        }

        VkResult result;
        if (count == 0) {
            result = vkGetDisplayModePropertiesKHR(physicalDevice, display, &count, NULL);
            write_to_guest_mem(all_para[1].data, &count, 0, sizeof(uint32_t));
        } else {
            uint64_t* guest_modes = (uint64_t*)malloc(count * sizeof(uint64_t));
            if (!guest_modes ||
                !copy_from_call_para_fast(all_para[2], guest_modes, count * sizeof(uint64_t))) {
                LOGE("Host: GetDisplayModePropertiesKHR failed to get guest mode handles");
                if (guest_modes) free(guest_modes);
                break;
            }

            VkDisplayModePropertiesKHR* props = (VkDisplayModePropertiesKHR*)malloc(count * sizeof(VkDisplayModePropertiesKHR));
            result = vkGetDisplayModePropertiesKHR(physicalDevice, display, &count, props);

            for (uint32_t i = 0; i < count; ++i) {
                uint64_t host_mode = (uint64_t)(uintptr_t)props[i].displayMode;
                insert_mapping(EXPRESS_VK_OBJECT_TYPE_DISPLAY_MODE_KHR, guest_modes[i], host_mode);
            }

            write_to_guest_mem(all_para[3].data, props, 0, count * sizeof(VkDisplayModePropertiesKHR));
            write_to_guest_mem(all_para[1].data, &count, 0, sizeof(uint32_t));

            free(props);
            free(guest_modes);
        }

        write_to_guest_mem(all_para[para_num - 1].data, &result, 0, sizeof(VkResult));

        LOGD("GetDisplayModePropertiesKHR count %d result %d", count, result);
    }
    break;

    case FUNID_vkGetDisplayModeProperties2KHR:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_phys_dev;
        memcpy(&guest_phys_dev, *ptr, 8); *ptr += 8;
        uint64_t guest_display;
        memcpy(&guest_display, *ptr, 8); *ptr += 8;

        VkPhysicalDevice physicalDevice = (VkPhysicalDevice)(uintptr_t)
        lookup_mapping(EXPRESS_VK_OBJECT_TYPE_PHYSICAL_DEVICE, guest_phys_dev);
        VkDisplayKHR display = (VkDisplayKHR)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DISPLAY_KHR, guest_display);

        uint32_t count = 0;
        if (!copy_from_call_para_fast(all_para[1], &count, sizeof(uint32_t))) {
            LOGE("Host: GetDisplayModeProperties2KHR failed to read count");
            break;
        }

        VkResult result;
        if (count == 0) {
            result = vkGetDisplayModeProperties2KHR(physicalDevice, display, &count, NULL);
            write_to_guest_mem(all_para[1].data, &count, 0, sizeof(uint32_t));
        } else {
            uint64_t* guest_modes = (uint64_t*)malloc(count * sizeof(uint64_t));
            if (!guest_modes ||
                !copy_from_call_para_fast(all_para[2], guest_modes, count * sizeof(uint64_t))) {
                LOGE("Host: GetDisplayModeProperties2KHR failed to get guest mode handles");
                if (guest_modes) free(guest_modes);
                break;
            }

            VkDisplayModeProperties2KHR* props = (VkDisplayModeProperties2KHR*)malloc(count * sizeof(VkDisplayModeProperties2KHR));
            for (uint32_t i = 0; i < count; ++i) {
                props[i].sType = VK_STRUCTURE_TYPE_DISPLAY_MODE_PROPERTIES_2_KHR;
                props[i].pNext = NULL;
            }
            result = vkGetDisplayModeProperties2KHR(physicalDevice, display, &count, props);

            for (uint32_t i = 0; i < count; ++i) {
                uint64_t host_mode = (uint64_t)(uintptr_t)props[i].displayModeProperties.displayMode;
                insert_mapping(EXPRESS_VK_OBJECT_TYPE_DISPLAY_MODE_KHR, guest_modes[i], host_mode);
            }

            write_to_guest_mem(all_para[3].data, props, 0, count * sizeof(VkDisplayModeProperties2KHR));
            write_to_guest_mem(all_para[1].data, &count, 0, sizeof(uint32_t));

            free(props);
            free(guest_modes);
        }

        write_to_guest_mem(all_para[para_num - 1].data, &result, 0, sizeof(VkResult));
        LOGD("GetDisplayModeProperties2KHR count %d", count);
    }
    break;

    case FUNID_vkGetPhysicalDevicePresentRectanglesKHR:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_phys_dev;
        memcpy(&guest_phys_dev, *ptr, 8); *ptr += 8;
        uint64_t guest_surface;
        memcpy(&guest_surface, *ptr, 8); *ptr += 8;
        uint32_t rect_count;
        memcpy(&rect_count, *ptr, 4); *ptr += 4;

        VkPhysicalDevice physicalDevice = (VkPhysicalDevice)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_PHYSICAL_DEVICE, guest_phys_dev);
        VkSurfaceKHR surface = (VkSurfaceKHR)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_SURFACE_KHR, guest_surface);

        VkResult result;
        uint32_t count = rect_count;

        if (count == 0) {
            result = vkGetPhysicalDevicePresentRectanglesKHR(physicalDevice, surface, &count, NULL);
            write_to_guest_mem(all_para[1].data, &count, 0, sizeof(uint32_t));
        } else {
            VkRect2D* rects = (VkRect2D*)malloc(count * sizeof(VkRect2D));
            result = vkGetPhysicalDevicePresentRectanglesKHR(physicalDevice, surface, &count, rects);
            write_to_guest_mem(all_para[1].data, &count, 0, sizeof(uint32_t));
            if (result == VK_SUCCESS) {
                write_to_guest_mem(all_para[2].data, rects, 0, count * sizeof(VkRect2D));
            }
            free(rects);
        }

        LOGD("GetPhysicalDevicePresentRectanglesKHR result=%d count=%d", result, count);
    }
    break;

    case FUNID_vkCmdResetQueryPool:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_cmd_buf;
        memcpy(&guest_cmd_buf, *ptr, 8); *ptr += 8;
        uint64_t guest_query_pool;
        memcpy(&guest_query_pool, *ptr, 8); *ptr += 8;
        uint32_t firstQuery;
        memcpy(&firstQuery, *ptr, 4); *ptr += 4;
        uint32_t queryCount;
        memcpy(&queryCount, *ptr, 4); *ptr += 4;

        VkCommandBuffer commandBuffer = (VkCommandBuffer)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER, guest_cmd_buf);
        VkQueryPool queryPool = (VkQueryPool)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_QUERY_POOL, guest_query_pool);

        vkCmdResetQueryPool(commandBuffer, queryPool, firstQuery, queryCount);

        LOGD("CmdResetQueryPool firstQuery=%d queryCount=%d", firstQuery, queryCount);
    }
    break;

    case FUNID_vkCreateDisplayModeKHR:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_physicalDevice;
        memcpy(&guest_physicalDevice, *ptr, sizeof(uint64_t));
        *ptr += sizeof(uint64_t);
        VkPhysicalDevice physicalDevice = (VkPhysicalDevice)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_PHYSICAL_DEVICE, guest_physicalDevice);

        uint64_t guest_display;
        memcpy(&guest_display, *ptr, sizeof(uint64_t));
        *ptr += sizeof(uint64_t);
        VkDisplayKHR display = (VkDisplayKHR)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DISPLAY_KHR, guest_display);

        VkDisplayModeCreateInfoKHR createInfo;
        decode_from_stream_VkDisplayModeCreateInfoKHR(VK_STRUCTURE_TYPE_MAX_ENUM, &createInfo, ptr);

        uint64_t guest_allocator_ptr;
        memcpy(&guest_allocator_ptr, *ptr, 8);
        *ptr += 8;

        const VkAllocationCallbacks* pAllocator = NULL;
        if (guest_allocator_ptr) {
            VkAllocationCallbacks allocator;
            decode_from_stream_VkAllocationCallbacks(VK_STRUCTURE_TYPE_MAX_ENUM, &allocator, ptr);
            pAllocator = &allocator;
        }

        uint64_t guest_mode;
        memcpy(&guest_mode, *ptr, sizeof(uint64_t));
        *ptr += sizeof(uint64_t);

        VkDisplayModeKHR mode;
        VkResult result = vkCreateDisplayModeKHR(physicalDevice, display, &createInfo, pAllocator, &mode);

        if (result == VK_SUCCESS) {
            insert_mapping(EXPRESS_VK_OBJECT_TYPE_DISPLAY_MODE_KHR, guest_mode, (uint64_t)(uintptr_t)mode);
        }

        write_to_guest_mem(all_para[1].data, &result, 0, sizeof(VkResult));
        LOGD("CreateDisplayModeKHR result %d", result);
    }
    break;

    case FUNID_vkCreateDisplayPlaneSurfaceKHR:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_instance;
        memcpy(&guest_instance, *ptr, sizeof(uint64_t));
        *ptr += sizeof(uint64_t);
        VkInstance instance = (VkInstance)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_INSTANCE, guest_instance);

        VkDisplaySurfaceCreateInfoKHR createInfo;
        decode_from_stream_VkDisplaySurfaceCreateInfoKHR(VK_STRUCTURE_TYPE_MAX_ENUM, &createInfo, ptr);

        uint64_t guest_allocator_ptr;
        memcpy(&guest_allocator_ptr, *ptr, 8);
        *ptr += 8;

        const VkAllocationCallbacks* pAllocator = NULL;
        if (guest_allocator_ptr) {
            VkAllocationCallbacks allocator;
            decode_from_stream_VkAllocationCallbacks(VK_STRUCTURE_TYPE_MAX_ENUM, &allocator, ptr);
            pAllocator = &allocator;
        }

        uint64_t guest_surface;
        memcpy(&guest_surface, *ptr, sizeof(uint64_t));
        *ptr += sizeof(uint64_t);

        VkSurfaceKHR surface;
        VkResult result = vkCreateDisplayPlaneSurfaceKHR(instance, &createInfo, pAllocator, &surface);

        if (result == VK_SUCCESS) {
            insert_mapping(EXPRESS_VK_OBJECT_TYPE_SURFACE_KHR, guest_surface, (uint64_t)(uintptr_t)surface);
        }

        write_to_guest_mem(all_para[1].data, &result, 0, sizeof(VkResult));
        LOGD("CreateDisplayPlaneSurfaceKHR result %d", result);
    }
    break;

    case FUNID_vkGetDeviceGroupPresentCapabilitiesKHR:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_device;
        memcpy(&guest_device, *ptr, sizeof(uint64_t));
        *ptr += sizeof(uint64_t);
        VkDevice device = (VkDevice)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_device);

        VkDeviceGroupPresentCapabilitiesKHR capabilities;
        VkResult result = vkGetDeviceGroupPresentCapabilitiesKHR(device, &capabilities);

        write_to_guest_mem(all_para[1].data, &capabilities, 0, sizeof(VkDeviceGroupPresentCapabilitiesKHR));
        write_to_guest_mem(all_para[2].data, &result, 0, sizeof(VkResult));
        LOGD("GetDeviceGroupPresentCapabilitiesKHR result %d", result);
    }
    break;

    case FUNID_vkGetDeviceGroupSurfacePresentModesKHR:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_device;
        memcpy(&guest_device, *ptr, sizeof(uint64_t));
        *ptr += sizeof(uint64_t);
        VkDevice device = (VkDevice)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE, guest_device);

        uint64_t guest_surface;
        memcpy(&guest_surface, *ptr, sizeof(uint64_t));
        *ptr += sizeof(uint64_t);
        VkSurfaceKHR surface = (VkSurfaceKHR)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_SURFACE_KHR, guest_surface);

        VkDeviceGroupPresentModeFlagsKHR modes;
        VkResult result = vkGetDeviceGroupSurfacePresentModesKHR(device, surface, &modes);

        write_to_guest_mem(all_para[1].data, &modes, 0, sizeof(VkDeviceGroupPresentModeFlagsKHR));
        write_to_guest_mem(all_para[2].data, &result, 0, sizeof(VkResult));
        LOGD("GetDeviceGroupSurfacePresentModesKHR result %d", result);
    }
    break;

    case FUNID_vkGetDisplayPlaneCapabilities2KHR:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_physicalDevice;
        memcpy(&guest_physicalDevice, *ptr, sizeof(uint64_t));
        *ptr += sizeof(uint64_t);
        VkPhysicalDevice physicalDevice = (VkPhysicalDevice)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_PHYSICAL_DEVICE, guest_physicalDevice);

        VkDisplayPlaneInfo2KHR planeInfo;
        decode_from_stream_VkDisplayPlaneInfo2KHR(VK_STRUCTURE_TYPE_MAX_ENUM, &planeInfo, ptr);

        VkDisplayPlaneCapabilities2KHR capabilities;
        VkResult result = vkGetDisplayPlaneCapabilities2KHR(physicalDevice, &planeInfo, &capabilities);

        write_to_guest_mem(all_para[1].data, &capabilities, 0, sizeof(VkDisplayPlaneCapabilities2KHR));
        write_to_guest_mem(all_para[2].data, &result, 0, sizeof(VkResult));
        LOGD("GetDisplayPlaneCapabilities2KHR result %d", result);
    }
    break;

    case FUNID_vkGetDisplayPlaneCapabilitiesKHR:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_physicalDevice;
        memcpy(&guest_physicalDevice, *ptr, sizeof(uint64_t));
        *ptr += sizeof(uint64_t);
        VkPhysicalDevice physicalDevice = (VkPhysicalDevice)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_PHYSICAL_DEVICE, guest_physicalDevice);

        uint64_t guest_mode;
        memcpy(&guest_mode, *ptr, sizeof(uint64_t));
        *ptr += sizeof(uint64_t);
        VkDisplayModeKHR mode = (VkDisplayModeKHR)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DISPLAY_MODE_KHR, guest_mode);

        uint32_t planeIndex;
        memcpy(&planeIndex, *ptr, sizeof(uint32_t));
        *ptr += sizeof(uint32_t);

        VkDisplayPlaneCapabilitiesKHR capabilities;
        VkResult result = vkGetDisplayPlaneCapabilitiesKHR(physicalDevice, mode, planeIndex, &capabilities);

        write_to_guest_mem(all_para[1].data, &capabilities, 0, sizeof(VkDisplayPlaneCapabilitiesKHR));
        write_to_guest_mem(all_para[2].data, &result, 0, sizeof(VkResult));
        LOGD("GetDisplayPlaneCapabilitiesKHR result %d", result);
    }
    break;

    case FUNID_vkGetDisplayPlaneSupportedDisplaysKHR:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_physicalDevice;
        memcpy(&guest_physicalDevice, *ptr, sizeof(uint64_t));
        *ptr += sizeof(uint64_t);
        VkPhysicalDevice physicalDevice = (VkPhysicalDevice)(uintptr_t)lookup_mapping(EXPRESS_VK_OBJECT_TYPE_PHYSICAL_DEVICE, guest_physicalDevice);

        uint32_t planeIndex;
        memcpy(&planeIndex, *ptr, sizeof(uint32_t));
        *ptr += sizeof(uint32_t);

        uint32_t count = 0;
        if (!copy_from_call_para_fast(all_para[1], &count, sizeof(uint32_t))) {
            LOGE("Host: GetDisplayPlaneSupportedDisplaysKHR failed to read count");
            break;
        }

        VkResult result;
        if (count == 0) {
            result = vkGetDisplayPlaneSupportedDisplaysKHR(physicalDevice, planeIndex, &count, NULL);
            write_to_guest_mem(all_para[1].data, &count, 0, sizeof(uint32_t));
        } else {
            VkDisplayKHR* displays = (VkDisplayKHR*)malloc(count * sizeof(VkDisplayKHR));
            uint64_t* guest_displays = (uint64_t*)malloc(count * sizeof(uint64_t));
            if (!guest_displays ||
                !copy_from_call_para_fast(all_para[2], guest_displays, count * sizeof(uint64_t))) {
                free(displays);
                LOGE("Host: GetDisplayPlaneSupportedDisplaysKHR failed to get guest display handles");
                if (guest_displays) free(guest_displays);
                break;
            }

            result = vkGetDisplayPlaneSupportedDisplaysKHR(physicalDevice, planeIndex, &count, displays);

            for (uint32_t i = 0; i < count; ++i) {
                insert_mapping(EXPRESS_VK_OBJECT_TYPE_DISPLAY_KHR, guest_displays[i], (uint64_t)(uintptr_t)displays[i]);
            }

            free(displays);
            free(guest_displays);
        }

        write_to_guest_mem(all_para[para_num - 1].data, &result, 0, sizeof(VkResult));
        LOGD("GetDisplayPlaneSupportedDisplaysKHR result %d", result);
    }
    break;

    case FUNID_vkGetMemoryNativeBufferOHOS:
    {
        int para_num = get_para_from_call(call, all_para, MAX_PARA_NUM);
        int need_free = 0;
        char* stream = call_para_to_ptr(all_para[0], &need_free);
        uint8_t** ptr = (uint8_t**)&stream;

        uint64_t guest_memory;
        uint64_t ret_handle;
        memcpy(&guest_memory, *ptr, sizeof(uint64_t));

        uint64_t memory = (uint64_t)(uintptr_t)
            lookup_mapping(EXPRESS_VK_OBJECT_TYPE_DEVICE_MEMORY, guest_memory);

        uint64_t gbuffer_id = lookup_memory_gbuffer_mapping(memory);

        if (gbuffer_id == 0) {
            LOGE("GetMemoryNativeBufferOHOS: cannot find native buffer for memory %llx", memory);
            ret_handle = 0;
            write_to_guest_mem(all_para[1].data, &ret_handle, 0, sizeof(uint64_t));
        } else {
            Hardware_Buffer* nativeBuffer = get_gbuffer_from_global_map(gbuffer_id);
            if (nativeBuffer == NULL) {
                LOGE("GetMemoryNativeBufferOHOS: get null native buffer for gbuffer id %llx", gbuffer_id);
                ret_handle = 0;
                write_to_guest_mem(all_para[1].data, &ret_handle, 0, sizeof(uint64_t));
                break;
            }
            ret_handle = nativeBuffer->vk_buffer_handle;
            write_to_guest_mem(all_para[1].data, &ret_handle, 0, sizeof(uint64_t));
            LOGD("GetMemoryNativeBufferOHOS ptr %llx handle %llx", memory, ret_handle);
        }
    }
    break;

    default:
        LOGE("Unhandled Vulkan function ID: %d", fun_id);
        break;

    }
    call->callback(call, 1);
}
