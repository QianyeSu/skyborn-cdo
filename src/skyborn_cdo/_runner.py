"""
Low-level subprocess runner for CDO commands.
"""

import os
import shlex
import subprocess
import tempfile
from pathlib import Path
from typing import List, Optional, Union


class CdoError(Exception):
    """Exception raised when a CDO command fails."""

    def __init__(self, message: str, returncode: int = -1, stderr: str = "", cmd: str = ""):
        self.returncode = returncode
        self.stderr = stderr
        self.cmd = cmd
        super().__init__(message)


class CdoRunner:
    """
    Low-level CDO command executor.

    Wraps ``subprocess.run`` to invoke the CDO binary with proper
    environment setup and error handling.
    """

    def __init__(self, cdo_path: str, env: Optional[dict] = None, debug: bool = False):
        """
        Parameters
        ----------
        cdo_path : str
            Absolute path to the CDO executable.
        env : dict, optional
            Environment variables for the CDO process.
        debug : bool
            If True, print commands and stderr to stdout.
        """
        self.cdo_path = cdo_path
        self.env = env or os.environ.copy()
        self.debug = debug

    def run(
        self,
        args: List[str],
        input_files: Optional[List[str]] = None,
        output_file: Optional[str] = None,
        options: Optional[List[str]] = None,
        return_output: bool = False,
        timeout: Optional[int] = None,
    ) -> Union[str, int]:
        """
        Execute a CDO command.

        Parameters
        ----------
        args : list of str
            CDO operator and its arguments, e.g. ["-mergetime"].
        input_files : list of str, optional
            Input file paths.
        output_file : str, optional
            Output file path.
        options : list of str, optional
            Global CDO options like ["-O", "-s"].
        return_output : bool
            If True, return stdout content instead of returncode.
        timeout : int, optional
            Timeout in seconds.

        Returns
        -------
        str or int
            stdout content if ``return_output`` is True, else returncode.

        Raises
        ------
        CdoError
            If CDO exits with a non-zero return code.
        """
        cmd = [self.cdo_path]

        if options:
            cmd.extend(options)

        cmd.extend(args)

        if input_files:
            cmd.extend(input_files)

        if output_file:
            cmd.append(output_file)

        if self.debug:
            print(f"[skyborn-cdo] Running: {' '.join(cmd)}")

        try:
            result = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                env=self.env,
                timeout=timeout,
                stdin=subprocess.DEVNULL,
            )
        except subprocess.TimeoutExpired as e:
            raise CdoError(
                f"CDO command timed out after {timeout}s: {' '.join(cmd)}",
                returncode=-1,
                stderr=str(e),
                cmd=" ".join(cmd),
            )
        except FileNotFoundError:
            raise CdoError(
                f"CDO binary not found at: {self.cdo_path}",
                returncode=-1,
                cmd=" ".join(cmd),
            )

        if self.debug and result.stderr:
            print(f"[skyborn-cdo] stderr: {result.stderr}")

        if result.returncode != 0:
            raise CdoError(
                f"CDO command failed (exit code {result.returncode}):\n"
                f"  Command: {' '.join(cmd)}\n"
                f"  Error: {result.stderr.strip()}",
                returncode=result.returncode,
                stderr=result.stderr,
                cmd=" ".join(cmd),
            )

        if return_output:
            return result.stdout

        return result.returncode

    def run_raw(self, cmd_string: str, timeout: Optional[int] = None) -> subprocess.CompletedProcess:
        """
        Execute a raw CDO command string.

        The command string is parsed with ``shlex.split`` and the first
        token (``cdo``) is replaced with the actual CDO binary path.

        Parameters
        ----------
        cmd_string : str
            Full CDO command string, e.g. "cdo mergetime in1.nc in2.nc out.nc"
            or "cdo -O -s mergetime in*.nc out.nc". The leading ``cdo`` is optional.

        Returns
        -------
        subprocess.CompletedProcess
            The completed process object.

        Raises
        ------
        CdoError
            If CDO exits with non-zero return code.
        """
        if os.name == 'nt':
            parts = shlex.split(cmd_string, posix=False)
            parts = [p.strip('"').strip("'") for p in parts]
        else:
            parts = shlex.split(cmd_string)

        # Strip leading 'cdo' if present
        if parts and parts[0].lower() in ("cdo", "cdo.exe"):
            parts = parts[1:]

        cmd = [self.cdo_path] + parts

        if self.debug:
            print(f"[skyborn-cdo] Running: {' '.join(cmd)}")

        try:
            result = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                env=self.env,
                timeout=timeout,
                stdin=subprocess.DEVNULL,
            )
        except subprocess.TimeoutExpired as e:
            raise CdoError(
                f"CDO command timed out after {timeout}s: {cmd_string}",
                returncode=-1,
                stderr=str(e),
                cmd=cmd_string,
            )

        if result.returncode != 0:
            raise CdoError(
                f"CDO command failed (exit code {result.returncode}):\n"
                f"  Command: {cmd_string}\n"
                f"  Error: {result.stderr.strip()}",
                returncode=result.returncode,
                stderr=result.stderr,
                cmd=cmd_string,
            )

        return result
