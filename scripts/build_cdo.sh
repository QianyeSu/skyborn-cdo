#!/usr/bin/env bash
# =============================================================================
# build_cdo.sh - Compile CDO from source
# =============================================================================
# Runs after build_deps_*.sh has installed all dependencies.
#
# Environment variables:
#   CDO_SOURCE_DIR     - Path to CDO source (default: vendor/cdo)
#   CDO_DEPS_PREFIX    - Where dependencies are installed (default: /opt/cdo-deps)
#   CDO_INSTALL_PREFIX - Where CDO will be installed (default: /opt/cdo-install)
#   PARALLEL_JOBS      - Number of parallel make jobs (default: nproc)
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "${SCRIPT_DIR}")"

CDO_SOURCE="${CDO_SOURCE_DIR:-${PROJECT_DIR}/vendor/cdo}"
DEPS_PREFIX="${CDO_DEPS_PREFIX:-/opt/cdo-deps}"
INSTALL_PREFIX="${CDO_INSTALL_PREFIX:-/opt/cdo-install}"
JOBS="${PARALLEL_JOBS:-$(nproc)}"

echo "============================================"
echo "Building CDO"
echo "  Source:     ${CDO_SOURCE}"
echo "  Deps:       ${DEPS_PREFIX}"
echo "  Install to: ${INSTALL_PREFIX}"
echo "  Jobs:       ${JOBS}"
echo "============================================"

if [[ ! -f "${CDO_SOURCE}/configure.ac" ]]; then
    echo "ERROR: CDO source not found at ${CDO_SOURCE}"
    echo "       Expected configure.ac in that directory."
    exit 1
fi

cd "${CDO_SOURCE}"

# If configure doesn't exist, run autoreconf
if [[ ! -f configure ]]; then
    echo "[skyborn-cdo] Running autoreconf..."
    autoreconf -fvi
fi

export PKG_CONFIG_PATH="${DEPS_PREFIX}/lib/pkgconfig:${DEPS_PREFIX}/lib64/pkgconfig:${PKG_CONFIG_PATH:-}"
export LD_LIBRARY_PATH="${DEPS_PREFIX}/lib:${DEPS_PREFIX}/lib64:${LD_LIBRARY_PATH:-}"

echo "[skyborn-cdo] Configuring CDO..."

./configure \
    --prefix="${INSTALL_PREFIX}" \
    --with-netcdf="${DEPS_PREFIX}" \
    --with-hdf5="${DEPS_PREFIX}" \
    --with-eccodes="${DEPS_PREFIX}" \
    --with-fftw3 \
    --with-proj="${DEPS_PREFIX}" \
    --with-udunits2="${DEPS_PREFIX}" \
    --with-szlib="${DEPS_PREFIX}" \
    --disable-fortran \
    --disable-across \
    --enable-cgribex \
    CPPFLAGS="-I${DEPS_PREFIX}/include" \
    LDFLAGS="-L${DEPS_PREFIX}/lib -L${DEPS_PREFIX}/lib64 -Wl,-rpath,'\$ORIGIN/../lib'" \
    LIBS="-lz -lm"

echo "[skyborn-cdo] Building CDO..."
make -j"${JOBS}"

echo "[skyborn-cdo] Installing CDO..."
make install

echo "============================================"
echo "CDO build complete!"
echo "============================================"

# Print build summary
echo ""
echo "--- CDO configuration summary ---"
if [[ -f "${INSTALL_PREFIX}/bin/cdo" ]]; then
    "${INSTALL_PREFIX}/bin/cdo" --version 2>&1 || true
    echo ""
    echo "Binary:  ${INSTALL_PREFIX}/bin/cdo"
    echo "Size:    $(du -h "${INSTALL_PREFIX}/bin/cdo" | cut -f1)"
    echo ""
    echo "--- Linked libraries ---"
    if command -v ldd &>/dev/null; then
        ldd "${INSTALL_PREFIX}/bin/cdo" || true
    elif command -v otool &>/dev/null; then
        otool -L "${INSTALL_PREFIX}/bin/cdo" || true
    fi
else
    echo "WARNING: CDO binary not found at ${INSTALL_PREFIX}/bin/cdo"
fi
