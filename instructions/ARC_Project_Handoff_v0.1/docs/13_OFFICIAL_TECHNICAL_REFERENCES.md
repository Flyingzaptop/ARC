# Official technical references

These are reference points for implementation decisions. Re-check current API documentation while coding.

## Microsoft / D3D12 / DXGI

### D3D12 Residency
Microsoft documents the video-memory budget as dynamic and notes that applications should remain within that budget. It points to `IDXGIAdapter3` budget APIs and D3D12 residency management.

Reference:
https://learn.microsoft.com/en-us/windows/win32/direct3d12/residency

### IDXGIAdapter3::QueryVideoMemoryInfo
Use this family of DXGI APIs to obtain current process memory budget/usage information.

Reference:
https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_4/nf-dxgi1_4-idxgiadapter3-queryvideomemoryinfo

## Khronos / Vulkan

### Vulkan Layers
Khronos describes layers as dynamically loaded components capable of intercepting, evaluating and modifying Vulkan functions between the application and implementation.

Reference:
https://github.khronos.org/Vulkan-Site/guide/latest/layers.html

### VK_EXT_memory_budget
This extension exposes heap budget and estimated usage and explicitly describes reacting to memory pressure, including freeing/moving data or changing mip levels.

Reference:
https://registry.khronos.org/vulkan/specs/latest/man/html/VK_EXT_memory_budget.html

### Sparse resources
Vulkan sparse resources allow non-contiguous binding and, where sparse residency features are supported, partial residency.

Reference:
https://registry.khronos.org/vulkan/specs/latest-ratified/pdf/vkspec.pdf

## AMD GPUOpen

### FidelityFX Super Resolution 3
FSR 3 is available as open source under MIT. AMD documents DX12/Vulkan integration and frame-generation workloads, including asynchronous execution options.

Reference:
https://gpuopen.com/fidelityfx-super-resolution-3/

### FSR 3.1
FSR 3.1 separates frame generation from upscaling.

Reference:
https://gpuopen.com/learn/amd_fsr_3_1_release/

## Mesa / AMD

### RADV
Mesa documents RADV as a userspace Vulkan driver for AMD GCN/RDNA GPUs and describes the split between user-mode and kernel-mode driver responsibilities.

Reference:
https://docs.mesa3d.org/drivers/radv.html

## NVIDIA

### Open GPU Kernel Modules
NVIDIA's repository is specifically Linux open GPU kernel-module source. Its README states that the modules are used with corresponding GSP firmware and NVIDIA user-space driver components.

Reference:
https://github.com/NVIDIA/open-gpu-kernel-modules

## Design implication

These references support the current project decision:

- build ARC first above the vendor driver;
- use official memory-budget/residency mechanisms;
- use Vulkan layers for Vulkan observation/integration where appropriate;
- treat a fully custom Windows GPU driver as a separate R&D track;
- use AMD/Linux/RADV as the most practical open driver laboratory later.
