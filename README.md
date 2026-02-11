# skyborn-cdo

Pre-compiled [CDO (Climate Data Operators)](https://code.mpimet.mpg.de/projects/cdo) distributed as a pip-installable Python package.

This is a backend module for [Skyborn](https://github.com/QianyeSu/Skyborn) — an atmospheric science research toolkit.

### About CDO 2.5.3

This package bundles **CDO 2.5.3** with all required dependencies. For detailed CDO documentation and release information, see:

- **CDO Official Project**: https://code.mpimet.mpg.de/projects/cdo


## Installation

```bash
pip install skyborn-cdo
```

This installs a pre-compiled CDO binary along with all required libraries (NetCDF, HDF5, ecCodes, FFTW3, PROJ, UDUNITS2). **No system-level CDO installation needed.**

### Optional dependencies

```bash
# xarray support (returnXArray)
pip install skyborn-cdo[xarray]

# Test dependencies
pip install skyborn-cdo[test]
```

### Supported platforms

| Platform | Architecture | Status |
|----------|-------------|--------|
| Linux    | x86_64      | ✅ Supported |
| macOS    | arm64 (Apple Silicon) | ✅ Supported |
| Windows  | x86_64      | ✅ Supported |

## Quick Start

```python
from skyborn_cdo import Cdo

cdo = Cdo()
print(cdo.version())  # CDO 2.5.3
```

## Usage

skyborn-cdo provides two API styles — **command-line style** and **method-call style** — both covering all 900+ CDO operators.

### 1. Command-line Style — `cdo()`

Pass a full CDO command string, just like typing in a terminal. The leading `cdo` prefix is optional.

```python
from skyborn_cdo import Cdo

cdo = Cdo()

# With 'cdo' prefix (copy/paste from terminal)
cdo("cdo -O mergetime in1.nc in2.nc out.nc")

# Without 'cdo' prefix (also valid)
cdo("mergetime in1.nc in2.nc out.nc")

# Complex command with options
cdo("-O -f nc4 sellonlatbox,0,30,0,30 input.nc output.nc")

# Chained operators (CDO pipe syntax)
cdo("-O -f nc4 -fldmean -sellonlatbox,70,140,10,55 input.nc output.nc")
cdo("-O -f nc4 -sp2gpl -setgridtype,regular input.nc output.nc")
```

### 2. Method-call Style — `cdo.operator()`

Each of CDO's 900+ operators is available as a Python method via dynamic dispatch.

```python
from skyborn_cdo import Cdo

cdo = Cdo()

# Basic: operator(parameters, input=..., output=...)
cdo.sellonlatbox("0,30,0,30", input="input.nc", output="output.nc")

# No parameters needed
cdo.copy(input="input.nc", output="output.nc")
cdo.topo(output="topo.nc")

# Multiple input files (space-separated string or list)
cdo.mergetime(input="in1.nc in2.nc in3.nc", output="merged.nc")
cdo.mergetime(input=["in1.nc", "in2.nc", "in3.nc"], output="merged.nc")

# Wildcard / glob patterns (automatically expanded)
cdo.mergetime(input="data_2020*.nc", output="merged.nc")
cdo.mergetime(input="/path/to/data_20200?.nc", output="merged.nc")
cdo.ensmean(input="ensemble_*.nc", output="ensmean.nc")
```

> **Wildcards**: Both `cdo.operator(input="*.nc")` and `cdo("mergetime *.nc out.nc")` support glob patterns (`*`, `?`, `[...]`). Files are sorted alphabetically before being passed to CDO.

### 3. Options

CDO options like `-O` (overwrite), `-s` (silent), `-f nc4` (output format) can be set globally or per-call.

```python
# Global options — applied to every command
cdo = Cdo(options="-O -s")
cdo.copy(input="in.nc", output="out.nc")

# Per-call options — merged with global options
cdo = Cdo()
cdo.copy(input="in.nc", output="out.nc", options="-O -f nc4")

# Common options:
#   -O          Overwrite existing output files
#   -s          Silent mode (suppress informational messages)
#   -f nc4      Output in NetCDF4 format
#   -f nc4c     Output in NetCDF4 classic format
#   -f grb2     Output in GRIB2 format
#   -P 4        Use 4 threads for parallel processing
```

> **⚠️ Important**: CDO **does not overwrite** existing output files by default.
> If the output file already exists, CDO will raise an error:
> `Outputfile out.nc already exists!`
> Use `-O` to enable overwriting: `cdo = Cdo(options="-O")` or pass `options="-O"` per call.

### 4. Info Operators (return text output)

Operators like `info`, `sinfo`, `griddes`, `showname`, etc. return their text output as a string instead of writing to a file.

```python
cdo = Cdo()

# File information
info = cdo.sinfo(input="input.nc")
print(info)

# Grid description
grid = cdo.griddes(input="input.nc")
print(grid)

# Show variable names
names = cdo.showname(input="input.nc")
print(names)

# Show timestamps
dates = cdo.showdate(input="input.nc")
print(dates)

# Number of time steps / levels / variables
print(cdo.ntime(input="input.nc"))
print(cdo.nlevel(input="input.nc"))
print(cdo.nvar(input="input.nc"))
```

### 5. Return as xarray / netCDF4 / numpy

```python
# Return as xarray.Dataset (requires: pip install skyborn-cdo[xarray])
ds = cdo.sellonlatbox("0,30,0,30", input="input.nc", returnXArray=True)
print(ds)

# Return as netCDF4.Dataset (requires: pip install netCDF4)
nc = cdo.copy(input="input.nc", returnCdf=True)
print(nc.variables.keys())

# Return as numpy array (first variable)
arr = cdo.copy(input="input.nc", returnArray=True)
print(arr.shape)

# Return as masked numpy array
marr = cdo.copy(input="input.nc", returnMaArray=True)
```

### 6. Chained Operators (Pipeline)

CDO supports nesting operators in a single command. This is the most powerful feature for building complex processing pipelines.

#### Command-line style (recommended for complex chains)

```python
cdo = Cdo(options="-O")

# Remapping → Spatial selection → Field mean
cdo("-f nc4 -fldmean -sellonlatbox,70,140,10,55 -remapbil,r180x90 input.nc output.nc")

# Spectral mode conversion pipeline
cdo("-f nc4 -sp2gpl -setgridtype,regular input_spectral.nc output_regular.nc")

# Arithmetic pipeline
cdo("-addc,273.15 -mulc,0.01 input.nc output.nc")

# Conditional masking
cdo("-ifthen -gtc,0 topo.nc topo.nc positive_topo.nc")
```

#### Method-call style (using input as sub-command)

```python
cdo = Cdo(options="-O")

# Use CDO operator syntax in the input parameter for chaining
cdo.remapbil("r360x180", input="-mergetime in1.nc in2.nc in3.nc", output="out.nc")
cdo.fldmean(input="-sellonlatbox,70,140,10,55 input.nc", output="mean.nc")
cdo.timmean(input="-sellonlatbox,0,360,-30,30 -remapbil,r180x90 input.nc", output="out.nc")
```

### 7. Common Operator Examples

#### Spatial Operations

```python
cdo = Cdo(options="-O")

# Select region by longitude/latitude box
cdo.sellonlatbox("70,140,10,55", input="global.nc", output="china.nc")

# Select by grid index box
cdo.selindexbox("1,100,1,80", input="input.nc", output="subset.nc")

# Remap to regular grid
cdo.remapbil("r360x180", input="input.nc", output="1deg.nc")     # Bilinear
cdo.remapcon("r360x180", input="input.nc", output="1deg.nc")     # Conservative
cdo.remapnn("r360x180", input="input.nc", output="1deg.nc")      # Nearest neighbor
cdo.remapdis("r360x180", input="input.nc", output="1deg.nc")     # Distance weighted

# Zonal / Meridional mean
cdo.zonmean(input="input.nc", output="zonmean.nc")
cdo.mermean(input="input.nc", output="mermean.nc")

# Invert latitude direction
cdo.invertlat(input="input.nc", output="flipped.nc")
```

#### Time Operations

```python
cdo = Cdo(options="-O")

# Time averaging
cdo.timmean(input="input.nc", output="time_avg.nc")
cdo.timstd(input="input.nc", output="time_std.nc")
cdo.timmin(input="input.nc", output="time_min.nc")
cdo.timmax(input="input.nc", output="time_max.nc")

# Monthly / Seasonal / Yearly statistics
cdo.monmean(input="input.nc", output="monthly_mean.nc")
cdo.seasmean(input="input.nc", output="seasonal_mean.nc")
cdo.yearmonmean(input="input.nc", output="yearly_mean.nc")

# Select time steps
cdo.selyear("2020", input="input.nc", output="year2020.nc")
cdo.selmon("1,2,3", input="input.nc", output="jan_mar.nc")
cdo.seltimestep("1/10", input="input.nc", output="first10.nc")

# Set time axis
cdo.settaxis("2020-01-15,12:00,1mon", input="input.nc", output="redate.nc")

# Merge time series
cdo.mergetime(input="jan.nc feb.nc mar.nc", output="q1.nc")

# Split by month / year
cdo.splitmon(input="input.nc", output="monthly_")
cdo.splityear(input="input.nc", output="yearly_")

# Detrend
cdo.detrend(input="input.nc", output="detrended.nc")
```

#### Field Statistics

```python
cdo = Cdo(options="-O")

# Field (spatial) statistics
cdo.fldmean(input="input.nc", output="fldmean.nc")
cdo.fldstd(input="input.nc", output="fldstd.nc")
cdo.fldmin(input="input.nc", output="fldmin.nc")
cdo.fldmax(input="input.nc", output="fldmax.nc")
cdo.fldsum(input="input.nc", output="fldsum.nc")
```

#### Arithmetic

```python
cdo = Cdo(options="-O")

# Scalar operations
cdo.mulc("2.0", input="input.nc", output="doubled.nc")
cdo.addc("273.15", input="celsius.nc", output="kelvin.nc")
cdo.divc("100", input="input.nc", output="divided.nc")
cdo.subc("273.15", input="kelvin.nc", output="celsius.nc")

# Unary operations
cdo.abs(input="input.nc", output="absolute.nc")
cdo.sqrt(input="positive.nc", output="sqrt.nc")

# Two-file operations
cdo.add(input="a.nc b.nc", output="sum.nc")
cdo.sub(input="a.nc b.nc", output="diff.nc")
cdo.mul(input="a.nc b.nc", output="product.nc")
cdo.div(input="a.nc b.nc", output="ratio.nc")

# Custom expression
cdo.expr("'new_var=temp*2+precip;'", input="input.nc", output="computed.nc")
```

#### Vertical Level Operations

```python
cdo = Cdo(options="-O")

# Select specific levels
cdo.sellevel("85000,50000,20000", input="input.nc", output="levels.nc")

# Interpolate to new levels
cdo.intlevel("90000,85000,70000,50000,30000", input="input.nc", output="interp.nc")

# Show levels
print(cdo.showlevel(input="input.nc"))
```

#### Grid Type Conversion & Spectral Transforms

```python
cdo = Cdo(options="-O")

# Convert grid type
cdo.setgridtype("regular", input="input.nc", output="regular.nc")

# Spectral ↔ Grid-point transforms
cdo.sp2gpl(input="spectral.nc", output="gaussian_linear.nc")
cdo.sp2gp(input="spectral.nc", output="gaussian.nc")
cdo.gp2sp(input="gaussian.nc", output="spectral.nc")

# Complex chain: spectral to regular grid in NetCDF4
cdo("-O -f nc4 -sp2gpl -setgridtype,regular spectral.nc regular.nc")
```

#### Format Conversion

```python
cdo = Cdo(options="-O")

# NetCDF formats
cdo.copy(input="input.nc", output="out.nc", options="-f nc4")     # NetCDF4
cdo.copy(input="input.nc", output="out.nc", options="-f nc4c")    # NetCDF4 Classic
cdo.copy(input="input.nc", output="out.nc", options="-f nc2")     # NetCDF 64-bit

# GRIB formats
cdo.copy(input="input.nc", output="out.grb", options="-f grb")    # GRIB1
cdo.copy(input="input.nc", output="out.grb2", options="-f grb2")  # GRIB2
```

#### Variable Metadata

```python
cdo = Cdo(options="-O")

# Rename variable
cdo.chname("old_name,new_name", input="input.nc", output="renamed.nc")

# Set attributes
cdo.setattribute("varname@units=kg/m2", input="input.nc", output="units.nc")

# Set missing value
cdo.setmissval("-999", input="input.nc", output="newmiss.nc")
```

#### Ensemble Operations

```python
cdo = Cdo(options="-O")

# Ensemble mean (multiple realizations)
cdo.ensmean(input="run_*.nc", output="ensemble_mean.nc")

# Ensemble standard deviation
cdo.ensstd(input="run_*.nc", output="ensemble_std.nc")

# Ensemble variance
cdo.ensvar(input="run_001.nc run_002.nc run_003.nc", output="ensemble_var.nc")

# Ensemble sum
cdo.enssum(input=["member_1.nc", "member_2.nc", "member_3.nc"], output="ens_sum.nc")

# Ensemble percentiles
cdo.enspctl("25,50,75", input="run_*.nc", output="ensemble_quartiles.nc")
```

#### Grid Information and Modification

```python
cdo = Cdo(options="-O")

# Calculate grid cell areas
cdo.gridarea(input="input.nc", output="grid_areas.nc")

# Calculate grid weights (for weighted averaging)
cdo.gridweights(input="input.nc", output="weights.nc")

# Set grid type
cdo.setgridtype("regular", input="curvilinear.nc", output="regular.nc")

# Show grid description
print(cdo.griddes(input="input.nc"))

# Invert latitude direction (flip N-S)
cdo.invertlat(input="input.nc", output="flipped.nc")
```

#### Advanced Statistical Operations

```python
cdo = Cdo(options="-O")

# Field percentiles
cdo.fldpctl("10,50,90", input="input.nc", output="field_percentiles.nc")

# Field range (max - min)
cdo.fldrange(input="input.nc", output="field_range.nc")

# Time series percentiles
cdo.timpctl("25,50,75", input="timeseries.nc", output="time_percentiles.nc")

# Running mean (e.g., 5-timestep window)
cdo.runmean("5", input="input.nc", output="smoothed.nc")

# Trend removal
cdo.detrend(input="input.nc", output="detrended.nc")
```

### 8. Error Handling

skyborn-cdo captures all CDO errors and raises them as `CdoError` exceptions with full diagnostic information.

```python
from skyborn_cdo import Cdo, CdoError

cdo = Cdo()

try:
    cdo.sellonlatbox("0,30,0,30", input="nonexistent.nc", output="out.nc")
except CdoError as e:
    print(f"Error: {e}")
    print(f"Return code: {e.returncode}")
    print(f"CDO stderr: {e.stderr}")
    print(f"Command: {e.cmd}")
```

Common errors:
- **File not found**: `Open failed on >file.nc< No such file or directory`
- **Output exists**: `Outputfile out.nc already exists!` → Add `-O` option
- **Invalid parameters**: `Float parameter >abc< contains invalid character`
- **Timeout**: `CDO command timed out after 60s` → Increase timeout

### 9. Timeout

```python
# Global timeout for all commands (seconds)
cdo = Cdo(timeout=300)

# Per-call timeout override
cdo.remapcon("r3600x1800", input="input.nc", output="hires.nc", timeout=600)
cdo("cdo -O remapcon,r3600x1800 input.nc hires.nc", timeout=600)
```

### 10. Debug Mode

```python
# Print executed commands and CDO stderr output
cdo = Cdo(debug=True)
cdo.sellonlatbox("0,30,0,30", input="input.nc", output="output.nc")
# [skyborn-cdo] Running: /path/to/cdo -sellonlatbox,0,30,0,30 input.nc output.nc
# [skyborn-cdo] stderr: cdo    sellonlatbox: ...
```

### 11. Getting Help

#### Python API — `cdo.help()`

```python
from skyborn_cdo import Cdo

cdo = Cdo()

# Help for a specific operator
print(cdo.help("sellonlatbox"))
print(cdo.help("mergetime"))
print(cdo.help("remapbil"))

# General usage summary
print(cdo.help())

# List all available operators
print(cdo.operators())

# Check if an operator exists
cdo.has_operator("sellonlatbox")  # True
```

#### CLI — `skyborn-cdo -h` / `--help`

```bash
# Show general help
skyborn-cdo --help

# Show help for a specific CDO operator (passed to CDO directly)
skyborn-cdo -h sellonlatbox
skyborn-cdo -h mergetime
skyborn-cdo -h remapbil

# List all operators
skyborn-cdo --operators

# Also works via python -m
python -m skyborn_cdo --help
python -m skyborn_cdo -h sellonlatbox
```

## CLI

The package provides a `skyborn-cdo` command-line tool (also available as `python -m skyborn_cdo`):

```bash
# Show installation info and CDO version
skyborn-cdo --info

# Show help
skyborn-cdo --help

# Operator-level help (forwarded to CDO)
skyborn-cdo -h sellonlatbox

# List all operators
skyborn-cdo --operators

# Pass-through to CDO (any CDO command works)
skyborn-cdo -O -f nc4 copy input.nc output.nc
skyborn-cdo mergetime in1.nc in2.nc out.nc
skyborn-cdo -O -f nc4 -sp2gpl -setgridtype,regular spectral.nc regular.nc
```

## CDO Operators

skyborn-cdo provides access to **938 CDO operators** covering all aspects of climate data processing. Check operator availability:

```python
cdo = Cdo()
print(len(cdo.operators()))  # 938
cdo.has_operator("sellonlatbox")  # True
print(cdo.help("sellonlatbox"))  # Get operator documentation
```

### Operator Categories

| Category | Count | Examples | Use Cases |
|----------|-------|----------|-----------|
| **Time Operations** | 208 | `timmean`, `timstd`, `mergetime`, `seldate`, `monmean`, `yearsum` | Time series analysis, temporal statistics, date/time selection |
| **Statistical** | 222 | `fldmean`, `fldstd`, `ensmean`, `timavg`, `zonmean`, `mermean` | Spatial/temporal statistics, ensemble analysis |
| **Spatial Selection** | 35 | `sellonlatbox`, `selindexbox`, `selgrid`, `sellevel`, `selcode` | Regional extraction, level selection |
| **Grid/Remapping** | 64 | `remapbil`, `remapcon`, `remapnn`, `setgrid`, `gridarea` | Grid conversion, interpolation, regridding |
| **Arithmetic** | 60 | `add`, `mul`, `expr`, `addc`, `sqrt`, `log` | Mathematical operations, calculations |
| **Vertical Levels** | 13 | `ml2pl`, `intlevel`, `pressure`, `sealevelpressure` | Model level conversion, vertical interpolation |
| **Format/IO** | 47 | `copy`, `merge`, `split`, `cat`, `import_*`, `output*` | File operations, format conversion |
| **Info/Query** | 50 | `info`, `showname`, `griddes`, `ntime`, `showdate` | File inspection, metadata extraction |
| **Spectral/Grid Transform** | 28 | `sp2gp`, `gp2sp`, `sp2gpl`, `fourier` | Spectral transforms, Fourier analysis |
| **Others** | 211 | Specialized operators for specific domains | Advanced climate computations |

**Total: 938 operators**

### Are All Operators Useful?

**Commonly used** (daily work): ~80-100 operators
- Space: `sellonlatbox`, `remapbil`, `remapcon`, `zonmean`, `mermean`
- Time: `mergetime`, `timmean`, `monmean`, `yearsum`, `seldate`, `selyear`
- Statistics: `fldmean`, `fldstd`, `fldmin`, `fldmax`, `ensmean`
- Arithmetic: `add`, `sub`, `mul`, `div`, `addc`, `mulc`, `expr`
- Info: `sinfo`, `showname`, `griddes`, `ntime`

**Specialized operators** (400+): Domain-specific
- Meteorology: `sealevelpressure`, `ml2pl`, `geopotheight`
- Oceanography: `detrend`, `dmean`, `seasmean`
- Climate indices: `eca_*` (extreme climate events), `ydrun*` (running means)
- Statistical analysis: `trend`, `regres`, `corr`, `eof`
- Spectral analysis: `sp2gp`, `gp2sp`, `dft`, `filter`

**Legacy/Niche operators** (400+): Less frequently used but available
- Format-specific imports (`import_cmsaf`, `import_grads`)
- Experimental features (`remapavgtest`, `remapcon2test`)
- Highly specialized computations

**How to decide if you need an operator:**
```python
# Search operators by keyword
cdo = Cdo()
time_ops = [op for op in cdo.operators() if 'time' in op]
print(f"Time-related: {len(time_ops)} operators")  # 208

# Get detailed help for any operator
print(cdo.help("operator_name"))
```

## CDO Version

This package bundles **CDO 2.5.3** with the following libraries:
- NetCDF-C 4.9.x
- HDF5 1.14.x
- ecCodes 2.40+
- FFTW3 3.3.10
- PROJ 9.5.x
- UDUNITS2 2.2.28

Exact library versions vary by platform (Linux/macOS build from source, Windows uses MSYS2 packages).

## API Reference

### `Cdo` class

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `cdo_path` | `str` | `None` | Path to CDO binary (auto-discovered if None) |
| `options` | `str` | `""` | Default CDO options (e.g. `"-O -s -f nc4"`) |
| `env` | `dict` | `None` | Custom environment variables |
| `debug` | `bool` | `False` | Print commands and stderr |
| `timeout` | `int` | `None` | Default timeout in seconds |

### Operator method parameters

| Parameter | Type | Description |
|-----------|------|-------------|
| First positional arg | `str` | Operator parameters (e.g. `"0,30,0,30"`) |
| `input` | `str` or `list` | Input file(s) |
| `output` | `str` | Output file path |
| `options` | `str` | Additional CDO options for this call |
| `timeout` | `int` | Timeout override |
| `returnXArray` | `bool` | Return as xarray.Dataset |
| `returnCdf` | `bool` | Return as netCDF4.Dataset |
| `returnArray` | `bool` | Return as numpy.ndarray |
| `returnMaArray` | `bool` | Return as masked numpy.ndarray |

### Utility methods

| Method | Returns | Description |
|--------|---------|-------------|
| `cdo.help()` | `str` | General usage summary |
| `cdo.help("operator")` | `str` | CDO help text for a specific operator |
| `cdo.version()` | `str` | CDO version string |
| `cdo.operators()` | `set` | All available CDO operator names |
| `cdo.has_operator(name)` | `bool` | Check if an operator exists |
| `cdo.cleanup()` | — | Remove temporary files |

### `CdoError` exception

Import: `from skyborn_cdo import CdoError`

| Attribute | Type | Description |
|-----------|------|-------------|
| `returncode` | `int` | CDO exit code |
| `stderr` | `str` | Full CDO error output |
| `cmd` | `str` | Command that failed |

## Development

```bash
git clone --recurse-submodules https://github.com/QianyeSu/skyborn-cdo.git
cd skyborn-cdo
pip install -e ".[test]"
pytest tests/
```

## Author

**Qianye Su**  
Email: suqianye2000@gmail.com  
GitHub: [@QianyeSu](https://github.com/QianyeSu)

## License

This Python wrapper is licensed under **BSD-3-Clause**.

CDO itself is licensed under **BSD-3-Clause** by MPI für Meteorologie.
See [LICENSE](LICENSE) for details.
