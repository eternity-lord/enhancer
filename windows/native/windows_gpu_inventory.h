#ifndef ENHANCER_WINDOWS_GPU_INVENTORY_H
#define ENHANCER_WINDOWS_GPU_INVENTORY_H

#include <cstdint>
#include <string>
#include <vector>

namespace enhancer {

// Windows adapter inventory is intentionally separate from the ncnn Vulkan list.  A DXGI
// adapter can be physically present even when its Vulkan driver is missing or too old for
// ncnn; keeping the lists separate prevents a non-Vulkan adapter index from being sent to
// ncnn while still making the hardware visible to the Studio diagnostics screen.
struct DisplayAdapterDesc {
    int index = 0;
    std::uint32_t vendor_id = 0;
    std::uint32_t type = 3; // 0=Discrete, 1=Integrated, 2=Virtual, 3=Software/Other
    std::uint64_t dedicated_video_memory_mb = 0;
    std::uint64_t shared_system_memory_mb = 0;
    bool is_software = false;
    bool is_remote = false;
    bool is_vulkan_visible = false;
    std::string name;
};

std::vector<DisplayAdapterDesc> enumerate_windows_display_adapters();

} // namespace enhancer

#endif // ENHANCER_WINDOWS_GPU_INVENTORY_H
