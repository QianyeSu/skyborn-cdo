"""
CLI entry point for skyborn-cdo.

Allows running CDO commands directly via: skyborn-cdo <cdo args>
or checking the installation via: skyborn-cdo --info
"""

import sys

from skyborn_cdo._cdo_binary import get_bundled_env, get_cdo_path, get_cdo_version


def main():
    """Entry point for the skyborn-cdo console script."""
    args = sys.argv[1:]

    if not args or args[0] in ("--info", "-i"):
        _print_info()
        return

    if args[0] in ("--help", "-h"):
        _print_help()
        return

    # Pass-through to CDO
    import subprocess

    cdo_path = get_cdo_path()
    env = get_bundled_env()

    result = subprocess.run(
        [cdo_path] + args,
        env=env,
    )
    sys.exit(result.returncode)


def _print_info():
    """Print skyborn-cdo installation info."""
    import skyborn_cdo

    print(f"skyborn-cdo version: {skyborn_cdo.__version__}")
    print(f"CDO version target:  {skyborn_cdo.__cdo_version__}")
    try:
        cdo_path = get_cdo_path()
        print(f"CDO binary path:     {cdo_path}")
        print()
        version = get_cdo_version(cdo_path)
        print(version)
    except FileNotFoundError as e:
        print(f"\nCDO binary NOT FOUND: {e}")


def _print_help():
    print("skyborn-cdo: Pre-compiled CDO (Climate Data Operators) for Python")
    print()
    print("Usage:")
    print("  skyborn-cdo --info         Show CDO binary info and version")
    print("  skyborn-cdo <cdo-args>     Pass arguments directly to CDO")
    print()
    print("Python API:")
    print("  from skyborn_cdo import Cdo")
    print('  cdo = Cdo()')
    print('  cdo("cdo mergetime in1.nc in2.nc out.nc")')
    print('  cdo.mergetime(input="in1.nc in2.nc", output="out.nc")')


if __name__ == "__main__":
    main()
