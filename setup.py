"""
setup.py for skyborn-cdo

Custom build_ext that:
1. Compiles all CDO dependencies from source (in CI)
2. Compiles CDO itself
3. Copies the binary + shared libs + data files into the package
4. Falls back to a dummy extension for development installs
"""

import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path

from setuptools import Extension, find_packages, setup
from setuptools.command.build_ext import build_ext

# Root of this project
HERE = Path(__file__).parent.resolve()
VENDOR_CDO = HERE / "vendor" / "cdo"
SRC_DIR = HERE / "src" / "skyborn_cdo"


class CdoBuildExt(build_ext):
    """
    Custom build_ext that compiles CDO and bundles it into the package.

    In CI, this calls the platform-specific build scripts to compile
    CDO and all its dependencies from source.

    For local development without the CDO source tree (vendor/cdo),
    it only builds the dummy C extension for platform tagging.
    """

    def run(self):
        # Always build the dummy platform extension
        build_ext.run(self)

        # Check if we should build CDO (CI mode)
        if os.environ.get("SKYBORN_CDO_BUILD", "0") == "1":
            self._build_cdo()
        elif VENDOR_CDO.exists() and (VENDOR_CDO / "configure.ac").exists():
            print(
                "[skyborn-cdo] CDO source found at vendor/cdo but SKYBORN_CDO_BUILD != 1. "
                "Set SKYBORN_CDO_BUILD=1 to compile CDO. "
                "Skipping CDO compilation (development mode)."
            )
        else:
            print(
                "[skyborn-cdo] No CDO source tree found (vendor/cdo/). "
                "Building Python wrapper only (CDO binary not bundled)."
            )

    def _build_cdo(self):
        """Compile CDO and copy artifacts into the package."""
        system = platform.system()
        scripts_dir = HERE / "scripts"

        deps_prefix = os.environ.get("CDO_DEPS_PREFIX", "/opt/cdo-deps")
        install_prefix = os.environ.get(
            "CDO_INSTALL_PREFIX", "/opt/cdo-install")

        print(f"[skyborn-cdo] Building CDO for {system}")
        print(f"[skyborn-cdo] Dependencies prefix: {deps_prefix}")
        print(f"[skyborn-cdo] Install prefix: {install_prefix}")

        # Step 1: Build dependencies
        if system == "Linux":
            self._run_script(scripts_dir / "build_deps_linux.sh")
        elif system == "Darwin":
            self._run_script(scripts_dir / "build_deps_macos.sh")
        elif system == "Windows":
            self._run_script(scripts_dir / "build_deps_windows.sh")
        else:
            raise RuntimeError(f"Unsupported platform: {system}")

        # Step 2: Build CDO
        self._run_script(scripts_dir / "build_cdo.sh")

        # Step 3: Copy artifacts into package
        self._install_artifacts(install_prefix, deps_prefix)

    def _run_script(self, script_path: Path):
        """Execute a build script."""
        if not script_path.exists():
            raise FileNotFoundError(f"Build script not found: {script_path}")

        print(f"[skyborn-cdo] Running: {script_path}")

        if platform.system() == "Windows":
            # Run through MSYS2 bash
            cmd = ["bash", str(script_path)]
        else:
            cmd = ["bash", str(script_path)]

        env = os.environ.copy()
        env["CDO_SOURCE_DIR"] = str(VENDOR_CDO)

        result = subprocess.run(cmd, env=env, cwd=str(HERE))
        if result.returncode != 0:
            raise RuntimeError(f"Build script failed: {script_path}")

    def _install_artifacts(self, install_prefix: str, deps_prefix: str):
        """Copy compiled CDO binary, libraries, and data files into the package."""
        bin_dir = SRC_DIR / "bin"
        lib_dir = SRC_DIR / "lib"
        share_dir = SRC_DIR / "share"

        # Create directories
        bin_dir.mkdir(parents=True, exist_ok=True)
        lib_dir.mkdir(parents=True, exist_ok=True)
        share_dir.mkdir(parents=True, exist_ok=True)

        system = platform.system()
        exe_name = "cdo.exe" if system == "Windows" else "cdo"

        # Copy CDO binary
        cdo_bin = Path(install_prefix) / "bin" / exe_name
        if cdo_bin.exists():
            shutil.copy2(str(cdo_bin), str(bin_dir / exe_name))
            if system != "Windows":
                os.chmod(str(bin_dir / exe_name), 0o755)
            print(f"[skyborn-cdo] Installed CDO binary: {bin_dir / exe_name}")

        # Copy shared libraries
        for lib_src_dir in [
            Path(deps_prefix) / "lib",
            Path(deps_prefix) / "lib64",
            Path(install_prefix) / "lib",
        ]:
            if lib_src_dir.exists():
                if system == "Windows":
                    patterns = ["*.dll"]
                elif system == "Darwin":
                    patterns = ["*.dylib"]
                else:
                    patterns = ["*.so", "*.so.*"]

                for pattern in patterns:
                    for f in lib_src_dir.glob(pattern):
                        if f.is_file() and not f.is_symlink():
                            shutil.copy2(str(f), str(lib_dir / f.name))
                        elif f.is_symlink():
                            # Preserve symlinks
                            link_target = os.readlink(str(f))
                            dest = lib_dir / f.name
                            if dest.exists():
                                dest.unlink()
                            os.symlink(link_target, str(dest))

        # Copy data files
        data_dirs = {
            "eccodes": [
                Path(deps_prefix) / "share" / "eccodes",
            ],
            "proj": [
                Path(deps_prefix) / "share" / "proj",
            ],
            "udunits": [
                Path(deps_prefix) / "share" / "udunits",
            ],
        }

        for name, src_dirs in data_dirs.items():
            for src in src_dirs:
                if src.exists():
                    dst = share_dir / name
                    if dst.exists():
                        shutil.rmtree(str(dst))
                    shutil.copytree(str(src), str(dst))
                    print(f"[skyborn-cdo] Installed data: {dst}")

        print("[skyborn-cdo] Artifact installation complete.")


# Dummy C extension for platform wheel generation
ext_modules = [
    Extension(
        "skyborn_cdo._cdo_platform",
        sources=["src/skyborn_cdo/_cdo_platform.c"],
        optional=True,
    ),
]


setup(
    ext_modules=ext_modules,
    cmdclass={"build_ext": CdoBuildExt},
)
