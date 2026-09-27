#!/usr/bin/env bash
set -euo pipefail

probe_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "$probe_dir/../../.." && pwd)
build_dir=${BUILD_DIR:-"$repo_root/build/tests/buffer-cache"}
compiler=${CXX:-aarch64-linux-gnu-g++}
glslc_bin=${GLSLC:-glslc}
spirv_val=${SPIRV_VAL:-spirv-val}
vulkan_include=${VULKAN_INCLUDE_DIR:-"$repo_root/work/mesa/include"}

for tool in "$compiler" "$glslc_bin" "$spirv_val"; do
    command -v "$tool" >/dev/null || {
        echo "Missing required tool: $tool" >&2
        exit 1
    }
done
test -f "$vulkan_include/vulkan/vulkan.h" || {
    echo "Set VULKAN_INCLUDE_DIR to the Vulkan-Headers include directory" >&2
    exit 1
}
mkdir -p "$build_dir"
for shader in ssbo_pingpong guard_write guard_sample; do
    "$glslc_bin" --target-env=vulkan1.3 -O "$probe_dir/$shader.comp" \
        -o "$build_dir/$shader.comp.spv"
    "$spirv_val" --target-env vulkan1.3 "$build_dir/$shader.comp.spv"
done
"$compiler" -std=c++20 -O2 -Wall -Wextra -Wno-missing-field-initializers \
    -static-libstdc++ -static-libgcc -I"$vulkan_include" \
    "$probe_dir/buffer_cache_driver_probe.cpp" -ldl \
    -o "$build_dir/buffer_cache_driver_probe"
printf 'Built %s\n' "$build_dir/buffer_cache_driver_probe"
sha256sum "$build_dir/buffer_cache_driver_probe" "$build_dir/"*.spv
