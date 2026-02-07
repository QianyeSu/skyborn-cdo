# skyborn-cdo

Pre-compiled [CDO (Climate Data Operators)](https://code.mpimet.mpg.de/projects/cdo) distributed as a pip-installable Python package.

This is a backend module for [Skyborn](https://github.com/QianyeSu/Skyborn) — an atmospheric science research toolkit.

## Installation

```bash
pip install skyborn-cdo
```

This installs a pre-compiled CDO binary along with all required libraries (NetCDF, HDF5, ecCodes, FFTW3, PROJ, UDUNITS2). **No system-level CDO installation needed.**

### Supported platforms

| Platform | Architecture | Status |
|----------|-------------|--------|
| Linux    | x86_64      | ✅ Supported |
| macOS    | arm64 (Apple Silicon) | ✅ Supported |
| Windows  | x86_64      | ⚠️ Experimental |

## Usage

### Command-line style

```python
from skyborn_cdo import Cdo

cdo = Cdo()
cdo("cdo mergetime in1.nc in2.nc out.nc")
cdo("cdo -O -f nc4 sellonlatbox,0,30,0,30 input.nc output.nc")
```

### Method-call style

```python
from skyborn_cdo import Cdo

cdo = Cdo()
cdo.mergetime(input="in1.nc in2.nc", output="out.nc")
cdo.sellonlatbox("0,30,0,30", input="input.nc", output="output.nc")
info = cdo.info(input="input.nc")
print(info)
```

### With options

```python
cdo = Cdo(options="-O -s")  # Overwrite + silent
cdo.copy(input="in.nc", output="out.nc")
```

### Return as xarray Dataset

```python
# Requires: pip install skyborn-cdo[xarray]
ds = cdo.sellonlatbox("0,30,0,30", input="input.nc", returnXArray=True)
print(ds)
```

### Integration with Skyborn

```python
# In Skyborn main package:
from skyborn_cdo import Cdo
cdo = Cdo()
cdo.mergetime(input="*.nc", output="all_time.nc")
```

## CLI

The package also provides a command-line tool:

```bash
# Show installation info
skyborn-cdo --info

# Pass-through to CDO
skyborn-cdo -f nc4 copy input.nc output.nc
```

## CDO Version

This package bundles **CDO 2.5.3** with the following libraries:
- NetCDF-C 4.9.2
- HDF5 1.14.4
- ecCodes 2.38.0
- FFTW3 3.3.10
- PROJ 9.5.1
- UDUNITS2 2.2.28

## Development

```bash
git clone --recurse-submodules https://github.com/QianyeSu/skyborn-cdo.git
cd skyborn-cdo
pip install -e ".[test]"
pytest tests/
```

To build CDO from source locally:
```bash
SKYBORN_CDO_BUILD=1 pip install -e .
```

## License

This Python wrapper is licensed under **BSD-3-Clause**.

CDO itself is licensed under **BSD-3-Clause** by MPI für Meteorologie.
See [LICENSE](LICENSE) for details.
