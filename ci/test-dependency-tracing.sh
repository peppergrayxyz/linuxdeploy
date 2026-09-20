#!/usr/bin/env bash

set -euo pipefail

# use RAM disk if possible
if [ -d /docker-ramdisk ]; then
    TEMP_BASE=/docker-ramdisk
else
    TEMP_BASE=${TEMP_BASE:-/tmp}
fi

build_dir=$(mktemp -d -p "$TEMP_BASE" linuxdeploy-build-XXXXXX)
echo "build_dir: $build_dir"

cleanup () {
    if [ -d "$build_dir" ]; then
        rm -rf "$build_dir"
    fi
}
trap cleanup EXIT

export LINUXDEPLOY_LDD_REPORT_STDOUT=1

echo "## env"

env

echo "## build"

cmake_args=()
for var in AR AS RANLIB OBJCOPY STRIP; do
    if [[ -n "${!var:-}" ]]; then
        cmake_args+=("-DCMAKE_$var=$(command -v "${!var}")")
    fi
done
for var in CXX_FLAGS CXX_STANDARD_LIBRARIES; do
    if [[ -v "$var" ]]; then
        cmake_args+=("-DCMAKE_$var=${!var}")
    fi
done
if [[ -v LINKER_FLAGS ]]; then
    cmake_args+=(
        "-DCMAKE_EXE_LINKER_FLAGS=$LINKER_FLAGS"
        "-DCMAKE_SHARED_LINKER_FLAGS=$LINKER_FLAGS"
    )
fi

echo "## CMake variables from environment"
for arg in "${cmake_args[@]}"; do
    printf '%s\n' "${arg#-D}"
done

cmake \
    -S . \
    -B "$build_dir" \
    -G Ninja \
    -DBUILD_TESTING=ON \
    -DSTATIC_BUILD=OFF \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    "${cmake_args[@]}"

cmake --build "$build_dir"

echo "## tests"

ctest -V --test-dir "$build_dir" -R dependency_tracing

echo "## integration test"

appdir="$build_dir/appdir"

simple_bin="$build_dir/tests/simple_executable/simple_executable"
simple_lib="$build_dir/tests/simple_library/libsimple_library.so"
parent_lib="$build_dir/tests/dependency_parent/libdependency_parent.so"
leaf_lib="$build_dir/tests/dependency_leaf/libdependency_leaf.so"

echo "simple_executable:"
ldd "$simple_bin"
echo "libsimple_library:"
ldd "$simple_lib"
echo "dependency_parent:"
ldd "$parent_lib"
echo "dependency_leaf:"
ldd "$leaf_lib"


echo "## linuxdeploy"

"$build_dir/bin/linuxdeploy" \
    --appdir "$appdir" \
    --executable "$simple_bin"

echo "## check appdir"

set -x
test -x "$appdir/usr/bin/simple_executable"
for library in libsimple_library libdependency_parent libdependency_leaf; do
    test -f "$appdir/usr/lib/$library.so"
done
set +x

echo "## run simple_executable from appdir:"

if env -u LD_LIBRARY_PATH "$appdir/usr/bin/simple_executable"; then
    result=0
else
    result=$?
fi

printf "\n\nExit status: %d\n" "$result"

[ "$result" -eq 0 ]
