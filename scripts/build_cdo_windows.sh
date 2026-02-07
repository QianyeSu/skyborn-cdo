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
    --disable-fortran \
    --enable-cgribex \
    CPPFLAGS="-I${DEPS_PREFIX}/include" \
    LDFLAGS="-L${DEPS_PREFIX}/lib" \
    LIBS="-lz -lm -lws2_32"

echo "[skyborn-cdo] Building CDO..."
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
