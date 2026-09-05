# FLIME Implementation Source

This directory contains the source snapshot for the implementation
described in the FLIME paper. It is divided into the [`host`](host/) and
[`guest`](guest/) sides of the virtual GPU.

## Code Organization

| Directory | Contents | Detailed guide |
| --- | --- | --- |
| [`host/`](host/) | Host state machine, profiling and adaptive-forwarding planning, Vulkan bridge and backend hooks, transport/batching integration, headers, and Meson source lists. | [Host-side source and porting](host/README.md) |
| [`guest/`](guest/) | Guest protocol and lifecycle, descriptor/template specialization, command/submit tracking, integrated Android Vulkan HAL entry points, generated Vulkan helpers, platform headers, and the Soong module. | [Guest-side source and porting](guest/README.md) |

## For Developers

The host and guest files preserve the surrounding Vulkan, RPC, submission, and
lifecycle context needed to inspect FLIME. This includes curated snapshots of
integration-heavy files such as `vk_trans.c` and `all_out_vk.cpp`.

The snapshot has been anonymized for double-blind review and prepared through
the standard organizational publication-clearance process. Platform services
and baseline components supplied by the target Android, QEMU, and virtual-GPU
trees are represented by the integration boundaries documented in the host and
guest guides.

> **Implementation-size note:** Because the snapshot preserves integration
> context and relies on platform components from the target trees, its raw line
> count is not directly comparable with the FLIME-specific implementation count
> reported in the paper.

### Reading the Paper Implementation

For progressive specialization, begin with
`guest/express_vk_flime_guest_core.cpp`,
`guest/express_vk_flime_guest_descriptor.cpp`, and
`guest/express_vk_flime_guest_command.cpp`; then follow their protocol messages
through `host/hw/express-gpu/express_vk_flime.c` and
`host/hw/express-gpu/express_vk_flime_bridge.c`.

For Adaptive Forwarding, inspect the profiling, EWMA-estimation, and
dynamic-programming planner paths in
`host/hw/express-gpu/express_vk_flime.c`, then follow their profile, control,
and route messages through `host/hw/express-gpu/express_vk_flime_bridge.c` and
`host/hw/teleport-express/`. The planner combines guest-preparation,
host-realization, and handoff costs to select forwarding boundaries for
subsequent periods. The larger `guest/all_out_vk.cpp` and
`host/hw/express-gpu/vk_trans.c` files show how these mechanisms connect to
normal Android Vulkan HAL calls, RPC, native descriptor realization, queue
submission, batching, reset, and teardown.

The target trees supply the baseline services identified in the host and guest
guides. When connecting them, preserve the published guest/host wire constants,
function identifiers, structure layouts, response sizes, and call ordering.
