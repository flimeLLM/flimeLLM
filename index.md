---
layout: default
---

![Platform: Android](https://img.shields.io/badge/Platform-Android-green)
![Code: released](https://img.shields.io/badge/Code-released-blue)

## Table of Contents

- [Introduction](#introduction)
- [Progressive Specialization](#progressive-specialization)
- [Code Organization](#code-organization)
- [For Developers](#for-developers)
  - [Porting Guide](#porting-guide)
- [Evaluation Snapshot](#evaluation-snapshot)

## Introduction

State-of-the-art mobile GPU virtualization delivers near-native performance
for rendering workloads on commodity PCs and servers. Yet under the same
virtualization stack, mobile LLM inference remains 2–5× slower than native
execution, often producing an undesirable user experience. Our analysis finds
two main causes: the repeated dependency-resolution cost of frequent
configuration calls, and limited pipeline parallelism under autoregressive
token dependencies.

We present FLIME, a mobile-emulator design that exploits cross-iteration
regularity in LLM inference. FLIME progressively recognizes recurring GPU-call
sequences, specializes their virtualized execution, removes redundant work
across iterations, and exposes additional forwarding and execution
parallelism. Across representative mobile LLM workloads, FLIME narrows the
emulator-to-native slowdown from 2.2–5.2× to 1.2–2.1× while adding negligible
overhead to unrelated applications. This page provides the anonymized
implementation artifact used to inspect and port the design.

## Progressive Specialization

<figure class="project-figure project-figure--wide">
  <img src="{{ '/assets/images/progressive-specialization.png' | relative_url }}" alt="FLIME progresses through Detect, Learn, Match, Fast, and Recover states as workloads change." loading="lazy">
  <figcaption>Figure 1: Progressive specialization in FLIME.</figcaption>
</figure>

## Code Organization

**The code is available [here](https://github.com/flimeLLM/flimeLLM/tree/main/code).** The release is split into [`code/host/`](https://github.com/flimeLLM/flimeLLM/tree/main/code/host)
and [`code/guest/`](https://github.com/flimeLLM/flimeLLM/tree/main/code/guest).
The tables use a directory–role–source presentation and group files by logical
FLIME component so that the main implementation path remains visible.

### Host-side Source

| Area | Role | Main published source |
| --- | --- | --- |
| FLIME core | Progressive state machine, template validation, recovery, cost profiling, EWMA estimation, and dynamic-programming construction of forwarding boundaries. | [`express_vk_flime.c`](https://github.com/flimeLLM/flimeLLM/blob/main/code/host/hw/express-gpu/express_vk_flime.c)<br>[`express_vk_flime.h`](https://github.com/flimeLLM/flimeLLM/blob/main/code/host/include/hw/express-gpu/express_vk_flime.h) |
| Vulkan bridge | Decodes FLIME control and route messages and connects learned plans to descriptor realization and queue submission. | [`express_vk_flime_bridge.c`](https://github.com/flimeLLM/flimeLLM/blob/main/code/host/hw/express-gpu/express_vk_flime_bridge.c)<br>[`express_vk_flime_bridge.h`](https://github.com/flimeLLM/flimeLLM/blob/main/code/host/include/hw/express-gpu/express_vk_flime_bridge.h) |
| Vulkan/device integration | Existing vSoC/QEMU Vulkan and device paths containing the concrete RPC, handle, submit, and lifecycle hooks. | [`vk_trans.c`](https://github.com/flimeLLM/flimeLLM/blob/main/code/host/hw/express-gpu/vk_trans.c)<br>[`vk_trans.h`](https://github.com/flimeLLM/flimeLLM/blob/main/code/host/include/hw/express-gpu/vk_trans.h)<br>[`express_gpu.c`](https://github.com/flimeLLM/flimeLLM/blob/main/code/host/hw/express-gpu/express_gpu.c) |
| Transport and batching | FLIME message transport, cluster preflight, checked batching, writable-response handling, reset, and teardown integration. | [`hw/teleport-express/`](https://github.com/flimeLLM/flimeLLM/tree/main/code/host/hw/teleport-express)<br>[`include/hw/teleport-express/`](https://github.com/flimeLLM/flimeLLM/tree/main/code/host/include/hw/teleport-express) |
| Build integration | Meson source lists for the selected host files. | [`express-gpu/meson.build`](https://github.com/flimeLLM/flimeLLM/blob/main/code/host/hw/express-gpu/meson.build)<br>[`teleport-express/meson.build`](https://github.com/flimeLLM/flimeLLM/blob/main/code/host/hw/teleport-express/meson.build) |

### Guest-side Source

| Area | Role | Main published source |
| --- | --- | --- |
| Vulkan HAL integration | Curated integration snapshot showing Android HAL export, normal RPC behavior, FLIME hooks, and ordering boundaries. | [`all_out_vk.cpp`](https://github.com/flimeLLM/flimeLLM/blob/main/code/guest/all_out_vk.cpp) |
| FLIME core | Protocol state, negotiation, routing, matching, recovery, and shared internal state. | [`express_vk_flime_guest_core.cpp`](https://github.com/flimeLLM/flimeLLM/blob/main/code/guest/express_vk_flime_guest_core.cpp)<br>[`express_vk_flime_guest_internal.h`](https://github.com/flimeLLM/flimeLLM/blob/main/code/guest/express_vk_flime_guest_internal.h) |
| Descriptor specialization | Descriptor shadowing, update-template capture, signature construction, matching, and fallback. | [`express_vk_flime_guest_descriptor.cpp`](https://github.com/flimeLLM/flimeLLM/blob/main/code/guest/express_vk_flime_guest_descriptor.cpp) |
| Command and submit path | Command recording, batching boundaries, queue submission, and recovery transitions. | [`express_vk_flime_guest_command.cpp`](https://github.com/flimeLLM/flimeLLM/blob/main/code/guest/express_vk_flime_guest_command.cpp) |
| Public integration API | Lifecycle and hook interface used by the integrated Vulkan entry points. | [`express_vk_flime_guest.cpp`](https://github.com/flimeLLM/flimeLLM/blob/main/code/guest/express_vk_flime_guest.cpp)<br>[`express_vk_flime_guest.h`](https://github.com/flimeLLM/flimeLLM/blob/main/code/guest/express_vk_flime_guest.h) |
| Generated Vulkan support | Encoder, deep-copy, extension-structure, counting, and generated dispatch support retained to make the surrounding call path understandable. | [`all_out_vk_gen.cpp`](https://github.com/flimeLLM/flimeLLM/blob/main/code/guest/all_out_vk_gen.cpp)<br>[`express_vk_encode_to_stream.cpp`](https://github.com/flimeLLM/flimeLLM/blob/main/code/guest/express_vk_encode_to_stream.cpp)<br>[`express_vk_deepcopy_guest.cpp`](https://github.com/flimeLLM/flimeLLM/blob/main/code/guest/express_vk_deepcopy_guest.cpp)<br>[`express_vk_extension_structs_guest.cpp`](https://github.com/flimeLLM/flimeLLM/blob/main/code/guest/express_vk_extension_structs_guest.cpp) |
| Platform and build support | Android native-buffer declarations, Vulkan compatibility headers, and the Soong module definition. | [`express_vk_android_native_buffer_gfxstream.h`](https://github.com/flimeLLM/flimeLLM/blob/main/code/guest/express_vk_android_native_buffer_gfxstream.h)<br>[`vulkan_gfxstream.h`](https://github.com/flimeLLM/flimeLLM/blob/main/code/guest/vulkan_gfxstream.h)<br>[`Android.bp`](https://github.com/flimeLLM/flimeLLM/blob/main/code/guest/Android.bp) |

For the implementation described in the paper, begin with the FLIME core,
descriptor, and command files on the guest, then follow their wire messages
through the host core and bridge. Adaptive Forwarding profiles guest
preparation, host realization, and handoff costs. The host core smooths these
measurements with an exponentially weighted moving average (EWMA) and uses
dynamic programming to select forwarding boundaries for subsequent periods.
The transport files preserve the corresponding message, batching, and response
semantics. The larger `all_out_vk.cpp` and `vk_trans.c` files are most useful
for understanding where these mechanisms enter the normal Vulkan HAL, RPC,
queue-submit, and teardown flows. More file-level notes are available in the
[host README](https://github.com/flimeLLM/flimeLLM/blob/main/code/host/README.md)
and [guest README](https://github.com/flimeLLM/flimeLLM/blob/main/code/guest/README.md).

## For Developers

This source snapshot has been anonymized for double-blind review and prepared
through the standard organizational publication-clearance process. It retains
the surrounding integration context needed to inspect FLIME, with identifying
metadata and unrelated mechanisms outside the paper design omitted.

Platform-specific dependencies are supplied by the target Android and emulator
trees. The guide below identifies these integration boundaries and explains how
to connect the released host and guest components to a compatible system.

### Porting Guide

#### Host Integration

The published host layout follows a vSoC host tree based on QEMU 7.1. A port
should merge the two supplied Meson source lists into the target build and
connect the FLIME bridge at five existing boundaries: control/route RPC
dispatch, descriptor realization, queue submission, device teardown, and
process teardown. The target transport must preserve the published wire
function identifiers, structure sizes, call ordering, checked multi-buffer
batching, and writable response lengths. The guest and host must be updated as
one protocol pair.

Adaptive Forwarding depends on profiling hooks for guest preparation, host
realization, and handoff. The host core smooths these timing samples with EWMA
and applies dynamic programming to select forwarding boundaries for subsequent
periods. A port must preserve the profile indices, timing units, plan delivery,
and route ordering across the guest/host protocol. Handle mapping, display
integration, and generic Vulkan decoding remain responsibilities of the
baseline virtual-GPU stack and are not replaced by FLIME.

#### Guest Driver and Android 13 HAL Discovery

The released guest code targets the Vulkan HAL used by our Android 13 system
image. These identifiers describe this artifact's integration; Android does
not require every vendor driver to use the suffix `express`.

| Item | Configuration used by this artifact |
| --- | --- |
| Soong module | `vulkan.express` |
| 64-bit installed library | `/vendor/lib64/hw/vulkan.express.so` |
| Vulkan HAL selector | `ro.hardware.vulkan=express` |
| Product package | `PRODUCT_PACKAGES += vulkan.express` |

This target uses Android's Vulkan HAL mechanism, not the ICD JSON mechanism
normally used by desktop Vulkan loaders. The shared library must export
`HAL_MODULE_INFO_SYM` with default symbol visibility, use
`HWVULKAN_HARDWARE_MODULE_ID`, and accept `HWVULKAN_DEVICE_0` in its module
`open` callback. The returned `hwvulkan_device_t` supplies
`EnumerateInstanceExtensionProperties`, `CreateInstance`, and
`GetInstanceProcAddr`; the Android loader discovers the remaining Vulkan entry
points through these hooks. The published `all_out_vk.cpp` shows this concrete
HAL export.

`Android.bp` defines the shared library, while the target product makefile must
add it to `PRODUCT_PACKAGES` and set the matching HAL selector. Defining the
Soong module alone does not install it into the product image. On a product
that also exposes a 32-bit vendor ABI, the corresponding library normally
resides under `/vendor/lib/hw/`. A port may choose another HAL suffix, but the
module name, installed filename, product property, and architecture must agree.

#### Android WSI and Swapchain Integration

At the application boundary, Android's system `libvulkan.so` exposes
`VK_KHR_surface`, `VK_KHR_android_surface`, and `VK_KHR_swapchain`, owns the
surface/swapchain objects and direct `ANativeWindow` interaction, and
cooperates with the HAL driver through Android's private
`VK_ANDROID_native_buffer` interface. Porting the guest to another Android
release or device therefore primarily changes the platform-facing WSI and
memory-sharing path:

- `ANativeWindow` and `BufferQueue` integration;
- gralloc allocation, usage flags, and native-buffer import;
- swapchain image acquisition and presentation;
- acquire/release fences and native synchronization;
- advertised Android extensions and external-memory capabilities.

The FLIME descriptor shadowing, template matching, command recording, protocol,
and queue-submit optimization remain below this boundary. Their call sites may
need to be rewired to a target driver's wrapper classes or generated dispatch
layer, but the core mechanisms do not need to be redesigned solely because the
WSI implementation changes.

Vulkan feature XML files in the Android permissions directories advertise
capabilities to the framework, PackageManager, and applications; they are not
driver-loading manifests. Declarations such as
`android.hardware.vulkan.version`, `android.hardware.vulkan.level`,
`android.hardware.vulkan.compute`, and `android.software.vulkan.deqp.level`
must match capabilities that are actually implemented and validated. In
particular, a declared dEQP level must be consistent with the conformance tests
that the system passes.

#### Linux and Windows Guests

A non-Android guest replaces the Android WSI, native-buffer, synchronization,
and HAL adapter with its platform equivalents. Linux commonly uses Wayland,
XCB, or Xlib surface extensions, while Windows uses `VK_KHR_win32_surface` and
Win32 presentation primitives. These systems use the Khronos loader/driver
interface and its platform-specific ICD discovery, rather than Android's
Vulkan HAL discovery. The FLIME descriptor/template/submit core and wire
protocol remain reusable.

Porting references:

- [AOSP: Implement Vulkan](https://source.android.com/docs/core/graphics/implement-vulkan)
- [AOSP: Android Vulkan architecture](https://source.android.com/docs/core/graphics/arch-vulkan)
- [Android 13 Compatibility Definition: Vulkan](https://source.android.com/docs/compatibility/13/android-13-cdd)
- [Android Vulkan hardware features](https://developer.android.com/guide/topics/manifest/uses-feature-element#vulkan-hardware-features)
- [AOSP Soong build system](https://android.googlesource.com/platform/build/soong/+/master/README.md)
- [Khronos Loader–Driver Interface](https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderDriverInterface.md)

## Evaluation Snapshot

The Android benchmark harness follows the paper's evaluation protocol: a
256-token prompt, 128-token greedy decoding, five runs per configuration, and
exclusion of the first eight decode tokens from steady-state measurements.
Across six backend–model combinations spanning three model families and
135M–3B parameters, FLIME reduced GAE decode latency by 48.9–81.9%, lowering
the average emulator-to-native slowdown from 4.51× to 1.39×.

<figure class="project-figure project-figure--app">
  <img src="{{ '/assets/images/mobile-llm-evaluation.png' | relative_url }}" alt="Android Vulkan LLM benchmark interface configured for llama.cpp and SmolLM2 135M with a 256-token prompt and 128-token decode." loading="lazy">
  <figcaption>Android benchmark harness configured for the llama.cpp / SmolLM2-135M workload. The screenshot shows the workload configuration, not a standalone reproduction of the aggregate results.</figcaption>
</figure>
