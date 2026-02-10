#!/usr/bin/env bash
# =============================================================================
# build_deps_windows.sh - Compile CDO dependencies on Windows (MSYS2/MinGW64)
# =============================================================================
# This script is intended to run inside MSYS2 MINGW64 shell on GitHub Actions.
# It uses pacman to install pre-built MinGW packages where available, and
# compiles remaining libraries from source.
#
# Environment variables:
#   CDO_DEPS_PREFIX  - Installation prefix (default: /mingw64)
#   PARALLEL_JOBS    - Number of parallel make jobs (default: nproc)
# =============================================================================

set -euo pipefail

PREFIX="${CDO_DEPS_PREFIX:-/mingw64}"
JOBS="${PARALLEL_JOBS:-$(nproc)}"
BUILD_DIR="/tmp/cdo-build-deps"

echo "============================================"
echo "Building CDO dependencies for Windows (MSYS2/MinGW64)"
echo "  PREFIX:    ${PREFIX}"
echo "  JOBS:      ${JOBS}"
echo "============================================"

# NOTE: Base packages (gcc, cmake, hdf5, netcdf, fftw, proj, eccodes, etc.)
#       are installed by the GitHub Actions workflow via setup-msys2 `install:`.
#       This script only builds libraries NOT available in MSYS2 repos.

# UDUNITS2 and libaec are not in MSYS2 repos, build from source
mkdir -p "${BUILD_DIR}"

export PKG_CONFIG_PATH="${PREFIX}/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
export CFLAGS="-O2"
export CXXFLAGS="-O2"
export LDFLAGS="-L${PREFIX}/lib"
export CPPFLAGS="-I${PREFIX}/include"

# ---- UDUNITS2 ----
UDUNITS_VERSION="2.2.28"
echo "--- Building UDUNITS2 ---"
cd "${BUILD_DIR}"
wget -q "https://downloads.unidata.ucar.edu/udunits/${UDUNITS_VERSION}/udunits-${UDUNITS_VERSION}.tar.gz" -O "udunits-${UDUNITS_VERSION}.tar.gz"
tar xf "udunits-${UDUNITS_VERSION}.tar.gz"
cd "udunits-${UDUNITS_VERSION}"
./configure \
    --prefix="${PREFIX}" \
    --enable-shared \
    --disable-static
make -j"${JOBS}"
make install
echo "--- UDUNITS2 installed ---"

# ---- libaec (szlib replacement) ----
LIBAEC_VERSION="1.1.3"
echo "--- Building libaec ---"
cd "${BUILD_DIR}"
wget -q "https://github.com/MathisRosenhauer/libaec/releases/download/v${LIBAEC_VERSION}/libaec-${LIBAEC_VERSION}.tar.gz" -O "libaec-${LIBAEC_VERSION}.tar.gz"
tar xf "libaec-${LIBAEC_VERSION}.tar.gz"
cd "libaec-${LIBAEC_VERSION}"
mkdir -p build && cd build
cmake .. -G "MinGW Makefiles" \
    -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
    -DBUILD_SHARED_LIBS=ON
mingw32-make -j"${JOBS}"
mingw32-make install
echo "--- libaec installed ---"

echo "============================================"
echo "All dependencies built successfully!"
echo "  Installed to: ${PREFIX}"
echo "============================================"
ls -la "${PREFIX}/lib/" | head -30 || true
ls -la "${PREFIX}/bin/" | grep -E '\.(dll|exe)$' | head -20 || true
