#ifndef ENHANCER_PLATFORM_IO_H
#define ENHANCER_PLATFORM_IO_H

#include <cstdint>
#include <cstdio>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace enhancer {

#ifdef _WIN32
inline std::wstring utf8_to_wide_win32(const std::string& utf8) {
    if (utf8.empty()) {
        return std::wstring();
    }
    const int needed = ::MultiByteToWideChar(
        CP_UTF8,
        0,
        utf8.data(),
        static_cast<int>(utf8.size()),
        nullptr,
        0
    );
    if (needed <= 0) {
        return std::wstring();
    }
    std::wstring wide(static_cast<size_t>(needed), L'\0');
    ::MultiByteToWideChar(
        CP_UTF8,
        0,
        utf8.data(),
        static_cast<int>(utf8.size()),
        &wide[0],
        needed
    );
    return wide;
}

inline std::string wide_to_utf8_win32(const wchar_t* wide) {
    if (!wide || wide[0] == L'\0') {
        return std::string();
    }
    const int needed = ::WideCharToMultiByte(
        CP_UTF8,
        0,
        wide,
        -1,
        nullptr,
        0,
        nullptr,
        nullptr
    );
    if (needed <= 1) {
        return std::string();
    }
    std::string out(static_cast<size_t>(needed - 1), '\0');
    ::WideCharToMultiByte(
        CP_UTF8,
        0,
        wide,
        -1,
        &out[0],
        needed - 1,
        nullptr,
        nullptr
    );
    return out;
}
#endif

inline std::FILE* open_utf8_file(const std::string& utf8_path, const char* mode) noexcept {
    if (utf8_path.empty() || !mode) {
        return nullptr;
    }
#ifdef _WIN32
    const std::wstring wpath = utf8_to_wide_win32(utf8_path);
    const std::wstring wmode = utf8_to_wide_win32(mode);
    if (wpath.empty() || wmode.empty()) {
        return nullptr;
    }
#ifdef _MSC_VER
    std::FILE* fp = nullptr;
    if (::_wfopen_s(&fp, wpath.c_str(), wmode.c_str()) != 0) {
        return nullptr;
    }
    return fp;
#else
    return ::_wfopen(wpath.c_str(), wmode.c_str());
#endif
#else
    return std::fopen(utf8_path.c_str(), mode);
#endif
}

inline bool utf8_file_exists(const std::string& utf8_path) noexcept {
    std::FILE* fp = open_utf8_file(utf8_path, "rb");
    if (!fp) {
        return false;
    }
    std::fclose(fp);
    return true;
}

inline uint64_t query_utf8_file_size(const std::string& utf8_path) noexcept {
    std::FILE* fp = open_utf8_file(utf8_path, "rb");
    if (!fp) {
        return 0;
    }
#ifdef _WIN32
    if (::_fseeki64(fp, 0, SEEK_END) != 0) {
        std::fclose(fp);
        return 0;
    }
    const __int64 sz = ::_ftelli64(fp);
    std::fclose(fp);
    return (sz > 0) ? static_cast<uint64_t>(sz) : 0;
#else
    if (std::fseek(fp, 0, SEEK_END) != 0) {
        std::fclose(fp);
        return 0;
    }
    const long sz = std::ftell(fp);
    std::fclose(fp);
    return (sz > 0) ? static_cast<uint64_t>(sz) : 0;
#endif
}

// Resolves a relative model path such as "models/realesrgan-x4plus.param" against `base_dir`.
// Supports both `base_dir/models/foo.param` and `base_dir/foo.param` (when `base_dir` itself is `models`).
inline std::string resolve_model_file_path(
    const std::string& base_dir,
    const std::string& asset_rel_path
) {
    if (base_dir.empty()) {
        return asset_rel_path;
    }
    std::string clean_base = base_dir;
    while (!clean_base.empty() && (clean_base.back() == '/' || clean_base.back() == '\\')) {
        clean_base.pop_back();
    }

    const std::string candidate1 = clean_base + "/" + asset_rel_path;
    if (utf8_file_exists(candidate1)) {
        return candidate1;
    }

    const std::string prefix = "models/";
    if (asset_rel_path.compare(0, prefix.size(), prefix) == 0) {
        const std::string stripped = asset_rel_path.substr(prefix.size());
        const std::string candidate2 = clean_base + "/" + stripped;
        if (utf8_file_exists(candidate2)) {
            return candidate2;
        }
    }
    return candidate1;
}

} // namespace enhancer

#endif // ENHANCER_PLATFORM_IO_H
