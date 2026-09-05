/*
 * Core state, control protocol, descriptor shadow, and route planner.
 *
 * Wire formats are intentionally written field by field. Host and guest may
 * have different compilers, pointer widths, or Vulkan header revisions, so
 * native C++ layout is never part of the protocol.
 */
#include "express_vk_flime_guest_internal.h"

#include "define_vk.h"

#include <algorithm>
#include <atomic>
#include <errno.h>
#include <limits>
#include <new>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <log/log.h>

#ifndef FUNID_vkExpressFlimeControlANDROID
#define FUNID_vkExpressFlimeControlANDROID 1906
#endif

#ifndef FUNID_vkExpressFlimeRoutedDescriptorUpdatesANDROID
#define FUNID_vkExpressFlimeRoutedDescriptorUpdatesANDROID 1907
#endif

namespace flime_guest_internal {

const uint32_t kWireMagic = 0x4d494c46u;       // FLIM
const uint32_t kControlMagic = 0x434c4646u;    // FFLC
const uint32_t kCapsReplyMagic = 0x52434646u;  // FFCR
const uint32_t kRouteMagic = 0x44524c46u;      // FLRD
const uint32_t kRouteReplyMagic = 0x52524c46u; // FLRR
const uint32_t kControlPageMagic = 0x50434c46u;// FLCP

const uint16_t kProtocolMajor = 1;
const uint16_t kProtocolMinor = 1;
const size_t kWireHeaderBytes = 64;
const size_t kCapsBytes = 40;
const size_t kProfileBytes = 32;
const size_t kUnitBytes = 48;
const size_t kChunkBytes = 48;
const size_t kProgressBytes = 32;
const size_t kIntervalBytes = 8;
const size_t kControlHeaderBytes = 64;
const size_t kControlBoundaryBytes = 16;
const size_t kCapsReplyBytes = 48;
const size_t kRouteHeaderBytes = 104;
const size_t kRouteRecordBytes = 64;
const size_t kRouteReplyBytes = 64;
const size_t kControlPageHeaderBytes = 32;
const size_t kControlPageBytes = 1120;
const size_t kControlAllocationBytes = 4096;

const uint32_t kMaxRoutePacketBytes = 16u * 1024u * 1024u;
const uint32_t kMaxRouteRecords = 4096;
const uint32_t kHardMaxUnits = 4096;
const uint32_t kHardMaxChunks = 64;
const uint32_t kHistoryLimit = 256;
const uint32_t kDispatchesPerUnit = 10;
const uint32_t kReplanPeriods = 32;
const size_t kMaxSemanticCalls = 256u * 1024u;
const uint64_t kMaxTemplateBytes = UINT64_C(256) * 1024u * 1024u;

const uint64_t kCapProgressive = UINT64_C(1) << 0;
const uint64_t kCapDirect = UINT64_C(1) << 1;
const uint64_t kCapRecovery = UINT64_C(1) << 2;
const uint64_t kCapAdaptive = UINT64_C(1) << 3;
const uint64_t kCapUnitProfile = UINT64_C(1) << 4;
const uint64_t kCapChunkProfile = UINT64_C(1) << 5;
const uint64_t kCapEarlyRelease = UINT64_C(1) << 6;
const uint64_t kCapControlPlan = UINT64_C(1) << 7;
const uint64_t kAllCapabilities =
    kCapProgressive | kCapDirect | kCapRecovery | kCapAdaptive |
    kCapUnitProfile | kCapChunkProfile | kCapEarlyRelease | kCapControlPlan;


enum ProgressEvent {
    kProgressLearnComplete = 0,
    kProgressMatchComplete = 1,
    kProgressFastComplete = 2,
    kProgressMismatch = 3,
    kProgressRecoveryComplete = 4,
};


enum PayloadKind {
    kPayloadBuffer = 1,
    kPayloadImage = 2,
    kPayloadTexel = 3,
};

enum ControlPayloadKind {
    kControlCaps = 1,
    kControlPlan = 2,
};

const uint32_t kPeriodSingleFlush = 1u << 0;
const uint32_t kPeriodFineProfile = 1u << 1;
const uint32_t kPeriodStableFast = 1u << 2;
const uint32_t kPeriodForceReplan = 1u << 3;
const uint32_t kUnitFinal = 1u << 0;
const uint16_t kProgressMatchSucceeded = 1u << 0;
const uint16_t kProgressGenericShadowRan = 1u << 1;
const uint16_t kControlLegacy = 1u << 0;
const uint16_t kControlPlanValid = 1u << 1;
const uint16_t kControlRequestFine = 1u << 2;
const uint16_t kRouteBegin = 1u << 0;
const uint16_t kRouteFinal = 1u << 1;
const uint16_t kRouteRecovery = 1u << 2;
const uint16_t kRouteSingle = 1u << 3;
const uint16_t kRouteFallbackFlush = 1u << 4;
const uint16_t kRouteDerived = 1u << 0;
const uint32_t kRouteFallbackRequired = 1u << 0;
const uint32_t kRouteRecoveryRequired = 1u << 4;
const uint32_t kRouteFallbackDrained = 1u << 5;

uint64_t NowNs() {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return static_cast<uint64_t>(ts.tv_sec) * UINT64_C(1000000000) +
           static_cast<uint64_t>(ts.tv_nsec);
}

uint64_t FinishPrepareNs(uint64_t started_ns) {
    const uint64_t finished_ns = NowNs();
    if (started_ns == 0 || finished_ns == 0 ||
        finished_ns < started_ns) {
        return 0;
    }
    return finished_ns - started_ns;
}


uint64_t Mix64(uint64_t value) {
    value ^= value >> 30;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27;
    value *= UINT64_C(0x94d049bb133111eb);
    value ^= value >> 31;
    return value;
}

uint64_t HashWord(uint64_t hash, uint64_t value) {
    return (hash ^ Mix64(value + UINT64_C(0x9e3779b97f4a7c15))) *
           UINT64_C(1099511628211);
}

uint16_t GetLe16(const uint8_t* data) {
    return static_cast<uint16_t>(data[0]) |
           static_cast<uint16_t>(data[1]) << 8;
}

uint32_t GetLe32(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) |
           static_cast<uint32_t>(data[1]) << 8 |
           static_cast<uint32_t>(data[2]) << 16 |
           static_cast<uint32_t>(data[3]) << 24;
}

uint64_t GetLe64(const uint8_t* data) {
    return static_cast<uint64_t>(GetLe32(data)) |
           static_cast<uint64_t>(GetLe32(data + 4)) << 32;
}

void PutLe16(uint8_t* data, uint16_t value) {
    data[0] = static_cast<uint8_t>(value);
    data[1] = static_cast<uint8_t>(value >> 8);
}

void PutLe32(uint8_t* data, uint32_t value) {
    data[0] = static_cast<uint8_t>(value);
    data[1] = static_cast<uint8_t>(value >> 8);
    data[2] = static_cast<uint8_t>(value >> 16);
    data[3] = static_cast<uint8_t>(value >> 24);
}

void PutLe64(uint8_t* data, uint64_t value) {
    PutLe32(data, static_cast<uint32_t>(value));
    PutLe32(data + 4, static_cast<uint32_t>(value >> 32));
}

bool AddSize(size_t left, size_t right, size_t* out) {
    if (out == NULL || left > std::numeric_limits<size_t>::max() - right) {
        return false;
    }
    *out = left + right;
    return true;
}

struct BytePacket {
    explicit BytePacket(size_t bytes) : data(bytes, 0) {}

    bool InRange(size_t offset, size_t bytes) const {
        return offset <= data.size() && bytes <= data.size() - offset;
    }

    void U16(size_t offset, uint16_t value) {
        if (InRange(offset, sizeof(value))) PutLe16(&data[offset], value);
    }

    void U32(size_t offset, uint32_t value) {
        if (InRange(offset, sizeof(value))) PutLe32(&data[offset], value);
    }

    void U64(size_t offset, uint64_t value) {
        if (InRange(offset, sizeof(value))) PutLe64(&data[offset], value);
    }

    std::vector<uint8_t> data;
};

std::mutex g_mutex;
std::condition_variable g_submit_cv;
std::unordered_map<uint64_t, std::shared_ptr<DeviceState> > g_devices;
std::unordered_map<uint64_t, uint64_t> g_queue_devices;
std::unordered_map<uint64_t, uint64_t> g_command_devices;
std::unordered_map<uint64_t, uint64_t> g_set_devices;
std::unordered_map<uint64_t, uint64_t> g_pool_devices;
OwnedCluster g_cluster;
uint64_t g_stream_serial = 1;
thread_local int g_skip_next_fun_id = -1;
thread_local bool g_allow_next_cluster = false;
thread_local bool g_block_next_write = false;
thread_local bool g_flush_cluster_after_write = false;
thread_local uint64_t g_route_after_write_command = 0;
thread_local uint64_t g_named_write_command = 0;

std::shared_ptr<DeviceState> FindDeviceLocked(VkDevice device) {
    const uint64_t key = HandleBits(device);
    std::unordered_map<uint64_t, std::shared_ptr<DeviceState> >::iterator it =
        g_devices.find(key);
    return it == g_devices.end() ? std::shared_ptr<DeviceState>() : it->second;
}

std::shared_ptr<DeviceState> FindByMappedKeyLocked(
    const std::unordered_map<uint64_t, uint64_t>& mapping,
    uint64_t object) {
    std::unordered_map<uint64_t, uint64_t>::const_iterator owner =
        mapping.find(object);
    if (owner == mapping.end()) return std::shared_ptr<DeviceState>();
    std::unordered_map<uint64_t, std::shared_ptr<DeviceState> >::iterator state =
        g_devices.find(owner->second);
    return state == g_devices.end() ? std::shared_ptr<DeviceState>() : state->second;
}

std::shared_ptr<DeviceState> FindQueueLocked(VkQueue queue) {
    return FindByMappedKeyLocked(g_queue_devices, HandleBits(queue));
}

std::shared_ptr<DeviceState> FindCommandLocked(VkCommandBuffer command_buffer) {
    return FindByMappedKeyLocked(g_command_devices, HandleBits(command_buffer));
}

void InitializeWireHeader(BytePacket* packet,
                          WireType type,
                          uint32_t flags,
                          uint32_t records,
                          const DeviceState& state,
                          uint64_t period_id,
                          uint64_t plan_epoch) {
    packet->U32(0, kWireMagic);
    packet->U16(4, kProtocolMajor);
    packet->U16(6, kProtocolMinor);
    packet->U16(8, static_cast<uint16_t>(type));
    packet->U16(10, static_cast<uint16_t>(kWireHeaderBytes));
    packet->U32(12, static_cast<uint32_t>(packet->data.size()));
    packet->U32(16, flags);
    packet->U32(20, records);
    packet->U64(24, state.process_id);
    packet->U64(32, state.stream_id);
    packet->U64(40, period_id);
    packet->U64(48, plan_epoch);
}

 * The host publishes this word with a 64-bit qatomic_set.  i686 gives a
 * plain uint64_t only four-byte type alignment even though the control page
 * and offset are both eight-byte aligned.  Preserve the single-copy seqlock
 * load and make that stronger ABI guarantee visible to Clang.
 */
const size_t kControlSequenceOffset = 24;
typedef uint64_t FlimeAlignedControlSequence
    __attribute__((aligned(8)));

static_assert(alignof(FlimeAlignedControlSequence) == 8);
static_assert(kControlSequenceOffset +
                  sizeof(FlimeAlignedControlSequence) ==
              kControlPageHeaderBytes);

bool LoadControlSequenceAcquire(const DeviceState* state, uint64_t* value) {
    if (state == NULL || state->control_page == NULL || value == NULL) {
        return false;
    }
    const uint8_t* address =
        state->control_page + kControlSequenceOffset;
    if ((reinterpret_cast<uintptr_t>(address) &
         (alignof(FlimeAlignedControlSequence) - 1)) != 0) {
        return false;
    }
    const FlimeAlignedControlSequence* sequence =
        reinterpret_cast<const FlimeAlignedControlSequence*>(address);
    *value = __atomic_load_n(sequence, __ATOMIC_ACQUIRE);
    return true;
}

std::unordered_map<uint64_t, RecordedCommandStream> g_recorded_commands;

void EnterLegacy(DeviceState* state);
void ClearEarlyRoute(DeviceState* state);

bool IsSpecialized(const DeviceState& state) {
    return state.negotiated && state.stage != kStageDetect &&
           state.stage != kStageLegacy;
}

bool HasRequiredCapabilities(const DeviceState& state) {
    return (state.capabilities & kAllCapabilities) == kAllCapabilities;
}

bool CopyControlSnapshot(DeviceState* state,
                         uint16_t* payload_kind,
                         std::vector<uint8_t>* payload) {
    if (state == NULL || state->control_page == NULL || payload_kind == NULL ||
        payload == NULL) {
        return false;
    }
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        uint64_t first;
        if (!LoadControlSequenceAcquire(state, &first)) return false;
        if ((first & 1u) != 0) continue;
        uint8_t header[kControlPageHeaderBytes];
        memcpy(header, state->control_page, sizeof(header));
        if (GetLe32(header) != kControlPageMagic ||
            GetLe16(header + 4) != kProtocolMajor ||
            GetLe16(header + 6) > kProtocolMinor ||
            GetLe16(header + 8) != kControlPageHeaderBytes ||
            GetLe32(header + 12) < kControlPageHeaderBytes ||
            GetLe32(header + 12) > kControlPageBytes ||
            GetLe32(header + 16) >
                GetLe32(header + 12) - kControlPageHeaderBytes ||
            GetLe32(header + 20) != 0 || GetLe64(header + 24) != first) {
            return false;
        }
        const uint32_t payload_bytes = GetLe32(header + 16);
        std::vector<uint8_t> copy(payload_bytes);
        if (payload_bytes != 0) {
            memcpy(&copy[0], state->control_page + kControlPageHeaderBytes,
                   payload_bytes);
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        uint64_t second;
        if (!LoadControlSequenceAcquire(state, &second)) return false;
        if (first != second || (second & 1u) != 0) continue;
        *payload_kind = GetLe16(header + 10);
        payload->swap(copy);
        state->control_sequence = second;
        return true;
    }
    return false;
}

bool ParseCaps(DeviceState* state, const std::vector<uint8_t>& payload) {
    if (state == NULL || payload.size() != kCapsReplyBytes ||
        GetLe32(&payload[0]) != kCapsReplyMagic ||
        GetLe16(&payload[4]) != kProtocolMajor ||
        GetLe16(&payload[6]) > kProtocolMinor ||
        GetLe16(&payload[8]) != kCapsReplyBytes ||
        GetLe32(&payload[12]) != 0 || GetLe64(&payload[40]) != 0) {
        return false;
    }
    const uint16_t status = GetLe16(&payload[10]);
    if (status == 1u) {
        EnterLegacy(state);
        state->capabilities = 0;
        return true;
    }
    if (status != 0u) return false;
    state->capabilities = GetLe64(&payload[16]);
    state->max_units = GetLe32(&payload[24]);
    state->max_chunks = GetLe32(&payload[28]);
    state->dispatches_per_unit = GetLe32(&payload[32]);
    state->replan_periods = GetLe32(&payload[36]);
    return state->max_units != 0 && state->max_units <= kHardMaxUnits &&
           state->max_chunks != 0 && state->max_chunks <= kHardMaxChunks &&
           state->dispatches_per_unit == kDispatchesPerUnit &&
           state->replan_periods == kReplanPeriods &&
           HasRequiredCapabilities(*state);
}

bool ParsePlan(DeviceState* state, const std::vector<uint8_t>& payload) {
    if (state == NULL || payload.size() < kControlHeaderBytes ||
        GetLe32(&payload[0]) != kControlMagic ||
        GetLe16(&payload[4]) != kProtocolMajor ||
        GetLe16(&payload[6]) > kProtocolMinor ||
        GetLe16(&payload[8]) != kControlHeaderBytes ||
        GetLe32(&payload[12]) != payload.size() ||
        GetLe64(&payload[16]) != state->process_id ||
        GetLe64(&payload[24]) != state->stream_id ||
        GetLe32(&payload[52]) != 0 ||
        GetLe64(&payload[56]) != state->capabilities) {
        return false;
    }
    const uint16_t flags = GetLe16(&payload[10]);
    const uint32_t count = GetLe32(&payload[48]);
    size_t expected;
    if ((flags & ~(kControlLegacy | kControlPlanValid |
                   kControlRequestFine)) != 0 ||
        !AddSize(kControlHeaderBytes,
                 static_cast<size_t>(count) * kControlBoundaryBytes,
                 &expected) || expected != payload.size() ||
        count > state->max_chunks) {
        return false;
    }
    if ((flags & kControlLegacy) != 0) {
        EnterLegacy(state);
        return true;
    }
    state->request_fine_profile = (flags & kControlRequestFine) != 0;
    if ((flags & kControlPlanValid) == 0) {
        state->plan_valid = false;
        state->plan.clear();
        return count == 0 && GetLe64(&payload[32]) == 0;
    }
    const uint64_t epoch = GetLe64(&payload[32]);
    const uint64_t apply_period = GetLe64(&payload[40]);
    if (epoch == 0 || apply_period == 0 || count == 0) return false;
    std::vector<PlanBoundary> plan;
    plan.reserve(count);
    uint32_t prior_unit = 0;
    uint64_t prior_offset = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const size_t offset = kControlHeaderBytes + i * kControlBoundaryBytes;
        PlanBoundary boundary;
        boundary.unit_past_end = GetLe32(&payload[offset]);
        boundary.flags = GetLe32(&payload[offset + 4]);
        boundary.template_offset = GetLe64(&payload[offset + 8]);
        if (boundary.unit_past_end <= prior_unit ||
            boundary.template_offset <= prior_offset ||
            boundary.unit_past_end > state->max_units ||
            (boundary.flags & ~kUnitFinal) != 0 ||
            ((i + 1 == count) != ((boundary.flags & kUnitFinal) != 0))) {
            return false;
        }
        plan.push_back(boundary);
        prior_unit = boundary.unit_past_end;
        prior_offset = boundary.template_offset;
    }
    state->plan_epoch = epoch;
    state->plan_apply_period = apply_period;
    state->plan.swap(plan);
    state->plan_valid = true;
    return true;
}

bool RefreshControlPage(DeviceState* state) {
    if (state == NULL || state->control_page == NULL) return false;
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        uint64_t sequence;
        if (!LoadControlSequenceAcquire(state, &sequence)) return false;
        if ((sequence & 1u) != 0) continue;
        if (sequence == state->control_sequence) return true;
        uint16_t kind = 0;
        std::vector<uint8_t> payload;
        if (!CopyControlSnapshot(state, &kind, &payload)) continue;
        if (kind == kControlCaps) return ParseCaps(state, payload);
        if (kind == kControlPlan) return ParsePlan(state, payload);
        return false;
    }
    return false;
}

ssize_t FlushClusterLocked(int fd) {
    if (g_cluster.Empty()) return 0;
    if (g_cluster.fd != fd || g_cluster.calls.empty() ||
        g_cluster.payload.size() <= 1) {
        errno = EPROTO;
        return -1;
    }
    ParamManager outer;
    outer.addPtr(&g_cluster.calls[0], static_cast<int>(g_cluster.calls.size()));
    outer.addPtr(&g_cluster.payload[0],
                 static_cast<int>(g_cluster.payload.size()));
    const ssize_t result = outer.write(fd, EXPRESS_GPU_DEVICE_ID, 9999, true);
    if (result == 48) g_cluster.Clear();
    return result;
}

bool FlushAnyClusterLocked() {
    if (g_cluster.Empty()) return true;
    const int fd = g_cluster.fd;
    return fd >= 0 && FlushClusterLocked(fd) == 48;
}

bool SendControlLocked(DeviceState* state, const std::vector<uint8_t>& packet) {
    if (state == NULL || state->fd < 0 || state->control_page == NULL ||
        packet.size() < kWireHeaderBytes ||
        packet.size() > kMaxRoutePacketBytes ||
        !FlushAnyClusterLocked()) {
        return false;
    }
    /* Consume an asynchronous planner publication before taking the request's
     * response-generation watermark.  Every successful control RPC publishes
     * a fresh seqlock snapshot; accepting an unchanged page would silently
     * treat a host-side validation failure as success. */
    if (!RefreshControlPage(state)) return false;
    const uint64_t previous_sequence = state->control_sequence;
    ParamManager manager;
    manager.addPtr(const_cast<uint8_t*>(&packet[0]),
                   static_cast<int>(packet.size()));
    manager.addPtr(state->control_page, static_cast<int>(kControlPageBytes));
    state->control_page_exposed = true;
    if (manager.write(state->fd, EXPRESS_GPU_DEVICE_ID,
                      FUNID_vkExpressFlimeControlANDROID, true) != 48) {
        return false;
    }
    return RefreshControlPage(state) &&
           state->control_sequence != previous_sequence;
}

void ClearOccurrence(DeviceState* state,
                     bool preserve_descriptor_records) {
    if (state == NULL) return;
    if (!preserve_descriptor_records) {
        for (std::unordered_map<uint64_t, SetState>::iterator set =
                 state->sets.begin(); set != state->sets.end(); ++set) {
            for (std::map<
                     std::pair<uint32_t, uint32_t>,
                     DescriptorSlot>::iterator slot =
                     set->second.slots.begin();
                 slot != set->second.slots.end(); ++slot) {
                slot->second.pending_record = -1;
            }
        }
        state->records.clear();
    }
    state->calls.clear();
    state->units.clear();
    state->chunks.clear();
    state->template_bytes = 0;
    state->interval_hash = UINT64_C(1469598103934665603);
    state->dispatch_count = 0;
    state->generic_shadow_ran = false;
    state->recovery_checkpoint.Clear();
    state->descriptor_plan_cursor = 0;
    state->building_descriptor_cache_complete = true;
    state->building_descriptor_plans.clear();
    state->descriptor_role_bindings.clear();
    state->descriptor_handle_roles.clear();
    state->mismatch_pending = false;
    if (state->occurrence_serial == UINT64_MAX) {
        state->transport_failed = true;
    } else {
        ++state->occurrence_serial;
    }
    state->active_period_id = 0;
    state->active_period_flags = 0;
    state->active_submission_id = 0;
    state->active_chunk_count = 0;
    state->active_plan_epoch = 0;
    state->period_start_ns = 0;
}

void ClearActiveSubmit(DeviceState* state) {
    if (state == NULL) return;
    state->active_submit = false;
    state->active_recovery = false;
    state->active_learning = false;
    state->active_fast = false;
    state->active_token = 0;
    state->active_template_bytes = 0;
    state->active_signature = 0;
    state->active_dispatches = 0;
    state->active_calls.clear();
    state->active_primary_commands.clear();
    state->active_descriptor_plans.clear();
    state->active_descriptor_cache_complete = false;
    state->active_generic_shadow_ran = false;
    state->recovery_checkpoint.Clear();
    state->active_consumed_descriptor_sets.clear();
    state->active_frontier_update_ids.clear();
    /* These bindings are occurrence-local, including aborted submissions. */
    state->descriptor_role_bindings.clear();
    state->descriptor_handle_roles.clear();
    state->active_period_id = 0;
    state->active_period_flags = 0;
    state->active_submission_id = 0;
    state->active_chunk_count = 0;
    state->active_plan_epoch = 0;
    state->period_start_ns = 0;
    state->units.clear();
    state->chunks.clear();
}

void EnterLegacy(DeviceState* state) {
    if (state == NULL) return;
    state->stage = kStageLegacy;
    state->negotiated = false;
    state->plan_valid = false;
    state->plan.clear();
    state->learned.clear();
    state->learned_descriptor_plans.clear();
    state->learned_descriptor_cache_complete = false;
    state->learned_signature = 0;
    state->learned_primary_commands.clear();
    state->learned_submit_call = SemanticCall();
    state->acked_plan_epoch = 0;
    state->interval_announced = false;
    state->session_invalidated = false;
    ClearEarlyRoute(state);
    ClearOccurrence(state);
    ClearActiveSubmit(state);
}

bool AppendSemantic(std::vector<SemanticCall>* calls,
                    uint64_t* bytes,
                    uint32_t* dispatches,
                    int fun_id,
                    const uint64_t* structural,
                    size_t structural_count,
                    const uint64_t* handles,
                    size_t handle_count,
                    uint64_t payload_hash,
                    uint64_t encoded_bytes,
                    bool dispatch) {
    if (calls == NULL || bytes == NULL || dispatches == NULL) return false;
    SemanticCall call;
    call.fun_id = fun_id;
    if (structural != NULL) {
        call.structural.assign(structural, structural + structural_count);
    }
    if (handles != NULL) call.handles.assign(handles, handles + handle_count);
    call.payload_hash = payload_hash;
    call.encoded_bytes = std::max<uint64_t>(encoded_bytes, 1);
    if (*bytes > kMaxTemplateBytes - call.encoded_bytes ||
        calls->size() >= kMaxSemanticCalls) {
        return false;
    }
    call.template_offset = *bytes;
    call.dispatch = dispatch;
    call.execute_secondary = false;
    calls->push_back(call);
    *bytes += call.encoded_bytes;
    if (dispatch) ++*dispatches;
    return true;
}

bool AppendCommandSemanticLocked(VkCommandBuffer command_buffer,
                                 int fun_id,
                                 const uint64_t* structural,
                                 size_t structural_count,
                                 const uint64_t* handles,
                                 size_t handle_count,
                                 uint64_t payload_hash,
                                 uint64_t encoded_bytes,
                                 bool dispatch) {
    const uint64_t prepare_started_ns = NowNs();
    const uint64_t key = HandleBits(command_buffer);
    std::shared_ptr<DeviceState> state = FindCommandLocked(command_buffer);
    if (!state || state->stage == kStageLegacy) return false;
    std::unordered_map<uint64_t, CommandState>::const_iterator command =
        state->commands.find(key);
    if (command == state->commands.end() || !command->second.recording) {
        return false;
    }
    RecordedCommandStream& stream = g_recorded_commands[key];
    if (!AppendSemantic(
        &stream.calls, &stream.bytes, &stream.dispatches, fun_id,
        structural, structural_count, handles, handle_count,
        payload_hash, encoded_bytes, dispatch)) {
        return false;
    }
    stream.calls.back().prepare_ns =
        FinishPrepareNs(prepare_started_ns);
    return true;
}

bool AppendExecuteSemanticLocked(VkCommandBuffer command_buffer,
                                 int fun_id,
                                 uint32_t command_count,
                                 const VkCommandBuffer* commands,
                                 uint64_t encoded_bytes) {
    const uint64_t prepare_started_ns = NowNs();
    uint64_t structural[1] = { command_count };
    std::vector<uint64_t> handles;
    handles.reserve(command_count);
    for (uint32_t i = 0; i < command_count; ++i) {
        handles.push_back(HandleBits(commands[i]));
    }
    if (!AppendCommandSemanticLocked(
            command_buffer, fun_id, structural, 1,
            handles.empty() ? NULL : &handles[0],
            handles.size(), 0, encoded_bytes, false)) {
        return false;
    }
    RecordedCommandStream &stream =
        g_recorded_commands[HandleBits(command_buffer)];
    if (!stream.calls.empty()) {
        stream.calls.back().execute_secondary = true;
        stream.calls.back().secondary_commands = handles;
        stream.calls.back().prepare_ns =
            FinishPrepareNs(prepare_started_ns);
    }
    return true;
}

void MarkPreWriteSemantic(int fun_id, bool allow_cluster) {
    g_skip_next_fun_id = fun_id;
    g_allow_next_cluster = allow_cluster;
    g_block_next_write = false;
    g_flush_cluster_after_write = false;
    g_route_after_write_command = 0;
    g_named_write_command = 0;
}

/*
 * Authoritative descriptor shadow for the FLIME guest bridge.
 *
 * Shared wire/state declarations live in express_vk_flime_guest_internal.h.
 * Mutations follow a validate-then-commit rule: no observable shadow state
 * changes until an entire Vulkan operation has passed validation.
 */


static const uint32_t kFlimeShadowMaxDescriptors = 1024u * 1024u;

/*
 * Object handles may be recycled after destroy/free.  Fast plans therefore
 * guard a monotonic guest-side lifetime number in addition to the handle bits.
 * Zero and UINT64_MAX are never published, so wrap is fail-closed.
 */
static bool FlimeNextDescriptorGeneration(DeviceState *device,
                                          uint64_t *generation) {
    if (device == NULL || generation == NULL ||
        device->next_descriptor_generation == 0u ||
        device->next_descriptor_generation == UINT64_MAX) {
        return false;
    }
    *generation = device->next_descriptor_generation++;
    return true;
}

static bool FlimeDescriptorTypeIsBuffer(VkDescriptorType type) {
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

static bool FlimeDescriptorTypeIsImage(VkDescriptorType type) {
    switch (type) {
    case VK_DESCRIPTOR_TYPE_SAMPLER:
    case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
    case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
    case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
    case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
        return true;
    default:
        return false;
    }
}

static bool FlimeDescriptorTypeIsTexel(VkDescriptorType type) {
    return type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER ||
           type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
}

static bool FlimeDescriptorTypeIsSupported(VkDescriptorType type) {
    return FlimeDescriptorTypeIsBuffer(type) ||
           FlimeDescriptorTypeIsImage(type) ||
           FlimeDescriptorTypeIsTexel(type);
}

static const LayoutBinding *FlimeFindLayoutBinding(const LayoutState &layout,
                                                    uint32_t binding) {
    for (size_t i = 0; i < layout.bindings.size(); ++i) {
        if (layout.bindings[i].binding == binding) {
            return &layout.bindings[i];
        }
    }
    return NULL;
}

static bool FlimeSameImmutableSamplers(const LayoutBinding &a,
                                       const LayoutBinding &b) {
    if (a.immutable_samplers.size() != b.immutable_samplers.size()) {
        return false;
    }
    for (size_t i = 0; i < a.immutable_samplers.size(); ++i) {
        if (HandleBits(a.immutable_samplers[i]) !=
            HandleBits(b.immutable_samplers[i])) {
            return false;
        }
    }
    return true;
}

static bool FlimeBindingsCanContinue(const LayoutBinding &first,
                                     const LayoutBinding &next) {
    return next.binding == first.binding + 1u &&
           next.type == first.type &&
           next.stages == first.stages &&
           FlimeSameImmutableSamplers(first, next);
}

static bool FlimeBuildLayoutState(const VkDescriptorSetLayoutCreateInfo *info,
                                  LayoutState *out) {
    if (info == NULL || out == NULL ||
        info->sType != VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO ||
        (info->bindingCount != 0u && info->pBindings == NULL) ||
        info->bindingCount > kFlimeShadowMaxDescriptors) {
        return false;
    }

    LayoutState built = {};
    built.supported = info->pNext == NULL;
    std::set<uint32_t> seen;
    uint64_t descriptor_total = 0u;
    for (uint32_t i = 0; i < info->bindingCount; ++i) {
        const VkDescriptorSetLayoutBinding &src = info->pBindings[i];
        LayoutBinding dst = {};
        dst.binding = src.binding;
        dst.type = src.descriptorType;
        dst.count = src.descriptorCount;
        dst.stages = src.stageFlags;
        descriptor_total += src.descriptorCount;
        if (src.descriptorCount == 0u ||
            descriptor_total > kFlimeShadowMaxDescriptors ||
            !FlimeDescriptorTypeIsSupported(src.descriptorType) ||
            !seen.insert(src.binding).second) {
            built.supported = false;
        }
        if (src.pImmutableSamplers != NULL) {
            if (src.descriptorType != VK_DESCRIPTOR_TYPE_SAMPLER &&
                src.descriptorType != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
                built.supported = false;
            } else {
                dst.immutable_samplers.assign(src.pImmutableSamplers,
                                              src.pImmutableSamplers +
                                                  src.descriptorCount);
            }
        }
        built.bindings.push_back(dst);
    }
    std::sort(built.bindings.begin(), built.bindings.end(),
              [](const LayoutBinding &a, const LayoutBinding &b) {
                  return a.binding < b.binding;
              });
    *out = built;
    return true;
}

bool FlimeShadowCreateLayout(DeviceState *device,
                                    VkDescriptorSetLayout layout,
                                    const VkDescriptorSetLayoutCreateInfo *info) {
    LayoutState built = {};
    const uint64_t key = HandleBits(layout);
    if (device == NULL || key == 0u ||
        device->layouts.find(key) != device->layouts.end() ||
        !FlimeBuildLayoutState(info, &built) ||
        !FlimeNextDescriptorGeneration(device, &built.generation)) {
        return false;
    }
    device->layouts[key] = built;
    return true;
}

bool FlimeShadowDestroyLayout(DeviceState *device,
                                     VkDescriptorSetLayout layout) {
    if (device == NULL) return false;
    const uint64_t key = HandleBits(layout);
    std::unordered_map<uint64_t, LayoutState>::iterator found =
        device->layouts.find(key);
    if (found == device->layouts.end()) return false;
    for (std::unordered_map<uint64_t, UpdateTemplateState>::iterator it =
             device->update_templates.begin();
         it != device->update_templates.end(); ++it) {
        if (HandleBits(it->second.layout) == key) {
            it->second.supported = false;
        }
    }
    device->layouts.erase(found);
    return true;
}

static bool FlimePopulateSetSlots(SetState *set) {
    if (set == NULL) return false;
    uint64_t total = 0u;
    for (size_t i = 0; i < set->layout.bindings.size(); ++i) {
        const LayoutBinding &binding = set->layout.bindings[i];
        total += binding.count;
        if (total > kFlimeShadowMaxDescriptors) return false;
        for (uint32_t element = 0; element < binding.count; ++element) {
            DescriptorSlot slot = {};
            slot.pending_record = -1;
            set->slots[std::make_pair(binding.binding, element)] = slot;
        }
    }
    return true;
}

bool FlimeShadowCreatePool(DeviceState *device,
                                  VkDevice owner,
                                  VkDescriptorPool pool) {
    const uint64_t key = HandleBits(pool);
    if (device == NULL || key == 0u ||
        device->pools.find(key) != device->pools.end()) return false;
    PoolState state = {};
    state.device = owner;
    if (!FlimeNextDescriptorGeneration(device, &state.generation)) {
        return false;
    }
    device->pools[key] = state;
    return true;
}

static void FlimeDiscardSetPending(DeviceState *device, uint64_t set_key) {
    if (device == NULL) return;
    std::unordered_map<uint64_t, SetState>::iterator set_it =
        device->sets.find(set_key);
    if (set_it == device->sets.end()) return;
    for (std::map<std::pair<uint32_t, uint32_t>, DescriptorSlot>::iterator it =
             set_it->second.slots.begin();
         it != set_it->second.slots.end(); ++it) {
        const int64_t index = it->second.pending_record;
        if (index >= 0 && static_cast<size_t>(index) < device->records.size()) {
            PendingRecord &record = device->records[static_cast<size_t>(index)];
            if (!record.released) record.elided = true;
        }
        it->second.pending_record = -1;
    }
}

bool FlimeShadowDestroyPool(DeviceState *device,
                                   VkDescriptorPool pool,
                                   std::vector<uint64_t> *removed_sets) {
    if (device == NULL) return false;
    const uint64_t key = HandleBits(pool);
    std::unordered_map<uint64_t, PoolState>::iterator pool_it =
        device->pools.find(key);
    if (pool_it == device->pools.end()) return false;
    if (removed_sets != NULL) {
        removed_sets->assign(pool_it->second.sets.begin(),
                             pool_it->second.sets.end());
    }
    for (std::set<uint64_t>::const_iterator it = pool_it->second.sets.begin();
         it != pool_it->second.sets.end(); ++it) {
        FlimeDiscardSetPending(device, *it);
        device->sets.erase(*it);
    }
    device->pools.erase(pool_it);
    return true;
}

bool FlimeShadowResetPool(DeviceState *device,
                                 VkDescriptorPool pool,
                                 std::vector<uint64_t> *removed_sets) {
    if (device == NULL) return false;
    std::unordered_map<uint64_t, PoolState>::iterator pool_it =
        device->pools.find(HandleBits(pool));
    if (pool_it == device->pools.end()) return false;
    uint64_t new_generation = 0u;
    if (!FlimeNextDescriptorGeneration(device, &new_generation)) {
        return false;
    }
    if (removed_sets != NULL) {
        removed_sets->assign(pool_it->second.sets.begin(),
                             pool_it->second.sets.end());
    }
    for (std::set<uint64_t>::const_iterator it = pool_it->second.sets.begin();
         it != pool_it->second.sets.end(); ++it) {
        FlimeDiscardSetPending(device, *it);
        device->sets.erase(*it);
    }
    pool_it->second.sets.clear();
    pool_it->second.generation = new_generation;
    return true;
}

bool FlimeShadowAllocateSets(DeviceState *device,
                                    VkDevice owner,
                                    VkDescriptorPool pool,
                                    uint32_t count,
                                    const VkDescriptorSetLayout *layouts,
                                    const VkDescriptorSet *sets) {
    if (device == NULL || count > kFlimeShadowMaxDescriptors ||
        (count != 0u && (layouts == NULL || sets == NULL))) return false;
    const uint64_t pool_key = HandleBits(pool);
    std::unordered_map<uint64_t, PoolState>::iterator pool_it =
        device->pools.find(pool_key);
    if (pool_it == device->pools.end() ||
        HandleBits(pool_it->second.device) != HandleBits(owner)) return false;

    std::set<uint64_t> seen_sets;
    std::vector<std::pair<uint64_t, SetState> > staged;
    staged.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        const uint64_t set_key = HandleBits(sets[i]);
        std::unordered_map<uint64_t, LayoutState>::iterator layout_it =
            device->layouts.find(HandleBits(layouts[i]));
        if (set_key == 0u || layout_it == device->layouts.end() ||
            device->sets.find(set_key) != device->sets.end() ||
            !seen_sets.insert(set_key).second) return false;
        SetState state = {};
        state.device = owner;
        state.pool = pool;
        state.supported = layout_it->second.supported;
        if (!FlimeNextDescriptorGeneration(device, &state.generation)) {
            return false;
        }
        state.pool_generation = pool_it->second.generation;
        state.layout = layout_it->second;
        if (!FlimePopulateSetSlots(&state)) return false;
        staged.push_back(std::make_pair(set_key, state));
    }
    for (size_t i = 0; i < staged.size(); ++i) {
        device->sets[staged[i].first] = staged[i].second;
        pool_it->second.sets.insert(staged[i].first);
    }
    return true;
}

bool FlimeShadowFreeSets(DeviceState *device,
                                VkDescriptorPool pool,
                                uint32_t count,
                                const VkDescriptorSet *sets,
                                std::vector<uint64_t> *removed_sets) {
    if (device == NULL || count > kFlimeShadowMaxDescriptors ||
        (count != 0u && sets == NULL)) return false;
    const uint64_t pool_key = HandleBits(pool);
    std::unordered_map<uint64_t, PoolState>::iterator pool_it =
        device->pools.find(pool_key);
    if (pool_it == device->pools.end()) return false;
    std::set<uint64_t> unique;
    for (uint32_t i = 0; i < count; ++i) {
        const uint64_t set_key = HandleBits(sets[i]);
        std::unordered_map<uint64_t, SetState>::iterator set_it =
            device->sets.find(set_key);
        if (set_it == device->sets.end() ||
            HandleBits(set_it->second.pool) != pool_key ||
            !unique.insert(set_key).second) return false;
    }
    if (removed_sets != NULL) removed_sets->assign(unique.begin(), unique.end());
    for (std::set<uint64_t>::const_iterator it = unique.begin();
         it != unique.end(); ++it) {
        FlimeDiscardSetPending(device, *it);
        device->sets.erase(*it);
        pool_it->second.sets.erase(*it);
    }
    return true;
}

struct FlimeShadowAddress {
    uint64_t set;
    uint32_t binding;
    uint32_t element;
};

struct FlimeShadowKey {
    uint64_t set;
    uint32_t binding;
    uint32_t element;
    bool operator<(const FlimeShadowKey &other) const {
        if (set != other.set) return set < other.set;
        if (binding != other.binding) return binding < other.binding;
        return element < other.element;
    }
};

struct FlimeShadowMutation {
    FlimeShadowAddress address;
    DescriptorValue value;
    uint64_t update_id;
    uint64_t template_offset;
    uint16_t flags;
};

static FlimeShadowKey FlimeAddressKey(const FlimeShadowAddress &address) {
    FlimeShadowKey key = {};
    key.set = address.set;
    key.binding = address.binding;
    key.element = address.element;
    return key;
}

static bool FlimeResolveSpan(const DeviceState *device,
                             VkDescriptorSet set,
                             uint32_t binding,
                             uint32_t array_element,
                             uint32_t count,
                             VkDescriptorType type,
                             std::vector<FlimeShadowAddress> *out) {
    if (device == NULL || out == NULL || count == 0u ||
        count > kFlimeShadowMaxDescriptors) return false;
    const uint64_t set_key = HandleBits(set);
    std::unordered_map<uint64_t, SetState>::const_iterator set_it =
        device->sets.find(set_key);
    if (set_it == device->sets.end() || !set_it->second.supported ||
        !set_it->second.layout.supported) return false;
    const std::vector<LayoutBinding> &bindings = set_it->second.layout.bindings;
    size_t binding_index = 0u;
    while (binding_index < bindings.size() &&
           bindings[binding_index].binding != binding) ++binding_index;
    if (binding_index == bindings.size() ||
        bindings[binding_index].type != type ||
        array_element >= bindings[binding_index].count) return false;

    std::vector<FlimeShadowAddress> resolved;
    resolved.reserve(count);
    uint32_t remaining = count;
    uint32_t element = array_element;
    while (remaining != 0u) {
        const LayoutBinding &current = bindings[binding_index];
        const uint32_t available = current.count - element;
        const uint32_t take = remaining < available ? remaining : available;
        for (uint32_t i = 0; i < take; ++i) {
            FlimeShadowAddress address = {};
            address.set = set_key;
            address.binding = current.binding;
            address.element = element + i;
            if (set_it->second.slots.find(
                    std::make_pair(address.binding, address.element)) ==
                set_it->second.slots.end()) return false;
            resolved.push_back(address);
        }
        remaining -= take;
        if (remaining == 0u) break;
        ++binding_index;
        if (binding_index == bindings.size() ||
            !FlimeBindingsCanContinue(current, bindings[binding_index])) {
            return false;
        }
        element = 0u;
    }
    out->swap(resolved);
    return true;
}

static bool FlimeGetImmutableSampler(const DeviceState *device,
                                     const FlimeShadowAddress &address,
                                     bool *has_immutable,
                                     VkSampler *sampler) {
    if (device == NULL || has_immutable == NULL || sampler == NULL) return false;
    std::unordered_map<uint64_t, SetState>::const_iterator set_it =
        device->sets.find(address.set);
    if (set_it == device->sets.end()) return false;
    const LayoutBinding *binding =
        FlimeFindLayoutBinding(set_it->second.layout, address.binding);
    if (binding == NULL || address.element >= binding->count) return false;
    *has_immutable = !binding->immutable_samplers.empty();
    *sampler = VK_NULL_HANDLE;
    if (*has_immutable) {
        if (binding->immutable_samplers.size() != binding->count) return false;
        *sampler = binding->immutable_samplers[address.element];
    }
    return true;
}

static bool FlimeMaterializeWriteElement(
        const DeviceState *device,
        const FlimeShadowAddress &address,
        const VkWriteDescriptorSet &write,
        uint32_t source_index,
        DescriptorValue *out) {
    if (out == NULL) return false;
    DescriptorValue value = {};
    value.valid = true;
    value.type = write.descriptorType;
    if (FlimeDescriptorTypeIsBuffer(write.descriptorType)) {
        if (write.pBufferInfo == NULL) return false;
        const VkDescriptorBufferInfo &src = write.pBufferInfo[source_index];
        value.kind = kPayloadBuffer;
        value.buffer = src.buffer;
        value.offset = src.offset;
        value.range = src.range;
    } else if (FlimeDescriptorTypeIsImage(write.descriptorType)) {
        if (write.pImageInfo == NULL) return false;
        const VkDescriptorImageInfo &src = write.pImageInfo[source_index];
        bool has_immutable = false;
        VkSampler immutable_sampler = VK_NULL_HANDLE;
        if (!FlimeGetImmutableSampler(device, address, &has_immutable,
                                      &immutable_sampler)) return false;
        value.kind = kPayloadImage;
        if (write.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER ||
            write.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
            value.sampler = has_immutable ? immutable_sampler : src.sampler;
        }
        if (write.descriptorType != VK_DESCRIPTOR_TYPE_SAMPLER) {
            value.image_view = src.imageView;
            value.image_layout = src.imageLayout;
        } else {
            value.image_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        }
    } else if (FlimeDescriptorTypeIsTexel(write.descriptorType)) {
        if (write.pTexelBufferView == NULL) return false;
        value.kind = kPayloadTexel;
        value.buffer_view = write.pTexelBufferView[source_index];
    } else {
        return false;
    }
    *out = value;
    return true;
}

static bool FlimeApplyDestinationImmutableSampler(
        const DeviceState *device,
        const FlimeShadowAddress &address,
        DescriptorValue *value) {
    bool has_immutable = false;
    VkSampler immutable_sampler = VK_NULL_HANDLE;
    if (value == NULL || !FlimeGetImmutableSampler(
            device, address, &has_immutable, &immutable_sampler)) return false;
    if (has_immutable &&
        (value->type == VK_DESCRIPTOR_TYPE_SAMPLER ||
         value->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)) {
        value->sampler = immutable_sampler;
    }
    return true;
}

static bool FlimeLayoutAcceptsSpan(const LayoutState &layout,
                                   uint32_t binding,
                                   uint32_t array_element,
                                   uint32_t count,
                                   VkDescriptorType type) {
    if (!layout.supported || count == 0u ||
        count > kFlimeShadowMaxDescriptors) return false;
    size_t index = 0u;
    while (index < layout.bindings.size() &&
           layout.bindings[index].binding != binding) ++index;
    if (index == layout.bindings.size() ||
        layout.bindings[index].type != type ||
        array_element >= layout.bindings[index].count) return false;
    uint32_t remaining = count;
    uint32_t element = array_element;
    while (remaining != 0u) {
        const LayoutBinding &current = layout.bindings[index];
        const uint32_t available = current.count - element;
        const uint32_t take = remaining < available ? remaining : available;
        remaining -= take;
        if (remaining == 0u) return true;
        ++index;
        if (index == layout.bindings.size() ||
            !FlimeBindingsCanContinue(current, layout.bindings[index])) {
            return false;
        }
        element = 0u;
    }
    return true;
}

static size_t FlimeTemplateObjectSize(VkDescriptorType type) {
    if (FlimeDescriptorTypeIsBuffer(type)) {
        return sizeof(VkDescriptorBufferInfo);
    }
    if (FlimeDescriptorTypeIsImage(type)) {
        return sizeof(VkDescriptorImageInfo);
    }
    if (FlimeDescriptorTypeIsTexel(type)) {
        return sizeof(VkBufferView);
    }
    return 0u;
}

bool FlimeShadowCreateUpdateTemplate(
        DeviceState *device,
        VkDevice owner,
        VkDescriptorUpdateTemplate update_template,
        const VkDescriptorUpdateTemplateCreateInfo *info) {
    const uint64_t template_key = HandleBits(update_template);
    if (device == NULL || info == NULL || template_key == 0u ||
        device->update_templates.find(template_key) !=
            device->update_templates.end() ||
        info->sType != VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO ||
        (info->descriptorUpdateEntryCount != 0u &&
         info->pDescriptorUpdateEntries == NULL) ||
        info->descriptorUpdateEntryCount > kFlimeShadowMaxDescriptors) {
        return false;
    }
    UpdateTemplateState built = {};
    built.device = owner;
    built.layout = info->descriptorSetLayout;
    built.type = info->templateType;
    built.supported = info->pNext == NULL &&
        info->templateType == VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET;
    if (!FlimeNextDescriptorGeneration(device, &built.generation)) {
        return false;
    }
    std::unordered_map<uint64_t, LayoutState>::const_iterator layout_it =
        device->layouts.find(HandleBits(info->descriptorSetLayout));
    if (layout_it == device->layouts.end()) {
        built.supported = false;
        built.layout_generation = 0u;
    } else {
        built.layout_generation = layout_it->second.generation;
    }
    for (uint32_t i = 0; i < info->descriptorUpdateEntryCount; ++i) {
        const VkDescriptorUpdateTemplateEntry &src =
            info->pDescriptorUpdateEntries[i];
        TemplateEntry dst = {};
        dst.binding = src.dstBinding;
        dst.array_element = src.dstArrayElement;
        dst.count = src.descriptorCount;
        dst.type = src.descriptorType;
        dst.offset = src.offset;
        dst.stride = src.stride;
        const size_t max_size = static_cast<size_t>(-1);
        const size_t object_size = FlimeTemplateObjectSize(src.descriptorType);
        bool range_ok = object_size != 0u;
        size_t last_offset = src.offset;
        if (src.descriptorCount > 1u) {
            range_ok = range_ok &&
                src.stride <= (max_size - src.offset) /
                                  (src.descriptorCount - 1u);
            if (range_ok) {
                last_offset += src.stride * (src.descriptorCount - 1u);
            }
        }
        range_ok = range_ok && last_offset <= max_size - object_size;
        if (src.descriptorCount == 0u ||
            src.descriptorCount > kFlimeShadowMaxDescriptors ||
            !FlimeDescriptorTypeIsSupported(src.descriptorType) ||
            !range_ok ||
            (layout_it != device->layouts.end() &&
             !FlimeLayoutAcceptsSpan(layout_it->second, src.dstBinding,
                                     src.dstArrayElement, src.descriptorCount,
                                     src.descriptorType))) built.supported = false;
        built.entries.push_back(dst);
    }
    device->update_templates[template_key] = built;
    return true;
}

bool FlimeShadowDestroyUpdateTemplate(
        DeviceState *device, VkDescriptorUpdateTemplate update_template) {
    if (device == NULL) return false;
    const uint64_t key = HandleBits(update_template);
    std::unordered_map<uint64_t, UpdateTemplateState>::iterator found =
        device->update_templates.find(key);
    if (found == device->update_templates.end()) return false;
    device->update_templates.erase(found);
    return true;
}

static bool FlimeReadOverlayValue(
        const DeviceState *device,
        const std::map<FlimeShadowKey, DescriptorValue> &overlay,
        const FlimeShadowAddress &address,
        DescriptorValue *out) {
    if (device == NULL || out == NULL) return false;
    std::map<FlimeShadowKey, DescriptorValue>::const_iterator overlay_it =
        overlay.find(FlimeAddressKey(address));
    if (overlay_it != overlay.end()) {
        *out = overlay_it->second;
        return true;
    }
    std::unordered_map<uint64_t, SetState>::const_iterator set_it =
        device->sets.find(address.set);
    if (set_it == device->sets.end()) return false;
    std::map<std::pair<uint32_t, uint32_t>, DescriptorSlot>::const_iterator
        slot_it = set_it->second.slots.find(
            std::make_pair(address.binding, address.element));
    if (slot_it == set_it->second.slots.end()) return false;
    *out = slot_it->second.value;
    return true;
}

static void FlimeStageShadowMutation(
        const FlimeShadowAddress &address,
        const DescriptorValue &value,
        uint64_t update_id,
        uint64_t template_offset,
        uint16_t flags,
        std::map<FlimeShadowKey, DescriptorValue> *overlay,
        std::vector<FlimeShadowMutation> *mutations) {
    FlimeShadowMutation mutation = {};
    mutation.address = address;
    mutation.value = value;
    mutation.update_id = update_id;
    mutation.template_offset = template_offset;
    mutation.flags = flags;
    (*overlay)[FlimeAddressKey(address)] = value;
    mutations->push_back(mutation);
}

static bool FlimeCommitShadowMutations(
        DeviceState *device,
        const std::vector<FlimeShadowMutation> &mutations,
        uint64_t next_update_id,
        bool capture_records) {
    const uint64_t max_signed_index =
        (~static_cast<uint64_t>(0u)) >> 1u;
    if (device == NULL ||
        (capture_records &&
         (next_update_id == 0u || next_update_id == UINT64_MAX)) ||
        device->records.size() > max_signed_index ||
        mutations.size() > max_signed_index - device->records.size()) {
        return false;
    }
    for (size_t i = 0; i < mutations.size(); ++i) {
        std::unordered_map<uint64_t, SetState>::const_iterator set_it =
            device->sets.find(mutations[i].address.set);
        if (set_it == device->sets.end() ||
            set_it->second.slots.find(std::make_pair(
                mutations[i].address.binding, mutations[i].address.element)) ==
                set_it->second.slots.end()) return false;
    }
    if (!capture_records) {
        for (size_t i = 0; i < mutations.size(); ++i) {
            const FlimeShadowMutation &mutation = mutations[i];
            SetState &set = device->sets.find(mutation.address.set)->second;
            DescriptorSlot &slot = set.slots.find(std::make_pair(
                mutation.address.binding, mutation.address.element))->second;
            if (slot.pending_record >= 0 &&
                static_cast<size_t>(slot.pending_record) <
                    device->records.size()) {
                PendingRecord &old =
                    device->records[static_cast<size_t>(slot.pending_record)];
                if (!old.released) old.elided = true;
            }
            slot.value = mutation.value;
            slot.pending_record = -1;
        }
        return true;
    }
    device->records.reserve(device->records.size() + mutations.size());
    for (size_t i = 0; i < mutations.size(); ++i) {
        const FlimeShadowMutation &mutation = mutations[i];
        SetState &set = device->sets.find(mutation.address.set)->second;
        DescriptorSlot &slot = set.slots.find(std::make_pair(
            mutation.address.binding, mutation.address.element))->second;
        if (slot.pending_record >= 0 &&
            static_cast<size_t>(slot.pending_record) < device->records.size()) {
            PendingRecord &old =
                device->records[static_cast<size_t>(slot.pending_record)];
            if (!old.released) old.elided = true;
        }
        PendingRecord record = {};
        record.update_id = mutation.update_id;
        record.template_offset = mutation.template_offset;
        record.set = (VkDescriptorSet)(uintptr_t)mutation.address.set;
        record.binding = mutation.address.binding;
        record.array_element = mutation.address.element;
        record.type = mutation.value.type;
        record.flags = mutation.flags;
        record.value = mutation.value;
        device->records.push_back(record);
        slot.value = mutation.value;
        slot.pending_record = static_cast<int64_t>(device->records.size() - 1u);
    }
    device->next_update_id = next_update_id;
    return true;
}

static bool FlimePlanDescriptorWrites(
        const DeviceState *device,
        uint64_t template_offset,
        uint64_t *next_update_id,
        uint32_t write_count,
        const VkWriteDescriptorSet *writes,
        std::map<FlimeShadowKey, DescriptorValue> *overlay,
        std::vector<FlimeShadowMutation> *mutations) {
    if (device == NULL || next_update_id == NULL || overlay == NULL ||
        mutations == NULL || (write_count != 0u && writes == NULL) ||
        write_count > kFlimeShadowMaxDescriptors) {
        return false;
    }
    const uint64_t max_value = ~static_cast<uint64_t>(0u);
    for (uint32_t i = 0; i < write_count; ++i) {
        const VkWriteDescriptorSet &write = writes[i];
        if (write.sType != VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET ||
            write.pNext != NULL || write.descriptorCount == 0u ||
            write.descriptorCount > kFlimeShadowMaxDescriptors ||
            !FlimeDescriptorTypeIsSupported(write.descriptorType)) {
            return false;
        }
        std::vector<FlimeShadowAddress> addresses;
        if (!FlimeResolveSpan(device, write.dstSet, write.dstBinding,
                              write.dstArrayElement, write.descriptorCount,
                              write.descriptorType, &addresses) ||
            addresses.size() != write.descriptorCount) {
            return false;
        }
        for (uint32_t element = 0; element < write.descriptorCount; ++element) {
            DescriptorValue value = {};
            if (!FlimeMaterializeWriteElement(device, addresses[element],
                                              write, element, &value) ||
                *next_update_id == 0u || *next_update_id == max_value) {
                return false;
            }
            const uint64_t ordinal =
                static_cast<uint64_t>(mutations->size());
            if (ordinal > max_value - template_offset) {
                return false;
            }
            const uint64_t record_id = (*next_update_id)++;
            FlimeStageShadowMutation(addresses[element], value, record_id,
                                     template_offset + ordinal, 0u,
                                     overlay, mutations);
        }
    }
    return true;
}

static bool FlimePlanDescriptorCopies(
        const DeviceState *device,
        uint64_t template_offset,
        uint64_t *next_update_id,
        uint32_t copy_count,
        const VkCopyDescriptorSet *copies,
        std::map<FlimeShadowKey, DescriptorValue> *overlay,
        std::vector<FlimeShadowMutation> *mutations) {
    if (device == NULL || next_update_id == NULL || overlay == NULL ||
        mutations == NULL || (copy_count != 0u && copies == NULL) ||
        copy_count > kFlimeShadowMaxDescriptors) {
        return false;
    }
    const uint64_t max_value = ~static_cast<uint64_t>(0u);
    for (uint32_t i = 0; i < copy_count; ++i) {
        const VkCopyDescriptorSet &copy = copies[i];
        if (copy.sType != VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET ||
            copy.pNext != NULL || copy.descriptorCount == 0u ||
            copy.descriptorCount > kFlimeShadowMaxDescriptors) {
            return false;
        }
        std::unordered_map<uint64_t, SetState>::const_iterator source_set =
            device->sets.find(HandleBits(copy.srcSet));
        if (source_set == device->sets.end() || !source_set->second.supported) {
            return false;
        }
        const LayoutBinding *source_binding = FlimeFindLayoutBinding(
            source_set->second.layout, copy.srcBinding);
        if (source_binding == NULL ||
            !FlimeDescriptorTypeIsSupported(source_binding->type)) {
            return false;
        }
        std::vector<FlimeShadowAddress> sources;
        std::vector<FlimeShadowAddress> destinations;
        if (!FlimeResolveSpan(device, copy.srcSet, copy.srcBinding,
                              copy.srcArrayElement, copy.descriptorCount,
                              source_binding->type, &sources) ||
            !FlimeResolveSpan(device, copy.dstSet, copy.dstBinding,
                              copy.dstArrayElement, copy.descriptorCount,
                              source_binding->type, &destinations) ||
            sources.size() != copy.descriptorCount ||
            destinations.size() != copy.descriptorCount) {
            return false;
        }
        std::vector<DescriptorValue> snapshot;
        snapshot.reserve(copy.descriptorCount);
        for (uint32_t element = 0; element < copy.descriptorCount; ++element) {
            DescriptorValue value = {};
            if (!FlimeReadOverlayValue(device, *overlay, sources[element],
                                       &value) || !value.valid ||
                value.type != source_binding->type) {
                return false;
            }
            snapshot.push_back(value);
        }
        for (uint32_t element = 0; element < copy.descriptorCount; ++element) {
            DescriptorValue value = snapshot[element];
            if (!FlimeApplyDestinationImmutableSampler(
                    device, destinations[element], &value) ||
                *next_update_id == 0u || *next_update_id == max_value) {
                return false;
            }
            const uint64_t ordinal =
                static_cast<uint64_t>(mutations->size());
            if (ordinal > max_value - template_offset) {
                return false;
            }
            const uint64_t record_id = (*next_update_id)++;
            FlimeStageShadowMutation(destinations[element], value, record_id,
                                     template_offset + ordinal,
                                     kRouteDerived, overlay, mutations);
        }
    }
    return true;
}

bool FlimeCountDescriptorRecords(
        uint32_t write_count, const VkWriteDescriptorSet *writes,
        uint32_t copy_count, const VkCopyDescriptorSet *copies,
        uint64_t *out) {
    if (out == NULL || (write_count != 0u && writes == NULL) ||
        (copy_count != 0u && copies == NULL) ||
        write_count > kFlimeShadowMaxDescriptors ||
        copy_count > kFlimeShadowMaxDescriptors) {
        return false;
    }
    uint64_t total = 0u;
    for (uint32_t i = 0; i < write_count; ++i) {
        total += writes[i].descriptorCount;
        if (total > kFlimeShadowMaxDescriptors) return false;
    }
    for (uint32_t i = 0; i < copy_count; ++i) {
        total += copies[i].descriptorCount;
        if (total > kFlimeShadowMaxDescriptors) return false;
    }
    *out = total;
    return true;
}

bool FlimeShadowUpdateDescriptorSets(
        DeviceState *device, uint64_t template_offset,
        uint32_t write_count, const VkWriteDescriptorSet *writes,
        uint32_t copy_count, const VkCopyDescriptorSet *copies,
        bool capture_records) {
    if (device == NULL) return false;
    uint64_t expected_records = 0u;
    if (!FlimeCountDescriptorRecords(write_count, writes, copy_count, copies,
                                     &expected_records)) {
        return false;
    }
    uint64_t next_update_id = device->next_update_id;
    if (next_update_id == 0u) next_update_id = 1u;
    std::map<FlimeShadowKey, DescriptorValue> overlay;
    std::vector<FlimeShadowMutation> mutations;
    if (!FlimePlanDescriptorWrites(device, template_offset, &next_update_id,
                                   write_count, writes, &overlay, &mutations) ||
        !FlimePlanDescriptorCopies(device, template_offset, &next_update_id,
                                   copy_count, copies, &overlay, &mutations) ||
        mutations.size() != static_cast<size_t>(expected_records)) {
        return false;
    }
    return FlimeCommitShadowMutations(device, mutations, next_update_id,
                                      capture_records);
}

static bool FlimeLayoutsEquivalent(const LayoutState &left,
                                   const LayoutState &right) {
    if (!left.supported || !right.supported ||
        left.bindings.size() != right.bindings.size()) {
        return false;
    }
    for (size_t i = 0; i < left.bindings.size(); ++i) {
        const LayoutBinding &a = left.bindings[i];
        const LayoutBinding &b = right.bindings[i];
        if (a.binding != b.binding || a.type != b.type ||
            a.count != b.count || a.stages != b.stages ||
            !FlimeSameImmutableSamplers(a, b)) {
            return false;
        }
    }
    return true;
}

template <typename T>
static T FlimeLoadTemplateObject(const uint8_t *base, size_t offset) {
    T object;
    memcpy(&object, base + offset, sizeof(object));
    return object;
}

static bool FlimeMaterializeTemplateElement(
        const DeviceState *device,
        const FlimeShadowAddress &address,
        VkDescriptorType type,
        const uint8_t *data,
        size_t offset,
        DescriptorValue *out) {
    if (data == NULL || out == NULL) return false;
    VkWriteDescriptorSet write = {};
    write.descriptorType = type;
    if (FlimeDescriptorTypeIsBuffer(type)) {
        const VkDescriptorBufferInfo info =
            FlimeLoadTemplateObject<VkDescriptorBufferInfo>(data, offset);
        write.pBufferInfo = &info;
        return FlimeMaterializeWriteElement(device, address, write, 0u, out);
    }
    if (FlimeDescriptorTypeIsImage(type)) {
        const VkDescriptorImageInfo info =
            FlimeLoadTemplateObject<VkDescriptorImageInfo>(data, offset);
        write.pImageInfo = &info;
        return FlimeMaterializeWriteElement(device, address, write, 0u, out);
    }
    if (FlimeDescriptorTypeIsTexel(type)) {
        const VkBufferView view =
            FlimeLoadTemplateObject<VkBufferView>(data, offset);
        write.pTexelBufferView = &view;
        return FlimeMaterializeWriteElement(device, address, write, 0u, out);
    }
    return false;
}

bool FlimeCountTemplateRecords(
        const DeviceState *device,
        VkDescriptorUpdateTemplate update_template,
        uint64_t *out) {
    if (device == NULL || out == NULL) return false;
    std::unordered_map<uint64_t, UpdateTemplateState>::const_iterator found =
        device->update_templates.find(HandleBits(update_template));
    if (found == device->update_templates.end() ||
        !found->second.supported) {
        return false;
    }
    uint64_t total = 0u;
    for (size_t i = 0; i < found->second.entries.size(); ++i) {
        total += found->second.entries[i].count;
        if (total > kFlimeShadowMaxDescriptors) return false;
    }
    *out = total;
    return true;
}

bool FlimeShadowUpdateWithTemplate(
        DeviceState *device, uint64_t template_offset,
        VkDescriptorSet set, VkDescriptorUpdateTemplate update_template,
        const void *data, bool capture_records) {
    if (device == NULL) return false;
    std::unordered_map<uint64_t, UpdateTemplateState>::iterator template_it =
        device->update_templates.find(HandleBits(update_template));
    std::unordered_map<uint64_t, SetState>::iterator set_it =
        device->sets.find(HandleBits(set));
    if (template_it == device->update_templates.end() ||
        set_it == device->sets.end() || !template_it->second.supported ||
        !set_it->second.supported ||
        HandleBits(template_it->second.device) !=
            HandleBits(set_it->second.device) ||
        template_it->second.type !=
            VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET ||
        template_it->second.entries.size() > kFlimeShadowMaxDescriptors ||
        (!template_it->second.entries.empty() && data == NULL)) {
        return false;
    }
    std::unordered_map<uint64_t, LayoutState>::const_iterator layout_it =
        device->layouts.find(HandleBits(template_it->second.layout));
    if (layout_it == device->layouts.end() ||
        !FlimeLayoutsEquivalent(set_it->second.layout, layout_it->second)) {
        return false;
    }
    uint64_t expected_records = 0u;
    for (size_t i = 0; i < template_it->second.entries.size(); ++i) {
        expected_records += template_it->second.entries[i].count;
        if (expected_records > kFlimeShadowMaxDescriptors) return false;
    }
    uint64_t next_update_id = device->next_update_id;
    if (next_update_id == 0u) next_update_id = 1u;
    const uint64_t max_value = ~static_cast<uint64_t>(0u);
    const uint8_t *bytes = static_cast<const uint8_t *>(data);
    std::map<FlimeShadowKey, DescriptorValue> overlay;
    std::vector<FlimeShadowMutation> mutations;
    for (size_t i = 0; i < template_it->second.entries.size(); ++i) {
        const TemplateEntry &entry = template_it->second.entries[i];
        std::vector<FlimeShadowAddress> addresses;
        if (!FlimeResolveSpan(device, set, entry.binding,
                              entry.array_element, entry.count,
                              entry.type, &addresses) ||
            addresses.size() != entry.count) {
            return false;
        }
        for (uint32_t element = 0; element < entry.count; ++element) {
            const size_t max_size = static_cast<size_t>(-1);
            if (element != 0u &&
                entry.stride > (max_size - entry.offset) / element) {
                return false;
            }
            DescriptorValue value = {};
            const size_t source_offset = entry.offset + entry.stride * element;
            if (!FlimeMaterializeTemplateElement(device, addresses[element],
                                                 entry.type, bytes,
                                                 source_offset, &value) ||
                next_update_id == 0u || next_update_id == max_value) {
                return false;
            }
            const uint64_t ordinal =
                static_cast<uint64_t>(mutations.size());
            if (ordinal > max_value - template_offset) return false;
            const uint64_t record_id = next_update_id++;
            FlimeStageShadowMutation(addresses[element], value, record_id,
                                     template_offset + ordinal, 0u,
                                     &overlay, &mutations);
        }
    }
    if (mutations.size() != static_cast<size_t>(expected_records)) {
        return false;
    }
    return FlimeCommitShadowMutations(device, mutations, next_update_id,
                                      capture_records);
}

/*
 * Compiled calls own projection metadata only.  Application pointers and
 * descriptor payloads are always read again when a Fast occurrence executes.
 */
static bool FlimeFastLayoutBindingsTopologyEqual(
        const std::vector<LayoutBinding> &actual,
        const std::vector<LayoutBinding> &expected) {
    if (actual.size() != expected.size()) {
        return false;
    }
    for (size_t i = 0; i < expected.size(); ++i) {
        const LayoutBinding &binding = actual[i];
        const LayoutBinding &shape = expected[i];
        if (binding.binding != shape.binding ||
            binding.type != shape.type || binding.count != shape.count ||
            binding.stages != shape.stages ||
            !FlimeSameImmutableSamplers(binding, shape)) {
            return false;
        }
    }
    return true;
}

static bool FlimeFastLayoutTopologyMatches(
        const LayoutState &layout,
        const std::vector<LayoutBinding> &expected) {
    return layout.supported &&
           FlimeFastLayoutBindingsTopologyEqual(
               layout.bindings, expected);
}

static bool FlimeFastCaptureSetGuard(
        const DeviceState *device, uint64_t set_key,
        FlimeFastDescriptorCall *plan, uint32_t *guard_index) {
    if (device == NULL || plan == NULL || guard_index == NULL ||
        set_key == 0u) {
        return false;
    }
    std::unordered_map<uint64_t, SetState>::const_iterator set_it =
        device->sets.find(set_key);
    if (set_it == device->sets.end() || !set_it->second.supported ||
        !set_it->second.layout.supported ||
        set_it->second.generation == 0u ||
        set_it->second.layout.generation == 0u) {
        return false;
    }
    const uint64_t pool_key = HandleBits(set_it->second.pool);
    std::unordered_map<uint64_t, PoolState>::const_iterator pool_it =
        device->pools.find(pool_key);
    if (pool_it == device->pools.end() ||
        pool_it->second.generation == 0u ||
        set_it->second.pool_generation != pool_it->second.generation ||
        pool_it->second.sets.find(set_key) == pool_it->second.sets.end()) {
        return false;
    }
    for (size_t i = 0; i < plan->set_guards.size(); ++i) {
        if (plan->set_guards[i].set != set_key) continue;
        const FlimeFastSetGuard &known = plan->set_guards[i];
        if (known.set_generation != set_it->second.generation ||
            known.layout_generation != set_it->second.layout.generation ||
            known.pool != pool_key ||
            known.pool_generation != pool_it->second.generation ||
            i > UINT32_MAX) {
            return false;
        }
        *guard_index = static_cast<uint32_t>(i);
        return true;
    }
    if (plan->set_guards.size() >= UINT32_MAX) return false;
    FlimeFastSetGuard guard = {};
    guard.occurrence_role = 0u;
    guard.set = set_key;
    guard.set_generation = set_it->second.generation;
    guard.layout_generation = set_it->second.layout.generation;
    guard.pool = pool_key;
    guard.pool_generation = pool_it->second.generation;
    guard.layout_bindings = set_it->second.layout.bindings;
    plan->set_guards.push_back(guard);
    *guard_index =
        static_cast<uint32_t>(plan->set_guards.size() - 1u);
    return true;
}

static bool FlimeFastAssignOccurrenceSetRoles(
        std::vector<FlimeFastDescriptorCall> *plans) {
    if (plans == NULL) return false;
    std::map<uint64_t, uint32_t> roles;
    uint32_t next_role = 1u;
    for (size_t call = 0; call < plans->size(); ++call) {
        FlimeFastDescriptorCall &plan = (*plans)[call];
        for (size_t i = 0; i < plan.set_guards.size(); ++i) {
            FlimeFastSetGuard &guard = plan.set_guards[i];
            if (guard.set == 0u) return false;
            std::map<uint64_t, uint32_t>::const_iterator known =
                roles.find(guard.set);
            if (known != roles.end()) {
                guard.occurrence_role = known->second;
                continue;
            }
            if (next_role == 0u || next_role == UINT32_MAX) return false;
            guard.occurrence_role = next_role;
            roles[guard.set] = next_role++;
        }
    }
    return true;
}

static bool FlimeFastCaptureAddress(
        const DeviceState *device, const FlimeShadowAddress &address,
        FlimeFastDescriptorCall *plan, FlimeFastAddress *compiled) {
    if (device == NULL || plan == NULL || compiled == NULL) return false;
    std::unordered_map<uint64_t, SetState>::const_iterator set_it =
        device->sets.find(address.set);
    if (set_it == device->sets.end() ||
        set_it->second.slots.find(std::make_pair(
            address.binding, address.element)) ==
            set_it->second.slots.end() ||
        !FlimeFastCaptureSetGuard(
            device, address.set, plan, &compiled->set_guard)) {
        return false;
    }
    compiled->binding = address.binding;
    compiled->element = address.element;
    return true;
}

bool FlimeCompileDescriptorSetFastPlan(
        const DeviceState *device, int fun_id, size_t semantic_call_index,
        uint64_t template_offset, uint64_t encoded_bytes,
        uint32_t write_count, const VkWriteDescriptorSet *writes,
        uint32_t copy_count, const VkCopyDescriptorSet *copies,
        FlimeFastDescriptorCall *out) {
    if (device == NULL || out == NULL ||
        (write_count != 0u && writes == NULL) ||
        (copy_count != 0u && copies == NULL) ||
        write_count > kFlimeShadowMaxDescriptors ||
        copy_count > kFlimeShadowMaxDescriptors) {
        return false;
    }
    uint64_t expected_records = 0u;
    if (!FlimeCountDescriptorRecords(
            write_count, writes, copy_count, copies, &expected_records) ||
        expected_records > kFlimeShadowMaxDescriptors) {
        return false;
    }
    FlimeFastDescriptorCall plan;
    plan.kind = kFlimeFastUpdateSets;
    plan.fun_id = fun_id;
    plan.semantic_call_index = semantic_call_index;
    plan.template_offset = template_offset;
    plan.encoded_bytes = encoded_bytes;
    plan.write_count = write_count;
    plan.copy_count = copy_count;
    plan.operations.reserve(static_cast<size_t>(expected_records));
    std::map<FlimeShadowKey, uint32_t> last_writer;

    for (uint32_t i = 0; i < write_count; ++i) {
        const VkWriteDescriptorSet &write = writes[i];
        if (write.sType != VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET ||
            write.pNext != NULL || write.descriptorCount == 0u ||
            write.descriptorCount > kFlimeShadowMaxDescriptors ||
            !FlimeDescriptorTypeIsSupported(write.descriptorType)) {
            return false;
        }
        FlimeFastWriteShape shape = {};
        if (!FlimeFastCaptureSetGuard(
                device, HandleBits(write.dstSet), &plan,
                &shape.set_guard)) {
            return false;
        }
        shape.binding = write.dstBinding;
        shape.array_element = write.dstArrayElement;
        shape.count = write.descriptorCount;
        shape.type = write.descriptorType;
        plan.writes.push_back(shape);
        std::vector<FlimeShadowAddress> addresses;
        if (!FlimeResolveSpan(
                device, write.dstSet, write.dstBinding,
                write.dstArrayElement, write.descriptorCount,
                write.descriptorType, &addresses) ||
            addresses.size() != write.descriptorCount) {
            return false;
        }
        for (uint32_t element = 0; element < write.descriptorCount; ++element) {
            if (plan.operations.size() >= UINT32_MAX) return false;
            FlimeFastRouteOp operation = {};
            if (!FlimeFastCaptureAddress(
                    device, addresses[element], &plan,
                    &operation.destination) ||
                !FlimeGetImmutableSampler(
                    device, addresses[element],
                    &operation.has_immutable_sampler,
                    &operation.immutable_sampler)) {
                return false;
            }
            operation.type = write.descriptorType;
            operation.route_flags = 0u;
            operation.source = kFlimeFastRawWrite;
            operation.source_outer = i;
            operation.source_inner = element;
            plan.operations.push_back(operation);
            last_writer[FlimeAddressKey(addresses[element])] =
                static_cast<uint32_t>(plan.operations.size() - 1u);
        }
    }

    for (uint32_t i = 0; i < copy_count; ++i) {
        const VkCopyDescriptorSet &copy = copies[i];
        if (copy.sType != VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET ||
            copy.pNext != NULL || copy.descriptorCount == 0u ||
            copy.descriptorCount > kFlimeShadowMaxDescriptors) {
            return false;
        }
        std::unordered_map<uint64_t, SetState>::const_iterator source_set =
            device->sets.find(HandleBits(copy.srcSet));
        if (source_set == device->sets.end() ||
            !source_set->second.supported) {
            return false;
        }
        const LayoutBinding *source_binding = FlimeFindLayoutBinding(
            source_set->second.layout, copy.srcBinding);
        if (source_binding == NULL ||
            !FlimeDescriptorTypeIsSupported(source_binding->type)) {
            return false;
        }
        FlimeFastCopyShape shape = {};
        if (!FlimeFastCaptureSetGuard(
                device, HandleBits(copy.srcSet), &plan,
                &shape.source_guard) ||
            !FlimeFastCaptureSetGuard(
                device, HandleBits(copy.dstSet), &plan,
                &shape.destination_guard)) {
            return false;
        }
        shape.source_binding = copy.srcBinding;
        shape.source_array_element = copy.srcArrayElement;
        shape.destination_binding = copy.dstBinding;
        shape.destination_array_element = copy.dstArrayElement;
        shape.count = copy.descriptorCount;
        plan.copies.push_back(shape);
        std::vector<FlimeShadowAddress> sources;
        std::vector<FlimeShadowAddress> destinations;
        if (!FlimeResolveSpan(
                device, copy.srcSet, copy.srcBinding,
                copy.srcArrayElement, copy.descriptorCount,
                source_binding->type, &sources) ||
            !FlimeResolveSpan(
                device, copy.dstSet, copy.dstBinding,
                copy.dstArrayElement, copy.descriptorCount,
                source_binding->type, &destinations) ||
            sources.size() != copy.descriptorCount ||
            destinations.size() != copy.descriptorCount) {
            return false;
        }
        /*
         * Freeze every source selector before publishing this copy's
         * destinations.  This preserves Vulkan's overlapping-copy snapshot.
         */
        std::vector<FlimeFastRouteOp> copy_operations;
        copy_operations.reserve(copy.descriptorCount);
        for (uint32_t element = 0; element < copy.descriptorCount; ++element) {
            FlimeFastRouteOp operation = {};
            if (!FlimeFastCaptureAddress(
                    device, destinations[element], &plan,
                    &operation.destination) ||
                !FlimeGetImmutableSampler(
                    device, destinations[element],
                    &operation.has_immutable_sampler,
                    &operation.immutable_sampler)) {
                return false;
            }
            operation.type = source_binding->type;
            operation.route_flags = kRouteDerived;
            std::map<FlimeShadowKey, uint32_t>::const_iterator prior =
                last_writer.find(FlimeAddressKey(sources[element]));
            if (prior != last_writer.end()) {
                operation.source = kFlimeFastPriorMutation;
                operation.source_outer = prior->second;
            } else {
                operation.source = kFlimeFastShadowSlot;
                if (!FlimeFastCaptureAddress(
                        device, sources[element], &plan,
                        &operation.source_slot)) {
                    return false;
                }
            }
            copy_operations.push_back(operation);
        }
        if (copy_operations.size() >
            static_cast<size_t>(UINT32_MAX) - plan.operations.size()) {
            return false;
        }
        const uint32_t first_operation =
            static_cast<uint32_t>(plan.operations.size());
        plan.operations.insert(
            plan.operations.end(),
            copy_operations.begin(), copy_operations.end());
        for (uint32_t element = 0; element < copy.descriptorCount; ++element) {
            last_writer[FlimeAddressKey(destinations[element])] =
                first_operation + element;
        }
    }
    if (plan.operations.size() != static_cast<size_t>(expected_records)) {
        return false;
    }
    *out = plan;
    return true;
}

bool FlimeCompileDescriptorTemplateFastPlan(
        const DeviceState *device, int fun_id, size_t semantic_call_index,
        uint64_t template_offset, uint64_t encoded_bytes,
        VkDescriptorSet set, VkDescriptorUpdateTemplate update_template,
        const void *data, FlimeFastDescriptorCall *out) {
    if (device == NULL || out == NULL) return false;
    std::unordered_map<uint64_t, UpdateTemplateState>::const_iterator
        template_it = device->update_templates.find(
            HandleBits(update_template));
    std::unordered_map<uint64_t, SetState>::const_iterator set_it =
        device->sets.find(HandleBits(set));
    if (template_it == device->update_templates.end() ||
        set_it == device->sets.end() || !template_it->second.supported ||
        !set_it->second.supported ||
        HandleBits(template_it->second.device) !=
            HandleBits(set_it->second.device) ||
        template_it->second.type !=
            VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET ||
        template_it->second.entries.size() > kFlimeShadowMaxDescriptors ||
        (!template_it->second.entries.empty() && data == NULL)) {
        return false;
    }
    std::unordered_map<uint64_t, LayoutState>::const_iterator layout_it =
        device->layouts.find(HandleBits(template_it->second.layout));
    if (layout_it == device->layouts.end() ||
        template_it->second.layout_generation !=
            layout_it->second.generation ||
        !FlimeLayoutsEquivalent(set_it->second.layout, layout_it->second)) {
        return false;
    }
    FlimeFastDescriptorCall plan;
    plan.kind = kFlimeFastUpdateTemplate;
    plan.fun_id = fun_id;
    plan.semantic_call_index = semantic_call_index;
    plan.template_offset = template_offset;
    plan.encoded_bytes = encoded_bytes;
    plan.update_template = HandleBits(update_template);
    plan.update_template_generation = template_it->second.generation;
    plan.template_layout_generation =
        template_it->second.layout_generation;
    if (!FlimeFastCaptureSetGuard(
            device, HandleBits(set), &plan,
            &plan.template_set_guard)) {
        return false;
    }
    uint64_t expected_records = 0u;
    for (size_t i = 0; i < template_it->second.entries.size(); ++i) {
        expected_records += template_it->second.entries[i].count;
        if (expected_records > kFlimeShadowMaxDescriptors) return false;
    }
    plan.operations.reserve(static_cast<size_t>(expected_records));
    for (size_t i = 0; i < template_it->second.entries.size(); ++i) {
        const TemplateEntry &entry = template_it->second.entries[i];
        std::vector<FlimeShadowAddress> addresses;
        if (!FlimeResolveSpan(
                device, set, entry.binding, entry.array_element,
                entry.count, entry.type, &addresses) ||
            addresses.size() != entry.count) {
            return false;
        }
        for (uint32_t element = 0; element < entry.count; ++element) {
            const size_t max_size = static_cast<size_t>(-1);
            if (element != 0u &&
                entry.stride > (max_size - entry.offset) / element) {
                return false;
            }
            FlimeFastRouteOp operation = {};
            if (!FlimeFastCaptureAddress(
                    device, addresses[element], &plan,
                    &operation.destination) ||
                !FlimeGetImmutableSampler(
                    device, addresses[element],
                    &operation.has_immutable_sampler,
                    &operation.immutable_sampler)) {
                return false;
            }
            operation.type = entry.type;
            operation.route_flags = 0u;
            operation.source = kFlimeFastTemplateData;
            operation.template_data_offset =
                entry.offset + entry.stride * element;
            plan.operations.push_back(operation);
        }
    }
    if (plan.operations.size() != static_cast<size_t>(expected_records)) {
        return false;
    }
    *out = plan;
    return true;
}

static bool FlimeFastAddressTopologyEqual(
        const FlimeFastAddress &left,
        const FlimeFastAddress &right) {
    return left.set_guard == right.set_guard &&
           left.binding == right.binding &&
           left.element == right.element;
}

/*
 * Match validates the projection graph, but arms the cache with Match's fresh
 * concrete guards.  Exact object identities are deliberately ignored here;
 * global SemanticCall role matching already validates handle aliasing.
 */
bool FlimeFastPlansTopologyEquivalent(
        const FlimeFastDescriptorCall &left,
        const FlimeFastDescriptorCall &right) {
    if (left.kind != right.kind || left.fun_id != right.fun_id ||
        left.semantic_call_index != right.semantic_call_index ||
        left.template_offset != right.template_offset ||
        left.encoded_bytes != right.encoded_bytes ||
        left.write_count != right.write_count ||
        left.copy_count != right.copy_count ||
        left.template_set_guard != right.template_set_guard ||
        left.set_guards.size() != right.set_guards.size() ||
        left.writes.size() != right.writes.size() ||
        left.copies.size() != right.copies.size() ||
        left.operations.size() != right.operations.size()) {
        return false;
    }
    for (size_t i = 0; i < left.set_guards.size(); ++i) {
        /*
         * Concrete set/pool identities may be renamed between occurrences,
         * but Match must still prove the complete allocation-time layout
         * shape, including immutable sampler identity.  Fast then binds that
         * shape to a live occurrence-local role.
         */
        if (!FlimeFastLayoutBindingsTopologyEqual(
                left.set_guards[i].layout_bindings,
                right.set_guards[i].layout_bindings)) {
            return false;
        }
    }
    for (size_t i = 0; i < left.writes.size(); ++i) {
        const FlimeFastWriteShape &a = left.writes[i];
        const FlimeFastWriteShape &b = right.writes[i];
        if (a.set_guard != b.set_guard || a.binding != b.binding ||
            a.array_element != b.array_element || a.count != b.count ||
            a.type != b.type) {
            return false;
        }
    }
    for (size_t i = 0; i < left.copies.size(); ++i) {
        const FlimeFastCopyShape &a = left.copies[i];
        const FlimeFastCopyShape &b = right.copies[i];
        if (a.source_guard != b.source_guard ||
            a.destination_guard != b.destination_guard ||
            a.source_binding != b.source_binding ||
            a.source_array_element != b.source_array_element ||
            a.destination_binding != b.destination_binding ||
            a.destination_array_element != b.destination_array_element ||
            a.count != b.count) {
            return false;
        }
    }
    for (size_t i = 0; i < left.operations.size(); ++i) {
        const FlimeFastRouteOp &a = left.operations[i];
        const FlimeFastRouteOp &b = right.operations[i];
        if (!FlimeFastAddressTopologyEqual(
                a.destination, b.destination) ||
            a.type != b.type || a.route_flags != b.route_flags ||
            a.source != b.source || a.source_outer != b.source_outer ||
            a.source_inner != b.source_inner ||
            !FlimeFastAddressTopologyEqual(
                a.source_slot, b.source_slot) ||
            a.template_data_offset != b.template_data_offset ||
            a.has_immutable_sampler != b.has_immutable_sampler) {
            return false;
        }
    }
    return true;
}

static bool FlimeFastBoundSetsEqual(const FlimeFastBoundSet &left,
                                    const FlimeFastBoundSet &right) {
    return left.set == right.set &&
           left.set_generation == right.set_generation &&
           left.layout_generation == right.layout_generation &&
           left.pool == right.pool &&
           left.pool_generation == right.pool_generation;
}

static bool FlimeFastInspectSet(
        const DeviceState *device, uint64_t set_key,
        const FlimeFastSetGuard &guard, FlimeFastBoundSet *bound) {
    if (device == NULL || bound == NULL || set_key == 0u ||
        guard.set == 0u || guard.set_generation == 0u ||
        guard.layout_generation == 0u || guard.pool == 0u ||
        guard.pool_generation == 0u) {
        return false;
    }
    std::unordered_map<uint64_t, SetState>::const_iterator set_it =
        device->sets.find(set_key);
    if (set_it == device->sets.end() || !set_it->second.supported ||
        !FlimeFastLayoutTopologyMatches(
            set_it->second.layout, guard.layout_bindings)) {
        return false;
    }
    const uint64_t pool_key = HandleBits(set_it->second.pool);
    std::unordered_map<uint64_t, PoolState>::const_iterator pool_it =
        device->pools.find(pool_key);
    if (set_it->second.generation == 0u ||
        set_it->second.layout.generation == 0u ||
        set_it->second.pool_generation == 0u ||
        pool_it == device->pools.end() ||
        pool_it->second.generation != set_it->second.pool_generation ||
        pool_it->second.sets.find(set_key) == pool_it->second.sets.end() ||
        HandleBits(set_it->second.device) != HandleBits(device->device) ||
        HandleBits(pool_it->second.device) != HandleBits(device->device)) {
        return false;
    }
    /*
     * Equal handle bits mean this is the lifetime captured by Match, not a
     * rename.  A recycled numeric handle must miss even when its layout shape
     * happens to be identical.
     */
    if (set_key == guard.set &&
        (set_it->second.generation != guard.set_generation ||
         set_it->second.layout.generation != guard.layout_generation ||
         pool_key != guard.pool ||
         pool_it->second.generation != guard.pool_generation)) {
        return false;
    }
    bound->set = set_key;
    bound->set_generation = set_it->second.generation;
    bound->layout_generation = set_it->second.layout.generation;
    bound->pool = pool_key;
    bound->pool_generation = pool_it->second.generation;
    return true;
}

static bool FlimeFastBindSetRole(
        const DeviceState *device, const FlimeFastDescriptorCall &plan,
        uint32_t guard_index, uint64_t set_key,
        std::map<uint32_t, FlimeFastBoundSet> *role_bindings,
        std::map<uint64_t, uint32_t> *handle_roles,
        std::vector<FlimeFastBoundSet> *bound_sets) {
    if (device == NULL || role_bindings == NULL || handle_roles == NULL ||
        bound_sets == NULL || guard_index >= plan.set_guards.size() ||
        bound_sets->size() != plan.set_guards.size()) {
        return false;
    }
    const FlimeFastSetGuard &guard = plan.set_guards[guard_index];
    if (guard.occurrence_role == 0u) return false;
    FlimeFastBoundSet current = {};
    if (!FlimeFastInspectSet(device, set_key, guard, &current)) return false;

    std::map<uint32_t, FlimeFastBoundSet>::const_iterator role =
        role_bindings->find(guard.occurrence_role);
    if (role != role_bindings->end() &&
        !FlimeFastBoundSetsEqual(role->second, current)) {
        return false;
    }
    std::map<uint64_t, uint32_t>::const_iterator reverse =
        handle_roles->find(set_key);
    if (reverse != handle_roles->end() &&
        reverse->second != guard.occurrence_role) {
        return false;
    }
    FlimeFastBoundSet &local = (*bound_sets)[guard_index];
    if (local.set != 0u && !FlimeFastBoundSetsEqual(local, current)) {
        return false;
    }
    if (role == role_bindings->end()) {
        (*role_bindings)[guard.occurrence_role] = current;
    }
    if (reverse == handle_roles->end()) {
        (*handle_roles)[set_key] = guard.occurrence_role;
    }
    local = current;
    return true;
}

static bool FlimeFastResolveAddress(
        const DeviceState *device,
        const FlimeFastDescriptorCall &plan,
        const std::vector<FlimeFastBoundSet> &bound_sets,
        const FlimeFastAddress &compiled,
        FlimeShadowAddress *address) {
    if (device == NULL || address == NULL ||
        bound_sets.size() != plan.set_guards.size() ||
        compiled.set_guard >= bound_sets.size() ||
        bound_sets[compiled.set_guard].set == 0u) {
        return false;
    }
    const uint64_t set_key = bound_sets[compiled.set_guard].set;
    std::unordered_map<uint64_t, SetState>::const_iterator set_it =
        device->sets.find(set_key);
    if (set_it == device->sets.end() ||
        set_it->second.slots.find(std::make_pair(
            compiled.binding, compiled.element)) ==
            set_it->second.slots.end()) {
        return false;
    }
    address->set = set_key;
    address->binding = compiled.binding;
    address->element = compiled.element;
    return true;
}

static bool FlimeFastGetImmutableSampler(
        const DeviceState *device,
        const FlimeFastDescriptorCall &plan,
        const std::vector<FlimeFastBoundSet> &bound_sets,
        const FlimeFastAddress &compiled,
        bool *has_immutable, VkSampler *sampler) {
    FlimeShadowAddress address = {};
    if (has_immutable == NULL || sampler == NULL ||
        !FlimeFastResolveAddress(
            device, plan, bound_sets, compiled, &address)) {
        return false;
    }
    std::unordered_map<uint64_t, SetState>::const_iterator set_it =
        device->sets.find(address.set);
    if (set_it == device->sets.end()) return false;
    const LayoutBinding *binding =
        FlimeFindLayoutBinding(set_it->second.layout, address.binding);
    if (binding == NULL || address.element >= binding->count) return false;
    *has_immutable = !binding->immutable_samplers.empty();
    *sampler = VK_NULL_HANDLE;
    if (*has_immutable) {
        if (binding->immutable_samplers.size() != binding->count) return false;
        *sampler = binding->immutable_samplers[address.element];
    }
    return true;
}

static bool FlimeFastMaterializeWrite(
        const VkWriteDescriptorSet &write, uint32_t source_index,
        bool has_immutable_sampler, VkSampler immutable_sampler,
        DescriptorValue *out) {
    if (out == NULL || source_index >= write.descriptorCount) return false;
    DescriptorValue value;
    value.valid = true;
    value.type = write.descriptorType;
    if (FlimeDescriptorTypeIsBuffer(write.descriptorType)) {
        if (write.pBufferInfo == NULL) return false;
        const VkDescriptorBufferInfo &source =
            write.pBufferInfo[source_index];
        value.kind = kPayloadBuffer;
        value.buffer = source.buffer;
        value.offset = source.offset;
        value.range = source.range;
    } else if (FlimeDescriptorTypeIsImage(write.descriptorType)) {
        if (write.pImageInfo == NULL) return false;
        const VkDescriptorImageInfo &source =
            write.pImageInfo[source_index];
        value.kind = kPayloadImage;
        if (write.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER ||
            write.descriptorType ==
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
            value.sampler = has_immutable_sampler
                ? immutable_sampler : source.sampler;
        }
        if (write.descriptorType != VK_DESCRIPTOR_TYPE_SAMPLER) {
            value.image_view = source.imageView;
            value.image_layout = source.imageLayout;
        }
    } else if (FlimeDescriptorTypeIsTexel(write.descriptorType)) {
        if (write.pTexelBufferView == NULL) return false;
        value.kind = kPayloadTexel;
        value.buffer_view = write.pTexelBufferView[source_index];
    } else {
        return false;
    }
    *out = value;
    return true;
}

static bool FlimeFastMaterializeTemplate(
        VkDescriptorType type, const uint8_t *data, size_t offset,
        bool has_immutable_sampler, VkSampler immutable_sampler,
        DescriptorValue *out) {
    if (data == NULL || out == NULL) return false;
    VkWriteDescriptorSet write = {};
    write.descriptorCount = 1u;
    write.descriptorType = type;
    if (FlimeDescriptorTypeIsBuffer(type)) {
        const VkDescriptorBufferInfo value =
            FlimeLoadTemplateObject<VkDescriptorBufferInfo>(data, offset);
        write.pBufferInfo = &value;
        return FlimeFastMaterializeWrite(
            write, 0u, has_immutable_sampler, immutable_sampler, out);
    }
    if (FlimeDescriptorTypeIsImage(type)) {
        const VkDescriptorImageInfo value =
            FlimeLoadTemplateObject<VkDescriptorImageInfo>(data, offset);
        write.pImageInfo = &value;
        return FlimeFastMaterializeWrite(
            write, 0u, has_immutable_sampler, immutable_sampler, out);
    }
    if (FlimeDescriptorTypeIsTexel(type)) {
        const VkBufferView value =
            FlimeLoadTemplateObject<VkBufferView>(data, offset);
        write.pTexelBufferView = &value;
        return FlimeFastMaterializeWrite(
            write, 0u, has_immutable_sampler, immutable_sampler, out);
    }
    return false;
}

/*
 * This is deliberately two-phase.  Every guard, source and payload is
 * materialized into local vectors before the authoritative shadow is touched.
 * Consequently a cache miss can safely run the generic path and Recover.
 */
static bool FlimeFastExecutePlan(
        DeviceState *device, const FlimeFastDescriptorCall &plan,
        const std::vector<FlimeFastBoundSet> &bound_sets,
        const VkWriteDescriptorSet *writes,
        const uint8_t *template_data, bool capture_records) {
    if (device == NULL || bound_sets.size() != plan.set_guards.size() ||
        plan.operations.size() > kFlimeShadowMaxDescriptors) {
        return false;
    }
    for (size_t i = 0; i < plan.set_guards.size(); ++i) {
        FlimeFastBoundSet current = {};
        if (bound_sets[i].set == 0u ||
            !FlimeFastInspectSet(
                device, bound_sets[i].set, plan.set_guards[i], &current) ||
            !FlimeFastBoundSetsEqual(current, bound_sets[i])) {
            return false;
        }
    }
    uint64_t next_update_id = device->next_update_id;
    if (next_update_id == 0u) next_update_id = 1u;
    const uint64_t max_value = ~static_cast<uint64_t>(0u);
    if (static_cast<uint64_t>(plan.operations.size()) >
        max_value - next_update_id) {
        return false;
    }
    std::vector<DescriptorValue> values;
    std::vector<FlimeShadowMutation> mutations;
    values.reserve(plan.operations.size());
    mutations.reserve(plan.operations.size());
    for (size_t i = 0; i < plan.operations.size(); ++i) {
        const FlimeFastRouteOp &operation = plan.operations[i];
        FlimeShadowAddress destination = {};
        DescriptorValue value;
        bool has_immutable_sampler = false;
        VkSampler immutable_sampler = VK_NULL_HANDLE;
        if (!FlimeFastResolveAddress(
                device, plan, bound_sets,
                operation.destination, &destination) ||
            !FlimeFastGetImmutableSampler(
                device, plan, bound_sets, operation.destination,
                &has_immutable_sampler, &immutable_sampler) ||
            has_immutable_sampler != operation.has_immutable_sampler ||
            operation.type == VK_DESCRIPTOR_TYPE_MAX_ENUM ||
            (operation.route_flags & ~kRouteDerived) != 0u) {
            return false;
        }
        if (operation.source == kFlimeFastRawWrite) {
            if (writes == NULL ||
                operation.source_outer >= plan.write_count ||
                operation.route_flags != 0u ||
                !FlimeFastMaterializeWrite(
                    writes[operation.source_outer],
                    operation.source_inner,
                    has_immutable_sampler, immutable_sampler, &value)) {
                return false;
            }
        } else if (operation.source == kFlimeFastTemplateData) {
            if (plan.kind != kFlimeFastUpdateTemplate ||
                operation.route_flags != 0u ||
                !FlimeFastMaterializeTemplate(
                    operation.type, template_data,
                    operation.template_data_offset,
                    has_immutable_sampler, immutable_sampler, &value)) {
                return false;
            }
        } else if (operation.source == kFlimeFastPriorMutation) {
            if (operation.route_flags != kRouteDerived ||
                operation.source_outer >= i ||
                operation.source_outer >= values.size()) {
                return false;
            }
            value = values[operation.source_outer];
        } else if (operation.source == kFlimeFastShadowSlot) {
            FlimeShadowAddress source = {};
            if (operation.route_flags != kRouteDerived ||
                !FlimeFastResolveAddress(
                    device, plan, bound_sets,
                    operation.source_slot, &source)) {
                return false;
            }
            std::unordered_map<uint64_t, SetState>::const_iterator source_set =
                device->sets.find(source.set);
            if (source_set == device->sets.end()) return false;
            std::map<std::pair<uint32_t, uint32_t>,
                     DescriptorSlot>::const_iterator source_slot =
                source_set->second.slots.find(
                    std::make_pair(source.binding, source.element));
            if (source_slot == source_set->second.slots.end()) return false;
            value = source_slot->second.value;
        } else {
            return false;
        }
        if (!value.valid || value.type != operation.type) return false;
        if (operation.route_flags == kRouteDerived &&
            has_immutable_sampler &&
            (value.type == VK_DESCRIPTOR_TYPE_SAMPLER ||
             value.type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)) {
            value.sampler = immutable_sampler;
        }
        const uint64_t ordinal = static_cast<uint64_t>(mutations.size());
        if (ordinal > max_value - plan.template_offset ||
            next_update_id == 0u || next_update_id == max_value) {
            return false;
        }
        FlimeShadowMutation mutation = {};
        mutation.address = destination;
        mutation.value = value;
        mutation.update_id = next_update_id++;
        mutation.template_offset = plan.template_offset + ordinal;
        mutation.flags = operation.route_flags;
        values.push_back(value);
        mutations.push_back(mutation);
    }
    return FlimeCommitShadowMutations(
        device, mutations, next_update_id, capture_records);
}

FlimeFastApplyResult FlimeApplyDescriptorSetFastPlan(
        DeviceState *device, const FlimeFastDescriptorCall &plan,
        int fun_id, size_t semantic_call_index,
        uint64_t template_offset, uint64_t encoded_bytes,
        uint32_t write_count, const VkWriteDescriptorSet *writes,
        uint32_t copy_count, const VkCopyDescriptorSet *copies,
        bool capture_records) {
    if (device == NULL || plan.kind != kFlimeFastUpdateSets ||
        plan.fun_id != fun_id ||
        plan.semantic_call_index != semantic_call_index ||
        plan.template_offset != template_offset ||
        plan.encoded_bytes != encoded_bytes ||
        plan.write_count != write_count || plan.copy_count != copy_count ||
        plan.writes.size() != write_count ||
        plan.copies.size() != copy_count ||
        (write_count != 0u && writes == NULL) ||
        (copy_count != 0u && copies == NULL)) {
        return kFlimeFastMiss;
    }
    std::map<uint32_t, FlimeFastBoundSet> role_bindings =
        device->descriptor_role_bindings;
    std::map<uint64_t, uint32_t> handle_roles =
        device->descriptor_handle_roles;
    std::vector<FlimeFastBoundSet> bound_sets(plan.set_guards.size());
    for (uint32_t i = 0; i < write_count; ++i) {
        const VkWriteDescriptorSet &write = writes[i];
        const FlimeFastWriteShape &shape = plan.writes[i];
        if (shape.set_guard >= plan.set_guards.size() ||
            write.sType != VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET ||
            write.pNext != NULL ||
            write.dstBinding != shape.binding ||
            write.dstArrayElement != shape.array_element ||
            write.descriptorCount != shape.count ||
            write.descriptorType != shape.type ||
            !FlimeFastBindSetRole(
                device, plan, shape.set_guard, HandleBits(write.dstSet),
                &role_bindings, &handle_roles, &bound_sets)) {
            return kFlimeFastMiss;
        }
    }
    for (uint32_t i = 0; i < copy_count; ++i) {
        const VkCopyDescriptorSet &copy = copies[i];
        const FlimeFastCopyShape &shape = plan.copies[i];
        if (shape.source_guard >= plan.set_guards.size() ||
            shape.destination_guard >= plan.set_guards.size() ||
            copy.sType != VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET ||
            copy.pNext != NULL ||
            copy.srcBinding != shape.source_binding ||
            copy.srcArrayElement != shape.source_array_element ||
            copy.dstBinding != shape.destination_binding ||
            copy.dstArrayElement != shape.destination_array_element ||
            copy.descriptorCount != shape.count ||
            !FlimeFastBindSetRole(
                device, plan, shape.source_guard, HandleBits(copy.srcSet),
                &role_bindings, &handle_roles, &bound_sets) ||
            !FlimeFastBindSetRole(
                device, plan, shape.destination_guard,
                HandleBits(copy.dstSet), &role_bindings,
                &handle_roles, &bound_sets)) {
            return kFlimeFastMiss;
        }
    }
    for (size_t i = 0; i < bound_sets.size(); ++i) {
        if (bound_sets[i].set == 0u) return kFlimeFastMiss;
    }
    if (!FlimeFastExecutePlan(
            device, plan, bound_sets, writes, NULL, capture_records)) {
        return kFlimeFastMiss;
    }
    device->descriptor_role_bindings.swap(role_bindings);
    device->descriptor_handle_roles.swap(handle_roles);
    return kFlimeFastApplied;
}

FlimeFastApplyResult FlimeApplyDescriptorTemplateFastPlan(
        DeviceState *device, const FlimeFastDescriptorCall &plan,
        int fun_id, size_t semantic_call_index,
        uint64_t template_offset, uint64_t encoded_bytes,
        VkDescriptorSet set,
        VkDescriptorUpdateTemplate update_template,
        const void *data, bool capture_records) {
    if (device == NULL || plan.kind != kFlimeFastUpdateTemplate ||
        plan.fun_id != fun_id ||
        plan.semantic_call_index != semantic_call_index ||
        plan.template_offset != template_offset ||
        plan.encoded_bytes != encoded_bytes ||
        plan.template_set_guard >= plan.set_guards.size() ||
        HandleBits(update_template) != plan.update_template ||
        (!plan.operations.empty() && data == NULL)) {
        return kFlimeFastMiss;
    }
    std::unordered_map<uint64_t, UpdateTemplateState>::const_iterator found =
        device->update_templates.find(plan.update_template);
    if (found == device->update_templates.end() ||
        !found->second.supported ||
        found->second.generation != plan.update_template_generation ||
        found->second.layout_generation !=
            plan.template_layout_generation ||
        found->second.type !=
            VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET) {
        return kFlimeFastMiss;
    }
    std::map<uint32_t, FlimeFastBoundSet> role_bindings =
        device->descriptor_role_bindings;
    std::map<uint64_t, uint32_t> handle_roles =
        device->descriptor_handle_roles;
    std::vector<FlimeFastBoundSet> bound_sets(plan.set_guards.size());
    if (!FlimeFastBindSetRole(
            device, plan, plan.template_set_guard, HandleBits(set),
            &role_bindings, &handle_roles, &bound_sets)) {
        return kFlimeFastMiss;
    }
    for (size_t i = 0; i < bound_sets.size(); ++i) {
        if (bound_sets[i].set == 0u) return kFlimeFastMiss;
    }
    if (!FlimeFastExecutePlan(
            device, plan, bound_sets, NULL,
            static_cast<const uint8_t *>(data), capture_records)) {
        return kFlimeFastMiss;
    }
    device->descriptor_role_bindings.swap(role_bindings);
    device->descriptor_handle_roles.swap(handle_roles);
    return kFlimeFastApplied;
}

static bool FlimeDescriptorValueShapeValid(const DescriptorValue &value) {
    if (!value.valid) {
        return value.type == VK_DESCRIPTOR_TYPE_MAX_ENUM &&
               value.kind == 0u;
    }
    if (FlimeDescriptorTypeIsBuffer(value.type)) {
        return value.kind == kPayloadBuffer;
    }
    if (FlimeDescriptorTypeIsImage(value.type)) {
        return value.kind == kPayloadImage;
    }
    if (FlimeDescriptorTypeIsTexel(value.type)) {
        return value.kind == kPayloadTexel;
    }
    return false;
}

static bool FlimeDescriptorValuesEqual(const DescriptorValue &left,
                                       const DescriptorValue &right) {
    return left.valid == right.valid &&
           left.type == right.type &&
           left.kind == right.kind &&
           HandleBits(left.buffer) == HandleBits(right.buffer) &&
           left.offset == right.offset &&
           left.range == right.range &&
           HandleBits(left.sampler) == HandleBits(right.sampler) &&
           HandleBits(left.image_view) == HandleBits(right.image_view) &&
           left.image_layout == right.image_layout &&
           HandleBits(left.buffer_view) == HandleBits(right.buffer_view);
}

static uint64_t FlimeHashDescriptorValue(uint64_t hash,
                                         const DescriptorValue &value) {
    hash = HashWord(hash, value.valid ? 1u : 0u);
    hash = HashWord(hash, static_cast<uint32_t>(value.type));
    hash = HashWord(hash, value.kind);
    hash = HashWord(hash, HandleBits(value.buffer));
    hash = HashWord(hash, value.offset);
    hash = HashWord(hash, value.range);
    hash = HashWord(hash, HandleBits(value.sampler));
    hash = HashWord(hash, HandleBits(value.image_view));
    hash = HashWord(hash, static_cast<uint32_t>(value.image_layout));
    return HashWord(hash, HandleBits(value.buffer_view));
}

static bool FlimeHashRecoveryShadow(
        const DeviceState *device,
        const std::set<uint64_t> &consumed_sets,
        uint64_t *shadow_hash, size_t *valid_slots) {
    if (device == NULL || shadow_hash == NULL || valid_slots == NULL) {
        return false;
    }
    uint64_t hash = HashWord(
        UINT64_C(0x5245434f56455259), consumed_sets.size());
    size_t valid = 0u;
    for (std::set<uint64_t>::const_iterator key = consumed_sets.begin();
         key != consumed_sets.end(); ++key) {
        std::unordered_map<uint64_t, SetState>::const_iterator set_it =
            device->sets.find(*key);
        if (set_it == device->sets.end() || !set_it->second.supported ||
            !set_it->second.layout.supported ||
            set_it->second.generation == 0u ||
            set_it->second.layout.generation == 0u ||
            set_it->second.pool_generation == 0u) {
            return false;
        }
        const uint64_t pool_key = HandleBits(set_it->second.pool);
        std::unordered_map<uint64_t, PoolState>::const_iterator pool_it =
            device->pools.find(pool_key);
        if (pool_it == device->pools.end() ||
            pool_it->second.generation != set_it->second.pool_generation ||
            pool_it->second.sets.find(*key) == pool_it->second.sets.end() ||
            HandleBits(set_it->second.device) != HandleBits(device->device) ||
            HandleBits(pool_it->second.device) != HandleBits(device->device)) {
            return false;
        }
        hash = HashWord(hash, *key);
        hash = HashWord(hash, set_it->second.generation);
        hash = HashWord(hash, set_it->second.layout.generation);
        hash = HashWord(hash, pool_key);
        hash = HashWord(hash, pool_it->second.generation);
        hash = HashWord(hash, set_it->second.slots.size());
        uint64_t layout_slots = 0u;
        for (size_t binding = 0;
             binding < set_it->second.layout.bindings.size(); ++binding) {
            layout_slots +=
                set_it->second.layout.bindings[binding].count;
            if (layout_slots > kFlimeShadowMaxDescriptors) return false;
        }
        if (layout_slots != set_it->second.slots.size()) return false;
        for (std::map<std::pair<uint32_t, uint32_t>,
                      DescriptorSlot>::const_iterator slot =
                 set_it->second.slots.begin();
             slot != set_it->second.slots.end(); ++slot) {
            const LayoutBinding *binding = FlimeFindLayoutBinding(
                set_it->second.layout, slot->first.first);
            if (binding == NULL || slot->first.second >= binding->count ||
                !FlimeDescriptorValueShapeValid(slot->second.value) ||
                (slot->second.value.valid &&
                 slot->second.value.type != binding->type)) {
                return false;
            }
            hash = HashWord(hash, slot->first.first);
            hash = HashWord(hash, slot->first.second);
            hash = FlimeHashDescriptorValue(hash, slot->second.value);
            if (slot->second.value.valid) {
                if (valid == kMaxRouteRecords) return false;
                ++valid;
            }
        }
    }
    *shadow_hash = hash;
    *valid_slots = valid;
    return true;
}

static bool FlimePendingRecordMatchesSlot(
        const DeviceState *device, const PendingRecord &record,
        size_t record_index, bool require_pending_link) {
    if (device == NULL || record.elided || record.released ||
        record.update_id == 0u || !record.value.valid ||
        !FlimeDescriptorValueShapeValid(record.value) ||
        record.type != record.value.type ||
        (record.flags & ~kRouteDerived) != 0u) {
        return false;
    }
    std::unordered_map<uint64_t, SetState>::const_iterator set_it =
        device->sets.find(HandleBits(record.set));
    if (set_it == device->sets.end()) return false;
    std::map<std::pair<uint32_t, uint32_t>,
             DescriptorSlot>::const_iterator slot =
        set_it->second.slots.find(
            std::make_pair(record.binding, record.array_element));
    if (slot == set_it->second.slots.end() ||
        !FlimeDescriptorValuesEqual(slot->second.value, record.value)) {
        return false;
    }
    return !require_pending_link ||
           (record_index <= static_cast<size_t>(INT64_MAX) &&
            slot->second.pending_record ==
                static_cast<int64_t>(record_index));
}

/*
 * Replace the pending suffix with a complete image of the authoritative
 * descriptor shadow for every replay set.  Recovery is deliberately an
 * all-pending transaction: the caller must first add every live PendingRecord
 * destination to consumed_sets.  All validation and allocation precede the
 * records/pending-link swap, so failure leaves the prior shadow transaction
 * intact for fallback.
 */
static bool FlimeShadowRebuildRecoveryCheckpoint(
        DeviceState *device,
        const std::set<uint64_t> &consumed_sets) {
    if (device == NULL) return false;
    device->recovery_checkpoint.Clear();

    uint64_t shadow_hash = 0u;
    size_t valid_slots = 0u;
    if (!FlimeHashRecoveryShadow(
            device, consumed_sets, &shadow_hash, &valid_slots) ||
        device->next_update_id == 0u ||
        device->next_update_id == UINT64_MAX) {
        return false;
    }

    std::set<uint64_t> all_update_ids;
    std::set<uint64_t> checkpoint_consumed_sets = consumed_sets;
    std::set<
        std::pair<uint64_t, std::pair<uint32_t, uint32_t> > > live_slots;
    std::vector<PendingRecord> rebuilt;
    rebuilt.reserve(valid_slots);
    for (size_t i = 0; i < device->records.size(); ++i) {
        const PendingRecord &record = device->records[i];
        if (record.elided) continue;
        if (!FlimePendingRecordMatchesSlot(device, record, i, true) ||
            record.update_id >= device->next_update_id ||
            !all_update_ids.insert(record.update_id).second ||
            !live_slots.insert(std::make_pair(
                HandleBits(record.set), std::make_pair(
                    record.binding, record.array_element))).second) {
            return false;
        }
        if (consumed_sets.find(HandleBits(record.set)) ==
            consumed_sets.end()) {
            return false;
        }
    }

    for (std::unordered_map<uint64_t, SetState>::const_iterator set =
             device->sets.begin(); set != device->sets.end(); ++set) {
        for (std::map<std::pair<uint32_t, uint32_t>,
                      DescriptorSlot>::const_iterator slot =
                 set->second.slots.begin();
             slot != set->second.slots.end(); ++slot) {
            if (slot->second.pending_record < 0) continue;
            const size_t index =
                static_cast<size_t>(slot->second.pending_record);
            if (index >= device->records.size()) return false;
            const PendingRecord &record = device->records[index];
            if (record.elided || record.released ||
                HandleBits(record.set) != set->first ||
                record.binding != slot->first.first ||
                record.array_element != slot->first.second ||
                !FlimeDescriptorValuesEqual(
                    record.value, slot->second.value)) {
                return false;
            }
        }
    }

    if (rebuilt.size() > kMaxRouteRecords ||
        valid_slots > kMaxRouteRecords - rebuilt.size()) {
        return false;
    }
    uint64_t next_update_id = device->next_update_id;
    if (valid_slots != 0u &&
        static_cast<uint64_t>(valid_slots) >=
            UINT64_MAX - next_update_id) {
        return false;
    }
    std::set<uint64_t> rebuilt_update_ids;
    for (std::set<uint64_t>::const_iterator key = consumed_sets.begin();
         key != consumed_sets.end(); ++key) {
        std::unordered_map<uint64_t, SetState>::const_iterator set_it =
            device->sets.find(*key);
        if (set_it == device->sets.end()) return false;
        for (std::map<std::pair<uint32_t, uint32_t>,
                      DescriptorSlot>::const_iterator slot =
                 set_it->second.slots.begin();
             slot != set_it->second.slots.end(); ++slot) {
            if (!slot->second.value.valid) continue;
            if (next_update_id == 0u || next_update_id == UINT64_MAX ||
                !all_update_ids.insert(next_update_id).second) {
                return false;
            }
            PendingRecord record;
            record.update_id = next_update_id++;
            record.template_offset = 0u;
            record.set = (VkDescriptorSet)(uintptr_t)(*key);
            record.binding = slot->first.first;
            record.array_element = slot->first.second;
            record.type = slot->second.value.type;
            record.flags = 0u;
            record.value = slot->second.value;
            rebuilt_update_ids.insert(record.update_id);
            rebuilt.push_back(record);
        }
    }
    if (rebuilt_update_ids.size() != valid_slots ||
        rebuilt.size() > static_cast<size_t>(INT64_MAX)) {
        return false;
    }

    std::map<FlimeShadowKey, int64_t> pending_links;
    for (size_t i = 0; i < rebuilt.size(); ++i) {
        FlimeShadowAddress address = {};
        address.set = HandleBits(rebuilt[i].set);
        address.binding = rebuilt[i].binding;
        address.element = rebuilt[i].array_element;
        std::unordered_map<uint64_t, SetState>::const_iterator set_it =
            device->sets.find(address.set);
        if (set_it == device->sets.end() ||
            set_it->second.slots.find(std::make_pair(
                address.binding, address.element)) ==
                set_it->second.slots.end() ||
            !pending_links.insert(std::make_pair(
                FlimeAddressKey(address), static_cast<int64_t>(i))).second) {
            return false;
        }
    }

    device->records.swap(rebuilt);
    for (std::unordered_map<uint64_t, SetState>::iterator set =
             device->sets.begin(); set != device->sets.end(); ++set) {
        for (std::map<std::pair<uint32_t, uint32_t>,
                      DescriptorSlot>::iterator slot =
                 set->second.slots.begin();
             slot != set->second.slots.end(); ++slot) {
            FlimeShadowAddress address = {};
            address.set = set->first;
            address.binding = slot->first.first;
            address.element = slot->first.second;
            std::map<FlimeShadowKey, int64_t>::const_iterator pending =
                pending_links.find(FlimeAddressKey(address));
            slot->second.pending_record = pending == pending_links.end()
                ? -1 : pending->second;
        }
    }
    device->next_update_id = next_update_id;
    device->recovery_checkpoint.shadow_hash = shadow_hash;
    device->recovery_checkpoint.next_update_id = next_update_id;
    device->recovery_checkpoint.consumed_sets.swap(
        checkpoint_consumed_sets);
    device->recovery_checkpoint.rebuilt_update_ids.swap(
        rebuilt_update_ids);
    device->recovery_checkpoint.ready = true;
    return true;
}

/*
 * Recovery cannot leave a deferred suffix behind when it returns to Detect.
 * Form the replay set from both this submit's consumers and every still-live
 * PendingRecord, validate every slot back-reference before changing anything,
 * and only then atomically rebuild the full-shadow checkpoint.
 */
static bool FlimeShadowPrepareCompleteRecoveryCheckpoint(
        DeviceState *device, std::set<uint64_t> *replay_sets) {
    if (device == NULL || replay_sets == NULL) return false;
    for (size_t i = 0; i < device->records.size(); ++i) {
        const PendingRecord &record = device->records[i];
        if (record.elided) continue;
        const uint64_t set_key = HandleBits(record.set);
        if (record.released || record.update_id == 0u ||
            record.update_id >= device->next_update_id || set_key == 0u ||
            !FlimePendingRecordMatchesSlot(device, record, i, true)) {
            return false;
        }
        replay_sets->insert(set_key);
        if (replay_sets->size() > kFlimeShadowMaxDescriptors) return false;
    }
    return FlimeShadowRebuildRecoveryCheckpoint(device, *replay_sets);
}

static bool FlimeShadowVerifyRecoveryCheckpoint(
        const DeviceState *device,
        const std::set<uint64_t> &consumed_sets,
        const std::set<uint64_t> &frontier_update_ids,
        bool routed) {
    if (device == NULL || !device->recovery_checkpoint.ready ||
        device->recovery_checkpoint.consumed_sets != consumed_sets ||
        device->recovery_checkpoint.rebuilt_update_ids !=
            frontier_update_ids ||
        device->recovery_checkpoint.next_update_id !=
            device->next_update_id ||
        device->records.size() !=
            device->recovery_checkpoint.rebuilt_update_ids.size()) {
        return false;
    }
    uint64_t shadow_hash = 0u;
    size_t valid_slots = 0u;
    if (!FlimeHashRecoveryShadow(
            device, consumed_sets, &shadow_hash, &valid_slots) ||
        shadow_hash != device->recovery_checkpoint.shadow_hash ||
        valid_slots !=
            device->recovery_checkpoint.rebuilt_update_ids.size()) {
        return false;
    }

    std::set<uint64_t> seen_update_ids;
    std::set<uint64_t> offsets;
    std::set<
        std::pair<uint64_t, std::pair<uint32_t, uint32_t> > > rebuilt_slots;
    size_t rebuilt_records = 0u;
    for (size_t i = 0; i < device->records.size(); ++i) {
        const PendingRecord &record = device->records[i];
        const uint64_t set_key = HandleBits(record.set);
        const bool rebuilt =
            device->recovery_checkpoint.rebuilt_update_ids.find(
                record.update_id) !=
            device->recovery_checkpoint.rebuilt_update_ids.end();
        if (record.elided || record.update_id == 0u ||
            record.update_id >= device->next_update_id ||
            !rebuilt ||
            !seen_update_ids.insert(record.update_id).second) {
            return false;
        }

        std::unordered_map<uint64_t, SetState>::const_iterator set_it =
            device->sets.find(set_key);
        if (consumed_sets.find(set_key) == consumed_sets.end() ||
            set_it == device->sets.end() || record.flags != 0u ||
            record.released != routed || !record.value.valid ||
            !FlimeDescriptorValueShapeValid(record.value) ||
            record.type != record.value.type ||
            record.template_offset >=
                device->recovery_checkpoint.rebuilt_update_ids.size() ||
            !offsets.insert(record.template_offset).second) {
            return false;
        }
        std::map<std::pair<uint32_t, uint32_t>,
                 DescriptorSlot>::const_iterator slot =
            set_it->second.slots.find(
                std::make_pair(record.binding, record.array_element));
        if (slot == set_it->second.slots.end() ||
            !FlimeDescriptorValuesEqual(
                slot->second.value, record.value) ||
            (routed ? slot->second.pending_record != -1 :
             slot->second.pending_record != static_cast<int64_t>(i)) ||
            !rebuilt_slots.insert(std::make_pair(
                set_key, std::make_pair(
                    record.binding, record.array_element))).second) {
            return false;
        }
        ++rebuilt_records;
    }
    if (rebuilt_records !=
            device->recovery_checkpoint.rebuilt_update_ids.size() ||
        offsets.size() != rebuilt_records) {
        return false;
    }
    for (std::set<uint64_t>::const_iterator key = consumed_sets.begin();
         key != consumed_sets.end(); ++key) {
        std::unordered_map<uint64_t, SetState>::const_iterator set_it =
            device->sets.find(*key);
        if (set_it == device->sets.end()) return false;
        for (std::map<std::pair<uint32_t, uint32_t>,
                      DescriptorSlot>::const_iterator slot =
                 set_it->second.slots.begin();
             slot != set_it->second.slots.end(); ++slot) {
            const bool has_record = rebuilt_slots.find(std::make_pair(
                *key, std::make_pair(
                    slot->first.first, slot->first.second))) !=
                rebuilt_slots.end();
            if (has_record != slot->second.value.valid) return false;
        }
    }
    if (routed) {
        for (std::unordered_map<uint64_t, SetState>::const_iterator set =
                 device->sets.begin(); set != device->sets.end(); ++set) {
            for (std::map<std::pair<uint32_t, uint32_t>,
                          DescriptorSlot>::const_iterator slot =
                     set->second.slots.begin();
                 slot != set->second.slots.end(); ++slot) {
                if (slot->second.pending_record != -1) return false;
            }
        }
    }
    return seen_update_ids.size() == device->records.size();
}

static bool FlimeShadowMarkRecordReleased(DeviceState *device, size_t index) {
    if (device == NULL || index >= device->records.size()) return false;
    PendingRecord &record = device->records[index];
    record.released = true;
    std::unordered_map<uint64_t, SetState>::iterator set_it =
        device->sets.find(HandleBits(record.set));
    if (set_it != device->sets.end()) {
        std::map<std::pair<uint32_t, uint32_t>, DescriptorSlot>::iterator
            slot_it = set_it->second.slots.find(
                std::make_pair(record.binding, record.array_element));
        if (slot_it != set_it->second.slots.end() &&
            slot_it->second.pending_record == static_cast<int64_t>(index)) {
            slot_it->second.pending_record = -1;
        }
    }
    return true;
}

static bool FlimeDescriptorReferencesObject(const DescriptorValue &value,
                                            VkObjectType object_type,
                                            uint64_t object) {
    if (!value.valid || object == 0u) return false;
    switch (object_type) {
    case VK_OBJECT_TYPE_BUFFER:
        return HandleBits(value.buffer) == object;
    case VK_OBJECT_TYPE_BUFFER_VIEW:
        return HandleBits(value.buffer_view) == object;
    case VK_OBJECT_TYPE_IMAGE_VIEW:
        return HandleBits(value.image_view) == object;
    case VK_OBJECT_TYPE_SAMPLER:
        return HandleBits(value.sampler) == object;
    default:
        return false;
    }
}

bool FlimeShadowInvalidatePayloadObject(DeviceState *device,
                                               VkObjectType object_type,
                                               uint64_t object,
                                               bool *changed) {
    if (device == NULL || changed == NULL) return false;
    *changed = false;
    if (object == 0u) return true;
    if (object_type != VK_OBJECT_TYPE_BUFFER &&
        object_type != VK_OBJECT_TYPE_BUFFER_VIEW &&
        object_type != VK_OBJECT_TYPE_IMAGE_VIEW &&
        object_type != VK_OBJECT_TYPE_SAMPLER) {
        return false;
    }
    for (std::unordered_map<uint64_t, SetState>::iterator set =
             device->sets.begin(); set != device->sets.end(); ++set) {
        for (std::map<std::pair<uint32_t, uint32_t>, DescriptorSlot>::iterator
                 slot = set->second.slots.begin();
             slot != set->second.slots.end(); ++slot) {
            DescriptorSlot &entry = slot->second;
            if (!FlimeDescriptorReferencesObject(
                    entry.value, object_type, object)) {
                continue;
            }
            if (entry.pending_record >= 0 &&
                static_cast<size_t>(entry.pending_record) <
                    device->records.size()) {
                PendingRecord &record =
                    device->records[static_cast<size_t>(
                        entry.pending_record)];
                if (!record.released) record.elided = true;
            }
            entry.pending_record = -1;
            entry.value = DescriptorValue();
            *changed = true;
        }
        if (object_type == VK_OBJECT_TYPE_SAMPLER) {
            for (size_t i = 0; i < set->second.layout.bindings.size(); ++i) {
                const std::vector<VkSampler> &samplers =
                    set->second.layout.bindings[i].immutable_samplers;
                for (size_t j = 0; j < samplers.size(); ++j) {
                    if (HandleBits(samplers[j]) == object) {
                        set->second.supported = false;
                        *changed = true;
                    }
                }
            }
        }
    }
    if (object_type == VK_OBJECT_TYPE_SAMPLER) {
        for (std::unordered_map<uint64_t, LayoutState>::iterator layout =
                 device->layouts.begin(); layout != device->layouts.end();
             ++layout) {
            for (size_t i = 0; i < layout->second.bindings.size(); ++i) {
                const std::vector<VkSampler> &samplers =
                    layout->second.bindings[i].immutable_samplers;
                for (size_t j = 0; j < samplers.size(); ++j) {
                    if (HandleBits(samplers[j]) == object) {
                        layout->second.supported = false;
                        *changed = true;
                    }
                }
            }
        }
    }
    return true;
}


/*
 * Period discovery, template matching, chunk planning and the FLIME wire
 * protocol.  The encoder treats every submitted command buffer as a replayable
 * semantic stream, so recording once and submitting many times is observable.
 */

const uint32_t kRouteReplyKnownFlags = 63u;

enum RouteReplyDisposition {
    kRouteReplyAccepted,
    kRouteReplyNeedsRecovery,
    kRouteReplyInvalid,
};

size_t LiveRecordCount(const DeviceState& state);

struct ProtocolOccurrence {
    std::vector<SemanticCall> calls;
    std::set<uint64_t> consumed_descriptor_sets;
    std::set<uint64_t> frontier_update_ids;
    uint64_t bytes;
    uint32_t dispatches;
    uint64_t detect_signature;
    uint64_t signature;
    uint64_t frontier_base_bytes;
    uint64_t frontier_base_signature;
    uint64_t frontier_prefix_bytes;
    bool route_all_records;
    bool has_opaque_command;
    bool frontier_prepared;

    ProtocolOccurrence()
        : bytes(0), dispatches(0), detect_signature(0), signature(0),
          frontier_base_bytes(0), frontier_base_signature(0),
          frontier_prefix_bytes(0), route_all_records(false),
          has_opaque_command(false), frontier_prepared(false) {}
};

struct RoutedRecordGroup {
    uint64_t template_begin;
    uint64_t template_end;
    std::vector<size_t> record_indices;

    RoutedRecordGroup() : template_begin(0), template_end(0) {}
};

uint64_t ProtocolMix(uint64_t hash, uint64_t value) {
    hash ^= value + UINT64_C(0x9e3779b97f4a7c15) +
            (hash << 6) + (hash >> 2);
    return hash;
}

bool ProtocolAppendCall(const SemanticCall& source,
                        ProtocolOccurrence* occurrence) {
    if (occurrence == NULL ||
        occurrence->calls.size() >= kMaxSemanticCalls) {
        return false;
    }
    SemanticCall call = source;
    const uint64_t span = std::max<uint64_t>(call.encoded_bytes, 1);
    if (occurrence->bytes > kMaxTemplateBytes - span) return false;
    call.template_offset = occurrence->bytes;
    occurrence->calls.push_back(call);
    occurrence->bytes += span;
    occurrence->has_opaque_command =
        occurrence->has_opaque_command || call.opaque;
    if (call.dispatch) {
        if (occurrence->dispatches == UINT32_MAX) return false;
        ++occurrence->dispatches;
    }
    return true;
}

bool ProtocolAppendRecordedLocked(
        DeviceState* state,
        uint64_t command_key,
        std::set<uint64_t>* recursion_stack,
        unsigned depth,
        ProtocolOccurrence* occurrence) {
    if (state == NULL || recursion_stack == NULL || occurrence == NULL ||
        depth > 64 || !recursion_stack->insert(command_key).second) {
        return false;
    }
    std::unordered_map<uint64_t, CommandState>::const_iterator command =
        state->commands.find(command_key);
    std::unordered_map<uint64_t, RecordedCommandStream>::const_iterator stream =
        g_recorded_commands.find(command_key);
    if (command == state->commands.end() || !command->second.executable ||
        command->second.recording || stream == g_recorded_commands.end() ||
        stream->second.generation != command->second.generation) {
        recursion_stack->erase(command_key);
        return false;
    }
    std::map<uint32_t, uint64_t> bound_compute_sets;
    for (size_t i = 0; i < stream->second.calls.size(); ++i) {
        const SemanticCall& call = stream->second.calls[i];
        if (!ProtocolAppendCall(call, occurrence)) {
            recursion_stack->erase(command_key);
            return false;
        }
        if (call.fun_id == FUNID_vkCmdBindDescriptorSets) {
            if (call.structural.size() < 4 ||
                call.structural[1] > UINT32_MAX ||
                call.structural[2] > UINT32_MAX ||
                call.structural[1] >
                    UINT32_MAX - call.structural[2] ||
                call.handles.size() !=
                    static_cast<size_t>(2 + call.structural[2])) {
                recursion_stack->erase(command_key);
                return false;
            }
            if (call.structural[0] ==
                    static_cast<uint32_t>(
                        VK_PIPELINE_BIND_POINT_COMPUTE)) {
                const uint32_t first_set =
                    static_cast<uint32_t>(call.structural[1]);
                const uint32_t set_count =
                    static_cast<uint32_t>(call.structural[2]);
                for (uint32_t set_index = 0;
                     set_index < set_count; ++set_index) {
                    const uint32_t slot = first_set + set_index;
                    const uint64_t descriptor_set =
                        call.handles[2 + set_index];
                    if (descriptor_set == 0) {
                        bound_compute_sets.erase(slot);
                    } else {
                        bound_compute_sets[slot] = descriptor_set;
                    }
                }
            }
        }
        if (call.dispatch) {
            for (std::map<uint32_t, uint64_t>::const_iterator set =
                     bound_compute_sets.begin();
                 set != bound_compute_sets.end(); ++set) {
                if (set->second != 0) {
                    occurrence->consumed_descriptor_sets.insert(
                        set->second);
                }
            }
        }
        if (!call.execute_secondary) continue;
        for (size_t child = 0; child < call.secondary_commands.size();
             ++child) {
            if (!ProtocolAppendRecordedLocked(
                    state, call.secondary_commands[child], recursion_stack,
                    depth + 1, occurrence)) {
                recursion_stack->erase(command_key);
                return false;
            }
        }
    }
    recursion_stack->erase(command_key);
    return true;
}

bool AssembleOccurrenceLocked(
        DeviceState* state,
        const std::vector<VkCommandBuffer>& submitted,
        const SemanticCall& submit_call,
        ProtocolOccurrence* occurrence) {
    if (state == NULL || occurrence == NULL) return false;
    *occurrence = ProtocolOccurrence();
    for (size_t i = 0; i < state->calls.size(); ++i) {
        if (!ProtocolAppendCall(state->calls[i], occurrence)) return false;
    }
    std::set<uint64_t> recursion_stack;
    for (size_t i = 0; i < submitted.size(); ++i) {
        if (!ProtocolAppendRecordedLocked(
                state, HandleBits(submitted[i]), &recursion_stack, 0,
                occurrence)) {
            return false;
        }
    }
    if (!ProtocolAppendCall(submit_call, occurrence) ||
        occurrence->calls.empty() || occurrence->bytes == 0) {
        return false;
    }

    /*
     * Detect deliberately sees only the ordered API operation identities.
     * Runtime structure, handles, payload and encoded sizes belong exclusively
     * to the stricter Learn/Match signature below.
     */
    uint64_t detect_hash = UINT64_C(0x4445544543544f50);
    detect_hash = ProtocolMix(detect_hash, occurrence->calls.size());
    for (size_t i = 0; i < occurrence->calls.size(); ++i) {
        detect_hash = ProtocolMix(
            detect_hash,
            static_cast<uint32_t>(occurrence->calls[i].fun_id));
    }
    occurrence->detect_signature = detect_hash;

    std::map<uint64_t, uint32_t> roles;
    uint32_t next_role = 1;
    uint64_t hash = UINT64_C(0xcbf29ce484222325);
    for (size_t i = 0; i < occurrence->calls.size(); ++i) {
        const SemanticCall& call = occurrence->calls[i];
        hash = ProtocolMix(hash, static_cast<uint32_t>(call.fun_id));
        hash = ProtocolMix(hash, call.structural.size());
        for (size_t field = 0; field < call.structural.size(); ++field) {
            hash = ProtocolMix(hash, call.structural[field]);
        }
        hash = ProtocolMix(hash, call.handles.size());
        for (size_t field = 0; field < call.handles.size(); ++field) {
            const uint64_t handle = call.handles[field];
            uint32_t role = 0;
            if (handle != 0) {
                std::map<uint64_t, uint32_t>::iterator known =
                    roles.find(handle);
                if (known == roles.end()) {
                    if (next_role == UINT32_MAX) return false;
                    role = next_role++;
                    roles[handle] = role;
                } else {
                    role = known->second;
                }
            }
            hash = ProtocolMix(hash, role);
        }
        hash = ProtocolMix(hash, call.dispatch ? 1 : 0);
        hash = ProtocolMix(hash, call.execute_secondary ? 1 : 0);
        hash = ProtocolMix(hash, call.encoded_bytes);
    }
    occurrence->signature = hash;
    return true;
}

uint32_t FrontierRecordCount(const ProtocolOccurrence& occurrence) {
    return static_cast<uint32_t>(occurrence.frontier_update_ids.size());
}

/*
 * Freeze the Type-II frontier after the submitted command-buffer graph has
 * been expanded.  A record is eligible only when this submit contains a
 * compute dispatch reached after binding its destination set.  The frozen
 * update-id set, rather than the mutable set handle alone, is then used by
 * normal routing, recovery, and completion.
 *
 * Records can survive several unrelated submissions.  Such carried records
 * occupy a dense prefix; records created by this occurrence retain their
 * immutable call_offset+ordinal positions after that prefix.  Semantic call
 * offsets and the total byte span move by the same carried width.  Re-running
 * this function therefore produces identical geometry instead of collapsing
 * current records into unit zero.
 */
bool PrepareRecordFrontierLocked(DeviceState* state,
                                 ProtocolOccurrence* occurrence,
                                 bool route_all_records) {
    if (state == NULL || occurrence == NULL) return false;
    if (!occurrence->frontier_prepared) {
        occurrence->frontier_base_bytes = occurrence->bytes;
        occurrence->frontier_base_signature = occurrence->signature;
    }
    occurrence->bytes = occurrence->frontier_base_bytes;
    occurrence->signature = occurrence->frontier_base_signature;
    occurrence->frontier_prefix_bytes = 0;
    occurrence->route_all_records = route_all_records;
    occurrence->frontier_update_ids.clear();

    uint64_t rebuilt_bytes = 0;
    for (size_t i = 0; i < occurrence->calls.size(); ++i) {
        const uint64_t span = std::max<uint64_t>(
            occurrence->calls[i].encoded_bytes, 1);
        if (rebuilt_bytes > kMaxTemplateBytes - span) return false;
        occurrence->calls[i].template_offset = rebuilt_bytes;
        rebuilt_bytes += span;
    }
    if (!occurrence->calls.empty() &&
        rebuilt_bytes != occurrence->frontier_base_bytes) {
        return false;
    }

    std::map<uint64_t, uint32_t> roles;
    uint32_t next_role = 1;
    for (size_t i = 0; i < occurrence->calls.size(); ++i) {
        const SemanticCall& call = occurrence->calls[i];
        for (size_t field = 0; field < call.handles.size(); ++field) {
            const uint64_t handle = call.handles[field];
            if (handle != 0 && roles.find(handle) == roles.end()) {
                if (next_role == UINT32_MAX) return false;
                roles[handle] = next_role++;
            }
        }
    }

    std::vector<size_t> selected;
    std::vector<size_t> carried;
    std::vector<size_t> current;
    selected.reserve(state->records.size());
    carried.reserve(state->records.size());
    current.reserve(state->records.size());
    for (size_t i = 0; i < state->records.size(); ++i) {
        PendingRecord& record = state->records[i];
        if (record.elided) continue;
        const uint64_t set = HandleBits(record.set);
        if (!route_all_records &&
            occurrence->consumed_descriptor_sets.find(set) ==
                occurrence->consumed_descriptor_sets.end()) {
            continue;
        }
        if (record.update_id == 0 ||
            !occurrence->frontier_update_ids.insert(record.update_id).second ||
            selected.size() >= kMaxRouteRecords) {
            return false;
        }
        selected.push_back(i);
        if (!route_all_records &&
            record.source_occurrence_serial == state->occurrence_serial) {
            current.push_back(i);
        } else {
            carried.push_back(i);
        }
    }

    const auto by_update_id =
        [state](size_t left, size_t right) {
            return state->records[left].update_id <
                   state->records[right].update_id;
        };
    std::sort(carried.begin(), carried.end(), by_update_id);
    std::sort(current.begin(), current.end(),
              [state](size_t left, size_t right) {
                  const PendingRecord& a = state->records[left];
                  const PendingRecord& b = state->records[right];
                  if (a.source_template_offset != b.source_template_offset) {
                      return a.source_template_offset <
                             b.source_template_offset;
                  }
                  return a.update_id < b.update_id;
              });

    if (route_all_records) {
        std::sort(selected.begin(), selected.end(), by_update_id);
        for (size_t i = 0; i < selected.size(); ++i) {
            state->records[selected[i]].template_offset =
                static_cast<uint64_t>(i);
        }
        occurrence->bytes = std::max<uint64_t>(
            occurrence->frontier_base_bytes,
            std::max<uint64_t>(selected.size(), 1));
        occurrence->frontier_prepared = true;
        return true;
    }

    const uint64_t carried_bytes =
        static_cast<uint64_t>(carried.size());
    if (occurrence->frontier_base_bytes >
        kMaxTemplateBytes - carried_bytes) {
        return false;
    }
    for (size_t i = 0; i < carried.size(); ++i) {
        state->records[carried[i]].template_offset =
            static_cast<uint64_t>(i);
    }
    for (size_t i = 0; i < occurrence->calls.size(); ++i) {
        if (occurrence->calls[i].template_offset >
            UINT64_MAX - carried_bytes) {
            return false;
        }
        occurrence->calls[i].template_offset += carried_bytes;
    }
    std::set<uint64_t> final_offsets;
    for (size_t i = 0; i < carried.size(); ++i) {
        final_offsets.insert(state->records[carried[i]].template_offset);
    }
    for (size_t i = 0; i < current.size(); ++i) {
        PendingRecord& record = state->records[current[i]];
        if (record.source_template_offset >=
                occurrence->frontier_base_bytes ||
            record.source_template_offset >
                UINT64_MAX - carried_bytes) {
            return false;
        }
        record.template_offset =
            carried_bytes + record.source_template_offset;
        if (!final_offsets.insert(record.template_offset).second) {
            return false;
        }
    }
    occurrence->bytes = occurrence->frontier_base_bytes + carried_bytes;
    occurrence->frontier_prefix_bytes = carried_bytes;

    std::sort(selected.begin(), selected.end(),
              [state](size_t left, size_t right) {
                  const PendingRecord& a = state->records[left];
                  const PendingRecord& b = state->records[right];
                  if (a.template_offset != b.template_offset) {
                      return a.template_offset < b.template_offset;
                  }
                  return a.update_id < b.update_id;
              });
    uint64_t frontier_hash = UINT64_C(0x5459504532494949);
    for (size_t i = 0; i < selected.size(); ++i) {
        const PendingRecord& record = state->records[selected[i]];
        const uint64_t set = HandleBits(record.set);
        std::map<uint64_t, uint32_t>::const_iterator role = roles.find(set);
        if (role == roles.end()) return false;
        frontier_hash = ProtocolMix(frontier_hash, role->second);
        frontier_hash = ProtocolMix(frontier_hash, record.binding);
        frontier_hash = ProtocolMix(frontier_hash, record.array_element);
        frontier_hash = ProtocolMix(
            frontier_hash, static_cast<uint32_t>(record.type));
        frontier_hash = ProtocolMix(frontier_hash, record.flags);
    }
    occurrence->signature = ProtocolMix(
        occurrence->signature, UINT64_C(0x46524f4e54494552));
    occurrence->signature = ProtocolMix(
        occurrence->signature, selected.size());
    occurrence->signature = ProtocolMix(
        occurrence->signature, frontier_hash);
    occurrence->signature = ProtocolMix(
        occurrence->signature, occurrence->bytes);
    occurrence->frontier_prepared = true;
    return true;
}

void LearnTemplate(const ProtocolOccurrence& occurrence,
                   std::vector<TemplateCall>* learned) {
    learned->clear();
    learned->reserve(occurrence.calls.size());
    std::map<uint64_t, uint32_t> roles;
    uint32_t next_role = 1;
    for (size_t i = 0; i < occurrence.calls.size(); ++i) {
        const SemanticCall& source = occurrence.calls[i];
        TemplateCall call;
        call.fun_id = source.fun_id;
        call.structural = source.structural;
        call.encoded_bytes = source.encoded_bytes;
        call.template_offset = source.template_offset;
        call.dispatch = source.dispatch;
        call.execute_secondary = source.execute_secondary;
        for (size_t field = 0; field < source.handles.size(); ++field) {
            const uint64_t handle = source.handles[field];
            uint32_t role = 0;
            if (handle != 0) {
                std::map<uint64_t, uint32_t>::iterator known =
                    roles.find(handle);
                if (known == roles.end()) {
                    role = next_role++;
                    roles[handle] = role;
                } else {
                    role = known->second;
                }
            }
            call.handle_roles.push_back(role);
        }
        learned->push_back(call);
    }
}

bool MatchTemplate(const std::vector<TemplateCall>& learned,
                   const ProtocolOccurrence& occurrence) {
    if (learned.size() != occurrence.calls.size()) return false;
    std::map<uint32_t, uint64_t> role_to_handle;
    std::map<uint64_t, uint32_t> handle_to_role;
    for (size_t i = 0; i < learned.size(); ++i) {
        const TemplateCall& expected = learned[i];
        const SemanticCall& actual = occurrence.calls[i];
        if (expected.fun_id != actual.fun_id ||
            expected.structural != actual.structural ||
            expected.handle_roles.size() != actual.handles.size() ||
            expected.encoded_bytes != actual.encoded_bytes ||
            expected.template_offset != actual.template_offset ||
            expected.dispatch != actual.dispatch ||
            expected.execute_secondary != actual.execute_secondary) {
            return false;
        }
        for (size_t field = 0; field < actual.handles.size(); ++field) {
            const uint32_t role = expected.handle_roles[field];
            const uint64_t handle = actual.handles[field];
            if ((role == 0) != (handle == 0)) return false;
            if (role == 0) continue;
            std::map<uint32_t, uint64_t>::iterator by_role =
                role_to_handle.find(role);
            std::map<uint64_t, uint32_t>::iterator by_handle =
                handle_to_role.find(handle);
            if ((by_role != role_to_handle.end() &&
                 by_role->second != handle) ||
                (by_handle != handle_to_role.end() &&
                 by_handle->second != role)) {
                return false;
            }
            role_to_handle[role] = handle;
            handle_to_role[handle] = role;
        }
    }
    return true;
}

bool MatchTemplatePrefix(const std::vector<TemplateCall>& learned,
                         const std::vector<SemanticCall>& actual) {
    if (actual.size() > learned.size()) return false;
    std::map<uint32_t, uint64_t> role_to_handle;
    std::map<uint64_t, uint32_t> handle_to_role;
    for (size_t i = 0; i < actual.size(); ++i) {
        const TemplateCall& expected = learned[i];
        const SemanticCall& observed = actual[i];
        if (expected.fun_id != observed.fun_id ||
            expected.structural != observed.structural ||
            expected.handle_roles.size() != observed.handles.size() ||
            expected.encoded_bytes != observed.encoded_bytes ||
            expected.template_offset != observed.template_offset ||
            expected.dispatch != observed.dispatch ||
            expected.execute_secondary != observed.execute_secondary) {
            return false;
        }
        for (size_t field = 0; field < observed.handles.size(); ++field) {
            const uint32_t role = expected.handle_roles[field];
            const uint64_t handle = observed.handles[field];
            if ((role == 0) != (handle == 0)) return false;
            if (role == 0) continue;
            std::map<uint32_t, uint64_t>::iterator by_role =
                role_to_handle.find(role);
            std::map<uint64_t, uint32_t>::iterator by_handle =
                handle_to_role.find(handle);
            if ((by_role != role_to_handle.end() &&
                 by_role->second != handle) ||
                (by_handle != handle_to_role.end() &&
                 by_handle->second != role)) {
                return false;
            }
            role_to_handle[role] = handle;
            handle_to_role[handle] = role;
        }
    }
    return true;
}

size_t RepeatedSuffixPeriod(const std::vector<uint64_t>& history) {
    const size_t count = history.size();
    size_t best = 0;
    for (size_t begin = 0; begin + 1 < count; ++begin) {
        const size_t length = count - begin;
        std::vector<size_t> prefix(length, 0);
        for (size_t i = 1; i < length; ++i) {
            size_t matched = prefix[i - 1];
            while (matched != 0 &&
                   history[begin + i] != history[begin + matched]) {
                matched = prefix[matched - 1];
            }
            if (history[begin + i] == history[begin + matched]) ++matched;
            prefix[i] = matched;
        }
        const size_t period = length - prefix[length - 1];
        if (period != 0 && length >= 2 * period &&
            length % period == 0 && (best == 0 || period < best)) {
            best = period;
        }
    }
    return best;
}

bool SendIntervalLocked(DeviceState* state, uint64_t signature);

bool ObserveDetectLocked(DeviceState* state, uint64_t signature) {
    if (state == NULL) return false;
    state->interval_history.push_back(signature);
    if (state->interval_history.size() > kHistoryLimit) {
        state->interval_history.erase(state->interval_history.begin());
    }
    /*
     * The wire/core detector supports a bounded multi-interval period, but this
     * guest currently owns one phase template and one first-consuming submit
     * frontier.  Keep the explicit period-one safety restriction until phase
     * templates are represented; accepting p > 1 here would reuse the wrong
     * route on alternating submission shapes.
     */
    if (RepeatedSuffixPeriod(state->interval_history) != 1 ||
        state->interval_announced) {
        return true;
    }
    if (!SendIntervalLocked(state, signature) ||
        !SendIntervalLocked(state, signature)) {
        return false;
    }
    if (state->stage == kStageLegacy) return true;
    state->interval_announced = true;
    state->stage = kStageLearn;
    return true;
}

bool BuildUnits(const ProtocolOccurrence& occurrence,
                std::vector<UnitSample>* units) {
    if (units == NULL || occurrence.calls.empty() ||
        occurrence.dispatches == 0) {
        return false;
    }
    units->clear();
    uint64_t unit_begin = 0;
    uint64_t unit_prepare_ns = 0;
    uint32_t unit_dispatches = 0;
    uint32_t total_dispatches = 0;
    for (size_t i = 0; i < occurrence.calls.size(); ++i) {
        const SemanticCall& call = occurrence.calls[i];
        if (call.dispatch && unit_dispatches == kDispatchesPerUnit) {
            if (call.template_offset <= unit_begin ||
                units->size() >= kHardMaxUnits) {
                return false;
            }
            UnitSample unit = {};
            unit.index = static_cast<uint32_t>(units->size());
            unit.dispatch_end = total_dispatches;
            unit.template_offset = unit_begin;
            unit.encoded_bytes = call.template_offset - unit_begin;
            unit.prepare_ns = unit_prepare_ns;
            units->push_back(unit);
            unit_begin = call.template_offset;
            unit_prepare_ns = 0;
            unit_dispatches = 0;
        }
        if (unit_prepare_ns > UINT64_MAX - call.prepare_ns) {
            return false;
        }
        unit_prepare_ns += call.prepare_ns;
        if (call.dispatch) {
            ++unit_dispatches;
            ++total_dispatches;
        }
    }
    if (unit_dispatches == 0 || occurrence.bytes <= unit_begin ||
        units->size() >= kHardMaxUnits) {
        return false;
    }
    UnitSample final_unit = {};
    final_unit.index = static_cast<uint32_t>(units->size());
    final_unit.flags = kUnitFinal;
    final_unit.dispatch_end = total_dispatches;
    final_unit.template_offset = unit_begin;
    final_unit.encoded_bytes = occurrence.bytes - unit_begin;
    final_unit.prepare_ns = unit_prepare_ns;
    units->push_back(final_unit);
    return total_dispatches == occurrence.dispatches;
}

bool PlanMatchesUnits(const DeviceState& state,
                      const std::vector<UnitSample>& units) {
    if (!state.plan_valid || state.plan.empty() || units.empty()) return false;
    uint32_t previous_unit = 0;
    uint64_t previous_offset = 0;
    for (size_t i = 0; i < state.plan.size(); ++i) {
        const PlanBoundary& boundary = state.plan[i];
        if (boundary.unit_past_end <= previous_unit ||
            boundary.unit_past_end > units.size()) {
            return false;
        }
        const UnitSample& last = units[boundary.unit_past_end - 1];
        if (last.template_offset >
            UINT64_MAX - last.encoded_bytes) {
            return false;
        }
        const uint64_t expected =
            last.template_offset + last.encoded_bytes;
        if (boundary.template_offset != expected ||
            boundary.template_offset <= previous_offset) {
            return false;
        }
        previous_unit = boundary.unit_past_end;
        previous_offset = boundary.template_offset;
    }
    return previous_unit == units.size();
}

void BuildFineChunks(const std::vector<UnitSample>& units,
                     uint32_t maximum_chunks,
                     std::vector<ChunkSample>* chunks) {
    chunks->clear();
    if (units.empty() || maximum_chunks == 0) return;

    /*
     * Without an installed plan FLRD permits exactly one 0xB single-flush
     * route.  Fine profiling is still represented by the complete unit table;
     * its sole chunk spans that table and therefore matches routed geometry.
     */
    ChunkSample chunk = {};
    chunk.index = 0;
    chunk.first_unit = 0;
    chunk.unit_past_end = static_cast<uint32_t>(units.size());
    chunks->push_back(chunk);
}

bool BuildChunksLocked(DeviceState* state,
                       uint64_t period_id,
                       const std::vector<UnitSample>& units,
                       std::vector<ChunkSample>* chunks,
                       uint32_t* period_flags,
                       uint64_t* begin_ack_epoch,
                       uint64_t* active_plan_epoch) {
    if (state == NULL || chunks == NULL || period_flags == NULL ||
        begin_ack_epoch == NULL || active_plan_epoch == NULL ||
        units.empty() || units.size() > state->max_units ||
        state->max_chunks == 0) {
        return false;
    }
    chunks->clear();
    *begin_ack_epoch = 0;
    *active_plan_epoch = 0;
    /*
     * The host owns the replan cadence and publishes a request through the
     * seqlock control page.  A second guest-local modulo counter drifts during
     * initial Fast warmup and can profile the wrong periods.
     */
    const bool force_fine = state->request_fine_profile;
    const bool geometry_ok =
        state->stage == kStageFast && !force_fine &&
        PlanMatchesUnits(*state, units);
    const bool pending_applies =
        geometry_ok && state->plan_epoch != 0 &&
        state->plan_epoch != state->acked_plan_epoch &&
        state->plan_apply_period <= period_id;
    const bool already_active =
        geometry_ok && state->plan_epoch != 0 &&
        state->plan_epoch == state->acked_plan_epoch;
    const bool planned = pending_applies || already_active;
    if (planned) {
        uint32_t first = 0;
        if (pending_applies) *begin_ack_epoch = state->plan_epoch;
        *active_plan_epoch = state->plan_epoch;
        for (size_t i = 0; i < state->plan.size(); ++i) {
            ChunkSample chunk = {};
            chunk.index = static_cast<uint32_t>(i);
            chunk.first_unit = first;
            chunk.unit_past_end = state->plan[i].unit_past_end;
            chunks->push_back(chunk);
            first = chunk.unit_past_end;
        }
        *period_flags = kPeriodStableFast;
    } else {
        BuildFineChunks(units, state->max_chunks, chunks);
        *period_flags = kPeriodSingleFlush | kPeriodFineProfile;
        if (state->stage == kStageFast && state->plan_valid) {
            /* Host requires every Fast period with a published plan to carry
             * STABLE_FAST, including a fine-profile/single-flush replan. */
            *period_flags |= kPeriodStableFast;
        }
        if (force_fine && state->stage == kStageFast) {
            *period_flags |= kPeriodForceReplan;
        }
    }
    return !chunks->empty() && chunks->size() <= state->max_chunks;
}

bool SendIntervalLocked(DeviceState* state, uint64_t signature) {
    if (state == NULL) return false;
    BytePacket packet(kWireHeaderBytes + kIntervalBytes);
    InitializeWireHeader(&packet, kWireInterval, 0, 1, *state, 0, 0);
    packet.U64(kWireHeaderBytes, signature);
    return SendControlLocked(state, packet.data);
}

bool SendPeriodBeginLocked(DeviceState* state,
                           uint64_t period_id,
                           uint32_t period_flags,
                           uint64_t begin_ack_epoch) {
    if (state == NULL || period_id == 0) return false;
    BytePacket packet(kWireHeaderBytes);
    InitializeWireHeader(&packet, kWirePeriodBegin, period_flags, 0,
                         *state, period_id, begin_ack_epoch);
    if (!SendControlLocked(state, packet.data) ||
        state->stage == kStageLegacy) return false;
    if (begin_ack_epoch != 0) state->acked_plan_epoch = begin_ack_epoch;
    return true;
}

bool SendProgressLocked(DeviceState* state,
                        ProgressEvent event,
                        uint16_t flags,
                        uint32_t template_entries,
                        uint64_t completed_period) {
    if (state == NULL) return false;
    const bool template_event =
        event == kProgressLearnComplete ||
        event == kProgressMatchComplete ||
        event == kProgressFastComplete;
    if ((template_event &&
         (template_entries == 0 ||
          template_entries > kMaxSemanticCalls)) ||
        (!template_event && template_entries != 0)) {
        return false;
    }
    if (event != kProgressFastComplete) completed_period = 0;
    BytePacket packet(kWireHeaderBytes + kProgressBytes);
    InitializeWireHeader(&packet, kWireProgress, 0, 1, *state,
                         completed_period, 0);
    const size_t base = kWireHeaderBytes;
    packet.U16(base, static_cast<uint16_t>(event));
    packet.U16(base + 2, flags);
    packet.U32(base + 4, template_entries);
    packet.U64(base + 8, completed_period);
    return SendControlLocked(state, packet.data) &&
           state->stage != kStageLegacy;
}

bool SendProfileLocked(DeviceState* state,
                       uint64_t period_id,
                       uint32_t period_flags,
                       const std::vector<UnitSample>& units,
                       const std::vector<ChunkSample>& chunks) {
    if (state == NULL || period_id == 0 || units.empty() || chunks.empty() ||
        units.size() > state->max_units ||
        chunks.size() > state->max_chunks ||
        units.size() + chunks.size() > UINT32_MAX) {
        return false;
    }
    const size_t fixed = kWireHeaderBytes + kProfileBytes;
    if (units.size() > (SIZE_MAX - fixed) / kUnitBytes) return false;
    size_t bytes = fixed + units.size() * kUnitBytes;
    if (chunks.size() > (SIZE_MAX - bytes) / kChunkBytes) return false;
    bytes += chunks.size() * kChunkBytes;
    BytePacket packet(bytes);
    InitializeWireHeader(
        &packet, kWireProfilePeriod, 0,
        static_cast<uint32_t>(units.size() + chunks.size()), *state,
        period_id, 0);
    const size_t base = kWireHeaderBytes;
    const uint64_t finished_ns = NowNs();
    const uint64_t elapsed_ns =
        finished_ns >= state->period_start_ns
            ? finished_ns - state->period_start_ns : 0;
    packet.U32(base, static_cast<uint32_t>(units.size()));
    packet.U32(base + 4, static_cast<uint32_t>(chunks.size()));
    packet.U32(base + 8, period_flags);
    packet.U64(base + 16, elapsed_ns);

    size_t cursor = base + kProfileBytes;
    for (size_t i = 0; i < units.size(); ++i) {
        const UnitSample& unit = units[i];
        packet.U32(cursor, unit.index);
        packet.U32(cursor + 4, unit.flags);
        packet.U32(cursor + 8, unit.dispatch_end);
        packet.U64(cursor + 16, unit.template_offset);
        packet.U64(cursor + 24, unit.encoded_bytes);
        packet.U64(cursor + 32, unit.prepare_ns);
        cursor += kUnitBytes;
    }
    for (size_t i = 0; i < chunks.size(); ++i) {
        const ChunkSample& chunk = chunks[i];
        packet.U32(cursor, chunk.index);
        packet.U32(cursor + 4, chunk.first_unit);
        packet.U32(cursor + 8, chunk.unit_past_end);
        packet.U64(cursor + 16, chunk.handoff_ns);
        cursor += kChunkBytes;
    }
    return cursor == bytes && SendControlLocked(state, packet.data) &&
           state->stage != kStageLegacy;
}

bool NegotiateCapabilitiesLocked(DeviceState* state) {
    if (state == NULL || state->control_page == NULL ||
        state->process_id == 0 || state->stream_id == 0) {
        return false;
    }
    BytePacket packet(kWireHeaderBytes + kCapsBytes);
    InitializeWireHeader(&packet, kWireCapabilities, 0, 1,
                         *state, 0, 0);
    const size_t base = kWireHeaderBytes;
    packet.U16(base, kProtocolMajor);
    packet.U16(base + 2, kProtocolMinor);
    packet.U16(base + 4, kProtocolMajor);
    packet.U16(base + 6, kProtocolMinor);
    packet.U64(base + 8, kAllCapabilities);
    packet.U32(base + 16, kHardMaxUnits);
    packet.U32(base + 20, kHardMaxChunks);
    packet.U32(base + 24, kDispatchesPerUnit);
    packet.U32(base + 28, kReplanPeriods);
    if (!SendControlLocked(state, packet.data)) return false;
    if (state->stage == kStageLegacy ||
        !HasRequiredCapabilities(*state)) {
        return false;
    }
    state->negotiated = true;
    state->stage = kStageDetect;
    state->interval_announced = false;
    state->session_invalidated = false;
    return true;
}

bool SortAndValidateRecords(
        const DeviceState& state,
        const ProtocolOccurrence& occurrence,
        bool recovery,
        std::vector<size_t>* indices) {
    if (indices == NULL) return false;
    (void)recovery;
    indices->clear();
    if (occurrence.route_all_records &&
        occurrence.frontier_update_ids.size() != LiveRecordCount(state)) {
        return false;
    }
    std::set<uint64_t> update_ids;
    std::set<uint64_t> offsets;
    for (size_t i = 0; i < state.records.size(); ++i) {
        const PendingRecord& record = state.records[i];
        /*
         * A partial planned route rebuilds the complete grouping when FINAL
         * is emitted.  Keep already released prefix records in that grouping;
         * the caller selects only the unsent chunk range.  Recovery likewise
         * needs the full logical sequence so the host ledger can suppress the
         * prefix it has already realized.
         */
        if (record.elided) continue;
        if (occurrence.frontier_update_ids.find(record.update_id) ==
            occurrence.frontier_update_ids.end()) {
            continue;
        }
        if (record.update_id == 0 ||
            record.template_offset >= occurrence.bytes ||
            !record.value.valid ||
            (record.flags & ~kRouteDerived) != 0 ||
            !update_ids.insert(record.update_id).second ||
            !offsets.insert(record.template_offset).second) {
            return false;
        }
        indices->push_back(i);
    }
    std::sort(indices->begin(), indices->end(),
              [&state](size_t left, size_t right) {
                  const PendingRecord& a = state.records[left];
                  const PendingRecord& b = state.records[right];
                  if (a.template_offset != b.template_offset) {
                      return a.template_offset < b.template_offset;
                  }
                  return a.update_id < b.update_id;
              });
    return indices->size() == occurrence.frontier_update_ids.size() &&
           indices->size() <= kMaxRouteRecords;
}

bool BuildRecordGroups(
        const DeviceState& state,
        const ProtocolOccurrence& occurrence,
        const std::vector<UnitSample>& units,
        const std::vector<ChunkSample>& chunks,
        bool recovery,
        std::vector<RoutedRecordGroup>* groups,
        uint32_t* submission_record_count) {
    if (groups == NULL || submission_record_count == NULL) return false;
    std::vector<size_t> records;
    if (!SortAndValidateRecords(state, occurrence, recovery, &records) ||
        records.size() > UINT32_MAX) {
        return false;
    }
    *submission_record_count = static_cast<uint32_t>(records.size());
    groups->clear();
    if (recovery) {
        RoutedRecordGroup group;
        group.template_end = occurrence.bytes;
        group.record_indices.swap(records);
        groups->push_back(group);
        return true;
    }
    size_t record_cursor = 0;
    for (size_t i = 0; i < chunks.size(); ++i) {
        const ChunkSample& chunk = chunks[i];
        if (chunk.first_unit >= chunk.unit_past_end ||
            chunk.unit_past_end > units.size()) {
            return false;
        }
        RoutedRecordGroup group;
        group.template_begin = units[chunk.first_unit].template_offset;
        const UnitSample& last = units[chunk.unit_past_end - 1];
        if (last.template_offset > UINT64_MAX - last.encoded_bytes) {
            return false;
        }
        group.template_end = last.template_offset + last.encoded_bytes;
        while (record_cursor < records.size()) {
            const uint64_t offset =
                state.records[records[record_cursor]].template_offset;
            if (offset >= group.template_end) break;
            if (offset < group.template_begin) return false;
            group.record_indices.push_back(records[record_cursor++]);
        }
        groups->push_back(group);
    }
    return record_cursor == records.size() && !groups->empty();
}

bool RoutePayloadShape(const PendingRecord& record,
                       uint16_t* kind,
                       uint32_t* bytes) {
    if (kind == NULL || bytes == NULL || !record.value.valid ||
        record.value.type != record.type) {
        return false;
    }
    if (record.value.kind == kPayloadBuffer &&
        FlimeDescriptorTypeIsBuffer(record.type)) {
        *kind = kPayloadBuffer;
        *bytes = 24;
        return true;
    }
    if (record.value.kind == kPayloadImage &&
        FlimeDescriptorTypeIsImage(record.type)) {
        *kind = kPayloadImage;
        *bytes = 24;
        return true;
    }
    if (record.value.kind == kPayloadTexel &&
        FlimeDescriptorTypeIsTexel(record.type)) {
        *kind = kPayloadTexel;
        *bytes = 8;
        return true;
    }
    return false;
}

bool BuildRoutePacket(
        const DeviceState& state,
        uint64_t period_id,
        uint64_t plan_epoch,
        uint64_t submission_id,
        uint16_t route_flags,
        uint32_t chunk_index,
        uint32_t chunk_count,
        uint32_t first_unit,
        uint32_t unit_past_end,
        const RoutedRecordGroup& group,
        uint32_t submission_record_count,
        std::vector<uint8_t>* wire) {
    if (wire == NULL || period_id == 0 || submission_id == 0 ||
        chunk_count == 0 || chunk_index >= chunk_count ||
        group.record_indices.size() > UINT32_MAX) {
        return false;
    }
    size_t bytes = kRouteHeaderBytes;
    std::vector<uint32_t> payload_sizes;
    std::vector<uint16_t> payload_kinds;
    payload_sizes.reserve(group.record_indices.size());
    payload_kinds.reserve(group.record_indices.size());
    for (size_t i = 0; i < group.record_indices.size(); ++i) {
        const PendingRecord& record =
            state.records[group.record_indices[i]];
        uint16_t kind = 0;
        uint32_t payload = 0;
        if ((record.flags & ~kRouteDerived) != 0 ||
            !RoutePayloadShape(record, &kind, &payload) ||
            bytes > kMaxRoutePacketBytes - kRouteRecordBytes - payload) {
            return false;
        }
        bytes += kRouteRecordBytes + payload;
        payload_kinds.push_back(kind);
        payload_sizes.push_back(payload);
    }
    if (bytes > kMaxRoutePacketBytes || bytes > UINT32_MAX) return false;
    BytePacket packet(bytes);
    packet.U32(0, kRouteMagic);
    packet.U16(4, kProtocolMajor);
    packet.U16(6, kProtocolMinor);
    packet.U16(8, static_cast<uint16_t>(kRouteHeaderBytes));
    packet.U16(10, route_flags);
    packet.U32(12, static_cast<uint32_t>(bytes));
    packet.U32(16, static_cast<uint32_t>(group.record_indices.size()));
    packet.U32(20, submission_record_count);
    packet.U32(24, static_cast<uint32_t>(bytes - kRouteHeaderBytes));
    packet.U32(28, chunk_index);
    packet.U64(32, state.process_id);
    packet.U64(40, state.stream_id);
    packet.U64(48, period_id);
    packet.U64(56, plan_epoch);
    packet.U64(64, submission_id);
    packet.U64(72, HandleBits(state.device));
    packet.U64(80, HandleBits(state.queue));
    packet.U32(88, first_unit);
    packet.U32(92, unit_past_end);
    packet.U64(96, group.template_end);

    size_t cursor = kRouteHeaderBytes;
    for (size_t i = 0; i < group.record_indices.size(); ++i) {
        const PendingRecord& record =
            state.records[group.record_indices[i]];
        const uint32_t payload = payload_sizes[i];
        packet.U64(cursor, record.update_id);
        packet.U64(cursor + 8, record.template_offset);
        packet.U64(cursor + 16, HandleBits(record.set));
        packet.U32(cursor + 24, record.binding);
        packet.U32(cursor + 28, record.array_element);
        packet.U32(cursor + 32, 1);
        packet.U32(cursor + 36, static_cast<uint32_t>(record.type));
        packet.U16(cursor + 40, payload_kinds[i]);
        packet.U16(cursor + 42, record.flags);
        packet.U32(cursor + 44,
                   static_cast<uint32_t>(kRouteRecordBytes + payload));
        packet.U32(cursor + 48, payload);
        cursor += kRouteRecordBytes;
        if (payload_kinds[i] == kPayloadBuffer) {
            packet.U64(cursor, HandleBits(record.value.buffer));
            packet.U64(cursor + 8, record.value.offset);
            packet.U64(cursor + 16, record.value.range);
        } else if (payload_kinds[i] == kPayloadImage) {
            packet.U64(cursor, HandleBits(record.value.sampler));
            packet.U64(cursor + 8, HandleBits(record.value.image_view));
            packet.U32(cursor + 16,
                       static_cast<uint32_t>(record.value.image_layout));
        } else {
            packet.U64(cursor, HandleBits(record.value.buffer_view));
        }
        cursor += payload;
    }
    if (cursor != bytes) return false;
    wire->swap(packet.data);
    return true;
}

RouteReplyDisposition ValidateRouteReply(
        const uint8_t* reply,
        const DeviceState& state,
        uint64_t submission_id,
        uint32_t expected_records,
        bool recovery,
        uint64_t* host_service_ns) {
    if (host_service_ns != NULL) *host_service_ns = 0;
    if (reply == NULL ||
        GetLe32(reply) != kRouteReplyMagic ||
        GetLe16(reply + 4) != kProtocolMajor ||
        GetLe16(reply + 6) > kProtocolMinor ||
        GetLe16(reply + 8) != kRouteReplyBytes ||
        GetLe32(reply + 12) != kRouteReplyBytes ||
        GetLe32(reply + 28) != 0 ||
        GetLe64(reply + 32) != state.process_id ||
        GetLe64(reply + 40) != state.stream_id ||
        GetLe64(reply + 48) != submission_id ||
        (GetLe16(reply + 6) == 0 && GetLe64(reply + 56) != 0)) {
        return kRouteReplyInvalid;
    }
    const uint16_t status = GetLe16(reply + 10);
    const uint32_t flags = GetLe32(reply + 16);
    if ((flags & ~kRouteReplyKnownFlags) != 0) {
        return kRouteReplyInvalid;
    }
    if (status != 0 ||
        (flags & (kRouteFallbackRequired |
                  kRouteRecoveryRequired)) != 0) {
        return kRouteReplyNeedsRecovery;
    }
    const uint32_t accepted = GetLe32(reply + 20);
    const uint32_t deferred = GetLe32(reply + 24);
    const uint32_t duplicate_flag = 1u << 1;
    const uint32_t deferred_flag = 1u << 2;
    const uint32_t recovery_flag = 1u << 3;
    if (accepted != expected_records || deferred > accepted ||
        (flags & deferred_flag) == 0 ||
        ((flags & recovery_flag) != 0) != recovery ||
        (!recovery && (flags & duplicate_flag) != 0)) {
        return kRouteReplyInvalid;
    }
    if (host_service_ns != NULL && GetLe16(reply + 6) >= 1) {
        *host_service_ns = GetLe64(reply + 56);
    }
    return kRouteReplyAccepted;
}

RouteReplyDisposition SendRoutePacketLocked(
        DeviceState* state,
        const std::vector<uint8_t>& packet,
        uint64_t period_id,
        uint64_t submission_id,
        uint32_t expected_records,
        bool recovery,
        uint64_t* host_service_ns) {
    if (state == NULL || packet.empty() ||
        packet.size() > kMaxRoutePacketBytes ||
        !FlushAnyClusterLocked()) {
        return kRouteReplyInvalid;
    }
    uint8_t reply[kRouteReplyBytes] = {};
    ParamManager manager;
    manager.addPtr(const_cast<uint8_t*>(&packet[0]),
                   static_cast<int>(packet.size()));
    manager.addPtr(reply, static_cast<int>(sizeof(reply)));
    if (manager.write(state->fd, EXPRESS_GPU_DEVICE_ID,
                      FUNID_vkExpressFlimeRoutedDescriptorUpdatesANDROID,
                      true) != 48) {
        return kRouteReplyInvalid;
    }
    return ValidateRouteReply(reply, *state, submission_id,
                              expected_records, recovery,
                              host_service_ns);
}

bool ValidateFallbackReply(const uint8_t* reply,
                           const DeviceState& state,
                           uint64_t submission_id,
                           uint32_t expected_records) {
    if (reply == NULL ||
        GetLe32(reply) != kRouteReplyMagic ||
        GetLe16(reply + 4) != kProtocolMajor ||
        GetLe16(reply + 6) > kProtocolMinor ||
        GetLe16(reply + 8) != kRouteReplyBytes ||
        GetLe16(reply + 10) != 0 ||
        GetLe32(reply + 12) != kRouteReplyBytes ||
        GetLe32(reply + 16) != kRouteFallbackDrained ||
        GetLe32(reply + 20) != expected_records ||
        GetLe32(reply + 24) > expected_records ||
        GetLe32(reply + 28) != 0 ||
        GetLe64(reply + 32) != state.process_id ||
        GetLe64(reply + 40) != state.stream_id ||
        GetLe64(reply + 48) != submission_id ||
        (GetLe16(reply + 6) == 0 && GetLe64(reply + 56) != 0)) {
        return false;
    }
    return true;
}

/*
 * Leave specialized mode without reordering a generic RPC behind descriptor
 * writes that FLIME already suppressed.  The host filters emitted update ids
 * through its ledger, applies only the not-yet-released suffix, removes the
 * session/control sink, and sets FALLBACK_DRAINED before this call returns.
 */
bool FallbackToLegacyLocked(DeviceState* state,
                            uint64_t* drain_submission_id) {
    if (drain_submission_id != NULL) *drain_submission_id = 0;
    if (state == NULL || !IsSpecialized(*state) || state->active_submit ||
        state->next_period_id == 0 ||
        state->next_period_id == UINT64_MAX ||
        state->next_submission_id == 0 ||
        state->next_submission_id == UINT64_MAX ||
        !FlushAnyClusterLocked()) {
        return false;
    }

    ProtocolOccurrence occurrence;
    occurrence.bytes = std::max<uint64_t>(state->template_bytes, 1);
    RoutedRecordGroup group;
    if (!PrepareRecordFrontierLocked(state, &occurrence, true) ||
        !SortAndValidateRecords(
            *state, occurrence, false, &group.record_indices) ||
        group.record_indices.size() > UINT32_MAX) {
        return false;
    }
    group.template_begin = 0;
    group.template_end = occurrence.bytes;

    const uint64_t period_id = state->next_period_id++;
    const uint64_t submission_id = state->next_submission_id++;
    const uint32_t record_count =
        static_cast<uint32_t>(group.record_indices.size());
    std::vector<uint8_t> packet;
    if (!BuildRoutePacket(
            *state, period_id, 0, submission_id,
            kRouteBegin | kRouteFinal | kRouteFallbackFlush,
            0, 1, 0, 1, group, record_count, &packet)) {
        return false;
    }

    uint8_t reply[kRouteReplyBytes] = {};
    ParamManager manager;
    manager.addPtr(&packet[0], static_cast<int>(packet.size()));
    manager.addPtr(reply, static_cast<int>(sizeof(reply)));
    if (manager.write(
            state->fd, EXPRESS_GPU_DEVICE_ID,
            FUNID_vkExpressFlimeRoutedDescriptorUpdatesANDROID,
            true) != 48 ||
        !ValidateFallbackReply(
            reply, *state, submission_id, record_count)) {
        return false;
    }

    state->teardown_complete = true;
    state->control_page_exposed = false;
    EnterLegacy(state);
    state->transport_failed = false;
    if (drain_submission_id != NULL) {
        *drain_submission_id = submission_id;
    }
    return true;
}

bool PrepareFallbackLegacySubmitLocked(DeviceState* state,
                                       uint64_t* token) {
    uint64_t drain_submission_id = 0;
    if (token == NULL ||
        !FallbackToLegacyLocked(state, &drain_submission_id) ||
        drain_submission_id == 0) {
        if (state != NULL) state->transport_failed = true;
        return false;
    }
    state->active_submit = true;
    state->active_token = drain_submission_id;
    state->active_submission_id = drain_submission_id;
    *token = drain_submission_id;
    return true;
}

RouteReplyDisposition SendRoutedSubmissionRangeLocked(
        DeviceState* state,
        uint64_t period_id,
        uint64_t plan_epoch,
        uint64_t submission_id,
        const ProtocolOccurrence& occurrence,
        const std::vector<UnitSample>& units,
        std::vector<ChunkSample>* chunks,
        bool recovery,
        uint32_t first_chunk,
        uint32_t chunk_past_end) {
    if (state == NULL || HandleBits(state->queue) == 0) {
        return kRouteReplyInvalid;
    }
    if (chunks == NULL) return kRouteReplyInvalid;
    std::vector<RoutedRecordGroup> groups;
    uint32_t submission_records = 0;
    if (!BuildRecordGroups(*state, occurrence, units, *chunks, recovery,
                           &groups, &submission_records) ||
        groups.size() > UINT32_MAX ||
        first_chunk >= chunk_past_end ||
        chunk_past_end > groups.size() ||
        (recovery && (first_chunk != 0 || chunk_past_end != 1))) {
        return kRouteReplyInvalid;
    }
    const uint32_t chunk_count = static_cast<uint32_t>(groups.size());
    for (size_t i = first_chunk; i < chunk_past_end; ++i) {
        uint16_t route_flags = 0;
        uint32_t first_unit = 0;
        uint32_t unit_past_end = static_cast<uint32_t>(units.size());
        if (recovery) {
            if (groups.size() != 1) return kRouteReplyInvalid;
            route_flags = kRouteBegin | kRouteFinal | kRouteRecovery;
        } else {
            first_unit = (*chunks)[i].first_unit;
            unit_past_end = (*chunks)[i].unit_past_end;
            if (i == 0) route_flags |= kRouteBegin;
            if (i + 1 == groups.size()) route_flags |= kRouteFinal;
            if (groups.size() == 1 && plan_epoch == 0) {
                route_flags |= kRouteSingle;
            }
        }
        std::vector<uint8_t> packet;
        if (!BuildRoutePacket(
                *state, period_id, plan_epoch, submission_id, route_flags,
                static_cast<uint32_t>(i), chunk_count, first_unit,
                unit_past_end, groups[i], submission_records, &packet)) {
            return kRouteReplyInvalid;
        }
        const uint64_t handoff_start_ns = NowNs();
        uint64_t host_service_ns = 0;
        const RouteReplyDisposition disposition = SendRoutePacketLocked(
            state, packet, period_id, submission_id,
            static_cast<uint32_t>(groups[i].record_indices.size()), recovery,
            &host_service_ns);
        const uint64_t handoff_end_ns = NowNs();
        const uint64_t round_trip_ns =
            handoff_end_ns >= handoff_start_ns
                ? handoff_end_ns - handoff_start_ns : 0;
        /*
         * Protocol 1 reports the host admission/native-realization slice.
         * Subtracting it avoids charging the same host work once as guest-host
         * handoff and again in the host-side downstream estimator.  Minor 0
         * peers report zero and retain the old conservative measurement.
         */
        (*chunks)[i].handoff_ns =
            round_trip_ns - std::min<uint64_t>(
                round_trip_ns, host_service_ns);
        if (disposition != kRouteReplyAccepted) return disposition;
        for (size_t record = 0;
             record < groups[i].record_indices.size(); ++record) {
            if (!FlimeShadowMarkRecordReleased(
                    state, groups[i].record_indices[record])) {
                return kRouteReplyInvalid;
            }
        }
    }
    return kRouteReplyAccepted;
}

RouteReplyDisposition SendRoutedSubmissionLocked(
        DeviceState* state,
        uint64_t period_id,
        uint64_t plan_epoch,
        uint64_t submission_id,
        const ProtocolOccurrence& occurrence,
        const std::vector<UnitSample>& units,
        std::vector<ChunkSample>* chunks,
        bool recovery) {
    if (chunks == NULL || chunks->empty() || chunks->size() > UINT32_MAX) {
        return kRouteReplyInvalid;
    }
    return SendRoutedSubmissionRangeLocked(
        state, period_id, plan_epoch, submission_id, occurrence, units,
        chunks, recovery, 0,
        recovery ? 1u : static_cast<uint32_t>(chunks->size()));
}

bool DescriptorCacheCompleteForSubmit(const DeviceState& state) {
    if (state.stage == kStageLearn) {
        return state.building_descriptor_cache_complete &&
               state.descriptor_plan_cursor ==
                   state.building_descriptor_plans.size();
    }
    if (state.stage == kStageMatch) {
        return state.building_descriptor_cache_complete &&
               state.learned_descriptor_cache_complete &&
               state.descriptor_plan_cursor ==
                   state.learned_descriptor_plans.size() &&
               state.building_descriptor_plans.size() ==
                   state.learned_descriptor_plans.size();
    }
    if (state.stage == kStageFast) {
        return state.building_descriptor_cache_complete &&
               state.learned_descriptor_cache_complete &&
               state.descriptor_plan_cursor ==
                   state.learned_descriptor_plans.size();
    }
    return true;
}

void SaveActiveOccurrence(DeviceState* state,
                           const ProtocolOccurrence& occurrence,
                           const std::vector<VkCommandBuffer>& submitted,
                           uint64_t period_id,
                          uint64_t submission_id,
                          uint32_t period_flags,
                          uint64_t plan_epoch,
                          bool learning,
                          bool fast,
                          bool recovery) {
    state->active_submit = true;
    state->active_recovery = recovery;
    state->active_learning = learning;
    state->active_fast = fast;
    state->active_token = submission_id;
    state->active_period_id = period_id;
    state->active_submission_id = submission_id;
    state->active_period_flags = period_flags;
    state->active_plan_epoch = plan_epoch;
    state->active_chunk_count = recovery ? 1u :
        static_cast<uint32_t>(state->chunks.size());
    state->active_template_bytes = occurrence.bytes;
    state->active_signature = occurrence.signature;
    state->active_dispatches = occurrence.dispatches;
    state->active_calls = occurrence.calls;
    state->active_primary_commands.clear();
    state->active_primary_commands.reserve(submitted.size());
    for (size_t i = 0; i < submitted.size(); ++i) {
        state->active_primary_commands.push_back(
            HandleBits(submitted[i]));
    }
    state->active_descriptor_plans =
        state->building_descriptor_plans;
    state->active_descriptor_cache_complete =
        DescriptorCacheCompleteForSubmit(*state) &&
        FlimeFastAssignOccurrenceSetRoles(
            &state->active_descriptor_plans);
    state->active_generic_shadow_ran = recovery
        ? FlimeShadowVerifyRecoveryCheckpoint(
            state, occurrence.consumed_descriptor_sets,
            occurrence.frontier_update_ids, true)
        /*
         * Learn and Match always take the authoritative generic path.  An
         * empty descriptor stream is a successful no-op shadow pass, not a
         * reason to omit the canonical GENERIC_SHADOW_RAN progress flag.
         * Fast alone may report false because it uses the compiled plan.
         */
        : (!fast || state->generic_shadow_ran);
    state->active_consumed_descriptor_sets =
        occurrence.consumed_descriptor_sets;
    state->active_frontier_update_ids =
        occurrence.frontier_update_ids;
}

void ClearEarlyRoute(DeviceState* state) {
    if (state == NULL) return;
    state->early_route_ready = false;
    state->early_recovery = false;
    state->early_period_id = 0;
    state->early_submission_id = 0;
    state->early_period_flags = 0;
    state->early_plan_epoch = 0;
    state->early_start_ns = 0;
    state->early_next_chunk = 0;
    state->early_command_buffer = 0;
    state->early_queue = 0;
    state->early_signature = 0;
    state->early_template_bytes = 0;
    state->early_dispatches = 0;
    state->early_descriptor_plan_cursor = 0;
    state->early_calls.clear();
    state->early_command_graph.clear();
    state->early_consumed_descriptor_sets.clear();
    state->early_frontier_update_ids.clear();
    state->early_units.clear();
    state->early_chunks.clear();
}

bool SameUnitGeometry(const std::vector<UnitSample>& left,
                      const std::vector<UnitSample>& right) {
    if (left.size() != right.size()) return false;
    for (size_t i = 0; i < left.size(); ++i) {
        if (left[i].index != right[i].index ||
            left[i].flags != right[i].flags ||
            left[i].dispatch_end != right[i].dispatch_end ||
            left[i].template_offset != right[i].template_offset ||
            left[i].encoded_bytes != right[i].encoded_bytes) {
            return false;
        }
    }
    return true;
}

bool SameChunkGeometry(const std::vector<ChunkSample>& left,
                       const std::vector<ChunkSample>& right) {
    if (left.size() != right.size()) return false;
    for (size_t i = 0; i < left.size(); ++i) {
        if (left[i].index != right[i].index ||
            left[i].first_unit != right[i].first_unit ||
            left[i].unit_past_end != right[i].unit_past_end) {
            return false;
        }
    }
    return true;
}

bool SameSemanticCalls(const std::vector<SemanticCall>& left,
                       const std::vector<SemanticCall>& right) {
    if (left.size() != right.size()) return false;
    for (size_t i = 0; i < left.size(); ++i) {
        const SemanticCall& a = left[i];
        const SemanticCall& b = right[i];
        if (a.fun_id != b.fun_id || a.structural != b.structural ||
            a.handles != b.handles || a.payload_hash != b.payload_hash ||
            a.encoded_bytes != b.encoded_bytes ||
            a.template_offset != b.template_offset ||
            a.dispatch != b.dispatch ||
            a.execute_secondary != b.execute_secondary ||
            a.opaque != b.opaque ||
            a.secondary_commands != b.secondary_commands) {
            return false;
        }
    }
    return true;
}

bool SnapshotCommandGraphNodeLocked(
        const DeviceState& state,
        uint64_t command_key,
        std::set<uint64_t>* recursion_stack,
        unsigned depth,
        std::vector<std::pair<uint64_t, uint64_t> >* graph) {
    if (command_key == 0 || recursion_stack == NULL || graph == NULL ||
        depth > 64 || graph->size() >= kMaxSemanticCalls ||
        !recursion_stack->insert(command_key).second) {
        return false;
    }
    std::unordered_map<uint64_t, CommandState>::const_iterator command =
        state.commands.find(command_key);
    std::unordered_map<uint64_t, RecordedCommandStream>::const_iterator stream =
        g_recorded_commands.find(command_key);
    if (command == state.commands.end() || command->second.recording ||
        !command->second.executable ||
        stream == g_recorded_commands.end() ||
        stream->second.generation != command->second.generation) {
        recursion_stack->erase(command_key);
        return false;
    }
    graph->push_back(std::make_pair(
        command_key, command->second.generation));
    for (size_t i = 0; i < stream->second.calls.size(); ++i) {
        const SemanticCall& call = stream->second.calls[i];
        if (!call.execute_secondary) continue;
        for (size_t child = 0; child < call.secondary_commands.size();
             ++child) {
            if (!SnapshotCommandGraphNodeLocked(
                    state, call.secondary_commands[child],
                    recursion_stack, depth + 1, graph)) {
                recursion_stack->erase(command_key);
                return false;
            }
        }
    }
    recursion_stack->erase(command_key);
    return true;
}

bool SnapshotSubmittedCommandGraphLocked(
        const DeviceState& state,
        const std::vector<VkCommandBuffer>& submitted,
        std::vector<std::pair<uint64_t, uint64_t> >* graph) {
    if (graph == NULL || submitted.empty()) return false;
    graph->clear();
    std::set<uint64_t> recursion_stack;
    for (size_t i = 0; i < submitted.size(); ++i) {
        if (!SnapshotCommandGraphNodeLocked(
                state, HandleBits(submitted[i]), &recursion_stack, 0,
                graph)) {
            graph->clear();
            return false;
        }
    }
    return !graph->empty();
}

bool MaybeAdvanceFastRouteLocked(DeviceState* state,
                                 uint64_t command_key);

bool MaybeRouteFastPrefixLocked(DeviceState* state) {
    if (state == NULL) return false;
    if (state->stage != kStageFast || state->transport_failed ||
        state->active_submit || state->early_route_ready) {
        return !state->transport_failed;
    }
    if (state->calls.size() > state->learned_global_call_count) {
        state->mismatch_pending = true;
        return true;
    }
    if (state->calls.size() == state->learned_global_call_count) {
        if (DescriptorCacheCompleteForSubmit(*state) &&
            state->descriptor_plan_cursor ==
                state->learned_descriptor_plans.size() &&
            state->learned_primary_commands.size() == 1) {
            /*
             * Record-once workloads do not call EndCommandBuffer every period.
             * The last expected descriptor call is therefore the earliest
             * point at which both global-call and compiled-plan cursors are
             * complete; reuse the learned primary to prove the full graph.
             *
             * Do this full proof before the cheap prefix-only test.  Carried
             * records shift learned call offsets during frontier preparation,
             * while state->calls still contains immutable occurrence-local
             * offsets; MaybeAdvance performs the correctly rebased comparison.
             */
            return MaybeAdvanceFastRouteLocked(
                state, state->learned_primary_commands[0]);
        }
        if (!MatchTemplatePrefix(state->learned, state->calls)) {
            /*
             * Do not fail the Vulkan call.  No route has been emitted yet, so
             * the complete occurrence can enter recovery at QueueSubmit.
             */
            state->mismatch_pending = true;
        }
    }
    return true;
}

bool IsFastPlanningBoundaryLocked(DeviceState* state,
                                  VkCommandBuffer command_buffer,
                                  uint32_t dispatch_count) {
    (void)state;
    (void)command_buffer;
    (void)dispatch_count;
    /*
     * A dispatch by itself does not prove the primary/secondary graph or the
     * descriptor-consumer frontier.  Keep Type-I clustering active here;
     * EndCommandBuffer and the record-once descriptor-cursor boundary invoke
     * MaybeAdvance only after they can reconstruct and validate the full graph.
     */
    return false;
}

bool MaybeAdvanceFastRouteLocked(DeviceState* state,
                                 uint64_t command_key) {
    if (state == NULL || state->transport_failed) return false;
    if (state->early_route_ready) {
        if (state->early_command_buffer == command_key) return true;
        if (!FallbackToLegacyLocked(state, NULL)) {
            state->transport_failed = true;
            return false;
        }
        return true;
    }
    if (state->stage != kStageFast || !state->negotiated ||
        !HasRequiredCapabilities(*state) || state->active_submit ||
        state->mismatch_pending || command_key == 0 ||
        state->registered_queues.size() != 1 ||
        HandleBits(state->queue) == 0 ||
        *state->registered_queues.begin() != HandleBits(state->queue) ||
        state->learned_primary_commands.size() != 1 ||
        state->learned_primary_commands[0] != command_key ||
        (state->learned_submit_call.fun_id != FUNID_vkQueueSubmit &&
         state->learned_submit_call.fun_id != FUNID_vkQueueSubmit2) ||
        state->calls.size() != state->learned_global_call_count ||
        !DescriptorCacheCompleteForSubmit(*state) ||
        state->descriptor_plan_cursor !=
            state->learned_descriptor_plans.size()) {
        return true;
    }
    std::unordered_map<uint64_t, CommandState>::const_iterator command =
        state->commands.find(command_key);
    if (command == state->commands.end() || command->second.recording ||
        !command->second.executable) {
        return true;
    }

    std::vector<VkCommandBuffer> submitted;
    submitted.push_back((VkCommandBuffer)(uintptr_t)command_key);
    ProtocolOccurrence occurrence;
    if (!AssembleOccurrenceLocked(
            state, submitted, state->learned_submit_call, &occurrence)) {
        return true;
    }
    if (occurrence.has_opaque_command) {
        if (!FallbackToLegacyLocked(state, NULL)) {
            state->transport_failed = true;
            return false;
        }
        return true;
    }
    std::vector<std::pair<uint64_t, uint64_t> > command_graph;
    if (occurrence.dispatches == 0 ||
        !SnapshotSubmittedCommandGraphLocked(
            *state, submitted, &command_graph) ||
        !PrepareRecordFrontierLocked(state, &occurrence, false) ||
        FrontierRecordCount(occurrence) != state->learned_record_count ||
        occurrence.signature != state->learned_signature ||
        occurrence.bytes != state->learned_template_bytes ||
        !MatchTemplate(state->learned, occurrence)) {
        return true;
    }

    std::vector<UnitSample> units;
    std::vector<ChunkSample> chunks;
    uint32_t period_flags = 0;
    uint64_t begin_ack_epoch = 0;
    uint64_t active_plan_epoch = 0;
    if (!BuildUnits(occurrence, &units) ||
        !SameUnitGeometry(state->learned_units, units) ||
        !state->plan_valid || state->plan_epoch == 0 ||
        state->plan_epoch != state->acked_plan_epoch ||
        state->plan_apply_period > state->next_period_id ||
        !PlanMatchesUnits(*state, units) ||
        state->next_period_id == 0 ||
        state->next_period_id == UINT64_MAX ||
        state->next_submission_id == 0 ||
        state->next_submission_id == UINT64_MAX ||
        !BuildChunksLocked(
            state, state->next_period_id, units, &chunks, &period_flags,
            &begin_ack_epoch, &active_plan_epoch) ||
        chunks.size() <= 1 || begin_ack_epoch != 0 ||
        active_plan_epoch != state->plan_epoch ||
        period_flags != kPeriodStableFast) {
        return true;
    }

    const uint64_t period_id = state->next_period_id++;
    const uint64_t submission_id = state->next_submission_id++;
    const uint64_t started_ns = NowNs();
    if (!SendPeriodBeginLocked(
            state, period_id, period_flags, 0) ||
        SendRoutedSubmissionRangeLocked(
            state, period_id, active_plan_epoch, submission_id,
            occurrence, units, &chunks, false, 0,
            static_cast<uint32_t>(chunks.size() - 1)) !=
                kRouteReplyAccepted) {
        if (!FallbackToLegacyLocked(state, NULL)) {
            state->transport_failed = true;
            return false;
        }
        return true;
    }

    state->early_route_ready = true;
    state->early_recovery = false;
    state->early_period_id = period_id;
    state->early_submission_id = submission_id;
    state->early_period_flags = period_flags;
    state->early_plan_epoch = active_plan_epoch;
    state->early_start_ns = started_ns;
    state->early_next_chunk =
        static_cast<uint32_t>(chunks.size() - 1);
    state->early_command_buffer = command_key;
    state->early_queue = HandleBits(state->queue);
    state->early_signature = occurrence.signature;
    state->early_template_bytes = occurrence.bytes;
    state->early_dispatches = occurrence.dispatches;
    state->early_descriptor_plan_cursor =
        state->descriptor_plan_cursor;
    state->early_calls = occurrence.calls;
    state->early_command_graph.swap(command_graph);
    state->early_consumed_descriptor_sets =
        occurrence.consumed_descriptor_sets;
    state->early_frontier_update_ids =
        occurrence.frontier_update_ids;
    state->early_units.swap(units);
    state->early_chunks.swap(chunks);
    return true;
}

ProtocolPrepareResult CompleteEarlyRouteLocked(
        DeviceState* state,
        const std::vector<VkCommandBuffer>& submitted,
        ProtocolOccurrence* occurrence,
        uint64_t* token) {
    if (state == NULL || occurrence == NULL || token == NULL ||
        !state->early_route_ready) {
        return kProtocolBlocked;
    }
    if (occurrence->has_opaque_command) {
        return PrepareFallbackLegacySubmitLocked(state, token)
            ? kProtocolFallbackLegacy : kProtocolBlocked;
    }
    std::vector<std::pair<uint64_t, uint64_t> > actual_command_graph;
    const bool command_graph_exact =
        SnapshotSubmittedCommandGraphLocked(
            *state, submitted, &actual_command_graph);
    std::vector<UnitSample> actual_units;
    std::vector<ChunkSample> actual_chunks;
    uint32_t actual_period_flags = 0;
    uint64_t begin_ack_epoch = 0;
    uint64_t active_plan_epoch = 0;
    const bool exact =
        state->stage == kStageFast && state->negotiated &&
        HasRequiredCapabilities(*state) && !state->transport_failed &&
        !state->mismatch_pending && !state->early_recovery &&
        state->registered_queues.size() == 1 &&
        HandleBits(state->queue) == state->early_queue &&
        *state->registered_queues.begin() == state->early_queue &&
        submitted.size() == 1 &&
        HandleBits(submitted[0]) == state->early_command_buffer &&
        state->learned_primary_commands.size() == 1 &&
        state->learned_primary_commands[0] ==
            state->early_command_buffer &&
        command_graph_exact &&
        actual_command_graph == state->early_command_graph &&
        state->calls.size() == state->learned_global_call_count &&
        DescriptorCacheCompleteForSubmit(*state) &&
        state->descriptor_plan_cursor ==
            state->early_descriptor_plan_cursor &&
        occurrence->dispatches == state->early_dispatches &&
        occurrence->signature == state->early_signature &&
        occurrence->signature == state->learned_signature &&
        occurrence->bytes == state->early_template_bytes &&
        occurrence->bytes == state->learned_template_bytes &&
        occurrence->consumed_descriptor_sets ==
            state->early_consumed_descriptor_sets &&
        occurrence->frontier_update_ids ==
            state->early_frontier_update_ids &&
        FrontierRecordCount(*occurrence) ==
            state->learned_record_count &&
        SameSemanticCalls(occurrence->calls, state->early_calls) &&
        MatchTemplate(state->learned, *occurrence) &&
        BuildUnits(*occurrence, &actual_units) &&
        SameUnitGeometry(state->early_units, actual_units) &&
        state->plan_valid && state->plan_epoch != 0 &&
        state->plan_epoch == state->acked_plan_epoch &&
        state->plan_epoch == state->early_plan_epoch &&
        state->plan_apply_period <= state->early_period_id &&
        PlanMatchesUnits(*state, actual_units) &&
        BuildChunksLocked(
            state, state->early_period_id, actual_units, &actual_chunks,
            &actual_period_flags, &begin_ack_epoch,
            &active_plan_epoch) &&
        begin_ack_epoch == 0 &&
        active_plan_epoch == state->early_plan_epoch &&
        actual_period_flags == state->early_period_flags &&
        actual_period_flags == kPeriodStableFast &&
        SameChunkGeometry(state->early_chunks, actual_chunks) &&
        state->early_chunks.size() > 1 &&
        state->early_next_chunk + 1 == state->early_chunks.size();
    if (!exact) {
        return PrepareFallbackLegacySubmitLocked(state, token)
            ? kProtocolFallbackLegacy : kProtocolBlocked;
    }

    const RouteReplyDisposition disposition =
        SendRoutedSubmissionRangeLocked(
            state, state->early_period_id, state->early_plan_epoch,
            state->early_submission_id, *occurrence,
            state->early_units, &state->early_chunks, false,
            state->early_next_chunk, state->early_next_chunk + 1);
    if (disposition != kRouteReplyAccepted) {
        return PrepareFallbackLegacySubmitLocked(state, token)
            ? kProtocolFallbackLegacy : kProtocolBlocked;
    }

    state->units = actual_units;
    state->chunks = state->early_chunks;
    state->period_start_ns = state->early_start_ns;
    const uint64_t period_id = state->early_period_id;
    const uint64_t submission_id = state->early_submission_id;
    const uint32_t period_flags = state->early_period_flags;
    const uint64_t plan_epoch = state->early_plan_epoch;
    ClearEarlyRoute(state);
    SaveActiveOccurrence(
        state, *occurrence, submitted, period_id, submission_id,
        period_flags, plan_epoch, false, true, false);
    *token = submission_id;
    return kProtocolReady;
}

ProtocolPrepareResult ProtocolBeforeSubmitLocked(
        DeviceState* state,
        const std::vector<VkCommandBuffer>& submitted,
        const SemanticCall& submit_call,
        uint64_t* token) {
    if (token == NULL) return kProtocolBlocked;
    *token = 0;
    if (state == NULL || state->transport_failed || state->active_submit ||
        !FlushAnyClusterLocked() || !RefreshControlPage(state)) {
        if (state != NULL) state->transport_failed = true;
        return kProtocolBlocked;
    }
    ProtocolOccurrence occurrence;
    if (!AssembleOccurrenceLocked(
            state, submitted, submit_call, &occurrence)) {
        if (state->stage == kStageDetect ||
            state->stage == kStageLegacy) {
            ClearOccurrence(state);
            return kProtocolLegacy;
        }
        return PrepareFallbackLegacySubmitLocked(state, token)
            ? kProtocolFallbackLegacy : kProtocolBlocked;
    }
    const uint64_t occurrence_base_bytes = occurrence.bytes;
    const uint64_t occurrence_base_signature = occurrence.signature;
    if (!PrepareRecordFrontierLocked(state, &occurrence, false)) {
        if (state->stage == kStageDetect ||
            state->stage == kStageLegacy) {
            ClearOccurrence(state);
            return kProtocolLegacy;
        }
        return PrepareFallbackLegacySubmitLocked(state, token)
            ? kProtocolFallbackLegacy : kProtocolBlocked;
    }
    if (state->stage == kStageLegacy ||
        !state->negotiated || !HasRequiredCapabilities(*state)) {
        ClearOccurrence(state);
        return kProtocolLegacy;
    }
    if (state->stage == kStageDetect) {
        if (occurrence.dispatches == 0) {
            ClearOccurrence(state);
            return kProtocolLegacy;
        }
        const bool observed =
            ObserveDetectLocked(state, occurrence.detect_signature);
        ClearOccurrence(state);
        if (!observed) {
            state->transport_failed = true;
            return kProtocolBlocked;
        }
        return kProtocolLegacy;
    }
    if (occurrence.dispatches == 0) {
        return PrepareFallbackLegacySubmitLocked(state, token)
            ? kProtocolFallbackLegacy : kProtocolBlocked;
    }
    if (occurrence.has_opaque_command && IsSpecialized(*state)) {
        /*
         * The command may have been recorded while Detect was active and
         * submitted again after promotion.  Carry the classification in the
         * recorded stream so record-once workloads cannot bypass the guard.
         */
        return PrepareFallbackLegacySubmitLocked(state, token)
            ? kProtocolFallbackLegacy : kProtocolBlocked;
    }
    if (state->early_route_ready) {
        return CompleteEarlyRouteLocked(
            state, submitted, &occurrence, token);
    }

    const bool learning = state->stage == kStageLearn;
    const bool fast = state->stage == kStageFast;
    const bool forced_recovery = state->stage == kStageRecover;
    const bool descriptor_cache_complete =
        DescriptorCacheCompleteForSubmit(*state);
    if ((state->stage == kStageMatch ||
         state->stage == kStageFast) &&
        !descriptor_cache_complete) {
        /*
         * Missing, additional, stale or topologically different descriptor
         * plans are a local mismatch.  The generic shadow has already captured
         * every miss, so the existing single-flush Recover path is exact.
         */
        state->mismatch_pending = true;
    }
    const bool matches =
        forced_recovery ||
        (!state->mismatch_pending &&
         (learning ||
          (FrontierRecordCount(occurrence) ==
               state->learned_record_count &&
           occurrence.signature == state->learned_signature &&
           MatchTemplate(state->learned, occurrence))));
    bool recovery = forced_recovery || !matches;
    if (recovery) {
        /*
         * The first frontier was needed to decide whether the occurrence still
         * matches.  A recovery route, however, must not merely replay that
         * pending delta.  Fold every live Type-II destination into the recovery
         * set, validate every PendingRecord-to-slot back-reference, rebuild the
         * complete valid shadow image for all of those sets, and route the
         * result as one all-record frontier.  RECOVERY_COMPLETE returns to
         * Detect, so the checkpoint representation cannot retain a suffix.
         */
        if (!FlimeShadowPrepareCompleteRecoveryCheckpoint(
                state, &occurrence.consumed_descriptor_sets)) {
            return PrepareFallbackLegacySubmitLocked(state, token)
                ? kProtocolFallbackLegacy : kProtocolBlocked;
        }
        occurrence.bytes = occurrence_base_bytes;
        occurrence.signature = occurrence_base_signature;
        occurrence.route_all_records = true;
        occurrence.frontier_update_ids.clear();
        if (!PrepareRecordFrontierLocked(state, &occurrence, true) ||
            !FlimeShadowVerifyRecoveryCheckpoint(
                state, occurrence.consumed_descriptor_sets,
                occurrence.frontier_update_ids, false)) {
            return PrepareFallbackLegacySubmitLocked(state, token)
                ? kProtocolFallbackLegacy : kProtocolBlocked;
        }
    }
    std::vector<UnitSample> units;
    std::vector<ChunkSample> chunks;
    uint32_t period_flags = 0;
    uint64_t begin_ack_epoch = 0;
    uint64_t active_plan_epoch = 0;
    if (!BuildUnits(occurrence, &units) ||
        state->next_period_id == 0 ||
        state->next_period_id == UINT64_MAX ||
        state->next_submission_id == 0 ||
        state->next_submission_id == UINT64_MAX) {
        state->transport_failed = true;
        return kProtocolBlocked;
    }
    const uint64_t period_id = state->next_period_id++;
    const uint64_t submission_id = state->next_submission_id++;
    if (!BuildChunksLocked(
            state, period_id, units, &chunks, &period_flags,
            &begin_ack_epoch, &active_plan_epoch)) {
        state->transport_failed = true;
        return kProtocolBlocked;
    }
    if (recovery) {
        begin_ack_epoch = 0;
        active_plan_epoch = 0;
        period_flags = kPeriodSingleFlush | kPeriodFineProfile;
        if (state->stage == kStageFast && state->plan_valid) {
            period_flags |= kPeriodStableFast;
        }
        BuildFineChunks(units, state->max_chunks, &chunks);
        if (chunks.size() != 1) {
            state->transport_failed = true;
            return kProtocolBlocked;
        }
    }

    state->period_start_ns = NowNs();
    if (!SendPeriodBeginLocked(
            state, period_id, period_flags, begin_ack_epoch)) {
        state->transport_failed = true;
        return kProtocolBlocked;
    }
    if (!matches &&
        !SendProgressLocked(state, kProgressMismatch, 0, 0, 0)) {
        state->transport_failed = true;
        return kProtocolBlocked;
    }
    RouteReplyDisposition disposition = SendRoutedSubmissionLocked(
        state, period_id, active_plan_epoch, submission_id,
        occurrence, units, &chunks, recovery);
    if (!recovery && disposition == kRouteReplyNeedsRecovery) {
        /*
         * The normal route may already have released a prefix.  Replacing its
         * record identity/count now would violate the host ledger.  Since no
         * full-shadow checkpoint preceded this route, synchronously drain the
         * exact retained ledger and leave specialization instead of asserting
         * an unverified recovery.
         */
        return PrepareFallbackLegacySubmitLocked(state, token)
            ? kProtocolFallbackLegacy : kProtocolBlocked;
    }
    if (disposition != kRouteReplyAccepted) {
        if (recovery &&
            PrepareFallbackLegacySubmitLocked(state, token)) {
            return kProtocolFallbackLegacy;
        }
        state->transport_failed = true;
        return kProtocolBlocked;
    }
    if (recovery &&
        !FlimeShadowVerifyRecoveryCheckpoint(
            state, occurrence.consumed_descriptor_sets,
            occurrence.frontier_update_ids, true)) {
        return PrepareFallbackLegacySubmitLocked(state, token)
            ? kProtocolFallbackLegacy : kProtocolBlocked;
    }

    state->units.swap(units);
    state->chunks.swap(chunks);
    SaveActiveOccurrence(
        state, occurrence, submitted, period_id, submission_id, period_flags,
        active_plan_epoch, learning && !recovery,
        fast && !recovery, recovery);
    *token = submission_id;
    return kProtocolReady;
}

void ResetLearnedState(DeviceState* state) {
    if (state == NULL) return;
    state->stage = kStageDetect;
    state->learned.clear();
    state->learned_descriptor_plans.clear();
    state->learned_descriptor_cache_complete = false;
    state->learned_signature = 0;
    state->learned_template_bytes = 0;
    state->learned_global_call_count = 0;
    state->learned_record_count = 0;
    state->learned_units.clear();
    state->learned_primary_commands.clear();
    state->learned_submit_call = SemanticCall();
    state->interval_history.clear();
    state->interval_announced = false;
    state->interval_hash = 0;
    state->plan_valid = false;
    state->plan.clear();
    state->plan_epoch = 0;
    state->plan_apply_period = 0;
    state->acked_plan_epoch = 0;
    state->request_fine_profile = true;
    state->fast_periods = 0;
    ClearEarlyRoute(state);
}

bool RetireActiveFrontierLocked(DeviceState* state) {
    if (state == NULL) return false;
    std::set<uint64_t> found_active;
    std::set<
        std::pair<uint64_t, std::pair<uint32_t, uint32_t> > > live_slots;
    std::vector<PendingRecord> retained;
    retained.reserve(state->records.size());

    for (size_t i = 0; i < state->records.size(); ++i) {
        const PendingRecord& record = state->records[i];
        const bool active =
            state->active_frontier_update_ids.find(record.update_id) !=
                state->active_frontier_update_ids.end();
        if (active) {
            if (!found_active.insert(record.update_id).second ||
                state->active_consumed_descriptor_sets.find(
                    HandleBits(record.set)) ==
                    state->active_consumed_descriptor_sets.end() ||
                (!record.released && !record.elided)) {
                return false;
            }
            continue;
        }
        if (record.released || record.elided) continue;

        const uint64_t set_key = HandleBits(record.set);
        if (!FlimePendingRecordMatchesSlot(
                state, record, i, true) ||
            !live_slots.insert(std::make_pair(
                set_key, std::make_pair(
                    record.binding, record.array_element))).second) {
            return false;
        }
        retained.push_back(record);
    }
    if (found_active != state->active_frontier_update_ids) return false;

    for (std::unordered_map<uint64_t, SetState>::iterator set =
             state->sets.begin(); set != state->sets.end(); ++set) {
        for (std::map<std::pair<uint32_t, uint32_t>,
                      DescriptorSlot>::iterator slot =
                 set->second.slots.begin();
             slot != set->second.slots.end(); ++slot) {
            slot->second.pending_record = -1;
        }
    }
    state->records.swap(retained);
    for (size_t i = 0; i < state->records.size(); ++i) {
        const PendingRecord& record = state->records[i];
        SetState& set = state->sets.find(
            HandleBits(record.set))->second;
        DescriptorSlot& slot = set.slots.find(
            std::make_pair(
                record.binding, record.array_element))->second;
        slot.pending_record = static_cast<int64_t>(i);
    }
    return true;
}

bool RecoveryFrontierFullyRetiredLocked(const DeviceState* state) {
    if (state == NULL || !state->records.empty()) return false;
    for (std::unordered_map<uint64_t, SetState>::const_iterator set =
             state->sets.begin(); set != state->sets.end(); ++set) {
        for (std::map<std::pair<uint32_t, uint32_t>,
                      DescriptorSlot>::const_iterator slot =
                 set->second.slots.begin();
             slot != set->second.slots.end(); ++slot) {
            if (slot->second.pending_record != -1) return false;
        }
    }
    return true;
}

bool FinishOccurrenceLocked(DeviceState* state) {
    if (state == NULL) return false;
    const bool completed_recovery = state->active_recovery;
    if (!RetireActiveFrontierLocked(state)) {
        state->transport_failed = true;
        ClearActiveSubmit(state);
        return false;
    }
    if (completed_recovery &&
        !RecoveryFrontierFullyRetiredLocked(state)) {
        state->transport_failed = true;
        ClearActiveSubmit(state);
        return false;
    }
    /*
     * Global semantic calls belong to the completed queue occurrence, but
     * unconsumed descriptor records remain authoritative until a later
     * submitted graph reaches their destination sets.
     */
    ClearOccurrence(state, true);
    ClearActiveSubmit(state);
    return true;
}

bool FailActiveSubmitLocked(DeviceState* state) {
    if (state != NULL) {
        state->transport_failed = true;
        ClearActiveSubmit(state);
    }
    return false;
}

bool FallbackActiveRecoveryToLegacyLocked(DeviceState* state) {
    if (state == NULL || !state->active_submit ||
        !state->active_recovery) {
        return false;
    }
    /*
     * Queue submission has completed, so the active gate may be lowered while
     * the synchronous fallback packet drains the recovery ledger.  On success
     * EnterLegacy clears both occurrence and active state; on failure restore
     * the gate so the ordinary fatal cleanup remains well formed.
     */
    state->active_submit = false;
    if (FallbackToLegacyLocked(state, NULL)) return true;
    state->active_submit = true;
    return false;
}

bool ProtocolAfterSubmitLocked(DeviceState* state,
                               uint64_t token,
                               bool transport_ok) {
    if (state == NULL || !state->active_submit || token == 0 ||
        token != state->active_token ||
        token != state->active_submission_id || !transport_ok) {
        return FailActiveSubmitLocked(state);
    }
    bool reported = false;
    if (state->active_recovery) {
        if (!state->active_generic_shadow_ran ||
            !FlimeShadowVerifyRecoveryCheckpoint(
                state, state->active_consumed_descriptor_sets,
                state->active_frontier_update_ids, true)) {
            if (FallbackActiveRecoveryToLegacyLocked(state)) return true;
            return FailActiveSubmitLocked(state);
        }
        /*
         * The native QueueSubmit has returned VK_SUCCESS.  Retire the complete
         * routed checkpoint and prove that neither a record nor a slot link
         * survives before telling the host it may leave Recover.
         */
        if (!RetireActiveFrontierLocked(state) ||
            !RecoveryFrontierFullyRetiredLocked(state)) {
            if (FallbackActiveRecoveryToLegacyLocked(state)) return true;
            return FailActiveSubmitLocked(state);
        }
        reported = SendProgressLocked(
            state, kProgressRecoveryComplete,
            kProgressGenericShadowRan,
            0, 0);
        if (!reported) {
            if (FallbackActiveRecoveryToLegacyLocked(state)) return true;
            return FailActiveSubmitLocked(state);
        }
        ResetLearnedState(state);
        ClearOccurrence(state, true);
        ClearActiveSubmit(state);
        return true;
    } else {
        if (!SendProfileLocked(
                state, state->active_period_id,
                state->active_period_flags,
                state->units, state->chunks)) {
            return FailActiveSubmitLocked(state);
        }
        if (state->active_calls.empty() ||
            state->active_calls.size() > UINT32_MAX) {
            return FailActiveSubmitLocked(state);
        }
        const uint32_t entries =
            static_cast<uint32_t>(state->active_calls.size());
        if (state->active_learning) {
            ProtocolOccurrence occurrence;
            occurrence.calls = state->active_calls;
            occurrence.bytes = state->active_template_bytes;
            occurrence.dispatches = state->active_dispatches;
            occurrence.signature = state->active_signature;
            LearnTemplate(occurrence, &state->learned);
            state->learned_descriptor_plans =
                state->active_descriptor_plans;
            state->learned_descriptor_cache_complete =
                state->active_descriptor_cache_complete;
            state->learned_signature = occurrence.signature;
            state->learned_template_bytes = occurrence.bytes;
            state->learned_global_call_count =
                static_cast<uint32_t>(state->calls.size());
            state->learned_record_count =
                static_cast<uint32_t>(
                    state->active_frontier_update_ids.size());
            state->learned_units = state->units;
            state->learned_primary_commands =
                state->active_primary_commands;
            state->learned_submit_call = state->active_calls.back();
            reported = SendProgressLocked(
                state, kProgressLearnComplete,
                state->active_generic_shadow_ran
                    ? kProgressGenericShadowRan : 0u,
                entries, 0);
            if (reported) state->stage = kStageMatch;
        } else if (state->active_fast) {
            reported = SendProgressLocked(
                state, kProgressFastComplete,
                kProgressMatchSucceeded, entries,
                state->active_period_id);
            if (reported) {
                state->stage = kStageFast;
                ++state->fast_periods;
            }
        } else {
            reported = SendProgressLocked(
                state, kProgressMatchComplete,
                (state->active_generic_shadow_ran
                     ? kProgressGenericShadowRan : 0u) |
                    kProgressMatchSucceeded,
                entries, 0);
            if (reported) {
                /*
                 * Learn supplied the topology.  Match ran generic shadow again
                 * and now arms Fast with its newest concrete lifetime guards.
                 */
                state->learned_descriptor_plans =
                    state->active_descriptor_plans;
                state->learned_descriptor_cache_complete =
                    state->active_descriptor_cache_complete;
                state->learned_signature = state->active_signature;
                state->learned_template_bytes =
                    state->active_template_bytes;
                state->learned_record_count =
                    static_cast<uint32_t>(
                        state->active_frontier_update_ids.size());
                state->learned_units = state->units;
                state->learned_primary_commands =
                    state->active_primary_commands;
                state->learned_submit_call =
                    state->active_calls.back();
                state->stage = kStageFast;
            }
        }
    }
    if (!reported) {
        if (state->active_recovery &&
            FallbackActiveRecoveryToLegacyLocked(state)) {
            return true;
        }
        return FailActiveSubmitLocked(state);
    }
    if (!FinishOccurrenceLocked(state)) {
        return false;
    }
    return true;
}

/*
 * Public hooks used by the generated Vulkan encoder.
 *
 * The implementation remains independent of generated local variable names:
 * all integration points pass Vulkan objects and already-computed encoded
 * lengths.  Global state is protected by one mutex because ordering across
 * control, routed updates, clustered Type-I calls and QueueSubmit is the
 * correctness boundary.
 */

uint64_t NextStreamIdLocked(uint64_t process_id, uint64_t device_key) {
    for (;;) {
        const uint64_t serial = g_stream_serial++;
        uint64_t candidate = Mix64(
            process_id ^ device_key ^
            (serial * UINT64_C(0x9e3779b97f4a7c15)));
        if (candidate == 0) candidate = serial == 0 ? 1 : serial;
        bool collision = false;
        for (std::unordered_map<
                 uint64_t, std::shared_ptr<DeviceState> >::const_iterator it =
                 g_devices.begin(); it != g_devices.end(); ++it) {
            if (it->second->process_id == process_id &&
                it->second->stream_id == candidate) {
                collision = true;
                break;
            }
        }
        if (!collision) return candidate;
    }
}

bool AllocateControlPage(DeviceState* state) {
    if (state == NULL) return false;
    long system_page = sysconf(_SC_PAGESIZE);
    size_t allocation = system_page > 0
        ? static_cast<size_t>(system_page) : kControlAllocationBytes;
    if (allocation < kControlAllocationBytes) {
        allocation = kControlAllocationBytes;
    }
    void* memory = NULL;
    if (posix_memalign(&memory, allocation, allocation) != 0 ||
        memory == NULL) {
        return false;
    }
    memset(memory, 0, allocation);
    state->control_page = static_cast<uint8_t*>(memory);
    state->control_allocation_bytes = allocation;
    return true;
}

void RemoveDeviceMappingsLocked(const DeviceState& state) {
    for (std::unordered_map<uint64_t, uint64_t>::iterator it =
             g_queue_devices.begin(); it != g_queue_devices.end();) {
        if (it->second == HandleBits(state.device)) {
            it = g_queue_devices.erase(it);
        } else {
            ++it;
        }
    }
    for (std::unordered_map<uint64_t, uint64_t>::iterator it =
             g_command_devices.begin(); it != g_command_devices.end();) {
        if (it->second == HandleBits(state.device)) {
            g_recorded_commands.erase(it->first);
            it = g_command_devices.erase(it);
        } else {
            ++it;
        }
    }
    for (std::unordered_map<uint64_t, uint64_t>::iterator it =
             g_set_devices.begin(); it != g_set_devices.end();) {
        if (it->second == HandleBits(state.device)) {
            it = g_set_devices.erase(it);
        } else {
            ++it;
        }
    }
    for (std::unordered_map<uint64_t, uint64_t>::iterator it =
             g_pool_devices.begin(); it != g_pool_devices.end();) {
        if (it->second == HandleBits(state.device)) {
            it = g_pool_devices.erase(it);
        } else {
            ++it;
        }
    }
}

bool SendSessionPacketLocked(DeviceState* state, WireType type) {
    if (state == NULL || (type != kWireReset && type != kWireTeardown)) {
        return false;
    }
    BytePacket packet(kWireHeaderBytes);
    InitializeWireHeader(&packet, type, 0, 0, *state, 0, 0);
    return SendControlLocked(state, packet.data);
}

void ReinitializeSessionLocked(DeviceState* state) {
    if (state == NULL || state->control_page == NULL) return;
    ClearOccurrence(state);
    ClearActiveSubmit(state);
    ResetLearnedState(state);
    memset(state->control_page, 0, state->control_allocation_bytes);
    state->control_sequence = 0;
    state->stream_id =
        NextStreamIdLocked(state->process_id, HandleBits(state->device));
    state->negotiated = false;
    state->session_invalidated = false;
    state->transport_failed = false;
}

bool EnsureNegotiatedLocked(DeviceState* state) {
    if (state == NULL || state->transport_failed) return false;
    if (state->session_invalidated) ReinitializeSessionLocked(state);
    if (state->negotiated) return true;
    if (state->stage == kStageLegacy) return false;
    if (!NegotiateCapabilitiesLocked(state)) {
        if (state->stage != kStageLegacy) state->transport_failed = true;
        return false;
    }
    return true;
}

size_t LiveRecordCount(const DeviceState& state) {
    size_t count = 0;
    for (size_t i = 0; i < state.records.size(); ++i) {
        if (!state.records[i].elided) ++count;
    }
    return count;
}

void FailNamedWriteLocked(DeviceState* state, int fun_id) {
    if (state != NULL) state->transport_failed = true;
    g_skip_next_fun_id = fun_id;
    g_allow_next_cluster = false;
    g_block_next_write = true;
    g_flush_cluster_after_write = false;
    g_route_after_write_command = 0;
}

bool IsTypeIClusterFunction(int fun_id) {
    switch (fun_id) {
    case FUNID_vkCmdBindDescriptorSets:
    case FUNID_vkCmdBindPipeline:
    case FUNID_vkCmdDispatch:
    case FUNID_vkCmdDispatchBase:
    case FUNID_vkCmdDispatchIndirect:
    case FUNID_vkCmdExecuteCommands:
        return true;
    default:
        return false;
    }
}

ssize_t WriteFrozenDirectLocked(int fd,
                                ParamManager* manager,
                                const ParamManager::FrozenCall& call) {
    if (!FlushAnyClusterLocked()) return -1;
    return manager->writeFrozen(fd, call);
}

bool RebaseFrozenCallPointers(ParamManager::FrozenCall* call);

/*
 * Append one input-only asynchronous call to the owned Type-I envelope.
 *
 * FrozenCall keeps ordinary parameters and pointer parameters in separate
 * backing stores.  The clustered wire format instead needs every non-null
 * SendParam to name an offset in one contiguous save buffer.  Validate the
 * complete frozen framing and both backing-store walks before changing the
 * live cluster, then copy each payload and rewrite only the private call
 * image.  Callers are responsible for the input-only classification.
 */
ssize_t AppendOwnedClusterLocked(
        int fd,
        ParamManager* manager,
        ParamManager::FrozenCall* call) {
    const size_t kHeaderBytes = 2u * sizeof(uint64_t);
    const size_t kDescriptorBytes = sizeof(ParamManager::SendParam);
    const size_t kMaxParameters = 32u;
    const size_t kMaxCalls = 64u;
    const size_t kMaxCallBytes = 64u * 1024u;
    const size_t kMaxPayloadBytes = 1024u * 1024u;
    if (manager == NULL || call == NULL || call->empty() ||
        call->sendBuf.size() < 0 || call->paramStorage.size() < 0 ||
        call->pointerStorage.size() < 0 ||
        !RebaseFrozenCallPointers(call)) {
        return -1;
    }
    if (call->isSync) {
        const ssize_t direct =
            WriteFrozenDirectLocked(fd, manager, *call);
        return direct == call->size() ? 1 : -1;
    }

    const size_t call_bytes = static_cast<size_t>(call->sendBuf.size());
    const size_t param_bytes =
        static_cast<size_t>(call->paramStorage.size());
    const size_t pointer_bytes =
        static_cast<size_t>(call->pointerStorage.size());
    if (call_bytes < kHeaderBytes || call->data() == NULL) {
        return -1;
    }

    uint64_t encoded_id = 0;
    uint64_t descriptor_count = 0;
    memcpy(&encoded_id, call->data(), sizeof(encoded_id));
    memcpy(&descriptor_count,
           call->data() + sizeof(encoded_id),
           sizeof(descriptor_count));
    if (encoded_id != call->realId ||
        call->deviceId != EXPRESS_GPU_DEVICE_ID || call->funId < 0 ||
        (encoded_id >> 32) !=
            static_cast<uint32_t>(call->deviceId) ||
        (encoded_id & UINT64_C(0x00ffffff)) !=
            static_cast<uint32_t>(call->funId) ||
        (encoded_id & UINT64_C(0xff000000)) != 0 ||
        descriptor_count >
            (std::numeric_limits<size_t>::max() - kHeaderBytes) /
                kDescriptorBytes ||
        call_bytes != kHeaderBytes +
            static_cast<size_t>(descriptor_count) * kDescriptorBytes) {
        return -1;
    }
    if (descriptor_count > kMaxParameters) {
        const ssize_t direct =
            WriteFrozenDirectLocked(fd, manager, *call);
        return direct == call->size() ? 1 : -1;
    }

    std::vector<ParamManager::SendParam> descriptors(
        static_cast<size_t>(descriptor_count));
    size_t pointer_offset = 0;
    size_t payload_bytes = 0;
    bool cluster_compatible = true;
    for (size_t i = 0; i < descriptors.size(); ++i) {
        memcpy(&descriptors[i],
               call->data() + kHeaderBytes + i * kDescriptorBytes,
               kDescriptorBytes);
        const ParamManager::SendParam& descriptor = descriptors[i];
        if (descriptor.size > static_cast<uint64_t>(
                                  std::numeric_limits<int>::max())) {
            return -1;
        }
        if (i == 0 && param_bytes != 0) {
            if (descriptor.size != param_bytes ||
                descriptor.data != static_cast<uint64_t>(
                    reinterpret_cast<uintptr_t>(
                        call->paramStorage.data()))) {
                return -1;
            }
        } else if (descriptor.data != 0) {
            if (descriptor.size == 0 ||
                pointer_offset > pointer_bytes ||
                descriptor.size > pointer_bytes - pointer_offset ||
                descriptor.data != static_cast<uint64_t>(
                    reinterpret_cast<uintptr_t>(
                        call->pointerStorage.data() + pointer_offset))) {
                return -1;
            }
            pointer_offset += static_cast<size_t>(descriptor.size);
        } else if (descriptor.size != 0) {
            /*
             * A null Vulkan pointer may legally carry a non-zero nominal
             * length in a direct RPC.  Cluster preflight intentionally
             * rejects that ambiguous pair, so preserve it by direct send.
             */
            cluster_compatible = false;
        }

        if (descriptor.data != 0) {
            const size_t bytes = static_cast<size_t>(descriptor.size);
            if (bytes > std::numeric_limits<size_t>::max() -
                            payload_bytes) {
                return -1;
            }
            payload_bytes += bytes;
            if (payload_bytes > kMaxPayloadBytes) {
                cluster_compatible = false;
            }
        }
    }
    if (pointer_offset != pointer_bytes ||
        (param_bytes != 0 && descriptors.empty()) ||
        param_bytes > std::numeric_limits<size_t>::max() - pointer_bytes ||
        payload_bytes != param_bytes + pointer_bytes) {
        return -1;
    }
    if (!cluster_compatible || call_bytes > kMaxCallBytes ||
        payload_bytes > kMaxPayloadBytes - 1u) {
        const ssize_t direct =
            WriteFrozenDirectLocked(fd, manager, *call);
        return direct == call->size() ? 1 : -1;
    }

    if (g_cluster.Empty()) {
        if (g_cluster.fd != -1 || !g_cluster.calls.empty() ||
            g_cluster.payload.size() != 1 ||
            g_cluster.payload[0] != 0) {
            return -1;
        }
    } else if (g_cluster.fd < 0 || g_cluster.call_count > kMaxCalls ||
               g_cluster.calls.empty() ||
               g_cluster.calls.size() > kMaxCallBytes ||
               g_cluster.payload.size() <= 1 ||
               g_cluster.payload.size() > kMaxPayloadBytes ||
               g_cluster.payload[0] != 0) {
        return -1;
    }
    if (!g_cluster.Empty() && g_cluster.fd != fd &&
        !FlushAnyClusterLocked()) {
        return -1;
    }

    size_t padding_bytes =
        payload_bytes == 0 && g_cluster.payload.size() == 1 ? 1u : 0u;
    size_t additional_payload = payload_bytes + padding_bytes;
    if (!g_cluster.Empty() &&
        (g_cluster.call_count >= kMaxCalls ||
         g_cluster.calls.size() > kMaxCallBytes - call_bytes ||
         g_cluster.payload.size() >
             kMaxPayloadBytes - additional_payload)) {
        if (!FlushAnyClusterLocked()) return -1;
        padding_bytes = payload_bytes == 0 ? 1u : 0u;
        additional_payload = payload_bytes + padding_bytes;
    }
    if (!g_cluster.Empty() &&
        (g_cluster.calls.size() > kMaxCallBytes - call_bytes ||
         g_cluster.payload.size() >
             kMaxPayloadBytes - additional_payload)) {
        return -1;
    }
    if (g_cluster.Empty()) g_cluster.fd = fd;

    std::vector<uint8_t> encoded(call_bytes);
    memcpy(&encoded[0], call->data(), call_bytes);
    std::vector<uint8_t> owned_payload;
    owned_payload.reserve(payload_bytes);
    const size_t payload_base = g_cluster.payload.size();
    for (size_t i = 0; i < descriptors.size(); ++i) {
        const ParamManager::SendParam& descriptor = descriptors[i];
        uint64_t wire_offset = 0;
        if (descriptor.data != 0) {
            wire_offset = static_cast<uint64_t>(
                payload_base + owned_payload.size());
            const uint8_t* source = reinterpret_cast<const uint8_t*>(
                static_cast<uintptr_t>(descriptor.data));
            owned_payload.insert(
                owned_payload.end(), source,
                source + static_cast<size_t>(descriptor.size));
        }
        memcpy(&encoded[kHeaderBytes + i * kDescriptorBytes +
                        sizeof(uint64_t)],
               &wire_offset, sizeof(wire_offset));
    }
    if (owned_payload.size() != payload_bytes) return -1;

    g_cluster.calls.reserve(g_cluster.calls.size() + call_bytes);
    g_cluster.payload.reserve(
        g_cluster.payload.size() + additional_payload);
    if (padding_bytes != 0) g_cluster.payload.push_back(0);
    g_cluster.payload.insert(g_cluster.payload.end(),
                             owned_payload.begin(), owned_payload.end());
    g_cluster.calls.insert(g_cluster.calls.end(),
                           encoded.begin(), encoded.end());
    ++g_cluster.call_count;
    return 1;
}

bool DrainEarlyRouteLocked(DeviceState* state) {
    if (state == NULL || !state->early_route_ready) return true;
    if (!IsSpecialized(*state) || state->active_submit ||
        !FallbackToLegacyLocked(state, NULL)) {
        state->transport_failed = true;
        return false;
    }
    return true;
}

bool RawCallMustNotOvertakeEarlyRoute(int fun_id) {
    switch (fun_id) {
    case FUNID_vkCreateCommandPool:
    case FUNID_vkAllocateCommandBuffers:
    case FUNID_vkFreeCommandBuffers:
    case FUNID_vkBeginCommandBuffer:
    case FUNID_vkEndCommandBuffer:
    case FUNID_vkResetCommandBuffer:
    case FUNID_vkDestroyCommandPool:
    case FUNID_vkResetCommandPool:
    case FUNID_vkGetDeviceQueue:
    case FUNID_vkGetDeviceQueue2:
    case FUNID_vkDestroyBuffer:
    case FUNID_vkDestroyBufferView:
    case FUNID_vkDestroyImageView:
    case FUNID_vkDestroySampler:
    case FUNID_vkCreateDescriptorPool:
    case FUNID_vkDestroyDescriptorPool:
    case FUNID_vkResetDescriptorPool:
    case FUNID_vkCreateDescriptorSetLayout:
    case FUNID_vkDestroyDescriptorSetLayout:
    case FUNID_vkAllocateDescriptorSets:
    case FUNID_vkUpdateDescriptorSets:
    case FUNID_vkCreateDescriptorUpdateTemplate:
    case FUNID_vkDestroyDescriptorUpdateTemplate:
    case FUNID_vkUpdateDescriptorSetWithTemplate:
    case FUNID_vkMergePipelineCaches:
        return true;
    default:
        return false;
    }
}

bool DrainEarlyRoutesBeforeRawCallLocked(int fun_id) {
    if (!RawCallMustNotOvertakeEarlyRoute(fun_id)) return true;
    /*
     * Several generated lifecycle wrappers expose only a post-RPC bookkeeping
     * hook.  Intercept their common transport call so a successful native
     * Begin/End/Reset/Free or newly observed queue can never overtake an
     * already released FLIME prefix.  Draining all devices is conservative,
     * deterministic, and avoids decoding generated parameter layouts here.
     */
    for (std::unordered_map<
             uint64_t, std::shared_ptr<DeviceState> >::iterator it =
             g_devices.begin(); it != g_devices.end(); ++it) {
        if (it->second->early_route_ready &&
            !DrainEarlyRouteLocked(it->second.get())) {
            return false;
        }
    }
    return true;
}

bool RecordCommandCallLocked(
        int fun_id,
        VkCommandBuffer command_buffer,
        const uint64_t* structural,
        size_t structural_count,
        const uint64_t* handles,
        size_t handle_count,
        uint64_t payload_hash,
        uint64_t encoded_bytes,
        bool dispatch,
        bool cluster_safe) {
    std::shared_ptr<DeviceState> state =
        FindCommandLocked(command_buffer);
    if (!state || state->stage == kStageLegacy) {
        MarkPreWriteSemantic(fun_id, false);
        return true;
    }
    if (state->transport_failed) {
        FailNamedWriteLocked(state.get(), fun_id);
        return false;
    }
    if (state->early_route_ready) {
        if (!DrainEarlyRouteLocked(state.get())) {
            FailNamedWriteLocked(state.get(), fun_id);
            return false;
        }
        MarkPreWriteSemantic(fun_id, false);
        return true;
    }
    const bool recorded = state &&
        AppendCommandSemanticLocked(
            command_buffer, fun_id, structural, structural_count,
            handles, handle_count, payload_hash,
            encoded_bytes, dispatch);
    if (!recorded) {
        FailNamedWriteLocked(state.get(), fun_id);
        return false;
    }
    const bool allow_cluster =
        cluster_safe && IsSpecialized(*state) &&
        IsTypeIClusterFunction(fun_id);
    MarkPreWriteSemantic(fun_id, allow_cluster);
    g_named_write_command = HandleBits(command_buffer);
    return true;
}

uint64_t DescriptorPayloadHash(
        uint32_t write_count,
        const VkWriteDescriptorSet* writes,
        uint32_t copy_count,
        const VkCopyDescriptorSet* copies) {
    uint64_t hash = UINT64_C(1469598103934665603);
    for (uint32_t i = 0; i < write_count; ++i) {
        const VkWriteDescriptorSet& write = writes[i];
        hash = HashWord(hash, HandleBits(write.dstSet));
        hash = HashWord(hash, write.dstBinding);
        hash = HashWord(hash, write.dstArrayElement);
        hash = HashWord(hash, write.descriptorCount);
        hash = HashWord(hash, write.descriptorType);
        for (uint32_t j = 0; j < write.descriptorCount; ++j) {
            if (FlimeDescriptorTypeIsBuffer(write.descriptorType)) {
                const VkDescriptorBufferInfo& value = write.pBufferInfo[j];
                hash = HashWord(hash, HandleBits(value.buffer));
                hash = HashWord(hash, value.offset);
                hash = HashWord(hash, value.range);
            } else if (FlimeDescriptorTypeIsImage(write.descriptorType)) {
                const VkDescriptorImageInfo& value = write.pImageInfo[j];
                hash = HashWord(hash, HandleBits(value.sampler));
                hash = HashWord(hash, HandleBits(value.imageView));
                hash = HashWord(hash, value.imageLayout);
            } else if (FlimeDescriptorTypeIsTexel(write.descriptorType)) {
                hash = HashWord(
                    hash, HandleBits(write.pTexelBufferView[j]));
            }
        }
    }
    for (uint32_t i = 0; i < copy_count; ++i) {
        const VkCopyDescriptorSet& copy = copies[i];
        hash = HashWord(hash, HandleBits(copy.srcSet));
        hash = HashWord(hash, HandleBits(copy.dstSet));
        hash = HashWord(hash, copy.srcBinding);
        hash = HashWord(hash, copy.dstBinding);
        hash = HashWord(hash, copy.srcArrayElement);
        hash = HashWord(hash, copy.dstArrayElement);
        hash = HashWord(hash, copy.descriptorCount);
    }
    return hash;
}

bool BuildDescriptorStructural(
        uint32_t write_count,
        const VkWriteDescriptorSet* writes,
        uint32_t copy_count,
        const VkCopyDescriptorSet* copies,
        std::vector<uint64_t>* structural,
        std::vector<uint64_t>* handles) {
    if (structural == NULL || handles == NULL) return false;
    const uint64_t structural_count =
        UINT64_C(2) + UINT64_C(4) * write_count +
        UINT64_C(5) * copy_count;
    const uint64_t handle_count =
        static_cast<uint64_t>(write_count) +
        UINT64_C(2) * copy_count;
    if (structural_count > kMaxSemanticCalls ||
        handle_count > kMaxSemanticCalls) {
        return false;
    }
    structural->reserve(static_cast<size_t>(structural_count));
    handles->reserve(static_cast<size_t>(handle_count));
    structural->push_back(write_count);
    structural->push_back(copy_count);
    for (uint32_t i = 0; i < write_count; ++i) {
        structural->push_back(writes[i].dstBinding);
        structural->push_back(writes[i].dstArrayElement);
        structural->push_back(writes[i].descriptorCount);
        structural->push_back(writes[i].descriptorType);
        handles->push_back(HandleBits(writes[i].dstSet));
    }
    for (uint32_t i = 0; i < copy_count; ++i) {
        structural->push_back(copies[i].srcBinding);
        structural->push_back(copies[i].srcArrayElement);
        structural->push_back(copies[i].dstBinding);
        structural->push_back(copies[i].dstArrayElement);
        structural->push_back(copies[i].descriptorCount);
        handles->push_back(HandleBits(copies[i].srcSet));
        handles->push_back(HandleBits(copies[i].dstSet));
    }
    return true;
}

FlimeGuestUpdateAction UpdateFailureActionLocked(
        DeviceState* state,
        bool specialized) {
    g_skip_next_fun_id = -1;
    g_allow_next_cluster = false;
    g_block_next_write = false;
    g_flush_cluster_after_write = false;
    g_route_after_write_command = 0;
    g_named_write_command = 0;
    if (state == NULL || !specialized) {
        if (state != NULL) EnterLegacy(state);
        return FLIME_GUEST_UPDATE_LEGACY;
    }
    if (!state->transport_failed &&
        FallbackToLegacyLocked(state, NULL)) {
        return FLIME_GUEST_UPDATE_LEGACY;
    }
    state->transport_failed = true;
    return FLIME_GUEST_UPDATE_FATAL;
}

void NotePotentialDivergenceLocked(DeviceState* state) {
    if (state == NULL || !DrainEarlyRouteLocked(state)) return;
    if (IsSpecialized(*state) && LiveRecordCount(*state) != 0) {
        state->mismatch_pending = true;
    }
}

void NoteEarlyCommandMutationLocked(DeviceState* state) {
    if (state != NULL) {
        DrainEarlyRouteLocked(state);
    }
}

bool StampNewRecordOriginsLocked(DeviceState* state, size_t first_record) {
    if (state == NULL || state->occurrence_serial == 0 ||
        first_record > state->records.size()) {
        return false;
    }
    for (size_t i = first_record; i < state->records.size(); ++i) {
        PendingRecord& record = state->records[i];
        if (record.source_occurrence_serial != 0) return false;
        record.source_template_offset = record.template_offset;
        record.source_occurrence_serial = state->occurrence_serial;
    }
    return true;
}

bool AppendPNextShape(const void* chain,
                      std::vector<uint64_t>* structural) {
    if (structural == NULL) return false;
    struct ChainHeader {
        VkStructureType s_type;
        const void* next;
    };
    std::vector<uint64_t> types;
    std::set<const void*> visited;
    const void* cursor = chain;
    while (cursor != NULL) {
        if (types.size() >= 64 || !visited.insert(cursor).second) {
            return false;
        }
        const ChainHeader* header =
            static_cast<const ChainHeader*>(cursor);
        types.push_back(static_cast<uint32_t>(header->s_type));
        cursor = header->next;
    }
    if (structural->size() > kMaxSemanticCalls - 1 ||
        types.size() >
            kMaxSemanticCalls - structural->size() - 1) {
        return false;
    }
    structural->push_back(static_cast<uint64_t>(types.size()));
    structural->insert(structural->end(), types.begin(), types.end());
    return true;
}

bool BuildSubmitCall(
        VkQueue queue,
        uint32_t submit_count,
        const VkSubmitInfo* submits,
        VkFence fence,
        SemanticCall* call,
        std::vector<VkCommandBuffer>* commands) {
    if (call == NULL || commands == NULL ||
        (submit_count != 0 && submits == NULL)) {
        return false;
    }
    const uint64_t prepare_started_ns = NowNs();
    std::vector<uint64_t> structural;
    std::vector<uint64_t> handles;
    structural.push_back(submit_count);
    handles.push_back(HandleBits(queue));
    handles.push_back(HandleBits(fence));
    for (uint32_t i = 0; i < submit_count; ++i) {
        const VkSubmitInfo& submit = submits[i];
        if (submit.sType != VK_STRUCTURE_TYPE_SUBMIT_INFO ||
            (submit.waitSemaphoreCount != 0 &&
             (submit.pWaitSemaphores == NULL ||
              submit.pWaitDstStageMask == NULL)) ||
            (submit.commandBufferCount != 0 &&
             submit.pCommandBuffers == NULL) ||
            (submit.signalSemaphoreCount != 0 &&
             submit.pSignalSemaphores == NULL)) {
            return false;
        }
        if (!AppendPNextShape(submit.pNext, &structural)) return false;
        structural.push_back(submit.waitSemaphoreCount);
        structural.push_back(submit.commandBufferCount);
        structural.push_back(submit.signalSemaphoreCount);
        for (uint32_t j = 0; j < submit.waitSemaphoreCount; ++j) {
            structural.push_back(submit.pWaitDstStageMask[j]);
            handles.push_back(HandleBits(submit.pWaitSemaphores[j]));
        }
        for (uint32_t j = 0; j < submit.commandBufferCount; ++j) {
            commands->push_back(submit.pCommandBuffers[j]);
            handles.push_back(HandleBits(submit.pCommandBuffers[j]));
        }
        for (uint32_t j = 0; j < submit.signalSemaphoreCount; ++j) {
            handles.push_back(HandleBits(submit.pSignalSemaphores[j]));
        }
        if (structural.size() > kMaxSemanticCalls ||
            handles.size() > kMaxSemanticCalls ||
            commands->size() > kMaxSemanticCalls) {
            return false;
        }
    }
    call->fun_id = FUNID_vkQueueSubmit;
    call->structural.swap(structural);
    call->handles.swap(handles);
    call->payload_hash = 0;
    call->encoded_bytes =
        std::max<uint64_t>(
            1, static_cast<uint64_t>(
                call->structural.size() + call->handles.size()) * 8u);
    call->template_offset = 0;
    call->prepare_ns = FinishPrepareNs(prepare_started_ns);
    call->dispatch = false;
    call->execute_secondary = false;
    return true;
}

bool BuildSubmit2Call(
        VkQueue queue,
        uint32_t submit_count,
        const VkSubmitInfo2* submits,
        VkFence fence,
        SemanticCall* call,
        std::vector<VkCommandBuffer>* commands) {
    if (call == NULL || commands == NULL ||
        (submit_count != 0 && submits == NULL)) {
        return false;
    }
    const uint64_t prepare_started_ns = NowNs();
    std::vector<uint64_t> structural;
    std::vector<uint64_t> handles;
    structural.push_back(submit_count);
    handles.push_back(HandleBits(queue));
    handles.push_back(HandleBits(fence));
    for (uint32_t i = 0; i < submit_count; ++i) {
        const VkSubmitInfo2& submit = submits[i];
        if (submit.sType != VK_STRUCTURE_TYPE_SUBMIT_INFO_2 ||
            (submit.waitSemaphoreInfoCount != 0 &&
             submit.pWaitSemaphoreInfos == NULL) ||
            (submit.commandBufferInfoCount != 0 &&
             submit.pCommandBufferInfos == NULL) ||
            (submit.signalSemaphoreInfoCount != 0 &&
             submit.pSignalSemaphoreInfos == NULL)) {
            return false;
        }
        if (!AppendPNextShape(submit.pNext, &structural)) return false;
        structural.push_back(submit.flags);
        structural.push_back(submit.waitSemaphoreInfoCount);
        structural.push_back(submit.commandBufferInfoCount);
        structural.push_back(submit.signalSemaphoreInfoCount);
        for (uint32_t j = 0; j < submit.waitSemaphoreInfoCount; ++j) {
            const VkSemaphoreSubmitInfo& info =
                submit.pWaitSemaphoreInfos[j];
            if (info.sType != VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO ||
                !AppendPNextShape(info.pNext, &structural)) return false;
            structural.push_back(info.value);
            structural.push_back(info.stageMask);
            structural.push_back(info.deviceIndex);
            handles.push_back(HandleBits(info.semaphore));
        }
        for (uint32_t j = 0; j < submit.commandBufferInfoCount; ++j) {
            const VkCommandBufferSubmitInfo& info =
                submit.pCommandBufferInfos[j];
            if (info.sType !=
                    VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO ||
                !AppendPNextShape(info.pNext, &structural)) return false;
            structural.push_back(info.deviceMask);
            commands->push_back(info.commandBuffer);
            handles.push_back(HandleBits(info.commandBuffer));
        }
        for (uint32_t j = 0; j < submit.signalSemaphoreInfoCount; ++j) {
            const VkSemaphoreSubmitInfo& info =
                submit.pSignalSemaphoreInfos[j];
            if (info.sType != VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO ||
                !AppendPNextShape(info.pNext, &structural)) return false;
            structural.push_back(info.value);
            structural.push_back(info.stageMask);
            structural.push_back(info.deviceIndex);
            handles.push_back(HandleBits(info.semaphore));
        }
        if (structural.size() > kMaxSemanticCalls ||
            handles.size() > kMaxSemanticCalls ||
            commands->size() > kMaxSemanticCalls) {
            return false;
        }
    }
    call->fun_id = FUNID_vkQueueSubmit2;
    call->structural.swap(structural);
    call->handles.swap(handles);
    call->payload_hash = 0;
    call->encoded_bytes =
        std::max<uint64_t>(
            1, static_cast<uint64_t>(
                call->structural.size() + call->handles.size()) * 8u);
    call->template_offset = 0;
    call->prepare_ns = FinishPrepareNs(prepare_started_ns);
    call->dispatch = false;
    call->execute_secondary = false;
    return true;
}

/*
 * FrozenCall owns the bytes referenced by its SendParam descriptors, but
 * SimpleVector growth while freeze() copies successive pointer parameters can
 * relocate an earlier backing store.  Rebuild every non-null address from the
 * final storage bases before writeFrozen() sees the call.  The descriptor
 * sizes and exact backing-store totals are also treated as an integrity check:
 * on any disagreement the original call must not be dereferenced or sent.
 */
bool RebaseFrozenCallPointers(ParamManager::FrozenCall* call) {
    if (call == NULL || call->sendBuf.size() < 0 ||
        call->paramStorage.size() < 0 || call->pointerStorage.size() < 0) {
        return false;
    }

    const size_t send_bytes = static_cast<size_t>(call->sendBuf.size());
    const size_t param_bytes =
        static_cast<size_t>(call->paramStorage.size());
    const size_t pointer_bytes =
        static_cast<size_t>(call->pointerStorage.size());
    const size_t header_bytes = 2 * sizeof(uint64_t);
    if (send_bytes < header_bytes || call->sendBuf.data() == NULL ||
        (param_bytes != 0 && call->paramStorage.data() == NULL) ||
        (pointer_bytes != 0 && call->pointerStorage.data() == NULL)) {
        return false;
    }

    uint64_t encoded_fun_id = 0;
    uint64_t descriptor_count = 0;
    memcpy(&encoded_fun_id, call->sendBuf.data(), sizeof(encoded_fun_id));
    memcpy(&descriptor_count,
           call->sendBuf.data() + sizeof(encoded_fun_id),
           sizeof(descriptor_count));
    if (encoded_fun_id != call->realId ||
        descriptor_count >
            (std::numeric_limits<size_t>::max() - header_bytes) /
                sizeof(ParamManager::SendParam)) {
        return false;
    }
    const size_t descriptor_bytes =
        static_cast<size_t>(descriptor_count) *
        sizeof(ParamManager::SendParam);
    if (send_bytes != header_bytes + descriptor_bytes) {
        return false;
    }

    size_t descriptor_index = 0;
    if (param_bytes != 0) {
        if (descriptor_count == 0) return false;
        ParamManager::SendParam descriptor;
        char* slot = call->sendBuf.data() + header_bytes;
        memcpy(&descriptor, slot, sizeof(descriptor));
        if (descriptor.size != param_bytes || descriptor.data == 0) {
            return false;
        }
        descriptor.data = static_cast<uint64_t>(
            reinterpret_cast<uintptr_t>(call->paramStorage.data()));
        memcpy(slot, &descriptor, sizeof(descriptor));
        descriptor_index = 1;
    }

    size_t pointer_offset = 0;
    for (; descriptor_index < descriptor_count; ++descriptor_index) {
        char* slot = call->sendBuf.data() + header_bytes +
                     descriptor_index * sizeof(ParamManager::SendParam);
        ParamManager::SendParam descriptor;
        memcpy(&descriptor, slot, sizeof(descriptor));
        if (descriptor.data == 0) {
            continue;
        }
        if (descriptor.size == 0 || pointer_offset > pointer_bytes ||
            descriptor.size > pointer_bytes - pointer_offset) {
            return false;
        }
        descriptor.data = static_cast<uint64_t>(
            reinterpret_cast<uintptr_t>(
                call->pointerStorage.data() + pointer_offset));
        memcpy(slot, &descriptor, sizeof(descriptor));
        pointer_offset += static_cast<size_t>(descriptor.size);
    }
    return pointer_offset == pointer_bytes;
}

/*
 * Only this audited set may use the payload wildcard.  The call is still sent
 * verbatim; wildcard means merely that payload values do not specialize the
 * descriptor-transfer plan.  All listed calls are descriptor-neutral.
 *
 * The explicit current blacklist is the vkCmdDraw family listed first.  The
 * compute dispatch family, descriptor-set binds and vkCmdExecuteCommands have
 * rich hooks and never reach this path.  Default is deliberately fail-closed
 * so a future push-descriptor, descriptor-buffer bind/offset, ray-tracing,
 * mesh, DGC, or other unclassified descriptor consumer is opaque
 * automatically.
 */
bool CommandAllowsTypeIWildcard(int fun_id) {
    switch (fun_id) {
    case FUNID_vkCmdDraw:
    case FUNID_vkCmdDrawIndexed:
    case FUNID_vkCmdDrawIndirect:
    case FUNID_vkCmdDrawIndexedIndirect:
    case FUNID_vkCmdDrawIndirectCount:
    case FUNID_vkCmdDrawIndexedIndirectCount:
        return false;
    case FUNID_vkCmdBeginRenderPass:
    case FUNID_vkCmdBindVertexBuffers:
    case FUNID_vkCmdSetViewport:
    case FUNID_vkCmdSetScissor:
    case FUNID_vkCmdSetLineWidth:
    case FUNID_vkCmdSetDepthBias:
    case FUNID_vkCmdSetBlendConstants:
    case FUNID_vkCmdSetDepthBounds:
    case FUNID_vkCmdSetStencilCompareMask:
    case FUNID_vkCmdSetStencilWriteMask:
    case FUNID_vkCmdSetStencilReference:
    case FUNID_vkCmdBindIndexBuffer:
    case FUNID_vkCmdCopyBuffer:
    case FUNID_vkCmdCopyImage:
    case FUNID_vkCmdBlitImage:
    case FUNID_vkCmdCopyBufferToImage:
    case FUNID_vkCmdCopyImageToBuffer:
    case FUNID_vkCmdUpdateBuffer:
    case FUNID_vkCmdFillBuffer:
    case FUNID_vkCmdClearColorImage:
    case FUNID_vkCmdClearDepthStencilImage:
    case FUNID_vkCmdClearAttachments:
    case FUNID_vkCmdResolveImage:
    case FUNID_vkCmdSetEvent:
    case FUNID_vkCmdResetEvent:
    case FUNID_vkCmdWaitEvents:
    case FUNID_vkCmdPipelineBarrier:
    case FUNID_vkCmdBeginQuery:
    case FUNID_vkCmdEndQuery:
    case FUNID_vkCmdResetQueryPool:
    case FUNID_vkCmdWriteTimestamp:
    case FUNID_vkCmdCopyQueryPoolResults:
    case FUNID_vkCmdPushConstants:
    case FUNID_vkCmdNextSubpass:
    case FUNID_vkCmdEndRenderPass:
    case FUNID_vkCmdSetDeviceMask:
    case FUNID_vkCmdBeginRenderPass2:
    case FUNID_vkCmdNextSubpass2:
    case FUNID_vkCmdEndRenderPass2:
    case FUNID_vkCmdBeginRendering:
    case FUNID_vkCmdEndRendering:
    case FUNID_vkCmdBindVertexBuffers2:
    case FUNID_vkCmdBlitImage2:
    case FUNID_vkCmdCopyBuffer2:
    case FUNID_vkCmdCopyImage2:
    case FUNID_vkCmdCopyBufferToImage2:
    case FUNID_vkCmdCopyImageToBuffer2:
    case FUNID_vkCmdPipelineBarrier2:
    case FUNID_vkCmdResetEvent2:
    case FUNID_vkCmdResolveImage2:
    case FUNID_vkCmdSetCullMode:
    case FUNID_vkCmdSetDepthBiasEnable:
    case FUNID_vkCmdSetDepthBoundsTestEnable:
    case FUNID_vkCmdSetDepthCompareOp:
    case FUNID_vkCmdSetDepthTestEnable:
    case FUNID_vkCmdSetDepthWriteEnable:
    case FUNID_vkCmdSetEvent2:
    case FUNID_vkCmdSetFrontFace:
    case FUNID_vkCmdSetPrimitiveRestartEnable:
    case FUNID_vkCmdSetPrimitiveTopology:
    case FUNID_vkCmdSetRasterizerDiscardEnable:
    case FUNID_vkCmdSetScissorWithCount:
    case FUNID_vkCmdSetStencilOp:
    case FUNID_vkCmdSetStencilTestEnable:
    case FUNID_vkCmdSetViewportWithCount:
    case FUNID_vkCmdWaitEvents2:
    case FUNID_vkCmdWriteTimestamp2:
        return true;
    default:
        return false;
    }
}

}  // namespace flime_guest_internal
