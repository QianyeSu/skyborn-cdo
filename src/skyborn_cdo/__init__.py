"""
skyborn-cdo: Pre-compiled CDO (Climate Data Operators) for Python
=================================================================

This package ships a pre-compiled CDO binary and all its dependencies
(NetCDF, HDF5, ecCodes, FFTW3, PROJ, UDUNITS2, etc.) as a pip-installable
Python wheel. It provides a convenient Python API to invoke CDO operations
without requiring a separate CDO installation.

Usage
-----
Command-line style::

    from skyborn_cdo import Cdo
    cdo = Cdo()
    cdo("cdo mergetime in1.nc in2.nc out.nc")

Method-call style::

    from skyborn_cdo import Cdo
    cdo = Cdo()
    cdo.mergetime(input="in1.nc in2.nc", output="out.nc")

Integration with Skyborn::

    # In skyborn main package:
    from skyborn_cdo import Cdo
"""

from skyborn_cdo.cdo import Cdo
from skyborn_cdo._cdo_binary import get_cdo_path, get_cdo_version

__version__ = "2.5.3.0"
__cdo_version__ = "2.5.3"

__all__ = ["Cdo", "get_cdo_path", "get_cdo_version",
           "__version__", "__cdo_version__"]
