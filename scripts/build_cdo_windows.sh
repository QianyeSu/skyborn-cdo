#!/usr/bin/env bash
# =============================================================================
# build_cdo_windows.sh - Compile CDO from source on Windows (MSYS2/MinGW64)
# =============================================================================
# Runs after build_deps_windows.sh has installed all dependencies.
# Applies Windows compatibility patches to CDO source before building.
#
# Environment variables:
#   CDO_SOURCE_DIR     - Path to CDO source (default: vendor/cdo)
#   CDO_DEPS_PREFIX    - Where dependencies are (default: /mingw64)
#   CDO_INSTALL_PREFIX - Where CDO will be installed (default: /opt/cdo-install)
#   PARALLEL_JOBS      - Number of parallel make jobs (default: nproc)
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "${SCRIPT_DIR}")"

CDO_SOURCE="${CDO_SOURCE_DIR:-${PROJECT_DIR}/vendor/cdo}"
DEPS_PREFIX="${CDO_DEPS_PREFIX:-/mingw64}"
INSTALL_PREFIX="${CDO_INSTALL_PREFIX:-/opt/cdo-install}"
JOBS="${PARALLEL_JOBS:-$(nproc)}"

echo "============================================"
echo "Building CDO for Windows (MSYS2/MinGW64)"
echo "  Source:     ${CDO_SOURCE}"
echo "  Deps:       ${DEPS_PREFIX}"
echo "  Install to: ${INSTALL_PREFIX}"
echo "============================================"

cd "${CDO_SOURCE}"

# Apply Windows compatibility patch
PATCH_FILE="${PROJECT_DIR}/patches/windows-compat.patch"
if [[ -f "${PATCH_FILE}" ]]; then
    echo "[skyborn-cdo] Applying Windows compatibility patch..."
    patch -p1 --forward < "${PATCH_FILE}" || true
fi

# Prevent make from trying to regenerate autotools files.
# The vendored source includes pre-generated configure/Makefile.in/aclocal.m4,
# but git checkout sets all timestamps to the same time, which can cause make
# to think the generated files are stale and try to re-run aclocal/autoconf.
# Touch source files first, then generated files 1s later to ensure correct ordering.
echo "[skyborn-cdo] Fixing autotools timestamps..."
find . -name 'configure.ac' -exec touch {} +
find . -name 'Makefile.am' -exec touch {} +
sleep 1
find . -name aclocal.m4 -exec touch {} +
find . -name configure -exec touch {} +
find . -name Makefile.in -exec touch {} +
find . -name config.h.in -exec touch {} +

# Ensure configure and autotools helper scripts are executable (they may be stored as 644 in git)
find . -name configure -exec chmod +x {} +
find . \( -name 'config.sub' -o -name 'config.guess' -o -name 'install-sh' \
    -o -name 'missing' -o -name 'compile' -o -name 'depcomp' \
    -o -name 'ltmain.sh' -o -name 'test-driver' \) -exec chmod +x {} +

# If configure doesn't exist, run autoreconf
if [[ ! -f configure ]]; then
    echo "[skyborn-cdo] Running autoreconf..."
    autoreconf -fvi
fi

export PKG_CONFIG_PATH="${DEPS_PREFIX}/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
export PATH="${DEPS_PREFIX}/bin:${PATH}"

echo "[skyborn-cdo] Configuring CDO for Windows..."

./configure \
    --prefix="${INSTALL_PREFIX}" \
    --host=x86_64-w64-mingw32 \
    --with-netcdf="${DEPS_PREFIX}" \
    --with-hdf5="${DEPS_PREFIX}" \
    --with-eccodes="${DEPS_PREFIX}" \
    --with-fftw3 \
    --with-proj="${DEPS_PREFIX}" \
    --with-udunits2="${DEPS_PREFIX}" \
    --with-szlib="${DEPS_PREFIX}" \
    --with-threads=yes \
    --disable-fortran \
    --disable-across \
    --disable-custom-modules \
    --enable-cgribex \
    CXXFLAGS="-D_USE_MATH_DEFINES -O2 -std=c++20 -I${DEPS_PREFIX}/include -fopenmp -pthread" \
    CPPFLAGS="-I${DEPS_PREFIX}/include" \
    LDFLAGS="-L${DEPS_PREFIX}/lib" \
    LIBS="-lz -lm -lws2_32 -lrpcrt4"

echo "[skyborn-cdo] Building CDO..."
# Disable libcdi tests that use POSIX-only functions (srand48, lrand48, etc.)
# not available on MinGW/Windows
if [[ -f libcdi/Makefile ]]; then
    sed -i 's/ tests$//; s/ tests / /g' libcdi/Makefile
fi
make -j"${JOBS}"

echo "[skyborn-cdo] Installing CDO..."
make install

echo "============================================"
echo "CDO Windows build complete!"
echo "============================================"

if [[ -f "${INSTALL_PREFIX}/bin/cdo.exe" ]]; then
    echo "Binary: ${INSTALL_PREFIX}/bin/cdo.exe"
    echo "Size:   $(du -h "${INSTALL_PREFIX}/bin/cdo.exe" | cut -f1)"
    echo ""
    echo "--- DLL dependencies ---"
    ldd "${INSTALL_PREFIX}/bin/cdo.exe" || true
fi
