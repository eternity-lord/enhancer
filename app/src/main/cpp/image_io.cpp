#include "image_io.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <unistd.h>
#include <vector>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wunused-function"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace enhancer {

UniqueFd::~UniqueFd() noexcept {
    close_now();
}

bool UniqueFd::close_now() noexcept {
    if (fd_ < 0) {
        return true;
    }
    const int to_close = fd_;
    fd_ = -1;
    int rc = 0;
    do {
        rc = ::close(to_close);
    } while (rc < 0 && errno == EINTR);
    return rc == 0;
}

namespace {

class BufferedFdWriter {
public:
    static constexpr size_t kBufferSize = 65536;

    explicit BufferedFdWriter(int fd)
        : fd_(fd), buffer_(kBufferSize), used_(0), error_(false), last_errno_(0) {}

    void append(const uint8_t* src, size_t len) noexcept {
        if (error_ || len == 0) {
            return;
        }
        size_t offset = 0;
        while (offset < len) {
            const size_t space = kBufferSize - used_;
            const size_t chunk = std::min(space, len - offset);
            std::memcpy(buffer_.data() + used_, src + offset, chunk);
            used_ += chunk;
            offset += chunk;
            if (used_ == kBufferSize) {
                if (!flush()) {
                    return;
                }
            }
        }
    }

    bool flush() noexcept {
        if (error_) {
            return false;
        }
        size_t written_total = 0;
        while (written_total < used_) {
            const ssize_t n = ::write(fd_, buffer_.data() + written_total, used_ - written_total);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                error_ = true;
                last_errno_ = errno;
                return false;
            }
            if (n == 0) {
                error_ = true;
                last_errno_ = EIO;
                return false;
            }
            written_total += static_cast<size_t>(n);
        }
        used_ = 0;
        return true;
    }

    bool has_error() const noexcept { return error_; }
    int last_errno() const noexcept { return last_errno_; }

private:
    int fd_;
    std::vector<uint8_t> buffer_;
    size_t used_;
    bool error_;
    int last_errno_;
};

void stbi_fd_write_callback(void* context, void* data, int size) {
    if (!context || !data || size <= 0) {
        return;
    }
    auto* writer = static_cast<BufferedFdWriter*>(context);
    writer->append(static_cast<const uint8_t*>(data), static_cast<size_t>(size));
}

} // namespace

bool write_rgb_image_to_fd(
    int fd,
    const uint8_t* rgb_data,
    int width,
    int height,
    ImageFormat format,
    int jpeg_quality,
    std::string* out_error
) {
    if (fd < 0 || !rgb_data || width <= 0 || height <= 0) {
        if (out_error) {
            *out_error = "Invalid arguments passed to write_rgb_image_to_fd";
        }
        return false;
    }

    BufferedFdWriter writer(fd);
    int stbi_ok = 0;

    if (format == ImageFormat::PNG) {
        const int stride_bytes = width * 3;
        stbi_ok = stbi_write_png_to_func(
            &stbi_fd_write_callback,
            &writer,
            width,
            height,
            3,
            rgb_data,
            stride_bytes
        );
    } else {
        const int clamped_quality = std::clamp(jpeg_quality, 1, 100);
        stbi_ok = stbi_write_jpg_to_func(
            &stbi_fd_write_callback,
            &writer,
            width,
            height,
            3,
            rgb_data,
            clamped_quality
        );
    }

    const bool flushed = writer.flush();
    if (stbi_ok == 0 || !flushed || writer.has_error()) {
        if (out_error) {
            if (writer.has_error()) {
                *out_error = std::string("Write to output fd failed: ") + std::strerror(writer.last_errno());
            } else {
                *out_error = "stb_image_write failed to encode output image (out of memory or invalid image)";
            }
        }
        return false;
    }

    // Best-effort sync to storage (ignore EINVAL/EROFS on special fds like pipes)
    (void)::fsync(fd);
    return true;
}

} // namespace enhancer
