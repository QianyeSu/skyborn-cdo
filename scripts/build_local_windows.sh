#!/bin/bash
# Local Windows CDO build using MSYS2 MinGW-w64
# Run this from MSYS2 MinGW64 shell:
#   /c/msys64/mingw64.exe  (or from PowerShell: C:\msys64\usr\bin\bash.exe -lc "...")
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CDO_SRC="$PROJECT_DIR/vendor/cdo"
BUILD_DIR="$PROJECT_DIR/build_win64"
PREFIX="$PROJECT_DIR/install_win64"

echo "=== CDO Windows (MinGW-w64) Build ==="
echo "Source:  $CDO_SRC"
echo "Build:   $BUILD_DIR"
echo "Install: $PREFIX"

# Clean previous build
rm -rf "$BUILD_DIR" "$PREFIX"
mkdir -p "$BUILD_DIR" "$PREFIX"

cd "$CDO_SRC"

# Apply Windows compatibility patches using intelligent Python script
echo ""
echo "=== Applying Windows compatibility patches ==="
python "${PROJECT_DIR}/scripts/patch_cdo_windows.py" apply --cdo-src "$CDO_SRC"
if [ $? -ne 0 ]; then
    echo "Warning: Some patches failed to apply"
fi

# Check if configure exists (pre-generated)
if [ ! -f configure ]; then
    echo "Running autoreconf..."
    autoreconf -fiv
fi

# Configure CDO
cd "$BUILD_DIR"
echo ""
echo "=== Configuring CDO ==="
"$CDO_SRC/configure" \
    --prefix="$PREFIX" \
    --with-netcdf=/mingw64 \
    --with-hdf5=/mingw64 \
    --with-eccodes=/mingw64 \
    --with-fftw3 \
    --with-proj=/mingw64 \
    --with-udunits2=/mingw64 \
    --disable-custom-modules \
    --with-threads=yes \
    CFLAGS="-O2 -I/mingw64/include" \
    CXXFLAGS="-O2 -std=c++20 -I/mingw64/include" \
    CPPFLAGS="-I/mingw64/include" \
    LDFLAGS="-L/mingw64/lib" \
    LIBS="-lws2_32"

echo ""
echo "=== Building CDO ==="
make -j$(nproc)

echo ""
echo "=== Installing CDO ==="
make install

echo ""
echo "=== Build Complete ==="
echo "CDO binary: $PREFIX/bin/cdo.exe"
ls -la "$PREFIX/bin/"

echo ""
echo "=== Testing CDO ==="
"$PREFIX/bin/cdo.exe" --version || true

# Restore vendor directory to clean state (undo patch modifications)
echo ""
echo "=== Restoring vendor directory ==="
python "${PROJECT_DIR}/scripts/patch_cdo_windows.py" restore --cdo-src "$CDO_SRC"
