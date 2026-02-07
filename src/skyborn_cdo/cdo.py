"""
High-level Python wrapper for CDO (Climate Data Operators).

Provides two API styles:
1. Command-line style: ``cdo("cdo mergetime *.nc out.nc")``
2. Method-call style: ``cdo.mergetime(input="in1.nc in2.nc", output="out.nc")``
"""

import glob
import os
import shlex
import subprocess
import tempfile
from typing import Any, Dict, List, Optional, Union

from skyborn_cdo._cdo_binary import get_bundled_env, get_cdo_path
from skyborn_cdo._runner import CdoError, CdoRunner


class Cdo:
    """
    Python interface to CDO (Climate Data Operators).

    All 800+ CDO operators are available as methods via ``__getattr__``.

    Examples
    --------
    Command-line style::

        cdo = Cdo()
        cdo("cdo mergetime in1.nc in2.nc out.nc")
        cdo("cdo -O -f nc4 sellonlatbox,0,30,0,30 input.nc output.nc")

    Method-call style::

        cdo = Cdo()
        cdo.mergetime(input="in1.nc in2.nc", output="out.nc")
        cdo.sellonlatbox("0,30,0,30", input="input.nc", output="output.nc")
        cdo.info(input="input.nc")

    With options::

        cdo = Cdo(options="-O -s")
        cdo.copy(input="in.nc", output="out.nc")

    Return as xarray (requires xarray + netCDF4)::

        ds = cdo.sellonlatbox("0,30,0,30", input="input.nc", returnXArray=True)
    """

    def __init__(
        self,
        cdo_path: Optional[str] = None,
        options: str = "",
        env: Optional[dict] = None,
        debug: bool = False,
        timeout: Optional[int] = None,
    ):
        """
        Parameters
        ----------
        cdo_path : str, optional
            Path to CDO binary. If None, auto-discovers in order:
            bundled binary → $CDO env → system PATH.
        options : str
            Default global CDO options (e.g. "-O -s -f nc4").
        env : dict, optional
            Custom environment. Defaults to bundled env with data paths set.
        debug : bool
            If True, print executed commands.
        timeout : int, optional
            Default timeout for all commands in seconds.
        """
        self._cdo_path = get_cdo_path(cdo_path)
        self._default_options = shlex.split(options) if options else []
        self._env = env if env is not None else get_bundled_env()
        self._debug = debug
        self._timeout = timeout
        self._runner = CdoRunner(self._cdo_path, env=self._env, debug=debug)
        self._operators: Optional[set] = None
        self._tempfiles: List[str] = []

    @property
    def cdo_path(self) -> str:
        """Path to the CDO binary being used."""
        return self._cdo_path

    def __call__(self, cmd_string: str, **kwargs) -> Union[str, int]:
        """
        Execute a CDO command given as a full command string.

        Parameters
        ----------
        cmd_string : str
            Full CDO command, e.g. "cdo mergetime in1.nc in2.nc out.nc".
            The leading "cdo" is optional.

        Returns
        -------
        str or int
            If the command produces output, returns stdout. Otherwise
            returns 0 on success.

        Examples
        --------
        >>> cdo = Cdo()
        >>> cdo("cdo mergetime in1.nc in2.nc out.nc")
        >>> cdo("mergetime in1.nc in2.nc out.nc")  # 'cdo' prefix optional
        >>> cdo("-O -s mergetime in1.nc in2.nc out.nc")
        """
        timeout = kwargs.get("timeout", self._timeout)
        result = self._runner.run_raw(cmd_string, timeout=timeout)
        if result.stdout.strip():
            return result.stdout.strip()
        return 0

    def __getattr__(self, name: str):
        """
        Dynamically dispatch CDO operators as method calls.

        Parameters
        ----------
        name : str
            CDO operator name (e.g. 'mergetime', 'sellonlatbox', 'info').

        Returns
        -------
        callable
            A function that executes the named CDO operator.
        """
        # Avoid recursion for private / dunder attributes
        if name.startswith("_"):
            raise AttributeError(
                f"'{type(self).__name__}' object has no attribute '{name}'")

        def operator_method(*args, **kwargs):
            return self._execute_operator(name, *args, **kwargs)

        operator_method.__name__ = name
        operator_method.__doc__ = f"Execute CDO operator: {name}"
        return operator_method

    def _execute_operator(
        self,
        operator: str,
        *args,
        input: Optional[Union[str, List[str]]] = None,
        output: Optional[str] = None,
        options: Optional[str] = None,
        returnCdf: bool = False,
        returnXArray: bool = False,
        returnXDataset: bool = False,
        returnArray: bool = False,
        returnMaArray: bool = False,
        timeout: Optional[int] = None,
        **kwargs,
    ) -> Any:
        """
        Execute a CDO operator.

        Parameters
        ----------
        operator : str
            CDO operator name.
        *args : str
            Operator parameters (e.g. "0,30,0,30" for sellonlatbox).
        input : str or list of str, optional
            Input file(s). Can be a space-separated string or list.
        output : str, optional
            Output file path. If returnXArray/returnCdf, a tempfile is used.
        options : str, optional
            Additional options for this call (merged with default options).
        returnXArray : bool
            If True, return result as xarray.Dataset.
        returnCdf : bool
            If True, return result as netCDF4.Dataset.
        returnArray : bool
            If True, return result as numpy array (first variable).
        returnMaArray : bool
            If True, return result as masked numpy array (first variable).
        timeout : int, optional
            Override default timeout.

        Returns
        -------
        Various
            Depending on return* flags: xarray.Dataset, netCDF4.Dataset,
            numpy.ndarray, stdout string, or 0 on success.
        """
        # Build the operator string with parameters
        operator_params = ",".join(str(a) for a in args) if args else ""
        if operator_params:
            op_str = f"-{operator},{operator_params}"
        else:
            op_str = f"-{operator}"

        # Merge options
        cmd_options = list(self._default_options)
        if options:
            cmd_options.extend(shlex.split(options))

        # Process input files
        input_files = []
        if input is not None:
            if isinstance(input, (list, tuple)):
                input_files = list(input)
            else:
                input_files = shlex.split(str(input))

        # Handle return types that need a temp file
        need_output_file = returnXArray or returnXDataset or returnCdf or returnArray or returnMaArray
        temp_output = None

        if need_output_file and output is None:
            temp_output = tempfile.NamedTemporaryFile(
                suffix=".nc", prefix="skyborn_cdo_", delete=False
            )
            output = temp_output.name
            self._tempfiles.append(output)
            # Force NetCDF output for return types
            if "-f" not in " ".join(cmd_options):
                cmd_options.extend(["-f", "nc"])

        # Determine if we should capture output (info, showdate, etc.)
        info_operators = {
            "info", "infon", "infov", "sinfo", "sinfon", "sinfov",
            "showdate", "showtime", "showtimestamp", "showcode",
            "showname", "showlevel", "showltype", "showunit",
            "showstdname", "showformat", "showgrid", "showgriddes",
            "showmon", "showyear", "showvar", "showparam",
            "ncode", "ndate", "ngridpoints", "nlevel", "nmon", "ntime",
            "nvar", "nyear", "griddes", "zaxisdes", "pardes", "vct",
            "showattribute", "showatts", "showattsglob",
            "npar", "gradsdes", "filedes", "partab",
        }

        return_output = (
            output is None
            and not need_output_file
            and operator.lower() in info_operators
        )

        # Build and execute command
        try:
            result = self._runner.run(
                args=[op_str],
                input_files=input_files,
                output_file=output,
                options=cmd_options,
                return_output=return_output,
                timeout=timeout or self._timeout,
            )
        except CdoError:
            # Clean up temp file on error
            if temp_output and os.path.exists(output):
                os.unlink(output)
            raise

        # Handle return types
        if returnXArray or returnXDataset:
            return self._return_xarray(output)
        elif returnCdf:
            return self._return_cdf(output)
        elif returnArray:
            return self._return_array(output, masked=False)
        elif returnMaArray:
            return self._return_array(output, masked=True)
        elif return_output:
            return result

        return result

    def _return_xarray(self, filepath: str):
        """Open file as xarray Dataset."""
        try:
            import xarray as xr
        except ImportError:
            raise ImportError(
                "xarray is required for returnXArray. "
                "Install with: pip install skyborn-cdo[xarray]"
            )
        return xr.open_dataset(filepath)

    def _return_cdf(self, filepath: str):
        """Open file as netCDF4 Dataset."""
        try:
            import netCDF4
        except ImportError:
            raise ImportError(
                "netCDF4 is required for returnCdf. "
                "Install with: pip install netCDF4"
            )
        return netCDF4.Dataset(filepath)

    def _return_array(self, filepath: str, masked: bool = False):
        """Read first variable from file as numpy array."""
        try:
            import netCDF4
            import numpy as np
        except ImportError:
            raise ImportError(
                "netCDF4 and numpy are required for returnArray. "
                "Install with: pip install netCDF4 numpy"
            )
        with netCDF4.Dataset(filepath) as ds:
            # Find first non-dimension variable
            for varname in ds.variables:
                if varname not in ds.dimensions:
                    var = ds.variables[varname]
                    if masked:
                        return var[:]
                    else:
                        return np.asarray(var[:])
            raise CdoError(f"No data variables found in {filepath}")

    def version(self) -> str:
        """Return CDO version string."""
        from skyborn_cdo._cdo_binary import get_cdo_version
        return get_cdo_version(self._cdo_path)

    def operators(self) -> set:
        """
        Get set of all available CDO operators.

        Queries CDO binary with ``--operators`` flag on first call,
        then caches the result.

        Returns
        -------
        set of str
            Available operator names.
        """
        if self._operators is None:
            try:
                result = self._runner.run(
                    args=["--operators"],
                    return_output=True,
                )
                self._operators = set()
                for line in result.strip().split("\n"):
                    parts = line.strip().split()
                    if parts:
                        self._operators.add(parts[0])
            except CdoError:
                self._operators = set()
        return self._operators

    def has_operator(self, name: str) -> bool:
        """Check if a CDO operator is available."""
        return name in self.operators()

    def cleanup(self):
        """Remove all temporary files created by this instance."""
        for f in self._tempfiles:
            try:
                if os.path.exists(f):
                    os.unlink(f)
            except OSError:
                pass
        self._tempfiles.clear()

    def __del__(self):
        """Clean up temp files on garbage collection."""
        self.cleanup()

    def __repr__(self) -> str:
        return f"Cdo(cdo_path='{self._cdo_path}', options={self._default_options})"
