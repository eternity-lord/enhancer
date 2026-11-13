#ifndef ENHANCER_IMAGE_IO_H
#define ENHANCER_IMAGE_IO_H

#include "enhancer_types.h"

#include <cstdint>
#include <string>

namespace enhancer {

// RAII wrapper for a detached Android ParcelFileDescriptor POSIX fd.
class UniqueFd {
public:
    UniqueFd() noexcept : fd_(-1) {}
    explicit UniqueFd(int fd) noexcept : fd_(fd) {}
    ~UniqueFd() noexcept;

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    UniqueFd(UniqueFd&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            close_now();
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    int get() const noexcept { return fd_; }
    bool valid() const noexcept { return fd_ >= 0; }

    // Closes fd and returns true on success.
    bool close_now() noexcept;

private:
    int fd_;
};

// Stream-encodes packed RGB888 pixels to 4:4:4 JPEG or lossless PNG directly into `fd`
// with ZERO second full-size image buffer (64 KB output ring + single-row PNG filter scratch).
// Does NOT close `fd` (caller manages lifetime via UniqueFd).
bool write_rgb_image_to_fd(
    int fd,
    const uint8_t* rgb_data,
    int width,
    int height,
    ImageFormat format,
    int jpeg_quality,
    std::string* out_error,
    EnhancerStatus* out_status = nullptr,
    uint64_t* out_requested_bytes = nullptr,
    int dpi = 300
);

} // namespace enhancer

#endif // ENHANCER_IMAGE_IO_H
