#!/usr/bin/env bash
# =============================================================================
# build_deps_macos.sh - Compile all CDO dependencies on macOS
# =============================================================================
# For use in cibuildwheel on macOS (arm64 or x86_64).
# We compile everything from source rather than using Homebrew to ensure
# consistent deployment targets and architecture support.
#
# Environment variables:
#   CDO_DEPS_PREFIX        - Installation prefix (default: /opt/cdo-deps)
#   PARALLEL_JOBS          - Number of parallel make jobs (default: sysctl hw.ncpu)
#   MACOSX_DEPLOYMENT_TARGET - Minimum macOS version (default: 11.0)
# =============================================================================

set -euo pipefail

PREFIX="${CDO_DEPS_PREFIX:-/opt/cdo-deps}"
JOBS="${PARALLEL_JOBS:-$(sysctl -n hw.ncpu)}"
BUILD_DIR="/tmp/cdo-build-deps"
export MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-11.0}"

# Detect architecture
ARCH="$(uname -m)"

# Versions (same as Linux)
ZLIB_VERSION="1.3.1"
LIBAEC_VERSION="1.1.3"
HDF5_VERSION="1.14.4-3"
HDF5_DIR_VERSION="1.14.4"
NETCDF_VERSION="4.9.2"
ECCODES_VERSION="2.38.0"
FFTW_VERSION="3.3.10"
PROJ_VERSION="9.5.1"
UDUNITS_VERSION="2.2.28"

echo "============================================"
echo "Building CDO dependencies for macOS"
echo "  Arch:       ${ARCH}"
echo "  Target:     macOS ${MACOSX_DEPLOYMENT_TARGET}"
echo "  PREFIX:     ${PREFIX}"
echo "  JOBS:       ${JOBS}"
echo "============================================"

mkdir -p "${PREFIX}" "${BUILD_DIR}"

export PKG_CONFIG_PATH="${PREFIX}/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
export DYLD_LIBRARY_PATH="${PREFIX}/lib:${DYLD_LIBRARY_PATH:-}"
export CFLAGS="-fPIC -O2 -arch ${ARCH}"
export CXXFLAGS="-fPIC -O2 -arch ${ARCH}"
export LDFLAGS="-L${PREFIX}/lib -arch ${ARCH}"
export CPPFLAGS="-I${PREFIX}/include"
export CMAKE_OSX_ARCHITECTURES="${ARCH}"

# Helper function
build_autotools() {
    local name="$1"
    local url="$2"
    local configure_args="${3:-}"

    echo "--- Building ${name} ---"
    cd "${BUILD_DIR}"
    local archive="${url##*/}"
    [[ -f "${archive}" ]] || curl -fsSL "${url}" -o "${archive}"

    local dir
    dir=$(tar tf "${archive}" | head -1 | cut -d/ -f1)
    rm -rf "${dir}"
    tar xf "${archive}"
    cd "${dir}"

    ./configure --prefix="${PREFIX}" ${configure_args}
    make -j"${JOBS}"
    make install
    echo "--- ${name} installed ---"
}

# ---- 1. zlib ----
build_autotools "zlib" \
    "https://github.com/madler/zlib/releases/download/v${ZLIB_VERSION}/zlib-${ZLIB_VERSION}.tar.gz" \
    "--shared"

# ---- 2. libaec ----
echo "--- Building libaec ---"
cd "${BUILD_DIR}"
curl -fsSL "https://github.com/MathisRosworski/libaec/releases/download/v${LIBAEC_VERSION}/libaec-${LIBAEC_VERSION}.tar.gz" -o "libaec-${LIBAEC_VERSION}.tar.gz"
tar xf "libaec-${LIBAEC_VERSION}.tar.gz"
cd "libaec-${LIBAEC_VERSION}"
mkdir -p build && cd build
cmake .. \
    -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DBUILD_SHARED_LIBS=ON \
    -DCMAKE_OSX_ARCHITECTURES="${ARCH}" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET}"
make -j"${JOBS}"
make install
echo "--- libaec installed ---"

# ---- 3. HDF5 ----
echo "--- Building HDF5 ---"
cd "${BUILD_DIR}"
HDF5_URL="https://github.com/HDFGroup/hdf5/releases/download/hdf5_${HDF5_VERSION}/hdf5-${HDF5_DIR_VERSION}.tar.gz"
curl -fsSL "${HDF5_URL}" -o "hdf5-${HDF5_DIR_VERSION}.tar.gz" || \
    curl -fsSL "https://support.hdfgroup.org/releases/hdf5/v${HDF5_DIR_VERSION}/downloads/hdf5-${HDF5_DIR_VERSION}.tar.gz" -o "hdf5-${HDF5_DIR_VERSION}.tar.gz"
tar xf "hdf5-${HDF5_DIR_VERSION}.tar.gz"
cd "hdf5-${HDF5_DIR_VERSION}"*
./configure \
    --prefix="${PREFIX}" \
    --enable-shared \
    --disable-static \
    --with-szlib="${PREFIX}" \
    --with-zlib="${PREFIX}" \
    --enable-hl \
    --disable-tests \
    --disable-tools
make -j"${JOBS}"
make install
echo "--- HDF5 installed ---"

# ---- 4. NetCDF-C ----
echo "--- Building NetCDF-C ---"
cd "${BUILD_DIR}"
curl -fsSL "https://github.com/Unidata/netcdf-c/releases/download/v${NETCDF_VERSION}/netcdf-c-${NETCDF_VERSION}.tar.gz" -o "netcdf-c-${NETCDF_VERSION}.tar.gz"
tar xf "netcdf-c-${NETCDF_VERSION}.tar.gz"
cd "netcdf-c-${NETCDF_VERSION}"
CPPFLAGS="-I${PREFIX}/include" LDFLAGS="-L${PREFIX}/lib -arch ${ARCH}" \
    ./configure \
    --prefix="${PREFIX}" \
    --enable-shared \
    --disable-static \
    --enable-netcdf-4 \
    --disable-dap \
    --disable-byterange \
    --disable-testsets
make -j"${JOBS}"
make install
echo "--- NetCDF-C installed ---"

# ---- 5. ecCodes ----
echo "--- Building ecCodes ---"
cd "${BUILD_DIR}"
curl -fsSL "https://confluence.ecmwf.int/download/attachments/45757960/eccodes-${ECCODES_VERSION}-Source.tar.gz" -o "eccodes-${ECCODES_VERSION}.tar.gz" || \
    curl -fsSL "https://github.com/ecmwf/eccodes/releases/download/${ECCODES_VERSION}/eccodes-${ECCODES_VERSION}-Source.tar.gz" -o "eccodes-${ECCODES_VERSION}.tar.gz"
tar xf "eccodes-${ECCODES_VERSION}.tar.gz"
cd "eccodes-${ECCODES_VERSION}-Source"*
mkdir -p build && cd build
cmake .. \
    -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_FORTRAN=OFF \
    -DENABLE_PYTHON=OFF \
    -DENABLE_MEMFS=ON \
    -DENABLE_NETCDF=ON \
    -DENABLE_JPG=OFF \
    -DENABLE_PNG=ON \
    -DENABLE_AEC=ON \
    -DAEC_DIR="${PREFIX}" \
    -DHDF5_DIR="${PREFIX}" \
    -DNETCDF_DIR="${PREFIX}" \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DBUILD_SHARED_LIBS=ON \
    -DCMAKE_OSX_ARCHITECTURES="${ARCH}" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET}"
make -j"${JOBS}"
make install
echo "--- ecCodes installed ---"

# ---- 6. FFTW3 ----
build_autotools "FFTW3" \
    "https://www.fftw.org/fftw-${FFTW_VERSION}.tar.gz" \
    "--enable-shared --disable-static --enable-threads"

# ---- 7. PROJ ----
echo "--- Building PROJ ---"
cd "${BUILD_DIR}"
curl -fsSL "https://github.com/OSGeo/PROJ/releases/download/${PROJ_VERSION}/proj-${PROJ_VERSION}.tar.gz" -o "proj-${PROJ_VERSION}.tar.gz"
tar xf "proj-${PROJ_VERSION}.tar.gz"
cd "proj-${PROJ_VERSION}"
mkdir -p build && cd build
cmake .. \
    -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=ON \
    -DBUILD_TESTING=OFF \
    -DBUILD_APPS=OFF \
    -DENABLE_TIFF=OFF \
    -DENABLE_CURL=OFF \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DCMAKE_OSX_ARCHITECTURES="${ARCH}" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET}"
make -j"${JOBS}"
make install
echo "--- PROJ installed ---"

# ---- 8. UDUNITS2 ----
build_autotools "UDUNITS2" \
    "https://downloads.unidata.ucar.edu/udunits/${UDUNITS_VERSION}/udunits-${UDUNITS_VERSION}.tar.gz" \
    "--enable-shared --disable-static"

echo "============================================"
echo "All dependencies built successfully!"
echo "  Installed to: ${PREFIX}"
echo "============================================"
ls -la "${PREFIX}/lib/"
