# FLIME

This repository contains the anonymized implementation artifact and project
page for **FLIME: Streamlined Mobile GPU Virtualization for Efficient LLM
Inference**.

The public landing page is defined in [`index.md`](index.md). The released
implementation snapshot is under [`code/`](code/) and is divided into the host and guest
sides of the virtual GPU.

## Code Organization

| Side | Published structure | Start here |
| --- | --- | --- |
| [Host](code/host/) | FLIME state and adaptive-forwarding planning, the Vulkan bridge, integrated device/backend hooks, transport/batching, and Meson source lists. | [`express_vk_flime.c`](code/host/hw/express-gpu/express_vk_flime.c), [`express_vk_flime_bridge.c`](code/host/hw/express-gpu/express_vk_flime_bridge.c), and the [host guide](code/host/README.md) |
| [Guest](code/guest/) | FLIME core, descriptor specialization, command/submit tracking, the integrated Android Vulkan HAL, generated Vulkan support, and `Android.bp`. | [`express_vk_flime_guest_core.cpp`](code/guest/express_vk_flime_guest_core.cpp), [`express_vk_flime_guest_descriptor.cpp`](code/guest/express_vk_flime_guest_descriptor.cpp), [`all_out_vk.cpp`](code/guest/all_out_vk.cpp), and the [guest guide](code/guest/README.md) |

The detailed [Code Organization](index.md#code-organization) section maps the
paper mechanisms to published files. Component-level documentation is available
in the [code overview](code/README.md).

## For Developers

This source snapshot has been anonymized for double-blind review and prepared
through the standard organizational publication-clearance process. It retains
the surrounding integration context needed to inspect FLIME, with identifying
metadata and unrelated mechanisms outside the paper design omitted.

Platform-specific dependencies are supplied by the target Android and emulator
trees. The [Porting Guide](index.md#porting-guide) explains how to connect the
released components to a compatible system, with additional details in the
[host guide](code/host/README.md) and [guest guide](code/guest/README.md).

### Local Preview

With Ruby and Bundler installed:

```bash
bundle install
bundle exec jekyll serve
```

Then open <http://127.0.0.1:4000/>. GitHub Pages will build the site
automatically after Pages is enabled for the repository.
