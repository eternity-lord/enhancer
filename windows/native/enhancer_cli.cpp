#include "enhancer_c_api.h"
#include "platform_io.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

struct ModelSpec {
    const char* slug;
    const char* param_rel;
    const char* bin_rel;
    int scale;
};

constexpr ModelSpec kModels[] = {
    {"photo-x4",       "models/realesrgan-x4plus.param",         "models/realesrgan-x4plus.bin",         4},
    {"general-v3",     "models/realesr-general-x4v3.param",      "models/realesr-general-x4v3.bin",      4},
    {"general-wdn",    "models/realesr-general-wdn-x4v3.param",  "models/realesr-general-wdn-x4v3.bin",  4},
    {"lsdir-x4",       "models/lsdir-compact-x4.param",          "models/lsdir-compact-x4.bin",          4},
    {"anime-x4",       "models/realesrgan-x4plus-anime.param",   "models/realesrgan-x4plus-anime.bin",   4},
    {"compact-x2",     "models/realesr-animevideov3-x2.param",   "models/realesr-animevideov3-x2.bin",   2},
    {"compact-x3",     "models/realesr-animevideov3-x3.param",   "models/realesr-animevideov3-x3.bin",   3},
    {"compact-x4",     "models/realesr-animevideov3-x4.param",   "models/realesr-animevideov3-x4.bin",   4},
};

const ModelSpec* find_model(const std::string& slug) {
    for (const auto& m : kModels) {
        if (slug == m.slug) return &m;
    }
    return nullptr;
}

std::string detect_default_models_dir(const std::string& exe_path_utf8) {
    std::filesystem::path exe_dir;
    try {
#ifdef _WIN32
        exe_dir = std::filesystem::path(enhancer::utf8_to_wide_win32(exe_path_utf8)).parent_path();
#else
        exe_dir = std::filesystem::path(exe_path_utf8).parent_path();
#endif
    } catch (...) {
    }

    const std::vector<std::filesystem::path> candidates = {
        exe_dir,
        exe_dir / "models",
        std::filesystem::current_path(),
        std::filesystem::current_path() / "app" / "src" / "main" / "assets"
    };

    for (const auto& c : candidates) {
        if (c.empty()) continue;
        std::string s;
#ifdef _WIN32
        s = enhancer::wide_to_utf8_win32(c.wstring().c_str());
#else
        s = c.string();
#endif
        const std::string probe = enhancer::resolve_model_file_path(s, "models/realesrgan-x4plus.param");
        if (enhancer::utf8_file_exists(probe)) {
            return s;
        }
    }
    return ".";
}

void ENHANCER_CALL cli_progress_cb(int32_t pct, const char* backend, int32_t tile, void* /*ud*/) {
    std::fprintf(stderr, "\r[Enhancer] %3d%% | %s | tile=%d px   ", pct, backend ? backend : "", tile);
    if (pct >= 100) {
        std::fprintf(stderr, "\n");
    }
    std::fflush(stderr);
}

void print_usage() {
    std::printf(
        "Enhancer v3.0 Studio CLI (Windows / Cross-Platform — Vulkan GPU + Multi-Core CPU)\n"
        "Built by Mohammad Irani | 100%% Offline AI Super-Resolution, Face Restore & Light AI\n\n"
        "Usage:\n"
        "  enhancer-cli --diag [--models-dir <dir>]\n"
        "  enhancer-cli --self-test [--models-dir <dir>]\n"
        "  enhancer-cli --seam-test [--models-dir <dir>]\n"
        "  enhancer-cli -i <input.jpg> -o <output.jpg> [options]\n\n"
        "Options:\n"
        "  --mode <prime|gentle|balanced|strong|ultra|digital-art|old-photo>  (default: balanced)\n"
        "  --model <photo-x4|general-v3|general-wdn|lsdir-x4|anime-x4|compact-x2|compact-x3|compact-x4>\n"
        "  --target <native|2k|4k|8k|16k>                                     (default: 4k)\n"
        "  --faces <0|1>               Enable SCRFD + CodeFormer 512x512      (default: mode preset)\n"
        "  --face-strength <0..100>    Face restoration strength              (default: 70)\n"
        "  --light-ai <0|1>            Enable Smart HDR Light AI              (default: mode preset)\n"
        "  --light-strength <0..100>   Light AI strength                      (default: 45)\n"
        "  --denoise <0..100>          Pre-SR JPEG deblock & denoise          (default: mode preset)\n"
        "  --ultra-intensity <0..100>  Ultra mode multi-scale intensity       (default: 65)\n"
        "  --ultra-size <0..100>       Ultra mode Size of Changes             (default: 45)\n"
        "  --sharpness <0..100>        Adaptive edge sharpness                (default: 12)\n"
        "  --max-quality               Enable full-res supersampling + 32px feather\n"
        "  --tta                       Enable 8x Test-Time Augmentation\n"
        "  --cpu                       Force Multi-Core CPU Precision backend (no GPU required)\n"
        "  --gpu <auto|0|1|...>        Select Vulkan GPU index (default: auto best discrete GPU)\n"
        "  --threads <0..64>           CPU OpenMP thread count (0 = auto)\n"
        "  --tile <0|64|128|192|256|384|512>  Tile size in px (0 = auto)\n"
        "  --fp32                      Force 32-bit storage on Vulkan\n"
        "  --format <jpg|png>          Output format (default: inferred from -o or jpg)\n"
        "  --quality <90..100>         4:4:4 JPEG quality (default: 98)\n"
        "  --dpi <72..1200>            Print DPI metadata (default: 300)\n"
        "  --models-dir <dir>          Directory containing neural model files\n"
    );
}

int run_cli_utf8(const std::vector<std::string>& args) {
    if (args.size() <= 1) {
        print_usage();
        return 0;
    }

    std::string models_dir = detect_default_models_dir(args[0]);
    std::string input_path;
    std::string output_path;
    bool do_diag = false;
    bool do_self_test = false;
    bool do_seam_test = false;

    int mode_code = 2; // Balanced
    std::string model_slug = "general-v3";
    bool user_set_model = false;
    int faces_on = 1;
    bool user_set_faces = false;
    int face_strength = 70;
    int light_ai_on = 1;
    bool user_set_light = false;
    int light_strength = 45;
    int denoise = 18;
    bool user_set_denoise = false;
    int sharpness = 12;
    bool user_set_sharpness = false;
    int ultra_intensity = 65;
    int ultra_size = 45;
    int target_long = 3840;
    int tile_size = 0;
    int disable_fp16 = 0;
    int max_quality = 0;
    int tta_x8 = 0;
    int force_cpu = 0;
    int gpu_index = -1;
    int num_threads = 0;
    int dpi = 300;
    int format_code = -1;
    int jpeg_quality = 98;

    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto next_val = [&]() -> std::string {
            return (i + 1 < args.size()) ? args[++i] : "";
        };

        if (a == "--help" || a == "-h") {
            print_usage();
            return 0;
        } else if (a == "--diag") {
            do_diag = true;
        } else if (a == "--self-test") {
            do_self_test = true;
        } else if (a == "--seam-test") {
            do_seam_test = true;
        } else if (a == "-i" || a == "--input") {
            input_path = next_val();
        } else if (a == "-o" || a == "--output") {
            output_path = next_val();
        } else if (a == "--models-dir") {
            models_dir = next_val();
        } else if (a == "--mode") {
            const std::string m = next_val();
            if (m == "prime") { mode_code = 0; if (!user_set_model) model_slug = "photo-x4"; if (!user_set_faces) faces_on = 1; if (!user_set_light) light_ai_on = 0; if (!user_set_denoise) denoise = 10; if (!user_set_sharpness) sharpness = 8; }
            else if (m == "gentle") { mode_code = 1; if (!user_set_model) model_slug = "lsdir-x4"; if (!user_set_faces) faces_on = 0; if (!user_set_light) light_ai_on = 0; if (!user_set_denoise) denoise = 0; if (!user_set_sharpness) sharpness = 5; }
            else if (m == "balanced") { mode_code = 2; if (!user_set_model) model_slug = "general-v3"; if (!user_set_faces) faces_on = 1; if (!user_set_light) light_ai_on = 1; if (!user_set_denoise) denoise = 18; if (!user_set_sharpness) sharpness = 12; }
            else if (m == "strong") { mode_code = 3; if (!user_set_model) model_slug = "general-wdn"; if (!user_set_faces) faces_on = 1; if (!user_set_light) light_ai_on = 1; if (!user_set_denoise) denoise = 60; if (!user_set_sharpness) sharpness = 18; }
            else if (m == "ultra") { mode_code = 4; if (!user_set_model) model_slug = "photo-x4"; if (!user_set_faces) faces_on = 1; if (!user_set_light) light_ai_on = 1; if (!user_set_denoise) denoise = 15; if (!user_set_sharpness) sharpness = 20; }
            else if (m == "digital-art") { mode_code = 5; if (!user_set_model) model_slug = "anime-x4"; if (!user_set_faces) faces_on = 0; if (!user_set_light) light_ai_on = 0; if (!user_set_denoise) denoise = 12; if (!user_set_sharpness) sharpness = 14; }
            else if (m == "old-photo") { mode_code = 6; if (!user_set_model) model_slug = "photo-x4"; if (!user_set_faces) faces_on = 1; if (!user_set_light) light_ai_on = 1; if (!user_set_denoise) denoise = 35; if (!user_set_sharpness) sharpness = 10; }
        } else if (a == "--model") {
            model_slug = next_val();
            user_set_model = true;
        } else if (a == "--target") {
            const std::string t = next_val();
            if (t == "native" || t == "0") target_long = 0;
            else if (t == "2k" || t == "2K") target_long = 2560;
            else if (t == "4k" || t == "4K") target_long = 3840;
            else if (t == "8k" || t == "8K") target_long = 7680;
            else if (t == "16k" || t == "16K") target_long = 15360;
            else target_long = std::atoi(t.c_str());
        } else if (a == "--faces") {
            faces_on = std::atoi(next_val().c_str()) != 0 ? 1 : 0;
            user_set_faces = true;
        } else if (a == "--face-strength") {
            face_strength = std::clamp(std::atoi(next_val().c_str()), 0, 100);
        } else if (a == "--light-ai") {
            light_ai_on = std::atoi(next_val().c_str()) != 0 ? 1 : 0;
            user_set_light = true;
        } else if (a == "--light-strength") {
            light_strength = std::clamp(std::atoi(next_val().c_str()), 0, 100);
        } else if (a == "--denoise") {
            denoise = std::clamp(std::atoi(next_val().c_str()), 0, 100);
            user_set_denoise = true;
        } else if (a == "--ultra-intensity") {
            ultra_intensity = std::clamp(std::atoi(next_val().c_str()), 0, 100);
        } else if (a == "--ultra-size") {
            ultra_size = std::clamp(std::atoi(next_val().c_str()), 0, 100);
        } else if (a == "--sharpness") {
            sharpness = std::clamp(std::atoi(next_val().c_str()), 0, 100);
            user_set_sharpness = true;
        } else if (a == "--max-quality") {
            max_quality = 1;
        } else if (a == "--tta") {
            tta_x8 = 1;
        } else if (a == "--cpu") {
            force_cpu = 1;
        } else if (a == "--gpu") {
            const std::string g = next_val();
            gpu_index = (g == "auto") ? -1 : std::atoi(g.c_str());
        } else if (a == "--threads") {
            num_threads = std::clamp(std::atoi(next_val().c_str()), 0, 64);
        } else if (a == "--tile") {
            tile_size = std::atoi(next_val().c_str());
        } else if (a == "--fp32") {
            disable_fp16 = 1;
        } else if (a == "--format") {
            const std::string f = next_val();
            format_code = (f == "png" || f == "PNG") ? 1 : 0;
        } else if (a == "--quality") {
            jpeg_quality = std::clamp(std::atoi(next_val().c_str()), 1, 100);
        } else if (a == "--dpi") {
            dpi = std::clamp(std::atoi(next_val().c_str()), 72, 2400);
        }
    }

    if (do_diag) {
        int32_t avail_mb = 0, total_mb = 0, big_cores = 0;
        enhancer_get_system_memory_mb(&avail_mb, &total_mb);
        const int32_t total_cores = enhancer_get_cpu_thread_count(&big_cores);
        char diag_buf[4096]{};
        enhancer_get_diagnostics_summary(diag_buf, sizeof(diag_buf));

        EnhancerCGpuInfo gpus[16]{};
        const int32_t gpu_count = enhancer_enumerate_gpus(gpus, 16);

        std::printf("=== Enhancer v3.0 Hardware & Engine Diagnostics ===\n");
        std::printf("Models Directory : %s\n", models_dir.c_str());
        std::printf("System Memory    : %d MB available / %d MB total\n", avail_mb, total_mb);
        std::printf("CPU Logical Cores: %d total (%d performance)\n", total_cores, big_cores);
        std::printf("Vulkan GPUs      : %d\n", gpu_count);
        for (int32_t i = 0; i < std::min<int32_t>(gpu_count, 16); ++i) {
            const char* kind = (gpus[i].type == 0) ? "Discrete" :
                               (gpus[i].type == 1) ? "Integrated" : "Virtual/Other";
            std::printf(
                "  [GPU %d]%s %s (%s, Heap Budget: %u MB, Vendor: 0x%04X)\n",
                gpus[i].index,
                gpus[i].is_recommended ? " [Recommended]" : "",
                gpus[i].name,
                kind,
                gpus[i].heap_budget_mb,
                gpus[i].vendor_id
            );
        }
        std::printf("%s\n", diag_buf);
        enhancer_shutdown();
        return 0;
    }

    if (do_self_test) {
        int32_t vk_pass = 0, cpu_pass = 0;
        int64_t vk_ms = 0, cpu_ms = 0;
        std::vector<char> log_buf(16384, '\0');
        const int32_t rc = enhancer_run_self_test(
            models_dir.c_str(),
            &vk_pass,
            &vk_ms,
            &cpu_pass,
            &cpu_ms,
            log_buf.data(),
            static_cast<int32_t>(log_buf.size())
        );
        std::printf("%s\n", log_buf.data());
        enhancer_shutdown();
        return (rc == 0 && cpu_pass != 0) ? 0 : 1;
    }

    if (do_seam_test) {
        int32_t overall_pass = 0;
        std::vector<char> sum_buf(2048, '\0');
        std::vector<char> tbl_buf(65536, '\0');
        const int32_t rc = enhancer_run_seam_test(
            models_dir.c_str(),
            [](int32_t done, int32_t total, const char* lbl, void*) {
                std::fprintf(stderr, "\r[SeamTest %2d/%2d] %s   ", done, total, lbl ? lbl : "");
                if (done >= total) std::fprintf(stderr, "\n");
            },
            nullptr,
            &overall_pass,
            sum_buf.data(),
            static_cast<int32_t>(sum_buf.size()),
            tbl_buf.data(),
            static_cast<int32_t>(tbl_buf.size())
        );
        std::printf("%s\n", tbl_buf.data());
        enhancer_shutdown();
        return (rc == 0 && overall_pass != 0) ? 0 : 1;
    }

    if (input_path.empty() || output_path.empty()) {
        std::fprintf(stderr, "Error: both -i <input> and -o <output> are required.\n\n");
        print_usage();
        enhancer_shutdown();
        return 1;
    }

    const ModelSpec* spec = find_model(model_slug);
    if (!spec) {
        std::fprintf(stderr, "Error: unknown model '%s'.\n", model_slug.c_str());
        enhancer_shutdown();
        return 1;
    }

    if (format_code < 0) {
        const size_t dot = output_path.find_last_of('.');
        const std::string ext = (dot != std::string::npos) ? output_path.substr(dot + 1) : "";
        format_code = (ext == "png" || ext == "PNG") ? 1 : 0;
    }

    EnhancerCJobConfig cfg{};
    cfg.models_dir_utf8 = models_dir.c_str();
    cfg.param_rel_utf8 = spec->param_rel;
    cfg.bin_rel_utf8 = spec->bin_rel;
    cfg.output_path_utf8 = output_path.c_str();
    cfg.model_scale = spec->scale;
    cfg.target_long_side = target_long;
    cfg.tile_size = tile_size;
    cfg.disable_fp16 = disable_fp16;
    cfg.max_quality = max_quality;
    cfg.tta_x8 = tta_x8;
    cfg.safe_mode = 0;
    cfg.force_cpu = force_cpu;
    cfg.gpu_index = gpu_index;
    cfg.num_threads = num_threads;
    cfg.sharpness = sharpness;
    cfg.enhance_mode = mode_code;
    cfg.enhance_faces = faces_on;
    cfg.face_strength = static_cast<float>(face_strength) / 100.0f;
    cfg.light_ai = light_ai_on;
    cfg.light_ai_strength = static_cast<float>(light_strength) / 100.0f;
    cfg.denoise_strength = static_cast<float>(denoise) / 100.0f;
    cfg.ultra_intensity = static_cast<float>(ultra_intensity) / 100.0f;
    cfg.ultra_size = static_cast<float>(ultra_size) / 100.0f;
    cfg.dpi = dpi;
    cfg.output_format = format_code;
    cfg.jpeg_quality = jpeg_quality;

    int32_t src_w = 0, src_h = 0;
    EnhancerCResult res{};
    const auto t0 = std::chrono::steady_clock::now();
    enhancer_upscale_file(input_path.c_str(), &cfg, &cli_progress_cb, nullptr, &src_w, &src_h, &res);
    const auto t1 = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(t1 - t0).count();

    if (res.status_code != 0) {
        std::fprintf(stderr, "\n[FAILED] status=%d | %s\n", res.status_code, res.error_detail);
        enhancer_shutdown();
        return 2;
    }

    std::printf(
        "[DONE] %dx%d -> %dx%d (%d DPI) in %.2f s | Backend: %s | Faces: %d/%d | Saved: %s\n",
        src_w,
        src_h,
        res.out_width,
        res.out_height,
        dpi,
        secs,
        res.backend_used,
        res.faces_restored,
        res.faces_detected,
        output_path.c_str()
    );
    enhancer_shutdown();
    return 0;
}

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** wargv) {
    ::SetConsoleOutputCP(CP_UTF8);
    std::vector<std::string> args;
    args.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        args.push_back(enhancer::wide_to_utf8_win32(wargv[i]));
    }
    return run_cli_utf8(args);
}
#else
int main(int argc, char** argv) {
    std::vector<std::string> args;
    args.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
    return run_cli_utf8(args);
}
#endif
