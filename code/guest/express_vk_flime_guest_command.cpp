/* Command recording and queue submission hooks. */
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

void FlimeGuestCmdBindDescriptorSets(
        int fun_id,
        VkCommandBuffer command_buffer,
        VkPipelineBindPoint bind_point,
        VkPipelineLayout layout,
        uint32_t first_set,
        uint32_t descriptor_set_count,
        const VkDescriptorSet* descriptor_sets,
        uint32_t dynamic_offset_count,
        const uint32_t* dynamic_offsets,
        uint64_t encoded_bytes) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if ((descriptor_set_count != 0 && descriptor_sets == NULL) ||
        (dynamic_offset_count != 0 && dynamic_offsets == NULL)) {
        FailNamedWriteLocked(
            FindCommandLocked(command_buffer).get(), fun_id);
        return;
    }
    std::vector<uint64_t> structural;
    std::vector<uint64_t> handles;
    structural.push_back(static_cast<uint32_t>(bind_point));
    structural.push_back(first_set);
    structural.push_back(descriptor_set_count);
    structural.push_back(dynamic_offset_count);
    handles.push_back(HandleBits(command_buffer));
    handles.push_back(HandleBits(layout));
    for (uint32_t i = 0; i < descriptor_set_count; ++i) {
        handles.push_back(HandleBits(descriptor_sets[i]));
    }
    uint64_t hash = UINT64_C(1469598103934665603);
    for (uint32_t i = 0; i < dynamic_offset_count; ++i) {
        structural.push_back(dynamic_offsets[i]);
        hash = HashWord(hash, dynamic_offsets[i]);
    }
    RecordCommandCallLocked(
        fun_id, command_buffer,
        &structural[0], structural.size(),
        &handles[0], handles.size(), hash,
        encoded_bytes, false, true);
}

void FlimeGuestCmdBindPipeline(int fun_id,
                               VkCommandBuffer command_buffer,
                               VkPipelineBindPoint bind_point,
                               VkPipeline pipeline,
                               uint64_t encoded_bytes) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const uint64_t structural[1] = {
        static_cast<uint32_t>(bind_point)
    };
    const uint64_t handles[2] = {
        HandleBits(command_buffer), HandleBits(pipeline)
    };
    RecordCommandCallLocked(
        fun_id, command_buffer, structural, 1, handles, 2,
        0, encoded_bytes, false, true);
}

void FlimeGuestCmdDispatch(int fun_id,
                           VkCommandBuffer command_buffer,
                           uint32_t group_count_x,
                           uint32_t group_count_y,
                           uint32_t group_count_z,
                           uint64_t encoded_bytes) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const uint64_t structural[3] = {
        group_count_x, group_count_y, group_count_z
    };
    const uint64_t handles[1] = { HandleBits(command_buffer) };
    if (RecordCommandCallLocked(
            fun_id, command_buffer, structural, 3, handles, 1,
            0, encoded_bytes, true, true)) {
        std::unordered_map<uint64_t, RecordedCommandStream>::const_iterator
            stream = g_recorded_commands.find(HandleBits(command_buffer));
        std::shared_ptr<DeviceState> state =
            FindCommandLocked(command_buffer);
        if (state && stream != g_recorded_commands.end() &&
            IsFastPlanningBoundaryLocked(
                state.get(), command_buffer, stream->second.dispatches)) {
            g_flush_cluster_after_write = true;
            g_route_after_write_command = HandleBits(command_buffer);
        }
    }
}

void FlimeGuestCmdDispatchBase(
        int fun_id,
        VkCommandBuffer command_buffer,
        uint32_t base_group_x,
        uint32_t base_group_y,
        uint32_t base_group_z,
        uint32_t group_count_x,
        uint32_t group_count_y,
        uint32_t group_count_z,
        uint64_t encoded_bytes) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const uint64_t structural[6] = {
        base_group_x, base_group_y, base_group_z,
        group_count_x, group_count_y, group_count_z
    };
    const uint64_t handles[1] = { HandleBits(command_buffer) };
    if (RecordCommandCallLocked(
            fun_id, command_buffer, structural, 6, handles, 1,
            0, encoded_bytes, true, true)) {
        std::unordered_map<uint64_t, RecordedCommandStream>::const_iterator
            stream = g_recorded_commands.find(HandleBits(command_buffer));
        std::shared_ptr<DeviceState> state =
            FindCommandLocked(command_buffer);
        if (state && stream != g_recorded_commands.end() &&
            IsFastPlanningBoundaryLocked(
                state.get(), command_buffer, stream->second.dispatches)) {
            g_flush_cluster_after_write = true;
            g_route_after_write_command = HandleBits(command_buffer);
        }
    }
}

void FlimeGuestCmdDispatchIndirect(
        int fun_id,
        VkCommandBuffer command_buffer,
        VkBuffer buffer,
        VkDeviceSize offset,
        uint64_t encoded_bytes) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const uint64_t structural[1] = { offset };
    const uint64_t handles[2] = {
        HandleBits(command_buffer), HandleBits(buffer)
    };
    if (RecordCommandCallLocked(
            fun_id, command_buffer, structural, 1, handles, 2,
            0, encoded_bytes, true, true)) {
        std::unordered_map<uint64_t, RecordedCommandStream>::const_iterator
            stream = g_recorded_commands.find(HandleBits(command_buffer));
        std::shared_ptr<DeviceState> state =
            FindCommandLocked(command_buffer);
        if (state && stream != g_recorded_commands.end() &&
            IsFastPlanningBoundaryLocked(
                state.get(), command_buffer, stream->second.dispatches)) {
            g_flush_cluster_after_write = true;
            g_route_after_write_command = HandleBits(command_buffer);
        }
    }
}

void FlimeGuestCmdExecuteCommands(
        int fun_id,
        VkCommandBuffer command_buffer,
        uint32_t command_buffer_count,
        const VkCommandBuffer* command_buffers,
        uint64_t encoded_bytes) {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::shared_ptr<DeviceState> state =
        FindCommandLocked(command_buffer);
    if (!state || state->stage == kStageLegacy) {
        MarkPreWriteSemantic(fun_id, false);
        return;
    }
    if (state->transport_failed) {
        FailNamedWriteLocked(state.get(), fun_id);
        return;
    }
    if (state->early_route_ready) {
        if (!DrainEarlyRouteLocked(state.get())) {
            FailNamedWriteLocked(state.get(), fun_id);
            return;
        }
        MarkPreWriteSemantic(fun_id, false);
        return;
    }
    if ((command_buffer_count != 0 && command_buffers == NULL) ||
        !AppendExecuteSemanticLocked(
            command_buffer, fun_id, command_buffer_count,
            command_buffers, encoded_bytes)) {
        FailNamedWriteLocked(state.get(), fun_id);
        return;
    }
    MarkPreWriteSemantic(
        fun_id, IsSpecialized(*state) &&
                    IsTypeIClusterFunction(fun_id));
    g_named_write_command = HandleBits(command_buffer);
}

FlimeGuestSubmitGate FlimeGuestBeforeQueueSubmit(
        VkQueue queue,
        uint32_t submit_count,
        const VkSubmitInfo* submits,
        VkFence fence,
        FlimeGuestSubmitToken* token) {
    if (token == NULL) return FLIME_GUEST_SUBMIT_BLOCKED;
    *token = FlimeGuestSubmitToken();
    token->queue = queue;
    std::unique_lock<std::mutex> lock(g_mutex);
    std::shared_ptr<DeviceState> state = FindQueueLocked(queue);
    if (!state) return FLIME_GUEST_SUBMIT_LEGACY;
    while (state->active_submit || state->lifecycle_inflight != 0) {
        g_submit_cv.wait(lock);
    }
    token->device = state->device;
    token->valid = true;
    if (state->queue == VK_NULL_HANDLE) {
        state->queue = queue;
    } else if (HandleBits(state->queue) != HandleBits(queue)) {
        if (state->early_route_ready &&
            !DrainEarlyRouteLocked(state.get())) {
            return FLIME_GUEST_SUBMIT_BLOCKED;
        }
        state->queue = queue;
    }
    if (!EnsureNegotiatedLocked(state.get())) {
        return state->stage == kStageLegacy
            ? FLIME_GUEST_SUBMIT_LEGACY
            : FLIME_GUEST_SUBMIT_BLOCKED;
    }
    SemanticCall submit_call;
    std::vector<VkCommandBuffer> commands;
    if (!BuildSubmitCall(
            queue, submit_count, submits, fence,
            &submit_call, &commands)) {
        if (state->stage == kStageDetect) {
            ClearOccurrence(state.get());
            return FLIME_GUEST_SUBMIT_LEGACY;
        }
        uint64_t fallback_token = 0;
        if (IsSpecialized(*state) &&
            PrepareFallbackLegacySubmitLocked(
                state.get(), &fallback_token)) {
            token->submission_id = fallback_token;
            token->fallback_legacy = true;
            token->specialized = false;
            return FLIME_GUEST_SUBMIT_LEGACY;
        }
        state->transport_failed = true;
        return FLIME_GUEST_SUBMIT_BLOCKED;
    }
    uint64_t protocol_token = 0;
    const ProtocolPrepareResult result = ProtocolBeforeSubmitLocked(
        state.get(), commands, submit_call, &protocol_token);
    if (result == kProtocolLegacy ||
        result == kProtocolFallbackLegacy) {
        if (result == kProtocolFallbackLegacy) {
            token->submission_id = protocol_token;
            token->fallback_legacy = true;
        }
        token->specialized = false;
        return FLIME_GUEST_SUBMIT_LEGACY;
    }
    if (result != kProtocolReady) {
        return FLIME_GUEST_SUBMIT_BLOCKED;
    }
    token->submission_id = protocol_token;
    token->period_id = state->active_period_id;
    token->flags = state->active_period_flags;
    token->specialized = true;
    return FLIME_GUEST_SUBMIT_READY;
}

FlimeGuestSubmitGate FlimeGuestBeforeQueueSubmit2(
        VkQueue queue,
        uint32_t submit_count,
        const VkSubmitInfo2* submits,
        VkFence fence,
        FlimeGuestSubmitToken* token) {
    if (token == NULL) return FLIME_GUEST_SUBMIT_BLOCKED;
    *token = FlimeGuestSubmitToken();
    token->queue = queue;
    std::unique_lock<std::mutex> lock(g_mutex);
    std::shared_ptr<DeviceState> state = FindQueueLocked(queue);
    if (!state) return FLIME_GUEST_SUBMIT_LEGACY;
    while (state->active_submit || state->lifecycle_inflight != 0) {
        g_submit_cv.wait(lock);
    }
    token->device = state->device;
    token->valid = true;
    if (state->queue == VK_NULL_HANDLE) {
        state->queue = queue;
    } else if (HandleBits(state->queue) != HandleBits(queue)) {
        if (state->early_route_ready &&
            !DrainEarlyRouteLocked(state.get())) {
            return FLIME_GUEST_SUBMIT_BLOCKED;
        }
        state->queue = queue;
    }
    if (!EnsureNegotiatedLocked(state.get())) {
        return state->stage == kStageLegacy
            ? FLIME_GUEST_SUBMIT_LEGACY
            : FLIME_GUEST_SUBMIT_BLOCKED;
    }
    SemanticCall submit_call;
    std::vector<VkCommandBuffer> commands;
    if (!BuildSubmit2Call(
            queue, submit_count, submits, fence,
            &submit_call, &commands)) {
        if (state->stage == kStageDetect) {
            ClearOccurrence(state.get());
            return FLIME_GUEST_SUBMIT_LEGACY;
        }
        uint64_t fallback_token = 0;
        if (IsSpecialized(*state) &&
            PrepareFallbackLegacySubmitLocked(
                state.get(), &fallback_token)) {
            token->submission_id = fallback_token;
            token->fallback_legacy = true;
            token->specialized = false;
            return FLIME_GUEST_SUBMIT_LEGACY;
        }
        state->transport_failed = true;
        return FLIME_GUEST_SUBMIT_BLOCKED;
    }
    uint64_t protocol_token = 0;
    const ProtocolPrepareResult result = ProtocolBeforeSubmitLocked(
        state.get(), commands, submit_call, &protocol_token);
    if (result == kProtocolLegacy ||
        result == kProtocolFallbackLegacy) {
        if (result == kProtocolFallbackLegacy) {
            token->submission_id = protocol_token;
            token->fallback_legacy = true;
        }
        token->specialized = false;
        return FLIME_GUEST_SUBMIT_LEGACY;
    }
    if (result != kProtocolReady) {
        return FLIME_GUEST_SUBMIT_BLOCKED;
    }
    token->submission_id = protocol_token;
    token->period_id = state->active_period_id;
    token->flags = state->active_period_flags;
    token->specialized = true;
    return FLIME_GUEST_SUBMIT_READY;
}

void FlimeGuestAfterQueueSubmit(const FlimeGuestSubmitToken* token,
                                VkResult result) {
    if (token == NULL || !token->valid ||
        (!token->specialized && !token->fallback_legacy)) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        std::shared_ptr<DeviceState> state =
            FindDeviceLocked(token->device);
        if (state && token->fallback_legacy) {
            if (state->active_submit &&
                state->active_token == token->submission_id) {
                if (result != VK_SUCCESS) {
                    state->transport_failed = true;
                }
                ClearActiveSubmit(state.get());
            }
        } else if (state) {
            ProtocolAfterSubmitLocked(
                state.get(), token->submission_id, result == VK_SUCCESS);
        }
    }
    g_submit_cv.notify_all();
}
