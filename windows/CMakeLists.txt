cmake_minimum_required(VERSION 3.20)
project(EnhancerWindowsNative LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(ENHANCER_SHARED_CPP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../app/src/main/cpp")

# 1. Locate prebuilt ncnn (with Vulkan / SimpleVK + OpenMP + AVX2)
find_package(ncnn REQUIRED)

# Remove -fno-exceptions if exported by ncnn target so C++ exceptions are caught cleanly at C API boundary
get_target_property(_ncnn_opts ncnn INTERFACE_COMPILE_OPTIONS)
if(_ncnn_opts)
    list(REMOVE_ITEM _ncnn_opts "-fno-exceptions")
    set_target_properties(ncnn PROPERTIES INTERFACE_COMPILE_OPTIONS "${_ncnn_opts}")
endif()

# 2. Locate system ZLIB or automatically build madler/zlib v1.3.1 (needed on Windows MSVC)
find_package(ZLIB QUIET)
if(ZLIB_FOUND)
    set(ENHANCER_ZLIB_TARGET ZLIB::ZLIB)
else()
    include(FetchContent)
    set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(ZLIB_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        enhancer_zlib
        URL "https://codeload.github.com/madler/zlib/tar.gz/refs/tags/v1.3.1"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(enhancer_zlib)
    if(TARGET zlibstatic)
        set(ENHANCER_ZLIB_TARGET zlibstatic)
        target_include_directories(zlibstatic INTERFACE
            "${enhancer_zlib_SOURCE_DIR}"
            "${enhancer_zlib_BINARY_DIR}"
        )
    else()
        set(ENHANCER_ZLIB_TARGET zlib)
    endif()
endif()

find_package(OpenMP)

set(ENHANCER_CORE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/native/enhancer_c_api.cpp"
    "${ENHANCER_SHARED_CPP_DIR}/upscaler.cpp"
    "${ENHANCER_SHARED_CPP_DIR}/face_restorer.cpp"
    "${ENHANCER_SHARED_CPP_DIR}/photo_enhance_ops.cpp"
    "${ENHANCER_SHARED_CPP_DIR}/tile_ops.cpp"
    "${ENHANCER_SHARED_CPP_DIR}/resizer.cpp"
    "${ENHANCER_SHARED_CPP_DIR}/image_io.cpp"
)

add_library(enhancer_engine SHARED ${ENHANCER_CORE_SOURCES})

target_compile_definitions(enhancer_engine PRIVATE
    ENHANCER_ENGINE_EXPORTS
    _CRT_SECURE_NO_WARNINGS
)

target_include_directories(enhancer_engine PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/native"
    PRIVATE
    "${ENHANCER_SHARED_CPP_DIR}"
)

if(MSVC)
    target_compile_options(enhancer_engine PRIVATE
        /O2
        /EHsc
        /utf-8
        /W3
        /wd4244
        /wd4267
        /wd4996
    )
else()
    target_compile_options(enhancer_engine PRIVATE
        -O3
        -fexceptions
        -Wall
        -Wextra
        -Wno-unused-parameter
    )
endif()

target_link_libraries(enhancer_engine PRIVATE
    ncnn
    ${ENHANCER_ZLIB_TARGET}
)

if(OpenMP_CXX_FOUND)
    target_link_libraries(enhancer_engine PRIVATE OpenMP::OpenMP_CXX)
endif()

# Command-line executable (`enhancer-cli.exe` on Windows)
add_executable(enhancer-cli
    "${CMAKE_CURRENT_SOURCE_DIR}/native/enhancer_cli.cpp"
)

target_include_directories(enhancer-cli PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/native"
    "${ENHANCER_SHARED_CPP_DIR}"
)

if(MSVC)
    target_compile_options(enhancer-cli PRIVATE /O2 /EHsc /utf-8 /W3)
else()
    target_compile_options(enhancer-cli PRIVATE -O3 -Wall -Wextra)
endif()

target_link_libraries(enhancer-cli PRIVATE enhancer_engine)
