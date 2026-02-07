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

    # Library path — ensure bundled .so/.dylib/.dll can be found
    lib_dir = pkg_dir / "lib"
    if lib_dir.is_dir():
        system = platform.system()
        if system == "Linux":
            existing = env.get("LD_LIBRARY_PATH", "")
            env["LD_LIBRARY_PATH"] = f"{lib_dir}:{existing}" if existing else str(
                lib_dir)
        elif system == "Darwin":
            existing = env.get("DYLD_LIBRARY_PATH", "")
            env["DYLD_LIBRARY_PATH"] = f"{lib_dir}:{existing}" if existing else str(
                lib_dir)
        elif system == "Windows":
            existing = env.get("PATH", "")
            env["PATH"] = f"{lib_dir};{existing}" if existing else str(lib_dir)

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
    try:
        result = subprocess.run(
            [cdo, "--version"],
            capture_output=True,
            text=True,
            timeout=10,
            env=env,
        )
        # CDO prints version to stderr
        output = result.stderr.strip() or result.stdout.strip()
        return output
    except (subprocess.TimeoutExpired, OSError) as e:
        return f"Error getting CDO version: {e}"
