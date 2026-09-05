/*
 * Guest half of FLIME for the express Vulkan transport.
 *
 * This interface deliberately exposes Vulkan-level lifecycle and semantic
 * hooks instead of reaching into generated encoder state.  The implementation
 * owns the authoritative descriptor shadow used by routed updates.  Callers
 * must still execute every hook even while FLIME is in legacy mode so a later
 * Learn transition starts from complete state.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include <vulkan/vulkan.h>

struct ParamManager;

enum FlimeGuestUpdateAction {
    FLIME_GUEST_UPDATE_LEGACY = 0,
    FLIME_GUEST_UPDATE_SUPPRESS = 1,
    FLIME_GUEST_UPDATE_FATAL = 2,
};

enum FlimeGuestSubmitGate {
    FLIME_GUEST_SUBMIT_LEGACY = 0,
    FLIME_GUEST_SUBMIT_READY = 1,
    FLIME_GUEST_SUBMIT_BLOCKED = 2,
};

struct FlimeGuestSubmitToken {
    VkDevice device;
    VkQueue queue;
    uint64_t submission_id;
    uint64_t period_id;
    uint32_t flags;
    bool specialized;
    bool fallback_legacy;
    bool valid;
};

/*
 * All ordinary transport writes go through this ordering point.  Semantic hooks
 * mark named writes before entry; this wrapper clusters only an audited
 * asynchronous Type-I whitelist,
 * and drains that cluster before synchronous/unlisted calls.  FLIME's own
 * control and route RPCs bypass the wrapper to prevent recursive observation.
 */
ssize_t FlimeGuestWrite(ParamManager* manager,
                        int fd,
                        int device_id,
                        int fun_id,
                        bool sync);
/*
 * Writes a generated command-buffer operation that has no richer FLIME hook.
 * Ordinary Type-I calls contribute only operation identity and exact encoded
 * size (their payload is a wildcard); descriptor consumers or descriptor-state
 * mutations from the audited unsafe list invalidate specialization.
 */
ssize_t FlimeGuestWriteCommand(ParamManager* manager,
                               int fd,
                               int device_id,
                               int fun_id,
                               bool sync,
                               VkCommandBuffer command_buffer);
bool FlimeGuestFlushTransport(int fd);

void FlimeGuestRegisterDevice(int fd, VkDevice device);
bool FlimeGuestBeforeDestroyDevice(VkDevice device);
void FlimeGuestDestroyDevice(VkDevice device);
void FlimeGuestRegisterQueue(VkDevice device, VkQueue queue);

void FlimeGuestRegisterCommandBuffers(VkDevice device,
                                      VkCommandPool pool,
                                      uint32_t count,
                                      const VkCommandBuffer* command_buffers);
void FlimeGuestFreeCommandBuffers(VkDevice device,
                                  VkCommandPool pool,
                                  uint32_t count,
                                  const VkCommandBuffer* command_buffers);
void FlimeGuestDestroyCommandPool(VkDevice device, VkCommandPool pool);
void FlimeGuestResetCommandPool(VkDevice device,
                                VkCommandPool pool,
                                VkResult result);
void FlimeGuestBeginCommandBuffer(int fun_id,
                                  VkCommandBuffer command_buffer,
                                  const VkCommandBufferBeginInfo* begin_info,
                                  VkResult result,
                                  uint64_t encoded_bytes);
void FlimeGuestEndCommandBuffer(int fun_id,
                                VkCommandBuffer command_buffer,
                                VkResult result,
                                uint64_t encoded_bytes);
void FlimeGuestResetCommandBuffer(int fun_id,
                                  VkCommandBuffer command_buffer,
                                  VkCommandBufferResetFlags flags,
                                  VkResult result,
                                  uint64_t encoded_bytes);

/*
 * Reserve a descriptor-lifecycle RPC against FLIME submission commit.
 * Every successful before call must be paired with an after call, including
 * transport-failure paths.
 */
void FlimeGuestBeforeDescriptorLifecycle(VkDevice device);
/*
 * Called with the lifecycle reservation held and before an RPC that can
 * retire descriptor-set or descriptor-payload handles.  Specialized mode
 * must synchronously drain retained routed records while those handles still
 * map on the host.
 */
bool FlimeGuestPrepareDescriptorRetirement(VkDevice device);
void FlimeGuestAfterDescriptorLifecycle(VkDevice device,
                                        bool transport_ok);

void FlimeGuestCreateDescriptorSetLayout(
    VkDevice device,
    const VkDescriptorSetLayoutCreateInfo* create_info,
    VkDescriptorSetLayout layout,
    VkResult result);
void FlimeGuestDestroyDescriptorSetLayout(VkDevice device,
                                          VkDescriptorSetLayout layout);
void FlimeGuestCreateDescriptorPool(VkDevice device,
                                    const VkDescriptorPoolCreateInfo* create_info,
                                    VkDescriptorPool pool,
                                    VkResult result);
void FlimeGuestDestroyDescriptorPool(VkDevice device, VkDescriptorPool pool);
void FlimeGuestAllocateDescriptorSets(
    VkDevice device,
    const VkDescriptorSetAllocateInfo* allocate_info,
    const VkDescriptorSet* sets,
    VkResult result);
void FlimeGuestFreeDescriptorSets(VkDevice device,
                                  VkDescriptorPool pool,
                                  uint32_t count,
                                  const VkDescriptorSet* sets,
                                  VkResult result);
void FlimeGuestResetDescriptorPool(VkDevice device,
                                   VkDescriptorPool pool,
                                   VkResult result);

void FlimeGuestCreateDescriptorUpdateTemplate(
    VkDevice device,
    const VkDescriptorUpdateTemplateCreateInfo* create_info,
    VkDescriptorUpdateTemplate descriptor_update_template,
    VkResult result);
void FlimeGuestDestroyDescriptorUpdateTemplate(
    VkDevice device,
    VkDescriptorUpdateTemplate descriptor_update_template);
void FlimeGuestDestroyDescriptorPayload(VkDevice device,
                                        VkObjectType object_type,
                                        uint64_t object);

FlimeGuestUpdateAction FlimeGuestUpdateDescriptorSets(
    int fun_id,
    VkDevice device,
    uint32_t write_count,
    const VkWriteDescriptorSet* writes,
    uint32_t copy_count,
    const VkCopyDescriptorSet* copies,
    uint64_t encoded_bytes);
FlimeGuestUpdateAction FlimeGuestUpdateDescriptorSetWithTemplate(
    int fun_id,
    VkDevice device,
    VkDescriptorSet set,
    VkDescriptorUpdateTemplate descriptor_update_template,
    const void* data,
    uint64_t encoded_bytes);

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
    uint64_t encoded_bytes);
void FlimeGuestCmdBindPipeline(int fun_id,
                               VkCommandBuffer command_buffer,
                               VkPipelineBindPoint bind_point,
                               VkPipeline pipeline,
                               uint64_t encoded_bytes);
void FlimeGuestCmdDispatch(int fun_id,
                           VkCommandBuffer command_buffer,
                           uint32_t group_count_x,
                           uint32_t group_count_y,
                           uint32_t group_count_z,
                           uint64_t encoded_bytes);
void FlimeGuestCmdDispatchBase(int fun_id,
                               VkCommandBuffer command_buffer,
                               uint32_t base_group_x,
                               uint32_t base_group_y,
                               uint32_t base_group_z,
                               uint32_t group_count_x,
                               uint32_t group_count_y,
                               uint32_t group_count_z,
                               uint64_t encoded_bytes);
void FlimeGuestCmdDispatchIndirect(int fun_id,
                                   VkCommandBuffer command_buffer,
                                   VkBuffer buffer,
                                   VkDeviceSize offset,
                                   uint64_t encoded_bytes);
void FlimeGuestCmdExecuteCommands(int fun_id,
                                  VkCommandBuffer command_buffer,
                                  uint32_t command_buffer_count,
                                  const VkCommandBuffer* command_buffers,
                                  uint64_t encoded_bytes);

FlimeGuestSubmitGate FlimeGuestBeforeQueueSubmit(
    VkQueue queue,
    uint32_t submit_count,
    const VkSubmitInfo* submits,
    VkFence fence,
    FlimeGuestSubmitToken* token);
FlimeGuestSubmitGate FlimeGuestBeforeQueueSubmit2(
    VkQueue queue,
    uint32_t submit_count,
    const VkSubmitInfo2* submits,
    VkFence fence,
    FlimeGuestSubmitToken* token);
void FlimeGuestAfterQueueSubmit(const FlimeGuestSubmitToken* token,
                                VkResult result);
