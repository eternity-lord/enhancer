#include "image_io.h"
#include "platform_io.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif
#include <vector>
#include <zlib.h>

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

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace enhancer {

namespace {

// Full-chroma 4:4:4 JPEG encoder built on stb_image_write's DCT/Huffman primitives.
// Enforces 1x1 (0x11) 4:4:4 full chroma across the entire quality range (including 90..100)
// and streams 8x8 MCUs directly without allocating any intermediate image buffer.
int write_jpg_full_chroma_444(
    stbi_write_func* func,
    void* context,
    int width,
    int height,
    const uint8_t* rgb_data,
    int quality,
    int dpi
) {
    static const unsigned char std_dc_luminance_nrcodes[] = {0,0,1,5,1,1,1,1,1,1,0,0,0,0,0,0,0};
    static const unsigned char std_dc_luminance_values[] = {0,1,2,3,4,5,6,7,8,9,10,11};
    static const unsigned char std_ac_luminance_nrcodes[] = {0,0,2,1,3,3,2,4,3,5,5,4,4,0,0,1,0x7d};
    static const unsigned char std_ac_luminance_values[] = {
        0x01,0x02,0x03,0x00,0x04,0x11,0x05,0x12,0x21,0x31,0x41,0x06,0x13,0x51,0x61,0x07,0x22,0x71,0x14,0x32,0x81,0x91,0xa1,0x08,
        0x23,0x42,0xb1,0xc1,0x15,0x52,0xd1,0xf0,0x24,0x33,0x62,0x72,0x82,0x09,0x0a,0x16,0x17,0x18,0x19,0x1a,0x25,0x26,0x27,0x28,
        0x29,0x2a,0x34,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,
        0x5a,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x83,0x84,0x85,0x86,0x87,0x88,0x89,
        0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,
        0xb7,0xb8,0xb9,0xba,0xc2,0xc3,0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,0xe1,0xe2,
        0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,0xf9,0xfa
    };
    static const unsigned char std_dc_chrominance_nrcodes[] = {0,0,3,1,1,1,1,1,1,1,1,1,0,0,0,0,0};
    static const unsigned char std_dc_chrominance_values[] = {0,1,2,3,4,5,6,7,8,9,10,11};
    static const unsigned char std_ac_chrominance_nrcodes[] = {0,0,2,1,2,4,4,3,4,7,5,4,4,0,1,2,0x77};
    static const unsigned char std_ac_chrominance_values[] = {
        0x00,0x01,0x02,0x03,0x11,0x04,0x05,0x21,0x31,0x06,0x12,0x41,0x51,0x07,0x61,0x71,0x13,0x22,0x32,0x81,0x08,0x14,0x42,0x91,
        0xa1,0xb1,0xc1,0x09,0x23,0x33,0x52,0xf0,0x15,0x62,0x72,0xd1,0x0a,0x16,0x24,0x34,0xe1,0x25,0xf1,0x17,0x18,0x19,0x1a,0x26,
        0x27,0x28,0x29,0x2a,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4a,0x53,0x54,0x55,0x56,0x57,0x58,
        0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x82,0x83,0x84,0x85,0x86,0x87,
        0x88,0x89,0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,
        0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,
        0xe2,0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,0xf9,0xfa
    };
    static const unsigned short YDC_HT[256][2] = { {0,2},{2,3},{3,3},{4,3},{5,3},{6,3},{14,4},{30,5},{62,6},{126,7},{254,8},{510,9}};
    static const unsigned short UVDC_HT[256][2] = { {0,2},{1,2},{2,2},{6,3},{14,4},{30,5},{62,6},{126,7},{254,8},{510,9},{1022,10},{2046,11}};
    static const unsigned short YAC_HT[256][2] = {
        {10,4},{0,2},{1,2},{4,3},{11,4},{26,5},{120,7},{248,8},{1014,10},{65410,16},{65411,16},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
        {12,4},{27,5},{121,7},{502,9},{2038,11},{65412,16},{65413,16},{65414,16},{65415,16},{65416,16},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
        {28,5},{249,8},{1015,10},{4084,12},{65417,16},{65418,16},{65419,16},{65420,16},{65421,16},{65422,16},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
        {58,6},{503,9},{4085,12},{65423,16},{65424,16},{65425,16},{65426,16},{65427,16},{65428,16},{65429,16},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
        {59,6},{1016,10},{65430,16},{65431,16},{65432,16},{65433,16},{65434,16},{65435,16},{65436,16},{65437,16},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
        {122,7},{2039,11},{65438,16},{65439,16},{65440,16},{65441,16},{65442,16},{65443,16},{65444,16},{65445,16},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
        {123,7},{4086,12},{65446,16},{65447,16},{65448,16},{65449,16},{65450,16},{65451,16},{65452,16},{65453,16},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
        {250,8},{4087,12},{65454,16},{65455,16},{65456,16},{65457,16},{65458,16},{65459,16},{65460,16},{65461,16},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
        {504,9},{32704,15},{65462,16},{65463,16},{65464,16},{65465,16},{65466,16},{65467,16},{65468,16},{65469,16},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
        {505,9},{65470,16},{65471,16},{65472,16},{65473,16},{65474,16},{65475,16},{65476,16},{65477,16},{65478,16},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
        {506,9},{65479,16},{65480,16},{65481,16},{65482,16},{65483,16},{65484,16},{65485,16},{65486,16},{65487,16},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
        {1017,10},{65488,16},{65489,16},{65490,16},{65491,16},{65492,16},{65493,16},{65494,16},{65495,16},{65496,16},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
        {1018,10},{65497,16},{65498,16},{65499,16},{65500,16},{65501,16},{65502,16},{65503,16},{65504,16},{65505,16},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
        {2040,11},{65506,16},{65507,16},{65508,16},{65509,16},{65510,16},{65511,16},{65512,16},{65513,16},{65514,16},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
        {65515,16},{65516,16},{65517,16},{65518,16},{65519,16},{65520,16},{65521,16},{65522,16},{65523,16},{65524,16},{0,0},{0,0},{0,0},{0,0},{0,0},
        {2041,11},{65525,16},{65526,16},{65527,16},{65528,16},{65529,16},{65530,16},{65531,16},{65532,16},{65533,16},{65534,16},{0,0},{0,0},{0,0},{0,0},{0,0}
    };
    static const unsigned short UVAC_HT[256][2] = {
        {0,2},{1,2},{4,3},{10,4},{24,5},{25,5},{56,6},{120,7},{500,9},{1014,10},{4084,12},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
        {11,4},{57,6},{246,8},{501,9},{2038,11},{4085,12},{65416,16},{65417,16},{65418,16},{65419,16},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
        {26,5},{247,8},{1015,10},{4086,12},{32706,15},{65420,16},{65421,16},{65422,16},{65423,16},{65424,16},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
        {27,5},{248,8},{1016,10},{4087,12},{65425,16},{65426,16},{65427,16},{65428,16},{65429,16},{65430,16},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
        {58,6},{502,9},{65431,16},{65432,16},{65433,16},{65434,16},{65435,16},{65436,16},{65437,16},{65438,16},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
        {59,6},{1017,10},{65439,16},{65440,16},{65441,16},{65442,16},{65443,16},{65444,16},{65445,16},{65446,16},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
        {121,7},{2039,11},{65447,16},{65448,16},{65449,16},{65450,16},{65451,16},{65452,16},{65453,16},{65454,16},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
        {122,7},{2040,11},{65455,16},{65456,16},{65457,16},{65458,16},{65459,16},{65460,16},{65461,16},{65462,16},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
        {249,8},{65463,16},{65464,16},{65465,16},{65466,16},{65467,16},{65468,16},{65469,16},{65470,16},{65471,16},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
        {503,9},{65472,16},{65473,16},{65474,16},{65475,16},{65476,16},{65477,16},{65478,16},{65479,16},{65480,16},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
        {504,9},{65481,16},{65482,16},{65483,16},{65484,16},{65485,16},{65486,16},{65487,16},{65488,16},{65489,16},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
        {505,9},{65490,16},{65491,16},{65492,16},{65493,16},{65494,16},{65495,16},{65496,16},{65497,16},{65498,16},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
        {506,9},{65499,16},{65500,16},{65501,16},{65502,16},{65503,16},{65504,16},{65505,16},{65506,16},{65507,16},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
        {2041,11},{65508,16},{65509,16},{65510,16},{65511,16},{65512,16},{65513,16},{65514,16},{65515,16},{65516,16},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
        {16352,14},{65517,16},{65518,16},{65519,16},{65520,16},{65521,16},{65522,16},{65523,16},{65524,16},{65525,16},{0,0},{0,0},{0,0},{0,0},{0,0},
        {1018,10},{32707,15},{65526,16},{65527,16},{65528,16},{65529,16},{65530,16},{65531,16},{65532,16},{65533,16},{65534,16},{0,0},{0,0},{0,0},{0,0},{0,0}
    };
    static const int YQT[] = {
        16,11,10,16,24,40,51,61,12,12,14,19,26,58,60,55,14,13,16,24,40,57,69,56,14,17,22,29,51,87,80,62,
        18,22,37,56,68,109,103,77,24,35,55,64,81,104,113,92,49,64,78,87,103,121,120,101,72,92,95,98,112,100,103,99
    };
    static const int UVQT[] = {
        17,18,24,47,99,99,99,99,18,21,26,66,99,99,99,99,24,26,56,99,99,99,99,99,47,66,99,99,99,99,99,99,
        99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99
    };
    static const float aasf[] = {
        1.0f * 2.828427125f, 1.387039845f * 2.828427125f, 1.306562965f * 2.828427125f, 1.175875602f * 2.828427125f,
        1.0f * 2.828427125f, 0.785694958f * 2.828427125f, 0.541196100f * 2.828427125f, 0.275899379f * 2.828427125f
    };

    if (!rgb_data || width <= 0 || height <= 0 || width > 65535 || height > 65535) {
        return 0;
    }

    stbi__write_context s = {0};
    stbi__start_write_callbacks(&s, func, context);

    const int clamped_q = std::clamp(quality, 1, 100);
    int q_scale = (clamped_q < 50) ? (5000 / clamped_q) : (200 - clamped_q * 2);
    // At studio quality >= 97 (default is 98), step smoothly to unit quantizers so smooth areas
    // never suffer 8x8 DCT AC-zeroing micro-block mesh texture:
    //   Q=97 -> q_scale=2, Q=98 -> q_scale=1, Q=99..100 -> q_scale=0 (all 64 quantizers = 1).
    if (clamped_q == 97) {
        q_scale = 2;
    } else if (clamped_q == 98) {
        q_scale = 1;
    } else if (clamped_q >= 99) {
        q_scale = 0;
    }

    unsigned char YTable[64];
    unsigned char UVTable[64];
    float fdtbl_Y[64];
    float fdtbl_UV[64];

    // Use fine perceptual YQT for both luma and 4:4:4 full-chroma tables when quality >= 90
    // (standard Annex K UVQT fills 50/64 entries with 99 assuming 4:2:0 subsampling, which
    // creates visible 8x8 chroma block steps on smooth areas even at Q=95..98).
    const int* chroma_qt = (clamped_q >= 90) ? YQT : UVQT;

    for (int i = 0; i < 64; ++i) {
        const int yti = (YQT[i] * q_scale + 50) / 100;
        const int uvti = (chroma_qt[i] * q_scale + 50) / 100;
        YTable[stbiw__jpg_ZigZag[i]] = static_cast<unsigned char>(std::clamp(yti, 1, 255));
        UVTable[stbiw__jpg_ZigZag[i]] = static_cast<unsigned char>(std::clamp(uvti, 1, 255));
    }

    for (int row = 0, k = 0; row < 8; ++row) {
        for (int col = 0; col < 8; ++col, ++k) {
            fdtbl_Y[k] = 1.0f / (YTable[stbiw__jpg_ZigZag[k]] * aasf[row] * aasf[col]);
            fdtbl_UV[k] = 1.0f / (UVTable[stbiw__jpg_ZigZag[k]] * aasf[row] * aasf[col]);
        }
    }

    // SOF0 sampling factor byte for Component 1 (Y) is fixed to 0x11 (1x1 -> 4:4:4 full chroma!)
    // JFIF APP0 carries print-ready DPI density (units = 1 -> dots per inch, default 300x300 DPI).
    const int safe_dpi = std::clamp(dpi > 0 ? dpi : 300, 72, 2400);
    const unsigned char dpi_hi = static_cast<unsigned char>((safe_dpi >> 8) & 0xFF);
    const unsigned char dpi_lo = static_cast<unsigned char>(safe_dpi & 0xFF);
    const unsigned char head0[] = {
        0xFF,0xD8,0xFF,0xE0,0,0x10,'J','F','I','F',0,1,1,1,dpi_hi,dpi_lo,dpi_hi,dpi_lo,0,0,0xFF,0xDB,0,0x84,0
    };
    static const unsigned char head2[] = {
        0xFF,0xDA,0,0xC,3,1,0,2,0x11,3,0x11,0,0x3F,0
    };
    const unsigned char head1[] = {
        0xFF,0xC0,0,0x11,8,
        static_cast<unsigned char>(height >> 8), STBIW_UCHAR(height),
        static_cast<unsigned char>(width >> 8), STBIW_UCHAR(width),
        3,
        1, 0x11, 0, // Y:  1x1 (no 4:2:0 subsampling)
        2, 0x11, 1, // Cb: 1x1
        3, 0x11, 1, // Cr: 1x1
        0xFF,0xC4,0x01,0xA2,0
    };

    s.func(s.context, const_cast<unsigned char*>(head0), sizeof(head0));
    s.func(s.context, YTable, sizeof(YTable));
    stbiw__putc(&s, 1);
    s.func(s.context, UVTable, sizeof(UVTable));
    s.func(s.context, const_cast<unsigned char*>(head1), sizeof(head1));
    s.func(s.context, const_cast<unsigned char*>(std_dc_luminance_nrcodes + 1), sizeof(std_dc_luminance_nrcodes) - 1);
    s.func(s.context, const_cast<unsigned char*>(std_dc_luminance_values), sizeof(std_dc_luminance_values));
    stbiw__putc(&s, 0x10);
    s.func(s.context, const_cast<unsigned char*>(std_ac_luminance_nrcodes + 1), sizeof(std_ac_luminance_nrcodes) - 1);
    s.func(s.context, const_cast<unsigned char*>(std_ac_luminance_values), sizeof(std_ac_luminance_values));
    stbiw__putc(&s, 1);
    s.func(s.context, const_cast<unsigned char*>(std_dc_chrominance_nrcodes + 1), sizeof(std_dc_chrominance_nrcodes) - 1);
    s.func(s.context, const_cast<unsigned char*>(std_dc_chrominance_values), sizeof(std_dc_chrominance_values));
    stbiw__putc(&s, 0x11);
    s.func(s.context, const_cast<unsigned char*>(std_ac_chrominance_nrcodes + 1), sizeof(std_ac_chrominance_nrcodes) - 1);
    s.func(s.context, const_cast<unsigned char*>(std_ac_chrominance_values), sizeof(std_ac_chrominance_values));
    s.func(s.context, const_cast<unsigned char*>(head2), sizeof(head2));

    static const unsigned short fillBits[] = {0x7F, 7};
    int DCY = 0;
    int DCU = 0;
    int DCV = 0;
    int bitBuf = 0;
    int bitCnt = 0;

    for (int y = 0; y < height; y += 8) {
        for (int x = 0; x < width; x += 8) {
            float Y[64];
            float U[64];
            float V[64];
            for (int row = y, pos = 0; row < y + 8; ++row) {
                const int clamped_row = (row < height) ? row : (height - 1);
                const size_t base_p = static_cast<size_t>(clamped_row) * static_cast<size_t>(width) * 3;
                for (int col = x; col < x + 8; ++col, ++pos) {
                    const int clamped_col = (col < width) ? col : (width - 1);
                    const size_t p = base_p + static_cast<size_t>(clamped_col) * 3;
                    const float r = static_cast<float>(rgb_data[p + 0]);
                    const float g = static_cast<float>(rgb_data[p + 1]);
                    const float b = static_cast<float>(rgb_data[p + 2]);
                    Y[pos] = +0.29900f * r + 0.58700f * g + 0.11400f * b - 128.0f;
                    U[pos] = -0.16874f * r - 0.33126f * g + 0.50000f * b;
                    V[pos] = +0.50000f * r - 0.41869f * g - 0.08131f * b;
                }
            }
            DCY = stbiw__jpg_processDU(&s, &bitBuf, &bitCnt, Y, 8, fdtbl_Y,  DCY, YDC_HT, YAC_HT);
            DCU = stbiw__jpg_processDU(&s, &bitBuf, &bitCnt, U, 8, fdtbl_UV, DCU, UVDC_HT, UVAC_HT);
            DCV = stbiw__jpg_processDU(&s, &bitBuf, &bitCnt, V, 8, fdtbl_UV, DCV, UVDC_HT, UVAC_HT);
        }
    }

    stbiw__jpg_writeBits(&s, &bitBuf, &bitCnt, fillBits);
    stbiw__putc(&s, 0xFF);
    stbiw__putc(&s, 0xD9);
    return 1;
}

} // namespace

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

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
#ifdef _WIN32
    rc = ::_close(to_close);
#else
    do {
        rc = ::close(to_close);
    } while (rc < 0 && errno == EINTR);
#endif
    return rc == 0;
}

namespace {

class BufferedFdWriter {
public:
    static constexpr size_t kBufferSize = 65536;

    explicit BufferedFdWriter(int fd)
        : fd_(fd), fp_(nullptr), buffer_(kBufferSize), used_(0), error_(false), last_errno_(0) {}

    explicit BufferedFdWriter(std::FILE* fp)
        : fd_(-1), fp_(fp), buffer_(kBufferSize), used_(0), error_(false), last_errno_(0) {}

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
        if (used_ == 0) {
            return true;
        }
        if (fp_ != nullptr) {
            const size_t written = std::fwrite(buffer_.data(), 1, used_, fp_);
            if (written != used_) {
                error_ = true;
                last_errno_ = errno != 0 ? errno : EIO;
                return false;
            }
            used_ = 0;
            return true;
        }
        size_t written_total = 0;
        while (written_total < used_) {
#ifdef _WIN32
            const unsigned int chunk = static_cast<unsigned int>(
                std::min<size_t>(used_ - written_total, 0x40000000u)
            );
            const int n = ::_write(fd_, buffer_.data() + written_total, chunk);
#else
            const ssize_t n = ::write(fd_, buffer_.data() + written_total, used_ - written_total);
#endif
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
    std::FILE* fp_;
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

inline void write_u32_be(uint8_t out[4], uint32_t v) noexcept {
    out[0] = static_cast<uint8_t>((v >> 24) & 0xFFu);
    out[1] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    out[2] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    out[3] = static_cast<uint8_t>(v & 0xFFu);
}

void write_png_chunk(
    BufferedFdWriter& writer,
    const char type[4],
    const uint8_t* payload,
    uint32_t payload_len
) noexcept {
    uint8_t len_be[4];
    write_u32_be(len_be, payload_len);
    writer.append(len_be, 4);
    writer.append(reinterpret_cast<const uint8_t*>(type), 4);
    if (payload_len > 0 && payload != nullptr) {
        writer.append(payload, payload_len);
    }

    uLong crc = ::crc32(0L, Z_NULL, 0);
    crc = ::crc32(crc, reinterpret_cast<const Bytef*>(type), 4);
    if (payload_len > 0 && payload != nullptr) {
        crc = ::crc32(crc, reinterpret_cast<const Bytef*>(payload), payload_len);
    }
    uint8_t crc_be[4];
    write_u32_be(crc_be, static_cast<uint32_t>(crc));
    writer.append(crc_be, 4);
}

inline uint8_t paeth_predictor(int a, int b, int c) noexcept {
    const int p = a + b - c;
    const int pa = std::abs(p - a);
    const int pb = std::abs(p - b);
    const int pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) {
        return static_cast<uint8_t>(a);
    }
    if (pb <= pc) {
        return static_cast<uint8_t>(b);
    }
    return static_cast<uint8_t>(c);
}

// Streams RGB888 rows directly into 64 KB PNG IDAT chunks using zlib.
// Allocates ONLY two single-row filter buffers (`2 * (width * 3 + 1)` bytes) + 64 KB deflate buffer:
// ZERO second full-size image buffer!
bool write_png_streaming_zlib(
    BufferedFdWriter& writer,
    const uint8_t* rgb_data,
    int width,
    int height,
    int dpi,
    EnhancerStatus& out_status,
    uint64_t& out_req_bytes,
    std::string& out_err
) {
    size_t row_bytes = 0;
    if (!checked_mul_size(static_cast<size_t>(width), 3u, row_bytes) ||
        row_bytes > std::numeric_limits<size_t>::max() - 1u) {
        out_status = EnhancerStatus::ERR_INVALID_ARGS;
        out_err = "PNG row stride overflow";
        return false;
    }
    const size_t filtered_len = row_bytes + 1u;
    constexpr size_t kIdatChunkBytes = 65536u;

    std::vector<uint8_t> cand_sub;
    std::vector<uint8_t> cand_paeth;
    std::vector<uint8_t> zbuf;
    try {
        out_req_bytes = static_cast<uint64_t>(filtered_len) * 2u + kIdatChunkBytes;
        cand_sub.resize(filtered_len);
        cand_paeth.resize(filtered_len);
        zbuf.resize(kIdatChunkBytes);
    } catch (const std::bad_alloc&) {
        out_status = EnhancerStatus::ERR_OUT_OF_MEMORY;
        out_err = "std::bad_alloc allocating PNG single-row filter scratch buffers";
        return false;
    }

    static const uint8_t kPngSig[8] = {137u, 80u, 78u, 71u, 13u, 10u, 26u, 10u};
    writer.append(kPngSig, sizeof(kPngSig));

    uint8_t ihdr[13];
    write_u32_be(ihdr + 0, static_cast<uint32_t>(width));
    write_u32_be(ihdr + 4, static_cast<uint32_t>(height));
    ihdr[8] = 8u;  // 8 bits per channel
    ihdr[9] = 2u;  // Truecolor RGB
    ihdr[10] = 0u; // Deflate compression
    ihdr[11] = 0u; // Adaptive filtering
    ihdr[12] = 0u; // Non-interlaced
    write_png_chunk(writer, "IHDR", ihdr, sizeof(ihdr));

    // Emit print-ready pHYs chunk (1 inch = 0.0254 m => 300 DPI = 11811 pixels/meter)
    const int safe_dpi = std::clamp(dpi > 0 ? dpi : 300, 72, 2400);
    const uint32_t ppm = static_cast<uint32_t>(static_cast<double>(safe_dpi) * 39.37007874 + 0.5);
    uint8_t phys[9];
    write_u32_be(phys + 0, ppm);
    write_u32_be(phys + 4, ppm);
    phys[8] = 1u; // unit = meter
    write_png_chunk(writer, "pHYs", phys, sizeof(phys));

    z_stream strm{};
    const int init_rc = ::deflateInit2(&strm, 6, Z_DEFLATED, 15, 8, Z_FILTERED);
    if (init_rc != Z_OK) {
        if (init_rc == Z_MEM_ERROR) {
            out_status = EnhancerStatus::ERR_OUT_OF_MEMORY;
            out_req_bytes = 262144u;
            out_err = "zlib deflateInit2 failed with Z_MEM_ERROR";
        } else {
            out_status = EnhancerStatus::ERR_ENCODE_WRITE;
            out_err = "zlib deflateInit2 failed with rc=" + std::to_string(init_rc);
        }
        return false;
    }

    strm.next_out = zbuf.data();
    strm.avail_out = static_cast<uInt>(zbuf.size());

    auto flush_zbuf_if_full = [&]() -> bool {
        if (strm.avail_out == 0) {
            write_png_chunk(writer, "IDAT", zbuf.data(), static_cast<uint32_t>(zbuf.size()));
            if (writer.has_error()) {
                return false;
            }
            strm.next_out = zbuf.data();
            strm.avail_out = static_cast<uInt>(zbuf.size());
        }
        return true;
    };

    for (int y = 0; y < height; ++y) {
        const uint8_t* cur = rgb_data + static_cast<size_t>(y) * row_bytes;
        const uint8_t* prev = (y > 0) ? (cur - row_bytes) : nullptr;

        // Candidate 1: Filter 1 (Sub)
        cand_sub[0] = 1u;
        uint64_t score_sub = 0;
        for (size_t i = 0; i < row_bytes; ++i) {
            const uint8_t left = (i >= 3u) ? cur[i - 3u] : 0u;
            const uint8_t d = static_cast<uint8_t>(cur[i] - left);
            cand_sub[1u + i] = d;
            score_sub += static_cast<uint64_t>(std::abs(static_cast<int>(static_cast<int8_t>(d))));
        }

        const uint8_t* chosen = cand_sub.data();
        if (prev != nullptr) {
            // Candidate 2: Filter 4 (Paeth) using previous row directly from `rgb_data`
            cand_paeth[0] = 4u;
            uint64_t score_paeth = 0;
            for (size_t i = 0; i < row_bytes; ++i) {
                const int a = (i >= 3u) ? cur[i - 3u] : 0;
                const int b = prev[i];
                const int c = (i >= 3u) ? prev[i - 3u] : 0;
                const uint8_t d = static_cast<uint8_t>(cur[i] - paeth_predictor(a, b, c));
                cand_paeth[1u + i] = d;
                score_paeth += static_cast<uint64_t>(std::abs(static_cast<int>(static_cast<int8_t>(d))));
            }
            if (score_paeth < score_sub) {
                chosen = cand_paeth.data();
            }
        }

        size_t rem = filtered_len;
        const uint8_t* ptr = chosen;
        while (rem > 0) {
            const uInt chunk = static_cast<uInt>(std::min<size_t>(rem, 0x40000000u));
            strm.next_in = const_cast<Bytef*>(ptr);
            strm.avail_in = chunk;
            while (strm.avail_in > 0) {
                const int drc = ::deflate(&strm, Z_NO_FLUSH);
                if (drc == Z_MEM_ERROR) {
                    ::deflateEnd(&strm);
                    out_status = EnhancerStatus::ERR_OUT_OF_MEMORY;
                    out_err = "zlib deflate failed with Z_MEM_ERROR";
                    return false;
                }
                if (drc == Z_STREAM_ERROR) {
                    ::deflateEnd(&strm);
                    out_status = EnhancerStatus::ERR_ENCODE_WRITE;
                    out_err = "zlib deflate failed with Z_STREAM_ERROR";
                    return false;
                }
                if (!flush_zbuf_if_full()) {
                    ::deflateEnd(&strm);
                    out_status = EnhancerStatus::ERR_ENCODE_WRITE;
                    out_err = std::string("Write to output fd failed during PNG IDAT: ") +
                        std::strerror(writer.last_errno());
                    return false;
                }
            }
            ptr += chunk;
            rem -= chunk;
        }
    }

    int finish_rc = Z_OK;
    while (finish_rc != Z_STREAM_END) {
        finish_rc = ::deflate(&strm, Z_FINISH);
        if (finish_rc != Z_OK && finish_rc != Z_STREAM_END) {
            ::deflateEnd(&strm);
            out_status = (finish_rc == Z_MEM_ERROR)
                ? EnhancerStatus::ERR_OUT_OF_MEMORY
                : EnhancerStatus::ERR_ENCODE_WRITE;
            out_err = "zlib deflate(Z_FINISH) failed with rc=" + std::to_string(finish_rc);
            return false;
        }
        const size_t have = zbuf.size() - static_cast<size_t>(strm.avail_out);
        if (have > 0 && (strm.avail_out == 0 || finish_rc == Z_STREAM_END)) {
            write_png_chunk(writer, "IDAT", zbuf.data(), static_cast<uint32_t>(have));
            if (writer.has_error()) {
                ::deflateEnd(&strm);
                out_status = EnhancerStatus::ERR_ENCODE_WRITE;
                out_err = std::string("Write to output fd failed flushing PNG IDAT: ") +
                    std::strerror(writer.last_errno());
                return false;
            }
            strm.next_out = zbuf.data();
            strm.avail_out = static_cast<uInt>(zbuf.size());
        }
    }

    ::deflateEnd(&strm);
    write_png_chunk(writer, "IEND", nullptr, 0);
    return !writer.has_error();
}

} // namespace

bool write_rgb_image_to_fd(
    int fd,
    const uint8_t* rgb_data,
    int width,
    int height,
    ImageFormat format,
    int jpeg_quality,
    std::string* out_error,
    EnhancerStatus* out_status,
    uint64_t* out_requested_bytes,
    int dpi
) {
    if (out_status) {
        *out_status = EnhancerStatus::OK;
    }
    if (out_requested_bytes) {
        *out_requested_bytes = 0;
    }

    size_t total_rgb_bytes = 0;
    if (fd < 0 || !rgb_data || !checked_image_bytes(width, height, 3, total_rgb_bytes)) {
        if (out_status) {
            *out_status = EnhancerStatus::ERR_INVALID_ARGS;
        }
        if (out_error) {
            *out_error = "Invalid arguments or dimensions passed to write_rgb_image_to_fd";
        }
        return false;
    }

    std::unique_ptr<BufferedFdWriter> writer_ptr;
    try {
        if (out_requested_bytes) {
            *out_requested_bytes = BufferedFdWriter::kBufferSize;
        }
        writer_ptr = std::make_unique<BufferedFdWriter>(fd);
    } catch (const std::bad_alloc&) {
        if (out_status) {
            *out_status = EnhancerStatus::ERR_OUT_OF_MEMORY;
        }
        if (out_error) {
            *out_error = "std::bad_alloc allocating 64 KB BufferedFdWriter";
        }
        return false;
    }

    BufferedFdWriter& writer = *writer_ptr;

    if (format == ImageFormat::PNG) {
        EnhancerStatus png_status = EnhancerStatus::OK;
        uint64_t png_req_bytes = 0;
        std::string png_err;
        const bool png_ok = write_png_streaming_zlib(
            writer,
            rgb_data,
            width,
            height,
            dpi,
            png_status,
            png_req_bytes,
            png_err
        );
        if (out_requested_bytes && png_req_bytes > 0) {
            *out_requested_bytes = png_req_bytes;
        }
        if (!png_ok) {
            if (out_status) {
                *out_status = (png_status != EnhancerStatus::OK)
                    ? png_status
                    : EnhancerStatus::ERR_ENCODE_WRITE;
            }
            if (out_error) {
                *out_error = !png_err.empty() ? png_err : "Streaming PNG encoding failed";
            }
            return false;
        }
    } else {
        if (width > 65535 || height > 65535) {
            if (out_status) {
                *out_status = EnhancerStatus::ERR_INVALID_ARGS;
            }
            if (out_error) {
                *out_error = "JPEG dimensions exceed 65535x65535 limit; select PNG or 8K target";
            }
            return false;
        }
        const int clamped_quality = std::clamp(jpeg_quality, 1, 100);
        const int jpg_ok = write_jpg_full_chroma_444(
            &stbi_fd_write_callback,
            &writer,
            width,
            height,
            rgb_data,
            clamped_quality,
            dpi
        );
        if (jpg_ok == 0) {
            if (out_status) {
                *out_status = EnhancerStatus::ERR_ENCODE_WRITE;
            }
            if (out_error) {
                *out_error = "4:4:4 JPEG encoder failed";
            }
            return false;
        }
    }

    const bool flushed = writer.flush();
    if (!flushed || writer.has_error()) {
        if (out_status) {
            *out_status = EnhancerStatus::ERR_ENCODE_WRITE;
        }
        if (out_error) {
            *out_error = std::string("Write to output fd failed: ") + std::strerror(writer.last_errno());
        }
        return false;
    }

#ifdef _WIN32
    (void)::_commit(fd);
#else
    (void)::fsync(fd);
#endif
    return true;
}

bool write_rgb_image_to_path(
    const std::string& utf8_path,
    const uint8_t* rgb_data,
    int width,
    int height,
    ImageFormat format,
    int jpeg_quality,
    std::string* out_error,
    EnhancerStatus* out_status,
    uint64_t* out_requested_bytes,
    int dpi
) {
    if (out_status) {
        *out_status = EnhancerStatus::OK;
    }
    if (out_requested_bytes) {
        *out_requested_bytes = 0;
    }

    size_t total_rgb_bytes = 0;
    if (utf8_path.empty() || !rgb_data || !checked_image_bytes(width, height, 3, total_rgb_bytes)) {
        if (out_status) {
            *out_status = EnhancerStatus::ERR_INVALID_ARGS;
        }
        if (out_error) {
            *out_error = "Invalid arguments or dimensions passed to write_rgb_image_to_path";
        }
        return false;
    }

    std::FILE* fp = open_utf8_file(utf8_path, "wb");
    if (!fp) {
        if (out_status) {
            *out_status = EnhancerStatus::ERR_ENCODE_WRITE;
        }
        if (out_error) {
            *out_error = "Could not open output file for writing: " + utf8_path;
        }
        return false;
    }

    std::unique_ptr<BufferedFdWriter> writer_ptr;
    try {
        if (out_requested_bytes) {
            *out_requested_bytes = BufferedFdWriter::kBufferSize;
        }
        writer_ptr = std::make_unique<BufferedFdWriter>(fp);
    } catch (const std::bad_alloc&) {
        std::fclose(fp);
        if (out_status) {
            *out_status = EnhancerStatus::ERR_OUT_OF_MEMORY;
        }
        if (out_error) {
            *out_error = "std::bad_alloc allocating 64 KB BufferedFdWriter";
        }
        return false;
    }

    BufferedFdWriter& writer = *writer_ptr;
    if (format == ImageFormat::PNG) {
        EnhancerStatus png_status = EnhancerStatus::OK;
        uint64_t png_req_bytes = 0;
        std::string png_err;
        const bool png_ok = write_png_streaming_zlib(
            writer,
            rgb_data,
            width,
            height,
            dpi,
            png_status,
            png_req_bytes,
            png_err
        );
        if (out_requested_bytes && png_req_bytes > 0) {
            *out_requested_bytes = png_req_bytes;
        }
        if (!png_ok) {
            std::fclose(fp);
            if (out_status) {
                *out_status = (png_status != EnhancerStatus::OK)
                    ? png_status
                    : EnhancerStatus::ERR_ENCODE_WRITE;
            }
            if (out_error) {
                *out_error = !png_err.empty() ? png_err : "Streaming PNG encoding failed";
            }
            return false;
        }
    } else {
        if (width > 65535 || height > 65535) {
            std::fclose(fp);
            if (out_status) {
                *out_status = EnhancerStatus::ERR_INVALID_ARGS;
            }
            if (out_error) {
                *out_error = "JPEG dimensions exceed 65535x65535 limit; select PNG";
            }
            return false;
        }
        const int clamped_quality = std::clamp(jpeg_quality, 1, 100);
        const int jpg_ok = write_jpg_full_chroma_444(
            &stbi_fd_write_callback,
            &writer,
            width,
            height,
            rgb_data,
            clamped_quality,
            dpi
        );
        if (jpg_ok == 0) {
            std::fclose(fp);
            if (out_status) {
                *out_status = EnhancerStatus::ERR_ENCODE_WRITE;
            }
            if (out_error) {
                *out_error = "4:4:4 JPEG encoder failed";
            }
            return false;
        }
    }

    const bool flushed = writer.flush();
    const int flush_rc = std::fflush(fp);
    const int close_rc = std::fclose(fp);
    if (!flushed || writer.has_error() || flush_rc != 0 || close_rc != 0) {
        if (out_status) {
            *out_status = EnhancerStatus::ERR_ENCODE_WRITE;
        }
        if (out_error) {
            *out_error = "Failed flushing or closing output image file: " + utf8_path;
        }
        return false;
    }

    return true;
}

bool decode_image_file_utf8_to_rgba(
    const std::string& utf8_path,
    NativeByteBuffer& out_rgba,
    int& out_width,
    int& out_height,
    std::string* out_error
) {
    out_rgba.reset();
    out_width = 0;
    out_height = 0;

    std::FILE* fp = open_utf8_file(utf8_path, "rb");
    if (!fp) {
        if (out_error) {
            *out_error = "Could not open input image file: " + utf8_path;
        }
        return false;
    }

    int w = 0;
    int h = 0;
    int comp = 0;
    stbi_uc* decoded = stbi_load_from_file(fp, &w, &h, &comp, 4);
    std::fclose(fp);

    if (!decoded || w <= 0 || h <= 0) {
        if (decoded) {
            stbi_image_free(decoded);
        }
        if (out_error) {
            const char* reason = stbi_failure_reason();
            *out_error = std::string("Failed decoding input image (") +
                (reason ? reason : "unsupported or corrupt") + "): " + utf8_path;
        }
        return false;
    }

    // stbi_load_from_file uses std::malloc under the hood, matching NativeByteBuffer FreeDeleter.
    out_rgba = NativeByteBuffer(reinterpret_cast<uint8_t*>(decoded));
    out_width = w;
    out_height = h;
    return true;
}

} // namespace enhancer
