/*
 * Copyright 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <hardware/hwvulkan.h>

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <string.h>
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <unwind.h>
#include <dlfcn.h>
#include <vector>
#include <atomic>


#include <algorithm>
#include <array>
#include <climits>
#include <utility>

#include <log/log.h>
#include "define_vk.h"
#include "ParamManager.h"
#include "express_vk_flime_guest.h"

#ifndef EXPRESS_VK_WRAPPER_INFO_LOG
#define EXPRESS_VK_WRAPPER_INFO_LOG 0
#endif

#if !EXPRESS_VK_WRAPPER_INFO_LOG
#undef ALOGI
#undef ALOGD
#define ALOGI(...) ((void)0)
#define ALOGD(...) ((void)0)
#endif

#include "express_vk_deepcopy_guest.h"
#include "all_out_vk_gen.h"
#include "express_vk_counting_guest.h"
#include "express_vk_encode_to_stream.h"
#include "gralloc_express.h"
#include <unordered_map>
#include <unordered_set>


#ifndef EXPRESS_VK_PER_CALL_LOG
#define EXPRESS_VK_PER_CALL_LOG 0
#endif

#if EXPRESS_VK_PER_CALL_LOG
#define EVK_PER_CALL_LOG(...) ALOGI(__VA_ARGS__)
#else
#define EVK_PER_CALL_LOG(...) ((void)0)
#endif

#ifndef EXPRESS_VK_GUEST_MEM_TRACE
#define EXPRESS_VK_GUEST_MEM_TRACE 0
#endif

#if EXPRESS_VK_GUEST_MEM_TRACE
#define GUEST_MEM_TRACE(...) \
    __android_log_print(ANDROID_LOG_INFO, "ExpressVkMemTrace", __VA_ARGS__)
#else
#define GUEST_MEM_TRACE(...) ((void)0)
#endif


using namespace null_driver;

#ifndef FUNID_vkExpressRegisterMappedMemoryANDROID
#define FUNID_vkExpressRegisterMappedMemoryANDROID 1902
#endif

#ifndef FUNID_vkExpressUnregisterMappedMemoryANDROID
#define FUNID_vkExpressUnregisterMappedMemoryANDROID 1903
#endif

#ifndef FUNID_vkExpressWaitFenceAndInvalidateANDROID
#define FUNID_vkExpressWaitFenceAndInvalidateANDROID 1904
#endif

static constexpr const char* kExpressVkMemDevicePath = "/dev/express_mem";
static constexpr uint64_t kExpressVkMemMaxMmapSize = 255ull * 1024ull * 1024ull;

static int get_express_gpu_fd();
namespace null_driver {
static void ForgetDescriptorUpdateTemplateInfo(
    VkDescriptorUpdateTemplate descriptor_update_template);
}

static uint64_t AlignUpToPage(uint64_t size) {
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) page_size = 4096;
    uint64_t page = (uint64_t)page_size;
    return (size + page - 1) & ~(page - 1);
}

static char* TryMmapExpressVkMem(uint64_t requested_size, int* out_fd, uint64_t* out_map_size) {
    if (!out_fd || !out_map_size || requested_size == 0 || requested_size > kExpressVkMemMaxMmapSize) {
        return nullptr;
    }

    uint64_t map_size = AlignUpToPage(requested_size);
    int fd = open(kExpressVkMemDevicePath, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        ALOGW("[ExpressVkMem] open %s failed errno=%d, fallback to heap", kExpressVkMemDevicePath, errno);
        return nullptr;
    }

    void* ptr = mmap(nullptr, (size_t)map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) {
        ALOGW("[ExpressVkMem] mmap size=%llu failed errno=%d, fallback to heap",
              (unsigned long long)map_size, errno);
        close(fd);
        return nullptr;
    }

    *out_fd = fd;
    *out_map_size = map_size;
    ALOGI("[ExpressVkMem] mmap success requested=%llu mapped=%llu ptr=%p fd=%d",
          (unsigned long long)requested_size,
          (unsigned long long)map_size,
          ptr,
          fd);
    return (char*)ptr;
}

static void RegisterExpressVkMappedMemory(VkDevice device, VkDeviceMemory memory, char* ptr, uint64_t size) {
    if (!ptr || size == 0) return;

    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;

    mgr.addParam64((uint64_t)(uintptr_t)device);
    mgr.addParam64((uint64_t)(uintptr_t)memory);
    mgr.addParam64(size);
    mgr.addPtr(ptr, (int)size);
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkExpressRegisterMappedMemoryANDROID, true);

    if (size >= 1024ull * 1024ull) {
        GUEST_MEM_TRACE("[GUEST_MEM_TRACE] register device=0x%llx memory=0x%llx ptr=%p size=%llu size_mb=%llu",
                        (unsigned long long)(uintptr_t)device,
                        (unsigned long long)(uintptr_t)memory,
                        ptr,
                        (unsigned long long)size,
                        (unsigned long long)(size / (1024ull * 1024ull)));
    }
    ALOGI("[ExpressVkMem] registered device=%llx memory=%llx ptr=%p size=%llu",
          (unsigned long long)(uintptr_t)device,
          (unsigned long long)(uintptr_t)memory,
          ptr,
          (unsigned long long)size);
}

static void UnregisterExpressVkMappedMemory(VkDevice device, VkDeviceMemory memory) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;

    mgr.addParam64((uint64_t)(uintptr_t)device);
    mgr.addParam64((uint64_t)(uintptr_t)memory);
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkExpressUnregisterMappedMemoryANDROID, true);

    GUEST_MEM_TRACE("[GUEST_MEM_TRACE] unregister device=0x%llx memory=0x%llx",
                    (unsigned long long)(uintptr_t)device,
                    (unsigned long long)(uintptr_t)memory);
    ALOGI("[ExpressVkMem] unregistered device=%llx memory=%llx",
          (unsigned long long)(uintptr_t)device,
          (unsigned long long)(uintptr_t)memory);
}

struct VkPhysicalDevice_T {
    hwvulkan_dispatch_t dispatch;
};

struct VkInstance_T {
    hwvulkan_dispatch_t dispatch;
    VkAllocationCallbacks allocator;
    VkPhysicalDevice_T physical_device;
    uint64_t next_callback_handle;
};

struct VkQueue_T {
    hwvulkan_dispatch_t dispatch;
};

struct VkCommandBuffer_T {
    hwvulkan_dispatch_t dispatch;
};

struct VkCommandPool_T {
    hwvulkan_dispatch_t dispatch;
};

struct VkFence_T {
    hwvulkan_dispatch_t dispatch;
};
struct VkSemaphore_T {
    hwvulkan_dispatch_t dispatch;
};
// Non-dispatchable Vulkan object types
struct VkBuffer_T { };
struct VkBufferView_T { };
struct VkImage_T { };
struct VkImageView_T { };
struct VkRenderPass_T { };
struct VkPipeline_T { };
struct VkPipelineLayout_T { };
struct VkPipelineCache_T { };
struct VkDescriptorPool_T { };
struct VkDescriptorSet_T { };
struct VkDescriptorSetLayout_T { };
struct VkSampler_T { };
struct VkFramebuffer_T { };
struct VkEvent_T { };
struct VkQueryPool_T { };
struct VkShaderModule_T { };

// Additional object types used by FUNIDs
struct VkDeviceMemory_T {
    char* map_data;
    uint64_t offset;
    uint64_t length;
    uint32_t target;
    uint32_t access;
    uint8_t express_vk_mem_registered;
    int express_vk_mem_fd;
    uint64_t express_vk_mem_map_size;
};
struct VkDescriptorUpdateTemplate_T { };
struct VkPrivateDataSlot_T { };
struct VkRenderPass2_T { };
struct VkPipelineExecutableKHR_T { };
struct VkSwapchainKHR_T { };
struct VkSurfaceKHR_T { };
struct VkDisplayKHR_T { };
struct VkDisplayModeKHR_T { };
struct VkSamplerYcbcrConversion_T { };

namespace {
// Handles for non-dispatchable objects are either pointers, or arbitrary
// 64-bit non-zero values. We only use pointers when we need to keep state for
// the object even in a null driver. For the rest, we form a handle as:
//   [63:63] = 1 to distinguish from pointer handles*
//   [62:56] = non-zero handle type enum value
//   [55: 0] = per-handle-type incrementing counter
// * This works because virtual addresses with the high bit set are reserved
// for kernel data in all ABIs we run on.
//
// We never reclaim handles on vkDestroy*. It's not even necessary for us to
// have distinct handles for live objects, and practically speaking we won't
// ever create 2^56 objects of the same type from a single VkDevice in a null
// driver.
//
// Using a namespace here instead of 'enum class' since we want scoped
// constants but also want implicit conversions to integral types.
namespace HandleType {
enum Enum {
    kBufferView,
    kDebugReportCallbackEXT,
    kDescriptorPool,
    kDescriptorSet,
    kDescriptorSetLayout,
    kEvent,
    kFence,
    kFramebuffer,
    kImageView,
    kPipeline,
    kPipelineCache,
    kPipelineLayout,
    kQueryPool,
    kRenderPass,
    kSampler,
    kSemaphore,
    kShaderModule,

    kNumTypes
};
}  // namespace HandleType

const VkDeviceSize kMaxDeviceMemory = 0x10000000;  // 256 MiB, arbitrary

}  // anonymous namespace

struct VkDevice_T {
    hwvulkan_dispatch_t dispatch;
    VkAllocationCallbacks allocator;
    VkInstance_T* instance;
    VkQueue_T queue;
    std::array<uint64_t, HandleType::kNumTypes> next_handle;
};

#include <mutex>

static int g_express_gpu_fd = -1;
static std::mutex g_express_gpu_mutex;

static int get_express_gpu_fd() {
    std::lock_guard<std::mutex> lock(g_express_gpu_mutex);
    
    if (g_express_gpu_fd < 0) {
        g_express_gpu_fd = open(EXPRESS_GPU_NAME, O_RDWR);
        if (g_express_gpu_fd < 0) {
            LOGE("Failed to open %s: %s", EXPRESS_GPU_NAME, strerror(errno));
        }
    }
    
    return g_express_gpu_fd;
}

/*
 * ParamManager::write() returns the bytes consumed from its descriptor array,
 * not the Vulkan result.  A call with an inline parameter block plus N pointer
 * parameters therefore writes the fixed framing below.  Synchronous Vulkan
 * wrappers use this check after the host has copied VkResult back through the
 * final addPtr(); a short transport write always wins over that result.
 */
static bool IsCompleteParamManagerWrite(ssize_t written,
                                        size_t rpc_parameter_count) {
    const size_t expected =
        sizeof(uint64_t) * 2u +
        sizeof(ParamManager::SendParam) * rpc_parameter_count;
    return written >= 0 && static_cast<size_t>(written) == expected;
}


#include <unordered_map>
#include <mutex>
#include <vector>

struct MemoryRangeSpan {
    VkDeviceSize offset;
    VkDeviceSize size;
    uint64_t generation;
};

struct ActiveMappedMemoryRecord {
    VkDevice device;
    VkDeviceMemory memory;
    uint64_t size;
    uint8_t* map_data;
    std::vector<uint8_t> shadow;
    std::vector<MemoryRangeSpan> recently_flushed_ranges;
    std::vector<MemoryRangeSpan> recently_invalidated_ranges;
    std::vector<MemoryRangeSpan> recently_clean_submit_ranges;
    uint32_t submit_clean_streak;
    uint64_t last_submit_generation;
};

struct BufferMemoryBindingRecord {
    VkDevice device;
    VkDeviceMemory memory;
    VkDeviceSize memory_offset;
    VkDeviceSize buffer_size;
};

enum class DescriptorBufferAccess : uint8_t {
    kFlushOnly = 0,
    kPotentialWrite = 1,
};

struct DescriptorBufferUse {
    VkBuffer buffer;
    VkDeviceSize offset;
    VkDeviceSize range;
    uint32_t descriptor_type;
    DescriptorBufferAccess access;
    bool uses_dynamic_offset;
};

struct TrackedMemoryRange {
    VkDevice device;
    VkDeviceMemory memory;
    VkDeviceSize offset;
    VkDeviceSize size;
};

enum class OutputHintSource : uint8_t {
    kDescriptorMaybeWrite = 0,
    kCopyBufferDst = 1,
};

enum class OutputHintStrength : uint8_t {
    kWeakMaybeWrite = 0,
    kStrongReadback = 1,
};

struct OutputMemoryRangeHint {
    TrackedMemoryRange range;
    OutputHintSource source;
    OutputHintStrength strength;
    bool wait_commit_eligible;
};

struct DescriptorSetSyncSignature {
    uint64_t version;
    uint64_t hash;
};

struct DescriptorSetSyncHintCacheEntry {
    uint64_t version = 0;
    uint64_t hash = 0;
    std::vector<TrackedMemoryRange> flush_ranges;
    std::vector<OutputMemoryRangeHint> output_hints;
    uint64_t flush_bytes = 0;
    uint64_t output_bytes = 0;
};


struct BufferSyncRange {
    VkBuffer buffer;
    VkDeviceSize offset;
    VkDeviceSize size;
};

struct SubmitSyncHints {
    bool present = false;
    std::vector<TrackedMemoryRange> flush_ranges;
    std::vector<TrackedMemoryRange> wait_flush_ranges;
    std::vector<OutputMemoryRangeHint> output_hints;
};

namespace null_driver {
static void MergeSubmitSyncHints(SubmitSyncHints* dst, const SubmitSyncHints& src);
}

struct ExpressVkSubmitRangeHintWire {
    uint64_t device;
    uint64_t memory;
    uint64_t offset;
    uint64_t size;
};

static constexpr uint32_t kExpressVkSubmitHintsMagic = 0x48564b45u; // "EKVH"
static constexpr uint32_t kExpressVkSubmitHintsVersion = 1;

static std::unordered_map<VkDeviceMemory, ActiveMappedMemoryRecord> g_active_mapped_memories;
static std::unordered_map<VkBuffer, BufferMemoryBindingRecord> g_buffer_memory_bindings;
static std::unordered_map<VkBuffer, VkDeviceSize> g_buffer_sizes;
static std::unordered_map<VkDescriptorSet, std::unordered_map<uint64_t, DescriptorBufferUse>> g_descriptor_set_buffer_uses;
static std::unordered_map<VkDescriptorSet, uint64_t> g_descriptor_set_versions;
static std::unordered_map<VkDescriptorSet, DescriptorSetSyncHintCacheEntry> g_descriptor_set_hint_cache;
static uint64_t g_descriptor_set_global_version = 1;
static std::unordered_map<VkDescriptorSet, VkDescriptorPool> g_descriptor_set_to_pool;
static std::unordered_map<VkDescriptorPool, std::unordered_set<VkDescriptorSet>> g_descriptor_pool_sets;
static std::unordered_map<VkCommandBuffer, std::unordered_set<VkDescriptorSet>> g_command_buffer_descriptor_sets;
static std::unordered_map<VkCommandBuffer, std::unordered_map<VkDescriptorSet, std::unordered_map<uint64_t, std::vector<uint32_t>>>> g_command_buffer_descriptor_dynamic_offsets;
static std::unordered_set<VkCommandBuffer> g_command_buffer_unknown_dynamic_offsets;
static std::unordered_map<VkCommandBuffer, std::vector<BufferSyncRange>> g_command_buffer_flush_buffer_ranges;
static std::unordered_map<VkCommandBuffer, std::vector<BufferSyncRange>> g_command_buffer_invalidate_buffer_ranges;
struct CommandBufferSubmitHintCacheEntry {
    uint64_t epoch = 0;
    SubmitSyncHints hints;
};
static std::unordered_map<uint64_t, CommandBufferSubmitHintCacheEntry> g_signature_submit_hint_cache;

static uint64_t ComputeCommandBufferSyncSignatureLocked(VkCommandBuffer cb) {
    uint64_t hash = 14695981039346656037ull;
    auto ds_it = g_command_buffer_descriptor_sets.find(cb);
    if (ds_it != g_command_buffer_descriptor_sets.end()) {
        for (VkDescriptorSet ds : ds_it->second) {
            hash ^= (uint64_t)(uintptr_t)ds; hash *= 1099511628211ull;
            auto version_it = g_descriptor_set_versions.find(ds);
            const uint64_t version =
                version_it != g_descriptor_set_versions.end() ? version_it->second : 0;
            hash ^= version; hash *= 1099511628211ull;
        }
    }
    auto fl_it = g_command_buffer_flush_buffer_ranges.find(cb);
    if (fl_it != g_command_buffer_flush_buffer_ranges.end()) {
        for (const auto& r : fl_it->second) {
            hash ^= (uint64_t)(uintptr_t)r.buffer; hash *= 1099511628211ull;
            hash ^= r.offset; hash *= 1099511628211ull;
            hash ^= r.size; hash *= 1099511628211ull;
        }
    }
    auto in_it = g_command_buffer_invalidate_buffer_ranges.find(cb);
    if (in_it != g_command_buffer_invalidate_buffer_ranges.end()) {
        for (const auto& r : in_it->second) {
            hash ^= (uint64_t)(uintptr_t)r.buffer; hash *= 1099511628211ull;
            hash ^= r.offset; hash *= 1099511628211ull;
            hash ^= r.size; hash *= 1099511628211ull;
        }
    }
    return hash;
}

static uint64_t g_submit_hint_cache_epoch = 1;
static std::unordered_map<VkFence, std::vector<TrackedMemoryRange>> g_fence_pending_invalidate_ranges;
static std::unordered_set<VkDeviceMemory> g_flush_hint_memories;
static std::unordered_set<VkDeviceMemory> g_invalidate_hint_memories;
static std::unordered_set<VkDeviceMemory> g_recently_flushed_memories;
static std::vector<TrackedMemoryRange> g_flush_hint_ranges;
static std::vector<TrackedMemoryRange> g_invalidate_hint_ranges;
static std::vector<TrackedMemoryRange> g_pending_upload_wait_ranges;
static std::unordered_map<VkFence, VkQueue> g_deferred_fence_queues;
static std::unordered_set<VkQueue> g_deferred_wait_queues;
static std::unordered_set<VkFence> g_completed_deferred_fences;
static bool g_skip_next_implicit_flush_scan = false;
static std::mutex g_mapped_mutex;
static std::mutex g_submit_hint_stats_mutex;
static std::vector<std::vector<uint8_t>> g_shadow_buffer_pool;
static size_t g_shadow_buffer_pool_bytes = 0;
static constexpr size_t kMaxShadowBufferPoolBytes = 512ull * 1024ull * 1024ull;
static constexpr size_t kMaxShadowBufferPoolEntries = 8;
static uint64_t g_submit_generation = 0;
static constexpr VkDeviceSize kImplicitInvalidateSmallFallbackBytes = 4ull * 1024ull * 1024ull;
static constexpr VkDeviceSize kImplicitInvalidateLargeFallbackLimitBytes = 16ull * 1024ull * 1024ull;
static constexpr VkDeviceSize kImplicitTinyControlBytes = 256;
static constexpr VkDeviceSize kRegisteredShadowUpdateCopyLimitBytes = 4ull * 1024ull * 1024ull;
// Shadow is the host-upload dirty baseline. After a host->guest invalidate,
// keeping the old shadow can make later guest writes look clean even though the
// host still contains the GPU-written data. Keep correctness first here.
static constexpr bool kAllowLargeRegisteredShadowUpdateSkip = false;
static constexpr VkDeviceSize kDescriptorEarlyUploadMinBytes = 256ull * 1024ull;
static constexpr VkDeviceSize kCommandEarlyUploadMinBytes = 1024ull * 1024ull;
static constexpr VkDeviceSize kEarlyUploadMaxBytes = 16ull * 1024ull * 1024ull;
static constexpr VkDeviceSize kEarlyUploadShadowRefreshLimitBytes = 16ull * 1024ull * 1024ull;
static constexpr VkDeviceSize kCommandAutoInvalidateMaxBytes = 4ull * 1024ull * 1024ull;
static constexpr VkDeviceSize kWaitCommittedInvalidateMaxBytes = 4ull * 1024ull * 1024ull;
static constexpr VkDeviceSize kHostAsyncUploadThresholdBytes = 4ull * 1024ull * 1024ull;

enum class ExpressVkSyncCorrectnessMode : uint8_t {
    // Optimized mode: descriptor/copy hints, clean caches, and throttles may
    // narrow synchronization work. Use only after comparing against strict mode.
    kOptimizedHints = 0,
    // Strict oracle: every submit scans all active mapped memory against shadow;
    // every CPU-visible readback boundary refreshes all active mapped memory.
    kStrictShadowOracle = 1,
};

static constexpr ExpressVkSyncCorrectnessMode kSyncCorrectnessMode =
    ExpressVkSyncCorrectnessMode::kStrictShadowOracle;

static constexpr bool UseStrictShadowOracleSync() {
    return kSyncCorrectnessMode ==
           ExpressVkSyncCorrectnessMode::kStrictShadowOracle;
}

enum class ExpressVkReadbackCorrectnessMode : uint8_t {
    // Current fast path: semantic output hints and bounded weak descriptor
    // readbacks. This is a prefetch policy, not a complete correctness proof.
    kHintPrefetchOnly = 0,
    // Correctness oracle: after CPU-visible synchronization, invalidate every
    // active mapped range that may be read by guest CPU code. Future dirty-page
    // implementations should replace the range producer, not the wait boundary.
    kConservativeMappedFallback = 1,
};

static constexpr ExpressVkReadbackCorrectnessMode kReadbackCorrectnessMode =
    ExpressVkReadbackCorrectnessMode::kConservativeMappedFallback;

static constexpr bool UseConservativeMappedReadbackFallback() {
    return kReadbackCorrectnessMode ==
           ExpressVkReadbackCorrectnessMode::kConservativeMappedFallback;
}
// Correctness-guarded lazy fence waits:
// - vkWaitForFences may return virtually for fence-only synchronization points.
// - Real waiting is forced at CPU-visible readback and command/memory lifetime
//   boundaries, so host work cannot outlive resources it uses.
// - Submit coalescing changes command-buffer lifetime timing. Keep it disabled
//   while the lazy-wait boundary logs tell us where the remaining cost sits.
// Correctness A/B: use real host fence waits while validating conservative sync.
static constexpr bool kEnableDeferredFenceWait = false;
static constexpr bool kEnableSubmitCoalescing = false;
static constexpr bool kEnableSignatureSubmitHintCache = false;

#ifndef EXPRESS_VK_ENABLE_DESCRIPTOR_HINT_CACHE
#define EXPRESS_VK_ENABLE_DESCRIPTOR_HINT_CACHE 1
#endif
static constexpr bool kEnableDescriptorSetSyncHintCache =
    EXPRESS_VK_ENABLE_DESCRIPTOR_HINT_CACHE != 0;
static constexpr bool kEnableWaitInvalidateFused = false;
static constexpr bool kEnableDeferredWeakDescriptorReadbackFence = true;
// Keeping weak descriptor output fences real made the hot path slower: it turns
// every per-dispatch fence wait into a real host vkWaitForFences. Keep weak
// maybe-write readback deferred, and reserve real host fences for strong output
// hints such as vkCmdCopyBuffer destinations.
static constexpr bool kPreferHostFenceForWeakDescriptorReadback = false;
static constexpr bool kSuppressWeakReadbackWhenStrongCopyPresent = true;
static constexpr uint32_t kSubmitCoalesceMaxBatch = 16;
static constexpr uint64_t kSubmitHintStatsLogEvery = 2048;
static constexpr uint64_t kSubmitRpcStatsLogEvery = 2048;
static constexpr uint64_t kSubmitCohortStatsLogEvery = 2048;
static constexpr uint64_t kSubmitCoalesceStatsLogEvery = 256;
static constexpr uint64_t kDeferredWaitStatsLogEvery = 256;
static constexpr uint64_t kSubmitHintCacheStatsLogEvery = 2048;
static constexpr uint64_t kDescriptorHintCacheStatsLogEvery = 2048;
static constexpr uint64_t kWaitInvalidateFusedStatsLogEvery = 256;
static constexpr uint64_t kLargeCleanRangeStatsLogEvery = 2048;
static constexpr uint64_t kReadbackFenceStatsLogEvery = 2048;
static constexpr uint64_t kImplicitFlushStatsLogEvery = 2048;
static constexpr uint64_t kMappedFlushStatsLogEvery = 2048;
static constexpr bool kEnableGuestSyncBreakdownLog = false;
static constexpr uint64_t kGuestSyncBreakdownLogIntervalUs = 250000;
static constexpr uint64_t kGuestSyncBreakdownForceEvents = 8;
static constexpr uint64_t kSyncPolicyStatsLogEvery = 2048;
static constexpr uint64_t kSlowWaitDiagLogUs = 5000;
static constexpr bool kEnableHintCleanRangeCache = !UseStrictShadowOracleSync();
static constexpr uint64_t kHintCleanCacheGenerationTtl = 1;
static constexpr size_t kMaxSubmitCleanRangeCacheEntries = 64;
static constexpr bool kEnableLargeCleanRangeVerifiedCache = !UseStrictShadowOracleSync();
static constexpr VkDeviceSize kLargeCleanRangeMinBytes = 16ull * 1024ull * 1024ull;
static constexpr uint64_t kLargeCleanRangeVerifyEvery = 16;
static constexpr uint64_t kLargeCleanRangeGenerationTtl = 512;
static constexpr VkDeviceSize kLargeCleanPartialVerifyMaxBytes = 1024ull * 1024ull;
static constexpr bool kEnableAggressiveCleanHintSkip = false;
static constexpr uint32_t kAggressiveCleanStreakThreshold = 17;
static constexpr uint64_t kAggressiveLargeCleanVerifyEvery = 512;
static constexpr uint64_t kAggressiveMediumCleanVerifyEvery = 128;
static constexpr bool kEnableDirectDirtySmallHintFlush = !UseStrictShadowOracleSync();
static constexpr bool kEnableNoHintCleanScanThrottle = !UseStrictShadowOracleSync();
static constexpr uint32_t kNoHintCleanScanWarmup = 16;
static constexpr uint32_t kNoHintCleanScanSkipBudget = 4;

// Perf tuning for matmul benchmark vs gfxstream:
// 1) Disable global implicit flush/invalidate on every submit/wait (very expensive for large mapped buffers).
// 2) Disable redundant map-time invalidate fallback since vkMapMemory now performs host->guest sync directly.
static constexpr bool kEnableImplicitGlobalMappedSync = true;
static constexpr bool kEnableMapTimeInvalidateFallback = UseStrictShadowOracleSync();
static constexpr bool kEnableDescriptorBindEarlyUpload = false;
static constexpr bool kEnableCommandCopyEarlyUpload = !UseStrictShadowOracleSync();
// Readback attribution is deliberately observational: it samples the first
// bytes after invalidate and compares them with the local shadow, without
// changing synchronization decisions. This tells us whether weak descriptor
// readbacks are actually refreshing data or just paying a conservative fence.
// Keep it off for timing runs; enable only when we need fresh attribution.
static constexpr bool kEnableReadbackChangeSampling = false;
static constexpr VkDeviceSize kReadbackChangeSampleBytes = 256;
static constexpr bool kEnableReadbackRangeSampleLog = false;
static constexpr VkDeviceSize kReadbackRangeSampleMinBytes = 1024ull * 1024ull;
static constexpr bool kEnableDescriptorTraceLog = false;
static constexpr VkDeviceSize kDescriptorTraceMinRangeBytes = 1024ull * 1024ull;
static constexpr uint32_t kDescriptorTraceMaxEntriesPerEvent = 12;

// Guest-side memory shape probe. This is intentionally observational: it does
// not change sync policy, descriptor hints, or host RPC payloads. Use the
// [MEM_SHAPE_*] log lines to see whether frameworks such as ncnn pack many
// small logical buffers into a few large VkDeviceMemory pools.
static constexpr bool kEnableMemShapeProbe = false;
static constexpr VkDeviceSize kMemShapeLargeAllocLogBytes = 1024ull * 1024ull;
static constexpr VkDeviceSize kMemShapeLargeBufferLogBytes = 1024ull * 1024ull;
static constexpr VkDeviceSize kMemShapeBlockBytes = 64ull * 1024ull;
static constexpr uint64_t kMemShapeSummaryEveryEvents = 4096;
static constexpr uint64_t kMemShapeSummaryEverySubmits = 64;
static constexpr size_t kMemShapeTopMemories = 8;
static constexpr size_t kMemShapeMaxTrackedBlocksPerMemory = 262144;
static uint64_t g_sync_trace_seq = 0;
static constexpr VkDeviceSize kImplicitDirtyChunkBytes = 4096;
static constexpr bool kEnableImplicitSyncDiagLog = false;
static constexpr bool kEnableLocalPerfLog = false;
static constexpr VkDeviceSize kGuestMemTraceLargeBytes = 1024ull * 1024ull;
static constexpr uint32_t kGuestMemTraceMaxRanges = 8;
static uint64_t g_flush_hint_rounds = 0;
static uint64_t g_invalidate_hint_rounds = 0;
static constexpr uint64_t kPeriodicFullSyncEvery = 0;
static uint32_t g_no_hint_clean_scan_streak = 0;
static uint32_t g_no_hint_clean_scan_skip_remaining = 0;

enum class MemShapeRangeKind : uint8_t {
    kDescriptorUpdate = 0,
    kDescriptorDispatch = 1,
    kDescriptorWriteDispatch = 2,
    kCopySrc = 3,
    kCopyDst = 4,
    kFlush = 5,
    kInvalidate = 6,
    kMap = 7,
};

struct MemShapeRangeStats {
    uint64_t count = 0;
    uint64_t bytes = 0;
    VkDeviceSize min_size = UINT64_MAX;
    VkDeviceSize max_size = 0;
    uint64_t whole_size_count = 0;
    uint64_t le_4k = 0;
    uint64_t le_64k = 0;
    uint64_t le_1m = 0;
    uint64_t gt_1m = 0;
};

struct MemShapeBufferRecord {
    VkDevice device = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    VkBufferUsageFlags usage = 0;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize memory_offset = 0;
    bool bound = false;
};

struct MemShapeMemoryRecord {
    VkDevice device = VK_NULL_HANDLE;
    VkDeviceSize allocation_size = 0;
    uint32_t memory_type_index = UINT32_MAX;
    bool express_registered = false;
    bool active_mapped = false;
    uint64_t bind_events = 0;
    uint64_t current_bound_buffers = 0;
    uint64_t map_count = 0;
    uint64_t unmap_count = 0;
    uint64_t descriptor_update_write_ranges = 0;
    uint64_t descriptor_dispatch_write_ranges = 0;
    uint64_t unresolved_descriptor_ranges = 0;
    uint64_t unresolved_copy_ranges = 0;
    bool block_tracking_saturated = false;
    std::unordered_set<uint64_t> touched_blocks_64k;
    MemShapeRangeStats descriptor_update;
    MemShapeRangeStats descriptor_dispatch;
    MemShapeRangeStats descriptor_write_dispatch;
    MemShapeRangeStats copy_src;
    MemShapeRangeStats copy_dst;
    MemShapeRangeStats flush;
    MemShapeRangeStats invalidate;
    MemShapeRangeStats map;
};

static std::unordered_map<VkDeviceMemory, MemShapeMemoryRecord> g_mem_shape_memories;
static std::unordered_map<VkBuffer, MemShapeBufferRecord> g_mem_shape_buffers;
static uint64_t g_mem_shape_events = 0;
static uint64_t g_mem_shape_last_summary_event = 0;
static uint64_t g_mem_shape_submit_calls = 0;
static uint64_t g_mem_shape_unresolved_global = 0;

static uint64_t MemShapeMb(uint64_t bytes) {
    return bytes / (1024ull * 1024ull);
}

static uint64_t MemShapeKb(uint64_t bytes) {
    return bytes / 1024ull;
}

static void MemShapeAddStats(MemShapeRangeStats* stats,
                             VkDeviceSize size,
                             bool whole_size) {
    if (!stats || size == 0) return;
    stats->count++;
    stats->bytes += (uint64_t)size;
    stats->min_size = std::min(stats->min_size, size);
    stats->max_size = std::max(stats->max_size, size);
    if (whole_size) {
        stats->whole_size_count++;
    }
    if (size <= 4ull * 1024ull) {
        stats->le_4k++;
    } else if (size <= 64ull * 1024ull) {
        stats->le_64k++;
    } else if (size <= 1024ull * 1024ull) {
        stats->le_1m++;
    } else {
        stats->gt_1m++;
    }
}

static uint64_t MemShapeStatsAvgKb(const MemShapeRangeStats& stats) {
    if (stats.count == 0) return 0;
    return MemShapeKb(stats.bytes / stats.count);
}

static uint64_t MemShapeStatsMinKb(const MemShapeRangeStats& stats) {
    return stats.count == 0 || stats.min_size == UINT64_MAX ?
        0 : MemShapeKb((uint64_t)stats.min_size);
}

static uint64_t MemShapeStatsMaxKb(const MemShapeRangeStats& stats) {
    return stats.count == 0 ? 0 : MemShapeKb((uint64_t)stats.max_size);
}

static MemShapeRangeStats* MemShapeStatsForKind(MemShapeMemoryRecord* rec,
                                                MemShapeRangeKind kind) {
    if (!rec) return nullptr;
    switch (kind) {
        case MemShapeRangeKind::kDescriptorUpdate:
            return &rec->descriptor_update;
        case MemShapeRangeKind::kDescriptorDispatch:
            return &rec->descriptor_dispatch;
        case MemShapeRangeKind::kDescriptorWriteDispatch:
            return &rec->descriptor_write_dispatch;
        case MemShapeRangeKind::kCopySrc:
            return &rec->copy_src;
        case MemShapeRangeKind::kCopyDst:
            return &rec->copy_dst;
        case MemShapeRangeKind::kFlush:
            return &rec->flush;
        case MemShapeRangeKind::kInvalidate:
            return &rec->invalidate;
        case MemShapeRangeKind::kMap:
            return &rec->map;
        default:
            return nullptr;
    }
}

static bool MemShapeResolveBufferRangeLocked(VkBuffer buffer,
                                             VkDeviceSize buffer_offset,
                                             VkDeviceSize buffer_range,
                                             VkDeviceMemory* out_memory,
                                             VkDeviceSize* out_offset,
                                             VkDeviceSize* out_size,
                                             bool* out_whole_size) {
    if (!out_memory || !out_offset || !out_size || !out_whole_size ||
        buffer == VK_NULL_HANDLE) {
        return false;
    }

    auto binding_it = g_buffer_memory_bindings.find(buffer);
    if (binding_it == g_buffer_memory_bindings.end() ||
        binding_it->second.memory == VK_NULL_HANDLE) {
        return false;
    }

    VkDeviceSize buffer_size = binding_it->second.buffer_size;
    if (buffer_size == 0) {
        auto size_it = g_buffer_sizes.find(buffer);
        if (size_it != g_buffer_sizes.end()) {
            buffer_size = size_it->second;
        }
    }
    if (buffer_size == 0 || buffer_offset > buffer_size) {
        return false;
    }

    const bool whole_size = (buffer_range == VK_WHOLE_SIZE);
    VkDeviceSize resolved_size = buffer_range;
    if (whole_size || buffer_offset + resolved_size > buffer_size) {
        resolved_size = buffer_size - buffer_offset;
    }
    if (resolved_size == 0) {
        return false;
    }

    *out_memory = binding_it->second.memory;
    *out_offset = binding_it->second.memory_offset + buffer_offset;
    *out_size = resolved_size;
    *out_whole_size = whole_size;
    return true;
}

static uint64_t MemShapeMemoryActivityBytes(const MemShapeMemoryRecord& rec) {
    return rec.descriptor_update.bytes +
           rec.descriptor_dispatch.bytes +
           rec.copy_src.bytes +
           rec.copy_dst.bytes +
           rec.flush.bytes +
           rec.invalidate.bytes +
           rec.map.bytes;
}

static uint64_t MemShapeMemoryRangeCount(const MemShapeMemoryRecord& rec) {
    return rec.descriptor_update.count +
           rec.descriptor_dispatch.count +
           rec.copy_src.count +
           rec.copy_dst.count +
           rec.flush.count +
           rec.invalidate.count +
           rec.map.count;
}

static void MemShapeLogOneMemoryLocked(const char* tag,
                                       uint32_t rank,
                                       VkDeviceMemory memory,
                                       const MemShapeMemoryRecord& rec) {
    const MemShapeRangeStats& desc = rec.descriptor_dispatch.count != 0 ?
        rec.descriptor_dispatch : rec.descriptor_update;
    ALOGI("[MEM_SHAPE_%s] rank=%u memory=%llx alloc_mb=%llu type=%u registered=%d "
          "active_mapped=%d buffers=%llu bind_events=%llu maps=%llu unmaps=%llu "
          "blocks64k=%zu block_saturated=%d activity_mb=%llu "
          "desc_upd_ranges=%llu desc_upd_mb=%llu desc_write_upd_ranges=%llu "
          "desc_dispatch_ranges=%llu desc_dispatch_mb=%llu "
          "desc_write_dispatch_ranges=%llu "
          "desc_write_dispatch_mb=%llu copy_src_ranges=%llu copy_src_mb=%llu "
          "copy_dst_ranges=%llu copy_dst_mb=%llu flush_ranges=%llu flush_mb=%llu "
          "invalidate_ranges=%llu invalidate_mb=%llu map_ranges=%llu map_mb=%llu "
          "desc_min_kb=%llu desc_avg_kb=%llu desc_max_kb=%llu desc_le4k=%llu "
          "desc_le64k=%llu desc_le1m=%llu desc_gt1m=%llu unresolved_desc=%llu "
          "unresolved_copy=%llu",
          tag ? tag : "TOP",
          rank,
          (unsigned long long)(uintptr_t)memory,
          (unsigned long long)MemShapeMb((uint64_t)rec.allocation_size),
          rec.memory_type_index,
          (int)rec.express_registered,
          (int)rec.active_mapped,
          (unsigned long long)rec.current_bound_buffers,
          (unsigned long long)rec.bind_events,
          (unsigned long long)rec.map_count,
          (unsigned long long)rec.unmap_count,
          rec.touched_blocks_64k.size(),
          (int)rec.block_tracking_saturated,
          (unsigned long long)MemShapeMb(MemShapeMemoryActivityBytes(rec)),
          (unsigned long long)rec.descriptor_update.count,
          (unsigned long long)MemShapeMb(rec.descriptor_update.bytes),
          (unsigned long long)rec.descriptor_update_write_ranges,
          (unsigned long long)rec.descriptor_dispatch.count,
          (unsigned long long)MemShapeMb(rec.descriptor_dispatch.bytes),
          (unsigned long long)rec.descriptor_write_dispatch.count,
          (unsigned long long)MemShapeMb(rec.descriptor_write_dispatch.bytes),
          (unsigned long long)rec.copy_src.count,
          (unsigned long long)MemShapeMb(rec.copy_src.bytes),
          (unsigned long long)rec.copy_dst.count,
          (unsigned long long)MemShapeMb(rec.copy_dst.bytes),
          (unsigned long long)rec.flush.count,
          (unsigned long long)MemShapeMb(rec.flush.bytes),
          (unsigned long long)rec.invalidate.count,
          (unsigned long long)MemShapeMb(rec.invalidate.bytes),
          (unsigned long long)rec.map.count,
          (unsigned long long)MemShapeMb(rec.map.bytes),
          (unsigned long long)MemShapeStatsMinKb(desc),
          (unsigned long long)MemShapeStatsAvgKb(desc),
          (unsigned long long)MemShapeStatsMaxKb(desc),
          (unsigned long long)desc.le_4k,
          (unsigned long long)desc.le_64k,
          (unsigned long long)desc.le_1m,
          (unsigned long long)desc.gt_1m,
          (unsigned long long)rec.unresolved_descriptor_ranges,
          (unsigned long long)rec.unresolved_copy_ranges);
}

static void MemShapeLogSummaryLocked(const char* reason) {
    if (!kEnableMemShapeProbe) return;

    uint64_t total_alloc_bytes = 0;
    uint64_t active_mapped = 0;
    uint64_t total_activity_bytes = 0;
    uint64_t total_range_count = 0;
    for (const auto& pair : g_mem_shape_memories) {
        total_alloc_bytes += (uint64_t)pair.second.allocation_size;
        total_activity_bytes += MemShapeMemoryActivityBytes(pair.second);
        total_range_count += MemShapeMemoryRangeCount(pair.second);
        if (pair.second.active_mapped) {
            active_mapped++;
        }
    }

    ALOGI("[MEM_SHAPE_SUMMARY] reason=%s events=%llu submits=%llu memories=%zu "
          "buffers=%zu active_mapped=%llu total_alloc_mb=%llu activity_mb=%llu "
          "range_events=%llu unresolved_global=%llu",
          reason ? reason : "periodic",
          (unsigned long long)g_mem_shape_events,
          (unsigned long long)g_mem_shape_submit_calls,
          g_mem_shape_memories.size(),
          g_mem_shape_buffers.size(),
          (unsigned long long)active_mapped,
          (unsigned long long)MemShapeMb(total_alloc_bytes),
          (unsigned long long)MemShapeMb(total_activity_bytes),
          (unsigned long long)total_range_count,
          (unsigned long long)g_mem_shape_unresolved_global);

    std::vector<std::pair<VkDeviceMemory, const MemShapeMemoryRecord*>> ranked;
    ranked.reserve(g_mem_shape_memories.size());
    for (const auto& pair : g_mem_shape_memories) {
        ranked.push_back({pair.first, &pair.second});
    }
    std::sort(ranked.begin(), ranked.end(),
              [](const std::pair<VkDeviceMemory, const MemShapeMemoryRecord*>& a,
                 const std::pair<VkDeviceMemory, const MemShapeMemoryRecord*>& b) {
                  const uint64_t activity_a = MemShapeMemoryActivityBytes(*a.second);
                  const uint64_t activity_b = MemShapeMemoryActivityBytes(*b.second);
                  if (activity_a != activity_b) return activity_a > activity_b;
                  return a.second->allocation_size > b.second->allocation_size;
              });

    const size_t top_count = std::min(kMemShapeTopMemories, ranked.size());
    for (size_t i = 0; i < top_count; ++i) {
        MemShapeLogOneMemoryLocked("TOP", (uint32_t)(i + 1),
                                   ranked[i].first, *ranked[i].second);
    }
    g_mem_shape_last_summary_event = g_mem_shape_events;
}

static void MemShapeMaybeLogSummaryLocked(const char* reason) {
    if (!kEnableMemShapeProbe) return;
    if (g_mem_shape_events - g_mem_shape_last_summary_event >=
        kMemShapeSummaryEveryEvents) {
        MemShapeLogSummaryLocked(reason);
    }
}

static void MemShapeBumpEventLocked(const char* reason) {
    if (!kEnableMemShapeProbe) return;
    g_mem_shape_events++;
    MemShapeMaybeLogSummaryLocked(reason);
}

static void MemShapeRecordAllocationLocked(VkDevice device,
                                           VkDeviceMemory memory,
                                           VkDeviceSize allocation_size,
                                           uint32_t memory_type_index,
                                           bool express_registered) {
    if (!kEnableMemShapeProbe || memory == VK_NULL_HANDLE) return;
    MemShapeMemoryRecord& rec = g_mem_shape_memories[memory];
    rec.device = device;
    rec.allocation_size = allocation_size;
    rec.memory_type_index = memory_type_index;
    rec.express_registered = express_registered;
    MemShapeBumpEventLocked("alloc");
    if (allocation_size >= kMemShapeLargeAllocLogBytes) {
        ALOGI("[MEM_SHAPE_ALLOC] memory=%llx device=%llx alloc_mb=%llu type=%u registered=%d",
              (unsigned long long)(uintptr_t)memory,
              (unsigned long long)(uintptr_t)device,
              (unsigned long long)MemShapeMb((uint64_t)allocation_size),
              memory_type_index,
              (int)express_registered);
    }
}

static void MemShapeRecordBufferLocked(VkDevice device,
                                       VkBuffer buffer,
                                       VkDeviceSize size,
                                       VkBufferUsageFlags usage) {
    if (!kEnableMemShapeProbe || buffer == VK_NULL_HANDLE) return;
    MemShapeBufferRecord& rec = g_mem_shape_buffers[buffer];
    rec.device = device;
    rec.size = size;
    rec.usage = usage;
    MemShapeBumpEventLocked("create_buffer");
    if (size >= kMemShapeLargeBufferLogBytes) {
        ALOGI("[MEM_SHAPE_BUFFER] buffer=%llx device=%llx size_mb=%llu usage=0x%x",
              (unsigned long long)(uintptr_t)buffer,
              (unsigned long long)(uintptr_t)device,
              (unsigned long long)MemShapeMb((uint64_t)size),
              usage);
    }
}

static void MemShapeRecordBindingLocked(VkDevice device,
                                        VkBuffer buffer,
                                        VkDeviceMemory memory,
                                        VkDeviceSize memory_offset,
                                        VkDeviceSize buffer_size) {
    if (!kEnableMemShapeProbe || buffer == VK_NULL_HANDLE ||
        memory == VK_NULL_HANDLE) {
        return;
    }
    MemShapeBufferRecord& buffer_rec = g_mem_shape_buffers[buffer];
    if (buffer_rec.bound && buffer_rec.memory != memory) {
        auto old_mem_it = g_mem_shape_memories.find(buffer_rec.memory);
        if (old_mem_it != g_mem_shape_memories.end() &&
            old_mem_it->second.current_bound_buffers > 0) {
            old_mem_it->second.current_bound_buffers--;
        }
    }
    if (!buffer_rec.bound || buffer_rec.memory != memory) {
        g_mem_shape_memories[memory].current_bound_buffers++;
    }
    buffer_rec.device = device;
    if (buffer_size != 0) {
        buffer_rec.size = buffer_size;
    }
    buffer_rec.memory = memory;
    buffer_rec.memory_offset = memory_offset;
    buffer_rec.bound = true;

    MemShapeMemoryRecord& mem_rec = g_mem_shape_memories[memory];
    mem_rec.device = device;
    mem_rec.bind_events++;
    MemShapeBumpEventLocked("bind_buffer");

    if (buffer_rec.size >= kMemShapeLargeBufferLogBytes ||
        mem_rec.allocation_size >= kMemShapeLargeAllocLogBytes ||
        memory_offset != 0) {
        const uint64_t end_offset =
            (memory_offset > UINT64_MAX - buffer_rec.size) ?
                UINT64_MAX : (uint64_t)(memory_offset + buffer_rec.size);
        ALOGI("[MEM_SHAPE_BIND] memory=%llx buffer=%llx alloc_mb=%llu "
              "buf_mb=%llu offset_kb=%llu end_kb=%llu usage=0x%x "
              "buffers_in_mem=%llu bind_events=%llu",
              (unsigned long long)(uintptr_t)memory,
              (unsigned long long)(uintptr_t)buffer,
              (unsigned long long)MemShapeMb((uint64_t)mem_rec.allocation_size),
              (unsigned long long)MemShapeMb((uint64_t)buffer_rec.size),
              (unsigned long long)MemShapeKb((uint64_t)memory_offset),
              (unsigned long long)MemShapeKb(end_offset),
              buffer_rec.usage,
              (unsigned long long)mem_rec.current_bound_buffers,
              (unsigned long long)mem_rec.bind_events);
    }
}

static void MemShapeForgetBufferLocked(VkBuffer buffer) {
    if (!kEnableMemShapeProbe || buffer == VK_NULL_HANDLE) return;
    auto buffer_it = g_mem_shape_buffers.find(buffer);
    if (buffer_it == g_mem_shape_buffers.end()) return;
    if (buffer_it->second.bound) {
        auto mem_it = g_mem_shape_memories.find(buffer_it->second.memory);
        if (mem_it != g_mem_shape_memories.end() &&
            mem_it->second.current_bound_buffers > 0) {
            mem_it->second.current_bound_buffers--;
        }
    }
    g_mem_shape_buffers.erase(buffer_it);
    MemShapeBumpEventLocked("forget_buffer");
}

static void MemShapeForgetMemoryLocked(VkDeviceMemory memory,
                                       const char* reason) {
    if (!kEnableMemShapeProbe || memory == VK_NULL_HANDLE) return;
    auto mem_it = g_mem_shape_memories.find(memory);
    if (mem_it == g_mem_shape_memories.end()) return;
    MemShapeLogOneMemoryLocked("FREE", 0, memory, mem_it->second);
    g_mem_shape_memories.erase(mem_it);
    MemShapeBumpEventLocked(reason ? reason : "forget_memory");
}

static void MemShapeNoteMemoryRangeLocked(VkDeviceMemory memory,
                                          VkDeviceSize offset,
                                          VkDeviceSize size,
                                          bool whole_size,
                                          MemShapeRangeKind kind) {
    if (!kEnableMemShapeProbe || memory == VK_NULL_HANDLE || size == 0) return;
    MemShapeMemoryRecord& rec = g_mem_shape_memories[memory];
    if (rec.allocation_size != 0) {
        if (offset > rec.allocation_size) {
            return;
        }
        if (whole_size || offset + size > rec.allocation_size || offset + size < offset) {
            size = rec.allocation_size - offset;
        }
    }
    if (size == 0) return;

    MemShapeRangeStats* stats = MemShapeStatsForKind(&rec, kind);
    MemShapeAddStats(stats, size, whole_size);
    if (kind == MemShapeRangeKind::kDescriptorWriteDispatch) {
        rec.descriptor_dispatch_write_ranges++;
    }

    if (!rec.block_tracking_saturated) {
        const uint64_t first_block = (uint64_t)(offset / kMemShapeBlockBytes);
        const uint64_t last_byte =
            ((uint64_t)offset > UINT64_MAX - ((uint64_t)size - 1)) ?
                UINT64_MAX : (uint64_t)(offset + size - 1);
        const uint64_t last_block = last_byte / kMemShapeBlockBytes;
        for (uint64_t block = first_block; block <= last_block; ++block) {
            rec.touched_blocks_64k.insert(block);
            if (rec.touched_blocks_64k.size() >= kMemShapeMaxTrackedBlocksPerMemory) {
                rec.block_tracking_saturated = true;
                break;
            }
        }
    }
    MemShapeBumpEventLocked("range");
}

static void MemShapeNoteBufferRangeLocked(VkBuffer buffer,
                                          VkDeviceSize offset,
                                          VkDeviceSize range,
                                          MemShapeRangeKind kind) {
    if (!kEnableMemShapeProbe || buffer == VK_NULL_HANDLE) return;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize memory_offset = 0;
    VkDeviceSize resolved_size = 0;
    bool whole_size = false;
    if (!MemShapeResolveBufferRangeLocked(buffer,
                                          offset,
                                          range,
                                          &memory,
                                          &memory_offset,
                                          &resolved_size,
                                          &whole_size)) {
        g_mem_shape_unresolved_global++;
        return;
    }
    MemShapeNoteMemoryRangeLocked(memory, memory_offset, resolved_size, whole_size, kind);
}

static bool MemShapeDescriptorMayWrite(VkDescriptorType descriptor_type) {
    return descriptor_type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
           descriptor_type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
}

static void MemShapeNoteDescriptorBufferLocked(VkBuffer buffer,
                                               VkDeviceSize offset,
                                               VkDeviceSize range,
                                               VkDescriptorType descriptor_type,
                                               bool dispatch_use) {
    if (!kEnableMemShapeProbe || buffer == VK_NULL_HANDLE) return;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize memory_offset = 0;
    VkDeviceSize resolved_size = 0;
    bool whole_size = false;
    if (!MemShapeResolveBufferRangeLocked(buffer,
                                          offset,
                                          range,
                                          &memory,
                                          &memory_offset,
                                          &resolved_size,
                                          &whole_size)) {
        g_mem_shape_unresolved_global++;
        return;
    }

    MemShapeRangeKind kind = dispatch_use ?
        MemShapeRangeKind::kDescriptorDispatch :
        MemShapeRangeKind::kDescriptorUpdate;
    MemShapeNoteMemoryRangeLocked(memory, memory_offset, resolved_size, whole_size, kind);
    if (MemShapeDescriptorMayWrite(descriptor_type)) {
        auto mem_it = g_mem_shape_memories.find(memory);
        if (mem_it != g_mem_shape_memories.end()) {
            if (dispatch_use) {
                MemShapeNoteMemoryRangeLocked(memory,
                                              memory_offset,
                                              resolved_size,
                                              whole_size,
                                              MemShapeRangeKind::kDescriptorWriteDispatch);
            } else {
                mem_it->second.descriptor_update_write_ranges++;
            }
        }
    }
}

static void MemShapeNoteDescriptorUseForDispatchLocked(const DescriptorBufferUse& use,
                                                       VkDeviceSize dynamic_offset) {
    if (!kEnableMemShapeProbe || use.buffer == VK_NULL_HANDLE) return;
    if (dynamic_offset > UINT64_MAX - use.offset) {
        g_mem_shape_unresolved_global++;
        return;
    }
    MemShapeNoteDescriptorBufferLocked(use.buffer,
                                       use.offset + dynamic_offset,
                                       use.range,
                                       (VkDescriptorType)use.descriptor_type,
                                       true);
}

static void MemShapeNoteDispatchDescriptorsLocked(VkCommandBuffer commandBuffer,
                                                  uint32_t groupCountX,
                                                  uint32_t groupCountY,
                                                  uint32_t groupCountZ) {
    if (!kEnableMemShapeProbe || commandBuffer == VK_NULL_HANDLE) return;
    (void)groupCountX;
    (void)groupCountY;
    (void)groupCountZ;
    auto sets_it = g_command_buffer_descriptor_sets.find(commandBuffer);
    if (sets_it == g_command_buffer_descriptor_sets.end()) return;

    const bool unknown_dynamic =
        g_command_buffer_unknown_dynamic_offsets.find(commandBuffer) !=
        g_command_buffer_unknown_dynamic_offsets.end();
    for (VkDescriptorSet descriptor_set : sets_it->second) {
        auto set_it = g_descriptor_set_buffer_uses.find(descriptor_set);
        if (set_it == g_descriptor_set_buffer_uses.end()) continue;
        for (const auto& use_pair : set_it->second) {
            const DescriptorBufferUse& use = use_pair.second;
            if (!use.uses_dynamic_offset) {
                MemShapeNoteDescriptorUseForDispatchLocked(use, 0);
                continue;
            }
            if (unknown_dynamic) {
                MemShapeNoteDescriptorBufferLocked(use.buffer,
                                                   0,
                                                   VK_WHOLE_SIZE,
                                                   (VkDescriptorType)use.descriptor_type,
                                                   true);
                continue;
            }
            auto cmd_dyn_it = g_command_buffer_descriptor_dynamic_offsets.find(commandBuffer);
            if (cmd_dyn_it == g_command_buffer_descriptor_dynamic_offsets.end()) {
                MemShapeNoteDescriptorUseForDispatchLocked(use, 0);
                continue;
            }
            auto set_dyn_it = cmd_dyn_it->second.find(descriptor_set);
            if (set_dyn_it == cmd_dyn_it->second.end()) {
                MemShapeNoteDescriptorUseForDispatchLocked(use, 0);
                continue;
            }
            auto dyn_it = set_dyn_it->second.find(use_pair.first);
            if (dyn_it == set_dyn_it->second.end() || dyn_it->second.empty()) {
                MemShapeNoteDescriptorUseForDispatchLocked(use, 0);
                continue;
            }
            for (uint32_t dyn : dyn_it->second) {
                MemShapeNoteDescriptorUseForDispatchLocked(use, dyn);
            }
        }
    }
}

static void MemShapeNoteMappedRangesLocked(uint32_t memoryRangeCount,
                                           const VkMappedMemoryRange* ranges,
                                           MemShapeRangeKind kind) {
    if (!kEnableMemShapeProbe || !ranges || memoryRangeCount == 0) return;
    for (uint32_t i = 0; i < memoryRangeCount; ++i) {
        VkDeviceMemory memory = ranges[i].memory;
        if (memory == VK_NULL_HANDLE) continue;
        auto mem_it = g_mem_shape_memories.find(memory);
        VkDeviceSize allocation_size = 0;
        if (mem_it != g_mem_shape_memories.end()) {
            allocation_size = mem_it->second.allocation_size;
        } else {
            VkDeviceMemory_T* mem = (VkDeviceMemory_T*)memory;
            if (mem) {
                allocation_size = mem->length;
            }
        }
        VkDeviceSize offset = ranges[i].offset;
        if (allocation_size != 0 && offset > allocation_size) {
            offset = allocation_size;
        }
        VkDeviceSize size = ranges[i].size;
        const bool whole_size = (size == VK_WHOLE_SIZE);
        if (whole_size && allocation_size == 0) {
            g_mem_shape_unresolved_global++;
            continue;
        }
        if (allocation_size != 0 &&
            (whole_size || offset + size > allocation_size || offset + size < offset)) {
            size = allocation_size - offset;
        }
        MemShapeNoteMemoryRangeLocked(memory, offset, size, whole_size, kind);
    }
}

static void MemShapeRecordMapLocked(VkDevice device,
                                    VkDeviceMemory memory,
                                    VkDeviceSize offset,
                                    VkDeviceSize size) {
    if (!kEnableMemShapeProbe || memory == VK_NULL_HANDLE) return;
    MemShapeMemoryRecord& rec = g_mem_shape_memories[memory];
    rec.device = device;
    rec.active_mapped = true;
    rec.map_count++;
    MemShapeNoteMemoryRangeLocked(memory, offset, size, false, MemShapeRangeKind::kMap);
    ALOGI("[MEM_SHAPE_MAP] memory=%llx offset_kb=%llu size_mb=%llu maps=%llu",
          (unsigned long long)(uintptr_t)memory,
          (unsigned long long)MemShapeKb((uint64_t)offset),
          (unsigned long long)MemShapeMb((uint64_t)size),
          (unsigned long long)rec.map_count);
}

static void MemShapeRecordUnmapLocked(VkDeviceMemory memory) {
    if (!kEnableMemShapeProbe || memory == VK_NULL_HANDLE) return;
    auto mem_it = g_mem_shape_memories.find(memory);
    if (mem_it == g_mem_shape_memories.end()) return;
    mem_it->second.active_mapped = false;
    mem_it->second.unmap_count++;
    ALOGI("[MEM_SHAPE_UNMAP] memory=%llx unmaps=%llu",
          (unsigned long long)(uintptr_t)memory,
          (unsigned long long)mem_it->second.unmap_count);
    MemShapeBumpEventLocked("unmap");
}

static void MemShapeNoteSubmit(const char* reason, bool force_summary = false) {
    if (!kEnableMemShapeProbe) return;
    std::lock_guard<std::mutex> lock(g_mapped_mutex);
    g_mem_shape_submit_calls++;
    if (force_summary ||
        (g_mem_shape_submit_calls % kMemShapeSummaryEverySubmits) == 0) {
        MemShapeLogSummaryLocked(reason);
    }
}

static void MemShapeForceSummary(const char* reason) {
    if (!kEnableMemShapeProbe) return;
    std::lock_guard<std::mutex> lock(g_mapped_mutex);
    MemShapeLogSummaryLocked(reason);
}

struct SubmitHintStats {
    uint64_t submit_calls;
    uint64_t submit2_calls;
    uint64_t submit_batches;
    uint64_t deferred_submits;
    uint64_t non_deferred_submits;
    uint64_t hint_present_submits;
    uint64_t wire_nonzero_submits;
    uint64_t semantic_flush_ranges;
    uint64_t wait_flush_ranges;
    uint64_t output_hints;
    uint64_t output_wire_ranges;
    uint64_t wire_bytes;
    uint64_t max_wire_bytes;
};

static SubmitHintStats g_submit_hint_stats;

struct SubmitRpcStats {
    uint64_t submit_calls;
    uint64_t submit2_calls;
    uint64_t submit_batches;
    uint64_t command_buffers;
    uint64_t wait_semaphores;
    uint64_t signal_semaphores;
    uint64_t with_wait_semaphore;
    uint64_t with_signal_semaphore;
    uint64_t with_fence;
    uint64_t deferred_fence;
    uint64_t non_deferred_fence;
    uint64_t sync_write_count;
    uint64_t async_write_count;
    uint64_t hint_us;
    uint64_t encode_us;
    uint64_t write_us;
    uint64_t total_us;
    uint64_t max_write_us;
    uint64_t max_total_us;
    uint64_t first_call_us;
    uint64_t last_call_us;
    uint64_t write_buckets[12];
    uint64_t total_buckets[12];
};

static SubmitRpcStats g_submit_rpc_stats;

struct SubmitCohortStats {
    uint64_t calls;
    uint64_t same_queue_as_prev;
    uint64_t mergeable;
    uint64_t blocked_by_wait_semaphore;
    uint64_t blocked_by_signal_semaphore;
    uint64_t blocked_by_sync_fence;
    uint64_t blocked_by_multi_submit;
    uint64_t blocked_by_queue_change;
    uint64_t empty_sync;
    uint64_t single_cmd;
    uint64_t command_buffers;
    uint64_t total_gap_us;
    uint64_t min_gap_us;
    uint64_t max_gap_us;
    uint64_t gap_buckets[12];
    VkQueue last_queue;
    uint64_t last_submit_end_us;
};

static SubmitCohortStats g_submit_cohort_stats;

struct PendingSubmitCohortEntry {
    std::vector<VkCommandBuffer> command_buffers;
    SubmitSyncHints hints;
    uint64_t hint_us;
    bool has_fence;
    bool deferred_fence;
};

struct SubmitCoalesceStats {
    uint64_t enqueued_submits;
    uint64_t flushed_groups;
    uint64_t flushed_submits;
    uint64_t immediate_submits;
    uint64_t threshold_flushes;
    uint64_t sync_flushes;
    uint64_t queue_change_flushes;
    uint64_t non_coalescible_flushes;
    uint64_t submit2_flushes;
    uint64_t present_flushes;
    uint64_t wait_flushes;
    uint64_t bind_sparse_flushes;
    uint64_t deferred_fences;
    uint64_t max_group_size;
};

static std::mutex g_submit_coalesce_mutex;
static VkQueue g_submit_coalesce_queue = VK_NULL_HANDLE;
static std::vector<PendingSubmitCohortEntry> g_submit_coalesce_pending;
static SubmitCoalesceStats g_submit_coalesce_stats;

#ifndef EXPRESS_VK_DISABLE_DESCRIPTOR_POOL_REUSE_ABLATION
#define EXPRESS_VK_DISABLE_DESCRIPTOR_POOL_REUSE_ABLATION 0
#endif

#ifndef EXPRESS_VK_DISABLE_DEFERRED_DESCRIPTOR_POOL_DESTROY_ABLATION
#define EXPRESS_VK_DISABLE_DEFERRED_DESCRIPTOR_POOL_DESTROY_ABLATION 0
#endif

static constexpr bool kEnableDeferredDescriptorPoolDestroy =
    !EXPRESS_VK_DISABLE_DEFERRED_DESCRIPTOR_POOL_DESTROY_ABLATION;
static constexpr bool kEnableDescriptorPoolReuse =
    !EXPRESS_VK_DISABLE_DESCRIPTOR_POOL_REUSE_ABLATION;
static constexpr size_t kMaxDeferredDescriptorPoolDestroys = 4096;
static constexpr size_t kMaxCachedDescriptorPools = 1024;
static constexpr uint64_t kDescriptorLifecycleStatsLogEvery = 512;

struct DescriptorPoolSignature {
    VkDevice device = VK_NULL_HANDLE;
    uint32_t flags = 0;
    uint32_t max_sets = 0;
    bool reusable = false;
    std::vector<std::pair<uint32_t, uint32_t>> pool_sizes;
};

struct CachedDescriptorPool {
    DescriptorPoolSignature signature;
    VkDescriptorPool pool = VK_NULL_HANDLE;
};

struct DeferredDescriptorPoolDestroy {
    std::vector<uint8_t> payload;
    VkDevice device = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
};

struct DescriptorLifecycleStats {
    uint64_t create_pool_calls = 0;
    uint64_t allocate_set_calls = 0;
    uint64_t allocated_sets = 0;
    uint64_t update_set_calls = 0;
    uint64_t update_template_calls = 0;
    uint64_t bind_set_calls = 0;
    uint64_t bound_sets = 0;
    uint64_t destroy_pool_calls = 0;
    uint64_t cached_pool_destroys = 0;
    uint64_t cache_hits = 0;
    uint64_t reset_for_reuse = 0;
    uint64_t cache_evictions = 0;
    uint64_t deferred_pool_destroys = 0;
    uint64_t flushed_pool_destroys = 0;
    uint64_t destroy_flushes = 0;
    uint64_t threshold_flushes = 0;
    uint64_t forced_flushes = 0;
    uint64_t peak_pending_destroys = 0;
    uint64_t create_pool_us = 0;
    uint64_t allocate_set_us = 0;
    uint64_t update_set_us = 0;
    uint64_t update_template_us = 0;
    uint64_t bind_set_us = 0;
    uint64_t bind_early_upload_us = 0;
    uint64_t bind_early_upload_disabled = 0;
    uint64_t copy_early_upload_disabled = 0;
};

static std::mutex g_descriptor_lifecycle_mutex;
static std::vector<DeferredDescriptorPoolDestroy>
    g_deferred_descriptor_pool_destroys;
static std::vector<CachedDescriptorPool> g_descriptor_pool_cache;
static std::unordered_map<VkDescriptorPool, DescriptorPoolSignature> g_descriptor_pool_signatures;
static DescriptorLifecycleStats g_descriptor_lifecycle_stats;

struct ImplicitFlushStats {
    uint64_t calls;
    uint64_t hint_calls;
    uint64_t scan_calls;
    uint64_t skip_metadata_calls;
    uint64_t empty_calls;
    uint64_t clean_scan_calls;
    uint64_t dirty_scan_calls;
    uint64_t flush_invocations;
    uint64_t mapped_seen;
    uint64_t hint_ranges;
    uint64_t hint_bytes;
    uint64_t scanned_bytes;
    uint64_t compared_chunks;
    uint64_t dirty_chunks;
    uint64_t dirty_ranges;
    uint64_t dirty_bytes;
    uint64_t hint_clean_cache_hits;
    uint64_t hint_memcmp_checks;
    uint64_t hint_memcmp_bytes;
    uint64_t hint_dirty_ranges;
    uint64_t clean_scan_skip_calls;
    uint64_t scan_us;
    uint64_t rpc_us;
    uint64_t total_us;
    uint64_t max_scan_us;
    uint64_t max_rpc_us;
    uint64_t max_total_us;
    uint64_t scan_buckets[12];
    uint64_t total_buckets[12];
};

static ImplicitFlushStats g_implicit_flush_stats;

struct MappedFlushStats {
    uint64_t calls;
    uint64_t ranges;
    uint64_t bytes;
    uint64_t copy_us;
    uint64_t encode_us;
    uint64_t addptr_us;
    uint64_t rpc_us;
    uint64_t record_us;
    uint64_t shadow_us;
    uint64_t total_us;
    uint64_t max_total_us;
    uint64_t total_buckets[12];
};

static MappedFlushStats g_mapped_flush_stats;

struct GuestSyncBreakdownStats {
    uint64_t window_start_us = 0;

    uint64_t implicit_calls = 0;
    uint64_t implicit_scan_calls = 0;
    uint64_t implicit_hint_calls = 0;
    uint64_t implicit_skip_calls = 0;
    uint64_t implicit_empty_calls = 0;
    uint64_t implicit_mapped_seen = 0;
    uint64_t implicit_scanned_bytes = 0;
    uint64_t implicit_compared_chunks = 0;
    uint64_t implicit_dirty_chunks = 0;
    uint64_t implicit_dirty_ranges = 0;
    uint64_t implicit_dirty_bytes = 0;
    uint64_t implicit_flush_invocations = 0;
    uint64_t implicit_scan_us = 0;
    uint64_t implicit_rpc_us = 0;
    uint64_t implicit_total_us = 0;

    uint64_t mapped_flush_calls = 0;
    uint64_t mapped_flush_ranges = 0;
    uint64_t mapped_flush_bytes = 0;
    uint64_t mapped_flush_copy_us = 0;
    uint64_t mapped_flush_encode_us = 0;
    uint64_t mapped_flush_addptr_us = 0;
    uint64_t mapped_flush_rpc_us = 0;
    uint64_t mapped_flush_record_us = 0;
    uint64_t mapped_flush_shadow_us = 0;
    uint64_t mapped_flush_total_us = 0;

    uint64_t mapped_invalidate_calls = 0;
    uint64_t mapped_invalidate_ranges = 0;
    uint64_t mapped_invalidate_bytes = 0;
    uint64_t mapped_invalidate_skipped_synced = 0;
    uint64_t mapped_invalidate_copy_us = 0;
    uint64_t mapped_invalidate_param_us = 0;
    uint64_t mapped_invalidate_addptr_us = 0;
    uint64_t mapped_invalidate_rpc_us = 0;
    uint64_t mapped_invalidate_update_us = 0;
    uint64_t mapped_invalidate_total_us = 0;

    uint64_t conservative_readback_calls = 0;
    uint64_t conservative_readback_ranges = 0;
    uint64_t conservative_readback_bytes = 0;
    uint64_t conservative_readback_skipped_synced = 0;
    uint64_t conservative_preserve_bytes = 0;
    uint64_t conservative_preserve_us = 0;
    uint64_t conservative_invalidate_us = 0;
    uint64_t conservative_total_us = 0;
};

static GuestSyncBreakdownStats g_guest_sync_breakdown_stats;

enum class SyncPolicySource : uint8_t {
    kHintRange = 0,
    kHintMemory = 1,
    kNoHintScan = 2,
    kNoHintCleanSkip = 3,
    kMetadataSkip = 4,
    kEmpty = 5,
};

struct SyncPolicyClassStats {
    uint64_t samples;
    uint64_t clean;
    uint64_t dirty;
    uint64_t unknown;
    uint64_t cache_skip;
    uint64_t action_flush;
    uint64_t action_skip;
    uint64_t aggressive_clean_skip;
    uint64_t direct_dirty_flush;
    uint64_t sample_verify;
    uint64_t bytes;
    uint64_t dirty_bytes;
    uint64_t cost_us;
};

struct SyncPolicyBatch {
    std::vector<std::pair<uint32_t, SyncPolicyClassStats>> classes;
};

struct SyncPolicyStats {
    uint64_t batches;
    uint64_t samples;
    uint64_t clean;
    uint64_t dirty;
    uint64_t unknown;
    uint64_t cache_skip;
    uint64_t action_flush;
    uint64_t action_skip;
    uint64_t aggressive_clean_skip;
    uint64_t direct_dirty_flush;
    uint64_t sample_verify;
    uint64_t bytes;
    uint64_t dirty_bytes;
    uint64_t cost_us;
    std::unordered_map<uint32_t, SyncPolicyClassStats> classes;
};

static SyncPolicyStats g_sync_policy_stats;

struct SubmitHintCacheStats {
    uint64_t lookups = 0;
    uint64_t hits = 0;
    uint64_t misses = 0;
    uint64_t no_metadata = 0;
    uint64_t invalidations = 0;
    uint64_t reused_flush_ranges = 0;
    uint64_t reused_output_hints = 0;
    uint64_t reused_flush_bytes = 0;
    uint64_t built_flush_ranges = 0;
    uint64_t built_output_hints = 0;
};

struct DescriptorHintCacheStats {
    uint64_t lookups = 0;
    uint64_t hits = 0;
    uint64_t misses = 0;
    uint64_t dynamic_bypass = 0;
    uint64_t invalidations = 0;
    uint64_t reused_ranges = 0;
    uint64_t reused_outputs = 0;
    uint64_t reused_bytes = 0;
    uint64_t built_ranges = 0;
    uint64_t built_outputs = 0;
    uint64_t built_bytes = 0;
};

struct LargeCleanRangeStats {
    uint64_t candidates = 0;
    uint64_t hits = 0;
    uint64_t verified = 0;
    uint64_t partial_verified = 0;
    uint64_t partial_hits = 0;
    uint64_t promoted = 0;
    uint64_t failed = 0;
    uint64_t skipped_bytes = 0;
    uint64_t verified_bytes = 0;
    uint64_t partial_verified_bytes = 0;
    uint64_t partial_saved_bytes = 0;
    uint64_t failed_bytes = 0;
    uint64_t ttl_miss = 0;
    uint64_t range_miss = 0;
    uint64_t partial_too_large = 0;
    uint64_t no_shadow = 0;
    uint64_t dirty_overlap = 0;
    uint64_t cleared_by_capacity = 0;
    uint64_t cleared_by_dirty = 0;
};

struct WaitInvalidateFusedStats {
    uint64_t calls = 0;
    uint64_t ranges = 0;
    uint64_t bytes = 0;
    uint64_t wait_us = 0;
    uint64_t invalidate_us = 0;
    uint64_t total_us = 0;
    uint64_t fallback = 0;
    uint64_t no_ranges = 0;
    uint64_t timeout = 0;
    uint64_t failed = 0;
};

struct TargetedReadbackWaitStats {
    uint64_t calls = 0;
    uint64_t ranges = 0;
    uint64_t bytes = 0;
    uint64_t wait_us = 0;
    uint64_t invalidate_us = 0;
    uint64_t total_us = 0;
    uint64_t skipped_synced = 0;
    uint64_t timeout = 0;
    uint64_t failed = 0;
};

struct WeakReadbackSuppressionStats {
    uint64_t candidates = 0;
    uint64_t submit_hits = 0;
    uint64_t strong_hints = 0;
    uint64_t strong_bytes = 0;
    uint64_t weak_dropped = 0;
    uint64_t weak_dropped_bytes = 0;
    uint64_t weak_kept = 0;
    uint64_t weak_kept_bytes = 0;
    uint64_t tiny_weak_kept = 0;
    uint64_t multi_submit_bypass = 0;
};

struct ReadbackFenceStats {
    uint64_t submits = 0;
    uint64_t with_fence = 0;
    uint64_t deferred = 0;
    uint64_t blocked_readback = 0;
    uint64_t no_fence = 0;
    uint64_t output_hints = 0;
    uint64_t output_ranges = 0;
    uint64_t output_bytes = 0;
};

struct ReadbackHintClassStats {
    uint64_t samples = 0;
    uint64_t auto_readback = 0;
    uint64_t wait_eligible = 0;
    uint64_t deferred_submits = 0;
    uint64_t blocked_submits = 0;
    uint64_t bytes = 0;
    uint64_t auto_bytes = 0;
};

enum class InvalidateChangeClass : uint8_t {
    kExplicit = 0,
    kDeferredWeakReadback = 1,
    kTargetedStrongReadback = 2,
    kOther = 3,
    kCount = 4,
};

struct InvalidateChangeClassStats {
    uint64_t calls = 0;
    uint64_t ranges = 0;
    uint64_t bytes = 0;
    uint64_t sample_bytes = 0;
    uint64_t changed_samples = 0;
    uint64_t clean_samples = 0;
    uint64_t no_shadow = 0;
    uint64_t untracked = 0;
};

struct InvalidateChangeStats {
    std::array<InvalidateChangeClassStats, (size_t)InvalidateChangeClass::kCount> classes;
};

static std::mutex g_submit_hint_cache_stats_mutex;
static SubmitHintCacheStats g_submit_hint_cache_stats;
static DescriptorHintCacheStats g_descriptor_hint_cache_stats;
static LargeCleanRangeStats g_large_clean_range_stats;
static ReadbackFenceStats g_readback_fence_stats;
static WaitInvalidateFusedStats g_wait_invalidate_fused_stats;
static TargetedReadbackWaitStats g_targeted_readback_wait_stats;
static WeakReadbackSuppressionStats g_weak_readback_suppression_stats;
static std::unordered_map<uint32_t, ReadbackHintClassStats> g_readback_hint_class_stats;
static InvalidateChangeStats g_invalidate_change_stats;
static thread_local const char* g_current_invalidate_reason = nullptr;

static const char* OutputHintSourceName(OutputHintSource source);
static const char* OutputHintStrengthName(OutputHintStrength strength);

static uint64_t ExpressVkNowUs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

static uint64_t ExpressVkElapsedUs(const struct timespec& start,
                                   const struct timespec& end) {
    if (end.tv_sec < start.tv_sec ||
        (end.tv_sec == start.tv_sec && end.tv_nsec <= start.tv_nsec)) {
        return 0;
    }
    int64_t sec = (int64_t)(end.tv_sec - start.tv_sec);
    int64_t nsec = (int64_t)(end.tv_nsec - start.tv_nsec);
    if (nsec < 0) {
        sec--;
        nsec += 1000000000ll;
    }
    if (sec < 0) return 0;
    return (uint64_t)sec * 1000000ull + (uint64_t)nsec / 1000ull;
}

// Lightweight windowed Vulkan timing for LLM decode profiling. This is separate
// from ALOGI/ALOGD so it still works when verbose wrapper logging is disabled.
static constexpr bool kEnableLlmVkTimingLog = true;
static constexpr uint64_t kLlmVkTimingWindowUs = 500000;
static constexpr uint64_t kLlmVkTimingMinEvents = 4096;

struct LlmVkTimingCounters {
    std::atomic<uint64_t> events{0};

    std::atomic<uint64_t> cmd_dispatch_calls{0};
    std::atomic<uint64_t> cmd_dispatch_groups{0};
    std::atomic<uint64_t> cmd_dispatch_record_us{0};
    std::atomic<uint64_t> cmd_dispatch_max_record_us{0};
    std::atomic<uint64_t> cmd_dispatch_indirect_calls{0};

    std::atomic<uint64_t> cmd_copy_buffer_calls{0};
    std::atomic<uint64_t> cmd_copy_buffer_regions{0};
    std::atomic<uint64_t> cmd_copy_buffer_bytes{0};
    std::atomic<uint64_t> cmd_copy_buffer_record_us{0};
    std::atomic<uint64_t> cmd_copy_buffer_max_record_us{0};

    std::atomic<uint64_t> cmd_pipeline_barrier_calls{0};
    std::atomic<uint64_t> cmd_pipeline_barrier_memory_barriers{0};
    std::atomic<uint64_t> cmd_pipeline_barrier_buffer_barriers{0};
    std::atomic<uint64_t> cmd_pipeline_barrier_image_barriers{0};
    std::atomic<uint64_t> cmd_pipeline_barrier_prepare_us{0};
    std::atomic<uint64_t> cmd_pipeline_barrier_write_us{0};
    std::atomic<uint64_t> cmd_pipeline_barrier_total_us{0};
    std::atomic<uint64_t> cmd_pipeline_barrier_max_prepare_us{0};
    std::atomic<uint64_t> cmd_pipeline_barrier_max_write_us{0};
    std::atomic<uint64_t> cmd_pipeline_barrier_max_total_us{0};

    std::atomic<uint64_t> submit_calls{0};
    std::atomic<uint64_t> submit_batches{0};
    std::atomic<uint64_t> submit_cmd_bufs{0};
    std::atomic<uint64_t> submit_wait_sems{0};
    std::atomic<uint64_t> submit_signal_sems{0};
    std::atomic<uint64_t> submit_with_fence{0};
    std::atomic<uint64_t> submit_deferred_fence{0};
    std::atomic<uint64_t> submit_hint_us{0};
    std::atomic<uint64_t> submit_encode_us{0};
    std::atomic<uint64_t> submit_write_us{0};
    std::atomic<uint64_t> submit_total_us{0};
    std::atomic<uint64_t> submit_max_total_us{0};

    std::atomic<uint64_t> wait_calls{0};
    std::atomic<uint64_t> wait_fences{0};
    std::atomic<uint64_t> wait_rpc_us{0};
    std::atomic<uint64_t> wait_implicit_us{0};
    std::atomic<uint64_t> wait_total_us{0};
    std::atomic<uint64_t> wait_max_total_us{0};
    std::atomic<uint64_t> wait_target_ranges{0};
    std::atomic<uint64_t> wait_request_bytes{0};
    std::atomic<uint64_t> wait_invalidate_bytes{0};
    std::atomic<uint64_t> wait_skipped_synced{0};
    std::atomic<uint64_t> wait_timeouts{0};

    std::atomic<uint64_t> queue_idle_calls{0};
    std::atomic<uint64_t> queue_idle_total_us{0};
    std::atomic<uint64_t> queue_idle_invalidate_bytes{0};
    std::atomic<uint64_t> device_idle_calls{0};
    std::atomic<uint64_t> device_idle_total_us{0};
    std::atomic<uint64_t> device_idle_invalidate_bytes{0};

    std::atomic<uint64_t> map_calls{0};
    std::atomic<uint64_t> map_bytes{0};
    std::atomic<uint64_t> map_rpc_us{0};
    std::atomic<uint64_t> map_shadow_us{0};
    std::atomic<uint64_t> map_total_us{0};
    std::atomic<uint64_t> map_max_total_us{0};
    std::atomic<uint64_t> unmap_calls{0};

    std::atomic<uint64_t> flush_calls{0};
    std::atomic<uint64_t> flush_ranges{0};
    std::atomic<uint64_t> flush_bytes{0};
    std::atomic<uint64_t> flush_rpc_us{0};
    std::atomic<uint64_t> flush_shadow_us{0};
    std::atomic<uint64_t> flush_total_us{0};
    std::atomic<uint64_t> flush_max_total_us{0};

    std::atomic<uint64_t> invalidate_calls{0};
    std::atomic<uint64_t> invalidate_ranges{0};
    std::atomic<uint64_t> invalidate_bytes{0};
    std::atomic<uint64_t> invalidate_skipped_synced{0};
    std::atomic<uint64_t> invalidate_rpc_us{0};
    std::atomic<uint64_t> invalidate_update_us{0};
    std::atomic<uint64_t> invalidate_total_us{0};
    std::atomic<uint64_t> invalidate_max_total_us{0};

    std::atomic<uint64_t> conservative_calls{0};
    std::atomic<uint64_t> conservative_ranges{0};
    std::atomic<uint64_t> conservative_bytes{0};
    std::atomic<uint64_t> conservative_skipped_synced{0};
    std::atomic<uint64_t> conservative_preserve_bytes{0};
    std::atomic<uint64_t> conservative_preserve_us{0};
    std::atomic<uint64_t> conservative_invalidate_us{0};
    std::atomic<uint64_t> conservative_total_us{0};
    std::atomic<uint64_t> conservative_max_total_us{0};
};

static LlmVkTimingCounters g_llm_vk_timing;
static std::atomic<uint64_t> g_llm_vk_timing_window_start_us{0};
static std::atomic_flag g_llm_vk_timing_log_lock = ATOMIC_FLAG_INIT;

static void LlmVkTimingAtomicMax(std::atomic<uint64_t>& field, uint64_t value) {
    uint64_t old = field.load(std::memory_order_relaxed);
    while (old < value &&
           !field.compare_exchange_weak(old,
                                        value,
                                        std::memory_order_relaxed,
                                        std::memory_order_relaxed)) {
    }
}

static uint64_t LlmVkTimingTake(std::atomic<uint64_t>& field) {
    return field.exchange(0, std::memory_order_relaxed);
}

static void LlmVkTimingMaybeLog(const char* reason) {
    if (!kEnableLlmVkTimingLog) return;

    const uint64_t now_us = ExpressVkNowUs();
    uint64_t start_us = g_llm_vk_timing_window_start_us.load(std::memory_order_relaxed);
    if (start_us == 0) {
        uint64_t expected = 0;
        if (g_llm_vk_timing_window_start_us.compare_exchange_strong(
                expected, now_us, std::memory_order_relaxed)) {
            start_us = now_us;
        } else {
            start_us = expected;
        }
    }

    const uint64_t events =
        g_llm_vk_timing.events.fetch_add(1, std::memory_order_relaxed) + 1;
    if (now_us - start_us < kLlmVkTimingWindowUs &&
        events < kLlmVkTimingMinEvents) {
        return;
    }
    if (g_llm_vk_timing_log_lock.test_and_set(std::memory_order_acquire)) {
        return;
    }

    start_us = g_llm_vk_timing_window_start_us.load(std::memory_order_relaxed);
    const uint64_t window_us = now_us > start_us ? now_us - start_us : 0;
    const uint64_t event_count = LlmVkTimingTake(g_llm_vk_timing.events);
    g_llm_vk_timing_window_start_us.store(now_us, std::memory_order_relaxed);

#define TAKE_LLM_TIMING_FIELD(name) \
    const uint64_t name = LlmVkTimingTake(g_llm_vk_timing.name)

    TAKE_LLM_TIMING_FIELD(cmd_dispatch_calls);
    TAKE_LLM_TIMING_FIELD(cmd_dispatch_groups);
    TAKE_LLM_TIMING_FIELD(cmd_dispatch_record_us);
    TAKE_LLM_TIMING_FIELD(cmd_dispatch_max_record_us);
    TAKE_LLM_TIMING_FIELD(cmd_dispatch_indirect_calls);
    TAKE_LLM_TIMING_FIELD(cmd_copy_buffer_calls);
    TAKE_LLM_TIMING_FIELD(cmd_copy_buffer_regions);
    TAKE_LLM_TIMING_FIELD(cmd_copy_buffer_bytes);
    TAKE_LLM_TIMING_FIELD(cmd_copy_buffer_record_us);
    TAKE_LLM_TIMING_FIELD(cmd_copy_buffer_max_record_us);
    TAKE_LLM_TIMING_FIELD(cmd_pipeline_barrier_calls);
    TAKE_LLM_TIMING_FIELD(cmd_pipeline_barrier_memory_barriers);
    TAKE_LLM_TIMING_FIELD(cmd_pipeline_barrier_buffer_barriers);
    TAKE_LLM_TIMING_FIELD(cmd_pipeline_barrier_image_barriers);
    TAKE_LLM_TIMING_FIELD(cmd_pipeline_barrier_prepare_us);
    TAKE_LLM_TIMING_FIELD(cmd_pipeline_barrier_write_us);
    TAKE_LLM_TIMING_FIELD(cmd_pipeline_barrier_total_us);
    TAKE_LLM_TIMING_FIELD(cmd_pipeline_barrier_max_prepare_us);
    TAKE_LLM_TIMING_FIELD(cmd_pipeline_barrier_max_write_us);
    TAKE_LLM_TIMING_FIELD(cmd_pipeline_barrier_max_total_us);
    TAKE_LLM_TIMING_FIELD(submit_calls);
    TAKE_LLM_TIMING_FIELD(submit_batches);
    TAKE_LLM_TIMING_FIELD(submit_cmd_bufs);
    TAKE_LLM_TIMING_FIELD(submit_wait_sems);
    TAKE_LLM_TIMING_FIELD(submit_signal_sems);
    TAKE_LLM_TIMING_FIELD(submit_with_fence);
    TAKE_LLM_TIMING_FIELD(submit_deferred_fence);
    TAKE_LLM_TIMING_FIELD(submit_hint_us);
    TAKE_LLM_TIMING_FIELD(submit_encode_us);
    TAKE_LLM_TIMING_FIELD(submit_write_us);
    TAKE_LLM_TIMING_FIELD(submit_total_us);
    TAKE_LLM_TIMING_FIELD(submit_max_total_us);
    TAKE_LLM_TIMING_FIELD(wait_calls);
    TAKE_LLM_TIMING_FIELD(wait_fences);
    TAKE_LLM_TIMING_FIELD(wait_rpc_us);
    TAKE_LLM_TIMING_FIELD(wait_implicit_us);
    TAKE_LLM_TIMING_FIELD(wait_total_us);
    TAKE_LLM_TIMING_FIELD(wait_max_total_us);
    TAKE_LLM_TIMING_FIELD(wait_target_ranges);
    TAKE_LLM_TIMING_FIELD(wait_request_bytes);
    TAKE_LLM_TIMING_FIELD(wait_invalidate_bytes);
    TAKE_LLM_TIMING_FIELD(wait_skipped_synced);
    TAKE_LLM_TIMING_FIELD(wait_timeouts);
    TAKE_LLM_TIMING_FIELD(queue_idle_calls);
    TAKE_LLM_TIMING_FIELD(queue_idle_total_us);
    TAKE_LLM_TIMING_FIELD(queue_idle_invalidate_bytes);
    TAKE_LLM_TIMING_FIELD(device_idle_calls);
    TAKE_LLM_TIMING_FIELD(device_idle_total_us);
    TAKE_LLM_TIMING_FIELD(device_idle_invalidate_bytes);
    TAKE_LLM_TIMING_FIELD(map_calls);
    TAKE_LLM_TIMING_FIELD(map_bytes);
    TAKE_LLM_TIMING_FIELD(map_rpc_us);
    TAKE_LLM_TIMING_FIELD(map_shadow_us);
    TAKE_LLM_TIMING_FIELD(map_total_us);
    TAKE_LLM_TIMING_FIELD(map_max_total_us);
    TAKE_LLM_TIMING_FIELD(unmap_calls);
    TAKE_LLM_TIMING_FIELD(flush_calls);
    TAKE_LLM_TIMING_FIELD(flush_ranges);
    TAKE_LLM_TIMING_FIELD(flush_bytes);
    TAKE_LLM_TIMING_FIELD(flush_rpc_us);
    TAKE_LLM_TIMING_FIELD(flush_shadow_us);
    TAKE_LLM_TIMING_FIELD(flush_total_us);
    TAKE_LLM_TIMING_FIELD(flush_max_total_us);
    TAKE_LLM_TIMING_FIELD(invalidate_calls);
    TAKE_LLM_TIMING_FIELD(invalidate_ranges);
    TAKE_LLM_TIMING_FIELD(invalidate_bytes);
    TAKE_LLM_TIMING_FIELD(invalidate_skipped_synced);
    TAKE_LLM_TIMING_FIELD(invalidate_rpc_us);
    TAKE_LLM_TIMING_FIELD(invalidate_update_us);
    TAKE_LLM_TIMING_FIELD(invalidate_total_us);
    TAKE_LLM_TIMING_FIELD(invalidate_max_total_us);
    TAKE_LLM_TIMING_FIELD(conservative_calls);
    TAKE_LLM_TIMING_FIELD(conservative_ranges);
    TAKE_LLM_TIMING_FIELD(conservative_bytes);
    TAKE_LLM_TIMING_FIELD(conservative_skipped_synced);
    TAKE_LLM_TIMING_FIELD(conservative_preserve_bytes);
    TAKE_LLM_TIMING_FIELD(conservative_preserve_us);
    TAKE_LLM_TIMING_FIELD(conservative_invalidate_us);
    TAKE_LLM_TIMING_FIELD(conservative_total_us);
    TAKE_LLM_TIMING_FIELD(conservative_max_total_us);

#undef TAKE_LLM_TIMING_FIELD

    const uint64_t activity =
        cmd_dispatch_calls + cmd_copy_buffer_calls +
        cmd_pipeline_barrier_calls + submit_calls + wait_calls + queue_idle_calls +
        device_idle_calls + map_calls + unmap_calls + flush_calls +
        invalidate_calls + conservative_calls;
    if (activity != 0) {
        const uint64_t cmd_pipeline_barrier_prepare_avg_us =
            cmd_pipeline_barrier_calls ?
                cmd_pipeline_barrier_prepare_us / cmd_pipeline_barrier_calls : 0;
        const uint64_t cmd_pipeline_barrier_write_avg_us =
            cmd_pipeline_barrier_calls ?
                cmd_pipeline_barrier_write_us / cmd_pipeline_barrier_calls : 0;
        const uint64_t cmd_pipeline_barrier_total_avg_us =
            cmd_pipeline_barrier_calls ?
                cmd_pipeline_barrier_total_us / cmd_pipeline_barrier_calls : 0;
        __android_log_print(
            ANDROID_LOG_INFO,
            "ExpressVkTiming",
            "[LLM_VK_TIMING] reason=%s window_ms=%llu events=%llu "
            "dispatch=%llu dispatch_indirect=%llu dispatch_groups=%llu "
            "dispatch_record_us=%llu dispatch_max_us=%llu "
            "copy_cmd=%llu copy_regions=%llu copy_mb=%llu "
            "copy_record_us=%llu copy_max_us=%llu "
            "pipeline_barrier=%llu pb_mem=%llu pb_buf=%llu pb_img=%llu "
            "pb_prepare_us=%llu pb_write_us=%llu pb_total_us=%llu "
            "pb_prepare_avg_us=%llu pb_write_avg_us=%llu pb_total_avg_us=%llu "
            "pb_max_prepare_us=%llu pb_max_write_us=%llu pb_max_total_us=%llu "
            "submit_calls=%llu logical_submits=%llu cmd_bufs=%llu "
            "wait_sems=%llu signal_sems=%llu submit_fences=%llu "
            "deferred_fences=%llu submit_hint_us=%llu submit_encode_us=%llu "
            "submit_write_us=%llu submit_total_us=%llu submit_max_us=%llu "
            "wait_calls=%llu wait_fences=%llu wait_rpc_us=%llu "
            "wait_implicit_us=%llu wait_total_us=%llu wait_max_us=%llu "
            "wait_target_ranges=%llu wait_request_mb=%llu wait_invalidate_mb=%llu "
            "wait_skipped=%llu wait_timeouts=%llu "
            "queue_idle=%llu queue_idle_us=%llu queue_idle_invalidate_mb=%llu "
            "device_idle=%llu device_idle_us=%llu device_idle_invalidate_mb=%llu "
            "map_calls=%llu map_mb=%llu map_rpc_us=%llu map_shadow_us=%llu "
            "map_total_us=%llu map_max_us=%llu unmap_calls=%llu "
            "flush_calls=%llu flush_ranges=%llu flush_mb=%llu "
            "flush_rpc_us=%llu flush_shadow_us=%llu flush_total_us=%llu "
            "flush_max_us=%llu "
            "invalidate_calls=%llu invalidate_ranges=%llu invalidate_mb=%llu "
            "invalidate_skipped=%llu invalidate_rpc_us=%llu "
            "invalidate_update_us=%llu invalidate_total_us=%llu "
            "invalidate_max_us=%llu "
            "conservative_calls=%llu conservative_ranges=%llu conservative_mb=%llu "
            "conservative_skipped=%llu preserve_mb=%llu preserve_us=%llu "
            "readback_invalidate_us=%llu readback_total_us=%llu "
            "readback_max_us=%llu gpu_ts_us=unavailable_guest_only",
            reason ? reason : "periodic",
            (unsigned long long)(window_us / 1000ull),
            (unsigned long long)event_count,
            (unsigned long long)cmd_dispatch_calls,
            (unsigned long long)cmd_dispatch_indirect_calls,
            (unsigned long long)cmd_dispatch_groups,
            (unsigned long long)cmd_dispatch_record_us,
            (unsigned long long)cmd_dispatch_max_record_us,
            (unsigned long long)cmd_copy_buffer_calls,
            (unsigned long long)cmd_copy_buffer_regions,
            (unsigned long long)(cmd_copy_buffer_bytes / (1024ull * 1024ull)),
            (unsigned long long)cmd_copy_buffer_record_us,
            (unsigned long long)cmd_copy_buffer_max_record_us,
            (unsigned long long)cmd_pipeline_barrier_calls,
            (unsigned long long)cmd_pipeline_barrier_memory_barriers,
            (unsigned long long)cmd_pipeline_barrier_buffer_barriers,
            (unsigned long long)cmd_pipeline_barrier_image_barriers,
            (unsigned long long)cmd_pipeline_barrier_prepare_us,
            (unsigned long long)cmd_pipeline_barrier_write_us,
            (unsigned long long)cmd_pipeline_barrier_total_us,
            (unsigned long long)cmd_pipeline_barrier_prepare_avg_us,
            (unsigned long long)cmd_pipeline_barrier_write_avg_us,
            (unsigned long long)cmd_pipeline_barrier_total_avg_us,
            (unsigned long long)cmd_pipeline_barrier_max_prepare_us,
            (unsigned long long)cmd_pipeline_barrier_max_write_us,
            (unsigned long long)cmd_pipeline_barrier_max_total_us,
            (unsigned long long)submit_calls,
            (unsigned long long)submit_batches,
            (unsigned long long)submit_cmd_bufs,
            (unsigned long long)submit_wait_sems,
            (unsigned long long)submit_signal_sems,
            (unsigned long long)submit_with_fence,
            (unsigned long long)submit_deferred_fence,
            (unsigned long long)submit_hint_us,
            (unsigned long long)submit_encode_us,
            (unsigned long long)submit_write_us,
            (unsigned long long)submit_total_us,
            (unsigned long long)submit_max_total_us,
            (unsigned long long)wait_calls,
            (unsigned long long)wait_fences,
            (unsigned long long)wait_rpc_us,
            (unsigned long long)wait_implicit_us,
            (unsigned long long)wait_total_us,
            (unsigned long long)wait_max_total_us,
            (unsigned long long)wait_target_ranges,
            (unsigned long long)(wait_request_bytes / (1024ull * 1024ull)),
            (unsigned long long)(wait_invalidate_bytes / (1024ull * 1024ull)),
            (unsigned long long)wait_skipped_synced,
            (unsigned long long)wait_timeouts,
            (unsigned long long)queue_idle_calls,
            (unsigned long long)queue_idle_total_us,
            (unsigned long long)(queue_idle_invalidate_bytes / (1024ull * 1024ull)),
            (unsigned long long)device_idle_calls,
            (unsigned long long)device_idle_total_us,
            (unsigned long long)(device_idle_invalidate_bytes / (1024ull * 1024ull)),
            (unsigned long long)map_calls,
            (unsigned long long)(map_bytes / (1024ull * 1024ull)),
            (unsigned long long)map_rpc_us,
            (unsigned long long)map_shadow_us,
            (unsigned long long)map_total_us,
            (unsigned long long)map_max_total_us,
            (unsigned long long)unmap_calls,
            (unsigned long long)flush_calls,
            (unsigned long long)flush_ranges,
            (unsigned long long)(flush_bytes / (1024ull * 1024ull)),
            (unsigned long long)flush_rpc_us,
            (unsigned long long)flush_shadow_us,
            (unsigned long long)flush_total_us,
            (unsigned long long)flush_max_total_us,
            (unsigned long long)invalidate_calls,
            (unsigned long long)invalidate_ranges,
            (unsigned long long)(invalidate_bytes / (1024ull * 1024ull)),
            (unsigned long long)invalidate_skipped_synced,
            (unsigned long long)invalidate_rpc_us,
            (unsigned long long)invalidate_update_us,
            (unsigned long long)invalidate_total_us,
            (unsigned long long)invalidate_max_total_us,
            (unsigned long long)conservative_calls,
            (unsigned long long)conservative_ranges,
            (unsigned long long)(conservative_bytes / (1024ull * 1024ull)),
            (unsigned long long)conservative_skipped_synced,
            (unsigned long long)(conservative_preserve_bytes / (1024ull * 1024ull)),
            (unsigned long long)conservative_preserve_us,
            (unsigned long long)conservative_invalidate_us,
            (unsigned long long)conservative_total_us,
            (unsigned long long)conservative_max_total_us);
    }

    g_llm_vk_timing_log_lock.clear(std::memory_order_release);
}

static void LlmVkTimingNoteSubmit(uint32_t submit_count,
                                  uint64_t command_buffers,
                                  uint64_t wait_semaphores,
                                  uint64_t signal_semaphores,
                                  bool has_fence,
                                  bool deferred_fence,
                                  uint64_t hint_us,
                                  uint64_t encode_us,
                                  uint64_t write_us,
                                  uint64_t total_us) {
    if (!kEnableLlmVkTimingLog) return;
    g_llm_vk_timing.submit_calls.fetch_add(1, std::memory_order_relaxed);
    g_llm_vk_timing.submit_batches.fetch_add(submit_count, std::memory_order_relaxed);
    g_llm_vk_timing.submit_cmd_bufs.fetch_add(command_buffers, std::memory_order_relaxed);
    g_llm_vk_timing.submit_wait_sems.fetch_add(wait_semaphores, std::memory_order_relaxed);
    g_llm_vk_timing.submit_signal_sems.fetch_add(signal_semaphores, std::memory_order_relaxed);
    if (has_fence) g_llm_vk_timing.submit_with_fence.fetch_add(1, std::memory_order_relaxed);
    if (deferred_fence) g_llm_vk_timing.submit_deferred_fence.fetch_add(1, std::memory_order_relaxed);
    g_llm_vk_timing.submit_hint_us.fetch_add(hint_us, std::memory_order_relaxed);
    g_llm_vk_timing.submit_encode_us.fetch_add(encode_us, std::memory_order_relaxed);
    g_llm_vk_timing.submit_write_us.fetch_add(write_us, std::memory_order_relaxed);
    g_llm_vk_timing.submit_total_us.fetch_add(total_us, std::memory_order_relaxed);
    LlmVkTimingAtomicMax(g_llm_vk_timing.submit_max_total_us, total_us);
    LlmVkTimingMaybeLog("submit");
}

static void LlmVkTimingNoteWait(uint32_t fence_count,
                                uint64_t rpc_us,
                                uint64_t implicit_us,
                                uint64_t total_us,
                                uint64_t target_ranges,
                                uint64_t request_bytes,
                                uint64_t invalidate_bytes,
                                uint64_t skipped_synced,
                                VkResult result) {
    if (!kEnableLlmVkTimingLog) return;
    g_llm_vk_timing.wait_calls.fetch_add(1, std::memory_order_relaxed);
    g_llm_vk_timing.wait_fences.fetch_add(fence_count, std::memory_order_relaxed);
    g_llm_vk_timing.wait_rpc_us.fetch_add(rpc_us, std::memory_order_relaxed);
    g_llm_vk_timing.wait_implicit_us.fetch_add(implicit_us, std::memory_order_relaxed);
    g_llm_vk_timing.wait_total_us.fetch_add(total_us, std::memory_order_relaxed);
    LlmVkTimingAtomicMax(g_llm_vk_timing.wait_max_total_us, total_us);
    g_llm_vk_timing.wait_target_ranges.fetch_add(target_ranges, std::memory_order_relaxed);
    g_llm_vk_timing.wait_request_bytes.fetch_add(request_bytes, std::memory_order_relaxed);
    g_llm_vk_timing.wait_invalidate_bytes.fetch_add(invalidate_bytes, std::memory_order_relaxed);
    g_llm_vk_timing.wait_skipped_synced.fetch_add(skipped_synced, std::memory_order_relaxed);
    if (result == VK_TIMEOUT) g_llm_vk_timing.wait_timeouts.fetch_add(1, std::memory_order_relaxed);
    LlmVkTimingMaybeLog("wait_fences");
}

static void LlmVkTimingNoteMap(uint64_t bytes,
                               uint64_t rpc_us,
                               uint64_t shadow_us,
                               uint64_t total_us) {
    if (!kEnableLlmVkTimingLog) return;
    g_llm_vk_timing.map_calls.fetch_add(1, std::memory_order_relaxed);
    g_llm_vk_timing.map_bytes.fetch_add(bytes, std::memory_order_relaxed);
    g_llm_vk_timing.map_rpc_us.fetch_add(rpc_us, std::memory_order_relaxed);
    g_llm_vk_timing.map_shadow_us.fetch_add(shadow_us, std::memory_order_relaxed);
    g_llm_vk_timing.map_total_us.fetch_add(total_us, std::memory_order_relaxed);
    LlmVkTimingAtomicMax(g_llm_vk_timing.map_max_total_us, total_us);
    LlmVkTimingMaybeLog("map_memory");
}

static void LlmVkTimingNoteCmdDispatch(uint64_t groups, uint64_t record_us) {
    if (!kEnableLlmVkTimingLog) return;
    g_llm_vk_timing.cmd_dispatch_calls.fetch_add(1, std::memory_order_relaxed);
    g_llm_vk_timing.cmd_dispatch_groups.fetch_add(groups, std::memory_order_relaxed);
    g_llm_vk_timing.cmd_dispatch_record_us.fetch_add(record_us, std::memory_order_relaxed);
    LlmVkTimingAtomicMax(g_llm_vk_timing.cmd_dispatch_max_record_us, record_us);
    LlmVkTimingMaybeLog("cmd_dispatch");
}

static void LlmVkTimingNoteCmdCopyBuffer(uint32_t regions,
                                         uint64_t bytes,
                                         uint64_t record_us) {
    if (!kEnableLlmVkTimingLog) return;
    g_llm_vk_timing.cmd_copy_buffer_calls.fetch_add(1, std::memory_order_relaxed);
    g_llm_vk_timing.cmd_copy_buffer_regions.fetch_add(regions, std::memory_order_relaxed);
    g_llm_vk_timing.cmd_copy_buffer_bytes.fetch_add(bytes, std::memory_order_relaxed);
    g_llm_vk_timing.cmd_copy_buffer_record_us.fetch_add(record_us, std::memory_order_relaxed);
    LlmVkTimingAtomicMax(g_llm_vk_timing.cmd_copy_buffer_max_record_us, record_us);
    LlmVkTimingMaybeLog("cmd_copy_buffer");
}

static void LlmVkTimingNoteCmdPipelineBarrier(uint32_t memory_barriers,
                                              uint32_t buffer_barriers,
                                              uint32_t image_barriers,
                                              uint64_t prepare_us,
                                              uint64_t write_us,
                                              uint64_t total_us) {
    if (!kEnableLlmVkTimingLog) return;
    g_llm_vk_timing.cmd_pipeline_barrier_calls.fetch_add(1, std::memory_order_relaxed);
    g_llm_vk_timing.cmd_pipeline_barrier_memory_barriers.fetch_add(memory_barriers, std::memory_order_relaxed);
    g_llm_vk_timing.cmd_pipeline_barrier_buffer_barriers.fetch_add(buffer_barriers, std::memory_order_relaxed);
    g_llm_vk_timing.cmd_pipeline_barrier_image_barriers.fetch_add(image_barriers, std::memory_order_relaxed);
    g_llm_vk_timing.cmd_pipeline_barrier_prepare_us.fetch_add(prepare_us, std::memory_order_relaxed);
    g_llm_vk_timing.cmd_pipeline_barrier_write_us.fetch_add(write_us, std::memory_order_relaxed);
    g_llm_vk_timing.cmd_pipeline_barrier_total_us.fetch_add(total_us, std::memory_order_relaxed);
    LlmVkTimingAtomicMax(g_llm_vk_timing.cmd_pipeline_barrier_max_prepare_us, prepare_us);
    LlmVkTimingAtomicMax(g_llm_vk_timing.cmd_pipeline_barrier_max_write_us, write_us);
    LlmVkTimingAtomicMax(g_llm_vk_timing.cmd_pipeline_barrier_max_total_us, total_us);
    LlmVkTimingMaybeLog("cmd_pipeline_barrier");
}

static uint64_t TotalTrackedRangeBytes(const std::vector<TrackedMemoryRange>& ranges) {
    uint64_t bytes = 0;
    for (const TrackedMemoryRange& range : ranges) {
        bytes += range.size;
    }
    return bytes;
}

static void MaybeLogSubmitHintCacheStatsLocked(const char* reason) {
    const uint64_t events = g_submit_hint_cache_stats.lookups +
                            g_submit_hint_cache_stats.invalidations;
    if (events == 0 || (events % kSubmitHintCacheStatsLogEvery) != 0) return;
    ALOGI("[HINT_CACHE_SUMMARY] reason=%s lookups=%llu hits=%llu misses=%llu no_metadata=%llu invalidations=%llu reused_ranges=%llu reused_outputs=%llu reused_mb=%llu built_ranges=%llu built_outputs=%llu epoch=%llu entries=%zu",
          reason ? reason : "periodic",
          (unsigned long long)g_submit_hint_cache_stats.lookups,
          (unsigned long long)g_submit_hint_cache_stats.hits,
          (unsigned long long)g_submit_hint_cache_stats.misses,
          (unsigned long long)g_submit_hint_cache_stats.no_metadata,
          (unsigned long long)g_submit_hint_cache_stats.invalidations,
          (unsigned long long)g_submit_hint_cache_stats.reused_flush_ranges,
          (unsigned long long)g_submit_hint_cache_stats.reused_output_hints,
          (unsigned long long)(g_submit_hint_cache_stats.reused_flush_bytes / (1024ull * 1024ull)),
          (unsigned long long)g_submit_hint_cache_stats.built_flush_ranges,
          (unsigned long long)g_submit_hint_cache_stats.built_output_hints,
          (unsigned long long)g_submit_hint_cache_epoch,
          g_signature_submit_hint_cache.size());
}

static void NoteSubmitHintCacheLookup(bool hit,
                                      bool has_metadata,
                                      uint64_t flush_ranges,
                                      uint64_t output_hints,
                                      uint64_t flush_bytes) {
    std::lock_guard<std::mutex> lock(g_submit_hint_cache_stats_mutex);
    g_submit_hint_cache_stats.lookups++;
    if (!has_metadata) {
        g_submit_hint_cache_stats.no_metadata++;
    } else if (hit) {
        g_submit_hint_cache_stats.hits++;
        g_submit_hint_cache_stats.reused_flush_ranges += flush_ranges;
        g_submit_hint_cache_stats.reused_output_hints += output_hints;
        g_submit_hint_cache_stats.reused_flush_bytes += flush_bytes;
    } else {
        g_submit_hint_cache_stats.misses++;
        g_submit_hint_cache_stats.built_flush_ranges += flush_ranges;
        g_submit_hint_cache_stats.built_output_hints += output_hints;
    }
    MaybeLogSubmitHintCacheStatsLocked(hit ? "hit" : (has_metadata ? "miss" : "no_metadata"));
}

static void MaybeLogDescriptorHintCacheStatsLocked(const char* reason) {
    const uint64_t events = g_descriptor_hint_cache_stats.lookups +
                            g_descriptor_hint_cache_stats.invalidations +
                            g_descriptor_hint_cache_stats.dynamic_bypass;
    if (events == 0 || (events % kDescriptorHintCacheStatsLogEvery) != 0) return;
    ALOGI("[DESC_HINT_CACHE_SUMMARY] reason=%s lookups=%llu hits=%llu misses=%llu dynamic_bypass=%llu invalidations=%llu cached_sets=%zu reused_ranges=%llu reused_outputs=%llu reused_mb=%llu built_ranges=%llu built_outputs=%llu built_mb=%llu",
          reason ? reason : "periodic",
          (unsigned long long)g_descriptor_hint_cache_stats.lookups,
          (unsigned long long)g_descriptor_hint_cache_stats.hits,
          (unsigned long long)g_descriptor_hint_cache_stats.misses,
          (unsigned long long)g_descriptor_hint_cache_stats.dynamic_bypass,
          (unsigned long long)g_descriptor_hint_cache_stats.invalidations,
          g_descriptor_set_hint_cache.size(),
          (unsigned long long)g_descriptor_hint_cache_stats.reused_ranges,
          (unsigned long long)g_descriptor_hint_cache_stats.reused_outputs,
          (unsigned long long)(g_descriptor_hint_cache_stats.reused_bytes / (1024ull * 1024ull)),
          (unsigned long long)g_descriptor_hint_cache_stats.built_ranges,
          (unsigned long long)g_descriptor_hint_cache_stats.built_outputs,
          (unsigned long long)(g_descriptor_hint_cache_stats.built_bytes / (1024ull * 1024ull)));
}

static void NoteDescriptorHintCacheLookup(bool hit,
                                          bool dynamic_bypass,
                                          uint64_t flush_ranges,
                                          uint64_t output_hints,
                                          uint64_t bytes) {
    std::lock_guard<std::mutex> lock(g_submit_hint_cache_stats_mutex);
    g_descriptor_hint_cache_stats.lookups++;
    if (dynamic_bypass) {
        g_descriptor_hint_cache_stats.dynamic_bypass++;
    } else if (hit) {
        g_descriptor_hint_cache_stats.hits++;
        g_descriptor_hint_cache_stats.reused_ranges += flush_ranges;
        g_descriptor_hint_cache_stats.reused_outputs += output_hints;
        g_descriptor_hint_cache_stats.reused_bytes += bytes;
    } else {
        g_descriptor_hint_cache_stats.misses++;
        g_descriptor_hint_cache_stats.built_ranges += flush_ranges;
        g_descriptor_hint_cache_stats.built_outputs += output_hints;
        g_descriptor_hint_cache_stats.built_bytes += bytes;
    }
    MaybeLogDescriptorHintCacheStatsLocked(dynamic_bypass ? "dynamic_bypass" : (hit ? "hit" : "miss"));
}

static void NoteDescriptorHintCacheInvalidationLocked(const char* reason) {
    std::lock_guard<std::mutex> lock(g_submit_hint_cache_stats_mutex);
    g_descriptor_hint_cache_stats.invalidations++;
    MaybeLogDescriptorHintCacheStatsLocked(reason);
}

static void InvalidateAllDescriptorSetHintCachesLocked(const char* reason) {
    if (!g_descriptor_set_hint_cache.empty()) {
        g_descriptor_set_hint_cache.clear();
    }
    NoteDescriptorHintCacheInvalidationLocked(reason);
}

static uint64_t DescriptorSetVersionLocked(VkDescriptorSet set) {
    auto it = g_descriptor_set_versions.find(set);
    if (it != g_descriptor_set_versions.end()) return it->second;
    uint64_t version = g_descriptor_set_global_version++;
    if (g_descriptor_set_global_version == 0) g_descriptor_set_global_version = 1;
    g_descriptor_set_versions[set] = version;
    return version;
}

static void BumpDescriptorSetVersionLocked(VkDescriptorSet set, const char* reason) {
    if (set == VK_NULL_HANDLE) return;
    g_descriptor_set_versions[set] = g_descriptor_set_global_version++;
    if (g_descriptor_set_global_version == 0) g_descriptor_set_global_version = 1;
    g_descriptor_set_hint_cache.erase(set);
    NoteDescriptorHintCacheInvalidationLocked(reason);
}

static void EraseDescriptorSetHintCacheLocked(VkDescriptorSet set, const char* reason) {
    if (set == VK_NULL_HANDLE) return;
    g_descriptor_set_hint_cache.erase(set);
    g_descriptor_set_versions.erase(set);
    NoteDescriptorHintCacheInvalidationLocked(reason);
}

static uint64_t HashDescriptorSetUsesLocked(const std::unordered_map<uint64_t, DescriptorBufferUse>& uses) {
    uint64_t hash = 14695981039346656037ull;
    for (const auto& pair : uses) {
        const DescriptorBufferUse& use = pair.second;
        hash ^= pair.first; hash *= 1099511628211ull;
        hash ^= (uint64_t)(uintptr_t)use.buffer; hash *= 1099511628211ull;
        hash ^= (uint64_t)use.offset; hash *= 1099511628211ull;
        hash ^= (uint64_t)use.range; hash *= 1099511628211ull;
        hash ^= (uint64_t)use.access; hash *= 1099511628211ull;
        hash ^= (uint64_t)use.uses_dynamic_offset; hash *= 1099511628211ull;
    }
    return hash;
}

static void MaybeLogWaitInvalidateFusedStatsLocked(const char* reason) {
    const uint64_t events = g_wait_invalidate_fused_stats.calls +
                            g_wait_invalidate_fused_stats.fallback +
                            g_wait_invalidate_fused_stats.no_ranges;
    if (events == 0 || (events % kWaitInvalidateFusedStatsLogEvery) != 0) return;
    ALOGI("[WAIT_INVALIDATE_FUSED_SUMMARY] reason=%s calls=%llu ranges=%llu bytes_mb=%llu wait_us=%llu invalidate_us=%llu total_us=%llu fallback=%llu no_ranges=%llu timeout=%llu failed=%llu",
          reason ? reason : "periodic",
          (unsigned long long)g_wait_invalidate_fused_stats.calls,
          (unsigned long long)g_wait_invalidate_fused_stats.ranges,
          (unsigned long long)(g_wait_invalidate_fused_stats.bytes / (1024ull * 1024ull)),
          (unsigned long long)g_wait_invalidate_fused_stats.wait_us,
          (unsigned long long)g_wait_invalidate_fused_stats.invalidate_us,
          (unsigned long long)g_wait_invalidate_fused_stats.total_us,
          (unsigned long long)g_wait_invalidate_fused_stats.fallback,
          (unsigned long long)g_wait_invalidate_fused_stats.no_ranges,
          (unsigned long long)g_wait_invalidate_fused_stats.timeout,
          (unsigned long long)g_wait_invalidate_fused_stats.failed);
}

static void NoteWaitInvalidateFused(bool called,
                                    uint64_t ranges,
                                    uint64_t bytes,
                                    uint64_t wait_us,
                                    uint64_t invalidate_us,
                                    uint64_t total_us,
                                    bool fallback,
                                    bool no_ranges,
                                    VkResult result) {
    std::lock_guard<std::mutex> lock(g_submit_hint_cache_stats_mutex);
    if (called) {
        g_wait_invalidate_fused_stats.calls++;
        g_wait_invalidate_fused_stats.ranges += ranges;
        g_wait_invalidate_fused_stats.bytes += bytes;
        g_wait_invalidate_fused_stats.wait_us += wait_us;
        g_wait_invalidate_fused_stats.invalidate_us += invalidate_us;
        g_wait_invalidate_fused_stats.total_us += total_us;
    }
    if (fallback) g_wait_invalidate_fused_stats.fallback++;
    if (no_ranges) g_wait_invalidate_fused_stats.no_ranges++;
    if (result == VK_TIMEOUT) g_wait_invalidate_fused_stats.timeout++;
    else if (result != VK_SUCCESS) g_wait_invalidate_fused_stats.failed++;
    MaybeLogWaitInvalidateFusedStatsLocked(fallback ? "fallback" : (no_ranges ? "no_ranges" : "call"));
}

static void MaybeLogTargetedReadbackWaitStatsLocked(const char* reason) {
    const uint64_t events = g_targeted_readback_wait_stats.calls +
                            g_targeted_readback_wait_stats.timeout +
                            g_targeted_readback_wait_stats.failed;
    if (events == 0 || (events % kWaitInvalidateFusedStatsLogEvery) != 0) return;

    const uint64_t calls = g_targeted_readback_wait_stats.calls;
    ALOGI("[TARGETED_READBACK_WAIT_SUMMARY] reason=%s calls=%llu ranges=%llu bytes_mb=%llu wait_us=%llu invalidate_us=%llu total_us=%llu avg_wait_us=%llu avg_inv_us=%llu avg_total_us=%llu skipped_synced=%llu timeout=%llu failed=%llu prefer_host_fence=%d",
          reason ? reason : "periodic",
          (unsigned long long)calls,
          (unsigned long long)g_targeted_readback_wait_stats.ranges,
          (unsigned long long)(g_targeted_readback_wait_stats.bytes / (1024ull * 1024ull)),
          (unsigned long long)g_targeted_readback_wait_stats.wait_us,
          (unsigned long long)g_targeted_readback_wait_stats.invalidate_us,
          (unsigned long long)g_targeted_readback_wait_stats.total_us,
          (unsigned long long)(calls ? g_targeted_readback_wait_stats.wait_us / calls : 0),
          (unsigned long long)(calls ? g_targeted_readback_wait_stats.invalidate_us / calls : 0),
          (unsigned long long)(calls ? g_targeted_readback_wait_stats.total_us / calls : 0),
          (unsigned long long)g_targeted_readback_wait_stats.skipped_synced,
          (unsigned long long)g_targeted_readback_wait_stats.timeout,
          (unsigned long long)g_targeted_readback_wait_stats.failed,
          (int)kPreferHostFenceForWeakDescriptorReadback);
}

static void NoteTargetedReadbackWait(uint64_t ranges,
                                     uint64_t bytes,
                                     uint64_t wait_us,
                                     uint64_t invalidate_us,
                                     uint64_t total_us,
                                     uint64_t skipped_synced,
                                     VkResult result) {
    std::lock_guard<std::mutex> lock(g_submit_hint_cache_stats_mutex);
    TargetedReadbackWaitStats& s = g_targeted_readback_wait_stats;
    s.calls++;
    s.ranges += ranges;
    s.bytes += bytes;
    s.wait_us += wait_us;
    s.invalidate_us += invalidate_us;
    s.total_us += total_us;
    s.skipped_synced += skipped_synced;
    if (result == VK_TIMEOUT) {
        s.timeout++;
    } else if (result != VK_SUCCESS) {
        s.failed++;
    }
    MaybeLogTargetedReadbackWaitStatsLocked(result == VK_SUCCESS ? "success" : "non_success");
}

static void NoteWeakReadbackSuppression(uint64_t candidates,
                                        bool submit_hit,
                                        uint64_t strong_hints,
                                        uint64_t strong_bytes,
                                        uint64_t weak_dropped,
                                        uint64_t weak_dropped_bytes,
                                        uint64_t weak_kept,
                                        uint64_t weak_kept_bytes,
                                        uint64_t tiny_weak_kept,
                                        bool multi_submit_bypass) {
    if (candidates == 0 && strong_hints == 0 && weak_dropped == 0 &&
        weak_kept == 0 && !multi_submit_bypass) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_submit_hint_cache_stats_mutex);
    WeakReadbackSuppressionStats& s = g_weak_readback_suppression_stats;
    s.candidates += candidates;
    if (submit_hit) s.submit_hits++;
    s.strong_hints += strong_hints;
    s.strong_bytes += strong_bytes;
    s.weak_dropped += weak_dropped;
    s.weak_dropped_bytes += weak_dropped_bytes;
    s.weak_kept += weak_kept;
    s.weak_kept_bytes += weak_kept_bytes;
    s.tiny_weak_kept += tiny_weak_kept;
    if (multi_submit_bypass) s.multi_submit_bypass++;

    const uint64_t events = s.candidates + s.multi_submit_bypass;
    if (events != 0 && (events % kReadbackFenceStatsLogEvery) == 0) {
        ALOGI("[WEAK_READBACK_SUPPRESS_SUMMARY] candidates=%llu submit_hits=%llu strong_hints=%llu strong_mb=%llu weak_dropped=%llu weak_dropped_mb=%llu weak_kept=%llu weak_kept_mb=%llu tiny_kept=%llu multi_submit_bypass=%llu enabled=%d",
              (unsigned long long)s.candidates,
              (unsigned long long)s.submit_hits,
              (unsigned long long)s.strong_hints,
              (unsigned long long)(s.strong_bytes / (1024ull * 1024ull)),
              (unsigned long long)s.weak_dropped,
              (unsigned long long)(s.weak_dropped_bytes / (1024ull * 1024ull)),
              (unsigned long long)s.weak_kept,
              (unsigned long long)(s.weak_kept_bytes / (1024ull * 1024ull)),
              (unsigned long long)s.tiny_weak_kept,
              (unsigned long long)s.multi_submit_bypass,
              (int)kSuppressWeakReadbackWhenStrongCopyPresent);
    }
}

static void InvalidateAllSubmitHintCachesLocked(const char* reason) {
    g_submit_hint_cache_epoch++;
    if (g_submit_hint_cache_epoch == 0) g_submit_hint_cache_epoch = 1;
    if (g_signature_submit_hint_cache.size() > 4096) {
        g_signature_submit_hint_cache.clear();
    }
    std::lock_guard<std::mutex> stats_lock(g_submit_hint_cache_stats_mutex);
    g_submit_hint_cache_stats.invalidations++;
    MaybeLogSubmitHintCacheStatsLocked(reason);
}

static void InvalidateCommandBufferSubmitHintCacheLocked(VkCommandBuffer commandBuffer,
                                                         const char* reason) {
    if (commandBuffer != VK_NULL_HANDLE) {
        // g_command_buffer_submit_hint_cache.erase removed
    }
    std::lock_guard<std::mutex> stats_lock(g_submit_hint_cache_stats_mutex);
    g_submit_hint_cache_stats.invalidations++;
    MaybeLogSubmitHintCacheStatsLocked(reason);
}

static void MaybeLogLargeCleanRangeStatsLocked(const char* reason) {
    const uint64_t events = g_large_clean_range_stats.candidates +
                            g_large_clean_range_stats.hits +
                            g_large_clean_range_stats.verified +
                            g_large_clean_range_stats.failed;
    if (events == 0 || (events % kLargeCleanRangeStatsLogEvery) != 0) return;
    ALOGI("[LARGE_CLEAN_CACHE_SUMMARY] reason=%s candidates=%llu hits=%llu verified=%llu partial_verified=%llu partial_hits=%llu promoted=%llu failed=%llu skipped_mb=%llu verified_mb=%llu partial_verified_mb=%llu partial_saved_mb=%llu failed_mb=%llu ttl_miss=%llu range_miss=%llu partial_too_large=%llu no_shadow=%llu dirty_overlap=%llu cleared_by_capacity=%llu cleared_by_dirty=%llu verify_every=%llu ttl=%llu partial_max_kb=%llu",
          reason ? reason : "periodic",
          (unsigned long long)g_large_clean_range_stats.candidates,
          (unsigned long long)g_large_clean_range_stats.hits,
          (unsigned long long)g_large_clean_range_stats.verified,
          (unsigned long long)g_large_clean_range_stats.partial_verified,
          (unsigned long long)g_large_clean_range_stats.partial_hits,
          (unsigned long long)g_large_clean_range_stats.promoted,
          (unsigned long long)g_large_clean_range_stats.failed,
          (unsigned long long)(g_large_clean_range_stats.skipped_bytes / (1024ull * 1024ull)),
          (unsigned long long)(g_large_clean_range_stats.verified_bytes / (1024ull * 1024ull)),
          (unsigned long long)(g_large_clean_range_stats.partial_verified_bytes / (1024ull * 1024ull)),
          (unsigned long long)(g_large_clean_range_stats.partial_saved_bytes / (1024ull * 1024ull)),
          (unsigned long long)(g_large_clean_range_stats.failed_bytes / (1024ull * 1024ull)),
          (unsigned long long)g_large_clean_range_stats.ttl_miss,
          (unsigned long long)g_large_clean_range_stats.range_miss,
          (unsigned long long)g_large_clean_range_stats.partial_too_large,
          (unsigned long long)g_large_clean_range_stats.no_shadow,
          (unsigned long long)g_large_clean_range_stats.dirty_overlap,
          (unsigned long long)g_large_clean_range_stats.cleared_by_capacity,
          (unsigned long long)g_large_clean_range_stats.cleared_by_dirty,
          (unsigned long long)kLargeCleanRangeVerifyEvery,
          (unsigned long long)kLargeCleanRangeGenerationTtl,
          (unsigned long long)(kLargeCleanPartialVerifyMaxBytes / 1024ull));
}

static void NoteLargeCleanRangeCandidate() {
    std::lock_guard<std::mutex> lock(g_submit_hint_cache_stats_mutex);
    g_large_clean_range_stats.candidates++;
    MaybeLogLargeCleanRangeStatsLocked("candidate");
}

static void NoteLargeCleanRangeHit(uint64_t bytes) {
    std::lock_guard<std::mutex> lock(g_submit_hint_cache_stats_mutex);
    g_large_clean_range_stats.hits++;
    g_large_clean_range_stats.skipped_bytes += bytes;
    MaybeLogLargeCleanRangeStatsLocked("hit");
}

static void NoteLargeCleanRangeVerify(uint64_t bytes) {
    std::lock_guard<std::mutex> lock(g_submit_hint_cache_stats_mutex);
    g_large_clean_range_stats.verified++;
    g_large_clean_range_stats.verified_bytes += bytes;
    MaybeLogLargeCleanRangeStatsLocked("verify");
}

static void NoteLargeCleanRangePartialVerify(uint64_t verified_bytes,
                                             uint64_t saved_bytes,
                                             bool hit) {
    std::lock_guard<std::mutex> lock(g_submit_hint_cache_stats_mutex);
    g_large_clean_range_stats.partial_verified++;
    g_large_clean_range_stats.partial_verified_bytes += verified_bytes;
    g_large_clean_range_stats.partial_saved_bytes += saved_bytes;
    if (hit) g_large_clean_range_stats.partial_hits++;
    MaybeLogLargeCleanRangeStatsLocked(hit ? "partial_hit" : "partial_verify");
}

static void NoteLargeCleanRangePromote(uint64_t bytes) {
    std::lock_guard<std::mutex> lock(g_submit_hint_cache_stats_mutex);
    g_large_clean_range_stats.promoted++;
    MaybeLogLargeCleanRangeStatsLocked("promote");
}

static void NoteLargeCleanRangeFail(uint64_t bytes) {
    std::lock_guard<std::mutex> lock(g_submit_hint_cache_stats_mutex);
    g_large_clean_range_stats.failed++;
    g_large_clean_range_stats.failed_bytes += bytes;
    MaybeLogLargeCleanRangeStatsLocked("fail");
}

static void MaybeLogReadbackFenceStatsLocked(const char* reason) {
    const uint64_t events = g_readback_fence_stats.submits;
    if (events == 0 || (events % kReadbackFenceStatsLogEvery) != 0) return;
    ALOGI("[READBACK_FENCE_SUMMARY] reason=%s submits=%llu with_fence=%llu deferred=%llu blocked_readback=%llu no_fence=%llu output_hints=%llu output_ranges=%llu output_mb=%llu",
          reason ? reason : "periodic",
          (unsigned long long)g_readback_fence_stats.submits,
          (unsigned long long)g_readback_fence_stats.with_fence,
          (unsigned long long)g_readback_fence_stats.deferred,
          (unsigned long long)g_readback_fence_stats.blocked_readback,
          (unsigned long long)g_readback_fence_stats.no_fence,
          (unsigned long long)g_readback_fence_stats.output_hints,
          (unsigned long long)g_readback_fence_stats.output_ranges,
          (unsigned long long)(g_readback_fence_stats.output_bytes / (1024ull * 1024ull)));

    uint32_t top_sample_keys[4] = {};
    uint64_t top_samples[4] = {};
    uint32_t top_auto_byte_keys[4] = {};
    uint64_t top_auto_bytes[4] = {};
    for (const auto& item : g_readback_hint_class_stats) {
        const uint64_t samples = item.second.samples;
        for (size_t i = 0; i < 4; ++i) {
            if (samples <= top_samples[i]) continue;
            for (size_t j = 3; j > i; --j) {
                top_samples[j] = top_samples[j - 1];
                top_sample_keys[j] = top_sample_keys[j - 1];
            }
            top_samples[i] = samples;
            top_sample_keys[i] = item.first;
            break;
        }

        const uint64_t auto_bytes = item.second.auto_bytes;
        for (size_t i = 0; i < 4; ++i) {
            if (auto_bytes <= top_auto_bytes[i]) continue;
            for (size_t j = 3; j > i; --j) {
                top_auto_bytes[j] = top_auto_bytes[j - 1];
                top_auto_byte_keys[j] = top_auto_byte_keys[j - 1];
            }
            top_auto_bytes[i] = auto_bytes;
            top_auto_byte_keys[i] = item.first;
            break;
        }
    }

    auto log_class = [](const char* tag, size_t rank, uint32_t key,
                        const ReadbackHintClassStats& c) {
        const OutputHintSource source = (OutputHintSource)(key & 0x0fu);
        const OutputHintStrength strength = (OutputHintStrength)((key >> 4) & 0x0fu);
        const uint32_t auto_flag = (key >> 8) & 0x01u;
        const uint32_t wait_eligible_flag = (key >> 9) & 0x01u;
        const uint32_t size_bucket = (key >> 10) & 0x0fu;
        ALOGI("[%s] rank=%zu source=%s strength=%s size_bucket=%u auto_key=%u wait_key=%u samples=%llu auto=%llu wait_eligible=%llu deferred_submits=%llu blocked_submits=%llu bytes_mb=%llu auto_mb=%llu",
              tag,
              rank,
              OutputHintSourceName(source),
              OutputHintStrengthName(strength),
              size_bucket,
              auto_flag,
              wait_eligible_flag,
              (unsigned long long)c.samples,
              (unsigned long long)c.auto_readback,
              (unsigned long long)c.wait_eligible,
              (unsigned long long)c.deferred_submits,
              (unsigned long long)c.blocked_submits,
              (unsigned long long)(c.bytes / (1024ull * 1024ull)),
              (unsigned long long)(c.auto_bytes / (1024ull * 1024ull)));
    };

    for (size_t i = 0; i < 4; ++i) {
        if (top_samples[i] == 0) continue;
        log_class("READBACK_HINT_TOP", i + 1, top_sample_keys[i],
                  g_readback_hint_class_stats[top_sample_keys[i]]);
    }
    for (size_t i = 0; i < 4; ++i) {
        if (top_auto_bytes[i] == 0) continue;
        log_class("READBACK_HINT_AUTO_BYTES_TOP", i + 1, top_auto_byte_keys[i],
                  g_readback_hint_class_stats[top_auto_byte_keys[i]]);
    }
}

static uint32_t SyncPolicySizeBucket(VkDeviceSize size) {
    if (size <= 256ull) return 0;
    if (size <= 4096ull) return 1;
    if (size <= 64ull * 1024ull) return 2;
    if (size <= 1024ull * 1024ull) return 3;
    if (size <= 4ull * 1024ull * 1024ull) return 4;
    if (size <= 16ull * 1024ull * 1024ull) return 5;
    if (size <= 64ull * 1024ull * 1024ull) return 6;
    return 7;
}

static uint32_t SyncPolicyOffsetBucket(VkDeviceSize offset) {
    if (offset == 0) return 0;
    if (offset <= 4096ull) return 1;
    if (offset <= 1024ull * 1024ull) return 2;
    return 3;
}

static uint32_t SyncPolicyCleanStreakBucket(uint32_t streak) {
    if (streak == 0) return 0;
    if (streak == 1) return 1;
    if (streak <= 4) return 2;
    if (streak <= 16) return 3;
    return 4;
}

static bool SyncPolicyIsAggressiveCleanCandidate(VkDeviceSize offset,
                                                 VkDeviceSize size,
                                                 bool registered,
                                                 uint32_t clean_streak,
                                                 uint64_t* verify_every) {
    if (verify_every) *verify_every = 0;
    if (!kEnableAggressiveCleanHintSkip || !registered || offset != 0 ||
        clean_streak < kAggressiveCleanStreakThreshold) {
        return false;
    }

    const uint32_t size_bucket = SyncPolicySizeBucket(size);
    if (size_bucket == 6) {
        if (verify_every) *verify_every = kAggressiveLargeCleanVerifyEvery;
        return true;
    }
    if (size_bucket == 3) {
        if (verify_every) *verify_every = kAggressiveMediumCleanVerifyEvery;
        return true;
    }
    return false;
}

static bool SyncPolicyShouldVerifyAggressiveClean(uint64_t verify_every) {
    return verify_every != 0 && (g_submit_generation % verify_every) == 0;
}

static bool SyncPolicyIsDirectDirtySmallCandidate(VkDeviceSize offset,
                                                  VkDeviceSize size,
                                                  bool registered,
                                                  uint32_t clean_streak) {
    return kEnableDirectDirtySmallHintFlush &&
           registered &&
           offset == 0 &&
           clean_streak == 0 &&
           SyncPolicySizeBucket(size) == 2;
}

static bool SyncPolicyIsLargeCleanCacheCandidate(VkDeviceSize size,
                                                 bool registered) {
    return kEnableLargeCleanRangeVerifiedCache &&
           registered &&
           size >= kLargeCleanRangeMinBytes;
}

static bool SyncPolicyShouldVerifyLargeClean() {
    return kLargeCleanRangeVerifyEvery == 0 ||
           (g_submit_generation % kLargeCleanRangeVerifyEvery) == 0;
}

static uint32_t MakeSyncPolicyKey(SyncPolicySource source,
                                  VkDeviceSize offset,
                                  VkDeviceSize size,
                                  bool registered,
                                  uint32_t clean_streak) {
    return ((uint32_t)source & 0x0fu) |
           (SyncPolicySizeBucket(size) << 4) |
           (SyncPolicyOffsetBucket(offset) << 8) |
           ((registered ? 1u : 0u) << 10) |
           (SyncPolicyCleanStreakBucket(clean_streak) << 11);
}

static const char* SyncPolicySourceName(uint32_t key) {
    switch ((SyncPolicySource)(key & 0x0fu)) {
    case SyncPolicySource::kHintRange: return "hint_range";
    case SyncPolicySource::kHintMemory: return "hint_memory";
    case SyncPolicySource::kNoHintScan: return "no_hint_scan";
    case SyncPolicySource::kNoHintCleanSkip: return "no_hint_clean_skip";
    case SyncPolicySource::kMetadataSkip: return "metadata_skip";
    case SyncPolicySource::kEmpty: return "empty";
    default: return "unknown";
    }
}

static const char* OutputHintSourceName(OutputHintSource source) {
    switch (source) {
    case OutputHintSource::kDescriptorMaybeWrite: return "descriptor_maybe_write";
    case OutputHintSource::kCopyBufferDst: return "copy_buffer_dst";
    default: return "unknown";
    }
}

static const char* OutputHintStrengthName(OutputHintStrength strength) {
    switch (strength) {
    case OutputHintStrength::kWeakMaybeWrite: return "weak_maybe_write";
    case OutputHintStrength::kStrongReadback: return "strong_readback";
    default: return "unknown";
    }
}

static const char* InvalidateChangeClassName(InvalidateChangeClass cls) {
    switch (cls) {
    case InvalidateChangeClass::kExplicit: return "explicit_or_global";
    case InvalidateChangeClass::kDeferredWeakReadback: return "deferred_weak";
    case InvalidateChangeClass::kTargetedStrongReadback: return "targeted_strong";
    case InvalidateChangeClass::kOther: return "other";
    default: return "unknown";
    }
}

static InvalidateChangeClass ClassifyInvalidateReason(const char* reason) {
    if (!reason || reason[0] == '\0') {
        return InvalidateChangeClass::kExplicit;
    }
    if (strstr(reason, "targeted_readback") != nullptr) {
        return InvalidateChangeClass::kTargetedStrongReadback;
    }
    if (strstr(reason, "deferred_readback") != nullptr) {
        return InvalidateChangeClass::kDeferredWeakReadback;
    }
    return InvalidateChangeClass::kOther;
}

static void MaybeLogInvalidateChangeStatsLocked(const char* reason,
                                                InvalidateChangeClass changed_class,
                                                bool force) {
    uint64_t total_calls = 0;
    for (const auto& cls : g_invalidate_change_stats.classes) {
        total_calls += cls.calls;
    }
    if (!force && (total_calls == 0 || (total_calls % kReadbackFenceStatsLogEvery) != 0)) {
        return;
    }

    ALOGI("[INVALIDATE_CHANGE_SUMMARY] reason=%s changed_class=%s total_calls=%llu sample_bytes=%llu",
          reason ? reason : "periodic",
          InvalidateChangeClassName(changed_class),
          (unsigned long long)total_calls,
          (unsigned long long)kReadbackChangeSampleBytes);
    for (size_t i = 0; i < (size_t)InvalidateChangeClass::kCount; ++i) {
        const auto& cls = g_invalidate_change_stats.classes[i];
        if (cls.calls == 0) continue;
        const uint64_t compared = cls.changed_samples + cls.clean_samples;
        const uint64_t changed_rate_x1000 =
            compared ? (cls.changed_samples * 1000ull) / compared : 0;
        ALOGI("[INVALIDATE_CHANGE_CLASS] class=%s calls=%llu ranges=%llu bytes_mb=%llu sample_kb=%llu changed=%llu clean=%llu changed_rate_x1000=%llu no_shadow=%llu untracked=%llu",
              InvalidateChangeClassName((InvalidateChangeClass)i),
              (unsigned long long)cls.calls,
              (unsigned long long)cls.ranges,
              (unsigned long long)(cls.bytes / (1024ull * 1024ull)),
              (unsigned long long)(cls.sample_bytes / 1024ull),
              (unsigned long long)cls.changed_samples,
              (unsigned long long)cls.clean_samples,
              (unsigned long long)changed_rate_x1000,
              (unsigned long long)cls.no_shadow,
              (unsigned long long)cls.untracked);
    }
}

static void NoteInvalidateChangeStats(InvalidateChangeClass cls,
                                      uint64_t ranges,
                                      uint64_t bytes,
                                      uint64_t sample_bytes,
                                      uint64_t changed_samples,
                                      uint64_t clean_samples,
                                      uint64_t no_shadow,
                                      uint64_t untracked,
                                      const char* reason) {
    std::lock_guard<std::mutex> lock(g_submit_hint_cache_stats_mutex);
    InvalidateChangeClassStats& dst =
        g_invalidate_change_stats.classes[(size_t)cls];
    dst.calls++;
    dst.ranges += ranges;
    dst.bytes += bytes;
    dst.sample_bytes += sample_bytes;
    dst.changed_samples += changed_samples;
    dst.clean_samples += clean_samples;
    dst.no_shadow += no_shadow;
    dst.untracked += untracked;
    MaybeLogInvalidateChangeStatsLocked(reason, cls, false);
}

static uint32_t MakeReadbackHintClassKey(const OutputMemoryRangeHint& hint,
                                         bool auto_readback) {
    return ((uint32_t)hint.source & 0x0fu) |
           (((uint32_t)hint.strength & 0x0fu) << 4) |
           ((auto_readback ? 1u : 0u) << 8) |
           ((hint.wait_commit_eligible ? 1u : 0u) << 9) |
           (SyncPolicySizeBucket(hint.range.size) << 10);
}

static void AddReadbackHintClassObservation(
    std::vector<std::pair<uint32_t, ReadbackHintClassStats>>* classes,
    uint32_t key,
    const OutputMemoryRangeHint& hint,
    bool auto_readback) {
    if (!classes) return;

    ReadbackHintClassStats* stats = nullptr;
    for (auto& item : *classes) {
        if (item.first == key) {
            stats = &item.second;
            break;
        }
    }
    if (!stats) {
        classes->push_back({key, {}});
        stats = &classes->back().second;
    }

    stats->samples++;
    if (auto_readback) stats->auto_readback++;
    if (hint.wait_commit_eligible) stats->wait_eligible++;
    stats->bytes += hint.range.size;
    if (auto_readback) stats->auto_bytes += hint.range.size;
}

static void AddSyncPolicyObservation(SyncPolicyBatch* batch,
                                     uint32_t key,
                                     uint64_t samples,
                                     uint64_t bytes,
                                     uint64_t dirty_bytes,
                                     bool clean,
                                     bool dirty,
                                     bool unknown,
                                     bool cache_skip,
                                     bool action_flush,
                                     bool action_skip,
                                     uint64_t cost_us,
                                     uint64_t aggressive_clean_skip = 0,
                                     uint64_t direct_dirty_flush = 0,
                                     uint64_t sample_verify = 0) {
    if (!batch || samples == 0) return;

    SyncPolicyClassStats* stats = nullptr;
    for (auto& item : batch->classes) {
        if (item.first == key) {
            stats = &item.second;
            break;
        }
    }
    if (!stats) {
        batch->classes.push_back({key, {}});
        stats = &batch->classes.back().second;
    }

    stats->samples += samples;
    if (clean) stats->clean += samples;
    if (dirty) stats->dirty += samples;
    if (unknown) stats->unknown += samples;
    if (cache_skip) stats->cache_skip += samples;
    if (action_flush) stats->action_flush += samples;
    if (action_skip) stats->action_skip += samples;
    stats->aggressive_clean_skip += aggressive_clean_skip;
    stats->direct_dirty_flush += direct_dirty_flush;
    stats->sample_verify += sample_verify;
    stats->bytes += bytes;
    stats->dirty_bytes += dirty_bytes;
    stats->cost_us += cost_us;
}

static void AddSyncPolicyBatchCost(SyncPolicyBatch* batch, uint64_t cost_us) {
    if (!batch || cost_us == 0) return;
    uint64_t total_samples = 0;
    for (const auto& item : batch->classes) {
        total_samples += item.second.samples;
    }
    if (total_samples == 0) return;
    for (auto& item : batch->classes) {
        item.second.cost_us += (cost_us * item.second.samples) / total_samples;
    }
}

static void NoteSyncPolicyBatch(const SyncPolicyBatch& batch) {
    if (batch.classes.empty()) return;

    std::lock_guard<std::mutex> lock(g_submit_hint_stats_mutex);
    SyncPolicyStats& s = g_sync_policy_stats;
    s.batches++;

    for (const auto& item : batch.classes) {
        const uint32_t key = item.first;
        const SyncPolicyClassStats& delta = item.second;
        SyncPolicyClassStats& dst = s.classes[key];
        dst.samples += delta.samples;
        dst.clean += delta.clean;
        dst.dirty += delta.dirty;
        dst.unknown += delta.unknown;
        dst.cache_skip += delta.cache_skip;
        dst.action_flush += delta.action_flush;
        dst.action_skip += delta.action_skip;
        dst.aggressive_clean_skip += delta.aggressive_clean_skip;
        dst.direct_dirty_flush += delta.direct_dirty_flush;
        dst.sample_verify += delta.sample_verify;
        dst.bytes += delta.bytes;
        dst.dirty_bytes += delta.dirty_bytes;
        dst.cost_us += delta.cost_us;

        s.samples += delta.samples;
        s.clean += delta.clean;
        s.dirty += delta.dirty;
        s.unknown += delta.unknown;
        s.cache_skip += delta.cache_skip;
        s.action_flush += delta.action_flush;
        s.action_skip += delta.action_skip;
        s.aggressive_clean_skip += delta.aggressive_clean_skip;
        s.direct_dirty_flush += delta.direct_dirty_flush;
        s.sample_verify += delta.sample_verify;
        s.bytes += delta.bytes;
        s.dirty_bytes += delta.dirty_bytes;
        s.cost_us += delta.cost_us;
    }

    if (s.batches == 0 || (s.batches % kSyncPolicyStatsLogEvery) != 0) {
        return;
    }

    ALOGI("[SYNC_POLICY_SUMMARY] batches=%llu samples=%llu classes=%zu "
          "clean=%llu dirty=%llu unknown=%llu cache_skip=%llu "
          "action_flush=%llu action_skip=%llu aggressive_clean_skip=%llu "
          "direct_dirty_flush=%llu sample_verify=%llu bytes_mb=%llu "
          "dirty_mb=%llu avg_cost_us=%llu",
          (unsigned long long)s.batches,
          (unsigned long long)s.samples,
          s.classes.size(),
          (unsigned long long)s.clean,
          (unsigned long long)s.dirty,
          (unsigned long long)s.unknown,
          (unsigned long long)s.cache_skip,
          (unsigned long long)s.action_flush,
          (unsigned long long)s.action_skip,
          (unsigned long long)s.aggressive_clean_skip,
          (unsigned long long)s.direct_dirty_flush,
          (unsigned long long)s.sample_verify,
          (unsigned long long)(s.bytes / (1024ull * 1024ull)),
          (unsigned long long)(s.dirty_bytes / (1024ull * 1024ull)),
          (unsigned long long)(s.samples ? s.cost_us / s.samples : 0));

    uint32_t top_keys[4] = {};
    uint64_t top_samples[4] = {};
    for (const auto& item : s.classes) {
        const uint64_t samples = item.second.samples;
        for (size_t i = 0; i < 4; ++i) {
            if (samples <= top_samples[i]) continue;
            for (size_t j = 3; j > i; --j) {
                top_samples[j] = top_samples[j - 1];
                top_keys[j] = top_keys[j - 1];
            }
            top_samples[i] = samples;
            top_keys[i] = item.first;
            break;
        }
    }

    for (size_t i = 0; i < 4; ++i) {
        if (top_samples[i] == 0) continue;
        const uint32_t key = top_keys[i];
        const SyncPolicyClassStats& c = s.classes[key];
        const uint64_t known = c.clean + c.dirty;
        const uint64_t dirty_rate_x1000 = known ? (c.dirty * 1000ull) / known : 0;
        ALOGI("[SYNC_POLICY_TOP] rank=%zu source=%s size_bucket=%u offset_bucket=%u "
              "registered=%u clean_streak_bucket=%u samples=%llu clean=%llu "
              "dirty=%llu unknown=%llu cache_skip=%llu flush=%llu skip=%llu "
              "aggressive_clean_skip=%llu direct_dirty_flush=%llu "
              "sample_verify=%llu dirty_rate_x1000=%llu bytes_mb=%llu "
              "dirty_mb=%llu avg_cost_us=%llu",
              i + 1,
              SyncPolicySourceName(key),
              (key >> 4) & 0x0fu,
              (key >> 8) & 0x03u,
              (key >> 10) & 0x01u,
              (key >> 11) & 0x07u,
              (unsigned long long)c.samples,
              (unsigned long long)c.clean,
              (unsigned long long)c.dirty,
              (unsigned long long)c.unknown,
              (unsigned long long)c.cache_skip,
              (unsigned long long)c.action_flush,
              (unsigned long long)c.action_skip,
              (unsigned long long)c.aggressive_clean_skip,
              (unsigned long long)c.direct_dirty_flush,
              (unsigned long long)c.sample_verify,
              (unsigned long long)dirty_rate_x1000,
              (unsigned long long)(c.bytes / (1024ull * 1024ull)),
              (unsigned long long)(c.dirty_bytes / (1024ull * 1024ull)),
              (unsigned long long)(c.samples ? c.cost_us / c.samples : 0));
    }

    uint32_t cost_keys[4] = {};
    uint64_t top_cost[4] = {};
    for (const auto& item : s.classes) {
        const uint64_t cost_us = item.second.cost_us;
        for (size_t i = 0; i < 4; ++i) {
            if (cost_us <= top_cost[i]) continue;
            for (size_t j = 3; j > i; --j) {
                top_cost[j] = top_cost[j - 1];
                cost_keys[j] = cost_keys[j - 1];
            }
            top_cost[i] = cost_us;
            cost_keys[i] = item.first;
            break;
        }
    }

    for (size_t i = 0; i < 4; ++i) {
        if (top_cost[i] == 0) continue;
        const uint32_t key = cost_keys[i];
        const SyncPolicyClassStats& c = s.classes[key];
        const uint64_t known = c.clean + c.dirty;
        const uint64_t dirty_rate_x1000 = known ? (c.dirty * 1000ull) / known : 0;
        ALOGI("[SYNC_POLICY_COST_TOP] rank=%zu source=%s size_bucket=%u "
              "offset_bucket=%u registered=%u clean_streak_bucket=%u "
              "samples=%llu clean=%llu dirty=%llu unknown=%llu cache_skip=%llu "
              "flush=%llu skip=%llu aggressive_clean_skip=%llu "
              "direct_dirty_flush=%llu sample_verify=%llu dirty_rate_x1000=%llu "
              "cost_us=%llu avg_cost_us=%llu bytes_mb=%llu dirty_mb=%llu",
              i + 1,
              SyncPolicySourceName(key),
              (key >> 4) & 0x0fu,
              (key >> 8) & 0x03u,
              (key >> 10) & 0x01u,
              (key >> 11) & 0x07u,
              (unsigned long long)c.samples,
              (unsigned long long)c.clean,
              (unsigned long long)c.dirty,
              (unsigned long long)c.unknown,
              (unsigned long long)c.cache_skip,
              (unsigned long long)c.action_flush,
              (unsigned long long)c.action_skip,
              (unsigned long long)c.aggressive_clean_skip,
              (unsigned long long)c.direct_dirty_flush,
              (unsigned long long)c.sample_verify,
              (unsigned long long)dirty_rate_x1000,
              (unsigned long long)c.cost_us,
              (unsigned long long)(c.samples ? c.cost_us / c.samples : 0),
              (unsigned long long)(c.bytes / (1024ull * 1024ull)),
              (unsigned long long)(c.dirty_bytes / (1024ull * 1024ull)));
    }

    uint32_t byte_keys[4] = {};
    uint64_t top_bytes[4] = {};
    for (const auto& item : s.classes) {
        const uint64_t bytes = item.second.bytes;
        for (size_t i = 0; i < 4; ++i) {
            if (bytes <= top_bytes[i]) continue;
            for (size_t j = 3; j > i; --j) {
                top_bytes[j] = top_bytes[j - 1];
                byte_keys[j] = byte_keys[j - 1];
            }
            top_bytes[i] = bytes;
            byte_keys[i] = item.first;
            break;
        }
    }

    for (size_t i = 0; i < 4; ++i) {
        if (top_bytes[i] == 0) continue;
        const uint32_t key = byte_keys[i];
        const SyncPolicyClassStats& c = s.classes[key];
        const uint64_t known = c.clean + c.dirty;
        const uint64_t dirty_rate_x1000 = known ? (c.dirty * 1000ull) / known : 0;
        ALOGI("[SYNC_POLICY_BYTES_TOP] rank=%zu source=%s size_bucket=%u "
              "offset_bucket=%u registered=%u clean_streak_bucket=%u "
              "samples=%llu clean=%llu dirty=%llu unknown=%llu cache_skip=%llu "
              "flush=%llu skip=%llu aggressive_clean_skip=%llu "
              "direct_dirty_flush=%llu sample_verify=%llu dirty_rate_x1000=%llu "
              "bytes_mb=%llu dirty_mb=%llu cost_us=%llu avg_cost_us=%llu",
              i + 1,
              SyncPolicySourceName(key),
              (key >> 4) & 0x0fu,
              (key >> 8) & 0x03u,
              (key >> 10) & 0x01u,
              (key >> 11) & 0x07u,
              (unsigned long long)c.samples,
              (unsigned long long)c.clean,
              (unsigned long long)c.dirty,
              (unsigned long long)c.unknown,
              (unsigned long long)c.cache_skip,
              (unsigned long long)c.action_flush,
              (unsigned long long)c.action_skip,
              (unsigned long long)c.aggressive_clean_skip,
              (unsigned long long)c.direct_dirty_flush,
              (unsigned long long)c.sample_verify,
              (unsigned long long)dirty_rate_x1000,
              (unsigned long long)(c.bytes / (1024ull * 1024ull)),
              (unsigned long long)(c.dirty_bytes / (1024ull * 1024ull)),
              (unsigned long long)c.cost_us,
              (unsigned long long)(c.samples ? c.cost_us / c.samples : 0));
    }

    uint32_t unknown_keys[4] = {};
    uint64_t unknown_samples[4] = {};
    for (const auto& item : s.classes) {
        const uint64_t unknown = item.second.unknown;
        for (size_t i = 0; i < 4; ++i) {
            if (unknown <= unknown_samples[i]) continue;
            for (size_t j = 3; j > i; --j) {
                unknown_samples[j] = unknown_samples[j - 1];
                unknown_keys[j] = unknown_keys[j - 1];
            }
            unknown_samples[i] = unknown;
            unknown_keys[i] = item.first;
            break;
        }
    }

    for (size_t i = 0; i < 4; ++i) {
        if (unknown_samples[i] == 0) continue;
        const uint32_t key = unknown_keys[i];
        const SyncPolicyClassStats& c = s.classes[key];
        ALOGI("[SYNC_POLICY_UNKNOWN_TOP] rank=%zu source=%s size_bucket=%u "
              "offset_bucket=%u registered=%u clean_streak_bucket=%u "
              "samples=%llu unknown=%llu bytes_mb=%llu avg_cost_us=%llu",
              i + 1,
              SyncPolicySourceName(key),
              (key >> 4) & 0x0fu,
              (key >> 8) & 0x03u,
              (key >> 10) & 0x01u,
              (key >> 11) & 0x07u,
              (unsigned long long)c.samples,
              (unsigned long long)c.unknown,
              (unsigned long long)(c.bytes / (1024ull * 1024ull)),
              (unsigned long long)(c.samples ? c.cost_us / c.samples : 0));
    }
}

static size_t SubmitRpcStatsBucket(uint64_t us) {
    static const uint64_t kBounds[] = {
        10ull, 25ull, 50ull, 100ull, 200ull, 500ull,
        1000ull, 2000ull, 5000ull, 10000ull, 20000ull
    };
    for (size_t i = 0; i < sizeof(kBounds) / sizeof(kBounds[0]); ++i) {
        if (us <= kBounds[i]) return i;
    }
    return sizeof(kBounds) / sizeof(kBounds[0]);
}

static uint64_t SubmitRpcStatsP95BucketUs(const uint64_t buckets[12], uint64_t total) {
    if (total == 0) return 0;
    static const uint64_t kBounds[] = {
        10ull, 25ull, 50ull, 100ull, 200ull, 500ull,
        1000ull, 2000ull, 5000ull, 10000ull, 20000ull, 20001ull
    };
    const uint64_t rank = (total * 95ull + 99ull) / 100ull;
    uint64_t seen = 0;
    for (size_t i = 0; i < 12; ++i) {
        seen += buckets[i];
        if (seen >= rank) return kBounds[i];
    }
    return kBounds[11];
}

static void MaybeLogGuestSyncBreakdownStatsLocked(const char* reason) {
    if (!kEnableGuestSyncBreakdownLog) return;

    GuestSyncBreakdownStats& s = g_guest_sync_breakdown_stats;
    const uint64_t now_us = ExpressVkNowUs();
    if (s.window_start_us == 0) {
        s.window_start_us = now_us;
    }

    const uint64_t window_us = now_us > s.window_start_us ?
        now_us - s.window_start_us : 0;

    const uint64_t activity =
        s.implicit_calls + s.mapped_flush_calls +
        s.mapped_invalidate_calls + s.conservative_readback_calls;
    if (window_us < kGuestSyncBreakdownLogIntervalUs &&
        activity < kGuestSyncBreakdownForceEvents) {
        return;
    }
    if (activity == 0) {
        s.window_start_us = now_us;
        return;
    }

    ALOGI("[GUEST_SYNC_BREAKDOWN] reason=%s window_ms=%llu "
          "implicit_calls=%llu scan=%llu hint=%llu skip=%llu empty=%llu "
          "mapped_seen=%llu scanned_mb=%llu compared_chunks=%llu "
          "dirty_chunks=%llu dirty_ranges=%llu dirty_mb=%llu "
          "implicit_scan_us=%llu implicit_rpc_us=%llu implicit_total_us=%llu "
          "flush_calls=%llu flush_ranges=%llu flush_mb=%llu "
          "flush_copy_us=%llu flush_encode_us=%llu flush_addptr_us=%llu "
          "flush_rpc_us=%llu flush_record_us=%llu flush_shadow_us=%llu "
          "flush_total_us=%llu "
          "invalidate_calls=%llu invalidate_ranges=%llu invalidate_mb=%llu "
          "invalidate_skipped=%llu invalidate_copy_us=%llu "
          "invalidate_param_us=%llu invalidate_addptr_us=%llu "
          "invalidate_rpc_us=%llu invalidate_update_us=%llu "
          "invalidate_total_us=%llu "
          "conservative_readback_calls=%llu conservative_ranges=%llu "
          "conservative_mb=%llu conservative_skipped=%llu "
          "preserve_mb=%llu preserve_us=%llu readback_invalidate_us=%llu "
          "readback_total_us=%llu",
          reason ? reason : "periodic",
          (unsigned long long)(window_us / 1000ull),
          (unsigned long long)s.implicit_calls,
          (unsigned long long)s.implicit_scan_calls,
          (unsigned long long)s.implicit_hint_calls,
          (unsigned long long)s.implicit_skip_calls,
          (unsigned long long)s.implicit_empty_calls,
          (unsigned long long)s.implicit_mapped_seen,
          (unsigned long long)(s.implicit_scanned_bytes / (1024ull * 1024ull)),
          (unsigned long long)s.implicit_compared_chunks,
          (unsigned long long)s.implicit_dirty_chunks,
          (unsigned long long)s.implicit_dirty_ranges,
          (unsigned long long)(s.implicit_dirty_bytes / (1024ull * 1024ull)),
          (unsigned long long)s.implicit_scan_us,
          (unsigned long long)s.implicit_rpc_us,
          (unsigned long long)s.implicit_total_us,
          (unsigned long long)s.mapped_flush_calls,
          (unsigned long long)s.mapped_flush_ranges,
          (unsigned long long)(s.mapped_flush_bytes / (1024ull * 1024ull)),
          (unsigned long long)s.mapped_flush_copy_us,
          (unsigned long long)s.mapped_flush_encode_us,
          (unsigned long long)s.mapped_flush_addptr_us,
          (unsigned long long)s.mapped_flush_rpc_us,
          (unsigned long long)s.mapped_flush_record_us,
          (unsigned long long)s.mapped_flush_shadow_us,
          (unsigned long long)s.mapped_flush_total_us,
          (unsigned long long)s.mapped_invalidate_calls,
          (unsigned long long)s.mapped_invalidate_ranges,
          (unsigned long long)(s.mapped_invalidate_bytes / (1024ull * 1024ull)),
          (unsigned long long)s.mapped_invalidate_skipped_synced,
          (unsigned long long)s.mapped_invalidate_copy_us,
          (unsigned long long)s.mapped_invalidate_param_us,
          (unsigned long long)s.mapped_invalidate_addptr_us,
          (unsigned long long)s.mapped_invalidate_rpc_us,
          (unsigned long long)s.mapped_invalidate_update_us,
          (unsigned long long)s.mapped_invalidate_total_us,
          (unsigned long long)s.conservative_readback_calls,
          (unsigned long long)s.conservative_readback_ranges,
          (unsigned long long)(s.conservative_readback_bytes / (1024ull * 1024ull)),
          (unsigned long long)s.conservative_readback_skipped_synced,
          (unsigned long long)(s.conservative_preserve_bytes / (1024ull * 1024ull)),
          (unsigned long long)s.conservative_preserve_us,
          (unsigned long long)s.conservative_invalidate_us,
          (unsigned long long)s.conservative_total_us);

    g_guest_sync_breakdown_stats = {};
    g_guest_sync_breakdown_stats.window_start_us = now_us;
}

static void NoteSubmitCohortStats(const char* api_name,
                                  VkQueue queue,
                                  uint32_t submit_count,
                                  uint64_t command_buffers,
                                  uint64_t wait_semaphores,
                                  uint64_t signal_semaphores,
                                  bool has_fence,
                                  bool deferred_fence,
                                  uint64_t submit_begin_us,
                                  uint64_t submit_end_us) {
    std::lock_guard<std::mutex> lock(g_submit_hint_stats_mutex);
    SubmitCohortStats& s = g_submit_cohort_stats;
    const bool has_prev = s.calls != 0;
    const bool same_queue = has_prev && s.last_queue == queue;
    const bool sync_fence = has_fence && !deferred_fence;
    const bool single_submit = submit_count == 1;
    const bool sync_empty = wait_semaphores == 0 &&
                            signal_semaphores == 0 &&
                            !sync_fence;
    const bool mergeable = same_queue && single_submit && sync_empty;

    s.calls++;
    if (same_queue) {
        s.same_queue_as_prev++;
        uint64_t gap_us = 0;
        if (submit_begin_us >= s.last_submit_end_us) {
            gap_us = submit_begin_us - s.last_submit_end_us;
        }
        s.total_gap_us += gap_us;
        if (s.min_gap_us == 0 || gap_us < s.min_gap_us) {
            s.min_gap_us = gap_us;
        }
        s.max_gap_us = std::max<uint64_t>(s.max_gap_us, gap_us);
        s.gap_buckets[SubmitRpcStatsBucket(gap_us)]++;
    } else if (has_prev) {
        s.blocked_by_queue_change++;
    }
    if (mergeable) s.mergeable++;
    if (wait_semaphores != 0) s.blocked_by_wait_semaphore++;
    if (signal_semaphores != 0) s.blocked_by_signal_semaphore++;
    if (sync_fence) s.blocked_by_sync_fence++;
    if (!single_submit) s.blocked_by_multi_submit++;
    if (sync_empty) s.empty_sync++;
    if (command_buffers == 1) s.single_cmd++;
    s.command_buffers += command_buffers;
    s.last_queue = queue;
    s.last_submit_end_us = submit_end_us;

    if (s.calls != 0 && (s.calls % kSubmitCohortStatsLogEvery) == 0) {
        const uint64_t avg_gap_us =
            s.same_queue_as_prev ? s.total_gap_us / s.same_queue_as_prev : 0;
        const uint64_t avg_cmds = s.calls ? s.command_buffers / s.calls : 0;
        ALOGI("[SUBMIT_COHORT_SUMMARY] calls=%llu last_api=%s same_queue=%llu "
              "mergeable=%llu empty_sync=%llu single_cmd=%llu avg_cmds=%llu "
              "blocked_wait=%llu blocked_signal=%llu blocked_sync_fence=%llu "
              "blocked_multi_submit=%llu blocked_queue_change=%llu "
              "avg_gap_us=%llu p95_gap_bucket_us=%llu min_gap_us=%llu "
              "max_gap_us=%llu",
              (unsigned long long)s.calls,
              api_name ? api_name : "unknown",
              (unsigned long long)s.same_queue_as_prev,
              (unsigned long long)s.mergeable,
              (unsigned long long)s.empty_sync,
              (unsigned long long)s.single_cmd,
              (unsigned long long)avg_cmds,
              (unsigned long long)s.blocked_by_wait_semaphore,
              (unsigned long long)s.blocked_by_signal_semaphore,
              (unsigned long long)s.blocked_by_sync_fence,
              (unsigned long long)s.blocked_by_multi_submit,
              (unsigned long long)s.blocked_by_queue_change,
              (unsigned long long)avg_gap_us,
              (unsigned long long)SubmitRpcStatsP95BucketUs(s.gap_buckets,
                                                            s.same_queue_as_prev),
              (unsigned long long)(s.same_queue_as_prev ? s.min_gap_us : 0),
              (unsigned long long)s.max_gap_us);
    }
}

static void NoteSubmitRpcStats(const char* api_name,
                               uint32_t submit_count,
                               uint64_t command_buffers,
                               uint64_t wait_semaphores,
                               uint64_t signal_semaphores,
                               bool has_fence,
                               bool deferred_fence,
                               bool sync_write,
                               uint64_t hint_us,
                               uint64_t encode_us,
                               uint64_t write_us,
                               uint64_t total_us) {
    std::lock_guard<std::mutex> lock(g_submit_hint_stats_mutex);
    SubmitRpcStats& s = g_submit_rpc_stats;
    const uint64_t now_us = ExpressVkNowUs();
    if (s.first_call_us == 0) {
        s.first_call_us = now_us;
    }
    s.last_call_us = now_us;
    if (api_name && strcmp(api_name, "vkQueueSubmit2") == 0) {
        s.submit2_calls++;
    } else {
        s.submit_calls++;
    }
    s.submit_batches += submit_count;
    s.command_buffers += command_buffers;
    s.wait_semaphores += wait_semaphores;
    s.signal_semaphores += signal_semaphores;
    if (wait_semaphores != 0) s.with_wait_semaphore++;
    if (signal_semaphores != 0) s.with_signal_semaphore++;
    if (has_fence) s.with_fence++;
    if (deferred_fence) {
        s.deferred_fence++;
    } else {
        s.non_deferred_fence++;
    }
    if (sync_write) {
        s.sync_write_count++;
    } else {
        s.async_write_count++;
    }
    s.hint_us += hint_us;
    s.encode_us += encode_us;
    s.write_us += write_us;
    s.total_us += total_us;
    s.max_write_us = std::max<uint64_t>(s.max_write_us, write_us);
    s.max_total_us = std::max<uint64_t>(s.max_total_us, total_us);
    s.write_buckets[SubmitRpcStatsBucket(write_us)]++;
    s.total_buckets[SubmitRpcStatsBucket(total_us)]++;

    LlmVkTimingNoteSubmit(submit_count,
                          command_buffers,
                          wait_semaphores,
                          signal_semaphores,
                          has_fence,
                          deferred_fence,
                          hint_us,
                          encode_us,
                          write_us,
                          total_us);

    const uint64_t total_calls = s.submit_calls + s.submit2_calls;
    if (total_calls != 0 && (total_calls % kSubmitRpcStatsLogEvery) == 0) {
        const uint64_t avg_hint = s.hint_us / total_calls;
        const uint64_t avg_encode = s.encode_us / total_calls;
        const uint64_t avg_write = s.write_us / total_calls;
        const uint64_t avg_total = s.total_us / total_calls;
        const uint64_t logical_submits = s.submit_batches;
        const uint64_t avg_total_per_logical =
            logical_submits ? s.total_us / logical_submits : 0;
        const uint64_t avg_hint_per_logical =
            logical_submits ? s.hint_us / logical_submits : 0;
        const uint64_t saved_host_rpcs =
            logical_submits > total_calls ? logical_submits - total_calls : 0;
        const uint64_t reduction_x1000 =
            logical_submits ? (saved_host_rpcs * 1000ull) / logical_submits : 0;
        const uint64_t submit_window_ms =
            (s.first_call_us != 0 && s.last_call_us >= s.first_call_us) ?
                (s.last_call_us - s.first_call_us) / 1000ull : 0;
        ALOGI("[SUBMIT_RPC_SUMMARY] host_rpcs=%llu submit=%llu submit2=%llu logical_submits=%llu "
              "cmd_bufs=%llu wait_sems=%llu signal_sems=%llu with_wait=%llu "
              "with_signal=%llu with_fence=%llu deferred=%llu non_deferred=%llu "
              "sync_writes=%llu async_writes=%llu avg_hint_us=%llu avg_encode_us=%llu "
              "avg_write_us=%llu avg_total_us=%llu p95_write_bucket_us=%llu "
              "p95_total_bucket_us=%llu max_write_us=%llu max_total_us=%llu "
              "avg_hint_us_per_logical=%llu avg_total_us_per_logical=%llu "
              "saved_host_rpcs=%llu reduction_x1000=%llu submit_accounted_ms=%llu "
              "submit_window_ms=%llu",
              (unsigned long long)total_calls,
              (unsigned long long)s.submit_calls,
              (unsigned long long)s.submit2_calls,
              (unsigned long long)logical_submits,
              (unsigned long long)s.command_buffers,
              (unsigned long long)s.wait_semaphores,
              (unsigned long long)s.signal_semaphores,
              (unsigned long long)s.with_wait_semaphore,
              (unsigned long long)s.with_signal_semaphore,
              (unsigned long long)s.with_fence,
              (unsigned long long)s.deferred_fence,
              (unsigned long long)s.non_deferred_fence,
              (unsigned long long)s.sync_write_count,
              (unsigned long long)s.async_write_count,
              (unsigned long long)avg_hint,
              (unsigned long long)avg_encode,
              (unsigned long long)avg_write,
              (unsigned long long)avg_total,
              (unsigned long long)SubmitRpcStatsP95BucketUs(s.write_buckets, total_calls),
              (unsigned long long)SubmitRpcStatsP95BucketUs(s.total_buckets, total_calls),
              (unsigned long long)s.max_write_us,
              (unsigned long long)s.max_total_us,
              (unsigned long long)avg_hint_per_logical,
              (unsigned long long)avg_total_per_logical,
              (unsigned long long)saved_host_rpcs,
              (unsigned long long)reduction_x1000,
              (unsigned long long)(s.total_us / 1000ull),
              (unsigned long long)submit_window_ms);
    }
}

static void NoteImplicitFlushStats(const char* mode,
                                   size_t mapped_count,
                                   uint64_t hint_ranges,
                                   uint64_t hint_bytes,
                                   uint64_t scanned_bytes,
                                   uint64_t compared_chunks,
                                   uint64_t dirty_chunks,
                                   uint64_t dirty_ranges,
                                   uint64_t dirty_bytes,
                                   uint64_t flush_invocations,
                                   uint64_t hint_clean_cache_hits,
                                   uint64_t hint_memcmp_checks,
                                   uint64_t hint_memcmp_bytes,
                                   uint64_t hint_dirty_ranges,
                                   uint64_t scan_us,
                                   uint64_t rpc_us,
                                   uint64_t total_us) {
    std::lock_guard<std::mutex> lock(g_submit_hint_stats_mutex);
    ImplicitFlushStats& s = g_implicit_flush_stats;
    s.calls++;
    if (mode && strcmp(mode, "hint") == 0) {
        s.hint_calls++;
    } else if (mode && strcmp(mode, "skip_metadata") == 0) {
        s.skip_metadata_calls++;
    } else if (mode && strcmp(mode, "skip_clean_scan") == 0) {
        s.clean_scan_skip_calls++;
    } else if (mode && strcmp(mode, "empty") == 0) {
        s.empty_calls++;
    } else {
        s.scan_calls++;
        if (dirty_ranges == 0) {
            s.clean_scan_calls++;
        } else {
            s.dirty_scan_calls++;
        }
    }
    s.flush_invocations += flush_invocations;
    s.mapped_seen += mapped_count;
    s.hint_ranges += hint_ranges;
    s.hint_bytes += hint_bytes;
    s.scanned_bytes += scanned_bytes;
    s.compared_chunks += compared_chunks;
    s.dirty_chunks += dirty_chunks;
    s.dirty_ranges += dirty_ranges;
    s.dirty_bytes += dirty_bytes;
    s.hint_clean_cache_hits += hint_clean_cache_hits;
    s.hint_memcmp_checks += hint_memcmp_checks;
    s.hint_memcmp_bytes += hint_memcmp_bytes;
    s.hint_dirty_ranges += hint_dirty_ranges;
    s.scan_us += scan_us;
    s.rpc_us += rpc_us;
    s.total_us += total_us;
    s.max_scan_us = std::max<uint64_t>(s.max_scan_us, scan_us);
    s.max_rpc_us = std::max<uint64_t>(s.max_rpc_us, rpc_us);
    s.max_total_us = std::max<uint64_t>(s.max_total_us, total_us);
    s.scan_buckets[SubmitRpcStatsBucket(scan_us)]++;
    s.total_buckets[SubmitRpcStatsBucket(total_us)]++;

    GuestSyncBreakdownStats& b = g_guest_sync_breakdown_stats;
    b.implicit_calls++;
    if (mode && strcmp(mode, "hint") == 0) {
        b.implicit_hint_calls++;
    } else if (mode && (strcmp(mode, "skip_metadata") == 0 ||
                        strcmp(mode, "skip_clean_scan") == 0)) {
        b.implicit_skip_calls++;
    } else if (mode && strcmp(mode, "empty") == 0) {
        b.implicit_empty_calls++;
    } else {
        b.implicit_scan_calls++;
    }
    b.implicit_mapped_seen += mapped_count;
    b.implicit_scanned_bytes += scanned_bytes;
    b.implicit_compared_chunks += compared_chunks;
    b.implicit_dirty_chunks += dirty_chunks;
    b.implicit_dirty_ranges += dirty_ranges;
    b.implicit_dirty_bytes += dirty_bytes;
    b.implicit_flush_invocations += flush_invocations;
    b.implicit_scan_us += scan_us;
    b.implicit_rpc_us += rpc_us;
    b.implicit_total_us += total_us;
    MaybeLogGuestSyncBreakdownStatsLocked(mode ? mode : "implicit_flush");

    if (s.calls != 0 && (s.calls % kImplicitFlushStatsLogEvery) == 0) {
        ALOGI("[IMPLICIT_FLUSH_SUMMARY] calls=%llu hint=%llu scan=%llu "
              "skip_metadata=%llu skip_clean_scan=%llu empty=%llu "
              "clean_scan=%llu dirty_scan=%llu "
              "flush_invocations=%llu mapped_seen=%llu hint_ranges=%llu "
              "hint_bytes=%llu scanned_bytes=%llu compared_chunks=%llu "
              "dirty_chunks=%llu dirty_ranges=%llu dirty_bytes=%llu "
              "hint_clean_cache_hits=%llu hint_memcmp_checks=%llu "
              "hint_memcmp_bytes=%llu hint_dirty_ranges=%llu "
              "avg_scan_us=%llu avg_rpc_us=%llu avg_total_us=%llu "
              "p95_scan_bucket_us=%llu p95_total_bucket_us=%llu "
              "max_scan_us=%llu max_rpc_us=%llu max_total_us=%llu",
              (unsigned long long)s.calls,
              (unsigned long long)s.hint_calls,
              (unsigned long long)s.scan_calls,
              (unsigned long long)s.skip_metadata_calls,
              (unsigned long long)s.clean_scan_skip_calls,
              (unsigned long long)s.empty_calls,
              (unsigned long long)s.clean_scan_calls,
              (unsigned long long)s.dirty_scan_calls,
              (unsigned long long)s.flush_invocations,
              (unsigned long long)s.mapped_seen,
              (unsigned long long)s.hint_ranges,
              (unsigned long long)s.hint_bytes,
              (unsigned long long)s.scanned_bytes,
              (unsigned long long)s.compared_chunks,
              (unsigned long long)s.dirty_chunks,
              (unsigned long long)s.dirty_ranges,
              (unsigned long long)s.dirty_bytes,
              (unsigned long long)s.hint_clean_cache_hits,
              (unsigned long long)s.hint_memcmp_checks,
              (unsigned long long)s.hint_memcmp_bytes,
              (unsigned long long)s.hint_dirty_ranges,
              (unsigned long long)(s.scan_us / s.calls),
              (unsigned long long)(s.rpc_us / s.calls),
              (unsigned long long)(s.total_us / s.calls),
              (unsigned long long)SubmitRpcStatsP95BucketUs(s.scan_buckets, s.calls),
              (unsigned long long)SubmitRpcStatsP95BucketUs(s.total_buckets, s.calls),
              (unsigned long long)s.max_scan_us,
              (unsigned long long)s.max_rpc_us,
              (unsigned long long)s.max_total_us);
    }
}

static void NoteMappedFlushStats(uint32_t memory_range_count,
                                 uint64_t bytes,
                                 uint64_t copy_us,
                                 uint64_t encode_us,
                                 uint64_t addptr_us,
                                 uint64_t rpc_us,
                                 uint64_t record_us,
                                 uint64_t shadow_us,
                                 uint64_t total_us) {
    std::lock_guard<std::mutex> lock(g_submit_hint_stats_mutex);
    MappedFlushStats& s = g_mapped_flush_stats;
    s.calls++;
    s.ranges += memory_range_count;
    s.bytes += bytes;
    s.copy_us += copy_us;
    s.encode_us += encode_us;
    s.addptr_us += addptr_us;
    s.rpc_us += rpc_us;
    s.record_us += record_us;
    s.shadow_us += shadow_us;
    s.total_us += total_us;
    s.max_total_us = std::max<uint64_t>(s.max_total_us, total_us);
    s.total_buckets[SubmitRpcStatsBucket(total_us)]++;

    if (kEnableLlmVkTimingLog) {
        g_llm_vk_timing.flush_calls.fetch_add(1, std::memory_order_relaxed);
        g_llm_vk_timing.flush_ranges.fetch_add(memory_range_count, std::memory_order_relaxed);
        g_llm_vk_timing.flush_bytes.fetch_add(bytes, std::memory_order_relaxed);
        g_llm_vk_timing.flush_rpc_us.fetch_add(rpc_us, std::memory_order_relaxed);
        g_llm_vk_timing.flush_shadow_us.fetch_add(shadow_us, std::memory_order_relaxed);
        g_llm_vk_timing.flush_total_us.fetch_add(total_us, std::memory_order_relaxed);
        LlmVkTimingAtomicMax(g_llm_vk_timing.flush_max_total_us, total_us);
        LlmVkTimingMaybeLog("mapped_flush");
    }

    GuestSyncBreakdownStats& b = g_guest_sync_breakdown_stats;
    b.mapped_flush_calls++;
    b.mapped_flush_ranges += memory_range_count;
    b.mapped_flush_bytes += bytes;
    b.mapped_flush_copy_us += copy_us;
    b.mapped_flush_encode_us += encode_us;
    b.mapped_flush_addptr_us += addptr_us;
    b.mapped_flush_rpc_us += rpc_us;
    b.mapped_flush_record_us += record_us;
    b.mapped_flush_shadow_us += shadow_us;
    b.mapped_flush_total_us += total_us;
    MaybeLogGuestSyncBreakdownStatsLocked("mapped_flush");

    if (kEnableLocalPerfLog &&
        s.calls != 0 && (s.calls % kMappedFlushStatsLogEvery) == 0) {
        ALOGI("[PERF_FLUSH_SUMMARY] calls=%llu ranges=%llu bytes=%llu "
              "avg_copy_us=%llu avg_encode_us=%llu avg_addptr_us=%llu "
              "avg_rpc_us=%llu avg_record_us=%llu avg_shadow_us=%llu "
              "avg_total_us=%llu p95_total_bucket_us=%llu max_total_us=%llu",
              (unsigned long long)s.calls,
              (unsigned long long)s.ranges,
              (unsigned long long)s.bytes,
              (unsigned long long)(s.copy_us / s.calls),
              (unsigned long long)(s.encode_us / s.calls),
              (unsigned long long)(s.addptr_us / s.calls),
              (unsigned long long)(s.rpc_us / s.calls),
              (unsigned long long)(s.record_us / s.calls),
              (unsigned long long)(s.shadow_us / s.calls),
              (unsigned long long)(s.total_us / s.calls),
              (unsigned long long)SubmitRpcStatsP95BucketUs(s.total_buckets, s.calls),
              (unsigned long long)s.max_total_us);
    }
}

static void NoteMappedInvalidateStats(uint32_t memory_range_count,
                                      uint64_t bytes,
                                      uint32_t skipped_synced,
                                      uint64_t copy_us,
                                      uint64_t param_us,
                                      uint64_t addptr_us,
                                      uint64_t rpc_us,
                                      uint64_t update_us,
                                      uint64_t total_us) {
    std::lock_guard<std::mutex> lock(g_submit_hint_stats_mutex);
    GuestSyncBreakdownStats& b = g_guest_sync_breakdown_stats;
    b.mapped_invalidate_calls++;
    b.mapped_invalidate_ranges += memory_range_count;
    b.mapped_invalidate_bytes += bytes;
    b.mapped_invalidate_skipped_synced += skipped_synced;
    b.mapped_invalidate_copy_us += copy_us;
    b.mapped_invalidate_param_us += param_us;
    b.mapped_invalidate_addptr_us += addptr_us;
    b.mapped_invalidate_rpc_us += rpc_us;
    b.mapped_invalidate_update_us += update_us;
    b.mapped_invalidate_total_us += total_us;
    if (kEnableLlmVkTimingLog) {
        g_llm_vk_timing.invalidate_calls.fetch_add(1, std::memory_order_relaxed);
        g_llm_vk_timing.invalidate_ranges.fetch_add(memory_range_count, std::memory_order_relaxed);
        g_llm_vk_timing.invalidate_bytes.fetch_add(bytes, std::memory_order_relaxed);
        g_llm_vk_timing.invalidate_skipped_synced.fetch_add(skipped_synced, std::memory_order_relaxed);
        g_llm_vk_timing.invalidate_rpc_us.fetch_add(rpc_us, std::memory_order_relaxed);
        g_llm_vk_timing.invalidate_update_us.fetch_add(update_us, std::memory_order_relaxed);
        g_llm_vk_timing.invalidate_total_us.fetch_add(total_us, std::memory_order_relaxed);
        LlmVkTimingAtomicMax(g_llm_vk_timing.invalidate_max_total_us, total_us);
        LlmVkTimingMaybeLog("mapped_invalidate");
    }
    MaybeLogGuestSyncBreakdownStatsLocked("mapped_invalidate");
}

static void NoteConservativeReadbackStats(uint64_t ranges,
                                          uint64_t bytes,
                                          uint64_t skipped_synced,
                                          uint64_t preserve_bytes,
                                          uint64_t preserve_us,
                                          uint64_t invalidate_us,
                                          uint64_t total_us,
                                          const char* reason) {
    std::lock_guard<std::mutex> lock(g_submit_hint_stats_mutex);
    GuestSyncBreakdownStats& b = g_guest_sync_breakdown_stats;
    b.conservative_readback_calls++;
    b.conservative_readback_ranges += ranges;
    b.conservative_readback_bytes += bytes;
    b.conservative_readback_skipped_synced += skipped_synced;
    b.conservative_preserve_bytes += preserve_bytes;
    b.conservative_preserve_us += preserve_us;
    b.conservative_invalidate_us += invalidate_us;
    b.conservative_total_us += total_us;
    if (kEnableLlmVkTimingLog) {
        g_llm_vk_timing.conservative_calls.fetch_add(1, std::memory_order_relaxed);
        g_llm_vk_timing.conservative_ranges.fetch_add(ranges, std::memory_order_relaxed);
        g_llm_vk_timing.conservative_bytes.fetch_add(bytes, std::memory_order_relaxed);
        g_llm_vk_timing.conservative_skipped_synced.fetch_add(skipped_synced, std::memory_order_relaxed);
        g_llm_vk_timing.conservative_preserve_bytes.fetch_add(preserve_bytes, std::memory_order_relaxed);
        g_llm_vk_timing.conservative_preserve_us.fetch_add(preserve_us, std::memory_order_relaxed);
        g_llm_vk_timing.conservative_invalidate_us.fetch_add(invalidate_us, std::memory_order_relaxed);
        g_llm_vk_timing.conservative_total_us.fetch_add(total_us, std::memory_order_relaxed);
        LlmVkTimingAtomicMax(g_llm_vk_timing.conservative_max_total_us, total_us);
        LlmVkTimingMaybeLog(reason ? reason : "conservative_readback");
    }
    MaybeLogGuestSyncBreakdownStatsLocked(reason ? reason : "conservative_readback");
}

static void AppendTrackedRange(std::vector<TrackedMemoryRange>* ranges,
                               const TrackedMemoryRange& range) {
    if (!ranges || range.memory == VK_NULL_HANDLE || range.size == 0) return;

    for (TrackedMemoryRange& existing : *ranges) {
        if (existing.device != range.device || existing.memory != range.memory) continue;

        const VkDeviceSize existing_end = existing.offset + existing.size;
        const VkDeviceSize range_end = range.offset + range.size;
        const bool overlaps = !(range_end < existing.offset || existing_end < range.offset);
        const bool adjacent = (existing_end == range.offset) || (range_end == existing.offset);
        if (!overlaps && !adjacent) continue;

        const VkDeviceSize merged_begin = std::min(existing.offset, range.offset);
        const VkDeviceSize merged_end = std::max(existing_end, range_end);
        existing.offset = merged_begin;
        existing.size = merged_end - merged_begin;
        return;
    }

    ranges->push_back(range);
}

static bool IsExpressVkRegisteredMemoryHandle(VkDeviceMemory memory) {
    VkDeviceMemory_T* mem = (VkDeviceMemory_T*)memory;
    return mem && mem->express_vk_mem_registered;
}

namespace null_driver {
static void FlushPendingSubmitCohort(const char* reason);
static void FlushPendingSubmitCohortForQueue(VkQueue queue, const char* reason);
static void ImplicitInvalidateAllMappedMemories();
static uint64_t InvalidateAllMappedMemoriesForReadback(
    const char* reason,
    uint64_t* out_bytes,
    uint64_t* out_skipped_synced = nullptr);
}

struct DeferredWaitStats {
    uint64_t virtual_waits = 0;
    uint64_t virtual_fences = 0;
    uint64_t virtual_readback_waits = 0;
    uint64_t real_waits = 0;
    uint64_t real_wait_us = 0;
    uint64_t drain_calls = 0;
    uint64_t drain_empty_calls = 0;
    uint64_t drained_queues = 0;
    uint64_t drain_us = 0;
    uint64_t drain_invalidate_us = 0;
    uint64_t lifecycle_drains = 0;
    uint64_t readback_drains = 0;
    uint64_t idle_drains = 0;
    uint64_t other_drains = 0;
    uint64_t lifecycle_us = 0;
    uint64_t readback_us = 0;
    uint64_t idle_us = 0;
    uint64_t other_us = 0;
    uint64_t lifecycle_queues = 0;
    uint64_t readback_queues = 0;
    uint64_t idle_queues = 0;
    uint64_t other_queues = 0;
    uint64_t max_pending_queues = 0;
    uint64_t deferred_readback_drains = 0;
    uint64_t deferred_readback_queues = 0;
    uint64_t deferred_readback_ranges = 0;
    uint64_t deferred_readback_bytes = 0;
    uint64_t deferred_readback_wait_us = 0;
    uint64_t deferred_readback_invalidate_us = 0;
    uint64_t deferred_readback_total_us = 0;
    uint64_t deferred_readback_fallbacks = 0;
};

static std::mutex g_deferred_wait_stats_mutex;
static DeferredWaitStats g_deferred_wait_stats;

static const char* DeferredDrainClass(const char* reason) {
    if (!reason) return "other";
    if (strcmp(reason, "map_memory") == 0 || strstr(reason, "invalidate") ||
        strstr(reason, "readback")) {
        return "readback";
    }
    if (strstr(reason, "destroy") || strstr(reason, "free") ||
        strstr(reason, "reset") || strstr(reason, "unmap")) {
        return "lifecycle";
    }
    if (strstr(reason, "idle") || strstr(reason, "wait_idle")) return "idle";
    return "other";
}

static bool DeferredDrainNeedsImplicitInvalidate(const char* reason) {
    // Resetting a command buffer is a command-buffer lifetime boundary, not a
    // CPU readback boundary. Keep the queue wait for lifetime safety, but leave
    // host-visible memory sync to explicit readback/map/invalidate/wait paths.
    if (reason && strcmp(reason, "reset_command_buffer") == 0) {
        return false;
    }
    return true;
}

static void MaybeLogDeferredWaitStatsLocked(const char* reason,
                                            const char* event,
                                            uint64_t pending_queues,
                                            bool force) {
    const uint64_t events =
        g_deferred_wait_stats.virtual_waits + g_deferred_wait_stats.real_waits +
        g_deferred_wait_stats.drain_calls;
    if (!force && (events == 0 || (events % kDeferredWaitStatsLogEvery) != 0)) {
        return;
    }

    ALOGV("[DEFER_WAIT_SUMMARY] event=%s reason=%s virtual=%llu virtual_readback=%llu fences=%llu real=%llu real_us=%llu drains=%llu empty=%llu drained_queues=%llu drain_us=%llu invalidate_us=%llu lifecycle=%llu/%lluus/q%llu readback=%llu/%lluus/q%llu idle=%llu/%lluus/q%llu other=%llu/%lluus/q%llu pending=%llu max_pending=%llu deferred_readback=%llu/q%llu/r%llu/mb%llu wait_us=%llu inv_us=%llu total_us=%llu fallback=%llu",
          event ? event : "summary",
          reason ? reason : "unknown",
          (unsigned long long)g_deferred_wait_stats.virtual_waits,
          (unsigned long long)g_deferred_wait_stats.virtual_readback_waits,
          (unsigned long long)g_deferred_wait_stats.virtual_fences,
          (unsigned long long)g_deferred_wait_stats.real_waits,
          (unsigned long long)g_deferred_wait_stats.real_wait_us,
          (unsigned long long)g_deferred_wait_stats.drain_calls,
          (unsigned long long)g_deferred_wait_stats.drain_empty_calls,
          (unsigned long long)g_deferred_wait_stats.drained_queues,
          (unsigned long long)g_deferred_wait_stats.drain_us,
          (unsigned long long)g_deferred_wait_stats.drain_invalidate_us,
          (unsigned long long)g_deferred_wait_stats.lifecycle_drains,
          (unsigned long long)g_deferred_wait_stats.lifecycle_us,
          (unsigned long long)g_deferred_wait_stats.lifecycle_queues,
          (unsigned long long)g_deferred_wait_stats.readback_drains,
          (unsigned long long)g_deferred_wait_stats.readback_us,
          (unsigned long long)g_deferred_wait_stats.readback_queues,
          (unsigned long long)g_deferred_wait_stats.idle_drains,
          (unsigned long long)g_deferred_wait_stats.idle_us,
          (unsigned long long)g_deferred_wait_stats.idle_queues,
          (unsigned long long)g_deferred_wait_stats.other_drains,
          (unsigned long long)g_deferred_wait_stats.other_us,
          (unsigned long long)g_deferred_wait_stats.other_queues,
          (unsigned long long)pending_queues,
          (unsigned long long)g_deferred_wait_stats.max_pending_queues,
          (unsigned long long)g_deferred_wait_stats.deferred_readback_drains,
          (unsigned long long)g_deferred_wait_stats.deferred_readback_queues,
          (unsigned long long)g_deferred_wait_stats.deferred_readback_ranges,
          (unsigned long long)(g_deferred_wait_stats.deferred_readback_bytes / (1024ull * 1024ull)),
          (unsigned long long)g_deferred_wait_stats.deferred_readback_wait_us,
          (unsigned long long)g_deferred_wait_stats.deferred_readback_invalidate_us,
          (unsigned long long)g_deferred_wait_stats.deferred_readback_total_us,
          (unsigned long long)g_deferred_wait_stats.deferred_readback_fallbacks);
}

static void NoteDeferredVirtualWaitStats(uint32_t fence_count,
                                         uint64_t pending_queues,
                                         bool has_readback) {
    std::lock_guard<std::mutex> lock(g_deferred_wait_stats_mutex);
    g_deferred_wait_stats.virtual_waits++;
    if (has_readback) {
        g_deferred_wait_stats.virtual_readback_waits++;
    }
    g_deferred_wait_stats.virtual_fences += fence_count;
    g_deferred_wait_stats.max_pending_queues =
        std::max(g_deferred_wait_stats.max_pending_queues, pending_queues);
    MaybeLogDeferredWaitStatsLocked("wait_fences", "virtual", pending_queues, false);
}

static void NoteDeferredRealWaitStats(uint64_t wait_us,
                                      uint64_t pending_queues) {
    std::lock_guard<std::mutex> lock(g_deferred_wait_stats_mutex);
    g_deferred_wait_stats.real_waits++;
    g_deferred_wait_stats.real_wait_us += wait_us;
    g_deferred_wait_stats.max_pending_queues =
        std::max(g_deferred_wait_stats.max_pending_queues, pending_queues);
    MaybeLogDeferredWaitStatsLocked("wait_fences", "real", pending_queues, false);
}

static void NoteDeferredDrainStats(const char* reason,
                                   uint64_t drained_queues,
                                   uint64_t drain_us,
                                   uint64_t invalidate_us,
                                   uint64_t pending_after) {
    std::lock_guard<std::mutex> lock(g_deferred_wait_stats_mutex);
    g_deferred_wait_stats.drain_calls++;
    if (drained_queues == 0) {
        g_deferred_wait_stats.drain_empty_calls++;
    }
    g_deferred_wait_stats.drained_queues += drained_queues;
    g_deferred_wait_stats.drain_us += drain_us;
    g_deferred_wait_stats.drain_invalidate_us += invalidate_us;
    g_deferred_wait_stats.max_pending_queues =
        std::max(g_deferred_wait_stats.max_pending_queues, pending_after);

    const char* cls = DeferredDrainClass(reason);
    if (strcmp(cls, "lifecycle") == 0) {
        g_deferred_wait_stats.lifecycle_drains++;
        g_deferred_wait_stats.lifecycle_us += drain_us;
        g_deferred_wait_stats.lifecycle_queues += drained_queues;
    } else if (strcmp(cls, "readback") == 0) {
        g_deferred_wait_stats.readback_drains++;
        g_deferred_wait_stats.readback_us += drain_us;
        g_deferred_wait_stats.readback_queues += drained_queues;
    } else if (strcmp(cls, "idle") == 0) {
        g_deferred_wait_stats.idle_drains++;
        g_deferred_wait_stats.idle_us += drain_us;
        g_deferred_wait_stats.idle_queues += drained_queues;
    } else {
        g_deferred_wait_stats.other_drains++;
        g_deferred_wait_stats.other_us += drain_us;
        g_deferred_wait_stats.other_queues += drained_queues;
    }

    MaybeLogDeferredWaitStatsLocked(reason,
                                    drained_queues ? "drain" : "drain_empty",
                                    pending_after,
                                    drained_queues != 0);
}

static void NoteDeferredReadbackDrainStats(const char* reason,
                                           uint64_t queues,
                                           uint64_t ranges,
                                           uint64_t bytes,
                                           uint64_t wait_us,
                                           uint64_t invalidate_us,
                                           uint64_t total_us,
                                           bool fallback) {
    std::lock_guard<std::mutex> lock(g_deferred_wait_stats_mutex);
    g_deferred_wait_stats.deferred_readback_drains++;
    g_deferred_wait_stats.deferred_readback_queues += queues;
    g_deferred_wait_stats.deferred_readback_ranges += ranges;
    g_deferred_wait_stats.deferred_readback_bytes += bytes;
    g_deferred_wait_stats.deferred_readback_wait_us += wait_us;
    g_deferred_wait_stats.deferred_readback_invalidate_us += invalidate_us;
    g_deferred_wait_stats.deferred_readback_total_us += total_us;
    if (fallback) {
        g_deferred_wait_stats.deferred_readback_fallbacks++;
    }
    MaybeLogDeferredWaitStatsLocked(reason,
                                    fallback ? "deferred_readback_fallback" : "deferred_readback",
                                    0,
                                    true);
}

static VkResult RawQueueWaitIdleForDeferred(VkQueue queue, const char* reason) {
    if (queue == VK_NULL_HANDLE) return VK_SUCCESS;
    FlushPendingSubmitCohortForQueue(queue, reason ? reason : "deferred_wait");

    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    uint64_t guest_queue = (uint64_t)(uintptr_t)queue;

    mgr.addParam64(guest_queue);

    VkResult vkResult = VK_SUCCESS;
    mgr.addPtr(&vkResult, sizeof(VkResult));
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkQueueWaitIdle, true);

    {
        std::lock_guard<std::mutex> lock(g_mapped_mutex);
        if (vkResult == VK_SUCCESS) {
            g_deferred_wait_queues.erase(queue);
            for (auto it = g_deferred_fence_queues.begin();
                 it != g_deferred_fence_queues.end();) {
                if (it->second == queue) {
                    g_completed_deferred_fences.insert(it->first);
                    it = g_deferred_fence_queues.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }

    ALOGD("[SYNC_GUEST] deferred_queue_wait reason=%s queue=%llx result=%d",
          reason ? reason : "unknown",
          (unsigned long long)(uintptr_t)queue,
          (int)vkResult);
    return vkResult;
}

static void DrainDeferredQueues(const char* reason) {
    std::vector<VkQueue> queues;
    {
        std::lock_guard<std::mutex> lock(g_mapped_mutex);
        queues.assign(g_deferred_wait_queues.begin(), g_deferred_wait_queues.end());
    }

    const uint64_t start_us = ExpressVkNowUs();
    for (VkQueue queue : queues) {
        RawQueueWaitIdleForDeferred(queue, reason);
    }
    const uint64_t wait_us = ExpressVkNowUs() - start_us;

    uint64_t invalidate_us = 0;
    if (!queues.empty() &&
        kEnableImplicitGlobalMappedSync &&
        DeferredDrainNeedsImplicitInvalidate(reason)) {
        const uint64_t invalidate_start_us = ExpressVkNowUs();
        ImplicitInvalidateAllMappedMemories();
        invalidate_us = ExpressVkNowUs() - invalidate_start_us;
    }

    size_t pending_after = 0;
    {
        std::lock_guard<std::mutex> lock(g_mapped_mutex);
        pending_after = g_deferred_wait_queues.size();
    }
    if (wait_us + invalidate_us >= kSlowWaitDiagLogUs) {
        ALOGI("[WAIT_SLOW_DRAIN] reason=%s queues=%zu wait_us=%llu invalidate_us=%llu total_us=%llu pending_after=%zu",
              reason ? reason : "unknown",
              queues.size(),
              (unsigned long long)wait_us,
              (unsigned long long)invalidate_us,
              (unsigned long long)(wait_us + invalidate_us),
              pending_after);
    }
    NoteDeferredDrainStats(reason,
                           (uint64_t)queues.size(),
                           wait_us + invalidate_us,
                           invalidate_us,
                           (uint64_t)pending_after);
}

static void TrackDeferredFenceWait(VkFence fence, VkQueue queue) {
    if (!kEnableDeferredFenceWait ||
        fence == VK_NULL_HANDLE ||
        queue == VK_NULL_HANDLE) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_mapped_mutex);
    g_completed_deferred_fences.erase(fence);
    g_deferred_fence_queues[fence] = queue;
    g_deferred_wait_queues.insert(queue);
}

static bool IsFenceWaitDeferredLocked(VkFence fence) {
    return fence != VK_NULL_HANDLE &&
           (g_deferred_fence_queues.find(fence) != g_deferred_fence_queues.end() ||
            g_completed_deferred_fences.find(fence) != g_completed_deferred_fences.end());
}

static void PruneDeferredQueueLocked(VkQueue queue) {
    if (queue == VK_NULL_HANDLE) return;
    for (const auto& item : g_deferred_fence_queues) {
        if (item.second == queue) return;
    }
    g_deferred_wait_queues.erase(queue);
}

static void ForgetDeferredFence(VkFence fence) {
    if (fence == VK_NULL_HANDLE) return;
    std::lock_guard<std::mutex> lock(g_mapped_mutex);
    auto it = g_deferred_fence_queues.find(fence);
    g_completed_deferred_fences.erase(fence);
    if (it == g_deferred_fence_queues.end()) return;
    VkQueue queue = it->second;
    g_deferred_fence_queues.erase(it);
    PruneDeferredQueueLocked(queue);
}

static void ForgetDeferredFenceKeepQueue(VkFence fence) {
    if (fence == VK_NULL_HANDLE) return;
    std::lock_guard<std::mutex> lock(g_mapped_mutex);
    g_deferred_fence_queues.erase(fence);
    g_completed_deferred_fences.erase(fence);
}

static std::vector<uint8_t> AcquireShadowBufferLocked(size_t size,
                                                      bool* reused,
                                                      size_t* reused_capacity) {
    if (reused) *reused = false;
    if (reused_capacity) *reused_capacity = 0;

    size_t best_index = g_shadow_buffer_pool.size();
    size_t best_capacity = SIZE_MAX;
    for (size_t i = 0; i < g_shadow_buffer_pool.size(); ++i) {
        const size_t capacity = g_shadow_buffer_pool[i].capacity();
        if (capacity >= size && capacity < best_capacity) {
            best_index = i;
            best_capacity = capacity;
        }
    }

    std::vector<uint8_t> shadow;
    if (best_index != g_shadow_buffer_pool.size()) {
        shadow = std::move(g_shadow_buffer_pool[best_index]);
        g_shadow_buffer_pool_bytes -= shadow.capacity();
        g_shadow_buffer_pool.erase(g_shadow_buffer_pool.begin() + best_index);
        if (reused) *reused = true;
        if (reused_capacity) *reused_capacity = shadow.capacity();
    }
    if (shadow.capacity() < size) {
        shadow.reserve(size);
    }
    return shadow;
}

static void ReleaseShadowBufferLocked(std::vector<uint8_t>&& shadow,
                                      bool* pooled,
                                      size_t* released_capacity) {
    const size_t capacity = shadow.capacity();
    if (pooled) *pooled = false;
    if (released_capacity) *released_capacity = capacity;

    if (capacity == 0) return;
    if (g_shadow_buffer_pool.size() >= kMaxShadowBufferPoolEntries) return;
    if (g_shadow_buffer_pool_bytes + capacity > kMaxShadowBufferPoolBytes) return;

    g_shadow_buffer_pool_bytes += capacity;
    g_shadow_buffer_pool.push_back(std::move(shadow));
    if (pooled) *pooled = true;
}

static void EraseTrackedRangesForMemoryLocked(VkDeviceMemory memory) {
    auto erase_memory = [memory](std::vector<TrackedMemoryRange>* ranges) {
        ranges->erase(
            std::remove_if(ranges->begin(), ranges->end(),
                           [memory](const TrackedMemoryRange& range) {
                               return range.memory == memory;
                           }),
            ranges->end());
    };
    erase_memory(&g_flush_hint_ranges);
    erase_memory(&g_invalidate_hint_ranges);
    erase_memory(&g_pending_upload_wait_ranges);
}

static VkDeviceSize ClampMappedRangeSize(VkDeviceSize mapped_size,
                                         VkDeviceSize offset,
                                         VkDeviceSize size) {
    if (offset >= mapped_size) return 0;
    const VkDeviceSize available = mapped_size - offset;
    if (size == VK_WHOLE_SIZE || size > available) return available;
    return size;
}

static void AppendMemoryRangeSpan(std::vector<MemoryRangeSpan>* ranges,
                                  VkDeviceSize offset,
                                  VkDeviceSize size,
                                  uint64_t generation) {
    if (!ranges || size == 0) return;

    VkDeviceSize begin = offset;
    VkDeviceSize end = offset + size;
    uint64_t max_gen = generation;

    std::vector<MemoryRangeSpan> new_ranges;
    for (const MemoryRangeSpan& existing : *ranges) {
        const VkDeviceSize existing_end = existing.offset + existing.size;
        const bool overlaps = !(end < existing.offset || existing_end < begin);
        const bool adjacent = (existing_end == begin) || (end == existing.offset);
        if (!overlaps && !adjacent) {
            new_ranges.push_back(existing);
        } else {
            begin = std::min(begin, existing.offset);
            end = std::max(end, existing_end);
            max_gen = std::max(max_gen, existing.generation);
        }
    }
    new_ranges.push_back({begin, end - begin, max_gen});
    *ranges = std::move(new_ranges);
}

static void EraseMemoryRangeSpanOverlaps(std::vector<MemoryRangeSpan>* ranges,
                                         VkDeviceSize offset,
                                         VkDeviceSize size) {
    if (!ranges || size == 0) return;
    const VkDeviceSize begin = offset;
    VkDeviceSize end = offset + size;
    if (end < begin) end = UINT64_MAX;

    std::vector<MemoryRangeSpan> kept;
    kept.reserve(ranges->size());
    for (const MemoryRangeSpan& existing : *ranges) {
        VkDeviceSize existing_end = existing.offset + existing.size;
        if (existing_end < existing.offset) existing_end = UINT64_MAX;
        if (end <= existing.offset || existing_end <= begin) {
            kept.push_back(existing);
            continue;
        }
        if (existing.offset < begin) {
            kept.push_back({existing.offset, begin - existing.offset, existing.generation});
        }
        if (existing_end > end && end != UINT64_MAX) {
            kept.push_back({end, existing_end - end, existing.generation});
        }
    }
    *ranges = std::move(kept);
}

static bool RangeCoveredByGeneration(const std::vector<MemoryRangeSpan>& ranges,
                                     VkDeviceSize offset,
                                     VkDeviceSize size,
                                     uint64_t min_generation) {
    if (size == 0) return true;
    for (const MemoryRangeSpan& range : ranges) {
        const VkDeviceSize range_end = range.offset + range.size;
        const VkDeviceSize want_end = offset + size;
        if (range.generation >= min_generation &&
            range.offset <= offset &&
            range_end >= want_end) {
            return true;
        }
    }
    return false;
}

static void CollectUncoveredRangeSpansByGeneration(
    const std::vector<MemoryRangeSpan>& ranges,
    VkDeviceSize offset,
    VkDeviceSize size,
    uint64_t min_generation,
    std::vector<std::pair<VkDeviceSize, VkDeviceSize>>* holes) {
    if (!holes) return;
    holes->clear();
    if (size == 0) return;

    const VkDeviceSize want_begin = offset;
    const VkDeviceSize want_end =
        (offset > UINT64_MAX - size) ? UINT64_MAX : offset + size;
    std::vector<std::pair<VkDeviceSize, VkDeviceSize>> covered;
    covered.reserve(ranges.size());

    for (const MemoryRangeSpan& range : ranges) {
        if (range.generation < min_generation || range.size == 0) continue;
        VkDeviceSize range_end =
            (range.offset > UINT64_MAX - range.size) ? UINT64_MAX : range.offset + range.size;
        if (range_end <= want_begin || range.offset >= want_end) continue;
        covered.push_back({
            std::max(range.offset, want_begin),
            std::min(range_end, want_end),
        });
    }

    if (covered.empty()) {
        holes->push_back({want_begin, want_end - want_begin});
        return;
    }

    std::sort(covered.begin(), covered.end());
    VkDeviceSize cursor = want_begin;
    for (const auto& span : covered) {
        if (span.second <= cursor) continue;
        if (span.first > cursor) {
            holes->push_back({cursor, span.first - cursor});
        }
        cursor = std::max(cursor, span.second);
        if (cursor >= want_end) break;
    }
    if (cursor < want_end) {
        holes->push_back({cursor, want_end - cursor});
    }
}

static bool NormalizeMappedRangeLocked(const VkMappedMemoryRange& in,
                                       TrackedMemoryRange* out) {
    if (!out || in.memory == VK_NULL_HANDLE) return false;
    auto it = g_active_mapped_memories.find(in.memory);
    if (it == g_active_mapped_memories.end()) return false;

    const ActiveMappedMemoryRecord& rec = it->second;
    const VkDeviceSize size = ClampMappedRangeSize(rec.size, in.offset, in.size);
    if (size == 0) return false;

    out->device = rec.device;
    out->memory = rec.memory;
    out->offset = in.offset;
    out->size = size;
    return true;
}

static void AppendMappedRangeForInvalidateLocked(
    std::unordered_map<VkDevice, std::vector<VkMappedMemoryRange>>* ranges_by_dev,
    const ActiveMappedMemoryRecord& rec,
    VkDeviceSize offset,
    VkDeviceSize size,
    uint64_t* total_bytes,
    size_t* skipped_already_synced) {
    if (!ranges_by_dev || !rec.map_data || rec.size == 0) return;
    const VkDeviceSize clamped_size = ClampMappedRangeSize(rec.size, offset, size);
    if (clamped_size == 0) return;

    if (RangeCoveredByGeneration(rec.recently_invalidated_ranges,
                                 offset,
                                 clamped_size,
                                 rec.last_submit_generation)) {
        if (skipped_already_synced) (*skipped_already_synced)++;
        return;
    }

    VkMappedMemoryRange range = {};
    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.pNext = nullptr;
    range.memory = rec.memory;
    range.offset = offset;
    range.size = clamped_size;
    (*ranges_by_dev)[rec.device].push_back(range);
    if (total_bytes) *total_bytes += clamped_size;
}

static bool IsMappedRangeRecentlyInvalidatedLocked(const VkMappedMemoryRange& range) {
    TrackedMemoryRange normalized = {};
    if (!NormalizeMappedRangeLocked(range, &normalized)) return false;
    auto it = g_active_mapped_memories.find(normalized.memory);
    if (it == g_active_mapped_memories.end()) return false;
    const ActiveMappedMemoryRecord& rec = it->second;
    return RangeCoveredByGeneration(rec.recently_invalidated_ranges,
                                    normalized.offset,
                                    normalized.size,
                                    rec.last_submit_generation);
}

static void RecordMappedRangeSpansLocked(uint32_t memoryRangeCount,
                                         const VkMappedMemoryRange* pMemoryRanges,
                                         bool flushed,
                                         bool invalidated) {
    if (!pMemoryRanges || memoryRangeCount == 0) return;
    for (uint32_t i = 0; i < memoryRangeCount; ++i) {
        TrackedMemoryRange normalized = {};
        if (!NormalizeMappedRangeLocked(pMemoryRanges[i], &normalized)) continue;
        auto it = g_active_mapped_memories.find(normalized.memory);
        if (it == g_active_mapped_memories.end()) continue;
        ActiveMappedMemoryRecord& rec = it->second;
        if (flushed) {
            AppendMemoryRangeSpan(&rec.recently_flushed_ranges,
                                  normalized.offset,
                                  normalized.size,
                                  g_submit_generation);
            if (IsExpressVkRegisteredMemoryHandle(normalized.memory) &&
                normalized.size >= kHostAsyncUploadThresholdBytes) {
                AppendTrackedRange(&g_pending_upload_wait_ranges, normalized);
            }
        }
        if (invalidated) {
            AppendMemoryRangeSpan(&rec.recently_invalidated_ranges,
                                  normalized.offset,
                                  normalized.size,
                                  g_submit_generation);
        }
    }
}

static void TrackBufferMemoryBinding(VkDevice device,
                                     VkBuffer buffer,
                                     VkDeviceMemory memory,
                                     VkDeviceSize memoryOffset) {
    if (buffer == VK_NULL_HANDLE || memory == VK_NULL_HANDLE) return;
    std::lock_guard<std::mutex> lock(g_mapped_mutex);
    VkDeviceSize buffer_size = 0;
    auto size_it = g_buffer_sizes.find(buffer);
    if (size_it != g_buffer_sizes.end()) {
        buffer_size = size_it->second;
    }
    g_buffer_memory_bindings[buffer] = {device, memory, memoryOffset, buffer_size};
    MemShapeRecordBindingLocked(device, buffer, memory, memoryOffset, buffer_size);
    if (buffer_size >= kGuestMemTraceLargeBytes || memoryOffset != 0) {
        VkDeviceMemory_T* mem = (VkDeviceMemory_T*)memory;
        GUEST_MEM_TRACE("[GUEST_MEM_TRACE] bind_buffer device=0x%llx buffer=0x%llx memory=0x%llx memory_offset=0x%llx buffer_size=%llu buffer_mb=%llu alloc_size=%llu registered=%d",
                        (unsigned long long)(uintptr_t)device,
                        (unsigned long long)(uintptr_t)buffer,
                        (unsigned long long)(uintptr_t)memory,
                        (unsigned long long)memoryOffset,
                        (unsigned long long)buffer_size,
                        (unsigned long long)((uint64_t)buffer_size / (1024ull * 1024ull)),
                        (unsigned long long)(mem ? mem->length : 0),
                        (mem && mem->express_vk_mem_registered) ? 1 : 0);
    }
    InvalidateAllSubmitHintCachesLocked("buffer_binding");
    InvalidateAllDescriptorSetHintCachesLocked("buffer_binding");
}

static void ForgetTrackedBuffer(VkBuffer buffer) {
    if (buffer == VK_NULL_HANDLE) return;
    std::lock_guard<std::mutex> lock(g_mapped_mutex);
    MemShapeForgetBufferLocked(buffer);
    g_buffer_memory_bindings.erase(buffer);
    g_buffer_sizes.erase(buffer);
    InvalidateAllSubmitHintCachesLocked("forget_buffer");
    InvalidateAllDescriptorSetHintCachesLocked("forget_buffer");
}

static void ForgetTrackedCommandBuffer(VkCommandBuffer commandBuffer) {
    if (commandBuffer == VK_NULL_HANDLE) return;
    std::lock_guard<std::mutex> lock(g_mapped_mutex);
    g_command_buffer_descriptor_sets.erase(commandBuffer);
    g_command_buffer_descriptor_dynamic_offsets.erase(commandBuffer);
    g_command_buffer_unknown_dynamic_offsets.erase(commandBuffer);
    g_command_buffer_flush_buffer_ranges.erase(commandBuffer);
    g_command_buffer_invalidate_buffer_ranges.erase(commandBuffer);
    InvalidateCommandBufferSubmitHintCacheLocked(commandBuffer, "forget_command_buffer");
}

static void ForgetTrackedDescriptorSets(uint32_t count,
                                        const VkDescriptorSet* pDescriptorSets) {
    if (!pDescriptorSets || count == 0) return;
    std::lock_guard<std::mutex> lock(g_mapped_mutex);
    for (uint32_t i = 0; i < count; ++i) {
        VkDescriptorSet set = pDescriptorSets[i];
        auto pool_it = g_descriptor_set_to_pool.find(set);
        if (pool_it != g_descriptor_set_to_pool.end()) {
            auto sets_it = g_descriptor_pool_sets.find(pool_it->second);
            if (sets_it != g_descriptor_pool_sets.end()) {
                sets_it->second.erase(set);
                if (sets_it->second.empty()) {
                    g_descriptor_pool_sets.erase(sets_it);
                }
            }
            g_descriptor_set_to_pool.erase(pool_it);
        }

        g_descriptor_set_buffer_uses.erase(set);
        EraseDescriptorSetHintCacheLocked(set, "forget_descriptor_set");
        for (auto& cmd_pair : g_command_buffer_descriptor_sets) {
            cmd_pair.second.erase(set);
        }
        for (auto& cmd_pair : g_command_buffer_descriptor_dynamic_offsets) {
            cmd_pair.second.erase(set);
        }
    }
    InvalidateAllSubmitHintCachesLocked("forget_descriptor_sets");
}

static void RememberDescriptorSetsForPool(VkDescriptorPool pool,
                                          uint32_t count,
                                          const VkDescriptorSet* pDescriptorSets) {
    if (pool == VK_NULL_HANDLE || !pDescriptorSets || count == 0) return;
    std::lock_guard<std::mutex> lock(g_mapped_mutex);
    for (uint32_t i = 0; i < count; ++i) {
        VkDescriptorSet set = pDescriptorSets[i];
        if (set == VK_NULL_HANDLE) continue;
        g_descriptor_set_to_pool[set] = pool;
        g_descriptor_pool_sets[pool].insert(set);
    }
}

static void ForgetTrackedDescriptorSetsForPool(VkDescriptorPool pool) {
    if (pool == VK_NULL_HANDLE) return;
    std::lock_guard<std::mutex> lock(g_mapped_mutex);

    auto sets_it = g_descriptor_pool_sets.find(pool);
    if (sets_it == g_descriptor_pool_sets.end()) return;

    std::vector<VkDescriptorSet> sets(sets_it->second.begin(), sets_it->second.end());
    g_descriptor_pool_sets.erase(sets_it);

    for (VkDescriptorSet set : sets) {
        g_descriptor_set_to_pool.erase(set);
        g_descriptor_set_buffer_uses.erase(set);
        EraseDescriptorSetHintCacheLocked(set, "forget_descriptor_set");
        for (auto& cmd_pair : g_command_buffer_descriptor_sets) {
            cmd_pair.second.erase(set);
        }
        for (auto& cmd_pair : g_command_buffer_descriptor_dynamic_offsets) {
            cmd_pair.second.erase(set);
        }
    }
    InvalidateAllSubmitHintCachesLocked("forget_descriptor_pool");
}

static DescriptorPoolSignature MakeDescriptorPoolSignature(
    VkDevice device,
    const VkDescriptorPoolCreateInfo* pCreateInfo) {
    DescriptorPoolSignature sig;
    sig.device = device;
    if (!pCreateInfo) return sig;

    sig.flags = pCreateInfo->flags;
    sig.max_sets = pCreateInfo->maxSets;
    /*
     * The pool-size signature does not canonicalize extension state.  Never
     * cache a pool created with a pNext chain: treating such a pool as an
     * extension-free pool can change allocation limits or other semantics.
     */
    sig.reusable = pCreateInfo->pNext == nullptr;
    for (uint32_t i = 0; i < pCreateInfo->poolSizeCount; ++i) {
        const VkDescriptorPoolSize& size = pCreateInfo->pPoolSizes[i];
        auto it = std::find_if(sig.pool_sizes.begin(), sig.pool_sizes.end(),
                               [&size](const std::pair<uint32_t, uint32_t>& item) {
                                   return item.first == (uint32_t)size.type;
                               });
        if (it == sig.pool_sizes.end()) {
            sig.pool_sizes.push_back({(uint32_t)size.type, size.descriptorCount});
        } else {
            it->second += size.descriptorCount;
        }
    }
    std::sort(sig.pool_sizes.begin(), sig.pool_sizes.end());
    return sig;
}

static bool DescriptorPoolSignatureEquals(const DescriptorPoolSignature& a,
                                          const DescriptorPoolSignature& b) {
    return a.reusable &&
           b.reusable &&
           a.device == b.device &&
           a.flags == b.flags &&
           a.max_sets == b.max_sets &&
           a.pool_sizes == b.pool_sizes;
}

static VkResult SendResetDescriptorPoolForReuse(VkDevice device,
                                                VkDescriptorPool pool) {
    int express_gpu = get_express_gpu_fd();
    ParamManager mgr;
    mgr.addParam64((uint64_t)(uintptr_t)device);
    mgr.addParam64((uint64_t)(uintptr_t)pool);
    mgr.addParam32(0);
    VkResult result = VK_ERROR_DEVICE_LOST;
    mgr.addPtr(&result, sizeof(result));
    FlimeGuestBeforeDescriptorLifecycle(device);
    const ssize_t written =
        FlimeGuestWrite(&mgr,
                        express_gpu,
                        EXPRESS_GPU_DEVICE_ID,
                        FUNID_vkResetDescriptorPool,
                        true);
    const bool transport_ok = IsCompleteParamManagerWrite(written, 2);
    if (!transport_ok) {
        result = VK_ERROR_DEVICE_LOST;
    }
    if (result == VK_SUCCESS) {
        ForgetTrackedDescriptorSetsForPool(pool);
    }
    FlimeGuestResetDescriptorPool(device, pool, result);
    FlimeGuestAfterDescriptorLifecycle(device, transport_ok);
    return result;
}

static bool TryAcquireCachedDescriptorPool(const DescriptorPoolSignature& signature,
                                           VkDescriptorPool* out_pool) {
    if (!out_pool || !signature.reusable) return false;
    std::lock_guard<std::mutex> lock(g_descriptor_lifecycle_mutex);
    for (auto it = g_descriptor_pool_cache.begin();
         it != g_descriptor_pool_cache.end();
         ++it) {
        if (!DescriptorPoolSignatureEquals(it->signature, signature)) continue;

        *out_pool = it->pool;
        g_descriptor_pool_cache.erase(it);
        g_descriptor_lifecycle_stats.cache_hits++;
        g_descriptor_lifecycle_stats.reset_for_reuse++;
        return true;
    }
    return false;
}

static void MaybeLogDescriptorLifecycleStatsLocked(const char* reason);

static bool CacheDescriptorPoolForReuse(VkDescriptorPool pool,
                                        const DescriptorPoolSignature& signature) {
    if (pool == VK_NULL_HANDLE || !signature.reusable) return false;
    std::lock_guard<std::mutex> lock(g_descriptor_lifecycle_mutex);
    if (g_descriptor_pool_cache.size() >= kMaxCachedDescriptorPools) {
        g_descriptor_lifecycle_stats.cache_evictions++;
        return false;
    }

    g_descriptor_pool_cache.push_back({signature, pool});
    DescriptorLifecycleStats& s = g_descriptor_lifecycle_stats;
    s.destroy_pool_calls++;
    s.cached_pool_destroys++;
    MaybeLogDescriptorLifecycleStatsLocked("periodic");
    return true;
}

static void MaybeLogDescriptorLifecycleStatsLocked(const char* reason) {
    DescriptorLifecycleStats& s = g_descriptor_lifecycle_stats;
    const uint64_t activity =
        s.create_pool_calls + s.allocate_set_calls + s.update_set_calls +
        s.update_template_calls + s.bind_set_calls + s.destroy_pool_calls;
    if (activity == 0) return;

    const bool periodic =
        kDescriptorLifecycleStatsLogEvery != 0 &&
        (activity % kDescriptorLifecycleStatsLogEvery) == 0;
    const bool forced = reason && strcmp(reason, "periodic") != 0;
    if (!periodic && !forced) return;

    auto avg_us = [](uint64_t total, uint64_t count) -> unsigned long long {
        return (unsigned long long)(count ? (total / count) : 0);
    };

    ALOGI("[DESC_LIFECYCLE_SUMMARY] reason=%s create_pool=%llu "
          "allocate_calls=%llu allocated_sets=%llu update=%llu "
          "update_template=%llu bind_calls=%llu bound_sets=%llu "
          "destroy_calls=%llu cached_destroy=%llu cache_hits=%llu "
          "reset_reuse=%llu cache_evictions=%llu deferred_destroy=%llu flushed_destroy=%llu "
          "flushes=%llu threshold_flushes=%llu forced_flushes=%llu "
          "pending=%zu cached=%zu peak_pending=%llu "
          "time_us create=%llu alloc=%llu update=%llu template=%llu bind=%llu bind_upload=%llu "
          "avg_us create=%llu alloc=%llu update=%llu template=%llu bind=%llu bind_upload=%llu "
          "bind_upload_enabled=%d bind_upload_disabled=%llu copy_upload_enabled=%d copy_upload_disabled=%llu",
          reason ? reason : "unknown",
          (unsigned long long)s.create_pool_calls,
          (unsigned long long)s.allocate_set_calls,
          (unsigned long long)s.allocated_sets,
          (unsigned long long)s.update_set_calls,
          (unsigned long long)s.update_template_calls,
          (unsigned long long)s.bind_set_calls,
          (unsigned long long)s.bound_sets,
          (unsigned long long)s.destroy_pool_calls,
          (unsigned long long)s.cached_pool_destroys,
          (unsigned long long)s.cache_hits,
          (unsigned long long)s.reset_for_reuse,
          (unsigned long long)s.cache_evictions,
          (unsigned long long)s.deferred_pool_destroys,
          (unsigned long long)s.flushed_pool_destroys,
          (unsigned long long)s.destroy_flushes,
          (unsigned long long)s.threshold_flushes,
          (unsigned long long)s.forced_flushes,
          g_deferred_descriptor_pool_destroys.size(),
          g_descriptor_pool_cache.size(),
          (unsigned long long)s.peak_pending_destroys,
          (unsigned long long)s.create_pool_us,
          (unsigned long long)s.allocate_set_us,
          (unsigned long long)s.update_set_us,
          (unsigned long long)s.update_template_us,
          (unsigned long long)s.bind_set_us,
          (unsigned long long)s.bind_early_upload_us,
          avg_us(s.create_pool_us, s.create_pool_calls),
          avg_us(s.allocate_set_us, s.allocate_set_calls),
          avg_us(s.update_set_us, s.update_set_calls),
          avg_us(s.update_template_us, s.update_template_calls),
          avg_us(s.bind_set_us, s.bind_set_calls),
          avg_us(s.bind_early_upload_us, s.bind_set_calls),
          (int)kEnableDescriptorBindEarlyUpload,
          (unsigned long long)s.bind_early_upload_disabled,
          (int)kEnableCommandCopyEarlyUpload,
          (unsigned long long)s.copy_early_upload_disabled);
}

static std::vector<uint8_t> BuildDestroyDescriptorPoolPayload(
    VkDevice device,
    VkDescriptorPool descriptorPool,
    const VkAllocationCallbacks* pAllocator) {
    Allocator vkAllocator;

    VkAllocationCallbacks* local_pAllocator = nullptr;
    if (pAllocator) {
        local_pAllocator = (VkAllocationCallbacks*)malloc(sizeof(VkAllocationCallbacks));
        if (local_pAllocator) {
            deepcopy_VkAllocationCallbacks(
                &vkAllocator,
                VK_STRUCTURE_TYPE_MAX_ENUM,
                pAllocator,
                local_pAllocator);
        }
    }

    size_t byte_count = 0;
    if (pAllocator && local_pAllocator) {
        count_VkAllocationCallbacks(
            0,
            VK_STRUCTURE_TYPE_MAX_ENUM,
            local_pAllocator,
            &byte_count);
    }
    byte_count += 24;

    std::vector<uint8_t> payload(byte_count);
    uint8_t* send_buffer = payload.data();
    uint8_t** stream_ptr = &send_buffer;

    uint64_t guest_device = (uint64_t)(uintptr_t)device;
    uint64_t guest_descriptorPool = (uint64_t)(uintptr_t)descriptorPool;

    memcpy(*stream_ptr, &guest_device, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);

    memcpy(*stream_ptr, &guest_descriptorPool, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);

    uint64_t allocPtr = (uint64_t)(uintptr_t)(local_pAllocator ? pAllocator : nullptr);
    memcpy(*stream_ptr, &allocPtr, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);
    if (pAllocator && local_pAllocator) {
        encode_to_stream_VkAllocationCallbacks(
            VK_STRUCTURE_TYPE_MAX_ENUM,
            local_pAllocator,
            stream_ptr);
    }

    if (local_pAllocator) {
        free(local_pAllocator);
    }
    return payload;
}

static bool SendDestroyDescriptorPoolPayload(
        VkDevice device,
        VkDescriptorPool descriptorPool,
        const std::vector<uint8_t>& payload) {
    if (payload.empty()) return false;

    ParamManager mgr;
    void* send_buffer = mgr.addExternalParamPtr(payload.size());
    memcpy(send_buffer, payload.data(), payload.size());

    FlimeGuestBeforeDescriptorLifecycle(device);
    const ssize_t written =
        FlimeGuestWrite(&mgr,
                        get_express_gpu_fd(),
                        EXPRESS_GPU_DEVICE_ID,
                        FUNID_vkDestroyDescriptorPool,
                        true);
    const bool transport_ok = IsCompleteParamManagerWrite(written, 1);
    FlimeGuestAfterDescriptorLifecycle(device, transport_ok);
    return transport_ok;
}

static void FlushDeferredDescriptorPoolDestroys(const char* reason, bool force) {
    std::vector<DeferredDescriptorPoolDestroy> pending;
    {
        std::lock_guard<std::mutex> lock(g_descriptor_lifecycle_mutex);
        if (g_deferred_descriptor_pool_destroys.empty()) return;
        if (!force &&
            g_deferred_descriptor_pool_destroys.size() < kMaxDeferredDescriptorPoolDestroys) {
            return;
        }

        pending.swap(g_deferred_descriptor_pool_destroys);
        DescriptorLifecycleStats& s = g_descriptor_lifecycle_stats;
        s.destroy_flushes++;
        s.flushed_pool_destroys += pending.size();
        if (reason && strcmp(reason, "threshold") == 0) {
            s.threshold_flushes++;
        } else {
            s.forced_flushes++;
        }
    }

    int express_gpu = get_express_gpu_fd();
    const uint64_t start_us = ExpressVkNowUs();
    FlimeGuestFlushTransport(express_gpu);
    for (const DeferredDescriptorPoolDestroy& item : pending) {
        SendDestroyDescriptorPoolPayload(
            item.device, item.pool, item.payload);
    }
    const uint64_t elapsed_us = ExpressVkNowUs() - start_us;

    {
        std::lock_guard<std::mutex> lock(g_descriptor_lifecycle_mutex);
        ALOGI("[DESC_DESTROY_FLUSH] reason=%s flushed=%zu pending_after=%zu us=%llu",
              reason ? reason : "unknown",
              pending.size(),
              g_deferred_descriptor_pool_destroys.size(),
              (unsigned long long)elapsed_us);
        MaybeLogDescriptorLifecycleStatsLocked(reason ? reason : "flush");
    }
}

static void DeferDescriptorPoolDestroy(VkDevice device,
                                       VkDescriptorPool descriptorPool,
                                       const VkAllocationCallbacks* pAllocator) {
    DeferredDescriptorPoolDestroy pending;
    pending.payload =
        BuildDestroyDescriptorPoolPayload(
            device, descriptorPool, pAllocator);
    pending.device = device;
    pending.pool = descriptorPool;

    bool should_flush = false;
    {
        std::lock_guard<std::mutex> lock(g_descriptor_lifecycle_mutex);
        DescriptorLifecycleStats& s = g_descriptor_lifecycle_stats;
        s.destroy_pool_calls++;
        s.deferred_pool_destroys++;
        g_deferred_descriptor_pool_destroys.push_back(std::move(pending));
        s.peak_pending_destroys = std::max<uint64_t>(
            s.peak_pending_destroys,
            g_deferred_descriptor_pool_destroys.size());
        should_flush =
            g_deferred_descriptor_pool_destroys.size() >=
            kMaxDeferredDescriptorPoolDestroys;
        MaybeLogDescriptorLifecycleStatsLocked("periodic");
    }

    if (should_flush) {
        FlushDeferredDescriptorPoolDestroys("threshold", false);
    }
}

static void FlushCachedDescriptorPools(const char* reason, VkDevice device_filter) {
    std::vector<CachedDescriptorPool> cached;
    {
        std::lock_guard<std::mutex> lock(g_descriptor_lifecycle_mutex);
        if (g_descriptor_pool_cache.empty()) return;

        for (auto it = g_descriptor_pool_cache.begin();
             it != g_descriptor_pool_cache.end();) {
            if (device_filter == VK_NULL_HANDLE || it->signature.device == device_filter) {
                cached.push_back(std::move(*it));
                g_descriptor_pool_signatures.erase(cached.back().pool);
                it = g_descriptor_pool_cache.erase(it);
            } else {
                ++it;
            }
        }

        if (cached.empty()) return;
        DescriptorLifecycleStats& s = g_descriptor_lifecycle_stats;
        s.destroy_flushes++;
        s.flushed_pool_destroys += cached.size();
        s.forced_flushes++;
    }

    int express_gpu = get_express_gpu_fd();
    const uint64_t start_us = ExpressVkNowUs();
    FlimeGuestFlushTransport(express_gpu);
    for (const CachedDescriptorPool& item : cached) {
        const std::vector<uint8_t> payload =
            BuildDestroyDescriptorPoolPayload(
                item.signature.device, item.pool, nullptr);
        SendDestroyDescriptorPoolPayload(
            item.signature.device, item.pool, payload);
    }
    const uint64_t elapsed_us = ExpressVkNowUs() - start_us;

    {
        std::lock_guard<std::mutex> lock(g_descriptor_lifecycle_mutex);
        ALOGI("[DESC_CACHE_FLUSH] reason=%s flushed=%zu cached_after=%zu us=%llu",
              reason ? reason : "unknown",
              cached.size(),
              g_descriptor_pool_cache.size(),
              (unsigned long long)elapsed_us);
        MaybeLogDescriptorLifecycleStatsLocked(reason ? reason : "cache_flush");
    }
}
// static void ForgetTrackedBuffer(VkBuffer buffer);
// static void ForgetTrackedCommandBuffer(VkCommandBuffer commandBuffer);
// static void ForgetTrackedDescriptorSets(uint32_t count, const VkDescriptorSet* pDescriptorSets);

// -----------------------------------------------------------------------------
// Declare HAL_MODULE_INFO_SYM early so it can be referenced by nulldrv_device
// later.

namespace {
int OpenDevice(const hw_module_t* module, const char* id, hw_device_t** device);
hw_module_methods_t nulldrv_module_methods = {.open = OpenDevice};
}  // namespace

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-variable-declarations"
__attribute__((visibility("default"))) hwvulkan_module_t HAL_MODULE_INFO_SYM = {
    .common =
        {
            .tag = HARDWARE_MODULE_TAG,
            .module_api_version = HWVULKAN_MODULE_API_VERSION_0_1,
            .hal_api_version = HARDWARE_HAL_API_VERSION,
            .id = HWVULKAN_HARDWARE_MODULE_ID,
            .name = "Null Vulkan Driver",
            .author = "The Android Open Source Project",
            .methods = &nulldrv_module_methods,
        },
};
#pragma clang diagnostic pop

// -----------------------------------------------------------------------------

namespace {

int CloseDevice(struct hw_device_t* /*device*/) {
    // nothing to do - opening a device doesn't allocate any resources
    return 0;
}

hwvulkan_device_t nulldrv_device = {
    .common =
        {
            .tag = HARDWARE_DEVICE_TAG,
            .version = HWVULKAN_DEVICE_API_VERSION_0_1,
            .module = &HAL_MODULE_INFO_SYM.common,
            .close = CloseDevice,
        },
    .EnumerateInstanceExtensionProperties =
        EnumerateInstanceExtensionProperties,
    .CreateInstance = CreateInstance,
    .GetInstanceProcAddr = GetInstanceProcAddr};

int OpenDevice(const hw_module_t* /*module*/,
               const char* id,
               hw_device_t** device) {
    if (strcmp(id, HWVULKAN_DEVICE_0) == 0) {
        *device = &nulldrv_device.common;
        return 0;
    }
    return -ENOENT;
}

VkInstance_T* GetInstanceFromPhysicalDevice(
    VkPhysicalDevice_T* physical_device) {
    return reinterpret_cast<VkInstance_T*>(
        reinterpret_cast<uintptr_t>(physical_device) -
        offsetof(VkInstance_T, physical_device));
}

uint64_t AllocHandle(uint64_t type, uint64_t* next_handle) {
    const uint64_t kHandleMask = (UINT64_C(1) << 56) - 1;
    ALOGE_IF(*next_handle == kHandleMask,
             "non-dispatchable handles of type=%" PRIu64
             " are about to overflow",
             type);
    return (UINT64_C(1) << 63) | ((type & 0x7) << 56) |
           ((*next_handle)++ & kHandleMask);
}

template <class Handle>
Handle AllocHandle(VkInstance instance, HandleType::Enum type) {
    return reinterpret_cast<Handle>(
        AllocHandle(type, &instance->next_callback_handle));
}

template <class Handle>
Handle AllocHandle(VkDevice device, HandleType::Enum type) {
    return reinterpret_cast<Handle>(
        AllocHandle(type, &device->next_handle[type]));
}

// #include <string.h>

typedef enum ExpressVkObjectType {
    EXPRESS_VK_OBJECT_TYPE_INSTANCE,
    EXPRESS_VK_OBJECT_TYPE_PHYSICAL_DEVICE,
    EXPRESS_VK_OBJECT_TYPE_DEVICE,
    EXPRESS_VK_OBJECT_TYPE_QUEUE,
    EXPRESS_VK_OBJECT_TYPE_COMMAND_BUFFER,
    EXPRESS_VK_OBJECT_TYPE_DEVICE_MEMORY,
    EXPRESS_VK_OBJECT_TYPE_BUFFER,
    EXPRESS_VK_OBJECT_TYPE_BUFFER_VIEW,
    EXPRESS_VK_OBJECT_TYPE_IMAGE,
    EXPRESS_VK_OBJECT_TYPE_IMAGE_VIEW,
    EXPRESS_VK_OBJECT_TYPE_SHADER_MODULE,
    EXPRESS_VK_OBJECT_TYPE_DESCRIPTOR_POOL,
    EXPRESS_VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT,
    EXPRESS_VK_OBJECT_TYPE_DESCRIPTOR_SET,
    EXPRESS_VK_OBJECT_TYPE_SAMPLER,
    EXPRESS_VK_OBJECT_TYPE_PIPELINE,
    EXPRESS_VK_OBJECT_TYPE_PIPELINE_CACHE,
    EXPRESS_VK_OBJECT_TYPE_PIPELINE_LAYOUT,
    EXPRESS_VK_OBJECT_TYPE_RENDER_PASS,
    EXPRESS_VK_OBJECT_TYPE_FRAMEBUFFER,
    EXPRESS_VK_OBJECT_TYPE_COMMAND_POOL,
    EXPRESS_VK_OBJECT_TYPE_FENCE,
    EXPRESS_VK_OBJECT_TYPE_SEMAPHORE,
    EXPRESS_VK_OBJECT_TYPE_EVENT,
    EXPRESS_VK_OBJECT_TYPE_QUERY_POOL,
    EXPRESS_VK_OBJECT_TYPE_SAMPLER_YCBCR_CONVERSION,
    EXPRESS_VK_OBJECT_TYPE_DESCRIPTOR_UPDATE_TEMPLATE,
    EXPRESS_VK_OBJECT_TYPE_SURFACE_KHR,
    EXPRESS_VK_OBJECT_TYPE_SWAPCHAIN_KHR,
    EXPRESS_VK_OBJECT_TYPE_DISPLAY_KHR,
    EXPRESS_VK_OBJECT_TYPE_DISPLAY_MODE_KHR,
    EXPRESS_VK_OBJECT_TYPE_VALIDATION_CACHE_EXT,
    EXPRESS_VK_OBJECT_TYPE_DEBUG_REPORT_CALLBACK_EXT,
    EXPRESS_VK_OBJECT_TYPE_DEBUG_UTILS_MESSENGER_EXT,
    EXPRESS_VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_NV,
    EXPRESS_VK_OBJECT_TYPE_INDIRECT_COMMANDS_LAYOUT_NV,
    EXPRESS_VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR,
    EXPRESS_VK_OBJECT_TYPE_CU_MODULE_NVX,
    EXPRESS_VK_OBJECT_TYPE_CU_FUNCTION_NVX,
    EXPRESS_VK_OBJECT_TYPE_MICROMAP_EXT,

    EXPRESS_VK_OBJECT_TYPE_MAX_ENUM
} ExpressVkObjectType;

constexpr size_t MAX_OBJECT_TYPE = EXPRESS_VK_OBJECT_TYPE_MICROMAP_EXT + 1;
static uint64_t counters[MAX_OBJECT_TYPE] = {0};
static std::atomic_flag counter_lock = ATOMIC_FLAG_INIT;

inline void spin_lock(std::atomic_flag& lock) {
    while (lock.test_and_set(std::memory_order_acquire));
}

inline void spin_unlock(std::atomic_flag& lock) {
    lock.clear(std::memory_order_release);
}

uint64_t get_vk_object_guest_counter(ExpressVkObjectType type) {
    spin_lock(counter_lock);
    uint64_t value = ++counters[type];
    spin_unlock(counter_lock);
    return value;
}

VKAPI_ATTR void* DefaultAllocate(void*,
                                 size_t size,
                                 size_t alignment,
                                 VkSystemAllocationScope) {
    void* ptr = nullptr;
    // Vulkan requires 'alignment' to be a power of two, but posix_memalign
    // additionally requires that it be at least sizeof(void*).
    int ret = posix_memalign(&ptr, std::max(alignment, sizeof(void*)), size);
    return ret == 0 ? ptr : nullptr;
}

VKAPI_ATTR void* DefaultReallocate(void*,
                                   void* ptr,
                                   size_t size,
                                   size_t alignment,
                                   VkSystemAllocationScope) {
    if (size == 0) {
        free(ptr);
        return nullptr;
    }

    // TODO(jessehall): Right now we never shrink allocations; if the new
    // request is smaller than the existing chunk, we just continue using it.
    // The null driver never reallocs, so this doesn't matter. If that changes,
    // or if this code is copied into some other project, this should probably
    // have a heuristic to allocate-copy-free when doing so will save "enough"
    // space.
    size_t old_size = ptr ? malloc_usable_size(ptr) : 0;
    if (size <= old_size)
        return ptr;

    void* new_ptr = nullptr;
    if (posix_memalign(&new_ptr, std::max(alignment, sizeof(void*)), size) != 0)
        return nullptr;
    if (ptr) {
        memcpy(new_ptr, ptr, std::min(old_size, size));
        free(ptr);
    }
    return new_ptr;
}

VKAPI_ATTR void DefaultFree(void*, void* ptr) {
    free(ptr);
}

const VkAllocationCallbacks kDefaultAllocCallbacks = {
    .pUserData = nullptr,
    .pfnAllocation = DefaultAllocate,
    .pfnReallocation = DefaultReallocate,
    .pfnFree = DefaultFree,
};

}  // namespace

namespace null_driver {

#define DEFINE_OBJECT_HANDLE_CONVERSION(T)              \
    T* Get##T##FromHandle(Vk##T h);                     \
    T* Get##T##FromHandle(Vk##T h) {                    \
        return reinterpret_cast<T*>(uintptr_t(h));      \
    }                                                   \
    Vk##T GetHandleTo##T(const T* obj);                 \
    Vk##T GetHandleTo##T(const T* obj) {                \
        return Vk##T(reinterpret_cast<uintptr_t>(obj)); \
    }

// -----------------------------------------------------------------------------
// Global

VKAPI_ATTR VkResult EnumerateInstanceVersion(uint32_t* pApiVersion) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    mgr.addPtr(pApiVersion, sizeof(uint32_t));
    
    VkResult vkResult = VK_SUCCESS;
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkEnumerateInstanceVersion, true);
    // Pass through host version unchanged.  Capping to 1.0 breaks instance
    // creation because the Android 13 Vulkan loader requires >= 1.1 from HAL.
    ALOGI("EnumerateInstanceVersion %u.%u.%u",
          VK_VERSION_MAJOR(*pApiVersion), VK_VERSION_MINOR(*pApiVersion),
          VK_VERSION_PATCH(*pApiVersion));
    
    return vkResult;
}

// VKAPI_ATTR
// VkResult EnumerateInstanceExtensionProperties(
//     const char* layer_name,
//     uint32_t* count,
//     VkExtensionProperties* properties) {
//     ALOGI("EnumerateInstanceExtensionProperties %s", layer_name);
//     if (layer_name) {
//         ALOGW(
//             "Driver vkEnumerateInstanceExtensionProperties shouldn't be called "
//             "with a layer name ('%s')",
//             layer_name);
//     }


//     const VkExtensionProperties kExtensions[] = {

//         {
//             VK_KHR_SURFACE_EXTENSION_NAME,

//         },

//         {
//             VK_KHR_ANDROID_SURFACE_EXTENSION_NAME,

//         },

//         {
//             VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
//             VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_SPEC_VERSION
//         },

//     };

//     const uint32_t kExtensionsCount = sizeof(kExtensions) / sizeof(kExtensions[0]);


//     if (!properties || *count > kExtensionsCount)
//         *count = kExtensionsCount;
    
//     ALOGI("EnumerateInstanceExtensionProperties %s %d %d", layer_name, *count, kExtensionsCount);
//     if (properties){
//         std::copy(kExtensions, kExtensions + *count, properties);
//         for (uint32_t i = 0; i < *count; ++i) {
//             ALOGI("extension %s %d", properties[i].extensionName,
//                 properties[i].specVersion);
//         }
//     }
//     return *count < kExtensionsCount ? VK_INCOMPLETE : VK_SUCCESS;
// }

VKAPI_ATTR VkResult EnumerateInstanceExtensionProperties(
    const char* pLayerName,
    uint32_t* pPropertyCount,
    VkExtensionProperties* pProperties) {
    
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    // Android specific extensions
    const VkExtensionProperties kAndroidExtensions[] = {
        { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_SURFACE_SPEC_VERSION },
        { VK_KHR_ANDROID_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_SPEC_VERSION },
        { VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_SPEC_VERSION }
    };
    const uint32_t kAndroidExtensionsCount = sizeof(kAndroidExtensions) / sizeof(kAndroidExtensions[0]);
    
    // Prepare parameters
    uint32_t has_layer = pLayerName ? 1 : 0;
    size_t layer_name_size = pLayerName ? strlen(pLayerName) + 1 : 0;
    char* buffer = (char*)mgr.addExternalParamPtr(sizeof(uint32_t) + layer_name_size);
    memcpy(buffer, &has_layer, sizeof(uint32_t));
    if (pLayerName) {
        strcpy(buffer + sizeof(uint32_t), pLayerName);
    }
    
    uint32_t host_count = 0;
    VkExtensionProperties* host_properties = nullptr;
    if (pProperties) {
        host_properties = (VkExtensionProperties*)malloc(*pPropertyCount * sizeof(VkExtensionProperties));
    }
    
    mgr.addPtr(pPropertyCount, sizeof(uint32_t));
    if (host_properties) {
        mgr.addPtr(host_properties, *pPropertyCount * sizeof(VkExtensionProperties));
    }
    VkResult vkResult = VK_SUCCESS;
    
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkEnumerateInstanceExtensionProperties, true);
    
    
    // Read host count from updated pPropertyCount
    host_count = *pPropertyCount;
    
    // Merge Android extensions with host extensions
    uint32_t total_count = host_count + kAndroidExtensionsCount;
    
    if (!pProperties) {
        *pPropertyCount = total_count;
        if (host_properties) free(host_properties);
        return VK_SUCCESS;
    }
    
    uint32_t copy_count = (*pPropertyCount < total_count) ? *pPropertyCount : total_count;
    uint32_t android_copy = (copy_count < kAndroidExtensionsCount) ? copy_count : kAndroidExtensionsCount;
    uint32_t host_copy = copy_count - android_copy;
    
    // Copy Android extensions first
    for (uint32_t i = 0; i < android_copy; ++i) {
        pProperties[i] = kAndroidExtensions[i];
    }
    
    // Copy host extensions
    for (uint32_t i = 0; i < host_copy; ++i) {
        pProperties[android_copy + i] = host_properties[i];
    }
    
    // *pPropertyCount = copy_count;
    if (host_properties) free(host_properties);

    ALOGI("EnumerateInstanceExtensionProperties %s %d %d %d",
          pLayerName ? pLayerName : "null", copy_count, total_count, *pPropertyCount);
    
    return VK_SUCCESS;
}

VKAPI_ATTR
VkResult CreateInstance(const VkInstanceCreateInfo* create_info,
                        const VkAllocationCallbacks* allocator,
                        VkInstance* out_instance) {
    if (!create_info || !out_instance) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    *out_instance = VK_NULL_HANDLE;

    int express_gpu = get_express_gpu_fd();
    ALOGI("in express driver create instance! %d %d %d", express_gpu, (int)sizeof(create_info), (int)sizeof(allocator));

    thread_local ParamManager mgr;

    ALOGI("encode vulkan instanceCreateInfo with %lld extensions %d",
          (long long)create_info->sType,
          create_info->enabledExtensionCount);
    size_t count = 0;
    size_t* countPtr = &count;
    count_VkInstanceCreateInfo(0, VK_STRUCTURE_TYPE_MAX_ENUM,
                               const_cast<VkInstanceCreateInfo*>(create_info),
                               countPtr);

    count += 8;

    char* send_buffer = (char*)mgr.addExternalParamPtr(count);
    ALOGI("size of vkcreateinstance param is %d send buffer is %lld", (int)count, (long long)send_buffer);
    uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;
    encode_to_stream_VkInstanceCreateInfo(
        VK_STRUCTURE_TYPE_MAX_ENUM,
        const_cast<VkInstanceCreateInfo*>(create_info),
        send_buffer_ptr);
    
    /*
     * Allocation callbacks contain guest function addresses and therefore
     * cannot be invoked by the host Vulkan loader.  The host still consumes
     * this legacy marker, so encode a canonical NULL allocator.
     */
    uint64_t cgen_var_0 = 0;
    memcpy((*send_buffer_ptr), &cgen_var_0, 8);
    *send_buffer_ptr += sizeof(cgen_var_0);

    ALOGI("size of vkInstanceCreateInfo is %d appinfo is %lld appinfo %d",
          (int)count,
          (long long)create_info->pApplicationInfo,
          create_info->pApplicationInfo
              ? (int)create_info->pApplicationInfo->sType
              : -1);

    if (!allocator)
        allocator = &kDefaultAllocCallbacks;
    VkInstance_T* instance =
        static_cast<VkInstance_T*>(allocator->pfnAllocation(
            allocator->pUserData, sizeof(VkInstance_T), alignof(VkInstance_T),
            VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE));
    if (!instance)
    {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    instance->dispatch.magic = HWVULKAN_DISPATCH_MAGIC;
    instance->allocator = *allocator;
    instance->physical_device.dispatch.magic = HWVULKAN_DISPATCH_MAGIC;
    instance->next_callback_handle = 0;
    *out_instance = instance;

    ALOGI("current send buffer is %lld %lld vk instance %lld", (long long)*send_buffer_ptr, (long long)send_buffer, (long long)*out_instance);

    VkResult vkResult = VK_ERROR_DEVICE_LOST;
    mgr.addParam64((uint64_t)*out_instance);
    mgr.addPtr(&vkResult, sizeof(VkResult)); /* VkResult */
    const ssize_t written = FlimeGuestWrite(
        &mgr,
        express_gpu,
        EXPRESS_GPU_DEVICE_ID,
        FUNID_vkCreateInstance,
        true);
    if (!IsCompleteParamManagerWrite(written, 2)) {
        vkResult = VK_ERROR_DEVICE_LOST;
    }

    ALOGI("after write vkCreateInstance %d", vkResult);
    if(vkResult != VK_SUCCESS) {
        instance->allocator.pfnFree(instance->allocator.pUserData, instance);
        *out_instance = VK_NULL_HANDLE;
        return vkResult;
    }
    return VK_SUCCESS;
}

VKAPI_ATTR PFN_vkVoidFunction GetInstanceProcAddr(
    VkInstance instance, const char* name) {
    ALOGI("in GetInstanceProcAddr %s", name);

    // Core 1.0
    if (strcmp(name, "vkCreateInstance") == 0)                          return (PFN_vkVoidFunction)CreateInstance;
    if (strcmp(name, "vkDestroyInstance") == 0)                         return (PFN_vkVoidFunction)DestroyInstance;
    if (strcmp(name, "vkEnumeratePhysicalDevices") == 0)                return (PFN_vkVoidFunction)EnumeratePhysicalDevices;
    if (strcmp(name, "vkGetPhysicalDeviceFeatures") == 0)               return (PFN_vkVoidFunction)GetPhysicalDeviceFeatures;
    if (strcmp(name, "vkGetPhysicalDeviceFormatProperties") == 0)       return (PFN_vkVoidFunction)GetPhysicalDeviceFormatProperties;
    if (strcmp(name, "vkGetPhysicalDeviceImageFormatProperties") == 0)  return (PFN_vkVoidFunction)GetPhysicalDeviceImageFormatProperties;
    if (strcmp(name, "vkGetPhysicalDeviceProperties") == 0)             return (PFN_vkVoidFunction)GetPhysicalDeviceProperties;
    if (strcmp(name, "vkGetPhysicalDeviceQueueFamilyProperties") == 0)  return (PFN_vkVoidFunction)GetPhysicalDeviceQueueFamilyProperties;
    if (strcmp(name, "vkGetPhysicalDeviceMemoryProperties") == 0)       return (PFN_vkVoidFunction)GetPhysicalDeviceMemoryProperties;
    if (strcmp(name, "vkGetDeviceProcAddr") == 0)                       return (PFN_vkVoidFunction)GetDeviceProcAddr;

    // Core 1.1+
    if (strcmp(name, "vkEnumerateInstanceVersion") == 0)                return (PFN_vkVoidFunction)EnumerateInstanceVersion;
    if (strcmp(name, "vkEnumerateInstanceExtensionProperties") == 0)    return (PFN_vkVoidFunction)EnumerateInstanceExtensionProperties;
    if (strcmp(name, "vkEnumerateInstanceLayerProperties") == 0)        return (PFN_vkVoidFunction)EnumerateInstanceLayerProperties;
    if (strcmp(name, "vkGetPhysicalDeviceExternalBufferPropertiesKHR") == 0) return (PFN_vkVoidFunction)GetPhysicalDeviceExternalBufferPropertiesKHR;
    if (strcmp(name, "vkGetPhysicalDeviceExternalFencePropertiesKHR") == 0)  return (PFN_vkVoidFunction)GetPhysicalDeviceExternalFenceProperties;
    if (strcmp(name, "vkGetPhysicalDeviceExternalSemaphorePropertiesKHR") == 0) return (PFN_vkVoidFunction)GetPhysicalDeviceExternalSemaphoreProperties;

    // Physical device queries – instance-level, MUST be returned by GetInstanceProcAddr
    // (ncnn and the Android Vulkan loader resolve these here)
    if (strcmp(name, "vkEnumerateDeviceExtensionProperties") == 0)              return (PFN_vkVoidFunction)EnumerateDeviceExtensionProperties;

    // Vulkan 1.1 promoted (core names)
    if (strcmp(name, "vkGetPhysicalDeviceFeatures2") == 0)                      return (PFN_vkVoidFunction)GetPhysicalDeviceFeatures2;
    if (strcmp(name, "vkGetPhysicalDeviceProperties2") == 0)                    return (PFN_vkVoidFunction)GetPhysicalDeviceProperties2;
    if (strcmp(name, "vkGetPhysicalDeviceFormatProperties2") == 0)              return (PFN_vkVoidFunction)GetPhysicalDeviceFormatProperties2;
    if (strcmp(name, "vkGetPhysicalDeviceImageFormatProperties2") == 0)         return (PFN_vkVoidFunction)GetPhysicalDeviceImageFormatProperties2;
    if (strcmp(name, "vkGetPhysicalDeviceQueueFamilyProperties2") == 0)         return (PFN_vkVoidFunction)GetPhysicalDeviceQueueFamilyProperties2;
    if (strcmp(name, "vkGetPhysicalDeviceMemoryProperties2") == 0)              return (PFN_vkVoidFunction)GetPhysicalDeviceMemoryProperties2;
    if (strcmp(name, "vkGetPhysicalDeviceSparseImageFormatProperties2") == 0)   return (PFN_vkVoidFunction)GetPhysicalDeviceSparseImageFormatProperties2;

    // KHR aliases (pre-1.1 drivers / extension-based query paths)
    if (strcmp(name, "vkGetPhysicalDeviceFeatures2KHR") == 0)                   return (PFN_vkVoidFunction)GetPhysicalDeviceFeatures2KHR;
    if (strcmp(name, "vkGetPhysicalDeviceProperties2KHR") == 0)                 return (PFN_vkVoidFunction)GetPhysicalDeviceProperties2KHR;
    if (strcmp(name, "vkGetPhysicalDeviceFormatProperties2KHR") == 0)           return (PFN_vkVoidFunction)GetPhysicalDeviceFormatProperties2KHR;
    if (strcmp(name, "vkGetPhysicalDeviceImageFormatProperties2KHR") == 0)      return (PFN_vkVoidFunction)GetPhysicalDeviceImageFormatProperties2KHR;
    if (strcmp(name, "vkGetPhysicalDeviceQueueFamilyProperties2KHR") == 0)      return (PFN_vkVoidFunction)GetPhysicalDeviceQueueFamilyProperties2KHR;
    if (strcmp(name, "vkGetPhysicalDeviceMemoryProperties2KHR") == 0)           return (PFN_vkVoidFunction)GetPhysicalDeviceMemoryProperties2KHR;
    if (strcmp(name, "vkGetPhysicalDeviceSparseImageFormatProperties2KHR") == 0) return (PFN_vkVoidFunction)GetPhysicalDeviceSparseImageFormatProperties2KHR;


    // Surface (KHR)
    // if (strcmp(name, "vkCreateSurfaceKHR") == 0)                        return (PFN_vkVoidFunction)CreateSurfaceKHR;
    // if (strcmp(name, "vkDestroySurfaceKHR") == 0)                       return (PFN_vkVoidFunction)DestroySurfaceKHR;
    // if (strcmp(name, "vkGetPhysicalDeviceSurfaceSupportKHR") == 0)      return (PFN_vkVoidFunction)GetPhysicalDeviceSurfaceSupportKHR;
    // if (strcmp(name, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR") == 0) return (PFN_vkVoidFunction)GetPhysicalDeviceSurfaceCapabilitiesKHR;
    // if (strcmp(name, "vkGetPhysicalDeviceSurfaceFormatsKHR") == 0)      return (PFN_vkVoidFunction)GetPhysicalDeviceSurfaceFormatsKHR;
    // if (strcmp(name, "vkGetPhysicalDeviceSurfacePresentModesKHR") == 0) return (PFN_vkVoidFunction)GetPhysicalDeviceSurfacePresentModesKHR;
    // if (strcmp(name, "vkGetPhysicalDeviceSurfaceFormatsKHR") == 0)      return (PFN_vkVoidFunction)GetPhysicalDeviceSurfaceFormatsKHR;
    // if (strcmp(name, "vkGetPhysicalDeviceSurfacePresentModesKHR") == 0) return (PFN_vkVoidFunction)GetPhysicalDeviceSurfacePresentModesKHR;
    // if (strcmp(name, "vkGetPhysicalDeviceSurfaceFormats2KHR") == 0)      return (PFN_vkVoidFunction)GetPhysicalDeviceSurfaceFormats2KHR;
    // if (strcmp(name, "vkGetPhysicalDeviceSurfaceSupportKHR") == 0)      return (PFN_vkVoidFunction)GetPhysicalDeviceSurfaceSupportKHR;
    // if (strcmp(name, "vkGetPhysicalDeviceSurfaceCapabilities2KHR") == 0)      return (PFN_vkVoidFunction)GetPhysicalDeviceSurfaceCapabilities2KHR;

    // // Swapchain (KHR)
    if (strcmp(name, "vkCreateSwapchainKHR") == 0)                      return (PFN_vkVoidFunction)CreateSwapchainKHR;
    // if (strcmp(name, "vkDestroySwapchainKHR") == 0)                     return (PFN_vkVoidFunction)DestroySwapchainKHR;
    if (strcmp(name, "vkGetSwapchainImagesKHR") == 0)                   return (PFN_vkVoidFunction)GetSwapchainImagesKHR_special;
    if (strcmp(name, "vkAcquireNextImageKHR_special") == 0)                     return (PFN_vkVoidFunction)AcquireNextImageKHR_special;
    // if (strcmp(name, "vkAcquireNextImage2KHR") == 0)                    return (PFN_vkVoidFunction)AcquireNextImage2KHR;
    if (strcmp(name, "vkQueuePresentKHR") == 0)                         return (PFN_vkVoidFunction)QueuePresentKHR_special;

    // Android Surface
    if (strcmp(name, "vkCreateAndroidSurfaceKHR") == 0)                 return (PFN_vkVoidFunction)CreateAndroidSurfaceKHR;

    // Device creation
    if (strcmp(name, "vkCreateDevice") == 0)                            return (PFN_vkVoidFunction)CreateDevice;

    // Fallback
    return GetInstanceProcAddr(name);
}

VKAPI_ATTR PFN_vkVoidFunction GetDeviceProcAddr(
    VkDevice device, const char* name) {
    ALOGI("in GetDeviceProcAddr %s", name);

    // Core device-level
    if (strcmp(name, "vkGetDeviceQueue") == 0)                         return (PFN_vkVoidFunction)GetDeviceQueue;
    if (strcmp(name, "vkGetDeviceQueue2") == 0)                        return (PFN_vkVoidFunction)GetDeviceQueue2;
    if (strcmp(name, "vkQueueSubmit") == 0)                            return (PFN_vkVoidFunction)QueueSubmit;
    if (strcmp(name, "vkQueueSubmit2") == 0)                           return (PFN_vkVoidFunction)QueueSubmit2;
    if (strcmp(name, "vkQueueBindSparse") == 0)                        return (PFN_vkVoidFunction)QueueBindSparse;
    // if (strcmp(name, "vkQueuePresentKHR") == 0)                        return (PFN_vkVoidFunction)QueuePresentKHR;
    if (strcmp(name, "vkQueueWaitIdle") == 0)                          return (PFN_vkVoidFunction)QueueWaitIdle;
    if (strcmp(name, "vkDeviceWaitIdle") == 0)                         return (PFN_vkVoidFunction)DeviceWaitIdle;

    // Command Pools & Buffers
    if (strcmp(name, "vkAllocateCommandBuffers") == 0)                 return (PFN_vkVoidFunction)AllocateCommandBuffers;
    if (strcmp(name, "vkFreeCommandBuffers") == 0)                     return (PFN_vkVoidFunction)FreeCommandBuffers;
    if (strcmp(name, "vkCreateCommandPool") == 0)                      return (PFN_vkVoidFunction)CreateCommandPool;
    if (strcmp(name, "vkDestroyCommandPool") == 0)                     return (PFN_vkVoidFunction)DestroyCommandPool;
    if (strcmp(name, "vkResetCommandBuffer") == 0)                     return (PFN_vkVoidFunction)ResetCommandBuffer;
    if (strcmp(name, "vkBeginCommandBuffer") == 0)                     return (PFN_vkVoidFunction)BeginCommandBuffer;
    if (strcmp(name, "vkEndCommandBuffer") == 0)                       return (PFN_vkVoidFunction)EndCommandBuffer;

    // Render Pass / Dynamic Rendering
    if (strcmp(name, "vkCmdBeginRenderPass") == 0)                     return (PFN_vkVoidFunction)CmdBeginRenderPass;
    if (strcmp(name, "vkCmdBeginRenderPass2") == 0)                    return (PFN_vkVoidFunction)CmdBeginRenderPass2;
    if (strcmp(name, "vkCmdEndRenderPass") == 0)                       return (PFN_vkVoidFunction)CmdEndRenderPass;
    if (strcmp(name, "vkCmdEndRenderPass2") == 0)                      return (PFN_vkVoidFunction)CmdEndRenderPass2;
    if (strcmp(name, "vkCmdBeginRendering") == 0)                      return (PFN_vkVoidFunction)CmdBeginRendering;
    if (strcmp(name, "vkCmdEndRendering") == 0)                        return (PFN_vkVoidFunction)CmdEndRendering;
    if (strcmp(name, "vkCmdNextSubpass") == 0)                         return (PFN_vkVoidFunction)CmdNextSubpass;
    if (strcmp(name, "vkCmdNextSubpass2") == 0)                        return (PFN_vkVoidFunction)CmdNextSubpass2;

    // Draw / Dispatch
    if (strcmp(name, "vkCmdDraw") == 0)                                return (PFN_vkVoidFunction)CmdDraw;
    if (strcmp(name, "vkCmdDrawIndexed") == 0)                         return (PFN_vkVoidFunction)CmdDrawIndexed;
    if (strcmp(name, "vkCmdDrawIndirect") == 0)                        return (PFN_vkVoidFunction)CmdDrawIndirect;
    if (strcmp(name, "vkCmdDrawIndexedIndirect") == 0)                 return (PFN_vkVoidFunction)CmdDrawIndexedIndirect;
    if (strcmp(name, "vkCmdDrawIndirectCount") == 0)                   return (PFN_vkVoidFunction)CmdDrawIndirectCount;
    if (strcmp(name, "vkCmdDrawIndexedIndirectCount") == 0)            return (PFN_vkVoidFunction)CmdDrawIndexedIndirectCount;
    if (strcmp(name, "vkCmdDispatch") == 0)                            return (PFN_vkVoidFunction)CmdDispatch;
    if (strcmp(name, "vkCmdDispatchBase") == 0)                        return (PFN_vkVoidFunction)CmdDispatchBase;
    if (strcmp(name, "vkCmdDispatchIndirect") == 0)                    return (PFN_vkVoidFunction)CmdDispatchIndirect;

    // Copy / Blit / Clear / Resolve
    if (strcmp(name, "vkCmdCopyBuffer") == 0)                          return (PFN_vkVoidFunction)CmdCopyBuffer;
    if (strcmp(name, "vkCmdCopyBuffer2") == 0)                         return (PFN_vkVoidFunction)CmdCopyBuffer2;
    if (strcmp(name, "vkCmdCopyBufferToImage") == 0)                   return (PFN_vkVoidFunction)CmdCopyBufferToImage;
    if (strcmp(name, "vkCmdCopyBufferToImage2") == 0)                  return (PFN_vkVoidFunction)CmdCopyBufferToImage2;
    if (strcmp(name, "vkCmdCopyImage") == 0)                           return (PFN_vkVoidFunction)CmdCopyImage;
    if (strcmp(name, "vkCmdCopyImage2") == 0)                          return (PFN_vkVoidFunction)CmdCopyImage2;
    if (strcmp(name, "vkCmdCopyImageToBuffer") == 0)                   return (PFN_vkVoidFunction)CmdCopyImageToBuffer;
    if (strcmp(name, "vkCmdCopyImageToBuffer2") == 0)                  return (PFN_vkVoidFunction)CmdCopyImageToBuffer2;
    if (strcmp(name, "vkCmdBlitImage") == 0)                           return (PFN_vkVoidFunction)CmdBlitImage;
    if (strcmp(name, "vkCmdBlitImage2") == 0)                          return (PFN_vkVoidFunction)CmdBlitImage2;
    if (strcmp(name, "vkCmdClearColorImage") == 0)                     return (PFN_vkVoidFunction)CmdClearColorImage;
    if (strcmp(name, "vkCmdClearDepthStencilImage") == 0)              return (PFN_vkVoidFunction)CmdClearDepthStencilImage;
    if (strcmp(name, "vkCmdClearAttachments") == 0)                    return (PFN_vkVoidFunction)CmdClearAttachments;
    if (strcmp(name, "vkCmdResolveImage") == 0)                        return (PFN_vkVoidFunction)CmdResolveImage;
    if (strcmp(name, "vkCmdResolveImage2") == 0)                       return (PFN_vkVoidFunction)CmdResolveImage2;

    // Pipeline state
    if (strcmp(name, "vkCmdBindPipeline") == 0)                        return (PFN_vkVoidFunction)CmdBindPipeline;
    if (strcmp(name, "vkCmdBindDescriptorSets") == 0)                  return (PFN_vkVoidFunction)CmdBindDescriptorSets;

    if (strcmp(name, "vkCmdSetViewport") == 0)                         return (PFN_vkVoidFunction)CmdSetViewport;
    if (strcmp(name, "vkCmdSetViewportWithCount") == 0)                return (PFN_vkVoidFunction)CmdSetViewportWithCount;
    if (strcmp(name, "vkCmdSetScissor") == 0)                          return (PFN_vkVoidFunction)CmdSetScissor;
    if (strcmp(name, "vkCmdSetScissorWithCount") == 0)                 return (PFN_vkVoidFunction)CmdSetScissorWithCount;
    if (strcmp(name, "vkCmdSetLineWidth") == 0)                        return (PFN_vkVoidFunction)CmdSetLineWidth;
    if (strcmp(name, "vkCmdSetDepthBias") == 0)                        return (PFN_vkVoidFunction)CmdSetDepthBias;
    if (strcmp(name, "vkCmdSetDepthBiasEnable") == 0)                  return (PFN_vkVoidFunction)CmdSetDepthBiasEnable;
    if (strcmp(name, "vkCmdSetDepthBounds") == 0)                      return (PFN_vkVoidFunction)CmdSetDepthBounds;
    if (strcmp(name, "vkCmdSetDepthBoundsTestEnable") == 0)            return (PFN_vkVoidFunction)CmdSetDepthBoundsTestEnable;
    if (strcmp(name, "vkCmdSetDepthCompareOp") == 0)                   return (PFN_vkVoidFunction)CmdSetDepthCompareOp;
    if (strcmp(name, "vkCmdSetDepthTestEnable") == 0)                  return (PFN_vkVoidFunction)CmdSetDepthTestEnable;
    if (strcmp(name, "vkCmdSetDepthWriteEnable") == 0)                 return (PFN_vkVoidFunction)CmdSetDepthWriteEnable;
    if (strcmp(name, "vkCmdSetStencilTestEnable") == 0)                return (PFN_vkVoidFunction)CmdSetStencilTestEnable;
    if (strcmp(name, "vkCmdSetStencilOp") == 0)                        return (PFN_vkVoidFunction)CmdSetStencilOp;
    if (strcmp(name, "vkCmdSetStencilCompareMask") == 0)               return (PFN_vkVoidFunction)CmdSetStencilCompareMask;
    if (strcmp(name, "vkCmdSetStencilWriteMask") == 0)                 return (PFN_vkVoidFunction)CmdSetStencilWriteMask;
    if (strcmp(name, "vkCmdSetStencilReference") == 0)                 return (PFN_vkVoidFunction)CmdSetStencilReference;
    if (strcmp(name, "vkCmdSetFrontFace") == 0)                        return (PFN_vkVoidFunction)CmdSetFrontFace;
    if (strcmp(name, "vkCmdSetCullMode") == 0)                         return (PFN_vkVoidFunction)CmdSetCullMode;
    if (strcmp(name, "vkCmdSetPrimitiveTopology") == 0)                return (PFN_vkVoidFunction)CmdSetPrimitiveTopology;
    if (strcmp(name, "vkCmdSetPrimitiveRestartEnable") == 0)           return (PFN_vkVoidFunction)CmdSetPrimitiveRestartEnable;
    if (strcmp(name, "vkCmdSetRasterizerDiscardEnable") == 0)          return (PFN_vkVoidFunction)CmdSetRasterizerDiscardEnable;

    // Queries & timestamps
    if (strcmp(name, "vkCmdBeginQuery") == 0)                         return (PFN_vkVoidFunction)CmdBeginQuery;
    if (strcmp(name, "vkCmdEndQuery") == 0)                           return (PFN_vkVoidFunction)CmdEndQuery;
    if (strcmp(name, "vkCmdFillBuffer") == 0)                         return (PFN_vkVoidFunction)CmdFillBuffer;
    if (strcmp(name, "vkCmdResetEvent") == 0)                         return (PFN_vkVoidFunction)CmdResetEvent;
    if (strcmp(name, "vkCmdSetEvent") == 0)                           return (PFN_vkVoidFunction)CmdSetEvent;
    if (strcmp(name, "vkCmdEndQuery") == 0)                           return (PFN_vkVoidFunction)CmdEndQuery;
    if (strcmp(name, "vkCmdBindVertexBuffers") == 0)                  return (PFN_vkVoidFunction)CmdBindVertexBuffers;
    if (strcmp(name, "vkCmdBindIndexBuffer") == 0)                   return (PFN_vkVoidFunction)CmdBindIndexBuffer;
    if (strcmp(name, "vkCmdExecuteCommands") == 0)                   return (PFN_vkVoidFunction)CmdExecuteCommands;
    if (strcmp(name, "vkCmdPushConstants") == 0)                     return (PFN_vkVoidFunction)CmdPushConstants;
    if (strcmp(name, "vkCmdUpdateBuffer") == 0)                      return (PFN_vkVoidFunction)CmdUpdateBuffer;


    if (strcmp(name, "vkCmdResetQueryPool") == 0)                     return (PFN_vkVoidFunction)CmdResetQueryPool;
    if (strcmp(name, "vkCmdCopyQueryPoolResults") == 0)               return (PFN_vkVoidFunction)CmdCopyQueryPoolResults;
    if (strcmp(name, "vkCmdWriteTimestamp") == 0)                     return (PFN_vkVoidFunction)CmdWriteTimestamp;
    if (strcmp(name, "vkCmdWriteTimestamp2") == 0)                    return (PFN_vkVoidFunction)CmdWriteTimestamp2;

    // Memory & Descriptors
    if (strcmp(name, "vkCreateBuffer") == 0)                          return (PFN_vkVoidFunction)CreateBuffer;
    if (strcmp(name, "vkDestroyBuffer") == 0)                         return (PFN_vkVoidFunction)DestroyBuffer;
    if (strcmp(name, "vkCreateBufferView") == 0)                      return (PFN_vkVoidFunction)CreateBufferView;
    if (strcmp(name, "vkDestroyBufferView") == 0)                     return (PFN_vkVoidFunction)DestroyBufferView;
    if (strcmp(name, "vkGetBufferMemoryRequirements") == 0)           return (PFN_vkVoidFunction)GetBufferMemoryRequirements;
    if (strcmp(name, "vkGetBufferMemoryRequirements2") == 0)          return (PFN_vkVoidFunction)GetBufferMemoryRequirements2;
    if (strcmp(name, "vkGetBufferDeviceAddress") == 0)                return (PFN_vkVoidFunction)GetBufferDeviceAddress;
    if (strcmp(name, "vkGetBufferOpaqueCaptureAddress") == 0)         return (PFN_vkVoidFunction)GetBufferOpaqueCaptureAddress;
    if (strcmp(name, "vkBindBufferMemory") == 0)                      return (PFN_vkVoidFunction)BindBufferMemory;
    if (strcmp(name, "vkBindBufferMemory2") == 0)                     return (PFN_vkVoidFunction)BindBufferMemory2;
    if (strcmp(name, "vkBindBufferMemory2KHR") == 0)                  return (PFN_vkVoidFunction)BindBufferMemory2KHR;
    if (strcmp(name, "vkCreateImage") == 0)                           return (PFN_vkVoidFunction)CreateImage;
    if (strcmp(name, "vkDestroyImage") == 0)                          return (PFN_vkVoidFunction)DestroyImage;
    if (strcmp(name, "vkCreateImageView") == 0)                       return (PFN_vkVoidFunction)CreateImageView;
    if (strcmp(name, "vkDestroyImageView") == 0)                      return (PFN_vkVoidFunction)DestroyImageView;
    if (strcmp(name, "vkGetImageMemoryRequirements") == 0)            return (PFN_vkVoidFunction)GetImageMemoryRequirements;
    if (strcmp(name, "vkGetImageMemoryRequirements2") == 0)           return (PFN_vkVoidFunction)GetImageMemoryRequirements2;
    if (strcmp(name, "vkGetImageSparseMemoryRequirements") == 0)      return (PFN_vkVoidFunction)GetImageSparseMemoryRequirements;
    if (strcmp(name, "vkGetImageSparseMemoryRequirements2") == 0)     return (PFN_vkVoidFunction)GetImageSparseMemoryRequirements2;
    if (strcmp(name, "vkGetImageSubresourceLayout") == 0)             return (PFN_vkVoidFunction)GetImageSubresourceLayout;
    if (strcmp(name, "vkBindImageMemory") == 0)                       return (PFN_vkVoidFunction)BindImageMemory;
    if (strcmp(name, "vkBindImageMemory2") == 0)                      return (PFN_vkVoidFunction)BindImageMemory2;
    if (strcmp(name, "vkBindImageMemory2KHR") == 0)                   return (PFN_vkVoidFunction)BindImageMemory2KHR;
    if (strcmp(name, "vkFlushMappedMemoryRanges") == 0)               return (PFN_vkVoidFunction)FlushMappedMemoryRanges;
    if (strcmp(name, "vkInvalidateMappedMemoryRanges") == 0)          return (PFN_vkVoidFunction)InvalidateMappedMemoryRanges;
    if (strcmp(name, "vkMapMemory") == 0)                             return (PFN_vkVoidFunction)MapMemory;
    if (strcmp(name, "vkUnmapMemory") == 0)                           return (PFN_vkVoidFunction)UnmapMemory;
    if (strcmp(name, "vkAllocateMemory") == 0)                        return (PFN_vkVoidFunction)AllocateMemory;  
    if (strcmp(name, "vkFreeMemory") == 0)                            return (PFN_vkVoidFunction)FreeMemory;

    // Descriptor Pools & Sets
    if (strcmp(name, "vkCreateDescriptorPool") == 0)                  return (PFN_vkVoidFunction)CreateDescriptorPool;
    if (strcmp(name, "vkDestroyDescriptorPool") == 0)                 return (PFN_vkVoidFunction)DestroyDescriptorPool;
    if (strcmp(name, "vkAllocateDescriptorSets") == 0)                return (PFN_vkVoidFunction)AllocateDescriptorSets;
    if (strcmp(name, "vkFreeDescriptorSets") == 0)                    return (PFN_vkVoidFunction)FreeDescriptorSets;
    if (strcmp(name, "vkUpdateDescriptorSets") == 0)                  return (PFN_vkVoidFunction)UpdateDescriptorSets;
    if (strcmp(name, "vkCreateDescriptorSetLayout") == 0)             return (PFN_vkVoidFunction)CreateDescriptorSetLayout;
    if (strcmp(name, "vkDestroyDescriptorSetLayout") == 0)            return (PFN_vkVoidFunction)DestroyDescriptorSetLayout;
    if (strcmp(name, "vkGetDescriptorSetLayoutSupport") == 0)         return (PFN_vkVoidFunction)GetDescriptorSetLayoutSupport;
    if (strcmp(name, "vkCreateDescriptorUpdateTemplate") == 0)        return (PFN_vkVoidFunction)CreateDescriptorUpdateTemplate;
    if (strcmp(name, "vkDestroyDescriptorUpdateTemplate") == 0)       return (PFN_vkVoidFunction)DestroyDescriptorUpdateTemplate;
    if (strcmp(name, "vkUpdateDescriptorSetWithTemplate") == 0)       return (PFN_vkVoidFunction)UpdateDescriptorSetWithTemplate;

    // Pipeline Cache
    if (strcmp(name, "vkCreatePipelineCache") == 0)                   return (PFN_vkVoidFunction)CreatePipelineCache;
    if (strcmp(name, "vkDestroyPipelineCache") == 0)                  return (PFN_vkVoidFunction)DestroyPipelineCache;
    if (strcmp(name, "vkGetPipelineCacheData") == 0)                  return (PFN_vkVoidFunction)GetPipelineCacheData;
    if (strcmp(name, "vkMergePipelineCaches") == 0)                   return (PFN_vkVoidFunction)MergePipelineCaches;

    // Pipeline Layout & Pipelines
    if (strcmp(name, "vkCreateShaderModule") == 0)                    return (PFN_vkVoidFunction)CreateShaderModule;
    if (strcmp(name, "vkCreatePipelineLayout") == 0)                  return (PFN_vkVoidFunction)CreatePipelineLayout;
    if (strcmp(name, "vkDestroyPipelineLayout") == 0)                 return (PFN_vkVoidFunction)DestroyPipelineLayout;
    if (strcmp(name, "vkCreateGraphicsPipelines") == 0)               return (PFN_vkVoidFunction)CreateGraphicsPipelines;
    if (strcmp(name, "vkCreateComputePipelines") == 0)                return (PFN_vkVoidFunction)CreateComputePipelines;
    if (strcmp(name, "vkDestroyPipeline") == 0)                       return (PFN_vkVoidFunction)DestroyPipeline;

    // Sync Primitives
    if (strcmp(name, "vkCreateFence") == 0)                           return (PFN_vkVoidFunction)CreateFence;
    if (strcmp(name, "vkDestroyFence") == 0)                          return (PFN_vkVoidFunction)DestroyFence;
    if (strcmp(name, "vkGetFenceStatus") == 0)                        return (PFN_vkVoidFunction)GetFenceStatus;
    if (strcmp(name, "vkResetFences") == 0)                           return (PFN_vkVoidFunction)ResetFences;
    if (strcmp(name, "vkResetQueryPool") == 0)                        return (PFN_vkVoidFunction)ResetQueryPool;

    if (strcmp(name, "vkWaitForFences") == 0)                         return (PFN_vkVoidFunction)WaitForFences;
    if (strcmp(name, "vkCreateSemaphore") == 0)                       return (PFN_vkVoidFunction)CreateSemaphore;
    if (strcmp(name, "vkDestroySemaphore") == 0)                      return (PFN_vkVoidFunction)DestroySemaphore;
    if (strcmp(name, "vkSignalSemaphore") == 0)                       return (PFN_vkVoidFunction)SignalSemaphore;
    if (strcmp(name, "vkWaitSemaphores") == 0)                        return (PFN_vkVoidFunction)WaitSemaphores;
    if (strcmp(name, "vkGetSemaphoreCounterValue") == 0)              return (PFN_vkVoidFunction)GetSemaphoreCounterValue;
    // if (strcmp(name, "vkResetSemaphore") == 0)                        return (PFN_vkVoidFunction)ResetSemaphore;

    // Events & Queries
    if (strcmp(name, "vkCreateEvent") == 0)                           return (PFN_vkVoidFunction)CreateEvent;
    if (strcmp(name, "vkDestroyEvent") == 0)                          return (PFN_vkVoidFunction)DestroyEvent;
    if (strcmp(name, "vkSetEvent") == 0)                              return (PFN_vkVoidFunction)SetEvent;
    if (strcmp(name, "vkResetEvent") == 0)                            return (PFN_vkVoidFunction)ResetEvent;
    if (strcmp(name, "vkGetEventStatus") == 0)                        return (PFN_vkVoidFunction)GetEventStatus;

    // Display & Surface Extensions
    // if (strcmp(name, "vkCreateDisplayModeKHR") == 0)                  return (PFN_vkVoidFunction)CreateDisplayModeKHR;
    // if (strcmp(name, "vkGetDisplayModePropertiesKHR") == 0)           return (PFN_vkVoidFunction)GetDisplayModePropertiesKHR;
    // if (strcmp(name, "vkGetDisplayModeProperties2KHR") == 0)          return (PFN_vkVoidFunction)GetDisplayModeProperties2KHR;
    // if (strcmp(name, "vkGetDisplayPlaneCapabilitiesKHR") == 0)        return (PFN_vkVoidFunction)GetDisplayPlaneCapabilitiesKHR;
    // if (strcmp(name, "vkGetDisplayPlaneCapabilities2KHR") == 0)       return (PFN_vkVoidFunction)GetDisplayPlaneCapabilities2KHR;
    // if (strcmp(name, "vkGetDisplayPlaneSupportedDisplaysKHR") == 0)   return (PFN_vkVoidFunction)GetDisplayPlaneSupportedDisplaysKHR;
    // if (strcmp(name, "vkCreateDisplayPlaneSurfaceKHR") == 0)          return (PFN_vkVoidFunction)CreateDisplayPlaneSurfaceKHR;
    // if (strcmp(name, "vkEnumerateDeviceLayerProperties") == 0)        return (PFN_vkVoidFunction)EnumerateDeviceLayerProperties;
    // if (strcmp(name, "vkGetBufferOpaqueCaptureAddress") == 0)         return (PFN_vkVoidFunction)GetBufferOpaqueCaptureAddress;
    // if (strcmp(name, "vkGetDescriptorSetLayoutSupport") == 0)          return (PFN_vkVoidFunction)GetDescriptorSetLayoutSupport;
    // if (strcmp(name, "vkGetDeviceGroupPeerMemoryFeatures") == 0)        return (PFN_vkVoidFunction)GetDeviceGroupPeerMemoryFeatures;
    // if (strcmp(name, "vkGetDeviceGroupPresentCapabilitiesKHR") == 0)       return (PFN_vkVoidFunction)GetDeviceGroupPresentCapabilitiesKHR;
    // if (strcmp(name, "vkGetDeviceImageMemoryRequirements") == 0)   return (PFN_vkVoidFunction)GetDeviceImageMemoryRequirements;
    // if (strcmp(name, "vkGetDeviceImageSparseMemoryRequirements") == 0)          return (PFN_vkVoidFunction)GetDeviceImageSparseMemoryRequirements;


    // Miscellaneous
    if (strcmp(name, "vkEnumeratePhysicalDeviceGroups") == 0)         return (PFN_vkVoidFunction)EnumeratePhysicalDeviceGroups;
    if (strcmp(name, "vkTrimCommandPool") == 0)                       return (PFN_vkVoidFunction)TrimCommandPool;
    if (strcmp(name, "vkDeviceWaitIdle") == 0)                        return (PFN_vkVoidFunction)DeviceWaitIdle;

    if (strcmp(name, "vkDestroyDevice") == 0)                         return (PFN_vkVoidFunction)DestroyDevice;
    if (strcmp(name, "vkCreateRenderPass") == 0)                      return (PFN_vkVoidFunction)CreateRenderPass;
    if (strcmp(name, "vkDestroyRenderPass") == 0)                     return (PFN_vkVoidFunction)DestroyRenderPass;
    if (strcmp(name, "vkCreateFramebuffer") == 0)                     return (PFN_vkVoidFunction)CreateFramebuffer;
    if (strcmp(name, "vkDestroyFramebuffer") == 0)                    return (PFN_vkVoidFunction)DestroyFramebuffer;
    if (strcmp(name, "vkCmdPipelineBarrier") == 0)                    return (PFN_vkVoidFunction)CmdPipelineBarrier;
    if (strcmp(name, "vkCreateSampler") == 0)                         return (PFN_vkVoidFunction)CreateSampler;

    // ncnn-required: query/pool/sampler/shader destroy + misc (Cat A: impl exists, was missing)
    if (strcmp(name, "vkCreateQueryPool") == 0)                      return (PFN_vkVoidFunction)CreateQueryPool;
    if (strcmp(name, "vkDestroyQueryPool") == 0)                     return (PFN_vkVoidFunction)DestroyQueryPool;
    if (strcmp(name, "vkGetQueryPoolResults") == 0)                  return (PFN_vkVoidFunction)GetQueryPoolResults;
    if (strcmp(name, "vkDestroySampler") == 0)                       return (PFN_vkVoidFunction)DestroySampler;
    if (strcmp(name, "vkDestroyShaderModule") == 0)                  return (PFN_vkVoidFunction)DestroyShaderModule;
    if (strcmp(name, "vkResetCommandPool") == 0)                     return (PFN_vkVoidFunction)ResetCommandPool;
    if (strcmp(name, "vkResetDescriptorPool") == 0)                  return (PFN_vkVoidFunction)ResetDescriptorPool;
    if (strcmp(name, "vkGetDeviceMemoryCommitment") == 0)            return (PFN_vkVoidFunction)GetDeviceMemoryCommitment;
    if (strcmp(name, "vkEnumerateDeviceLayerProperties") == 0)       return (PFN_vkVoidFunction)EnumerateDeviceLayerProperties;

    // KHR aliases (ncnn resolves these; point to promoted core implementations)
    if (strcmp(name, "vkCreateDescriptorUpdateTemplateKHR") == 0)   return (PFN_vkVoidFunction)CreateDescriptorUpdateTemplate;
    if (strcmp(name, "vkDestroyDescriptorUpdateTemplateKHR") == 0)  return (PFN_vkVoidFunction)DestroyDescriptorUpdateTemplate;
    if (strcmp(name, "vkUpdateDescriptorSetWithTemplateKHR") == 0)  return (PFN_vkVoidFunction)UpdateDescriptorSetWithTemplate;
    if (strcmp(name, "vkGetBufferDeviceAddressKHR") == 0)           return (PFN_vkVoidFunction)GetBufferDeviceAddress;
    if (strcmp(name, "vkGetBufferDeviceAddressEXT") == 0)           return (PFN_vkVoidFunction)GetBufferDeviceAddress;
    if (strcmp(name, "vkGetBufferMemoryRequirements2KHR") == 0)     return (PFN_vkVoidFunction)GetBufferMemoryRequirements2;
    if (strcmp(name, "vkGetBufferOpaqueCaptureAddressKHR") == 0)    return (PFN_vkVoidFunction)GetBufferOpaqueCaptureAddress;
    if (strcmp(name, "vkGetDescriptorSetLayoutSupportKHR") == 0)    return (PFN_vkVoidFunction)GetDescriptorSetLayoutSupport;
    if (strcmp(name, "vkGetImageMemoryRequirements2KHR") == 0)      return (PFN_vkVoidFunction)GetImageMemoryRequirements2;
    if (strcmp(name, "vkTrimCommandPoolKHR") == 0)                  return (PFN_vkVoidFunction)TrimCommandPool;
    if (strcmp(name, "vkEnumeratePhysicalDeviceGroupsKHR") == 0)    return (PFN_vkVoidFunction)EnumeratePhysicalDeviceGroups;

     // VK_KHR_sampler_ycbcr_conversion — core (Vulkan 1.1) + KHR alias
    if (strcmp(name, "vkCreateSamplerYcbcrConversion") == 0)        return (PFN_vkVoidFunction)CreateSamplerYcbcrConversion;
    if (strcmp(name, "vkCreateSamplerYcbcrConversionKHR") == 0)     return (PFN_vkVoidFunction)CreateSamplerYcbcrConversion;
    if (strcmp(name, "vkDestroySamplerYcbcrConversion") == 0)       return (PFN_vkVoidFunction)DestroySamplerYcbcrConversion;
    if (strcmp(name, "vkDestroySamplerYcbcrConversionKHR") == 0)    return (PFN_vkVoidFunction)DestroySamplerYcbcrConversion;
    // VK_KHR_buffer_device_address — GetDeviceMemoryOpaqueCaptureAddress core + KHR alias
    if (strcmp(name, "vkGetDeviceMemoryOpaqueCaptureAddress") == 0)    return (PFN_vkVoidFunction)GetDeviceMemoryOpaqueCaptureAddress;
    if (strcmp(name, "vkGetDeviceMemoryOpaqueCaptureAddressKHR") == 0) return (PFN_vkVoidFunction)GetDeviceMemoryOpaqueCaptureAddress;


    return GetInstanceProcAddr(name);
}

VkResult CreateAndroidSurfaceKHR(
    VkInstance instance,
    const VkAndroidSurfaceCreateInfoKHR* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSurfaceKHR* pSurface) {
    ALOGI("CreateAndroidSurfaceKHR with surface %lld", (long long)pSurface);

    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;

    uint64_t guestWin = (uint64_t)(uintptr_t)pCreateInfo->window;
    uint64_t hostSurface = 0;
    
    mgr.addParam64((uint64_t)instance);
    mgr.addParam64(guestWin);
    mgr.addPtr(pSurface, sizeof(VkSurfaceKHR));
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCreateAndroidSurfaceKHR, false);

    // *out_surface = (VkSurfaceKHR)hostSurface;
    return VK_SUCCESS;
}
// VKAPI_ATTR VkResult GetPhysicalDeviceSurfaceFormatsKHR(
//     VkPhysicalDevice physicalDevice,
//     VkSurfaceKHR surface,
//     uint32_t* pSurfaceFormatCount,
//     VkSurfaceFormatKHR* pSurfaceFormats) {
    
//     int express_gpu = get_express_gpu_fd();
//     thread_local ParamManager mgr;
    
//     uint64_t guest_phys_dev = (uint64_t)(uintptr_t)physicalDevice;
//     uint64_t guest_surface = (uint64_t)(uintptr_t)surface;
    
//     mgr.addParam64(guest_phys_dev);
//     mgr.addParam64(guest_surface);
//     mgr.addPtr(pSurfaceFormatCount, sizeof(uint32_t));
    
//     if (pSurfaceFormats && *pSurfaceFormatCount > 0) {
//         mgr.addPtr(pSurfaceFormats, sizeof(VkSurfaceFormatKHR) * (*pSurfaceFormatCount));
//     }
    
//     VkResult vkResult = VK_SUCCESS;
//     FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID,
//               FUNID_vkGetPhysicalDeviceSurfaceFormatsKHR, true);
    
//     return vkResult;
// }

// VKAPI_ATTR VkResult GetPhysicalDeviceSurfaceFormats2KHR(
//     VkPhysicalDevice physicalDevice,
//     const VkPhysicalDeviceSurfaceInfo2KHR* pSurfaceInfo,
//     uint32_t* pSurfaceFormatCount,
//     VkSurfaceFormat2KHR* pSurfaceFormats) {
    
//     int express_gpu = get_express_gpu_fd();
//     thread_local ParamManager mgr;
//     Allocator vkAllocator;
//     LOGI("GetPhysicalDeviceSurfaceFormats2KHR called");
    
//     VkPhysicalDeviceSurfaceInfo2KHR* local_pSurfaceInfo = nullptr;
//     if (pSurfaceInfo) {
//         local_pSurfaceInfo = (VkPhysicalDeviceSurfaceInfo2KHR*)malloc(sizeof(VkPhysicalDeviceSurfaceInfo2KHR));
//         if (!local_pSurfaceInfo) return VK_ERROR_OUT_OF_HOST_MEMORY;
//         deepcopy_VkPhysicalDeviceSurfaceInfo2KHR(&vkAllocator, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR,
//                                                   pSurfaceInfo, local_pSurfaceInfo);
//     }
    
//     size_t count = 0;
//     size_t* countPtr = &count;
//     count_VkPhysicalDeviceSurfaceInfo2KHR(0, VK_STRUCTURE_TYPE_MAX_ENUM, local_pSurfaceInfo, countPtr);
//     count += 12;
    
//     char* send_buffer = (char*)mgr.addExternalParamPtr(count);
//     uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;
    
//     uint64_t guest_phys_dev = (uint64_t)(uintptr_t)physicalDevice;
//     memcpy(*send_buffer_ptr, &guest_phys_dev, 8);
//     *send_buffer_ptr += 8;
    
//     encode_to_stream_VkPhysicalDeviceSurfaceInfo2KHR(VK_STRUCTURE_TYPE_MAX_ENUM, 
//                                                       local_pSurfaceInfo, send_buffer_ptr);
    
//     uint32_t format_count = *pSurfaceFormatCount;
//     memcpy(*send_buffer_ptr, &format_count, 4);
//     *send_buffer_ptr += 4;
    
//     mgr.addPtr(pSurfaceFormatCount, sizeof(uint32_t));
    
//     if (pSurfaceFormats && *pSurfaceFormatCount > 0) {
//         mgr.addPtr(pSurfaceFormats, sizeof(VkSurfaceFormat2KHR) * (*pSurfaceFormatCount));
//     }
    
//     VkResult vkResult = VK_SUCCESS;
//     mgr.addPtr(&vkResult, sizeof(VkResult));
//     FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID,
//               FUNID_vkGetPhysicalDeviceSurfaceFormats2KHR, true);
    
//     if (local_pSurfaceInfo) free(local_pSurfaceInfo);
//     return vkResult;
// }

// VKAPI_ATTR VkResult GetPhysicalDeviceSurfaceCapabilitiesKHR(
//     VkPhysicalDevice physicalDevice,
//     VkSurfaceKHR surface,
//     VkSurfaceCapabilitiesKHR* pSurfaceCapabilities) {
    
//     int express_gpu = get_express_gpu_fd();
//     thread_local ParamManager mgr;
    
//     uint64_t guest_phys_dev = (uint64_t)(uintptr_t)physicalDevice;
//     uint64_t guest_surface = (uint64_t)(uintptr_t)surface;
    
//     mgr.addParam64(guest_phys_dev);
//     mgr.addParam64(guest_surface);
//     mgr.addPtr(pSurfaceCapabilities, sizeof(VkSurfaceCapabilitiesKHR));
    
//     VkResult vkResult = VK_SUCCESS;
//     FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID,
//               FUNID_vkGetPhysicalDeviceSurfaceCapabilitiesKHR, true);
    
//     return vkResult;
// }

// VKAPI_ATTR VkResult GetPhysicalDeviceSurfaceCapabilities2KHR(
//     VkPhysicalDevice physicalDevice,
//     const VkPhysicalDeviceSurfaceInfo2KHR* pSurfaceInfo,
//     VkSurfaceCapabilities2KHR* pSurfaceCapabilities) {
    
//     int express_gpu = get_express_gpu_fd();
//     thread_local ParamManager mgr;
//     Allocator vkAllocator;
    
//     VkPhysicalDeviceSurfaceInfo2KHR* local_pSurfaceInfo = nullptr;
//     if (pSurfaceInfo) {
//         local_pSurfaceInfo = (VkPhysicalDeviceSurfaceInfo2KHR*)malloc(sizeof(VkPhysicalDeviceSurfaceInfo2KHR));
//         if (!local_pSurfaceInfo) return VK_ERROR_OUT_OF_HOST_MEMORY;
//         deepcopy_VkPhysicalDeviceSurfaceInfo2KHR(&vkAllocator, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR, 
//                                                   pSurfaceInfo, local_pSurfaceInfo);
//     }
    
//     size_t count = 0;
//     size_t* countPtr = &count;
//     count_VkPhysicalDeviceSurfaceInfo2KHR(0, VK_STRUCTURE_TYPE_MAX_ENUM, local_pSurfaceInfo, countPtr);
//     count += 8;
    
//     char* send_buffer = (char*)mgr.addExternalParamPtr(count);
//     uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;
    
//     uint64_t guest_phys_dev = (uint64_t)(uintptr_t)physicalDevice;
//     memcpy(*send_buffer_ptr, &guest_phys_dev, 8);
//     *send_buffer_ptr += 8;
    
//     encode_to_stream_VkPhysicalDeviceSurfaceInfo2KHR(VK_STRUCTURE_TYPE_MAX_ENUM, 
//                                                       local_pSurfaceInfo, send_buffer_ptr);
    
//     mgr.addPtr(pSurfaceCapabilities, sizeof(VkSurfaceCapabilities2KHR));
    
//     VkResult vkResult = VK_SUCCESS;
//     FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID,
//               FUNID_vkGetPhysicalDeviceSurfaceCapabilities2KHR, true);
    
//     if (local_pSurfaceInfo) free(local_pSurfaceInfo);
//     return vkResult;
// }

// VKAPI_ATTR VkResult GetPhysicalDeviceSurfacePresentModesKHR(
//     VkPhysicalDevice physicalDevice,
//     VkSurfaceKHR surface,
//     uint32_t* pPresentModeCount,
//     VkPresentModeKHR* pPresentModes) {
    
//     int express_gpu = get_express_gpu_fd();
//     thread_local ParamManager mgr;
    
//     uint64_t guest_phys_dev = (uint64_t)(uintptr_t)physicalDevice;
//     uint64_t guest_surface = (uint64_t)(uintptr_t)surface;
    
//     mgr.addParam64(guest_phys_dev);
//     mgr.addParam64(guest_surface);
//     mgr.addPtr(pPresentModeCount, sizeof(uint32_t));
    
//     if (pPresentModes && *pPresentModeCount > 0) {
//         mgr.addPtr(pPresentModes, sizeof(VkPresentModeKHR) * (*pPresentModeCount));
//     }
    
//     VkResult vkResult = VK_SUCCESS;
//     FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID,
//               FUNID_vkGetPhysicalDeviceSurfacePresentModesKHR, true);
    
//     return vkResult;
// }

// VKAPI_ATTR VkResult GetPhysicalDeviceSurfaceSupportKHR(
//     VkPhysicalDevice physicalDevice,
//     uint32_t queueFamilyIndex,
//     VkSurfaceKHR surface,
//     VkBool32* pSupported) {
    
//     int express_gpu = get_express_gpu_fd();
//     thread_local ParamManager mgr;
    
//     uint64_t guest_phys_dev = (uint64_t)(uintptr_t)physicalDevice;
//     uint64_t guest_surface = (uint64_t)(uintptr_t)surface;
    
//     mgr.addParam64(guest_phys_dev);
//     mgr.addParam32(queueFamilyIndex);
//     mgr.addParam64(guest_surface);
//     mgr.addPtr(pSupported, sizeof(VkBool32));
    
//     VkResult vkResult = VK_SUCCESS;
//     FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID,
//               FUNID_vkGetPhysicalDeviceSurfaceSupportKHR, true);
    
//     return vkResult;
// }

// VKAPI_ATTR VkResult GetPhysicalDeviceDisplayPropertiesKHR(
//     VkPhysicalDevice physicalDevice,
//     uint32_t* pPropertyCount,
//     VkDisplayPropertiesKHR* pProperties) {
//     LOGI("vkGetPhysicalDeviceDisplayPropertiesKHR called");
    
//     int express_gpu = get_express_gpu_fd();
//     thread_local ParamManager mgr;
    
//     uint64_t guest_phys_dev = (uint64_t)(uintptr_t)physicalDevice;
    
//     const VkAllocationCallbacks* alloc = &kDefaultAllocCallbacks;
//     if (pProperties && *pPropertyCount > 0) {
//         for (uint32_t i = 0; i < *pPropertyCount; ++i) {
//             VkDisplayKHR_T* display = static_cast<VkDisplayKHR_T*>(alloc->pfnAllocation(
//                 alloc->pUserData,
//                 sizeof(VkDisplayKHR_T),
//                 alignof(VkDisplayKHR_T),
//                 VK_SYSTEM_ALLOCATION_SCOPE_OBJECT));
//             pProperties[i].display = (VkDisplayKHR)display;
//         }
//     }
    
//     mgr.addParam64(guest_phys_dev);
//     mgr.addPtr(pPropertyCount, sizeof(uint32_t));
    
//     if (pProperties && *pPropertyCount > 0) {
//         uint64_t* guest_displays = (uint64_t*)malloc(*pPropertyCount * sizeof(uint64_t));
//         for (uint32_t i = 0; i < *pPropertyCount; ++i) {
//             guest_displays[i] = (uint64_t)(uintptr_t)pProperties[i].display;
//         }
//         mgr.addPtr(guest_displays, *pPropertyCount * sizeof(uint64_t));
//         mgr.addPtr(pProperties, *pPropertyCount * sizeof(VkDisplayPropertiesKHR));
//         free(guest_displays);
//     }
    
//     VkResult vkResult = VK_SUCCESS;
//     FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID,
//               FUNID_vkGetPhysicalDeviceDisplayPropertiesKHR, true);
    
//     return vkResult;
// }

// VKAPI_ATTR VkResult GetPhysicalDeviceDisplayProperties2KHR(
//     VkPhysicalDevice physicalDevice,
//     uint32_t* pPropertyCount,
//     VkDisplayProperties2KHR* pProperties) {
    
//     int express_gpu = get_express_gpu_fd();
//     thread_local ParamManager mgr;
    
//     uint64_t guest_phys_dev = (uint64_t)(uintptr_t)physicalDevice;
    
//     const VkAllocationCallbacks* alloc = &kDefaultAllocCallbacks;
//     if (pProperties && *pPropertyCount > 0) {
//         for (uint32_t i = 0; i < *pPropertyCount; ++i) {
//             pProperties[i].sType = VK_STRUCTURE_TYPE_DISPLAY_PROPERTIES_2_KHR;
//             pProperties[i].pNext = NULL;
            
//             VkDisplayKHR_T* display = static_cast<VkDisplayKHR_T*>(alloc->pfnAllocation(
//                 alloc->pUserData,
//                 sizeof(VkDisplayKHR_T),
//                 alignof(VkDisplayKHR_T),
//                 VK_SYSTEM_ALLOCATION_SCOPE_OBJECT));
//             pProperties[i].displayProperties.display = (VkDisplayKHR)display;
//         }
//     }
    
//     mgr.addParam64(guest_phys_dev);
//     mgr.addPtr(pPropertyCount, sizeof(uint32_t));
    
//     if (pProperties && *pPropertyCount > 0) {
//         uint64_t* guest_displays = (uint64_t*)malloc(*pPropertyCount * sizeof(uint64_t));
//         for (uint32_t i = 0; i < *pPropertyCount; ++i) {
//             guest_displays[i] = (uint64_t)(uintptr_t)pProperties[i].displayProperties.display;
//         }
//         mgr.addPtr(guest_displays, *pPropertyCount * sizeof(uint64_t));
//         mgr.addPtr(pProperties, *pPropertyCount * sizeof(VkDisplayProperties2KHR));
//         free(guest_displays);
//     }
    
//     VkResult vkResult = VK_SUCCESS;
//     FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID,
//               FUNID_vkGetPhysicalDeviceDisplayProperties2KHR, true);
    
//     return vkResult;
// }

// VKAPI_ATTR VkResult GetPhysicalDeviceDisplayPlanePropertiesKHR(
//     VkPhysicalDevice physicalDevice,
//     uint32_t* pPropertyCount,
//     VkDisplayPlanePropertiesKHR* pProperties) {
    
//     int express_gpu = get_express_gpu_fd();
//     thread_local ParamManager mgr;
    
//     uint64_t guest_phys_dev = (uint64_t)(uintptr_t)physicalDevice;
    
//     const VkAllocationCallbacks* alloc = &kDefaultAllocCallbacks;
//     if (pProperties && *pPropertyCount > 0) {
//         for (uint32_t i = 0; i < *pPropertyCount; ++i) {
//             VkDisplayKHR_T* display = static_cast<VkDisplayKHR_T*>(alloc->pfnAllocation(
//                 alloc->pUserData,
//                 sizeof(VkDisplayKHR_T),
//                 alignof(VkDisplayKHR_T),
//                 VK_SYSTEM_ALLOCATION_SCOPE_OBJECT));
//             pProperties[i].currentDisplay = (VkDisplayKHR)display;
//         }
//     }
    
//     mgr.addParam64(guest_phys_dev);
//     mgr.addPtr(pPropertyCount, sizeof(uint32_t));
    
//     if (pProperties && *pPropertyCount > 0) {
//         uint64_t* guest_displays = (uint64_t*)malloc(*pPropertyCount * sizeof(uint64_t));
//         for (uint32_t i = 0; i < *pPropertyCount; ++i) {
//             guest_displays[i] = (uint64_t)(uintptr_t)pProperties[i].currentDisplay;
//         }
//         mgr.addPtr(guest_displays, *pPropertyCount * sizeof(uint64_t));
//         mgr.addPtr(pProperties, *pPropertyCount * sizeof(VkDisplayPlanePropertiesKHR));
//         free(guest_displays);
//     }
    
//     VkResult vkResult = VK_SUCCESS;
//     FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID,
//               FUNID_vkGetPhysicalDeviceDisplayPlanePropertiesKHR, true);
    
//     return vkResult;
// }

// VKAPI_ATTR VkResult GetPhysicalDeviceDisplayPlaneProperties2KHR(
//     VkPhysicalDevice physicalDevice,
//     uint32_t* pPropertyCount,
//     VkDisplayPlaneProperties2KHR* pProperties) {
    
//     int express_gpu = get_express_gpu_fd();
//     thread_local ParamManager mgr;
    
//     uint64_t guest_phys_dev = (uint64_t)(uintptr_t)physicalDevice;
    
//     const VkAllocationCallbacks* alloc = &kDefaultAllocCallbacks;
//     if (pProperties && *pPropertyCount > 0) {
//         for (uint32_t i = 0; i < *pPropertyCount; ++i) {
//             pProperties[i].sType = VK_STRUCTURE_TYPE_DISPLAY_PLANE_PROPERTIES_2_KHR;
//             pProperties[i].pNext = NULL;
            
//             VkDisplayKHR_T* display = static_cast<VkDisplayKHR_T*>(alloc->pfnAllocation(
//                 alloc->pUserData,
//                 sizeof(VkDisplayKHR_T),
//                 alignof(VkDisplayKHR_T),
//                 VK_SYSTEM_ALLOCATION_SCOPE_OBJECT));
//             pProperties[i].displayPlaneProperties.currentDisplay = (VkDisplayKHR)display;
//         }
//     }
    
//     mgr.addParam64(guest_phys_dev);
//     mgr.addPtr(pPropertyCount, sizeof(uint32_t));
    
//     if (pProperties && *pPropertyCount > 0) {
//         uint64_t* guest_displays = (uint64_t*)malloc(*pPropertyCount * sizeof(uint64_t));
//         for (uint32_t i = 0; i < *pPropertyCount; ++i) {
//             guest_displays[i] = (uint64_t)(uintptr_t)pProperties[i].displayPlaneProperties.currentDisplay;
//         }
//         mgr.addPtr(guest_displays, *pPropertyCount * sizeof(uint64_t));
//         mgr.addPtr(pProperties, *pPropertyCount * sizeof(VkDisplayPlaneProperties2KHR));
//         free(guest_displays);
//     }
    
//     VkResult vkResult = VK_SUCCESS;
//     FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID,
//               FUNID_vkGetPhysicalDeviceDisplayPlaneProperties2KHR, true);
    
//     return vkResult;
// }

// VKAPI_ATTR VkResult GetDisplayModePropertiesKHR(
//     VkPhysicalDevice physicalDevice,
//     VkDisplayKHR display,
//     uint32_t* pPropertyCount,
//     VkDisplayModePropertiesKHR* pProperties) {
    
//     int express_gpu = get_express_gpu_fd();
//     thread_local ParamManager mgr;
    
//     uint64_t guest_phys_dev = (uint64_t)(uintptr_t)physicalDevice;
//     uint64_t guest_display = (uint64_t)(uintptr_t)display;
    
//     const VkAllocationCallbacks* alloc = &kDefaultAllocCallbacks;
//     if (pProperties && *pPropertyCount > 0) {
//         for (uint32_t i = 0; i < *pPropertyCount; ++i) {
//             VkDisplayModeKHR_T* mode = static_cast<VkDisplayModeKHR_T*>(alloc->pfnAllocation(
//                 alloc->pUserData,
//                 sizeof(VkDisplayModeKHR_T),
//                 alignof(VkDisplayModeKHR_T),
//                 VK_SYSTEM_ALLOCATION_SCOPE_OBJECT));
//             pProperties[i].displayMode = (VkDisplayModeKHR)mode;
//         }
//     }
    
//     mgr.addParam64(guest_phys_dev);
//     mgr.addParam64(guest_display);
//     mgr.addPtr(pPropertyCount, sizeof(uint32_t));
    
//     if (pProperties && *pPropertyCount > 0) {
//         uint64_t* guest_modes = (uint64_t*)malloc(*pPropertyCount * sizeof(uint64_t));
//         for (uint32_t i = 0; i < *pPropertyCount; ++i) {
//             guest_modes[i] = (uint64_t)(uintptr_t)pProperties[i].displayMode;
//         }
//         mgr.addPtr(guest_modes, *pPropertyCount * sizeof(uint64_t));
//         mgr.addPtr(pProperties, *pPropertyCount * sizeof(VkDisplayModePropertiesKHR));
//         free(guest_modes);
//     }
    
//     VkResult vkResult = VK_SUCCESS;
//     mgr.addPtr(&vkResult, sizeof(VkResult));
//     FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID,
//               FUNID_vkGetDisplayModePropertiesKHR, true);
    
//     return vkResult;
// }

// VKAPI_ATTR VkResult GetDisplayModeProperties2KHR(
//     VkPhysicalDevice physicalDevice,
//     VkDisplayKHR display,
//     uint32_t* pPropertyCount,
//     VkDisplayModeProperties2KHR* pProperties) {
    
//     int express_gpu = get_express_gpu_fd();
//     thread_local ParamManager mgr;
    
//     uint64_t guest_phys_dev = (uint64_t)(uintptr_t)physicalDevice;
//     uint64_t guest_display = (uint64_t)(uintptr_t)display;
    
//     const VkAllocationCallbacks* alloc = &kDefaultAllocCallbacks;
//     if (pProperties && *pPropertyCount > 0) {
//         for (uint32_t i = 0; i < *pPropertyCount; ++i) {
//             pProperties[i].sType = VK_STRUCTURE_TYPE_DISPLAY_MODE_PROPERTIES_2_KHR;
//             pProperties[i].pNext = NULL;
            
//             VkDisplayModeKHR_T* mode = static_cast<VkDisplayModeKHR_T*>(alloc->pfnAllocation(
//                 alloc->pUserData,
//                 sizeof(VkDisplayModeKHR_T),
//                 alignof(VkDisplayModeKHR_T),
//                 VK_SYSTEM_ALLOCATION_SCOPE_OBJECT));
//             pProperties[i].displayModeProperties.displayMode = (VkDisplayModeKHR)mode;
//         }
//     }
    
//     mgr.addParam64(guest_phys_dev);
//     mgr.addParam64(guest_display);
//     mgr.addPtr(pPropertyCount, sizeof(uint32_t));
    
//     if (pProperties && *pPropertyCount > 0) {
//         uint64_t* guest_modes = (uint64_t*)malloc(*pPropertyCount * sizeof(uint64_t));
//         for (uint32_t i = 0; i < *pPropertyCount; ++i) {
//             guest_modes[i] = (uint64_t)(uintptr_t)pProperties[i].displayModeProperties.displayMode;
//         }
//         mgr.addPtr(guest_modes, *pPropertyCount * sizeof(uint64_t));
//         mgr.addPtr(pProperties, *pPropertyCount * sizeof(VkDisplayModeProperties2KHR));
//         free(guest_modes);
//     }
    
//     VkResult vkResult = VK_SUCCESS;
//     mgr.addPtr(&vkResult, sizeof(VkResult));
//     FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID,
//               FUNID_vkGetDisplayModeProperties2KHR, true);
    
//     return vkResult;
// }

// VKAPI_ATTR VkResult GetPhysicalDevicePresentRectanglesKHR(
//     VkPhysicalDevice physicalDevice,
//     VkSurfaceKHR surface,
//     uint32_t* pRectCount,
//     VkRect2D* pRects) {
    
//     int express_gpu = get_express_gpu_fd();
//     thread_local ParamManager mgr;
    
//     uint64_t guest_phys_dev = (uint64_t)(uintptr_t)physicalDevice;
//     uint64_t guest_surface = (uint64_t)(uintptr_t)surface;
//     uint32_t rect_count = *pRectCount;
    
//     size_t count = 8 + 8 + 4;
//     char* send_buffer = (char*)mgr.addExternalParamPtr(count);
//     uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;
    
//     memcpy(*send_buffer_ptr, &guest_phys_dev, 8);
//     *send_buffer_ptr += 8;
//     memcpy(*send_buffer_ptr, &guest_surface, 8);
//     *send_buffer_ptr += 8;
//     memcpy(*send_buffer_ptr, &rect_count, 4);
//     *send_buffer_ptr += 4;
    
//     mgr.addPtr(pRectCount, sizeof(uint32_t));
    
//     if (pRects && rect_count > 0) {
//         mgr.addPtr(pRects, sizeof(VkRect2D) * rect_count);
//     }
    
//     VkResult vkResult = VK_SUCCESS;
    
//     FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID,
//               FUNID_vkGetPhysicalDevicePresentRectanglesKHR, true);
    
//     return vkResult;
// }

// VKAPI_ATTR VkResult CreateDisplayModeKHR(
//     VkPhysicalDevice physicalDevice,
//     VkDisplayKHR display,
//     const VkDisplayModeCreateInfoKHR* pCreateInfo,
//     const VkAllocationCallbacks* pAllocator,
//     VkDisplayModeKHR* pMode) {
    
//     int express_gpu = get_express_gpu_fd();
//     thread_local ParamManager mgr;
//     Allocator vkAllocator;
    
//     VkDisplayModeCreateInfoKHR* local_pCreateInfo = nullptr;
//     if (pCreateInfo) {
//         local_pCreateInfo = (VkDisplayModeCreateInfoKHR*)malloc(sizeof(VkDisplayModeCreateInfoKHR));
//         if (!local_pCreateInfo) return VK_ERROR_OUT_OF_HOST_MEMORY;
//         deepcopy_VkDisplayModeCreateInfoKHR(&vkAllocator, VK_STRUCTURE_TYPE_DISPLAY_MODE_CREATE_INFO_KHR, pCreateInfo, local_pCreateInfo);
//     }
    
//     VkAllocationCallbacks* local_pAllocator = nullptr;
//     if (pAllocator) {
//         local_pAllocator = (VkAllocationCallbacks*)malloc(sizeof(VkAllocationCallbacks));
//         if (!local_pAllocator) {
//             if (local_pCreateInfo) free(local_pCreateInfo);
//             return VK_ERROR_OUT_OF_HOST_MEMORY;
//         }
//         deepcopy_VkAllocationCallbacks(&vkAllocator, VK_STRUCTURE_TYPE_MAX_ENUM, pAllocator, local_pAllocator);
//     }
    
//     size_t count = 0;
//     size_t* countPtr = &count;
//     count += sizeof(uint64_t) * 2;
//     if (pCreateInfo) {
//         count_VkDisplayModeCreateInfoKHR(0, VK_STRUCTURE_TYPE_MAX_ENUM, local_pCreateInfo, countPtr);
//     }
//     count += 8;
//     if (pAllocator) {
//         count_VkAllocationCallbacks(0, VK_STRUCTURE_TYPE_MAX_ENUM, local_pAllocator, countPtr);
//     }
//     count += sizeof(uint64_t);
    
//     char* send_buffer = (char*)mgr.addExternalParamPtr(count);
//     uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;
    
//     uint64_t guest_physicalDevice = (uint64_t)(uintptr_t)physicalDevice;
//     memcpy(*send_buffer_ptr, &guest_physicalDevice, sizeof(uint64_t));
//     *send_buffer_ptr += sizeof(uint64_t);
    
//     uint64_t guest_display = (uint64_t)(uintptr_t)display;
//     memcpy(*send_buffer_ptr, &guest_display, sizeof(uint64_t));
//     *send_buffer_ptr += sizeof(uint64_t);
    
//     if (pCreateInfo) {
//         encode_to_stream_VkDisplayModeCreateInfoKHR(VK_STRUCTURE_TYPE_MAX_ENUM, local_pCreateInfo, send_buffer_ptr);
//     }
    
//     uint64_t cgen_var_allocator = (uint64_t)(uintptr_t)local_pAllocator;
//     memcpy(*send_buffer_ptr, &cgen_var_allocator, 8);
//     *send_buffer_ptr += 8;
    
//     if (pAllocator) {
//         encode_to_stream_VkAllocationCallbacks(VK_STRUCTURE_TYPE_MAX_ENUM, local_pAllocator, send_buffer_ptr);
//     }
    
//     uint64_t guest_mode = (uint64_t)(uintptr_t)pMode;
//     memcpy(*send_buffer_ptr, &guest_mode, sizeof(uint64_t));
//     *send_buffer_ptr += sizeof(uint64_t);
    
//     VkResult vkResult = VK_SUCCESS;
//     mgr.addPtr(&vkResult, sizeof(VkResult));
//     FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCreateDisplayModeKHR, false);
    
//     if (local_pCreateInfo) free(local_pCreateInfo);
//     if (local_pAllocator) free(local_pAllocator);
    
//     return vkResult;
// }

// VKAPI_ATTR VkResult CreateDisplayPlaneSurfaceKHR(
//     VkInstance instance,
//     const VkDisplaySurfaceCreateInfoKHR* pCreateInfo,
//     const VkAllocationCallbacks* pAllocator,
//     VkSurfaceKHR* pSurface) {
    
//     int express_gpu = get_express_gpu_fd();
//     thread_local ParamManager mgr;
//     Allocator vkAllocator;
    
//     VkDisplaySurfaceCreateInfoKHR* local_pCreateInfo = nullptr;
//     if (pCreateInfo) {
//         local_pCreateInfo = (VkDisplaySurfaceCreateInfoKHR*)malloc(sizeof(VkDisplaySurfaceCreateInfoKHR));
//         if (!local_pCreateInfo) return VK_ERROR_OUT_OF_HOST_MEMORY;
//         deepcopy_VkDisplaySurfaceCreateInfoKHR(&vkAllocator, VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR, pCreateInfo, local_pCreateInfo);
//     }
    
//     VkAllocationCallbacks* local_pAllocator = nullptr;
//     if (pAllocator) {
//         local_pAllocator = (VkAllocationCallbacks*)malloc(sizeof(VkAllocationCallbacks));
//         if (!local_pAllocator) {
//             if (local_pCreateInfo) free(local_pCreateInfo);
//             return VK_ERROR_OUT_OF_HOST_MEMORY;
//         }
//         deepcopy_VkAllocationCallbacks(&vkAllocator, VK_STRUCTURE_TYPE_MAX_ENUM, pAllocator, local_pAllocator);
//     }
    
//     size_t count = 0;
//     size_t* countPtr = &count;
//     count += sizeof(uint64_t);
//     if (pCreateInfo) {
//         count_VkDisplaySurfaceCreateInfoKHR(0, VK_STRUCTURE_TYPE_MAX_ENUM, local_pCreateInfo, countPtr);
//     }
//     count += 8;
//     if (pAllocator) {
//         count_VkAllocationCallbacks(0, VK_STRUCTURE_TYPE_MAX_ENUM, local_pAllocator, countPtr);
//     }
//     count += sizeof(uint64_t);
    
//     char* send_buffer = (char*)mgr.addExternalParamPtr(count);
//     uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;
    
//     uint64_t guest_instance = (uint64_t)(uintptr_t)instance;
//     memcpy(*send_buffer_ptr, &guest_instance, sizeof(uint64_t));
//     *send_buffer_ptr += sizeof(uint64_t);
    
//     if (pCreateInfo) {
//         encode_to_stream_VkDisplaySurfaceCreateInfoKHR(VK_STRUCTURE_TYPE_MAX_ENUM, local_pCreateInfo, send_buffer_ptr);
//     }
    
//     uint64_t cgen_var_allocator = (uint64_t)(uintptr_t)local_pAllocator;
//     memcpy(*send_buffer_ptr, &cgen_var_allocator, 8);
//     *send_buffer_ptr += 8;
    
//     if (pAllocator) {
//         encode_to_stream_VkAllocationCallbacks(VK_STRUCTURE_TYPE_MAX_ENUM, local_pAllocator, send_buffer_ptr);
//     }
    
//     uint64_t guest_surface = (uint64_t)(uintptr_t)pSurface;
//     memcpy(*send_buffer_ptr, &guest_surface, sizeof(uint64_t));
//     *send_buffer_ptr += sizeof(uint64_t);
    
//     VkResult vkResult = VK_SUCCESS;
//     mgr.addPtr(&vkResult, sizeof(VkResult));
//     FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCreateDisplayPlaneSurfaceKHR, false);
    
//     if (local_pCreateInfo) free(local_pCreateInfo);
//     if (local_pAllocator) free(local_pAllocator);
    
//     return vkResult;
// }

// VKAPI_ATTR VkResult GetDeviceGroupPresentCapabilitiesKHR(
//     VkDevice device,
//     VkDeviceGroupPresentCapabilitiesKHR* pDeviceGroupPresentCapabilities) {
    
//     int express_gpu = get_express_gpu_fd();
//     thread_local ParamManager mgr;
    
//     uint64_t guest_device = (uint64_t)(uintptr_t)device;
//     mgr.addParam64(guest_device);
//     mgr.addPtr(pDeviceGroupPresentCapabilities, sizeof(VkDeviceGroupPresentCapabilitiesKHR));
    
//     VkResult vkResult = VK_SUCCESS;
//     mgr.addPtr(&vkResult, sizeof(VkResult));
//     FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkGetDeviceGroupPresentCapabilitiesKHR, true);
    
//     return vkResult;
// }

// VKAPI_ATTR VkResult GetDeviceGroupSurfacePresentModesKHR(
//     VkDevice device,
//     VkSurfaceKHR surface,
//     VkDeviceGroupPresentModeFlagsKHR* pModes) {
    
//     int express_gpu = get_express_gpu_fd();
//     thread_local ParamManager mgr;
    
//     uint64_t guest_device = (uint64_t)(uintptr_t)device;
//     uint64_t guest_surface = (uint64_t)(uintptr_t)surface;
    
//     mgr.addParam64(guest_device);
//     mgr.addParam64(guest_surface);
//     mgr.addPtr(pModes, sizeof(VkDeviceGroupPresentModeFlagsKHR));
    
//     VkResult vkResult = VK_SUCCESS;
//     mgr.addPtr(&vkResult, sizeof(VkResult));
//     FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkGetDeviceGroupSurfacePresentModesKHR, true);
    
//     return vkResult;
// }

// VKAPI_ATTR VkResult GetDisplayPlaneCapabilities2KHR(
//     VkPhysicalDevice physicalDevice,
//     const VkDisplayPlaneInfo2KHR* pDisplayPlaneInfo,
//     VkDisplayPlaneCapabilities2KHR* pCapabilities) {
    
//     int express_gpu = get_express_gpu_fd();
//     thread_local ParamManager mgr;
//     Allocator vkAllocator;
    
//     VkDisplayPlaneInfo2KHR* local_pDisplayPlaneInfo = nullptr;
//     if (pDisplayPlaneInfo) {
//         local_pDisplayPlaneInfo = (VkDisplayPlaneInfo2KHR*)malloc(sizeof(VkDisplayPlaneInfo2KHR));
//         if (!local_pDisplayPlaneInfo) return VK_ERROR_OUT_OF_HOST_MEMORY;
//         deepcopy_VkDisplayPlaneInfo2KHR(&vkAllocator, VK_STRUCTURE_TYPE_DISPLAY_PLANE_INFO_2_KHR, pDisplayPlaneInfo, local_pDisplayPlaneInfo);
//     }
    
//     size_t count = 0;
//     size_t* countPtr = &count;
//     count += sizeof(uint64_t);
//     if (pDisplayPlaneInfo) {
//         count_VkDisplayPlaneInfo2KHR(0, VK_STRUCTURE_TYPE_MAX_ENUM, local_pDisplayPlaneInfo, countPtr);
//     }
    
//     char* send_buffer = (char*)mgr.addExternalParamPtr(count);
//     uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;
    
//     uint64_t guest_physicalDevice = (uint64_t)(uintptr_t)physicalDevice;
//     memcpy(*send_buffer_ptr, &guest_physicalDevice, sizeof(uint64_t));
//     *send_buffer_ptr += sizeof(uint64_t);
    
//     if (pDisplayPlaneInfo) {
//         encode_to_stream_VkDisplayPlaneInfo2KHR(VK_STRUCTURE_TYPE_MAX_ENUM, local_pDisplayPlaneInfo, send_buffer_ptr);
//     }
    
//     mgr.addPtr(pCapabilities, sizeof(VkDisplayPlaneCapabilities2KHR));
    
//     VkResult vkResult = VK_SUCCESS;
//     mgr.addPtr(&vkResult, sizeof(VkResult));
//     FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkGetDisplayPlaneCapabilities2KHR, true);
    
//     if (local_pDisplayPlaneInfo) free(local_pDisplayPlaneInfo);
    
//     return vkResult;
// }

// VKAPI_ATTR VkResult GetDisplayPlaneCapabilitiesKHR(
//     VkPhysicalDevice physicalDevice,
//     VkDisplayModeKHR mode,
//     uint32_t planeIndex,
//     VkDisplayPlaneCapabilitiesKHR* pCapabilities) {
    
//     int express_gpu = get_express_gpu_fd();
//     thread_local ParamManager mgr;
    
//     uint64_t guest_physicalDevice = (uint64_t)(uintptr_t)physicalDevice;
//     uint64_t guest_mode = (uint64_t)(uintptr_t)mode;
    
//     mgr.addParam64(guest_physicalDevice);
//     mgr.addParam64(guest_mode);
//     mgr.addParam32(planeIndex);
//     mgr.addPtr(pCapabilities, sizeof(VkDisplayPlaneCapabilitiesKHR));
    
//     VkResult vkResult = VK_SUCCESS;
//     mgr.addPtr(&vkResult, sizeof(VkResult));
//     FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkGetDisplayPlaneCapabilitiesKHR, true);
    
//     return vkResult;
// }

// VKAPI_ATTR VkResult GetDisplayPlaneSupportedDisplaysKHR(
//     VkPhysicalDevice physicalDevice,
//     uint32_t planeIndex,
//     uint32_t* pDisplayCount,
//     VkDisplayKHR* pDisplays) {
    
//     int express_gpu = get_express_gpu_fd();
//     thread_local ParamManager mgr;
    
//     uint64_t guest_physicalDevice = (uint64_t)(uintptr_t)physicalDevice;
    
//     mgr.addParam64(guest_physicalDevice);
//     mgr.addParam32(planeIndex);
//     mgr.addPtr(pDisplayCount, sizeof(uint32_t));
    
//     if (pDisplays && *pDisplayCount > 0) {
//         mgr.addPtr(pDisplays, sizeof(VkDisplayKHR) * (*pDisplayCount));
//     }
    
//     VkResult vkResult = VK_SUCCESS;
//     mgr.addPtr(&vkResult, sizeof(VkResult));
//     FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkGetDisplayPlaneSupportedDisplaysKHR, true);
    
//     return vkResult;
// }

VKAPI_ATTR VkResult CreateSwapchainKHR(
    VkDevice                         device,
    const VkSwapchainCreateInfoKHR*  create_info,
    const VkAllocationCallbacks*     pAllocator,
    VkSwapchainKHR*                  pSwapchain,
    uint32_t                         pSwapchainImageCount)
{
    ALOGI("CreateSwapchainKHR with swapchain %lld", (long long)pSwapchain);

    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;


    uint64_t guestDevice      = (uint64_t)(uintptr_t)device;
    uint64_t guestSurface     = (uint64_t)(uintptr_t)create_info->surface;
    uint32_t minImageCount    = pSwapchainImageCount;
    uint32_t imageFormat      = create_info->imageFormat;
    uint32_t width            = create_info->imageExtent.width;
    uint32_t height           = create_info->imageExtent.height;
    uint32_t presentMode      = create_info->presentMode;

    mgr.addParam64(guestDevice);
    mgr.addParam64(guestSurface);
    mgr.addParam32(minImageCount);
    mgr.addParam32(imageFormat);
    mgr.addParam32(width);
    mgr.addParam32(height);
    mgr.addParam32(presentMode);
    mgr.addPtr(pSwapchain, sizeof(VkSwapchainKHR));

    FlimeGuestWrite(&mgr, express_gpu,
              EXPRESS_GPU_DEVICE_ID,
              FUNID_vkCreateSwapchainKHR,
              /*sync=*/false);

    ALOGI("CreateSwapchainKHR %lld",(long long)pSwapchain);

    return VK_SUCCESS;
}

VKAPI_ATTR VkResult GetSwapchainImagesKHR_special(
    VkDevice           device,
    VkSwapchainKHR     swapchain,
    uint32_t*          pSwapchainImageCount,
    VkImage*           pSwapchainImages,
    uint64_t*          pBufferHandles)
{
    ALOGI("GetSwapchainImagesKHR with swapchain %lld buffer %lld", (long long)swapchain, (long long)pBufferHandles[0]);
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;

    mgr.addParam64((uint64_t)(uintptr_t)device);
    mgr.addParam64((uint64_t)(uintptr_t)swapchain);
    mgr.addParam32(*pSwapchainImageCount);


    mgr.addPtr(pSwapchainImages,
               sizeof(VkImage) * (*pSwapchainImageCount));
    // mgr.addPtr(pBufferHandles,
    //            sizeof(uint64_t) * (*pSwapchainImageCount));
    uint64_t *gbuffer_ids = new uint64_t[*pSwapchainImageCount];
    int *bufferWidth = new int[*pSwapchainImageCount];
    int *bufferHeight = new int[*pSwapchainImageCount];
    int count = (int)*pSwapchainImageCount;

    for(int i = 0; i < count; i++) {
        ALOGI("GetSwapchainImagesKHR: image %d, handle %lld",
              i, (long long)pSwapchainImages[i]);

        Gralloc_Express_Handle *g_handle = gralloc_express_handle((buffer_handle_t)pBufferHandles[i]);
        if (g_handle) {
            gbuffer_ids[i] = g_handle->info.gbuffer_id;
            bufferWidth[i] = g_handle->info.width;
            bufferHeight[i] = g_handle->info.height;
        } else {
            gbuffer_ids[i] = 0;
            bufferWidth[i] = 0;
            bufferHeight[i] = 0;
            ALOGE("GetSwapchainImagesKHR: Failed to get Gralloc_Express_Handle for image %d", i);
        }
        ALOGI("GetSwapchainImagesKHR: handle %lld, width %d, height %d",
              (long long)gbuffer_ids[i], bufferWidth[i], bufferHeight[i]);
    }

    mgr.addPtr(gbuffer_ids, sizeof(uint64_t) * (*pSwapchainImageCount));
    mgr.addPtr(bufferWidth, sizeof(uint32_t) * (*pSwapchainImageCount));
    mgr.addPtr(bufferHeight, sizeof(uint32_t) * (*pSwapchainImageCount));

    FlimeGuestWrite(&mgr,
        express_gpu,
        EXPRESS_GPU_DEVICE_ID,
        FUNID_vkGetSwapchainImagesKHR,
        false);
    ALOGI("guest GetSwapchainImagesKHR %lld %lld", (long long)swapchain, (long long)pSwapchainImages);
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult AcquireNextImageKHR_special(VkDevice device,
                                           VkSwapchainKHR swapchain,
                                           uint64_t timeout,
                                           VkSemaphore semaphore,
                                           VkFence fence,
                                           uint32_t* pImageIndex,
                                           uint64_t buffer_handle) {
    ALOGI("vkAcquireNextImageKHR with swapchain %lld handle %lld", (long long)swapchain, (long long)timeout);
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;

    Gralloc_Express_Handle *g_handle = gralloc_express_handle((buffer_handle_t)buffer_handle);
    ALOGI("get handle magic %lld, id %lld width %d height %d",
          (long long)g_handle->magic, (long long)g_handle->info.gbuffer_id, g_handle->info.width, g_handle->info.height);

    
    uint64_t guest_device = (uint64_t)(uintptr_t)device;
    uint64_t guest_swapchain = (uint64_t)(uintptr_t)swapchain;
    uint64_t guest_semaphore = (uint64_t)(uintptr_t)semaphore;
    uint64_t guest_fence = (uint64_t)(uintptr_t)fence;
    
    mgr.addParam64(guest_device);
    mgr.addParam64(guest_swapchain);
    mgr.addParam64(timeout);
    mgr.addParam64(guest_semaphore);
    mgr.addParam64(guest_fence);
    mgr.addParam64(buffer_handle);
    mgr.addParam32(*pImageIndex);
    
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkAcquireNextImageKHR, true);
    
    ALOGI("vkAcquireNextImageKHR device=%lld swapchain=%lld timeout=%lld index %d", 
          (long long)guest_device, (long long)guest_swapchain, (long long)timeout, *pImageIndex);
    
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult QueuePresentKHR_special(VkQueue queue,
                                   const VkPresentInfoKHR* pPresentInfo,
                                   uint64_t* pBufferHandles) {
    FlushPendingSubmitCohortForQueue(queue, "queue_present");

    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    Allocator vkAllocator;
    
    VkPresentInfoKHR* local_pPresentInfo = nullptr;
    if (pPresentInfo) {
        local_pPresentInfo = (VkPresentInfoKHR*)malloc(sizeof(VkPresentInfoKHR));
        if (!local_pPresentInfo) {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        deepcopy_VkPresentInfoKHR(&vkAllocator, VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, pPresentInfo, local_pPresentInfo);
    }
    
    size_t count = 0;
    size_t* countPtr = &count;
    count_VkPresentInfoKHR(0, VK_STRUCTURE_TYPE_MAX_ENUM, local_pPresentInfo, countPtr);
    count += 8; // for queue handle
    
    char* send_buffer = (char*)mgr.addExternalParamPtr(count);
    uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;
    
    uint64_t guest_queue = (uint64_t)(uintptr_t)queue;
    memcpy(*send_buffer_ptr, &guest_queue, 8);
    *send_buffer_ptr += 8;
    
    encode_to_stream_VkPresentInfoKHR(VK_STRUCTURE_TYPE_MAX_ENUM, local_pPresentInfo, send_buffer_ptr);
    
    ALOGI("QueuePresentKHR queue=%lld swapchainCount=%d", 
          (long long)queue, pPresentInfo->swapchainCount);
    
    uint64_t* buffer_ids = new uint64_t[pPresentInfo->swapchainCount];
    for (uint32_t i = 0; i < pPresentInfo->swapchainCount; i++) {
        Gralloc_Express_Handle* g_handle = gralloc_express_handle((buffer_handle_t)pBufferHandles[i]);
        if (g_handle) {
            buffer_ids[i] = g_handle->info.gbuffer_id;
            ALOGI("QueuePresentKHR: swapchain %d, handle %lld, gbuffer_id %lld",
                  i, (long long)pBufferHandles[i], (long long)buffer_ids[i]);
        } else {
            buffer_ids[i] = 0;
            ALOGE("QueuePresentKHR: Failed to get Gralloc_Express_Handle for swapchain %d", i);
        }
    }
    mgr.addPtr(buffer_ids, sizeof(uint64_t) * pPresentInfo->swapchainCount);
    
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkQueuePresentKHR, false);

    delete [] buffer_ids;
    if (local_pPresentInfo) free(local_pPresentInfo);
    return VK_SUCCESS;
}
// -----------------------------------------------------------------------------
// Instance


VKAPI_ATTR void VKAPI_CALL DestroyBuffer(
    VkDevice device,
    VkBuffer buffer,
    const VkAllocationCallbacks* pAllocator)
{
    if (kEnableDeferredFenceWait) {
        DrainDeferredQueues("destroy_buffer");
    }

    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;

    Allocator vkAllocator;

    VkAllocationCallbacks* local_pAllocator = nullptr;

    if (pAllocator) {
        local_pAllocator = (VkAllocationCallbacks*)malloc(sizeof(VkAllocationCallbacks));
        if (!local_pAllocator) {
            ALOGE("DestroyBuffer: Failed to allocate memory for VkAllocationCallbacks");
            return;
        }
        deepcopy_VkAllocationCallbacks(
            &vkAllocator,
            VK_STRUCTURE_TYPE_MAX_ENUM,
            pAllocator,
            local_pAllocator);
    }

    size_t byte_count = 0;
    if (pAllocator) {
        count_VkAllocationCallbacks(
            0,
            VK_STRUCTURE_TYPE_MAX_ENUM,
            local_pAllocator,
            &byte_count);
    }

    byte_count += 24; // 8 bytes for device, 8 bytes for buffer, 8 bytes for allocator pointer

    char* send_buffer = (char*)mgr.addExternalParamPtr(byte_count);
    uint8_t** stream_ptr = (uint8_t**)&send_buffer;

    uint64_t guest_device = (uint64_t)(uintptr_t)device;
    uint64_t guest_buffer = (uint64_t)(uintptr_t)buffer;

    memcpy(*stream_ptr, &guest_device, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);

    memcpy(*stream_ptr, &guest_buffer, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);

    uint64_t allocPtr = (uint64_t)(uintptr_t)pAllocator;
    memcpy(*stream_ptr, &allocPtr, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);
    if (pAllocator) {
        encode_to_stream_VkAllocationCallbacks(
            VK_STRUCTURE_TYPE_MAX_ENUM,
            local_pAllocator,
            stream_ptr);
    }

    // mgr.addParam64((uint64_t)(uintptr_t)device);
    // mgr.addParam64((uint64_t)(uintptr_t)buffer);

    FlimeGuestBeforeDescriptorLifecycle(device);
    if (!FlimeGuestPrepareDescriptorRetirement(device)) {
        mgr.clear();
        FlimeGuestAfterDescriptorLifecycle(device, false);
        if (local_pAllocator) {
            free(local_pAllocator);
        }
        ALOGE("FLIME could not drain descriptor records before "
              "vkDestroyBuffer; destroy was not sent");
        return;
    }
    FlimeGuestDestroyDescriptorPayload(
        device, VK_OBJECT_TYPE_BUFFER, guest_buffer);
    const ssize_t written = FlimeGuestWrite(&mgr,
        express_gpu,
        EXPRESS_GPU_DEVICE_ID,
        FUNID_vkDestroyBuffer,
        true);
    FlimeGuestAfterDescriptorLifecycle(
        device, IsCompleteParamManagerWrite(written, 1));

    ForgetTrackedBuffer(buffer);

    if (local_pAllocator) {
        free(local_pAllocator);
    }

    ALOGI("DestroyBuffer: Sent destroy request for buffer %lld", (long long)guest_buffer);
}

VKAPI_ATTR void VKAPI_CALL DestroyBufferView(
    VkDevice device,
    VkBufferView bufferView,
    const VkAllocationCallbacks* pAllocator)
{
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;

    Allocator vkAllocator;

    VkAllocationCallbacks* local_pAllocator = nullptr;
    if (pAllocator) {
        local_pAllocator = (VkAllocationCallbacks*)malloc(sizeof(VkAllocationCallbacks));
        if (!local_pAllocator) {
            ALOGE("DestroyBufferView: Failed to allocate memory for VkAllocationCallbacks");
            return;
        }
        deepcopy_VkAllocationCallbacks(
            &vkAllocator,
            VK_STRUCTURE_TYPE_MAX_ENUM,
            pAllocator,
            local_pAllocator);
    }

    size_t byte_count = 0;
    if (pAllocator) {
        count_VkAllocationCallbacks(
            0,
            VK_STRUCTURE_TYPE_MAX_ENUM,
            local_pAllocator,
            &byte_count);
    }
    byte_count += 24; // 8 bytes for device, 8 bytes for bufferView, 8 bytes for allocator pointer

    char* send_buffer = (char*)mgr.addExternalParamPtr(byte_count);
    uint8_t** stream_ptr = (uint8_t**)&send_buffer;

    uint64_t guest_device = (uint64_t)(uintptr_t)device;
    uint64_t guest_bufferView = (uint64_t)(uintptr_t)bufferView;

    memcpy(*stream_ptr, &guest_device, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);

    memcpy(*stream_ptr, &guest_bufferView, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);

    uint64_t allocPtr = (uint64_t)(uintptr_t)pAllocator;
    memcpy(*stream_ptr, &allocPtr, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);
    if (pAllocator) {
        encode_to_stream_VkAllocationCallbacks(
            VK_STRUCTURE_TYPE_MAX_ENUM,
            local_pAllocator,
            stream_ptr);
    }

    // mgr.addParam64((uint64_t)(uintptr_t)device);
    // mgr.addParam64((uint64_t)(uintptr_t)bufferView);

    FlimeGuestBeforeDescriptorLifecycle(device);
    if (!FlimeGuestPrepareDescriptorRetirement(device)) {
        mgr.clear();
        FlimeGuestAfterDescriptorLifecycle(device, false);
        if (local_pAllocator) {
            free(local_pAllocator);
        }
        ALOGE("FLIME could not drain descriptor records before "
              "vkDestroyBufferView; destroy was not sent");
        return;
    }
    FlimeGuestDestroyDescriptorPayload(
        device, VK_OBJECT_TYPE_BUFFER_VIEW, guest_bufferView);
    const ssize_t written = FlimeGuestWrite(&mgr,
        express_gpu,
        EXPRESS_GPU_DEVICE_ID,
        FUNID_vkDestroyBufferView,
        true);
    FlimeGuestAfterDescriptorLifecycle(
        device, IsCompleteParamManagerWrite(written, 1));

    if (local_pAllocator) {
        free(local_pAllocator);
    }

    ALOGI("DestroyBufferView: Sent destroy request for bufferView %lld", (long long)guest_bufferView);
}

VKAPI_ATTR void VKAPI_CALL DestroyCommandPool(
    VkDevice device,
    VkCommandPool commandPool,
    const VkAllocationCallbacks* pAllocator)
{
    FlushPendingSubmitCohort("destroy_command_pool");
    if (kEnableDeferredFenceWait) {
        DrainDeferredQueues("destroy_command_pool");
    }
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;

    Allocator vkAllocator;

    VkAllocationCallbacks* local_pAllocator = nullptr;
    if (pAllocator) {
        local_pAllocator = (VkAllocationCallbacks*)malloc(sizeof(VkAllocationCallbacks));
        if (!local_pAllocator) {
            ALOGE("DestroyCommandPool: Failed to allocate memory for VkAllocationCallbacks");
            return;
        }
        deepcopy_VkAllocationCallbacks(
            &vkAllocator,
            VK_STRUCTURE_TYPE_MAX_ENUM,
            pAllocator,
            local_pAllocator);
    }

    size_t byte_count = 0;
    if (pAllocator) {
        count_VkAllocationCallbacks(
            0,
            VK_STRUCTURE_TYPE_MAX_ENUM,
            local_pAllocator,
            &byte_count);
    }
    byte_count += 24; // 8 bytes for device, 8 bytes for commandPool, 8 bytes for allocator pointer

    char* send_buffer = (char*)mgr.addExternalParamPtr(byte_count);
    uint8_t** stream_ptr = (uint8_t**)&send_buffer;

    uint64_t guest_device = (uint64_t)(uintptr_t)device;
    uint64_t guest_commandPool = (uint64_t)(uintptr_t)commandPool;

    memcpy(*stream_ptr, &guest_device, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);

    memcpy(*stream_ptr, &guest_commandPool, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);

    uint64_t allocPtr = (uint64_t)(uintptr_t)pAllocator;
    memcpy(*stream_ptr, &allocPtr, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);
    if (pAllocator) {
        encode_to_stream_VkAllocationCallbacks(
            VK_STRUCTURE_TYPE_MAX_ENUM,
            local_pAllocator,
            stream_ptr);
    }

    // mgr.addParam64((uint64_t)(uintptr_t)device);
    // mgr.addParam64((uint64_t)(uintptr_t)commandPool);

    FlimeGuestBeforeDescriptorLifecycle(device);
    {
        std::lock_guard<std::mutex> lock(g_mapped_mutex);
        InvalidateAllSubmitHintCachesLocked("destroy_command_pool");
    }
    FlimeGuestDestroyCommandPool(device, commandPool);
    const ssize_t written =
        FlimeGuestWrite(&mgr,
                        express_gpu,
                        EXPRESS_GPU_DEVICE_ID,
                        FUNID_vkDestroyCommandPool,
                        true);
    FlimeGuestAfterDescriptorLifecycle(
        device, IsCompleteParamManagerWrite(written, 1));

    if (local_pAllocator) {
        free(local_pAllocator);
    }

    ALOGI("DestroyCommandPool: Sent destroy request for commandPool %lld", (long long)guest_commandPool);
}

VKAPI_ATTR void VKAPI_CALL DestroyDescriptorPool(
    VkDevice device,
    VkDescriptorPool descriptorPool,
    const VkAllocationCallbacks* pAllocator)
{
    if (descriptorPool == VK_NULL_HANDLE) return;
    if (kEnableDeferredFenceWait) {
        DrainDeferredQueues("destroy_descriptor_pool");
    }

    FlimeGuestBeforeDescriptorLifecycle(device);
    if (!FlimeGuestPrepareDescriptorRetirement(device)) {
        FlimeGuestAfterDescriptorLifecycle(device, false);
        ALOGE("FLIME could not drain descriptor records before "
              "vkDestroyDescriptorPool; destroy was not sent");
        return;
    }
    ForgetTrackedDescriptorSetsForPool(descriptorPool);

    if (kEnableDescriptorPoolReuse && pAllocator == nullptr) {
        DescriptorPoolSignature signature;
        bool have_signature = false;
        {
            std::lock_guard<std::mutex> lock(g_descriptor_lifecycle_mutex);
            auto it = g_descriptor_pool_signatures.find(descriptorPool);
            if (it != g_descriptor_pool_signatures.end()) {
                signature = it->second;
                have_signature = true;
            }
        }

        if (have_signature &&
            signature.device == device &&
            CacheDescriptorPoolForReuse(descriptorPool, signature)) {
            FlimeGuestResetDescriptorPool(
                device, descriptorPool, VK_SUCCESS);
            FlimeGuestAfterDescriptorLifecycle(device, true);
            ALOGV("DestroyDescriptorPool cached descriptorPool=%lld",
                  (long long)(uintptr_t)descriptorPool);
            return;
        }
    }
    FlimeGuestDestroyDescriptorPool(device, descriptorPool);
    FlimeGuestAfterDescriptorLifecycle(device, true);

    {
        std::lock_guard<std::mutex> lock(g_descriptor_lifecycle_mutex);
        g_descriptor_pool_signatures.erase(descriptorPool);
    }

    if (kEnableDeferredDescriptorPoolDestroy) {
        DeferDescriptorPoolDestroy(device, descriptorPool, pAllocator);
        ALOGV("DestroyDescriptorPool deferred descriptorPool=%lld",
              (long long)(uintptr_t)descriptorPool);
        return;
    }

    const std::vector<uint8_t> payload =
        BuildDestroyDescriptorPoolPayload(
            device, descriptorPool, pAllocator);
    SendDestroyDescriptorPoolPayload(
        device, descriptorPool, payload);

    {
        std::lock_guard<std::mutex> lock(g_descriptor_lifecycle_mutex);
        g_descriptor_lifecycle_stats.destroy_pool_calls++;
        g_descriptor_lifecycle_stats.flushed_pool_destroys++;
        MaybeLogDescriptorLifecycleStatsLocked("destroy_immediate");
    }
}

VKAPI_ATTR void VKAPI_CALL DestroyDescriptorSetLayout(
    VkDevice device,
    VkDescriptorSetLayout descriptorSetLayout,
    const VkAllocationCallbacks* pAllocator)
{
    if (kEnableDeferredFenceWait) {
        DrainDeferredQueues("destroy_descriptor_set_layout");
    }

    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;

    Allocator vkAllocator;

    VkAllocationCallbacks* local_pAllocator = nullptr;
    if (pAllocator) {
        local_pAllocator = (VkAllocationCallbacks*)malloc(sizeof(VkAllocationCallbacks));
        if (!local_pAllocator) {
            ALOGE("DestroyDescriptorSetLayout: Failed to allocate memory for VkAllocationCallbacks");
            return;
        }
        deepcopy_VkAllocationCallbacks(
            &vkAllocator,
            VK_STRUCTURE_TYPE_MAX_ENUM,
            pAllocator,
            local_pAllocator);
    }

    size_t byte_count = 0;
    if (pAllocator) {
        count_VkAllocationCallbacks(
            0,
            VK_STRUCTURE_TYPE_MAX_ENUM,
            local_pAllocator,
            &byte_count);
    }
    byte_count += 24; // 8 bytes for device, 8 bytes for descriptorSetLayout

    char* send_buffer = (char*)mgr.addExternalParamPtr(byte_count);
    uint8_t** stream_ptr = (uint8_t**)&send_buffer;

    uint64_t guest_device = (uint64_t)(uintptr_t)device;
    uint64_t guest_descriptorSetLayout = (uint64_t)(uintptr_t)descriptorSetLayout;

    memcpy(*stream_ptr, &guest_device, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);

    memcpy(*stream_ptr, &guest_descriptorSetLayout, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);

    uint64_t allocPtr = (uint64_t)(uintptr_t)pAllocator;
    memcpy(*stream_ptr, &allocPtr, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);
    if (pAllocator) {
        encode_to_stream_VkAllocationCallbacks(
            VK_STRUCTURE_TYPE_MAX_ENUM,
            local_pAllocator,
            stream_ptr);
    }

    // mgr.addParam64((uint64_t)(uintptr_t)device);
    // mgr.addParam64((uint64_t)(uintptr_t)descriptorSetLayout);

    FlimeGuestBeforeDescriptorLifecycle(device);
    FlimeGuestDestroyDescriptorSetLayout(device, descriptorSetLayout);
    const ssize_t written =
        FlimeGuestWrite(&mgr,
                        express_gpu,
                        EXPRESS_GPU_DEVICE_ID,
                        FUNID_vkDestroyDescriptorSetLayout,
                        true);
    FlimeGuestAfterDescriptorLifecycle(
        device, IsCompleteParamManagerWrite(written, 1));

    if (local_pAllocator) {
        free(local_pAllocator);
    }

    ALOGI("DestroyDescriptorSetLayout: Sent destroy request for descriptorSetLayout %lld", (long long)guest_descriptorSetLayout);
}

VKAPI_ATTR void VKAPI_CALL DestroyDescriptorUpdateTemplate(
    VkDevice device,
    VkDescriptorUpdateTemplate descriptorUpdateTemplate,
    const VkAllocationCallbacks* pAllocator)
{
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;

    Allocator vkAllocator;

    VkAllocationCallbacks* local_pAllocator = nullptr;
    if (pAllocator) {
        local_pAllocator = (VkAllocationCallbacks*)malloc(sizeof(VkAllocationCallbacks));
        if (!local_pAllocator) {
            ALOGE("DestroyDescriptorUpdateTemplate: Failed to allocate memory for VkAllocationCallbacks");
            return;
        }
        deepcopy_VkAllocationCallbacks(
            &vkAllocator,
            VK_STRUCTURE_TYPE_MAX_ENUM,
            pAllocator,
            local_pAllocator);
    }

    size_t byte_count = 0;
    if (pAllocator) {
        count_VkAllocationCallbacks(
            0,
            VK_STRUCTURE_TYPE_MAX_ENUM,
            local_pAllocator,
            &byte_count);
    }
    byte_count += 24; // 8 bytes for device, 8 bytes for descriptorUpdateTemplate

    char* send_buffer = (char*)mgr.addExternalParamPtr(byte_count);
    uint8_t** stream_ptr = (uint8_t**)&send_buffer;

    uint64_t guest_device = (uint64_t)(uintptr_t)device;
    uint64_t guest_descriptorUpdateTemplate = (uint64_t)(uintptr_t)descriptorUpdateTemplate;

    memcpy(*stream_ptr, &guest_device, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);

    memcpy(*stream_ptr, &guest_descriptorUpdateTemplate, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);

    uint64_t allocPtr = (uint64_t)(uintptr_t)pAllocator;
    memcpy(*stream_ptr, &allocPtr, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);
    if (pAllocator) {
        encode_to_stream_VkAllocationCallbacks(
            VK_STRUCTURE_TYPE_MAX_ENUM,
            local_pAllocator,
            stream_ptr);
    }

    // mgr.addParam64((uint64_t)(uintptr_t)device);
    // mgr.addParam64((uint64_t)(uintptr_t)descriptorUpdateTemplate);

    FlimeGuestBeforeDescriptorLifecycle(device);
    FlimeGuestDestroyDescriptorUpdateTemplate(
        device, descriptorUpdateTemplate);
    const ssize_t written =
        FlimeGuestWrite(&mgr,
                        express_gpu,
                        EXPRESS_GPU_DEVICE_ID,
                        FUNID_vkDestroyDescriptorUpdateTemplate,
                        true);
    FlimeGuestAfterDescriptorLifecycle(
        device, IsCompleteParamManagerWrite(written, 1));
    ForgetDescriptorUpdateTemplateInfo(descriptorUpdateTemplate);

    if (local_pAllocator) {
        free(local_pAllocator);
    }

    ALOGI("DestroyDescriptorUpdateTemplate: Sent destroy request for descriptorUpdateTemplate %lld", (long long)guest_descriptorUpdateTemplate);
}

VKAPI_ATTR void VKAPI_CALL DestroyDevice(
    VkDevice device,
    const VkAllocationCallbacks* pAllocator)
{
    FlushCachedDescriptorPools("destroy_device", device);
    FlushDeferredDescriptorPoolDestroys("destroy_device", true);

    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;

    Allocator vkAllocator;

    VkAllocationCallbacks* local_pAllocator = nullptr;
    if (pAllocator) {
        local_pAllocator = (VkAllocationCallbacks*)malloc(sizeof(VkAllocationCallbacks));
        if (!local_pAllocator) {
            ALOGE("DestroyDevice: Failed to allocate memory for VkAllocationCallbacks");
            return;
        }
        deepcopy_VkAllocationCallbacks(
            &vkAllocator,
            VK_STRUCTURE_TYPE_MAX_ENUM,
            pAllocator,
            local_pAllocator);
    }

    size_t byte_count = 0;
    if (pAllocator) {
        count_VkAllocationCallbacks(
            0,
            VK_STRUCTURE_TYPE_MAX_ENUM,
            local_pAllocator,
            &byte_count);
    }
    byte_count += 16; // 8 bytes for device handle , 8 bytes for allocator pointer

    char* send_buffer = (char*)mgr.addExternalParamPtr(byte_count);
    uint8_t** stream_ptr = (uint8_t**)&send_buffer;

    uint64_t guest_device = (uint64_t)(uintptr_t)device;

    memcpy(*stream_ptr, &guest_device, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);

    uint64_t allocPtr = (uint64_t)(uintptr_t)pAllocator;
    memcpy(*stream_ptr, &allocPtr, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);
    if (pAllocator) {
        encode_to_stream_VkAllocationCallbacks(
            VK_STRUCTURE_TYPE_MAX_ENUM,
            local_pAllocator,
            stream_ptr);
    }

    // mgr.addParam64((uint64_t)(uintptr_t)device);

    FlimeGuestBeforeDestroyDevice(device);
    FlimeGuestWrite(&mgr,
        express_gpu,
        EXPRESS_GPU_DEVICE_ID,
        FUNID_vkDestroyDevice,
        false);
    FlimeGuestDestroyDevice(device);

    if (local_pAllocator) {
        free(local_pAllocator);
    }

    ALOGI("DestroyDevice: Sent destroy request for device %lld", (long long)guest_device);
}

VKAPI_ATTR void VKAPI_CALL DestroyEvent(
    VkDevice device,
    VkEvent event,
    const VkAllocationCallbacks* pAllocator)
{
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;

    Allocator vkAllocator;

    VkAllocationCallbacks* local_pAllocator = nullptr;
    if (pAllocator) {
        local_pAllocator = (VkAllocationCallbacks*)malloc(sizeof(VkAllocationCallbacks));
        if (!local_pAllocator) {
            ALOGE("DestroyEvent: Failed to allocate memory for VkAllocationCallbacks");
            return;
        }
        deepcopy_VkAllocationCallbacks(
            &vkAllocator,
            VK_STRUCTURE_TYPE_MAX_ENUM,
            pAllocator,
            local_pAllocator);
    }

    size_t byte_count = 0;
    if (pAllocator) {
        count_VkAllocationCallbacks(
            0,
            VK_STRUCTURE_TYPE_MAX_ENUM,
            local_pAllocator,
            &byte_count);
    }
    byte_count += 24; // 8 bytes for device, 8 bytes for event, 8 bytes for allocator pointer

    char* send_buffer = (char*)mgr.addExternalParamPtr(byte_count);
    uint8_t** stream_ptr = (uint8_t**)&send_buffer;

    uint64_t guest_device = (uint64_t)(uintptr_t)device;
    uint64_t guest_event = (uint64_t)(uintptr_t)event;

    memcpy(*stream_ptr, &guest_device, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);

    memcpy(*stream_ptr, &guest_event, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);

    uint64_t allocPtr = (uint64_t)(uintptr_t)pAllocator;
    memcpy(*stream_ptr, &allocPtr, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);
    if (pAllocator) {
        encode_to_stream_VkAllocationCallbacks(
            VK_STRUCTURE_TYPE_MAX_ENUM,
            local_pAllocator,
            stream_ptr);
    }

    // mgr.addParam64((uint64_t)(uintptr_t)device);
    // mgr.addParam64((uint64_t)(uintptr_t)event);

    FlimeGuestWrite(&mgr,
        express_gpu,
        EXPRESS_GPU_DEVICE_ID,
        FUNID_vkDestroyEvent,
        false);

    if (local_pAllocator) {
        free(local_pAllocator);
    }

    ALOGI("DestroyEvent: Sent destroy request for event %lld", (long long)guest_event);
}

VKAPI_ATTR void VKAPI_CALL DestroyFence(
    VkDevice device,
    VkFence fence,
    const VkAllocationCallbacks* pAllocator)
{
    if (kEnableDeferredFenceWait && fence != VK_NULL_HANDLE) {
        bool deferred_with_readback = false;
        {
            std::lock_guard<std::mutex> lock(g_mapped_mutex);
            deferred_with_readback =
                g_deferred_fence_queues.find(fence) != g_deferred_fence_queues.end() &&
                g_fence_pending_invalidate_ranges.find(fence) !=
                    g_fence_pending_invalidate_ranges.end();
        }
        if (deferred_with_readback) {
            DrainDeferredQueues("destroy_fence_readback");
        }
    }

    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;

    Allocator vkAllocator;

    VkAllocationCallbacks* local_pAllocator = nullptr;
    if (pAllocator) {
        local_pAllocator = (VkAllocationCallbacks*)malloc(sizeof(VkAllocationCallbacks));
        if (!local_pAllocator) {
            ALOGE("DestroyFence: Failed to allocate memory for VkAllocationCallbacks");
            return;
        }
        deepcopy_VkAllocationCallbacks(
            &vkAllocator,
            VK_STRUCTURE_TYPE_MAX_ENUM,
            pAllocator,
            local_pAllocator);
    }

    size_t byte_count = 0;
    if (pAllocator) {
        count_VkAllocationCallbacks(
            0,
            VK_STRUCTURE_TYPE_MAX_ENUM,
            local_pAllocator,
            &byte_count);
    }
    byte_count += 24; // 8 bytes for device, 8 bytes for fence, 8 bytes for allocator pointer

    char* send_buffer = (char*)mgr.addExternalParamPtr(byte_count);
    uint8_t** stream_ptr = (uint8_t**)&send_buffer;

    uint64_t guest_device = (uint64_t)(uintptr_t)device;
    uint64_t guest_fence = (uint64_t)(uintptr_t)fence;

    memcpy(*stream_ptr, &guest_device, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);

    memcpy(*stream_ptr, &guest_fence, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);

    uint64_t allocPtr = (uint64_t)(uintptr_t)pAllocator;
    memcpy(*stream_ptr, &allocPtr, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);
    if (pAllocator) {
        encode_to_stream_VkAllocationCallbacks(
            VK_STRUCTURE_TYPE_MAX_ENUM,
            local_pAllocator,
            stream_ptr);
    }

    // mgr.addParam64((uint64_t)(uintptr_t)device);
    // mgr.addParam64((uint64_t)(uintptr_t)fence);

    FlimeGuestWrite(&mgr,
        express_gpu,
        EXPRESS_GPU_DEVICE_ID,
        FUNID_vkDestroyFence,
        false);
    {
        std::lock_guard<std::mutex> lock(g_mapped_mutex);
        g_fence_pending_invalidate_ranges.erase(fence);
    }
    if (kEnableDeferredFenceWait) {
        ForgetDeferredFenceKeepQueue(fence);
    } else {
        ForgetDeferredFence(fence);
    }

    if (local_pAllocator) {
        free(local_pAllocator);
    }

    ALOGI("DestroyFence: Sent destroy request for fence %lld", (long long)guest_fence);
}

VKAPI_ATTR void VKAPI_CALL DestroyFramebuffer(
    VkDevice device,
    VkFramebuffer framebuffer,
    const VkAllocationCallbacks* pAllocator)
{
    if (framebuffer == VK_NULL_HANDLE) {
        ALOGD("DestroyFramebuffer: null framebuffer, skip host destroy");
        return;
    }

    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;

    Allocator vkAllocator;

    VkAllocationCallbacks* local_pAllocator = nullptr;
    if (pAllocator) {
        local_pAllocator = (VkAllocationCallbacks*)malloc(sizeof(VkAllocationCallbacks));
        if (!local_pAllocator) {
            ALOGE("DestroyFramebuffer: Failed to allocate memory for VkAllocationCallbacks");
            return;
        }
        deepcopy_VkAllocationCallbacks(
            &vkAllocator,
            VK_STRUCTURE_TYPE_MAX_ENUM,
            pAllocator,
            local_pAllocator);
    }

    size_t byte_count = 0;
    if (pAllocator) {
        count_VkAllocationCallbacks(
            0,
            VK_STRUCTURE_TYPE_MAX_ENUM,
            local_pAllocator,
            &byte_count);
    }
    byte_count += 24; // 8 bytes for device, 8 bytes for framebuffer, 8 bytes for allocator pointer

    char* send_buffer = (char*)mgr.addExternalParamPtr(byte_count);
    uint8_t** stream_ptr = (uint8_t**)&send_buffer;

    uint64_t guest_device = (uint64_t)(uintptr_t)device;
    uint64_t guest_framebuffer = (uint64_t)(uintptr_t)framebuffer;

    memcpy(*stream_ptr, &guest_device, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);

    memcpy(*stream_ptr, &guest_framebuffer, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);

    uint64_t allocPtr = (uint64_t)(uintptr_t)pAllocator;
    memcpy(*stream_ptr, &allocPtr, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);
    if (pAllocator) {
        encode_to_stream_VkAllocationCallbacks(
            VK_STRUCTURE_TYPE_MAX_ENUM,
            local_pAllocator,
            stream_ptr);
    }

    // mgr.addParam64((uint64_t)(uintptr_t)device);
    // mgr.addParam64((uint64_t)(uintptr_t)framebuffer);

    FlimeGuestWrite(&mgr,
        express_gpu,
        EXPRESS_GPU_DEVICE_ID,
        FUNID_vkDestroyFramebuffer,
        false);

    if (local_pAllocator) {
        free(local_pAllocator);
    }

    ALOGI("DestroyFramebuffer: Sent destroy request for framebuffer %lld", (long long)guest_framebuffer);
}

VKAPI_ATTR void VKAPI_CALL DestroyImage(
    VkDevice device,
    VkImage image,
    const VkAllocationCallbacks* pAllocator)
{
    if (kEnableDeferredFenceWait) {
        DrainDeferredQueues("destroy_image");
    }

    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;

    Allocator vkAllocator;

    VkAllocationCallbacks* local_pAllocator = nullptr;
    if (pAllocator) {
        local_pAllocator = (VkAllocationCallbacks*)malloc(sizeof(VkAllocationCallbacks));
        if (!local_pAllocator) {
            ALOGE("DestroyImage: Failed to allocate memory for VkAllocationCallbacks");
            return;
        }
        deepcopy_VkAllocationCallbacks(
            &vkAllocator,
            VK_STRUCTURE_TYPE_MAX_ENUM,
            pAllocator,
            local_pAllocator);
    }

    size_t byte_count = 0;
    if (pAllocator) {
        count_VkAllocationCallbacks(
            0,
            VK_STRUCTURE_TYPE_MAX_ENUM,
            local_pAllocator,
            &byte_count);
    }
    byte_count += 24; // 8 bytes for device, 8 bytes for image

    char* send_buffer = (char*)mgr.addExternalParamPtr(byte_count);
    uint8_t** stream_ptr = (uint8_t**)&send_buffer;

    uint64_t guest_device = (uint64_t)(uintptr_t)device;
    uint64_t guest_image = (uint64_t)(uintptr_t)image;

    memcpy(*stream_ptr, &guest_device, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);

    memcpy(*stream_ptr, &guest_image, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);

    uint64_t allocPtr = (uint64_t)(uintptr_t)pAllocator;
    memcpy(*stream_ptr, &allocPtr, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);
    if (pAllocator) {
        encode_to_stream_VkAllocationCallbacks(
            VK_STRUCTURE_TYPE_MAX_ENUM,
            local_pAllocator,
            stream_ptr);
    }

    // mgr.addParam64((uint64_t)(uintptr_t)device);
    // mgr.addParam64((uint64_t)(uintptr_t)image);

    FlimeGuestWrite(&mgr,
        express_gpu,
        EXPRESS_GPU_DEVICE_ID,
        FUNID_vkDestroyImage,
        false);

    if (local_pAllocator) {
        free(local_pAllocator);
    }

    ALOGI("DestroyImage: Sent destroy request for image %lld", (long long)guest_image);
}

VKAPI_ATTR void VKAPI_CALL DestroyImageView(
    VkDevice device,
    VkImageView imageView,
    const VkAllocationCallbacks* pAllocator)
{
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;

    Allocator vkAllocator;

    VkAllocationCallbacks* local_pAllocator = nullptr;
    if (pAllocator) {
        local_pAllocator = (VkAllocationCallbacks*)malloc(sizeof(VkAllocationCallbacks));
        if (!local_pAllocator) {
            ALOGE("DestroyImageView: Failed to allocate memory for VkAllocationCallbacks");
            return;
        }
        deepcopy_VkAllocationCallbacks(
            &vkAllocator,
            VK_STRUCTURE_TYPE_MAX_ENUM,
            pAllocator,
            local_pAllocator);
    }

    size_t byte_count = 0;
    if (pAllocator) {
        count_VkAllocationCallbacks(
            0,
            VK_STRUCTURE_TYPE_MAX_ENUM,
            local_pAllocator,
            &byte_count);
    }
    byte_count += 24; // 8 bytes for device, 8 bytes for imageView, 8 bytes for allocator pointer

    char* send_buffer = (char*)mgr.addExternalParamPtr(byte_count);
    uint8_t** stream_ptr = (uint8_t**)&send_buffer;

    uint64_t guest_device = (uint64_t)(uintptr_t)device;
    uint64_t guest_imageView = (uint64_t)(uintptr_t)imageView;

    memcpy(*stream_ptr, &guest_device, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);

    memcpy(*stream_ptr, &guest_imageView, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);

    uint64_t allocPtr = (uint64_t)(uintptr_t)pAllocator;
    memcpy(*stream_ptr, &allocPtr, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);
    if (pAllocator) {
        encode_to_stream_VkAllocationCallbacks(
            VK_STRUCTURE_TYPE_MAX_ENUM,
            local_pAllocator,
            stream_ptr);
    }

    // mgr.addParam64((uint64_t)(uintptr_t)device);
    // mgr.addParam64((uint64_t)(uintptr_t)imageView);

    FlimeGuestBeforeDescriptorLifecycle(device);
    if (!FlimeGuestPrepareDescriptorRetirement(device)) {
        mgr.clear();
        FlimeGuestAfterDescriptorLifecycle(device, false);
        if (local_pAllocator) {
            free(local_pAllocator);
        }
        ALOGE("FLIME could not drain descriptor records before "
              "vkDestroyImageView; destroy was not sent");
        return;
    }
    FlimeGuestDestroyDescriptorPayload(
        device, VK_OBJECT_TYPE_IMAGE_VIEW, guest_imageView);
    const ssize_t written = FlimeGuestWrite(&mgr,
        express_gpu,
        EXPRESS_GPU_DEVICE_ID,
        FUNID_vkDestroyImageView,
        true);
    FlimeGuestAfterDescriptorLifecycle(
        device, IsCompleteParamManagerWrite(written, 1));

    if (local_pAllocator) {
        free(local_pAllocator);
    }

    ALOGI("DestroyImageView: Sent destroy request for imageView %lld", (long long)guest_imageView);
}

VKAPI_ATTR void VKAPI_CALL DestroyInstance(
    VkInstance instance,
    const VkAllocationCallbacks* pAllocator)
{
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;

    (void)pAllocator;
    const size_t byte_count = 16;

    char* send_buffer = (char*)mgr.addExternalParamPtr(byte_count);
    uint8_t** stream_ptr = (uint8_t**)&send_buffer;

    uint64_t guest_instance = (uint64_t)(uintptr_t)instance;

    memcpy(*stream_ptr, &guest_instance, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);

    /* Host allocators are always NULL across the process boundary. */
    uint64_t allocPtr = 0;
    memcpy(*stream_ptr, &allocPtr, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);

    // mgr.addParam64((uint64_t)(uintptr_t)instance);

    const ssize_t written = FlimeGuestWrite(&mgr,
        express_gpu,
        EXPRESS_GPU_DEVICE_ID,
        FUNID_vkDestroyInstance,
        false);
    if (!IsCompleteParamManagerWrite(written, 1)) {
        ALOGE("DestroyInstance: short transport write for instance %lld",
              (long long)guest_instance);
    }
    if (instance && instance->allocator.pfnFree) {
        instance->allocator.pfnFree(instance->allocator.pUserData, instance);
    }

    ALOGI("DestroyInstance: Sent destroy request for instance %lld", (long long)guest_instance);
}

VKAPI_ATTR void VKAPI_CALL DestroyPipeline(
    VkDevice device,
    VkPipeline pipeline,
    const VkAllocationCallbacks* pAllocator)
{
    if (kEnableDeferredFenceWait) {
        DrainDeferredQueues("destroy_pipeline");
    }

    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;

    Allocator vkAllocator;

    VkAllocationCallbacks* local_pAllocator = nullptr;
    if (pAllocator) {
        local_pAllocator = (VkAllocationCallbacks*)malloc(sizeof(VkAllocationCallbacks));
        if (!local_pAllocator) {
            ALOGE("DestroyPipeline: Failed to allocate memory for VkAllocationCallbacks");
            return;
        }
        deepcopy_VkAllocationCallbacks(
            &vkAllocator,
            VK_STRUCTURE_TYPE_MAX_ENUM,
            pAllocator,
            local_pAllocator);
    }

    size_t byte_count = 0;
    if (pAllocator) {
        count_VkAllocationCallbacks(
            0,
            VK_STRUCTURE_TYPE_MAX_ENUM,
            local_pAllocator,
            &byte_count);
    }
    byte_count += 24; // 8 bytes for device, 8 bytes for pipeline, 8 bytes for allocator pointer

    char* send_buffer = (char*)mgr.addExternalParamPtr(byte_count);
    uint8_t** stream_ptr = (uint8_t**)&send_buffer;

    uint64_t guest_device = (uint64_t)(uintptr_t)device;
    uint64_t guest_pipeline = (uint64_t)(uintptr_t)pipeline;

    memcpy(*stream_ptr, &guest_device, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);

    memcpy(*stream_ptr, &guest_pipeline, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);

    uint64_t allocPtr = (uint64_t)(uintptr_t)pAllocator;
    memcpy(*stream_ptr, &allocPtr, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);
    if (pAllocator) {
        encode_to_stream_VkAllocationCallbacks(
            VK_STRUCTURE_TYPE_MAX_ENUM,
            local_pAllocator,
            stream_ptr);
    }

    // mgr.addParam64((uint64_t)(uintptr_t)device);
    // mgr.addParam64((uint64_t)(uintptr_t)pipeline);

    FlimeGuestWrite(&mgr,
        express_gpu,
        EXPRESS_GPU_DEVICE_ID,
        FUNID_vkDestroyPipeline,
        false);

    if (local_pAllocator) {
        free(local_pAllocator);
    }

    ALOGI("DestroyPipeline: Sent destroy request for pipeline %lld", (long long)guest_pipeline);
}

VKAPI_ATTR void VKAPI_CALL DestroyPipelineCache(
    VkDevice device,
    VkPipelineCache pipelineCache,
    const VkAllocationCallbacks* pAllocator)
{
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;

    Allocator vkAllocator;

    VkAllocationCallbacks* local_pAllocator = nullptr;
    if (pAllocator) {
        local_pAllocator = (VkAllocationCallbacks*)malloc(sizeof(VkAllocationCallbacks));
        if (!local_pAllocator) {
            ALOGE("DestroyPipelineCache: Failed to allocate memory for VkAllocationCallbacks");
            return;
        }
        deepcopy_VkAllocationCallbacks(
            &vkAllocator,
            VK_STRUCTURE_TYPE_MAX_ENUM,
            pAllocator,
            local_pAllocator);
    }

    size_t byte_count = 0;
    if (pAllocator) {
        count_VkAllocationCallbacks(
            0,
            VK_STRUCTURE_TYPE_MAX_ENUM,
            local_pAllocator,
            &byte_count);
    }
    byte_count += 24; // 8 bytes for device, 8 bytes for pipelineCache

    char* send_buffer = (char*)mgr.addExternalParamPtr(byte_count);
    uint8_t** stream_ptr = (uint8_t**)&send_buffer;

    uint64_t guest_device = (uint64_t)(uintptr_t)device;
    uint64_t guest_pipelineCache = (uint64_t)(uintptr_t)pipelineCache;

    memcpy(*stream_ptr, &guest_device, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);

    memcpy(*stream_ptr, &guest_pipelineCache, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);

    uint64_t allocPtr = (uint64_t)(uintptr_t)pAllocator;
    memcpy(*stream_ptr, &allocPtr, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);
    if (pAllocator) {
        encode_to_stream_VkAllocationCallbacks(
            VK_STRUCTURE_TYPE_MAX_ENUM,
            local_pAllocator,
            stream_ptr);
    }

    // mgr.addParam64((uint64_t)(uintptr_t)device);
    // mgr.addParam64((uint64_t)(uintptr_t)pipelineCache);

    FlimeGuestWrite(&mgr,
        express_gpu,
        EXPRESS_GPU_DEVICE_ID,
        FUNID_vkDestroyPipelineCache,
        false);

    if (local_pAllocator) {
        free(local_pAllocator);
    }

    ALOGI("DestroyPipelineCache: Sent destroy request for pipelineCache %lld", (long long)guest_pipelineCache);
}

VKAPI_ATTR void VKAPI_CALL DestroyPipelineLayout(
    VkDevice device,
    VkPipelineLayout pipelineLayout,
    const VkAllocationCallbacks* pAllocator)
{
    if (kEnableDeferredFenceWait) {
        DrainDeferredQueues("destroy_pipeline_layout");
    }

    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;

    Allocator vkAllocator;

    VkAllocationCallbacks* local_pAllocator = nullptr;
    if (pAllocator) {
        local_pAllocator = (VkAllocationCallbacks*)malloc(sizeof(VkAllocationCallbacks));
        if (!local_pAllocator) {
            ALOGE("DestroyPipelineLayout: Failed to allocate memory for VkAllocationCallbacks");
            return;
        }
        deepcopy_VkAllocationCallbacks(
            &vkAllocator,
            VK_STRUCTURE_TYPE_MAX_ENUM,
            pAllocator,
            local_pAllocator);
    }

    size_t byte_count = 0;
    if (pAllocator) {
        count_VkAllocationCallbacks(
            0,
            VK_STRUCTURE_TYPE_MAX_ENUM,
            local_pAllocator,
            &byte_count);
    }
    byte_count += 24; // 8 bytes for device, 8 bytes for pipelineLayout

    char* send_buffer = (char*)mgr.addExternalParamPtr(byte_count);
    uint8_t** stream_ptr = (uint8_t**)&send_buffer;

    uint64_t guest_device = (uint64_t)(uintptr_t)device;
    uint64_t guest_pipelineLayout = (uint64_t)(uintptr_t)pipelineLayout;

    memcpy(*stream_ptr, &guest_device, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);

    memcpy(*stream_ptr, &guest_pipelineLayout, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);

    uint64_t allocPtr = (uint64_t)(uintptr_t)pAllocator;
    memcpy(*stream_ptr, &allocPtr, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);
    if (pAllocator) {
        encode_to_stream_VkAllocationCallbacks(
            VK_STRUCTURE_TYPE_MAX_ENUM,
            local_pAllocator,
            stream_ptr);
    }

    // mgr.addParam64((uint64_t)(uintptr_t)device);
    // mgr.addParam64((uint64_t)(uintptr_t)pipelineLayout);

    FlimeGuestWrite(&mgr,
        express_gpu,
        EXPRESS_GPU_DEVICE_ID,
        FUNID_vkDestroyPipelineLayout,
        false);

    if (local_pAllocator) {
        free(local_pAllocator);
    }

    ALOGI("DestroyPipelineLayout: Sent destroy request for pipelineLayout %lld", (long long)guest_pipelineLayout);
}

VKAPI_ATTR void VKAPI_CALL DestroyPrivateDataSlot(
    VkDevice device,
    VkPrivateDataSlot privateDataSlot,
    const VkAllocationCallbacks* pAllocator)
{
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;

    Allocator vkAllocator;

    VkAllocationCallbacks* local_pAllocator = nullptr;
    if (pAllocator) {
        local_pAllocator = (VkAllocationCallbacks*)malloc(sizeof(VkAllocationCallbacks));
        if (!local_pAllocator) {
            ALOGE("DestroyPrivateDataSlot: Failed to allocate memory for VkAllocationCallbacks");
            return;
        }
        deepcopy_VkAllocationCallbacks(
            &vkAllocator,
            VK_STRUCTURE_TYPE_MAX_ENUM,
            pAllocator,
            local_pAllocator);
    }

    size_t byte_count = 0;
    if (pAllocator) {
        count_VkAllocationCallbacks(
            0,
            VK_STRUCTURE_TYPE_MAX_ENUM,
            local_pAllocator,
            &byte_count);
    }
    byte_count += 24; // 8 bytes for device, 8 bytes for privateDataSlot, 8 bytes for allocator pointer

    char* send_buffer = (char*)mgr.addExternalParamPtr(byte_count);
    uint8_t** stream_ptr = (uint8_t**)&send_buffer;

    uint64_t guest_device = (uint64_t)(uintptr_t)device;
    uint64_t guest_privateDataSlot = (uint64_t)(uintptr_t)privateDataSlot;

    memcpy(*stream_ptr, &guest_device, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);

    memcpy(*stream_ptr, &guest_privateDataSlot, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);

    uint64_t allocPtr = (uint64_t)(uintptr_t)pAllocator;
    memcpy(*stream_ptr, &allocPtr, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);
    if (pAllocator) {
        encode_to_stream_VkAllocationCallbacks(
            VK_STRUCTURE_TYPE_MAX_ENUM,
            local_pAllocator,
            stream_ptr);
    }

    // mgr.addParam64((uint64_t)(uintptr_t)device);
    // mgr.addParam64((uint64_t)(uintptr_t)privateDataSlot);

    FlimeGuestWrite(&mgr,
        express_gpu,
        EXPRESS_GPU_DEVICE_ID,
        FUNID_vkDestroyPrivateDataSlot,
        false);

    if (local_pAllocator) {
        free(local_pAllocator);
    }

    ALOGI("DestroyPrivateDataSlot: Sent destroy request for privateDataSlot %lld", (long long)guest_privateDataSlot);
}

VKAPI_ATTR void VKAPI_CALL DestroyQueryPool(
    VkDevice device,
    VkQueryPool queryPool,
    const VkAllocationCallbacks* pAllocator)
{
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;

    Allocator vkAllocator;

    VkAllocationCallbacks* local_pAllocator = nullptr;
    if (pAllocator) {
        local_pAllocator = (VkAllocationCallbacks*)malloc(sizeof(VkAllocationCallbacks));
        if (!local_pAllocator) {
            ALOGE("DestroyQueryPool: Failed to allocate memory for VkAllocationCallbacks");
            return;
        }
        deepcopy_VkAllocationCallbacks(
            &vkAllocator,
            VK_STRUCTURE_TYPE_MAX_ENUM,
            pAllocator,
            local_pAllocator);
    }

    size_t byte_count = 0;
    if (pAllocator) {
        count_VkAllocationCallbacks(
            0,
            VK_STRUCTURE_TYPE_MAX_ENUM,
            local_pAllocator,
            &byte_count);
    }
    byte_count += 24; // 8 bytes for device, 8 bytes for queryPool, 8 bytes for allocator pointer

    char* send_buffer = (char*)mgr.addExternalParamPtr(byte_count);
    uint8_t** stream_ptr = (uint8_t**)&send_buffer;

    uint64_t guest_device = (uint64_t)(uintptr_t)device;
    uint64_t guest_queryPool = (uint64_t)(uintptr_t)queryPool;

    memcpy(*stream_ptr, &guest_device, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);

    memcpy(*stream_ptr, &guest_queryPool, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);

    uint64_t allocPtr = (uint64_t)(uintptr_t)pAllocator;
    memcpy(*stream_ptr, &allocPtr, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);
    if (pAllocator) {
        encode_to_stream_VkAllocationCallbacks(
            VK_STRUCTURE_TYPE_MAX_ENUM,
            local_pAllocator,
            stream_ptr);
    }

    // mgr.addParam64((uint64_t)(uintptr_t)device);
    // mgr.addParam64((uint64_t)(uintptr_t)queryPool);

    FlimeGuestWrite(&mgr,
        express_gpu,
        EXPRESS_GPU_DEVICE_ID,
        FUNID_vkDestroyQueryPool,
        false);

    if (local_pAllocator) {
        free(local_pAllocator);
    }

    ALOGI("DestroyQueryPool: Sent destroy request for queryPool %lld", (long long)guest_queryPool);
}

VKAPI_ATTR void VKAPI_CALL DestroyRenderPass(
    VkDevice device,
    VkRenderPass renderPass,
    const VkAllocationCallbacks* pAllocator)
{
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;

    Allocator vkAllocator;

    VkAllocationCallbacks* local_pAllocator = nullptr;
    if (pAllocator) {
        local_pAllocator = (VkAllocationCallbacks*)malloc(sizeof(VkAllocationCallbacks));
        if (!local_pAllocator) {
            ALOGE("DestroyRenderPass: Failed to allocate memory for VkAllocationCallbacks");
            return;
        }
        deepcopy_VkAllocationCallbacks(
            &vkAllocator,
            VK_STRUCTURE_TYPE_MAX_ENUM,
            pAllocator,
            local_pAllocator);
    }

    size_t byte_count = 0;
    if (pAllocator) {
        count_VkAllocationCallbacks(
            0,
            VK_STRUCTURE_TYPE_MAX_ENUM,
            local_pAllocator,
            &byte_count);
    }
    byte_count += 24; // 8 bytes for device, 8 bytes for renderPass, 8 bytes for allocator pointer

    char* send_buffer = (char*)mgr.addExternalParamPtr(byte_count);
    uint8_t** stream_ptr = (uint8_t**)&send_buffer;

    uint64_t guest_device = (uint64_t)(uintptr_t)device;
    uint64_t guest_renderPass = (uint64_t)(uintptr_t)renderPass;

    memcpy(*stream_ptr, &guest_device, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);

    memcpy(*stream_ptr, &guest_renderPass, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);

    uint64_t allocPtr = (uint64_t)(uintptr_t)pAllocator;
    memcpy(*stream_ptr, &allocPtr, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);
    if (pAllocator) {
        encode_to_stream_VkAllocationCallbacks(
            VK_STRUCTURE_TYPE_MAX_ENUM,
            local_pAllocator,
            stream_ptr);
    }

    // mgr.addParam64((uint64_t)(uintptr_t)device);
    // mgr.addParam64((uint64_t)(uintptr_t)renderPass);

    FlimeGuestWrite(&mgr,
        express_gpu,
        EXPRESS_GPU_DEVICE_ID,
        FUNID_vkDestroyRenderPass,
        false);

    if (local_pAllocator) {
        free(local_pAllocator);
    }

    ALOGI("DestroyRenderPass: Sent destroy request for renderPass %lld", (long long)guest_renderPass);
}

VKAPI_ATTR void VKAPI_CALL DestroySampler(
    VkDevice device,
    VkSampler sampler,
    const VkAllocationCallbacks* pAllocator)
{
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;

    Allocator vkAllocator;

    VkAllocationCallbacks* local_pAllocator = nullptr;
    if (pAllocator) {
        local_pAllocator = (VkAllocationCallbacks*)malloc(sizeof(VkAllocationCallbacks));
        if (!local_pAllocator) {
            ALOGE("DestroySampler: Failed to allocate memory for VkAllocationCallbacks");
            return;
        }
        deepcopy_VkAllocationCallbacks(
            &vkAllocator,
            VK_STRUCTURE_TYPE_MAX_ENUM,
            pAllocator,
            local_pAllocator);
    }

    size_t byte_count = 0;
    if (pAllocator) {
        count_VkAllocationCallbacks(
            0,
            VK_STRUCTURE_TYPE_MAX_ENUM,
            local_pAllocator,
            &byte_count);
    }
    byte_count += 24; // 8 bytes for device, 8 bytes for sampler, 8 bytes for allocator pointer

    char* send_buffer = (char*)mgr.addExternalParamPtr(byte_count);
    uint8_t** stream_ptr = (uint8_t**)&send_buffer;

    uint64_t guest_device = (uint64_t)(uintptr_t)device;
    uint64_t guest_sampler = (uint64_t)(uintptr_t)sampler;

    memcpy(*stream_ptr, &guest_device, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);

    memcpy(*stream_ptr, &guest_sampler, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);

    uint64_t allocPtr = (uint64_t)(uintptr_t)pAllocator;
    memcpy(*stream_ptr, &allocPtr, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);
    if (pAllocator) {
        encode_to_stream_VkAllocationCallbacks(
            VK_STRUCTURE_TYPE_MAX_ENUM,
            local_pAllocator,
            stream_ptr);
    }

    // mgr.addParam64((uint64_t)(uintptr_t)device);
    // mgr.addParam64((uint64_t)(uintptr_t)sampler);

    FlimeGuestBeforeDescriptorLifecycle(device);
    if (!FlimeGuestPrepareDescriptorRetirement(device)) {
        mgr.clear();
        FlimeGuestAfterDescriptorLifecycle(device, false);
        if (local_pAllocator) {
            free(local_pAllocator);
        }
        ALOGE("FLIME could not drain descriptor records before "
              "vkDestroySampler; destroy was not sent");
        return;
    }
    FlimeGuestDestroyDescriptorPayload(
        device, VK_OBJECT_TYPE_SAMPLER, guest_sampler);
    const ssize_t written = FlimeGuestWrite(&mgr,
        express_gpu,
        EXPRESS_GPU_DEVICE_ID,
        FUNID_vkDestroySampler,
        true);
    FlimeGuestAfterDescriptorLifecycle(
        device, IsCompleteParamManagerWrite(written, 1));

    if (local_pAllocator) {
        free(local_pAllocator);
    }

    ALOGI("DestroySampler: Sent destroy request for sampler %lld", (long long)guest_sampler);
}

VKAPI_ATTR void VKAPI_CALL DestroySamplerYcbcrConversion(
    VkDevice device,
    VkSamplerYcbcrConversion ycbcrConversion,
    const VkAllocationCallbacks* pAllocator)
{
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;

    Allocator vkAllocator;

    VkAllocationCallbacks* local_pAllocator = nullptr;
    if (pAllocator) {
        local_pAllocator = (VkAllocationCallbacks*)malloc(sizeof(VkAllocationCallbacks));
        if (!local_pAllocator) {
            ALOGE("DestroySamplerYcbcrConversion: Failed to allocate memory for VkAllocationCallbacks");
            return;
        }
        deepcopy_VkAllocationCallbacks(
            &vkAllocator,
            VK_STRUCTURE_TYPE_MAX_ENUM,
            pAllocator,
            local_pAllocator);
    }

    size_t byte_count = 0;
    if (pAllocator) {
        count_VkAllocationCallbacks(
            0,
            VK_STRUCTURE_TYPE_MAX_ENUM,
            local_pAllocator,
            &byte_count);
    }
    byte_count += 24; // 8 bytes for device, 8 bytes for ycbcrConversion, 8 bytes for allocator pointer

    char* send_buffer = (char*)mgr.addExternalParamPtr(byte_count);
    uint8_t** stream_ptr = (uint8_t**)&send_buffer;

    uint64_t guest_device = (uint64_t)(uintptr_t)device;
    uint64_t guest_ycbcrConversion = (uint64_t)(uintptr_t)ycbcrConversion;

    memcpy(*stream_ptr, &guest_device, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);

    memcpy(*stream_ptr, &guest_ycbcrConversion, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);

    uint64_t allocPtr = (uint64_t)(uintptr_t)pAllocator;
    memcpy(*stream_ptr, &allocPtr, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);
    if (pAllocator) {
        encode_to_stream_VkAllocationCallbacks(
            VK_STRUCTURE_TYPE_MAX_ENUM,
            local_pAllocator,
            stream_ptr);
    }

    // mgr.addParam64((uint64_t)(uintptr_t)device);
    // mgr.addParam64((uint64_t)(uintptr_t)ycbcrConversion);

    FlimeGuestWrite(&mgr,
        express_gpu,
        EXPRESS_GPU_DEVICE_ID,
        FUNID_vkDestroySamplerYcbcrConversion,
        false);

    if (local_pAllocator) {
        free(local_pAllocator);
    }

    ALOGI("DestroySamplerYcbcrConversion: Sent destroy request for ycbcrConversion %lld", (long long)guest_ycbcrConversion);
}

VKAPI_ATTR void VKAPI_CALL DestroySemaphore(
    VkDevice device,
    VkSemaphore semaphore,
    const VkAllocationCallbacks* pAllocator)
{
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;

    Allocator vkAllocator;

    VkAllocationCallbacks* local_pAllocator = nullptr;
    if (pAllocator) {
        local_pAllocator = (VkAllocationCallbacks*)malloc(sizeof(VkAllocationCallbacks));
        if (!local_pAllocator) {
            ALOGE("DestroySemaphore: Failed to allocate memory for VkAllocationCallbacks");
            return;
        }
        deepcopy_VkAllocationCallbacks(
            &vkAllocator,
            VK_STRUCTURE_TYPE_MAX_ENUM,
            pAllocator,
            local_pAllocator);
    }

    size_t byte_count = 0;
    if (pAllocator) {
        count_VkAllocationCallbacks(
            0,
            VK_STRUCTURE_TYPE_MAX_ENUM,
            local_pAllocator,
            &byte_count);
    }
    byte_count += 24; // 8 bytes for device, 8 bytes for semaphore, 8 bytes for allocator pointer

    char* send_buffer = (char*)mgr.addExternalParamPtr(byte_count);
    uint8_t** stream_ptr = (uint8_t**)&send_buffer;

    uint64_t guest_device = (uint64_t)(uintptr_t)device;
    uint64_t guest_semaphore = (uint64_t)(uintptr_t)semaphore;

    memcpy(*stream_ptr, &guest_device, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);

    memcpy(*stream_ptr, &guest_semaphore, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);

    uint64_t allocPtr = (uint64_t)(uintptr_t)pAllocator;
    memcpy(*stream_ptr, &allocPtr, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);
    if (pAllocator) {
        encode_to_stream_VkAllocationCallbacks(
            VK_STRUCTURE_TYPE_MAX_ENUM,
            local_pAllocator,
            stream_ptr);
    }

    // mgr.addParam64((uint64_t)(uintptr_t)device);
    // mgr.addParam64((uint64_t)(uintptr_t)semaphore);

    FlimeGuestWrite(&mgr,
        express_gpu,
        EXPRESS_GPU_DEVICE_ID,
        FUNID_vkDestroySemaphore,
        false);

    if (local_pAllocator) {
        free(local_pAllocator);
    }

    ALOGI("DestroySemaphore: Sent destroy request for semaphore %lld", (long long)guest_semaphore);
}

VKAPI_ATTR void VKAPI_CALL DestroyShaderModule(
    VkDevice device,
    VkShaderModule shaderModule,
    const VkAllocationCallbacks* pAllocator)
{
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;

    Allocator vkAllocator;

    VkAllocationCallbacks* local_pAllocator = nullptr;
    if (pAllocator) {
        local_pAllocator = (VkAllocationCallbacks*)malloc(sizeof(VkAllocationCallbacks));
        if (!local_pAllocator) {
            ALOGE("DestroyShaderModule: Failed to allocate memory for VkAllocationCallbacks");
            return;
        }
        deepcopy_VkAllocationCallbacks(
            &vkAllocator,
            VK_STRUCTURE_TYPE_MAX_ENUM,
            pAllocator,
            local_pAllocator);
    }

    size_t byte_count = 0;
    if (pAllocator) {
        count_VkAllocationCallbacks(
            0,
            VK_STRUCTURE_TYPE_MAX_ENUM,
            local_pAllocator,
            &byte_count);
    }
    byte_count += 24; // 8 bytes for device, 8 bytes for shaderModule, 8 bytes for allocator pointer

    char* send_buffer = (char*)mgr.addExternalParamPtr(byte_count);
    uint8_t** stream_ptr = (uint8_t**)&send_buffer;

    uint64_t guest_device = (uint64_t)(uintptr_t)device;
    uint64_t guest_shaderModule = (uint64_t)(uintptr_t)shaderModule;

    memcpy(*stream_ptr, &guest_device, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);

    memcpy(*stream_ptr, &guest_shaderModule, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);

    uint64_t allocPtr = (uint64_t)(uintptr_t)pAllocator;
    memcpy(*stream_ptr, &allocPtr, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);
    if (pAllocator) {
        encode_to_stream_VkAllocationCallbacks(
            VK_STRUCTURE_TYPE_MAX_ENUM,
            local_pAllocator,
            stream_ptr);
    }

    // mgr.addParam64((uint64_t)(uintptr_t)device);
    // mgr.addParam64((uint64_t)(uintptr_t)shaderModule);

    FlimeGuestWrite(&mgr,
        express_gpu,
        EXPRESS_GPU_DEVICE_ID,
        FUNID_vkDestroyShaderModule,
        false);

    if (local_pAllocator) {
        free(local_pAllocator);
    }

    ALOGI("DestroyShaderModule: Sent destroy request for shaderModule %lld", (long long)guest_shaderModule);
}

VKAPI_ATTR void VKAPI_CALL DestroySurfaceKHR(
    VkInstance instance,
    VkSurfaceKHR surface,
    const VkAllocationCallbacks* pAllocator)
{
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;

    Allocator vkAllocator;

    VkAllocationCallbacks* local_pAllocator = nullptr;
    if (pAllocator) {
        local_pAllocator = (VkAllocationCallbacks*)malloc(sizeof(VkAllocationCallbacks));
        if (!local_pAllocator) {
            ALOGE("DestroySurfaceKHR: Failed to allocate memory for VkAllocationCallbacks");
            return;
        }
        deepcopy_VkAllocationCallbacks(
            &vkAllocator,
            VK_STRUCTURE_TYPE_MAX_ENUM,
            pAllocator,
            local_pAllocator);
    }

    size_t byte_count = 0;
    if (pAllocator) {
        count_VkAllocationCallbacks(
            0,
            VK_STRUCTURE_TYPE_MAX_ENUM,
            local_pAllocator,
            &byte_count);
    }
    byte_count += 24; // 8 bytes for instance, 8 bytes for surface, 8 bytes for allocator pointer

    char* send_buffer = (char*)mgr.addExternalParamPtr(byte_count);
    uint8_t** stream_ptr = (uint8_t**)&send_buffer;

    uint64_t guest_instance = (uint64_t)(uintptr_t)instance;
    uint64_t guest_surface = (uint64_t)(uintptr_t)surface;

    memcpy(*stream_ptr, &guest_instance, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);

    memcpy(*stream_ptr, &guest_surface, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);

    uint64_t allocPtr = (uint64_t)(uintptr_t)pAllocator;
    memcpy(*stream_ptr, &allocPtr, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);
    if (pAllocator) {
        encode_to_stream_VkAllocationCallbacks(
            VK_STRUCTURE_TYPE_MAX_ENUM,
            local_pAllocator,
            stream_ptr);
    }

    // mgr.addParam64((uint64_t)(uintptr_t)device);
    // mgr.addParam64((uint64_t)(uintptr_t)surface);

    FlimeGuestWrite(&mgr,
        express_gpu,
        EXPRESS_GPU_DEVICE_ID,
        FUNID_vkDestroySurfaceKHR,
        false);

    if (local_pAllocator) {
        free(local_pAllocator);
    }

    ALOGI("DestroySurfaceKHR: Sent destroy request for surface %lld", (long long)guest_surface);
}

VKAPI_ATTR void VKAPI_CALL DestroySwapchainKHR(
    VkDevice device,
    VkSwapchainKHR swapchain,
    const VkAllocationCallbacks* pAllocator)
{
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;

    Allocator vkAllocator;

    VkAllocationCallbacks* local_pAllocator = nullptr;
    if (pAllocator) {
        local_pAllocator = (VkAllocationCallbacks*)malloc(sizeof(VkAllocationCallbacks));
        if (!local_pAllocator) {
            ALOGE("DestroySwapchainKHR: Failed to allocate memory for VkAllocationCallbacks");
            return;
        }
        deepcopy_VkAllocationCallbacks(
            &vkAllocator,
            VK_STRUCTURE_TYPE_MAX_ENUM,
            pAllocator,
            local_pAllocator);
    }

    size_t byte_count = 0;
    if (pAllocator) {
        count_VkAllocationCallbacks(
            0,
            VK_STRUCTURE_TYPE_MAX_ENUM,
            local_pAllocator,
            &byte_count);
    }
    byte_count += 24; // 8 bytes for device, 8 bytes for swapchain, 8 bytes for allocator pointer

    char* send_buffer = (char*)mgr.addExternalParamPtr(byte_count);
    uint8_t** stream_ptr = (uint8_t**)&send_buffer;

    uint64_t guest_device = (uint64_t)(uintptr_t)device;
    uint64_t guest_swapchain = (uint64_t)(uintptr_t)swapchain;

    memcpy(*stream_ptr, &guest_device, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);

    memcpy(*stream_ptr, &guest_swapchain, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);

    uint64_t allocPtr = (uint64_t)(uintptr_t)pAllocator;
    memcpy(*stream_ptr, &allocPtr, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);
    if (pAllocator) {
        encode_to_stream_VkAllocationCallbacks(
            VK_STRUCTURE_TYPE_MAX_ENUM,
            local_pAllocator,
            stream_ptr);
    }

    // mgr.addParam64((uint64_t)(uintptr_t)device);
    // mgr.addParam64((uint64_t)(uintptr_t)swapchain);

    FlimeGuestWrite(&mgr,
        express_gpu,
        EXPRESS_GPU_DEVICE_ID,
        FUNID_vkDestroySwapchainKHR,
        false);

    if (local_pAllocator) {
        free(local_pAllocator);
    }

    ALOGI("DestroySwapchainKHR: Sent destroy request for swapchain %lld", (long long)guest_swapchain);
}

// -----------------------------------------------------------------------------
// PhysicalDevice

VKAPI_ATTR VkResult EnumeratePhysicalDevices(VkInstance instance,
                                              uint32_t* pPhysicalDeviceCount,
                                              VkPhysicalDevice* pPhysicalDevices) {
    if (pPhysicalDeviceCount == nullptr) {
        ALOGE("EnumeratePhysicalDevices called with null pPhysicalDeviceCount");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;

    uint64_t guest_inst = (uint64_t)(uintptr_t)instance;
    uint32_t requested_count = pPhysicalDevices ? *pPhysicalDeviceCount : 0;

    const VkAllocationCallbacks* alloc = &kDefaultAllocCallbacks;

    for (uint32_t i = 0; i < requested_count; ++i) {
        VkPhysicalDevice_T* dev =
            static_cast<VkPhysicalDevice_T*>(alloc->pfnAllocation(
                alloc->pUserData,
                sizeof(VkPhysicalDevice_T),
                alignof(VkPhysicalDevice_T),
                VK_SYSTEM_ALLOCATION_SCOPE_DEVICE));
        if (!dev) {
            for (uint32_t j = 0; j < i; ++j) {
                alloc->pfnFree(alloc->pUserData, (void*)pPhysicalDevices[j]);
                pPhysicalDevices[j] = VK_NULL_HANDLE;
            }
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        dev->dispatch.magic = HWVULKAN_DISPATCH_MAGIC;
        pPhysicalDevices[i] = (VkPhysicalDevice)dev;
    }

    mgr.addParam64(guest_inst);
    mgr.addPtr(pPhysicalDeviceCount, sizeof(uint32_t));
    if (pPhysicalDevices && requested_count > 0) {
        mgr.addPtr(pPhysicalDevices, sizeof(VkPhysicalDevice) * requested_count);
    }

    VkResult vkResult = VK_SUCCESS;
    mgr.addPtr(&vkResult, sizeof(VkResult));

    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID,
              FUNID_vkEnumeratePhysicalDevices, true);

    uint32_t returned_count = *pPhysicalDeviceCount;
    if (pPhysicalDevices && returned_count > requested_count) {
        returned_count = requested_count;
    }

    if (vkResult != VK_SUCCESS && vkResult != VK_INCOMPLETE) {
        for (uint32_t i = 0; i < requested_count; ++i) {
            alloc->pfnFree(alloc->pUserData, (void*)pPhysicalDevices[i]);
            pPhysicalDevices[i] = VK_NULL_HANDLE;
        }
    } else if (pPhysicalDevices && requested_count > returned_count) {
        for (uint32_t i = returned_count; i < requested_count; ++i) {
            alloc->pfnFree(alloc->pUserData, (void*)pPhysicalDevices[i]);
            pPhysicalDevices[i] = VK_NULL_HANDLE;
        }
    }

    ALOGI("EnumeratePhysicalDevices %lld %lld", (long long)*pPhysicalDeviceCount, (long long)vkResult);
    if (pPhysicalDevices) {
        for (uint32_t i = 0; i < returned_count; ++i) {
            ALOGI("physical device %d %lld", i, (long long)pPhysicalDevices[i]);
        }
    }
    return vkResult;
}

VKAPI_ATTR VkResult EnumerateDeviceLayerProperties(VkPhysicalDevice physicalDevice,
                                                   uint32_t* pPropertyCount,
                                                   VkLayerProperties* pProperties) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    uint64_t guest_dev = (uint64_t)(uintptr_t)physicalDevice;
    
    mgr.addParam64(guest_dev);
    mgr.addPtr(pPropertyCount, sizeof(uint32_t));
    if (pProperties && *pPropertyCount > 0) {
        mgr.addPtr(pProperties, sizeof(VkLayerProperties) * (*pPropertyCount));
    }
    
    VkResult vkResult = VK_SUCCESS;
    mgr.addPtr(&vkResult, sizeof(VkResult));
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkEnumerateDeviceLayerProperties, true);
    
    return vkResult;
}

// VkResult EnumerateDeviceExtensionProperties(VkPhysicalDevice /*gpu*/,
//                                             const char* layer_name,
//                                             uint32_t* count,
//                                             VkExtensionProperties* properties) {
//     ALOGI("EnumerateDeviceExtensionProperties !%s", layer_name);
//     if (layer_name) {
//         ALOGW(
//             "Driver vkEnumerateDeviceExtensionProperties shouldn't be called "
//             "with a layer name ('%s')",
//             layer_name);
//         *count = 0;
//         return VK_SUCCESS;
//     }


//     const VkExtensionProperties kExtensions[] = {
//         { VK_ANDROID_NATIVE_BUFFER_EXTENSION_NAME,
//           VK_ANDROID_NATIVE_BUFFER_SPEC_VERSION },
//         { VK_KHR_SWAPCHAIN_EXTENSION_NAME,
//           VK_KHR_SWAPCHAIN_SPEC_VERSION }
//     };
//     const uint32_t kExtensionsCount =
//         sizeof(kExtensions) / sizeof(kExtensions[0]);


//     if (!properties || *count > kExtensionsCount)
//         *count = kExtensionsCount;
//     if (properties)
//         std::copy(kExtensions, kExtensions + *count, properties);
//     return *count < kExtensionsCount ? VK_INCOMPLETE : VK_SUCCESS;
// }
VKAPI_ATTR VkResult EnumerateDeviceExtensionProperties(
    VkPhysicalDevice physicalDevice,
    const char* pLayerName,
    uint32_t* pPropertyCount,
    VkExtensionProperties* pProperties) {
    
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    // Android specific device extensions
    const VkExtensionProperties kAndroidExtensions[] = {
        { VK_ANDROID_NATIVE_BUFFER_EXTENSION_NAME, VK_ANDROID_NATIVE_BUFFER_SPEC_VERSION },
        { VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_SWAPCHAIN_SPEC_VERSION }
    };
    const uint32_t kAndroidExtensionsCount = sizeof(kAndroidExtensions) / sizeof(kAndroidExtensions[0]);
    
    // Prepare parameters
    uint64_t guest_device = (uint64_t)(uintptr_t)physicalDevice;
    uint32_t has_layer = pLayerName ? 1 : 0;
    size_t layer_name_size = pLayerName ? strlen(pLayerName) + 1 : 0;
    char* buffer = (char*)mgr.addExternalParamPtr(sizeof(uint64_t) + sizeof(uint32_t) + layer_name_size);
    memcpy(buffer, &guest_device, sizeof(uint64_t));
    memcpy(buffer + sizeof(uint64_t), &has_layer, sizeof(uint32_t));
    if (pLayerName) {
        strcpy(buffer + sizeof(uint64_t) + sizeof(uint32_t), pLayerName);
    }
    
    uint32_t host_count = 0;
    VkExtensionProperties* host_properties = nullptr;
    if (pProperties) {
        host_properties = (VkExtensionProperties*)malloc(*pPropertyCount * sizeof(VkExtensionProperties));
    }
    
    mgr.addPtr(pPropertyCount, sizeof(uint32_t));
    if (host_properties) {
        mgr.addPtr(host_properties, *pPropertyCount * sizeof(VkExtensionProperties));
    }
    VkResult vkResult = VK_SUCCESS;
    
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkEnumerateDeviceExtensionProperties, true);
    
    if (vkResult != VK_SUCCESS) {
        if (host_properties) free(host_properties);
        return vkResult;
    }
    
    // Read host count from updated pPropertyCount
    host_count = *pPropertyCount;

    // Filter extensions whose VkPhysicalDeviceProperties2 / VkPhysicalDeviceFeatures2
    // structs require a pNext chain to be populated.  The guest transport only
    // serialises sizeof(base-struct) bytes – the chained extension structs are
    // never sent to / received from the host, so they stay zero-initialised.
    // Advertising such extensions causes libraries like ncnn to query properties
    // that come back all-zeros, then crash (e.g. least_common_multiple(64, 0)).
    // NOTE: when pProperties==nullptr we cannot filter (no names available), so
    // the count may be slightly over-stated; callers must tolerate that per spec.
    if (host_properties) {
        static const char* const kPNextDependentExts[] = {
            "VK_KHR_robustness2",    // robustStorageBufferAccessSizeAlignment -> crash
            "VK_EXT_robustness2",    // same struct via EXT alias
            // Cooperative matrix: feature/property structs live in pNext.
            // If cooperativeMatrix appears true but the matrix-property query
            // function is not forwarded, coopmat_M/N/K stay 0 → FPE_INTDIV.
            "VK_KHR_cooperative_matrix",
            "VK_NV_cooperative_matrix",
            "VK_NV_cooperative_matrix2",
            // Subgroup-size-control properties (minSubgroupSize, etc.) also
            // live in a pNext chain.  When not forwarded they stay 0 and
            // ncnn divides maxComputeWorkGroupInvocations by subgroupSize → FPE.
            "VK_EXT_subgroup_size_control",
            "VK_KHR_shader_subgroup_extended_types",
            // Cooperative vector (NV) — same pNext issue
            "VK_NV_cooperative_vector",
            // Extend this list for any other ext whose Properties/Features live
            // exclusively in a pNext-chained struct.
            // VK_KHR_push_descriptor: ncnn resolves vkCmdPushDescriptorSetWithTemplateKHR
            // and vkCmdPushDescriptorSetKHR via vkGetDeviceProcAddr at inference time.
            // These command-level functions have NO implementation in the guest HAL
            // (express-gpu protocol has no push-descriptor encoding), so GetDeviceProcAddr
            // returns NULL → SIGSEGV when ncnn calls them after vkCmdBindPipeline.
            // Blocking the extension forces ncnn into the fallback descriptor path
            // (vkUpdateDescriptorSetWithTemplate + vkCmdBindDescriptorSets), which works.
            "VK_KHR_push_descriptor",
            // Extend this list for any other ext whose Properties/Features live
            // exclusively in a pNext-chained struct, or whose command functions
            // lack a guest-HAL implementation.
        };
        uint32_t out = 0;
        for (uint32_t i = 0; i < host_count; i++) {
            bool drop = false;
            for (const char* name : kPNextDependentExts) {
                if (strcmp(host_properties[i].extensionName, name) == 0) {
                    drop = true;
                    break;
                }
            }
            if (drop) {
                ALOGI("EnumerateDeviceExtensionProperties: dropping pNext-dep ext: %s (guest transport cannot relay pNext chain)",
                      host_properties[i].extensionName);
            } else {
                host_properties[out++] = host_properties[i];
            }
        }
        host_count = out;
    }

    // Merge Android extensions with host extensions
    uint32_t total_count = host_count + kAndroidExtensionsCount;
    
    if (!pProperties) {
        *pPropertyCount = total_count;
        if (host_properties) free(host_properties);
        return VK_SUCCESS;
    }
    
    uint32_t copy_count = (*pPropertyCount < total_count) ? *pPropertyCount : total_count;
    uint32_t android_copy = (copy_count < kAndroidExtensionsCount) ? copy_count : kAndroidExtensionsCount;
    uint32_t host_copy = copy_count - android_copy;
    ALOGI("EnumerateDeviceExtensionProperties: %d total extensions (after pNext filter), %d Android, %d host", total_count, android_copy, host_copy);
    
    // Copy Android extensions first
    for (uint32_t i = 0; i < android_copy; ++i) {
        pProperties[i] = kAndroidExtensions[i];
    }
    
    // Copy host extensions
    for (uint32_t i = 0; i < host_copy; ++i) {
        pProperties[android_copy + i] = host_properties[i];
    }
    
    *pPropertyCount = total_count;
    if (host_properties) free(host_properties);
    ALOGI("EnumerateDeviceExtensionProperties: %d extensions returned", *pPropertyCount);
    
    return VK_SUCCESS;
}

void GetPhysicalDeviceProperties(VkPhysicalDevice physical_device,
                                 VkPhysicalDeviceProperties* properties) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;

    mgr.addParam64((uint64_t)(uintptr_t)physical_device);
    mgr.addPtr(properties, sizeof(VkPhysicalDeviceProperties));
    FlimeGuestWrite(&mgr,
        express_gpu,
        EXPRESS_GPU_DEVICE_ID,
        FUNID_vkGetPhysicalDeviceProperties,
        true);
    ALOGI("get FUNID_vkGetPhysicalDeviceProperties result %d %d %d %d",
          properties->apiVersion, properties->driverVersion,
          properties->vendorID, properties->deviceID);
}

void GetPhysicalDeviceProperties2KHR(VkPhysicalDevice physical_device,
                                  VkPhysicalDeviceProperties2KHR* properties) {
    GetPhysicalDeviceProperties(physical_device, &properties->properties);

    ALOGI("Properties2KHR: vendorID=0x%x deviceID=0x%x pNext=%p",
          properties->properties.vendorID, properties->properties.deviceID,
          properties->pNext);

    // Walk the pNext chain and handle known structs.
    // Because the guest transport does NOT relay the pNext chain to the host,
    // all chained structs stay zero-initialised.  Any struct whose fields are
    // later used as divisors (e.g. subgroupSize) MUST be patched up here to
    // avoid SIGFPE.
    int nodeCount = 0;
    VkBaseOutStructure* node =
        reinterpret_cast<VkBaseOutStructure*>(properties->pNext);
    while (node) {
        ALOGI("Properties2KHR: pNext node[%d] sType=%d", nodeCount++, node->sType);
        switch (static_cast<int>(node->sType)) {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENTATION_PROPERTIES_ANDROID: {
            auto* p = reinterpret_cast<VkPhysicalDevicePresentationPropertiesANDROID*>(node);
            p->sharedImage = VK_TRUE;
        } break;

        // ---- Vulkan 1.1 core subgroup properties ----
        // ncnn divides maxComputeWorkGroupInvocations by subgroupSize.
        // If subgroupSize is left at 0 from the unfilled pNext → SIGFPE.
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES: {
            auto* p = reinterpret_cast<VkPhysicalDeviceSubgroupProperties*>(node);
            if (p->subgroupSize == 0) {
                // Provide safe defaults based on vendor (same logic ncnn uses
                // for pre-1.1 fallback in gpu.cpp line 1050-1058).
                uint32_t vid = properties->properties.vendorID;
                if      (vid == 0x5143) p->subgroupSize = 128; // Qualcomm Adreno
                else if (vid == 0x13b5) p->subgroupSize = 16;  // ARM Mali
                else if (vid == 0x1010) p->subgroupSize = 32;  // ImgTec PowerVR
                else if (vid == 0x1002) p->subgroupSize = 64;  // AMD
                else if (vid == 0x10de) p->subgroupSize = 32;  // NVIDIA
                else if (vid == 0x8086) p->subgroupSize = 32;  // Intel
                else                    p->subgroupSize = 32;  // safe fallback
                // Mark basic compute+ballot support so ncnn can use the device.
                p->supportedStages    = VK_SHADER_STAGE_COMPUTE_BIT;
                p->supportedOperations = VK_SUBGROUP_FEATURE_BASIC_BIT
                                       | VK_SUBGROUP_FEATURE_BALLOT_BIT
                                       | VK_SUBGROUP_FEATURE_SHUFFLE_BIT;
                p->quadOperationsInAllStages = VK_FALSE;
                ALOGI("GetPhysicalDeviceProperties2KHR: patched subgroupSize=0 → %u (vendorID=0x%x)",
                      p->subgroupSize, vid);
            }
        } break;

        // ---- VK_EXT_subgroup_size_control / Vulkan 1.3 ----
        // minSubgroupSize=0 also causes div-by-zero in ncnn.
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES_EXT: {
            auto* p = reinterpret_cast<VkPhysicalDeviceSubgroupSizeControlPropertiesEXT*>(node);
            if (p->minSubgroupSize == 0 || p->maxSubgroupSize == 0) {
                // Walk the chain again to find the subgroup size we just patched
                uint32_t ss = 32; // fallback
                VkBaseOutStructure* scan = reinterpret_cast<VkBaseOutStructure*>(properties->pNext);
                while (scan) {
                    if (scan->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES) {
                        ss = reinterpret_cast<VkPhysicalDeviceSubgroupProperties*>(scan)->subgroupSize;
                        break;
                    }
                    scan = scan->pNext;
                }
                if (ss == 0) ss = 32;
                p->minSubgroupSize = ss;
                p->maxSubgroupSize = ss;
                p->maxComputeWorkgroupSubgroups =
                    properties->properties.limits.maxComputeWorkGroupInvocations / ss;
                if (p->maxComputeWorkgroupSubgroups == 0)
                    p->maxComputeWorkgroupSubgroups = 1;
                ALOGI("GetPhysicalDeviceProperties2KHR: patched subgroup_size_control min=%u max=%u",
                      p->minSubgroupSize, p->maxSubgroupSize);
            }
        } break;

        default:
            // Silently ignore other extension query structs not yet handled.
            break;
        }
        node = node->pNext;
    }
    ALOGI("Properties2KHR: pNext walk done, %d nodes visited", nodeCount);
}

void GetPhysicalDeviceQueueFamilyProperties(
    VkPhysicalDevice physicalDevice,
    uint32_t* count,
    VkQueueFamilyProperties* properties) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    char* buf = (char*)mgr.addExternalParamPtr(sizeof(uint64_t));
    uint64_t guest_physical_device = (uint64_t)(uintptr_t)physicalDevice;
    memcpy(buf, &guest_physical_device, sizeof(uint64_t));
    mgr.addPtr(count, sizeof(uint32_t));
    mgr.addPtr(properties, (properties && count && *count > 0) ? sizeof(VkQueueFamilyProperties) * (*count) : 0);
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkGetPhysicalDeviceQueueFamilyProperties, true);
    ALOGI("get FUNID_vkGetPhysicalDeviceQueueFamilyProperties result %lld", (long long)count);
}

void GetPhysicalDeviceQueueFamilyProperties2KHR(VkPhysicalDevice physical_device, uint32_t* count, VkQueueFamilyProperties2KHR* properties) {
    // note: even though multiple structures, this is safe to forward in this
    // case since we only expose one queue family.
    GetPhysicalDeviceQueueFamilyProperties(physical_device, count, properties ? &properties->queueFamilyProperties : nullptr);
}

void GetPhysicalDeviceMemoryProperties(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceMemoryProperties* properties) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    char* buf = (char*)mgr.addExternalParamPtr(sizeof(uint64_t));
    uint64_t guest_physical_device = (uint64_t)(uintptr_t)physicalDevice;
    memcpy(buf, &guest_physical_device, sizeof(uint64_t));
    mgr.addPtr(properties, sizeof(VkPhysicalDeviceMemoryProperties));
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkGetPhysicalDeviceMemoryProperties, true);

    ALOGI("get FUNID_vkGetPhysicalDeviceMemoryProperties result %d %d %d", properties->memoryTypeCount, properties->memoryTypes[0].propertyFlags, properties->memoryHeaps[0].flags);
}

void GetPhysicalDeviceMemoryProperties2KHR(VkPhysicalDevice physical_device, VkPhysicalDeviceMemoryProperties2KHR* properties) {
    GetPhysicalDeviceMemoryProperties(physical_device, &properties->memoryProperties);
}

VKAPI_ATTR void GetPhysicalDeviceFeatures(VkPhysicalDevice physicalDevice,
                                          VkPhysicalDeviceFeatures* pFeatures) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    mgr.addParam64((uint64_t)(uintptr_t)physicalDevice);
    mgr.addPtr(pFeatures, sizeof(VkPhysicalDeviceFeatures));
    
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkGetPhysicalDeviceFeatures, true);
}

void GetPhysicalDeviceFeatures2KHR(VkPhysicalDevice physical_device, VkPhysicalDeviceFeatures2KHR* features) {
    GetPhysicalDeviceFeatures(physical_device, &features->features);
}

// -----------------------------------------------------------------------------
// Device

// VkResult CreateDevice(VkPhysicalDevice physical_device,
//                       const VkDeviceCreateInfo* create_info,
//                       const VkAllocationCallbacks* allocator,
//                       VkDevice* out_device) {
//     ALOGI("CreateDevice");
//     VkInstance_T* instance = GetInstanceFromPhysicalDevice(physical_device);
//     if (!allocator)
//         allocator = &instance->allocator;
//     VkDevice_T* device = static_cast<VkDevice_T*>(allocator->pfnAllocation(
//         allocator->pUserData, sizeof(VkDevice_T), alignof(VkDevice_T),
//         VK_SYSTEM_ALLOCATION_SCOPE_DEVICE));
//     if (!device)
//         return VK_ERROR_OUT_OF_HOST_MEMORY;

//     device->dispatch.magic = HWVULKAN_DISPATCH_MAGIC;
//     device->allocator = *allocator;
//     device->instance = instance;
//     device->queue.dispatch.magic = HWVULKAN_DISPATCH_MAGIC;
//     std::fill(device->next_handle.begin(), device->next_handle.end(),
//               UINT64_C(0));

//     for (uint32_t i = 0; i < create_info->enabledExtensionCount; i++) {
//         if (strcmp(create_info->ppEnabledExtensionNames[i],
//                    VK_ANDROID_NATIVE_BUFFER_EXTENSION_NAME) == 0) {
//             ALOGV("Enabling " VK_ANDROID_NATIVE_BUFFER_EXTENSION_NAME);
//         }
//     }

//     *out_device = device;
//     return VK_SUCCESS;
// }

// === guest.cpp ===
VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(
    VkPhysicalDevice                      physicalDevice,
    const VkDeviceCreateInfo*            pCreateInfo,
    const VkAllocationCallbacks*         pAllocator,
    VkDevice*                            pDevice) 
{

    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;

    ALOGI("CreateDevice: physicalDevice %lld, pCreateInfo %p, pAllocator %p, pDevice %p",
         (long long)physicalDevice, pCreateInfo, pAllocator, pDevice);

    Allocator vkAllocator;
    VkDeviceCreateInfo* local_pCreateInfo = nullptr;
    if (pCreateInfo) {
        local_pCreateInfo = (VkDeviceCreateInfo*)malloc(sizeof(VkDeviceCreateInfo));
        if (!local_pCreateInfo) {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        deepcopy_VkDeviceCreateInfo(
            &vkAllocator,
            VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            pCreateInfo,
            local_pCreateInfo);
    }

    VkAllocationCallbacks* local_pAllocator = nullptr;
    if (pAllocator) {
        local_pAllocator = (VkAllocationCallbacks*)malloc(sizeof(VkAllocationCallbacks));
        if (!local_pAllocator) {
            free(local_pCreateInfo);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        deepcopy_VkAllocationCallbacks(
            &vkAllocator,
            VK_STRUCTURE_TYPE_MAX_ENUM,
            pAllocator,
            local_pAllocator);
    }

    size_t byte_count = 0;
    count_VkDeviceCreateInfo(
        0,
        VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        local_pCreateInfo,
        &byte_count);
    if (pAllocator) {
        count_VkAllocationCallbacks(
            0,
            VK_STRUCTURE_TYPE_MAX_ENUM,
            local_pAllocator,
            &byte_count);
    }

    byte_count += 8;

    char* send_buffer = (char*)mgr.addExternalParamPtr(byte_count);
    uint8_t** stream_ptr = (uint8_t**)&send_buffer;

    encode_to_stream_VkDeviceCreateInfo(
        VK_STRUCTURE_TYPE_MAX_ENUM,
        local_pCreateInfo,
        stream_ptr);

    uint64_t guest_alloc_ptr = (uint64_t)(uintptr_t)local_pAllocator;
    memcpy(*stream_ptr, &guest_alloc_ptr, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);

    if (pAllocator) {
        encode_to_stream_VkAllocationCallbacks(
            VK_STRUCTURE_TYPE_MAX_ENUM,
            local_pAllocator,
            stream_ptr);
    }

    const VkAllocationCallbacks* useAlloc = pAllocator ? pAllocator : &kDefaultAllocCallbacks;
    VkDevice_T* device = static_cast<VkDevice_T*>(
        useAlloc->pfnAllocation(
            useAlloc->pUserData,
            sizeof(VkDevice_T),
            alignof(VkDevice_T),
            VK_SYSTEM_ALLOCATION_SCOPE_DEVICE));
    if (!device) {
        free(local_pCreateInfo);
        if (pAllocator) free(local_pAllocator);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    device->dispatch.magic = HWVULKAN_DISPATCH_MAGIC;
    device->allocator = *useAlloc;
    *pDevice = (VkDevice)device;


    mgr.addParam64((uint64_t)(uintptr_t)physicalDevice);
    mgr.addParam64((uint64_t)(uintptr_t)*pDevice);


    VkResult vkResult = VK_ERROR_DEVICE_LOST;
    mgr.addPtr(&vkResult, sizeof(VkResult));

    const ssize_t written = FlimeGuestWrite(&mgr,
        express_gpu,
        EXPRESS_GPU_DEVICE_ID,
        FUNID_vkCreateDevice,
        true);
    if (!IsCompleteParamManagerWrite(written, 2)) {
        vkResult = VK_ERROR_DEVICE_LOST;
    }
    if (vkResult == VK_SUCCESS) {
        FlimeGuestRegisterDevice(express_gpu, *pDevice);
    } else {
        useAlloc->pfnFree(useAlloc->pUserData, device);
        *pDevice = VK_NULL_HANDLE;
    }


    free(local_pCreateInfo);
    if (pAllocator) free(local_pAllocator);

    ALOGI("get create device result %d", vkResult);

    return vkResult;
}


// void DestroyDevice(VkDevice device,
//                    const VkAllocationCallbacks* /*allocator*/) {
//     if (!device)
//         return;
//     device->allocator.pfnFree(device->allocator.pUserData, device);
// }

// void GetDeviceQueue(VkDevice device, uint32_t, uint32_t, VkQueue* queue) {
//     ALOGI("GetDeviceQueue");
//     *queue = &device->queue;
// }

// === guest.cpp ===
VKAPI_ATTR void VKAPI_CALL GetDeviceQueue(
    VkDevice        device,
    uint32_t        queueFamilyIndex,
    uint32_t        queueIndex,
    VkQueue*        pQueue)
{

    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;



    const VkAllocationCallbacks* allocCb = &kDefaultAllocCallbacks;
    VkQueue_T* guestQueue = static_cast<VkQueue_T*>(
        allocCb->pfnAllocation(
            allocCb->pUserData,
            sizeof(VkQueue_T),
            alignof(VkQueue_T),
            VK_SYSTEM_ALLOCATION_SCOPE_OBJECT));
    if (guestQueue == nullptr) {
        *pQueue = VK_NULL_HANDLE;
        return;
    }
    guestQueue->dispatch.magic = HWVULKAN_DISPATCH_MAGIC;
    *pQueue = (VkQueue)guestQueue;


    uint64_t guest_dev_handle  = (uint64_t)(uintptr_t)device;
    uint64_t guest_queue_handle = (uint64_t)(uintptr_t)guestQueue;

    mgr.addParam64(guest_dev_handle);
    mgr.addParam32(queueFamilyIndex);
    mgr.addParam32(queueIndex);
    mgr.addParam64(guest_queue_handle);


    const ssize_t written = FlimeGuestWrite(&mgr,
        express_gpu,
        EXPRESS_GPU_DEVICE_ID,
        FUNID_vkGetDeviceQueue,
        false);
    if (IsCompleteParamManagerWrite(written, 1)) {
        FlimeGuestRegisterQueue(device, *pQueue);
    } else {
        allocCb->pfnFree(allocCb->pUserData, guestQueue);
        *pQueue = VK_NULL_HANDLE;
    }
    ALOGI("finish GetDeviceQueue %lld %d %d %lld",
          (long long)guest_dev_handle, queueFamilyIndex, queueIndex,
          (long long)guest_queue_handle);
}


// -----------------------------------------------------------------------------
// CommandPool

struct CommandPool {
    typedef VkCommandPool HandleType;
    VkAllocationCallbacks allocator;
};
DEFINE_OBJECT_HANDLE_CONVERSION(CommandPool)

VKAPI_ATTR VkResult VKAPI_CALL CreateCommandPool(
    VkDevice device,
    const VkCommandPoolCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkCommandPool* pCommandPool)
{
    ALOGI("CreateCommandPool with device %lld", (long long)device);
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    Allocator vkAllocator;
    
    VkCommandPoolCreateInfo* localInfo = (VkCommandPoolCreateInfo*)malloc(sizeof(VkCommandPoolCreateInfo));
    deepcopy_VkCommandPoolCreateInfo(&vkAllocator, VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, pCreateInfo, localInfo);
    
    VkAllocationCallbacks* localAlloc = nullptr;
    if (pAllocator) {
        localAlloc = (VkAllocationCallbacks*)malloc(sizeof(VkAllocationCallbacks));
        deepcopy_VkAllocationCallbacks(&vkAllocator, VK_STRUCTURE_TYPE_MAX_ENUM, pAllocator, localAlloc);
    }
    
    size_t byteCount = 0;
    count_VkCommandPoolCreateInfo(0, VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, localInfo, &byteCount);
    if (pAllocator) count_VkAllocationCallbacks(0, VK_STRUCTURE_TYPE_MAX_ENUM, localAlloc, &byteCount);
    byteCount += sizeof(uint64_t) * 3; // allocPtr + device + commandPool
    
    char* buf = (char*)mgr.addExternalParamPtr(byteCount);
    uint8_t** ptr = (uint8_t**)&buf;
    
    encode_to_stream_VkCommandPoolCreateInfo(VK_STRUCTURE_TYPE_MAX_ENUM, localInfo, ptr);
    
    uint64_t allocPtr = (uint64_t)(uintptr_t)pAllocator;
    memcpy(*ptr, &allocPtr, sizeof(uint64_t));
    *ptr += sizeof(uint64_t);
    
    if (pAllocator) encode_to_stream_VkAllocationCallbacks(VK_STRUCTURE_TYPE_MAX_ENUM, localAlloc, ptr);
    
    uint64_t devicePtr = (uint64_t)(uintptr_t)device;
    memcpy(*ptr, &devicePtr, sizeof(uint64_t));
    *ptr += sizeof(uint64_t);
    
    if (!pAllocator) pAllocator = &kDefaultAllocCallbacks;
    VkCommandPool_T* pool = static_cast<VkCommandPool_T*>(pAllocator->pfnAllocation(
        pAllocator->pUserData, sizeof(VkCommandPool_T), alignof(VkCommandPool_T),
        VK_SYSTEM_ALLOCATION_SCOPE_OBJECT));
    
    if (!pool) {
        mgr.clear();
        free(localInfo);
        if (localAlloc) free(localAlloc);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    
    pool->dispatch.magic = HWVULKAN_DISPATCH_MAGIC;
    *pCommandPool = (VkCommandPool)pool;
    
    uint64_t poolPtr = (uint64_t)(uintptr_t)*pCommandPool;
    memcpy(*ptr, &poolPtr, sizeof(uint64_t));
    *ptr += sizeof(uint64_t);

    VkResult vkResult = VK_ERROR_DEVICE_LOST;
    mgr.addPtr(&vkResult, sizeof(vkResult));
    FlimeGuestBeforeDescriptorLifecycle(device);
    const ssize_t written =
        FlimeGuestWrite(&mgr,
                        express_gpu,
                        EXPRESS_GPU_DEVICE_ID,
                        FUNID_vkCreateCommandPool,
                        true);
    const bool transport_ok = IsCompleteParamManagerWrite(written, 2);
    if (!transport_ok) {
        vkResult = VK_ERROR_DEVICE_LOST;
    }
    FlimeGuestAfterDescriptorLifecycle(device, transport_ok);
    if (vkResult != VK_SUCCESS) {
        pAllocator->pfnFree(pAllocator->pUserData, pool);
        *pCommandPool = VK_NULL_HANDLE;
    }
    
    ALOGI("CreateCommandPool device %lld pool %lld", (long long)device, (long long)*pCommandPool);
    
    free(localInfo);
    if (localAlloc) free(localAlloc);
    return vkResult;
}


// void DestroyCommandPool(VkDevice /*device*/,
//                         VkCommandPool cmd_pool,
//                         const VkAllocationCallbacks* /*allocator*/) {
//     CommandPool* pool = GetCommandPoolFromHandle(cmd_pool);
//     pool->allocator.pfnFree(pool->allocator.pUserData, pool);
// }

// -----------------------------------------------------------------------------
// CmdBuffer

VKAPI_ATTR VkResult VKAPI_CALL AllocateCommandBuffers(
    VkDevice device,
    const VkCommandBufferAllocateInfo* pAllocateInfo,
    VkCommandBuffer* pCommandBuffers)
{
    int express_gpu = get_express_gpu_fd();
    ParamManager mgr;
    Allocator vkAllocator;
    
    VkCommandBufferAllocateInfo* localInfo = (VkCommandBufferAllocateInfo*)malloc(sizeof(VkCommandBufferAllocateInfo));
    deepcopy_VkCommandBufferAllocateInfo(&vkAllocator, VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, pAllocateInfo, localInfo);
    
    size_t byteCount = 0;
    count_VkCommandBufferAllocateInfo(0, VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, localInfo, &byteCount);
    byteCount += sizeof(uint64_t) * 2; // device + commandBuffers array size
    byteCount += sizeof(uint64_t) * localInfo->commandBufferCount; // command buffer pointers
    
    char* buf = (char*)mgr.addExternalParamPtr(byteCount);
    uint8_t** ptr = (uint8_t**)&buf;
    
    encode_to_stream_VkCommandBufferAllocateInfo(VK_STRUCTURE_TYPE_MAX_ENUM, localInfo, ptr);
    
    uint64_t devicePtr = (uint64_t)(uintptr_t)device;
    memcpy(*ptr, &devicePtr, sizeof(uint64_t));
    *ptr += sizeof(uint64_t);
    
    const VkAllocationCallbacks* allocator = &kDefaultAllocCallbacks;
    for (uint32_t i = 0; i < localInfo->commandBufferCount; ++i) {
        VkCommandBuffer_T* cmdBuf = static_cast<VkCommandBuffer_T*>(allocator->pfnAllocation(
            allocator->pUserData, sizeof(VkCommandBuffer_T), alignof(VkCommandBuffer_T),
            VK_SYSTEM_ALLOCATION_SCOPE_OBJECT));
        
        if (!cmdBuf) {
            for (uint32_t j = 0; j < i; ++j) {
                allocator->pfnFree(allocator->pUserData,
                                   (void*)pCommandBuffers[j]);
                pCommandBuffers[j] = VK_NULL_HANDLE;
            }
            mgr.clear();
            free(localInfo);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        
        cmdBuf->dispatch.magic = HWVULKAN_DISPATCH_MAGIC;
        pCommandBuffers[i] = (VkCommandBuffer)cmdBuf;
        
        uint64_t cmdBufPtr = (uint64_t)(uintptr_t)pCommandBuffers[i];
        memcpy(*ptr, &cmdBufPtr, sizeof(uint64_t));
        *ptr += sizeof(uint64_t);
    }

    VkResult vkResult = VK_ERROR_DEVICE_LOST;
    mgr.addPtr(&vkResult, sizeof(vkResult));
    FlimeGuestBeforeDescriptorLifecycle(device);
    const ssize_t written =
        FlimeGuestWrite(&mgr,
                        express_gpu,
                        EXPRESS_GPU_DEVICE_ID,
                        FUNID_vkAllocateCommandBuffers,
                        true);
    const bool transport_ok = IsCompleteParamManagerWrite(written, 2);
    if (!transport_ok) {
        vkResult = VK_ERROR_DEVICE_LOST;
    }
    if (vkResult == VK_SUCCESS) {
        FlimeGuestRegisterCommandBuffers(
            device,
            pAllocateInfo->commandPool,
            pAllocateInfo->commandBufferCount,
            pCommandBuffers);
    } else {
        for (uint32_t i = 0; i < pAllocateInfo->commandBufferCount; ++i) {
            allocator->pfnFree(allocator->pUserData,
                               (void*)pCommandBuffers[i]);
            pCommandBuffers[i] = VK_NULL_HANDLE;
        }
    }
    FlimeGuestAfterDescriptorLifecycle(device, transport_ok);
    
    ALOGI("AllocateCommandBuffers device %lld count %d", (long long)device, localInfo->commandBufferCount);
    
    free(localInfo);
    return vkResult;
}

VKAPI_ATTR VkResult VKAPI_CALL BeginCommandBuffer(
    VkCommandBuffer commandBuffer,
    const VkCommandBufferBeginInfo* pBeginInfo)
{
    FlushPendingSubmitCohort("begin_command_buffer");

    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    Allocator vkAllocator;
    
    VkCommandBufferBeginInfo* localInfo = (VkCommandBufferBeginInfo*)malloc(sizeof(VkCommandBufferBeginInfo));
    deepcopy_VkCommandBufferBeginInfo(&vkAllocator, VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, pBeginInfo, localInfo);
    
    size_t byteCount = 0;
    count_VkCommandBufferBeginInfo(0, VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, localInfo, &byteCount);
    byteCount += sizeof(uint64_t); // commandBuffer
    
    char* buf = (char*)mgr.addExternalParamPtr(byteCount);
    uint8_t** ptr = (uint8_t**)&buf;
    
    encode_to_stream_VkCommandBufferBeginInfo(VK_STRUCTURE_TYPE_MAX_ENUM, localInfo, ptr);
    
    uint64_t cmdBufPtr = (uint64_t)(uintptr_t)commandBuffer;
    memcpy(*ptr, &cmdBufPtr, sizeof(uint64_t));
    *ptr += sizeof(uint64_t);

    VkResult vkResult = VK_ERROR_DEVICE_LOST;
    mgr.addPtr(&vkResult, sizeof(vkResult));
    
    const ssize_t written =
        FlimeGuestWrite(&mgr,
                        express_gpu,
                        EXPRESS_GPU_DEVICE_ID,
                        FUNID_vkBeginCommandBuffer,
                        true);
    if (!IsCompleteParamManagerWrite(written, 2)) {
        vkResult = VK_ERROR_DEVICE_LOST;
    }
    if (vkResult == VK_SUCCESS) {
        ForgetTrackedCommandBuffer(commandBuffer);
    }
    FlimeGuestBeginCommandBuffer(FUNID_vkBeginCommandBuffer,
                                 commandBuffer,
                                 pBeginInfo,
                                 vkResult,
                                 byteCount);
    
    ALOGI("BeginCommandBuffer %lld", (long long)commandBuffer);
    
    free(localInfo);
    return vkResult;
}
VKAPI_ATTR void VKAPI_CALL CmdBeginRenderPass(
    VkCommandBuffer commandBuffer,
    const VkRenderPassBeginInfo* pRenderPassBegin,
    VkSubpassContents contents)
{
    int express_gpu = get_express_gpu_fd();
    ParamManager mgr;
    Allocator vkAllocator;
    
    VkRenderPassBeginInfo* localInfo = (VkRenderPassBeginInfo*)malloc(sizeof(VkRenderPassBeginInfo));
    deepcopy_VkRenderPassBeginInfo(&vkAllocator, VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, pRenderPassBegin, localInfo);
    
    size_t byteCount = 0;
    count_VkRenderPassBeginInfo(0, VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, localInfo, &byteCount);
    byteCount += sizeof(uint64_t) + sizeof(uint32_t); // cmdBuf + contents
    
    char* buf = (char*)mgr.addExternalParamPtr(byteCount);
    uint8_t** ptr = (uint8_t**)&buf;
    
    uint64_t cmdBufPtr = (uint64_t)(uintptr_t)commandBuffer;
    memcpy(*ptr, &cmdBufPtr, sizeof(uint64_t));
    *ptr += sizeof(uint64_t);
    
    encode_to_stream_VkRenderPassBeginInfo(VK_STRUCTURE_TYPE_MAX_ENUM, localInfo, ptr);
    
    memcpy(*ptr, &contents, sizeof(uint32_t));
    *ptr += sizeof(uint32_t);
    
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdBeginRenderPass, false, commandBuffer);
    
    ALOGI("CmdBeginRenderPass %lld renderpass %lld", (long long)commandBuffer, (long long)pRenderPassBegin->renderPass);
    
    free(localInfo);
}


VKAPI_ATTR void VKAPI_CALL CmdBindPipeline(
    VkCommandBuffer commandBuffer,
    VkPipelineBindPoint pipelineBindPoint,
    VkPipeline pipeline)
{
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    size_t byteCount = sizeof(uint64_t) * 2 + sizeof(uint32_t); // cmdBuf + pipeline + bindPoint
    
    char* buf = (char*)mgr.addExternalParamPtr(byteCount);
    uint8_t** ptr = (uint8_t**)&buf;
    
    uint64_t cmdBufPtr = (uint64_t)(uintptr_t)commandBuffer;
    memcpy(*ptr, &cmdBufPtr, sizeof(uint64_t));
    *ptr += sizeof(uint64_t);
    
    memcpy(*ptr, &pipelineBindPoint, sizeof(uint32_t));
    *ptr += sizeof(uint32_t);
    
    uint64_t pipelinePtr = (uint64_t)(uintptr_t)pipeline;
    memcpy(*ptr, &pipelinePtr, sizeof(uint64_t));
    *ptr += sizeof(uint64_t);
    
    FlimeGuestCmdBindPipeline(FUNID_vkCmdBindPipeline,
                              commandBuffer,
                              pipelineBindPoint,
                              pipeline,
                              byteCount);
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdBindPipeline, false);
    
    ALOGI("CmdBindPipeline %lld pipeline %lld", (long long)commandBuffer, (long long)pipeline);
}


VKAPI_ATTR void VKAPI_CALL CmdBindVertexBuffers(
    VkCommandBuffer commandBuffer,
    uint32_t firstBinding,
    uint32_t bindingCount,
    const VkBuffer* pBuffers,
    const VkDeviceSize* pOffsets)
{
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    size_t byteCount = sizeof(uint64_t) + sizeof(uint32_t) * 2; // cmdBuf + firstBinding + bindingCount
    byteCount += sizeof(uint64_t) * bindingCount; // buffers
    byteCount += sizeof(VkDeviceSize) * bindingCount; // offsets
    
    char* buf = (char*)mgr.addExternalParamPtr(byteCount);
    uint8_t** ptr = (uint8_t**)&buf;
    
    uint64_t cmdBufPtr = (uint64_t)(uintptr_t)commandBuffer;
    memcpy(*ptr, &cmdBufPtr, sizeof(uint64_t));
    *ptr += sizeof(uint64_t);
    
    memcpy(*ptr, &firstBinding, sizeof(uint32_t));
    *ptr += sizeof(uint32_t);
    memcpy(*ptr, &bindingCount, sizeof(uint32_t));
    *ptr += sizeof(uint32_t);
    
    for (uint32_t i = 0; i < bindingCount; ++i) {
        uint64_t bufPtr = (uint64_t)(uintptr_t)pBuffers[i];
        memcpy(*ptr, &bufPtr, sizeof(uint64_t));
        *ptr += sizeof(uint64_t);
    }
    
    for (uint32_t i = 0; i < bindingCount; ++i) {
        memcpy(*ptr, &pOffsets[i], sizeof(VkDeviceSize));
        *ptr += sizeof(VkDeviceSize);
    }
    
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdBindVertexBuffers, false, commandBuffer);
    
    ALOGI("CmdBindVertexBuffers %lld first %d count %d", (long long)commandBuffer, firstBinding, bindingCount);
}

VKAPI_ATTR void FreeCommandBuffers(VkDevice device,
                                   VkCommandPool commandPool,
                                   uint32_t commandBufferCount,
                                   const VkCommandBuffer* pCommandBuffers) {
    FlushPendingSubmitCohort("free_command_buffers");
    if (kEnableDeferredFenceWait) {
        DrainDeferredQueues("free_command_buffers");
    }

    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;

    size_t count = sizeof(uint64_t) + sizeof(uint64_t) + sizeof(uint32_t) + 
                   commandBufferCount * sizeof(uint64_t);
    
    char* send_buffer = (char*)mgr.addExternalParamPtr(count);
    uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;

    uint64_t guest_device = (uint64_t)(uintptr_t)device;
    memcpy(*send_buffer_ptr, &guest_device, sizeof(uint64_t));
    *send_buffer_ptr += sizeof(uint64_t);
    
    uint64_t guest_command_pool = (uint64_t)(uintptr_t)commandPool;
    memcpy(*send_buffer_ptr, &guest_command_pool, sizeof(uint64_t));
    *send_buffer_ptr += sizeof(uint64_t);
    
    memcpy(*send_buffer_ptr, &commandBufferCount, sizeof(uint32_t));
    *send_buffer_ptr += sizeof(uint32_t);
    
    for (uint32_t i = 0; i < commandBufferCount; ++i) {
        uint64_t guest_cmd_buffer = (uint64_t)(uintptr_t)pCommandBuffers[i];
        memcpy(*send_buffer_ptr, &guest_cmd_buffer, sizeof(uint64_t));
        *send_buffer_ptr += sizeof(uint64_t);
    }
    
    FlimeGuestBeforeDescriptorLifecycle(device);
    FlimeGuestFreeCommandBuffers(device,
                                 commandPool,
                                 commandBufferCount,
                                 pCommandBuffers);
    const ssize_t written =
        FlimeGuestWrite(&mgr,
                        express_gpu,
                        EXPRESS_GPU_DEVICE_ID,
                        FUNID_vkFreeCommandBuffers,
                        true);
    
    // Free guest command buffer objects
    const VkAllocationCallbacks* alloc = &kDefaultAllocCallbacks;
    for (uint32_t i = 0; i < commandBufferCount; ++i) {
        ForgetTrackedCommandBuffer(pCommandBuffers[i]);
        alloc->pfnFree(alloc->pUserData, (void*)pCommandBuffers[i]);
    }
    FlimeGuestAfterDescriptorLifecycle(
        device, IsCompleteParamManagerWrite(written, 1));
}

// -----------------------------------------------------------------------------
// DeviceMemory


// #include <unordered_map>
// #include <mutex>
// #include <vector>

// struct ActiveMappedMemoryRecord {
//     VkDevice device;
//     VkDeviceMemory memory;
//     uint64_t size;
//     uint8_t* map_data;
//     std::vector<uint8_t> shadow;
// };

// struct BufferMemoryBindingRecord {
//     VkDevice device;
//     VkDeviceMemory memory;
//     VkDeviceSize memory_offset;
//     VkDeviceSize buffer_size;
// };

// enum class DescriptorBufferAccess : uint8_t {
//     kFlushOnly = 0,
//     kPotentialWrite = 1,
// };

// struct DescriptorBufferUse {
//     VkBuffer buffer;
//     VkDeviceSize offset;
//     VkDeviceSize range;
//     DescriptorBufferAccess access;
// };

// struct TrackedMemoryRange {
//     VkDevice device;
//     VkDeviceMemory memory;
//     VkDeviceSize offset;
//     VkDeviceSize size;
// };

// static std::unordered_map<VkDeviceMemory, ActiveMappedMemoryRecord> g_active_mapped_memories;
// static std::unordered_map<VkBuffer, BufferMemoryBindingRecord> g_buffer_memory_bindings;
// static std::unordered_map<VkBuffer, VkDeviceSize> g_buffer_sizes;
// static std::unordered_map<VkDescriptorSet, std::unordered_map<uint64_t, DescriptorBufferUse>> g_descriptor_set_buffer_uses;
// Descriptor hint cache globals are defined with the active sync-tracking maps above.
// Keep this old declaration block commented to avoid duplicate definitions.
// static std::unordered_map<VkDescriptorSet, uint64_t> g_descriptor_set_versions;
// static std::unordered_map<VkDescriptorSet, DescriptorSetSyncHintCacheEntry> g_descriptor_set_hint_cache;
// static uint64_t g_descriptor_set_global_version = 1;
// static std::unordered_map<VkCommandBuffer, std::unordered_set<VkDescriptorSet>> g_command_buffer_descriptor_sets;
// static std::unordered_set<VkDeviceMemory> g_flush_hint_memories;
// static std::unordered_set<VkDeviceMemory> g_invalidate_hint_memories;
// static std::unordered_set<VkDeviceMemory> g_recently_flushed_memories;
// static std::vector<TrackedMemoryRange> g_flush_hint_ranges;
// static std::mutex g_mapped_mutex;

VKAPI_ATTR VkResult FlushMappedMemoryRanges(VkDevice device, uint32_t memoryRangeCount, const VkMappedMemoryRange* pMemoryRanges);

struct TrackedRangeUploadCheckStats {
    uint64_t clean_cache_hits;
    uint64_t memcmp_checks;
    uint64_t memcmp_bytes;
    uint64_t dirty_ranges;
};

static void ResetImplicitCleanScanThrottleLocked() {
    g_no_hint_clean_scan_streak = 0;
    g_no_hint_clean_scan_skip_remaining = 0;
}

static bool TrackedRangeNeedsUploadLocked(const TrackedMemoryRange& range,
                                          VkDeviceSize min_bytes,
                                          VkDeviceSize max_bytes,
                                          TrackedMemoryRange* normalized) {
    if (range.memory == VK_NULL_HANDLE || range.size == 0) return false;
    auto it = g_active_mapped_memories.find(range.memory);
    if (it == g_active_mapped_memories.end()) return false;
    ActiveMappedMemoryRecord& rec = it->second;
    if (!rec.map_data || rec.size == 0) return false;
    if (!IsExpressVkRegisteredMemoryHandle(range.memory)) return false;

    const VkDeviceSize size = ClampMappedRangeSize(rec.size, range.offset, range.size);
    if (size < min_bytes || size > max_bytes) return false;

    if (rec.shadow.size() == rec.size &&
        memcmp(rec.map_data + range.offset, rec.shadow.data() + range.offset, (size_t)size) == 0) {
        return false;
    }

    if (normalized) {
        *normalized = {rec.device, rec.memory, range.offset, size};
    }
    return true;
}

static bool TrackedRangeNeedsSubmitUploadLocked(const TrackedMemoryRange& range,
                                                TrackedMemoryRange* normalized,
                                                TrackedRangeUploadCheckStats* stats,
                                                SyncPolicyBatch* policy_batch) {
    if (range.memory == VK_NULL_HANDLE || range.size == 0) return false;
    auto it = g_active_mapped_memories.find(range.memory);
    if (it == g_active_mapped_memories.end()) return false;
    ActiveMappedMemoryRecord& rec = it->second;
    if (!rec.map_data || rec.size == 0) return false;

    const VkDeviceSize size = ClampMappedRangeSize(rec.size, range.offset, range.size);
    if (size == 0) return false;
    const bool registered = IsExpressVkRegisteredMemoryHandle(range.memory);
    const uint32_t policy_key = MakeSyncPolicyKey(SyncPolicySource::kHintRange,
                                                  range.offset,
                                                  size,
                                                  registered,
                                                  rec.submit_clean_streak);
    const bool large_clean_candidate =
        SyncPolicyIsLargeCleanCacheCandidate(size, registered);
    if (large_clean_candidate) {
        NoteLargeCleanRangeCandidate();
    }
    uint64_t aggressive_verify_every = 0;
    const bool aggressive_clean_candidate =
        SyncPolicyIsAggressiveCleanCandidate(range.offset,
                                             size,
                                             registered,
                                             rec.submit_clean_streak,
                                             &aggressive_verify_every);
    const bool aggressive_sample_verify =
        aggressive_clean_candidate &&
        SyncPolicyShouldVerifyAggressiveClean(aggressive_verify_every);

    if (aggressive_clean_candidate && !aggressive_sample_verify) {
        if (rec.submit_clean_streak != UINT32_MAX) {
            rec.submit_clean_streak++;
        }
        if (kEnableHintCleanRangeCache && rec.shadow.size() == rec.size) {
            if (rec.recently_clean_submit_ranges.size() >= kMaxSubmitCleanRangeCacheEntries) {
                g_large_clean_range_stats.cleared_by_capacity++;
                rec.recently_clean_submit_ranges.erase(rec.recently_clean_submit_ranges.begin());
            }
            AppendMemoryRangeSpan(&rec.recently_clean_submit_ranges,
                                  range.offset,
                                  size,
                                  g_submit_generation);
        }
        if (stats) stats->clean_cache_hits++;
        AddSyncPolicyObservation(policy_batch,
                                 policy_key,
                                 1,
                                 (uint64_t)size,
                                 0,
                                 true,
                                 false,
                                 false,
                                 true,
                                 false,
                                 true,
                                 0,
                                 1,
                                 0,
                                 0);
        return false;
    }

    if (SyncPolicyIsDirectDirtySmallCandidate(range.offset,
                                              size,
                                              registered,
                                              rec.submit_clean_streak)) {
        EraseMemoryRangeSpanOverlaps(&rec.recently_clean_submit_ranges,
                                     range.offset,
                                     size);
        g_large_clean_range_stats.dirty_overlap++;
        g_large_clean_range_stats.cleared_by_dirty++;
        rec.submit_clean_streak = 0;
        if (stats) stats->dirty_ranges++;
        AddSyncPolicyObservation(policy_batch,
                                 policy_key,
                                 1,
                                 (uint64_t)size,
                                 (uint64_t)size,
                                 false,
                                 true,
                                 false,
                                 false,
                                 true,
                                 false,
                                 0,
                                 0,
                                 1,
                                 0);
        if (normalized) {
            *normalized = {rec.device, rec.memory, range.offset, size};
        }
        return true;
    }

    if (!aggressive_sample_verify &&
        kEnableHintCleanRangeCache &&
        rec.shadow.size() == rec.size) {
        const bool large_verify = large_clean_candidate &&
                                  SyncPolicyShouldVerifyLargeClean();
        const uint64_t ttl = large_clean_candidate ?
            kLargeCleanRangeGenerationTtl : kHintCleanCacheGenerationTtl;
        const uint64_t min_generation =
            g_submit_generation > ttl ? g_submit_generation - ttl : 0;
        const bool covered = RangeCoveredByGeneration(rec.recently_clean_submit_ranges,
                                                      range.offset,
                                                      size,
                                                      min_generation);
        if (!covered && large_clean_candidate) {
            if (rec.recently_clean_submit_ranges.empty()) {
                g_large_clean_range_stats.range_miss++;
            } else {
                bool old_cover = RangeCoveredByGeneration(rec.recently_clean_submit_ranges,
                                                          range.offset,
                                                          size,
                                                          0);
                if (old_cover) g_large_clean_range_stats.ttl_miss++;
                else g_large_clean_range_stats.range_miss++;
            }

            std::vector<std::pair<VkDeviceSize, VkDeviceSize>> uncovered_spans;
            CollectUncoveredRangeSpansByGeneration(rec.recently_clean_submit_ranges,
                                                   range.offset,
                                                   size,
                                                   min_generation,
                                                   &uncovered_spans);
            uint64_t uncovered_bytes = 0;
            for (const auto& span : uncovered_spans) {
                uncovered_bytes += (uint64_t)span.second;
            }

            if (uncovered_bytes != 0 &&
                uncovered_bytes < (uint64_t)size &&
                uncovered_bytes <= kLargeCleanPartialVerifyMaxBytes) {
                bool partial_dirty = false;
                for (const auto& span : uncovered_spans) {
                    if (span.second == 0) continue;
                    if (stats) {
                        stats->memcmp_checks++;
                        stats->memcmp_bytes += (uint64_t)span.second;
                    }
                    if (memcmp(rec.map_data + span.first,
                               rec.shadow.data() + span.first,
                               (size_t)span.second) != 0) {
                        partial_dirty = true;
                        break;
                    }
                }

                NoteLargeCleanRangePartialVerify(
                    uncovered_bytes,
                    (uint64_t)size - uncovered_bytes,
                    !partial_dirty);

                if (!partial_dirty) {
                    if (rec.submit_clean_streak != UINT32_MAX) {
                        rec.submit_clean_streak++;
                    }
                    if (rec.recently_clean_submit_ranges.size() >=
                        kMaxSubmitCleanRangeCacheEntries) {
                        g_large_clean_range_stats.cleared_by_capacity++;
                        rec.recently_clean_submit_ranges.erase(
                            rec.recently_clean_submit_ranges.begin());
                    }
                    AppendMemoryRangeSpan(&rec.recently_clean_submit_ranges,
                                          range.offset,
                                          size,
                                          g_submit_generation);
                    if (stats) stats->clean_cache_hits++;
                    AddSyncPolicyObservation(policy_batch,
                                             policy_key,
                                             1,
                                             (uint64_t)size,
                                             0,
                                             true,
                                             false,
                                             false,
                                             true,
                                             false,
                                             true,
                                             0,
                                             0,
                                             0,
                                             1);
                    return false;
                }
            } else if (uncovered_bytes > kLargeCleanPartialVerifyMaxBytes &&
                       uncovered_bytes < (uint64_t)size) {
                g_large_clean_range_stats.partial_too_large++;
            }
        }
        if (covered && !large_verify) {
            if (stats) stats->clean_cache_hits++;
            if (large_clean_candidate) {
                NoteLargeCleanRangeHit((uint64_t)size);
            }
            AddSyncPolicyObservation(policy_batch,
                                     policy_key,
                                     1,
                                     (uint64_t)size,
                                     0,
                                     true,
                                     false,
                                     false,
                                     true,
                                     false,
                                     true,
                                     0,
                                     0,
                                     0,
                                     0);
            return false;
        }
        if (covered && large_verify) {
            NoteLargeCleanRangeVerify((uint64_t)size);
        }
    }

    if (large_clean_candidate && rec.shadow.size() != rec.size) {
        g_large_clean_range_stats.no_shadow++;
    }

    if (stats) {
        stats->memcmp_checks++;
        stats->memcmp_bytes += (uint64_t)size;
    }

    if (rec.shadow.size() == rec.size &&
        memcmp(rec.map_data + range.offset, rec.shadow.data() + range.offset, (size_t)size) == 0) {
        if (rec.submit_clean_streak != UINT32_MAX) {
            rec.submit_clean_streak++;
        }
        if (kEnableHintCleanRangeCache) {
            if (rec.recently_clean_submit_ranges.size() >= kMaxSubmitCleanRangeCacheEntries) {
                g_large_clean_range_stats.cleared_by_capacity++;
                rec.recently_clean_submit_ranges.erase(rec.recently_clean_submit_ranges.begin());
            }
            AppendMemoryRangeSpan(&rec.recently_clean_submit_ranges,
                                  range.offset,
                                  size,
                                  g_submit_generation);
        }
        if (large_clean_candidate) {
            NoteLargeCleanRangePromote((uint64_t)size);
        }
        AddSyncPolicyObservation(policy_batch,
                                 policy_key,
                                 1,
                                 (uint64_t)size,
                                 0,
                                 true,
                                 false,
                                 false,
                                 false,
                                 false,
                                 true,
                                 0,
                                 0,
                                 0,
                                 aggressive_sample_verify ? 1 : 0);
        return false;
    }

    EraseMemoryRangeSpanOverlaps(&rec.recently_clean_submit_ranges,
                                 range.offset,
                                 size);
    g_large_clean_range_stats.dirty_overlap++;
    g_large_clean_range_stats.cleared_by_dirty++;
    if (large_clean_candidate) {
        NoteLargeCleanRangeFail((uint64_t)size);
    }
    rec.submit_clean_streak = 0;
    if (stats) stats->dirty_ranges++;
    AddSyncPolicyObservation(policy_batch,
                             policy_key,
                             1,
                             (uint64_t)size,
                             (uint64_t)size,
                             false,
                             true,
                             false,
                             false,
                             true,
                             false,
                             0,
                             0,
                             0,
                             aggressive_sample_verify ? 1 : 0);

    if (normalized) {
        *normalized = {rec.device, rec.memory, range.offset, size};
    }
    return true;
}

static void ForceRefreshShadowForRanges(uint32_t memoryRangeCount,
                                        const VkMappedMemoryRange* pMemoryRanges) {
    if (!pMemoryRanges || memoryRangeCount == 0) return;
    std::lock_guard<std::mutex> lock(g_mapped_mutex);
    for (uint32_t i = 0; i < memoryRangeCount; ++i) {
        auto it = g_active_mapped_memories.find(pMemoryRanges[i].memory);
        if (it == g_active_mapped_memories.end()) continue;
        ActiveMappedMemoryRecord& rec = it->second;
        if (!rec.map_data || rec.size == 0) continue;

        const VkDeviceSize size =
            ClampMappedRangeSize(rec.size, pMemoryRanges[i].offset, pMemoryRanges[i].size);
        if (size == 0 || size > kEarlyUploadShadowRefreshLimitBytes) continue;

        if (rec.shadow.size() != rec.size) {
            rec.shadow.resize((size_t)rec.size);
        }
        memcpy(rec.shadow.data() + pMemoryRanges[i].offset,
               rec.map_data + pMemoryRanges[i].offset,
               (size_t)size);
    }
}

static void EarlyUploadTrackedRanges(const std::vector<TrackedMemoryRange>& ranges,
                                     VkDeviceSize min_bytes,
                                     const char* reason) {
    if (ranges.empty()) return;

    std::unordered_map<VkDevice, std::vector<VkMappedMemoryRange>> ranges_by_dev;
    uint64_t total_bytes = 0;
    size_t selected_ranges = 0;
    {
        std::lock_guard<std::mutex> lock(g_mapped_mutex);
        for (const TrackedMemoryRange& range : ranges) {
            TrackedMemoryRange normalized = {};
            if (!TrackedRangeNeedsUploadLocked(
                    range, min_bytes, kEarlyUploadMaxBytes, &normalized)) {
                continue;
            }

            VkMappedMemoryRange mapped_range = {};
            mapped_range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
            mapped_range.pNext = nullptr;
            mapped_range.memory = normalized.memory;
            mapped_range.offset = normalized.offset;
            mapped_range.size = normalized.size;
            ranges_by_dev[normalized.device].push_back(mapped_range);
            total_bytes += (uint64_t)normalized.size;
            selected_ranges++;
        }
    }

    if (selected_ranges == 0) return;

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (auto& pair : ranges_by_dev) {
        FlushMappedMemoryRanges(pair.first, pair.second.size(), pair.second.data());
        ForceRefreshShadowForRanges(pair.second.size(), pair.second.data());
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);

    if (kEnableImplicitSyncDiagLog) {
        double total_ms = (t1.tv_sec - t0.tv_sec) * 1000.0 +
                          (t1.tv_nsec - t0.tv_nsec) / 1000000.0;
        ALOGD("[SYNC_GUEST] early_upload reason=%s ranges=%zu bytes=%llu ms=%.3f",
              reason ? reason : "unknown",
              selected_ranges,
              (unsigned long long)total_bytes,
              total_ms);
    }
}
VKAPI_ATTR VkResult InvalidateMappedMemoryRanges(VkDevice device, uint32_t memoryRangeCount, const VkMappedMemoryRange* pMemoryRanges);

static uint64_t MakeDescriptorSlotKey(uint32_t binding, uint32_t arrayElement) {
    return (uint64_t(binding) << 32) | uint64_t(arrayElement);
}

static bool DescriptorTypeUsesBufferInfo(VkDescriptorType type) {
    switch (type) {
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
            return true;
        default:
            return false;
    }
}

static DescriptorBufferAccess GetDescriptorBufferAccess(VkDescriptorType type) {
    switch (type) {
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
            return DescriptorBufferAccess::kPotentialWrite;
        default:
            return DescriptorBufferAccess::kFlushOnly;
    }
}

static bool DescriptorTypeUsesDynamicOffset(VkDescriptorType type) {
    return type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
           type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
}

static bool DescriptorBufferUseEquals(const DescriptorBufferUse& a,
                                      const DescriptorBufferUse& b) {
    return a.buffer == b.buffer &&
           a.offset == b.offset &&
           a.range == b.range &&
           a.descriptor_type == b.descriptor_type &&
           a.access == b.access &&
           a.uses_dynamic_offset == b.uses_dynamic_offset;
}

static void LogDescriptorTraceUseLocked(const char* event,
                                        VkCommandBuffer commandBuffer,
                                        VkDescriptorSet descriptor_set,
                                        uint64_t slot_key,
                                        const DescriptorBufferUse& use,
                                        VkDeviceSize dynamic_offset,
                                        uint32_t entry_index);

static bool RememberDescriptorBufferUseLocked(VkDescriptorSet descriptor_set,
                                              uint32_t binding,
                                              uint32_t array_element,
                                              VkDescriptorType descriptor_type,
                                              const VkDescriptorBufferInfo& buffer_info) {
    if (descriptor_set == VK_NULL_HANDLE || buffer_info.buffer == VK_NULL_HANDLE) return false;

    DescriptorBufferUse new_use = {
        buffer_info.buffer,
        buffer_info.offset,
        buffer_info.range,
        (uint32_t)descriptor_type,
        GetDescriptorBufferAccess(descriptor_type),
        DescriptorTypeUsesDynamicOffset(descriptor_type),
    };
    auto& uses = g_descriptor_set_buffer_uses[descriptor_set];
    const uint64_t slot_key = MakeDescriptorSlotKey(binding, array_element);
    auto existing = uses.find(slot_key);
    if (existing != uses.end() &&
        DescriptorBufferUseEquals(existing->second, new_use)) {
        return false;
    }

    uses[slot_key] = new_use;
    BumpDescriptorSetVersionLocked(descriptor_set, "descriptor_write");
    LogDescriptorTraceUseLocked("update_desc",
                                VK_NULL_HANDLE,
                                descriptor_set,
                                slot_key,
                                new_use,
                                0,
                                0);
    MemShapeNoteDescriptorBufferLocked(buffer_info.buffer,
                                       buffer_info.offset,
                                       buffer_info.range,
                                       descriptor_type,
                                       false);
    return true;
}

static void CaptureDescriptorBufferWritesLocked(uint32_t descriptorWriteCount,
                                                const VkWriteDescriptorSet* pDescriptorWrites) {
    if (!pDescriptorWrites || descriptorWriteCount == 0) return;

    for (uint32_t i = 0; i < descriptorWriteCount; ++i) {
        const VkWriteDescriptorSet& write = pDescriptorWrites[i];
        if (!DescriptorTypeUsesBufferInfo(write.descriptorType) || !write.pBufferInfo) continue;

        for (uint32_t j = 0; j < write.descriptorCount; ++j) {
            RememberDescriptorBufferUseLocked(
                write.dstSet,
                write.dstBinding,
                write.dstArrayElement + j,
                write.descriptorType,
                write.pBufferInfo[j]);
        }
    }
}

static void CaptureDescriptorCopiesLocked(uint32_t descriptorCopyCount,
                                          const VkCopyDescriptorSet* pDescriptorCopies) {
    if (!pDescriptorCopies || descriptorCopyCount == 0) return;

    for (uint32_t i = 0; i < descriptorCopyCount; ++i) {
        const VkCopyDescriptorSet& copy = pDescriptorCopies[i];
        auto src_set_it = g_descriptor_set_buffer_uses.find(copy.srcSet);
        if (src_set_it == g_descriptor_set_buffer_uses.end()) continue;

        for (uint32_t j = 0; j < copy.descriptorCount; ++j) {
            const uint64_t src_key =
                MakeDescriptorSlotKey(copy.srcBinding, copy.srcArrayElement + j);
            const uint64_t dst_key =
                MakeDescriptorSlotKey(copy.dstBinding, copy.dstArrayElement + j);
            auto src_use_it = src_set_it->second.find(src_key);
            if (src_use_it == src_set_it->second.end()) {
                auto& dst_uses = g_descriptor_set_buffer_uses[copy.dstSet];
                if (dst_uses.erase(dst_key) != 0) {
                    BumpDescriptorSetVersionLocked(copy.dstSet, "descriptor_copy_erase");
                }
                continue;
            }
            auto& dst_uses = g_descriptor_set_buffer_uses[copy.dstSet];
            auto dst_use_it = dst_uses.find(dst_key);
            if (dst_use_it != dst_uses.end() &&
                DescriptorBufferUseEquals(dst_use_it->second, src_use_it->second)) {
                continue;
            }
            dst_uses[dst_key] = src_use_it->second;
            BumpDescriptorSetVersionLocked(copy.dstSet, "descriptor_copy");
            LogDescriptorTraceUseLocked("copy_desc",
                                        VK_NULL_HANDLE,
                                        copy.dstSet,
                                        dst_key,
                                        src_use_it->second,
                                        0,
                                        0);
            MemShapeNoteDescriptorBufferLocked(src_use_it->second.buffer,
                                               src_use_it->second.offset,
                                               src_use_it->second.range,
                                               (VkDescriptorType)src_use_it->second.descriptor_type,
                                               false);
        }
    }
}

static bool TrackedRangesOverlap(const TrackedMemoryRange& a,
                                 const TrackedMemoryRange& b) {
    if (a.device != b.device || a.memory != b.memory || a.size == 0 || b.size == 0) {
        return false;
    }
    const VkDeviceSize a_end =
        (a.offset > UINT64_MAX - a.size) ? UINT64_MAX : a.offset + a.size;
    const VkDeviceSize b_end =
        (b.offset > UINT64_MAX - b.size) ? UINT64_MAX : b.offset + b.size;
    return a.offset < b_end && b.offset < a_end;
}

static void CanonicalizeTrackedRanges(std::vector<TrackedMemoryRange>* ranges) {
    if (!ranges || ranges->size() < 2) return;

    std::sort(ranges->begin(), ranges->end(),
              [](const TrackedMemoryRange& a, const TrackedMemoryRange& b) {
                  if (a.device != b.device) {
                      return (uintptr_t)a.device < (uintptr_t)b.device;
                  }
                  if (a.memory != b.memory) {
                      return (uintptr_t)a.memory < (uintptr_t)b.memory;
                  }
                  if (a.offset != b.offset) return a.offset < b.offset;
                  return a.size < b.size;
              });

    std::vector<TrackedMemoryRange> merged;
    merged.reserve(ranges->size());
    for (const TrackedMemoryRange& range : *ranges) {
        if (range.memory == VK_NULL_HANDLE || range.size == 0) continue;
        if (merged.empty()) {
            merged.push_back(range);
            continue;
        }

        TrackedMemoryRange& last = merged.back();
        const VkDeviceSize last_end =
            (last.offset > UINT64_MAX - last.size) ? UINT64_MAX : last.offset + last.size;
        const VkDeviceSize range_end =
            (range.offset > UINT64_MAX - range.size) ? UINT64_MAX : range.offset + range.size;
        const bool same_memory = last.device == range.device && last.memory == range.memory;
        const bool overlaps_or_adjacent = same_memory && range.offset <= last_end;
        if (!overlaps_or_adjacent) {
            merged.push_back(range);
            continue;
        }

        const VkDeviceSize merged_end = std::max(last_end, range_end);
        last.size = merged_end - last.offset;
    }

    ranges->swap(merged);
}

static void AppendOutputMemoryRangeHint(std::vector<OutputMemoryRangeHint>* hints,
                                        const TrackedMemoryRange& range,
                                        OutputHintSource source,
                                        OutputHintStrength strength,
                                        bool wait_commit_eligible) {
    if (!hints || range.memory == VK_NULL_HANDLE || range.size == 0) return;

    for (OutputMemoryRangeHint& existing : *hints) {
        if (existing.source != source ||
            existing.strength != strength ||
            existing.wait_commit_eligible != wait_commit_eligible ||
            existing.range.device != range.device ||
            existing.range.memory != range.memory) {
            continue;
        }

        const VkDeviceSize existing_end = existing.range.offset + existing.range.size;
        const VkDeviceSize range_end = range.offset + range.size;
        const bool overlaps =
            !(range_end < existing.range.offset || existing_end < range.offset);
        const bool adjacent =
            (existing_end == range.offset) || (range_end == existing.range.offset);
        if (!overlaps && !adjacent) continue;

        const VkDeviceSize merged_begin = std::min(existing.range.offset, range.offset);
        const VkDeviceSize merged_end = std::max(existing_end, range_end);
        existing.range.offset = merged_begin;
        existing.range.size = merged_end - merged_begin;
        return;
    }

    hints->push_back({range, source, strength, wait_commit_eligible});
}

static void AppendDescriptorOutputReadbackHint(
    std::vector<OutputMemoryRangeHint>* output_hints,
    const TrackedMemoryRange& range) {
    (void)output_hints;
    (void)range;
    // Descriptor storage-buffer writes are "may write" metadata. They are useful
    // for future prefetch experiments, but not strong enough to define a
    // correctness range. Correctness now comes from semantic strong hints plus
    // the conservative mapped readback fallback.
}

static void CanonicalizeOutputMemoryRangeHints(std::vector<OutputMemoryRangeHint>* hints) {
    if (!hints || hints->size() < 2) return;

    std::sort(hints->begin(), hints->end(),
              [](const OutputMemoryRangeHint& a, const OutputMemoryRangeHint& b) {
                  if (a.source != b.source) return (uint8_t)a.source < (uint8_t)b.source;
                  if (a.strength != b.strength) return (uint8_t)a.strength < (uint8_t)b.strength;
                  if (a.wait_commit_eligible != b.wait_commit_eligible) {
                      return a.wait_commit_eligible < b.wait_commit_eligible;
                  }
                  if (a.range.device != b.range.device) {
                      return (uintptr_t)a.range.device < (uintptr_t)b.range.device;
                  }
                  if (a.range.memory != b.range.memory) {
                      return (uintptr_t)a.range.memory < (uintptr_t)b.range.memory;
                  }
                  if (a.range.offset != b.range.offset) return a.range.offset < b.range.offset;
                  return a.range.size < b.range.size;
              });

    std::vector<OutputMemoryRangeHint> merged;
    merged.reserve(hints->size());
    for (const OutputMemoryRangeHint& hint : *hints) {
        AppendOutputMemoryRangeHint(&merged,
                                    hint.range,
                                    hint.source,
                                    hint.strength,
                                    hint.wait_commit_eligible);
    }
    hints->swap(merged);
}

static bool ShouldAutoInvalidateOutputHintLocked(const OutputMemoryRangeHint& hint);

static void SuppressWeakDescriptorReadbacksIfStrongCopyPresent(SubmitSyncHints* hints,
                                                               uint32_t submitCount) {
    if (!hints || hints->output_hints.empty() ||
        !kSuppressWeakReadbackWhenStrongCopyPresent) {
        return;
    }

    uint64_t strong_hints = 0;
    uint64_t strong_bytes = 0;
    uint64_t weak_candidates = 0;
    uint64_t weak_candidate_bytes = 0;
    for (const OutputMemoryRangeHint& hint : hints->output_hints) {
        if (hint.strength == OutputHintStrength::kStrongReadback) {
            strong_hints++;
            strong_bytes += hint.range.size;
        } else if (hint.source == OutputHintSource::kDescriptorMaybeWrite &&
                   hint.strength == OutputHintStrength::kWeakMaybeWrite) {
            weak_candidates++;
            weak_candidate_bytes += hint.range.size;
        }
    }

    if (strong_hints == 0 || weak_candidates == 0) {
        NoteWeakReadbackSuppression(weak_candidates,
                                    false,
                                    strong_hints,
                                    strong_bytes,
                                    0,
                                    0,
                                    weak_candidates,
                                    weak_candidate_bytes,
                                    0,
                                    false);
        return;
    }

    if (submitCount != 1) {
        NoteWeakReadbackSuppression(weak_candidates,
                                    false,
                                    strong_hints,
                                    strong_bytes,
                                    0,
                                    0,
                                    weak_candidates,
                                    weak_candidate_bytes,
                                    0,
                                    true);
        return;
    }

    std::vector<OutputMemoryRangeHint> filtered;
    filtered.reserve(hints->output_hints.size());
    uint64_t weak_dropped = 0;
    uint64_t weak_dropped_bytes = 0;
    uint64_t weak_kept = 0;
    uint64_t weak_kept_bytes = 0;
    uint64_t tiny_weak_kept = 0;

    for (const OutputMemoryRangeHint& hint : hints->output_hints) {
        const bool weak_descriptor =
            hint.source == OutputHintSource::kDescriptorMaybeWrite &&
            hint.strength == OutputHintStrength::kWeakMaybeWrite;
        if (!weak_descriptor) {
            filtered.push_back(hint);
            continue;
        }

        // Weak descriptor outputs are speculative only. The correctness path no
        // longer keeps workload-shaped weak ranges here.
        if (ShouldAutoInvalidateOutputHintLocked(hint)) {
            filtered.push_back(hint);
            weak_kept++;
            if (hint.range.size <= kImplicitTinyControlBytes) {
                tiny_weak_kept++;
            }
            weak_kept_bytes += hint.range.size;
            continue;
        }

        weak_dropped++;
        weak_dropped_bytes += hint.range.size;
    }

    hints->output_hints.swap(filtered);
    NoteWeakReadbackSuppression(weak_candidates,
                                weak_dropped != 0,
                                strong_hints,
                                strong_bytes,
                                weak_dropped,
                                weak_dropped_bytes,
                                weak_kept,
                                weak_kept_bytes,
                                tiny_weak_kept,
                                false);
}

static bool ShouldAutoInvalidateOutputHintLocked(const OutputMemoryRangeHint& hint) {
    if (!hint.wait_commit_eligible) return false;
    if (hint.range.memory == VK_NULL_HANDLE || hint.range.size == 0) return false;
    if (hint.range.size > kWaitCommittedInvalidateMaxBytes) return false;

    if (hint.strength == OutputHintStrength::kStrongReadback) {
        return true;
    }

    return false;
}

static bool ShouldAutoInvalidateOutputHint(const OutputMemoryRangeHint& hint) {
    std::lock_guard<std::mutex> lock(g_mapped_mutex);
    return ShouldAutoInvalidateOutputHintLocked(hint);
}

static void AppendOutputHintRanges(const std::vector<OutputMemoryRangeHint>& hints,
                                   std::vector<TrackedMemoryRange>* ranges) {
    if (!ranges) return;
    for (const OutputMemoryRangeHint& hint : hints) {
        if (!ShouldAutoInvalidateOutputHintLocked(hint)) continue;
        AppendTrackedRange(ranges, hint.range);
    }
}

static bool SubmitSyncHintsNeedAutoReadback(const SubmitSyncHints& hints) {
    for (const OutputMemoryRangeHint& hint : hints.output_hints) {
        if (ShouldAutoInvalidateOutputHint(hint)) {
            return true;
        }
    }
    return false;
}

static bool SubmitSyncHintsNeedImmediateHostFence(const SubmitSyncHints& hints) {
    std::lock_guard<std::mutex> lock(g_mapped_mutex);
    for (const OutputMemoryRangeHint& hint : hints.output_hints) {
        if (!ShouldAutoInvalidateOutputHintLocked(hint)) continue;
        if (hint.strength == OutputHintStrength::kStrongReadback) {
            return true;
        }
        if (hint.source != OutputHintSource::kDescriptorMaybeWrite) {
            return true;
        }
    }
    return false;
}

static void NoteReadbackFenceDecision(const char* api,
                                      VkFence fence,
                                      const SubmitSyncHints& hints,
                                      bool deferred) {
    std::vector<TrackedMemoryRange> output_ranges;
    std::vector<std::pair<uint32_t, ReadbackHintClassStats>> local_hint_classes;
    local_hint_classes.reserve(8);
    {
        std::lock_guard<std::mutex> mapped_lock(g_mapped_mutex);
        AppendOutputHintRanges(hints.output_hints, &output_ranges);
        for (const OutputMemoryRangeHint& hint : hints.output_hints) {
            const bool auto_readback = ShouldAutoInvalidateOutputHintLocked(hint);
            const uint32_t key = MakeReadbackHintClassKey(hint, auto_readback);
            AddReadbackHintClassObservation(&local_hint_classes,
                                            key,
                                            hint,
                                            auto_readback);
        }
    }
    const bool has_auto_readback = !output_ranges.empty();
    const bool blocked_by_readback =
        fence != VK_NULL_HANDLE && !deferred && has_auto_readback;

    std::lock_guard<std::mutex> lock(g_submit_hint_cache_stats_mutex);
    g_readback_fence_stats.submits++;
    if (fence == VK_NULL_HANDLE) {
        g_readback_fence_stats.no_fence++;
    } else {
        g_readback_fence_stats.with_fence++;
        if (deferred) {
            g_readback_fence_stats.deferred++;
        } else if (has_auto_readback) {
            g_readback_fence_stats.blocked_readback++;
        }
    }
    g_readback_fence_stats.output_hints += hints.output_hints.size();
    g_readback_fence_stats.output_ranges += output_ranges.size();
    g_readback_fence_stats.output_bytes += TotalTrackedRangeBytes(output_ranges);
    for (const auto& item : local_hint_classes) {
        ReadbackHintClassStats& dst = g_readback_hint_class_stats[item.first];
        const ReadbackHintClassStats& src = item.second;
        dst.samples += src.samples;
        dst.auto_readback += src.auto_readback;
        dst.wait_eligible += src.wait_eligible;
        dst.bytes += src.bytes;
        dst.auto_bytes += src.auto_bytes;
        if (deferred) dst.deferred_submits += src.samples;
        if (blocked_by_readback) dst.blocked_submits += src.samples;
    }
    MaybeLogReadbackFenceStatsLocked(api);
}

static bool ShouldDeferFenceWaitForSubmit(VkFence fence,
                                           const SubmitSyncHints& hints) {
    const bool has_auto_readback = SubmitSyncHintsNeedAutoReadback(hints);
    const bool needs_immediate_host_fence =
        has_auto_readback && SubmitSyncHintsNeedImmediateHostFence(hints);
    const bool weak_descriptor_readback =
        has_auto_readback && !needs_immediate_host_fence;
    return kEnableDeferredFenceWait &&
           fence != VK_NULL_HANDLE &&
           hints.present &&
           (!has_auto_readback ||
            (kEnableDeferredWeakDescriptorReadbackFence &&
             !kPreferHostFenceForWeakDescriptorReadback &&
             weak_descriptor_readback &&
             !needs_immediate_host_fence));
}

static void AppendBufferSyncRange(std::vector<BufferSyncRange>* ranges,
                                  const BufferSyncRange& range) {
    if (!ranges || range.buffer == VK_NULL_HANDLE || range.size == 0) return;

    if (range.size != VK_WHOLE_SIZE) {
        for (BufferSyncRange& existing : *ranges) {
            if (existing.buffer != range.buffer || existing.size == VK_WHOLE_SIZE) continue;

            const VkDeviceSize existing_end = existing.offset + existing.size;
            const VkDeviceSize range_end = range.offset + range.size;
            const bool overlaps = !(range_end < existing.offset || existing_end < range.offset);
            const bool adjacent = (existing_end == range.offset) || (range_end == existing.offset);
            if (!overlaps && !adjacent) continue;

            const VkDeviceSize merged_begin = std::min(existing.offset, range.offset);
            const VkDeviceSize merged_end = std::max(existing_end, range_end);
            existing.offset = merged_begin;
            existing.size = merged_end - merged_begin;
            return;
        }
    }

    ranges->push_back(range);
}

static bool ResolveBufferRangeLocked(VkBuffer buffer,
                                     VkDeviceSize bufferOffset,
                                     VkDeviceSize bufferRange,
                                     TrackedMemoryRange* out_range) {
    if (!out_range || buffer == VK_NULL_HANDLE) return false;

    auto binding_it = g_buffer_memory_bindings.find(buffer);
    if (binding_it == g_buffer_memory_bindings.end()) return false;

    auto active_it = g_active_mapped_memories.find(binding_it->second.memory);
    if (active_it == g_active_mapped_memories.end()) return false;

    VkDeviceSize buffer_size = binding_it->second.buffer_size;
    if (bufferOffset > buffer_size) return false;

    VkDeviceSize resolved_size = bufferRange;
    if (resolved_size == VK_WHOLE_SIZE || bufferOffset + resolved_size > buffer_size) {
        resolved_size = buffer_size - bufferOffset;
    }
    if (resolved_size == 0) return false;

    VkDeviceSize memory_offset = binding_it->second.memory_offset + bufferOffset;
    if (memory_offset >= active_it->second.size) return false;
    if (memory_offset + resolved_size > active_it->second.size) {
        resolved_size = active_it->second.size - memory_offset;
    }
    if (resolved_size == 0) return false;

    out_range->device = binding_it->second.device;
    out_range->memory = binding_it->second.memory;
    out_range->offset = memory_offset;
    out_range->size = resolved_size;
    return true;
}

static uint32_t DescriptorSlotBinding(uint64_t slot_key) {
    return (uint32_t)(slot_key >> 32);
}

static uint32_t DescriptorSlotArrayElement(uint64_t slot_key) {
    return (uint32_t)(slot_key & 0xffffffffu);
}

static const char* DescriptorAccessName(DescriptorBufferAccess access) {
    return access == DescriptorBufferAccess::kPotentialWrite ? "may_write" : "flush_only";
}

static void LogDescriptorTraceUseLocked(const char* event,
                                        VkCommandBuffer commandBuffer,
                                        VkDescriptorSet descriptor_set,
                                        uint64_t slot_key,
                                        const DescriptorBufferUse& use,
                                        VkDeviceSize dynamic_offset,
                                        uint32_t entry_index) {
    if (!kEnableDescriptorTraceLog || use.buffer == VK_NULL_HANDLE) return;
    if (dynamic_offset > UINT64_MAX - use.offset) return;

    const VkDeviceSize descriptor_offset = use.offset + dynamic_offset;
    TrackedMemoryRange range = {};
    const bool resolved =
        ResolveBufferRangeLocked(use.buffer, descriptor_offset, use.range, &range);
    if (!resolved) {
        if (use.range == VK_WHOLE_SIZE || use.range >= kDescriptorTraceMinRangeBytes) {
            ALOGI("[DESC_TRACE] event=%s unresolved cmd=%llx set=%llx binding=%u elem=%u type=%u access=%s buffer=%llx desc_off=%llu dyn=%llu desc_range=%llu entry=%u",
                  event ? event : "unknown",
                  (unsigned long long)(uintptr_t)commandBuffer,
                  (unsigned long long)(uintptr_t)descriptor_set,
                  DescriptorSlotBinding(slot_key),
                  DescriptorSlotArrayElement(slot_key),
                  use.descriptor_type,
                  DescriptorAccessName(use.access),
                  (unsigned long long)(uintptr_t)use.buffer,
                  (unsigned long long)use.offset,
                  (unsigned long long)dynamic_offset,
                  (unsigned long long)use.range,
                  entry_index);
        }
        return;
    }
    if (range.size < kDescriptorTraceMinRangeBytes) return;

    float f0 = 0.0f;
    float f1 = 0.0f;
    float f2 = 0.0f;
    float f3 = 0.0f;
    auto active_it = g_active_mapped_memories.find(range.memory);
    if (active_it != g_active_mapped_memories.end()) {
        const ActiveMappedMemoryRecord& rec = active_it->second;
        if (rec.map_data && range.offset < rec.size) {
            const VkDeviceSize available = rec.size - range.offset;
            const size_t sample_bytes =
                (size_t)std::min<VkDeviceSize>(available, 4 * sizeof(float));
            const uint8_t* sample = rec.map_data + range.offset;
            if (sample_bytes >= sizeof(float)) {
                memcpy(&f0, sample, sizeof(float));
            }
            if (sample_bytes >= 2 * sizeof(float)) {
                memcpy(&f1, sample + sizeof(float), sizeof(float));
            }
            if (sample_bytes >= 3 * sizeof(float)) {
                memcpy(&f2, sample + 2 * sizeof(float), sizeof(float));
            }
            if (sample_bytes >= 4 * sizeof(float)) {
                memcpy(&f3, sample + 3 * sizeof(float), sizeof(float));
            }
        }
    }

    ALOGI("[DESC_TRACE] event=%s cmd=%llx set=%llx binding=%u elem=%u type=%u access=%s buffer=%llx desc_off=%llu dyn=%llu desc_range=%llu memory=%llx mem_off=%llu size=%llu f32=[%.6f,%.6f,%.6f,%.6f] entry=%u",
          event ? event : "unknown",
          (unsigned long long)(uintptr_t)commandBuffer,
          (unsigned long long)(uintptr_t)descriptor_set,
          DescriptorSlotBinding(slot_key),
          DescriptorSlotArrayElement(slot_key),
          use.descriptor_type,
          DescriptorAccessName(use.access),
          (unsigned long long)(uintptr_t)use.buffer,
          (unsigned long long)use.offset,
          (unsigned long long)dynamic_offset,
          (unsigned long long)use.range,
          (unsigned long long)(uintptr_t)range.memory,
          (unsigned long long)range.offset,
          (unsigned long long)range.size,
          (double)f0,
          (double)f1,
          (double)f2,
          (double)f3,
          entry_index);
}

static void LogDescriptorSetTraceLocked(const char* event,
                                        VkCommandBuffer commandBuffer,
                                        VkDescriptorSet descriptor_set,
                                        uint32_t* logged_entries) {
    if (!kEnableDescriptorTraceLog || !logged_entries ||
        *logged_entries >= kDescriptorTraceMaxEntriesPerEvent ||
        descriptor_set == VK_NULL_HANDLE) {
        return;
    }

    auto set_it = g_descriptor_set_buffer_uses.find(descriptor_set);
    if (set_it == g_descriptor_set_buffer_uses.end()) return;

    for (const auto& use_pair : set_it->second) {
        if (*logged_entries >= kDescriptorTraceMaxEntriesPerEvent) return;
        const DescriptorBufferUse& use = use_pair.second;
        if (!use.uses_dynamic_offset) {
            LogDescriptorTraceUseLocked(event,
                                        commandBuffer,
                                        descriptor_set,
                                        use_pair.first,
                                        use,
                                        0,
                                        *logged_entries);
            (*logged_entries)++;
            continue;
        }

        auto cmd_dyn_it = g_command_buffer_descriptor_dynamic_offsets.find(commandBuffer);
        if (cmd_dyn_it == g_command_buffer_descriptor_dynamic_offsets.end()) {
            LogDescriptorTraceUseLocked(event,
                                        commandBuffer,
                                        descriptor_set,
                                        use_pair.first,
                                        use,
                                        0,
                                        *logged_entries);
            (*logged_entries)++;
            continue;
        }

        auto set_dyn_it = cmd_dyn_it->second.find(descriptor_set);
        if (set_dyn_it == cmd_dyn_it->second.end()) {
            LogDescriptorTraceUseLocked(event,
                                        commandBuffer,
                                        descriptor_set,
                                        use_pair.first,
                                        use,
                                        0,
                                        *logged_entries);
            (*logged_entries)++;
            continue;
        }

        auto dyn_it = set_dyn_it->second.find(use_pair.first);
        if (dyn_it == set_dyn_it->second.end() || dyn_it->second.empty()) {
            LogDescriptorTraceUseLocked(event,
                                        commandBuffer,
                                        descriptor_set,
                                        use_pair.first,
                                        use,
                                        0,
                                        *logged_entries);
            (*logged_entries)++;
            continue;
        }

        for (uint32_t dyn : dyn_it->second) {
            if (*logged_entries >= kDescriptorTraceMaxEntriesPerEvent) return;
            LogDescriptorTraceUseLocked(event,
                                        commandBuffer,
                                        descriptor_set,
                                        use_pair.first,
                                        use,
                                        dyn,
                                        *logged_entries);
            (*logged_entries)++;
        }
    }
}

static void ResolveBufferSyncRangesLocked(
    const std::vector<BufferSyncRange>& buffer_ranges,
    std::vector<TrackedMemoryRange>* ranges) {
    if (!ranges) return;

    for (const BufferSyncRange& buffer_range : buffer_ranges) {
        TrackedMemoryRange range = {};
        if (!ResolveBufferRangeLocked(
                buffer_range.buffer,
                buffer_range.offset,
                buffer_range.size,
                &range)) {
            continue;
        }
        AppendTrackedRange(ranges, range);
    }
}

static void ResolveBufferSyncRangesCappedLocked(
    const std::vector<BufferSyncRange>& buffer_ranges,
    VkDeviceSize max_auto_bytes,
    OutputHintSource source,
    OutputHintStrength strength,
    std::vector<OutputMemoryRangeHint>* output_hints) {
    if (!output_hints) return;

    for (const BufferSyncRange& buffer_range : buffer_ranges) {
        TrackedMemoryRange range = {};
        if (!ResolveBufferRangeLocked(
                buffer_range.buffer,
                buffer_range.offset,
                buffer_range.size,
                &range)) {
            continue;
        }
        if (range.size == 0 || range.size > max_auto_bytes) continue;
        AppendOutputMemoryRangeHint(output_hints,
                                    range,
                                    source,
                                    strength,
                                    true);
    }
}

static void AppendDescriptorUseHintLocked(
    VkCommandBuffer commandBuffer,
    VkDescriptorSet descriptor_set,
    uint64_t slot_key,
    const DescriptorBufferUse& use,
    std::vector<TrackedMemoryRange>* flush_ranges,
    std::vector<OutputMemoryRangeHint>* output_hints) {
    auto append_one = [&](VkDeviceSize dynamic_offset) {
        if (dynamic_offset > UINT64_MAX - use.offset) return;
        const VkDeviceSize resolved_offset = use.offset + dynamic_offset;
        TrackedMemoryRange range = {};
        if (!ResolveBufferRangeLocked(use.buffer, resolved_offset, use.range, &range)) return;
        AppendTrackedRange(flush_ranges, range);
        if (use.access == DescriptorBufferAccess::kPotentialWrite) {
            AppendDescriptorOutputReadbackHint(output_hints, range);
        }
    };

    if (!use.uses_dynamic_offset) {
        append_one(0);
        return;
    }

    if (g_command_buffer_unknown_dynamic_offsets.find(commandBuffer) !=
        g_command_buffer_unknown_dynamic_offsets.end()) {
        TrackedMemoryRange range = {};
        if (!ResolveBufferRangeLocked(use.buffer, 0, VK_WHOLE_SIZE, &range)) return;
        AppendTrackedRange(flush_ranges, range);
        if (use.access == DescriptorBufferAccess::kPotentialWrite) {
            AppendDescriptorOutputReadbackHint(output_hints, range);
        }
        return;
    }

    auto cmd_dyn_it = g_command_buffer_descriptor_dynamic_offsets.find(commandBuffer);
    if (cmd_dyn_it == g_command_buffer_descriptor_dynamic_offsets.end()) {
        append_one(0);
        return;
    }
    auto set_dyn_it = cmd_dyn_it->second.find(descriptor_set);
    if (set_dyn_it == cmd_dyn_it->second.end()) {
        append_one(0);
        return;
    }
    auto slot_dyn_it = set_dyn_it->second.find(slot_key);
    if (slot_dyn_it == set_dyn_it->second.end() || slot_dyn_it->second.empty()) {
        append_one(0);
        return;
    }

    for (uint32_t dynamic_offset : slot_dyn_it->second) {
        append_one(dynamic_offset);
    }
}

static void GatherDescriptorSetSyncHintsCachedLocked(
    VkDescriptorSet descriptor_set,
    VkCommandBuffer commandBuffer,
    std::vector<TrackedMemoryRange>* flush_ranges,
    std::vector<OutputMemoryRangeHint>* output_hints) {
    auto set_it = g_descriptor_set_buffer_uses.find(descriptor_set);
    if (set_it == g_descriptor_set_buffer_uses.end()) return;

    bool has_dynamic = false;
    for (const auto& use_pair : set_it->second) {
        if (use_pair.second.uses_dynamic_offset) {
            has_dynamic = true;
            break;
        }
    }

    if (has_dynamic) {
        for (const auto& use_pair : set_it->second) {
            AppendDescriptorUseHintLocked(commandBuffer,
                                          descriptor_set,
                                          use_pair.first,
                                          use_pair.second,
                                          flush_ranges,
                                          output_hints);
        }
        NoteDescriptorHintCacheLookup(false, true, 0, 0, 0);
        return;
    }

    uint64_t version = 0;
    uint64_t hash = 0;
    if (kEnableDescriptorSetSyncHintCache) {
        version = DescriptorSetVersionLocked(descriptor_set);
        hash = HashDescriptorSetUsesLocked(set_it->second);
        auto cache_it = g_descriptor_set_hint_cache.find(descriptor_set);
        if (cache_it != g_descriptor_set_hint_cache.end() &&
            cache_it->second.version == version &&
            cache_it->second.hash == hash) {
            for (const TrackedMemoryRange& range : cache_it->second.flush_ranges) {
                AppendTrackedRange(flush_ranges, range);
            }
            for (const OutputMemoryRangeHint& hint : cache_it->second.output_hints) {
                AppendOutputMemoryRangeHint(output_hints,
                                            hint.range,
                                            hint.source,
                                            hint.strength,
                                            hint.wait_commit_eligible);
            }
            NoteDescriptorHintCacheLookup(true,
                                          false,
                                          cache_it->second.flush_ranges.size(),
                                          cache_it->second.output_hints.size(),
                                          cache_it->second.flush_bytes);
            return;
        }
    }

    DescriptorSetSyncHintCacheEntry built;
    built.version = version;
    built.hash = hash;
    for (const auto& use_pair : set_it->second) {
        AppendDescriptorUseHintLocked(commandBuffer,
                                      descriptor_set,
                                      use_pair.first,
                                      use_pair.second,
                                      &built.flush_ranges,
                                      &built.output_hints);
    }
    CanonicalizeTrackedRanges(&built.flush_ranges);
    CanonicalizeOutputMemoryRangeHints(&built.output_hints);
    built.flush_bytes = TotalTrackedRangeBytes(built.flush_ranges);
    for (const OutputMemoryRangeHint& hint : built.output_hints) {
        built.output_bytes += hint.range.size;
    }

    for (const TrackedMemoryRange& range : built.flush_ranges) {
        AppendTrackedRange(flush_ranges, range);
    }
    for (const OutputMemoryRangeHint& hint : built.output_hints) {
        AppendOutputMemoryRangeHint(output_hints,
                                    hint.range,
                                    hint.source,
                                    hint.strength,
                                    hint.wait_commit_eligible);
    }
    NoteDescriptorHintCacheLookup(false,
                                  false,
                                  built.flush_ranges.size(),
                                  built.output_hints.size(),
                                  built.flush_bytes);
    if (kEnableDescriptorSetSyncHintCache) {
        g_descriptor_set_hint_cache[descriptor_set] = std::move(built);
    }
}

static void GatherSyncHintRangesFromCommandBufferLocked(
    VkCommandBuffer commandBuffer,
    std::vector<TrackedMemoryRange>* flush_ranges,
    std::vector<OutputMemoryRangeHint>* output_hints) {
    auto cmd_it = g_command_buffer_descriptor_sets.find(commandBuffer);
    if (cmd_it != g_command_buffer_descriptor_sets.end()) {
        for (VkDescriptorSet descriptor_set : cmd_it->second) {
            GatherDescriptorSetSyncHintsCachedLocked(descriptor_set,
                                                     commandBuffer,
                                                     flush_ranges,
                                                     output_hints);
        }
    }

    auto flush_it = g_command_buffer_flush_buffer_ranges.find(commandBuffer);
    if (flush_it != g_command_buffer_flush_buffer_ranges.end()) {
        ResolveBufferSyncRangesLocked(flush_it->second, flush_ranges);
    }

    auto invalidate_it = g_command_buffer_invalidate_buffer_ranges.find(commandBuffer);
    if (invalidate_it != g_command_buffer_invalidate_buffer_ranges.end()) {
        ResolveBufferSyncRangesCappedLocked(invalidate_it->second,
                                            kCommandAutoInvalidateMaxBytes,
                                            OutputHintSource::kCopyBufferDst,
                                            OutputHintStrength::kStrongReadback,
                                            output_hints);
    }
}

static void GatherDescriptorUploadRangesForBoundSetsLocked(
    VkCommandBuffer commandBuffer,
    uint32_t descriptorSetCount,
    const VkDescriptorSet* pDescriptorSets,
    std::vector<TrackedMemoryRange>* upload_ranges) {
    if (!pDescriptorSets || descriptorSetCount == 0 || !upload_ranges) return;

    std::vector<OutputMemoryRangeHint> ignored_outputs;
    for (uint32_t i = 0; i < descriptorSetCount; ++i) {
        VkDescriptorSet descriptor_set = pDescriptorSets[i];
        if (descriptor_set == VK_NULL_HANDLE) continue;

        auto set_it = g_descriptor_set_buffer_uses.find(descriptor_set);
        if (set_it == g_descriptor_set_buffer_uses.end()) continue;

        for (const auto& use_pair : set_it->second) {
            AppendDescriptorUseHintLocked(commandBuffer,
                                          descriptor_set,
                                          use_pair.first,
                                          use_pair.second,
                                          upload_ranges,
                                          &ignored_outputs);
        }
    }
}

static bool CommandBufferHasSyncMetadataLocked(VkCommandBuffer commandBuffer) {
    return g_command_buffer_descriptor_sets.find(commandBuffer) != g_command_buffer_descriptor_sets.end() ||
           g_command_buffer_flush_buffer_ranges.find(commandBuffer) != g_command_buffer_flush_buffer_ranges.end() ||
           g_command_buffer_invalidate_buffer_ranges.find(commandBuffer) != g_command_buffer_invalidate_buffer_ranges.end();
}

static void GatherCachedSyncHintRangesFromCommandBufferLocked(
    VkCommandBuffer commandBuffer,
    SubmitSyncHints* hints,
    bool* saw_sync_metadata) {
    if (!hints || commandBuffer == VK_NULL_HANDLE) return;

    const bool has_metadata = CommandBufferHasSyncMetadataLocked(commandBuffer);
    if (!has_metadata) {
        NoteSubmitHintCacheLookup(false, false, 0, 0, 0);
        return;
    }
    if (saw_sync_metadata) *saw_sync_metadata = true;

    uint64_t sig = 0;
    if (kEnableSignatureSubmitHintCache) {
        sig = ComputeCommandBufferSyncSignatureLocked(commandBuffer);
        auto cache_it = g_signature_submit_hint_cache.find(sig);
        if (cache_it != g_signature_submit_hint_cache.end() &&
            cache_it->second.epoch == g_submit_hint_cache_epoch) {
            MergeSubmitSyncHints(hints, cache_it->second.hints);
            NoteSubmitHintCacheLookup(true,
                                      true,
                                      cache_it->second.hints.flush_ranges.size(),
                                      cache_it->second.hints.output_hints.size(),
                                      TotalTrackedRangeBytes(cache_it->second.hints.flush_ranges));
            return;
        }
    }

    SubmitSyncHints built;
    GatherSyncHintRangesFromCommandBufferLocked(commandBuffer,
                                                &built.flush_ranges,
                                                &built.output_hints);
    CanonicalizeTrackedRanges(&built.flush_ranges);
    CanonicalizeOutputMemoryRangeHints(&built.output_hints);
    built.present = true;
    if (kEnableSignatureSubmitHintCache) {
        g_signature_submit_hint_cache[sig] = {g_submit_hint_cache_epoch, built};
    }
    MergeSubmitSyncHints(hints, built);
    NoteSubmitHintCacheLookup(false,
                              true,
                              built.flush_ranges.size(),
                              built.output_hints.size(),
                              TotalTrackedRangeBytes(built.flush_ranges));
}

static void ConsumePendingUploadWaitRangesLocked(const std::vector<TrackedMemoryRange>& submit_flush_ranges,
                                                 std::vector<TrackedMemoryRange>* wait_ranges) {
    if (!wait_ranges || g_pending_upload_wait_ranges.empty()) return;

    std::vector<TrackedMemoryRange> remaining;
    remaining.reserve(g_pending_upload_wait_ranges.size());
    for (const TrackedMemoryRange& pending : g_pending_upload_wait_ranges) {
        bool consumed =
            UseConservativeMappedReadbackFallback() || submit_flush_ranges.empty();
        for (const TrackedMemoryRange& submitted : submit_flush_ranges) {
            if (TrackedRangesOverlap(pending, submitted)) {
                consumed = true;
                break;
            }
        }

        if (consumed) {
            AppendTrackedRange(wait_ranges, pending);
        } else {
            remaining.push_back(pending);
        }
    }

    g_pending_upload_wait_ranges.swap(remaining);
    CanonicalizeTrackedRanges(wait_ranges);
}

static void FinalizeSubmitWaitFlushRanges(SubmitSyncHints* hints) {
    if (!hints) return;

    std::lock_guard<std::mutex> lock(g_mapped_mutex);
    if (!hints->present && g_pending_upload_wait_ranges.empty()) return;

    hints->wait_flush_ranges.clear();
    ConsumePendingUploadWaitRangesLocked(hints->flush_ranges, &hints->wait_flush_ranges);
    if (!hints->wait_flush_ranges.empty()) {
        hints->present = true;
        if (kEnableImplicitSyncDiagLog) {
            ALOGI("[SYNC_GUEST] submit_wait_upload_hints ranges=%zu bytes=%llu",
                  hints->wait_flush_ranges.size(),
                  (unsigned long long)TotalTrackedRangeBytes(hints->wait_flush_ranges));
        }
    }
}

static SubmitSyncHints PrimeFlushHintsFromSubmitInfos(uint32_t submitCount,
                                                      const VkSubmitInfo* pSubmits) {
    SubmitSyncHints hints;
    if (!pSubmits || submitCount == 0) return hints;

    bool saw_sync_metadata = false;
    {
        std::lock_guard<std::mutex> lock(g_mapped_mutex);
        const uint64_t submit_generation = ++g_submit_generation;
        for (auto& kv : g_active_mapped_memories) {
            kv.second.last_submit_generation = submit_generation;
        }

        for (uint32_t i = 0; i < submitCount; ++i) {
            const VkSubmitInfo& submit = pSubmits[i];
            for (uint32_t j = 0; j < submit.commandBufferCount; ++j) {
                GatherCachedSyncHintRangesFromCommandBufferLocked(
                    submit.pCommandBuffers[j], &hints, &saw_sync_metadata);
            }
        }

        CanonicalizeTrackedRanges(&hints.flush_ranges);
        CanonicalizeOutputMemoryRangeHints(&hints.output_hints);
        SuppressWeakDescriptorReadbacksIfStrongCopyPresent(&hints, submitCount);
        CanonicalizeOutputMemoryRangeHints(&hints.output_hints);

        if (!hints.flush_ranges.empty()) {
            g_flush_hint_ranges.insert(
                g_flush_hint_ranges.end(), hints.flush_ranges.begin(), hints.flush_ranges.end());
            CanonicalizeTrackedRanges(&g_flush_hint_ranges);
        }
        if (!hints.output_hints.empty()) {
            std::vector<TrackedMemoryRange> output_ranges;
            AppendOutputHintRanges(hints.output_hints, &output_ranges);
            CanonicalizeTrackedRanges(&output_ranges);
            uint64_t output_bytes = 0;
            for (const TrackedMemoryRange& range : output_ranges) {
                output_bytes += range.size;
            }
            if (!output_ranges.empty() && kEnableImplicitSyncDiagLog) {
                EVK_PER_CALL_LOG("[SYNC_GUEST] output_hint_ranges api=vkQueueSubmit generation=%llu ranges=%zu bytes=%llu",
                                 (unsigned long long)submit_generation,
                                 output_ranges.size(),
                                 (unsigned long long)output_bytes);
            }
            g_invalidate_hint_ranges.insert(
                g_invalidate_hint_ranges.end(), output_ranges.begin(), output_ranges.end());
            CanonicalizeTrackedRanges(&g_invalidate_hint_ranges);
        }
        hints.present = saw_sync_metadata;
        g_skip_next_implicit_flush_scan = saw_sync_metadata && hints.flush_ranges.empty();
    }
    return hints;
}

static SubmitSyncHints PrimeFlushHintsFromSubmitInfo2(uint32_t submitCount,
                                                      const VkSubmitInfo2* pSubmits) {
    SubmitSyncHints hints;
    if (!pSubmits || submitCount == 0) return hints;

    bool saw_sync_metadata = false;
    {
        std::lock_guard<std::mutex> lock(g_mapped_mutex);
        const uint64_t submit_generation = ++g_submit_generation;
        for (auto& kv : g_active_mapped_memories) {
            kv.second.last_submit_generation = submit_generation;
        }

        for (uint32_t i = 0; i < submitCount; ++i) {
            const VkSubmitInfo2& submit = pSubmits[i];
            for (uint32_t j = 0; j < submit.commandBufferInfoCount; ++j) {
                GatherCachedSyncHintRangesFromCommandBufferLocked(
                    submit.pCommandBufferInfos[j].commandBuffer,
                    &hints,
                    &saw_sync_metadata);
            }
        }

        CanonicalizeTrackedRanges(&hints.flush_ranges);
        CanonicalizeOutputMemoryRangeHints(&hints.output_hints);
        SuppressWeakDescriptorReadbacksIfStrongCopyPresent(&hints, submitCount);
        CanonicalizeOutputMemoryRangeHints(&hints.output_hints);

        if (!hints.flush_ranges.empty()) {
            g_flush_hint_ranges.insert(
                g_flush_hint_ranges.end(), hints.flush_ranges.begin(), hints.flush_ranges.end());
            CanonicalizeTrackedRanges(&g_flush_hint_ranges);
        }
        if (!hints.output_hints.empty()) {
            std::vector<TrackedMemoryRange> output_ranges;
            AppendOutputHintRanges(hints.output_hints, &output_ranges);
            CanonicalizeTrackedRanges(&output_ranges);
            uint64_t output_bytes = 0;
            for (const TrackedMemoryRange& range : output_ranges) {
                output_bytes += range.size;
            }
            if (!output_ranges.empty() && kEnableImplicitSyncDiagLog) {
                EVK_PER_CALL_LOG("[SYNC_GUEST] output_hint_ranges api=vkQueueSubmit2 generation=%llu ranges=%zu bytes=%llu",
                                 (unsigned long long)submit_generation,
                                 output_ranges.size(),
                                 (unsigned long long)output_bytes);
            }
            g_invalidate_hint_ranges.insert(
                g_invalidate_hint_ranges.end(), output_ranges.begin(), output_ranges.end());
            CanonicalizeTrackedRanges(&g_invalidate_hint_ranges);
        }
        hints.present = saw_sync_metadata;
        g_skip_next_implicit_flush_scan = saw_sync_metadata && hints.flush_ranges.empty();
    }
    return hints;
}

// static void TrackBufferMemoryBinding(VkDevice device,
//                                      VkBuffer buffer,
//                                      VkDeviceMemory memory,
//                                      VkDeviceSize memoryOffset) {
//     if (buffer == VK_NULL_HANDLE || memory == VK_NULL_HANDLE) return;
//     std::lock_guard<std::mutex> lock(g_mapped_mutex);
//     VkDeviceSize buffer_size = 0;
//     auto size_it = g_buffer_sizes.find(buffer);
//     if (size_it != g_buffer_sizes.end()) {
//         buffer_size = size_it->second;
//     }
//     g_buffer_memory_bindings[buffer] = {device, memory, memoryOffset, buffer_size};
// }

// static void ForgetTrackedBuffer(VkBuffer buffer) {
//     if (buffer == VK_NULL_HANDLE) return;
//     std::lock_guard<std::mutex> lock(g_mapped_mutex);
//     g_buffer_memory_bindings.erase(buffer);
//     g_buffer_sizes.erase(buffer);
// }

// static void ForgetTrackedCommandBuffer(VkCommandBuffer commandBuffer) {
//     if (commandBuffer == VK_NULL_HANDLE) return;
//     std::lock_guard<std::mutex> lock(g_mapped_mutex);
//     g_command_buffer_descriptor_sets.erase(commandBuffer);
// }

// static void ForgetTrackedDescriptorSets(uint32_t count,
//                                         const VkDescriptorSet* pDescriptorSets) {
//     if (!pDescriptorSets || count == 0) return;
//     std::lock_guard<std::mutex> lock(g_mapped_mutex);
//     for (uint32_t i = 0; i < count; ++i) {
//         g_descriptor_set_buffer_uses.erase(pDescriptorSets[i]);
//     }
// }

static void MarkBufferAsFlushHint(VkBuffer buffer) {
    if (buffer == VK_NULL_HANDLE) return;
    std::lock_guard<std::mutex> lock(g_mapped_mutex);
    auto bit = g_buffer_memory_bindings.find(buffer);
    if (bit == g_buffer_memory_bindings.end()) return;
    if (g_active_mapped_memories.find(bit->second.memory) == g_active_mapped_memories.end()) return;
    g_flush_hint_memories.insert(bit->second.memory);
}

static void MarkBufferAsInvalidateHint(VkBuffer buffer) {
    if (buffer == VK_NULL_HANDLE) return;
    std::lock_guard<std::mutex> lock(g_mapped_mutex);
    auto bit = g_buffer_memory_bindings.find(buffer);
    if (bit == g_buffer_memory_bindings.end()) return;
    if (g_active_mapped_memories.find(bit->second.memory) == g_active_mapped_memories.end()) return;
    g_invalidate_hint_memories.insert(bit->second.memory);
}

static void MarkBufferRangeAsFlushHint(VkBuffer buffer,
                                       VkDeviceSize offset,
                                       VkDeviceSize size) {
    if (buffer == VK_NULL_HANDLE || size == 0) return;
    std::lock_guard<std::mutex> lock(g_mapped_mutex);
    TrackedMemoryRange range = {};
    if (ResolveBufferRangeLocked(buffer, offset, size, &range)) {
        AppendTrackedRange(&g_flush_hint_ranges, range);
    }
}

static void MarkBufferRangeAsInvalidateHint(VkBuffer buffer,
                                            VkDeviceSize offset,
                                            VkDeviceSize size) {
    if (buffer == VK_NULL_HANDLE || size == 0) return;
    std::lock_guard<std::mutex> lock(g_mapped_mutex);
    TrackedMemoryRange range = {};
    if (ResolveBufferRangeLocked(buffer, offset, size, &range)) {
        AppendTrackedRange(&g_invalidate_hint_ranges, range);
    }
}

static void RecordRecentlyFlushedRanges(uint32_t memoryRangeCount,
                                        const VkMappedMemoryRange* pMemoryRanges) {
    if (!pMemoryRanges || memoryRangeCount == 0) return;
    std::lock_guard<std::mutex> lock(g_mapped_mutex);
    for (uint32_t i = 0; i < memoryRangeCount; ++i) {
        if (pMemoryRanges[i].memory != VK_NULL_HANDLE) {
            g_recently_flushed_memories.insert(pMemoryRanges[i].memory);
        }
    }
    RecordMappedRangeSpansLocked(memoryRangeCount, pMemoryRanges, true, false);
}

static void RecordRecentlyInvalidatedRanges(uint32_t memoryRangeCount,
                                            const VkMappedMemoryRange* pMemoryRanges) {
    if (!pMemoryRanges || memoryRangeCount == 0) return;
    std::lock_guard<std::mutex> lock(g_mapped_mutex);
    RecordMappedRangeSpansLocked(memoryRangeCount, pMemoryRanges, false, true);
}

static void StoreFenceInvalidateHints(VkFence fence, const SubmitSyncHints& hints) {
    if (fence == VK_NULL_HANDLE || hints.output_hints.empty()) return;

    std::vector<TrackedMemoryRange> registered_ranges;
    registered_ranges.reserve(hints.output_hints.size());
    {
        std::lock_guard<std::mutex> lock(g_mapped_mutex);
        for (const OutputMemoryRangeHint& output_hint : hints.output_hints) {
            if (!ShouldAutoInvalidateOutputHintLocked(output_hint)) continue;
            const TrackedMemoryRange& range = output_hint.range;
            if (range.memory == VK_NULL_HANDLE || range.size == 0) continue;
            auto it = g_active_mapped_memories.find(range.memory);
            if (it == g_active_mapped_memories.end()) continue;
            if (!IsExpressVkRegisteredMemoryHandle(range.memory)) continue;
            const VkDeviceSize size =
                ClampMappedRangeSize(it->second.size, range.offset, range.size);
            if (size == 0) continue;
            if (size > kWaitCommittedInvalidateMaxBytes) continue;
            registered_ranges.push_back({range.device, range.memory, range.offset, size});
        }

        if (registered_ranges.empty()) {
            g_fence_pending_invalidate_ranges.erase(fence);
        } else {
            g_fence_pending_invalidate_ranges[fence] = std::move(registered_ranges);
        }
    }
}

static size_t MarkFenceInvalidateHintsCommitted(uint32_t fenceCount,
                                                const VkFence* pFences,
                                                VkBool32 waitAll) {
    if (!pFences || fenceCount == 0) return 0;
    if (!waitAll && fenceCount != 1) return 0;

    size_t committed_ranges = 0;
    uint64_t committed_bytes = 0;
    std::lock_guard<std::mutex> lock(g_mapped_mutex);
    for (uint32_t i = 0; i < fenceCount; ++i) {
        auto fit = g_fence_pending_invalidate_ranges.find(pFences[i]);
        if (fit == g_fence_pending_invalidate_ranges.end()) continue;

        for (const TrackedMemoryRange& range : fit->second) {
            auto mit = g_active_mapped_memories.find(range.memory);
            if (mit == g_active_mapped_memories.end()) continue;
            ActiveMappedMemoryRecord& rec = mit->second;
            const VkDeviceSize size =
                ClampMappedRangeSize(rec.size, range.offset, range.size);
            if (size == 0) continue;

            AppendMemoryRangeSpan(&rec.recently_invalidated_ranges,
                                  range.offset,
                                  size,
                                  rec.last_submit_generation);
            committed_ranges++;
            committed_bytes += size;
        }
        g_fence_pending_invalidate_ranges.erase(fit);
    }

    if (committed_ranges != 0 && kEnableImplicitSyncDiagLog) {
        ALOGD("[SYNC_GUEST] wait_committed_invalidate ranges=%zu bytes=%llu",
              committed_ranges,
              (unsigned long long)committed_bytes);
    }
    return committed_ranges;
}

static void UpdateShadowWithRanges(VkDevice device,
                                   uint32_t memoryRangeCount,
                                   const VkMappedMemoryRange* pMemoryRanges) {
    if (!pMemoryRanges || memoryRangeCount == 0) return;

    uint32_t skipped_registered_ranges = 0;
    uint64_t skipped_registered_bytes = 0;

    std::lock_guard<std::mutex> lock(g_mapped_mutex);
    for (uint32_t i = 0; i < memoryRangeCount; ++i) {
        auto it = g_active_mapped_memories.find(pMemoryRanges[i].memory);
        if (it == g_active_mapped_memories.end()) continue;

        ActiveMappedMemoryRecord& rec = it->second;
        if (rec.device != device || !rec.map_data || rec.size == 0) continue;

        VkDeviceSize offset_bytes = pMemoryRanges[i].offset;
        if (offset_bytes > rec.size) offset_bytes = rec.size;

        VkDeviceSize size_bytes = pMemoryRanges[i].size;
        if (size_bytes == VK_WHOLE_SIZE ||
            offset_bytes + size_bytes > rec.size ||
            offset_bytes + size_bytes < offset_bytes) {
            size_bytes = rec.size - offset_bytes;
        }

        if (size_bytes == 0) continue;

        EraseMemoryRangeSpanOverlaps(&rec.recently_clean_submit_ranges,
                                     offset_bytes,
                                     size_bytes);
        rec.submit_clean_streak = 0;

        const bool registered_memory = IsExpressVkRegisteredMemoryHandle(pMemoryRanges[i].memory);
        const bool has_shadow_baseline = rec.shadow.size() == rec.size;
        if (kAllowLargeRegisteredShadowUpdateSkip &&
            registered_memory &&
            has_shadow_baseline &&
            size_bytes >= kRegisteredShadowUpdateCopyLimitBytes) {
            skipped_registered_ranges++;
            skipped_registered_bytes += (uint64_t)size_bytes;
            continue;
        }

        if (rec.shadow.size() != rec.size) {
            rec.shadow.resize(rec.size);
        }

        memcpy(rec.shadow.data() + offset_bytes, rec.map_data + offset_bytes, (size_t)size_bytes);
    }

    if (skipped_registered_ranges != 0 && kEnableImplicitSyncDiagLog) {
        ALOGD("[SYNC_GUEST] shadow_update_skip_registered ranges=%u bytes=%llu threshold=%llu",
              skipped_registered_ranges,
              (unsigned long long)skipped_registered_bytes,
              (unsigned long long)kRegisteredShadowUpdateCopyLimitBytes);
    }
}

static void SampleInvalidateChanges(VkDevice device,
                                    uint32_t memoryRangeCount,
                                    const VkMappedMemoryRange* pMemoryRanges,
                                    const char* reason) {
    if (!kEnableReadbackChangeSampling) return;
    if (!pMemoryRanges || memoryRangeCount == 0) return;

    const InvalidateChangeClass cls = ClassifyInvalidateReason(reason);
    uint64_t ranges = 0;
    uint64_t bytes = 0;
    uint64_t sample_bytes = 0;
    uint64_t changed_samples = 0;
    uint64_t clean_samples = 0;
    uint64_t no_shadow = 0;
    uint64_t untracked = 0;

    {
        std::lock_guard<std::mutex> lock(g_mapped_mutex);
        for (uint32_t i = 0; i < memoryRangeCount; ++i) {
            auto it = g_active_mapped_memories.find(pMemoryRanges[i].memory);
            if (it == g_active_mapped_memories.end()) {
                untracked++;
                continue;
            }

            ActiveMappedMemoryRecord& rec = it->second;
            if (rec.device != device || !rec.map_data || rec.size == 0) {
                untracked++;
                continue;
            }

            VkDeviceSize offset_bytes = pMemoryRanges[i].offset;
            if (offset_bytes > rec.size) offset_bytes = rec.size;

            VkDeviceSize size_bytes = pMemoryRanges[i].size;
            if (size_bytes == VK_WHOLE_SIZE ||
                offset_bytes + size_bytes > rec.size ||
                offset_bytes + size_bytes < offset_bytes) {
                size_bytes = rec.size - offset_bytes;
            }
            if (size_bytes == 0) continue;

            ranges++;
            bytes += (uint64_t)size_bytes;

            if (rec.shadow.size() != rec.size) {
                no_shadow++;
                continue;
            }

            const VkDeviceSize sample_bytes_this_range =
                std::min<VkDeviceSize>(size_bytes, kReadbackChangeSampleBytes);
            if (sample_bytes_this_range == 0) continue;

            const uint8_t* before = rec.shadow.data() + offset_bytes;
            const uint8_t* after =
                reinterpret_cast<const uint8_t*>(rec.map_data) + offset_bytes;
            sample_bytes += (uint64_t)sample_bytes_this_range;
            if (memcmp(before, after, (size_t)sample_bytes_this_range) == 0) {
                clean_samples++;
            } else {
                changed_samples++;
            }
        }
    }

    NoteInvalidateChangeStats(cls,
                              ranges,
                              bytes,
                              sample_bytes,
                              changed_samples,
                              clean_samples,
                              no_shadow,
                              untracked,
                              reason);
}

static void ImplicitFlushAllMappedMemories() {
    const uint64_t total_start_us = ExpressVkNowUs();
    SyncPolicyBatch policy_batch;
    policy_batch.classes.reserve(8);

    if (UseStrictShadowOracleSync()) {
        std::lock_guard<std::mutex> lock(g_mapped_mutex);
        g_flush_hint_ranges.clear();
        g_flush_hint_memories.clear();
        g_skip_next_implicit_flush_scan = false;
        ResetImplicitCleanScanThrottleLocked();
    }

    // Fast path for common matmul staging pattern: src upload buffers are known
    // from command recording and are significantly cheaper than whole-map scan.
    if (!UseStrictShadowOracleSync())
    {
        std::unordered_map<VkDevice, std::vector<VkMappedMemoryRange>> hint_ranges_by_dev;
        uint64_t hint_bytes = 0;
        uint64_t hint_ranges = 0;
        size_t hint_memories = 0;
        size_t mapped_count = 0;
        TrackedRangeUploadCheckStats hint_check_stats = {};
        const uint64_t hint_scan_start_us = ExpressVkNowUs();
        {
            std::lock_guard<std::mutex> lock(g_mapped_mutex);
            mapped_count = g_active_mapped_memories.size();
            if (!g_flush_hint_ranges.empty()) {
                for (const TrackedMemoryRange& tracked : g_flush_hint_ranges) {
                    TrackedMemoryRange normalized = {};
                    if (!TrackedRangeNeedsSubmitUploadLocked(
                            tracked, &normalized, &hint_check_stats, &policy_batch)) {
                        continue;
                    }
                    VkMappedMemoryRange r = {};
                    r.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
                    r.pNext = nullptr;
                    r.memory = normalized.memory;
                    r.offset = normalized.offset;
                    r.size = normalized.size;
                    hint_ranges_by_dev[normalized.device].push_back(r);
                    hint_bytes += normalized.size;
                    hint_ranges++;
                }
                g_flush_hint_ranges.clear();
            }

            if (!g_flush_hint_memories.empty()) {
                hint_memories = g_flush_hint_memories.size();
                for (VkDeviceMemory mem : g_flush_hint_memories) {
                    auto it = g_active_mapped_memories.find(mem);
                    if (it == g_active_mapped_memories.end()) continue;
                    const ActiveMappedMemoryRecord& rec = it->second;
                    if (!rec.map_data || rec.size == 0) continue;
                    const uint32_t policy_key =
                        MakeSyncPolicyKey(SyncPolicySource::kHintMemory,
                                          0,
                                          rec.size,
                                          IsExpressVkRegisteredMemoryHandle(mem),
                                          rec.submit_clean_streak);
                    AddSyncPolicyObservation(&policy_batch,
                                             policy_key,
                                             1,
                                             rec.size,
                                             0,
                                             false,
                                             false,
                                             true,
                                             false,
                                             true,
                                             false,
                                             0);

                    VkMappedMemoryRange r = {};
                    r.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
                    r.pNext = nullptr;
                    r.memory = rec.memory;
                    r.offset = 0;
                    r.size = rec.size;
                    hint_ranges_by_dev[rec.device].push_back(r);
                    hint_bytes += rec.size;
                    hint_ranges++;
                }
                g_flush_hint_memories.clear();
            }
        }

        if (hint_ranges > 0) {
            const uint64_t hint_scan_done_us = ExpressVkNowUs();
            g_flush_hint_rounds++;
            if (kPeriodicFullSyncEvery > 0 && (g_flush_hint_rounds % (kPeriodicFullSyncEvery ? kPeriodicFullSyncEvery : 1)) == 0) {
                if (kEnableImplicitSyncDiagLog) {
                    ALOGD("[SYNC_GUEST] implicit_flush_hint seq=%llu mode=periodic_full_fallback",
                          (unsigned long long)(++g_sync_trace_seq));
                }
                // Fall through to legacy scan path periodically as a safety net.
            } else {
            const uint64_t rpc_start_us = ExpressVkNowUs();
            for (const auto& pair : hint_ranges_by_dev) {
                FlushMappedMemoryRanges(pair.first, pair.second.size(), pair.second.data());
            }
            const uint64_t rpc_done_us = ExpressVkNowUs();
            NoteImplicitFlushStats("hint",
                                   mapped_count,
                                   hint_ranges,
                                   hint_bytes,
                                   0,
                                   0,
                                   0,
                                   0,
                                   0,
                                   hint_ranges_by_dev.size(),
                                   hint_check_stats.clean_cache_hits,
                                   hint_check_stats.memcmp_checks,
                                   hint_check_stats.memcmp_bytes,
                                   hint_check_stats.dirty_ranges,
                                   hint_scan_done_us - hint_scan_start_us,
                                   rpc_done_us - rpc_start_us,
                                   rpc_done_us - total_start_us);
            AddSyncPolicyBatchCost(&policy_batch, hint_scan_done_us - hint_scan_start_us);
            NoteSyncPolicyBatch(policy_batch);
            if (kEnableImplicitSyncDiagLog) {
                ALOGD("[SYNC_GUEST] implicit_flush_hint seq=%llu hinted_memories=%zu ranges=%llu bytes=%llu rpc_ms=%.3f",
                      (unsigned long long)(++g_sync_trace_seq),
                      hint_memories,
                      (unsigned long long)hint_ranges,
                      (unsigned long long)hint_bytes,
                      (double)(rpc_done_us - rpc_start_us) / 1000.0);
            }
            return;
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_mapped_mutex);
        if (!UseStrictShadowOracleSync() && g_skip_next_implicit_flush_scan) {
            g_skip_next_implicit_flush_scan = false;
            NoteImplicitFlushStats("skip_metadata",
                                   g_active_mapped_memories.size(),
                                   0,
                                   0,
                                   0,
                                   0,
                                   0,
                                   0,
                                   0,
                                   0,
                                   0,
                                   0,
                                   0,
                                   0,
                                   0,
                                   0,
                                   ExpressVkNowUs() - total_start_us);
            AddSyncPolicyObservation(&policy_batch,
                                     MakeSyncPolicyKey(SyncPolicySource::kMetadataSkip,
                                                       0,
                                                       0,
                                                       false,
                                                       0),
                                     g_active_mapped_memories.size(),
                                     0,
                                     0,
                                     false,
                                     false,
                                     true,
                                     false,
                                     false,
                                     true,
                                     0);
            AddSyncPolicyBatchCost(&policy_batch, ExpressVkNowUs() - total_start_us);
            NoteSyncPolicyBatch(policy_batch);
            if (kEnableImplicitSyncDiagLog) {
                ALOGD("[SYNC_GUEST] implicit_flush_skip seq=%llu reason=metadata_without_cpu_input",
                      (unsigned long long)(++g_sync_trace_seq));
            }
            return;
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_mapped_mutex);
        if (kEnableNoHintCleanScanThrottle &&
            g_no_hint_clean_scan_skip_remaining > 0 &&
            !g_active_mapped_memories.empty()) {
            g_no_hint_clean_scan_skip_remaining--;
            const size_t skipped_memories = g_active_mapped_memories.size();
            NoteImplicitFlushStats("skip_clean_scan",
                                   skipped_memories,
                                   0,
                                   0,
                                   0,
                                   0,
                                   0,
                                   0,
                                   0,
                                   0,
                                   0,
                                   0,
                                   0,
                                   0,
                                   0,
                                   0,
                                   ExpressVkNowUs() - total_start_us);
            AddSyncPolicyObservation(&policy_batch,
                                     MakeSyncPolicyKey(SyncPolicySource::kNoHintCleanSkip,
                                                       0,
                                                       0,
                                                       false,
                                                       g_no_hint_clean_scan_streak),
                                     skipped_memories,
                                     0,
                                     0,
                                     false,
                                     false,
                                     true,
                                     false,
                                     false,
                                     true,
                                     0);
            AddSyncPolicyBatchCost(&policy_batch, ExpressVkNowUs() - total_start_us);
            NoteSyncPolicyBatch(policy_batch);
            if (kEnableImplicitSyncDiagLog) {
                ALOGD("[SYNC_GUEST] implicit_flush_skip seq=%llu reason=no_hint_clean_cache remaining=%u clean_streak=%u",
                      (unsigned long long)(++g_sync_trace_seq),
                      g_no_hint_clean_scan_skip_remaining,
                      g_no_hint_clean_scan_streak);
            }
            return;
        }
    }

    struct timespec t0_scan, t1_scan, t0_rpc, t1_rpc;
    clock_gettime(CLOCK_MONOTONIC, &t0_scan);

    std::unordered_map<VkDevice, std::vector<VkMappedMemoryRange>> ranges_by_dev;
    size_t mapped_count = 0;
    uint64_t scanned_bytes = 0;
    uint64_t compared_chunks = 0;
    uint64_t dirty_chunks = 0;
    uint64_t dirty_bytes = 0;
    uint64_t dirty_ranges = 0;

    {
        std::lock_guard<std::mutex> lock(g_mapped_mutex);
        if (g_active_mapped_memories.empty()) {
            NoteImplicitFlushStats("empty",
                                   0,
                                   0,
                                   0,
                                   0,
                                   0,
                                   0,
                                   0,
                                   0,
                                   0,
                                   0,
                                   0,
                                   0,
                                   0,
                                   0,
                                   0,
                                   ExpressVkNowUs() - total_start_us);
            AddSyncPolicyObservation(&policy_batch,
                                     MakeSyncPolicyKey(SyncPolicySource::kEmpty,
                                                       0,
                                                       0,
                                                       false,
                                                       0),
                                     1,
                                     0,
                                     0,
                                     false,
                                     false,
                                     true,
                                     false,
                                     false,
                                     true,
                                     0);
            AddSyncPolicyBatchCost(&policy_batch, ExpressVkNowUs() - total_start_us);
            NoteSyncPolicyBatch(policy_batch);
            return;
        }

        mapped_count = g_active_mapped_memories.size();

        for (auto& kv : g_active_mapped_memories) {
            ActiveMappedMemoryRecord& rec = kv.second;
            if (!rec.map_data || rec.size == 0) continue;
            const uint32_t memory_policy_key =
                MakeSyncPolicyKey(SyncPolicySource::kNoHintScan,
                                  0,
                                  rec.size,
                                  IsExpressVkRegisteredMemoryHandle(rec.memory),
                                  rec.submit_clean_streak);
            uint64_t rec_dirty_bytes = 0;

            if (rec.shadow.size() != rec.size) {
                rec.shadow.resize(rec.size);
                memcpy(rec.shadow.data(), rec.map_data, (size_t)rec.size);
                rec.submit_clean_streak = 0;

                VkMappedMemoryRange r = {};
                r.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
                r.pNext = nullptr;
                r.memory = rec.memory;
                r.offset = 0;
                r.size = rec.size;
                ranges_by_dev[rec.device].push_back(r);
                dirty_bytes += (uint64_t)r.size;
                dirty_ranges++;
                rec_dirty_bytes += (uint64_t)r.size;
                AddSyncPolicyObservation(&policy_batch,
                                         memory_policy_key,
                                         1,
                                         rec.size,
                                         rec.size,
                                         false,
                                         true,
                                         false,
                                         false,
                                         true,
                                         false,
                                         0);
                continue;
            }

            VkDeviceSize range_start = 0;
            VkDeviceSize range_end = 0;
            bool in_range = false;

            for (VkDeviceSize off = 0; off < rec.size; off += kImplicitDirtyChunkBytes) {
                VkDeviceSize chunk = std::min<VkDeviceSize>(
                    kImplicitDirtyChunkBytes, rec.size - off);
                scanned_bytes += (uint64_t)chunk;
                compared_chunks++;

                if (memcmp(rec.map_data + off, rec.shadow.data() + off, (size_t)chunk) != 0) {
                    dirty_chunks++;
                    EraseMemoryRangeSpanOverlaps(&rec.recently_clean_submit_ranges,
                                                 off,
                                                 chunk);
                    rec.submit_clean_streak = 0;
                    rec_dirty_bytes += (uint64_t)chunk;
                    memcpy(rec.shadow.data() + off, rec.map_data + off, (size_t)chunk);
                    if (!in_range) {
                        range_start = off;
                        range_end = off + chunk;
                        in_range = true;
                    } else {
                        range_end = off + chunk;
                    }
                } else if (in_range) {
                    VkMappedMemoryRange r = {};
                    r.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
                    r.pNext = nullptr;
                    r.memory = rec.memory;
                    r.offset = range_start;
                    r.size = range_end - range_start;
                    ranges_by_dev[rec.device].push_back(r);
                    dirty_bytes += (uint64_t)r.size;
                    dirty_ranges++;
                    in_range = false;
                }
            }

            if (in_range) {
                VkMappedMemoryRange r = {};
                r.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
                r.pNext = nullptr;
                r.memory = rec.memory;
                r.offset = range_start;
                r.size = range_end - range_start;
                ranges_by_dev[rec.device].push_back(r);
                dirty_bytes += (uint64_t)r.size;
                dirty_ranges++;
            }

            if (rec_dirty_bytes == 0) {
                if (rec.submit_clean_streak != UINT32_MAX) {
                    rec.submit_clean_streak++;
                }
            }
            AddSyncPolicyObservation(&policy_batch,
                                     memory_policy_key,
                                     1,
                                     rec.size,
                                     rec_dirty_bytes,
                                     rec_dirty_bytes == 0,
                                     rec_dirty_bytes != 0,
                                     false,
                                     false,
                                     rec_dirty_bytes != 0,
                                     rec_dirty_bytes == 0,
                                     0);
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &t1_scan);
    double scan_ms = (t1_scan.tv_sec - t0_scan.tv_sec) * 1000.0 +
                     (t1_scan.tv_nsec - t0_scan.tv_nsec) / 1000000.0;

    uint64_t flush_seq = ++g_sync_trace_seq;

    if (dirty_ranges == 0) {
        {
            std::lock_guard<std::mutex> lock(g_mapped_mutex);
            if (kEnableNoHintCleanScanThrottle) {
                g_no_hint_clean_scan_streak++;
                if (g_no_hint_clean_scan_streak >= kNoHintCleanScanWarmup &&
                    g_no_hint_clean_scan_skip_remaining == 0) {
                    g_no_hint_clean_scan_skip_remaining = kNoHintCleanScanSkipBudget;
                }
            }
        }
        NoteImplicitFlushStats("scan",
                               mapped_count,
                               0,
                               0,
                               scanned_bytes,
                               compared_chunks,
                               dirty_chunks,
                               dirty_ranges,
                               dirty_bytes,
                               0,
                               0,
                               0,
                               0,
                               0,
                               ExpressVkElapsedUs(t0_scan, t1_scan),
                               0,
                               ExpressVkNowUs() - total_start_us);
        AddSyncPolicyBatchCost(&policy_batch, ExpressVkElapsedUs(t0_scan, t1_scan));
        NoteSyncPolicyBatch(policy_batch);
        if (kEnableImplicitSyncDiagLog) {
            ALOGD("[SYNC_GUEST] implicit_flush_diag seq=%llu mapped=%zu ranges=0 bytes=0 scan_ms=%.3f scanned_bytes=%llu chunks=%llu dirty_chunks=0",
                  (unsigned long long)flush_seq,
                  mapped_count,
                  scan_ms,
                  (unsigned long long)scanned_bytes,
                  (unsigned long long)compared_chunks);
        }
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_mapped_mutex);
        ResetImplicitCleanScanThrottleLocked();
    }

    clock_gettime(CLOCK_MONOTONIC, &t0_rpc);

    for (const auto& pair : ranges_by_dev) {
        FlushMappedMemoryRanges(pair.first, pair.second.size(), pair.second.data());
    }

    clock_gettime(CLOCK_MONOTONIC, &t1_rpc);
    double rpc_ms = (t1_rpc.tv_sec - t0_rpc.tv_sec) * 1000.0 +
                    (t1_rpc.tv_nsec - t0_rpc.tv_nsec) / 1000000.0;
    NoteImplicitFlushStats("scan",
                           mapped_count,
                           0,
                           0,
                           scanned_bytes,
                           compared_chunks,
                           dirty_chunks,
                           dirty_ranges,
                           dirty_bytes,
                           ranges_by_dev.size(),
                           0,
                           0,
                           0,
                           0,
                           ExpressVkElapsedUs(t0_scan, t1_scan),
                           ExpressVkElapsedUs(t0_rpc, t1_rpc),
                           ExpressVkNowUs() - total_start_us);
    AddSyncPolicyBatchCost(&policy_batch, ExpressVkElapsedUs(t0_scan, t1_scan));
    NoteSyncPolicyBatch(policy_batch);

    if (kEnableImplicitSyncDiagLog) {
        ALOGD("[SYNC_GUEST] implicit_flush_diag seq=%llu mapped=%zu ranges=%llu bytes=%llu scan_ms=%.3f rpc_ms=%.3f scanned_bytes=%llu chunks=%llu dirty_chunks=%llu",
              (unsigned long long)flush_seq,
              mapped_count,
              (unsigned long long)dirty_ranges,
              (unsigned long long)dirty_bytes,
              scan_ms,
              rpc_ms,
              (unsigned long long)scanned_bytes,
              (unsigned long long)compared_chunks,
              (unsigned long long)dirty_chunks);
    }
}

static void ImplicitInvalidateAllMappedMemories() {
    std::unordered_map<VkDevice, std::vector<VkMappedMemoryRange>> ranges_by_dev;
    size_t mapped_count = 0;
    uint64_t total_bytes = 0;
    size_t hinted_memories = 0;
    size_t skipped_large = 0;
    size_t skipped_synced = 0;
    bool used_hint = false;
    bool used_flush_filter = false;

    {
        std::lock_guard<std::mutex> lock(g_mapped_mutex);
        if (g_active_mapped_memories.empty()) return;

        mapped_count = g_active_mapped_memories.size();

        auto add_range = [&](const ActiveMappedMemoryRecord& rec,
                             VkDeviceSize offset,
                             VkDeviceSize size) {
            AppendMappedRangeForInvalidateLocked(&ranges_by_dev,
                                                 rec,
                                                 offset,
                                                 size,
                                                 &total_bytes,
                                                 &skipped_synced);
        };

        auto add_full_range_if_small = [&](const ActiveMappedMemoryRecord& rec,
                                           VkDeviceSize max_size) {
            if (!rec.map_data || rec.size == 0) return;
            if (rec.size > max_size) {
                skipped_large++;
                return;
            }
            add_range(rec, 0, rec.size);
        };

        if (!g_invalidate_hint_ranges.empty()) {
            g_invalidate_hint_rounds++;
            const bool periodic_full =
                kPeriodicFullSyncEvery > 0 &&
                ((g_invalidate_hint_rounds % (kPeriodicFullSyncEvery ? kPeriodicFullSyncEvery : 1)) == 0);

            if (periodic_full) {
                for (const auto& pair : g_active_mapped_memories) {
                    add_full_range_if_small(pair.second, kImplicitInvalidateSmallFallbackBytes);
                }
            } else {
                used_hint = true;
                hinted_memories = g_invalidate_hint_ranges.size();

                for (const TrackedMemoryRange& tracked : g_invalidate_hint_ranges) {
                    if (tracked.memory == VK_NULL_HANDLE || tracked.size == 0) continue;
                    auto it = g_active_mapped_memories.find(tracked.memory);
                    if (it == g_active_mapped_memories.end()) continue;
                    add_range(it->second, tracked.offset, tracked.size);
                }
            }
            g_invalidate_hint_ranges.clear();
        } else if (!g_invalidate_hint_memories.empty()) {
            g_invalidate_hint_rounds++;
            const bool periodic_full =
                kPeriodicFullSyncEvery > 0 &&
                ((g_invalidate_hint_rounds % (kPeriodicFullSyncEvery ? kPeriodicFullSyncEvery : 1)) == 0);

            if (periodic_full) {
                for (const auto& pair : g_active_mapped_memories) {
                    add_full_range_if_small(pair.second, kImplicitInvalidateSmallFallbackBytes);
                }
            } else {
                used_hint = true;
                hinted_memories = g_invalidate_hint_memories.size();
                std::unordered_set<VkDeviceMemory> added;

                for (VkDeviceMemory mem : g_invalidate_hint_memories) {
                    auto it = g_active_mapped_memories.find(mem);
                    if (it == g_active_mapped_memories.end()) continue;
                    add_full_range_if_small(it->second, kImplicitInvalidateLargeFallbackLimitBytes);
                    added.insert(mem);
                }

                // Keep tiny control buffers safe in hint mode.
                for (const auto& pair : g_active_mapped_memories) {
                    const ActiveMappedMemoryRecord& rec = pair.second;
                    if (!rec.map_data || rec.size == 0 || rec.size > kImplicitTinyControlBytes) continue;
                    if (added.find(rec.memory) != added.end()) continue;
                    add_range(rec, 0, rec.size);
                }
            }
            g_invalidate_hint_memories.clear();
        } else if (!g_recently_flushed_memories.empty()) {
            // Fallback optimization for compute paths without copy command hints:
            // keep tiny control/result buffers fresh, but avoid pulling large
            // input buffers back immediately after we just pushed them.
            used_flush_filter = true;
            for (const auto& pair : g_active_mapped_memories) {
                const ActiveMappedMemoryRecord& rec = pair.second;
                if (!rec.map_data || rec.size == 0) continue;

                const bool is_tiny_ctrl = rec.size <= kImplicitTinyControlBytes;
                const bool small_result_candidate = rec.size <= kImplicitInvalidateSmallFallbackBytes;
                const bool was_flushed =
                    g_recently_flushed_memories.find(rec.memory) != g_recently_flushed_memories.end();
                if (was_flushed && !is_tiny_ctrl) continue;

                if (is_tiny_ctrl || small_result_candidate) {
                    add_range(rec, 0, rec.size);
                } else {
                    skipped_large++;
                }
            }
        } else {
            for (const auto& pair : g_active_mapped_memories) {
                add_full_range_if_small(pair.second, kImplicitInvalidateSmallFallbackBytes);
            }
        }

        g_recently_flushed_memories.clear();
    }

    const char* mode = used_hint ? "hint" : (used_flush_filter ? "flushed_filter" : "full");
    if (kEnableImplicitSyncDiagLog && (total_bytes != 0 || skipped_large != 0)) {
        ALOGD("[SYNC_GUEST] implicit_invalidate seq=%llu mapped=%zu hinted=%zu mode=%s total_bytes=%llu skipped_large=%zu skipped_synced=%zu",
              (unsigned long long)(++g_sync_trace_seq),
              mapped_count,
              hinted_memories,
              mode,
              (unsigned long long)total_bytes,
              skipped_large,
              skipped_synced);
    }

    for (const auto& pair : ranges_by_dev) {
        InvalidateMappedMemoryRanges(pair.first, pair.second.size(), pair.second.data());
    }
}

static uint32_t SubmitSyncHintsOutputWireCount(const SubmitSyncHints& hints) {
    size_t output_count = 0;
    for (const OutputMemoryRangeHint& hint : hints.output_hints) {
        if (ShouldAutoInvalidateOutputHint(hint)) ++output_count;
    }
    return (uint32_t)output_count;
}

static size_t SubmitSyncHintsWireSize(const SubmitSyncHints& hints) {
    if (!hints.present) return 0;
    const uint32_t output_count = SubmitSyncHintsOutputWireCount(hints);
    const size_t range_count = hints.wait_flush_ranges.size() + output_count;
    return sizeof(uint32_t) * 4 + range_count * sizeof(ExpressVkSubmitRangeHintWire);
}

static void NoteSubmitHintStats(const char* api_name,
                                const SubmitSyncHints& hints,
                                size_t wire_bytes,
                                bool deferred,
                                uint32_t submit_count) {
    std::lock_guard<std::mutex> lock(g_submit_hint_stats_mutex);
    SubmitHintStats& s = g_submit_hint_stats;
    if (api_name && strcmp(api_name, "vkQueueSubmit2") == 0) {
        s.submit2_calls++;
    } else {
        s.submit_calls++;
    }
    s.submit_batches += submit_count;
    if (deferred) {
        s.deferred_submits++;
    } else {
        s.non_deferred_submits++;
    }
    if (hints.present) {
        s.hint_present_submits++;
    }
    if (wire_bytes != 0) {
        s.wire_nonzero_submits++;
    }
    s.semantic_flush_ranges += hints.flush_ranges.size();
    s.wait_flush_ranges += hints.wait_flush_ranges.size();
    s.output_hints += hints.output_hints.size();
    s.output_wire_ranges += SubmitSyncHintsOutputWireCount(hints);
    s.wire_bytes += wire_bytes;
    s.max_wire_bytes = std::max<uint64_t>(s.max_wire_bytes, wire_bytes);

    const uint64_t total_calls = s.submit_calls + s.submit2_calls;
    if (total_calls != 0 && (total_calls % kSubmitHintStatsLogEvery) == 0) {
        ALOGI("[EKVH_GUEST_SUMMARY] calls=%llu submit=%llu submit2=%llu batches=%llu "
              "deferred=%llu non_deferred=%llu hint_present=%llu wire_nonzero=%llu "
              "semantic_flush_ranges=%llu wait_flush_ranges=%llu output_hints=%llu "
              "output_wire_ranges=%llu wire_bytes=%llu max_wire_bytes=%llu",
              (unsigned long long)total_calls,
              (unsigned long long)s.submit_calls,
              (unsigned long long)s.submit2_calls,
              (unsigned long long)s.submit_batches,
              (unsigned long long)s.deferred_submits,
              (unsigned long long)s.non_deferred_submits,
              (unsigned long long)s.hint_present_submits,
              (unsigned long long)s.wire_nonzero_submits,
              (unsigned long long)s.semantic_flush_ranges,
              (unsigned long long)s.wait_flush_ranges,
              (unsigned long long)s.output_hints,
              (unsigned long long)s.output_wire_ranges,
              (unsigned long long)s.wire_bytes,
              (unsigned long long)s.max_wire_bytes);
    }
}

static void DumpExpressVkGuestPerfStats(const char* reason) {
    if (!kEnableLocalPerfLog) {
        (void)reason;
        return;
    }
    std::lock_guard<std::mutex> lock(g_submit_hint_stats_mutex);
    const uint64_t submit_calls =
        g_submit_rpc_stats.submit_calls + g_submit_rpc_stats.submit2_calls;
    const uint64_t logical_submits = g_submit_rpc_stats.submit_batches;
    const uint64_t saved_host_rpcs =
        logical_submits > submit_calls ? logical_submits - submit_calls : 0;
    const uint64_t reduction_x1000 =
        logical_submits ? (saved_host_rpcs * 1000ull) / logical_submits : 0;
    const uint64_t avg_hint_per_logical =
        logical_submits ? g_submit_rpc_stats.hint_us / logical_submits : 0;
    const uint64_t avg_total_per_logical =
        logical_submits ? g_submit_rpc_stats.total_us / logical_submits : 0;
    const uint64_t submit_window_ms =
        (g_submit_rpc_stats.first_call_us != 0 &&
         g_submit_rpc_stats.last_call_us >= g_submit_rpc_stats.first_call_us) ?
            (g_submit_rpc_stats.last_call_us -
             g_submit_rpc_stats.first_call_us) / 1000ull : 0;
    const uint64_t hint_calls = g_implicit_flush_stats.calls;
    const uint64_t mapped_flush_calls = g_mapped_flush_stats.calls;
    ALOGI("[GUEST_PERF_SNAPSHOT] reason=%s host_submit_rpcs=%llu logical_submits=%llu "
          "saved_host_rpcs=%llu reduction_x1000=%llu avg_hint_us_per_rpc=%llu "
          "avg_write_us_per_rpc=%llu avg_total_us_per_rpc=%llu "
          "avg_hint_us_per_logical=%llu avg_total_us_per_logical=%llu "
          "submit_accounted_ms=%llu submit_window_ms=%llu deferred=%llu "
          "non_deferred=%llu",
          reason ? reason : "unknown",
          (unsigned long long)submit_calls,
          (unsigned long long)logical_submits,
          (unsigned long long)saved_host_rpcs,
          (unsigned long long)reduction_x1000,
          (unsigned long long)(submit_calls ? g_submit_rpc_stats.hint_us / submit_calls : 0),
          (unsigned long long)(submit_calls ? g_submit_rpc_stats.write_us / submit_calls : 0),
          (unsigned long long)(submit_calls ? g_submit_rpc_stats.total_us / submit_calls : 0),
          (unsigned long long)avg_hint_per_logical,
          (unsigned long long)avg_total_per_logical,
          (unsigned long long)(g_submit_rpc_stats.total_us / 1000ull),
          (unsigned long long)submit_window_ms,
          (unsigned long long)g_submit_rpc_stats.deferred_fence,
          (unsigned long long)g_submit_rpc_stats.non_deferred_fence);
    ALOGI("[GUEST_PERF_SNAPSHOT] reason=%s submit_cohort_calls=%llu "
          "same_queue=%llu mergeable=%llu empty_sync=%llu single_cmd=%llu "
          "blocked_wait=%llu blocked_signal=%llu blocked_sync_fence=%llu "
          "blocked_multi_submit=%llu blocked_queue_change=%llu avg_gap_us=%llu "
          "p95_gap_bucket_us=%llu max_gap_us=%llu",
          reason ? reason : "unknown",
          (unsigned long long)g_submit_cohort_stats.calls,
          (unsigned long long)g_submit_cohort_stats.same_queue_as_prev,
          (unsigned long long)g_submit_cohort_stats.mergeable,
          (unsigned long long)g_submit_cohort_stats.empty_sync,
          (unsigned long long)g_submit_cohort_stats.single_cmd,
          (unsigned long long)g_submit_cohort_stats.blocked_by_wait_semaphore,
          (unsigned long long)g_submit_cohort_stats.blocked_by_signal_semaphore,
          (unsigned long long)g_submit_cohort_stats.blocked_by_sync_fence,
          (unsigned long long)g_submit_cohort_stats.blocked_by_multi_submit,
          (unsigned long long)g_submit_cohort_stats.blocked_by_queue_change,
          (unsigned long long)(g_submit_cohort_stats.same_queue_as_prev ?
                               g_submit_cohort_stats.total_gap_us /
                                   g_submit_cohort_stats.same_queue_as_prev : 0),
          (unsigned long long)SubmitRpcStatsP95BucketUs(
              g_submit_cohort_stats.gap_buckets,
              g_submit_cohort_stats.same_queue_as_prev),
          (unsigned long long)g_submit_cohort_stats.max_gap_us);
    ALOGI("[GUEST_PERF_SNAPSHOT] reason=%s submit_coalesce_enqueued=%llu "
          "groups=%llu flushed_submits=%llu avg_group=%llu max_group=%llu "
          "immediate=%llu deferred_fences=%llu threshold=%llu wait=%llu "
          "queue_change=%llu non_coalescible=%llu submit2=%llu present=%llu "
          "bind_sparse=%llu saved_host_rpcs=%llu reduction_x1000=%llu",
          reason ? reason : "unknown",
          (unsigned long long)g_submit_coalesce_stats.enqueued_submits,
          (unsigned long long)g_submit_coalesce_stats.flushed_groups,
          (unsigned long long)g_submit_coalesce_stats.flushed_submits,
          (unsigned long long)(g_submit_coalesce_stats.flushed_groups ?
                               g_submit_coalesce_stats.flushed_submits /
                                   g_submit_coalesce_stats.flushed_groups : 0),
          (unsigned long long)g_submit_coalesce_stats.max_group_size,
          (unsigned long long)g_submit_coalesce_stats.immediate_submits,
          (unsigned long long)g_submit_coalesce_stats.deferred_fences,
          (unsigned long long)g_submit_coalesce_stats.threshold_flushes,
          (unsigned long long)g_submit_coalesce_stats.wait_flushes,
          (unsigned long long)g_submit_coalesce_stats.queue_change_flushes,
          (unsigned long long)g_submit_coalesce_stats.non_coalescible_flushes,
          (unsigned long long)g_submit_coalesce_stats.submit2_flushes,
          (unsigned long long)g_submit_coalesce_stats.present_flushes,
          (unsigned long long)g_submit_coalesce_stats.bind_sparse_flushes,
          (unsigned long long)(g_submit_coalesce_stats.flushed_submits >
                               g_submit_coalesce_stats.flushed_groups ?
                               g_submit_coalesce_stats.flushed_submits -
                                   g_submit_coalesce_stats.flushed_groups : 0),
          (unsigned long long)(g_submit_coalesce_stats.flushed_submits ?
                               ((g_submit_coalesce_stats.flushed_submits >
                                 g_submit_coalesce_stats.flushed_groups ?
                                 g_submit_coalesce_stats.flushed_submits -
                                     g_submit_coalesce_stats.flushed_groups : 0) *
                                1000ull) /
                                   g_submit_coalesce_stats.flushed_submits : 0));
    {
        std::lock_guard<std::mutex> deferred_lock(g_deferred_wait_stats_mutex);
        ALOGI("[GUEST_PERF_SNAPSHOT] reason=%s deferred_wait_virtual=%llu "
              "virtual_readback=%llu real=%llu real_us=%llu drains=%llu "
              "empty_drains=%llu drained_queues=%llu drain_us=%llu "
              "deferred_readback_calls=%llu queues=%llu ranges=%llu bytes_mb=%llu "
              "wait_us=%llu invalidate_us=%llu total_us=%llu fallback=%llu",
              reason ? reason : "unknown",
              (unsigned long long)g_deferred_wait_stats.virtual_waits,
              (unsigned long long)g_deferred_wait_stats.virtual_readback_waits,
              (unsigned long long)g_deferred_wait_stats.real_waits,
              (unsigned long long)g_deferred_wait_stats.real_wait_us,
              (unsigned long long)g_deferred_wait_stats.drain_calls,
              (unsigned long long)g_deferred_wait_stats.drain_empty_calls,
              (unsigned long long)g_deferred_wait_stats.drained_queues,
              (unsigned long long)g_deferred_wait_stats.drain_us,
              (unsigned long long)g_deferred_wait_stats.deferred_readback_drains,
              (unsigned long long)g_deferred_wait_stats.deferred_readback_queues,
              (unsigned long long)g_deferred_wait_stats.deferred_readback_ranges,
              (unsigned long long)(g_deferred_wait_stats.deferred_readback_bytes /
                                   (1024ull * 1024ull)),
              (unsigned long long)g_deferred_wait_stats.deferred_readback_wait_us,
              (unsigned long long)g_deferred_wait_stats.deferred_readback_invalidate_us,
              (unsigned long long)g_deferred_wait_stats.deferred_readback_total_us,
              (unsigned long long)g_deferred_wait_stats.deferred_readback_fallbacks);
    }
    ALOGI("[GUEST_PERF_SNAPSHOT] reason=%s targeted_readback_calls=%llu "
          "ranges=%llu bytes_mb=%llu wait_us=%llu invalidate_us=%llu "
          "total_us=%llu avg_wait_us=%llu avg_inv_us=%llu avg_total_us=%llu "
          "skipped_synced=%llu prefer_host_fence=%d",
          reason ? reason : "unknown",
          (unsigned long long)g_targeted_readback_wait_stats.calls,
          (unsigned long long)g_targeted_readback_wait_stats.ranges,
          (unsigned long long)(g_targeted_readback_wait_stats.bytes / (1024ull * 1024ull)),
          (unsigned long long)g_targeted_readback_wait_stats.wait_us,
          (unsigned long long)g_targeted_readback_wait_stats.invalidate_us,
          (unsigned long long)g_targeted_readback_wait_stats.total_us,
          (unsigned long long)(g_targeted_readback_wait_stats.calls ?
                               g_targeted_readback_wait_stats.wait_us /
                                   g_targeted_readback_wait_stats.calls : 0),
          (unsigned long long)(g_targeted_readback_wait_stats.calls ?
                               g_targeted_readback_wait_stats.invalidate_us /
                                   g_targeted_readback_wait_stats.calls : 0),
          (unsigned long long)(g_targeted_readback_wait_stats.calls ?
                               g_targeted_readback_wait_stats.total_us /
                                   g_targeted_readback_wait_stats.calls : 0),
          (unsigned long long)g_targeted_readback_wait_stats.skipped_synced,
          (int)kPreferHostFenceForWeakDescriptorReadback);
    ALOGI("[GUEST_PERF_SNAPSHOT] reason=%s weak_suppress_candidates=%llu "
          "submit_hits=%llu strong_hints=%llu strong_mb=%llu "
          "weak_dropped=%llu weak_dropped_mb=%llu weak_kept=%llu "
          "weak_kept_mb=%llu tiny_kept=%llu multi_submit_bypass=%llu enabled=%d",
          reason ? reason : "unknown",
          (unsigned long long)g_weak_readback_suppression_stats.candidates,
          (unsigned long long)g_weak_readback_suppression_stats.submit_hits,
          (unsigned long long)g_weak_readback_suppression_stats.strong_hints,
          (unsigned long long)(g_weak_readback_suppression_stats.strong_bytes /
                               (1024ull * 1024ull)),
          (unsigned long long)g_weak_readback_suppression_stats.weak_dropped,
          (unsigned long long)(g_weak_readback_suppression_stats.weak_dropped_bytes /
                               (1024ull * 1024ull)),
          (unsigned long long)g_weak_readback_suppression_stats.weak_kept,
          (unsigned long long)(g_weak_readback_suppression_stats.weak_kept_bytes /
                               (1024ull * 1024ull)),
          (unsigned long long)g_weak_readback_suppression_stats.tiny_weak_kept,
          (unsigned long long)g_weak_readback_suppression_stats.multi_submit_bypass,
          (int)kSuppressWeakReadbackWhenStrongCopyPresent);
    for (size_t i = 0; i < (size_t)InvalidateChangeClass::kCount; ++i) {
        const InvalidateChangeClassStats& cls = g_invalidate_change_stats.classes[i];
        if (cls.calls == 0) continue;
        const uint64_t compared = cls.changed_samples + cls.clean_samples;
        const uint64_t changed_rate_x1000 =
            compared ? (cls.changed_samples * 1000ull) / compared : 0;
        ALOGI("[GUEST_PERF_SNAPSHOT] reason=%s invalidate_change class=%s "
              "calls=%llu ranges=%llu bytes_mb=%llu sample_kb=%llu "
              "changed=%llu clean=%llu changed_rate_x1000=%llu "
              "no_shadow=%llu untracked=%llu sample_bytes=%llu",
              reason ? reason : "unknown",
              InvalidateChangeClassName((InvalidateChangeClass)i),
              (unsigned long long)cls.calls,
              (unsigned long long)cls.ranges,
              (unsigned long long)(cls.bytes / (1024ull * 1024ull)),
              (unsigned long long)(cls.sample_bytes / 1024ull),
              (unsigned long long)cls.changed_samples,
              (unsigned long long)cls.clean_samples,
              (unsigned long long)changed_rate_x1000,
              (unsigned long long)cls.no_shadow,
              (unsigned long long)cls.untracked,
              (unsigned long long)kReadbackChangeSampleBytes);
    }
    ALOGI("[GUEST_PERF_SNAPSHOT] reason=%s implicit_calls=%llu hint=%llu scan=%llu "
          "skip_metadata=%llu skip_clean_scan=%llu clean_scan=%llu dirty_scan=%llu "
          "hint_cache_hits=%llu hint_memcmp_checks=%llu hint_memcmp_mb=%llu "
          "scanned_mb=%llu dirty_mb=%llu avg_implicit_us=%llu",
          reason ? reason : "unknown",
          (unsigned long long)hint_calls,
          (unsigned long long)g_implicit_flush_stats.hint_calls,
          (unsigned long long)g_implicit_flush_stats.scan_calls,
          (unsigned long long)g_implicit_flush_stats.skip_metadata_calls,
          (unsigned long long)g_implicit_flush_stats.clean_scan_skip_calls,
          (unsigned long long)g_implicit_flush_stats.clean_scan_calls,
          (unsigned long long)g_implicit_flush_stats.dirty_scan_calls,
          (unsigned long long)g_implicit_flush_stats.hint_clean_cache_hits,
          (unsigned long long)g_implicit_flush_stats.hint_memcmp_checks,
          (unsigned long long)(g_implicit_flush_stats.hint_memcmp_bytes / (1024ull * 1024ull)),
          (unsigned long long)(g_implicit_flush_stats.scanned_bytes / (1024ull * 1024ull)),
          (unsigned long long)(g_implicit_flush_stats.dirty_bytes / (1024ull * 1024ull)),
          (unsigned long long)(hint_calls ? g_implicit_flush_stats.total_us / hint_calls : 0));
    ALOGI("[GUEST_PERF_SNAPSHOT] reason=%s mapped_flush_calls=%llu ranges=%llu "
          "bytes_mb=%llu avg_flush_us=%llu avg_flush_rpc_us=%llu "
          "ekvh_semantic_ranges=%llu ekvh_wait_ranges=%llu ekvh_output_hints=%llu "
          "ekvh_wire_bytes=%llu",
          reason ? reason : "unknown",
          (unsigned long long)mapped_flush_calls,
          (unsigned long long)g_mapped_flush_stats.ranges,
          (unsigned long long)(g_mapped_flush_stats.bytes / (1024ull * 1024ull)),
          (unsigned long long)(mapped_flush_calls ? g_mapped_flush_stats.total_us / mapped_flush_calls : 0),
          (unsigned long long)(mapped_flush_calls ? g_mapped_flush_stats.rpc_us / mapped_flush_calls : 0),
          (unsigned long long)g_submit_hint_stats.semantic_flush_ranges,
          (unsigned long long)g_submit_hint_stats.wait_flush_ranges,
          (unsigned long long)g_submit_hint_stats.output_hints,
          (unsigned long long)g_submit_hint_stats.wire_bytes);
    ALOGI("[GUEST_PERF_SNAPSHOT] reason=%s policy_batches=%llu samples=%llu "
          "classes=%zu clean=%llu dirty=%llu unknown=%llu cache_skip=%llu "
          "flush=%llu skip=%llu aggressive_clean_skip=%llu "
          "direct_dirty_flush=%llu sample_verify=%llu bytes_mb=%llu "
          "dirty_mb=%llu avg_policy_cost_us=%llu",
          reason ? reason : "unknown",
          (unsigned long long)g_sync_policy_stats.batches,
          (unsigned long long)g_sync_policy_stats.samples,
          g_sync_policy_stats.classes.size(),
          (unsigned long long)g_sync_policy_stats.clean,
          (unsigned long long)g_sync_policy_stats.dirty,
          (unsigned long long)g_sync_policy_stats.unknown,
          (unsigned long long)g_sync_policy_stats.cache_skip,
          (unsigned long long)g_sync_policy_stats.action_flush,
          (unsigned long long)g_sync_policy_stats.action_skip,
          (unsigned long long)g_sync_policy_stats.aggressive_clean_skip,
          (unsigned long long)g_sync_policy_stats.direct_dirty_flush,
          (unsigned long long)g_sync_policy_stats.sample_verify,
          (unsigned long long)(g_sync_policy_stats.bytes / (1024ull * 1024ull)),
          (unsigned long long)(g_sync_policy_stats.dirty_bytes / (1024ull * 1024ull)),
          (unsigned long long)(g_sync_policy_stats.samples ?
                               g_sync_policy_stats.cost_us / g_sync_policy_stats.samples : 0));
}

static void WriteSubmitSyncHintsWire(uint8_t** dst, const SubmitSyncHints& hints) {
    if (!dst || !*dst) return;
    if (!hints.present) return;

    const uint32_t magic = kExpressVkSubmitHintsMagic;
    const uint32_t version = kExpressVkSubmitHintsVersion;
    const uint32_t flush_count = (uint32_t)hints.wait_flush_ranges.size();
    const uint32_t output_count = SubmitSyncHintsOutputWireCount(hints);

    memcpy(*dst, &magic, sizeof(magic));
    *dst += sizeof(magic);
    memcpy(*dst, &version, sizeof(version));
    *dst += sizeof(version);
    memcpy(*dst, &flush_count, sizeof(flush_count));
    *dst += sizeof(flush_count);
    memcpy(*dst, &output_count, sizeof(output_count));
    *dst += sizeof(output_count);

    auto write_range = [&](const TrackedMemoryRange& range) {
        ExpressVkSubmitRangeHintWire wire = {};
        wire.device = (uint64_t)(uintptr_t)range.device;
        wire.memory = (uint64_t)(uintptr_t)range.memory;
        wire.offset = (uint64_t)range.offset;
        wire.size = (uint64_t)range.size;
        memcpy(*dst, &wire, sizeof(wire));
        *dst += sizeof(wire);
    };

    for (const TrackedMemoryRange& range : hints.wait_flush_ranges) {
        write_range(range);
    }
    for (const OutputMemoryRangeHint& hint : hints.output_hints) {
        if (!ShouldAutoInvalidateOutputHint(hint)) continue;
        write_range(hint.range);
    }
}

struct QueueSubmitHostSendStats {
    uint64_t command_buffers = 0;
    uint64_t wait_semaphores = 0;
    uint64_t signal_semaphores = 0;
    size_t hint_wire_size = 0;
    uint64_t encode_us = 0;
    uint64_t write_us = 0;
    uint64_t total_us = 0;
};

static void MergeSubmitSyncHints(SubmitSyncHints* dst, const SubmitSyncHints& src) {
    if (!dst || !src.present) return;
    dst->present = true;
    for (const TrackedMemoryRange& range : src.flush_ranges) {
        AppendTrackedRange(&dst->flush_ranges, range);
    }
    for (const TrackedMemoryRange& range : src.wait_flush_ranges) {
        AppendTrackedRange(&dst->wait_flush_ranges, range);
    }
    for (const OutputMemoryRangeHint& hint : src.output_hints) {
        AppendOutputMemoryRangeHint(&dst->output_hints,
                                    hint.range,
                                    hint.source,
                                    hint.strength,
                                    hint.wait_commit_eligible);
    }
}

static VkResult SendQueueSubmitToHost(
        VkQueue queue,
        uint32_t submitCount,
        const VkSubmitInfo* pSubmits,
        VkFence host_fence,
        const SubmitSyncHints& submit_hints,
        QueueSubmitHostSendStats* out_stats) {
    QueueSubmitHostSendStats stats = {};
    stats.hint_wire_size = SubmitSyncHintsWireSize(submit_hints);

    const uint64_t start_us = ExpressVkNowUs();
    const uint64_t encode_start_us = ExpressVkNowUs();
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;

    mgr.addParam64((uint64_t)(uintptr_t)queue);
    mgr.addParam32(submitCount);
    mgr.addParam64((uint64_t)(uintptr_t)host_fence);

    if (submitCount != 0 && pSubmits == nullptr) {
        mgr.clear();
        if (out_stats) {
            *out_stats = stats;
        }
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    size_t total_size = stats.hint_wire_size;
    if (total_size > static_cast<size_t>(INT_MAX)) {
        mgr.clear();
        if (out_stats) {
            *out_stats = stats;
        }
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    for (uint32_t i = 0; i < submitCount; ++i) {
        stats.wait_semaphores += pSubmits[i].waitSemaphoreCount;
        stats.command_buffers += pSubmits[i].commandBufferCount;
        stats.signal_semaphores += pSubmits[i].signalSemaphoreCount;
        const size_t size_before = total_size;
        count_VkSubmitInfo(0,
                           VK_STRUCTURE_TYPE_MAX_ENUM,
                           &pSubmits[i],
                           &total_size);
        if (total_size < size_before ||
            total_size > static_cast<size_t>(INT_MAX)) {
            mgr.clear();
            if (out_stats) {
                *out_stats = stats;
            }
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
    }

    char* buffer = (char*)mgr.addExternalParamPtr(
        static_cast<int>(total_size));
    uint8_t* ptr = (uint8_t*)buffer;

    /*
     * Do not memcpy VkSubmitInfo here.  In particular, pNext is a guest
     * address and must never become a host Vulkan pointer.  The generated
     * codec serializes the extension chain and all referenced arrays by
     * value; its matching host decoder rebuilds host-owned structures and
     * remaps semaphore and command-buffer handles.  This also carries
     * VkTimelineSemaphoreSubmitInfo value arrays correctly.
     */
    for (uint32_t i = 0; i < submitCount; ++i) {
        encode_to_stream_VkSubmitInfo(VK_STRUCTURE_TYPE_MAX_ENUM,
                                      &pSubmits[i],
                                      &ptr);
    }

    WriteSubmitSyncHintsWire(&ptr, submit_hints);

    VkResult vk_result = VK_ERROR_DEVICE_LOST;
    mgr.addPtr(&vk_result, sizeof(vk_result));
    const uint64_t encode_done_us = ExpressVkNowUs();
    const uint64_t write_start_us = ExpressVkNowUs();
    const ssize_t transport_bytes =
        FlimeGuestWrite(&mgr,
                        express_gpu,
                        EXPRESS_GPU_DEVICE_ID,
                        FUNID_vkQueueSubmit,
                        true);
    const uint64_t write_done_us = ExpressVkNowUs();
    if (!IsCompleteParamManagerWrite(transport_bytes, 2)) {
        vk_result = VK_ERROR_DEVICE_LOST;
    }

    stats.encode_us = encode_done_us - encode_start_us;
    stats.write_us = write_done_us - write_start_us;
    stats.total_us = write_done_us - start_us;
    if (out_stats) {
        *out_stats = stats;
    }
    return vk_result;
}

static void NoteSubmitCoalesceFlush(const char* reason,
                                    uint64_t group_size,
                                    uint64_t deferred_fences) {
    std::lock_guard<std::mutex> lock(g_submit_hint_stats_mutex);
    SubmitCoalesceStats& s = g_submit_coalesce_stats;
    s.flushed_groups++;
    s.flushed_submits += group_size;
    s.deferred_fences += deferred_fences;
    s.max_group_size = std::max<uint64_t>(s.max_group_size, group_size);

    if (reason && strcmp(reason, "threshold") == 0) {
        s.threshold_flushes++;
    } else if (reason && (strcmp(reason, "queue_wait_idle") == 0 ||
                          strcmp(reason, "device_wait_idle") == 0 ||
                          strcmp(reason, "wait_fences") == 0 ||
                          strcmp(reason, "get_fence_status") == 0)) {
        s.wait_flushes++;
    } else if (reason && strcmp(reason, "queue_change") == 0) {
        s.queue_change_flushes++;
    } else if (reason && strcmp(reason, "non_coalescible") == 0) {
        s.non_coalescible_flushes++;
    } else if (reason && strcmp(reason, "queue_submit2") == 0) {
        s.submit2_flushes++;
    } else if (reason && strcmp(reason, "queue_present") == 0) {
        s.present_flushes++;
    } else if (reason && strcmp(reason, "queue_bind_sparse") == 0) {
        s.bind_sparse_flushes++;
    } else {
        s.sync_flushes++;
    }

    if (s.flushed_groups != 0 &&
        (s.flushed_groups % kSubmitCoalesceStatsLogEvery) == 0) {
        const uint64_t avg_group =
            s.flushed_groups ? s.flushed_submits / s.flushed_groups : 0;
        const uint64_t saved_host_rpcs =
            s.flushed_submits > s.flushed_groups ?
                s.flushed_submits - s.flushed_groups : 0;
        const uint64_t reduction_x1000 =
            s.flushed_submits ?
                (saved_host_rpcs * 1000ull) / s.flushed_submits : 0;
        ALOGI("[SUBMIT_COALESCE_SUMMARY] enqueued=%llu groups=%llu "
              "flushed_submits=%llu avg_group=%llu max_group=%llu "
              "immediate=%llu deferred_fences=%llu threshold=%llu wait=%llu "
              "queue_change=%llu non_coalescible=%llu submit2=%llu "
              "present=%llu bind_sparse=%llu other_sync=%llu "
              "saved_host_rpcs=%llu reduction_x1000=%llu",
              (unsigned long long)s.enqueued_submits,
              (unsigned long long)s.flushed_groups,
              (unsigned long long)s.flushed_submits,
              (unsigned long long)avg_group,
              (unsigned long long)s.max_group_size,
              (unsigned long long)s.immediate_submits,
              (unsigned long long)s.deferred_fences,
              (unsigned long long)s.threshold_flushes,
              (unsigned long long)s.wait_flushes,
              (unsigned long long)s.queue_change_flushes,
              (unsigned long long)s.non_coalescible_flushes,
              (unsigned long long)s.submit2_flushes,
              (unsigned long long)s.present_flushes,
              (unsigned long long)s.bind_sparse_flushes,
              (unsigned long long)s.sync_flushes,
              (unsigned long long)saved_host_rpcs,
              (unsigned long long)reduction_x1000);
    }
}

static void FlushPendingSubmitCohort(const char* reason) {
    std::vector<PendingSubmitCohortEntry> pending;
    VkQueue queue = VK_NULL_HANDLE;
    {
        std::lock_guard<std::mutex> lock(g_submit_coalesce_mutex);
        if (g_submit_coalesce_pending.empty()) return;
        queue = g_submit_coalesce_queue;
        pending.swap(g_submit_coalesce_pending);
        g_submit_coalesce_queue = VK_NULL_HANDLE;
    }

    SubmitSyncHints merged_hints;
    std::vector<VkSubmitInfo> submits(pending.size());
    uint64_t total_hint_us = 0;
    uint64_t deferred_fences = 0;
    for (size_t i = 0; i < pending.size(); ++i) {
        VkSubmitInfo submit = {};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.pNext = nullptr;
        submit.commandBufferCount = (uint32_t)pending[i].command_buffers.size();
        submit.pCommandBuffers = pending[i].command_buffers.data();
        submits[i] = submit;
        MergeSubmitSyncHints(&merged_hints, pending[i].hints);
        total_hint_us += pending[i].hint_us;
        if (pending[i].deferred_fence) deferred_fences++;
    }
    CanonicalizeOutputMemoryRangeHints(&merged_hints.output_hints);

    QueueSubmitHostSendStats send_stats;
    SendQueueSubmitToHost(queue,
                          (uint32_t)submits.size(),
                          submits.data(),
                          VK_NULL_HANDLE,
                          merged_hints,
                          &send_stats);
    NoteSubmitHintStats("vkQueueSubmit_coalesced",
                        merged_hints,
                        send_stats.hint_wire_size,
                        deferred_fences != 0,
                        (uint32_t)submits.size());
    NoteSubmitRpcStats("vkQueueSubmit",
                       (uint32_t)submits.size(),
                       send_stats.command_buffers,
                       send_stats.wait_semaphores,
                       send_stats.signal_semaphores,
                       false,
                       false,
                       false,
                       total_hint_us,
                       send_stats.encode_us,
                       send_stats.write_us,
                       total_hint_us + send_stats.total_us);
    NoteSubmitCoalesceFlush(reason ? reason : "unknown",
                            pending.size(),
                            deferred_fences);
}

static void FlushPendingSubmitCohortForQueue(VkQueue queue, const char* reason) {
    bool should_flush = false;
    {
        std::lock_guard<std::mutex> lock(g_submit_coalesce_mutex);
        should_flush = !g_submit_coalesce_pending.empty() &&
                       (queue == VK_NULL_HANDLE || g_submit_coalesce_queue == queue);
    }
    if (should_flush) {
        FlushPendingSubmitCohort(reason);
    }
}

static bool TryEnqueueSubmitCohort(VkQueue queue,
                                   PendingSubmitCohortEntry&& entry) {
    bool flush_queue_change = false;
    bool flush_threshold = false;
    {
        std::lock_guard<std::mutex> lock(g_submit_coalesce_mutex);
        flush_queue_change = !g_submit_coalesce_pending.empty() &&
                             g_submit_coalesce_queue != queue;
    }
    if (flush_queue_change) {
        FlushPendingSubmitCohort("queue_change");
    }

    {
        std::lock_guard<std::mutex> lock(g_submit_coalesce_mutex);
        if (g_submit_coalesce_pending.empty()) {
            g_submit_coalesce_queue = queue;
        }
        g_submit_coalesce_pending.push_back(std::move(entry));
        flush_threshold =
            g_submit_coalesce_pending.size() >= kSubmitCoalesceMaxBatch;
    }
    {
        std::lock_guard<std::mutex> lock(g_submit_hint_stats_mutex);
        g_submit_coalesce_stats.enqueued_submits++;
    }
    if (flush_threshold) {
        FlushPendingSubmitCohort("threshold");
    }
    return true;
}

struct DeviceMemory {

    typedef VkDeviceMemory HandleType;
    VkDeviceSize size;
    alignas(16) uint8_t data[0];
};
DEFINE_OBJECT_HANDLE_CONVERSION(DeviceMemory)

VKAPI_ATTR VkResult VKAPI_CALL AllocateMemory(
    VkDevice                    device,
    const VkMemoryAllocateInfo* pAllocateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDeviceMemory*             pMemory)
{
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    Allocator vkAllocator;

    VkMemoryAllocateInfo* localInfo = (VkMemoryAllocateInfo*)malloc(sizeof(VkMemoryAllocateInfo));
    deepcopy_VkMemoryAllocateInfo(&vkAllocator, VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, pAllocateInfo, localInfo);

    VkAllocationCallbacks* localAlloc = nullptr;
    if (pAllocator) {
        localAlloc = (VkAllocationCallbacks*)malloc(sizeof(VkAllocationCallbacks));
        deepcopy_VkAllocationCallbacks(&vkAllocator, VK_STRUCTURE_TYPE_MAX_ENUM, pAllocator, localAlloc);
    }

    // Keep stream field order compatible with host FUNID_vkAllocateMemory decoder.
    // Android path does not use imported native-buffer metadata, so keep them zero.
    uint64_t native_buffer_id = 0;
    uint32_t buffer_width = 0;
    uint32_t buffer_height = 0;
    uint64_t native_buffer_handle = 0;

    size_t byteCount = 0;
    count_VkMemoryAllocateInfo(0, VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, localInfo, &byteCount);
    if (pAllocator) count_VkAllocationCallbacks(0, VK_STRUCTURE_TYPE_MAX_ENUM, localAlloc, &byteCount);
    byteCount += sizeof(uint64_t) + sizeof(uint64_t) + sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint64_t);

    char* buf = (char*)mgr.addExternalParamPtr(byteCount);
    uint8_t** ptr = (uint8_t**)&buf;
    encode_to_stream_VkMemoryAllocateInfo(VK_STRUCTURE_TYPE_MAX_ENUM, localInfo, ptr);

    uint64_t allocPtr = (uint64_t)(uintptr_t)pAllocator;
    memcpy(*ptr, &allocPtr, sizeof(uint64_t));
    *ptr += sizeof(uint64_t);
    if (pAllocator) encode_to_stream_VkAllocationCallbacks(VK_STRUCTURE_TYPE_MAX_ENUM, localAlloc, ptr);

    memcpy(*ptr, &native_buffer_id, sizeof(uint64_t));
    *ptr += sizeof(uint64_t);
    memcpy(*ptr, &buffer_width, sizeof(uint32_t));
    *ptr += sizeof(uint32_t);
    memcpy(*ptr, &buffer_height, sizeof(uint32_t));
    *ptr += sizeof(uint32_t);
    memcpy(*ptr, &native_buffer_handle, sizeof(uint64_t));
    *ptr += sizeof(uint64_t);

    VkDeviceMemory_T* mem = (VkDeviceMemory_T*)malloc(sizeof(VkDeviceMemory_T));
    memset(mem, 0, sizeof(*mem));
    mem->express_vk_mem_fd = -1;
    int express_mem_fd = -1;
    uint64_t express_mem_map_size = 0;
    char* express_mem_ptr = TryMmapExpressVkMem(
        (uint64_t)localInfo->allocationSize,
        &express_mem_fd,
        &express_mem_map_size);
    if (express_mem_ptr) {
        mem->map_data = express_mem_ptr;
        mem->express_vk_mem_registered = 1;
        mem->express_vk_mem_fd = express_mem_fd;
        mem->express_vk_mem_map_size = express_mem_map_size;
    } else {
        mem->map_data = new char[localInfo->allocationSize];
        mem->express_vk_mem_registered = 0;
        mem->express_vk_mem_fd = -1;
        mem->express_vk_mem_map_size = 0;
    }
    mem->length = localInfo->allocationSize;
        ALOGI("allocate memory info size %lld native_buffer_id=%llx width=%u height=%u handle=%llx",
            (long long)localInfo->allocationSize,
            (unsigned long long)native_buffer_id,
            buffer_width,
            buffer_height,
            (unsigned long long)native_buffer_handle);

    *pMemory = (VkDeviceMemory)mem;

    {
        std::lock_guard<std::mutex> lock(g_mapped_mutex);
        MemShapeRecordAllocationLocked(device,
                                       *pMemory,
                                       localInfo->allocationSize,
                                       localInfo->memoryTypeIndex,
                                       mem->express_vk_mem_registered != 0);
    }

    if (localInfo->allocationSize >= kGuestMemTraceLargeBytes) {
        GUEST_MEM_TRACE("[GUEST_MEM_TRACE] allocate device=0x%llx memory=0x%llx size=%llu size_mb=%llu memoryTypeIndex=%u registered=%d map_ptr=%p map_size=%llu",
                        (unsigned long long)(uintptr_t)device,
                        (unsigned long long)(uintptr_t)*pMemory,
                        (unsigned long long)localInfo->allocationSize,
                        (unsigned long long)((uint64_t)localInfo->allocationSize / (1024ull * 1024ull)),
                        localInfo->memoryTypeIndex,
                        mem->express_vk_mem_registered != 0 ? 1 : 0,
                        mem->map_data,
                        (unsigned long long)mem->express_vk_mem_map_size);
    }

    mgr.addParam64((uint64_t)(uintptr_t)device);
    mgr.addParam64((uint64_t)(uintptr_t)*pMemory);

    VkResult res = VK_SUCCESS;

    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkAllocateMemory, true);

    if (mem->express_vk_mem_registered) {
        RegisterExpressVkMappedMemory(device, *pMemory, mem->map_data, mem->length);
    }

    free(localInfo);
    if (localAlloc) free(localAlloc);

    return res;
}

VKAPI_ATTR void FreeMemory(VkDevice device,
                             VkDeviceMemory memory,
                             const VkAllocationCallbacks* pAllocator) {
    if (kEnableDeferredFenceWait) {
        DrainDeferredQueues("free_memory");
    }

    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    VkDeviceMemory_T* mem = (VkDeviceMemory_T*)(memory);
    
    uint64_t guest_device = (uint64_t)(uintptr_t)device;
    uint64_t guest_memory = (uint64_t)(uintptr_t)memory;
    
    if (mem && mem->express_vk_mem_registered) {
        UnregisterExpressVkMappedMemory(device, memory);
    }

    mgr.addParam64(guest_device);
    mgr.addParam64(guest_memory);

    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkFreeMemory, false);

    {
        std::lock_guard<std::mutex> lock(g_mapped_mutex);
        auto active_it = g_active_mapped_memories.find(memory);
        bool shadow_pooled = false;
        size_t shadow_capacity = 0;
        if (active_it != g_active_mapped_memories.end()) {
            ReleaseShadowBufferLocked(
                std::move(active_it->second.shadow),
                &shadow_pooled,
                &shadow_capacity);
            g_active_mapped_memories.erase(active_it);
        }
        g_flush_hint_memories.erase(memory);
        g_invalidate_hint_memories.erase(memory);
        g_recently_flushed_memories.erase(memory);
        EraseTrackedRangesForMemoryLocked(memory);
        MemShapeForgetMemoryLocked(memory, "free_memory");
        ResetImplicitCleanScanThrottleLocked();
        if (kEnableLocalPerfLog) {
            ALOGI("[PERF_FreeMemoryLocal] memory=%lld pooled=%d shadow_capacity=%zu pool_entries=%zu pool_bytes=%zu local_bytes=%lld",
                  (long long)memory,
                  (int)shadow_pooled,
                  shadow_capacity,
                  g_shadow_buffer_pool.size(),
                  g_shadow_buffer_pool_bytes,
                  mem ? (long long)mem->length : 0LL);
        }
    }

    if (mem && mem->length >= kGuestMemTraceLargeBytes) {
        GUEST_MEM_TRACE("[GUEST_MEM_TRACE] free device=0x%llx memory=0x%llx size=%llu registered=%d map_ptr=%p",
                        (unsigned long long)(uintptr_t)device,
                        (unsigned long long)(uintptr_t)memory,
                        (unsigned long long)mem->length,
                        mem->express_vk_mem_registered != 0 ? 1 : 0,
                        mem->map_data);
    }

    if (mem) {
        if (mem->express_vk_mem_registered) {
            if (mem->map_data && mem->express_vk_mem_map_size != 0) {
                munmap(mem->map_data, (size_t)mem->express_vk_mem_map_size);
            }
            if (mem->express_vk_mem_fd >= 0) {
                close(mem->express_vk_mem_fd);
            }
        } else {
            delete[] mem->map_data;
        }
        mem->map_data = nullptr;
        free(mem);
    }
}

VKAPI_ATTR VkResult VKAPI_CALL MapMemory(
    VkDevice       device,
    VkDeviceMemory memory,
    VkDeviceSize   offset,
    VkDeviceSize   size,
    VkMemoryMapFlags flags,
    void**          ppData)
{
    struct timespec t0_start, t1_init, t2_rpc, t3_lock, t4_shadow, t5_done;
    clock_gettime(CLOCK_MONOTONIC, &t0_start);
    if (kEnableDeferredFenceWait) {
        DrainDeferredQueues("map_memory");
    }
    
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;

    VkDeviceMemory_T* mem = (VkDeviceMemory_T*)(memory);
    if (kEnableLocalPerfLog) {
        ALOGI("memory info is %lld %lld", (long long)mem->length, (long long)mem->map_data);
    }

    VkDeviceSize sync_size = size;
    if (offset > mem->length) {
        sync_size = 0;
    } else if (size == VK_WHOLE_SIZE || offset + size > mem->length) {
        sync_size = mem->length - offset;
    }

    mgr.addParam64((uint64_t)(uintptr_t)device);
    mgr.addParam64((uint64_t)(uintptr_t)memory);
    mgr.addParam64(offset);
    mgr.addParam64(size);
    mgr.addParam32(flags);
    // Removed mgr.addPtr(mem->map_data + offset, (size_t)sync_size); as it sends 128MB payload unnecessarily

    *ppData = mem->map_data + offset;

    clock_gettime(CLOCK_MONOTONIC, &t1_init);
    double init_ms = (t1_init.tv_sec - t0_start.tv_sec) * 1000.0 + (t1_init.tv_nsec - t0_start.tv_nsec) / 1000000.0;

    // ALOGI("MapMemory device=%lld memory=%lld offset=%lld size=%lld flags=%u sync_size=%lld", (long long)device, (long long)memory, (long long)offset, (long long)size, flags, (long long)sync_size);

    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkMapMemory, false);
    
    clock_gettime(CLOCK_MONOTONIC, &t2_rpc);
    double rpc_ms = (t2_rpc.tv_sec - t1_init.tv_sec) * 1000.0 + (t2_rpc.tv_nsec - t1_init.tv_nsec) / 1000000.0;

    if (kEnableMapTimeInvalidateFallback && sync_size > 0) {
        VkMappedMemoryRange range = {};
        range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.pNext = nullptr;
        range.memory = memory;
        range.offset = offset;
        range.size = sync_size;
        InvalidateMappedMemoryRanges(device, 1, &range);
    }

    clock_gettime(CLOCK_MONOTONIC, &t3_lock);
    bool shadow_reused = false;
    bool old_shadow_pooled = false;
    size_t reused_capacity = 0;
    size_t old_shadow_capacity = 0;
    size_t pool_entries_after = 0;
    size_t pool_bytes_after = 0;
    struct timespec t4_acquire_done = t3_lock;
    struct timespec t4_copy_done = t3_lock;
    {
        std::lock_guard<std::mutex> lock(g_mapped_mutex);
        ActiveMappedMemoryRecord rec;
        rec.device = device;
        rec.memory = memory;
        rec.size = mem->length;
        rec.map_data = (uint8_t*)mem->map_data;
        rec.submit_clean_streak = 0;
        rec.last_submit_generation = g_submit_generation;
        
        clock_gettime(CLOCK_MONOTONIC, &t4_shadow);
        rec.shadow = AcquireShadowBufferLocked((size_t)mem->length, &shadow_reused, &reused_capacity);
        clock_gettime(CLOCK_MONOTONIC, &t4_acquire_done);

        if (mem->length > 0 && rec.map_data) {
            if (shadow_reused) {
                if (rec.shadow.size() != (size_t)mem->length) {
                    rec.shadow.resize((size_t)mem->length);
                }
                memcpy(rec.shadow.data(), rec.map_data, (size_t)mem->length);
            } else {
                rec.shadow.assign((uint8_t*)rec.map_data, (uint8_t*)rec.map_data + mem->length);
            }
        } else {
            rec.shadow.resize(mem->length);
        }
        clock_gettime(CLOCK_MONOTONIC, &t4_copy_done);

        auto old_it = g_active_mapped_memories.find(memory);
        if (old_it != g_active_mapped_memories.end()) {
            ReleaseShadowBufferLocked(
                std::move(old_it->second.shadow),
                &old_shadow_pooled,
                &old_shadow_capacity);
        }
        g_active_mapped_memories[memory] = std::move(rec);
        MemShapeRecordMapLocked(device, memory, offset, sync_size);
        ResetImplicitCleanScanThrottleLocked();
        pool_entries_after = g_shadow_buffer_pool.size();
        pool_bytes_after = g_shadow_buffer_pool_bytes;
    }
    
    clock_gettime(CLOCK_MONOTONIC, &t5_done);
    double lock_ms = (t4_shadow.tv_sec - t3_lock.tv_sec) * 1000.0 + (t4_shadow.tv_nsec - t3_lock.tv_nsec) / 1000000.0;
    double shadow_acquire_ms = (t4_acquire_done.tv_sec - t4_shadow.tv_sec) * 1000.0 + (t4_acquire_done.tv_nsec - t4_shadow.tv_nsec) / 1000000.0;
    double shadow_copy_ms = (t4_copy_done.tv_sec - t4_acquire_done.tv_sec) * 1000.0 + (t4_copy_done.tv_nsec - t4_acquire_done.tv_nsec) / 1000000.0;
    double shadow_insert_ms = (t5_done.tv_sec - t4_copy_done.tv_sec) * 1000.0 + (t5_done.tv_nsec - t4_copy_done.tv_nsec) / 1000000.0;
    double shadow_ms = (t5_done.tv_sec - t4_shadow.tv_sec) * 1000.0 + (t5_done.tv_nsec - t4_shadow.tv_nsec) / 1000000.0;
    double total_ms = (t5_done.tv_sec - t0_start.tv_sec) * 1000.0 + (t5_done.tv_nsec - t0_start.tv_nsec) / 1000000.0;
    
    if (kEnableLocalPerfLog) {
        ALOGI("[PERF_MapMemory] size=%lld offset=%lld req_size=%lld sync_size=%lld init_ms=%.3f rpc_ms=%.3f lock_ms=%.3f shadow_ms=%.3f total_ms=%.3f",
              (long long)mem->length,
              (long long)offset,
              (long long)size,
              (long long)sync_size,
              init_ms, rpc_ms, lock_ms, shadow_ms, total_ms);
        ALOGI("[PERF_MapMemoryShadow] memory=%lld reused=%d reused_capacity=%zu acquire_ms=%.3f copy_ms=%.3f insert_ms=%.3f pool_entries=%zu pool_bytes=%zu old_pooled=%d old_capacity=%zu",
              (long long)memory,
              (int)shadow_reused,
              reused_capacity,
              shadow_acquire_ms,
              shadow_copy_ms,
              shadow_insert_ms,
              pool_entries_after,
              pool_bytes_after,
              (int)old_shadow_pooled,
              old_shadow_capacity);
    }
    if (mem->length >= kGuestMemTraceLargeBytes || sync_size >= kGuestMemTraceLargeBytes) {
        GUEST_MEM_TRACE("[GUEST_MEM_TRACE] map device=0x%llx memory=0x%llx ptr=%p offset=0x%llx request_size=%llu sync_size=%llu alloc_size=%llu registered=%d shadow_reused=%d rpc_us=%llu shadow_us=%llu total_us=%llu",
                        (unsigned long long)(uintptr_t)device,
                        (unsigned long long)(uintptr_t)memory,
                        *ppData,
                        (unsigned long long)offset,
                        (unsigned long long)size,
                        (unsigned long long)sync_size,
                        (unsigned long long)mem->length,
                        mem->express_vk_mem_registered != 0 ? 1 : 0,
                        shadow_reused ? 1 : 0,
                        (unsigned long long)ExpressVkElapsedUs(t1_init, t2_rpc),
                        (unsigned long long)ExpressVkElapsedUs(t4_shadow, t5_done),
                        (unsigned long long)ExpressVkElapsedUs(t0_start, t5_done));
    }
    LlmVkTimingNoteMap((uint64_t)sync_size,
                       ExpressVkElapsedUs(t1_init, t2_rpc),
                       ExpressVkElapsedUs(t4_shadow, t5_done),
                       ExpressVkElapsedUs(t0_start, t5_done));

    return VK_SUCCESS;
}

// -----------------------------------------------------------------------------
// Buffer

struct Buffer {
    typedef VkBuffer HandleType;
    VkDeviceSize size;
};
DEFINE_OBJECT_HANDLE_CONVERSION(Buffer)

VKAPI_ATTR VkResult VKAPI_CALL CreateBuffer(
    VkDevice device,
    const VkBufferCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkBuffer* pBuffer)
{
    int express_gpu = get_express_gpu_fd();
    ParamManager mgr;
    Allocator vkAllocator;

    VkBufferCreateInfo* localInfo = (VkBufferCreateInfo*)malloc(sizeof(VkBufferCreateInfo));
    deepcopy_VkBufferCreateInfo(&vkAllocator, VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, pCreateInfo, localInfo);

    VkAllocationCallbacks* localAlloc = nullptr;
    if (pAllocator) {
        localAlloc = (VkAllocationCallbacks*)malloc(sizeof(VkAllocationCallbacks));
        deepcopy_VkAllocationCallbacks(&vkAllocator, VK_STRUCTURE_TYPE_MAX_ENUM, pAllocator, localAlloc);
    }

    size_t byteCount = 0;
    count_VkBufferCreateInfo(0, VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, localInfo, &byteCount);
    if (pAllocator) count_VkAllocationCallbacks(0, VK_STRUCTURE_TYPE_MAX_ENUM, localAlloc, &byteCount);
    byteCount += sizeof(uint64_t);

    char* buf = (char*)mgr.addExternalParamPtr(byteCount);
    uint8_t** ptr = (uint8_t**)&buf;
    encode_to_stream_VkBufferCreateInfo(VK_STRUCTURE_TYPE_MAX_ENUM, localInfo, ptr);

    uint64_t allocPtr = (uint64_t)(uintptr_t)pAllocator;
    memcpy(*ptr, &allocPtr, sizeof(uint64_t));
    *ptr += sizeof(uint64_t);
    if (pAllocator) encode_to_stream_VkAllocationCallbacks(VK_STRUCTURE_TYPE_MAX_ENUM, localAlloc, ptr);

    VkBuffer_T* b = (VkBuffer_T*)malloc(sizeof(VkBuffer_T));
    *pBuffer = (VkBuffer)b;

    {
        std::lock_guard<std::mutex> lock(g_mapped_mutex);
        g_buffer_sizes[*pBuffer] = pCreateInfo ? pCreateInfo->size : 0;
        MemShapeRecordBufferLocked(device,
                                   *pBuffer,
                                   pCreateInfo ? pCreateInfo->size : 0,
                                   pCreateInfo ? pCreateInfo->usage : 0);
    }

    if (pCreateInfo && pCreateInfo->size >= kGuestMemTraceLargeBytes) {
        GUEST_MEM_TRACE("[GUEST_MEM_TRACE] create_buffer device=0x%llx buffer=0x%llx size=%llu size_mb=%llu usage=0x%x sharing=%u",
                        (unsigned long long)(uintptr_t)device,
                        (unsigned long long)(uintptr_t)*pBuffer,
                        (unsigned long long)pCreateInfo->size,
                        (unsigned long long)((uint64_t)pCreateInfo->size / (1024ull * 1024ull)),
                        (unsigned int)pCreateInfo->usage,
                        (unsigned int)pCreateInfo->sharingMode);
    }

    mgr.addParam64((uint64_t)(uintptr_t)device);
    mgr.addParam64((uint64_t)(uintptr_t)*pBuffer);

    VkResult res = VK_ERROR_INITIALIZATION_FAILED;
    mgr.addPtr(&res, sizeof(VkResult));
    ALOGI("CreateBuffer device=%lld buffer=%lld", (long long)device, (long long)*pBuffer);

    // Buffer requirements are commonly queried immediately after creation.
    // Wait until the host has created and mapped the buffer before returning.
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCreateBuffer, true);

    ALOGI("finish CreateBuffer %lld %lld result=%d",
          (long long)device,
          (long long)*pBuffer,
          res);

    if (res != VK_SUCCESS) {
        {
            std::lock_guard<std::mutex> lock(g_mapped_mutex);
            MemShapeForgetBufferLocked(*pBuffer);
            g_buffer_sizes.erase(*pBuffer);
        }
        free(b);
        *pBuffer = VK_NULL_HANDLE;
    }

    free(localInfo);
    if (localAlloc) free(localAlloc);

    return res;
}


// void GetBufferMemoryRequirements(VkDevice,
//                                  VkBuffer buffer_handle,
//                                  VkMemoryRequirements* requirements) {
//     Buffer* buffer = GetBufferFromHandle(buffer_handle);
//     requirements->size = buffer->size;
//     requirements->alignment = 16;  // allow fast Neon/SSE memcpy
//     requirements->memoryTypeBits = 0x1;
// }
// guest.cpp
VKAPI_ATTR void VKAPI_CALL GetBufferMemoryRequirements(
    VkDevice               device,
    VkBuffer               buffer,
    VkMemoryRequirements*  pMemoryRequirements)
{
    int express_gpu = get_express_gpu_fd();
    ParamManager mgr;

    mgr.addParam64((uint64_t)(uintptr_t)device);
    mgr.addParam64((uint64_t)(uintptr_t)buffer);
    mgr.addPtr(pMemoryRequirements, sizeof(VkMemoryRequirements));
    ALOGI("GetBufferMemoryRequirements device=%lld buffer=%lld", (long long)device, (long long)buffer);
    FlimeGuestWrite(&mgr,
        express_gpu,
        EXPRESS_GPU_DEVICE_ID,
        FUNID_vkGetBufferMemoryRequirements,
        true);
    
    ALOGI("get buffer memory requirements size %lld alignment %lld", (long long)pMemoryRequirements->size, (long long)pMemoryRequirements->alignment);
}


// -----------------------------------------------------------------------------
// Image

struct Image {
    typedef VkImage HandleType;
    VkDeviceSize size;
};
DEFINE_OBJECT_HANDLE_CONVERSION(Image)

VKAPI_ATTR VkResult VKAPI_CALL CreateImage(
    VkDevice device,
    const VkImageCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkImage* pImage) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    Allocator vkAllocator;
    VkImageCreateInfo* local_pCreateInfo = nullptr;
    if (pCreateInfo) {
        local_pCreateInfo = (VkImageCreateInfo*)malloc(sizeof(VkImageCreateInfo));
        deepcopy_VkImageCreateInfo(&vkAllocator, VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, pCreateInfo, local_pCreateInfo);
    }
    VkAllocationCallbacks* local_pAllocator = nullptr;
    if (pAllocator) {
        local_pAllocator = (VkAllocationCallbacks*)malloc(sizeof(VkAllocationCallbacks));
        deepcopy_VkAllocationCallbacks(&vkAllocator, VK_STRUCTURE_TYPE_MAX_ENUM, pAllocator, local_pAllocator);
    }
    size_t byte_count = 0;
    count_VkImageCreateInfo(0, VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, local_pCreateInfo, &byte_count);
    if (pAllocator) {
        count_VkAllocationCallbacks(0, VK_STRUCTURE_TYPE_MAX_ENUM, local_pAllocator, &byte_count);
    }
    byte_count += 8;
    char* send_buffer = (char*)mgr.addExternalParamPtr(byte_count);
    uint8_t** stream_ptr = (uint8_t**)&send_buffer;
    encode_to_stream_VkImageCreateInfo(VK_STRUCTURE_TYPE_MAX_ENUM, local_pCreateInfo, stream_ptr);
    uint64_t guest_alloc_ptr = (uint64_t)(uintptr_t)local_pAllocator;
    memcpy(*stream_ptr, &guest_alloc_ptr, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);
    if (pAllocator) {
        encode_to_stream_VkAllocationCallbacks(VK_STRUCTURE_TYPE_MAX_ENUM, local_pAllocator, stream_ptr);
    }
    const VkAllocationCallbacks* useAlloc = pAllocator ? pAllocator : &kDefaultAllocCallbacks;
    VkImage_T* image = static_cast<VkImage_T*>(useAlloc->pfnAllocation(useAlloc->pUserData, sizeof(VkImage_T), alignof(VkImage_T), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT));
    if (!image) {
        free(local_pCreateInfo);
        if (pAllocator) free(local_pAllocator);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    *pImage = (VkImage)image;
    mgr.addParam64((uint64_t)(uintptr_t)device);
    mgr.addParam64((uint64_t)(uintptr_t)*pImage);
    VkResult vkResult = VK_SUCCESS;
    mgr.addPtr(&vkResult, sizeof(VkResult));
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCreateImage, false);
    free(local_pCreateInfo);
    if (pAllocator) free(local_pAllocator);
    return vkResult;
}

VKAPI_ATTR void VKAPI_CALL GetImageMemoryRequirements(
    VkDevice device,
    VkImage image,
    VkMemoryRequirements* pMemoryRequirements) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    char* buf = (char*)mgr.addExternalParamPtr(sizeof(uint64_t) * 2);
    uint64_t guest_device = (uint64_t)(uintptr_t)device;
    uint64_t guest_image = (uint64_t)(uintptr_t)image;
    memcpy(buf, &guest_device, sizeof(uint64_t));
    memcpy(buf + sizeof(uint64_t), &guest_image, sizeof(uint64_t));
    mgr.addPtr(pMemoryRequirements, sizeof(VkMemoryRequirements));
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkGetImageMemoryRequirements, true);

    ALOGI("GetImageMemoryRequirements get result %lld %lld %lld", (long long)pMemoryRequirements->size, (long long)pMemoryRequirements->alignment, (long long)pMemoryRequirements->memoryTypeBits);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateSampler(
    VkDevice device,
    const VkSamplerCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSampler* pSampler) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    Allocator vkAllocator;
    VkSamplerCreateInfo* local_pCreateInfo = nullptr;
    if (pCreateInfo) {
        local_pCreateInfo = (VkSamplerCreateInfo*)malloc(sizeof(VkSamplerCreateInfo));
        deepcopy_VkSamplerCreateInfo(&vkAllocator, VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, pCreateInfo, local_pCreateInfo);
    }
    VkAllocationCallbacks* local_pAllocator = nullptr;
    if (pAllocator) {
        local_pAllocator = (VkAllocationCallbacks*)malloc(sizeof(VkAllocationCallbacks));
        deepcopy_VkAllocationCallbacks(&vkAllocator, VK_STRUCTURE_TYPE_MAX_ENUM, pAllocator, local_pAllocator);
    }
    size_t byte_count = 0;
    count_VkSamplerCreateInfo(0, VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, local_pCreateInfo, &byte_count);
    if (pAllocator) {
        count_VkAllocationCallbacks(0, VK_STRUCTURE_TYPE_MAX_ENUM, local_pAllocator, &byte_count);
    }
    byte_count += 8;
    char* send_buffer = (char*)mgr.addExternalParamPtr(byte_count);
    uint8_t** stream_ptr = (uint8_t**)&send_buffer;
    encode_to_stream_VkSamplerCreateInfo(VK_STRUCTURE_TYPE_MAX_ENUM, local_pCreateInfo, stream_ptr);
    uint64_t guest_alloc_ptr = (uint64_t)(uintptr_t)local_pAllocator;
    memcpy(*stream_ptr, &guest_alloc_ptr, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);
    if (pAllocator) {
        encode_to_stream_VkAllocationCallbacks(VK_STRUCTURE_TYPE_MAX_ENUM, local_pAllocator, stream_ptr);
    }
    const VkAllocationCallbacks* useAlloc = pAllocator ? pAllocator : &kDefaultAllocCallbacks;
    VkSampler_T* sampler = static_cast<VkSampler_T*>(useAlloc->pfnAllocation(useAlloc->pUserData, sizeof(VkSampler_T), alignof(VkSampler_T), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT));
    if (!sampler) {
        free(local_pCreateInfo);
        if (pAllocator) free(local_pAllocator);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    *pSampler = (VkSampler)sampler;
    mgr.addParam64((uint64_t)(uintptr_t)device);
    mgr.addParam64((uint64_t)(uintptr_t)*pSampler);
    VkResult vkResult = VK_SUCCESS;
    mgr.addPtr(&vkResult, sizeof(VkResult));
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCreateSampler, true);
    free(local_pCreateInfo);
    if (pAllocator) free(local_pAllocator);
    return vkResult;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateDescriptorSetLayout(
    VkDevice device,
    const VkDescriptorSetLayoutCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDescriptorSetLayout* pSetLayout) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    Allocator vkAllocator;
    VkDescriptorSetLayoutCreateInfo* local_pCreateInfo = nullptr;
    if (pCreateInfo) {
        local_pCreateInfo = (VkDescriptorSetLayoutCreateInfo*)malloc(sizeof(VkDescriptorSetLayoutCreateInfo));
        deepcopy_VkDescriptorSetLayoutCreateInfo(&vkAllocator, VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, pCreateInfo, local_pCreateInfo);
    }
    VkAllocationCallbacks* local_pAllocator = nullptr;
    if (pAllocator) {
        local_pAllocator = (VkAllocationCallbacks*)malloc(sizeof(VkAllocationCallbacks));
        deepcopy_VkAllocationCallbacks(&vkAllocator, VK_STRUCTURE_TYPE_MAX_ENUM, pAllocator, local_pAllocator);
    }
    size_t byte_count = 0;
    count_VkDescriptorSetLayoutCreateInfo(0, VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, local_pCreateInfo, &byte_count);
    if (pAllocator) {
        count_VkAllocationCallbacks(0, VK_STRUCTURE_TYPE_MAX_ENUM, local_pAllocator, &byte_count);
    }
    byte_count += 8;
    char* send_buffer = (char*)mgr.addExternalParamPtr(byte_count);
    uint8_t** stream_ptr = (uint8_t**)&send_buffer;
    encode_to_stream_VkDescriptorSetLayoutCreateInfo(VK_STRUCTURE_TYPE_MAX_ENUM, local_pCreateInfo, stream_ptr);
    uint64_t guest_alloc_ptr = (uint64_t)(uintptr_t)local_pAllocator;
    memcpy(*stream_ptr, &guest_alloc_ptr, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);
    if (pAllocator) {
        encode_to_stream_VkAllocationCallbacks(VK_STRUCTURE_TYPE_MAX_ENUM, local_pAllocator, stream_ptr);
    }
    const VkAllocationCallbacks* useAlloc = pAllocator ? pAllocator : &kDefaultAllocCallbacks;
    VkDescriptorSetLayout_T* setLayout = static_cast<VkDescriptorSetLayout_T*>(useAlloc->pfnAllocation(useAlloc->pUserData, sizeof(VkDescriptorSetLayout_T), alignof(VkDescriptorSetLayout_T), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT));
    if (!setLayout) {
        mgr.clear();
        free(local_pCreateInfo);
        if (pAllocator) free(local_pAllocator);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    *pSetLayout = (VkDescriptorSetLayout)setLayout;
    mgr.addParam64((uint64_t)(uintptr_t)device);
    mgr.addParam64((uint64_t)(uintptr_t)*pSetLayout);
    VkResult vkResult = VK_ERROR_DEVICE_LOST;
    mgr.addPtr(&vkResult, sizeof(vkResult));
    FlimeGuestBeforeDescriptorLifecycle(device);
    const ssize_t written =
        FlimeGuestWrite(&mgr,
                        express_gpu,
                        EXPRESS_GPU_DEVICE_ID,
                        FUNID_vkCreateDescriptorSetLayout,
                        true);
    const bool transport_ok = IsCompleteParamManagerWrite(written, 2);
    if (!transport_ok) {
        vkResult = VK_ERROR_DEVICE_LOST;
    }
    FlimeGuestCreateDescriptorSetLayout(
        device, pCreateInfo, *pSetLayout, vkResult);
    FlimeGuestAfterDescriptorLifecycle(device, transport_ok);
    if (vkResult != VK_SUCCESS) {
        useAlloc->pfnFree(useAlloc->pUserData, setLayout);
        *pSetLayout = VK_NULL_HANDLE;
    }
    free(local_pCreateInfo);
    if (pAllocator) free(local_pAllocator);
    return vkResult;
}


VkResult GetSwapchainGrallocUsageANDROID(VkDevice,
                                         VkFormat,
                                         VkImageUsageFlags,
                                         int* grallocUsage) {
    // The null driver never reads or writes the gralloc buffer
    *grallocUsage = 0;
    return VK_SUCCESS;
}

VkResult GetSwapchainGrallocUsage2ANDROID(VkDevice,
                                          VkFormat,
                                          VkImageUsageFlags,
                                          VkSwapchainImageUsageFlagsANDROID,
                                          uint64_t* grallocConsumerUsage,
                                          uint64_t* grallocProducerUsage) {
    // The null driver never reads or writes the gralloc buffer
    *grallocConsumerUsage = 0;
    *grallocProducerUsage = 0;
    return VK_SUCCESS;
}

VkResult AcquireImageANDROID(VkDevice,
                             VkImage,
                             int fence,
                             VkSemaphore,
                             VkFence) {
    ALOGI("AcquireImageANDROID fence=%d", fence);
    close(fence);
    return VK_SUCCESS;
}

VkResult QueueSignalReleaseImageANDROID(VkQueue,
                                        uint32_t,
                                        const VkSemaphore*,
                                        VkImage,
                                        int* fence) {
    *fence = -1;
    return VK_SUCCESS;
}

// -----------------------------------------------------------------------------
// No-op types

VKAPI_ATTR VkResult CreateBufferView(VkDevice device,
                                    const VkBufferViewCreateInfo* pCreateInfo,
                                    const VkAllocationCallbacks* pAllocator,
                                    VkBufferView* pView) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    Allocator vkAllocator;
    
    VkBufferViewCreateInfo* local_pCreateInfo = nullptr;
    if (pCreateInfo) {
        local_pCreateInfo = (VkBufferViewCreateInfo*)malloc(sizeof(VkBufferViewCreateInfo));
        if (!local_pCreateInfo) {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        deepcopy_VkBufferViewCreateInfo(&vkAllocator, VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, pCreateInfo, local_pCreateInfo);
    }
    
    VkAllocationCallbacks* local_pAllocator = nullptr;
    if (pAllocator) {
        local_pAllocator = (VkAllocationCallbacks*)malloc(sizeof(VkAllocationCallbacks));
        if (!local_pAllocator) {
            if (local_pCreateInfo) free(local_pCreateInfo);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        deepcopy_VkAllocationCallbacks(&vkAllocator, VK_STRUCTURE_TYPE_MAX_ENUM, pAllocator, local_pAllocator);
    }
    
    size_t count = 0;
    size_t* countPtr = &count;
    count_VkBufferViewCreateInfo(0, VK_STRUCTURE_TYPE_MAX_ENUM, local_pCreateInfo, countPtr);
    if (pAllocator) {
        count_VkAllocationCallbacks(0, VK_STRUCTURE_TYPE_MAX_ENUM, local_pAllocator, countPtr);
    }
    count += 16; // device + allocator ptr
    
    char* send_buffer = (char*)mgr.addExternalParamPtr(count);
    uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;
    
    uint64_t guest_device = (uint64_t)(uintptr_t)device;
    memcpy(*send_buffer_ptr, &guest_device, 8);
    *send_buffer_ptr += 8;
    
    encode_to_stream_VkBufferViewCreateInfo(VK_STRUCTURE_TYPE_MAX_ENUM, local_pCreateInfo, send_buffer_ptr);
    
    uint64_t cgen_var_0 = (uint64_t)(uintptr_t)local_pAllocator;
    memcpy(*send_buffer_ptr, &cgen_var_0, 8);
    *send_buffer_ptr += 8;
    
    if (pAllocator) {
        encode_to_stream_VkAllocationCallbacks(VK_STRUCTURE_TYPE_MAX_ENUM, local_pAllocator, send_buffer_ptr);
    }
    
    const VkAllocationCallbacks* useAlloc = pAllocator ? pAllocator : &kDefaultAllocCallbacks;
    VkBufferView_T* bufferView = static_cast<VkBufferView_T*>(useAlloc->pfnAllocation(
        useAlloc->pUserData, sizeof(VkBufferView_T), alignof(VkBufferView_T),
        VK_SYSTEM_ALLOCATION_SCOPE_OBJECT));
    if (!bufferView) {
        if (local_pCreateInfo) free(local_pCreateInfo);
        if (local_pAllocator) free(local_pAllocator);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    *pView = (VkBufferView)bufferView;
    
    mgr.addParam64((uint64_t)(uintptr_t)*pView);
    
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCreateBufferView, false);
    
    if (local_pCreateInfo) free(local_pCreateInfo);
    if (local_pAllocator) free(local_pAllocator);
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateDescriptorPool(
    VkDevice device,
    const VkDescriptorPoolCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDescriptorPool* pDescriptorPool) {
    const uint64_t start_us = ExpressVkNowUs();
    if (!pDescriptorPool) return VK_ERROR_INITIALIZATION_FAILED;

    const DescriptorPoolSignature signature =
        MakeDescriptorPoolSignature(device, pCreateInfo);

    if (kEnableDescriptorPoolReuse && pAllocator == nullptr) {
        VkDescriptorPool cached_pool = VK_NULL_HANDLE;
        if (TryAcquireCachedDescriptorPool(signature, &cached_pool)) {
            const VkResult reset_result =
                SendResetDescriptorPoolForReuse(device, cached_pool);
            if (reset_result != VK_SUCCESS) {
                {
                    std::lock_guard<std::mutex> lock(
                        g_descriptor_lifecycle_mutex);
                    g_descriptor_pool_signatures.erase(cached_pool);
                    g_descriptor_lifecycle_stats.create_pool_calls++;
                    g_descriptor_lifecycle_stats.create_pool_us +=
                        ExpressVkNowUs() - start_us;
                    MaybeLogDescriptorLifecycleStatsLocked("reuse_reset_failed");
                }
                DeferDescriptorPoolDestroy(device, cached_pool, nullptr);
                *pDescriptorPool = VK_NULL_HANDLE;
                return reset_result;
            }
            *pDescriptorPool = cached_pool;
            {
                std::lock_guard<std::mutex> lock(g_descriptor_lifecycle_mutex);
                g_descriptor_pool_signatures[cached_pool] = signature;
                g_descriptor_lifecycle_stats.create_pool_calls++;
                g_descriptor_lifecycle_stats.create_pool_us += ExpressVkNowUs() - start_us;
                MaybeLogDescriptorLifecycleStatsLocked("periodic");
            }
            return VK_SUCCESS;
        }
    }

    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    Allocator vkAllocator;

    VkDescriptorPoolCreateInfo* localInfo = nullptr;
    if (pCreateInfo) {
        localInfo = (VkDescriptorPoolCreateInfo*)malloc(sizeof(VkDescriptorPoolCreateInfo));
        deepcopy_VkDescriptorPoolCreateInfo(
            &vkAllocator,
            VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            pCreateInfo,
            localInfo);
    }

    VkAllocationCallbacks* localAlloc = nullptr;
    if (pAllocator) {
        localAlloc = (VkAllocationCallbacks*)malloc(sizeof(VkAllocationCallbacks));
        deepcopy_VkAllocationCallbacks(
            &vkAllocator,
            VK_STRUCTURE_TYPE_MAX_ENUM,
            pAllocator,
            localAlloc);
    }

    size_t byte_count = 0;
    count_VkDescriptorPoolCreateInfo(
        0, VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        localInfo,
        &byte_count);
    if (pAllocator) {
        count_VkAllocationCallbacks(
            0, VK_STRUCTURE_TYPE_MAX_ENUM,
            localAlloc,
            &byte_count);
    }
    byte_count += 8;

    char* send_buffer = (char*)mgr.addExternalParamPtr(byte_count);
    uint8_t** stream_ptr = (uint8_t**)&send_buffer;

    encode_to_stream_VkDescriptorPoolCreateInfo(
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        localInfo,
        stream_ptr);

    uint64_t guest_alloc_ptr = (uint64_t)(uintptr_t)localAlloc;
    memcpy(*stream_ptr, &guest_alloc_ptr, sizeof(uint64_t));
    *stream_ptr += sizeof(uint64_t);

    if (pAllocator) {
        encode_to_stream_VkAllocationCallbacks(
            VK_STRUCTURE_TYPE_MAX_ENUM,
            localAlloc,
            stream_ptr);
    }

    mgr.addParam64((uint64_t)(uintptr_t)device);

    VkDescriptorPool_T* poolObj = (VkDescriptorPool_T*)malloc(sizeof(VkDescriptorPool_T));
    if (!poolObj) {
        mgr.clear();
        free(localInfo);
        if (localAlloc) free(localAlloc);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    *pDescriptorPool = (VkDescriptorPool)poolObj;
    mgr.addParam64((uint64_t)(uintptr_t)*pDescriptorPool);

    VkResult vkResult = VK_ERROR_DEVICE_LOST;
    mgr.addPtr(&vkResult, sizeof(vkResult));
    FlimeGuestBeforeDescriptorLifecycle(device);
    const ssize_t written =
        FlimeGuestWrite(&mgr,
                        express_gpu,
                        EXPRESS_GPU_DEVICE_ID,
                        FUNID_vkCreateDescriptorPool,
                        true);
    const bool transport_ok = IsCompleteParamManagerWrite(written, 2);
    if (!transport_ok) {
        vkResult = VK_ERROR_DEVICE_LOST;
    }
    FlimeGuestCreateDescriptorPool(
        device, pCreateInfo, *pDescriptorPool, vkResult);
    FlimeGuestAfterDescriptorLifecycle(device, transport_ok);
    {
        std::lock_guard<std::mutex> lock(g_descriptor_lifecycle_mutex);
        if (vkResult == VK_SUCCESS) {
            g_descriptor_pool_signatures[*pDescriptorPool] = signature;
        }
        g_descriptor_lifecycle_stats.create_pool_calls++;
        g_descriptor_lifecycle_stats.create_pool_us += ExpressVkNowUs() - start_us;
        MaybeLogDescriptorLifecycleStatsLocked("periodic");
    }
    if (vkResult != VK_SUCCESS) {
        free(poolObj);
        *pDescriptorPool = VK_NULL_HANDLE;
    }
    free(localInfo);
    if (localAlloc) free(localAlloc);
    return vkResult;
}


VKAPI_ATTR VkResult AllocateDescriptorSets(
    VkDevice device,
    const VkDescriptorSetAllocateInfo* pAllocateInfo,
    VkDescriptorSet* pDescriptorSets) {
    const uint64_t start_us = ExpressVkNowUs();
    
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;

    VkDescriptorSetAllocateInfo* local_pAllocateInfo = nullptr;
    if (pAllocateInfo) {
        local_pAllocateInfo = (VkDescriptorSetAllocateInfo*)malloc(sizeof(VkDescriptorSetAllocateInfo));
        if (!local_pAllocateInfo) {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        
        Allocator vkAllocator;
        deepcopy_VkDescriptorSetAllocateInfo(&vkAllocator, VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, 
                                           pAllocateInfo, local_pAllocateInfo);
    }
    
    size_t count = 0;
    size_t* countPtr = &count;
    count_VkDescriptorSetAllocateInfo(0, VK_STRUCTURE_TYPE_MAX_ENUM,
                                     (VkDescriptorSetAllocateInfo*)(local_pAllocateInfo), countPtr);
    count += 8;
    
    char* send_buffer = (char*)mgr.addExternalParamPtr(count);
    uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;

    uint64_t guest_device = (uint64_t)(uintptr_t)device;
    memcpy((*send_buffer_ptr), &guest_device, 8);
    (*send_buffer_ptr) += 8;
    
    encode_to_stream_VkDescriptorSetAllocateInfo(VK_STRUCTURE_TYPE_MAX_ENUM, 
                                               (VkDescriptorSetAllocateInfo*)(local_pAllocateInfo), 
                                               send_buffer_ptr);

    const VkAllocationCallbacks* alloc = &kDefaultAllocCallbacks;
    for (uint32_t i = 0; i < pAllocateInfo->descriptorSetCount; ++i) {
        VkDescriptorSet_T* desc_set = static_cast<VkDescriptorSet_T*>(alloc->pfnAllocation(alloc->pUserData, sizeof(VkDescriptorSet_T), alignof(VkDescriptorSet_T), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT));
        if (!desc_set) {
            for (uint32_t j = 0; j < i; ++j) {
                alloc->pfnFree(alloc->pUserData,
                               (void*)pDescriptorSets[j]);
                pDescriptorSets[j] = VK_NULL_HANDLE;
            }
            mgr.clear();
            if (local_pAllocateInfo) free(local_pAllocateInfo);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        pDescriptorSets[i] = (VkDescriptorSet)desc_set;
    }

    mgr.addPtr(pDescriptorSets, sizeof(VkDescriptorSet) * pAllocateInfo->descriptorSetCount);
    VkResult vkResult = VK_ERROR_DEVICE_LOST;
    mgr.addPtr(&vkResult, sizeof(vkResult));
    FlimeGuestBeforeDescriptorLifecycle(device);

    const ssize_t written =
        FlimeGuestWrite(&mgr,
                        express_gpu,
                        EXPRESS_GPU_DEVICE_ID,
                        FUNID_vkAllocateDescriptorSets,
                        true);
    const bool transport_ok = IsCompleteParamManagerWrite(written, 3);
    if (!transport_ok) {
        vkResult = VK_ERROR_DEVICE_LOST;
    }
    FlimeGuestAllocateDescriptorSets(
        device, pAllocateInfo, pDescriptorSets, vkResult);
    {
        std::lock_guard<std::mutex> lock(g_descriptor_lifecycle_mutex);
        g_descriptor_lifecycle_stats.allocate_set_calls++;
        if (vkResult == VK_SUCCESS) {
            g_descriptor_lifecycle_stats.allocated_sets +=
                pAllocateInfo->descriptorSetCount;
        }
        g_descriptor_lifecycle_stats.allocate_set_us += ExpressVkNowUs() - start_us;
        MaybeLogDescriptorLifecycleStatsLocked("periodic");
    }
    if (vkResult == VK_SUCCESS) {
        RememberDescriptorSetsForPool(pAllocateInfo->descriptorPool,
                                      pAllocateInfo->descriptorSetCount,
                                      pDescriptorSets);
    } else {
        for (uint32_t i = 0; i < pAllocateInfo->descriptorSetCount; ++i) {
            alloc->pfnFree(alloc->pUserData, (void*)pDescriptorSets[i]);
            pDescriptorSets[i] = VK_NULL_HANDLE;
        }
    }
    FlimeGuestAfterDescriptorLifecycle(device, transport_ok);
    
    if (local_pAllocateInfo) free(local_pAllocateInfo);
    
    return vkResult;
}

VKAPI_ATTR void UpdateDescriptorSets(
    VkDevice device,
    uint32_t descriptorWriteCount,
    const VkWriteDescriptorSet* pDescriptorWrites,
    uint32_t descriptorCopyCount,
    const VkCopyDescriptorSet* pDescriptorCopies) {
    const uint64_t start_us = ExpressVkNowUs();
    
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;

    VkWriteDescriptorSet* local_pDescriptorWrites = nullptr;
    VkCopyDescriptorSet* local_pDescriptorCopies = nullptr;
    
    if (pDescriptorWrites && descriptorWriteCount > 0) {
        local_pDescriptorWrites = (VkWriteDescriptorSet*)malloc(sizeof(VkWriteDescriptorSet) * descriptorWriteCount);
        if (!local_pDescriptorWrites) {
            return;
        }
        
        Allocator vkAllocator;
        for (uint32_t i = 0; i < descriptorWriteCount; ++i) {
            deepcopy_VkWriteDescriptorSet(&vkAllocator, VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                        &pDescriptorWrites[i], &local_pDescriptorWrites[i]);
        }
    }
    
    if (pDescriptorCopies && descriptorCopyCount > 0) {
        local_pDescriptorCopies = (VkCopyDescriptorSet*)malloc(sizeof(VkCopyDescriptorSet) * descriptorCopyCount);
        if (!local_pDescriptorCopies) {
            if (local_pDescriptorWrites) free(local_pDescriptorWrites);
            return;
        }
        
        Allocator vkAllocator;
        for (uint32_t i = 0; i < descriptorCopyCount; ++i) {
            deepcopy_VkCopyDescriptorSet(&vkAllocator, VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET,
                                       &pDescriptorCopies[i], &local_pDescriptorCopies[i]);
        }
    }

    size_t count = 0;
    size_t* countPtr = &count;
    count += 8 + 4 + 4; // device + descriptorWriteCount + descriptorCopyCount
    
    for (uint32_t i = 0; i < descriptorWriteCount; ++i) {
        count_VkWriteDescriptorSet(0, VK_STRUCTURE_TYPE_MAX_ENUM,
                                 (VkWriteDescriptorSet*)&local_pDescriptorWrites[i], countPtr);
    }
    
    for (uint32_t i = 0; i < descriptorCopyCount; ++i) {
        count_VkCopyDescriptorSet(0, VK_STRUCTURE_TYPE_MAX_ENUM,
                                (VkCopyDescriptorSet*)&local_pDescriptorCopies[i], countPtr);
    }
    
    char* send_buffer = (char*)mgr.addExternalParamPtr(count);
    uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;

    uint64_t guest_device = (uint64_t)(uintptr_t)device;
    memcpy((*send_buffer_ptr), &guest_device, 8);
    (*send_buffer_ptr) += 8;

    memcpy((*send_buffer_ptr), &descriptorWriteCount, 4);
    (*send_buffer_ptr) += 4;
    memcpy((*send_buffer_ptr), &descriptorCopyCount, 4);
    (*send_buffer_ptr) += 4;

    for (uint32_t i = 0; i < descriptorWriteCount; ++i) {
        encode_to_stream_VkWriteDescriptorSet(VK_STRUCTURE_TYPE_MAX_ENUM,
                                            (VkWriteDescriptorSet*)&local_pDescriptorWrites[i],
                                            send_buffer_ptr);
    }

    for (uint32_t i = 0; i < descriptorCopyCount; ++i) {
        encode_to_stream_VkCopyDescriptorSet(VK_STRUCTURE_TYPE_MAX_ENUM,
                                           (VkCopyDescriptorSet*)&local_pDescriptorCopies[i],
                                           send_buffer_ptr);
    }

    FlimeGuestBeforeDescriptorLifecycle(device);
    {
        std::lock_guard<std::mutex> lock(g_mapped_mutex);
        CaptureDescriptorBufferWritesLocked(
            descriptorWriteCount, local_pDescriptorWrites);
        CaptureDescriptorCopiesLocked(
            descriptorCopyCount, local_pDescriptorCopies);
        /* Descriptor-set cache is versioned per set; do not invalidate all submit caches here. */
    }

    const FlimeGuestUpdateAction flime_action =
        FlimeGuestUpdateDescriptorSets(FUNID_vkUpdateDescriptorSets,
                                       device,
                                       descriptorWriteCount,
                                       pDescriptorWrites,
                                       descriptorCopyCount,
                                       pDescriptorCopies,
                                       count);
    bool transport_ok = true;
    if (flime_action == FLIME_GUEST_UPDATE_LEGACY) {
        const ssize_t written =
            FlimeGuestWrite(&mgr,
                            express_gpu,
                            EXPRESS_GPU_DEVICE_ID,
                            FUNID_vkUpdateDescriptorSets,
                            true);
        transport_ok = IsCompleteParamManagerWrite(written, 1);
    } else {
        mgr.clear();
        if (flime_action == FLIME_GUEST_UPDATE_FATAL) {
            ALOGE("FLIME rejected vkUpdateDescriptorSets; update was not sent");
        }
    }
    FlimeGuestAfterDescriptorLifecycle(device, transport_ok);
    {
        std::lock_guard<std::mutex> lock(g_descriptor_lifecycle_mutex);
        g_descriptor_lifecycle_stats.update_set_calls++;
        g_descriptor_lifecycle_stats.update_set_us += ExpressVkNowUs() - start_us;
        MaybeLogDescriptorLifecycleStatsLocked("periodic");
    }
    
    if (local_pDescriptorWrites) free(local_pDescriptorWrites);
    if (local_pDescriptorCopies) free(local_pDescriptorCopies);
    
}

VKAPI_ATTR VkResult CreateEvent(VkDevice device,
                               const VkEventCreateInfo* pCreateInfo,
                               const VkAllocationCallbacks* pAllocator,
                               VkEvent* pEvent) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    Allocator vkAllocator;
    
    VkEventCreateInfo* local_pCreateInfo = nullptr;
    if (pCreateInfo) {
        local_pCreateInfo = (VkEventCreateInfo*)malloc(sizeof(VkEventCreateInfo));
        if (!local_pCreateInfo) {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        deepcopy_VkEventCreateInfo(&vkAllocator, VK_STRUCTURE_TYPE_EVENT_CREATE_INFO, pCreateInfo, local_pCreateInfo);
    }
    
    VkAllocationCallbacks* local_pAllocator = nullptr;
    if (pAllocator) {
        local_pAllocator = (VkAllocationCallbacks*)malloc(sizeof(VkAllocationCallbacks));
        if (!local_pAllocator) {
            if (local_pCreateInfo) free(local_pCreateInfo);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        deepcopy_VkAllocationCallbacks(&vkAllocator, VK_STRUCTURE_TYPE_MAX_ENUM, pAllocator, local_pAllocator);
    }
    
    size_t count = 0;
    size_t* countPtr = &count;
    count_VkEventCreateInfo(0, VK_STRUCTURE_TYPE_MAX_ENUM, local_pCreateInfo, countPtr);
    if (pAllocator) {
        count_VkAllocationCallbacks(0, VK_STRUCTURE_TYPE_MAX_ENUM, local_pAllocator, countPtr);
    }
    count += 16; // device + allocator ptr
    
    char* send_buffer = (char*)mgr.addExternalParamPtr(count);
    uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;
    
    uint64_t guest_device = (uint64_t)(uintptr_t)device;
    memcpy(*send_buffer_ptr, &guest_device, 8);
    *send_buffer_ptr += 8;
    
    encode_to_stream_VkEventCreateInfo(VK_STRUCTURE_TYPE_MAX_ENUM, local_pCreateInfo, send_buffer_ptr);
    
    uint64_t cgen_var_0 = (uint64_t)(uintptr_t)local_pAllocator;
    memcpy(*send_buffer_ptr, &cgen_var_0, 8);
    *send_buffer_ptr += 8;
    
    if (pAllocator) {
        encode_to_stream_VkAllocationCallbacks(VK_STRUCTURE_TYPE_MAX_ENUM, local_pAllocator, send_buffer_ptr);
    }
    
    const VkAllocationCallbacks* useAlloc = pAllocator ? pAllocator : &kDefaultAllocCallbacks;
    VkEvent_T* event = static_cast<VkEvent_T*>(useAlloc->pfnAllocation(
        useAlloc->pUserData, sizeof(VkEvent_T), alignof(VkEvent_T),
        VK_SYSTEM_ALLOCATION_SCOPE_OBJECT));
    if (!event) {
        if (local_pCreateInfo) free(local_pCreateInfo);
        if (local_pAllocator) free(local_pAllocator);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    *pEvent = (VkEvent)event;
    
    mgr.addParam64((uint64_t)(uintptr_t)*pEvent);
    
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCreateEvent, false);
    
    if (local_pCreateInfo) free(local_pCreateInfo);
    if (local_pAllocator) free(local_pAllocator);
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateFence(
    VkDevice device,
    const VkFenceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkFence* pFence) {
    
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    Allocator vkAllocator;
    
    VkFenceCreateInfo* localInfo = (VkFenceCreateInfo*)malloc(sizeof(VkFenceCreateInfo));
    deepcopy_VkFenceCreateInfo(&vkAllocator, VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, pCreateInfo, localInfo);
    
    VkAllocationCallbacks* localAlloc = nullptr;
    if (pAllocator) {
        localAlloc = (VkAllocationCallbacks*)malloc(sizeof(VkAllocationCallbacks));
        deepcopy_VkAllocationCallbacks(&vkAllocator, VK_STRUCTURE_TYPE_MAX_ENUM, pAllocator, localAlloc);
    }
    
    size_t byteCount = 0;
    count_VkFenceCreateInfo(0, VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, localInfo, &byteCount);
    if (pAllocator) count_VkAllocationCallbacks(0, VK_STRUCTURE_TYPE_MAX_ENUM, localAlloc, &byteCount);
    byteCount += sizeof(uint64_t);
    
    char* buf = (char*)mgr.addExternalParamPtr(byteCount);
    uint8_t** ptr = (uint8_t**)&buf;
    
    encode_to_stream_VkFenceCreateInfo(VK_STRUCTURE_TYPE_MAX_ENUM, localInfo, ptr);
    
    uint64_t allocPtr = (uint64_t)(uintptr_t)pAllocator;
    memcpy(*ptr, &allocPtr, sizeof(uint64_t));
    *ptr += sizeof(uint64_t);
    
    if (pAllocator) encode_to_stream_VkAllocationCallbacks(VK_STRUCTURE_TYPE_MAX_ENUM, localAlloc, ptr);
    
    const VkAllocationCallbacks* alloc = pAllocator ? pAllocator : &kDefaultAllocCallbacks;
    VkFence_T* fence = static_cast<VkFence_T*>(alloc->pfnAllocation(
        alloc->pUserData, sizeof(VkFence_T), alignof(VkFence_T),
        VK_SYSTEM_ALLOCATION_SCOPE_OBJECT));
    fence->dispatch.magic = HWVULKAN_DISPATCH_MAGIC;
    *pFence = (VkFence)fence;
    
    mgr.addParam64((uint64_t)(uintptr_t)device);
    mgr.addParam64((uint64_t)(uintptr_t)*pFence);
    
    // Fence destruction may happen soon after creation on framework-managed
    // temporary fences.  Wait until the host mapping is installed so a later
    // vkDestroyFence cannot race the asynchronous create path.
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCreateFence, true);
    
    ALOGI("CreateFence device=%lld fence=%lld", (long long)device, (long long)*pFence);
    
    free(localInfo);
    if (localAlloc) free(localAlloc);
    return VK_SUCCESS;
}



// VkResult CreateFramebuffer(VkDevice device,
//                            const VkFramebufferCreateInfo*,
//                            const VkAllocationCallbacks* /*allocator*/,
//                            VkFramebuffer* framebuffer) {
//     *framebuffer = AllocHandle<VkFramebuffer>(device, HandleType::kFramebuffer);
//     return VK_SUCCESS;
// }
// === guest.cpp ===
VKAPI_ATTR VkResult VKAPI_CALL CreateFramebuffer(
    VkDevice                              device,
    const VkFramebufferCreateInfo*       pCreateInfo,
    const VkAllocationCallbacks*         pAllocator,
    VkFramebuffer*                       pFramebuffer)
{
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    Allocator vkAllocator;


    VkFramebufferCreateInfo* localInfo = nullptr;
    if (pCreateInfo) {
        localInfo = (VkFramebufferCreateInfo*)malloc(sizeof(VkFramebufferCreateInfo));
        if (!localInfo) return VK_ERROR_OUT_OF_HOST_MEMORY;
        deepcopy_VkFramebufferCreateInfo(
            &vkAllocator,
            VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            pCreateInfo,
            localInfo);
    }


    VkAllocationCallbacks* localAlloc = nullptr;
    if (pAllocator) {
        localAlloc = (VkAllocationCallbacks*)malloc(sizeof(VkAllocationCallbacks));
        if (!localAlloc) { free(localInfo); return VK_ERROR_OUT_OF_HOST_MEMORY; }
        deepcopy_VkAllocationCallbacks(
            &vkAllocator,
            VK_STRUCTURE_TYPE_MAX_ENUM,
            pAllocator,
            localAlloc);
    }


    size_t byteCount = 0;
    count_VkFramebufferCreateInfo(
        0,
        VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        localInfo,
        &byteCount);
    if (pAllocator) {
        count_VkAllocationCallbacks(
            0,
            VK_STRUCTURE_TYPE_MAX_ENUM,
            localAlloc,
            &byteCount);
    }
    byteCount += sizeof(uint64_t);


    char* sendBuf = (char*)mgr.addExternalParamPtr(byteCount);
    uint8_t** ptr = (uint8_t**)&sendBuf;
    encode_to_stream_VkFramebufferCreateInfo(
        VK_STRUCTURE_TYPE_MAX_ENUM,
        localInfo,
        ptr);


    uint64_t allocPtr = (uint64_t)(uintptr_t)pAllocator;
    memcpy(*ptr, &allocPtr, sizeof(uint64_t));
    *ptr += sizeof(uint64_t);
    if (pAllocator) {
        encode_to_stream_VkAllocationCallbacks(
            VK_STRUCTURE_TYPE_MAX_ENUM,
            localAlloc,
            ptr);
    }


    VkFramebuffer_T* fb = (VkFramebuffer_T*)malloc(sizeof(VkFramebuffer_T));
    *pFramebuffer = (VkFramebuffer)fb;


    mgr.addParam64((uint64_t)(uintptr_t)device);
    mgr.addParam64((uint64_t)(uintptr_t)*pFramebuffer);


    FlimeGuestWrite(&mgr,
        express_gpu,
        EXPRESS_GPU_DEVICE_ID,
        FUNID_vkCreateFramebuffer,
        false);


    free(localInfo);
    if (localAlloc) free(localAlloc);

    return VK_SUCCESS;
}


// VkResult CreateImageView(VkDevice device,
//                          const VkImageViewCreateInfo*,
//                          const VkAllocationCallbacks* /*allocator*/,
//                          VkImageView* view) {
//     *view = AllocHandle<VkImageView>(device, HandleType::kImageView);
//     return VK_SUCCESS;
// }
// === guest.cpp ===
VKAPI_ATTR VkResult VKAPI_CALL CreateImageView(
    VkDevice                             device,
    const VkImageViewCreateInfo*        pCreateInfo,
    const VkAllocationCallbacks*        pAllocator,
    VkImageView*                         pView)
{
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    Allocator vkAllocator;


    VkImageViewCreateInfo* localInfo = nullptr;
    if (pCreateInfo) {
        localInfo = (VkImageViewCreateInfo*)malloc(sizeof(VkImageViewCreateInfo));
        if (!localInfo) return VK_ERROR_OUT_OF_HOST_MEMORY;
        deepcopy_VkImageViewCreateInfo(
            &vkAllocator,
            VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            pCreateInfo,
            localInfo);
    }


    VkAllocationCallbacks* localAlloc = nullptr;
    if (pAllocator) {
        localAlloc = (VkAllocationCallbacks*)malloc(sizeof(VkAllocationCallbacks));
        if (!localAlloc) { free(localInfo); return VK_ERROR_OUT_OF_HOST_MEMORY; }
        deepcopy_VkAllocationCallbacks(
            &vkAllocator,
            VK_STRUCTURE_TYPE_MAX_ENUM,
            pAllocator,
            localAlloc);
    }


    size_t byteCount = 0;
    count_VkImageViewCreateInfo(
        0,
        VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        localInfo,
        &byteCount);
    if (pAllocator) {
        count_VkAllocationCallbacks(
            0,
            VK_STRUCTURE_TYPE_MAX_ENUM,
            localAlloc,
            &byteCount);
    }
    byteCount += sizeof(uint64_t); // room for allocator pointer


    char* sendBuf = (char*)mgr.addExternalParamPtr(byteCount);
    uint8_t** ptr = (uint8_t**)&sendBuf;
    encode_to_stream_VkImageViewCreateInfo(
        VK_STRUCTURE_TYPE_MAX_ENUM,
        localInfo,
        ptr);
    ALOGI("image is %lld", (long long)localInfo->image);

    uint64_t allocPtr = (uint64_t)(uintptr_t)pAllocator;
    memcpy(*ptr, &allocPtr, sizeof(uint64_t));
    *ptr += sizeof(uint64_t);
    if (pAllocator) {
        encode_to_stream_VkAllocationCallbacks(
            VK_STRUCTURE_TYPE_MAX_ENUM,
            localAlloc,
            ptr);
    }


    VkImageView_T* iv = (VkImageView_T*)malloc(sizeof(VkImageView_T));
    *pView = (VkImageView)iv;


    mgr.addParam64((uint64_t)(uintptr_t)device);
    mgr.addParam64((uint64_t)(uintptr_t)*pView);


    VkResult result = VK_SUCCESS;
    // mgr.addPtr(&result, sizeof(VkResult));


    FlimeGuestWrite(&mgr,
        express_gpu,
        EXPRESS_GPU_DEVICE_ID,
        FUNID_vkCreateImageView,
        true);


    free(localInfo);
    if (localAlloc) free(localAlloc);

    return result;
}

VKAPI_ATTR VkResult CreateComputePipelines(VkDevice device,
                                           VkPipelineCache pipelineCache,
                                           uint32_t createInfoCount,
                                           const VkComputePipelineCreateInfo* pCreateInfos,
                                           const VkAllocationCallbacks* pAllocator,
                                           VkPipeline* pPipelines) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    Allocator vkAllocator;
    
    VkComputePipelineCreateInfo* local_pCreateInfos = nullptr;
    if (pCreateInfos && createInfoCount > 0) {
        local_pCreateInfos = (VkComputePipelineCreateInfo*)malloc(sizeof(VkComputePipelineCreateInfo) * createInfoCount);
        if (!local_pCreateInfos) return VK_ERROR_OUT_OF_HOST_MEMORY;
        
        for (uint32_t i = 0; i < createInfoCount; ++i) {
            deepcopy_VkComputePipelineCreateInfo(&vkAllocator, VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, 
                                                &pCreateInfos[i], &local_pCreateInfos[i]);
        }
    }
    
    VkAllocationCallbacks* local_pAllocator = nullptr;
    if (pAllocator) {
        local_pAllocator = (VkAllocationCallbacks*)malloc(sizeof(VkAllocationCallbacks));
        if (!local_pAllocator) {
            if (local_pCreateInfos) free(local_pCreateInfos);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        deepcopy_VkAllocationCallbacks(&vkAllocator, VK_STRUCTURE_TYPE_MAX_ENUM, pAllocator, local_pAllocator);
    }
    
    size_t count = 0;
    size_t* countPtr = &count;
    for (uint32_t i = 0; i < createInfoCount; ++i) {
        count_VkComputePipelineCreateInfo(0, VK_STRUCTURE_TYPE_MAX_ENUM, &local_pCreateInfos[i], countPtr);
    }
    if (pAllocator) {
        count_VkAllocationCallbacks(0, VK_STRUCTURE_TYPE_MAX_ENUM, local_pAllocator, countPtr);
    }
    count += sizeof(uint64_t) * 3 + sizeof(uint32_t) + sizeof(uint8_t);
    
    char* send_buffer = (char*)mgr.addExternalParamPtr(count);
    uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;
    
    uint64_t guest_device = (uint64_t)(uintptr_t)device;
    uint64_t guest_cache = (uint64_t)(uintptr_t)pipelineCache;
    uint64_t guest_allocator = (uint64_t)(uintptr_t)pAllocator;
    
    memcpy(*send_buffer_ptr, &guest_device, sizeof(uint64_t)); *send_buffer_ptr += sizeof(uint64_t);
    memcpy(*send_buffer_ptr, &guest_cache, sizeof(uint64_t)); *send_buffer_ptr += sizeof(uint64_t);
    memcpy(*send_buffer_ptr, &createInfoCount, sizeof(uint32_t)); *send_buffer_ptr += sizeof(uint32_t);
    
    for (uint32_t i = 0; i < createInfoCount; ++i) {
        encode_to_stream_VkComputePipelineCreateInfo(VK_STRUCTURE_TYPE_MAX_ENUM, &local_pCreateInfos[i], send_buffer_ptr);
    }
    
    memcpy(*send_buffer_ptr, &guest_allocator, sizeof(uint64_t)); *send_buffer_ptr += sizeof(uint64_t);
    uint8_t has_allocator = pAllocator ? 1 : 0;
    memcpy(*send_buffer_ptr, &has_allocator, sizeof(uint8_t)); *send_buffer_ptr += sizeof(uint8_t);
    if (pAllocator) {
        encode_to_stream_VkAllocationCallbacks(VK_STRUCTURE_TYPE_MAX_ENUM, local_pAllocator, send_buffer_ptr);
    }
    
    const VkAllocationCallbacks* useAlloc = pAllocator ? pAllocator : &kDefaultAllocCallbacks;
    for (uint32_t i = 0; i < createInfoCount; ++i) {
        VkPipeline_T* pipeline = static_cast<VkPipeline_T*>(useAlloc->pfnAllocation(
            useAlloc->pUserData, sizeof(VkPipeline_T), alignof(VkPipeline_T), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT));
        if (!pipeline) {
            if (local_pCreateInfos) free(local_pCreateInfos);
            if (local_pAllocator) free(local_pAllocator);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        pPipelines[i] = (VkPipeline)pipeline;
    }
    
    mgr.addPtr(pPipelines, sizeof(VkPipeline) * createInfoCount);
    VkResult vkResult = VK_SUCCESS;
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCreateComputePipelines, false);
    
    if (local_pCreateInfos) free(local_pCreateInfos);
    if (local_pAllocator) free(local_pAllocator);
    return vkResult;
}

VKAPI_ATTR VkResult VKAPI_CALL CreatePipelineCache(
    VkDevice                   device,
    const VkPipelineCacheCreateInfo*  pCreateInfo,
    const VkAllocationCallbacks*      pAllocator,
    VkPipelineCache*                  pPipelineCache)
{
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    Allocator vkAllocator;

    VkPipelineCacheCreateInfo* localInfo = nullptr;
    if (pCreateInfo) {
        localInfo = (VkPipelineCacheCreateInfo*)malloc(sizeof(VkPipelineCacheCreateInfo));
        if (!localInfo) return VK_ERROR_OUT_OF_HOST_MEMORY;
        deepcopy_VkPipelineCacheCreateInfo(
            &vkAllocator,
            VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
            pCreateInfo,
            localInfo);
    }

    VkAllocationCallbacks* localAlloc = nullptr;
    if (pAllocator) {
        localAlloc = (VkAllocationCallbacks*)malloc(sizeof(VkAllocationCallbacks));
        if (!localAlloc) { free(localInfo); return VK_ERROR_OUT_OF_HOST_MEMORY; }
        deepcopy_VkAllocationCallbacks(
            &vkAllocator,
            VK_STRUCTURE_TYPE_MAX_ENUM,
            pAllocator,
            localAlloc);
    }

    size_t byteCount = 0;
    count_VkPipelineCacheCreateInfo(
        0,
        VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
        localInfo,
        &byteCount);
    if (pAllocator) {
        count_VkAllocationCallbacks(
            0,
            VK_STRUCTURE_TYPE_MAX_ENUM,
            localAlloc,
            &byteCount);
    }
    byteCount += sizeof(uint64_t);

    char* buf = (char*)mgr.addExternalParamPtr(byteCount);
    uint8_t** ptr = (uint8_t**)&buf;
    encode_to_stream_VkPipelineCacheCreateInfo(
        VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
        localInfo,
        ptr);

    uint64_t allocPtr = (uint64_t)(uintptr_t)pAllocator;
    memcpy(*ptr, &allocPtr, sizeof(uint64_t));
    *ptr += sizeof(uint64_t);
    if (pAllocator) {
        encode_to_stream_VkAllocationCallbacks(
            VK_STRUCTURE_TYPE_MAX_ENUM,
            localAlloc,
            ptr);
    }

    VkPipelineCache_T* cache = (VkPipelineCache_T*)malloc(sizeof(VkPipelineCache_T));
    *pPipelineCache = (VkPipelineCache)cache;

    mgr.addParam64((uint64_t)(uintptr_t)device);
    mgr.addParam64((uint64_t)(uintptr_t)*pPipelineCache);

    FlimeGuestWrite(&mgr,
        express_gpu,
        EXPRESS_GPU_DEVICE_ID,
        FUNID_vkCreatePipelineCache,
        false);

    free(localInfo);
    if (localAlloc) free(localAlloc);

    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateGraphicsPipelines(
    VkDevice                                   device,
    VkPipelineCache                            pipelineCache,
    uint32_t                                   createInfoCount,
    const VkGraphicsPipelineCreateInfo*       pCreateInfos,
    const VkAllocationCallbacks*              pAllocator,
    VkPipeline*                                pPipelines)
{
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    Allocator vkAllocator;

    // deep‐copy array of VkGraphicsPipelineCreateInfo
    VkGraphicsPipelineCreateInfo* localInfos =
        (VkGraphicsPipelineCreateInfo*)malloc(sizeof(VkGraphicsPipelineCreateInfo) * createInfoCount);
    for (uint32_t i = 0; i < createInfoCount; i++) {
        deepcopy_VkGraphicsPipelineCreateInfo(
            &vkAllocator,
            VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            &pCreateInfos[i],
            &localInfos[i]);
    }

    VkAllocationCallbacks* localAlloc = nullptr;
    if (pAllocator) {
        localAlloc = (VkAllocationCallbacks*)malloc(sizeof(VkAllocationCallbacks));
        if (!localAlloc) { free(localInfos); return VK_ERROR_OUT_OF_HOST_MEMORY; }
        deepcopy_VkAllocationCallbacks(
            &vkAllocator,
            VK_STRUCTURE_TYPE_MAX_ENUM,
            pAllocator,
            localAlloc);
    }

    size_t byteCount = 0;
    for (uint32_t i = 0; i < createInfoCount; i++) {
        count_VkGraphicsPipelineCreateInfo(
            0,
            VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            &localInfos[i],
            &byteCount);
    }
    if (pAllocator) {
        count_VkAllocationCallbacks(
            0,
            VK_STRUCTURE_TYPE_MAX_ENUM,
            localAlloc,
            &byteCount);
    }
    byteCount += sizeof(uint64_t);
    byteCount += 20;

    // mgr.addParam64((uint64_t)(uintptr_t)device);
    // mgr.addParam64((uint64_t)(uintptr_t)pipelineCache);
    // // mgr.addParam32(0);
    // mgr.addParam32(createInfoCount);
    // ALOGI("create graphics pipelines count %lld device %lld cache %lld", (long long)createInfoCount, (long long)device, (long long)pipelineCache);

    char* buf = (char*)mgr.addExternalParamPtr(byteCount);
    uint8_t** ptr = (uint8_t**)&buf;

    memcpy(*ptr, &device, sizeof(uint64_t));
    *ptr += sizeof(uint64_t);
    memcpy(*ptr, &pipelineCache, sizeof(uint64_t));
    *ptr += sizeof(uint64_t);
    memcpy(*ptr, &createInfoCount, sizeof(uint32_t));
    *ptr += sizeof(uint32_t);
    ALOGI("create graphics pipelines count %lld device %lld cache %lld", (long long)createInfoCount, (long long)device, (long long)pipelineCache);


    for (uint32_t i = 0; i < createInfoCount; i++) {
        encode_to_stream_VkGraphicsPipelineCreateInfo(
            VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            &localInfos[i],
            ptr);
    }

    uint64_t allocPtr = (uint64_t)(uintptr_t)pAllocator;
    memcpy(*ptr, &allocPtr, sizeof(uint64_t));
    *ptr += sizeof(uint64_t);
    if (pAllocator) {
        encode_to_stream_VkAllocationCallbacks(
            VK_STRUCTURE_TYPE_MAX_ENUM,
            localAlloc,
            ptr);
    }

    VkPipeline_T* tmpPipes = (VkPipeline_T*)malloc(sizeof(VkPipeline_T) * createInfoCount);
    for (uint32_t i = 0; i < createInfoCount; i++) {
        pPipelines[i] = (VkPipeline)&tmpPipes[i];
        mgr.addParam64((uint64_t)(uintptr_t)pPipelines[i]);
        ALOGI("create graphics pipeline %lld", (long long)pPipelines[i]);
    }

    // mgr.addParam32(createInfoCount);

    FlimeGuestWrite(&mgr,
        express_gpu,
        EXPRESS_GPU_DEVICE_ID,
        FUNID_vkCreateGraphicsPipelines,
        false);

    free(localInfos);
    if (localAlloc) free(localAlloc);

    return VK_SUCCESS;
}


VKAPI_ATTR VkResult VKAPI_CALL CreatePipelineLayout(
    VkDevice                            device,
    const VkPipelineLayoutCreateInfo*  pCreateInfo,
    const VkAllocationCallbacks*       pAllocator,
    VkPipelineLayout*                  pPipelineLayout)
{
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    Allocator vkAllocator;

    VkPipelineLayoutCreateInfo* localInfo = nullptr;
    if (pCreateInfo) {
        localInfo = (VkPipelineLayoutCreateInfo*)malloc(sizeof(VkPipelineLayoutCreateInfo));
        if (!localInfo) return VK_ERROR_OUT_OF_HOST_MEMORY;
        deepcopy_VkPipelineLayoutCreateInfo(
            &vkAllocator,
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            pCreateInfo,
            localInfo);
    }

    VkAllocationCallbacks* localAlloc = nullptr;
    if (pAllocator) {
        localAlloc = (VkAllocationCallbacks*)malloc(sizeof(VkAllocationCallbacks));
        if (!localAlloc) { free(localInfo); return VK_ERROR_OUT_OF_HOST_MEMORY; }
        deepcopy_VkAllocationCallbacks(
            &vkAllocator,
            VK_STRUCTURE_TYPE_MAX_ENUM,
            pAllocator,
            localAlloc);
    }

    size_t byteCount = 0;
    count_VkPipelineLayoutCreateInfo(
        0,
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        localInfo,
        &byteCount);
    if (pAllocator) {
        count_VkAllocationCallbacks(
            0,
            VK_STRUCTURE_TYPE_MAX_ENUM,
            localAlloc,
            &byteCount);
    }
    byteCount += sizeof(uint64_t);

    char* buf = (char*)mgr.addExternalParamPtr(byteCount);
    uint8_t** ptr = (uint8_t**)&buf;
    encode_to_stream_VkPipelineLayoutCreateInfo(
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        localInfo,
        ptr);

    uint64_t allocPtr = (uint64_t)(uintptr_t)pAllocator;
    memcpy(*ptr, &allocPtr, sizeof(uint64_t));
    *ptr += sizeof(uint64_t);
    if (pAllocator) {
        encode_to_stream_VkAllocationCallbacks(
            VK_STRUCTURE_TYPE_MAX_ENUM,
            localAlloc,
            ptr);
    }

    VkPipelineLayout_T* layout = (VkPipelineLayout_T*)malloc(sizeof(VkPipelineLayout_T));
    *pPipelineLayout = (VkPipelineLayout)layout;

    mgr.addParam64((uint64_t)(uintptr_t)device);
    mgr.addParam64((uint64_t)(uintptr_t)*pPipelineLayout);

    FlimeGuestWrite(&mgr,
        express_gpu,
        EXPRESS_GPU_DEVICE_ID,
        FUNID_vkCreatePipelineLayout,
        false);

    free(localInfo);
    if (localAlloc) free(localAlloc);

    return VK_SUCCESS;
}

VKAPI_ATTR VkResult CreateQueryPool(VkDevice device,
                                   const VkQueryPoolCreateInfo* pCreateInfo,
                                   const VkAllocationCallbacks* pAllocator,
                                   VkQueryPool* pQueryPool) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    Allocator vkAllocator;
    
    VkQueryPoolCreateInfo* local_pCreateInfo = nullptr;
    if (pCreateInfo) {
        local_pCreateInfo = (VkQueryPoolCreateInfo*)malloc(sizeof(VkQueryPoolCreateInfo));
        if (!local_pCreateInfo) {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        deepcopy_VkQueryPoolCreateInfo(&vkAllocator, VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, pCreateInfo, local_pCreateInfo);
    }
    
    VkAllocationCallbacks* local_pAllocator = nullptr;
    if (pAllocator) {
        local_pAllocator = (VkAllocationCallbacks*)malloc(sizeof(VkAllocationCallbacks));
        if (!local_pAllocator) {
            if (local_pCreateInfo) free(local_pCreateInfo);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        deepcopy_VkAllocationCallbacks(&vkAllocator, VK_STRUCTURE_TYPE_MAX_ENUM, pAllocator, local_pAllocator);
    }
    
    size_t count = 0;
    size_t* countPtr = &count;
    count_VkQueryPoolCreateInfo(0, VK_STRUCTURE_TYPE_MAX_ENUM, local_pCreateInfo, countPtr);
    if (pAllocator) {
        count_VkAllocationCallbacks(0, VK_STRUCTURE_TYPE_MAX_ENUM, local_pAllocator, countPtr);
    }
    count += 16; // device + allocator pointer
    
    char* send_buffer = (char*)mgr.addExternalParamPtr(count);
    uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;
    
    uint64_t cgen_var_0 = (uint64_t)(uintptr_t)device;
    memcpy((*send_buffer_ptr), &cgen_var_0, 8);
    *send_buffer_ptr += 8;
    
    encode_to_stream_VkQueryPoolCreateInfo(VK_STRUCTURE_TYPE_MAX_ENUM, local_pCreateInfo, send_buffer_ptr);
    
    uint64_t cgen_var_1 = (uint64_t)(uintptr_t)local_pAllocator;
    memcpy((*send_buffer_ptr), &cgen_var_1, 8);
    *send_buffer_ptr += 8;
    
    if (pAllocator) {
        encode_to_stream_VkAllocationCallbacks(VK_STRUCTURE_TYPE_MAX_ENUM, local_pAllocator, send_buffer_ptr);
    }
    
    const VkAllocationCallbacks* useAlloc = pAllocator ? pAllocator : &kDefaultAllocCallbacks;
    VkQueryPool_T* queryPool = static_cast<VkQueryPool_T*>(useAlloc->pfnAllocation(
        useAlloc->pUserData, sizeof(VkQueryPool_T), alignof(VkQueryPool_T), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT));
    if (!queryPool) {
        if (local_pCreateInfo) free(local_pCreateInfo);
        if (local_pAllocator) free(local_pAllocator);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    
    *pQueryPool = (VkQueryPool)queryPool;
    
    mgr.addParam64((uint64_t)*pQueryPool);
    
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCreateQueryPool, false);
    
    if (local_pCreateInfo) free(local_pCreateInfo);
    if (local_pAllocator) free(local_pAllocator);
    
    return VK_SUCCESS;
}

// VkResult CreateRenderPass(VkDevice device,
//                           const VkRenderPassCreateInfo*,
//                           const VkAllocationCallbacks* /*allocator*/,
//                           VkRenderPass* renderpass) {
//     ALOGI("CreateRenderPass");
//     *renderpass = AllocHandle<VkRenderPass>(device, HandleType::kRenderPass);
//     return VK_SUCCESS;
// }

VKAPI_ATTR VkResult VKAPI_CALL CreateRenderPass(
    VkDevice                            device,
    const VkRenderPassCreateInfo*      pCreatePassInfo,
    const VkAllocationCallbacks*       pAllocator,
    VkRenderPass*                      pRenderPass)
{
    ALOGI("CreateRenderPass!");
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    Allocator vkAllocator;


    VkRenderPassCreateInfo* localInfo = nullptr;
    if (pCreatePassInfo) {
        localInfo = (VkRenderPassCreateInfo*)malloc(sizeof(VkRenderPassCreateInfo));
        if (!localInfo) return VK_ERROR_OUT_OF_HOST_MEMORY;
        deepcopy_VkRenderPassCreateInfo(
            &vkAllocator,
            VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
            pCreatePassInfo,
            localInfo);
    }


    VkAllocationCallbacks* localAlloc = nullptr;
    if (pAllocator) {
        localAlloc = (VkAllocationCallbacks*)malloc(sizeof(VkAllocationCallbacks));
        if (!localAlloc) {
            free(localInfo);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        deepcopy_VkAllocationCallbacks(
            &vkAllocator,
            VK_STRUCTURE_TYPE_MAX_ENUM,
            pAllocator,
            localAlloc);
    }


    size_t byteCount = 0;
    count_VkRenderPassCreateInfo(
        0,
        VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        localInfo,
        &byteCount);
    if (pAllocator) {
        count_VkAllocationCallbacks(
            0,
            VK_STRUCTURE_TYPE_MAX_ENUM,
            localAlloc,
            &byteCount);
    }

    byteCount += sizeof(uint64_t);


    char* send_buf = (char*)mgr.addExternalParamPtr(byteCount);
    uint8_t** sp = (uint8_t**)&send_buf;
    encode_to_stream_VkRenderPassCreateInfo(
        VK_STRUCTURE_TYPE_MAX_ENUM,
        localInfo,
        sp);


    uint64_t alloc_ptr = (uint64_t)(uintptr_t)pAllocator;
    memcpy(*sp, &alloc_ptr, sizeof(uint64_t));
    *sp += sizeof(uint64_t);
    if (pAllocator) {
        encode_to_stream_VkAllocationCallbacks(
            VK_STRUCTURE_TYPE_MAX_ENUM,
            localAlloc,
            sp);
    }



    VkRenderPass_T* rp = (VkRenderPass_T*)malloc(sizeof(VkRenderPass_T));
    // no dispatch magic needed for render pass
    *pRenderPass = (VkRenderPass)rp;


    mgr.addParam64((uint64_t)(uintptr_t)device);
    mgr.addParam64((uint64_t)(uintptr_t)*pRenderPass);


    VkResult result = VK_SUCCESS;
    // mgr.addPtr(&result, sizeof(VkResult));


    FlimeGuestWrite(&mgr,
        express_gpu,
        EXPRESS_GPU_DEVICE_ID,
        FUNID_vkCreateRenderPass,
        false);


    free(localInfo);
    if (localAlloc) free(localAlloc);

    return result;
}


VKAPI_ATTR VkResult VKAPI_CALL CreateSemaphore(
    VkDevice device,
    const VkSemaphoreCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSemaphore* pSemaphore) {
    
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    Allocator vkAllocator;
    
    VkSemaphoreCreateInfo* localInfo = (VkSemaphoreCreateInfo*)malloc(sizeof(VkSemaphoreCreateInfo));
    deepcopy_VkSemaphoreCreateInfo(&vkAllocator, VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, pCreateInfo, localInfo);
    
    VkAllocationCallbacks* localAlloc = nullptr;
    if (pAllocator) {
        localAlloc = (VkAllocationCallbacks*)malloc(sizeof(VkAllocationCallbacks));
        deepcopy_VkAllocationCallbacks(&vkAllocator, VK_STRUCTURE_TYPE_MAX_ENUM, pAllocator, localAlloc);
    }
    
    size_t byteCount = 0;
    count_VkSemaphoreCreateInfo(0, VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, localInfo, &byteCount);
    if (pAllocator) count_VkAllocationCallbacks(0, VK_STRUCTURE_TYPE_MAX_ENUM, localAlloc, &byteCount);
    byteCount += sizeof(uint64_t);
    
    char* buf = (char*)mgr.addExternalParamPtr(byteCount);
    uint8_t** ptr = (uint8_t**)&buf;
    
    encode_to_stream_VkSemaphoreCreateInfo(VK_STRUCTURE_TYPE_MAX_ENUM, localInfo, ptr);
    
    uint64_t allocPtr = (uint64_t)(uintptr_t)pAllocator;
    memcpy(*ptr, &allocPtr, sizeof(uint64_t));
    *ptr += sizeof(uint64_t);
    
    if (pAllocator) encode_to_stream_VkAllocationCallbacks(VK_STRUCTURE_TYPE_MAX_ENUM, localAlloc, ptr);
    
    const VkAllocationCallbacks* alloc = pAllocator ? pAllocator : &kDefaultAllocCallbacks;
    VkSemaphore_T* semaphore = static_cast<VkSemaphore_T*>(alloc->pfnAllocation(
        alloc->pUserData, sizeof(VkSemaphore_T), alignof(VkSemaphore_T),
        VK_SYSTEM_ALLOCATION_SCOPE_OBJECT));
    semaphore->dispatch.magic = HWVULKAN_DISPATCH_MAGIC;
    *pSemaphore = (VkSemaphore)semaphore;
    
    mgr.addParam64((uint64_t)(uintptr_t)device);
    mgr.addParam64((uint64_t)(uintptr_t)*pSemaphore);
    
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCreateSemaphore, false);
    
    ALOGI("CreateSemaphore device=%lld semaphore=%lld", (long long)device, (long long)*pSemaphore);
    
    free(localInfo);
    if (localAlloc) free(localAlloc);
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateShaderModule(
    VkDevice                           device,
    const VkShaderModuleCreateInfo*   pCreateInfo,
    const VkAllocationCallbacks*      pAllocator,
    VkShaderModule*                   pShaderModule)
{
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    Allocator vkAllocator;

    VkShaderModuleCreateInfo* localInfo = nullptr;
    if (pCreateInfo) {
        localInfo = (VkShaderModuleCreateInfo*)malloc(sizeof(VkShaderModuleCreateInfo));
        if (!localInfo) return VK_ERROR_OUT_OF_HOST_MEMORY;
        deepcopy_VkShaderModuleCreateInfo(
            &vkAllocator,
            VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            pCreateInfo,
            localInfo);
    }

    VkAllocationCallbacks* localAlloc = nullptr;
    if (pAllocator) {
        localAlloc = (VkAllocationCallbacks*)malloc(sizeof(VkAllocationCallbacks));
        if (!localAlloc) { free(localInfo); return VK_ERROR_OUT_OF_HOST_MEMORY; }
        deepcopy_VkAllocationCallbacks(
            &vkAllocator,
            VK_STRUCTURE_TYPE_MAX_ENUM,
            pAllocator,
            localAlloc);
    }

    size_t byteCount = 0;
    count_VkShaderModuleCreateInfo(
        0,
        VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        localInfo,
        &byteCount);
    if (pAllocator) {
        count_VkAllocationCallbacks(
            0,
            VK_STRUCTURE_TYPE_MAX_ENUM,
            localAlloc,
            &byteCount);
    }
    byteCount += sizeof(uint64_t);

    char* buf = (char*)mgr.addExternalParamPtr(byteCount);
    uint8_t** ptr = (uint8_t**)&buf;
    encode_to_stream_VkShaderModuleCreateInfo(
        VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        localInfo,
        ptr);

    uint64_t allocPtr = (uint64_t)(uintptr_t)pAllocator;
    memcpy(*ptr, &allocPtr, sizeof(uint64_t));
    *ptr += sizeof(uint64_t);
    if (pAllocator) {
        encode_to_stream_VkAllocationCallbacks(
            VK_STRUCTURE_TYPE_MAX_ENUM,
            localAlloc,
            ptr);
    }

    VkShaderModule_T* module = (VkShaderModule_T*)malloc(sizeof(VkShaderModule_T));
    *pShaderModule = (VkShaderModule)module;

    mgr.addParam64((uint64_t)(uintptr_t)device);
    mgr.addParam64((uint64_t)(uintptr_t)*pShaderModule);

    VkResult result = VK_SUCCESS;

    FlimeGuestWrite(&mgr,
        express_gpu,
        EXPRESS_GPU_DEVICE_ID,
        FUNID_vkCreateShaderModule,
        false);

    free(localInfo);
    if (localAlloc) free(localAlloc);

    return result;
}


VkResult CreateDebugReportCallbackEXT(VkInstance instance,
                                      const VkDebugReportCallbackCreateInfoEXT*,
                                      const VkAllocationCallbacks*,
                                      VkDebugReportCallbackEXT* callback) {
    *callback = AllocHandle<VkDebugReportCallbackEXT>(
        instance, HandleType::kDebugReportCallbackEXT);
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult CreateRenderPass2(VkDevice device,
                                      const VkRenderPassCreateInfo2* pCreateInfo,
                                      const VkAllocationCallbacks* pAllocator,
                                      VkRenderPass* pRenderPass) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    Allocator vkAllocator;
    
    VkRenderPassCreateInfo2* local_pCreateInfo = nullptr;
    if (pCreateInfo) {
        local_pCreateInfo = (VkRenderPassCreateInfo2*)malloc(sizeof(VkRenderPassCreateInfo2));
        if (!local_pCreateInfo) return VK_ERROR_OUT_OF_HOST_MEMORY;
        deepcopy_VkRenderPassCreateInfo2(&vkAllocator, VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2,
                                        pCreateInfo, local_pCreateInfo);
    }
    
    VkAllocationCallbacks* local_pAllocator = nullptr;
    if (pAllocator) {
        local_pAllocator = (VkAllocationCallbacks*)malloc(sizeof(VkAllocationCallbacks));
        if (!local_pAllocator) {
            if (local_pCreateInfo) free(local_pCreateInfo);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        deepcopy_VkAllocationCallbacks(&vkAllocator, VK_STRUCTURE_TYPE_MAX_ENUM, pAllocator, local_pAllocator);
    }
    
    size_t count = 0;
    size_t* countPtr = &count;
    count_VkRenderPassCreateInfo2(0, VK_STRUCTURE_TYPE_MAX_ENUM, local_pCreateInfo, countPtr);
    if (pAllocator) {
        count_VkAllocationCallbacks(0, VK_STRUCTURE_TYPE_MAX_ENUM, local_pAllocator, countPtr);
    }
    count += sizeof(uint64_t) * 2 + sizeof(uint8_t);
    
    char* send_buffer = (char*)mgr.addExternalParamPtr(count);
    uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;
    
    uint64_t guest_device = (uint64_t)(uintptr_t)device;
    uint64_t guest_allocator = (uint64_t)(uintptr_t)pAllocator;
    
    memcpy(*send_buffer_ptr, &guest_device, sizeof(uint64_t)); *send_buffer_ptr += sizeof(uint64_t);
    encode_to_stream_VkRenderPassCreateInfo2(VK_STRUCTURE_TYPE_MAX_ENUM, local_pCreateInfo, send_buffer_ptr);
    memcpy(*send_buffer_ptr, &guest_allocator, sizeof(uint64_t)); *send_buffer_ptr += sizeof(uint64_t);
    
    uint8_t has_allocator = pAllocator ? 1 : 0;
    memcpy(*send_buffer_ptr, &has_allocator, sizeof(uint8_t)); *send_buffer_ptr += sizeof(uint8_t);
    if (pAllocator) {
        encode_to_stream_VkAllocationCallbacks(VK_STRUCTURE_TYPE_MAX_ENUM, local_pAllocator, send_buffer_ptr);
    }
    
    const VkAllocationCallbacks* useAlloc = pAllocator ? pAllocator : &kDefaultAllocCallbacks;
    VkRenderPass_T* renderpass = static_cast<VkRenderPass_T*>(useAlloc->pfnAllocation(
        useAlloc->pUserData, sizeof(VkRenderPass_T), alignof(VkRenderPass_T), 
        VK_SYSTEM_ALLOCATION_SCOPE_OBJECT));
    if (!renderpass) {
        if (local_pCreateInfo) free(local_pCreateInfo);
        if (local_pAllocator) free(local_pAllocator);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    *pRenderPass = (VkRenderPass)renderpass;
    
    mgr.addPtr(pRenderPass, sizeof(VkRenderPass));
    VkResult vkResult = VK_SUCCESS;
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCreateRenderPass2, false);
    
    if (local_pCreateInfo) free(local_pCreateInfo);
    if (local_pAllocator) free(local_pAllocator);
    return vkResult;
}

// -----------------------------------------------------------------------------
// No-op entrypoints

// clang-format off
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"

VKAPI_ATTR void GetPhysicalDeviceFormatProperties(VkPhysicalDevice physicalDevice,
                                                    VkFormat format,
                                                    VkFormatProperties* pFormatProperties) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    uint64_t guest_device = (uint64_t)(uintptr_t)physicalDevice;
    
    mgr.addParam64(guest_device);
    mgr.addParam32((uint32_t)format);
    mgr.addPtr(pFormatProperties, sizeof(VkFormatProperties));
    
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkGetPhysicalDeviceFormatProperties, true);
    ALOGI("vkGetPhysicalDeviceFormatProperties device=%lld format=%d result =%d", 
          (long long)guest_device, (int)format, pFormatProperties->linearTilingFeatures);
}

void GetPhysicalDeviceFormatProperties2KHR(VkPhysicalDevice physicalDevice, VkFormat format, VkFormatProperties2KHR* pFormatProperties) {
    ALOGV("TODO: vk%s", __FUNCTION__);
}

VkResult GetPhysicalDeviceImageFormatProperties(VkPhysicalDevice physicalDevice, VkFormat format, VkImageType type, VkImageTiling tiling, VkImageUsageFlags usage, VkImageCreateFlags flags, VkImageFormatProperties* pImageFormatProperties) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;

    mgr.addParam64((uint64_t)(uintptr_t)physicalDevice);
    mgr.addParam32(format);
    mgr.addParam32(type);
    mgr.addParam32(tiling);
    mgr.addParam32(usage);
    mgr.addParam32(flags);
    mgr.addPtr(pImageFormatProperties, sizeof(VkImageFormatProperties));
    FlimeGuestWrite(&mgr,
        express_gpu,
        EXPRESS_GPU_DEVICE_ID,
        FUNID_vkGetPhysicalDeviceImageFormatProperties,
        true);

    ALOGI("get returned image properties %d %d %d %d %d",
          pImageFormatProperties->maxExtent.width,
          pImageFormatProperties->maxExtent.height,
          pImageFormatProperties->maxExtent.depth,
          pImageFormatProperties->maxMipLevels,
          pImageFormatProperties->maxArrayLayers);
    return VK_SUCCESS;
}

VkResult GetPhysicalDeviceImageFormatProperties2(VkPhysicalDevice physicalDevice, const VkPhysicalDeviceImageFormatInfo2* pImageFormatInfo, VkImageFormatProperties2* pImageFormatProperties) {
    ALOGI("GetPhysicalDeviceImageFormatProperties2: physicalDevice=%p, pImageFormatInfo=%p, pImageFormatProperties=%p",
          physicalDevice, pImageFormatInfo, pImageFormatProperties);
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    Allocator vkAllocator;

    mgr.addParam64((uint64_t)(uintptr_t)physicalDevice);
    VkPhysicalDeviceImageFormatInfo2* localImageFormatInfo = nullptr;
    if (pImageFormatInfo) {
        localImageFormatInfo = (VkPhysicalDeviceImageFormatInfo2*)malloc(
            sizeof(VkPhysicalDeviceImageFormatInfo2));
        if (!localImageFormatInfo) return VK_ERROR_OUT_OF_HOST_MEMORY;
        deepcopy_VkPhysicalDeviceImageFormatInfo2(
            &vkAllocator,
            VK_STRUCTURE_TYPE_MAX_ENUM, 
            pImageFormatInfo,
            localImageFormatInfo);
    }

    size_t count = 0;
    count_VkPhysicalDeviceImageFormatInfo2(
        0,
        VK_STRUCTURE_TYPE_MAX_ENUM,
        localImageFormatInfo,
        &count);
    
    char* send_buffer = (char*)mgr.addExternalParamPtr(count);
    uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;
    encode_to_stream_VkPhysicalDeviceImageFormatInfo2(
        VK_STRUCTURE_TYPE_MAX_ENUM,
        localImageFormatInfo,
        send_buffer_ptr);

    mgr.addPtr(pImageFormatProperties, sizeof(VkImageFormatProperties2));

    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkGetPhysicalDeviceImageFormatProperties2, true);

    ALOGI("get returned image format properties2: "
          "maxExtent=(%d, %d, %d), maxMipLevels=%d, maxArrayLayers=%d",
          pImageFormatProperties->imageFormatProperties.maxExtent.width,
          pImageFormatProperties->imageFormatProperties.maxExtent.height,
          pImageFormatProperties->imageFormatProperties.maxExtent.depth,
          pImageFormatProperties->imageFormatProperties.maxMipLevels,
          pImageFormatProperties->imageFormatProperties.maxArrayLayers);

    free(localImageFormatInfo);

    return VK_SUCCESS;
}

VkResult GetPhysicalDeviceImageFormatProperties2KHR(VkPhysicalDevice physicalDevice,
                                                    const VkPhysicalDeviceImageFormatInfo2KHR* pImageFormatInfo,
                                                    VkImageFormatProperties2KHR* pImageFormatProperties) {
    ALOGV("TODO: vk%s", __FUNCTION__);
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult EnumerateInstanceLayerProperties(uint32_t* pPropertyCount,
                                                     VkLayerProperties* pProperties) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    mgr.addPtr(pPropertyCount, sizeof(uint32_t));
    if (pProperties && *pPropertyCount > 0) {
        mgr.addPtr(pProperties, sizeof(VkLayerProperties) * (*pPropertyCount));
    }
    
    VkResult vkResult = VK_SUCCESS;
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkEnumerateInstanceLayerProperties, true);
    
    return vkResult;
}

VKAPI_ATTR VkResult QueueSubmit(VkQueue queue,
                                  uint32_t submitCount,
                                  const VkSubmitInfo* pSubmits,
                                  VkFence fence) {
    const uint64_t rpc_start_us = ExpressVkNowUs();
    SubmitSyncHints submit_hints;

    if (submitCount != 0 && pSubmits == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (kEnableImplicitGlobalMappedSync) {
        submit_hints = PrimeFlushHintsFromSubmitInfos(submitCount, pSubmits);
        struct timespec t0_imp, t1_imp;
        clock_gettime(CLOCK_MONOTONIC, &t0_imp);
        ImplicitFlushAllMappedMemories();
        clock_gettime(CLOCK_MONOTONIC, &t1_imp);
        if (kEnableImplicitSyncDiagLog) {
            double imp_ms = (t1_imp.tv_sec - t0_imp.tv_sec) * 1000.0 +
                            (t1_imp.tv_nsec - t0_imp.tv_nsec) / 1000000.0;
            EVK_PER_CALL_LOG("[SYNC_GUEST] queue_submit_implicit_flush queue=%llx submitCount=%u ms=%.3f",
                             (unsigned long long)(uintptr_t)queue,
                             submitCount,
                             imp_ms);
        }
        FinalizeSubmitWaitFlushRanges(&submit_hints);
    }
    const uint64_t hint_done_us = ExpressVkNowUs();
    
    const bool defer_fence_wait = ShouldDeferFenceWaitForSubmit(fence, submit_hints);
    NoteReadbackFenceDecision("vkQueueSubmit", fence, submit_hints, defer_fence_wait);
    VkFence host_fence = defer_fence_wait ? VK_NULL_HANDLE : fence;
    uint64_t guest_queue = (uint64_t)(uintptr_t)queue;
    uint64_t command_buffer_count = 0;
    uint64_t wait_semaphore_count = 0;
    uint64_t signal_semaphore_count = 0;

    for (uint32_t i = 0; pSubmits && i < submitCount; ++i) {
        wait_semaphore_count += pSubmits[i].waitSemaphoreCount;
        command_buffer_count += pSubmits[i].commandBufferCount;
        signal_semaphore_count += pSubmits[i].signalSemaphoreCount;
    }

    FlushPendingSubmitCohort("flime_before_queue_submit");
    FlimeGuestSubmitToken flime_submit_token = {};
    const FlimeGuestSubmitGate flime_submit_gate =
        FlimeGuestBeforeQueueSubmit(queue,
                                    submitCount,
                                    pSubmits,
                                    fence,
                                    &flime_submit_token);
    if (flime_submit_gate == FLIME_GUEST_SUBMIT_BLOCKED) {
        ALOGE("FLIME blocked vkQueueSubmit before the host commit");
        return VK_ERROR_DEVICE_LOST;
    }

    const bool can_coalesce =
        kEnableSubmitCoalescing &&
        !flime_submit_token.valid &&
        submitCount == 1 &&
        pSubmits &&
        pSubmits[0].pNext == nullptr &&
        pSubmits[0].waitSemaphoreCount == 0 &&
        pSubmits[0].signalSemaphoreCount == 0 &&
        pSubmits[0].commandBufferCount != 0 &&
        pSubmits[0].pCommandBuffers != nullptr &&
        host_fence == VK_NULL_HANDLE;

    if (can_coalesce) {
        if (kEnableImplicitGlobalMappedSync && fence != VK_NULL_HANDLE) {
            StoreFenceInvalidateHints(fence, submit_hints);
        }
        PendingSubmitCohortEntry entry;
        entry.command_buffers.assign(pSubmits[0].pCommandBuffers,
                                     pSubmits[0].pCommandBuffers +
                                         pSubmits[0].commandBufferCount);
        entry.hints = std::move(submit_hints);
        entry.hint_us = hint_done_us - rpc_start_us;
        entry.has_fence = fence != VK_NULL_HANDLE;
        entry.deferred_fence = defer_fence_wait;
        TryEnqueueSubmitCohort(queue, std::move(entry));

        if (defer_fence_wait) {
            TrackDeferredFenceWait(fence, queue);
        }
        const uint64_t rpc_done_us = ExpressVkNowUs();
        NoteSubmitCohortStats("vkQueueSubmit",
                              queue,
                              submitCount,
                              command_buffer_count,
                              wait_semaphore_count,
                              signal_semaphore_count,
                              fence != VK_NULL_HANDLE,
                              defer_fence_wait,
                              rpc_start_us,
                              rpc_done_us);

        ALOGV("vkQueueSubmit queued queue=%lld submitCount=%d fence=%lld deferred=%d",
              (long long)guest_queue, submitCount, (long long)(uintptr_t)fence,
              (int)defer_fence_wait);
        MemShapeNoteSubmit("queue_submit_coalesced");
        return VK_SUCCESS;
    }

    QueueSubmitHostSendStats send_stats;
    const VkResult vkResult =
        SendQueueSubmitToHost(queue,
                              submitCount,
                              pSubmits,
                              host_fence,
                              submit_hints,
                              &send_stats);
    FlimeGuestAfterQueueSubmit(&flime_submit_token, vkResult);
    if (vkResult == VK_SUCCESS &&
        kEnableImplicitGlobalMappedSync && fence != VK_NULL_HANDLE) {
        StoreFenceInvalidateHints(fence, submit_hints);
    }
    if (vkResult == VK_SUCCESS && defer_fence_wait) {
        TrackDeferredFenceWait(fence, queue);
    }
    const uint64_t rpc_done_us = ExpressVkNowUs();
    NoteSubmitHintStats("vkQueueSubmit",
                        submit_hints,
                        send_stats.hint_wire_size,
                        defer_fence_wait,
                        submitCount);
    NoteSubmitRpcStats("vkQueueSubmit",
                       submitCount,
                       send_stats.command_buffers,
                       send_stats.wait_semaphores,
                       send_stats.signal_semaphores,
                       fence != VK_NULL_HANDLE,
                       defer_fence_wait,
                       false,
                       hint_done_us - rpc_start_us,
                       send_stats.encode_us,
                       send_stats.write_us,
                       rpc_done_us - rpc_start_us);
    {
        std::lock_guard<std::mutex> lock(g_submit_hint_stats_mutex);
        g_submit_coalesce_stats.immediate_submits += submitCount;
    }
    NoteSubmitCohortStats("vkQueueSubmit",
                          queue,
                          submitCount,
                          command_buffer_count,
                          wait_semaphore_count,
                          signal_semaphore_count,
                          fence != VK_NULL_HANDLE,
                          defer_fence_wait,
                          rpc_start_us,
                          rpc_done_us);
    
    ALOGV("vkQueueSubmit queue=%lld submitCount=%d fence=%lld deferred=%d",
          (long long)guest_queue, submitCount, (long long)(uintptr_t)fence,
          (int)defer_fence_wait);
    MemShapeNoteSubmit("queue_submit");
    
    return vkResult;
}

VKAPI_ATTR VkResult QueueWaitIdle(VkQueue queue) {
    const uint64_t timing_start_us = ExpressVkNowUs();
    FlushPendingSubmitCohortForQueue(queue, "queue_wait_idle");
    if (kEnableDeferredFenceWait) {
        RawQueueWaitIdleForDeferred(queue, "queue_wait_idle");
    }

    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    uint64_t guest_queue = (uint64_t)(uintptr_t)queue;
    mgr.addParam64(guest_queue);

    VkResult vkResult = VK_SUCCESS;
    mgr.addPtr(&vkResult, sizeof(VkResult)); /* VkResult */
    
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkQueueWaitIdle, true);
    uint64_t invalidate_bytes = 0;
    if (kEnableImplicitGlobalMappedSync && vkResult == VK_SUCCESS) {
        if (UseConservativeMappedReadbackFallback()) {
            uint64_t skipped_synced = 0;
            InvalidateAllMappedMemoriesForReadback("queue_wait_idle_conservative",
                                                   &invalidate_bytes,
                                                   &skipped_synced);
        } else {
            ImplicitInvalidateAllMappedMemories();
        }
    }
    DumpExpressVkGuestPerfStats("queue_wait_idle");
    MemShapeForceSummary("queue_wait_idle");
    if (kEnableLlmVkTimingLog) {
        g_llm_vk_timing.queue_idle_calls.fetch_add(1, std::memory_order_relaxed);
        g_llm_vk_timing.queue_idle_total_us.fetch_add(ExpressVkNowUs() - timing_start_us,
                                                      std::memory_order_relaxed);
        g_llm_vk_timing.queue_idle_invalidate_bytes.fetch_add(invalidate_bytes,
                                                              std::memory_order_relaxed);
        LlmVkTimingMaybeLog("queue_wait_idle");
    }
    ALOGI("get vkqueuewaitidle result %d", vkResult);
    return vkResult;
}

VKAPI_ATTR VkResult DeviceWaitIdle(VkDevice device) {
    const uint64_t timing_start_us = ExpressVkNowUs();
    FlushPendingSubmitCohort("device_wait_idle");
    if (kEnableDeferredFenceWait) {
        DrainDeferredQueues("device_wait_idle");
    }

    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    uint64_t guest_device = (uint64_t)(uintptr_t)device;
    mgr.addParam64(guest_device);
    
    VkResult vkResult = VK_SUCCESS;
    mgr.addPtr(&vkResult, sizeof(VkResult));
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkDeviceWaitIdle, true);

    uint64_t invalidate_bytes = 0;
    if (kEnableImplicitGlobalMappedSync && vkResult == VK_SUCCESS) {
        if (UseConservativeMappedReadbackFallback()) {
            uint64_t skipped_synced = 0;
            InvalidateAllMappedMemoriesForReadback("device_wait_idle_conservative",
                                                   &invalidate_bytes,
                                                   &skipped_synced);
        } else {
            ImplicitInvalidateAllMappedMemories();
        }
    }
    DumpExpressVkGuestPerfStats("device_wait_idle");
    MemShapeForceSummary("device_wait_idle");
    if (kEnableLlmVkTimingLog) {
        g_llm_vk_timing.device_idle_calls.fetch_add(1, std::memory_order_relaxed);
        g_llm_vk_timing.device_idle_total_us.fetch_add(ExpressVkNowUs() - timing_start_us,
                                                       std::memory_order_relaxed);
        g_llm_vk_timing.device_idle_invalidate_bytes.fetch_add(invalidate_bytes,
                                                               std::memory_order_relaxed);
        LlmVkTimingMaybeLog("device_wait_idle");
    }
    
    return vkResult;
}

VKAPI_ATTR void VKAPI_CALL UnmapMemory(
    VkDevice       device,
    VkDeviceMemory memory)
{
    if (kEnableDeferredFenceWait) {
        DrainDeferredQueues("unmap_memory");
    }

    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;

    VkDeviceMemory_T* mem = (VkDeviceMemory_T*)(memory);
    if (!mem) {
        ALOGE("UnmapMemory called with null memory handle");
        return;
    }

    mgr.addParam64((uint64_t)(uintptr_t)device);
    mgr.addParam64((uint64_t)(uintptr_t)memory);
    if (!mem->express_vk_mem_registered) {
        mgr.addPtr(mem->map_data, mem->length);
    }
    if (kEnableLocalPerfLog) {
        ALOGI("UnmapMemory device=%lld memory=%lld", (long long)device, (long long)memory);
    }
    {
        std::lock_guard<std::mutex> lock(g_mapped_mutex);
        auto active_it = g_active_mapped_memories.find(memory);
        bool shadow_pooled = false;
        size_t shadow_capacity = 0;
        if (active_it != g_active_mapped_memories.end()) {
            ReleaseShadowBufferLocked(
                std::move(active_it->second.shadow),
                &shadow_pooled,
                &shadow_capacity);
            g_active_mapped_memories.erase(active_it);
        }
        g_flush_hint_memories.erase(memory);
        g_invalidate_hint_memories.erase(memory);
        g_recently_flushed_memories.erase(memory);
        EraseTrackedRangesForMemoryLocked(memory);
        MemShapeRecordUnmapLocked(memory);
        ResetImplicitCleanScanThrottleLocked();
        if (kEnableLocalPerfLog) {
            ALOGI("[PERF_UnmapMemoryShadow] memory=%lld pooled=%d capacity=%zu pool_entries=%zu pool_bytes=%zu",
                  (long long)memory,
                  (int)shadow_pooled,
                  shadow_capacity,
                  g_shadow_buffer_pool.size(),
                  g_shadow_buffer_pool_bytes);
        }
    }

    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkUnmapMemory, false);
    if (kEnableLlmVkTimingLog) {
        g_llm_vk_timing.unmap_calls.fetch_add(1, std::memory_order_relaxed);
        LlmVkTimingMaybeLog("unmap_memory");
    }
}


VKAPI_ATTR VkResult FlushMappedMemoryRanges(
    VkDevice device,
    uint32_t memoryRangeCount,
    const VkMappedMemoryRange* pMemoryRanges) {
    struct timespec t0_start, t1_copy, t2_encode, t3_addptr, t4_rpc, t5_record, t6_done;
    clock_gettime(CLOCK_MONOTONIC, &t0_start);
    
    uint64_t flush_seq = ++g_sync_trace_seq;
    
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;

    VkMappedMemoryRange* local_pMemoryRanges = nullptr;
    if (pMemoryRanges && memoryRangeCount > 0) {
        local_pMemoryRanges = (VkMappedMemoryRange*)malloc(sizeof(VkMappedMemoryRange) * memoryRangeCount);
        if (!local_pMemoryRanges) return VK_ERROR_OUT_OF_HOST_MEMORY;
        
        Allocator vkAllocator;
        for (uint32_t i = 0; i < memoryRangeCount; ++i) {
            deepcopy_VkMappedMemoryRange(&vkAllocator, VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                                       &pMemoryRanges[i], &local_pMemoryRanges[i]);
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1_copy);
    
    size_t count = 0;
    size_t* countPtr = &count;
    count += 8 + 4;
    
    for (uint32_t i = 0; i < memoryRangeCount; ++i) {
        count_VkMappedMemoryRange(0, VK_STRUCTURE_TYPE_MAX_ENUM,
                                (VkMappedMemoryRange*)&local_pMemoryRanges[i], countPtr);
    }
    
    char* send_buffer = (char*)mgr.addExternalParamPtr(count);
    uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;
    
    uint64_t guest_device = (uint64_t)(uintptr_t)device;
    memcpy((*send_buffer_ptr), &guest_device, 8);
    (*send_buffer_ptr) += 8;
    
    memcpy((*send_buffer_ptr), &memoryRangeCount, 4);
    (*send_buffer_ptr) += 4;
    
    for (uint32_t i = 0; i < memoryRangeCount; ++i) {
        encode_to_stream_VkMappedMemoryRange(VK_STRUCTURE_TYPE_MAX_ENUM,
                                           (VkMappedMemoryRange*)&local_pMemoryRanges[i],
                                           send_buffer_ptr);
    }
    clock_gettime(CLOCK_MONOTONIC, &t2_encode);

    uint64_t total_bytes = 0;
    for(uint32_t i = 0; i < memoryRangeCount; ++i) {
        VkDeviceMemory_T* mem = (VkDeviceMemory_T*)(local_pMemoryRanges[i].memory);
        if (!mem || !mem->map_data || mem->length == 0) {
            continue;
        }

        VkDeviceSize offset_bytes = local_pMemoryRanges[i].offset;
        if (offset_bytes > mem->length) {
            offset_bytes = mem->length;
        }

        VkDeviceSize size_bytes = local_pMemoryRanges[i].size;
        if (size_bytes == VK_WHOLE_SIZE ||
            offset_bytes + size_bytes > mem->length ||
            offset_bytes + size_bytes < offset_bytes) {
            size_bytes = mem->length - offset_bytes;
        }

        total_bytes += (uint64_t)size_bytes;

        if (!mem->express_vk_mem_registered) {
            mgr.addPtr(mem->map_data + offset_bytes, (size_t)size_bytes);
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t3_addptr);

    {
        std::lock_guard<std::mutex> lock(g_mapped_mutex);
        MemShapeNoteMappedRangesLocked(memoryRangeCount,
                                       local_pMemoryRanges,
                                       MemShapeRangeKind::kFlush);
    }

    if (kEnableImplicitSyncDiagLog) {
        ALOGD("[SYNC_GUEST] flush seq=%llu device=%llx ranges=%u bytes=%llu",
              (unsigned long long)flush_seq,
              (unsigned long long)(uintptr_t)device,
              memoryRangeCount,
              (unsigned long long)total_bytes);
    }

    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkFlushMappedMemoryRanges, false);
    clock_gettime(CLOCK_MONOTONIC, &t4_rpc);

    RecordRecentlyFlushedRanges(memoryRangeCount, local_pMemoryRanges);
    clock_gettime(CLOCK_MONOTONIC, &t5_record);
    
    UpdateShadowWithRanges(device, memoryRangeCount, local_pMemoryRanges);
    clock_gettime(CLOCK_MONOTONIC, &t6_done);
    
    double copy_ms = (t1_copy.tv_sec - t0_start.tv_sec) * 1000.0 + (t1_copy.tv_nsec - t0_start.tv_nsec) / 1000000.0;
    double encode_ms = (t2_encode.tv_sec - t1_copy.tv_sec) * 1000.0 + (t2_encode.tv_nsec - t1_copy.tv_nsec) / 1000000.0;
    double addptr_ms = (t3_addptr.tv_sec - t2_encode.tv_sec) * 1000.0 + (t3_addptr.tv_nsec - t2_encode.tv_nsec) / 1000000.0;
    double rpc_ms = (t4_rpc.tv_sec - t3_addptr.tv_sec) * 1000.0 + (t4_rpc.tv_nsec - t3_addptr.tv_nsec) / 1000000.0;
    double record_ms = (t5_record.tv_sec - t4_rpc.tv_sec) * 1000.0 + (t5_record.tv_nsec - t4_rpc.tv_nsec) / 1000000.0;
    double shadow_ms = (t6_done.tv_sec - t5_record.tv_sec) * 1000.0 + (t6_done.tv_nsec - t5_record.tv_nsec) / 1000000.0;
    double total_ms = (t6_done.tv_sec - t0_start.tv_sec) * 1000.0 + (t6_done.tv_nsec - t0_start.tv_nsec) / 1000000.0;
    NoteMappedFlushStats(memoryRangeCount,
                         total_bytes,
                         ExpressVkElapsedUs(t0_start, t1_copy),
                         ExpressVkElapsedUs(t1_copy, t2_encode),
                         ExpressVkElapsedUs(t2_encode, t3_addptr),
                         ExpressVkElapsedUs(t3_addptr, t4_rpc),
                         ExpressVkElapsedUs(t4_rpc, t5_record),
                         ExpressVkElapsedUs(t5_record, t6_done),
                         ExpressVkElapsedUs(t0_start, t6_done));
    
    ALOGV("[PERF_Flush] seq=%llu ranges=%u bytes=%llu copy_ms=%.3f encode_ms=%.3f addptr_ms=%.3f rpc_ms=%.3f record_ms=%.3f shadow_ms=%.3f total_ms=%.3f",
          (unsigned long long)flush_seq,
          memoryRangeCount,
          (unsigned long long)total_bytes,
          copy_ms, encode_ms, addptr_ms, rpc_ms, record_ms, shadow_ms, total_ms);
    
    if (local_pMemoryRanges) free(local_pMemoryRanges);
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult InvalidateMappedMemoryRanges(VkDevice device,
                                                   uint32_t memoryRangeCount,
                                                   const VkMappedMemoryRange* pMemoryRanges) {
    struct timespec t0_start, t1_copy, t2_param, t3_addptr, t4_rpc, t5_update, t6_done;
    clock_gettime(CLOCK_MONOTONIC, &t0_start);

    FlushPendingSubmitCohort("invalidate");
    if (kEnableDeferredFenceWait) {
        DrainDeferredQueues("invalidate");
    }
    
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    Allocator vkAllocator;

    uint64_t inval_seq = ++g_sync_trace_seq;

    VkMappedMemoryRange* local_ranges = nullptr;
    if (memoryRangeCount > 0) {
        local_ranges = (VkMappedMemoryRange*)malloc(sizeof(VkMappedMemoryRange) * memoryRangeCount);
        if (!local_ranges) {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        for (uint32_t i = 0; i < memoryRangeCount; ++i) {
            deepcopy_VkMappedMemoryRange(&vkAllocator, VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                                       &pMemoryRanges[i], &local_ranges[i]);
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1_copy);

    uint32_t skipped_synced_ranges = 0;
    if (local_ranges && memoryRangeCount > 0) {
        std::vector<VkMappedMemoryRange> filtered_ranges;
        filtered_ranges.reserve(memoryRangeCount);
        {
            std::lock_guard<std::mutex> lock(g_mapped_mutex);
            for (uint32_t i = 0; i < memoryRangeCount; ++i) {
                if (IsMappedRangeRecentlyInvalidatedLocked(local_ranges[i])) {
                    skipped_synced_ranges++;
                    continue;
                }
                filtered_ranges.push_back(local_ranges[i]);
            }
        }

        if (filtered_ranges.empty()) {
            GUEST_MEM_TRACE("[GUEST_MEM_TRACE] invalidate_skip seq=%llu reason=%s device=0x%llx original_ranges=%u skipped_synced=%u",
                            (unsigned long long)inval_seq,
                            g_current_invalidate_reason ? g_current_invalidate_reason : "explicit_or_unknown",
                            (unsigned long long)(uintptr_t)device,
                            memoryRangeCount,
                            skipped_synced_ranges);
            if (kEnableImplicitSyncDiagLog) {
                ALOGD("[SYNC_GUEST] invalidate_skip_already_synced seq=%llu device=%llx ranges=%u",
                      (unsigned long long)inval_seq,
                      (unsigned long long)(uintptr_t)device,
                      memoryRangeCount);
            }
            struct timespec t_skip_done;
            clock_gettime(CLOCK_MONOTONIC, &t_skip_done);
            NoteMappedInvalidateStats(0,
                                      0,
                                      skipped_synced_ranges,
                                      ExpressVkElapsedUs(t0_start, t1_copy),
                                      0,
                                      0,
                                      0,
                                      0,
                                      ExpressVkElapsedUs(t0_start, t_skip_done));
            free(local_ranges);
            return VK_SUCCESS;
        }

        if (filtered_ranges.size() != memoryRangeCount) {
            memoryRangeCount = (uint32_t)filtered_ranges.size();
            memcpy(local_ranges,
                   filtered_ranges.data(),
                   sizeof(VkMappedMemoryRange) * filtered_ranges.size());
        }
    }

    size_t count = 8 + 4;
    char* stream = (char*)mgr.addExternalParamPtr(count);
    uint8_t** ptr = (uint8_t**)&stream;

    uint64_t guest_device = (uint64_t)(uintptr_t)device;
    memcpy(*ptr, &guest_device, 8);
    *ptr += 8;

    memcpy(*ptr, &memoryRangeCount, 4);
    *ptr += 4;
    clock_gettime(CLOCK_MONOTONIC, &t2_param);

    if (memoryRangeCount > 0) {
        mgr.addPtr(local_ranges, memoryRangeCount * sizeof(VkMappedMemoryRange));
    }

    uint64_t total_bytes = 0;
    for (uint32_t i = 0; i < memoryRangeCount; ++i) {
        VkDeviceMemory_T* mem = (VkDeviceMemory_T*)(local_ranges[i].memory);
        if (!mem || !mem->map_data || mem->length == 0) {
            continue;
        }

        VkDeviceSize offset_bytes = local_ranges[i].offset;
        if (offset_bytes > mem->length) {
            offset_bytes = mem->length;
        }

        VkDeviceSize size_bytes = local_ranges[i].size;
        if (size_bytes == VK_WHOLE_SIZE ||
            offset_bytes + size_bytes > mem->length ||
            offset_bytes + size_bytes < offset_bytes) {
            size_bytes = mem->length - offset_bytes;
        }

        total_bytes += (uint64_t)size_bytes;

        if (!mem->express_vk_mem_registered) {
            mgr.addPtr(mem->map_data + offset_bytes, (size_t)size_bytes);
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t3_addptr);

    GUEST_MEM_TRACE("[GUEST_MEM_TRACE] invalidate seq=%llu reason=%s device=0x%llx ranges=%u bytes=%llu skipped_synced=%u",
                    (unsigned long long)inval_seq,
                    g_current_invalidate_reason ? g_current_invalidate_reason : "explicit_or_unknown",
                    (unsigned long long)(uintptr_t)device,
                    memoryRangeCount,
                    (unsigned long long)total_bytes,
                    skipped_synced_ranges);
    for (uint32_t i = 0; i < memoryRangeCount && i < kGuestMemTraceMaxRanges; ++i) {
        VkDeviceMemory_T* mem = (VkDeviceMemory_T*)(local_ranges[i].memory);
        VkDeviceSize offset_bytes = local_ranges[i].offset;
        VkDeviceSize size_bytes = local_ranges[i].size;
        VkDeviceSize alloc_size = mem ? mem->length : 0;
        if (mem && alloc_size != 0) {
            if (offset_bytes > alloc_size) {
                offset_bytes = alloc_size;
            }
            if (size_bytes == VK_WHOLE_SIZE ||
                offset_bytes + size_bytes > alloc_size ||
                offset_bytes + size_bytes < offset_bytes) {
                size_bytes = alloc_size - offset_bytes;
            }
        }
        GUEST_MEM_TRACE("[GUEST_MEM_TRACE] invalidate_range seq=%llu index=%u memory=0x%llx offset=0x%llx size=%llu alloc_size=%llu registered=%d ptr=%p",
                        (unsigned long long)inval_seq,
                        i,
                        (unsigned long long)(uintptr_t)local_ranges[i].memory,
                        (unsigned long long)offset_bytes,
                        (unsigned long long)size_bytes,
                        (unsigned long long)alloc_size,
                        (mem && mem->express_vk_mem_registered) ? 1 : 0,
                        mem ? mem->map_data : nullptr);
    }

    {
        std::lock_guard<std::mutex> lock(g_mapped_mutex);
        MemShapeNoteMappedRangesLocked(memoryRangeCount,
                                       local_ranges,
                                       MemShapeRangeKind::kInvalidate);
    }

    EVK_PER_CALL_LOG("[SYNC_GUEST] invalidate seq=%llu device=%llx ranges=%u bytes=%llu",
                     (unsigned long long)inval_seq,
                     (unsigned long long)(uintptr_t)device,
                     memoryRangeCount,
                     (unsigned long long)total_bytes);
    
    VkResult vkResult = VK_SUCCESS;
    mgr.addPtr(&vkResult, sizeof(VkResult));

    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkInvalidateMappedMemoryRanges, true);
    clock_gettime(CLOCK_MONOTONIC, &t4_rpc);

    if (vkResult == VK_SUCCESS) {
        if (kEnableReadbackChangeSampling) {
            SampleInvalidateChanges(device,
                                    memoryRangeCount,
                                    local_ranges,
                                    g_current_invalidate_reason);
        }
        RecordRecentlyInvalidatedRanges(memoryRangeCount, local_ranges);
        UpdateShadowWithRanges(device, memoryRangeCount, local_ranges);
    }
    clock_gettime(CLOCK_MONOTONIC, &t5_update);

    clock_gettime(CLOCK_MONOTONIC, &t6_done);

    double copy_ms = (t1_copy.tv_sec - t0_start.tv_sec) * 1000.0 + (t1_copy.tv_nsec - t0_start.tv_nsec) / 1000000.0;
    double param_ms = (t2_param.tv_sec - t1_copy.tv_sec) * 1000.0 + (t2_param.tv_nsec - t1_copy.tv_nsec) / 1000000.0;
    double addptr_ms = (t3_addptr.tv_sec - t2_param.tv_sec) * 1000.0 + (t3_addptr.tv_nsec - t2_param.tv_nsec) / 1000000.0;
    double rpc_ms = (t4_rpc.tv_sec - t3_addptr.tv_sec) * 1000.0 + (t4_rpc.tv_nsec - t3_addptr.tv_nsec) / 1000000.0;
    double update_ms = (t5_update.tv_sec - t4_rpc.tv_sec) * 1000.0 + (t5_update.tv_nsec - t4_rpc.tv_nsec) / 1000000.0;
    double total_ms = (t6_done.tv_sec - t0_start.tv_sec) * 1000.0 + (t6_done.tv_nsec - t0_start.tv_nsec) / 1000000.0;

    NoteMappedInvalidateStats(memoryRangeCount,
                              total_bytes,
                              skipped_synced_ranges,
                              ExpressVkElapsedUs(t0_start, t1_copy),
                              ExpressVkElapsedUs(t1_copy, t2_param),
                              ExpressVkElapsedUs(t2_param, t3_addptr),
                              ExpressVkElapsedUs(t3_addptr, t4_rpc),
                              ExpressVkElapsedUs(t4_rpc, t5_update),
                              ExpressVkElapsedUs(t0_start, t6_done));
    
    EVK_PER_CALL_LOG("[PERF_Invalidate] seq=%llu ranges=%u bytes=%llu skipped_synced=%u copy_ms=%.3f param_ms=%.3f addptr_ms=%.3f rpc_ms=%.3f update_ms=%.3f total_ms=%.3f result=%d",
                     (unsigned long long)inval_seq,
                     memoryRangeCount,
                     (unsigned long long)total_bytes,
                     skipped_synced_ranges,
                     copy_ms, param_ms, addptr_ms, rpc_ms, update_ms, total_ms,
                     (int)vkResult);

    if (local_ranges) free(local_ranges);
    
    return vkResult;
}

VKAPI_ATTR void GetDeviceMemoryCommitment(VkDevice device,
                                         VkDeviceMemory memory,
                                         VkDeviceSize* pCommittedMemoryInBytes) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    mgr.addParam64((uint64_t)(uintptr_t)device);
    mgr.addParam64((uint64_t)(uintptr_t)memory);
    
    mgr.addPtr(pCommittedMemoryInBytes, sizeof(VkDeviceSize));
    
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkGetDeviceMemoryCommitment, true);
    ALOGI("GetDeviceMemoryCommitment get returned device memory commitment: %lld bytes",
          (long long)*pCommittedMemoryInBytes);
}

VKAPI_ATTR VkResult VKAPI_CALL BindBufferMemory(
    VkDevice       device,
    VkBuffer       buffer,
    VkDeviceMemory memory,
    VkDeviceSize   memoryOffset)
{
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;

    mgr.addParam64((uint64_t)(uintptr_t)device);
    mgr.addParam64((uint64_t)(uintptr_t)buffer);
    mgr.addParam64((uint64_t)(uintptr_t)memory);
    mgr.addParam64(memoryOffset);

    VkResult res = VK_SUCCESS;
    FlimeGuestWrite(&mgr,
        express_gpu,
        EXPRESS_GPU_DEVICE_ID,
        FUNID_vkBindBufferMemory,
        false);

    TrackBufferMemoryBinding(device, buffer, memory, memoryOffset);

    return res;
}


VkResult BindImageMemory(VkDevice device, VkImage image, VkDeviceMemory mem, VkDeviceSize memOffset) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    mgr.addParam64((uint64_t)(uintptr_t)device);
    mgr.addParam64((uint64_t)(uintptr_t)image);
    mgr.addParam64((uint64_t)(uintptr_t)mem);
    mgr.addParam64(memOffset);
    VkResult res = VK_SUCCESS;
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkBindImageMemory, false);
    ALOGI("vkBindImageMemory device=%lld image=%lld memory=%lld offset=%lld",
          (long long)device, (long long)image, (long long)mem, (long long)memOffset);
    return res;
}

VKAPI_ATTR void GetImageSparseMemoryRequirements(VkDevice device,
                                                 VkImage image,
                                                 uint32_t* pSparseMemoryRequirementCount,
                                                 VkSparseImageMemoryRequirements* pSparseMemoryRequirements) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    mgr.addParam64((uint64_t)(uintptr_t)device);
    mgr.addParam64((uint64_t)(uintptr_t)image);
    mgr.addPtr(pSparseMemoryRequirementCount, sizeof(uint32_t));
    
    if (pSparseMemoryRequirements) {
        mgr.addPtr(pSparseMemoryRequirements, sizeof(VkSparseImageMemoryRequirements) * (*pSparseMemoryRequirementCount));
    }
    
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkGetImageSparseMemoryRequirements, true);
}

VKAPI_ATTR void GetPhysicalDeviceSparseImageFormatProperties(VkPhysicalDevice physicalDevice,
                                                           VkFormat format,
                                                           VkImageType type,
                                                           VkSampleCountFlagBits samples,
                                                           VkImageUsageFlags usage,
                                                           VkImageTiling tiling,
                                                           uint32_t* pPropertyCount,
                                                           VkSparseImageFormatProperties* pProperties) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    mgr.addParam64((uint64_t)(uintptr_t)physicalDevice);
    mgr.addParam32((uint32_t)format);
    mgr.addParam32((uint32_t)type);
    mgr.addParam32((uint32_t)samples);
    mgr.addParam32((uint32_t)usage);
    mgr.addParam32((uint32_t)tiling);
    mgr.addPtr(pPropertyCount, sizeof(uint32_t));
    
    if (pProperties) {
        mgr.addPtr(pProperties, sizeof(VkSparseImageFormatProperties) * (*pPropertyCount));
    }
    
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkGetPhysicalDeviceSparseImageFormatProperties, true);
}

void GetPhysicalDeviceSparseImageFormatProperties2KHR(VkPhysicalDevice physicalDevice,
                                                      VkPhysicalDeviceSparseImageFormatInfo2KHR const* pInfo,
                                                      unsigned int* pNumProperties,
                                                      VkSparseImageFormatProperties2KHR* pProperties) {
    ALOGV("TODO: vk%s", __FUNCTION__);
}


VKAPI_ATTR VkResult QueueBindSparse(VkQueue queue,
                                   uint32_t bindInfoCount,
                                   const VkBindSparseInfo* pBindInfo,
                                   VkFence fence) {
    FlushPendingSubmitCohortForQueue(queue, "queue_bind_sparse");

    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    size_t count = 0;
    size_t* countPtr = &count;
    
    // Calculate total size needed
    for (uint32_t i = 0; i < bindInfoCount; ++i) {
        count_VkBindSparseInfo(0, VK_STRUCTURE_TYPE_MAX_ENUM, &pBindInfo[i], countPtr);
    }
    count += sizeof(uint64_t) + sizeof(uint32_t) + sizeof(uint64_t); // queue + bindInfoCount + fence
    
    char* send_buffer = (char*)mgr.addExternalParamPtr(count);
    uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;
    
    // Encode queue
    uint64_t guest_queue = (uint64_t)(uintptr_t)queue;
    memcpy(*send_buffer_ptr, &guest_queue, sizeof(uint64_t));
    *send_buffer_ptr += sizeof(uint64_t);
    
    // Encode bindInfoCount
    memcpy(*send_buffer_ptr, &bindInfoCount, sizeof(uint32_t));
    *send_buffer_ptr += sizeof(uint32_t);
    
    // Encode bind infos
    for (uint32_t i = 0; i < bindInfoCount; ++i) {
        encode_to_stream_VkBindSparseInfo(VK_STRUCTURE_TYPE_MAX_ENUM, &pBindInfo[i], send_buffer_ptr);
    }
    
    // Encode fence
    uint64_t guest_fence = (uint64_t)(uintptr_t)fence;
    memcpy(*send_buffer_ptr, &guest_fence, sizeof(uint64_t));
    *send_buffer_ptr += sizeof(uint64_t);
    
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkQueueBindSparse, false);
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult ResetFences(VkDevice device,
                                  uint32_t fenceCount,
                                  const VkFence* pFences) {
    std::vector<VkFence> host_reset_fences;
    const VkFence* host_pFences = pFences;
    uint32_t host_fenceCount = fenceCount;

    if (kEnableDeferredFenceWait && pFences && fenceCount > 0) {
        bool deferred_with_readback = false;
        {
            std::lock_guard<std::mutex> lock(g_mapped_mutex);
            for (uint32_t i = 0; i < fenceCount; ++i) {
                if (g_deferred_fence_queues.find(pFences[i]) !=
                        g_deferred_fence_queues.end() &&
                    g_fence_pending_invalidate_ranges.find(pFences[i]) !=
                        g_fence_pending_invalidate_ranges.end()) {
                    deferred_with_readback = true;
                    break;
                }
            }
        }
        if (deferred_with_readback) {
            DrainDeferredQueues("reset_fences_readback");
        }
    }

    if (kEnableDeferredFenceWait && pFences && fenceCount > 0) {
        std::lock_guard<std::mutex> lock(g_mapped_mutex);
        host_reset_fences.reserve(fenceCount);
        for (uint32_t i = 0; i < fenceCount; ++i) {
            g_fence_pending_invalidate_ranges.erase(pFences[i]);
            g_completed_deferred_fences.erase(pFences[i]);
            auto deferred_it = g_deferred_fence_queues.find(pFences[i]);
            if (deferred_it != g_deferred_fence_queues.end()) {
                VkQueue queue = deferred_it->second;
                g_deferred_fence_queues.erase(deferred_it);
                PruneDeferredQueueLocked(queue);
                continue;
            }
            host_reset_fences.push_back(pFences[i]);
        }
        host_pFences = host_reset_fences.data();
        host_fenceCount = (uint32_t)host_reset_fences.size();
    }

    if (host_fenceCount == 0) {
        ALOGD("vkResetFences deferred-only device=%lld fenceCount=%d",
              (long long)(uintptr_t)device, fenceCount);
        return VK_SUCCESS;
    }

    FlushPendingSubmitCohort("reset_fences");

    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    uint64_t guest_device = (uint64_t)(uintptr_t)device;
    
    mgr.addParam64(guest_device);
    mgr.addParam32(host_fenceCount);
    
    if (host_pFences && host_fenceCount > 0) {
        uint64_t* guest_fences = (uint64_t*)mgr.addExternalParamPtr(host_fenceCount * sizeof(uint64_t));
        for (uint32_t i = 0; i < host_fenceCount; ++i) {
            guest_fences[i] = (uint64_t)(uintptr_t)host_pFences[i];
        }
    }
    
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkResetFences, false);
    if (pFences && fenceCount > 0) {
        std::lock_guard<std::mutex> lock(g_mapped_mutex);
        for (uint32_t i = 0; i < fenceCount; ++i) {
            g_fence_pending_invalidate_ranges.erase(pFences[i]);
            g_completed_deferred_fences.erase(pFences[i]);
            auto deferred_it = g_deferred_fence_queues.find(pFences[i]);
            if (deferred_it != g_deferred_fence_queues.end()) {
                VkQueue queue = deferred_it->second;
                g_deferred_fence_queues.erase(deferred_it);
                PruneDeferredQueueLocked(queue);
            }
        }
    }
    
    ALOGI("vkResetFences device=%lld fenceCount=%d hostFenceCount=%d",
          (long long)guest_device, fenceCount, host_fenceCount);
    
    return VK_SUCCESS;
}

static bool CollectPendingInvalidateRangesForFences(uint32_t fenceCount,
                                                    const VkFence* pFences,
                                                    VkBool32 waitAll,
                                                    std::vector<TrackedMemoryRange>* out_ranges);
static VkResult DrainDeferredFenceReadback(VkDevice device,
                                           uint32_t fenceCount,
                                           const VkFence* pFences,
                                           VkBool32 waitAll,
                                           const std::vector<TrackedMemoryRange>& ranges);

VKAPI_ATTR VkResult GetFenceStatus(VkDevice device, VkFence fence) {
    if (kEnableDeferredFenceWait) {
        bool deferred = false;
        bool deferred_with_readback = false;
        {
            std::lock_guard<std::mutex> lock(g_mapped_mutex);
            deferred = IsFenceWaitDeferredLocked(fence);
            deferred_with_readback =
                deferred &&
                g_fence_pending_invalidate_ranges.find(fence) !=
                    g_fence_pending_invalidate_ranges.end();
        }
        if (deferred_with_readback) {
            std::vector<TrackedMemoryRange> ranges;
            CollectPendingInvalidateRangesForFences(1, &fence, VK_TRUE, &ranges);
            return DrainDeferredFenceReadback(device, 1, &fence, VK_TRUE, ranges);
        }
        if (deferred) {
            if (UseConservativeMappedReadbackFallback()) {
                const uint64_t start_us = ExpressVkNowUs();
                DrainDeferredQueues("get_fence_status_conservative");
                uint64_t invalidate_bytes = 0;
                uint64_t skipped_synced = 0;
                const uint64_t invalidate_us = InvalidateAllMappedMemoriesForReadback(
                    "get_fence_status_conservative",
                    &invalidate_bytes,
                    &skipped_synced);
                const uint64_t total_us = ExpressVkNowUs() - start_us;
                if (total_us >= kSlowWaitDiagLogUs || invalidate_us >= kSlowWaitDiagLogUs) {
                    ALOGI("[WAIT_CONSERVATIVE_READBACK] path=get_fence_status fence=%llx invalidate_bytes=%llu skipped_synced=%llu invalidate_us=%llu total_us=%llu",
                          (unsigned long long)(uintptr_t)fence,
                          (unsigned long long)invalidate_bytes,
                          (unsigned long long)skipped_synced,
                          (unsigned long long)invalidate_us,
                          (unsigned long long)total_us);
                }
            }
            return VK_SUCCESS;
        }
    }

    FlushPendingSubmitCohort("get_fence_status");

    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    mgr.addParam64((uint64_t)(uintptr_t)device);
    mgr.addParam64((uint64_t)(uintptr_t)fence);
    
    VkResult vkResult = VK_SUCCESS;
    mgr.addPtr(&vkResult, sizeof(VkResult));
    
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkGetFenceStatus, true);
    ALOGI("GetFenceStatus device=%lld fence=%lld result=%d", 
          (long long)device, (long long)fence, vkResult);
    
    return vkResult;
}


static bool CollectPendingInvalidateRangesForFences(uint32_t fenceCount,
                                                    const VkFence* pFences,
                                                    VkBool32 waitAll,
                                                    std::vector<TrackedMemoryRange>* out_ranges) {
    if (!out_ranges || !pFences || fenceCount == 0) return false;
    if (!waitAll && fenceCount != 1) return false;

    std::lock_guard<std::mutex> lock(g_mapped_mutex);
    for (uint32_t i = 0; i < fenceCount; ++i) {
        auto it = g_fence_pending_invalidate_ranges.find(pFences[i]);
        if (it == g_fence_pending_invalidate_ranges.end()) continue;
        for (const TrackedMemoryRange& range : it->second) {
            AppendTrackedRange(out_ranges, range);
        }
    }
    CanonicalizeTrackedRanges(out_ranges);
    return !out_ranges->empty();
}

static bool CollectDeferredQueuesForFences(uint32_t fenceCount,
                                           const VkFence* pFences,
                                           std::vector<VkQueue>* out_queues) {
    if (!out_queues || !pFences || fenceCount == 0) return false;

    std::lock_guard<std::mutex> lock(g_mapped_mutex);
    for (uint32_t i = 0; i < fenceCount; ++i) {
        auto it = g_deferred_fence_queues.find(pFences[i]);
        if (it == g_deferred_fence_queues.end()) continue;

        VkQueue queue = it->second;
        if (queue == VK_NULL_HANDLE) continue;
        if (std::find(out_queues->begin(), out_queues->end(), queue) != out_queues->end()) {
            continue;
        }
        out_queues->push_back(queue);
    }
    return !out_queues->empty();
}

static uint64_t InvalidateTrackedMemoryRangesForReadback(
    const std::vector<TrackedMemoryRange>& ranges,
    const char* reason,
    uint64_t* out_bytes,
    uint64_t* out_skipped_synced = nullptr,
    bool force_refresh = false) {
    if (out_bytes) *out_bytes = 0;
    if (out_skipped_synced) *out_skipped_synced = 0;
    if (ranges.empty()) return 0;

    std::unordered_map<VkDevice, std::vector<VkMappedMemoryRange>> ranges_by_dev;
    uint64_t bytes = 0;
    size_t skipped = 0;
    size_t skipped_synced = 0;

    {
        std::lock_guard<std::mutex> lock(g_mapped_mutex);
        for (const TrackedMemoryRange& range : ranges) {
            if (range.memory == VK_NULL_HANDLE || range.size == 0) continue;
            auto it = g_active_mapped_memories.find(range.memory);
            if (it == g_active_mapped_memories.end()) {
                skipped++;
                continue;
            }

            ActiveMappedMemoryRecord& rec = it->second;
            const VkDeviceSize size =
                ClampMappedRangeSize(rec.size, range.offset, range.size);
            if (!rec.map_data || size == 0) {
                skipped++;
                continue;
            }
            if (!force_refresh &&
                RangeCoveredByGeneration(rec.recently_invalidated_ranges,
                                         range.offset,
                                         size,
                                         rec.last_submit_generation)) {
                skipped_synced++;
                continue;
            }

            VkMappedMemoryRange mapped = {};
            mapped.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
            mapped.pNext = nullptr;
            mapped.memory = rec.memory;
            mapped.offset = range.offset;
            mapped.size = size;
            ranges_by_dev[rec.device].push_back(mapped);
            bytes += size;
        }
    }

    const uint64_t start_us = ExpressVkNowUs();
    for (const auto& pair : ranges_by_dev) {
        const char* previous_reason = g_current_invalidate_reason;
        g_current_invalidate_reason = reason;
        InvalidateMappedMemoryRanges(pair.first,
                                     (uint32_t)pair.second.size(),
                                     pair.second.data());
        g_current_invalidate_reason = previous_reason;
    }
    const uint64_t elapsed_us = ExpressVkNowUs() - start_us;
    if (out_bytes) *out_bytes = bytes;
    if (out_skipped_synced) *out_skipped_synced = skipped_synced;

    if (kEnableImplicitSyncDiagLog && kEnableReadbackRangeSampleLog && force_refresh) {
        size_t logged = 0;
        std::lock_guard<std::mutex> lock(g_mapped_mutex);
        for (const auto& pair : ranges_by_dev) {
            for (const VkMappedMemoryRange& mapped : pair.second) {
                if (mapped.size < kReadbackRangeSampleMinBytes || logged >= 8) {
                    continue;
                }
                auto it = g_active_mapped_memories.find(mapped.memory);
                if (it == g_active_mapped_memories.end()) {
                    continue;
                }
                const ActiveMappedMemoryRecord& rec = it->second;
                if (!rec.map_data || mapped.offset >= rec.size) {
                    continue;
                }
                const VkDeviceSize available = rec.size - mapped.offset;
                const size_t sample_bytes =
                    (size_t)std::min<VkDeviceSize>(available, 4 * sizeof(float));
                float f0 = 0.0f;
                float f1 = 0.0f;
                float f2 = 0.0f;
                float f3 = 0.0f;
                const uint8_t* sample = rec.map_data + mapped.offset;
                if (sample_bytes >= sizeof(float)) {
                    memcpy(&f0, sample, sizeof(float));
                }
                if (sample_bytes >= 2 * sizeof(float)) {
                    memcpy(&f1, sample + sizeof(float), sizeof(float));
                }
                if (sample_bytes >= 3 * sizeof(float)) {
                    memcpy(&f2, sample + 2 * sizeof(float), sizeof(float));
                }
                if (sample_bytes >= 4 * sizeof(float)) {
                    memcpy(&f3, sample + 3 * sizeof(float), sizeof(float));
                }
                ALOGI("[SYNC_GUEST] readback_sample reason=%s memory=%llx offset=%llu size=%llu f32=[%.6f,%.6f,%.6f,%.6f]",
                      reason ? reason : "unknown",
                      (unsigned long long)(uintptr_t)mapped.memory,
                      (unsigned long long)mapped.offset,
                      (unsigned long long)mapped.size,
                      (double)f0,
                      (double)f1,
                      (double)f2,
                      (double)f3);
                logged++;
            }
        }
    }

    if (kEnableImplicitSyncDiagLog &&
        (bytes != 0 || skipped != 0 || skipped_synced != 0)) {
        EVK_PER_CALL_LOG("[SYNC_GUEST] deferred_readback_invalidate reason=%s devs=%zu ranges=%zu bytes=%llu skipped=%zu skipped_synced=%zu us=%llu",
                         reason ? reason : "unknown",
                         ranges_by_dev.size(),
                         ranges.size(),
                         (unsigned long long)bytes,
                         skipped,
                         skipped_synced,
                         (unsigned long long)elapsed_us);
    }
    return elapsed_us;
}

static uint64_t PreserveGuestDirtyRangesBeforeReadback(
    const char* reason,
    uint64_t* out_bytes,
    uint64_t* out_ranges) {
    if (out_bytes) *out_bytes = 0;
    if (out_ranges) *out_ranges = 0;

    std::unordered_map<VkDevice, std::vector<VkMappedMemoryRange>> ranges_by_dev;
    uint64_t dirty_bytes = 0;
    uint64_t dirty_ranges = 0;

    {
        std::lock_guard<std::mutex> lock(g_mapped_mutex);
        for (auto& pair : g_active_mapped_memories) {
            ActiveMappedMemoryRecord& rec = pair.second;
            if (!rec.map_data || rec.memory == VK_NULL_HANDLE || rec.size == 0) {
                continue;
            }

            if (rec.shadow.size() != rec.size) {
                rec.shadow.resize((size_t)rec.size);
                memcpy(rec.shadow.data(), rec.map_data, (size_t)rec.size);

                VkMappedMemoryRange r = {};
                r.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
                r.pNext = nullptr;
                r.memory = rec.memory;
                r.offset = 0;
                r.size = rec.size;
                ranges_by_dev[rec.device].push_back(r);
                dirty_bytes += (uint64_t)rec.size;
                dirty_ranges++;
                continue;
            }

            VkDeviceSize range_start = 0;
            VkDeviceSize range_end = 0;
            bool in_range = false;

            for (VkDeviceSize off = 0; off < rec.size; off += kImplicitDirtyChunkBytes) {
                const VkDeviceSize chunk =
                    std::min<VkDeviceSize>(kImplicitDirtyChunkBytes, rec.size - off);
                if (memcmp(rec.map_data + off, rec.shadow.data() + off, (size_t)chunk) != 0) {
                    memcpy(rec.shadow.data() + off, rec.map_data + off, (size_t)chunk);
                    EraseMemoryRangeSpanOverlaps(&rec.recently_clean_submit_ranges, off, chunk);
                    EraseMemoryRangeSpanOverlaps(&rec.recently_invalidated_ranges, off, chunk);
                    rec.submit_clean_streak = 0;
                    if (!in_range) {
                        range_start = off;
                        range_end = off + chunk;
                        in_range = true;
                    } else {
                        range_end = off + chunk;
                    }
                } else if (in_range) {
                    VkMappedMemoryRange r = {};
                    r.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
                    r.pNext = nullptr;
                    r.memory = rec.memory;
                    r.offset = range_start;
                    r.size = range_end - range_start;
                    ranges_by_dev[rec.device].push_back(r);
                    dirty_bytes += (uint64_t)r.size;
                    dirty_ranges++;
                    in_range = false;
                }
            }

            if (in_range) {
                VkMappedMemoryRange r = {};
                r.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
                r.pNext = nullptr;
                r.memory = rec.memory;
                r.offset = range_start;
                r.size = range_end - range_start;
                ranges_by_dev[rec.device].push_back(r);
                dirty_bytes += (uint64_t)r.size;
                dirty_ranges++;
            }
        }
    }

    const uint64_t start_us = ExpressVkNowUs();
    for (const auto& pair : ranges_by_dev) {
        FlushMappedMemoryRanges(pair.first,
                                (uint32_t)pair.second.size(),
                                pair.second.data());
    }
    const uint64_t elapsed_us = ExpressVkNowUs() - start_us;

    if (out_bytes) *out_bytes = dirty_bytes;
    if (out_ranges) *out_ranges = dirty_ranges;
    if (kEnableImplicitSyncDiagLog && dirty_ranges != 0) {
        ALOGI("[SYNC_GUEST] preserve_guest_dirty_before_readback reason=%s ranges=%llu bytes=%llu us=%llu",
              reason ? reason : "unknown",
              (unsigned long long)dirty_ranges,
              (unsigned long long)dirty_bytes,
              (unsigned long long)elapsed_us);
    }
    return elapsed_us;
}

static uint64_t InvalidateAllMappedMemoriesForReadback(
    const char* reason,
    uint64_t* out_bytes,
    uint64_t* out_skipped_synced) {
    uint64_t preserved_bytes = 0;
    uint64_t preserved_ranges = 0;
    const uint64_t preserve_us = PreserveGuestDirtyRangesBeforeReadback(
        reason,
        &preserved_bytes,
        &preserved_ranges);

    std::vector<TrackedMemoryRange> ranges;
    {
        std::lock_guard<std::mutex> lock(g_mapped_mutex);
        ranges.reserve(g_active_mapped_memories.size());
        for (const auto& pair : g_active_mapped_memories) {
            const ActiveMappedMemoryRecord& rec = pair.second;
            if (!rec.map_data || rec.memory == VK_NULL_HANDLE || rec.size == 0) {
                continue;
            }
            ranges.push_back({rec.device, rec.memory, 0, rec.size});
        }
    }

    uint64_t active_bytes = 0;
    for (const TrackedMemoryRange& r : ranges) {
        active_bytes += (uint64_t)r.size;
    }
    GUEST_MEM_TRACE("[GUEST_MEM_TRACE] conservative_readback_active reason=%s active_ranges=%zu active_bytes=%llu active_mb=%llu preserve_ranges=%llu preserve_bytes=%llu",
                    reason ? reason : "unknown",
                    ranges.size(),
                    (unsigned long long)active_bytes,
                    (unsigned long long)(active_bytes / (1024ull * 1024ull)),
                    (unsigned long long)preserved_ranges,
                    (unsigned long long)preserved_bytes);
    for (size_t i = 0; i < ranges.size() && i < kGuestMemTraceMaxRanges; ++i) {
        const TrackedMemoryRange& r = ranges[i];
        GUEST_MEM_TRACE("[GUEST_MEM_TRACE] conservative_readback_range reason=%s index=%zu device=0x%llx memory=0x%llx offset=0x%llx size=%llu size_mb=%llu",
                        reason ? reason : "unknown",
                        i,
                        (unsigned long long)(uintptr_t)r.device,
                        (unsigned long long)(uintptr_t)r.memory,
                        (unsigned long long)r.offset,
                        (unsigned long long)r.size,
                        (unsigned long long)((uint64_t)r.size / (1024ull * 1024ull)));
    }

    CanonicalizeTrackedRanges(&ranges);
    uint64_t bytes = 0;
    uint64_t skipped_synced = 0;
    const uint64_t elapsed_us = InvalidateTrackedMemoryRangesForReadback(
        ranges,
        reason,
        &bytes,
        &skipped_synced,
        true);
    if (out_bytes) *out_bytes = bytes;
    if (out_skipped_synced) *out_skipped_synced = skipped_synced;

    if (kEnableImplicitSyncDiagLog && (bytes != 0 || skipped_synced != 0)) {
        ALOGI("[SYNC_GUEST] conservative_readback reason=%s ranges=%zu bytes=%llu skipped_synced=%llu preserve_ranges=%llu preserve_bytes=%llu preserve_us=%llu invalidate_us=%llu total_us=%llu",
              reason ? reason : "unknown",
              ranges.size(),
              (unsigned long long)bytes,
              (unsigned long long)skipped_synced,
              (unsigned long long)preserved_ranges,
              (unsigned long long)preserved_bytes,
              (unsigned long long)preserve_us,
              (unsigned long long)elapsed_us,
              (unsigned long long)(preserve_us + elapsed_us));
    }
    NoteConservativeReadbackStats((uint64_t)ranges.size(),
                                  bytes,
                                  skipped_synced,
                                  preserved_bytes,
                                  preserve_us,
                                  elapsed_us,
                                  preserve_us + elapsed_us,
                                  reason);
    return preserve_us + elapsed_us;
}

static VkResult DrainDeferredFenceReadback(VkDevice device,
                                           uint32_t fenceCount,
                                           const VkFence* pFences,
                                           VkBool32 waitAll,
                                           const std::vector<TrackedMemoryRange>& ranges) {
    (void)device;
    if (ranges.empty()) {
        const uint64_t start_us = ExpressVkNowUs();
        DrainDeferredQueues("wait_fences_deferred_readback_no_ranges");
        uint64_t invalidate_us = 0;
        uint64_t invalidate_bytes = 0;
        uint64_t skipped_synced = 0;
        if (UseConservativeMappedReadbackFallback()) {
            invalidate_us = InvalidateAllMappedMemoriesForReadback(
                "wait_fences_conservative_no_hints",
                &invalidate_bytes,
                &skipped_synced);
        }
        const uint64_t total_us = ExpressVkNowUs() - start_us;
        if (total_us >= kSlowWaitDiagLogUs) {
            ALOGI("[WAIT_SLOW_DEFERRED_READBACK] no_ranges=1 fenceCount=%u waitAll=%d invalidate_bytes=%llu skipped_synced=%llu invalidate_us=%llu total_us=%llu fallback=1 conservative=%d",
                  fenceCount,
                  waitAll,
                  (unsigned long long)invalidate_bytes,
                  (unsigned long long)skipped_synced,
                  (unsigned long long)invalidate_us,
                  (unsigned long long)total_us,
                  (int)UseConservativeMappedReadbackFallback());
        }
        NoteDeferredReadbackDrainStats("wait_fences_deferred_readback_no_ranges",
                                       0,
                                       0,
                                       invalidate_bytes,
                                       total_us - invalidate_us,
                                       invalidate_us,
                                       total_us,
                                       true);
        return VK_SUCCESS;
    }

    std::vector<VkQueue> queues;
    const bool have_queues = CollectDeferredQueuesForFences(fenceCount,
                                                            pFences,
                                                            &queues);
    const uint64_t total_start_us = ExpressVkNowUs();
    uint64_t wait_us = 0;
    bool fallback = false;
    VkResult result = VK_SUCCESS;

    if (have_queues) {
        const uint64_t wait_start_us = ExpressVkNowUs();
        for (VkQueue queue : queues) {
            result = RawQueueWaitIdleForDeferred(queue, "wait_fences_deferred_readback");
            if (result != VK_SUCCESS) {
                break;
            }
        }
        wait_us = ExpressVkNowUs() - wait_start_us;
    } else {
        fallback = true;
        const uint64_t wait_start_us = ExpressVkNowUs();
        DrainDeferredQueues("wait_fences_deferred_readback_fallback");
        wait_us = ExpressVkNowUs() - wait_start_us;
    }

    uint64_t invalidate_us = 0;
    uint64_t invalidate_bytes = 0;
    if (result == VK_SUCCESS) {
        if (UseConservativeMappedReadbackFallback()) {
            invalidate_us = InvalidateAllMappedMemoriesForReadback(
                fallback ? "deferred_readback_conservative_fallback" :
                           "deferred_readback_conservative",
                &invalidate_bytes);
        } else {
            invalidate_us = InvalidateTrackedMemoryRangesForReadback(
                ranges,
                fallback ? "deferred_readback_fallback" : "deferred_readback",
                &invalidate_bytes);
        }
        MarkFenceInvalidateHintsCommitted(fenceCount, pFences, waitAll);
    }

    const uint64_t total_us = ExpressVkNowUs() - total_start_us;
    if (total_us >= kSlowWaitDiagLogUs || wait_us >= kSlowWaitDiagLogUs) {
        ALOGI("[WAIT_SLOW_DEFERRED_READBACK] no_ranges=0 fenceCount=%u waitAll=%d queues=%zu ranges=%zu request_bytes=%llu invalidate_bytes=%llu wait_us=%llu invalidate_us=%llu total_us=%llu fallback=%d conservative=%d result=%d",
              fenceCount,
              waitAll,
              queues.size(),
              ranges.size(),
              (unsigned long long)TotalTrackedRangeBytes(ranges),
              (unsigned long long)invalidate_bytes,
              (unsigned long long)wait_us,
              (unsigned long long)invalidate_us,
              (unsigned long long)total_us,
              (int)fallback,
              (int)UseConservativeMappedReadbackFallback(),
              (int)result);
    }
    NoteDeferredReadbackDrainStats("wait_fences_deferred_readback",
                                   (uint64_t)queues.size(),
                                   ranges.size(),
                                   invalidate_bytes,
                                   wait_us,
                                   invalidate_us,
                                   total_us,
                                   fallback);
    return result;
}

static VkResult ExpressWaitFenceAndInvalidate(VkDevice device,
                                               uint32_t fenceCount,
                                               const VkFence* pFences,
                                               VkBool32 waitAll,
                                               uint64_t timeout,
                                               const std::vector<TrackedMemoryRange>& ranges,
                                               uint64_t* out_wait_us,
                                               uint64_t* out_invalidate_us) {
    if (out_wait_us) *out_wait_us = 0;
    if (out_invalidate_us) *out_invalidate_us = 0;
    if (!pFences || fenceCount == 0 || ranges.empty()) return VK_NOT_READY;

    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;

    mgr.addParam64((uint64_t)(uintptr_t)device);
    mgr.addParam32(fenceCount);
    mgr.addParam32(waitAll);
    mgr.addParam64(timeout);
    mgr.addParam32((uint32_t)ranges.size());

    uint64_t* guest_fences = (uint64_t*)mgr.addExternalParamPtr(fenceCount * sizeof(uint64_t));
    for (uint32_t i = 0; i < fenceCount; ++i) {
        guest_fences[i] = (uint64_t)(uintptr_t)pFences[i];
    }

    struct ExpressWaitFenceInvalidateRangeWire {
        uint64_t memory;
        uint64_t offset;
        uint64_t size;
    };
    ExpressWaitFenceInvalidateRangeWire* wire_ranges =
        (ExpressWaitFenceInvalidateRangeWire*)mgr.addExternalParamPtr(
            ranges.size() * sizeof(ExpressWaitFenceInvalidateRangeWire));
    for (size_t i = 0; i < ranges.size(); ++i) {
        wire_ranges[i].memory = (uint64_t)(uintptr_t)ranges[i].memory;
        wire_ranges[i].offset = ranges[i].offset;
        wire_ranges[i].size = ranges[i].size;
    }

    VkResult vkResult = VK_SUCCESS;
    mgr.addPtr(&vkResult, sizeof(VkResult));
    uint64_t timing_us[2] = {0, 0};
    mgr.addPtr(timing_us, sizeof(timing_us));

    FlimeGuestWrite(&mgr, express_gpu,
              EXPRESS_GPU_DEVICE_ID,
              FUNID_vkExpressWaitFenceAndInvalidateANDROID,
              true);

    if (out_wait_us) *out_wait_us = timing_us[0];
    if (out_invalidate_us) *out_invalidate_us = timing_us[1];
    return vkResult;
}

VKAPI_ATTR VkResult WaitForFences(VkDevice device,
                                    uint32_t fenceCount,
                                    const VkFence* pFences,
                                    VkBool32 waitAll,
                                    uint64_t timeout) {
    struct timespec t0_start, t1_param, t2_rpc, t3_implicit, t4_done;
    clock_gettime(CLOCK_MONOTONIC, &t0_start);

    std::vector<VkFence> host_wait_fences;
    bool virtual_wait_satisfied = false;
    bool mixed_wait_all_needs_drain = false;
    if (kEnableDeferredFenceWait && pFences && fenceCount > 0) {
        std::lock_guard<std::mutex> lock(g_mapped_mutex);
        uint32_t deferred_count = 0;
        host_wait_fences.reserve(fenceCount);
        for (uint32_t i = 0; i < fenceCount; ++i) {
            if (IsFenceWaitDeferredLocked(pFences[i])) {
                deferred_count++;
            } else {
                host_wait_fences.push_back(pFences[i]);
            }
        }

        if ((waitAll && deferred_count == fenceCount) ||
            (!waitAll && deferred_count != 0)) {
            virtual_wait_satisfied = true;
        } else if (waitAll && deferred_count != 0) {
            mixed_wait_all_needs_drain = true;
        }
    }

    if (virtual_wait_satisfied) {
        FlushPendingSubmitCohort("wait_fences_deferred_virtual");
        size_t pending_queues = 0;
        bool has_readback = false;
        std::vector<TrackedMemoryRange> deferred_readback_ranges;
        {
            std::lock_guard<std::mutex> lock(g_mapped_mutex);
            pending_queues = g_deferred_wait_queues.size();
            for (uint32_t i = 0; pFences && i < fenceCount; ++i) {
                if (g_fence_pending_invalidate_ranges.find(pFences[i]) !=
                    g_fence_pending_invalidate_ranges.end()) {
                    has_readback = true;
                    break;
                }
            }
        }
        if (has_readback) {
            CollectPendingInvalidateRangesForFences(fenceCount,
                                                    pFences,
                                                    waitAll,
                                                    &deferred_readback_ranges);
        }
        NoteDeferredVirtualWaitStats(fenceCount,
                                     (uint64_t)pending_queues,
                                     has_readback);
        if (UseConservativeMappedReadbackFallback()) {
            const uint64_t start_us = ExpressVkNowUs();
            DrainDeferredQueues("wait_fences_conservative_virtual");
            uint64_t invalidate_bytes = 0;
            uint64_t skipped_synced = 0;
            const uint64_t invalidate_us = InvalidateAllMappedMemoriesForReadback(
                "wait_fences_conservative_virtual",
                &invalidate_bytes,
                &skipped_synced);
            if (has_readback) {
                MarkFenceInvalidateHintsCommitted(fenceCount, pFences, waitAll);
            }
            const uint64_t total_us = ExpressVkNowUs() - start_us;
            if (total_us >= kSlowWaitDiagLogUs || invalidate_us >= kSlowWaitDiagLogUs) {
                ALOGI("[WAIT_CONSERVATIVE_READBACK] path=virtual fenceCount=%u waitAll=%d pending_queues=%zu hinted=%d hint_ranges=%zu hint_bytes=%llu invalidate_bytes=%llu skipped_synced=%llu invalidate_us=%llu total_us=%llu",
                      fenceCount,
                      waitAll,
                      pending_queues,
                      (int)has_readback,
                      deferred_readback_ranges.size(),
                      (unsigned long long)TotalTrackedRangeBytes(deferred_readback_ranges),
                      (unsigned long long)invalidate_bytes,
                      (unsigned long long)skipped_synced,
                      (unsigned long long)invalidate_us,
                      (unsigned long long)total_us);
            }
            NoteDeferredReadbackDrainStats("wait_fences_conservative_virtual",
                                           (uint64_t)pending_queues,
                                           deferred_readback_ranges.size(),
                                           invalidate_bytes,
                                           total_us - invalidate_us,
                                           invalidate_us,
                                           total_us,
                                           true);
            return VK_SUCCESS;
        }
        if (has_readback) {
            VkResult readback_result = DrainDeferredFenceReadback(device,
                                                                  fenceCount,
                                                                  pFences,
                                                                  waitAll,
                                                                  deferred_readback_ranges);
            EVK_PER_CALL_LOG("[PERF_WaitForFences] deferred_readback fenceCount=%u waitAll=%d pending_queues=%zu ranges=%zu bytes=%llu result=%d",
                             fenceCount,
                             waitAll,
                             pending_queues,
                             deferred_readback_ranges.size(),
                             (unsigned long long)TotalTrackedRangeBytes(deferred_readback_ranges),
                             readback_result);
            return readback_result;
        }
        if (kEnableLocalPerfLog) {
            ALOGD("[PERF_WaitForFences] deferred_virtual fenceCount=%u waitAll=%d pending_queues=%zu readback_pending=%d result=%d",
                  fenceCount, waitAll, pending_queues, (int)has_readback, VK_SUCCESS);
        }
        return VK_SUCCESS;
    }

    if (mixed_wait_all_needs_drain) {
        DrainDeferredQueues("wait_fences_mixed_wait_all");
    }

    FlushPendingSubmitCohort("wait_fences");

    std::vector<TrackedMemoryRange> fused_invalidate_ranges;
    const bool has_fused_ranges =
        CollectPendingInvalidateRangesForFences(fenceCount,
                                                pFences,
                                                waitAll,
                                                &fused_invalidate_ranges);
    if (!has_fused_ranges) {
        NoteWaitInvalidateFused(false, 0, 0, 0, 0, 0, false, true, VK_SUCCESS);
    }

    if (has_fused_ranges && kEnableWaitInvalidateFused) {
        const uint64_t fused_start_us = ExpressVkNowUs();
        uint64_t fused_wait_us = 0;
        uint64_t fused_invalidate_us = 0;
        VkResult fused_result =
            ExpressWaitFenceAndInvalidate(device,
                                          fenceCount,
                                          pFences,
                                          waitAll,
                                          timeout,
                                          fused_invalidate_ranges,
                                          &fused_wait_us,
                                          &fused_invalidate_us);
        const uint64_t fused_total_us = ExpressVkNowUs() - fused_start_us;
        const uint64_t fused_bytes = TotalTrackedRangeBytes(fused_invalidate_ranges);
        NoteWaitInvalidateFused(true,
                                fused_invalidate_ranges.size(),
                                fused_bytes,
                                fused_wait_us,
                                fused_invalidate_us,
                                fused_total_us,
                                fused_result != VK_SUCCESS && fused_result != VK_TIMEOUT,
                                false,
                                fused_result);
        if (fused_result == VK_SUCCESS) {
            std::vector<VkMappedMemoryRange> committed_ranges;
            committed_ranges.reserve(fused_invalidate_ranges.size());
            for (const TrackedMemoryRange& range : fused_invalidate_ranges) {
                VkMappedMemoryRange mapped = {};
                mapped.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
                mapped.memory = range.memory;
                mapped.offset = range.offset;
                mapped.size = range.size;
                committed_ranges.push_back(mapped);
            }
            RecordRecentlyInvalidatedRanges((uint32_t)committed_ranges.size(),
                                            committed_ranges.data());
            MarkFenceInvalidateHintsCommitted(fenceCount, pFences, waitAll);
            clock_gettime(CLOCK_MONOTONIC, &t3_implicit);
            NoteDeferredRealWaitStats(fused_wait_us, 0);
            if (kEnableLocalPerfLog) {
                ALOGD("[PERF_WaitForFences] fused fenceCount=%u waitAll=%d ranges=%zu bytes=%llu wait_us=%llu invalidate_us=%llu total_us=%llu result=%d",
                      fenceCount,
                      waitAll,
                      fused_invalidate_ranges.size(),
                      (unsigned long long)fused_bytes,
                      (unsigned long long)fused_wait_us,
                      (unsigned long long)fused_invalidate_us,
                      (unsigned long long)fused_total_us,
                      fused_result);
            }
            return fused_result;
        }
        /* Timeout or unsupported/failure falls through to the old conservative path.
         * If timeout happened, old vkWaitForFences will also return timeout without
         * invalidating. If a host-side range lookup failed, the old path still commits
         * stored fence output hints. */
    } else if (has_fused_ranges) {
        NoteWaitInvalidateFused(false,
                                fused_invalidate_ranges.size(),
                                TotalTrackedRangeBytes(fused_invalidate_ranges),
                                0,
                                0,
                                0,
                                false,
                                false,
                                VK_SUCCESS);
    }

    const VkFence* host_pFences = pFences;
    uint32_t host_fenceCount = fenceCount;
    if (!host_wait_fences.empty() && host_wait_fences.size() != fenceCount) {
        host_pFences = host_wait_fences.data();
        host_fenceCount = (uint32_t)host_wait_fences.size();
    }
    
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    uint64_t guest_device = (uint64_t)(uintptr_t)device;
    
    mgr.addParam64(guest_device);
    mgr.addParam32(host_fenceCount);
    mgr.addParam32(waitAll);
    mgr.addParam64(timeout);
    
    if (host_pFences && host_fenceCount > 0) {
        uint64_t* guest_fences = (uint64_t*)mgr.addExternalParamPtr(host_fenceCount * sizeof(uint64_t));
        for (uint32_t i = 0; i < host_fenceCount; ++i) {
            guest_fences[i] = (uint64_t)(uintptr_t)host_pFences[i];
        }
    }
    VkResult vkResult = VK_SUCCESS;
    const bool async_wait_transport = false;
    if (!async_wait_transport) {
        mgr.addPtr(&vkResult, sizeof(VkResult));
    }
    
    clock_gettime(CLOCK_MONOTONIC, &t1_param);

    FlimeGuestWrite(&mgr, express_gpu,
              EXPRESS_GPU_DEVICE_ID,
              FUNID_vkWaitForFences,
              !async_wait_transport);
    clock_gettime(CLOCK_MONOTONIC, &t2_rpc);

    uint64_t targeted_invalidate_us = 0;
    uint64_t targeted_invalidate_bytes = 0;
    uint64_t targeted_skipped_synced = 0;
    if (kEnableImplicitGlobalMappedSync && vkResult == VK_SUCCESS) {
        if (UseConservativeMappedReadbackFallback()) {
            targeted_invalidate_us = InvalidateAllMappedMemoriesForReadback(
                has_fused_ranges ? "wait_fences_conservative_with_hints" :
                                   "wait_fences_conservative_no_hints",
                &targeted_invalidate_bytes,
                &targeted_skipped_synced);
            MarkFenceInvalidateHintsCommitted(fenceCount, pFences, waitAll);
        } else if (has_fused_ranges) {
            targeted_invalidate_us = InvalidateTrackedMemoryRangesForReadback(
                fused_invalidate_ranges,
                "wait_fences_targeted_readback",
                &targeted_invalidate_bytes,
                &targeted_skipped_synced);
            MarkFenceInvalidateHintsCommitted(fenceCount, pFences, waitAll);
        } else {
            ImplicitInvalidateAllMappedMemories();
            MarkFenceInvalidateHintsCommitted(host_fenceCount, host_pFences, waitAll);
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t3_implicit);
    
    double param_ms = (t1_param.tv_sec - t0_start.tv_sec) * 1000.0 + (t1_param.tv_nsec - t0_start.tv_nsec) / 1000000.0;
    double rpc_ms = (t2_rpc.tv_sec - t1_param.tv_sec) * 1000.0 + (t2_rpc.tv_nsec - t1_param.tv_nsec) / 1000000.0;
    double implicit_ms = (t3_implicit.tv_sec - t2_rpc.tv_sec) * 1000.0 + (t3_implicit.tv_nsec - t2_rpc.tv_nsec) / 1000000.0;
    double total_ms = (t3_implicit.tv_sec - t0_start.tv_sec) * 1000.0 + (t3_implicit.tv_nsec - t0_start.tv_nsec) / 1000000.0;
    size_t pending_queues = 0;
    if (kEnableDeferredFenceWait) {
        std::lock_guard<std::mutex> lock(g_mapped_mutex);
        pending_queues = g_deferred_wait_queues.size();
    }
    NoteDeferredRealWaitStats((uint64_t)(rpc_ms * 1000.0),
                              (uint64_t)pending_queues);
    if (has_fused_ranges) {
        NoteTargetedReadbackWait(fused_invalidate_ranges.size(),
                                 targeted_invalidate_bytes,
                                 (uint64_t)(rpc_ms * 1000.0),
                                 targeted_invalidate_us,
                                 (uint64_t)(total_ms * 1000.0),
                                 targeted_skipped_synced,
                                 vkResult);
    }

    if (kEnableLocalPerfLog) {
        ALOGD("[PERF_WaitForFences] fenceCount=%d waitAll=%d param_ms=%.3f rpc_ms=%.3f implicit_ms=%.3f total_ms=%.3f targeted=%d async_wait=%d target_ranges=%zu target_bytes=%llu skipped_synced=%llu result=%d",
              fenceCount,
              waitAll,
              param_ms,
              rpc_ms,
              implicit_ms,
              total_ms,
              (int)has_fused_ranges,
              (int)async_wait_transport,
              has_fused_ranges ? fused_invalidate_ranges.size() : 0,
              (unsigned long long)targeted_invalidate_bytes,
              (unsigned long long)targeted_skipped_synced,
              vkResult);
    }
    const uint64_t rpc_us = (uint64_t)(rpc_ms * 1000.0);
    const uint64_t implicit_us = (uint64_t)(implicit_ms * 1000.0);
    const uint64_t total_us = (uint64_t)(total_ms * 1000.0);
    const uint64_t wait_request_bytes = TotalTrackedRangeBytes(fused_invalidate_ranges);
    LlmVkTimingNoteWait(fenceCount,
                        rpc_us,
                        implicit_us,
                        total_us,
                        has_fused_ranges ? fused_invalidate_ranges.size() : 0,
                        wait_request_bytes,
                        targeted_invalidate_bytes,
                        targeted_skipped_synced,
                        vkResult);
    if (total_us >= kSlowWaitDiagLogUs || rpc_us >= kSlowWaitDiagLogUs) {
        ALOGI("[WAIT_SLOW_REAL] fenceCount=%u hostFenceCount=%u waitAll=%d timeout=%llu targeted=%d target_ranges=%zu request_bytes=%llu invalidate_bytes=%llu skipped_synced=%llu pending_queues=%zu param_us=%llu rpc_us=%llu implicit_us=%llu total_us=%llu result=%d",
              fenceCount,
              host_fenceCount,
              waitAll,
              (unsigned long long)timeout,
              (int)has_fused_ranges,
              has_fused_ranges ? fused_invalidate_ranges.size() : 0,
              (unsigned long long)wait_request_bytes,
              (unsigned long long)targeted_invalidate_bytes,
              (unsigned long long)targeted_skipped_synced,
              pending_queues,
              (unsigned long long)(param_ms * 1000.0),
              (unsigned long long)rpc_us,
              (unsigned long long)implicit_us,
              (unsigned long long)total_us,
              (int)vkResult);
    }
    
    return vkResult;
}

VKAPI_ATTR VkResult GetEventStatus(VkDevice device, VkEvent event) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    mgr.addParam64((uint64_t)(uintptr_t)device);
    mgr.addParam64((uint64_t)(uintptr_t)event);
    
    VkResult vkResult = VK_SUCCESS;
    mgr.addPtr(&vkResult, sizeof(VkResult));
    
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkGetEventStatus, true);

    EVK_PER_CALL_LOG("GetEventStatus device=%lld event=%lld result=%d", 
                     (long long)device, (long long)event, vkResult);
    return vkResult;
}

VKAPI_ATTR VkResult SetEvent(
    VkDevice device,
    VkEvent event) {
    
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    uint64_t guest_device = (uint64_t)(uintptr_t)device;
    uint64_t guest_event = (uint64_t)(uintptr_t)event;
    
    mgr.addParam64(guest_device);
    mgr.addParam64(guest_event);
    
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkSetEvent, false);
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL ResetCommandBuffer(
    VkCommandBuffer commandBuffer,
    VkCommandBufferResetFlags flags) {
    FlushPendingSubmitCohort("reset_command_buffer");
    if (kEnableDeferredFenceWait) {
        DrainDeferredQueues("reset_command_buffer");
    }

    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;

    mgr.addParam64((uint64_t)(uintptr_t)commandBuffer);
    mgr.addParam32((uint32_t)flags);

    VkResult vkResult = VK_ERROR_DEVICE_LOST;
    mgr.addPtr(&vkResult, sizeof(vkResult));
    const ssize_t written =
        FlimeGuestWrite(&mgr,
                        express_gpu,
                        EXPRESS_GPU_DEVICE_ID,
                        FUNID_vkResetCommandBuffer,
                        true);
    if (!IsCompleteParamManagerWrite(written, 2)) {
        vkResult = VK_ERROR_DEVICE_LOST;
    }
    if (vkResult == VK_SUCCESS) {
        ForgetTrackedCommandBuffer(commandBuffer);
    }
    FlimeGuestResetCommandBuffer(FUNID_vkResetCommandBuffer,
                                 commandBuffer,
                                 flags,
                                 vkResult,
                                 sizeof(uint64_t) + sizeof(uint32_t));

    EVK_PER_CALL_LOG("ResetCommandBuffer commandBuffer=%lld flags=%u", 
                     (long long)commandBuffer, flags);
    return vkResult;
}

VKAPI_ATTR VkResult VKAPI_CALL ResetCommandPool(
    VkDevice device,
    VkCommandPool commandPool,
    VkCommandPoolResetFlags flags) {
    FlushPendingSubmitCohort("reset_command_pool");
    if (kEnableDeferredFenceWait) {
        DrainDeferredQueues("reset_command_pool");
    }
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;

    mgr.addParam64((uint64_t)(uintptr_t)device);
    mgr.addParam64((uint64_t)(uintptr_t)commandPool);
    mgr.addParam32((uint32_t)flags);

    VkResult vkResult = VK_ERROR_DEVICE_LOST;
    mgr.addPtr(&vkResult, sizeof(vkResult));
    FlimeGuestBeforeDescriptorLifecycle(device);
    const ssize_t written =
        FlimeGuestWrite(&mgr,
                        express_gpu,
                        EXPRESS_GPU_DEVICE_ID,
                        FUNID_vkResetCommandPool,
                        true);
    const bool transport_ok = IsCompleteParamManagerWrite(written, 2);
    if (!transport_ok) {
        vkResult = VK_ERROR_DEVICE_LOST;
    }
    if (vkResult == VK_SUCCESS) {
        std::lock_guard<std::mutex> lock(g_mapped_mutex);
        InvalidateAllSubmitHintCachesLocked("reset_command_pool");
    }
    FlimeGuestResetCommandPool(device, commandPool, vkResult);
    FlimeGuestAfterDescriptorLifecycle(device, transport_ok);

    EVK_PER_CALL_LOG("ResetCommandPool device=%lld pool=%lld flags=%u", 
                     (long long)device, (long long)commandPool, flags);
    return vkResult;
}

VKAPI_ATTR VkResult VKAPI_CALL ResetDescriptorPool(
    VkDevice device,
    VkDescriptorPool descriptorPool,
    VkDescriptorPoolResetFlags flags) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;

    mgr.addParam64((uint64_t)(uintptr_t)device);
    mgr.addParam64((uint64_t)(uintptr_t)descriptorPool);
    mgr.addParam32((uint32_t)flags);

    VkResult vkResult = VK_ERROR_DEVICE_LOST;
    mgr.addPtr(&vkResult, sizeof(vkResult));
    FlimeGuestBeforeDescriptorLifecycle(device);
    if (!FlimeGuestPrepareDescriptorRetirement(device)) {
        mgr.clear();
        FlimeGuestAfterDescriptorLifecycle(device, false);
        return VK_ERROR_DEVICE_LOST;
    }
    const ssize_t written =
        FlimeGuestWrite(&mgr,
                        express_gpu,
                        EXPRESS_GPU_DEVICE_ID,
                        FUNID_vkResetDescriptorPool,
                        true);
    const bool transport_ok = IsCompleteParamManagerWrite(written, 2);
    if (!transport_ok) {
        vkResult = VK_ERROR_DEVICE_LOST;
    }
    if (vkResult == VK_SUCCESS) {
        ForgetTrackedDescriptorSetsForPool(descriptorPool);
    }
    FlimeGuestResetDescriptorPool(device, descriptorPool, vkResult);
    FlimeGuestAfterDescriptorLifecycle(device, transport_ok);

    EVK_PER_CALL_LOG("ResetDescriptorPool device=%lld pool=%lld flags=%u", 
                     (long long)device, (long long)descriptorPool, flags);
    return vkResult;
}

VKAPI_ATTR VkResult VKAPI_CALL ResetEvent(
    VkDevice device,
    VkEvent event) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;

    mgr.addParam64((uint64_t)(uintptr_t)device);
    mgr.addParam64((uint64_t)(uintptr_t)event);

    VkResult vkResult = VK_SUCCESS;
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkResetEvent, false);

    EVK_PER_CALL_LOG("ResetEvent device=%lld event=%lld", 
                     (long long)device, (long long)event);
    return vkResult;
}

VKAPI_ATTR void VKAPI_CALL ResetQueryPool(
    VkDevice device,
    VkQueryPool queryPool,
    uint32_t firstQuery,
    uint32_t queryCount) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;

    mgr.addParam64((uint64_t)(uintptr_t)device);
    mgr.addParam64((uint64_t)(uintptr_t)queryPool);
    mgr.addParam32(firstQuery);
    mgr.addParam32(queryCount);

    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkResetQueryPool, false);

    EVK_PER_CALL_LOG("ResetQueryPool device=%lld pool=%lld firstQuery=%u queryCount=%u", 
                     (long long)device, (long long)queryPool, firstQuery, queryCount);
}

VKAPI_ATTR VkResult GetQueryPoolResults(VkDevice device,
                                       VkQueryPool queryPool,
                                       uint32_t firstQuery,
                                       uint32_t queryCount,
                                       size_t dataSize,
                                       void* pData,
                                       VkDeviceSize stride,
                                       VkQueryResultFlags flags) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    mgr.addParam64((uint64_t)(uintptr_t)device);
    mgr.addParam64((uint64_t)(uintptr_t)queryPool);
    mgr.addParam32(firstQuery);
    mgr.addParam32(queryCount);
    mgr.addParam64(dataSize);
    mgr.addParam64(stride);
    mgr.addParam32(flags);
    mgr.addPtr(pData, dataSize);
    VkResult vkResult = VK_SUCCESS;
    mgr.addPtr(&vkResult, sizeof(VkResult));
    
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkGetQueryPoolResults, true);
    EVK_PER_CALL_LOG("GetQueryPoolResults device=%lld pool=%lld firstQuery=%u queryCount=%u dataSize=%zu stride=%lld flags=%u result=%d", 
                     (long long)device, (long long)queryPool, firstQuery, queryCount, dataSize, (long long)stride, flags, vkResult);
    return vkResult;
}


void GetImageSubresourceLayout(VkDevice device, VkImage image, const VkImageSubresource* pSubresource, VkSubresourceLayout* pLayout) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    mgr.addParam64((uint64_t)(uintptr_t)device);
    mgr.addParam64((uint64_t)(uintptr_t)image);
    mgr.addPtr((void*)pSubresource, sizeof(VkImageSubresource));
    mgr.addPtr(pLayout, sizeof(VkSubresourceLayout));
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkGetImageSubresourceLayout, true);
    EVK_PER_CALL_LOG("get FUNID_vkGetImageSubresourceLayout result %lld %lld %lld", (long long)pLayout->offset, (long long)pLayout->rowPitch, (long long)pLayout->size);
}

VKAPI_ATTR VkResult GetPipelineCacheData(VkDevice device,
                                        VkPipelineCache pipelineCache,
                                        size_t* pDataSize,
                                        void* pData) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    mgr.addParam64((uint64_t)(uintptr_t)device);
    mgr.addParam64((uint64_t)(uintptr_t)pipelineCache);
    
    mgr.addPtr(pDataSize, sizeof(size_t));
    if (pData && *pDataSize > 0) {
        mgr.addPtr(pData, *pDataSize);
    }
    
    VkResult vkResult = VK_SUCCESS;
    
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkGetPipelineCacheData, true);
    
    return vkResult;
}

VKAPI_ATTR VkResult MergePipelineCaches(VkDevice device,
                                       VkPipelineCache dstCache,
                                       uint32_t srcCacheCount,
                                       const VkPipelineCache* pSrcCaches) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    mgr.addParam64((uint64_t)(uintptr_t)device);
    mgr.addParam64((uint64_t)(uintptr_t)dstCache);
    mgr.addParam32(srcCacheCount);
    VkPipelineCache* local_pSrcCaches = (VkPipelineCache*)malloc(sizeof(VkPipelineCache) * srcCacheCount);
    memcpy(local_pSrcCaches, pSrcCaches, sizeof(VkPipelineCache) * srcCacheCount);
    mgr.addPtr((void*)local_pSrcCaches, sizeof(VkPipelineCache) * srcCacheCount);
    
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkMergePipelineCaches, false);
    return VK_SUCCESS;
}

VkResult FreeDescriptorSets(VkDevice device, VkDescriptorPool descriptorPool, uint32_t count, const VkDescriptorSet* pDescriptorSets) {
    int express_gpu = get_express_gpu_fd();
    EVK_PER_CALL_LOG("vkFreeDescriptorSets device=%lld pool=%lld", (long long)device, (long long)descriptorPool);
    thread_local ParamManager mgr;
    mgr.addParam64((uint64_t)(uintptr_t)device);
    mgr.addParam64((uint64_t)(uintptr_t)descriptorPool);
    
    mgr.addParam32(count);
    if (pDescriptorSets && count > 0) {
        // uint64_t* guest_sets = (uint64_t*)mgr.addExternalParamPtr(count * sizeof(uint64_t));
        for (uint32_t i = 0; i < count; ++i) {
            // guest_sets[i] = (uint64_t)(uintptr_t)pDescriptorSets[i];
            mgr.addParam64((uint64_t)(uintptr_t)pDescriptorSets[i]);
            EVK_PER_CALL_LOG("vkFreeDescriptorSets set %d: %llx", i, (long long)pDescriptorSets[i]);
        }
    }
    VkResult res = VK_ERROR_DEVICE_LOST;
    mgr.addPtr(&res, sizeof(res));
    FlimeGuestBeforeDescriptorLifecycle(device);
    if (!FlimeGuestPrepareDescriptorRetirement(device)) {
        mgr.clear();
        FlimeGuestAfterDescriptorLifecycle(device, false);
        return VK_ERROR_DEVICE_LOST;
    }
    const ssize_t written =
        FlimeGuestWrite(&mgr,
                        express_gpu,
                        EXPRESS_GPU_DEVICE_ID,
                        FUNID_vkFreeDescriptorSets,
                        true);
    const bool transport_ok = IsCompleteParamManagerWrite(written, 2);
    if (!transport_ok) {
        res = VK_ERROR_DEVICE_LOST;
    }
    FlimeGuestFreeDescriptorSets(
        device, descriptorPool, count, pDescriptorSets, res);
    if (res == VK_SUCCESS) {
        ForgetTrackedDescriptorSets(count, pDescriptorSets);
    }
    FlimeGuestAfterDescriptorLifecycle(device, transport_ok);

    return res;
}

VKAPI_ATTR void GetRenderAreaGranularity(
    VkDevice device,
    VkRenderPass renderPass,
    VkExtent2D* pGranularity) {
    
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    uint64_t guest_device = (uint64_t)(uintptr_t)device;
    uint64_t guest_render_pass = (uint64_t)(uintptr_t)renderPass;
    
    mgr.addParam64(guest_device);
    mgr.addParam64(guest_render_pass);
    mgr.addPtr(pGranularity, sizeof(VkExtent2D));
    
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkGetRenderAreaGranularity, true);
}

VKAPI_ATTR VkResult VKAPI_CALL EndCommandBuffer(VkCommandBuffer commandBuffer) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    mgr.addParam64((uint64_t)(uintptr_t)commandBuffer);
    VkResult vkResult = VK_ERROR_DEVICE_LOST;
    mgr.addPtr(&vkResult, sizeof(vkResult));
    const ssize_t written =
        FlimeGuestWrite(&mgr,
                        express_gpu,
                        EXPRESS_GPU_DEVICE_ID,
                        FUNID_vkEndCommandBuffer,
                        true);
    if (!IsCompleteParamManagerWrite(written, 2)) {
        vkResult = VK_ERROR_DEVICE_LOST;
    }
    FlimeGuestEndCommandBuffer(FUNID_vkEndCommandBuffer,
                               commandBuffer,
                               vkResult,
                               sizeof(uint64_t));
    
    EVK_PER_CALL_LOG("EndCommandBuffer cmd=%lld", (long long)commandBuffer);
    return vkResult;
}

VKAPI_ATTR void CmdSetViewport(VkCommandBuffer commandBuffer,
                               uint32_t firstViewport,
                               uint32_t viewportCount,
                               const VkViewport* pViewports) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    size_t count = sizeof(uint64_t) + sizeof(uint32_t) + sizeof(uint32_t);
    char* send_buffer = (char*)mgr.addExternalParamPtr(count);
    uint8_t** ptr = (uint8_t**)&send_buffer;
    
    uint64_t cmd_buffer = (uint64_t)(uintptr_t)commandBuffer;
    memcpy(*ptr, &cmd_buffer, sizeof(uint64_t)); *ptr += sizeof(uint64_t);
    memcpy(*ptr, &firstViewport, sizeof(uint32_t)); *ptr += sizeof(uint32_t);
    memcpy(*ptr, &viewportCount, sizeof(uint32_t)); *ptr += sizeof(uint32_t);
    
    mgr.addPtr((void*)pViewports, sizeof(VkViewport) * viewportCount);
    
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdSetViewport, false, commandBuffer);
}

VKAPI_ATTR void CmdSetScissor(VkCommandBuffer commandBuffer, uint32_t firstScissor, uint32_t scissorCount, const VkRect2D* pScissors) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    size_t scissors_size = scissorCount * sizeof(VkRect2D);
    size_t total_size = sizeof(uint32_t) * 2 + scissors_size;
    char* buffer = (char*)mgr.addExternalParamPtr(total_size);
    char* buffer_start = buffer;
    
    memcpy(buffer, &firstScissor, sizeof(uint32_t));
    buffer += sizeof(uint32_t);
    memcpy(buffer, &scissorCount, sizeof(uint32_t));
    buffer += sizeof(uint32_t);
    if (pScissors && scissorCount > 0) {
        memcpy(buffer, pScissors, scissors_size);
    }
    
    mgr.addParam64((uint64_t)(uintptr_t)commandBuffer);
    
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdSetScissor, false, commandBuffer);
}

VKAPI_ATTR void CmdSetLineWidth(VkCommandBuffer commandBuffer, float lineWidth) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    mgr.addParam64((uint64_t)(uintptr_t)commandBuffer);
    mgr.addParam32(*(uint32_t*)&lineWidth);
    
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdSetLineWidth, false, commandBuffer);
}

VKAPI_ATTR void CmdSetDepthBias(VkCommandBuffer commandBuffer,
                                float depthBiasConstantFactor,
                                float depthBiasClamp,
                                float depthBiasSlopeFactor) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    uint64_t guest_cmd_buffer = (uint64_t)(uintptr_t)commandBuffer;
    
    size_t count = sizeof(uint64_t) + sizeof(float) * 3;
    char* send_buffer = (char*)mgr.addExternalParamPtr(count);
    uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;
    
    memcpy(*send_buffer_ptr, &guest_cmd_buffer, sizeof(uint64_t));
    *send_buffer_ptr += sizeof(uint64_t);
    
    memcpy(*send_buffer_ptr, &depthBiasConstantFactor, sizeof(float));
    *send_buffer_ptr += sizeof(float);
    
    memcpy(*send_buffer_ptr, &depthBiasClamp, sizeof(float));
    *send_buffer_ptr += sizeof(float);
    
    memcpy(*send_buffer_ptr, &depthBiasSlopeFactor, sizeof(float));
    *send_buffer_ptr += sizeof(float);
    
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdSetDepthBias, false, commandBuffer);
}

VKAPI_ATTR void CmdSetBlendConstants(VkCommandBuffer commandBuffer,
                                     const float blendConstants[4]) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
   
    // Encode command buffer handle first, then blend constants
    mgr.addParam64((uint64_t)(uintptr_t)commandBuffer);
    
    char* send_buffer = (char*)mgr.addExternalParamPtr(16); // 4 floats = 16 bytes
    memcpy(send_buffer, blendConstants, 16);
   
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdSetBlendConstants, false, commandBuffer);
}

VKAPI_ATTR void CmdSetDepthBounds(VkCommandBuffer commandBuffer,
                                  float minDepthBounds,
                                  float maxDepthBounds) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
   
    mgr.addParam64((uint64_t)(uintptr_t)commandBuffer);
    
    char* send_buffer = (char*)mgr.addExternalParamPtr(8); // 2 floats = 8 bytes
    memcpy(send_buffer, &minDepthBounds, 4);
    memcpy(send_buffer + 4, &maxDepthBounds, 4);
   
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdSetDepthBounds, false, commandBuffer);
}

VKAPI_ATTR void CmdSetStencilCompareMask(VkCommandBuffer commandBuffer, VkStencilFaceFlags faceMask, uint32_t compareMask) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    mgr.addParam64((uint64_t)(uintptr_t)commandBuffer);
    mgr.addParam32((uint32_t)faceMask);
    mgr.addParam32(compareMask);
    
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdSetStencilCompareMask, false, commandBuffer);
}

VKAPI_ATTR void CmdSetStencilWriteMask(VkCommandBuffer commandBuffer,
                                       VkStencilFaceFlags faceMask,
                                       uint32_t writeMask) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    mgr.addParam64((uint64_t)(uintptr_t)commandBuffer);
    mgr.addParam32(faceMask);
    mgr.addParam32(writeMask);
    
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdSetStencilWriteMask, false, commandBuffer);
}


VKAPI_ATTR void CmdSetStencilReference(VkCommandBuffer commandBuffer,
                                       VkStencilFaceFlags faceMask,
                                       uint32_t reference) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    mgr.addParam64((uint64_t)(uintptr_t)commandBuffer);
    mgr.addParam32(faceMask);
    mgr.addParam32(reference);
    
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdSetStencilReference, false, commandBuffer);
}

VKAPI_ATTR void CmdBindDescriptorSets(VkCommandBuffer commandBuffer,
                                         VkPipelineBindPoint pipelineBindPoint,
                                         VkPipelineLayout layout,
                                         uint32_t firstSet,
                                         uint32_t descriptorSetCount,
                                         const VkDescriptorSet* pDescriptorSets,
                                         uint32_t dynamicOffsetCount,
                                         const uint32_t* pDynamicOffsets) {
    const uint64_t start_us = ExpressVkNowUs();
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    uint64_t guest_cmd = (uint64_t)(uintptr_t)commandBuffer;
    uint64_t guest_layout = (uint64_t)(uintptr_t)layout;
    
    size_t param_size = sizeof(uint64_t) * 2 + sizeof(uint32_t) * 4;
    char* send_buffer = (char*)mgr.addExternalParamPtr(param_size);
    uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;
    
    memcpy(*send_buffer_ptr, &guest_cmd, sizeof(uint64_t));
    *send_buffer_ptr += sizeof(uint64_t);
    memcpy(*send_buffer_ptr, &pipelineBindPoint, sizeof(uint32_t));
    *send_buffer_ptr += sizeof(uint32_t);
    memcpy(*send_buffer_ptr, &guest_layout, sizeof(uint64_t));
    *send_buffer_ptr += sizeof(uint64_t);
    memcpy(*send_buffer_ptr, &firstSet, sizeof(uint32_t));
    *send_buffer_ptr += sizeof(uint32_t);
    memcpy(*send_buffer_ptr, &descriptorSetCount, sizeof(uint32_t));
    *send_buffer_ptr += sizeof(uint32_t);
    memcpy(*send_buffer_ptr, &dynamicOffsetCount, sizeof(uint32_t));
    *send_buffer_ptr += sizeof(uint32_t);
    
    std::vector<uint64_t> guest_sets_storage;
    if (pDescriptorSets && descriptorSetCount > 0) {
        guest_sets_storage.resize(descriptorSetCount);
        for (uint32_t i = 0; i < descriptorSetCount; ++i) {
            guest_sets_storage[i] = (uint64_t)(uintptr_t)pDescriptorSets[i];
        }
        mgr.addPtr(guest_sets_storage.data(), descriptorSetCount * sizeof(uint64_t));
    }
    
    if (pDynamicOffsets && dynamicOffsetCount > 0) {
        mgr.addPtr((void*)pDynamicOffsets, dynamicOffsetCount * sizeof(uint32_t));
    }

    std::vector<TrackedMemoryRange> descriptor_early_upload_ranges;
    if (pipelineBindPoint == VK_PIPELINE_BIND_POINT_COMPUTE &&
        pDescriptorSets && descriptorSetCount > 0) {
        std::lock_guard<std::mutex> lock(g_mapped_mutex);
        std::unordered_set<VkDescriptorSet>& bound_sets =
            g_command_buffer_descriptor_sets[commandBuffer];
        for (uint32_t i = 0; i < descriptorSetCount; ++i) {
            if (pDescriptorSets[i] != VK_NULL_HANDLE) {
                bound_sets.insert(pDescriptorSets[i]);
            }
        }

        if (pDynamicOffsets && dynamicOffsetCount > 0) {
            uint32_t known_dynamic_slots = 0;
            for (uint32_t set_index = 0; set_index < descriptorSetCount; ++set_index) {
                VkDescriptorSet descriptor_set = pDescriptorSets[set_index];
                if (descriptor_set == VK_NULL_HANDLE) continue;

                auto set_it = g_descriptor_set_buffer_uses.find(descriptor_set);
                if (set_it == g_descriptor_set_buffer_uses.end()) continue;

                for (const auto& use_pair : set_it->second) {
                    if (use_pair.second.uses_dynamic_offset) {
                        known_dynamic_slots++;
                    }
                }
            }

            if (known_dynamic_slots != dynamicOffsetCount) {
                g_command_buffer_unknown_dynamic_offsets.insert(commandBuffer);
            } else {
                uint32_t dynamic_index = 0;
                for (uint32_t set_index = 0;
                     set_index < descriptorSetCount && dynamic_index < dynamicOffsetCount;
                     ++set_index) {
                    VkDescriptorSet descriptor_set = pDescriptorSets[set_index];
                    if (descriptor_set == VK_NULL_HANDLE) continue;

                    auto set_it = g_descriptor_set_buffer_uses.find(descriptor_set);
                    if (set_it == g_descriptor_set_buffer_uses.end()) continue;

                    std::vector<uint64_t> dynamic_slots;
                    for (const auto& use_pair : set_it->second) {
                        if (use_pair.second.uses_dynamic_offset) {
                            dynamic_slots.push_back(use_pair.first);
                        }
                    }
                    std::sort(dynamic_slots.begin(), dynamic_slots.end());

                    for (uint64_t slot_key : dynamic_slots) {
                        if (dynamic_index >= dynamicOffsetCount) break;
                        g_command_buffer_descriptor_dynamic_offsets[commandBuffer]
                            [descriptor_set][slot_key].push_back(pDynamicOffsets[dynamic_index++]);
                    }
                }
            }
        }

        if (kEnableDescriptorBindEarlyUpload) {
            GatherDescriptorUploadRangesForBoundSetsLocked(commandBuffer,
                                                           descriptorSetCount,
                                                           pDescriptorSets,
                                                           &descriptor_early_upload_ranges);
        }
        if (kEnableDescriptorTraceLog) {
            uint32_t logged_entries = 0;
            ALOGI("[DESC_TRACE] bind_summary cmd=%llx bindPoint=%u firstSet=%u sets=%u dyn_count=%u unknown_dyn=%d",
                  (unsigned long long)(uintptr_t)commandBuffer,
                  (uint32_t)pipelineBindPoint,
                  firstSet,
                  descriptorSetCount,
                  dynamicOffsetCount,
                  g_command_buffer_unknown_dynamic_offsets.find(commandBuffer) !=
                      g_command_buffer_unknown_dynamic_offsets.end());
            for (uint32_t i = 0; i < descriptorSetCount; ++i) {
                LogDescriptorSetTraceLocked("bind_desc",
                                            commandBuffer,
                                            pDescriptorSets[i],
                                            &logged_entries);
            }
        }
        /* Binding records command-buffer set use, but does not change descriptor content.
         * Descriptor/range hints are cached by descriptor-set version, so do not clear
         * global submit-hint cache on every bind. */
    }

    const uint64_t upload_start_us = ExpressVkNowUs();
    if (kEnableDescriptorBindEarlyUpload) {
        EarlyUploadTrackedRanges(descriptor_early_upload_ranges,
                                 kDescriptorEarlyUploadMinBytes,
                                 "descriptor_bind");
    }
    const uint64_t upload_us = ExpressVkNowUs() - upload_start_us;

    const uint64_t flime_encoded_bytes =
        param_size +
        ((pDescriptorSets && descriptorSetCount > 0)
             ? descriptorSetCount * sizeof(uint64_t)
             : 0) +
        ((pDynamicOffsets && dynamicOffsetCount > 0)
             ? dynamicOffsetCount * sizeof(uint32_t)
             : 0);
    FlimeGuestCmdBindDescriptorSets(FUNID_vkCmdBindDescriptorSets,
                                    commandBuffer,
                                    pipelineBindPoint,
                                    layout,
                                    firstSet,
                                    descriptorSetCount,
                                    pDescriptorSets,
                                    dynamicOffsetCount,
                                    pDynamicOffsets,
                                    flime_encoded_bytes);
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdBindDescriptorSets, false);
    {
        std::lock_guard<std::mutex> lock(g_descriptor_lifecycle_mutex);
        g_descriptor_lifecycle_stats.bind_set_calls++;
        g_descriptor_lifecycle_stats.bound_sets += descriptorSetCount;
        g_descriptor_lifecycle_stats.bind_set_us += ExpressVkNowUs() - start_us;
        g_descriptor_lifecycle_stats.bind_early_upload_us += upload_us;
        if (!kEnableDescriptorBindEarlyUpload &&
            pipelineBindPoint == VK_PIPELINE_BIND_POINT_COMPUTE &&
            descriptorSetCount > 0) {
            g_descriptor_lifecycle_stats.bind_early_upload_disabled++;
        }
        MaybeLogDescriptorLifecycleStatsLocked("periodic");
    }
}

VKAPI_ATTR void CmdBindIndexBuffer(VkCommandBuffer commandBuffer,
                                   VkBuffer buffer,
                                   VkDeviceSize offset,
                                   VkIndexType indexType) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    char* send_buffer = (char*)mgr.addExternalParamPtr(32);
    uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;
    
    uint64_t guest_cmd = (uint64_t)(uintptr_t)commandBuffer;
    uint64_t guest_buffer = (uint64_t)(uintptr_t)buffer;
    
    memcpy(*send_buffer_ptr, &guest_cmd, 8); *send_buffer_ptr += 8;
    memcpy(*send_buffer_ptr, &guest_buffer, 8); *send_buffer_ptr += 8;
    memcpy(*send_buffer_ptr, &offset, 8); *send_buffer_ptr += 8;
    memcpy(*send_buffer_ptr, &indexType, 4); *send_buffer_ptr += 4;
    
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdBindIndexBuffer, false, commandBuffer);
}

// void CmdDraw(VkCommandBuffer cmdBuffer, uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) {
// }

void VKAPI_CALL CmdDraw(
    VkCommandBuffer commandBuffer,
    uint32_t vertexCount,
    uint32_t instanceCount,
    uint32_t firstVertex,
    uint32_t firstInstance) {
    
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    mgr.addParam64((uint64_t)(uintptr_t)commandBuffer);
    mgr.addParam32(vertexCount);
    mgr.addParam32(instanceCount);
    mgr.addParam32(firstVertex);
    mgr.addParam32(firstInstance);
    
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID,
                           FUNID_vkCmdDraw, false, commandBuffer);
    
    EVK_PER_CALL_LOG("CmdDraw cmd=%lld vertices=%d instances=%d", (long long)commandBuffer, vertexCount, instanceCount);
}

VKAPI_ATTR void CmdDrawIndexed(VkCommandBuffer commandBuffer,
                              uint32_t indexCount,
                              uint32_t instanceCount,
                              uint32_t firstIndex,
                              int32_t vertexOffset,
                              uint32_t firstInstance) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    uint64_t guest_cmd_buffer = (uint64_t)(uintptr_t)commandBuffer;
    mgr.addParam64(guest_cmd_buffer);
    mgr.addParam32(indexCount);
    mgr.addParam32(instanceCount);
    mgr.addParam32(firstIndex);
    mgr.addParam32((uint32_t)vertexOffset);
    mgr.addParam32(firstInstance);
    
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdDrawIndexed, false, commandBuffer);
}

VKAPI_ATTR void CmdDrawIndirect(VkCommandBuffer commandBuffer,
                                VkBuffer buffer,
                                VkDeviceSize offset,
                                uint32_t drawCount,
                                uint32_t stride) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    mgr.addParam64((uint64_t)(uintptr_t)commandBuffer);
    mgr.addParam64((uint64_t)(uintptr_t)buffer);
    mgr.addParam64(offset);
    mgr.addParam32(drawCount);
    mgr.addParam32(stride);
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdDrawIndirect, false, commandBuffer);
}

VKAPI_ATTR void CmdDrawIndexedIndirect(VkCommandBuffer commandBuffer,
                                       VkBuffer buffer,
                                       VkDeviceSize offset,
                                       uint32_t drawCount,
                                       uint32_t stride) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    mgr.addParam64((uint64_t)(uintptr_t)commandBuffer);
    mgr.addParam64((uint64_t)(uintptr_t)buffer);
    mgr.addParam64(offset);
    mgr.addParam32(drawCount);
    mgr.addParam32(stride);
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdDrawIndexedIndirect, false, commandBuffer);
}

VKAPI_ATTR void CmdDispatch(VkCommandBuffer commandBuffer,
                           uint32_t groupCountX,
                           uint32_t groupCountY,
                           uint32_t groupCountZ) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    uint64_t guest_cmd_buffer = (uint64_t)(uintptr_t)commandBuffer;
    mgr.addParam64(guest_cmd_buffer);
    mgr.addParam32(groupCountX);
    mgr.addParam32(groupCountY);
    mgr.addParam32(groupCountZ);

    if (kEnableDescriptorTraceLog) {
        std::lock_guard<std::mutex> lock(g_mapped_mutex);
        uint32_t logged_entries = 0;
        size_t bound_set_count = 0;
        auto sets_it = g_command_buffer_descriptor_sets.find(commandBuffer);
        if (sets_it != g_command_buffer_descriptor_sets.end()) {
            bound_set_count = sets_it->second.size();
        }
        ALOGI("[DESC_TRACE] dispatch_summary cmd=%llx groups=(%u,%u,%u) bound_sets=%zu unknown_dyn=%d",
              (unsigned long long)(uintptr_t)commandBuffer,
              groupCountX,
              groupCountY,
              groupCountZ,
              bound_set_count,
              g_command_buffer_unknown_dynamic_offsets.find(commandBuffer) !=
                  g_command_buffer_unknown_dynamic_offsets.end());
        if (sets_it != g_command_buffer_descriptor_sets.end()) {
            for (VkDescriptorSet descriptor_set : sets_it->second) {
                LogDescriptorSetTraceLocked("dispatch_desc",
                                            commandBuffer,
                                            descriptor_set,
                                            &logged_entries);
            }
        }
    }

    if (kEnableMemShapeProbe) {
        std::lock_guard<std::mutex> lock(g_mapped_mutex);
        MemShapeNoteDispatchDescriptorsLocked(commandBuffer,
                                              groupCountX,
                                              groupCountY,
                                              groupCountZ);
    }
    
    const uint64_t record_start_us = ExpressVkNowUs();
    FlimeGuestCmdDispatch(FUNID_vkCmdDispatch,
                          commandBuffer,
                          groupCountX,
                          groupCountY,
                          groupCountZ,
                          sizeof(uint64_t) + 3 * sizeof(uint32_t));
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdDispatch, false);
    const uint64_t groups =
        (uint64_t)groupCountX * (uint64_t)groupCountY * (uint64_t)groupCountZ;
    LlmVkTimingNoteCmdDispatch(groups, ExpressVkNowUs() - record_start_us);
}

VKAPI_ATTR void CmdDispatchIndirect(VkCommandBuffer commandBuffer,
                                    VkBuffer buffer,
                                    VkDeviceSize offset) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    mgr.addParam64((uint64_t)(uintptr_t)commandBuffer);
    mgr.addParam64((uint64_t)(uintptr_t)buffer);
    mgr.addParam64(offset);
    FlimeGuestCmdDispatchIndirect(FUNID_vkCmdDispatchIndirect,
                                  commandBuffer,
                                  buffer,
                                  offset,
                                  3 * sizeof(uint64_t));
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdDispatchIndirect, false);
    if (kEnableLlmVkTimingLog) {
        g_llm_vk_timing.cmd_dispatch_indirect_calls.fetch_add(1, std::memory_order_relaxed);
        LlmVkTimingMaybeLog("cmd_dispatch_indirect");
    }
}

VKAPI_ATTR void CmdCopyBuffer(VkCommandBuffer commandBuffer,
                              VkBuffer srcBuffer,
                              VkBuffer dstBuffer,
                              uint32_t regionCount,
                              const VkBufferCopy* pRegions) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    uint64_t guest_cmd_buffer = (uint64_t)(uintptr_t)commandBuffer;
    uint64_t guest_src_buffer = (uint64_t)(uintptr_t)srcBuffer;
    uint64_t guest_dst_buffer = (uint64_t)(uintptr_t)dstBuffer;
    
    size_t count = sizeof(uint64_t) * 3 + sizeof(uint32_t) + 
                   sizeof(VkBufferCopy) * regionCount;
    char* send_buffer = (char*)mgr.addExternalParamPtr(count);
    uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;
    
    memcpy(*send_buffer_ptr, &guest_cmd_buffer, sizeof(uint64_t));
    *send_buffer_ptr += sizeof(uint64_t);
    
    memcpy(*send_buffer_ptr, &guest_src_buffer, sizeof(uint64_t));
    *send_buffer_ptr += sizeof(uint64_t);
    
    memcpy(*send_buffer_ptr, &guest_dst_buffer, sizeof(uint64_t));
    *send_buffer_ptr += sizeof(uint64_t);
    
    memcpy(*send_buffer_ptr, &regionCount, sizeof(uint32_t));
    *send_buffer_ptr += sizeof(uint32_t);
    
    memcpy(*send_buffer_ptr, pRegions, sizeof(VkBufferCopy) * regionCount);
    *send_buffer_ptr += sizeof(VkBufferCopy) * regionCount;

    std::vector<TrackedMemoryRange> copy_early_upload_ranges;
    uint64_t total_copy_bytes = 0;
    {
        std::lock_guard<std::mutex> lock(g_mapped_mutex);
        if (pRegions && regionCount > 0) {
            std::vector<BufferSyncRange>& flush_ranges =
                g_command_buffer_flush_buffer_ranges[commandBuffer];
            std::vector<BufferSyncRange>& invalidate_ranges =
                g_command_buffer_invalidate_buffer_ranges[commandBuffer];
            for (uint32_t i = 0; i < regionCount; ++i) {
                total_copy_bytes += (uint64_t)pRegions[i].size;
                AppendBufferSyncRange(&flush_ranges, {srcBuffer, pRegions[i].srcOffset, pRegions[i].size});
                AppendBufferSyncRange(&invalidate_ranges, {dstBuffer, pRegions[i].dstOffset, pRegions[i].size});
                MemShapeNoteBufferRangeLocked(srcBuffer,
                                              pRegions[i].srcOffset,
                                              pRegions[i].size,
                                              MemShapeRangeKind::kCopySrc);
                MemShapeNoteBufferRangeLocked(dstBuffer,
                                              pRegions[i].dstOffset,
                                              pRegions[i].size,
                                              MemShapeRangeKind::kCopyDst);
                TrackedMemoryRange tracked = {};
                if (ResolveBufferRangeLocked(srcBuffer, pRegions[i].srcOffset, pRegions[i].size, &tracked)) {
                    AppendTrackedRange(&copy_early_upload_ranges, tracked);
                }
            }
        } else {
            AppendBufferSyncRange(
                &g_command_buffer_flush_buffer_ranges[commandBuffer],
                {srcBuffer, 0, VK_WHOLE_SIZE});
            AppendBufferSyncRange(
                &g_command_buffer_invalidate_buffer_ranges[commandBuffer],
                {dstBuffer, 0, VK_WHOLE_SIZE});
            MemShapeNoteBufferRangeLocked(srcBuffer,
                                          0,
                                          VK_WHOLE_SIZE,
                                          MemShapeRangeKind::kCopySrc);
            MemShapeNoteBufferRangeLocked(dstBuffer,
                                          0,
                                          VK_WHOLE_SIZE,
                                          MemShapeRangeKind::kCopyDst);
            TrackedMemoryRange tracked = {};
            if (ResolveBufferRangeLocked(srcBuffer, 0, VK_WHOLE_SIZE, &tracked)) {
                AppendTrackedRange(&copy_early_upload_ranges, tracked);
            }
        }
        InvalidateCommandBufferSubmitHintCacheLocked(commandBuffer,
                                                     "cmd_copy_buffer");
    }
    if (kEnableCommandCopyEarlyUpload) {
        EarlyUploadTrackedRanges(copy_early_upload_ranges,
                                 kCommandEarlyUploadMinBytes,
                                 "cmd_copy_src");
    } else if (!copy_early_upload_ranges.empty()) {
        std::lock_guard<std::mutex> lock(g_descriptor_lifecycle_mutex);
        g_descriptor_lifecycle_stats.copy_early_upload_disabled++;
    }
    
    struct timespec t0_imp, t1_imp;
    clock_gettime(CLOCK_MONOTONIC, &t0_imp);
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdCopyBuffer, false, commandBuffer);
    clock_gettime(CLOCK_MONOTONIC, &t1_imp);
    double imp_ms = (t1_imp.tv_sec - t0_imp.tv_sec) * 1000.0 + (t1_imp.tv_nsec - t0_imp.tv_nsec) / 1000000.0;
    LlmVkTimingNoteCmdCopyBuffer(regionCount,
                                 total_copy_bytes,
                                 ExpressVkElapsedUs(t0_imp, t1_imp));
    EVK_PER_CALL_LOG("CmdCopyBuffer cmd=%lld srcBuffer=%lld dstBuffer=%lld regionCount=%u imp_ms=%.3f", 
                     (long long)commandBuffer, (long long)srcBuffer, (long long)dstBuffer, regionCount, imp_ms);
}

VKAPI_ATTR void CmdCopyImage(VkCommandBuffer commandBuffer,
                               VkImage srcImage,
                               VkImageLayout srcImageLayout,
                               VkImage dstImage,
                               VkImageLayout dstImageLayout,
                               uint32_t regionCount,
                               const VkImageCopy* pRegions) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    uint64_t guest_cmd = (uint64_t)(uintptr_t)commandBuffer;
    uint64_t guest_src = (uint64_t)(uintptr_t)srcImage;
    uint64_t guest_dst = (uint64_t)(uintptr_t)dstImage;
    
    size_t param_size = sizeof(uint64_t) * 3 + sizeof(uint32_t) * 3;
    char* send_buffer = (char*)mgr.addExternalParamPtr(param_size);
    uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;
    
    memcpy(*send_buffer_ptr, &guest_cmd, sizeof(uint64_t));
    *send_buffer_ptr += sizeof(uint64_t);
    memcpy(*send_buffer_ptr, &guest_src, sizeof(uint64_t));
    *send_buffer_ptr += sizeof(uint64_t);
    memcpy(*send_buffer_ptr, &srcImageLayout, sizeof(uint32_t));
    *send_buffer_ptr += sizeof(uint32_t);
    memcpy(*send_buffer_ptr, &guest_dst, sizeof(uint64_t));
    *send_buffer_ptr += sizeof(uint64_t);
    memcpy(*send_buffer_ptr, &dstImageLayout, sizeof(uint32_t));
    *send_buffer_ptr += sizeof(uint32_t);
    memcpy(*send_buffer_ptr, &regionCount, sizeof(uint32_t));
    *send_buffer_ptr += sizeof(uint32_t);
    
    if (pRegions && regionCount > 0) {
        mgr.addPtr((void*)pRegions, regionCount * sizeof(VkImageCopy));
    }
    
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdCopyImage, false, commandBuffer);
}

VKAPI_ATTR void CmdBlitImage(VkCommandBuffer commandBuffer,
                            VkImage srcImage,
                            VkImageLayout srcImageLayout,
                            VkImage dstImage,
                            VkImageLayout dstImageLayout,
                            uint32_t regionCount,
                            const VkImageBlit* pRegions,
                            VkFilter filter) {
    EVK_PER_CALL_LOG("in CmdBlitImage!");
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    size_t count = 0;
    size_t* countPtr = &count;
    count += regionCount * sizeof(VkImageBlit);
    count += sizeof(uint32_t) * 7; // all scalar params
    
    char* send_buffer = (char*)mgr.addExternalParamPtr(count);
    uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;
    
    // Encode parameters
    uint64_t cmdBuf = (uint64_t)(uintptr_t)commandBuffer;
    uint64_t srcImg = (uint64_t)(uintptr_t)srcImage;
    uint64_t dstImg = (uint64_t)(uintptr_t)dstImage;
    
    memcpy(*send_buffer_ptr, &cmdBuf, 8); *send_buffer_ptr += 8;
    memcpy(*send_buffer_ptr, &srcImg, 8); *send_buffer_ptr += 8;
    memcpy(*send_buffer_ptr, &srcImageLayout, 4); *send_buffer_ptr += 4;
    memcpy(*send_buffer_ptr, &dstImg, 8); *send_buffer_ptr += 8;
    memcpy(*send_buffer_ptr, &dstImageLayout, 4); *send_buffer_ptr += 4;
    memcpy(*send_buffer_ptr, &regionCount, 4); *send_buffer_ptr += 4;
    memcpy(*send_buffer_ptr, &filter, 4); *send_buffer_ptr += 4;
    
    if (regionCount > 0 && pRegions) {
        memcpy(*send_buffer_ptr, pRegions, regionCount * sizeof(VkImageBlit));
    }
    
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdBlitImage, false, commandBuffer);

    EVK_PER_CALL_LOG("CmdBlitImage cmd=%lld srcImage=%lld dstImage=%lld regionCount=%u filter=%u",
                     (long long)commandBuffer, (long long)srcImage, (long long)dstImage, regionCount, filter);
}

VKAPI_ATTR void CmdCopyBufferToImage(VkCommandBuffer commandBuffer,
                                      VkBuffer srcBuffer,
                                      VkImage dstImage,
                                      VkImageLayout dstImageLayout,
                                      uint32_t regionCount,
                                      const VkBufferImageCopy* pRegions) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    size_t count = 0;
    size_t* countPtr = &count;
    
    // Count size needed for VkBufferImageCopy array
    for (uint32_t i = 0; i < regionCount; ++i) {
        count_VkBufferImageCopy(0, VK_STRUCTURE_TYPE_MAX_ENUM, &pRegions[i], countPtr);
    }
    count += sizeof(uint64_t) * 3 + sizeof(uint32_t) + sizeof(uint32_t); // handles + layout + regionCount
    
    char* send_buffer = (char*)mgr.addExternalParamPtr(count);
    uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;
    
    // Encode parameters
    uint64_t guest_commandBuffer = (uint64_t)(uintptr_t)commandBuffer;
    uint64_t guest_srcBuffer = (uint64_t)(uintptr_t)srcBuffer;
    uint64_t guest_dstImage = (uint64_t)(uintptr_t)dstImage;
    
    memcpy(*send_buffer_ptr, &guest_commandBuffer, sizeof(uint64_t));
    *send_buffer_ptr += sizeof(uint64_t);
    memcpy(*send_buffer_ptr, &guest_srcBuffer, sizeof(uint64_t));
    *send_buffer_ptr += sizeof(uint64_t);
    memcpy(*send_buffer_ptr, &guest_dstImage, sizeof(uint64_t));
    *send_buffer_ptr += sizeof(uint64_t);
    memcpy(*send_buffer_ptr, &dstImageLayout, sizeof(uint32_t));
    *send_buffer_ptr += sizeof(uint32_t);
    memcpy(*send_buffer_ptr, &regionCount, sizeof(uint32_t));
    *send_buffer_ptr += sizeof(uint32_t);
    
    // Encode regions array
    for (uint32_t i = 0; i < regionCount; ++i) {
        encode_to_stream_VkBufferImageCopy(VK_STRUCTURE_TYPE_MAX_ENUM, &pRegions[i], send_buffer_ptr);
    }

    MarkBufferAsFlushHint(srcBuffer);
    
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdCopyBufferToImage, false, commandBuffer);
}

VKAPI_ATTR void CmdCopyImageToBuffer(VkCommandBuffer commandBuffer,
                                     VkImage srcImage,
                                     VkImageLayout srcImageLayout,
                                     VkBuffer dstBuffer,
                                     uint32_t regionCount,
                                     const VkBufferImageCopy* pRegions) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    uint64_t guest_cmd_buffer = (uint64_t)(uintptr_t)commandBuffer;
    uint64_t guest_src_image = (uint64_t)(uintptr_t)srcImage;
    uint64_t guest_dst_buffer = (uint64_t)(uintptr_t)dstBuffer;
    
    size_t count = sizeof(uint64_t) * 3 + sizeof(VkImageLayout) + sizeof(uint32_t) + 
                   sizeof(VkBufferImageCopy) * regionCount;
    char* send_buffer = (char*)mgr.addExternalParamPtr(count);
    uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;
    
    memcpy(*send_buffer_ptr, &guest_cmd_buffer, sizeof(uint64_t));
    *send_buffer_ptr += sizeof(uint64_t);
    
    memcpy(*send_buffer_ptr, &guest_src_image, sizeof(uint64_t));
    *send_buffer_ptr += sizeof(uint64_t);
    
    memcpy(*send_buffer_ptr, &srcImageLayout, sizeof(VkImageLayout));
    *send_buffer_ptr += sizeof(VkImageLayout);
    
    memcpy(*send_buffer_ptr, &guest_dst_buffer, sizeof(uint64_t));
    *send_buffer_ptr += sizeof(uint64_t);
    
    memcpy(*send_buffer_ptr, &regionCount, sizeof(uint32_t));
    *send_buffer_ptr += sizeof(uint32_t);
    
    memcpy(*send_buffer_ptr, pRegions, sizeof(VkBufferImageCopy) * regionCount);
    *send_buffer_ptr += sizeof(VkBufferImageCopy) * regionCount;

    MarkBufferAsInvalidateHint(dstBuffer);
    
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdCopyImageToBuffer, false, commandBuffer);
}

VKAPI_ATTR void CmdUpdateBuffer(VkCommandBuffer commandBuffer,
                               VkBuffer dstBuffer,
                               VkDeviceSize dstOffset,
                               VkDeviceSize dataSize,
                               const void* pData) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    mgr.addParam64((uint64_t)(uintptr_t)commandBuffer);
    mgr.addParam64((uint64_t)(uintptr_t)dstBuffer);
    mgr.addParam64(dstOffset);
    mgr.addParam64(dataSize);
    void* send_data = malloc(dataSize);
    memcpy(send_data, pData, dataSize);
    mgr.addPtr(send_data, dataSize);
    
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID,
              FUNID_vkCmdUpdateBuffer, false, commandBuffer);

    free(send_data);
}

VKAPI_ATTR void CmdFillBuffer(VkCommandBuffer commandBuffer,
                             VkBuffer dstBuffer,
                             VkDeviceSize dstOffset,
                             VkDeviceSize size,
                             uint32_t data) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    mgr.addParam64((uint64_t)(uintptr_t)commandBuffer);
    mgr.addParam64((uint64_t)(uintptr_t)dstBuffer);
    mgr.addParam64(dstOffset);
    mgr.addParam64(size);
    mgr.addParam32(data);
    
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID,
              FUNID_vkCmdFillBuffer, false, commandBuffer);
}
VKAPI_ATTR void CmdClearColorImage(VkCommandBuffer commandBuffer,
                                   VkImage image,
                                   VkImageLayout imageLayout,
                                   const VkClearColorValue* pColor,
                                   uint32_t rangeCount,
                                   const VkImageSubresourceRange* pRanges) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    size_t count = 0;
    size_t* countPtr = &count;
    count_VkClearColorValue(0, VK_STRUCTURE_TYPE_MAX_ENUM, (VkClearColorValue*)pColor, countPtr);
    for (uint32_t i = 0; i < rangeCount; ++i) {
        count_VkImageSubresourceRange(0, VK_STRUCTURE_TYPE_MAX_ENUM, (VkImageSubresourceRange*)&pRanges[i], countPtr);
    }
    count += sizeof(uint64_t) * 2 + sizeof(uint32_t) * 2;
    
    char* send_buffer = (char*)mgr.addExternalParamPtr(count);
    uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;
    
    uint64_t guest_cmd = (uint64_t)(uintptr_t)commandBuffer;
    memcpy(*send_buffer_ptr, &guest_cmd, sizeof(uint64_t));
    *send_buffer_ptr += sizeof(uint64_t);
    
    uint64_t guest_image = (uint64_t)(uintptr_t)image;
    memcpy(*send_buffer_ptr, &guest_image, sizeof(uint64_t));
    *send_buffer_ptr += sizeof(uint64_t);
    
    uint32_t layout = (uint32_t)imageLayout;
    memcpy(*send_buffer_ptr, &layout, sizeof(uint32_t));
    *send_buffer_ptr += sizeof(uint32_t);
    
    encode_to_stream_VkClearColorValue(VK_STRUCTURE_TYPE_MAX_ENUM, (VkClearColorValue*)pColor, send_buffer_ptr);
    
    memcpy(*send_buffer_ptr, &rangeCount, sizeof(uint32_t));
    *send_buffer_ptr += sizeof(uint32_t);
    
    for (uint32_t i = 0; i < rangeCount; ++i) {
        encode_to_stream_VkImageSubresourceRange(VK_STRUCTURE_TYPE_MAX_ENUM, (VkImageSubresourceRange*)&pRanges[i], send_buffer_ptr);
    }
    
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdClearColorImage, false, commandBuffer);
}

VKAPI_ATTR void CmdClearDepthStencilImage(VkCommandBuffer commandBuffer,
                                          VkImage image,
                                          VkImageLayout imageLayout,
                                          const VkClearDepthStencilValue* pDepthStencil,
                                          uint32_t rangeCount,
                                          const VkImageSubresourceRange* pRanges) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    size_t count = 0;
    size_t* countPtr = &count;
    count_VkClearDepthStencilValue(0, VK_STRUCTURE_TYPE_MAX_ENUM, (VkClearDepthStencilValue*)pDepthStencil, countPtr);
    for (uint32_t i = 0; i < rangeCount; ++i) {
        count_VkImageSubresourceRange(0, VK_STRUCTURE_TYPE_MAX_ENUM, (VkImageSubresourceRange*)&pRanges[i], countPtr);
    }
    count += sizeof(uint64_t) * 2 + sizeof(uint32_t) * 2;
    
    char* send_buffer = (char*)mgr.addExternalParamPtr(count);
    uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;
    
    uint64_t guest_cmd = (uint64_t)(uintptr_t)commandBuffer;
    memcpy(*send_buffer_ptr, &guest_cmd, sizeof(uint64_t));
    *send_buffer_ptr += sizeof(uint64_t);
    
    uint64_t guest_image = (uint64_t)(uintptr_t)image;
    memcpy(*send_buffer_ptr, &guest_image, sizeof(uint64_t));
    *send_buffer_ptr += sizeof(uint64_t);
    
    uint32_t layout = (uint32_t)imageLayout;
    memcpy(*send_buffer_ptr, &layout, sizeof(uint32_t));
    *send_buffer_ptr += sizeof(uint32_t);
    
    encode_to_stream_VkClearDepthStencilValue(VK_STRUCTURE_TYPE_MAX_ENUM, (VkClearDepthStencilValue*)pDepthStencil, send_buffer_ptr);
    
    memcpy(*send_buffer_ptr, &rangeCount, sizeof(uint32_t));
    *send_buffer_ptr += sizeof(uint32_t);
    
    for (uint32_t i = 0; i < rangeCount; ++i) {
        encode_to_stream_VkImageSubresourceRange(VK_STRUCTURE_TYPE_MAX_ENUM, (VkImageSubresourceRange*)&pRanges[i], send_buffer_ptr);
    }
    
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdClearDepthStencilImage, false, commandBuffer);
}

VKAPI_ATTR void CmdClearAttachments(VkCommandBuffer commandBuffer,
                                    uint32_t attachmentCount,
                                    const VkClearAttachment* pAttachments,
                                    uint32_t rectCount,
                                    const VkClearRect* pRects) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    size_t count = 16 + attachmentCount * sizeof(VkClearAttachment) + rectCount * sizeof(VkClearRect);
    char* send_buffer = (char*)mgr.addExternalParamPtr(count);
    uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;
    
    uint64_t guest_cmd = (uint64_t)(uintptr_t)commandBuffer;
    memcpy(*send_buffer_ptr, &guest_cmd, 8); *send_buffer_ptr += 8;
    memcpy(*send_buffer_ptr, &attachmentCount, 4); *send_buffer_ptr += 4;
    memcpy(*send_buffer_ptr, &rectCount, 4); *send_buffer_ptr += 4;
    
    if (pAttachments && attachmentCount > 0) {
        memcpy(*send_buffer_ptr, pAttachments, attachmentCount * sizeof(VkClearAttachment));
        *send_buffer_ptr += attachmentCount * sizeof(VkClearAttachment);
    }
    
    if (pRects && rectCount > 0) {
        memcpy(*send_buffer_ptr, pRects, rectCount * sizeof(VkClearRect));
        *send_buffer_ptr += rectCount * sizeof(VkClearRect);
    }
    
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdClearAttachments, false, commandBuffer);
}

VKAPI_ATTR void CmdResolveImage(VkCommandBuffer commandBuffer,
                                VkImage srcImage,
                                VkImageLayout srcImageLayout,
                                VkImage dstImage,
                                VkImageLayout dstImageLayout,
                                uint32_t regionCount,
                                const VkImageResolve* pRegions) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
   
    size_t count = sizeof(uint64_t) * 3 + sizeof(uint32_t) * 3;
    if (pRegions && regionCount > 0) {
        count += regionCount * sizeof(VkImageResolve);
    }
   
    char* send_buffer = (char*)mgr.addExternalParamPtr(count);
    uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;
   
    uint64_t guest_cmd = (uint64_t)(uintptr_t)commandBuffer;
    memcpy(*send_buffer_ptr, &guest_cmd, sizeof(uint64_t));
    *send_buffer_ptr += sizeof(uint64_t);
    
    uint64_t guest_src = (uint64_t)(uintptr_t)srcImage;
    memcpy(*send_buffer_ptr, &guest_src, sizeof(uint64_t));
    *send_buffer_ptr += sizeof(uint64_t);
    
    uint32_t src_layout = (uint32_t)srcImageLayout;
    memcpy(*send_buffer_ptr, &src_layout, sizeof(uint32_t));
    *send_buffer_ptr += sizeof(uint32_t);
    
    uint64_t guest_dst = (uint64_t)(uintptr_t)dstImage;
    memcpy(*send_buffer_ptr, &guest_dst, sizeof(uint64_t));
    *send_buffer_ptr += sizeof(uint64_t);
    
    uint32_t dst_layout = (uint32_t)dstImageLayout;
    memcpy(*send_buffer_ptr, &dst_layout, sizeof(uint32_t));
    *send_buffer_ptr += sizeof(uint32_t);
    
    memcpy(*send_buffer_ptr, &regionCount, sizeof(uint32_t));
    *send_buffer_ptr += sizeof(uint32_t);
   
    if (pRegions && regionCount > 0) {
        memcpy(*send_buffer_ptr, pRegions, regionCount * sizeof(VkImageResolve));
    }
   
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdResolveImage, false, commandBuffer);
}

VKAPI_ATTR void CmdSetEvent(VkCommandBuffer commandBuffer,
                           VkEvent event,
                           VkPipelineStageFlags stageMask) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    mgr.addParam64((uint64_t)(uintptr_t)commandBuffer);
    mgr.addParam64((uint64_t)(uintptr_t)event);
    mgr.addParam32(stageMask);
    
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID,
              FUNID_vkCmdSetEvent, false, commandBuffer);
}

VKAPI_ATTR void CmdResetEvent(VkCommandBuffer commandBuffer,
                             VkEvent event,
                             VkPipelineStageFlags stageMask) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    mgr.addParam64((uint64_t)(uintptr_t)commandBuffer);
    mgr.addParam64((uint64_t)(uintptr_t)event);
    mgr.addParam32(stageMask);
    
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID,
              FUNID_vkCmdResetEvent, false, commandBuffer);
}

VKAPI_ATTR void CmdWaitEvents(VkCommandBuffer commandBuffer,
                             uint32_t eventCount,
                             const VkEvent* pEvents,
                             VkPipelineStageFlags srcStageMask,
                             VkPipelineStageFlags dstStageMask,
                             uint32_t memoryBarrierCount,
                             const VkMemoryBarrier* pMemoryBarriers,
                             uint32_t bufferMemoryBarrierCount,
                             const VkBufferMemoryBarrier* pBufferMemoryBarriers,
                             uint32_t imageMemoryBarrierCount,
                             const VkImageMemoryBarrier* pImageMemoryBarriers) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    size_t count = 0;
    size_t* countPtr = &count;
    
    if (pMemoryBarriers) {
        for (uint32_t i = 0; i < memoryBarrierCount; ++i) {
            count_VkMemoryBarrier(0, VK_STRUCTURE_TYPE_MAX_ENUM, &pMemoryBarriers[i], countPtr);
        }
    }
    if (pBufferMemoryBarriers) {
        for (uint32_t i = 0; i < bufferMemoryBarrierCount; ++i) {
            count_VkBufferMemoryBarrier(0, VK_STRUCTURE_TYPE_MAX_ENUM, &pBufferMemoryBarriers[i], countPtr);
        }
    }
    if (pImageMemoryBarriers) {
        for (uint32_t i = 0; i < imageMemoryBarrierCount; ++i) {
            count_VkImageMemoryBarrier(0, VK_STRUCTURE_TYPE_MAX_ENUM, &pImageMemoryBarriers[i], countPtr);
        }
    }
    count += sizeof(uint64_t) * 1 + sizeof(uint32_t) * 6 + sizeof(uint64_t) * eventCount;
    
    char* send_buffer = (char*)mgr.addExternalParamPtr(count);
    uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;
    
    uint64_t cgen_var_0 = (uint64_t)(uintptr_t)commandBuffer;
    memcpy((*send_buffer_ptr), &cgen_var_0, 8); *send_buffer_ptr += 8;
    
    memcpy((*send_buffer_ptr), &eventCount, 4); *send_buffer_ptr += 4;
    for (uint32_t i = 0; i < eventCount; ++i) {
        uint64_t event_val = (uint64_t)(uintptr_t)pEvents[i];
        memcpy((*send_buffer_ptr), &event_val, 8); *send_buffer_ptr += 8;
    }
    
    memcpy((*send_buffer_ptr), &srcStageMask, 4); *send_buffer_ptr += 4;
    memcpy((*send_buffer_ptr), &dstStageMask, 4); *send_buffer_ptr += 4;
    memcpy((*send_buffer_ptr), &memoryBarrierCount, 4); *send_buffer_ptr += 4;
    memcpy((*send_buffer_ptr), &bufferMemoryBarrierCount, 4); *send_buffer_ptr += 4;
    memcpy((*send_buffer_ptr), &imageMemoryBarrierCount, 4); *send_buffer_ptr += 4;
    
    if (pMemoryBarriers) {
        for (uint32_t i = 0; i < memoryBarrierCount; ++i) {
            encode_to_stream_VkMemoryBarrier(VK_STRUCTURE_TYPE_MAX_ENUM, &pMemoryBarriers[i], send_buffer_ptr);
        }
    }
    if (pBufferMemoryBarriers) {
        for (uint32_t i = 0; i < bufferMemoryBarrierCount; ++i) {
            encode_to_stream_VkBufferMemoryBarrier(VK_STRUCTURE_TYPE_MAX_ENUM, &pBufferMemoryBarriers[i], send_buffer_ptr);
        }
    }
    if (pImageMemoryBarriers) {
        for (uint32_t i = 0; i < imageMemoryBarrierCount; ++i) {
            encode_to_stream_VkImageMemoryBarrier(VK_STRUCTURE_TYPE_MAX_ENUM, &pImageMemoryBarriers[i], send_buffer_ptr);
        }
    }
    
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID,
              FUNID_vkCmdWaitEvents, true, commandBuffer);
    EVK_PER_CALL_LOG("CmdWaitEvents commandBuffer=%lld eventCount=%d srcStageMask=%u dstStageMask=%u memoryBarrierCount=%d bufferMemoryBarrierCount=%d imageMemoryBarrierCount=%d",
                     (long long)commandBuffer, eventCount, srcStageMask, dstStageMask,
                     memoryBarrierCount, bufferMemoryBarrierCount, imageMemoryBarrierCount);
}

VKAPI_ATTR void VKAPI_CALL CmdPipelineBarrier(
    VkCommandBuffer commandBuffer,
    VkPipelineStageFlags srcStageMask,
    VkPipelineStageFlags dstStageMask,
    VkDependencyFlags dependencyFlags,
    uint32_t memoryBarrierCount,
    const VkMemoryBarrier* pMemoryBarriers,
    uint32_t bufferMemoryBarrierCount,
    const VkBufferMemoryBarrier* pBufferMemoryBarriers,
    uint32_t imageMemoryBarrierCount,
    const VkImageMemoryBarrier* pImageMemoryBarriers)
{
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    Allocator vkAllocator;
    const uint64_t prepare_start_us = ExpressVkNowUs();
    
    VkMemoryBarrier* localMemBarriers = nullptr;
    if (memoryBarrierCount > 0) {
        localMemBarriers = (VkMemoryBarrier*)malloc(memoryBarrierCount * sizeof(VkMemoryBarrier));
        for (uint32_t i = 0; i < memoryBarrierCount; ++i) {
            deepcopy_VkMemoryBarrier(&vkAllocator, VK_STRUCTURE_TYPE_MEMORY_BARRIER, &pMemoryBarriers[i], &localMemBarriers[i]);
        }
    }
    
    VkBufferMemoryBarrier* localBufBarriers = nullptr;
    if (bufferMemoryBarrierCount > 0) {
        localBufBarriers = (VkBufferMemoryBarrier*)malloc(bufferMemoryBarrierCount * sizeof(VkBufferMemoryBarrier));
        for (uint32_t i = 0; i < bufferMemoryBarrierCount; ++i) {
            deepcopy_VkBufferMemoryBarrier(&vkAllocator, VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, &pBufferMemoryBarriers[i], &localBufBarriers[i]);
        }
    }
    
    VkImageMemoryBarrier* localImgBarriers = nullptr;
    if (imageMemoryBarrierCount > 0) {
        localImgBarriers = (VkImageMemoryBarrier*)malloc(imageMemoryBarrierCount * sizeof(VkImageMemoryBarrier));
        for (uint32_t i = 0; i < imageMemoryBarrierCount; ++i) {
            deepcopy_VkImageMemoryBarrier(&vkAllocator, VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, &pImageMemoryBarriers[i], &localImgBarriers[i]);
        }
    }
    
    size_t byteCount = sizeof(uint64_t) + sizeof(uint32_t) * 6; // cmdBuf + flags + counts
    for (uint32_t i = 0; i < memoryBarrierCount; ++i) {
        count_VkMemoryBarrier(0, VK_STRUCTURE_TYPE_MEMORY_BARRIER, &localMemBarriers[i], &byteCount);
    }
    for (uint32_t i = 0; i < bufferMemoryBarrierCount; ++i) {
        count_VkBufferMemoryBarrier(0, VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, &localBufBarriers[i], &byteCount);
    }
    for (uint32_t i = 0; i < imageMemoryBarrierCount; ++i) {
        count_VkImageMemoryBarrier(0, VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, &localImgBarriers[i], &byteCount);
    }
    
    char* buf = (char*)mgr.addExternalParamPtr(byteCount);
    uint8_t** ptr = (uint8_t**)&buf;
    
    uint64_t cmdBufPtr = (uint64_t)(uintptr_t)commandBuffer;
    memcpy(*ptr, &cmdBufPtr, sizeof(uint64_t));
    *ptr += sizeof(uint64_t);
    
    memcpy(*ptr, &srcStageMask, sizeof(uint32_t));
    *ptr += sizeof(uint32_t);
    memcpy(*ptr, &dstStageMask, sizeof(uint32_t));
    *ptr += sizeof(uint32_t);
    memcpy(*ptr, &dependencyFlags, sizeof(uint32_t));
    *ptr += sizeof(uint32_t);
    memcpy(*ptr, &memoryBarrierCount, sizeof(uint32_t));
    *ptr += sizeof(uint32_t);
    memcpy(*ptr, &bufferMemoryBarrierCount, sizeof(uint32_t));
    *ptr += sizeof(uint32_t);
    memcpy(*ptr, &imageMemoryBarrierCount, sizeof(uint32_t));
    *ptr += sizeof(uint32_t);
    
    for (uint32_t i = 0; i < memoryBarrierCount; ++i) {
        encode_to_stream_VkMemoryBarrier(VK_STRUCTURE_TYPE_MAX_ENUM, &localMemBarriers[i], ptr);
    }
    for (uint32_t i = 0; i < bufferMemoryBarrierCount; ++i) {
        encode_to_stream_VkBufferMemoryBarrier(VK_STRUCTURE_TYPE_MAX_ENUM, &localBufBarriers[i], ptr);
    }
    for (uint32_t i = 0; i < imageMemoryBarrierCount; ++i) {
        encode_to_stream_VkImageMemoryBarrier(VK_STRUCTURE_TYPE_MAX_ENUM, &localImgBarriers[i], ptr);
    }
    
    const uint64_t prepare_end_us = ExpressVkNowUs();
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdPipelineBarrier, false, commandBuffer);
    const uint64_t write_end_us = ExpressVkNowUs();
    LlmVkTimingNoteCmdPipelineBarrier(memoryBarrierCount,
                                      bufferMemoryBarrierCount,
                                      imageMemoryBarrierCount,
                                      prepare_end_us - prepare_start_us,
                                      write_end_us - prepare_end_us,
                                      write_end_us - prepare_start_us);
    
    EVK_PER_CALL_LOG("CmdPipelineBarrier %lld barriers: mem %d buf %d img %d", (long long)commandBuffer, memoryBarrierCount, bufferMemoryBarrierCount, imageMemoryBarrierCount);
    
    if (localMemBarriers) free(localMemBarriers);
    if (localBufBarriers) free(localBufBarriers);
    if (localImgBarriers) free(localImgBarriers);
}

VKAPI_ATTR void CmdBeginQuery(VkCommandBuffer commandBuffer,
                              VkQueryPool queryPool,
                              uint32_t query,
                              VkQueryControlFlags flags) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    mgr.addParam64((uint64_t)(uintptr_t)commandBuffer);
    mgr.addParam64((uint64_t)(uintptr_t)queryPool);
    mgr.addParam32(query);
    mgr.addParam32(flags);
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdBeginQuery, false, commandBuffer);
}

VKAPI_ATTR void CmdEndQuery(VkCommandBuffer commandBuffer,
                            VkQueryPool queryPool,
                            uint32_t query) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    mgr.addParam64((uint64_t)(uintptr_t)commandBuffer);
    mgr.addParam64((uint64_t)(uintptr_t)queryPool);
    mgr.addParam32(query);
    
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID,
              FUNID_vkCmdEndQuery, false, commandBuffer);
}

VKAPI_ATTR void CmdResetQueryPool(
    VkCommandBuffer commandBuffer,
    VkQueryPool queryPool,
    uint32_t firstQuery,
    uint32_t queryCount) {
    
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    uint64_t guest_cmd_buf = (uint64_t)(uintptr_t)commandBuffer;
    uint64_t guest_query_pool = (uint64_t)(uintptr_t)queryPool;
    
    size_t count = 8 + 8 + 4 + 4;
    char* send_buffer = (char*)mgr.addExternalParamPtr(count);
    uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;
    
    memcpy(*send_buffer_ptr, &guest_cmd_buf, 8);
    *send_buffer_ptr += 8;
    memcpy(*send_buffer_ptr, &guest_query_pool, 8);
    *send_buffer_ptr += 8;
    memcpy(*send_buffer_ptr, &firstQuery, 4);
    *send_buffer_ptr += 4;
    memcpy(*send_buffer_ptr, &queryCount, 4);
    *send_buffer_ptr += 4;
    
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID,
              FUNID_vkCmdResetQueryPool, false, commandBuffer);
}

VKAPI_ATTR void CmdWriteTimestamp(VkCommandBuffer commandBuffer,
                                 VkPipelineStageFlagBits pipelineStage,
                                 VkQueryPool queryPool,
                                 uint32_t query) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    mgr.addParam64((uint64_t)(uintptr_t)commandBuffer);
    mgr.addParam32(pipelineStage);
    mgr.addParam64((uint64_t)(uintptr_t)queryPool);
    mgr.addParam32(query);
    
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID,
              FUNID_vkCmdWriteTimestamp, false, commandBuffer);
}


VKAPI_ATTR void CmdCopyQueryPoolResults(VkCommandBuffer commandBuffer,
                                        VkQueryPool queryPool,
                                        uint32_t firstQuery,
                                        uint32_t queryCount,
                                        VkBuffer dstBuffer,
                                        VkDeviceSize dstOffset,
                                        VkDeviceSize stride,
                                        VkQueryResultFlags flags) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    mgr.addParam64((uint64_t)(uintptr_t)commandBuffer);
    mgr.addParam64((uint64_t)(uintptr_t)queryPool);
    mgr.addParam32(firstQuery);
    mgr.addParam32(queryCount);
    mgr.addParam64((uint64_t)(uintptr_t)dstBuffer);
    mgr.addParam64(dstOffset);
    mgr.addParam64(stride);
    mgr.addParam32(flags);
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdCopyQueryPoolResults, false, commandBuffer);
}

VKAPI_ATTR void CmdPushConstants(VkCommandBuffer commandBuffer,
                                 VkPipelineLayout layout,
                                 VkShaderStageFlags stageFlags,
                                 uint32_t offset,
                                 uint32_t size,
                                 const void* pValues) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    size_t count = sizeof(uint64_t) * 2 + sizeof(uint32_t) * 3 + size;
    
    char* send_buffer = (char*)mgr.addExternalParamPtr(count);
    uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;
    
    uint64_t guest_commandBuffer = (uint64_t)(uintptr_t)commandBuffer;
    uint64_t guest_layout = (uint64_t)(uintptr_t)layout;
    
    memcpy(*send_buffer_ptr, &guest_commandBuffer, sizeof(uint64_t));
    *send_buffer_ptr += sizeof(uint64_t);
    memcpy(*send_buffer_ptr, &guest_layout, sizeof(uint64_t));
    *send_buffer_ptr += sizeof(uint64_t);
    memcpy(*send_buffer_ptr, &stageFlags, sizeof(uint32_t));
    *send_buffer_ptr += sizeof(uint32_t);
    memcpy(*send_buffer_ptr, &offset, sizeof(uint32_t));
    *send_buffer_ptr += sizeof(uint32_t);
    memcpy(*send_buffer_ptr, &size, sizeof(uint32_t));
    *send_buffer_ptr += sizeof(uint32_t);
    memcpy(*send_buffer_ptr, pValues, size);
    *send_buffer_ptr += size;
    
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdPushConstants, false, commandBuffer);
}

VKAPI_ATTR void CmdNextSubpass(VkCommandBuffer commandBuffer,
                               VkSubpassContents contents) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    mgr.addParam64((uint64_t)(uintptr_t)commandBuffer);
    mgr.addParam32((uint32_t)contents);
    
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdNextSubpass, false, commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL CmdEndRenderPass(VkCommandBuffer commandBuffer) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    mgr.addParam64((uint64_t)(uintptr_t)commandBuffer);
    
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdEndRenderPass, false, commandBuffer);
    
    EVK_PER_CALL_LOG("CmdEndRenderPass cmd=%lld", (long long)commandBuffer);
}

VKAPI_ATTR void CmdExecuteCommands(VkCommandBuffer commandBuffer,
                                  uint32_t commandBufferCount,
                                  const VkCommandBuffer* pCommandBuffers) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    size_t count = 4 + 8 * commandBufferCount;
    uint64_t guest_cmd_buffer = (uint64_t)(uintptr_t)commandBuffer;
    mgr.addParam64(guest_cmd_buffer);

    char* send_buffer = (char*)mgr.addExternalParamPtr(count);
    uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;
    
    memcpy(*send_buffer_ptr, &commandBufferCount, 4);
    *send_buffer_ptr += 4;
    
    for (uint32_t i = 0; i < commandBufferCount; ++i) {
        uint64_t guest_secondary_cmd = (uint64_t)(uintptr_t)pCommandBuffers[i];
        memcpy(*send_buffer_ptr, &guest_secondary_cmd, 8);
        *send_buffer_ptr += 8;
    }
    
    FlimeGuestCmdExecuteCommands(FUNID_vkCmdExecuteCommands,
                                  commandBuffer,
                                  commandBufferCount,
                                  pCommandBuffers,
                                  sizeof(uint64_t) + count);
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdExecuteCommands, false);
}

void DestroyDebugReportCallbackEXT(VkInstance instance, VkDebugReportCallbackEXT callback, const VkAllocationCallbacks* pAllocator) {
}

void DebugReportMessageEXT(VkInstance instance, VkDebugReportFlagsEXT flags, VkDebugReportObjectTypeEXT objectType, uint64_t object, size_t location, int32_t messageCode, const char* pLayerPrefix, const char* pMessage) {
}

VKAPI_ATTR VkResult BindBufferMemory2(VkDevice device,
                                      uint32_t bindInfoCount,
                                      const VkBindBufferMemoryInfo* pBindInfos) {
    EVK_PER_CALL_LOG("BindBufferMemory2: device=%p, bindInfoCount=%d, pBindInfos=%p",
                     device, bindInfoCount, pBindInfos);
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    size_t count = 0;
    size_t* countPtr = &count;
    for (uint32_t i = 0; i < bindInfoCount; ++i) {
        count_VkBindBufferMemoryInfo(0, VK_STRUCTURE_TYPE_MAX_ENUM, &pBindInfos[i], countPtr);
    }
    count += sizeof(uint32_t);
    
    char* send_buffer = (char*)mgr.addExternalParamPtr(count);
    uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;
    
    memcpy(*send_buffer_ptr, &bindInfoCount, sizeof(uint32_t));
    *send_buffer_ptr += sizeof(uint32_t);
    
    for (uint32_t i = 0; i < bindInfoCount; ++i) {
        encode_to_stream_VkBindBufferMemoryInfo(VK_STRUCTURE_TYPE_MAX_ENUM, &pBindInfos[i], send_buffer_ptr);
    }
    
    mgr.addParam64((uint64_t)(uintptr_t)device);
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkBindBufferMemory2, false);

    for (uint32_t i = 0; i < bindInfoCount; ++i) {
        TrackBufferMemoryBinding(device,
                                 pBindInfos[i].buffer,
                                 pBindInfos[i].memory,
                                 pBindInfos[i].memoryOffset);
    }
    
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult BindImageMemory2(VkDevice device,
                                      uint32_t bindInfoCount,
                                      const VkBindImageMemoryInfo* pBindInfos) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;

    size_t count = 0;
    size_t* countPtr = &count;

    count += sizeof(uint64_t); // device
    count += sizeof(uint32_t); // bindInfoCount
    
    for (uint32_t i = 0; i < bindInfoCount; ++i) {
        count_VkBindImageMemoryInfo(0, VK_STRUCTURE_TYPE_MAX_ENUM,
                                   (VkBindImageMemoryInfo*)(&pBindInfos[i]), countPtr);
    }
    
    char* send_buffer = (char*)mgr.addExternalParamPtr(count);
    uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;

    uint64_t guest_device = (uint64_t)(uintptr_t)device;
    memcpy(*send_buffer_ptr, &guest_device, sizeof(uint64_t));
    *send_buffer_ptr += sizeof(uint64_t);
    
    memcpy(*send_buffer_ptr, &bindInfoCount, sizeof(uint32_t));
    *send_buffer_ptr += sizeof(uint32_t);
    
    for (uint32_t i = 0; i < bindInfoCount; ++i) {
        encode_to_stream_VkBindImageMemoryInfo(VK_STRUCTURE_TYPE_MAX_ENUM,
                                              (VkBindImageMemoryInfo*)(&pBindInfos[i]), send_buffer_ptr);
    }
    
    VkResult vkResult = VK_SUCCESS;
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkBindImageMemory2, false);

    EVK_PER_CALL_LOG("BindImageMemory2: device=%p, bindInfoCount=%d, pBindInfos=%p",
                     device, bindInfoCount, pBindInfos);
    
    return vkResult;
}

VKAPI_ATTR VkResult BindBufferMemory2KHR(VkDevice device,
                                         uint32_t bindInfoCount,
                                         const VkBindBufferMemoryInfo* pBindInfos) {
    return BindBufferMemory2(device, bindInfoCount, pBindInfos);
}

VKAPI_ATTR VkResult BindImageMemory2KHR(VkDevice device,
                                        uint32_t bindInfoCount,
                                        const VkBindImageMemoryInfo* pBindInfos) {
    return BindImageMemory2(device, bindInfoCount, pBindInfos);
}

VKAPI_ATTR void GetDeviceGroupPeerMemoryFeatures(VkDevice device, 
                                                  uint32_t heapIndex,
                                                  uint32_t localDeviceIndex,
                                                  uint32_t remoteDeviceIndex,
                                                  VkPeerMemoryFeatureFlags* pPeerMemoryFeatures) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    mgr.addParam64((uint64_t)(uintptr_t)device);
    mgr.addParam32(heapIndex);
    mgr.addParam32(localDeviceIndex);
    mgr.addParam32(remoteDeviceIndex);
    
    mgr.addPtr(pPeerMemoryFeatures, sizeof(VkPeerMemoryFeatureFlags));
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkGetDeviceGroupPeerMemoryFeatures, true);
}

VKAPI_ATTR void CmdSetDeviceMask(VkCommandBuffer commandBuffer, uint32_t deviceMask) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    uint64_t guest_cmd_buffer = (uint64_t)(uintptr_t)commandBuffer;
    mgr.addParam64(guest_cmd_buffer);
    mgr.addParam32(deviceMask);
    
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdSetDeviceMask, false, commandBuffer);
}

VKAPI_ATTR void CmdDispatchBase(VkCommandBuffer commandBuffer,
                               uint32_t baseGroupX,
                               uint32_t baseGroupY,
                               uint32_t baseGroupZ,
                               uint32_t groupCountX,
                               uint32_t groupCountY,
                               uint32_t groupCountZ) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    uint64_t guest_cmd_buffer = (uint64_t)(uintptr_t)commandBuffer;
    mgr.addParam64(guest_cmd_buffer);
    mgr.addParam32(baseGroupX);
    mgr.addParam32(baseGroupY);
    mgr.addParam32(baseGroupZ);
    mgr.addParam32(groupCountX);
    mgr.addParam32(groupCountY);
    mgr.addParam32(groupCountZ);
    
    const uint64_t record_start_us = ExpressVkNowUs();
    FlimeGuestCmdDispatchBase(FUNID_vkCmdDispatchBase,
                              commandBuffer,
                              baseGroupX,
                              baseGroupY,
                              baseGroupZ,
                              groupCountX,
                              groupCountY,
                              groupCountZ,
                              sizeof(uint64_t) + 6 * sizeof(uint32_t));
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdDispatchBase, false);
    const uint64_t groups =
        (uint64_t)groupCountX * (uint64_t)groupCountY * (uint64_t)groupCountZ;
    LlmVkTimingNoteCmdDispatch(groups, ExpressVkNowUs() - record_start_us);
}

VKAPI_ATTR VkResult EnumeratePhysicalDeviceGroups(VkInstance instance,
                                                  uint32_t* pPhysicalDeviceGroupCount,
                                                  VkPhysicalDeviceGroupProperties* pPhysicalDeviceGroupProperties) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    mgr.addParam64((uint64_t)(uintptr_t)instance);
    mgr.addPtr(pPhysicalDeviceGroupCount, sizeof(uint32_t));
    
    if (pPhysicalDeviceGroupProperties) {
        mgr.addPtr(pPhysicalDeviceGroupProperties, sizeof(VkPhysicalDeviceGroupProperties) * (*pPhysicalDeviceGroupCount));
    }
    
    VkResult vkResult = VK_SUCCESS;
    mgr.addPtr(&vkResult, sizeof(VkResult));
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkEnumeratePhysicalDeviceGroups, true);
    
    return vkResult;
}

VKAPI_ATTR void GetImageMemoryRequirements2(VkDevice device,
                                           const VkImageMemoryRequirementsInfo2* pInfo,
                                           VkMemoryRequirements2* pMemoryRequirements) {
    ALOGI("GetImageMemoryRequirements2: device=%p, pInfo=%p, pMemoryRequirements=%p",
          device, pInfo, pMemoryRequirements);
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    size_t count = 0;
    size_t* countPtr = &count;
    count_VkImageMemoryRequirementsInfo2(0, VK_STRUCTURE_TYPE_MAX_ENUM, 
                                        (VkImageMemoryRequirementsInfo2*)pInfo, countPtr);
    count_VkMemoryRequirements2(0, VK_STRUCTURE_TYPE_MAX_ENUM,
                               (VkMemoryRequirements2*)pMemoryRequirements, countPtr);
    
    char* send_buffer = (char*)mgr.addExternalParamPtr(count);
    uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;
    
    encode_to_stream_VkImageMemoryRequirementsInfo2(VK_STRUCTURE_TYPE_MAX_ENUM,
                                                    (VkImageMemoryRequirementsInfo2*)pInfo, send_buffer_ptr);
    encode_to_stream_VkMemoryRequirements2(VK_STRUCTURE_TYPE_MAX_ENUM,
                                          (VkMemoryRequirements2*)pMemoryRequirements, send_buffer_ptr);
    
    mgr.addParam64((uint64_t)(uintptr_t)device);
    mgr.addPtr(pMemoryRequirements, sizeof(VkMemoryRequirements2));
    
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkGetImageMemoryRequirements2, true);
}

VKAPI_ATTR void GetBufferMemoryRequirements2(VkDevice device,
                                            const VkBufferMemoryRequirementsInfo2* pInfo,
                                            VkMemoryRequirements2* pMemoryRequirements) {
    ALOGI("GetBufferMemoryRequirements2: device=%p, pInfo=%p, pMemoryRequirements=%p",
          device, pInfo, pMemoryRequirements);
    int express_gpu = get_express_gpu_fd();
    ParamManager mgr;
    Allocator vkAllocator;
    
    VkBufferMemoryRequirementsInfo2* local_pInfo = nullptr;
    if (pInfo) {
        local_pInfo = (VkBufferMemoryRequirementsInfo2*)malloc(sizeof(VkBufferMemoryRequirementsInfo2));
        deepcopy_VkBufferMemoryRequirementsInfo2(&vkAllocator, VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2, pInfo, local_pInfo);
    }

    mgr.addParam64((uint64_t)(uintptr_t)device);

    size_t count = 0;
    size_t* countPtr = &count;
    count_VkBufferMemoryRequirementsInfo2(0, VK_STRUCTURE_TYPE_MAX_ENUM, local_pInfo, countPtr);
    // count += 8;
    
    char* send_buffer = (char*)mgr.addExternalParamPtr(count);
    uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;
    
    encode_to_stream_VkBufferMemoryRequirementsInfo2(VK_STRUCTURE_TYPE_MAX_ENUM, local_pInfo, send_buffer_ptr);
    
    mgr.addPtr(pMemoryRequirements, sizeof(VkMemoryRequirements2));
    
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkGetBufferMemoryRequirements2, true);
    
    if (local_pInfo) free(local_pInfo);
}

VKAPI_ATTR void GetImageSparseMemoryRequirements2(VkDevice device,
                                                  const VkImageSparseMemoryRequirementsInfo2* pInfo,
                                                  uint32_t* pSparseMemoryRequirementCount,
                                                  VkSparseImageMemoryRequirements2* pSparseMemoryRequirements) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    Allocator vkAllocator;
    
    VkImageSparseMemoryRequirementsInfo2* local_pInfo = nullptr;
    if (pInfo) {
        local_pInfo = (VkImageSparseMemoryRequirementsInfo2*)malloc(sizeof(VkImageSparseMemoryRequirementsInfo2));
        if (!local_pInfo) {
            return;
        }
        deepcopy_VkImageSparseMemoryRequirementsInfo2(&vkAllocator, VK_STRUCTURE_TYPE_MAX_ENUM, pInfo, local_pInfo);
    }
    
    size_t count = 0;
    count_VkImageSparseMemoryRequirementsInfo2(0, VK_STRUCTURE_TYPE_MAX_ENUM, local_pInfo, &count);
    count += sizeof(uint64_t);
    
    char* send_buffer = (char*)mgr.addExternalParamPtr(count);
    uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;
    
    uint64_t guest_device = (uint64_t)(uintptr_t)device;
    memcpy(*send_buffer_ptr, &guest_device, sizeof(uint64_t));
    *send_buffer_ptr += sizeof(uint64_t);
    
    encode_to_stream_VkImageSparseMemoryRequirementsInfo2(VK_STRUCTURE_TYPE_MAX_ENUM, local_pInfo, send_buffer_ptr);
    
    mgr.addPtr(pSparseMemoryRequirementCount, sizeof(uint32_t));
    
    if (pSparseMemoryRequirements) {
        mgr.addPtr(pSparseMemoryRequirements, sizeof(VkSparseImageMemoryRequirements2) * (*pSparseMemoryRequirementCount));
    }
    
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkGetImageSparseMemoryRequirements2, true);
    
    if (local_pInfo) free(local_pInfo);
}

VKAPI_ATTR void GetPhysicalDeviceFeatures2(VkPhysicalDevice physicalDevice,
                                          VkPhysicalDeviceFeatures2* pFeatures) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    mgr.addParam64((uint64_t)(uintptr_t)physicalDevice);
    mgr.addPtr(pFeatures, sizeof(VkPhysicalDeviceFeatures2));
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkGetPhysicalDeviceFeatures2, true);
}

void GetPhysicalDeviceProperties2(VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties2* pProperties) {
    if (!pProperties) {
        ALOGE("GetPhysicalDeviceProperties2 called with null pProperties");
        return;
    }

    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    Allocator vkAllocator;

    VkPhysicalDeviceProperties2* local_pProperties =
        (VkPhysicalDeviceProperties2*)malloc(sizeof(VkPhysicalDeviceProperties2));
    if (!local_pProperties) {
        ALOGE("GetPhysicalDeviceProperties2: OOM allocating local_pProperties");
        return;
    }
    // Deep-copy caller-provided structure to preserve/serialize pNext query chain.
    deepcopy_VkPhysicalDeviceProperties2(
        &vkAllocator,
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        pProperties,
        local_pProperties);

    size_t count = 0;
    count_VkPhysicalDeviceProperties2(
        0,
        VK_STRUCTURE_TYPE_MAX_ENUM,
        local_pProperties,
        &count);
    count += sizeof(uint64_t);

    char* send_buffer = (char*)mgr.addExternalParamPtr(count);
    uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;

    uint64_t guest_device = (uint64_t)(uintptr_t)physicalDevice;
    memcpy(*send_buffer_ptr, &guest_device, sizeof(uint64_t));
    *send_buffer_ptr += sizeof(uint64_t);

    encode_to_stream_VkPhysicalDeviceProperties2(
        VK_STRUCTURE_TYPE_MAX_ENUM,
        local_pProperties,
        send_buffer_ptr);

    mgr.addPtr(pProperties, sizeof(VkPhysicalDeviceProperties2));
    ALOGI("GetPhysicalDeviceProperties2 pNext null? %s",
          (pProperties->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 &&
           pProperties->pNext == nullptr)
              ? "yes"
              : "no");
    FlimeGuestWrite(&mgr,
        express_gpu,
        EXPRESS_GPU_DEVICE_ID,
        FUNID_vkGetPhysicalDeviceProperties2,
        true);

    free(local_pProperties);

    ALOGI("GetPhysicalDeviceProperties2 returned deviceName=%s, apiVersion=%u, driverVersion=%u, vendorID=%u, deviceID=%u",
          pProperties->properties.deviceName,
          pProperties->properties.apiVersion,
          pProperties->properties.driverVersion,
          pProperties->properties.vendorID,
          pProperties->properties.deviceID);
}

void GetPhysicalDeviceFormatProperties2(VkPhysicalDevice physicalDevice, VkFormat format, VkFormatProperties2* pFormatProperties) {
    ALOGI("GetPhysicalDeviceFormatProperties2: physicalDevice=%p, format=%u, pFormatProperties=%p",
          physicalDevice, format, pFormatProperties);
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    mgr.addParam64((uint64_t)(uintptr_t)physicalDevice);
    mgr.addParam32(format);
    mgr.addPtr(pFormatProperties, sizeof(VkFormatProperties2));
    FlimeGuestWrite(&mgr,
        express_gpu,
        EXPRESS_GPU_DEVICE_ID,
        FUNID_vkGetPhysicalDeviceFormatProperties2,
        true);
    ALOGI("get properties: linearTilingFeatures=%u, optimalTilingFeatures=%u, bufferFeatures=%u",
          pFormatProperties->formatProperties.linearTilingFeatures,
          pFormatProperties->formatProperties.optimalTilingFeatures,
          pFormatProperties->formatProperties.bufferFeatures);
}

void GetPhysicalDeviceQueueFamilyProperties2(VkPhysicalDevice physicalDevice, uint32_t* pQueueFamilyPropertyCount, VkQueueFamilyProperties2* pQueueFamilyProperties) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;

    mgr.addParam64((uint64_t)(uintptr_t)physicalDevice);
    mgr.addPtr(pQueueFamilyPropertyCount, sizeof(uint32_t));
    mgr.addPtr(pQueueFamilyProperties, sizeof(VkQueueFamilyProperties2) * (*pQueueFamilyPropertyCount));
    FlimeGuestWrite(&mgr,
        express_gpu,
        EXPRESS_GPU_DEVICE_ID,
        FUNID_vkGetPhysicalDeviceQueueFamilyProperties2,
        true);
    ALOGI("GetPhysicalDeviceQueueFamilyProperties2: returned values: pQueueFamilyPropertyCount=%u, pQueueFamilyProperties=%p",
          *pQueueFamilyPropertyCount, pQueueFamilyProperties);
}

void GetPhysicalDeviceMemoryProperties2(VkPhysicalDevice physicalDevice, VkPhysicalDeviceMemoryProperties2* pMemoryProperties) {
    LOGI("GetPhysicalDeviceMemoryProperties2: physicalDevice=%p, pMemoryProperties=%p", physicalDevice, pMemoryProperties);
    LOGI("sizeof VkPhysicalDeviceMemoryProperties2=%zu", sizeof(VkPhysicalDeviceMemoryProperties2));
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;

    mgr.addParam64((uint64_t)(uintptr_t)physicalDevice);
    mgr.addPtr(pMemoryProperties, sizeof(VkPhysicalDeviceMemoryProperties2));
    LOGI("before call: memoryTypeCount=%u, memoryTypes=%p, memoryHeapCount=%u, memoryHeaps=%p",
          pMemoryProperties->memoryProperties.memoryTypeCount,
          pMemoryProperties->memoryProperties.memoryTypes,
          pMemoryProperties->memoryProperties.memoryHeapCount,
          pMemoryProperties->memoryProperties.memoryHeaps);
    FlimeGuestWrite(&mgr,
        express_gpu,
        EXPRESS_GPU_DEVICE_ID,
        FUNID_vkGetPhysicalDeviceMemoryProperties2,
        true);

    LOGI("after call: memoryTypeCount=%u, memoryTypes=%p, memoryHeapCount=%u, memoryHeaps=%p",
          pMemoryProperties->memoryProperties.memoryTypeCount,
          pMemoryProperties->memoryProperties.memoryTypes,
          pMemoryProperties->memoryProperties.memoryHeapCount,
          pMemoryProperties->memoryProperties.memoryHeaps);
}

VKAPI_ATTR void GetPhysicalDeviceSparseImageFormatProperties2(VkPhysicalDevice physicalDevice,
                                                            const VkPhysicalDeviceSparseImageFormatInfo2* pFormatInfo,
                                                            uint32_t* pPropertyCount,
                                                            VkSparseImageFormatProperties2* pProperties) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    Allocator vkAllocator;
    
    VkPhysicalDeviceSparseImageFormatInfo2* local_pFormatInfo = nullptr;
    if (pFormatInfo) {
        local_pFormatInfo = (VkPhysicalDeviceSparseImageFormatInfo2*)malloc(sizeof(VkPhysicalDeviceSparseImageFormatInfo2));
        if (!local_pFormatInfo) {
            return;
        }
        deepcopy_VkPhysicalDeviceSparseImageFormatInfo2(&vkAllocator, VK_STRUCTURE_TYPE_MAX_ENUM, pFormatInfo, local_pFormatInfo);
    }
    
    size_t count = 0;
    count_VkPhysicalDeviceSparseImageFormatInfo2(0, VK_STRUCTURE_TYPE_MAX_ENUM, local_pFormatInfo, &count);
    count += sizeof(uint64_t);
    
    char* send_buffer = (char*)mgr.addExternalParamPtr(count);
    uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;
    
    uint64_t guest_device = (uint64_t)(uintptr_t)physicalDevice;
    memcpy(*send_buffer_ptr, &guest_device, sizeof(uint64_t));
    *send_buffer_ptr += sizeof(uint64_t);
    
    encode_to_stream_VkPhysicalDeviceSparseImageFormatInfo2(VK_STRUCTURE_TYPE_MAX_ENUM, local_pFormatInfo, send_buffer_ptr);
    
    mgr.addPtr(pPropertyCount, sizeof(uint32_t));
    
    if (pProperties) {
        mgr.addPtr(pProperties, sizeof(VkSparseImageFormatProperties2) * (*pPropertyCount));
    }
    
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkGetPhysicalDeviceSparseImageFormatProperties2, true);
    
    if (local_pFormatInfo) free(local_pFormatInfo);
}

VKAPI_ATTR void TrimCommandPool(VkDevice device,
                               VkCommandPool commandPool,
                               VkCommandPoolTrimFlags flags) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    uint64_t guest_device = (uint64_t)(uintptr_t)device;
    uint64_t guest_pool = (uint64_t)(uintptr_t)commandPool;
    
    mgr.addParam64(guest_device);
    mgr.addParam64(guest_pool);
    mgr.addParam32(flags);
    
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkTrimCommandPool, false);
}

VKAPI_ATTR void GetDeviceQueue2(VkDevice device,
                               const VkDeviceQueueInfo2* pQueueInfo,
                               VkQueue* pQueue) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    Allocator vkAllocator;
    
    VkDeviceQueueInfo2* local_pQueueInfo = nullptr;
    if (pQueueInfo) {
        local_pQueueInfo = (VkDeviceQueueInfo2*)malloc(sizeof(VkDeviceQueueInfo2));
        deepcopy_VkDeviceQueueInfo2(&vkAllocator, VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2, pQueueInfo, local_pQueueInfo);
    }
    
    size_t count = 0;
    size_t* countPtr = &count;
    count_VkDeviceQueueInfo2(0, VK_STRUCTURE_TYPE_MAX_ENUM, local_pQueueInfo, countPtr);
    count += 8;
    
    char* send_buffer = (char*)mgr.addExternalParamPtr(count);
    uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;
    
    mgr.addParam64((uint64_t)(uintptr_t)device);
    encode_to_stream_VkDeviceQueueInfo2(VK_STRUCTURE_TYPE_MAX_ENUM, local_pQueueInfo, send_buffer_ptr);
    
    const VkAllocationCallbacks* alloc = &kDefaultAllocCallbacks;
    VkQueue_T* queue = static_cast<VkQueue_T*>(alloc->pfnAllocation(
        alloc->pUserData, sizeof(VkQueue_T), alignof(VkQueue_T),
        VK_SYSTEM_ALLOCATION_SCOPE_DEVICE));
    if (queue == nullptr) {
        *pQueue = VK_NULL_HANDLE;
        if (local_pQueueInfo) free(local_pQueueInfo);
        return;
    }
    queue->dispatch.magic = HWVULKAN_DISPATCH_MAGIC;
    *pQueue = queue;
    
    mgr.addParam64((uint64_t)(uintptr_t)*pQueue);
    
    const ssize_t written = FlimeGuestWrite(
        &mgr, express_gpu, EXPRESS_GPU_DEVICE_ID,
        FUNID_vkGetDeviceQueue2, true);
    if (IsCompleteParamManagerWrite(written, 1)) {
        FlimeGuestRegisterQueue(device, *pQueue);
    } else {
        alloc->pfnFree(alloc->pUserData, queue);
        *pQueue = VK_NULL_HANDLE;
    }
    
    if (local_pQueueInfo) free(local_pQueueInfo);
}

VKAPI_ATTR VkResult CreateSamplerYcbcrConversion(VkDevice device,
                                                const VkSamplerYcbcrConversionCreateInfo* pCreateInfo,
                                                const VkAllocationCallbacks* pAllocator,
                                                VkSamplerYcbcrConversion* pYcbcrConversion) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    Allocator vkAllocator;
    
    VkSamplerYcbcrConversionCreateInfo* local_pCreateInfo = nullptr;
    if (pCreateInfo) {
        local_pCreateInfo = (VkSamplerYcbcrConversionCreateInfo*)malloc(sizeof(VkSamplerYcbcrConversionCreateInfo));
        if (!local_pCreateInfo) return VK_ERROR_OUT_OF_HOST_MEMORY;
        deepcopy_VkSamplerYcbcrConversionCreateInfo(&vkAllocator, VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO,
                                                   pCreateInfo, local_pCreateInfo);
    }
    
    VkAllocationCallbacks* local_pAllocator = nullptr;
    if (pAllocator) {
        local_pAllocator = (VkAllocationCallbacks*)malloc(sizeof(VkAllocationCallbacks));
        if (!local_pAllocator) {
            if (local_pCreateInfo) free(local_pCreateInfo);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        deepcopy_VkAllocationCallbacks(&vkAllocator, VK_STRUCTURE_TYPE_MAX_ENUM, pAllocator, local_pAllocator);
    }
    
    size_t count = 0;
    size_t* countPtr = &count;
    count_VkSamplerYcbcrConversionCreateInfo(0, VK_STRUCTURE_TYPE_MAX_ENUM, local_pCreateInfo, countPtr);
    if (pAllocator) {
        count_VkAllocationCallbacks(0, VK_STRUCTURE_TYPE_MAX_ENUM, local_pAllocator, countPtr);
    }
    count += sizeof(uint64_t) * 2 + sizeof(uint8_t);
    
    char* send_buffer = (char*)mgr.addExternalParamPtr(count);
    uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;
    
    uint64_t guest_device = (uint64_t)(uintptr_t)device;
    uint64_t guest_allocator = (uint64_t)(uintptr_t)pAllocator;
    
    memcpy(*send_buffer_ptr, &guest_device, sizeof(uint64_t)); *send_buffer_ptr += sizeof(uint64_t);
    encode_to_stream_VkSamplerYcbcrConversionCreateInfo(VK_STRUCTURE_TYPE_MAX_ENUM, local_pCreateInfo, send_buffer_ptr);
    memcpy(*send_buffer_ptr, &guest_allocator, sizeof(uint64_t)); *send_buffer_ptr += sizeof(uint64_t);
    
    uint8_t has_allocator = pAllocator ? 1 : 0;
    memcpy(*send_buffer_ptr, &has_allocator, sizeof(uint8_t)); *send_buffer_ptr += sizeof(uint8_t);
    if (pAllocator) {
        encode_to_stream_VkAllocationCallbacks(VK_STRUCTURE_TYPE_MAX_ENUM, local_pAllocator, send_buffer_ptr);
    }
    
    const VkAllocationCallbacks* useAlloc = pAllocator ? pAllocator : &kDefaultAllocCallbacks;
    VkSamplerYcbcrConversion_T* conversion = static_cast<VkSamplerYcbcrConversion_T*>(useAlloc->pfnAllocation(
        useAlloc->pUserData, sizeof(VkSamplerYcbcrConversion_T), alignof(VkSamplerYcbcrConversion_T), 
        VK_SYSTEM_ALLOCATION_SCOPE_OBJECT));
    if (!conversion) {
        if (local_pCreateInfo) free(local_pCreateInfo);
        if (local_pAllocator) free(local_pAllocator);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    *pYcbcrConversion = (VkSamplerYcbcrConversion)conversion;
    
    mgr.addPtr(pYcbcrConversion, sizeof(VkSamplerYcbcrConversion));
    VkResult vkResult = VK_SUCCESS;
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCreateSamplerYcbcrConversion, false);
    
    if (local_pCreateInfo) free(local_pCreateInfo);
    if (local_pAllocator) free(local_pAllocator);
    return vkResult;
}



struct DescriptorUpdateTemplateInfo {
    size_t data_size;
    std::vector<VkDescriptorUpdateTemplateEntry> entries;
};

static std::unordered_map<VkDescriptorUpdateTemplate, DescriptorUpdateTemplateInfo> g_template_info_map;
static std::mutex g_template_map_mutex;

static void ForgetDescriptorUpdateTemplateInfo(
    VkDescriptorUpdateTemplate descriptor_update_template) {
    std::lock_guard<std::mutex> lock(g_template_map_mutex);
    g_template_info_map.erase(descriptor_update_template);
}


static size_t calculate_descriptor_update_data_size(const VkDescriptorUpdateTemplateCreateInfo* pCreateInfo) {
    if (!pCreateInfo || !pCreateInfo->pDescriptorUpdateEntries || pCreateInfo->descriptorUpdateEntryCount == 0) {
        return 256;
    }
    
    size_t total_size = 0;
    for (uint32_t i = 0; i < pCreateInfo->descriptorUpdateEntryCount; ++i) {
        const VkDescriptorUpdateTemplateEntry* entry = &pCreateInfo->pDescriptorUpdateEntries[i];
        
        size_t descriptor_size = 0;
        
        switch (entry->descriptorType) {
            case VK_DESCRIPTOR_TYPE_SAMPLER:
            case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
            case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
            case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
            case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
                descriptor_size = sizeof(VkDescriptorImageInfo);
                break;
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
                descriptor_size = sizeof(VkDescriptorBufferInfo);
                break;
            case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
                descriptor_size = sizeof(VkBufferView);
                break;
            case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
                descriptor_size = sizeof(VkAccelerationStructureKHR);
                break;
            default:
                descriptor_size = sizeof(VkDescriptorImageInfo);
                break;
        }
        
        if (entry->descriptorCount == 0) {
            continue;
        }

        size_t stride = entry->stride != 0 ? entry->stride : descriptor_size;
        size_t last_offset = entry->offset + size_t(entry->descriptorCount - 1) * stride;
        size_t entry_end = last_offset + descriptor_size;
        
        if (entry_end > total_size) {
            total_size = entry_end;
        }
    }
    
    return total_size > 0 ? total_size : 256;
}

VKAPI_ATTR VkResult CreateDescriptorUpdateTemplate(VkDevice device,
                                                   const VkDescriptorUpdateTemplateCreateInfo* pCreateInfo,
                                                   const VkAllocationCallbacks* pAllocator,
                                                   VkDescriptorUpdateTemplate* pDescriptorUpdateTemplate) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    Allocator vkAllocator;
    
    VkDescriptorUpdateTemplateCreateInfo* local_pCreateInfo = nullptr;
    if (pCreateInfo) {
        local_pCreateInfo = (VkDescriptorUpdateTemplateCreateInfo*)malloc(sizeof(VkDescriptorUpdateTemplateCreateInfo));
        if (!local_pCreateInfo) return VK_ERROR_OUT_OF_HOST_MEMORY;
        deepcopy_VkDescriptorUpdateTemplateCreateInfo(&vkAllocator, VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO,
                                                     pCreateInfo, local_pCreateInfo);
    }
    
    VkAllocationCallbacks* local_pAllocator = nullptr;
    if (pAllocator) {
        local_pAllocator = (VkAllocationCallbacks*)malloc(sizeof(VkAllocationCallbacks));
        if (!local_pAllocator) {
            if (local_pCreateInfo) free(local_pCreateInfo);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        deepcopy_VkAllocationCallbacks(&vkAllocator, VK_STRUCTURE_TYPE_MAX_ENUM, pAllocator, local_pAllocator);
    }
    
    size_t data_size = calculate_descriptor_update_data_size(local_pCreateInfo);
    
    size_t count = 0;
    size_t* countPtr = &count;
    count_VkDescriptorUpdateTemplateCreateInfo(0, VK_STRUCTURE_TYPE_MAX_ENUM, local_pCreateInfo, countPtr);
    if (pAllocator) {
        count_VkAllocationCallbacks(0, VK_STRUCTURE_TYPE_MAX_ENUM, local_pAllocator, countPtr);
    }
    count += sizeof(uint64_t) * 2 + sizeof(uint8_t);
    
    char* send_buffer = (char*)mgr.addExternalParamPtr(count);
    uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;
    
    uint64_t guest_device = (uint64_t)(uintptr_t)device;
    uint64_t guest_allocator = (uint64_t)(uintptr_t)pAllocator;
    
    memcpy(*send_buffer_ptr, &guest_device, sizeof(uint64_t)); *send_buffer_ptr += sizeof(uint64_t);
    encode_to_stream_VkDescriptorUpdateTemplateCreateInfo(VK_STRUCTURE_TYPE_MAX_ENUM, local_pCreateInfo, send_buffer_ptr);
    memcpy(*send_buffer_ptr, &guest_allocator, sizeof(uint64_t)); *send_buffer_ptr += sizeof(uint64_t);
    
    uint8_t has_allocator = pAllocator ? 1 : 0;
    memcpy(*send_buffer_ptr, &has_allocator, sizeof(uint8_t)); *send_buffer_ptr += sizeof(uint8_t);
    if (pAllocator) {
        encode_to_stream_VkAllocationCallbacks(VK_STRUCTURE_TYPE_MAX_ENUM, local_pAllocator, send_buffer_ptr);
    }
    
    const VkAllocationCallbacks* useAlloc = pAllocator ? pAllocator : &kDefaultAllocCallbacks;
    VkDescriptorUpdateTemplate_T* template_obj = static_cast<VkDescriptorUpdateTemplate_T*>(useAlloc->pfnAllocation(
        useAlloc->pUserData, sizeof(VkDescriptorUpdateTemplate_T), alignof(VkDescriptorUpdateTemplate_T), 
        VK_SYSTEM_ALLOCATION_SCOPE_OBJECT));
    if (!template_obj) {
        mgr.clear();
        if (local_pCreateInfo) free(local_pCreateInfo);
        if (local_pAllocator) free(local_pAllocator);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    *pDescriptorUpdateTemplate = (VkDescriptorUpdateTemplate)template_obj;
    
    mgr.addPtr(pDescriptorUpdateTemplate, sizeof(VkDescriptorUpdateTemplate));
    VkResult vkResult = VK_ERROR_DEVICE_LOST;
    mgr.addPtr(&vkResult, sizeof(vkResult));
    FlimeGuestBeforeDescriptorLifecycle(device);
    const ssize_t written =
        FlimeGuestWrite(&mgr,
                        express_gpu,
                        EXPRESS_GPU_DEVICE_ID,
                        FUNID_vkCreateDescriptorUpdateTemplate,
                        true);
    const bool transport_ok = IsCompleteParamManagerWrite(written, 3);
    if (!transport_ok) {
        vkResult = VK_ERROR_DEVICE_LOST;
    }
    if (vkResult == VK_SUCCESS) {
        std::lock_guard<std::mutex> lock(g_template_map_mutex);
        DescriptorUpdateTemplateInfo info;
        info.data_size = data_size;
        if (local_pCreateInfo && local_pCreateInfo->pDescriptorUpdateEntries) {
            info.entries.assign(
                local_pCreateInfo->pDescriptorUpdateEntries,
                local_pCreateInfo->pDescriptorUpdateEntries +
                    local_pCreateInfo->descriptorUpdateEntryCount);
        }
        g_template_info_map[*pDescriptorUpdateTemplate] = info;
    }
    FlimeGuestCreateDescriptorUpdateTemplate(
        device,
        pCreateInfo,
        *pDescriptorUpdateTemplate,
        vkResult);
    FlimeGuestAfterDescriptorLifecycle(device, transport_ok);
    if (vkResult != VK_SUCCESS) {
        useAlloc->pfnFree(useAlloc->pUserData, template_obj);
        *pDescriptorUpdateTemplate = VK_NULL_HANDLE;
    }
    
    if (local_pCreateInfo) free(local_pCreateInfo);
    if (local_pAllocator) free(local_pAllocator);
    return vkResult;
}

VKAPI_ATTR void UpdateDescriptorSetWithTemplate(
    VkDevice device,
    VkDescriptorSet descriptorSet,
    VkDescriptorUpdateTemplate descriptorUpdateTemplate,
    const void* pData) {
    const uint64_t start_us = ExpressVkNowUs();
    
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    uint64_t guest_device = (uint64_t)(uintptr_t)device;
    uint64_t guest_descriptor_set = (uint64_t)(uintptr_t)descriptorSet;
    uint64_t guest_template = (uint64_t)(uintptr_t)descriptorUpdateTemplate;
    
    DescriptorUpdateTemplateInfo template_info;
    {
        std::lock_guard<std::mutex> lock(g_template_map_mutex);
        auto it = g_template_info_map.find(descriptorUpdateTemplate);
        if (it == g_template_info_map.end()) {
            ALOGE("UpdateDescriptorSetWithTemplate: template not found");
            return;
        }
        template_info = it->second;
    }
    
    size_t data_size = template_info.data_size;
    uint32_t entry_count = template_info.entries.size();
    
    size_t count = sizeof(uint64_t) * 3 + sizeof(size_t) + sizeof(uint32_t) + 
                   entry_count * sizeof(VkDescriptorUpdateTemplateEntry) + data_size;
    
    char* send_buffer = (char*)mgr.addExternalParamPtr(count);
    uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;
    
    memcpy(*send_buffer_ptr, &guest_device, sizeof(uint64_t));
    *send_buffer_ptr += sizeof(uint64_t);
    
    memcpy(*send_buffer_ptr, &guest_descriptor_set, sizeof(uint64_t));
    *send_buffer_ptr += sizeof(uint64_t);
    
    memcpy(*send_buffer_ptr, &guest_template, sizeof(uint64_t));
    *send_buffer_ptr += sizeof(uint64_t);
    
    memcpy(*send_buffer_ptr, &data_size, sizeof(size_t));
    *send_buffer_ptr += sizeof(size_t);
    
    memcpy(*send_buffer_ptr, &entry_count, sizeof(uint32_t));
    *send_buffer_ptr += sizeof(uint32_t);
    
    for (uint32_t i = 0; i < entry_count; ++i) {
        memcpy(*send_buffer_ptr, &template_info.entries[i], sizeof(VkDescriptorUpdateTemplateEntry));
        *send_buffer_ptr += sizeof(VkDescriptorUpdateTemplateEntry);
    }

    if (pData) {
        std::lock_guard<std::mutex> lock(g_mapped_mutex);
        for (const VkDescriptorUpdateTemplateEntry& entry : template_info.entries) {
            if (!DescriptorTypeUsesBufferInfo(entry.descriptorType)) continue;

            for (uint32_t j = 0; j < entry.descriptorCount; ++j) {
                const uint8_t* entry_ptr =
                    reinterpret_cast<const uint8_t*>(pData) + entry.offset + size_t(j) * entry.stride;
                const VkDescriptorBufferInfo* buffer_info =
                    reinterpret_cast<const VkDescriptorBufferInfo*>(entry_ptr);
                RememberDescriptorBufferUseLocked(
                    descriptorSet,
                    entry.dstBinding,
                    entry.dstArrayElement + j,
                    entry.descriptorType,
                    *buffer_info);
            }
        }
    }

    if (pData) {
        memcpy(*send_buffer_ptr, pData, data_size);
    }

    FlimeGuestBeforeDescriptorLifecycle(device);
    const FlimeGuestUpdateAction flime_action =
        FlimeGuestUpdateDescriptorSetWithTemplate(
            FUNID_vkUpdateDescriptorSetWithTemplate,
            device,
            descriptorSet,
            descriptorUpdateTemplate,
            pData,
            count);
    bool transport_ok = true;
    if (flime_action == FLIME_GUEST_UPDATE_LEGACY) {
        const ssize_t written =
            FlimeGuestWrite(&mgr,
                            express_gpu,
                            EXPRESS_GPU_DEVICE_ID,
                            FUNID_vkUpdateDescriptorSetWithTemplate,
                            true);
        transport_ok = IsCompleteParamManagerWrite(written, 1);
    } else {
        mgr.clear();
        if (flime_action == FLIME_GUEST_UPDATE_FATAL) {
            ALOGE("FLIME rejected vkUpdateDescriptorSetWithTemplate; "
                  "update was not sent");
        }
    }
    FlimeGuestAfterDescriptorLifecycle(device, transport_ok);
    {
        std::lock_guard<std::mutex> lock(g_descriptor_lifecycle_mutex);
        g_descriptor_lifecycle_stats.update_template_calls++;
        g_descriptor_lifecycle_stats.update_template_us += ExpressVkNowUs() - start_us;
        MaybeLogDescriptorLifecycleStatsLocked("periodic");
    }
}

VKAPI_ATTR void GetPhysicalDeviceExternalBufferProperties(VkPhysicalDevice physicalDevice,
                                                         const VkPhysicalDeviceExternalBufferInfo* pExternalBufferInfo,
                                                         VkExternalBufferProperties* pExternalBufferProperties) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    Allocator vkAllocator;
    
    VkPhysicalDeviceExternalBufferInfo* local_pExternalBufferInfo = nullptr;
    if (pExternalBufferInfo) {
        local_pExternalBufferInfo = (VkPhysicalDeviceExternalBufferInfo*)malloc(sizeof(VkPhysicalDeviceExternalBufferInfo));
        if (!local_pExternalBufferInfo) {
            return;
        }
        deepcopy_VkPhysicalDeviceExternalBufferInfo(&vkAllocator, VK_STRUCTURE_TYPE_MAX_ENUM, pExternalBufferInfo, local_pExternalBufferInfo);
    }
    
    size_t count = 0;
    count_VkPhysicalDeviceExternalBufferInfo(0, VK_STRUCTURE_TYPE_MAX_ENUM, local_pExternalBufferInfo, &count);
    count += sizeof(uint64_t);
    
    char* send_buffer = (char*)mgr.addExternalParamPtr(count);
    uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;
    
    uint64_t guest_device = (uint64_t)(uintptr_t)physicalDevice;
    memcpy(*send_buffer_ptr, &guest_device, sizeof(uint64_t));
    *send_buffer_ptr += sizeof(uint64_t);
    
    encode_to_stream_VkPhysicalDeviceExternalBufferInfo(VK_STRUCTURE_TYPE_MAX_ENUM, local_pExternalBufferInfo, send_buffer_ptr);
    
    mgr.addPtr(pExternalBufferProperties, sizeof(VkExternalBufferProperties));
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkGetPhysicalDeviceExternalBufferProperties, true);
    
    if (local_pExternalBufferInfo) free(local_pExternalBufferInfo);
}

VKAPI_ATTR void GetPhysicalDeviceExternalBufferPropertiesKHR(
    VkPhysicalDevice physicalDevice,
    const VkPhysicalDeviceExternalBufferInfo* pExternalBufferInfo,
    VkExternalBufferProperties* pExternalBufferProperties) {
    ALOGV("TODO: vkGetPhysicalDeviceExternalBufferPropertiesKHR unsupported in zvulkan bridge yet");

    // TODO: Forward vkGetPhysicalDeviceExternalBufferPropertiesKHR to host side.
    // For now report unsupported external memory capabilities.
    if (!pExternalBufferProperties) {
        return;
    }

    pExternalBufferProperties->sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES;
    pExternalBufferProperties->pNext = nullptr;
    pExternalBufferProperties->externalMemoryProperties.externalMemoryFeatures = 0;
    pExternalBufferProperties->externalMemoryProperties.exportFromImportedHandleTypes = 0;
    pExternalBufferProperties->externalMemoryProperties.compatibleHandleTypes = 0;

    (void)physicalDevice;
    (void)pExternalBufferInfo;
}

VKAPI_ATTR void GetPhysicalDeviceExternalFenceProperties(VkPhysicalDevice physicalDevice,
                                                        const VkPhysicalDeviceExternalFenceInfo* pExternalFenceInfo,
                                                        VkExternalFenceProperties* pExternalFenceProperties) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    Allocator vkAllocator;
    
    VkPhysicalDeviceExternalFenceInfo* local_pExternalFenceInfo = nullptr;
    if (pExternalFenceInfo) {
        local_pExternalFenceInfo = (VkPhysicalDeviceExternalFenceInfo*)malloc(sizeof(VkPhysicalDeviceExternalFenceInfo));
        if (!local_pExternalFenceInfo) {
            return;
        }
        deepcopy_VkPhysicalDeviceExternalFenceInfo(&vkAllocator, VK_STRUCTURE_TYPE_MAX_ENUM, pExternalFenceInfo, local_pExternalFenceInfo);
    }
    
    size_t count = 0;
    count_VkPhysicalDeviceExternalFenceInfo(0, VK_STRUCTURE_TYPE_MAX_ENUM, local_pExternalFenceInfo, &count);
    count += sizeof(uint64_t);
    
    char* send_buffer = (char*)mgr.addExternalParamPtr(count);
    uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;
    
    uint64_t guest_device = (uint64_t)(uintptr_t)physicalDevice;
    memcpy(*send_buffer_ptr, &guest_device, sizeof(uint64_t));
    *send_buffer_ptr += sizeof(uint64_t);
    
    encode_to_stream_VkPhysicalDeviceExternalFenceInfo(VK_STRUCTURE_TYPE_MAX_ENUM, local_pExternalFenceInfo, send_buffer_ptr);
    
    mgr.addPtr(pExternalFenceProperties, sizeof(VkExternalFenceProperties));
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkGetPhysicalDeviceExternalFenceProperties, true);
    
    if (local_pExternalFenceInfo) free(local_pExternalFenceInfo);
}

VKAPI_ATTR void GetPhysicalDeviceExternalSemaphoreProperties(VkPhysicalDevice physicalDevice,
                                                           const VkPhysicalDeviceExternalSemaphoreInfo* pExternalSemaphoreInfo,
                                                           VkExternalSemaphoreProperties* pExternalSemaphoreProperties) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    Allocator vkAllocator;
    
    VkPhysicalDeviceExternalSemaphoreInfo* local_pExternalSemaphoreInfo = nullptr;
    if (pExternalSemaphoreInfo) {
        local_pExternalSemaphoreInfo = (VkPhysicalDeviceExternalSemaphoreInfo*)malloc(sizeof(VkPhysicalDeviceExternalSemaphoreInfo));
        if (!local_pExternalSemaphoreInfo) {
            return;
        }
        deepcopy_VkPhysicalDeviceExternalSemaphoreInfo(&vkAllocator, VK_STRUCTURE_TYPE_MAX_ENUM, pExternalSemaphoreInfo, local_pExternalSemaphoreInfo);
    }
    
    size_t count = 0;
    count_VkPhysicalDeviceExternalSemaphoreInfo(0, VK_STRUCTURE_TYPE_MAX_ENUM, local_pExternalSemaphoreInfo, &count);
    count += sizeof(uint64_t);
    
    char* send_buffer = (char*)mgr.addExternalParamPtr(count);
    uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;
    
    uint64_t guest_device = (uint64_t)(uintptr_t)physicalDevice;
    memcpy(*send_buffer_ptr, &guest_device, sizeof(uint64_t));
    *send_buffer_ptr += sizeof(uint64_t);
    
    encode_to_stream_VkPhysicalDeviceExternalSemaphoreInfo(VK_STRUCTURE_TYPE_MAX_ENUM, local_pExternalSemaphoreInfo, send_buffer_ptr);
    
    mgr.addPtr(pExternalSemaphoreProperties, sizeof(VkExternalSemaphoreProperties));
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkGetPhysicalDeviceExternalSemaphoreProperties, true);
    
    if (local_pExternalSemaphoreInfo) free(local_pExternalSemaphoreInfo);
}

VKAPI_ATTR void GetDescriptorSetLayoutSupport(VkDevice device, 
                                               const VkDescriptorSetLayoutCreateInfo* pCreateInfo,
                                               VkDescriptorSetLayoutSupport* pSupport) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    Allocator vkAllocator;
    
    VkDescriptorSetLayoutCreateInfo* local_pCreateInfo = nullptr;
    if (pCreateInfo) {
        local_pCreateInfo = (VkDescriptorSetLayoutCreateInfo*)malloc(sizeof(VkDescriptorSetLayoutCreateInfo));
        deepcopy_VkDescriptorSetLayoutCreateInfo(&vkAllocator, VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, pCreateInfo, local_pCreateInfo);
    }
    
    VkDescriptorSetLayoutSupport* local_pSupport = nullptr;
    if (pSupport) {
        local_pSupport = (VkDescriptorSetLayoutSupport*)malloc(sizeof(VkDescriptorSetLayoutSupport));
        deepcopy_VkDescriptorSetLayoutSupport(&vkAllocator, VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_SUPPORT, pSupport, local_pSupport);
    }
    
    size_t count = 0;
    size_t* countPtr = &count;
    count_VkDescriptorSetLayoutCreateInfo(0, VK_STRUCTURE_TYPE_MAX_ENUM, local_pCreateInfo, countPtr);
    count_VkDescriptorSetLayoutSupport(0, VK_STRUCTURE_TYPE_MAX_ENUM, local_pSupport, countPtr);
    count += 8;
    
    char* send_buffer = (char*)mgr.addExternalParamPtr(count);
    uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;
    
    uint64_t cgen_var_0 = (uint64_t)(uintptr_t)device;
    memcpy((*send_buffer_ptr), &cgen_var_0, 8);
    *send_buffer_ptr += 8;
    
    encode_to_stream_VkDescriptorSetLayoutCreateInfo(VK_STRUCTURE_TYPE_MAX_ENUM, local_pCreateInfo, send_buffer_ptr);
    encode_to_stream_VkDescriptorSetLayoutSupport(VK_STRUCTURE_TYPE_MAX_ENUM, local_pSupport, send_buffer_ptr);
    
    mgr.addPtr(pSupport, sizeof(VkDescriptorSetLayoutSupport));
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkGetDescriptorSetLayoutSupport, true);
    
    if (local_pCreateInfo) free(local_pCreateInfo);
    if (local_pSupport) free(local_pSupport);
}

VKAPI_ATTR void CmdBeginRenderPass2(VkCommandBuffer commandBuffer,
                                    const VkRenderPassBeginInfo* pRenderPassBegin,
                                    const VkSubpassBeginInfo* pSubpassBeginInfo) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    Allocator vkAllocator;
    
    VkRenderPassBeginInfo* local_pRenderPassBegin = nullptr;
    VkSubpassBeginInfo* local_pSubpassBeginInfo = nullptr;
    
    if (pRenderPassBegin) {
        local_pRenderPassBegin = (VkRenderPassBeginInfo*)malloc(sizeof(VkRenderPassBeginInfo));
        if (!local_pRenderPassBegin) return;
        deepcopy_VkRenderPassBeginInfo(&vkAllocator, VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, pRenderPassBegin, local_pRenderPassBegin);
    }
    
    if (pSubpassBeginInfo) {
        local_pSubpassBeginInfo = (VkSubpassBeginInfo*)malloc(sizeof(VkSubpassBeginInfo));
        if (!local_pSubpassBeginInfo) {
            if (local_pRenderPassBegin) free(local_pRenderPassBegin);
            return;
        }
        deepcopy_VkSubpassBeginInfo(&vkAllocator, VK_STRUCTURE_TYPE_SUBPASS_BEGIN_INFO, pSubpassBeginInfo, local_pSubpassBeginInfo);
    }
    
    size_t count = 8;
    size_t* countPtr = &count;
    if (local_pRenderPassBegin) {
        count_VkRenderPassBeginInfo(0, VK_STRUCTURE_TYPE_MAX_ENUM, local_pRenderPassBegin, countPtr);
    }
    if (local_pSubpassBeginInfo) {
        count_VkSubpassBeginInfo(0, VK_STRUCTURE_TYPE_MAX_ENUM, local_pSubpassBeginInfo, countPtr);
    }
    
    char* send_buffer = (char*)mgr.addExternalParamPtr(count);
    uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;
    
    uint64_t guest_cmd = (uint64_t)(uintptr_t)commandBuffer;
    memcpy(*send_buffer_ptr, &guest_cmd, 8);
    *send_buffer_ptr += 8;
    
    if (local_pRenderPassBegin) {
        encode_to_stream_VkRenderPassBeginInfo(VK_STRUCTURE_TYPE_MAX_ENUM, local_pRenderPassBegin, send_buffer_ptr);
    }
    if (local_pSubpassBeginInfo) {
        encode_to_stream_VkSubpassBeginInfo(VK_STRUCTURE_TYPE_MAX_ENUM, local_pSubpassBeginInfo, send_buffer_ptr);
    }
    
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdBeginRenderPass2, false, commandBuffer);
    
    if (local_pRenderPassBegin) free(local_pRenderPassBegin);
    if (local_pSubpassBeginInfo) free(local_pSubpassBeginInfo);
}

VKAPI_ATTR void CmdNextSubpass2(VkCommandBuffer commandBuffer,
                                const VkSubpassBeginInfo* pSubpassBeginInfo,
                                const VkSubpassEndInfo* pSubpassEndInfo) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    size_t count = 0;
    size_t* countPtr = &count;
    
    if (pSubpassBeginInfo) {
        count_VkSubpassBeginInfo(0, VK_STRUCTURE_TYPE_MAX_ENUM, 
                                 (VkSubpassBeginInfo*)pSubpassBeginInfo, countPtr);
    }
    if (pSubpassEndInfo) {
        count_VkSubpassEndInfo(0, VK_STRUCTURE_TYPE_MAX_ENUM,
                               (VkSubpassEndInfo*)pSubpassEndInfo, countPtr);
    }
    count += 16; // Two 8-byte pointers
    
    char* send_buffer = (char*)mgr.addExternalParamPtr(count);
    uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;
    
    uint64_t begin_ptr = (uint64_t)(uintptr_t)pSubpassBeginInfo;
    memcpy(*send_buffer_ptr, &begin_ptr, 8);
    *send_buffer_ptr += 8;
    
    if (pSubpassBeginInfo) {
        encode_to_stream_VkSubpassBeginInfo(VK_STRUCTURE_TYPE_MAX_ENUM,
                                            (VkSubpassBeginInfo*)pSubpassBeginInfo,
                                            send_buffer_ptr);
    }
    
    uint64_t end_ptr = (uint64_t)(uintptr_t)pSubpassEndInfo;
    memcpy(*send_buffer_ptr, &end_ptr, 8);
    *send_buffer_ptr += 8;
    
    if (pSubpassEndInfo) {
        encode_to_stream_VkSubpassEndInfo(VK_STRUCTURE_TYPE_MAX_ENUM,
                                          (VkSubpassEndInfo*)pSubpassEndInfo,
                                          send_buffer_ptr);
    }
    
    mgr.addParam64((uint64_t)(uintptr_t)commandBuffer);
    
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdNextSubpass2, false, commandBuffer);
}

VKAPI_ATTR void CmdEndRenderPass2(VkCommandBuffer commandBuffer,
                                 const VkSubpassEndInfo* pSubpassEndInfo) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    Allocator vkAllocator;
    
    VkSubpassEndInfo* local_pSubpassEndInfo = nullptr;
    if (pSubpassEndInfo) {
        local_pSubpassEndInfo = (VkSubpassEndInfo*)malloc(sizeof(VkSubpassEndInfo));
        if (!local_pSubpassEndInfo) {
            return;
        }
        deepcopy_VkSubpassEndInfo(&vkAllocator, VK_STRUCTURE_TYPE_SUBPASS_END_INFO, pSubpassEndInfo, local_pSubpassEndInfo);
    }
    
    size_t count = 0;
    size_t* countPtr = &count;
    if (local_pSubpassEndInfo) {
        count_VkSubpassEndInfo(0, VK_STRUCTURE_TYPE_MAX_ENUM, local_pSubpassEndInfo, countPtr);
    }
    count += 8;

    uint64_t guest_cmd_buffer = (uint64_t)(uintptr_t)commandBuffer;
    mgr.addParam64(guest_cmd_buffer);    
    
    char* send_buffer = (char*)mgr.addExternalParamPtr(count);
    uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;
    
    uint64_t cgen_var_0 = (uint64_t)(uintptr_t)local_pSubpassEndInfo;
    memcpy((*send_buffer_ptr), &cgen_var_0, 8);
    *send_buffer_ptr += 8;
    
    if (local_pSubpassEndInfo) {
        encode_to_stream_VkSubpassEndInfo(VK_STRUCTURE_TYPE_MAX_ENUM, local_pSubpassEndInfo, send_buffer_ptr);
    }
    
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdEndRenderPass2, false, commandBuffer);
    
    if (local_pSubpassEndInfo) free(local_pSubpassEndInfo);
}

VKAPI_ATTR VkResult GetSemaphoreCounterValue(
    VkDevice device,
    VkSemaphore semaphore,
    uint64_t* pValue) {
    
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    uint64_t guest_device = (uint64_t)(uintptr_t)device;
    uint64_t guest_semaphore = (uint64_t)(uintptr_t)semaphore;
    
    mgr.addParam64(guest_device);
    mgr.addParam64(guest_semaphore);
    mgr.addPtr(pValue, sizeof(uint64_t));
    
    VkResult vkResult = VK_SUCCESS;
    mgr.addPtr(&vkResult, sizeof(VkResult));
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkGetSemaphoreCounterValue, true);
    
    return vkResult;
}

VKAPI_ATTR VkResult WaitSemaphores(
    VkDevice device,
    const VkSemaphoreWaitInfo* pWaitInfo,
    uint64_t timeout) {
    
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    Allocator vkAllocator;
    
    uint64_t guest_device = (uint64_t)(uintptr_t)device;
    
    VkSemaphoreWaitInfo* local_pWaitInfo = nullptr;
    if (pWaitInfo) {
        local_pWaitInfo = (VkSemaphoreWaitInfo*)malloc(sizeof(VkSemaphoreWaitInfo));
        if (!local_pWaitInfo) {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        deepcopy_VkSemaphoreWaitInfo(&vkAllocator, VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, 
                                    pWaitInfo, local_pWaitInfo);
    }
    
    size_t count = 0;
    size_t* countPtr = &count;
    count_VkSemaphoreWaitInfo(0, VK_STRUCTURE_TYPE_MAX_ENUM, 
                             local_pWaitInfo, countPtr);
    count += sizeof(uint64_t) * 2;
    
    char* send_buffer = (char*)mgr.addExternalParamPtr(count);
    uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;
    
    memcpy(*send_buffer_ptr, &guest_device, sizeof(uint64_t));
    *send_buffer_ptr += sizeof(uint64_t);
    memcpy(*send_buffer_ptr, &timeout, sizeof(uint64_t));
    *send_buffer_ptr += sizeof(uint64_t);
    
    encode_to_stream_VkSemaphoreWaitInfo(VK_STRUCTURE_TYPE_MAX_ENUM, 
                                        local_pWaitInfo, send_buffer_ptr);
    
    VkResult vkResult = VK_SUCCESS;
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkWaitSemaphores, false);
    
    if (local_pWaitInfo) free(local_pWaitInfo);
    
    return vkResult;
}

VKAPI_ATTR VkResult SignalSemaphore(
    VkDevice device,
    const VkSemaphoreSignalInfo* pSignalInfo) {
    
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    Allocator vkAllocator;
    
    uint64_t guest_device = (uint64_t)(uintptr_t)device;
    
    VkSemaphoreSignalInfo* local_pSignalInfo = nullptr;
    if (pSignalInfo) {
        local_pSignalInfo = (VkSemaphoreSignalInfo*)malloc(sizeof(VkSemaphoreSignalInfo));
        if (!local_pSignalInfo) {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        deepcopy_VkSemaphoreSignalInfo(&vkAllocator, VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, 
                                      pSignalInfo, local_pSignalInfo);
    }
    
    size_t count = 0;
    size_t* countPtr = &count;
    count_VkSemaphoreSignalInfo(0, VK_STRUCTURE_TYPE_MAX_ENUM, 
                               local_pSignalInfo, countPtr);
    count += sizeof(uint64_t);
    
    char* send_buffer = (char*)mgr.addExternalParamPtr(count);
    uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;
    
    memcpy(*send_buffer_ptr, &guest_device, sizeof(uint64_t));
    *send_buffer_ptr += sizeof(uint64_t);
    
    encode_to_stream_VkSemaphoreSignalInfo(VK_STRUCTURE_TYPE_MAX_ENUM, 
                                          local_pSignalInfo, send_buffer_ptr);
    
    VkResult vkResult = VK_SUCCESS;
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkSignalSemaphore, false);
    
    if (local_pSignalInfo) free(local_pSignalInfo);
    
    return vkResult;
}

VKAPI_ATTR void CmdDrawIndirectCount(VkCommandBuffer commandBuffer,
                                    VkBuffer buffer,
                                    VkDeviceSize offset,
                                    VkBuffer countBuffer,
                                    VkDeviceSize countBufferOffset,
                                    uint32_t maxDrawCount,
                                    uint32_t stride) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    uint64_t guest_cmd_buffer = (uint64_t)(uintptr_t)commandBuffer;
    uint64_t guest_buffer = (uint64_t)(uintptr_t)buffer;
    uint64_t guest_count_buffer = (uint64_t)(uintptr_t)countBuffer;
    
    mgr.addParam64(guest_cmd_buffer);
    mgr.addParam64(guest_buffer);
    mgr.addParam64(offset);
    mgr.addParam64(guest_count_buffer);
    mgr.addParam64(countBufferOffset);
    mgr.addParam32(maxDrawCount);
    mgr.addParam32(stride);
    
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdDrawIndirectCount, false, commandBuffer);
}

VKAPI_ATTR void CmdDrawIndexedIndirectCount(VkCommandBuffer commandBuffer,
                                           VkBuffer buffer,
                                           VkDeviceSize offset,
                                           VkBuffer countBuffer,
                                           VkDeviceSize countBufferOffset,
                                           uint32_t maxDrawCount,
                                           uint32_t stride) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    uint64_t guest_cmd_buffer = (uint64_t)(uintptr_t)commandBuffer;
    uint64_t guest_buffer = (uint64_t)(uintptr_t)buffer;
    uint64_t guest_count_buffer = (uint64_t)(uintptr_t)countBuffer;
    
    mgr.addParam64(guest_cmd_buffer);
    mgr.addParam64(guest_buffer);
    mgr.addParam64(offset);
    mgr.addParam64(guest_count_buffer);
    mgr.addParam64(countBufferOffset);
    mgr.addParam32(maxDrawCount);
    mgr.addParam32(stride);
    
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdDrawIndexedIndirectCount, false, commandBuffer);
}

VKAPI_ATTR uint64_t GetBufferOpaqueCaptureAddress(VkDevice device, 
                                                  const VkBufferDeviceAddressInfo* pInfo) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    Allocator vkAllocator;
    
    VkBufferDeviceAddressInfo* local_pInfo = nullptr;
    if (pInfo) {
        local_pInfo = (VkBufferDeviceAddressInfo*)malloc(sizeof(VkBufferDeviceAddressInfo));
        deepcopy_VkBufferDeviceAddressInfo(&vkAllocator, VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, pInfo, local_pInfo);
    }
    
    size_t count = 0;
    size_t* countPtr = &count;
    count_VkBufferDeviceAddressInfo(0, VK_STRUCTURE_TYPE_MAX_ENUM, local_pInfo, countPtr);
    count += 8;
    
    char* send_buffer = (char*)mgr.addExternalParamPtr(count);
    uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;
    
    uint64_t cgen_var_0 = (uint64_t)(uintptr_t)device;
    memcpy((*send_buffer_ptr), &cgen_var_0, 8);
    *send_buffer_ptr += 8;
    
    encode_to_stream_VkBufferDeviceAddressInfo(VK_STRUCTURE_TYPE_MAX_ENUM, local_pInfo, send_buffer_ptr);
    
    uint64_t result = 0;
    mgr.addPtr(&result, sizeof(uint64_t));
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkGetBufferOpaqueCaptureAddress, true);
    
    if (local_pInfo) free(local_pInfo);
    return result;
}

VKAPI_ATTR VkDeviceAddress GetBufferDeviceAddress(VkDevice device, 
                                                   const VkBufferDeviceAddressInfo* pInfo) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    Allocator vkAllocator;
    
    VkBufferDeviceAddressInfo* local_pInfo = nullptr;
    if (pInfo) {
        local_pInfo = (VkBufferDeviceAddressInfo*)malloc(sizeof(VkBufferDeviceAddressInfo));
        deepcopy_VkBufferDeviceAddressInfo(&vkAllocator, VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, pInfo, local_pInfo);
    }
    
    size_t count = 0;
    size_t* countPtr = &count;
    count_VkBufferDeviceAddressInfo(0, VK_STRUCTURE_TYPE_MAX_ENUM, local_pInfo, countPtr);
    count += 8;
    
    char* send_buffer = (char*)mgr.addExternalParamPtr(count);
    uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;
    
    uint64_t cgen_var_0 = (uint64_t)(uintptr_t)device;
    memcpy((*send_buffer_ptr), &cgen_var_0, 8);
    *send_buffer_ptr += 8;
    
    encode_to_stream_VkBufferDeviceAddressInfo(VK_STRUCTURE_TYPE_MAX_ENUM, local_pInfo, send_buffer_ptr);
    
    VkDeviceAddress result = 0;
    mgr.addPtr(&result, sizeof(VkDeviceAddress));
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkGetBufferDeviceAddress, true);
    
    if (local_pInfo) free(local_pInfo);
    return result;
}

VKAPI_ATTR uint64_t GetDeviceMemoryOpaqueCaptureAddress(VkDevice device, 
                                                        const VkDeviceMemoryOpaqueCaptureAddressInfo* pInfo) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    Allocator vkAllocator;
    
    VkDeviceMemoryOpaqueCaptureAddressInfo* local_pInfo = nullptr;
    if (pInfo) {
        local_pInfo = (VkDeviceMemoryOpaqueCaptureAddressInfo*)malloc(sizeof(VkDeviceMemoryOpaqueCaptureAddressInfo));
        deepcopy_VkDeviceMemoryOpaqueCaptureAddressInfo(&vkAllocator, VK_STRUCTURE_TYPE_DEVICE_MEMORY_OPAQUE_CAPTURE_ADDRESS_INFO, pInfo, local_pInfo);
    }
    
    size_t count = 0;
    size_t* countPtr = &count;
    count_VkDeviceMemoryOpaqueCaptureAddressInfo(0, VK_STRUCTURE_TYPE_MAX_ENUM, local_pInfo, countPtr);
    count += 8;
    
    char* send_buffer = (char*)mgr.addExternalParamPtr(count);
    uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;
    
    uint64_t cgen_var_0 = (uint64_t)(uintptr_t)device;
    memcpy((*send_buffer_ptr), &cgen_var_0, 8);
    *send_buffer_ptr += 8;
    
    encode_to_stream_VkDeviceMemoryOpaqueCaptureAddressInfo(VK_STRUCTURE_TYPE_MAX_ENUM, local_pInfo, send_buffer_ptr);
    
    uint64_t result = 0;
    mgr.addPtr(&result, sizeof(uint64_t));
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkGetDeviceMemoryOpaqueCaptureAddress, true);
    
    if (local_pInfo) free(local_pInfo);
    return result;
}

VKAPI_ATTR void CmdBeginRendering(VkCommandBuffer commandBuffer,
                                  const VkRenderingInfo* pRenderingInfo) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    Allocator vkAllocator;
    
    VkRenderingInfo* local_pRenderingInfo = nullptr;
    if (pRenderingInfo) {
        local_pRenderingInfo = (VkRenderingInfo*)malloc(sizeof(VkRenderingInfo));
        if (!local_pRenderingInfo) return;
        deepcopy_VkRenderingInfo(&vkAllocator, VK_STRUCTURE_TYPE_RENDERING_INFO, pRenderingInfo, local_pRenderingInfo);
    }
    
    size_t count = 0;
    size_t* countPtr = &count;
    count_VkRenderingInfo(0, VK_STRUCTURE_TYPE_MAX_ENUM, local_pRenderingInfo, countPtr);
    count += 8;
    
    char* send_buffer = (char*)mgr.addExternalParamPtr(count);
    uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;
    
    uint64_t guest_cmd = (uint64_t)(uintptr_t)commandBuffer;
    memcpy(*send_buffer_ptr, &guest_cmd, 8);
    *send_buffer_ptr += 8;
    
    if (local_pRenderingInfo) {
        encode_to_stream_VkRenderingInfo(VK_STRUCTURE_TYPE_MAX_ENUM, local_pRenderingInfo, send_buffer_ptr);
    }
    
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdBeginRendering, false, commandBuffer);
    
    if (local_pRenderingInfo) free(local_pRenderingInfo);
}

VKAPI_ATTR void CmdEndRendering(VkCommandBuffer commandBuffer) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    uint64_t guest_cmd_buffer = (uint64_t)(uintptr_t)commandBuffer;
    mgr.addParam64(guest_cmd_buffer);
    
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdEndRendering, false, commandBuffer);
}

VKAPI_ATTR void CmdBindVertexBuffers2(VkCommandBuffer commandBuffer, uint32_t firstBinding, uint32_t bindingCount, const VkBuffer* pBuffers, const VkDeviceSize* pOffsets, const VkDeviceSize* pSizes, const VkDeviceSize* pStrides) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    uint64_t guest_cmd_buffer = (uint64_t)(uintptr_t)commandBuffer;
    uint32_t has_sizes = pSizes ? 1 : 0;
    uint32_t has_strides = pStrides ? 1 : 0;
    
    size_t count = 16; // guest_cmd_buffer + firstBinding + bindingCount + has_sizes + has_strides
    count += bindingCount * 16; // pBuffers + pOffsets
    if (pSizes) count += bindingCount * 8;
    if (pStrides) count += bindingCount * 8;
    
    char* send_buffer = (char*)mgr.addExternalParamPtr(count);
    uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;
    
    memcpy(*send_buffer_ptr, &guest_cmd_buffer, 8); *send_buffer_ptr += 8;
    memcpy(*send_buffer_ptr, &firstBinding, 4); *send_buffer_ptr += 4;
    memcpy(*send_buffer_ptr, &bindingCount, 4); *send_buffer_ptr += 4;
    memcpy(*send_buffer_ptr, &has_sizes, 4); *send_buffer_ptr += 4;
    memcpy(*send_buffer_ptr, &has_strides, 4); *send_buffer_ptr += 4;
    
    for (uint32_t i = 0; i < bindingCount; ++i) {
        uint64_t guest_buffer = (uint64_t)(uintptr_t)pBuffers[i];
        memcpy(*send_buffer_ptr, &guest_buffer, 8); *send_buffer_ptr += 8;
        memcpy(*send_buffer_ptr, &pOffsets[i], 8); *send_buffer_ptr += 8;
    }
    
    if (pSizes) {
        for (uint32_t i = 0; i < bindingCount; ++i) {
            memcpy(*send_buffer_ptr, &pSizes[i], 8); *send_buffer_ptr += 8;
        }
    }
    
    if (pStrides) {
        for (uint32_t i = 0; i < bindingCount; ++i) {
            memcpy(*send_buffer_ptr, &pStrides[i], 8); *send_buffer_ptr += 8;
        }
    }
    
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdBindVertexBuffers2, false, commandBuffer);
}

VKAPI_ATTR void CmdBlitImage2(VkCommandBuffer commandBuffer,
                              const VkBlitImageInfo2* pBlitImageInfo) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    Allocator vkAllocator;
    
    VkBlitImageInfo2* local_pBlitImageInfo = nullptr;
    if (pBlitImageInfo) {
        local_pBlitImageInfo = (VkBlitImageInfo2*)malloc(sizeof(VkBlitImageInfo2));
        if (!local_pBlitImageInfo) return;
        deepcopy_VkBlitImageInfo2(&vkAllocator, VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2, pBlitImageInfo, local_pBlitImageInfo);
    }
    
    size_t count = 8;
    size_t* countPtr = &count;
    if (local_pBlitImageInfo) {
        count_VkBlitImageInfo2(0, VK_STRUCTURE_TYPE_MAX_ENUM, local_pBlitImageInfo, countPtr);
    }
    
    char* send_buffer = (char*)mgr.addExternalParamPtr(count);
    uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;
    
    uint64_t guest_cmd = (uint64_t)(uintptr_t)commandBuffer;
    memcpy(*send_buffer_ptr, &guest_cmd, 8);
    *send_buffer_ptr += 8;
    
    if (local_pBlitImageInfo) {
        encode_to_stream_VkBlitImageInfo2(VK_STRUCTURE_TYPE_MAX_ENUM, local_pBlitImageInfo, send_buffer_ptr);
    }
    
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdBlitImage2, false, commandBuffer);
    
    if (local_pBlitImageInfo) free(local_pBlitImageInfo);
}

VKAPI_ATTR void CmdCopyBuffer2(VkCommandBuffer commandBuffer,
                               const VkCopyBufferInfo2* pCopyBufferInfo) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    Allocator vkAllocator;
    
    VkCopyBufferInfo2* local_pCopyBufferInfo = nullptr;
    if (pCopyBufferInfo) {
        local_pCopyBufferInfo = (VkCopyBufferInfo2*)malloc(sizeof(VkCopyBufferInfo2));
        if (!local_pCopyBufferInfo) {
            return;
        }
        deepcopy_VkCopyBufferInfo2(&vkAllocator, VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2, pCopyBufferInfo, local_pCopyBufferInfo);
    }
    
    size_t count = 0;
    size_t* countPtr = &count;
    count_VkCopyBufferInfo2(0, VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2, local_pCopyBufferInfo, countPtr);
    count += sizeof(uint64_t);
    
    char* send_buffer = (char*)mgr.addExternalParamPtr(count);
    uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;
    
    uint64_t guest_cmd = (uint64_t)(uintptr_t)commandBuffer;
    memcpy(*send_buffer_ptr, &guest_cmd, sizeof(uint64_t));
    *send_buffer_ptr += sizeof(uint64_t);
    
    encode_to_stream_VkCopyBufferInfo2(VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2, local_pCopyBufferInfo, send_buffer_ptr);

    std::vector<TrackedMemoryRange> copy_early_upload_ranges;
    uint64_t total_copy_bytes = 0;
    if (local_pCopyBufferInfo) {
        std::lock_guard<std::mutex> lock(g_mapped_mutex);
        if (local_pCopyBufferInfo->pRegions && local_pCopyBufferInfo->regionCount > 0) {
            std::vector<BufferSyncRange>& flush_ranges =
                g_command_buffer_flush_buffer_ranges[commandBuffer];
            std::vector<BufferSyncRange>& invalidate_ranges =
                g_command_buffer_invalidate_buffer_ranges[commandBuffer];
            for (uint32_t i = 0; i < local_pCopyBufferInfo->regionCount; ++i) {
                const VkBufferCopy2& region = local_pCopyBufferInfo->pRegions[i];
                total_copy_bytes += (uint64_t)region.size;
                AppendBufferSyncRange(
                    &flush_ranges,
                    {local_pCopyBufferInfo->srcBuffer, region.srcOffset, region.size});
                AppendBufferSyncRange(
                    &invalidate_ranges,
                    {local_pCopyBufferInfo->dstBuffer, region.dstOffset, region.size});
                MemShapeNoteBufferRangeLocked(local_pCopyBufferInfo->srcBuffer,
                                              region.srcOffset,
                                              region.size,
                                              MemShapeRangeKind::kCopySrc);
                MemShapeNoteBufferRangeLocked(local_pCopyBufferInfo->dstBuffer,
                                              region.dstOffset,
                                              region.size,
                                              MemShapeRangeKind::kCopyDst);
                TrackedMemoryRange tracked = {};
                if (ResolveBufferRangeLocked(local_pCopyBufferInfo->srcBuffer,
                                             region.srcOffset,
                                             region.size,
                                             &tracked)) {
                    AppendTrackedRange(&copy_early_upload_ranges, tracked);
                }
            }
        } else {
            AppendBufferSyncRange(
                &g_command_buffer_flush_buffer_ranges[commandBuffer],
                {local_pCopyBufferInfo->srcBuffer, 0, VK_WHOLE_SIZE});
            AppendBufferSyncRange(
                &g_command_buffer_invalidate_buffer_ranges[commandBuffer],
                {local_pCopyBufferInfo->dstBuffer, 0, VK_WHOLE_SIZE});
            MemShapeNoteBufferRangeLocked(local_pCopyBufferInfo->srcBuffer,
                                          0,
                                          VK_WHOLE_SIZE,
                                          MemShapeRangeKind::kCopySrc);
            MemShapeNoteBufferRangeLocked(local_pCopyBufferInfo->dstBuffer,
                                          0,
                                          VK_WHOLE_SIZE,
                                          MemShapeRangeKind::kCopyDst);
            TrackedMemoryRange tracked = {};
            if (ResolveBufferRangeLocked(local_pCopyBufferInfo->srcBuffer, 0, VK_WHOLE_SIZE, &tracked)) {
                AppendTrackedRange(&copy_early_upload_ranges, tracked);
            }
        }
        InvalidateCommandBufferSubmitHintCacheLocked(commandBuffer,
                                                     "cmd_copy_buffer2");
    }
    if (kEnableCommandCopyEarlyUpload) {
        EarlyUploadTrackedRanges(copy_early_upload_ranges,
                                 kCommandEarlyUploadMinBytes,
                                 "cmd_copy2_src");
    } else if (!copy_early_upload_ranges.empty()) {
        std::lock_guard<std::mutex> lock(g_descriptor_lifecycle_mutex);
        g_descriptor_lifecycle_stats.copy_early_upload_disabled++;
    }
    
    const uint64_t record_start_us = ExpressVkNowUs();
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdCopyBuffer2, false, commandBuffer);
    LlmVkTimingNoteCmdCopyBuffer(local_pCopyBufferInfo ?
                                     local_pCopyBufferInfo->regionCount : 0,
                                 total_copy_bytes,
                                 ExpressVkNowUs() - record_start_us);
    
    if (local_pCopyBufferInfo) free(local_pCopyBufferInfo);
}
VKAPI_ATTR void CmdCopyImage2(VkCommandBuffer commandBuffer,
                              const VkCopyImageInfo2* pCopyImageInfo) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    Allocator vkAllocator;
    
    VkCopyImageInfo2* local_pCopyImageInfo = nullptr;
    if (pCopyImageInfo) {
        local_pCopyImageInfo = (VkCopyImageInfo2*)malloc(sizeof(VkCopyImageInfo2));
        if (!local_pCopyImageInfo) {
            return;
        }
        deepcopy_VkCopyImageInfo2(&vkAllocator, VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2, pCopyImageInfo, local_pCopyImageInfo);
    }
    
    size_t count = 0;
    size_t* countPtr = &count;
    count_VkCopyImageInfo2(0, VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2, local_pCopyImageInfo, countPtr);
    count += sizeof(uint64_t);
    
    char* send_buffer = (char*)mgr.addExternalParamPtr(count);
    uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;
    
    uint64_t guest_cmd = (uint64_t)(uintptr_t)commandBuffer;
    memcpy(*send_buffer_ptr, &guest_cmd, sizeof(uint64_t));
    *send_buffer_ptr += sizeof(uint64_t);
    
    encode_to_stream_VkCopyImageInfo2(VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2, local_pCopyImageInfo, send_buffer_ptr);
    
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdCopyImage2, false, commandBuffer);
    
    if (local_pCopyImageInfo) free(local_pCopyImageInfo);
}

VKAPI_ATTR void CmdCopyBufferToImage2(VkCommandBuffer commandBuffer,
                                      const VkCopyBufferToImageInfo2* pCopyBufferToImageInfo) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    Allocator vkAllocator;
    
    VkCopyBufferToImageInfo2* local_pCopyBufferToImageInfo = nullptr;
    if (pCopyBufferToImageInfo) {
        local_pCopyBufferToImageInfo = (VkCopyBufferToImageInfo2*)malloc(sizeof(VkCopyBufferToImageInfo2));
        if (!local_pCopyBufferToImageInfo) {
            return;
        }
        deepcopy_VkCopyBufferToImageInfo2(&vkAllocator, VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2, pCopyBufferToImageInfo, local_pCopyBufferToImageInfo);
    }
    
    size_t count = 0;
    size_t* countPtr = &count;
    count_VkCopyBufferToImageInfo2(0, VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2, local_pCopyBufferToImageInfo, countPtr);
    count += sizeof(uint64_t);
    
    char* send_buffer = (char*)mgr.addExternalParamPtr(count);
    uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;
    
    uint64_t guest_cmd = (uint64_t)(uintptr_t)commandBuffer;
    memcpy(*send_buffer_ptr, &guest_cmd, sizeof(uint64_t));
    *send_buffer_ptr += sizeof(uint64_t);
    
    encode_to_stream_VkCopyBufferToImageInfo2(VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2, local_pCopyBufferToImageInfo, send_buffer_ptr);

    if (local_pCopyBufferToImageInfo) {
        MarkBufferAsFlushHint(local_pCopyBufferToImageInfo->srcBuffer);
    }
    
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdCopyBufferToImage2, false, commandBuffer);
    
    if (local_pCopyBufferToImageInfo) free(local_pCopyBufferToImageInfo);
}

VKAPI_ATTR void CmdCopyImageToBuffer2(VkCommandBuffer commandBuffer,
                                      const VkCopyImageToBufferInfo2* pCopyImageToBufferInfo) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    Allocator vkAllocator;
    
    VkCopyImageToBufferInfo2* local_pCopyImageToBufferInfo = nullptr;
    if (pCopyImageToBufferInfo) {
        local_pCopyImageToBufferInfo = (VkCopyImageToBufferInfo2*)malloc(sizeof(VkCopyImageToBufferInfo2));
        if (!local_pCopyImageToBufferInfo) {
            return;
        }
        deepcopy_VkCopyImageToBufferInfo2(&vkAllocator, VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2, pCopyImageToBufferInfo, local_pCopyImageToBufferInfo);
    }
    
    size_t count = 0;
    size_t* countPtr = &count;
    count_VkCopyImageToBufferInfo2(0, VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2, local_pCopyImageToBufferInfo, countPtr);
    count += sizeof(uint64_t);
    
    char* send_buffer = (char*)mgr.addExternalParamPtr(count);
    uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;
    
    uint64_t guest_cmd = (uint64_t)(uintptr_t)commandBuffer;
    memcpy(*send_buffer_ptr, &guest_cmd, sizeof(uint64_t));
    *send_buffer_ptr += sizeof(uint64_t);
    
    encode_to_stream_VkCopyImageToBufferInfo2(VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2, local_pCopyImageToBufferInfo, send_buffer_ptr);

    if (local_pCopyImageToBufferInfo) {
        MarkBufferAsInvalidateHint(local_pCopyImageToBufferInfo->dstBuffer);
    }
    
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdCopyImageToBuffer2, false, commandBuffer);
    
    if (local_pCopyImageToBufferInfo) free(local_pCopyImageToBufferInfo);
}

VKAPI_ATTR void CmdPipelineBarrier2(VkCommandBuffer commandBuffer,
                                    const VkDependencyInfo* pDependencyInfo) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    Allocator vkAllocator;
    VkDependencyInfo* local_pDependencyInfo = nullptr;
    
    if (pDependencyInfo) {
        local_pDependencyInfo = (VkDependencyInfo*)malloc(sizeof(VkDependencyInfo));
        if (!local_pDependencyInfo) {
            return;
        }
        deepcopy_VkDependencyInfo(&vkAllocator, VK_STRUCTURE_TYPE_MAX_ENUM, 
                                  pDependencyInfo, local_pDependencyInfo);
    }
   
    size_t count = 0;
    size_t* countPtr = &count;
   
    if (local_pDependencyInfo) {
        count_VkDependencyInfo(0, VK_STRUCTURE_TYPE_MAX_ENUM,
                               local_pDependencyInfo, countPtr);
    }
    count += 8; // One 8-byte pointer
   
    char* send_buffer = (char*)mgr.addExternalParamPtr(count);
    uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;
   
    uint64_t dep_ptr = (uint64_t)(uintptr_t)local_pDependencyInfo;
    memcpy(*send_buffer_ptr, &dep_ptr, 8);
    *send_buffer_ptr += 8;
   
    if (local_pDependencyInfo) {
        encode_to_stream_VkDependencyInfo(VK_STRUCTURE_TYPE_MAX_ENUM,
                                          local_pDependencyInfo,
                                          send_buffer_ptr);
    }
   
    mgr.addParam64((uint64_t)(uintptr_t)commandBuffer);
   
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdPipelineBarrier2, false, commandBuffer);
    ALOGI("vkCmdPipelineBarrier2: commandBuffer=%p, pDependencyInfo=%p extension=%p",
          (void*)commandBuffer, (void*)local_pDependencyInfo, (void*)pDependencyInfo->pNext);
    
    if (local_pDependencyInfo) {
        free(local_pDependencyInfo);
    }
}

VKAPI_ATTR void CmdResetEvent2(VkCommandBuffer commandBuffer,
                               VkEvent event,
                               VkPipelineStageFlags2 stageMask) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    mgr.addParam64((uint64_t)(uintptr_t)commandBuffer);
    mgr.addParam64((uint64_t)(uintptr_t)event);
    mgr.addParam64(stageMask);
    
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdResetEvent2, false, commandBuffer);
}

VKAPI_ATTR void CmdResolveImage2(VkCommandBuffer commandBuffer,
                                 const VkResolveImageInfo2* pResolveImageInfo) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    Allocator vkAllocator;
    VkResolveImageInfo2* local_pResolveImageInfo = nullptr;
    
    if (pResolveImageInfo) {
        local_pResolveImageInfo = (VkResolveImageInfo2*)malloc(sizeof(VkResolveImageInfo2));
        if (!local_pResolveImageInfo) {
            return;
        }
        deepcopy_VkResolveImageInfo2(&vkAllocator, VK_STRUCTURE_TYPE_MAX_ENUM, 
                                     pResolveImageInfo, local_pResolveImageInfo);
    }
   
    size_t count = sizeof(uint64_t) * 2; // commandBuffer + resolve_ptr
    size_t* countPtr = &count;
   
    if (local_pResolveImageInfo) {
        count_VkResolveImageInfo2(0, VK_STRUCTURE_TYPE_MAX_ENUM,
                                  local_pResolveImageInfo, countPtr);
    }
   
    char* send_buffer = (char*)mgr.addExternalParamPtr(count);
    uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;
   
    uint64_t guest_cmd = (uint64_t)(uintptr_t)commandBuffer;
    memcpy(*send_buffer_ptr, &guest_cmd, sizeof(uint64_t));
    *send_buffer_ptr += sizeof(uint64_t);
   
    uint64_t resolve_ptr = (uint64_t)(uintptr_t)local_pResolveImageInfo;
    memcpy(*send_buffer_ptr, &resolve_ptr, sizeof(uint64_t));
    *send_buffer_ptr += sizeof(uint64_t);
   
    if (local_pResolveImageInfo) {
        encode_to_stream_VkResolveImageInfo2(VK_STRUCTURE_TYPE_MAX_ENUM,
                                             local_pResolveImageInfo,
                                             send_buffer_ptr);
    }
   
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdResolveImage2, false, commandBuffer);
    
    if (local_pResolveImageInfo) {
        free(local_pResolveImageInfo);
    }
}

VKAPI_ATTR void CmdSetCullMode(VkCommandBuffer commandBuffer, VkCullModeFlags cullMode) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    uint64_t guest_cmd_buffer = (uint64_t)(uintptr_t)commandBuffer;
    mgr.addParam64(guest_cmd_buffer);
    mgr.addParam32(cullMode);
    
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdSetCullMode, false, commandBuffer);
}

VKAPI_ATTR void CmdSetDepthBiasEnable(VkCommandBuffer commandBuffer, VkBool32 depthBiasEnable) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    uint64_t guest_cmd_buffer = (uint64_t)(uintptr_t)commandBuffer;
    mgr.addParam64(guest_cmd_buffer);
    mgr.addParam32(depthBiasEnable);
    
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdSetDepthBiasEnable, false, commandBuffer);
}

VKAPI_ATTR void CmdSetDepthBoundsTestEnable(VkCommandBuffer commandBuffer, VkBool32 depthBoundsTestEnable) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    uint64_t guest_cmd_buffer = (uint64_t)(uintptr_t)commandBuffer;
    mgr.addParam64(guest_cmd_buffer);
    mgr.addParam32(depthBoundsTestEnable);
    
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdSetDepthBoundsTestEnable, false, commandBuffer);
}

VKAPI_ATTR void CmdSetDepthCompareOp(VkCommandBuffer commandBuffer, VkCompareOp depthCompareOp) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    mgr.addParam64((uint64_t)(uintptr_t)commandBuffer);
    mgr.addParam32((uint32_t)depthCompareOp);
    
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdSetDepthCompareOp, false, commandBuffer);
}   

VKAPI_ATTR void CmdSetDepthTestEnable(VkCommandBuffer commandBuffer, VkBool32 depthTestEnable) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    uint64_t guest_cmd_buffer = (uint64_t)(uintptr_t)commandBuffer;
    mgr.addParam64(guest_cmd_buffer);
    mgr.addParam32(depthTestEnable);
    
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdSetDepthTestEnable, false, commandBuffer);
}

VKAPI_ATTR void CmdSetDepthWriteEnable(VkCommandBuffer commandBuffer, VkBool32 depthWriteEnable) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    uint64_t guest_cmd_buffer = (uint64_t)(uintptr_t)commandBuffer;
    mgr.addParam64(guest_cmd_buffer);
    mgr.addParam32(depthWriteEnable);
    
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdSetDepthWriteEnable, false, commandBuffer);
}

VKAPI_ATTR void CmdSetEvent2(VkCommandBuffer commandBuffer, VkEvent event, const VkDependencyInfo* pDependencyInfo) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    uint64_t guest_cmd_buffer = (uint64_t)(uintptr_t)commandBuffer;
    uint64_t guest_event = (uint64_t)(uintptr_t)event;
    uint32_t has_dependency_info = pDependencyInfo ? 1 : 0;
    
    VkDependencyInfo* local_pDependencyInfo = nullptr;
    size_t count = 0;
    size_t* countPtr = &count;
    
    if (pDependencyInfo) {
        local_pDependencyInfo = (VkDependencyInfo*)malloc(sizeof(VkDependencyInfo));
        if (!local_pDependencyInfo) return;
        
        Allocator vkAllocator;
        deepcopy_VkDependencyInfo(&vkAllocator, VK_STRUCTURE_TYPE_DEPENDENCY_INFO, pDependencyInfo, local_pDependencyInfo);
        count_VkDependencyInfo(0, VK_STRUCTURE_TYPE_DEPENDENCY_INFO, local_pDependencyInfo, countPtr);
    }
    
    count += 20; // guest_cmd_buffer + guest_event + has_dependency_info
    char* send_buffer = (char*)mgr.addExternalParamPtr(count);
    uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;
    
    memcpy(*send_buffer_ptr, &guest_cmd_buffer, 8); *send_buffer_ptr += 8;
    memcpy(*send_buffer_ptr, &guest_event, 8); *send_buffer_ptr += 8;
    memcpy(*send_buffer_ptr, &has_dependency_info, 4); *send_buffer_ptr += 4;
    
    if (pDependencyInfo) {
        encode_to_stream_VkDependencyInfo(VK_STRUCTURE_TYPE_DEPENDENCY_INFO, local_pDependencyInfo, send_buffer_ptr);
        free(local_pDependencyInfo);
    }
    
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdSetEvent2, false, commandBuffer);
}

VKAPI_ATTR void CmdSetFrontFace(VkCommandBuffer commandBuffer, VkFrontFace frontFace) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    uint64_t guest_cmd_buffer = (uint64_t)(uintptr_t)commandBuffer;
    mgr.addParam64(guest_cmd_buffer);
    mgr.addParam32(frontFace);
    
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdSetFrontFace, false, commandBuffer);
}

VKAPI_ATTR void CmdSetPrimitiveRestartEnable(VkCommandBuffer commandBuffer, VkBool32 primitiveRestartEnable) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    mgr.addParam64((uint64_t)(uintptr_t)commandBuffer);
    mgr.addParam32(primitiveRestartEnable);
    
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdSetPrimitiveRestartEnable, false, commandBuffer);
}

VKAPI_ATTR void CmdSetPrimitiveTopology(VkCommandBuffer commandBuffer, VkPrimitiveTopology primitiveTopology) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    mgr.addParam64((uint64_t)(uintptr_t)commandBuffer);
    mgr.addParam32((uint32_t)primitiveTopology);
    
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdSetPrimitiveTopology, false, commandBuffer);
}

VKAPI_ATTR void CmdSetRasterizerDiscardEnable(VkCommandBuffer commandBuffer, VkBool32 rasterizerDiscardEnable) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    mgr.addParam64((uint64_t)(uintptr_t)commandBuffer);
    mgr.addParam32(rasterizerDiscardEnable);
    
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdSetRasterizerDiscardEnable, false, commandBuffer);
}

VKAPI_ATTR void CmdSetScissorWithCount(VkCommandBuffer commandBuffer, uint32_t scissorCount, const VkRect2D* pScissors) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    size_t scissors_size = scissorCount * sizeof(VkRect2D);
    size_t total_size = sizeof(uint32_t) + scissors_size;
    char* buffer = (char*)mgr.addExternalParamPtr(total_size);
    char* buffer_start = buffer;
    
    memcpy(buffer, &scissorCount, sizeof(uint32_t));
    buffer += sizeof(uint32_t);
    if (pScissors && scissorCount > 0) {
        memcpy(buffer, pScissors, scissors_size);
    }
    
    mgr.addParam64((uint64_t)(uintptr_t)commandBuffer);
    
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdSetScissorWithCount, false, commandBuffer);
}

VKAPI_ATTR void CmdSetStencilOp(VkCommandBuffer commandBuffer, VkStencilFaceFlags faceMask, VkStencilOp failOp, VkStencilOp passOp, VkStencilOp depthFailOp, VkCompareOp compareOp) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    mgr.addParam64((uint64_t)(uintptr_t)commandBuffer);
    mgr.addParam32((uint32_t)faceMask);
    mgr.addParam32((uint32_t)failOp);
    mgr.addParam32((uint32_t)passOp);
    mgr.addParam32((uint32_t)depthFailOp);
    mgr.addParam32((uint32_t)compareOp);
    
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdSetStencilOp, false, commandBuffer);
}

VKAPI_ATTR void CmdSetStencilTestEnable(VkCommandBuffer commandBuffer,
                                        VkBool32 stencilTestEnable) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    mgr.addParam64((uint64_t)(uintptr_t)commandBuffer);
    mgr.addParam32(stencilTestEnable);
    
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdSetStencilTestEnable, false, commandBuffer);
}

VKAPI_ATTR void CmdSetViewportWithCount(VkCommandBuffer commandBuffer,
                                        uint32_t viewportCount,
                                        const VkViewport* pViewports) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    size_t count = sizeof(uint64_t) + sizeof(uint32_t);
    char* send_buffer = (char*)mgr.addExternalParamPtr(count);
    uint8_t** ptr = (uint8_t**)&send_buffer;
    
    uint64_t cmd_buffer = (uint64_t)(uintptr_t)commandBuffer;
    memcpy(*ptr, &cmd_buffer, sizeof(uint64_t)); *ptr += sizeof(uint64_t);
    memcpy(*ptr, &viewportCount, sizeof(uint32_t)); *ptr += sizeof(uint32_t);
    
    mgr.addPtr((void*)pViewports, sizeof(VkViewport) * viewportCount);
    
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdSetViewportWithCount, false, commandBuffer);
}

VKAPI_ATTR void CmdWaitEvents2(VkCommandBuffer commandBuffer,
                               uint32_t eventCount,
                               const VkEvent* pEvents,
                               const VkDependencyInfo* pDependencyInfos) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    // Calculate total size for dependency infos
    size_t dep_total_size = 0;
    for (uint32_t i = 0; i < eventCount; ++i) {
        size_t dep_size = 0;
        count_VkDependencyInfo(0, VK_STRUCTURE_TYPE_MAX_ENUM, &pDependencyInfos[i], &dep_size);
        dep_total_size += dep_size;
    }
    
    size_t count = sizeof(uint64_t) + sizeof(uint32_t) + dep_total_size;
    char* send_buffer = (char*)mgr.addExternalParamPtr(count);
    uint8_t** ptr = (uint8_t**)&send_buffer;
    
    uint64_t cmd_buffer = (uint64_t)(uintptr_t)commandBuffer;
    memcpy(*ptr, &cmd_buffer, sizeof(uint64_t)); *ptr += sizeof(uint64_t);
    memcpy(*ptr, &eventCount, sizeof(uint32_t)); *ptr += sizeof(uint32_t);
    
    for (uint32_t i = 0; i < eventCount; ++i) {
        encode_to_stream_VkDependencyInfo(VK_STRUCTURE_TYPE_MAX_ENUM, &pDependencyInfos[i], ptr);
    }
    
    mgr.addPtr((void*)pEvents, sizeof(VkEvent) * eventCount);
    
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdWaitEvents2, true, commandBuffer);
}

VKAPI_ATTR void CmdWriteTimestamp2(VkCommandBuffer commandBuffer,
                                   VkPipelineStageFlags2 stage,
                                   VkQueryPool queryPool,
                                   uint32_t query) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    mgr.addParam64((uint64_t)(uintptr_t)commandBuffer);
    mgr.addParam64(stage);
    mgr.addParam64((uint64_t)(uintptr_t)queryPool);
    mgr.addParam32(query);
    
    FlimeGuestWriteCommand(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCmdWriteTimestamp2, false, commandBuffer);
}

VKAPI_ATTR VkResult CreatePrivateDataSlot(VkDevice device,
                                          const VkPrivateDataSlotCreateInfo* pCreateInfo,
                                          const VkAllocationCallbacks* pAllocator,
                                          VkPrivateDataSlot* pPrivateDataSlot) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    Allocator vkAllocator;
    
    VkPrivateDataSlotCreateInfo* local_pCreateInfo = nullptr;
    if (pCreateInfo) {
        local_pCreateInfo = (VkPrivateDataSlotCreateInfo*)malloc(sizeof(VkPrivateDataSlotCreateInfo));
        if (!local_pCreateInfo) return VK_ERROR_OUT_OF_HOST_MEMORY;
        deepcopy_VkPrivateDataSlotCreateInfo(&vkAllocator, VK_STRUCTURE_TYPE_PRIVATE_DATA_SLOT_CREATE_INFO,
                                           pCreateInfo, local_pCreateInfo);
    }
    
    VkAllocationCallbacks* local_pAllocator = nullptr;
    if (pAllocator) {
        local_pAllocator = (VkAllocationCallbacks*)malloc(sizeof(VkAllocationCallbacks));
        if (!local_pAllocator) {
            if (local_pCreateInfo) free(local_pCreateInfo);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        deepcopy_VkAllocationCallbacks(&vkAllocator, VK_STRUCTURE_TYPE_MAX_ENUM, pAllocator, local_pAllocator);
    }
    
    size_t count = 0;
    size_t* countPtr = &count;
    count_VkPrivateDataSlotCreateInfo(0, VK_STRUCTURE_TYPE_MAX_ENUM, local_pCreateInfo, countPtr);
    if (pAllocator) {
        count_VkAllocationCallbacks(0, VK_STRUCTURE_TYPE_MAX_ENUM, local_pAllocator, countPtr);
    }
    count += sizeof(uint64_t) * 2 + sizeof(uint8_t);
    
    char* send_buffer = (char*)mgr.addExternalParamPtr(count);
    uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;
    
    uint64_t guest_device = (uint64_t)(uintptr_t)device;
    uint64_t guest_allocator = (uint64_t)(uintptr_t)pAllocator;
    
    memcpy(*send_buffer_ptr, &guest_device, sizeof(uint64_t)); *send_buffer_ptr += sizeof(uint64_t);
    encode_to_stream_VkPrivateDataSlotCreateInfo(VK_STRUCTURE_TYPE_MAX_ENUM, local_pCreateInfo, send_buffer_ptr);
    memcpy(*send_buffer_ptr, &guest_allocator, sizeof(uint64_t)); *send_buffer_ptr += sizeof(uint64_t);
    
    uint8_t has_allocator = pAllocator ? 1 : 0;
    memcpy(*send_buffer_ptr, &has_allocator, sizeof(uint8_t)); *send_buffer_ptr += sizeof(uint8_t);
    if (pAllocator) {
        encode_to_stream_VkAllocationCallbacks(VK_STRUCTURE_TYPE_MAX_ENUM, local_pAllocator, send_buffer_ptr);
    }
    
    const VkAllocationCallbacks* useAlloc = pAllocator ? pAllocator : &kDefaultAllocCallbacks;
    VkPrivateDataSlot_T* slot = static_cast<VkPrivateDataSlot_T*>(useAlloc->pfnAllocation(
        useAlloc->pUserData, sizeof(VkPrivateDataSlot_T), alignof(VkPrivateDataSlot_T), 
        VK_SYSTEM_ALLOCATION_SCOPE_OBJECT));
    if (!slot) {
        if (local_pCreateInfo) free(local_pCreateInfo);
        if (local_pAllocator) free(local_pAllocator);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    *pPrivateDataSlot = (VkPrivateDataSlot)slot;
    
    mgr.addPtr(pPrivateDataSlot, sizeof(VkPrivateDataSlot));
    VkResult vkResult = VK_SUCCESS;
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkCreatePrivateDataSlot, false);
    
    if (local_pCreateInfo) free(local_pCreateInfo);
    if (local_pAllocator) free(local_pAllocator);
    return vkResult;
}

VKAPI_ATTR void GetDeviceBufferMemoryRequirements(VkDevice device, 
                                                   const VkDeviceBufferMemoryRequirements* pInfo,
                                                   VkMemoryRequirements2* pMemoryRequirements) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    Allocator vkAllocator;
    
    VkDeviceBufferMemoryRequirements* local_pInfo = nullptr;
    if (pInfo) {
        local_pInfo = (VkDeviceBufferMemoryRequirements*)malloc(sizeof(VkDeviceBufferMemoryRequirements));
        deepcopy_VkDeviceBufferMemoryRequirements(&vkAllocator, VK_STRUCTURE_TYPE_DEVICE_BUFFER_MEMORY_REQUIREMENTS, pInfo, local_pInfo);
    }
    
    VkMemoryRequirements2* local_pMemoryRequirements = nullptr;
    if (pMemoryRequirements) {
        local_pMemoryRequirements = (VkMemoryRequirements2*)malloc(sizeof(VkMemoryRequirements2));
        deepcopy_VkMemoryRequirements2(&vkAllocator, VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, pMemoryRequirements, local_pMemoryRequirements);
    }
    
    size_t count = 0;
    size_t* countPtr = &count;
    count_VkDeviceBufferMemoryRequirements(0, VK_STRUCTURE_TYPE_MAX_ENUM, local_pInfo, countPtr);
    count_VkMemoryRequirements2(0, VK_STRUCTURE_TYPE_MAX_ENUM, local_pMemoryRequirements, countPtr);
    count += 8;
    
    char* send_buffer = (char*)mgr.addExternalParamPtr(count);
    uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;
    
    uint64_t cgen_var_0 = (uint64_t)(uintptr_t)device;
    memcpy((*send_buffer_ptr), &cgen_var_0, 8);
    *send_buffer_ptr += 8;
    
    encode_to_stream_VkDeviceBufferMemoryRequirements(VK_STRUCTURE_TYPE_MAX_ENUM, local_pInfo, send_buffer_ptr);
    encode_to_stream_VkMemoryRequirements2(VK_STRUCTURE_TYPE_MAX_ENUM, local_pMemoryRequirements, send_buffer_ptr);
    
    mgr.addPtr(pMemoryRequirements, sizeof(VkMemoryRequirements2));
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkGetDeviceBufferMemoryRequirements, true);
    
    if (local_pInfo) free(local_pInfo);
    if (local_pMemoryRequirements) free(local_pMemoryRequirements);
}

VKAPI_ATTR void GetDeviceImageMemoryRequirements(VkDevice device, 
                                                  const VkDeviceImageMemoryRequirements* pInfo,
                                                  VkMemoryRequirements2* pMemoryRequirements) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    Allocator vkAllocator;
    
    VkDeviceImageMemoryRequirements* local_pInfo = nullptr;
    if (pInfo) {
        local_pInfo = (VkDeviceImageMemoryRequirements*)malloc(sizeof(VkDeviceImageMemoryRequirements));
        deepcopy_VkDeviceImageMemoryRequirements(&vkAllocator, VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS, pInfo, local_pInfo);
    }
    
    VkMemoryRequirements2* local_pMemoryRequirements = nullptr;
    if (pMemoryRequirements) {
        local_pMemoryRequirements = (VkMemoryRequirements2*)malloc(sizeof(VkMemoryRequirements2));
        deepcopy_VkMemoryRequirements2(&vkAllocator, VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, pMemoryRequirements, local_pMemoryRequirements);
    }
    
    size_t count = 0;
    size_t* countPtr = &count;
    count_VkDeviceImageMemoryRequirements(0, VK_STRUCTURE_TYPE_MAX_ENUM, local_pInfo, countPtr);
    count_VkMemoryRequirements2(0, VK_STRUCTURE_TYPE_MAX_ENUM, local_pMemoryRequirements, countPtr);
    count += 8;
    
    char* send_buffer = (char*)mgr.addExternalParamPtr(count);
    uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;
    
    uint64_t cgen_var_0 = (uint64_t)(uintptr_t)device;
    memcpy((*send_buffer_ptr), &cgen_var_0, 8);
    *send_buffer_ptr += 8;
    
    encode_to_stream_VkDeviceImageMemoryRequirements(VK_STRUCTURE_TYPE_MAX_ENUM, local_pInfo, send_buffer_ptr);
    encode_to_stream_VkMemoryRequirements2(VK_STRUCTURE_TYPE_MAX_ENUM, local_pMemoryRequirements, send_buffer_ptr);
    
    mgr.addPtr(pMemoryRequirements, sizeof(VkMemoryRequirements2));
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkGetDeviceImageMemoryRequirements, true);
    
    if (local_pInfo) free(local_pInfo);
    if (local_pMemoryRequirements) free(local_pMemoryRequirements);
}

VKAPI_ATTR void GetDeviceImageSparseMemoryRequirements(VkDevice device, 
                                                        const VkDeviceImageMemoryRequirements* pInfo,
                                                        uint32_t* pSparseMemoryRequirementCount,
                                                        VkSparseImageMemoryRequirements2* pSparseMemoryRequirements) {
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    Allocator vkAllocator;
    
    VkDeviceImageMemoryRequirements* local_pInfo = nullptr;
    if (pInfo) {
        local_pInfo = (VkDeviceImageMemoryRequirements*)malloc(sizeof(VkDeviceImageMemoryRequirements));
        deepcopy_VkDeviceImageMemoryRequirements(&vkAllocator, VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS, pInfo, local_pInfo);
    }
    
    size_t count = 0;
    size_t* countPtr = &count;
    count_VkDeviceImageMemoryRequirements(0, VK_STRUCTURE_TYPE_MAX_ENUM, local_pInfo, countPtr);
    count += 8;
    
    char* send_buffer = (char*)mgr.addExternalParamPtr(count);
    uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;
    
    uint64_t cgen_var_0 = (uint64_t)(uintptr_t)device;
    memcpy((*send_buffer_ptr), &cgen_var_0, 8);
    *send_buffer_ptr += 8;
    
    encode_to_stream_VkDeviceImageMemoryRequirements(VK_STRUCTURE_TYPE_MAX_ENUM, local_pInfo, send_buffer_ptr);
    
    mgr.addPtr(pSparseMemoryRequirementCount, sizeof(uint32_t));
    if (pSparseMemoryRequirements && *pSparseMemoryRequirementCount > 0) {
        mgr.addPtr(pSparseMemoryRequirements, sizeof(VkSparseImageMemoryRequirements2) * (*pSparseMemoryRequirementCount));
    }
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkGetDeviceImageSparseMemoryRequirements, true);
    
    if (local_pInfo) free(local_pInfo);
}

VKAPI_ATTR VkResult GetPhysicalDeviceToolProperties(
    VkPhysicalDevice physicalDevice,
    uint32_t* pToolCount,
    VkPhysicalDeviceToolProperties* pToolProperties) {
    
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    uint64_t guest_physical_device = (uint64_t)(uintptr_t)physicalDevice;
    
    mgr.addParam64(guest_physical_device);
    mgr.addPtr(pToolCount, sizeof(uint32_t));
    
    if (pToolProperties) {
        mgr.addPtr(pToolProperties, sizeof(VkPhysicalDeviceToolProperties) * (*pToolCount));
    }
    
    VkResult vkResult = VK_SUCCESS;
    mgr.addPtr(&vkResult, sizeof(VkResult));
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkGetPhysicalDeviceToolProperties, true);
    
    return vkResult;
}

VKAPI_ATTR void GetPrivateData(
    VkDevice device,
    VkObjectType objectType,
    uint64_t objectHandle,
    VkPrivateDataSlot privateDataSlot,
    uint64_t* pData) {
    
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    uint64_t guest_device = (uint64_t)(uintptr_t)device;
    uint64_t guest_private_data_slot = (uint64_t)(uintptr_t)privateDataSlot;
    
    mgr.addParam64(guest_device);
    mgr.addParam32((uint32_t)objectType);
    mgr.addParam64(objectHandle);
    mgr.addParam64(guest_private_data_slot);
    mgr.addPtr(pData, sizeof(uint64_t));
    
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkGetPrivateData, true);
}

VKAPI_ATTR VkResult QueueSubmit2(
    VkQueue queue,
    uint32_t submitCount,
    const VkSubmitInfo2* pSubmits,
    VkFence fence) {
    
    const uint64_t rpc_start_us = ExpressVkNowUs();
    if (submitCount != 0 && pSubmits == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    FlushPendingSubmitCohortForQueue(queue, "queue_submit2");
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    Allocator vkAllocator;
    SubmitSyncHints submit_hints;

    if (kEnableImplicitGlobalMappedSync) {
        submit_hints = PrimeFlushHintsFromSubmitInfo2(submitCount, pSubmits);
        struct timespec t0_imp, t1_imp;
        clock_gettime(CLOCK_MONOTONIC, &t0_imp);
        ImplicitFlushAllMappedMemories();
        clock_gettime(CLOCK_MONOTONIC, &t1_imp);
        if (kEnableImplicitSyncDiagLog) {
            double imp_ms = (t1_imp.tv_sec - t0_imp.tv_sec) * 1000.0 +
                            (t1_imp.tv_nsec - t0_imp.tv_nsec) / 1000000.0;
            EVK_PER_CALL_LOG("[SYNC_GUEST] queue_submit2_implicit_flush queue=%llx submitCount=%u ms=%.3f",
                             (unsigned long long)(uintptr_t)queue,
                             submitCount,
                             imp_ms);
        }
        FinalizeSubmitWaitFlushRanges(&submit_hints);
    }
    const uint64_t hint_done_us = ExpressVkNowUs();
    
    const bool defer_fence_wait = ShouldDeferFenceWaitForSubmit(fence, submit_hints);
    NoteReadbackFenceDecision("vkQueueSubmit2", fence, submit_hints, defer_fence_wait);
    VkFence host_fence = defer_fence_wait ? VK_NULL_HANDLE : fence;
    uint64_t guest_queue = (uint64_t)(uintptr_t)queue;
    uint64_t guest_fence = (uint64_t)(uintptr_t)host_fence;
    uint64_t command_buffer_count = 0;
    uint64_t wait_semaphore_count = 0;
    uint64_t signal_semaphore_count = 0;

    const uint64_t encode_start_us = ExpressVkNowUs();
    VkSubmitInfo2* local_pSubmits = nullptr;
    if (submitCount > SIZE_MAX / sizeof(VkSubmitInfo2)) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    if (pSubmits) {
        local_pSubmits = (VkSubmitInfo2*)malloc(submitCount * sizeof(VkSubmitInfo2));
        if (!local_pSubmits) {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        for (uint32_t i = 0; i < submitCount; ++i) {
            wait_semaphore_count += pSubmits[i].waitSemaphoreInfoCount;
            command_buffer_count += pSubmits[i].commandBufferInfoCount;
            signal_semaphore_count += pSubmits[i].signalSemaphoreInfoCount;
            deepcopy_VkSubmitInfo2(&vkAllocator, VK_STRUCTURE_TYPE_SUBMIT_INFO_2, 
                                  &pSubmits[i], &local_pSubmits[i]);
        }
    }
    
    size_t count = 0;
    size_t* countPtr = &count;
    
    for (uint32_t i = 0; i < submitCount; ++i) {
        const size_t size_before = count;
        count_VkSubmitInfo2(0, VK_STRUCTURE_TYPE_MAX_ENUM, 
                           &local_pSubmits[i], countPtr);
        if (count < size_before ||
            count > static_cast<size_t>(INT_MAX)) {
            if (local_pSubmits) free(local_pSubmits);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
    }
    size_t hint_wire_size = SubmitSyncHintsWireSize(submit_hints);
    const size_t fixed_size =
        sizeof(uint64_t) * 2 + sizeof(uint32_t);
    if (count > static_cast<size_t>(INT_MAX) - fixed_size ||
        hint_wire_size >
            static_cast<size_t>(INT_MAX) - fixed_size - count) {
        if (local_pSubmits) free(local_pSubmits);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    count += fixed_size + hint_wire_size;
    
    char* send_buffer = (char*)mgr.addExternalParamPtr(
        static_cast<int>(count));
    uint8_t** send_buffer_ptr = (uint8_t**)&send_buffer;
    
    memcpy(*send_buffer_ptr, &guest_queue, sizeof(uint64_t));
    *send_buffer_ptr += sizeof(uint64_t);
    memcpy(*send_buffer_ptr, &guest_fence, sizeof(uint64_t));
    *send_buffer_ptr += sizeof(uint64_t);
    memcpy(*send_buffer_ptr, &submitCount, sizeof(uint32_t));
    *send_buffer_ptr += sizeof(uint32_t);
    
    for (uint32_t i = 0; i < submitCount; ++i) {
        encode_to_stream_VkSubmitInfo2(VK_STRUCTURE_TYPE_MAX_ENUM, 
                                      &local_pSubmits[i], send_buffer_ptr);
    }
    WriteSubmitSyncHintsWire(send_buffer_ptr, submit_hints);
    VkResult vkResult = VK_ERROR_DEVICE_LOST;
    mgr.addPtr(&vkResult, sizeof(vkResult));

    FlimeGuestSubmitToken flime_submit_token = {};
    const FlimeGuestSubmitGate flime_submit_gate =
        FlimeGuestBeforeQueueSubmit2(queue,
                                     submitCount,
                                     pSubmits,
                                     fence,
                                     &flime_submit_token);
    if (flime_submit_gate == FLIME_GUEST_SUBMIT_BLOCKED) {
        mgr.clear();
        if (local_pSubmits) free(local_pSubmits);
        ALOGE("FLIME blocked vkQueueSubmit2 before the host commit");
        return VK_ERROR_DEVICE_LOST;
    }
    const uint64_t encode_done_us = ExpressVkNowUs();
    
    const uint64_t write_start_us = ExpressVkNowUs();
    const ssize_t transport_bytes =
        FlimeGuestWrite(&mgr,
                        express_gpu,
                        EXPRESS_GPU_DEVICE_ID,
                        FUNID_vkQueueSubmit2,
                        true);
    const uint64_t write_done_us = ExpressVkNowUs();
    if (!IsCompleteParamManagerWrite(transport_bytes, 2)) {
        vkResult = VK_ERROR_DEVICE_LOST;
    }
    FlimeGuestAfterQueueSubmit(&flime_submit_token, vkResult);
    if (vkResult == VK_SUCCESS &&
        kEnableImplicitGlobalMappedSync && fence != VK_NULL_HANDLE) {
        StoreFenceInvalidateHints(fence, submit_hints);
    }
    if (vkResult == VK_SUCCESS && defer_fence_wait) {
        TrackDeferredFenceWait(fence, queue);
    }
    const uint64_t rpc_done_us = ExpressVkNowUs();
    NoteSubmitHintStats("vkQueueSubmit2",
                        submit_hints,
                        hint_wire_size,
                        defer_fence_wait,
                        submitCount);
    NoteSubmitRpcStats("vkQueueSubmit2",
                       submitCount,
                       command_buffer_count,
                       wait_semaphore_count,
                       signal_semaphore_count,
                       fence != VK_NULL_HANDLE,
                       defer_fence_wait,
                       false,
                       hint_done_us - rpc_start_us,
                       encode_done_us - encode_start_us,
                       write_done_us - write_start_us,
                       rpc_done_us - rpc_start_us);
    NoteSubmitCohortStats("vkQueueSubmit2",
                          queue,
                          submitCount,
                          command_buffer_count,
                          wait_semaphore_count,
                          signal_semaphore_count,
                          fence != VK_NULL_HANDLE,
                          defer_fence_wait,
                          rpc_start_us,
                          rpc_done_us);
    MemShapeNoteSubmit("queue_submit2");
    
    if (local_pSubmits) free(local_pSubmits);
    
    return vkResult;
}

VKAPI_ATTR VkResult SetPrivateData(
    VkDevice device,
    VkObjectType objectType,
    uint64_t objectHandle,
    VkPrivateDataSlot privateDataSlot,
    uint64_t data) {
    
    int express_gpu = get_express_gpu_fd();
    thread_local ParamManager mgr;
    
    uint64_t guest_device = (uint64_t)(uintptr_t)device;
    uint64_t guest_private_data_slot = (uint64_t)(uintptr_t)privateDataSlot;
    
    mgr.addParam64(guest_device);
    mgr.addParam32((uint32_t)objectType);
    mgr.addParam64(objectHandle);
    mgr.addParam64(guest_private_data_slot);
    mgr.addParam64(data);
    
    FlimeGuestWrite(&mgr, express_gpu, EXPRESS_GPU_DEVICE_ID, FUNID_vkSetPrivateData, false);
    
    return VK_SUCCESS;
}

#pragma clang diagnostic pop
// clang-format on

}  // namespace null_driver
