/* Descriptor lifecycle, shadowing, and update hooks. */
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

using namespace flime_guest_internal;

namespace {

void DisableDeviceFlimeLocked(DeviceState* state) {
    if (state == NULL || state->stage == kStageLegacy) return;
    if (state->active_submit) {
        state->transport_failed = true;
        return;
    }
    if (IsSpecialized(*state)) {
        if (!FallbackToLegacyLocked(state, NULL)) {
            state->transport_failed = true;
        }
        return;
    }
    if (state->negotiated &&
        !SendSessionPacketLocked(state, kWireTeardown)) {
        state->transport_failed = true;
        return;
    }
    state->teardown_complete = true;
    state->control_page_exposed = false;
    EnterLegacy(state);
    state->transport_failed = false;
}

}  // namespace

void FlimeGuestBeforeDescriptorLifecycle(VkDevice device) {
    std::unique_lock<std::mutex> lock(g_mutex);
    std::shared_ptr<DeviceState> state = FindDeviceLocked(device);
    if (!state) return;
    while (state->active_submit) {
        g_submit_cv.wait(lock);
    }
    if (!DrainEarlyRouteLocked(state.get())) {
        ++state->lifecycle_inflight;
        return;
    }
    ++state->lifecycle_inflight;
}

bool FlimeGuestPrepareDescriptorRetirement(VkDevice device) {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::shared_ptr<DeviceState> state = FindDeviceLocked(device);
    if (!state || !IsSpecialized(*state) ||
        (!state->early_route_ready && LiveRecordCount(*state) == 0)) {
        return true;
    }

    /*
     * Recovery and fallback packets retain already-released records so the
     * host ledger can suppress them.  The host still has to decode their
     * guest handles before that suppression decision, so retire no referenced
     * handle until a synchronous fallback flush has removed the session.
     * The caller's lifecycle reservation keeps QueueSubmit from overtaking
     * this drain and the following raw retirement RPC.
     */
    if (state->transport_failed ||
        !FallbackToLegacyLocked(state.get(), NULL)) {
        state->transport_failed = true;
        return false;
    }
    return true;
}

void FlimeGuestAfterDescriptorLifecycle(VkDevice device,
                                        bool transport_ok) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        std::shared_ptr<DeviceState> state = FindDeviceLocked(device);
        if (state) {
            if (!transport_ok) {
                state->transport_failed = true;
            }
            if (state->lifecycle_inflight != 0) {
                --state->lifecycle_inflight;
            }
        }
    }
    g_submit_cv.notify_all();
}

void FlimeGuestCreateDescriptorSetLayout(
        VkDevice device,
        const VkDescriptorSetLayoutCreateInfo* create_info,
        VkDescriptorSetLayout layout,
        VkResult result) {
    if (result != VK_SUCCESS) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    std::shared_ptr<DeviceState> state = FindDeviceLocked(device);
    if (!state) return;
    if (!FlimeShadowCreateLayout(state.get(), layout, create_info)) {
        DisableDeviceFlimeLocked(state.get());
    }
}

void FlimeGuestDestroyDescriptorSetLayout(
        VkDevice device,
        VkDescriptorSetLayout layout) {
    std::unique_lock<std::mutex> lock(g_mutex);
    std::shared_ptr<DeviceState> state = FindDeviceLocked(device);
    if (!state) return;
    NotePotentialDivergenceLocked(state.get());
    if (!FlimeShadowDestroyLayout(state.get(), layout) &&
        state->stage != kStageLegacy) {
        DisableDeviceFlimeLocked(state.get());
    }
}

void FlimeGuestCreateDescriptorPool(
        VkDevice device,
        const VkDescriptorPoolCreateInfo* create_info,
        VkDescriptorPool pool,
        VkResult result) {
    (void)create_info;
    if (result != VK_SUCCESS) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    std::shared_ptr<DeviceState> state = FindDeviceLocked(device);
    const uint64_t pool_key = HandleBits(pool);
    if (!state || pool_key == 0) return;
    if (!FlimeShadowCreatePool(state.get(), device, pool) ||
        (g_pool_devices.find(pool_key) != g_pool_devices.end() &&
         g_pool_devices[pool_key] != HandleBits(device))) {
        DisableDeviceFlimeLocked(state.get());
        return;
    }
    g_pool_devices[pool_key] = HandleBits(device);
}

void FlimeGuestDestroyDescriptorPool(
        VkDevice device,
        VkDescriptorPool pool) {
    std::unique_lock<std::mutex> lock(g_mutex);
    std::shared_ptr<DeviceState> state = FindDeviceLocked(device);
    if (!state) return;
    FlushAnyClusterLocked();
    NotePotentialDivergenceLocked(state.get());
    std::vector<uint64_t> removed_sets;
    if (!FlimeShadowDestroyPool(state.get(), pool, &removed_sets)) {
        if (state->stage != kStageLegacy) {
            DisableDeviceFlimeLocked(state.get());
        }
        return;
    }
    for (size_t i = 0; i < removed_sets.size(); ++i) {
        g_set_devices.erase(removed_sets[i]);
    }
    g_pool_devices.erase(HandleBits(pool));
}

void FlimeGuestAllocateDescriptorSets(
        VkDevice device,
        const VkDescriptorSetAllocateInfo* allocate_info,
        const VkDescriptorSet* sets,
        VkResult result) {
    if (result != VK_SUCCESS || allocate_info == NULL) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    std::shared_ptr<DeviceState> state = FindDeviceLocked(device);
    if (!state) return;
    if (!FlimeShadowAllocateSets(
            state.get(), device, allocate_info->descriptorPool,
            allocate_info->descriptorSetCount,
            allocate_info->pSetLayouts, sets)) {
        DisableDeviceFlimeLocked(state.get());
        return;
    }
    for (uint32_t i = 0; i < allocate_info->descriptorSetCount; ++i) {
        const uint64_t set_key = HandleBits(sets[i]);
        g_set_devices[set_key] = HandleBits(device);
        if (allocate_info->pNext != NULL) {
            state->sets[set_key].supported = false;
        }
    }
}

void FlimeGuestFreeDescriptorSets(
        VkDevice device,
        VkDescriptorPool pool,
        uint32_t count,
        const VkDescriptorSet* sets,
        VkResult result) {
    if (result != VK_SUCCESS) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    std::shared_ptr<DeviceState> state = FindDeviceLocked(device);
    if (!state) return;
    NotePotentialDivergenceLocked(state.get());
    std::vector<uint64_t> removed_sets;
    if (!FlimeShadowFreeSets(
            state.get(), pool, count, sets, &removed_sets)) {
        if (state->stage != kStageLegacy) {
            DisableDeviceFlimeLocked(state.get());
        }
        return;
    }
    for (size_t i = 0; i < removed_sets.size(); ++i) {
        g_set_devices.erase(removed_sets[i]);
    }
}

void FlimeGuestResetDescriptorPool(VkDevice device,
                                   VkDescriptorPool pool,
                                   VkResult result) {
    if (result != VK_SUCCESS) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    std::shared_ptr<DeviceState> state = FindDeviceLocked(device);
    if (!state) return;
    NotePotentialDivergenceLocked(state.get());
    std::vector<uint64_t> removed_sets;
    if (!FlimeShadowResetPool(state.get(), pool, &removed_sets)) {
        if (state->stage != kStageLegacy) {
            DisableDeviceFlimeLocked(state.get());
        }
        return;
    }
    for (size_t i = 0; i < removed_sets.size(); ++i) {
        g_set_devices.erase(removed_sets[i]);
    }
}

void FlimeGuestCreateDescriptorUpdateTemplate(
        VkDevice device,
        const VkDescriptorUpdateTemplateCreateInfo* create_info,
        VkDescriptorUpdateTemplate descriptor_update_template,
        VkResult result) {
    if (result != VK_SUCCESS) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    std::shared_ptr<DeviceState> state = FindDeviceLocked(device);
    if (!state) return;
    if (!FlimeShadowCreateUpdateTemplate(
            state.get(), device, descriptor_update_template, create_info)) {
        DisableDeviceFlimeLocked(state.get());
    }
}

void FlimeGuestDestroyDescriptorUpdateTemplate(
        VkDevice device,
        VkDescriptorUpdateTemplate descriptor_update_template) {
    std::unique_lock<std::mutex> lock(g_mutex);
    std::shared_ptr<DeviceState> state = FindDeviceLocked(device);
    if (!state) return;
    NotePotentialDivergenceLocked(state.get());
    if (!FlimeShadowDestroyUpdateTemplate(
            state.get(), descriptor_update_template) &&
        state->stage != kStageLegacy) {
        DisableDeviceFlimeLocked(state.get());
    }
}

void FlimeGuestDestroyDescriptorPayload(VkDevice device,
                                        VkObjectType object_type,
                                        uint64_t object) {
    std::unique_lock<std::mutex> lock(g_mutex);
    std::shared_ptr<DeviceState> state = FindDeviceLocked(device);
    if (!state) return;
    bool changed = false;
    if (!FlimeShadowInvalidatePayloadObject(
            state.get(), object_type, object, &changed)) {
        if (state->stage != kStageLegacy) {
            DisableDeviceFlimeLocked(state.get());
        }
        return;
    }
    if (changed && IsSpecialized(*state)) {
        state->mismatch_pending = true;
    }
}

static const FlimeFastDescriptorCall *FlimeExpectedDescriptorPlanLocked(
        const DeviceState *state, size_t position) {
    if (state == NULL || !state->learned_descriptor_cache_complete ||
        position >= state->learned_descriptor_plans.size()) {
        return NULL;
    }
    return &state->learned_descriptor_plans[position];
}

static void FlimeRememberDescriptorPlanLocked(
        DeviceState *state, GuestStage stage, size_t position,
        bool compiled, const FlimeFastDescriptorCall &plan) {
    if (state == NULL || (stage != kStageLearn && stage != kStageMatch)) {
        return;
    }
    if (!compiled) {
        state->building_descriptor_cache_complete = false;
        if (stage == kStageMatch) state->mismatch_pending = true;
        return;
    }
    if (stage == kStageMatch) {
        const FlimeFastDescriptorCall *expected =
            FlimeExpectedDescriptorPlanLocked(state, position);
        if (expected == NULL ||
            !FlimeFastPlansTopologyEquivalent(*expected, plan)) {
            state->building_descriptor_cache_complete = false;
            state->mismatch_pending = true;
        }
    }
    state->building_descriptor_plans.push_back(plan);
}

FlimeGuestUpdateAction FlimeGuestUpdateDescriptorSets(
        int fun_id,
        VkDevice device,
        uint32_t write_count,
        const VkWriteDescriptorSet* writes,
        uint32_t copy_count,
        const VkCopyDescriptorSet* copies,
        uint64_t encoded_bytes) {
    std::unique_lock<std::mutex> lock(g_mutex);
    std::shared_ptr<DeviceState> state = FindDeviceLocked(device);
    if (!state) return FLIME_GUEST_UPDATE_LEGACY;
    while (state->active_submit) {
        g_submit_cv.wait(lock);
    }
    if (state->transport_failed) {
        return UpdateFailureActionLocked(state.get(), true);
    }
    if (state->early_route_ready) {
        /*
         * A non-final prefix has transferred ownership of the frozen frontier
         * to the host.  Drain that exact ledger before admitting a later
         * descriptor mutation; the new call then executes once on the legacy
         * path and cannot be folded into an already-started submission.
         */
        if (!DrainEarlyRouteLocked(state.get())) {
            return FLIME_GUEST_UPDATE_FATAL;
        }
        MarkPreWriteSemantic(fun_id, false);
        return FLIME_GUEST_UPDATE_LEGACY;
    }
    const uint64_t prepare_started_ns = NowNs();
    const bool specialized = IsSpecialized(*state);
    uint64_t record_count = 0;
    if (!FlimeCountDescriptorRecords(
            write_count, writes, copy_count, copies, &record_count)) {
        return UpdateFailureActionLocked(state.get(), specialized);
    }
    const uint64_t normalized_bytes =
        std::max<uint64_t>(std::max<uint64_t>(encoded_bytes, 1),
                           record_count + 1);
    if (state->calls.size() >= kMaxSemanticCalls ||
        state->template_bytes > kMaxTemplateBytes - normalized_bytes ||
        (specialized &&
         (record_count > kMaxRouteRecords ||
          LiveRecordCount(*state) >
              kMaxRouteRecords - static_cast<size_t>(record_count)))) {
        return UpdateFailureActionLocked(state.get(), specialized);
    }
    const size_t saved_call_count = state->calls.size();
    const size_t saved_record_count = state->records.size();
    const uint64_t call_offset = state->template_bytes;
    const uint32_t saved_dispatch_count = state->dispatch_count;
    const GuestStage descriptor_stage = state->stage;
    const size_t plan_position = state->descriptor_plan_cursor;
    if (state->stage != kStageLegacy) {
        std::vector<uint64_t> structural;
        std::vector<uint64_t> handles;
        if (!BuildDescriptorStructural(
                write_count, writes, copy_count, copies,
                &structural, &handles)) {
            return UpdateFailureActionLocked(state.get(), specialized);
        }
        const uint64_t payload_hash = DescriptorPayloadHash(
            write_count, writes, copy_count, copies);
        if (!AppendSemantic(
                &state->calls, &state->template_bytes,
                &state->dispatch_count, fun_id,
                structural.empty() ? NULL : &structural[0],
                structural.size(),
                handles.empty() ? NULL : &handles[0],
                handles.size(), payload_hash,
                normalized_bytes, false)) {
            return UpdateFailureActionLocked(state.get(), specialized);
        }
    }
    FlimeFastDescriptorCall compiled_plan;
    const bool compile_plan =
        descriptor_stage == kStageLearn ||
        descriptor_stage == kStageMatch;
    const bool plan_compiled = !compile_plan ||
        FlimeCompileDescriptorSetFastPlan(
            state.get(), fun_id, saved_call_count, call_offset,
            normalized_bytes, write_count, writes, copy_count, copies,
            &compiled_plan);
    bool used_fast = false;
    if (descriptor_stage == kStageFast &&
        !state->mismatch_pending) {
        const FlimeFastDescriptorCall *expected =
            FlimeExpectedDescriptorPlanLocked(state.get(), plan_position);
        used_fast = expected != NULL &&
            FlimeApplyDescriptorSetFastPlan(
                state.get(), *expected, fun_id, saved_call_count,
                call_offset, normalized_bytes, write_count, writes,
                copy_count, copies, specialized) == kFlimeFastApplied;
        if (!used_fast) {
            /*
             * The compiled executor is validate-before-commit.  Generic shadow
             * can therefore capture the exact call for Recover without
             * duplicating any mutation.
             */
            state->mismatch_pending = true;
            state->building_descriptor_cache_complete = false;
        }
    }
    if (!used_fast) {
        if (!FlimeShadowUpdateDescriptorSets(
                state.get(), call_offset, write_count, writes,
                copy_count, copies, specialized)) {
            state->calls.resize(saved_call_count);
            state->template_bytes = call_offset;
            state->dispatch_count = saved_dispatch_count;
            return UpdateFailureActionLocked(state.get(), specialized);
        }
        state->generic_shadow_ran = true;
    }
    if (!StampNewRecordOriginsLocked(
            state.get(), saved_record_count)) {
        return UpdateFailureActionLocked(state.get(), specialized);
    }
    FlimeRememberDescriptorPlanLocked(
        state.get(), descriptor_stage, plan_position,
        plan_compiled, compiled_plan);
    if (descriptor_stage == kStageLearn ||
        descriptor_stage == kStageMatch ||
        descriptor_stage == kStageFast) {
        ++state->descriptor_plan_cursor;
    }
    if (descriptor_stage != kStageLegacy) {
        state->calls[saved_call_count].prepare_ns =
            FinishPrepareNs(prepare_started_ns);
    }
    if (specialized && !MaybeRouteFastPrefixLocked(state.get())) {
        /*
         * The current call is already committed to the authoritative shadow.
         * A failed route may have reached the host, so do not replay the raw
         * call and risk executing descriptor copies twice.
         */
        state->transport_failed = true;
        return UpdateFailureActionLocked(state.get(), true);
    }
    if (specialized) {
        g_skip_next_fun_id = -1;
        g_allow_next_cluster = false;
        g_block_next_write = false;
        g_flush_cluster_after_write = false;
        g_route_after_write_command = 0;
        g_named_write_command = 0;
        return FLIME_GUEST_UPDATE_SUPPRESS;
    }
    MarkPreWriteSemantic(fun_id, false);
    return FLIME_GUEST_UPDATE_LEGACY;
}

FlimeGuestUpdateAction FlimeGuestUpdateDescriptorSetWithTemplate(
        int fun_id,
        VkDevice device,
        VkDescriptorSet set,
        VkDescriptorUpdateTemplate descriptor_update_template,
        const void* data,
        uint64_t encoded_bytes) {
    std::unique_lock<std::mutex> lock(g_mutex);
    std::shared_ptr<DeviceState> state = FindDeviceLocked(device);
    if (!state) return FLIME_GUEST_UPDATE_LEGACY;
    while (state->active_submit) {
        g_submit_cv.wait(lock);
    }
    if (state->transport_failed) {
        return UpdateFailureActionLocked(state.get(), true);
    }
    if (state->early_route_ready) {
        if (!DrainEarlyRouteLocked(state.get())) {
            return FLIME_GUEST_UPDATE_FATAL;
        }
        MarkPreWriteSemantic(fun_id, false);
        return FLIME_GUEST_UPDATE_LEGACY;
    }
    const uint64_t prepare_started_ns = NowNs();
    const bool specialized = IsSpecialized(*state);
    uint64_t record_count = 0;
    if (!FlimeCountTemplateRecords(
            state.get(), descriptor_update_template, &record_count)) {
        return UpdateFailureActionLocked(state.get(), specialized);
    }
    const uint64_t normalized_bytes =
        std::max<uint64_t>(std::max<uint64_t>(encoded_bytes, 1),
                           record_count + 1);
    if (state->calls.size() >= kMaxSemanticCalls ||
        state->template_bytes > kMaxTemplateBytes - normalized_bytes ||
        (specialized &&
         (record_count > kMaxRouteRecords ||
          LiveRecordCount(*state) >
              kMaxRouteRecords - static_cast<size_t>(record_count)))) {
        return UpdateFailureActionLocked(state.get(), specialized);
    }
    const size_t saved_call_count = state->calls.size();
    const size_t saved_record_count = state->records.size();
    const uint64_t call_offset = state->template_bytes;
    const uint32_t saved_dispatch_count = state->dispatch_count;
    const GuestStage descriptor_stage = state->stage;
    const size_t plan_position = state->descriptor_plan_cursor;
    if (state->stage != kStageLegacy) {
        std::vector<uint64_t> structural;
        std::vector<uint64_t> handles;
        structural.push_back(record_count);
        std::unordered_map<uint64_t, UpdateTemplateState>::const_iterator
            found = state->update_templates.find(
                HandleBits(descriptor_update_template));
        if (found == state->update_templates.end()) {
            return UpdateFailureActionLocked(state.get(), specialized);
        }
        for (size_t i = 0; i < found->second.entries.size(); ++i) {
            const TemplateEntry& entry = found->second.entries[i];
            structural.push_back(entry.binding);
            structural.push_back(entry.array_element);
            structural.push_back(entry.count);
            structural.push_back(entry.type);
            structural.push_back(entry.offset);
            structural.push_back(entry.stride);
        }
        handles.push_back(HandleBits(set));
        handles.push_back(HandleBits(descriptor_update_template));
        uint64_t payload_hash = HashWord(
            UINT64_C(1469598103934665603), HandleBits(set));
        payload_hash = HashWord(
            payload_hash, HandleBits(descriptor_update_template));
        if (!AppendSemantic(
                &state->calls, &state->template_bytes,
                &state->dispatch_count, fun_id,
                &structural[0], structural.size(),
                &handles[0], handles.size(), payload_hash,
                normalized_bytes, false)) {
            return UpdateFailureActionLocked(state.get(), specialized);
        }
    }
    FlimeFastDescriptorCall compiled_plan;
    const bool compile_plan =
        descriptor_stage == kStageLearn ||
        descriptor_stage == kStageMatch;
    const bool plan_compiled = !compile_plan ||
        FlimeCompileDescriptorTemplateFastPlan(
            state.get(), fun_id, saved_call_count, call_offset,
            normalized_bytes, set, descriptor_update_template,
            data, &compiled_plan);
    bool used_fast = false;
    if (descriptor_stage == kStageFast &&
        !state->mismatch_pending) {
        const FlimeFastDescriptorCall *expected =
            FlimeExpectedDescriptorPlanLocked(state.get(), plan_position);
        used_fast = expected != NULL &&
            FlimeApplyDescriptorTemplateFastPlan(
                state.get(), *expected, fun_id, saved_call_count,
                call_offset, normalized_bytes, set,
                descriptor_update_template, data,
                specialized) == kFlimeFastApplied;
        if (!used_fast) {
            state->mismatch_pending = true;
            state->building_descriptor_cache_complete = false;
        }
    }
    if (!used_fast) {
        if (!FlimeShadowUpdateWithTemplate(
                state.get(), call_offset, set, descriptor_update_template,
                data, specialized)) {
            state->calls.resize(saved_call_count);
            state->template_bytes = call_offset;
            state->dispatch_count = saved_dispatch_count;
            return UpdateFailureActionLocked(state.get(), specialized);
        }
        state->generic_shadow_ran = true;
    }
    if (!StampNewRecordOriginsLocked(
            state.get(), saved_record_count)) {
        return UpdateFailureActionLocked(state.get(), specialized);
    }
    FlimeRememberDescriptorPlanLocked(
        state.get(), descriptor_stage, plan_position,
        plan_compiled, compiled_plan);
    if (descriptor_stage == kStageLearn ||
        descriptor_stage == kStageMatch ||
        descriptor_stage == kStageFast) {
        ++state->descriptor_plan_cursor;
    }
    if (descriptor_stage != kStageLegacy) {
        state->calls[saved_call_count].prepare_ns =
            FinishPrepareNs(prepare_started_ns);
    }
    if (specialized && !MaybeRouteFastPrefixLocked(state.get())) {
        state->transport_failed = true;
        return UpdateFailureActionLocked(state.get(), true);
    }
    if (specialized) {
        g_skip_next_fun_id = -1;
        g_allow_next_cluster = false;
        g_block_next_write = false;
        g_flush_cluster_after_write = false;
        g_route_after_write_command = 0;
        g_named_write_command = 0;
        return FLIME_GUEST_UPDATE_SUPPRESS;
    }
    MarkPreWriteSemantic(fun_id, false);
    return FLIME_GUEST_UPDATE_LEGACY;
}
