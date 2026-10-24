# PlatformIO extra script for the vendored dav1d (library.json build.extraScript).
#
# The library builds at -Os to keep flash down, but a photo's decode time is
# almost all in a few files: the entropy decoder, coefficient reading, motion
# compensation, the inverse transforms, intra prediction and the loop filters.
# Those build at -O2: about 20 KB more flash, a quarter faster per photo in a
# host benchmark (scripts/build/perf_lvgl.py does the same for LVGL).
# A board short of flash opts out with -DVOX_AV1_SMALL in its build_flags.
Import("env")

HOT = (
    "src/msac.c",
    "src/8bpc/recon_8.c",
    "src/8bpc/mc_8.c",
    "src/8bpc/itx_8.c",
    "src/8bpc/ipred_8.c",
    "src/8bpc/ipred_prepare_8.c",
    "src/8bpc/cdef_8.c",
    "src/8bpc/cdef_apply_8.c",
    "src/8bpc/loopfilter_8.c",
    "src/8bpc/lf_apply_8.c",
    "src/8bpc/looprestoration_8.c",
)

def _small(e):
    flags = " ".join(str(f) for f in e.get("BUILD_FLAGS", []))
    defines = " ".join(str(d) for d in e.Flatten(e.get("CPPDEFINES", [])))
    return "VOX_AV1_SMALL" in flags or "VOX_AV1_SMALL" in defines

def _fast(e, node):
    path = node.srcnode().get_path().replace("\\", "/")
    if not any(path.endswith(h) for h in HOT):
        return node
    flags = [f for f in e["CCFLAGS"] if f not in ("-Os", "-O1", "-O3")] + ["-O2"]
    return e.Object(node, CCFLAGS=flags)

if not _small(env):
    env.AddBuildMiddleware(_fast)
