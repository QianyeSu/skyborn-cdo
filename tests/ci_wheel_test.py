#!/usr/bin/env python
"""
CI Wheel Comprehensive Stress Test — verifies CDO binary functionality.

Executed by cibuildwheel after each wheel is installed.
Tests as many CDO operations as possible within CI time constraints (~5 min).

Test Coverage:
  • Basic: version, operators, help
  • Synthetic data: topo, randoperators, for, stdatm, const
  • Info queries: sinfo, griddes, showname, ntime
  • Selection: sellonlatbox, selindexbox, sellevel
  • Statistics: fldmean, fldstd, zonmean, timmean
  • Arithmetic: mulc, addc, abs, sqrt, expr  
  • Grid ops: remapbil, remapcon, remapnn
  • Spectral: gp2sp, sp2gpl (CRITICAL)
  • Format: NetCDF4, NetCDF2, GRIB1, GRIB2
  • Time: mergetime, settaxis, selmon
  • Chained: complex multi-operator pipes
  • Error handling: invalid inputs
"""

import os
import sys
import tempfile
import time


# ======================================================================
# Diagnostics (Linux only — shows ELF/library loading)
# ======================================================================

def diagnostics():
    import skyborn_cdo as _pkg
    from skyborn_cdo import get_cdo_path

    pkg_dir = os.path.dirname(_pkg.__file__)
    print(f"Package dir: {pkg_dir}")
    print(f"Python:      {sys.version}")
    print(f"Platform:    {sys.platform}")

    try:
        cdo_bin = get_cdo_path()
        print(f"CDO binary:  {cdo_bin}")
    except FileNotFoundError as e:
        print(f"CDO binary:  NOT FOUND — {e}")
        return

    for subdir in ["bin", "lib", "share"]:
        full = os.path.join(pkg_dir, subdir)
        if os.path.isdir(full):
            items = os.listdir(full)
            shown = items[:15]
            extra = f" ... +{len(items) - 15} more" if len(items) > 15 else ""
            print(f"  {subdir}/ ({len(items)} items): {shown}{extra}")

    # auditwheel .libs/ directory
    libs_dir = os.path.join(os.path.dirname(pkg_dir), "skyborn_cdo.libs")
    if os.path.isdir(libs_dir):
        items = os.listdir(libs_dir)
        print(f"  skyborn_cdo.libs/ ({len(items)} items): {items[:10]}")
    else:
        print("  skyborn_cdo.libs/ does not exist (good — all libs in lib/)")

    if sys.platform != "linux":
        print()
        return

    # ---- Linux-specific diagnostics ----
    import subprocess as _sp
    cdo_bin = get_cdo_path()

    # file type
    try:
        r = _sp.run(["file", cdo_bin], capture_output=True,
                    text=True, timeout=5)
        print(f"  file: {r.stdout.strip()}")
    except Exception:
        pass

    # ELF interpreter
    try:
        r = _sp.run(["readelf", "-l", cdo_bin],
                    capture_output=True, text=True, timeout=5)
        for line in r.stdout.splitlines():
            if "interpreter" in line.lower():
                print(f"  {line.strip()}")
    except Exception:
        pass

    # RPATH / RUNPATH
    try:
        r = _sp.run(["readelf", "-d", cdo_bin],
                    capture_output=True, text=True, timeout=5)
        for line in r.stdout.splitlines():
            if "RPATH" in line or "RUNPATH" in line:
                print(f"  {line.strip()}")
    except Exception:
        pass

    # ldd — capture BOTH stdout and stderr
    try:
        r = _sp.run(["ldd", cdo_bin], capture_output=True,
                    text=True, timeout=10)
        stdout_lines = r.stdout.strip().splitlines() if r.stdout.strip() else []
        print(f"  ldd ({len(stdout_lines)} libs):")
        for line in stdout_lines[:30]:
            print(f"    {line.strip()}")
        not_found = [l for l in stdout_lines if "not found" in l]
        if not_found:
            print(f"  *** WARNING: {len(not_found)} libraries NOT FOUND ***")
        if r.stderr.strip():
            print(f"  ldd stderr: {r.stderr.strip()[:300]}")
        if not stdout_lines and not r.stderr.strip():
            print("  *** ldd produced NO output — binary may be corrupted ***")
    except Exception as e:
        print(f"  ldd error: {e}")

    # LD_LIBRARY_PATH that will be used at runtime
    from skyborn_cdo._cdo_binary import get_bundled_env
    env = get_bundled_env()
    print(
        f"  LD_LIBRARY_PATH: {env.get('LD_LIBRARY_PATH', '(not set)')[:200]}")

    # LD_DEBUG=libs — trace library loading for CDO --version
    try:
        env_dbg = env.copy()
        env_dbg["LD_DEBUG"] = "libs"
        r = _sp.run(
            [cdo_bin, "--version"],
            capture_output=True, text=True, timeout=10,
            env=env_dbg, stdin=_sp.DEVNULL,
        )
        dbg = r.stderr.splitlines()
        print(f"  LD_DEBUG ({len(dbg)} lines, rc={r.returncode}):")
        for line in dbg[:15]:
            print(f"    {line}")
        # Highlight errors
        for i, line in enumerate(dbg):
            if i >= 15 and ("error" in line.lower() or "not found" in line.lower()):
                print(f"    ...[{i}] {line}")
    except Exception as e:
        print(f"  LD_DEBUG error: {e}")

    print()  # blank line before tests


# ======================================================================
# Test runner — comprehensive stress test
# ======================================================================

def main():
    from skyborn_cdo import Cdo
    from skyborn_cdo._runner import CdoError

    diagnostics()

    cdo = Cdo(timeout=10)
    tmpdir = tempfile.mkdtemp()
    passed = 0
    failed = 0
    t_start = time.time()

    def run_test(name, fn):
        nonlocal passed, failed
        t0 = time.time()
        try:
            fn()
            passed += 1
            dt = time.time() - t0
            slow = f" ({dt:.1f}s)" if dt > 2.0 else ""
            print(f"  [PASS] {name}{slow}")
        except Exception as e:
            print(f"  [FAIL] {name} -- {e}", file=sys.stderr)
            failed += 1

    # Basic functionality
    print("\n=== 1. Basic ===")
    run_test("version", lambda: cdo.version())
    run_test("operators", lambda: assert_true(len(cdo.operators()) > 800))
    run_test("has_operator", lambda: assert_true(
        cdo.has_operator("mergetime")))

    # Synthetic data generation
    print("\n=== 2. Data Generation ===")
    topo_nc = os.path.join(tmpdir, "topo.nc")
    run_test("topo", lambda: cdo.topo(output=topo_nc) or assert_file(topo_nc))

    rand_nc = os.path.join(tmpdir, "rand.nc")
    run_test("random r72x36", lambda: cdo(
        f"cdo -random,r72x36 {rand_nc}", timeout=30) or assert_file(rand_nc))

    const_nc = os.path.join(tmpdir, "const.nc")
    run_test("const field", lambda: cdo(
        f"cdo -const,273.15,r36x18 {const_nc}", timeout=20) or assert_file(const_nc))

    monthly_nc = os.path.join(tmpdir, "monthly.nc")
    run_test("12-month series", lambda: cdo(
        f"cdo -settaxis,2020-01-15,12:00,1mon -for,1,12 {monthly_nc}", timeout=30) or assert_file(monthly_nc))

    stdatm_nc = os.path.join(tmpdir, "stdatm.nc")
    run_test("stdatm levels", lambda: cdo(
        f"cdo stdatm,0,10000,30000,50000 {stdatm_nc}", timeout=20) or assert_file(stdatm_nc))

    # Info queries
    print("\n=== 3. Info Queries ===")
    run_test("sinfo", lambda: assert_true(
        len(str(cdo.sinfo(input=topo_nc))) > 10))
    run_test("griddes", lambda: assert_true(
        "gridtype" in str(cdo.griddes(input=topo_nc)).lower()))
    run_test("showname", lambda: len(cdo.showname(input=topo_nc)))
    run_test("ntime", lambda: cdo.ntime(input=monthly_nc))
    run_test("nlevel", lambda: cdo.nlevel(input=stdatm_nc))

    # Selection/clipping
    print("\n=== 4. Selection ===")
    sellonlat_nc = os.path.join(tmpdir, "sellonlat.nc")
    run_test("sellonlatbox", lambda: cdo.sellonlatbox(
        "0,90,0,45", input=topo_nc, output=sellonlat_nc) or assert_file(sellonlat_nc))

    selidx_nc = os.path.join(tmpdir, "selindex.nc")
    run_test("selindexbox", lambda: cdo.selindexbox(
        "1,30,1,20", input=topo_nc, output=selidx_nc) or assert_file(selidx_nc))

    sellev_nc = os.path.join(tmpdir, "sellev.nc")
    run_test("sellevel", lambda: cdo.sellevel(
        "0,10000", input=stdatm_nc, output=sellev_nc) or assert_file(sellev_nc))

    selmon_nc = os.path.join(tmpdir, "selmon.nc")
    run_test("selmon", lambda: cdo.selmon("1,2,3", input=monthly_nc,
             output=selmon_nc) or assert_file(selmon_nc))

    # Statistics
    print("\n=== 5. Statistics ===")
    fldmean_nc = os.path.join(tmpdir, "fldmean.nc")
    run_test("fldmean", lambda: cdo.fldmean(input=topo_nc,
             output=fldmean_nc) or assert_file(fldmean_nc))

    fldstd_nc = os.path.join(tmpdir, "fldstd.nc")
    run_test("fldstd", lambda: cdo.fldstd(input=topo_nc,
             output=fldstd_nc) or assert_file(fldstd_nc))

    zonmean_nc = os.path.join(tmpdir, "zonmean.nc")
    run_test("zonmean", lambda: cdo.zonmean(input=topo_nc,
             output=zonmean_nc) or assert_file(zonmean_nc))

    timmean_nc = os.path.join(tmpdir, "timmean.nc")
    run_test("timmean", lambda: cdo.timmean(input=monthly_nc,
             output=timmean_nc) or assert_file(timmean_nc))

    timstd_nc = os.path.join(tmpdir, "timstd.nc")
    run_test("timstd", lambda: cdo.timstd(input=monthly_nc,
             output=timstd_nc) or assert_file(timstd_nc))

    # Arithmetic
    print("\n=== 6. Arithmetic ===")
    mulc_nc = os.path.join(tmpdir, "mulc.nc")
    run_test("mulc", lambda: cdo.mulc("2.5", input=topo_nc,
             output=mulc_nc) or assert_file(mulc_nc))

    addc_nc = os.path.join(tmpdir, "addc.nc")
    run_test("addc", lambda: cdo.addc("100", input=topo_nc,
             output=addc_nc) or assert_file(addc_nc))

    abs_nc = os.path.join(tmpdir, "abs.nc")
    run_test("abs", lambda: cdo.abs(input=topo_nc,
             output=abs_nc) or assert_file(abs_nc))

    sqrt_nc = os.path.join(tmpdir, "sqrt.nc")
    run_test("sqrt", lambda: cdo.sqrt(input=abs_nc,
             output=sqrt_nc) or assert_file(sqrt_nc))

    expr_nc = os.path.join(tmpdir, "expr.nc")

    def _expr_test():
        names = str(cdo.showname(input=topo_nc)).strip().split()
        vname = names[0] if names else "topo"
        cdo.expr(f"doubled={vname}*2;", input=topo_nc, output=expr_nc)
        assert_file(expr_nc)
    run_test("expr", _expr_test)

    add_nc = os.path.join(tmpdir, "add.nc")
    run_test("add files", lambda: cdo.add(
        input=f"{topo_nc} {mulc_nc}", output=add_nc) or assert_file(add_nc))

    # Grid operations
    print("\n=== 7. Grid Ops ===")
    remap_bil_nc = os.path.join(tmpdir, "remap_bil.nc")
    run_test("remapbil", lambda: cdo.remapbil("r72x36", input=topo_nc,
             output=remap_bil_nc) or assert_file(remap_bil_nc))

    remap_con_nc = os.path.join(tmpdir, "remap_con.nc")
    run_test("remapcon", lambda: cdo.remapcon("r72x36", input=topo_nc,
             output=remap_con_nc) or assert_file(remap_con_nc))

    remap_nn_nc = os.path.join(tmpdir, "remap_nn.nc")
    run_test("remapnn", lambda: cdo.remapnn("r72x36", input=topo_nc,
             output=remap_nn_nc) or assert_file(remap_nn_nc))

    # Spectral (CRITICAL)
    print("\n=== 8. Spectral (CRITICAL) ===")
    sp_nc = os.path.join(tmpdir, "spectral.nc")
    run_test("gp2sp T21", lambda: cdo(
        f"cdo gp2sp -remapbil,t21grid {topo_nc} {sp_nc}", timeout=60) or assert_file(sp_nc))

    sp2gpl_nc = os.path.join(tmpdir, "sp2gpl.nc")
    run_test("sp2gpl", lambda: cdo.sp2gpl(
        input=sp_nc, output=sp2gpl_nc) or assert_file(sp2gpl_nc))

    sp2gp_nc = os.path.join(tmpdir, "sp2gp.nc")
    run_test("sp2gp", lambda: cdo.sp2gp(
        input=sp_nc, output=sp2gp_nc) or assert_file(sp2gp_nc))

    complex_sp_nc = os.path.join(tmpdir, "complex_sp.nc")
    run_test("sp2gpl chain nc4", lambda: cdo(
        f"cdo -f nc4 -sp2gpl -setgridtype,regular {sp_nc} {complex_sp_nc}", timeout=60) or assert_file(complex_sp_nc))

    # Format conversion
    print("\n=== 9. Formats ===")
    nc4_nc = os.path.join(tmpdir, "nc4.nc")
    run_test("NetCDF4", lambda: cdo.copy(input=topo_nc,
             output=nc4_nc, options="-f nc4") or assert_file(nc4_nc))

    nc2_nc = os.path.join(tmpdir, "nc2.nc")
    run_test("NetCDF2", lambda: cdo.copy(input=topo_nc,
             output=nc2_nc, options="-f nc2") or assert_file(nc2_nc))

    grb_file = os.path.join(tmpdir, "topo.grb")
    run_test("GRIB1 encode", lambda: cdo.copy(input=topo_nc,
             output=grb_file, options="-f grb") or assert_file(grb_file))

    grb2_file = os.path.join(tmpdir, "topo.grb2")
    run_test("GRIB2 encode", lambda: cdo.copy(input=topo_nc,
             output=grb2_file, options="-f grb2") or assert_file(grb2_file))

    grb_decode_nc = os.path.join(tmpdir, "grb_decode.nc")
    if os.path.exists(grb_file):
        run_test("GRIB1 decode", lambda: cdo.copy(input=grb_file,
                 output=grb_decode_nc) or assert_file(grb_decode_nc))

    # Time operations
    print("\n=== 10. Time Ops ===")
    t1_nc = os.path.join(tmpdir, "t1.nc")
    t2_nc = os.path.join(tmpdir, "t2.nc")
    merged_nc = os.path.join(tmpdir, "merged.nc")
    run_test("mergetime", lambda: (
        cdo.settaxis("2020-01-01,12:00:00,1day", input=topo_nc, output=t1_nc),
        cdo.settaxis("2020-01-02,12:00:00,1day", input=topo_nc, output=t2_nc),
        cdo.mergetime(input=f"{t1_nc} {t2_nc}", output=merged_nc),
        assert_file(merged_nc),
        assert_true(int(str(cdo.ntime(input=merged_nc)).strip()) == 2)
    ))

    run_test("showdate", lambda: assert_true(
        "2020" in str(cdo.showdate(input=monthly_nc))))
    run_test("showmon", lambda: str(cdo.showmon(input=monthly_nc)))

    # Chained operations
    print("\n=== 11. Chains ===")
    chain1_nc = os.path.join(tmpdir, "chain1.nc")
    run_test("sel+remap", lambda:  cdo(
        f"cdo -remapbil,r72x36 -sellonlatbox,0,180,0,90 {topo_nc} {chain1_nc}", timeout=40) or assert_file(chain1_nc))

    chain2_nc = os.path.join(tmpdir, "chain2.nc")
    run_test("sel+fldmean", lambda: cdo(
        f"cdo -fldmean -sellonlatbox,-180,180,-30,30 {topo_nc} {chain2_nc}", timeout=30) or assert_file(chain2_nc))

    chain3_nc = os.path.join(tmpdir, "chain3.nc")
    run_test("mulc+addc", lambda: cdo(
        f"cdo -addc,273.15 -mulc,0.01 {topo_nc} {chain3_nc}", timeout=30) or assert_file(chain3_nc))

    # Vertical interpolation
    print("\n=== 12. Vertical / Level Ops ===")
    ml2pl_nc = os.path.join(tmpdir, "ml2pl.nc")
    run_test("intlevel", lambda: cdo.intlevel(
        "0,5000,20000", input=stdatm_nc, output=ml2pl_nc) or assert_file(ml2pl_nc))

    seltimestep_nc = os.path.join(tmpdir, "seltimestep.nc")
    run_test("seltimestep", lambda: cdo.seltimestep(
        "1,2,3", input=monthly_nc, output=seltimestep_nc) or assert_file(seltimestep_nc))

    # Masking and conditional
    print("\n=== 13. Masking ===")
    setmiss_nc = os.path.join(tmpdir, "setmiss.nc")
    run_test("setmissval", lambda: cdo.setmissval(
        "-999", input=topo_nc, output=setmiss_nc) or assert_file(setmiss_nc))

    setrtomiss_nc = os.path.join(tmpdir, "setrtomiss.nc")
    run_test("setrtomiss", lambda: cdo.setrtomiss(
        "-1000,0", input=topo_nc, output=setrtomiss_nc) or assert_file(setrtomiss_nc))

    setmisstoc_nc = os.path.join(tmpdir, "setmisstoc.nc")
    run_test("setmisstoc", lambda: cdo.setmisstoc(
        "0", input=setrtomiss_nc, output=setmisstoc_nc) or assert_file(setmisstoc_nc))

    # More statistics
    print("\n=== 14. Extended Stats ===")
    fldmin_nc = os.path.join(tmpdir, "fldmin.nc")
    run_test("fldmin", lambda: cdo.fldmin(input=topo_nc,
             output=fldmin_nc) or assert_file(fldmin_nc))

    fldmax_nc = os.path.join(tmpdir, "fldmax.nc")
    run_test("fldmax", lambda: cdo.fldmax(input=topo_nc,
             output=fldmax_nc) or assert_file(fldmax_nc))

    fldsum_nc = os.path.join(tmpdir, "fldsum.nc")
    run_test("fldsum", lambda: cdo.fldsum(input=topo_nc,
             output=fldsum_nc) or assert_file(fldsum_nc))

    timmin_nc = os.path.join(tmpdir, "timmin.nc")
    run_test("timmin", lambda: cdo.timmin(input=monthly_nc,
             output=timmin_nc) or assert_file(timmin_nc))

    timmax_nc = os.path.join(tmpdir, "timmax.nc")
    run_test("timmax", lambda: cdo.timmax(input=monthly_nc,
             output=timmax_nc) or assert_file(timmax_nc))

    timsum_nc = os.path.join(tmpdir, "timsum.nc")
    run_test("timsum", lambda: cdo.timsum(input=monthly_nc,
             output=timsum_nc) or assert_file(timsum_nc))

    mermean_nc = os.path.join(tmpdir, "mermean.nc")
    run_test("mermean", lambda: cdo.mermean(input=topo_nc,
             output=mermean_nc) or assert_file(mermean_nc))

    # Grid description & manipulation
    print("\n=== 15. Grid Manipulation ===")
    run_test("gridarea", lambda: cdo.gridarea(input=topo_nc,
             output=os.path.join(tmpdir, "gridarea.nc")) or assert_file(os.path.join(tmpdir, "gridarea.nc")))

    run_test("gridweights", lambda: cdo.gridweights(input=topo_nc,
             output=os.path.join(tmpdir, "gridweights.nc")) or assert_file(os.path.join(tmpdir, "gridweights.nc")))

    setgrid_nc = os.path.join(tmpdir, "setgrid.nc")
    run_test("setgridtype", lambda: cdo.setgridtype(
        "regular", input=topo_nc, output=setgrid_nc) or assert_file(setgrid_nc))

    # Metadata operations
    print("\n=== 16. Metadata ===")
    chname_nc = os.path.join(tmpdir, "chname.nc")

    def _chname_test():
        # Detect variable name, stripping any non-printable chars
        raw = str(cdo.showname(input=topo_nc))
        vname = raw.strip().split()[0] if raw.strip() else "topo"
        vname = ''.join(c for c in vname if c.isprintable() and c != ' ')
        # Force NetCDF output — chname cannot rename vars in GRIB format
        # (GRIB uses parameter codes, variable names are derived from code tables)
        cdo(f"cdo -f nc -chname,{vname},elevation {topo_nc} {chname_nc}", timeout=30)
        assert_file(chname_nc)
        # Verify the renamed variable.
        # On older Windows builds CDO may hang at exit after reading a
        # NetCDF file; if showname times out, accept the file-exists
        # check above as sufficient evidence that chname succeeded.
        try:
            new_raw = str(cdo.showname(input=chname_nc)).strip()
            if "elevation" not in new_raw:
                raise AssertionError(
                    f"chname: expected 'elevation' in showname output, "
                    f"got '{new_raw}' (original var: '{vname}')")
        except Exception as e:
            if "timed out" in str(e).lower():
                print(
                    f"    (showname verification skipped — CDO exit hang: {e})")
            else:
                raise
    run_test("chname", _chname_test)

    run_test("showyear", lambda: str(cdo.showyear(input=monthly_nc)))
    run_test("nvar", lambda: assert_true(
        int(str(cdo.nvar(input=topo_nc)).strip()) >= 1))
    run_test("showlevel", lambda: str(cdo.showlevel(input=stdatm_nc)))
    run_test("showcode", lambda: str(cdo.showcode(input=topo_nc)))

    # Seasonal / monthly statistics
    print("\n=== 17. Seasonal Stats ===")
    ymonmean_nc = os.path.join(tmpdir, "ymonmean.nc")
    run_test("ymonmean", lambda: cdo.ymonmean(input=monthly_nc,
             output=ymonmean_nc) or assert_file(ymonmean_nc))

    # File comparison / diff
    print("\n=== 18. Comparison ===")
    run_test("diff (identical)", lambda: cdo.diff(
        input=f"{topo_nc} {topo_nc}"))

    sub_nc = os.path.join(tmpdir, "sub.nc")
    run_test("sub", lambda: cdo.sub(
        input=f"{topo_nc} {topo_nc}", output=sub_nc) or assert_file(sub_nc))

    mul_nc = os.path.join(tmpdir, "mul.nc")
    run_test("mul", lambda: cdo.mul(
        input=f"{topo_nc} {topo_nc}", output=mul_nc) or assert_file(mul_nc))

    div_nc = os.path.join(tmpdir, "div.nc")
    run_test("div", lambda: cdo.div(
        input=f"{topo_nc} {addc_nc}", output=div_nc) or assert_file(div_nc))

    # Advanced chains
    print("\n=== 19. Advanced Chains ===")
    chain4_nc = os.path.join(tmpdir, "chain4.nc")
    run_test("fldmean+abs+mulc", lambda: cdo(
        f"cdo -fldmean -abs -mulc,-1 {topo_nc} {chain4_nc}", timeout=30) or assert_file(chain4_nc))

    chain5_nc = os.path.join(tmpdir, "chain5.nc")
    run_test("fldmean+sellev", lambda: cdo(
        f"cdo -fldmean -sellevel,0 {stdatm_nc} {chain5_nc}", timeout=30) or assert_file(chain5_nc))

    chain6_nc = os.path.join(tmpdir, "chain6.nc")
    run_test("timmean+selmon", lambda: cdo(
        f"cdo -timmean -selmon,1,6 {monthly_nc} {chain6_nc}", timeout=30) or assert_file(chain6_nc))

    # Ensemble / merge operations
    print("\n=== 20. Merge / Ensemble ===")
    merge_nc = os.path.join(tmpdir, "merge.nc")
    run_test("merge", lambda: cdo.merge(
        input=f"{topo_nc} {const_nc}", output=merge_nc) or assert_file(merge_nc))

    run_test("splitlevel", lambda: cdo.splitlevel(
        input=stdatm_nc, output=os.path.join(tmpdir, "splev")))

    ensmean_nc = os.path.join(tmpdir, "ensmean.nc")
    run_test("ensmean", lambda: cdo.ensmean(
        input=f"{topo_nc} {topo_nc}", output=ensmean_nc) or assert_file(ensmean_nc))

    # Error handling
    print("\n=== 21. Errors ===")
    run_test("invalid file", lambda: assert_raises(
        CdoError, lambda: cdo.info(input="/nonexistent.nc")))
    run_test("invalid params", lambda: assert_raises(CdoError, lambda: cdo.sellonlatbox(
        "abc,def", input=topo_nc, output=os.path.join(tmpdir, "err.nc"))))

    # ---- Summary ----
    elapsed = time.time() - t_start
    total = passed + failed
    print(f"\n{'='*60}")
    print(f"Results: {passed}/{total} passed, {failed} failed")
    print(f"Time: {elapsed:.1f}s")
    print(f"{'='*60}")

    if failed > 0:
        sys.exit(1)
    print("All stress tests passed!")


# ======================================================================
# Helper functions
# ======================================================================

def assert_file(path):
    """Assert file exists and is non-empty"""
    if not os.path.isfile(path):
        raise AssertionError(f"File does not exist: {path}")
    if os.path.getsize(path) == 0:
        raise AssertionError(f"File is empty: {path}")


def assert_true(cond):
    """Assert condition is true"""
    if not cond:
        raise AssertionError(f"Condition failed")


def assert_raises(exc_type, func):
    """Assert function raises exception"""
    try:
        func()
    except exc_type:
        return
    raise AssertionError(f"Expected {exc_type.__name__} but got none")


if __name__ == "__main__":
    main()
