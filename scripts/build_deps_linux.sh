#!/usr/bin/env bash
# =============================================================================
# build_deps_linux.sh - Compile all CDO dependencies from source on Linux
# =============================================================================
# This script is designed to run inside a manylinux_2_28 Docker container
# (CentOS Stream / AlmaLinux based) used by cibuildwheel.
#
# Usage:
#   SKYBORN_CDO_BUILD=1 bash scripts/build_deps_linux.sh
#
# Environment variables:
#   CDO_DEPS_PREFIX  - Installation prefix (default: /opt/cdo-deps)
#   PARALLEL_JOBS    - Number of parallel make jobs (default: nproc)
# =============================================================================

set -euo pipefail

PREFIX="${CDO_DEPS_PREFIX:-/opt/cdo-deps}"
JOBS="${PARALLEL_JOBS:-$(nproc)}"
BUILD_DIR="/tmp/cdo-build-deps"

# Versions
ZLIB_VERSION="1.3.1"
LIBAEC_VERSION="1.1.3"
HDF5_VERSION="1.14.4-3"
HDF5_TAG="1.14.4.3"
NETCDF_VERSION="4.9.2"
ECCODES_VERSION="2.38.0"
FFTW_VERSION="3.3.10"
PROJ_VERSION="9.5.1"
UDUNITS_VERSION="2.2.28"
CURL_VERSION="8.11.1"

echo "============================================"
echo "Building CDO dependencies for Linux"
echo "  PREFIX:    ${PREFIX}"
echo "  JOBS:      ${JOBS}"
echo "  BUILD_DIR: ${BUILD_DIR}"
echo "============================================"

mkdir -p "${PREFIX}" "${BUILD_DIR}"

# Install system-level build tools that manylinux_2_28 might be missing
if command -v dnf &>/dev/null; then
    dnf install -y cmake gcc-c++ gcc-gfortran wget perl openssl-devel \
        sqlite-devel libtiff-devel libxml2-devel expat-devel 2>/dev/null || true
elif command -v yum &>/dev/null; then
    yum install -y cmake3 gcc-c++ gcc-gfortran wget perl openssl-devel \
        sqlite-devel libtiff-devel libxml2-devel expat-devel 2>/dev/null || true
fi

# Ensure cmake is available (some images have cmake3)
if ! command -v cmake &>/dev/null && command -v cmake3 &>/dev/null; then
    ln -sf "$(which cmake3)" /usr/local/bin/cmake
fi

export PKG_CONFIG_PATH="${PREFIX}/lib/pkgconfig:${PREFIX}/lib64/pkgconfig:${PKG_CONFIG_PATH:-}"
export LD_LIBRARY_PATH="${PREFIX}/lib:${PREFIX}/lib64:${LD_LIBRARY_PATH:-}"
export CFLAGS="-fPIC -O2"
export CXXFLAGS="-fPIC -O2"
export LDFLAGS="-L${PREFIX}/lib -L${PREFIX}/lib64"
export CPPFLAGS="-I${PREFIX}/include"

# Helper function
build_from_tar() {
    local name="$1"
    local url="$2"
    local configure_args="${3:-}"

    echo "--- Building ${name} ---"
    cd "${BUILD_DIR}"
    local archive="${url##*/}"
    if [[ ! -f "${archive}" ]]; then
        wget -q "${url}" -O "${archive}"
    fi

    local dir
    dir=$(set +o pipefail; tar tf "${archive}" | head -1 | cut -d/ -f1)
    rm -rf "${dir}"
    tar xf "${archive}"
    cd "${dir}"

    if [[ -f configure ]]; then
        ./configure --prefix="${PREFIX}" ${configure_args}
        make -j"${JOBS}"
        make install
    elif [[ -f CMakeLists.txt ]]; then
        mkdir -p build && cd build
        cmake .. -DCMAKE_INSTALL_PREFIX="${PREFIX}" ${configure_args}
        make -j"${JOBS}"
        make install
    fi
    echo "--- ${name} installed ---"
}

# ---- 1. zlib ----
build_from_tar "zlib" \
    "https://github.com/madler/zlib/releases/download/v${ZLIB_VERSION}/zlib-${ZLIB_VERSION}.tar.gz" \
    "--shared"

# ---- 2. libaec (SZIP replacement) ----
echo "--- Building libaec ---"
cd "${BUILD_DIR}"
wget -q "https://github.com/MathisRosenhauer/libaec/releases/download/v${LIBAEC_VERSION}/libaec-${LIBAEC_VERSION}.tar.gz" -O "libaec-${LIBAEC_VERSION}.tar.gz"
tar xf "libaec-${LIBAEC_VERSION}.tar.gz"
cd "libaec-${LIBAEC_VERSION}"
mkdir -p build && cd build
cmake .. \
    -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DBUILD_SHARED_LIBS=ON
make -j"${JOBS}"
make install
echo "--- libaec installed ---"

# ---- 3. HDF5 ----
echo "--- Building HDF5 ---"
cd "${BUILD_DIR}"
HDF5_URL="https://github.com/HDFGroup/hdf5/releases/download/hdf5_${HDF5_TAG}/hdf5-${HDF5_VERSION}.tar.gz"
wget -q "${HDF5_URL}" -O "hdf5-${HDF5_VERSION}.tar.gz" || \
    wget -q "https://support.hdfgroup.org/ftp/HDF5/releases/hdf5-1.14/hdf5-${HDF5_VERSION}/src/hdf5-${HDF5_VERSION}.tar.gz" -O "hdf5-${HDF5_VERSION}.tar.gz"
tar xf "hdf5-${HDF5_VERSION}.tar.gz"
cd "hdf5-${HDF5_VERSION}"
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
wget -q "https://github.com/Unidata/netcdf-c/archive/refs/tags/v${NETCDF_VERSION}.tar.gz" -O "netcdf-c-${NETCDF_VERSION}.tar.gz" || \
    wget -q "https://github.com/Unidata/netcdf-c/releases/download/v${NETCDF_VERSION}/netcdf-c-${NETCDF_VERSION}.tar.gz" -O "netcdf-c-${NETCDF_VERSION}.tar.gz" || \
    wget -q "https://downloads.unidata.ucar.edu/netcdf-c/${NETCDF_VERSION}/netcdf-c-${NETCDF_VERSION}.tar.gz" -O "netcdf-c-${NETCDF_VERSION}.tar.gz"
tar xf "netcdf-c-${NETCDF_VERSION}.tar.gz"
cd "netcdf-c-${NETCDF_VERSION}"
CPPFLAGS="-I${PREFIX}/include" LDFLAGS="-L${PREFIX}/lib -L${PREFIX}/lib64" \
    ./configure \
    --prefix="${PREFIX}" \
    --enable-shared \
    --disable-static \
    --enable-netcdf-4 \
    --disable-dap \
    --disable-byterange \
    --disable-testsets \
    --disable-libxml2 \
    --disable-nczarr
make -j"${JOBS}"
make install
echo "--- NetCDF-C installed ---"

# ---- 5. ecCodes ----
echo "--- Building ecCodes ---"
cd "${BUILD_DIR}"
wget -q "https://github.com/ecmwf/eccodes/archive/refs/tags/${ECCODES_VERSION}.tar.gz" -O "eccodes-${ECCODES_VERSION}.tar.gz" || \
    wget -q "https://confluence.ecmwf.int/download/attachments/45757960/eccodes-${ECCODES_VERSION}-Source.tar.gz" -O "eccodes-${ECCODES_VERSION}.tar.gz"
tar xf "eccodes-${ECCODES_VERSION}.tar.gz"
cd "eccodes-${ECCODES_VERSION}"
mkdir -p build && cd build
cmake .. \
    -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_FORTRAN=OFF \
    -DENABLE_PYTHON=OFF \
    -DENABLE_MEMFS=ON \
    -DENABLE_NETCDF=ON \
    -DENABLE_JPG=OFF \
    -DENABLE_PNG=OFF \
    -DENABLE_AEC=ON \
    -DAEC_DIR="${PREFIX}" \
    -DHDF5_DIR="${PREFIX}" \
    -DNETCDF_DIR="${PREFIX}" \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DBUILD_SHARED_LIBS=ON
make -j"${JOBS}"
make install
echo "--- ecCodes installed ---"

# ---- 6. FFTW3 ----
build_from_tar "FFTW3" \
    "https://www.fftw.org/fftw-${FFTW_VERSION}.tar.gz" \
    "--enable-shared --disable-static --enable-threads --enable-openmp"

# ---- 7. PROJ ----
echo "--- Building PROJ ---"
cd "${BUILD_DIR}"
wget -q "https://github.com/OSGeo/PROJ/releases/download/${PROJ_VERSION}/proj-${PROJ_VERSION}.tar.gz" -O "proj-${PROJ_VERSION}.tar.gz"
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
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
make -j"${JOBS}"
make install
echo "--- PROJ installed ---"

# ---- 8. UDUNITS2 ----
build_from_tar "UDUNITS2" \
    "https://downloads.unidata.ucar.edu/udunits/${UDUNITS_VERSION}/udunits-${UDUNITS_VERSION}.tar.gz" \
    "--enable-shared --disable-static"

echo "============================================"
echo "All dependencies built successfully!"
echo "  Installed to: ${PREFIX}"
echo "============================================"
ls -la "${PREFIX}/lib/"
