cmake_minimum_required(VERSION 3.22.1)
project(enhancer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Point directly to prebuilt ncnn with Vulkan (arm64-v8a)
set(ncnn_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ncnn-android-vulkan/arm64-v8a/lib/cmake/ncnn")
find_package(ncnn REQUIRED)

# Prebuilt ncnn exports -fno-exceptions in INTERFACE_COMPILE_OPTIONS.
# Remove -fno-exceptions from the interface property so enhancer can catch all C++ exceptions
# cleanly at the JNI boundary while keeping ABI-matching -fno-rtti.
get_target_property(_ncnn_interface_opts ncnn INTERFACE_COMPILE_OPTIONS)
if(_ncnn_interface_opts)
    list(REMOVE_ITEM _ncnn_interface_opts "-fno-exceptions")
    set_target_properties(ncnn PROPERTIES INTERFACE_COMPILE_OPTIONS "${_ncnn_interface_opts}")
endif()

add_library(enhancer SHARED
    enhancer_jni.cpp
    upscaler.cpp
    resizer.cpp
    image_io.cpp
)

target_include_directories(enhancer PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_compile_options(enhancer PRIVATE
    -fopenmp
    -fexceptions
    -O3
    -ffunction-sections
    -fdata-sections
    -Wall
    -Wextra
    -Wno-unused-parameter
)

target_link_options(enhancer PRIVATE
    -fopenmp
    -static-openmp
    -Wl,--gc-sections
)

target_link_libraries(enhancer PRIVATE
    ncnn
    android
    jnigraphics
    log
)
