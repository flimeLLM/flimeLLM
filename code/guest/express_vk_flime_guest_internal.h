/*
 * Shared implementation details for the FLIME guest transport.
 *
 * Public encoder integration belongs in express_vk_flime_guest.h.  This file
 * contains only state and helpers shared by the ordinary implementation units.
 */
#pragma once

#include "express_vk_flime_guest.h"
#include "ParamManager.h"

#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdlib.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace flime_guest_internal {

enum WireType {
    kWireCapabilities = 1,
    kWirePeriodBegin = 2,
    kWireProfilePeriod = 3,
    kWireProgress = 4,
    kWirePlanAck = 5,
    kWireReset = 6,
    kWireTeardown = 7,
    kWireInterval = 8,
};

enum GuestStage {
    kStageDetect,
    kStageLearn,
    kStageMatch,
    kStageFast,
    kStageRecover,
    kStageLegacy,
};

struct LayoutBinding {
    uint32_t binding;
    VkDescriptorType type;
    uint32_t count;
    VkShaderStageFlags stages;
    std::vector<VkSampler> immutable_samplers;
};

struct LayoutState {
    bool supported;
    uint64_t generation;
    std::vector<LayoutBinding> bindings;
};

struct DescriptorValue {
    DescriptorValue()
        : valid(false), type(VK_DESCRIPTOR_TYPE_MAX_ENUM), kind(0),
          buffer(VK_NULL_HANDLE), offset(0), range(0),
          sampler(VK_NULL_HANDLE), image_view(VK_NULL_HANDLE),
          image_layout(VK_IMAGE_LAYOUT_UNDEFINED),
          buffer_view(VK_NULL_HANDLE) {}

    bool valid;
    VkDescriptorType type;
    uint16_t kind;
    VkBuffer buffer;
    VkDeviceSize offset;
    VkDeviceSize range;
    VkSampler sampler;
    VkImageView image_view;
    VkImageLayout image_layout;
    VkBufferView buffer_view;
};

struct DescriptorSlot {
    DescriptorValue value;
    int64_t pending_record;

    DescriptorSlot() : pending_record(-1) {}
};

struct SetState {
    VkDevice device;
    VkDescriptorPool pool;
    bool supported;
    uint64_t generation;
    uint64_t pool_generation;
    LayoutState layout;
    std::map<std::pair<uint32_t, uint32_t>, DescriptorSlot> slots;
};

struct PoolState {
    VkDevice device;
    uint64_t generation;
    std::set<uint64_t> sets;
};

struct TemplateEntry {
    uint32_t binding;
    uint32_t array_element;
    uint32_t count;
    VkDescriptorType type;
    size_t offset;
    size_t stride;
};

struct UpdateTemplateState {
    VkDevice device;
    VkDescriptorSetLayout layout;
    VkDescriptorUpdateTemplateType type;
    bool supported;
    uint64_t generation;
    uint64_t layout_generation;
    std::vector<TemplateEntry> entries;
};

struct SemanticCall {
    SemanticCall()
        : fun_id(0), payload_hash(0), encoded_bytes(0), template_offset(0),
          prepare_ns(0), dispatch(false), execute_secondary(false),
          opaque(false) {}

    int fun_id;
    std::vector<uint64_t> structural;
    std::vector<uint64_t> handles;
    uint64_t payload_hash;
    uint64_t encoded_bytes;
    uint64_t template_offset;
    /*
     * Guest CPU time spent preparing this semantic operation.  This is
     * profiling-only metadata: matching and template signatures deliberately
     * ignore it.
     */
    uint64_t prepare_ns;
    bool dispatch;
    bool execute_secondary;
    /* Identity/size are known, but argument semantics are not certified. */
    bool opaque;
    std::vector<uint64_t> secondary_commands;
};

struct TemplateCall {
    int fun_id;
    std::vector<uint64_t> structural;
    std::vector<uint32_t> handle_roles;
    uint64_t encoded_bytes;
    uint64_t template_offset;
    bool dispatch;
    bool execute_secondary;
};

struct PendingRecord {
    PendingRecord()
        : update_id(0), template_offset(0), source_template_offset(0),
          source_occurrence_serial(0), set(VK_NULL_HANDLE), binding(0),
          array_element(0), type(VK_DESCRIPTOR_TYPE_MAX_ENUM), flags(0),
          elided(false), released(false) {}

    uint64_t update_id;
    uint64_t template_offset;
    /* Immutable origin used when carried records are rebased at submit. */
    uint64_t source_template_offset;
    uint64_t source_occurrence_serial;
    VkDescriptorSet set;
    uint32_t binding;
    uint32_t array_element;
    VkDescriptorType type;
    uint16_t flags;
    DescriptorValue value;
    bool elided;
    bool released;
};

struct FlimeRecoveryCheckpoint {
    FlimeRecoveryCheckpoint()
        : ready(false), shadow_hash(0), next_update_id(0) {}

    void Clear() {
        ready = false;
        shadow_hash = 0;
        next_update_id = 0;
        consumed_sets.clear();
        rebuilt_update_ids.clear();
    }

    bool ready;
    uint64_t shadow_hash;
    uint64_t next_update_id;
    std::set<uint64_t> consumed_sets;
    std::set<uint64_t> rebuilt_update_ids;
};

enum FlimeFastDescriptorCallKind {
    kFlimeFastUpdateSets,
    kFlimeFastUpdateTemplate,
};

enum FlimeFastValueSource {
    kFlimeFastRawWrite,
    kFlimeFastTemplateData,
    kFlimeFastShadowSlot,
    kFlimeFastPriorMutation,
};

enum FlimeFastApplyResult {
    kFlimeFastApplied,
    kFlimeFastMiss,
};

struct FlimeFastSetGuard {
    uint32_t occurrence_role;
    uint64_t set;
    uint64_t set_generation;
    uint64_t layout_generation;
    uint64_t pool;
    uint64_t pool_generation;
    std::vector<LayoutBinding> layout_bindings;
};

struct FlimeFastBoundSet {
    uint64_t set;
    uint64_t set_generation;
    uint64_t layout_generation;
    uint64_t pool;
    uint64_t pool_generation;
};

struct FlimeFastAddress {
    uint32_t set_guard;
    uint32_t binding;
    uint32_t element;
};

struct FlimeFastWriteShape {
    uint32_t set_guard;
    uint32_t binding;
    uint32_t array_element;
    uint32_t count;
    VkDescriptorType type;
};

struct FlimeFastCopyShape {
    uint32_t source_guard;
    uint32_t destination_guard;
    uint32_t source_binding;
    uint32_t source_array_element;
    uint32_t destination_binding;
    uint32_t destination_array_element;
    uint32_t count;
};

struct FlimeFastRouteOp {
    FlimeFastAddress destination;
    VkDescriptorType type;
    uint16_t route_flags;
    FlimeFastValueSource source;
    uint32_t source_outer;
    uint32_t source_inner;
    FlimeFastAddress source_slot;
    size_t template_data_offset;
    bool has_immutable_sampler;
    VkSampler immutable_sampler;
};

struct FlimeFastDescriptorCall {
    FlimeFastDescriptorCall()
        : kind(kFlimeFastUpdateSets), fun_id(0), semantic_call_index(0),
          template_offset(0), encoded_bytes(0), write_count(0), copy_count(0),
          template_set_guard(0), update_template(0),
          update_template_generation(0), template_layout_generation(0) {}

    FlimeFastDescriptorCallKind kind;
    int fun_id;
    size_t semantic_call_index;
    uint64_t template_offset;
    uint64_t encoded_bytes;
    uint32_t write_count;
    uint32_t copy_count;
    uint32_t template_set_guard;
    uint64_t update_template;
    uint64_t update_template_generation;
    uint64_t template_layout_generation;
    std::vector<FlimeFastSetGuard> set_guards;
    std::vector<FlimeFastWriteShape> writes;
    std::vector<FlimeFastCopyShape> copies;
    std::vector<FlimeFastRouteOp> operations;
};

struct UnitSample {
    uint32_t index;
    uint32_t flags;
    uint32_t dispatch_end;
    uint64_t template_offset;
    uint64_t encoded_bytes;
    uint64_t prepare_ns;
};

struct ChunkSample {
    uint32_t index;
    uint32_t first_unit;
    uint32_t unit_past_end;
    uint64_t handoff_ns;
};

struct PlanBoundary {
    uint32_t unit_past_end;
    uint32_t flags;
    uint64_t template_offset;
};

struct CommandState {
    VkDevice device;
    VkCommandPool pool;
    uint64_t generation;
    bool recording;
    bool executable;
};

struct DeviceState {
    DeviceState()
        : fd(-1), device(VK_NULL_HANDLE), process_id(0), stream_id(0),
          negotiated(false), stage(kStageDetect), capabilities(0),
          max_units(0), max_chunks(0), dispatches_per_unit(0),
          replan_periods(0), control_page(NULL), control_sequence(0),
          control_allocation_bytes(0), control_page_exposed(false),
          teardown_complete(false),
          queue(VK_NULL_HANDLE), next_period_id(1), next_submission_id(1),
          next_update_id(1), next_descriptor_generation(1),
          occurrence_serial(1),
          template_bytes(0), interval_hash(0),
          learned_signature(0), learned_template_bytes(0),
          learned_global_call_count(0), learned_record_count(0),
          dispatch_count(0),
          plan_epoch(0), plan_apply_period(0),
          plan_valid(false), request_fine_profile(true),
          acked_plan_epoch(0), interval_announced(false),
          session_invalidated(false), transport_failed(false),
          lifecycle_inflight(0),
          active_period_id(0), active_period_flags(0),
          active_submission_id(0), active_chunk_count(0),
          active_plan_epoch(0), active_submit(false),
          active_recovery(false), active_learning(false),
          active_fast(false), active_token(0),
          active_template_bytes(0), active_signature(0),
          active_dispatches(0),
          early_route_ready(false), early_recovery(false),
          early_period_id(0), early_submission_id(0),
          early_period_flags(0), early_plan_epoch(0),
          early_start_ns(0), early_next_chunk(0),
          early_command_buffer(0), early_queue(0), early_signature(0),
          early_template_bytes(0), early_dispatches(0),
          early_descriptor_plan_cursor(0),
          period_start_ns(0), mismatch_pending(false),
          generic_shadow_ran(false), descriptor_plan_cursor(0),
          building_descriptor_cache_complete(true),
          active_descriptor_cache_complete(false),
          learned_descriptor_cache_complete(false),
          active_generic_shadow_ran(false), fast_periods(0) {}

    ~DeviceState() {
        /*
         * A failed synchronous teardown cannot prove that an asynchronous
         * planner no longer owns the DMA address.  Keep that rare allocation
         * pinned until process exit instead of risking a host write-after-free.
         */
        if (!control_page_exposed || teardown_complete) {
            free(control_page);
        }
    }

    int fd;
    VkDevice device;
    uint64_t process_id;
    uint64_t stream_id;
    bool negotiated;
    GuestStage stage;
    uint64_t capabilities;
    uint32_t max_units;
    uint32_t max_chunks;
    uint32_t dispatches_per_unit;
    uint32_t replan_periods;
    uint8_t* control_page;
    uint64_t control_sequence;
    size_t control_allocation_bytes;
    bool control_page_exposed;
    bool teardown_complete;
    VkQueue queue;
    std::set<uint64_t> registered_queues;

    uint64_t next_period_id;
    uint64_t next_submission_id;
    uint64_t next_update_id;
    uint64_t next_descriptor_generation;
    uint64_t occurrence_serial;
    std::vector<uint64_t> interval_history;
    std::vector<SemanticCall> calls;
    std::vector<TemplateCall> learned;
    uint64_t template_bytes;
    uint64_t interval_hash;
    uint64_t learned_signature;
    uint64_t learned_template_bytes;
    uint32_t learned_global_call_count;
    uint32_t learned_record_count;
    std::vector<UnitSample> learned_units;
    std::vector<uint64_t> learned_primary_commands;
    SemanticCall learned_submit_call;
    uint32_t dispatch_count;
    std::vector<PendingRecord> records;

    std::unordered_map<uint64_t, LayoutState> layouts;
    std::unordered_map<uint64_t, PoolState> pools;
    std::unordered_map<uint64_t, SetState> sets;
    std::unordered_map<uint64_t, UpdateTemplateState> update_templates;
    std::unordered_map<uint64_t, CommandState> commands;

    uint64_t plan_epoch;
    uint64_t plan_apply_period;
    bool plan_valid;
    bool request_fine_profile;
    std::vector<PlanBoundary> plan;
    uint64_t acked_plan_epoch;
    bool interval_announced;
    bool session_invalidated;
    bool transport_failed;
    uint64_t lifecycle_inflight;

    uint64_t active_period_id;
    uint32_t active_period_flags;
    uint64_t active_submission_id;
    uint32_t active_chunk_count;
    uint64_t active_plan_epoch;
    bool active_submit;
    bool active_recovery;
    bool active_learning;
    bool active_fast;
    uint64_t active_token;
    uint64_t active_template_bytes;
    uint64_t active_signature;
    uint32_t active_dispatches;
    std::vector<SemanticCall> active_calls;
    std::vector<uint64_t> active_primary_commands;
    std::set<uint64_t> active_consumed_descriptor_sets;
    std::set<uint64_t> active_frontier_update_ids;
    bool early_route_ready;
    bool early_recovery;
    uint64_t early_period_id;
    uint64_t early_submission_id;
    uint32_t early_period_flags;
    uint64_t early_plan_epoch;
    uint64_t early_start_ns;
    uint32_t early_next_chunk;
    uint64_t early_command_buffer;
    uint64_t early_queue;
    uint64_t early_signature;
    uint64_t early_template_bytes;
    uint32_t early_dispatches;
    size_t early_descriptor_plan_cursor;
    std::vector<SemanticCall> early_calls;
    /*
     * Depth-first command graph captured when the non-final prefix is sent.
     * The flattened semantic stream checks topology and arguments; this
     * parallel snapshot makes command-buffer lifetime/generation part of the
     * QueueSubmit commit guard as well.
     */
    std::vector<std::pair<uint64_t, uint64_t> > early_command_graph;
    std::set<uint64_t> early_consumed_descriptor_sets;
    std::set<uint64_t> early_frontier_update_ids;
    std::vector<UnitSample> early_units;
    std::vector<ChunkSample> early_chunks;
    uint64_t period_start_ns;
    std::vector<UnitSample> units;
    std::vector<ChunkSample> chunks;
    bool mismatch_pending;
    bool generic_shadow_ran;
    FlimeRecoveryCheckpoint recovery_checkpoint;
    size_t descriptor_plan_cursor;
    bool building_descriptor_cache_complete;
    bool active_descriptor_cache_complete;
    bool learned_descriptor_cache_complete;
    bool active_generic_shadow_ran;
    std::vector<FlimeFastDescriptorCall> building_descriptor_plans;
    std::vector<FlimeFastDescriptorCall> active_descriptor_plans;
    std::vector<FlimeFastDescriptorCall> learned_descriptor_plans;
    std::map<uint32_t, FlimeFastBoundSet> descriptor_role_bindings;
    std::map<uint64_t, uint32_t> descriptor_handle_roles;
    uint64_t fast_periods;
};

struct OwnedCluster {
    OwnedCluster() : fd(-1), call_count(0) {
        payload.push_back(0);
    }

    void Clear() {
        fd = -1;
        call_count = 0;
        calls.clear();
        payload.clear();
        payload.push_back(0);
    }

    bool Empty() const { return call_count == 0; }

    int fd;
    uint64_t call_count;
    std::vector<uint8_t> calls;
    std::vector<uint8_t> payload;
};

struct RecordedCommandStream {
    std::vector<SemanticCall> calls;
    uint64_t bytes;
    uint32_t dispatches;
    uint64_t generation;

    RecordedCommandStream() : bytes(0), dispatches(0), generation(0) {}
};


enum ProtocolPrepareResult {
    kProtocolLegacy = 0,
    kProtocolReady = 1,
    kProtocolBlocked = 2,
    kProtocolFallbackLegacy = 3,
};

extern const uint32_t kMaxRouteRecords;
extern const size_t kMaxSemanticCalls;
extern const uint64_t kMaxTemplateBytes;

extern std::mutex g_mutex;
extern std::condition_variable g_submit_cv;
extern std::unordered_map<uint64_t, std::shared_ptr<DeviceState> > g_devices;
extern std::unordered_map<uint64_t, uint64_t> g_queue_devices;
extern std::unordered_map<uint64_t, uint64_t> g_command_devices;
extern std::unordered_map<uint64_t, uint64_t> g_set_devices;
extern std::unordered_map<uint64_t, uint64_t> g_pool_devices;
extern OwnedCluster g_cluster;
extern std::unordered_map<uint64_t, RecordedCommandStream> g_recorded_commands;

extern thread_local int g_skip_next_fun_id;
extern thread_local bool g_allow_next_cluster;
extern thread_local bool g_block_next_write;
extern thread_local bool g_flush_cluster_after_write;
extern thread_local uint64_t g_route_after_write_command;
extern thread_local uint64_t g_named_write_command;

uint64_t NowNs();
uint64_t FinishPrepareNs(uint64_t started_ns);

template <typename T>
inline uint64_t HandleBits(T value) {
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(value));
}

template <>
inline uint64_t HandleBits<uint64_t>(uint64_t value) {
    return value;
}

uint64_t HashWord(uint64_t hash, uint64_t value);
std::shared_ptr<DeviceState> FindDeviceLocked(VkDevice device);
std::shared_ptr<DeviceState> FindByMappedKeyLocked(
    const std::unordered_map<uint64_t, uint64_t>& mapping,
    uint64_t object);
std::shared_ptr<DeviceState> FindQueueLocked(VkQueue queue);
std::shared_ptr<DeviceState> FindCommandLocked(
    VkCommandBuffer command_buffer);

bool IsSpecialized(const DeviceState& state);
bool FlushAnyClusterLocked();
void ClearOccurrence(DeviceState* state,
                     bool preserve_descriptor_records = false);
void ClearActiveSubmit(DeviceState* state);
void EnterLegacy(DeviceState* state);
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
                    bool dispatch);
bool AppendCommandSemanticLocked(VkCommandBuffer command_buffer,
                                 int fun_id,
                                 const uint64_t* structural,
                                 size_t structural_count,
                                 const uint64_t* handles,
                                 size_t handle_count,
                                 uint64_t payload_hash,
                                 uint64_t encoded_bytes,
                                 bool dispatch);
bool AppendExecuteSemanticLocked(VkCommandBuffer command_buffer,
                                 int fun_id,
                                 uint32_t command_count,
                                 const VkCommandBuffer* commands,
                                 uint64_t encoded_bytes);
void MarkPreWriteSemantic(int fun_id, bool allow_cluster);

bool NegotiateCapabilitiesLocked(DeviceState* state);
bool FallbackToLegacyLocked(DeviceState* state,
                            uint64_t* drain_submission_id);
bool PrepareFallbackLegacySubmitLocked(DeviceState* state,
                                       uint64_t* token);
bool MaybeRouteFastPrefixLocked(DeviceState* state);
bool IsFastPlanningBoundaryLocked(DeviceState* state,
                                  VkCommandBuffer command_buffer,
                                  uint32_t dispatch_count);
bool MaybeAdvanceFastRouteLocked(DeviceState* state,
                                 uint64_t command_key);
ProtocolPrepareResult ProtocolBeforeSubmitLocked(
    DeviceState* state,
    const std::vector<VkCommandBuffer>& submitted,
    const SemanticCall& submit_call,
    uint64_t* token);
bool ProtocolAfterSubmitLocked(DeviceState* state,
                               uint64_t token,
                               bool transport_ok);

uint64_t NextStreamIdLocked(uint64_t process_id, uint64_t device_key);
bool AllocateControlPage(DeviceState* state);
void RemoveDeviceMappingsLocked(const DeviceState& state);
bool SendSessionPacketLocked(DeviceState* state, WireType type);
bool EnsureNegotiatedLocked(DeviceState* state);
size_t LiveRecordCount(const DeviceState& state);
void FailNamedWriteLocked(DeviceState* state, int fun_id);
bool IsTypeIClusterFunction(int fun_id);
bool RebaseFrozenCallPointers(ParamManager::FrozenCall* call);
ssize_t AppendOwnedClusterLocked(int fd,
                                 ParamManager* manager,
                                 ParamManager::FrozenCall* call);
bool DrainEarlyRouteLocked(DeviceState* state);
bool DrainEarlyRoutesBeforeRawCallLocked(int fun_id);
bool RecordCommandCallLocked(int fun_id,
                             VkCommandBuffer command_buffer,
                             const uint64_t* structural,
                             size_t structural_count,
                             const uint64_t* handles,
                             size_t handle_count,
                             uint64_t payload_hash,
                             uint64_t encoded_bytes,
                             bool dispatch,
                             bool cluster_safe);
uint64_t DescriptorPayloadHash(uint32_t write_count,
                               const VkWriteDescriptorSet* writes,
                               uint32_t copy_count,
                               const VkCopyDescriptorSet* copies);
bool BuildDescriptorStructural(uint32_t write_count,
                               const VkWriteDescriptorSet* writes,
                               uint32_t copy_count,
                               const VkCopyDescriptorSet* copies,
                               std::vector<uint64_t>* structural,
                               std::vector<uint64_t>* handles);
FlimeGuestUpdateAction UpdateFailureActionLocked(DeviceState* state,
                                                 bool specialized);
void NotePotentialDivergenceLocked(DeviceState* state);
void NoteEarlyCommandMutationLocked(DeviceState* state);
bool StampNewRecordOriginsLocked(DeviceState* state,
                                 size_t first_record);
bool BuildSubmitCall(VkQueue queue,
                     uint32_t submit_count,
                     const VkSubmitInfo* submits,
                     VkFence fence,
                     SemanticCall* call,
                     std::vector<VkCommandBuffer>* commands);
bool BuildSubmit2Call(VkQueue queue,
                      uint32_t submit_count,
                      const VkSubmitInfo2* submits,
                      VkFence fence,
                      SemanticCall* call,
                      std::vector<VkCommandBuffer>* commands);
bool CommandAllowsTypeIWildcard(int fun_id);

bool FlimeShadowCreateLayout(
    DeviceState* device,
    VkDescriptorSetLayout layout,
    const VkDescriptorSetLayoutCreateInfo* info);
bool FlimeShadowDestroyLayout(DeviceState* device,
                              VkDescriptorSetLayout layout);
bool FlimeShadowCreatePool(DeviceState* device,
                           VkDevice owner,
                           VkDescriptorPool pool);
bool FlimeShadowDestroyPool(DeviceState* device,
                            VkDescriptorPool pool,
                            std::vector<uint64_t>* removed_sets);
bool FlimeShadowResetPool(DeviceState* device,
                          VkDescriptorPool pool,
                          std::vector<uint64_t>* removed_sets);
bool FlimeShadowAllocateSets(DeviceState* device,
                             VkDevice owner,
                             VkDescriptorPool pool,
                             uint32_t count,
                             const VkDescriptorSetLayout* layouts,
                             const VkDescriptorSet* sets);
bool FlimeShadowFreeSets(DeviceState* device,
                         VkDescriptorPool pool,
                         uint32_t count,
                         const VkDescriptorSet* sets,
                         std::vector<uint64_t>* removed_sets);
bool FlimeShadowCreateUpdateTemplate(
    DeviceState* device,
    VkDevice owner,
    VkDescriptorUpdateTemplate update_template,
    const VkDescriptorUpdateTemplateCreateInfo* info);
bool FlimeShadowDestroyUpdateTemplate(
    DeviceState* device,
    VkDescriptorUpdateTemplate update_template);
bool FlimeCountDescriptorRecords(uint32_t write_count,
                                 const VkWriteDescriptorSet* writes,
                                 uint32_t copy_count,
                                 const VkCopyDescriptorSet* copies,
                                 uint64_t* out);
bool FlimeShadowUpdateDescriptorSets(
    DeviceState* device,
    uint64_t template_offset,
    uint32_t write_count,
    const VkWriteDescriptorSet* writes,
    uint32_t copy_count,
    const VkCopyDescriptorSet* copies,
    bool capture_records);
bool FlimeCountTemplateRecords(
    const DeviceState* device,
    VkDescriptorUpdateTemplate update_template,
    uint64_t* out);
bool FlimeShadowUpdateWithTemplate(
    DeviceState* device,
    uint64_t template_offset,
    VkDescriptorSet set,
    VkDescriptorUpdateTemplate update_template,
    const void* data,
    bool capture_records);
bool FlimeCompileDescriptorSetFastPlan(
    const DeviceState* device,
    int fun_id,
    size_t semantic_call_index,
    uint64_t template_offset,
    uint64_t encoded_bytes,
    uint32_t write_count,
    const VkWriteDescriptorSet* writes,
    uint32_t copy_count,
    const VkCopyDescriptorSet* copies,
    FlimeFastDescriptorCall* out);
bool FlimeCompileDescriptorTemplateFastPlan(
    const DeviceState* device,
    int fun_id,
    size_t semantic_call_index,
    uint64_t template_offset,
    uint64_t encoded_bytes,
    VkDescriptorSet set,
    VkDescriptorUpdateTemplate update_template,
    const void* data,
    FlimeFastDescriptorCall* out);
bool FlimeFastPlansTopologyEquivalent(
    const FlimeFastDescriptorCall& left,
    const FlimeFastDescriptorCall& right);
FlimeFastApplyResult FlimeApplyDescriptorSetFastPlan(
    DeviceState* device,
    const FlimeFastDescriptorCall& plan,
    int fun_id,
    size_t semantic_call_index,
    uint64_t template_offset,
    uint64_t encoded_bytes,
    uint32_t write_count,
    const VkWriteDescriptorSet* writes,
    uint32_t copy_count,
    const VkCopyDescriptorSet* copies,
    bool capture_records);
FlimeFastApplyResult FlimeApplyDescriptorTemplateFastPlan(
    DeviceState* device,
    const FlimeFastDescriptorCall& plan,
    int fun_id,
    size_t semantic_call_index,
    uint64_t template_offset,
    uint64_t encoded_bytes,
    VkDescriptorSet set,
    VkDescriptorUpdateTemplate update_template,
    const void* data,
    bool capture_records);
bool FlimeShadowInvalidatePayloadObject(DeviceState* device,
                                        VkObjectType object_type,
                                        uint64_t object,
                                        bool* changed);

}  // namespace flime_guest_internal
