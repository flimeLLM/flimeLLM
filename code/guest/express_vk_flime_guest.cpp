/* Transport and Vulkan object lifecycle hooks. */
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

ssize_t FlimeGuestWrite(ParamManager* manager,
                        int fd,
                        int device_id,
                        int fun_id,
                        bool sync) {
    if (manager == NULL) {
        errno = EINVAL;
        return -1;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!DrainEarlyRoutesBeforeRawCallLocked(fun_id)) {
        manager->clear();
        errno = EPROTO;
        return -1;
    }
    const bool named = g_skip_next_fun_id == fun_id;
    const bool allow_cluster =
        named && g_allow_next_cluster && !sync &&
        device_id == EXPRESS_GPU_DEVICE_ID &&
        IsTypeIClusterFunction(fun_id);
    const bool block = named && g_block_next_write;
    const bool flush_after = named && g_flush_cluster_after_write;
    const uint64_t route_after =
        named ? g_route_after_write_command : 0;
    const uint64_t command_owner =
        named ? g_named_write_command : 0;
    g_skip_next_fun_id = -1;
    g_allow_next_cluster = false;
    g_block_next_write = false;
    g_flush_cluster_after_write = false;
    g_route_after_write_command = 0;
    g_named_write_command = 0;
    if (block) {
        manager->clear();
        errno = EIO;
        return -1;
    }
    if (allow_cluster) {
        ParamManager::FrozenCall call =
            manager->freeze(device_id, fun_id, false);
        const ssize_t result =
            AppendOwnedClusterLocked(fd, manager, &call);
        if (result < 0) {
            std::shared_ptr<DeviceState> state =
                FindByMappedKeyLocked(g_command_devices, command_owner);
            if (state) state->transport_failed = true;
            errno = EPROTO;
        }
        if (result >= 0 && flush_after && !FlushAnyClusterLocked()) {
            std::shared_ptr<DeviceState> state =
                FindByMappedKeyLocked(g_command_devices, command_owner);
            if (state) state->transport_failed = true;
            errno = EIO;
            return -1;
        }
        if (result >= 0 && route_after != 0) {
            std::shared_ptr<DeviceState> state =
                FindByMappedKeyLocked(g_command_devices, route_after);
            if (!state ||
                !MaybeAdvanceFastRouteLocked(state.get(), route_after)) {
                if (state) state->transport_failed = true;
                errno = EIO;
                return -1;
            }
        }
        return result;
    }
    if (!FlushAnyClusterLocked()) {
        std::shared_ptr<DeviceState> state =
            FindByMappedKeyLocked(g_command_devices, command_owner);
        if (state) state->transport_failed = true;
        manager->clear();
        errno = EIO;
        return -1;
    }
    const ssize_t result = manager->write(fd, device_id, fun_id, sync);
    if (result < 0 && command_owner != 0) {
        std::shared_ptr<DeviceState> state =
            FindByMappedKeyLocked(g_command_devices, command_owner);
        if (state) state->transport_failed = true;
    }
    return result;
}

ssize_t FlimeGuestWriteCommand(ParamManager* manager,
                               int fd,
                               int device_id,
                               int fun_id,
                               bool sync,
                               VkCommandBuffer command_buffer) {
    if (manager == NULL || command_buffer == VK_NULL_HANDLE ||
        device_id != EXPRESS_GPU_DEVICE_ID) {
        if (manager != NULL) manager->clear();
        errno = EINVAL;
        return -1;
    }

    /*
     * Freezing gives the observer an owned and stable view of every direct and
     * pointer parameter without teaching FLIME the generated wire layout.  The
     * footprint includes the call record plus both backing stores, so adaptive
     * unit offsets account for commands whose arrays live in pointerStorage.
     */
    ParamManager::FrozenCall call =
        manager->freeze(device_id, fun_id, sync);
    if (call.empty() || call.size() < 0 ||
        call.paramStorage.size() < 0 || call.pointerStorage.size() < 0 ||
        !RebaseFrozenCallPointers(&call)) {
        std::lock_guard<std::mutex> lock(g_mutex);
        std::shared_ptr<DeviceState> failed =
            FindCommandLocked(command_buffer);
        if (failed) failed->transport_failed = true;
        errno = EPROTO;
        return -1;
    }
    const uint64_t encoded_bytes =
        static_cast<uint64_t>(call.size()) +
        static_cast<uint64_t>(call.paramStorage.size()) +
        static_cast<uint64_t>(call.pointerStorage.size());

    const bool descriptor_frontier_unsafe =
        !CommandAllowsTypeIWildcard(fun_id);
    std::lock_guard<std::mutex> lock(g_mutex);
    std::shared_ptr<DeviceState> state = FindCommandLocked(command_buffer);
    const bool initial_cluster_candidate =
        state && IsSpecialized(*state) && !state->transport_failed &&
        !state->early_route_ready && !descriptor_frontier_unsafe && !sync;
    if (!initial_cluster_candidate && !FlushAnyClusterLocked()) {
        if (state) state->transport_failed = true;
        errno = EIO;
        return -1;
    }
    if (state && state->early_route_ready &&
        !DrainEarlyRouteLocked(state.get())) {
        errno = EPROTO;
        return -1;
    }

    if (state && state->stage != kStageLegacy) {
        if (!AppendCommandSemanticLocked(
                command_buffer, fun_id, NULL, 0, NULL, 0, 0,
                std::max<uint64_t>(encoded_bytes, 1), false)) {
            /*
             * Do not let an operation missing from the semantic stream pass
             * while descriptor RPCs are suppressed.  A synchronous drain makes
             * a legacy retry ordered; otherwise fail the void command closed.
             */
            if (IsSpecialized(*state)) {
                if (!FallbackToLegacyLocked(state.get(), NULL)) {
                    state->transport_failed = true;
                    errno = EPROTO;
                    return -1;
                }
            } else {
                EnterLegacy(state.get());
            }
        } else {
            RecordedCommandStream& stream =
                g_recorded_commands[HandleBits(command_buffer)];
            if (stream.calls.empty()) {
                state->transport_failed = true;
                errno = EPROTO;
                return -1;
            }
            stream.calls.back().opaque = descriptor_frontier_unsafe;
            if (descriptor_frontier_unsafe && IsSpecialized(*state)) {
                /*
                 * Detect may hash every operation identity, but a descriptor
                 * consumer without a rich binding/frontier classifier cannot
                 * cross any specialized or early-routing boundary.  Drain the
                 * exact retained ledger synchronously before the opaque native
                 * command is sent, then execute it once in Legacy.
                 */
                if (!FallbackToLegacyLocked(state.get(), NULL)) {
                    state->transport_failed = true;
                    errno = EPROTO;
                    return -1;
                }
            }
        }
    }

    /*
     * Safe generic vkCmd calls are Type-I payload wildcards: the semantic
     * matcher ignores their values, but the real command still reaches the
     * host in program order.  Cluster only after specialization; Detect must
     * retain the original one-call observation stream, while opaque or
     * synchronous commands always form a direct-RPC ordering boundary.
     */
    const bool allow_cluster =
        state && IsSpecialized(*state) && !state->transport_failed &&
        !state->early_route_ready && !descriptor_frontier_unsafe && !sync;
    if (allow_cluster) {
        const ssize_t result =
            AppendOwnedClusterLocked(fd, manager, &call);
        if (result < 0) {
            state->transport_failed = true;
            errno = EPROTO;
        }
        return result;
    }
    if (!FlushAnyClusterLocked()) {
        if (state) state->transport_failed = true;
        errno = EIO;
        return -1;
    }
    const ssize_t result = manager->writeFrozen(fd, call);
    if (result != call.size()) {
        if (state) state->transport_failed = true;
    }
    return result;
}

bool FlimeGuestFlushTransport(int fd) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_cluster.Empty()) return true;
    if (fd >= 0 && g_cluster.fd != fd) {
        return FlushAnyClusterLocked();
    }
    return FlushAnyClusterLocked();
}

void FlimeGuestRegisterDevice(int fd, VkDevice device) {
    const uint64_t key = HandleBits(device);
    if (fd < 0 || key == 0) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_devices.find(key) != g_devices.end()) {
        g_devices[key]->transport_failed = true;
        return;
    }
    std::shared_ptr<DeviceState> state(new (std::nothrow) DeviceState());
    if (!state) return;
    state->fd = fd;
    state->device = device;
    state->process_id = static_cast<uint64_t>(getpid());
    state->stream_id = NextStreamIdLocked(state->process_id, key);
    if (!AllocateControlPage(state.get())) {
        state->stage = kStageLegacy;
        state->transport_failed = false;
    }
    g_devices[key] = state;
    if (state->control_page != NULL &&
        !NegotiateCapabilitiesLocked(state.get()) &&
        state->stage != kStageLegacy) {
        state->transport_failed = true;
    }
}

bool FlimeGuestBeforeDestroyDevice(VkDevice device) {
    const uint64_t key = HandleBits(device);
    std::unique_lock<std::mutex> lock(g_mutex);
    std::unordered_map<
        uint64_t, std::shared_ptr<DeviceState> >::iterator found =
        g_devices.find(key);
    if (found == g_devices.end()) return true;
    DeviceState* state = found->second.get();
    while (state->active_submit || state->lifecycle_inflight != 0) {
        g_submit_cv.wait(lock);
    }
    if (!DrainEarlyRouteLocked(state)) return false;
    if (!FlushAnyClusterLocked()) {
        state->transport_failed = true;
        return false;
    }
    if (!state->control_page_exposed || state->session_invalidated) {
        state->teardown_complete = true;
        state->control_page_exposed = false;
        state->negotiated = false;
        return true;
    }
    if (!SendSessionPacketLocked(state, kWireTeardown)) {
        state->transport_failed = true;
        return false;
    }
    state->teardown_complete = true;
    state->control_page_exposed = false;
    state->negotiated = false;
    return true;
}

void FlimeGuestDestroyDevice(VkDevice device) {
    const uint64_t key = HandleBits(device);
    std::lock_guard<std::mutex> lock(g_mutex);
    std::unordered_map<
        uint64_t, std::shared_ptr<DeviceState> >::iterator found =
        g_devices.find(key);
    if (found == g_devices.end()) return;
    std::shared_ptr<DeviceState> state = found->second;
    if (!state->teardown_complete && !state->control_page_exposed) {
        state->teardown_complete = true;
    }
    RemoveDeviceMappingsLocked(*state);
    g_devices.erase(found);
}

void FlimeGuestRegisterQueue(VkDevice device, VkQueue queue) {
    const uint64_t queue_key = HandleBits(queue);
    if (queue_key == 0) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    std::shared_ptr<DeviceState> state = FindDeviceLocked(device);
    if (!state) return;
    if (state->early_route_ready &&
        (state->registered_queues.size() != 1 ||
         state->registered_queues.find(queue_key) ==
             state->registered_queues.end()) &&
        !DrainEarlyRouteLocked(state.get())) {
        return;
    }
    std::unordered_map<uint64_t, uint64_t>::iterator existing =
        g_queue_devices.find(queue_key);
    if (existing != g_queue_devices.end() &&
        existing->second != HandleBits(device)) {
        state->transport_failed = true;
        return;
    }
    g_queue_devices[queue_key] = HandleBits(device);
    state->registered_queues.insert(queue_key);
}

void FlimeGuestRegisterCommandBuffers(
        VkDevice device,
        VkCommandPool pool,
        uint32_t count,
        const VkCommandBuffer* command_buffers) {
    if (count != 0 && command_buffers == NULL) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    std::shared_ptr<DeviceState> state = FindDeviceLocked(device);
    if (!state) return;
    for (uint32_t i = 0; i < count; ++i) {
        const uint64_t key = HandleBits(command_buffers[i]);
        if (key == 0 || state->commands.find(key) != state->commands.end() ||
            g_command_devices.find(key) != g_command_devices.end()) {
            state->transport_failed = true;
            return;
        }
    }
    for (uint32_t i = 0; i < count; ++i) {
        const uint64_t key = HandleBits(command_buffers[i]);
        CommandState command = {};
        command.device = device;
        command.pool = pool;
        command.generation = 1;
        command.recording = false;
        command.executable = false;
        state->commands[key] = command;
        RecordedCommandStream stream;
        stream.generation = command.generation;
        g_recorded_commands[key] = stream;
        g_command_devices[key] = HandleBits(device);
    }
}

void FlimeGuestFreeCommandBuffers(
        VkDevice device,
        VkCommandPool pool,
        uint32_t count,
        const VkCommandBuffer* command_buffers) {
    if (count != 0 && command_buffers == NULL) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    FlushAnyClusterLocked();
    std::shared_ptr<DeviceState> state = FindDeviceLocked(device);
    if (!state) return;
    NoteEarlyCommandMutationLocked(state.get());
    for (uint32_t i = 0; i < count; ++i) {
        const uint64_t key = HandleBits(command_buffers[i]);
        std::unordered_map<uint64_t, CommandState>::iterator found =
            state->commands.find(key);
        if (found == state->commands.end() ||
            HandleBits(found->second.pool) != HandleBits(pool)) {
            if (IsSpecialized(*state)) state->transport_failed = true;
            continue;
        }
        state->commands.erase(found);
        g_recorded_commands.erase(key);
        g_command_devices.erase(key);
    }
}

void FlimeGuestDestroyCommandPool(VkDevice device, VkCommandPool pool) {
    std::lock_guard<std::mutex> lock(g_mutex);
    FlushAnyClusterLocked();
    std::shared_ptr<DeviceState> state = FindDeviceLocked(device);
    if (!state) return;
    NoteEarlyCommandMutationLocked(state.get());
    for (std::unordered_map<uint64_t, CommandState>::iterator it =
             state->commands.begin(); it != state->commands.end();) {
        if (HandleBits(it->second.pool) == HandleBits(pool)) {
            g_recorded_commands.erase(it->first);
            g_command_devices.erase(it->first);
            it = state->commands.erase(it);
        } else {
            ++it;
        }
    }
}

void FlimeGuestResetCommandPool(VkDevice device,
                                VkCommandPool pool,
                                VkResult result) {
    if (result != VK_SUCCESS) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    std::shared_ptr<DeviceState> state = FindDeviceLocked(device);
    if (!state) return;
    NoteEarlyCommandMutationLocked(state.get());
    for (std::unordered_map<uint64_t, CommandState>::iterator it =
             state->commands.begin(); it != state->commands.end(); ++it) {
        if (HandleBits(it->second.pool) != HandleBits(pool)) continue;
        ++it->second.generation;
        if (it->second.generation == 0) it->second.generation = 1;
        it->second.recording = false;
        it->second.executable = false;
        RecordedCommandStream stream;
        stream.generation = it->second.generation;
        g_recorded_commands[it->first] = stream;
    }
}

void FlimeGuestBeginCommandBuffer(
        int fun_id,
        VkCommandBuffer command_buffer,
        const VkCommandBufferBeginInfo* begin_info,
        VkResult result,
        uint64_t encoded_bytes) {
    (void)fun_id;
    (void)begin_info;
    (void)encoded_bytes;
    if (result != VK_SUCCESS) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    std::shared_ptr<DeviceState> state =
        FindCommandLocked(command_buffer);
    if (!state) return;
    NoteEarlyCommandMutationLocked(state.get());
    std::unordered_map<uint64_t, CommandState>::iterator command =
        state->commands.find(HandleBits(command_buffer));
    if (command == state->commands.end()) return;
    ++command->second.generation;
    if (command->second.generation == 0) command->second.generation = 1;
    command->second.recording = true;
    command->second.executable = false;
    RecordedCommandStream stream;
    stream.generation = command->second.generation;
    g_recorded_commands[HandleBits(command_buffer)] = stream;
}

void FlimeGuestEndCommandBuffer(int fun_id,
                                VkCommandBuffer command_buffer,
                                VkResult result,
                                uint64_t encoded_bytes) {
    (void)fun_id;
    (void)encoded_bytes;
    if (result != VK_SUCCESS) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    std::shared_ptr<DeviceState> state =
        FindCommandLocked(command_buffer);
    if (!state) return;
    std::unordered_map<uint64_t, CommandState>::iterator command =
        state->commands.find(HandleBits(command_buffer));
    if (command == state->commands.end() || !command->second.recording) {
        if (IsSpecialized(*state)) state->transport_failed = true;
        return;
    }
    command->second.recording = false;
    command->second.executable = true;
    if (!MaybeAdvanceFastRouteLocked(
            state.get(), HandleBits(command_buffer))) {
        state->transport_failed = true;
    }
}

void FlimeGuestResetCommandBuffer(
        int fun_id,
        VkCommandBuffer command_buffer,
        VkCommandBufferResetFlags flags,
        VkResult result,
        uint64_t encoded_bytes) {
    (void)fun_id;
    (void)flags;
    (void)encoded_bytes;
    if (result != VK_SUCCESS) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    std::shared_ptr<DeviceState> state =
        FindCommandLocked(command_buffer);
    if (!state) return;
    NoteEarlyCommandMutationLocked(state.get());
    std::unordered_map<uint64_t, CommandState>::iterator command =
        state->commands.find(HandleBits(command_buffer));
    if (command == state->commands.end()) return;
    ++command->second.generation;
    if (command->second.generation == 0) command->second.generation = 1;
    command->second.recording = false;
    command->second.executable = false;
    RecordedCommandStream stream;
    stream.generation = command->second.generation;
    g_recorded_commands[HandleBits(command_buffer)] = stream;
}
