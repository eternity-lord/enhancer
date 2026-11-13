#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

CACHE_DIR="${ENHANCER_DEPS_CACHE_DIR:-${HOME}/.cache/enhancer-build-deps}"
GEN_CF_DIR="${REPO_ROOT}/app/build/generated/codeformer_assets/models"
GEN_NCNN_DIR="${REPO_ROOT}/app/build/generated/ncnn-android-vulkan"

ENC_SHA256="15a5b7b73a22ab0f54f9270c88367c53e568dc3f46e532fabb0028845b7876ff"
GEN_SHA256="34af3c500aa6912597867d1bd1bc2f4c98c7f9ad82fa39c045047b48989b9b3c"
CF_COMMIT="cfe0fe2d247c05f7e7de00fab9ee55030315eac0"
NCNN_VER="20261020"

mkdir -p "${CACHE_DIR}" "${GEN_CF_DIR}" "${GEN_NCNN_DIR}"

verify_sha256() {
    local file="$1"
    local expected="$2"
    if [ ! -f "${file}" ]; then
        return 1
    fi
    local actual
    actual="$(sha256sum "${file}" | awk '{print $1}')"
    [ "${actual}" = "${expected}" ]
}

# 1. Prepare SHA-256-verified CodeFormer FP16 weights (encoder.bin + generator.bin)
CACHED_ENC="${CACHE_DIR}/codeformer_encoder.bin"
CACHED_GEN="${CACHE_DIR}/codeformer_generator.bin"

if ! verify_sha256 "${CACHED_ENC}" "${ENC_SHA256}" || ! verify_sha256 "${CACHED_GEN}" "${GEN_SHA256}"; then
    echo "[prepare_build_deps] Fetching CodeFormer weights (${CF_COMMIT})..."
    TMP_CF="$(mktemp -d)"
    trap 'rm -rf "${TMP_CF}"' EXIT

    TARBALL_URL="https://codeload.github.com/FeiGeChuanShu/CodeFormer-ncnn/tar.gz/${CF_COMMIT}"
    curl -fsSL --retry 3 --retry-delay 2 -o "${TMP_CF}/cf.tar.gz" "${TARBALL_URL}"
    tar -xzf "${TMP_CF}/cf.tar.gz" -C "${TMP_CF}" \
        "CodeFormer-ncnn-${CF_COMMIT}/models/encoder.bin" \
        "CodeFormer-ncnn-${CF_COMMIT}/models/generator.bin"

    cp -f "${TMP_CF}/CodeFormer-ncnn-${CF_COMMIT}/models/encoder.bin" "${CACHED_ENC}"
    cp -f "${TMP_CF}/CodeFormer-ncnn-${CF_COMMIT}/models/generator.bin" "${CACHED_GEN}"
    rm -rf "${TMP_CF}"
    trap - EXIT

    if ! verify_sha256 "${CACHED_ENC}" "${ENC_SHA256}"; then
        echo "ERROR: SHA-256 mismatch for codeformer_encoder.bin" >&2
        exit 1
    fi
    if ! verify_sha256 "${CACHED_GEN}" "${GEN_SHA256}"; then
        echo "ERROR: SHA-256 mismatch for codeformer_generator.bin" >&2
        exit 1
    fi
fi

if ! verify_sha256 "${GEN_CF_DIR}/codeformer_encoder.bin" "${ENC_SHA256}"; then
    cp -f "${CACHED_ENC}" "${GEN_CF_DIR}/codeformer_encoder.bin"
fi
if ! verify_sha256 "${GEN_CF_DIR}/codeformer_generator.bin" "${GEN_SHA256}"; then
    cp -f "${CACHED_GEN}" "${GEN_CF_DIR}/codeformer_generator.bin"
fi
echo "[prepare_build_deps] Verified CodeFormer weights in ${GEN_CF_DIR}"

# 2. Prepare multi-ABI ncnn-${NCNN_VER}-android-vulkan (arm64-v8a, armeabi-v7a, x86_64)
CACHED_NCNN_ROOT="${CACHE_DIR}/ncnn-${NCNN_VER}-android-vulkan"
if [ ! -f "${CACHED_NCNN_ROOT}/armeabi-v7a/lib/cmake/ncnn/ncnnConfig.cmake" ] || \
   [ ! -f "${CACHED_NCNN_ROOT}/x86_64/lib/cmake/ncnn/ncnnConfig.cmake" ]; then
    echo "[prepare_build_deps] Downloading ncnn-${NCNN_VER}-android-vulkan.zip..."
    TMP_NCNN="$(mktemp -d)"
    NCNN_URL="https://github.com/Tencent/ncnn/releases/download/${NCNN_VER}/ncnn-${NCNN_VER}-android-vulkan.zip"
    if curl -fsSL --retry 3 --retry-delay 2 -o "${TMP_NCNN}/ncnn.zip" "${NCNN_URL}"; then
        unzip -q "${TMP_NCNN}/ncnn.zip" -d "${TMP_NCNN}"
        mkdir -p "${CACHED_NCNN_ROOT}"
        for abi in arm64-v8a armeabi-v7a x86_64; do
            if [ -d "${TMP_NCNN}/ncnn-${NCNN_VER}-android-vulkan/${abi}" ]; then
                rm -rf "${CACHED_NCNN_ROOT}/${abi}"
                mv "${TMP_NCNN}/ncnn-${NCNN_VER}-android-vulkan/${abi}" "${CACHED_NCNN_ROOT}/${abi}"
            fi
        done
    else
        echo "[prepare_build_deps] Warning: Could not download multi-ABI ncnn zip; falling back to bundled arm64-v8a." >&2
    fi
    rm -rf "${TMP_NCNN}"
fi

for abi in arm64-v8a armeabi-v7a x86_64; do
    if [ -d "${CACHED_NCNN_ROOT}/${abi}" ] && [ ! -f "${GEN_NCNN_DIR}/${abi}/lib/cmake/ncnn/ncnnConfig.cmake" ]; then
        rm -rf "${GEN_NCNN_DIR}/${abi}"
        cp -R "${CACHED_NCNN_ROOT}/${abi}" "${GEN_NCNN_DIR}/${abi}"
    fi
done
echo "[prepare_build_deps] Multi-ABI ncnn check complete."
