# Host-side Source

This directory preserves the vSoC/QEMU-relative paths of the published host
files and the surrounding integration context needed to inspect FLIME and
place it into a compatible host tree.

## Published Structure

| Area | Description | Source code |
| --- | --- | --- |
| FLIME core | Progressive state machine, learned-template validation, recovery, cost profiling, EWMA estimation, and dynamic-programming construction of forwarding boundaries. | [`hw/express-gpu/express_vk_flime.c`](hw/express-gpu/express_vk_flime.c)<br>[`include/hw/express-gpu/express_vk_flime.h`](include/hw/express-gpu/express_vk_flime.h) |
| Vulkan bridge | Wire-message decoding and the connection from FLIME control/route operations to descriptor realization and queue submission. | [`hw/express-gpu/express_vk_flime_bridge.c`](hw/express-gpu/express_vk_flime_bridge.c)<br>[`include/hw/express-gpu/express_vk_flime_bridge.h`](include/hw/express-gpu/express_vk_flime_bridge.h) |
| Device and Vulkan integration | Existing vSoC/QEMU paths containing the concrete RPC dispatch, native handle, descriptor, submit, reset, and lifecycle hooks. | [`hw/express-gpu/vk_trans.c`](hw/express-gpu/vk_trans.c)<br>[`include/hw/express-gpu/vk_trans.h`](include/hw/express-gpu/vk_trans.h)<br>[`hw/express-gpu/express_gpu.c`](hw/express-gpu/express_gpu.c) |
| Transport and batching | Cluster preflight, checked multi-buffer calls, writable responses, batching, FLIME routing, reset, and teardown integration. | [`hw/teleport-express/`](hw/teleport-express/)<br>[`include/hw/teleport-express/`](include/hw/teleport-express/) |
| Build integration | Meson source lists for the selected GPU and transport files. | [`hw/express-gpu/meson.build`](hw/express-gpu/meson.build)<br>[`hw/teleport-express/meson.build`](hw/teleport-express/meson.build) |

## Reading the Implementation

The paper's host-side control logic begins in `express_vk_flime.c`. Follow its
validated plans into `express_vk_flime_bridge.c`, then use `vk_trans.c` to see
the native descriptor realization, control and route RPC handlers, and
submission gates. Adaptive Forwarding profiles guest preparation, host
realization, and handoff costs, smooths those measurements with EWMA, and uses
dynamic programming to select forwarding boundaries for subsequent periods.
The `teleport-express` files show how the resulting forwarding decisions
interact with batching, response ownership, failure, reset, and teardown.

`vk_trans.c` and the transport sources retain surrounding baseline logic because
FLIME's correctness depends on the relative ordering of ordinary Vulkan calls,
learned fast paths, synchronous responses, and lifecycle events.

## Integration Dependencies

A target host tree supplies the generic Vulkan decoder, handle mapping, display
and platform backends, and the surrounding QEMU/vSoC device infrastructure.
The porting steps below describe where the released FLIME components connect
to those services.

## Porting

Start from a compatible [vSoC](https://github.com/VirtualSoC/vsoc) host tree
based on QEMU 7.1 and preserve the relative `hw/` and `include/hw/` layout.

1. Merge the two supplied Meson source lists into the target build; do not
   replace unrelated platform entries.
2. Connect the FLIME bridge at the existing control/route RPC, descriptor
   realization, queue-submit, device-teardown, and process-teardown boundaries.
3. Preserve the guest/host function identifiers, wire structures and sizes,
   call ordering, writable response lengths, and checked multi-buffer cluster
   semantics. Treat guest and host as one protocol version.
4. Wire the profiling path so guest-preparation, host-realization, and handoff
   samples reach the FLIME core, preserving timing units and unit/chunk indices
   used by its EWMA estimator and dynamic-programming planner.
5. Retain the baseline stack's handle mapping, generic decoding, display, and
   platform integration around these hooks.

The host does not require the target Android WSI implementation, but it must
agree with the guest on resource identifiers, memory-transfer behavior,
failure handling, and queue-submit ordering.
