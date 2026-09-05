# Guest-side Source

This directory contains a curated set of Android Vulkan guest files with the
integration context needed to follow FLIME. The FLIME-specific files are
accompanied by an integration snapshot of `all_out_vk.cpp` and the generated
Vulkan helpers needed to understand its call path.

## Published Structure

| Area | Description | Source code |
| --- | --- | --- |
| Vulkan HAL integration | Android HAL export, normal Vulkan RPC behavior, FLIME interception hooks, and the ordering boundaries among descriptors, command recording, batching, submission, and lifecycle events. | [`all_out_vk.cpp`](all_out_vk.cpp) |
| FLIME core | Protocol state, feature negotiation, matching, routing, recovery, and shared internal state. | [`express_vk_flime_guest_core.cpp`](express_vk_flime_guest_core.cpp)<br>[`express_vk_flime_guest_internal.h`](express_vk_flime_guest_internal.h) |
| Descriptor specialization | Descriptor shadowing, update-template capture, signatures, matching, fast paths, and fallback. | [`express_vk_flime_guest_descriptor.cpp`](express_vk_flime_guest_descriptor.cpp) |
| Command and submit path | Command recording, batching boundaries, queue-submit tracking, recovery, and fallback ordering. | [`express_vk_flime_guest_command.cpp`](express_vk_flime_guest_command.cpp) |
| Public integration API | Lifecycle and hook interface called by the integrated Vulkan entry points. | [`express_vk_flime_guest.cpp`](express_vk_flime_guest.cpp)<br>[`express_vk_flime_guest.h`](express_vk_flime_guest.h) |
| Generated Vulkan support | Generated entry-point declarations plus encoder, deep-copy, extension-structure, and counting helpers used by the normal RPC path. | [`all_out_vk_gen.cpp`](all_out_vk_gen.cpp)<br>[`all_out_vk_gen.h`](all_out_vk_gen.h)<br>[`express_vk_encode_to_stream.cpp`](express_vk_encode_to_stream.cpp)<br>[`express_vk_deepcopy_guest.cpp`](express_vk_deepcopy_guest.cpp)<br>[`express_vk_extension_structs_guest.cpp`](express_vk_extension_structs_guest.cpp)<br>[`express_vk_counting_guest.cpp`](express_vk_counting_guest.cpp) |
| Platform support | Android native-buffer declarations, Vulkan type definitions, allocator helpers, and compatibility headers. | [`express_vk_android_native_buffer_gfxstream.h`](express_vk_android_native_buffer_gfxstream.h)<br>[`vulkan_gfxstream.h`](vulkan_gfxstream.h)<br>[`define_vk.h`](define_vk.h)<br>[`vk_platform_compat.h`](vk_platform_compat.h) |
| Build integration | Soong definition for the Android Vulkan HAL shared library. | [`Android.bp`](Android.bp) |

## Reading the Implementation

Begin with `express_vk_flime_guest_core.cpp`, then read the descriptor and
command files for the specialization, matching, recording, submit, fallback,
and recovery paths described in the paper. `express_vk_flime_guest.cpp` exposes
the small integration API. Search for those hooks in `all_out_vk.cpp` to see
their exact placement relative to ordinary Vulkan encoding and transport.

`all_out_vk.cpp` is included as a curated integration snapshot. It mixes FLIME
with the baseline Android Vulkan HAL and RPC path because specialization depends
on when state is observed, when calls are frozen or forwarded, when batches
end, and how failures and teardown return to the normal path. The generated
support files retain the surrounding Vulkan plumbing needed to follow this
path. The file also retains the baseline mapped-memory shadow, flush, and
invalidate fallback used by the surrounding RPC driver; that coherency
plumbing is separate from FLIME's Adaptive Forwarding cost model and planner.

## Integration Dependencies

A target guest tree supplies its RPC transport, Gralloc implementation, Android
product configuration, and other platform services. ParamManager marks the
transport boundary used by this snapshot, while Android.bp shows how the
released sources are assembled into the Vulkan HAL module.

## Porting the Guest Core

Add the released sources to the target Vulkan guest-driver module and replace
the `ParamManager` boundary with that system's guest–host RPC transport.
The replacement must preserve frozen-call buffer ownership, call ordering,
synchronous response behavior, batch boundaries, and transport-failure
handling. Keep the lifecycle, descriptor, command-recording, and queue-submit
hooks at the ordering points shown in `all_out_vk.cpp`. The host must use the
same FLIME wire structures, sizes, function identifiers, and protocol version.

### Android 13 Vulkan HAL Discovery

The released code targets the Vulkan HAL used by our Android 13 system image.
The names below are the concrete configuration of this artifact; they are not
mandatory names for every Android Vulkan driver.

| Item | Configuration used by this artifact |
| --- | --- |
| Soong module | `vulkan.express` |
| 64-bit installed library | `/vendor/lib64/hw/vulkan.express.so` |
| Vulkan HAL selector | `ro.hardware.vulkan=express` |
| Product package | `PRODUCT_PACKAGES += vulkan.express` |

This target uses Android's Vulkan HAL mechanism, not the ICD JSON mechanism
used by desktop Vulkan loaders. The shared library must export
`HAL_MODULE_INFO_SYM` with default symbol visibility, set its module identifier
to `HWVULKAN_HARDWARE_MODULE_ID`, and accept `HWVULKAN_DEVICE_0` in the module
`open` callback. The returned `hwvulkan_device_t` supplies
`EnumerateInstanceExtensionProperties`, `CreateInstance`, and
`GetInstanceProcAddr`, through which the Android loader discovers the remaining
Vulkan entry points. A port must also preserve Android's required dispatchable
handle layout and initialize compatible HAL module and device versions.

The supplied `Android.bp` defines `vulkan.express`. The target device product
makefile must also add `vulkan.express` to `PRODUCT_PACKAGES` and set
`ro.hardware.vulkan=express`. Defining a Soong module alone does not place the
library in the product image, and a filename that does not match the selected
HAL suffix will not be selected. On a product that builds a 32-bit vendor ABI,
the corresponding library normally resides under `/vendor/lib/hw/`. A port may
choose a different suffix, but its module name, installed filename, product
property, and ABI must agree.

### Android WSI and Swapchain Integration

At the application boundary, Android's system `libvulkan.so` exposes
`VK_KHR_surface`, `VK_KHR_android_surface`, and `VK_KHR_swapchain`. The system
layer owns `VkSurfaceKHR`, `VkSwapchainKHR`, and direct `ANativeWindow`
interaction; the HAL driver cooperates with it through Android's private
`VK_ANDROID_native_buffer` interface.

Adapting this guest driver to another Android release or device therefore
primarily replaces the platform-facing WSI and shared-buffer path:

- `ANativeWindow` and `BufferQueue` integration;
- gralloc allocation, usage flags, and native-buffer import;
- swapchain image acquisition and presentation;
- acquire/release fences and the platform's native synchronization mechanism;
- Android extensions and external-memory capabilities actually supported by
  the target.

The FLIME descriptor-shadowing, descriptor-template, command-recording,
protocol, and queue-submit optimization remains below this platform boundary.
The integration calls may need to be rewired when a target driver uses
different wrapper classes or generated Vulkan dispatch code, but the core does
not need to be redesigned merely because the WSI implementation changes.

Vulkan feature XML files installed in Android permissions directories declare
capabilities visible to the framework, PackageManager, and applications. They
are not driver discovery manifests. Values such as
`android.hardware.vulkan.version`, `android.hardware.vulkan.level`,
`android.hardware.vulkan.compute`, and `android.software.vulkan.deqp.level`
must not claim capabilities beyond those actually implemented and validated;
the declared dEQP level must be consistent with the conformance tests the
system passes.

### Linux and Windows Guests

A non-Android port replaces the Android-specific HAL, WSI, native-buffer, and
synchronization adapter with the target platform's equivalents. Linux commonly
uses Wayland, XCB, or Xlib surface extensions, while Windows uses
`VK_KHR_win32_surface` and Win32 presentation primitives. These systems use the
Khronos loader/driver interface: Linux normally discovers ICD JSON manifests
from standard manifest locations, while Windows uses its driver registry
configuration. This is separate from Android Vulkan HAL discovery.

The platform adapter changes, but the FLIME descriptor/template/submit core and
guest–host wire protocol remain reusable.

## References

- [AOSP: Implement Vulkan](https://source.android.com/docs/core/graphics/implement-vulkan)
- [AOSP: Android Vulkan architecture](https://source.android.com/docs/core/graphics/arch-vulkan)
- [Android 13 Compatibility Definition: Vulkan](https://source.android.com/docs/compatibility/13/android-13-cdd)
- [Android Vulkan hardware features](https://developer.android.com/guide/topics/manifest/uses-feature-element#vulkan-hardware-features)
- [AOSP Soong build system](https://android.googlesource.com/platform/build/soong/+/master/README.md)
- [Khronos Loader–Driver Interface](https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderDriverInterface.md)
