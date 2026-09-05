/*
 * FLIME transport bridge for the express Vulkan protocol.
 *
 * The guest-facing layouts below are fixed little-endian ABIs.  They are
 * deliberately independent of native Vulkan struct layouts and must never be
 * decoded by casting untrusted bytes.  A routed record always contains a
 * complete VkWriteDescriptorSet payload: derived writes have already been
 * materialized by the authoritative guest shadow, while elided writes are not
 * transmitted at all.
 */
#ifndef HW_EXPRESS_GPU_EXPRESS_VK_FLIME_BRIDGE_H
#define HW_EXPRESS_GPU_EXPRESS_VK_FLIME_BRIDGE_H

#include "hw/express-gpu/express_vk_flime.h"

#include <vulkan/vulkan.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* FLRD and FLRR as little-endian byte strings. */
#define EXPRESS_VK_FLIME_ROUTE_MAGIC UINT32_C(0x44524c46)
#define EXPRESS_VK_FLIME_ROUTE_REPLY_MAGIC UINT32_C(0x52524c46)
/* FLCP: persistent host-to-guest control page, written with a seqlock. */
#define EXPRESS_VK_FLIME_CONTROL_PAGE_MAGIC UINT32_C(0x50434c46)

#define EXPRESS_VK_FLIME_ROUTE_HEADER_SIZE 104
#define EXPRESS_VK_FLIME_ROUTE_RECORD_SIZE 64
#define EXPRESS_VK_FLIME_ROUTE_BUFFER_SIZE 24
#define EXPRESS_VK_FLIME_ROUTE_IMAGE_SIZE 24
#define EXPRESS_VK_FLIME_ROUTE_TEXEL_SIZE 8
#define EXPRESS_VK_FLIME_ROUTE_REPLY_SIZE 64
#define EXPRESS_VK_FLIME_CONTROL_PAGE_HEADER_SIZE 32
#define EXPRESS_VK_FLIME_CONTROL_PAGE_MAX_SIZE                         \
    (EXPRESS_VK_FLIME_CONTROL_PAGE_HEADER_SIZE +                       \
     EXPRESS_VK_FLIME_CONTROL_HEADER_SIZE +                            \
     EXPRESS_VK_FLIME_HARD_MAX_CHUNKS *                                \
         EXPRESS_VK_FLIME_CONTROL_BOUNDARY_SIZE)

#define EXPRESS_VK_FLIME_ROUTE_MAX_PACKET_BYTES (16u * 1024u * 1024u)
#define EXPRESS_VK_FLIME_ROUTE_MAX_RECORDS 4096u
#define EXPRESS_VK_FLIME_ROUTE_MAX_ELEMENTS (1024u * 1024u)

typedef enum ExpressVkFlimeRouteFlag {
    /* The first planned chunk of a logical queue submission. */
    EXPRESS_VK_FLIME_ROUTE_SUBMISSION_BEGIN = 1u << 0,
    /* The last planned chunk.  Only this makes a submission executable. */
    EXPRESS_VK_FLIME_ROUTE_SUBMISSION_FINAL = 1u << 1,
    /* Re-send retained writes after an explicit recovery transition. */
    EXPRESS_VK_FLIME_ROUTE_RECOVERY_REPLAY = 1u << 2,
    /* Plan-independent one-chunk path; still gated by the original submit. */
    EXPRESS_VK_FLIME_ROUTE_SINGLE_FLUSH = 1u << 3,
    EXPRESS_VK_FLIME_ROUTE_PROFILE_BOOTSTRAP =
        EXPRESS_VK_FLIME_ROUTE_SINGLE_FLUSH,
    /*
     * Synchronously materialize the still-unreleased normalized suffix and
     * remove the FLIME session.  This is the ordering bridge used before the
     * guest resumes an otherwise unsupported RPC on the generic path.
     */
    EXPRESS_VK_FLIME_ROUTE_FALLBACK_FLUSH = 1u << 4,
} ExpressVkFlimeRouteFlag;

typedef enum ExpressVkFlimeRouteRecordFlag {
    /* Informational: the guest converted a template-derived value to a write. */
    EXPRESS_VK_FLIME_ROUTE_RECORD_DERIVED = 1u << 0,
} ExpressVkFlimeRouteRecordFlag;

typedef enum ExpressVkFlimeRoutePayloadKind {
    EXPRESS_VK_FLIME_ROUTE_PAYLOAD_BUFFER = 1,
    EXPRESS_VK_FLIME_ROUTE_PAYLOAD_IMAGE = 2,
    EXPRESS_VK_FLIME_ROUTE_PAYLOAD_TEXEL = 3,
} ExpressVkFlimeRoutePayloadKind;

typedef enum ExpressVkFlimeRouteStatus {
    EXPRESS_VK_FLIME_ROUTE_ACCEPTED = 0,
    EXPRESS_VK_FLIME_ROUTE_INVALID = 1,
    EXPRESS_VK_FLIME_ROUTE_UNSUPPORTED = 2,
    EXPRESS_VK_FLIME_ROUTE_NOT_NEGOTIATED = 3,
    EXPRESS_VK_FLIME_ROUTE_RESOURCE_LIMIT = 4,
    EXPRESS_VK_FLIME_ROUTE_PLAN_MISMATCH = 5,
    EXPRESS_VK_FLIME_ROUTE_BAD_STATE = 6,
} ExpressVkFlimeRouteStatus;

typedef enum ExpressVkFlimeRouteReplyFlag {
    EXPRESS_VK_FLIME_ROUTE_REPLY_FALLBACK_REQUIRED = 1u << 0,
    EXPRESS_VK_FLIME_ROUTE_REPLY_DUPLICATE_SUPPRESSED = 1u << 1,
    EXPRESS_VK_FLIME_ROUTE_REPLY_DEFERRED_TO_SUBMIT = 1u << 2,
    EXPRESS_VK_FLIME_ROUTE_REPLY_RECOVERY = 1u << 3,
    EXPRESS_VK_FLIME_ROUTE_REPLY_RECOVERY_REQUIRED = 1u << 4,
    /* The fallback suffix is applied and the session/control sink is gone. */
    EXPRESS_VK_FLIME_ROUTE_REPLY_FALLBACK_DRAINED = 1u << 5,
} ExpressVkFlimeRouteReplyFlag;

typedef struct ExpressVkFlimeRouteHeader {
    uint32_t magic_le;
    uint16_t major_le;
    uint16_t minor_le;
    uint16_t header_bytes_le;
    uint16_t flags_le;
    uint32_t packet_bytes_le;
    uint32_t record_count_le;
    uint32_t submission_record_count_le;
    uint32_t records_bytes_le;
    uint32_t chunk_index_le;
    uint64_t process_id_le;
    uint64_t stream_id_le;
    uint64_t period_id_le;
    uint64_t plan_epoch_le;
    uint64_t submission_id_le;
    uint64_t guest_device_le;
    uint64_t guest_queue_le;
    uint32_t first_unit_le;
    uint32_t unit_past_end_le;
    uint64_t template_offset_le;
} ExpressVkFlimeRouteHeader;

typedef struct ExpressVkFlimeRouteRecord {
    uint64_t update_id_le;
    uint64_t template_offset_le;
    uint64_t guest_dst_set_le;
    uint32_t dst_binding_le;
    uint32_t dst_array_element_le;
    uint32_t descriptor_count_le;
    uint32_t descriptor_type_le;
    uint16_t payload_kind_le;
    uint16_t flags_le;
    uint32_t record_bytes_le;
    uint32_t payload_bytes_le;
    uint32_t reserved0_le;
    uint64_t reserved1_le;
} ExpressVkFlimeRouteRecord;

typedef struct ExpressVkFlimeRouteBuffer {
    uint64_t guest_buffer_le;
    uint64_t offset_le;
    uint64_t range_le;
} ExpressVkFlimeRouteBuffer;

typedef struct ExpressVkFlimeRouteImage {
    uint64_t guest_sampler_le;
    uint64_t guest_image_view_le;
    uint32_t image_layout_le;
    uint32_t reserved0_le;
} ExpressVkFlimeRouteImage;

typedef struct ExpressVkFlimeRouteTexel {
    uint64_t guest_buffer_view_le;
} ExpressVkFlimeRouteTexel;

typedef struct ExpressVkFlimeRouteReply {
    uint32_t magic_le;
    uint16_t major_le;
    uint16_t minor_le;
    uint16_t header_bytes_le;
    uint16_t status_le;
    uint32_t reply_bytes_le;
    uint32_t flags_le;
    uint32_t accepted_records_le;
    uint32_t queued_writes_le;
    uint32_t reserved_pad_le;
    uint64_t process_id_le;
    uint64_t stream_id_le;
    uint64_t submission_id_le;
    /*
     * Time from host transport receipt through route admission and native
     * descriptor realization.  A guest may subtract this from synchronous
     * round-trip time to isolate guest/transport handoff overhead.
     */
    uint64_t host_service_ns_le;
} ExpressVkFlimeRouteReply;

typedef enum ExpressVkFlimeControlPayloadKind {
    EXPRESS_VK_FLIME_CONTROL_PAYLOAD_CAPS = 1,
    EXPRESS_VK_FLIME_CONTROL_PAYLOAD_PLAN = 2,
} ExpressVkFlimeControlPayloadKind;

/*
 * The guest reads sequence, retries when it is odd, copies header + payload,
 * then accepts the snapshot only when the same even sequence is observed.
 */
typedef struct ExpressVkFlimeControlPageHeader {
    uint32_t magic_le;
    uint16_t major_le;
    uint16_t minor_le;
    uint16_t header_bytes_le;
    uint16_t payload_kind_le;
    uint32_t page_bytes_le;
    uint32_t payload_bytes_le;
    uint32_t reserved0_le;
    uint64_t sequence_le;
} ExpressVkFlimeControlPageHeader;

struct VirtIODevice;

/*
 * Persistent control-page identity.  The bridge retains the VirtIODevice,
 * never the transient VirtQueue iovec mapping; each asynchronous publication
 * resolves this DMA address again through the device AddressSpace.
 */
typedef struct ExpressVkFlimeControlSink {
    struct VirtIODevice *vdev;
    uint64_t guest_address;
    size_t capacity;
} ExpressVkFlimeControlSink;

typedef struct ExpressVkFlimeSubmitBatch ExpressVkFlimeSubmitBatch;
typedef struct ExpressVkFlimeReleaseBatch ExpressVkFlimeReleaseBatch;

typedef enum ExpressVkFlimeSubmitGate {
    /* No negotiated FLIME work is associated with this queue. */
    EXPRESS_VK_FLIME_SUBMIT_LEGACY = 0,
    /* A completely validated FINAL submission was detached for execution. */
    EXPRESS_VK_FLIME_SUBMIT_READY = 1,
    /* FLIME work exists but FINAL/sequence/plan validation is incomplete. */
    EXPRESS_VK_FLIME_SUBMIT_BLOCKED = 2,
    EXPRESS_VK_FLIME_SUBMIT_ERROR = 3,
} ExpressVkFlimeSubmitGate;

/*
 * Ingests a core metadata/control packet and publishes the pending plan when
 * one exists (otherwise the active plan) into control_out.  The packet must
 * carry a non-zero stream_id and process_id must equal transport_process_id.
 */
bool express_vk_flime_bridge_control(uint64_t transport_process_id,
                                     const void *packet,
                                     size_t packet_bytes,
                                     const ExpressVkFlimeControlSink *
                                         new_control_sink,
                                     void *control_out,
                                     size_t control_capacity,
                                     size_t *control_bytes,
                                     GError **error);

/*
 * Validates and stages routed writes; it never calls Vulkan.  A non-FINAL
 * planned chunk may detach a release batch so vk_trans can preserve its
 * descriptor bookkeeping and realize the chunk early.  The FINAL tail
 * remains gated until the original queue submission.
 */
bool express_vk_flime_bridge_route(uint64_t transport_process_id,
                                   const void *packet,
                                   size_t packet_bytes,
                                   uint64_t host_receive_ns,
                                   ExpressVkFlimeRouteReply *reply,
                                   ExpressVkFlimeReleaseBatch **release_batch,
                                   GError **error);

VkDevice express_vk_flime_bridge_release_device(
    const ExpressVkFlimeReleaseBatch *batch);
uint32_t express_vk_flime_bridge_release_write_count(
    const ExpressVkFlimeReleaseBatch *batch);
const VkWriteDescriptorSet *express_vk_flime_bridge_release_writes(
    const ExpressVkFlimeReleaseBatch *batch);
void express_vk_flime_bridge_complete_release(
    ExpressVkFlimeReleaseBatch *batch, bool applied,
    uint64_t host_realize_ns);

/*
 * Called immediately before the real vkQueueSubmit/vkQueueSubmit2.  READY
 * batches contain only the FINAL suffix not already realized by progressive
 * forwarding.  The caller records its existing descriptor hooks,
 * performs at most one vkUpdateDescriptorSets call, then executes the original
 * queue submission.
 */
ExpressVkFlimeSubmitGate express_vk_flime_bridge_prepare_submit(
    uint64_t transport_process_id, uint64_t guest_queue, VkQueue host_queue,
    VkDevice host_device, ExpressVkFlimeSubmitBatch **batch);

VkDevice express_vk_flime_bridge_batch_device(
    const ExpressVkFlimeSubmitBatch *batch);
uint32_t express_vk_flime_bridge_batch_write_count(
    const ExpressVkFlimeSubmitBatch *batch);
const VkWriteDescriptorSet *express_vk_flime_bridge_batch_writes(
    const ExpressVkFlimeSubmitBatch *batch);
/* Called after the FINAL vkUpdateDescriptorSets and before real QueueSubmit. */
bool express_vk_flime_bridge_submit_updates_applied(
    ExpressVkFlimeSubmitBatch *batch);
void express_vk_flime_bridge_complete_submit(ExpressVkFlimeSubmitBatch *batch,
                                              VkResult result,
                                              uint64_t host_realize_ns);

/* Last-thread process teardown and per-device fallback teardown entry points. */
void express_vk_flime_bridge_cleanup_process(uint64_t process_id);
void express_vk_flime_bridge_cleanup_device(uint64_t process_id,
                                             uint64_t guest_device);
/* Virtio reset invalidates every guest-owned control page on this transport. */
void express_vk_flime_bridge_reset_transport(void);
void express_vk_flime_bridge_shutdown(void);

#endif /* HW_EXPRESS_GPU_EXPRESS_VK_FLIME_BRIDGE_H */
