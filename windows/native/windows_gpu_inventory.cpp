#include "windows_gpu_inventory.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <utility>

#pragma comment(lib, "dxgi.lib")

namespace {

std::string wide_to_utf8(const wchar_t* value) {
    if (!value || value[0] == L'\0') {
        return {};
    }
    const int needed = ::WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value,
        -1,
        nullptr,
        0,
        nullptr,
        nullptr
    );
    if (needed <= 1) {
        return {};
    }
    std::string result(static_cast<size_t>(needed - 1), '\0');
    ::WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value,
        -1,
        result.data(),
        needed - 1,
        nullptr,
        nullptr
    );
    return result;
}

} // namespace

namespace enhancer {

std::vector<DisplayAdapterDesc> enumerate_windows_display_adapters() {
    std::vector<DisplayAdapterDesc> result;

    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(::CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return result;
    }

    for (UINT adapter_index = 0;; ++adapter_index) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(adapter_index, &adapter) == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (!adapter) {
            continue;
        }

        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc))) {
            continue;
        }

        DisplayAdapterDesc out;
        out.index = static_cast<int>(adapter_index);
        out.vendor_id = desc.VendorId;
        out.dedicated_video_memory_mb = static_cast<std::uint64_t>(desc.DedicatedVideoMemory) / (1024ull * 1024ull);
        out.shared_system_memory_mb = static_cast<std::uint64_t>(desc.SharedSystemMemory) / (1024ull * 1024ull);
        out.is_software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        out.is_remote = (desc.Flags & DXGI_ADAPTER_FLAG_REMOTE) != 0;
        out.name = wide_to_utf8(desc.Description);
        if (out.name.empty()) {
            out.name = "Windows Graphics Adapter " + std::to_string(out.index);
        }

        // DXGI does not expose a portable "integrated" bit.  Integrated adapters normally
        // have no dedicated VRAM and use the shared-system-memory aperture; a discrete GPU
        // reports a non-zero dedicated segment.  The classification is only a UI hint—the
        // ncnn Vulkan type remains authoritative for actual neural inference.
        if (out.is_software || out.is_remote) {
            out.type = 3;
        } else if (out.dedicated_video_memory_mb == 0) {
            out.type = 1;
        } else {
            out.type = 0;
        }

        result.push_back(std::move(out));
    }

    return result;
}

} // namespace enhancer

#else

namespace enhancer {

std::vector<DisplayAdapterDesc> enumerate_windows_display_adapters() {
    return {};
}

} // namespace enhancer

#endif
