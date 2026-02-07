"""
Tests for skyborn_cdo Python API.
"""

import os
import subprocess
import sys
import tempfile

import pytest


class TestCdoBinaryDiscovery:
    """Test CDO binary discovery mechanisms."""

    def test_get_cdo_path_bundled(self):
        """Test that get_cdo_path finds bundled binary (if available)."""
        from skyborn_cdo._cdo_binary import _package_bin_dir, get_cdo_path

        bin_dir = _package_bin_dir()
        if (bin_dir / "cdo").exists() or (bin_dir / "cdo.exe").exists():
            path = get_cdo_path()
            assert os.path.isfile(path)
            assert "skyborn_cdo" in path
        else:
            pytest.skip("No bundled CDO binary present (development mode)")

    def test_get_cdo_path_env_var(self, tmp_path):
        """Test that CDO env var is respected."""
        from skyborn_cdo._cdo_binary import get_cdo_path

        fake_cdo = tmp_path / "cdo"
        fake_cdo.write_text("#!/bin/sh\necho fake")
        fake_cdo.chmod(0o755)

        original_env = os.environ.get("CDO")
        try:
            os.environ["CDO"] = str(fake_cdo)
            path = get_cdo_path()
            assert path == str(fake_cdo)
        finally:
            if original_env:
                os.environ["CDO"] = original_env
            else:
                os.environ.pop("CDO", None)

    def test_get_cdo_path_explicit(self, tmp_path):
        """Test that explicit path argument takes priority."""
        from skyborn_cdo._cdo_binary import get_cdo_path

        fake_cdo = tmp_path / "my_cdo"
        fake_cdo.write_text("#!/bin/sh\necho fake")
        fake_cdo.chmod(0o755)

        path = get_cdo_path(str(fake_cdo))
        assert path == str(fake_cdo)

    def test_get_cdo_path_not_found(self):
        """Test FileNotFoundError when CDO not available anywhere."""
        from skyborn_cdo._cdo_binary import get_cdo_path

        original_env = os.environ.get("CDO")
        original_path = os.environ.get("PATH")
        try:
            os.environ.pop("CDO", None)
            os.environ["PATH"] = ""  # Empty path
            with pytest.raises(FileNotFoundError, match="CDO binary not found"):
                get_cdo_path("/nonexistent/cdo")
        finally:
            if original_env:
                os.environ["CDO"] = original_env
            if original_path:
                os.environ["PATH"] = original_path

    def test_bundled_env(self):
        """Test that get_bundled_env returns proper environment."""
        from skyborn_cdo._cdo_binary import get_bundled_env

        env = get_bundled_env()
        assert isinstance(env, dict)
        assert "PATH" in env


class TestCdoRunner:
    """Test low-level CdoRunner."""

    def test_runner_invalid_binary(self):
        """Test CdoError on invalid binary path."""
        from skyborn_cdo._runner import CdoError, CdoRunner

        runner = CdoRunner("/nonexistent/cdo")
        with pytest.raises(CdoError, match="CDO binary not found"):
            runner.run(["--version"])


class TestCdoClass:
    """Test the high-level Cdo class."""

    @pytest.fixture
    def cdo(self):
        """Create a Cdo instance if CDO is available."""
        from skyborn_cdo import Cdo

        try:
            return Cdo()
        except FileNotFoundError:
            pytest.skip("CDO binary not available")

    def test_version(self, cdo):
        """Test that version() returns a string."""
        version = cdo.version()
        assert isinstance(version, str)
        assert "Climate Data Operators" in version or "CDO" in version

    def test_repr(self, cdo):
        """Test repr."""
        r = repr(cdo)
        assert "Cdo" in r
        assert "cdo_path" in r

    def test_call_version(self, cdo):
        """Test command-line style: cdo("--version")."""
        result = cdo("--version")
        assert isinstance(result, (str, int))

    def test_operators_list(self, cdo):
        """Test fetching operator list."""
        ops = cdo.operators()
        assert isinstance(ops, set)
        if ops:  # May be empty if CDO has issues
            assert "info" in ops or "copy" in ops
            assert "mergetime" in ops

    def test_has_operator(self, cdo):
        """Test operator existence check."""
        assert cdo.has_operator("copy") or cdo.has_operator("info")
        assert not cdo.has_operator("nonexistent_operator_xyz")

    def test_getattr_raises_for_private(self, cdo):
        """Test that private attributes raise AttributeError."""
        with pytest.raises(AttributeError):
            cdo._nonexistent

    def test_call_strip_cdo_prefix(self, cdo):
        """Test that 'cdo' prefix is stripped from command string."""
        # "cdo --version" and "--version" should both work
        from skyborn_cdo._runner import CdoRunner
        result = cdo("cdo --version")
        assert isinstance(result, (str, int))

    def test_cleanup(self, cdo):
        """Test cleanup removes temp files."""
        cdo._tempfiles.append("/tmp/nonexistent_test_file.nc")
        cdo.cleanup()
        assert len(cdo._tempfiles) == 0


class TestCdoOperations:
    """Integration tests that require a working CDO and test NC files."""

    @pytest.fixture
    def cdo(self):
        from skyborn_cdo import Cdo

        try:
            c = Cdo()
            c.version()  # Verify it works
            return c
        except (FileNotFoundError, Exception):
            pytest.skip("CDO binary not available or not functional")

    @pytest.fixture
    def sample_nc(self, cdo, tmp_path):
        """Create a minimal test NetCDF file using CDO."""
        outfile = str(tmp_path / "test.nc")
        try:
            # Create a simple grid with constant field using CDO's topo operator
            cdo.topo(output=outfile)
            return outfile
        except Exception:
            pytest.skip(
                "Could not create test NC file (NetCDF support may be missing)")

    def test_info(self, cdo, sample_nc):
        """Test cdo.info on a file."""
        result = cdo.info(input=sample_nc)
        assert isinstance(result, str)
        assert len(result) > 0

    def test_sinfo(self, cdo, sample_nc):
        """Test cdo.sinfo on a file."""
        result = cdo.sinfo(input=sample_nc)
        assert isinstance(result, str)

    def test_copy(self, cdo, sample_nc, tmp_path):
        """Test cdo.copy."""
        outfile = str(tmp_path / "copy_out.nc")
        cdo.copy(input=sample_nc, output=outfile)
        assert os.path.exists(outfile)
        assert os.path.getsize(outfile) > 0

    def test_call_style(self, cdo, sample_nc, tmp_path):
        """Test command-line style invocation."""
        outfile = str(tmp_path / "call_out.nc")
        cdo(f"cdo copy {sample_nc} {outfile}")
        assert os.path.exists(outfile)

    def test_options(self, sample_nc, tmp_path):
        """Test Cdo with default options."""
        from skyborn_cdo import Cdo

        try:
            cdo = Cdo(options="-O -s")
        except FileNotFoundError:
            pytest.skip("CDO not available")

        outfile = str(tmp_path / "opts_out.nc")
        cdo.copy(input=sample_nc, output=outfile)
        assert os.path.exists(outfile)

    def test_showname(self, cdo, sample_nc):
        """Test info operator that returns names."""
        result = cdo.showname(input=sample_nc)
        assert isinstance(result, str)


class TestCli:
    """Test CLI entry point."""

    def test_cli_info(self):
        """Test skyborn-cdo --info."""
        result = subprocess.run(
            [sys.executable, "-m", "skyborn_cdo._cli", "--info"],
            capture_output=True,
            text=True,
        )
        assert "skyborn-cdo version" in result.stdout

    def test_cli_help(self):
        """Test skyborn-cdo --help."""
        result = subprocess.run(
            [sys.executable, "-m", "skyborn_cdo._cli", "--help"],
            capture_output=True,
            text=True,
        )
        assert "skyborn-cdo" in result.stdout
        assert "Python API" in result.stdout
