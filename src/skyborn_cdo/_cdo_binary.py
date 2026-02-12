"""
Locate and manage the bundled CDO binary.

This module handles finding the CDO executable, either bundled inside
this package's wheel or installed externally on the system.
"""

import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Optional


def _package_bin_dir() -> Path:
    """Return the path to the bin/ directory bundled inside this package."""
    return Path(__file__).parent / "bin"


def _get_exe_name() -> str:
    """Return the CDO executable name for this platform."""
    if platform.system() == "Windows":
        return "cdo.exe"
    return "cdo"


def get_cdo_path(cdo_path: Optional[str] = None) -> str:
    """
    Find the CDO executable.

    Search order:
    1. Explicitly provided ``cdo_path`` argument
    2. ``CDO`` environment variable
    3. Bundled binary inside this package (``skyborn_cdo/bin/cdo``)
    4. System PATH via ``shutil.which("cdo")``

    Parameters
    ----------
    cdo_path : str, optional
        Explicit path to the CDO binary.

    Returns
    -------
    str
        Absolute path to the CDO executable.

    Raises
    ------
    FileNotFoundError
        If CDO cannot be found anywhere.
    """
    # 1. Explicit argument
    if cdo_path and os.path.isfile(cdo_path):
        return os.path.abspath(cdo_path)

    # 2. Environment variable
    env_cdo = os.environ.get("CDO")
    if env_cdo and os.path.isfile(env_cdo):
        return os.path.abspath(env_cdo)

    # 3. Bundled binary
    bundled = _package_bin_dir() / _get_exe_name()
    if bundled.is_file():
        return str(bundled)

    # 4. System PATH
    system_cdo = shutil.which("cdo")
    if system_cdo:
        return system_cdo

    raise FileNotFoundError(
        "CDO binary not found. Tried:\n"
        f"  1. Explicit path: {cdo_path}\n"
        f"  2. $CDO env var: {env_cdo}\n"
        f"  3. Bundled: {bundled}\n"
        "  4. System PATH\n"
        "\n"
        "If you installed skyborn-cdo from a platform wheel, the binary\n"
        "should be bundled. Otherwise, install CDO separately and set\n"
        "the CDO environment variable to its path."
    )


def get_bundled_env() -> dict:
    """
    Build environment variables for the bundled CDO to find its data files.

    Sets paths for ecCodes definition tables, PROJ data, UDUNITS2 XML, etc.

    Returns
    -------
    dict
        Merged environment dictionary (current env + CDO-specific paths).
    """
    env = os.environ.copy()
    pkg_dir = Path(__file__).parent

    # ecCodes definitions
    eccodes_defs = pkg_dir / "share" / "eccodes" / "definitions"
    if eccodes_defs.is_dir():
        env["ECCODES_DEFINITION_PATH"] = str(eccodes_defs)

    # ecCodes samples
    eccodes_samples = pkg_dir / "share" / "eccodes" / "samples"
    if eccodes_samples.is_dir():
        env["ECCODES_SAMPLES_PATH"] = str(eccodes_samples)

    # PROJ data
    proj_data = pkg_dir / "share" / "proj"
    if proj_data.is_dir():
        env["PROJ_DATA"] = str(proj_data)
        env["PROJ_LIB"] = str(proj_data)  # Legacy variable

    # UDUNITS2 XML
    udunits_xml = pkg_dir / "share" / "udunits" / "udunits2.xml"
    if udunits_xml.is_file():
        env["UDUNITS2_XML_PATH"] = str(udunits_xml)

    # Fallback for development mode on Windows: if bundled share/ data
    # directories are missing, try to locate them from MSYS2/MinGW64.
    if platform.system() == "Windows":
        msys2_share = Path(r"C:\msys64\mingw64\share")
        if msys2_share.is_dir():
            if "ECCODES_DEFINITION_PATH" not in env:
                fallback = msys2_share / "eccodes" / "definitions"
                if fallback.is_dir():
                    env["ECCODES_DEFINITION_PATH"] = str(fallback)
            if "ECCODES_SAMPLES_PATH" not in env:
                fallback = msys2_share / "eccodes" / "samples"
                if fallback.is_dir():
                    env["ECCODES_SAMPLES_PATH"] = str(fallback)
            if "PROJ_DATA" not in env:
                fallback = msys2_share / "proj"
                if fallback.is_dir():
                    env["PROJ_DATA"] = str(fallback)
                    env["PROJ_LIB"] = str(fallback)
            if "UDUNITS2_XML_PATH" not in env:
                fallback = msys2_share / "udunits" / "udunits2.xml"
                if fallback.is_file():
                    env["UDUNITS2_XML_PATH"] = str(fallback)

    # HDF5 file locking can cause hangs on Windows (and some network
    # filesystems on Linux/macOS).  Disable it for the bundled CDO.
    # Some HDF5 builds only recognise the string "FALSE", others only "0".
    env["HDF5_USE_FILE_LOCKING"] = "FALSE"
    # Prevent HDF5 atexit cleanup that can hang on Windows when the
    # process is terminated mid-write.
    env.setdefault("HDF5_NOCLEANUP", "1")

    # Library / DLL path — ensure bundled .so/.dylib/.dll can be found
    lib_dir = pkg_dir / "lib"
    bin_dir = pkg_dir / "bin"
    system = platform.system()

    if system == "Linux":
        paths = []
        if lib_dir.is_dir():
            paths.append(str(lib_dir))
        # auditwheel may place vendored .so files in a .libs/ sibling dir
        auditwheel_libs = pkg_dir.parent / (pkg_dir.name + ".libs")
        if auditwheel_libs.is_dir():
            paths.append(str(auditwheel_libs))
        if paths:
            existing = env.get("LD_LIBRARY_PATH", "")
            lib_path = ":".join(paths)
            env["LD_LIBRARY_PATH"] = f"{lib_path}:{existing}" if existing else lib_path
    elif system == "Darwin":
        if lib_dir.is_dir():
            existing = env.get("DYLD_LIBRARY_PATH", "")
            env["DYLD_LIBRARY_PATH"] = f"{lib_dir}:{existing}" if existing else str(
                lib_dir)
    elif system == "Windows":
        # On Windows, DLLs are typically co-located with cdo.exe in bin/.
        # Prepend both bin/ and lib/ (if they exist) to PATH so the
        # dynamic linker can resolve all dependencies.
        extra_dirs = []
        if bin_dir.is_dir():
            extra_dirs.append(str(bin_dir))
        if lib_dir.is_dir():
            extra_dirs.append(str(lib_dir))
        if extra_dirs:
            existing = env.get("PATH", "")
            prefix = ";".join(extra_dirs)
            env["PATH"] = f"{prefix};{existing}" if existing else prefix

    return env


def get_cdo_version(cdo_path: Optional[str] = None) -> str:
    """
    Get the version string of the CDO binary.

    Parameters
    ----------
    cdo_path : str, optional
        Path to CDO binary. If not given, uses :func:`get_cdo_path`.

    Returns
    -------
    str
        CDO version string (e.g. "Climate Data Operators version 2.5.3 ...").
    """
    cdo = get_cdo_path(cdo_path)
    env = get_bundled_env()
    _creationflags = subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0
    try:
        result = subprocess.run(
            [cdo, "--version"],
            capture_output=True,
            text=True,
            timeout=10,
            env=env,
            stdin=subprocess.DEVNULL,
            creationflags=_creationflags,
        )
        # CDO prints version to stderr
        output = result.stderr.strip() or result.stdout.strip()
        return output
    except (subprocess.TimeoutExpired, OSError) as e:
        return f"Error getting CDO version: {e}"
