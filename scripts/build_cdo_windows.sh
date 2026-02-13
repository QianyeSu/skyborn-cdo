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

# Apply Windows compatibility patches using intelligent Python script
echo "[skyborn-cdo] Applying Windows compatibility patches..."
python "${PROJECT_DIR}/scripts/patch_cdo_windows.py" apply --cdo-src "${CDO_SOURCE}"
if [ $? -ne 0 ]; then
    echo "[skyborn-cdo] ERROR: Patches failed to apply!"
    exit 1
fi

# Verify critical patches were applied
echo "[skyborn-cdo] Verifying patches..."
if ! grep -q "#include <pthread.h>" "${CDO_SOURCE}/src/process.h"; then
    echo "[skyborn-cdo] ERROR: pthread.h patch not applied to process.h!"
    head -25 "${CDO_SOURCE}/src/process.h"
    exit 1
fi
if ! grep -q 'std::to_string' "${CDO_SOURCE}/src/mpmo_color.h"; then
    echo "[skyborn-cdo] ERROR: mpmo_color.h rewrite not applied!"
    head -90 "${CDO_SOURCE}/src/mpmo_color.h"
    exit 1
fi
echo "[skyborn-cdo] Patches verified OK"

# Create GCC 15 workaround header: ensure pthread and locale are complete
# before any standard library template processing.
# GCC 15.2.0 issues:
#   1. <sstream> needs std::locale to be complete (not just forward-declared)
#   2. <mutex> (pulled by <locale>) needs pthread functions via gthr-posix.h
# Solution: Force-include pthread.h AND locale at the start of every .cc file.
GCC15_FIX="${CDO_SOURCE}/src/gcc15_fix.h"
cat > "${GCC15_FIX}" << 'FIXEOF'
/* GCC 15 + MinGW workaround: include pthread and locale FIRST */
#ifdef __cplusplus
#include <pthread.h>  /* Must be first: gthr-posix.h depends on this */
#include <locale>     /* Ensure complete before <sstream> processing */
#endif
FIXEOF
echo "[skyborn-cdo] Created GCC 15 workaround header: ${GCC15_FIX}"

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

# NOTE: Do NOT use --host=x86_64-w64-mingw32 here!
# In MSYS2/MinGW64, the compiler is already the MinGW cross-compiler and
# produces native Windows executables.  Adding --host makes autotools treat
# this as a cross-compilation, which disables AC_RUN_IFELSE runtime tests.
# Those skipped tests cause CDO's NetCDF format-selection code path (-f nc*)
# to be mis-configured, leading to hangs on Windows.
./configure \
    --prefix="${INSTALL_PREFIX}" \
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
    CFLAGS="-O2 -I${DEPS_PREFIX}/include" \
    CXXFLAGS="-D_USE_MATH_DEFINES -O2 -std=c++20 -Wno-template-body -include ${GCC15_FIX} -I${DEPS_PREFIX}/include" \
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
